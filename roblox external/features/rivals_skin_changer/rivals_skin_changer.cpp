#include "rivals_skin_changer.h"
#include <Windows.h>
#include <chrono>
#include <iostream>
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "game.h"
#include "offsets.h"

namespace features {

    static std::chrono::steady_clock::time_point last_skin_change = std::chrono::steady_clock::now();
    static bool has_changed_mesh = false;

    static void write_string(uintptr_t string_addr, const std::string& str) {
        if (!string_addr || !is_valid_address(string_addr)) {
            return;
        }

        int str_len = static_cast<int>(str.length());
        std::vector<char> buffer(str_len + 1);
        std::memcpy(buffer.data(), str.c_str(), str_len);
        buffer[str_len] = '\0';

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

    void RunRivalsSkinChanger() {
        if (!rivals_skin_changer_enabled) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_skin_change).count();

        if (elapsed < 10) {
            return;
        }

        last_skin_change = now;

        try {
            if (!g_base_address) {
                return;
            }

            instance datamodel = game::ReadDatamodel(g_base_address);
            if (!datamodel.is_valid()) {
                return;
            }

            instance workspace = datamodel.read_service("Workspace");
            if (!workspace.is_valid()) {
                return;
            }

            instance viewmodels = workspace.read_child("ViewModels");
            if (!viewmodels.is_valid()) {
                return;
            }

            instance firstperson = viewmodels.read_child("FirstPerson");
            if (!firstperson.is_valid()) {
                return;
            }

            instance players_service = datamodel.read_service("Players");
            if (!players_service.is_valid()) {
                return;
            }

            instance local_player = players_service.local_player();
            if (!local_player.is_valid()) {
                return;
            }

            std::string player_name = local_player.get_name();
            if (player_name.empty()) {
                return;
            }

            std::string model_name = player_name + " - Assault Rifle - Assault Rifle";

            instance weapon_model = firstperson.read_child(model_name);
            if (!weapon_model.is_valid()) {
                return;
            }

            instance item_visual = weapon_model.read_child("ItemVisual");
            if (!item_visual.is_valid()) {
                return;
            }

            instance body = item_visual.read_child("Body");
            if (!body.is_valid()) {
                return;
            }

            auto body_children = body.get_children();
            std::string new_meshid = "rbxassetid://137502385784703";
            
            for (const auto& child : body_children) {
                std::string child_class = child.get_class_name();

                if (child_class == "MeshPart") {
                    uintptr_t meshid_addr = child.address + 0x2e8;
                    write_string(meshid_addr, new_meshid);
                }
            }

        } catch (...) {
        }
    }

}
