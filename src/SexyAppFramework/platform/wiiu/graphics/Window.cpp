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

#include <SDL.h>
#include <coreinit/debug.h>

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "widget/WidgetManager.h"

using namespace Sexy;

// Cafe OS doesn't route plain fprintf(stderr, ...) anywhere visible (it just
// vanishes), unlike every desktop-ish platform this project also targets.
// OSReport() goes through the kernel debug log, which Cemu (and devkitPPC's
// udpdebug/wiiload consoles on real hardware) do actually surface - so any
// hard failure in window/renderer setup here uses that instead.

void SexyAppBase::MakeWindow()
{
	if (mWindow)
	{
		SDL_SetWindowFullscreen((SDL_Window*)mWindow, (!mIsWindowed ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	}
	else
	{
		SDL_Init(SDL_INIT_VIDEO);

		mWindow = (void*)SDL_CreateWindow(
			mTitle.c_str(),
			SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			mWidth * IMG_DOWNSCALE, mHeight * IMG_DOWNSCALE,
			SDL_WINDOW_SHOWN);

		if (!mWindow)
		{
			OSReport("FATAL: SDL_CreateWindow failed: %s\n", SDL_GetError());
			return;
		}

		mContext = (void*)SDL_CreateRenderer((SDL_Window*)mWindow, -1,
			SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);

		if (!mContext)
		{
			OSReport("FATAL: SDL_CreateRenderer failed: %s\n", SDL_GetError());
			SDL_DestroyWindow((SDL_Window*)mWindow);
			mWindow = nullptr;
			return;
		}
	}

	if (mGLInterface == nullptr)
	{
		mGLInterface = new GLInterface(this);
		if (!InitGLInterface())
		{
			OSReport("FATAL: Failed to initialize the Wii U render backend.\n");
			delete mGLInterface;
			mGLInterface = nullptr;
			return;
		}
	}

	bool isActive = mActive;
	mActive = !!(SDL_GetWindowFlags((SDL_Window*)mWindow) & SDL_WINDOW_INPUT_FOCUS);

	mPhysMinimized = false;
	if (mMinimized)
	{
		if (mMuteOnLostFocus)
			Unmute(true);

		mMinimized = false;
		isActive = mActive; // set this here so we don't call RehupFocus again.
		RehupFocus();
	}

	if (isActive != mActive)
		RehupFocus();

	ReInitImages();

	mWidgetManager->mImage = mGLInterface->GetScreenImage();
	mWidgetManager->MarkAllDirty();

	mGLInterface->UpdateViewport();
	mWidgetManager->Resize(mScreenBounds, mGLInterface->mPresentationRect);
}
