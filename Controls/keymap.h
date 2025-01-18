#include <linux/input-event-codes.h>

#define NUM_LINES 15

// Temporary key map: the sepcific arrangement of
// action/directional buttons may be changed later.

struct lineKeyPair  {
    unsigned int line;
    unsigned int key_code;
};

struct lineKeyPair line2key[NUM_LINES]   =   {
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
