/*
 * Copyright (C) 2026 Zhou Qiankang <wszqkzqk@qq.com>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
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

#include "LawnApp.h"
#include "Resources.h"
#include "Sexy.TodLib/TodStringFile.h"
#include <cstdlib>
#include <filesystem>
#include <vector>
using namespace Sexy;

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
extern "C" {
	unsigned int __stacksize__ = 512 * 1024;
}
#endif

#ifdef __IPHONEOS__
#include <SDL.h>
#include <fstream>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __wii__
#include <ogc/system.h>
#include <malloc.h>
#include <exception>

#include <csignal>
#include <unistd.h>
#include <ogc/lwp.h>
#include <tuxedo/thread.h>
#include <new>
#include "graphics/GLInterface.h"

// [wii-debug] temporary diagnostics for the silent-reset during loading

void WiiDebugMemoryBreakdown(const char* theWhen, bool theTryLock); // SexyAppBase.cpp

// Real free memory: what sbrk hasn't handed out yet in each RAM bank, plus
// free blocks inside the heap. (mallinfo's arena/in-use figures span the
// ~240 MB hole between MEM1 and MEM2, so they're not usable directly.)
static void WiiDebugMemLine(const char* theWhat)
{
	struct mallinfo mi = mallinfo();
	unsigned aMem1 = SYS_GetArena1Size(), aMem2 = SYS_GetArena2Size();
	printf("[wii-debug] mem %s: free %u KB (MEM1 unallocated %u KB, MEM2 unallocated %u KB, heap free blocks %u KB), uploads pending %u\n",
		theWhat, (aMem1 + aMem2 + (unsigned)mi.fordblks) / 1024, aMem1 / 1024, aMem2 / 1024,
		(unsigned)mi.fordblks / 1024, (unsigned)Sexy::GLInterface::PendingTextureUploads());
}

// Prints the return addresses up the PowerPC stack back-chain (each frame
// starts with a pointer to the caller's frame; the caller's saved LR sits
// 4 bytes after it). Symbolize with powerpc-eabi-addr2line -f -C -e the .elf.
static void WiiDebugBacktrace(const char* theWhat)
{
	struct mallinfo mi = mallinfo();
	printf("[wii-debug] %s on thread %p (heap arena %d, in use %d)\n",
		theWhat, (void*)LWP_GetSelf(), mi.arena, mi.uordblks);
	uint32_t* aFrame = (uint32_t*)__builtin_frame_address(0);
	for (int i = 0; i < 24 && aFrame != nullptr; i++)
	{
		uint32_t aAddr = (uint32_t)aFrame;
		if (aAddr < 0x80000000u || aAddr >= 0x94000000u || (aAddr & 3))
			break;
		uint32_t* aCaller = (uint32_t*)aFrame[0];
		if ((uint32_t)aCaller < 0x80000000u || (uint32_t)aCaller >= 0x94000000u)
			break;
		printf("[wii-debug]   #%d 0x%08x\n", i, (unsigned)aCaller[1]);
		aFrame = aCaller;
	}
}

static void WiiDebugTerminate()
{
	if (std::exception_ptr anEx = std::current_exception())
	{
		try { std::rethrow_exception(anEx); }
		catch (const std::bad_alloc&) { printf("[wii-debug] uncaught std::bad_alloc (out of memory)\n"); }
		catch (const std::exception& e) { printf("[wii-debug] uncaught exception: %s\n", e.what()); }
		catch (...) { printf("[wii-debug] uncaught non-std exception\n"); }
	}
	WiiDebugMemLine("at terminate");
	WiiDebugMemoryBreakdown("at terminate", true);
	WiiDebugBacktrace("std::terminate called");
	abort();
}

static void WiiDebugAbort(int)
{
	WiiDebugBacktrace("abort() called");
}

static void WiiDebugAtExit()
{
	WiiDebugBacktrace("exit() called");
}

// Hang detector: the main loop bumps this every iteration (SexyAppBase::UpdateApp).
volatile unsigned gWiiDebugHeartbeat = 0;
static KThread* gWiiDebugMainThread = nullptr;

// Walks a (saved) PowerPC stack back-chain starting at theSP.
static void WiiDebugWalkStack(uint32_t theSP)
{
	uint32_t* aFrame = (uint32_t*)theSP;
	for (int i = 0; i < 24; i++)
	{
		uint32_t aAddr = (uint32_t)aFrame;
		if (aAddr < 0x80000000u || aAddr >= 0x94000000u || (aAddr & 3))
			break;
		uint32_t* aCaller = (uint32_t*)aFrame[0];
		if (aCaller == nullptr || (uint32_t)aCaller < 0x80000000u || (uint32_t)aCaller >= 0x94000000u)
			break;
		printf("[wii-debug]   #%d 0x%08x\n", i, (unsigned)aCaller[1]);
		aFrame = aCaller;
	}
}

// Runs above the main thread's priority. When the main loop stops making
// progress for 3s, the main thread has been preempted by this one, so its
// registers are sitting in its KThread context: print where it's stuck.
static void* WiiDebugWatchdog(void*)
{
	unsigned aLast = gWiiDebugHeartbeat;
	int aStalledMs = 0;
	int aReports = 0;
	for (int aTick = 0;; aTick++)
	{
		usleep(500 * 1000);
		if (aTick % 4 == 0)
			WiiDebugMemLine("periodic");
		unsigned aNow = gWiiDebugHeartbeat;
		if (aNow != aLast)
		{
			aLast = aNow;
			aStalledMs = 0;
			aReports = 0;
			continue;
		}
		aStalledMs += 500;
		if (aStalledMs < 3000 || aReports >= 3)
			continue;
		aReports++;
		const PPCContext& c = gWiiDebugMainThread->ctx;
		printf("[wii-debug] main loop stalled %d ms (sample %d, thread state %d): pc 0x%08x lr 0x%08x sp 0x%08x\n",
			aStalledMs, aReports, gWiiDebugMainThread->state, (unsigned)c.pc, (unsigned)c.lr, (unsigned)c.gpr[1]);
		WiiDebugWalkStack(c.gpr[1]);
	}
	return nullptr;
}
#endif

bool (*gAppCloseRequest)();
bool (*gAppHasUsedCheatKeys)();
std::string (*gGetCurrentLevelName)();

#ifdef _WIN32
static std::vector<std::string> gUtf8ArgsStorage;
static std::vector<char*> gUtf8Argv;

static void BuildUtf8ArgsFromWin32(int& argc, char**& argv)
{
	int aWideArgc = 0;
	LPWSTR* aWideArgv = CommandLineToArgvW(GetCommandLineW(), &aWideArgc);
	if (aWideArgv == nullptr || aWideArgc <= 0)
		return;

	gUtf8ArgsStorage.clear();
	gUtf8Argv.clear();
	gUtf8ArgsStorage.reserve(static_cast<size_t>(aWideArgc));
	gUtf8Argv.reserve(static_cast<size_t>(aWideArgc));

	for (int i = 0; i < aWideArgc; ++i)
	{
		const wchar_t* aWide = aWideArgv[i];
		int aLen = WideCharToMultiByte(CP_UTF8, 0, aWide, -1, nullptr, 0, nullptr, nullptr);
		if (aLen <= 0)
		{
			gUtf8ArgsStorage.emplace_back();
		}
		else
		{
			std::string aUtf8;
			aUtf8.resize(static_cast<size_t>(aLen - 1));
			WideCharToMultiByte(CP_UTF8, 0, aWide, -1, aUtf8.data(), aLen, nullptr, nullptr);
			gUtf8ArgsStorage.emplace_back(std::move(aUtf8));
		}
	}

	for (auto& aStr : gUtf8ArgsStorage)
		gUtf8Argv.push_back(const_cast<char*>(aStr.c_str()));

	argc = static_cast<int>(gUtf8Argv.size());
	argv = gUtf8Argv.data();

	LocalFree(aWideArgv);
}
#endif

int main(int argc, char** argv)
{
#ifdef __3DS__
	osSetSpeedupEnable(true);
#endif

#ifdef __wii__
	// Route stdout/stderr to Dolphin's OSReport log (View > Show Log, enable
	// the "OSReport EXI" type); without this printf output goes nowhere.
	SYS_STDIO_Report(true);
	setvbuf(stdout, nullptr, _IONBF, 0);
	std::set_terminate(WiiDebugTerminate);
	signal(SIGABRT, WiiDebugAbort);
	atexit(WiiDebugAtExit);
	{
		gWiiDebugMainThread = KThreadGetSelf();
		static lwp_t sWatchdog;
		LWP_CreateThread(&sWatchdog, WiiDebugWatchdog, nullptr, nullptr, 64 * 1024, 120);
	}
#endif

#ifdef _WIN32
	BuildUtf8ArgsFromWin32(argc, argv);
#endif

#ifdef __IPHONEOS__
	bool aHasGameResources = false;
	std::filesystem::path aDocsPath;
	const char* aHome = std::getenv("HOME");
	if (aHome != nullptr && aHome[0] != '\0')
	{
		aDocsPath = std::filesystem::path(aHome) / "Documents";
		aHasGameResources = std::filesystem::is_regular_file(aDocsPath / "main.pak") &&
			std::filesystem::is_directory(aDocsPath / "properties");
	}

	if (!aHasGameResources)
	{
		const std::filesystem::path aReadmePath = aDocsPath / "README.txt";
		if (!aDocsPath.empty() && !std::filesystem::exists(aReadmePath))
		{
			std::ofstream(aReadmePath, std::ios::out | std::ios::trunc)
				<< "Place your `main.pak` and `properties/` folder here to play the game.\n";
		}

		SDL_Init(SDL_INIT_VIDEO);
		SDL_ShowSimpleMessageBox(
			SDL_MESSAGEBOX_ERROR,
			"Resources Not Found",
			"Please place main.pak and the properties/ folder into the "
			"PvZ Portable folder using the Files app or Finder/iTunes file sharing.\n\n"
			"The app will now exit.",
			NULL
		);
		SDL_Quit();
		return 1;
	}
#endif

	TodStringListSetColors(gLawnStringFormats, gLawnStringFormatCount);
	gGetCurrentLevelName = LawnGetCurrentLevelName;
	gAppCloseRequest = LawnGetCloseRequest;
	gAppHasUsedCheatKeys = LawnHasUsedCheatKeys;
	gExtractResourcesByName = Sexy::ExtractResourcesByName;
	gLawnApp = new LawnApp();
	gLawnApp->SetArgs(argc, argv);
	gLawnApp->Init();
	gLawnApp->Start();
#ifdef __wii__
	printf("[wii-debug] Start() returned (mShutdown=%d)\n", (int)gLawnApp->mShutdown);
#endif
#ifndef __EMSCRIPTEN__
	gLawnApp->Shutdown();
	if (gLawnApp)
		delete gLawnApp;
#endif

#ifdef __wii__
	// Returning from main makes libogc jump to the loader stub the Homebrew
	// Channel leaves at 0x80001800. Launched any other way (a forwarder
	// channel, or Dolphin booting the .dol directly) there's no stub and it
	// jumps into empty memory, so go back to the Wii Menu instead.
	if (memcmp((const void*)0x80001804, "STUBHAXX", 8) != 0)
		SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
#endif

	return 0;
};
