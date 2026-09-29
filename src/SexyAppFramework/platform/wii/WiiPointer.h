#ifndef __WIIPOINTER_H__
#define __WIIPOINTER_H__

// True while any Wiimote's IR pointer is on screen (SDL then drives the cursor
// and the Wiimote's B/A clicks itself). Kept in its own translation unit: the
// libogc/wiiuse headers clash with engine names like Sexy::Color.
bool WiiAnyPointerOnScreen();

// True when Wiimote theChannel (0-3) is being used held sideways (NES
// style): connected, nothing plugged into the extension port, and its IR
// pointer not on screen. The accelerometer can't tell a sideways grip from
// an upright one (it's a turn about the vertical axis), so this follows the
// usual Wii convention instead; pointing at the screen switches back.
bool WiiRemoteIsSideways(int theChannel);

#endif // __WIIPOINTER_H__
