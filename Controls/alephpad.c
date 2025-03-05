/*!
 * Written by Alessandro Malusà.
 * Monitor GPIO events using libgpiod and map them to virtual device events
 * via uinput.
 * 
 * This work is inspired by Adafruit's Retrogame utility:
 * https://github.com/adafruit/Adafruit-Retrogame/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <signal.h>
#include <gpiod.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include "keymap.h"

#ifndef CONSUMER
#define CONSUMER "alephpad"
#endif

/*!
 * Timeout intervals. Once a button press event occurs, the program
 * will issue repeat events at given intervals until a different button
 * is pressed, or the last pressed button is released. 
 */
#define NO_REP_TIMEOUT          -1
#define REP1_TIMEOUT            500000000   // 500ms -> 5e8ns
#define REP2_TIMEOUT            200000000   // 200ms -> 2e8ns
#define REP_TIMEOUT_DECREASE    5000000     // 5ms   -> 5e6ns
#define MIN_TIMEOUT             30000000    // 30ms  -> 3e7ns

#define DEBOUNCE_TIME           20000       // 20ms  -> 2e4us

// Abstract falling edges as button events.
enum buttonValue    {
    BTN_PRESS = GPIOD_EDGE_EVENT_FALLING_EDGE,
    BTN_RELEASE = GPIOD_EDGE_EVENT_RISING_EDGE
};

enum eventValue   {
    EVT_PRESS   = 1,
    EVT_RELEASE = 0,
    EVT_REPEAT  = 2
};

/*!
 * Simple error handler: Print an error message, clean up, exit with error.
 * @param msg: Custom error message.
 */
static void error(char* msg);

/*!
 * Set up uinput virtual device. Its file descriptor is a global variable.
 * @param path:     Path to uinput directory.
 * @param button:   Array of codes of keys to enable on the virtual device.
 */
static void deviceSetup(char* path, unsigned int* button);

/*!
 * Emit event on the given key with the specified value.
 * @param key:      Code of the key.
 * @param value:    Value of the event (press, release, repeat).
 */
static void emitKeyEvent(int key, int value);

/*!
 * Write Syn Report.
 */
static void emitSynEvent();

/*!
 * Set up the struct gpiod_line_request object `request`.
 * This contains the information of the GPIO pins to watch and the pull-up,
 * edge, and debounce time settings.
 * @param path: Path to the GPIO device.
 * @param line: Array of pin numbers to watch.
 */
static void prepareRequest(char* path, unsigned int* line);

/*!
 * Release all assets.
 */
static void cleanup();

/*!
 * Custom signal handler for graceful termination.
 * @param signo: Signal code.
 */
static void sigHandler(int signo);

// Global variables
bool
    running = true;             // Issues terminations when set to `false`
extern char
    *__progname,
    *program_invocation_name;
char *event_type[] = {          // Code-to-name for key events, for debugging
    "release",
    "press",
    "repeat"
};
int
    devfd;                      // Virtual device fd, set up by `deviceSetup()`
struct input_event              // From <linux/input.h>
    keyEv,                      // Container for key event information
    synEv;                      // Container for syn report event
struct gpiod_line_request       // From <gpiod.h>
    *request = NULL;            // GPIO request, set up by `prepareRequest()`

static void error(char* msg)   {
    fprintf(stderr, "%s: %s. Try 'sudo %s'?\n",
        __progname, msg, program_invocation_name);
    cleanup();
    exit(1);
}

static void deviceSetup(char *path, unsigned int *button) {
    int
        i;      // Generic index to iterate through `button`.
    /*
     * `struct uinput_setup` is defined in <linux/uinput.h> and contains the
     * basic information (name, manufacturer...) of a virtual device. Once
     * filled, it is passed to `ioctl()` before creating the device.
     */
    struct uinput_setup
        usetup;

    if((devfd = open(path, O_WRONLY | O_NONBLOCK)) < 0)
        error("Failed to open uinput path");

    // Enable key events
    ioctl(devfd, UI_SET_EVBIT, EV_KEY);
    for(i=0; i<NUM_LINES; i++)  {
        ioctl(devfd, UI_SET_KEYBIT, button[i]);
    }

    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype   = BUS_USB;
    usetup.id.vendor    = 0x1;
    usetup.id.product   = 0x1;
    strcpy(usetup.name, "Aleph Pad");

    ioctl(devfd, UI_DEV_SETUP, &usetup);
    ioctl(devfd, UI_DEV_CREATE);
}

static void emitKeyEvent(int button, int value)    {
    keyEv.code  = button;
    keyEv.value = value;
    write(devfd, &keyEv, sizeof(keyEv));
    fprintf(stdout, "%s: Emitting %s event on button %#05x.\n",
        __progname, event_type[value], button);
}

static void emitSynEvent()  {
    fprintf(stdout, "Emitting syn report.\n");
    write(devfd, &synEv, sizeof(synEv));
}

static void prepareRequest(char *path, unsigned int *line) {
    /*
     * Based on the libgpiod examples at 
     * https://github.com/brgl/libgpiod/blob/master/examples/
     * A struct gpiod_line_request object contains the information required
     * by a number of gpiod functions to read or poll GPIO values. A request
     * is created by specifying
     * - a chip, 
     * - request configurations, and
     * - line configurations, which in turn require settings for each line.
     * The required objects are declared here and initialized below. If any
     * of the relevant operations fails, an error message is printed but the
     * process is not halted. Instead, the function skips to the end, where
     * all temporary objects are cleared and resources released. In that case
     * the request is not created, and it is left up to the caller to halt
     * and gracefully exit the process.
     */
    struct gpiod_request_config *req_cfg = NULL;
    struct gpiod_line_settings *settings;
    struct gpiod_line_config *line_cfg;
    struct gpiod_chip *chip;
    unsigned int i;                 // Generic index
    bool error_occurred = false;    // Error detection flag

    if(!error_occurred) {
        // Open GPIO chip
        chip = gpiod_chip_open(path);
        error_occurred = (chip==NULL);
        if(error_occurred)   fprintf(stderr,
            "%s: Failed to open GPIO chip.\n", __progname);
    }
    if(!error_occurred) {
        /*
         * Create GPIO line settings, including:
         * - Direction (input/output)
         * - Edge (rising/falling/both)
         * - Bias (pull-up/down resistor)
         * - Active high or low
         * - Debounce time, in microseconds
         */
        settings = gpiod_line_settings_new();
        error_occurred = (settings==NULL);
        error_occurred |= 
            gpiod_line_settings_set_direction(
                settings,
                GPIOD_LINE_DIRECTION_INPUT
            ) < 0;
        error_occurred |=
            gpiod_line_settings_set_edge_detection(
                settings,
                GPIOD_LINE_EDGE_BOTH
            ) < 0;
        error_occurred |=
            gpiod_line_settings_set_bias(
                settings,
                GPIOD_LINE_BIAS_PULL_UP
            ) < 0;
        gpiod_line_settings_set_active_low(settings, false);
        gpiod_line_settings_set_debounce_period_us(settings, DEBOUNCE_TIME);
        
        if(error_occurred)  fprintf(stderr,
            "%s: Failed to create GPIO line settings.\n", __progname);
    }
    if(!error_occurred) {
        /*
         * Create GPIO configurations, a collection of settings of all
         * the pins to be included in the request.
         */
        line_cfg = gpiod_line_config_new();
        error_occurred = (line_cfg==NULL);
        if(error_occurred)  fprintf(stderr,
            "%s: Failed to create GPIO line configuration.\n", __progname);
        for(i=0; i<NUM_LINES && !error_occurred; i++)   {
            error_occurred = (gpiod_line_config_add_line_settings(line_cfg,
                &line[i], 1, settings) < 0);
            if(error_occurred)  fprintf(stderr,
                "%s: Failed to add GPIO line %d to configuration.\n",
                __progname, i);
        }
    }
    if(!error_occurred) {
        // Create request configuration
        req_cfg = gpiod_request_config_new();
        error_occurred = (req_cfg==NULL);
        if(error_occurred)  fprintf(stderr,
            "%s: Failed to create request configuration.\n", __progname);
        gpiod_request_config_set_consumer(req_cfg, CONSUMER);
    }
    if(!error_occurred) {
        // Create request
        request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    }

    // Free all temporary objects and free allocated resources
    if(req_cfg)     gpiod_request_config_free(req_cfg);
    if(line_cfg)    gpiod_line_config_free(line_cfg);
    if(settings)    gpiod_line_settings_free(settings);
    if(chip)        gpiod_chip_close(chip);
}

static void cleanup()   {
    // Todo: free event buffer
    if(devfd >= 0)  {
        ioctl(devfd, UI_DEV_DESTROY);
        close(devfd);
    }

    if(request) {
        gpiod_line_request_release(request);
    }
}

static void sigHandler(int signo)   {
    /*
     * Let the current iteration of the main cycle to conclude, and then
     * exit the loop.
     */
    running = false;
}

int main(int argc, char** argv) {
    // Declare all local variables
    char
        uinput_path[]   = "/dev/uinput",
        chip_path[]     = "/dev/gpiochip0";
    unsigned int
        line[NUM_LINES],        // Array of pin numbers
        button[NUM_LINES],      // Array of key codes
        i;                      // Generic counter
    int64_t
        timeout = NO_REP_TIMEOUT;   // Timeout for repeats, default none
    int
        last_button = -1,       // Last button pressed
        ret;                    // Store return values and error flags
    enum gpiod_line_value
        old_value[NUM_LINES],
        value[NUM_LINES];
    struct gpiod_edge_event_buffer
        *event_buffer;          // Store all GPIO edge events since last read
    struct gpiod_edge_event
        *event;                 // Store individual GPIO events
    struct sigaction
        act;
    unsigned int
        line2key_arr[32] = {};  // Quickly map pin numbers to key codes
    
    // Signal handling
    memset(&act, 0, sizeof(act));
    act.sa_handler = sigHandler;

    sigaction(SIGABRT, &act, NULL);
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    
    // Setup and initialize variables
    // ------------------------------
    // Lines and buttons
    for(i=0; i<NUM_LINES; i++)  {
        line[i]     = line2key_dict[i].line;
        button[i]   = line2key_dict[i].key_code;
        line2key_arr[line2key_dict[i].line] = line2key_dict[i].key_code;
    }

    // Create request and initialize internal event mask
    prepareRequest(chip_path, line);
    if(!request)    error("Failed to generate GPIO line request");
    if(gpiod_line_request_get_values(request, old_value) < 0)
        error("Edge event request failed");

    // Event buffer
    event_buffer = gpiod_edge_event_buffer_new(NUM_LINES);
    if(!event_buffer)
        error("Failed to create event buffer");

    // Uinput events and virtual device
    memset(&keyEv, 0, sizeof(keyEv));
    keyEv.type = EV_KEY;

    memset(&synEv, 0, sizeof(synEv));
    synEv.type = EV_SYN;
    synEv.code = SYN_REPORT;

    deviceSetup(uinput_path, button);

    // Main loop
    while(running)  {
        /*
         * Poll GPIO edge events for `timeout` microseconds if a repeat
         * event is pending, otherwise indefinitely.
         */
        ret = gpiod_line_request_wait_edge_events(request, timeout);
        if(ret < 0) {
            /*
             * Polling was terminated in a way that gpiod did not expect.
             * This warrants an error message unless this was caused by a
             * signal, in which case `running` would be `false`.
             */
            if(running)
                error("Error occurred while waiting for edge event");
        }
        else if(ret == 0)   {
            // Timeout occurred: Adjust timeout interval and issue repeat event
            fprintf(stdout, "Wait on edge timed out. Timeout: %d.\n", timeout);
            if(timeout == REP1_TIMEOUT)     timeout = REP2_TIMEOUT;
            else if(timeout > MIN_TIMEOUT)  timeout -= REP_TIMEOUT_DECREASE;

            emitKeyEvent(last_button, EVT_REPEAT);
        }
        else    {
            // Edge event detected. Record events in `event_buffer`.
            ret = gpiod_line_request_read_edge_events(
                request,
                event_buffer,
                NUM_LINES
            );
            if(ret < 0)
                error("Failed to read edge events");
            // We have `ret` events to parse. Process them sequentially.
            for(i=0; i<ret; i++)    {
                event = gpiod_edge_event_buffer_get_event(event_buffer, i);
                if(gpiod_edge_event_get_event_type(event)==BTN_PRESS)   {
                    last_button = line2key_arr[gpiod_edge_event_get_line_offset(event)];
                    timeout = REP1_TIMEOUT;
                }
                else    {
                    last_button = -1;
                    timeout = NO_REP_TIMEOUT;
                }
                emitKeyEvent(
                    line2key_arr[gpiod_edge_event_get_line_offset(event)],
                    gpiod_edge_event_get_event_type(event)==BTN_PRESS ?
                        EVT_PRESS : EVT_RELEASE
                );
            }
        }

        emitSynEvent();
    }

    cleanup();
    return 0;
}
