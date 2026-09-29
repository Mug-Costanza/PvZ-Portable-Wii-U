/*
 * Portions of this file are based on the PopCap Games Framework
 * Copyright (C) 2005-2009 PopCap Games, Inc.
 *
 * Copyright (C) 2026 Zhou Qiankang <wszqkzqk@qq.com>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later AND LicenseRef-PopCap
 *
 * This file is part of PvZ-Portable.
 *
 * PvZ-Portable is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PvZ-Portable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with PvZ-Portable. If not, see <https://www.gnu.org/licenses/>.
 */

// PvZ is entirely mouse-driven, but Wii U has no mouse. The GamePad's
// touchscreen is the natural equivalent (touch = click-and-drag - the same
// way this game already plays on Android/iOS). devkitPro's SDL2 exposes the
// GamePad's buttons/sticks as a joystick and its touch panel as a
// *controller touchpad* (SDL_GameControllerGetTouchpadFinger() /
// SDL_CONTROLLERTOUCHPAD* events) - never as real SDL_MOUSEMOTION or even
// SDL_FINGERDOWN events. The shared platform/default/Input.cpp only handles
// real mouse events, so it can't drive this game on Wii U at all. This talks
// to VPAD (the Cafe OS GamePad API) directly for touch/buttons instead,
// bypassing SDL input for that part. Confirmed working against Cemu: a
// touch on the GamePad view correctly reaches VPADRead() and drives the UI.
//
// Text entry (e.g. the profile-name field) is a separate problem: Wii U has
// no keyboard either, and the real fix there is Cafe OS's on-screen
// keyboard (nn::swkbd) - available in wut, but it renders itself via
// SwkbdDrawTV/SwkbdDrawDRC, i.e. it needs its own GX2 draw calls interleaved
// with our SDL_Renderer frame, which is a real chunk of separate work. Until
// that's built, this falls back to real SDL keyboard events for text, which
// still work through Cemu (and any Wii U setup with a USB/Bluetooth
// keyboard attached) even though mouse/touch don't.

#include <SDL.h>
#include <vpad/input.h>

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "widget/WidgetManager.h"
#include "misc/KeyCodes.h"

using namespace Sexy;

// Calibrate touch reports to a fixed, known resolution so the scale factor
// below is exact, rather than relying on whatever VPADGetTPCalibratedPoint's
// implicit default happens to be.
static constexpr VPADTouchPadResolution kTouchResolution = VPAD_TP_1280X720;
static constexpr int kTouchResolutionWidth = 1280;
static constexpr int kTouchResolutionHeight = 720;

static KeyCode SDLKeyToKeyCode(SDL_Keycode theSDLKey)
{
	if (theSDLKey >= SDLK_a && theSDLKey <= SDLK_z)
		return static_cast<KeyCode>(theSDLKey - SDLK_a + 'A');

	if (theSDLKey >= SDLK_0 && theSDLKey <= SDLK_9)
		return static_cast<KeyCode>(theSDLKey);

	switch (theSDLKey)
	{
		case SDLK_BACKSPACE:    return KEYCODE_BACK;
		case SDLK_TAB:          return KEYCODE_TAB;
		case SDLK_CLEAR:        return KEYCODE_CLEAR;
		case SDLK_RETURN:       return KEYCODE_RETURN;
		case SDLK_ESCAPE:       return KEYCODE_ESCAPE;
		case SDLK_SPACE:        return KEYCODE_SPACE;
		case SDLK_DELETE:       return KEYCODE_DELETE;

		case SDLK_LEFT:         return KEYCODE_LEFT;
		case SDLK_UP:           return KEYCODE_UP;
		case SDLK_RIGHT:        return KEYCODE_RIGHT;
		case SDLK_DOWN:         return KEYCODE_DOWN;

		case SDLK_INSERT:       return KEYCODE_INSERT;
		case SDLK_HOME:         return KEYCODE_HOME;
		case SDLK_END:          return KEYCODE_END;
		case SDLK_PAGEUP:       return KEYCODE_PRIOR;
		case SDLK_PAGEDOWN:     return KEYCODE_NEXT;

		case SDLK_LSHIFT:
		case SDLK_RSHIFT:       return KEYCODE_SHIFT;
		case SDLK_LCTRL:
		case SDLK_RCTRL:        return KEYCODE_CONTROL;
		case SDLK_LALT:
		case SDLK_RALT:         return KEYCODE_MENU;
		case SDLK_PAUSE:        return KEYCODE_PAUSE;
		case SDLK_CAPSLOCK:     return KEYCODE_CAPITAL;
		case SDLK_NUMLOCKCLEAR: return KEYCODE_NUMLOCK;
		case SDLK_SCROLLLOCK:   return KEYCODE_SCROLL;

		case SDLK_KP_0:         return KEYCODE_NUMPAD0;
		case SDLK_KP_1:         return KEYCODE_NUMPAD1;
		case SDLK_KP_2:         return KEYCODE_NUMPAD2;
		case SDLK_KP_3:         return KEYCODE_NUMPAD3;
		case SDLK_KP_4:         return KEYCODE_NUMPAD4;
		case SDLK_KP_5:         return KEYCODE_NUMPAD5;
		case SDLK_KP_6:         return KEYCODE_NUMPAD6;
		case SDLK_KP_7:         return KEYCODE_NUMPAD7;
		case SDLK_KP_8:         return KEYCODE_NUMPAD8;
		case SDLK_KP_9:         return KEYCODE_NUMPAD9;
		case SDLK_KP_MULTIPLY:  return KEYCODE_MULTIPLY;
		case SDLK_KP_PLUS:      return KEYCODE_ADD;
		case SDLK_KP_MINUS:     return KEYCODE_SUBTRACT;
		case SDLK_KP_PERIOD:    return KEYCODE_DECIMAL;
		case SDLK_KP_DIVIDE:    return KEYCODE_DIVIDE;
		case SDLK_KP_ENTER:     return KEYCODE_RETURN;

		case SDLK_F1:           return KEYCODE_F1;
		case SDLK_F2:           return KEYCODE_F2;
		case SDLK_F3:           return KEYCODE_F3;
		case SDLK_F4:           return KEYCODE_F4;
		case SDLK_F5:           return KEYCODE_F5;
		case SDLK_F6:           return KEYCODE_F6;
		case SDLK_F7:           return KEYCODE_F7;
		case SDLK_F8:           return KEYCODE_F8;
		case SDLK_F9:           return KEYCODE_F9;
		case SDLK_F10:          return KEYCODE_F10;
		case SDLK_F11:          return KEYCODE_F11;
		case SDLK_F12:          return KEYCODE_F12;

		default:                return KEYCODE_UNKNOWN;
	}
}

void SexyAppBase::InitInput()
{
	VPADInit();
	if (!mMouseIn)
		mMouseIn = true;

	// Deliberately NOT calling SDL_StartTextInput() here: on both macOS and
	// SteamOS, an active text-input state can trigger the host's own
	// on-screen/accessibility keyboard UI to pop up. That should only happen
	// while an actual text field (EditWidget) is focused, via
	// StartTextInput()/StopTextInput() below - not for the whole session,
	// which would otherwise leave a keyboard overlay showing (or poppable)
	// throughout normal gameplay that never needs one.
}

bool SexyAppBase::StartTextInput(std::string& /*theInput*/)
{
	// No native on-screen keyboard wired up (see file header), so this
	// doesn't return a completed string - but it's still the right hook to
	// scope SDL_TEXTINPUT to just the lifetime of the focused field, so the
	// host's own keyboard UI only appears for actual text entry.
	SDL_StartTextInput();
	return false;
}

void SexyAppBase::StopTextInput()
{
	SDL_StopTextInput();
}

bool SexyAppBase::ProcessDeferredMessages(bool /*singleMessage*/)
{
	static bool sWasTouched = false;
	static int sLastX = 0;
	static int sLastY = 0;

	VPADStatus status;
	VPADReadError error = VPAD_READ_SUCCESS;
	VPADRead(VPAD_CHAN_0, &status, 1, &error);

	if (error == VPAD_READ_SUCCESS)
	{
		// HOME doubles as Escape (pause/back), matching the keyboard
		// shortcut this game already understands everywhere else.
		if (status.trigger & VPAD_BUTTON_HOME)
		{
			mLastUserInputTick = mLastTimerTime;
			mWidgetManager->KeyDown(KeyCode::KEYCODE_ESCAPE);
		}
		if (status.release & VPAD_BUTTON_HOME)
		{
			mLastUserInputTick = mLastTimerTime;
			mWidgetManager->KeyUp(KeyCode::KEYCODE_ESCAPE);
		}

		VPADTouchData calibrated;
		VPADGetTPCalibratedPointEx(VPAD_CHAN_0, kTouchResolution, &calibrated, &status.tpNormal);

		bool touched = calibrated.touched != 0 && calibrated.validity == VPAD_VALID;
		if (touched)
		{
			int x = (int)((int64_t)calibrated.x * mWidth / kTouchResolutionWidth);
			int y = (int)((int64_t)calibrated.y * mHeight / kTouchResolutionHeight);

			mLastUserInputTick = mLastTimerTime;
			mWidgetManager->MouseMove(x, y);
			if (!sWasTouched)
				mWidgetManager->MouseDown(x, y, 1);

			sLastX = x;
			sLastY = y;
		}
		else if (sWasTouched)
		{
			mLastUserInputTick = mLastTimerTime;
			mWidgetManager->MouseUp(sLastX, sLastY, 1);
		}
		sWasTouched = touched;
	}

	SDL_Event event;
	if (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
			case SDL_QUIT:
				CloseRequestAsync();
				break;

			case SDL_KEYDOWN:
				mLastUserInputTick = mLastTimerTime;
				mWidgetManager->KeyDown(SDLKeyToKeyCode(event.key.keysym.sym));
				break;

			case SDL_KEYUP:
				mLastUserInputTick = mLastTimerTime;
				mWidgetManager->KeyUp(SDLKeyToKeyCode(event.key.keysym.sym));
				break;

			case SDL_TEXTINPUT:
				mLastUserInputTick = mLastTimerTime;
				mWidgetManager->KeyChar((char)event.text.text[0]);
				break;
		}
	}

	return SDL_HasEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}
