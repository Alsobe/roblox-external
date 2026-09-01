#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdint>

namespace process {
    bool FindRoblox(uint32_t& pid, uintptr_t& base);
    // main roblox window, used to convert screen coords into client coords
    HWND GetRobloxWindow();
}
