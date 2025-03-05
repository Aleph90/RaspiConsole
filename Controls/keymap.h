/*!
 * Tools for mapping GPIO pins to key codes (as defined in the linux library).
 * In accordance with the terminology of `libgpiod`, GPIO pins are called
 * "lines" instead. The map is expressed as an array of pin-key pairs,
 * implemented as a `lineKeyPair` structure. The total number of pairs is
 * pre-#define'd in the `NUM_LINES` macro.
 */

#include <linux/input-event-codes.h>

// Number of GPIO pins/keys used. Edit to match your layout.
#define NUM_LINES 15

// Simple struct to hold a GPIO pin number (line) and a key code.
struct lineKeyPair  {
    unsigned int line;
    unsigned int key_code;
};

// Pin-key map. Edit to match your layout.
struct lineKeyPair line2key_dict[NUM_LINES]   =   {
    {02,    BTN_WEST},          // Action Pad
    {03,    BTN_SOUTH},         // Action Pad
    {04,    BTN_EAST},          // Action Pad
    {05,    BTN_DPAD_UP},       // Direction Pad
    {06,    BTN_DPAD_DOWN},     // Direction Pad
    {12,    BTN_SELECT},        // Select Button
    {16,    BTN_START},         // Start Button
    {17,    BTN_NORTH},         // Action Pad
    {19,    BTN_DPAD_LEFT},     // Direction Pad
    {20,    BTN_TL},            // Upper Left Trigger
    {21,    BTN_TL2},           // Lower Left Trigger
    {22,    BTN_TR2},           // Lower Right Trigger
    {24,    BTN_MODE},          // Hotkey
    {26,    BTN_DPAD_RIGHT},    // Direction Pad
    {27,    BTN_TR}             // Upper Right Trigger
};
