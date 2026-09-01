#include "process.h"
#include "memory.h"

namespace process {
    bool FindRoblox(uint32_t& pid, uintptr_t& base) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return false;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);

        if (!Process32FirstW(snap, &pe)) {
            CloseHandle(snap);
            return false;
        }

        do {
            if (!_wcsicmp(pe.szExeFile, L"RobloxPlayerBeta.exe") ||
                !_wcsicmp(pe.szExeFile, L"Roblox.exe")) {

                HANDLE modSnap = CreateToolhelp32Snapshot(
                    TH32CS_SNAPMODULE, pe.th32ProcessID);

                if (modSnap == INVALID_HANDLE_VALUE)
                    continue;

                MODULEENTRY32W me{};
                me.dwSize = sizeof(me);

                if (Module32FirstW(modSnap, &me)) {
                    do {
                        if (!_wcsicmp(me.szModule, L"RobloxPlayerBeta.exe") ||
                            !_wcsicmp(me.szModule, L"Roblox.exe")) {
                            pid = pe.th32ProcessID;
                            base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                            CloseHandle(modSnap);
                            CloseHandle(snap);
                            return true;
                        }
                    } while (Module32NextW(modSnap, &me));
                }

                CloseHandle(modSnap);
            }
        } while (Process32NextW(snap, &pe));

        CloseHandle(snap);
        return false;
    }
}

namespace process {
    struct FindData { DWORD pid; HWND result; };

    static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lparam) {
        FindData* d = reinterpret_cast<FindData*>(lparam);
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid == d->pid && GetWindow(hwnd, GW_OWNER) == nullptr && IsWindowVisible(hwnd)) {
            d->result = hwnd;
            return FALSE;
        }
        return TRUE;
    }

    HWND GetRobloxWindow() {
        DWORD pid = (DWORD)mem::process_id.load();
        if (!pid) return nullptr;
        FindData d{ pid, nullptr };
        EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&d));
        return d.result;
    }
}
