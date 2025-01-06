/*
Written by Alessandro Malusà.

FEATURES:
    - Debugging: Print messages depending on debug level:
        * 0:    off
        * 1:    errors (default)
        * 3:    button events live (default if launched from terminal)
    - Key repeat: Repeat presses of the last pressed key at an accelerated
      rate. Repetition ends when any button is released or when a new
      one is pressed, in which case the rate of repetition is reset and
      repetition of the new button begins.
*/

/*
TODO:
- Include license information;
- Determine if Adafruit is to be credited.
*/

#define NUM_GPIOS           32
#define NUM_WATCHFILES      35

#define IDLE_TIMEOUT        -1
#define DEBOUNCE_TIMEOUT    20
#define REP1_TIMEOUT        500
#define REP2_TIMEOUT        100
#define REP_STEP            5
#define MIN_REP_TIMEOUT     30

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/input.h>
#include "keymap.c"

enum pendingState   {
    IDLE,
    DEBOUNCE,
    REPEAT_BEGIN,
    REPEAT_CONT,
};

bool
    running = true;
extern char
    *__progname,
    *program_invocation_name;
char
    sysfs_root[] = "/sys/class/gpio",
    debug = 1;          // Default debug level is 1: print errors

/*
TODO:
- Define pin initialize function;
- Define pin deinitialize function;
- Define signal handling function;
- Add debug messages;
*/

static void err(char *msg)  {
    printf("%s: %s. Try 'sudo %s'?\n",
        __progname, msg, program_invocation_name);
    /* TODO: deinit pins */
    exit(1);
}

static int pinSetup(int pin, char* attr, char* value)  {
    int fd, w = -1, len = strlen(value);
    char filename[50];
    sprintf(filename, "%s/gpio%d/%s", sysfs_root, pin, attr);
    if((fd = open(filename, O_WRONLY)) >= 0)    {
        w = (write(fd, value, len) != len);
        close(fd);
    }
    return w;
}

static int pinInit(struct pollfd *p, uint32_t *initial_state)   {
    char
        buf[50];        // Container 
    int
        fd,             // Generic file descriptor
        i;              // Generic index
    uint32_t
        pinmask = 0;

    for(i=0; i<NUM_GPIOS; i++)  {
        if(key[i] > KEY_RESERVED)   pinmask |= (1<<i);
    }
    /* TODO: pull(pinmask, 2); */

    sprintf(buf, "%s/export", sysfs_root);
    if((fd = open(buf, O_WRONLY)) < 0)  err("Can't open GPIO export file");
    for(i=0; i<NUM_GPIOS; i++)  {
        if(key[i] > KEY_RESERVED)   {
            // Configure pin
            sprintf(buf, "%d", i);
            write(fd, buf, strlen(buf));
            if(pinSetup(i, "active_low", "0")   ||
                pinSetup(i, "direction", "in")  ||
                pinSetup(i, "edge", "both"))    {
                err("Pin configuration failed");
            }

            // Build pin pollfd; Read initial state
            char x;
            sprintf(buf, "%s/gpio%d/value", sysfs_root, i);
            if((p[i].fd = open(buf, O_RDONLY | O_NONBLOCK)) < 0)
                err("Can't read pin value");
            if(read(p[i].fd, &x, 1) && (x == '0'))
                (*initial_state) |= (1<<i);
            p[i].events = POLLPRI | POLLERR | POLLHUP | POLLNVAL;
            p[i].revents = 0;
        }
    }
    close(fd);

    /* TODO: Create virtual device */
}

int main(int argc, char** argv) {
    // Todo: introduce variables and setup procedures
    // Create virtual device
    // Prepare file descriptors
    int
        keyfd = -1,                     // Virtual device file descriptor
        lastKey = -1,
        timeout = IDLE_TIMEOUT,
        i;                              // Generic index
    uint32_t                            // Bitmasks for the press state
        intstate=0,                     // From events - pre-debounce
        extstate=0,                     // Last issued on virtual device
        b;                              // Single bit mask
    char
        c;                              // Storage for pin value & SYN flag
    enum pendingState
        pending = IDLE;
    struct pollfd
        p[NUM_WATCHFILES];
    sigset_t
        sigset;                         // Mask for signals to be blocked
    struct input_event
        keyEv,
        synEv;

    // TODO: Setup virtual device

    // TODO: Setup file descriptors for GPIO events and signals
    /*
    SIGNAL DETECTION
    Signals are messages that processes use to communicate with each other.
    The content of a signal is encoded by an integer between 0 and 32,
    and represented as a bitmask with the corresponding bit set high.
    By default, when a process receives a signal its execution is paused
    and a special function, called a signal handler, is executed.
    Most signal codes have pre-defined behaviors, some of which can be
    overridden. A process can also ignore certain signals: this is done
    through an internal bitmask specifying which signal codes should be
    accepted and handled immediately, while the others will be left pending
    and typically handled at a later time.
    The following approach consists roughly in blocking all incoming signals
    and storing their content in a virtual file. When changes to this file
    are detected, the signal handler is called.
    */
   // Flag all signal codes in the mask sigset
   sigfill(&sigset);
   // Add all the signal codes flagged in sigset to the internal mask
   sigprocmask(SIG_BLOCK, &sigset, NULL);
   // Generate a new file descriptor to catch all signals whose code is flagged
   // in sigset and store it in the array of pollfd p.
   p[NUM_GPIOS].fd      = signalfd(-1, &sigset, 0);
   p[NUM_GPIOS].events  = POLLIN;
    
    // Setup virtual device events
    memset(&keyEv, 0, sizeof(keyEv));
    memset(&synEv, 0, sizeof(synEv));
    keyEv.type = EV_KEY;
    synEv.type = EV_SYN;
    synEv.code = SYN_REPORT;

    if(getpgrp() == tcgetpgrp(STDOUT_FILENO)) debug = 99;

    /*
    Main loop. Starting each iteration, the state can be
    - idle:     No pending events;
    - debounce: An event has been detected, waiting to debounce;
    - repeat:   A press has occurred, waiting for repetitions.
    Poll all file descriptors for new events.
    If idle, poll indefinitely.
    If debouncing, stop and issue a virtual device event if no
    new events are detected within the set timeout.
    If repeating, stop and issue a virtual device repeat event
    if no new events are detected within the set timeout, and
    adjust the timeout as relevant.
    */
    while(running)   {
        if(poll(p, NUM_WATCHFILES, timeout))    {
            // Event detected. Find out on which file.
            for(i=0; i<NUM_GPIOS; i++)  {   // For each GPIO pin...
                if(p[i].revents)    {       // ...if an event occurred
                    lseek(p[i].fd, 0, SEEK_SET);
                    read(p[i].fd, &c, 1);
                    if(c=='0')      intstate |= (1 << i);   // Press
                    else if(c=='1') intstate &= ~(1 << i);  // Release
                    pending = DEBOUNCE;
                    timeout = DEBOUNCE_TIMEOUT;
                    p[i].revents = 0;
                }
            }
            for(; i<NUM_WATCHFILES; i++) {
                if(p[i].revents)    {
                    /* TODO: Call signal handling function */
                    p[i].revents = 0;
                }
            }
        }
        // Otherwise, a timeout occurred: Check the state
        else if(pending == DEBOUNCE)  {
            // Timeout for debouncing elapsed: Issue key events
            for(b=1, i=0; i<NUM_GPIOS; b<<=1, i++)    {
                if((extstate & b) != (intstate & b))  {
                    keyEv.code = key[i];
                    keyEv.value = ((intstate & b) > 0);
                    write(keyfd, &keyEv, sizeof(keyEv));
                    c = 1;      // Flag that a key event was issued
                    if(intstate & b)   {
                        // Press event: start repeating
                        lastKey = i;
                        pending = REPEAT_BEGIN;
                        timeout = REP1_TIMEOUT;
                        if(debug>=3)    {
                            printf("%s: Key press on GPIO%02d, code %d\n",
                            __progname, i, key[i]);
                        }
                    }
                    else    {
                        // Release event: stop repeating
                        lastKey = -1;
                        pending = IDLE;
                        timeout = IDLE_TIMEOUT;
                        if(debug >= 3)  {
                            printf("%s: Key release on GPIO%02d, code %d\n",
                            __progname, i, key[i]);
                        }
                    }
                }
            }
            extstate = intstate;
        }
        else if(lastKey >= 0)  {
            // Nothing happened since the last press: issue repeat
            keyEv.code = key[lastKey];
            keyEv.value = 2;        // Key repeat event
            write(keyfd, &keyEv, sizeof(keyEv));
            c = 1;
            if(debug >= 3)  {
                printf("%s: Key repeat on GPIO%02d, code %d\n",
                __progname, lastKey, key[lastKey]);
            }
            if(pending == REPEAT_BEGIN) {
                pending = REPEAT_CONT;
                timeout = REP2_TIMEOUT;
            }
            else if(timeout > MIN_REP_TIMEOUT)  {
                timeout -= REP_STEP;
            }
        }

        // Key events only take effect after a SYN event is issued.
        if(c)   write(keyfd, &synEv, sizeof(synEv));
    }

   /*
   TODO: Introduce deinitialize procedures.
   */
    
    return 0;
}
