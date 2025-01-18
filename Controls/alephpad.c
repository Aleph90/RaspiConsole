/*
Written by Alessandro Malusà
*/

/* Figure out licensing stuff */

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

#define NO_REP_TIMEOUT          -1
#define REP1_TIMEOUT            500000000   // 500ms -> 5e8ns
#define REP2_TIMEOUT            200000000   // 200ms -> 2e8ns
#define REP_TIMEOUT_DECREASE    5000000     // 5ms   -> 5e6ns
#define MIN_TIMEOUT             30000000    // 30ms  -> 3e7ns

#define DEBOUNCE_TIME           20000       // 20ms  -> 2e4us

enum buttonValue    {
    BTN_PRESS = GPIOD_EDGE_EVENT_FALLING_EDGE,
    BTN_RELEASE = GPIOD_EDGE_EVENT_RISING_EDGE
};

enum EventValue   {
    EVT_PRESS   = 1,
    EVT_RELEASE = 0,
    EVT_REPEAT  = 2
};

static void error(char* msg);
static void deviceSetup(char *path, unsigned int *button);
static void emitKeyEvent(int key, int value);
static void emitSynEvent();
static void prepareRequest(char *path, unsigned int *line);
static void cleanup();
static void sigHandler(int signo);

bool
    running = true;
extern char
    *__progname,
    *program_invocation_name;
char *event_type[] = {
    "release",
    "press",
    "repeat"
};
int
    devfd;                          // Virtual device file descriptor
struct input_event
    keyEv,
    synEv;
struct gpiod_line_request
    *request = NULL;

static void error(char* msg)   {
    fprintf(stderr, "%s: %s. Try 'sudo %s'?\n",
        __progname, msg, program_invocation_name);
    cleanup();
    exit(1);
}

static void deviceSetup(char *path, unsigned int *button) {
    int
        i;
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
    struct gpiod_request_config *req_cfg = NULL;
    struct gpiod_line_settings *settings;
    struct gpiod_line_config *line_cfg;
    struct gpiod_chip *chip;
    unsigned int i;                 // Generic index
    bool error_occurred = false;

    if(!error_occurred) {
        chip = gpiod_chip_open(path);
        error_occurred = (chip==NULL);
        if(error_occurred)   fprintf(stderr,
            "%s: Failed to open GPIO chip.\n", __progname);
    }
    if(!error_occurred) {
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
        req_cfg = gpiod_request_config_new();
        error_occurred = (req_cfg==NULL);
        if(error_occurred)  fprintf(stderr,
            "%s: Failed to create request configuration.\n", __progname);
        gpiod_request_config_set_consumer(req_cfg, CONSUMER);
    }
    if(!error_occurred) {
        request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    }

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
    running = false;
}

int main(int argc, char** argv) {
    // Declare all local variables
    // ---------------------------
    char
        uinput_path[]   = "/dev/uinput",
        chip_path[]     = "/dev/gpiochip0";
    unsigned int
        line[NUM_LINES],
        button[NUM_LINES],
        i;                      // Generic counter
    int64_t
        timeout = NO_REP_TIMEOUT;
    int
        last_button = -1,       // Last button pressed
        ret;
    enum gpiod_line_value
        old_value[NUM_LINES],
        value[NUM_LINES];
    struct gpiod_edge_event_buffer
        *event_buffer;
    struct gpiod_edge_event
        *event;
    struct sigaction
        act;






    unsigned int porcoddue[32] = {};
    for(int a=0; a<NUM_LINES; a++)  {
        porcoddue[line2key[a].line] = line2key[a].key_code;
    }





    // Signal handling
    // ---------------
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
        line[i]     = line2key[i].line;
        button[i]   = line2key[i].key_code;
    }

    // Request
    prepareRequest(chip_path, line);
    if(!request)    error("Failed to generate GPIO line request");
    if(gpiod_line_request_get_values(request, old_value) < 0)
        error("Edge event request failed");

    // Event buffer
    event_buffer = gpiod_edge_event_buffer_new(NUM_LINES);
    if(!event_buffer)
        error("Failed to create event buffer");

    // Events and virtual device
    memset(&keyEv, 0, sizeof(keyEv));
    keyEv.type = EV_KEY;

    memset(&synEv, 0, sizeof(synEv));
    synEv.type = EV_SYN;
    synEv.code = SYN_REPORT;

    deviceSetup(uinput_path, button);

    for(i=0; i<NUM_LINES; i++)  {
        fprintf(stdout, "%d\n", old_value[i]);
    }

    // Main loop
    // ---------
    while(running)  {
        // fprintf(stdout, "Timeout: %d, ", timeout);
        ret = gpiod_line_request_wait_edge_events(request, timeout);
        fprintf(stdout, "Timeout: %d, return value: %d.\n", timeout, ret);
        if(ret < 0) {
            if(running)
                error("Error occurred while waiting for edge event");
        }
        else if(ret == 0)   {
            // Timeout occurred: Adjust timeout and issue repeat event
            fprintf(stdout, "Wait on edge timed out. Timeout: %d.\n", timeout);
            if(timeout == REP1_TIMEOUT)     timeout = REP2_TIMEOUT;
            else if(timeout > MIN_TIMEOUT)  timeout -= REP_TIMEOUT_DECREASE;

            emitKeyEvent(last_button, EVT_REPEAT);
        }
        // else    {
        //     // Edge event detected
        //     if(gpiod_line_request_get_values(request, value) < 0)
        //         error("Edge event request failed");
        //     // Request successful
        //     for(i=0; i<NUM_LINES; i++)  {
        //         if(value[i] != old_value[i])  {
        //             if(value[i]==BTN_PRESS)    {
        //                 last_button = button[i];
        //                 timeout = REP1_TIMEOUT;
        //             }
        //             else    {
        //                 last_button = -1;
        //                 timeout = NO_REP_TIMEOUT;
        //             }
        //             old_value[i] = value[i];
        //             emitKeyEvent(
        //                 button[i],
        //                 value[i]==BTN_PRESS? EVT_PRESS : EVT_RELEASE
        //             );
        //         }
        //     }
        // }
        else    {
            // Edge event detected
            ret = gpiod_line_request_read_edge_events(
                request,
                event_buffer,
                NUM_LINES
            );
            if(ret < 0)
                error("Failed to read edge events");
            for(i=0; i<ret; i++)    {
                event = gpiod_edge_event_buffer_get_event(event_buffer, i);
                if(gpiod_edge_event_get_event_type(event)==BTN_PRESS)   {
                    last_button = porcoddue[gpiod_edge_event_get_line_offset(event)];
                    timeout = REP1_TIMEOUT;
                }
                else    {
                    last_button = -1;
                    timeout = NO_REP_TIMEOUT;
                }
                emitKeyEvent(
                    porcoddue[gpiod_edge_event_get_line_offset(event)],
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
