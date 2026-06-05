#include "rage.h"
#include "globals.h"
#include "memory.h"
#include "offsets.h"
#include "game.h"
#include <Windows.h>
#include <atomic>
#include <iostream>
#include <vector>
#include <cstring>

namespace features {
namespace rage {

static std::atomic<bool> g_running{ false };
static HANDLE g_thread = nullptr;

static void write_string(uintptr_t string_addr, const std::string& str) {
    if (!string_addr || !is_valid_address(string_addr)) {
        return;
    }

    int str_len = static_cast<int>(str.length());
    std::vector<char> buffer(str_len + 1);
    std::memcpy(buffer.data(), str.c_str(), str_len);
    buffer[str_len] = '\0';

    // Roblox uses SSO (Small String Optimization)
    if (str_len < 16) {
        write_raw(string_addr, buffer.data(), static_cast<size_t>(str_len) + 1);
        write<int>(string_addr + Offsets::Misc::StringLength, str_len);
        write<int>(string_addr + 0x18, 15);
    } else {
        int current_capacity = read<int>(string_addr + 0x18);
        uintptr_t data_addr = 0;

        if (current_capacity >= str_len) {
            data_addr = read<uintptr_t>(string_addr);
        } else {
            data_addr = reinterpret_cast<uintptr_t>(VirtualAllocEx(
                mem::roblox_h.load(), 
                nullptr, 
                str_len + 1, 
                MEM_COMMIT | MEM_RESERVE, 
                PAGE_READWRITE
            ));
        }

        if (data_addr && is_valid_address(data_addr)) {
            write_raw(data_addr, buffer.data(), static_cast<size_t>(str_len) + 1);
            write<uintptr_t>(string_addr, data_addr);
            write<int>(string_addr + Offsets::Misc::StringLength, str_len);
            write<int>(string_addr + 0x18, str_len > current_capacity ? str_len : current_capacity);
        }
    }
}

static void korblox_thread() {
    while (g_running.load()) {
        if (!korblox_enabled) {
            Sleep(100);
            continue;
        }
        if (!g_base_address) {
            Sleep(100);
            continue;
        }
        
        try {
            instance dm = game::ReadDatamodel(g_base_address);
            if (!dm.is_valid()) {
                Sleep(100);
                continue;
            }
            
            instance players_service = dm.read_service("Players");
            if (!players_service.is_valid()) {
                Sleep(100);
                continue;
            }
            
            instance local = players_service.local_player();
            if (!local.is_valid()) {
                Sleep(100);
                continue;
            }
            
            instance modelinstance = local.model_instance();
            if (!modelinstance.is_valid()) {
                Sleep(100);
                continue;
            }
            
            // Get the leg parts
            instance right_foot = modelinstance.read_child("RightFoot");
            instance right_upper_leg = modelinstance.read_child("RightUpperLeg");
            instance right_lower_leg = modelinstance.read_child("RightLowerLeg");
            
            // Set transparency for RightFoot and RightLowerLeg
            if (right_foot.is_valid() && is_valid_address(right_foot.address)) {
                write<float>(right_foot.address + Offsets::BasePart::Transparency, 1.0f);
            }
            
            if (right_lower_leg.is_valid() && is_valid_address(right_lower_leg.address)) {
                write<float>(right_lower_leg.address + Offsets::BasePart::Transparency, 1.0f);
            }
            
            // Change RightUpperLeg MeshId to Korblox
            if (right_upper_leg.is_valid() && is_valid_address(right_upper_leg.address)) {
                // Set transparency to 0.0 (visible)
                write<float>(right_upper_leg.address + Offsets::BasePart::Transparency, 0.0f);
                
                // RightUpperLeg itself is a MeshPart, change its MeshId directly
                // MeshPart::MeshId is at offset 0x2e8
                uintptr_t meshid_addr = right_upper_leg.address + Offsets::MeshPart::MeshId;
                std::string korblox_mesh = "https://assetdelivery.roblox.com/v1/asset/?id=9598310133";
                write_string(meshid_addr, korblox_mesh);
            }
        } catch (...) {
            // Silently continue on error
        }
        
        Sleep(100);
    }
}

static DWORD WINAPI thread_proc(LPVOID) {
    korblox_thread();
    return 0;
}

void Initialize() {
    if (g_running.load()) return;
    g_running.store(true);
    g_thread = CreateThread(nullptr, 0, thread_proc, nullptr, 0, nullptr);
}

void Shutdown() {
    g_running.store(false);
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

}
}
