#include <Windows.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <chrono>
#include "skybox_changer.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "game.h"

namespace features {

    static bool had_players = false;
    static bool skybox_written = false;
    static int last_skybox_type = -1;
    static uintptr_t last_sky_address = 0;
    static uint64_t last_place_id = 0;
    static std::chrono::steady_clock::time_point place_change_time;
    static bool place_just_changed = false;

    struct AllocatedFace {
        uintptr_t string_addr;
        uintptr_t data_addr;
    };
    static std::vector<AllocatedFace> our_allocations;

    struct SkyboxPreset {
        const char* name;
        const char* up;
        const char* rt;
        const char* lf;
        const char* ft;
        const char* bk;
        const char* dn;
    };

    static const SkyboxPreset k_skybox_presets[] = {
        {"Piss", "rbxassetid://2651437350", "rbxassetid://2651436979", "rbxassetid://2651436494", "rbxassetid://2651435990", "rbxassetid://2651432901", "rbxassetid://2651434974"},
        {"Peach", "rbxassetid://566616187", "rbxassetid://566616082", "rbxassetid://566616044", "rbxassetid://566616141", "rbxassetid://566616113", "rbxassetid://566616232"},
        {"Saku", "http://www.roblox.com/asset/?id=271077958", "http://www.roblox.com/asset/?id=271042467", "http://www.roblox.com/asset/?id=271042310", "http://www.roblox.com/asset/?id=271042556", "http://www.roblox.com/asset/?id=271042516", "http://www.roblox.com/asset/?id=271077243"},
        {"Purple", "http://www.roblox.com/asset/?id=570557727", "http://www.roblox.com/asset/?id=570557672", "http://www.roblox.com/asset/?id=570557620", "http://www.roblox.com/asset/?id=570557559", "http://www.roblox.com/asset/?id=570557514", "http://www.roblox.com/asset/?id=570557775"},
        {"Retro", "rbxassetid://18164890128", "rbxassetid://18164873920", "rbxassetid://18164877945", "rbxassetid://18164870251", "rbxassetid://18164881924", "rbxassetid://18166113875"},
        {"Space", "rbxassetid://15983964246", "rbxassetid://15983966246", "rbxassetid://15983967420", "rbxassetid://15983965025", "rbxassetid://15983968922", "rbxassetid://15983966825"},
        {"Sea", "http://www.roblox.com/asset/?id=321846070", "http://www.roblox.com/asset/?id=321846207", "http://www.roblox.com/asset/?id=321846162", "http://www.roblox.com/asset/?id=321845951", "http://www.roblox.com/asset/?id=321846018", "http://www.roblox.com/asset/?id=321846104"},
        {"Night V2", "http://www.roblox.com/asset/?id=12064131", "http://www.roblox.com/asset/?id=12064115", "http://www.roblox.com/asset/?id=12063984", "http://www.roblox.com/asset/?id=12064121", "http://www.roblox.com/asset/?id=12064107", "http://www.roblox.com/asset/?id=12064152"},
        {"Dark", "rbxassetid://15470160563", "rbxassetid://15470158022", "rbxassetid://15470155938", "rbxassetid://15470153860", "rbxassetid://15470149279", "rbxassetid://15470151245"},
        {"Anime", "http://www.roblox.com/asset/?id=104038404823203", "http://www.roblox.com/asset/?id=99961685452126", "http://www.roblox.com/asset/?id=84924000207295", "http://www.roblox.com/asset/?id=95687237979398", "http://www.roblox.com/asset/?id=81858382098344", "http://www.roblox.com/asset/?id=138472117789684"},
        {"Beach", "http://www.roblox.com/asset/?id=151165227", "http://www.roblox.com/asset/?id=151165206", "http://www.roblox.com/asset/?id=151165191", "http://www.roblox.com/asset/?id=151165224", "http://www.roblox.com/asset/?id=151165214", "http://www.roblox.com/asset/?id=151165197"},
        {"Space V2", "http://www.roblox.com/asset/?id=16262366016", "http://www.roblox.com/asset/?id=16262363873", "http://www.roblox.com/asset/?id=16262362003", "http://www.roblox.com/asset/?id=16262360469", "http://www.roblox.com/asset/?id=16262356578", "http://www.roblox.com/asset/?id=16262358026"},
        {"Pink", "rbxassetid://12635316856", "rbxassetid://12635315817", "rbxassetid://12635313718", "rbxassetid://12635312870", "rbxassetid://12635309703", "rbxassetid://12635311686"},
        {"Rainbow", "rbxassetid://12877083856", "rbxassetid://12877085497", "rbxassetid://12877085497", "rbxassetid://12877085497", "rbxassetid://12877085497", "rbxassetid://12877086914"},
        {"Forest", "http://www.roblox.com/asset/?id=237593929", "http://www.roblox.com/asset/?id=237593835", "http://www.roblox.com/asset/?id=237593861", "http://www.roblox.com/asset/?id=237593922", "http://www.roblox.com/asset/?id=237593887", "http://www.roblox.com/asset/?id=237593849"},
        {"Night", "http://www.roblox.com/asset/?id=154185031", "http://www.roblox.com/asset/?id=154184972", "http://www.roblox.com/asset/?id=154184943", "http://www.roblox.com/asset/?id=154185021", "http://www.roblox.com/asset/?id=154185004", "http://www.roblox.com/asset/?id=154184960"},
        {"Lava", "http://www.roblox.com/asset/?id=4776130793", "http://www.roblox.com/asset/?id=4776133150", "http://www.roblox.com/asset/?id=4776128425", "http://www.roblox.com/asset/?id=4776131365", "http://www.roblox.com/asset/?id=4776124334", "http://www.roblox.com/asset/?id=4776125375"},
        {"Rainy", "http://www.roblox.com/asset/?id=4495867486", "http://www.roblox.com/asset/?id=4495866584", "http://www.roblox.com/asset/?id=4495866035", "http://www.roblox.com/asset/?id=4495865458", "http://www.roblox.com/asset/?id=4495864450", "http://www.roblox.com/asset/?id=4495864887"},
        {"Green", "rbxassetid://566611218", "rbxassetid://566611300", "rbxassetid://566611266", "rbxassetid://566611142", "rbxassetid://566611187", "rbxassetid://566613198"},
        {"Volcanic", "http://www.roblox.com/asset/?id=150281471", "http://www.roblox.com/asset/?id=150281426", "http://www.roblox.com/asset/?id=150281400", "http://www.roblox.com/asset/?id=150281461", "http://www.roblox.com/asset/?id=150281446", "http://www.roblox.com/asset/?id=150281418"},
        {"Minecraft", "http://www.roblox.com/asset/?id=8735166729", "http://www.roblox.com/asset/?id=8735166751", "http://www.roblox.com/asset/?id=8735166755", "http://www.roblox.com/asset/?id=8735231668", "rbxassetid://8735166756", "http://www.roblox.com/asset/?id=8735166707"},
        {"Lucid", "rbxassetid://8508112781", "rbxassetid://8508111092", "rbxassetid://8508107681", "rbxassetid://8508104949", "rbxassetid://8508098796", "rbxassetid://8508103588"},
        {"Nebulous", "rbxassetid://131036626982613", "rbxassetid://103716549795832", "rbxassetid://126542804346203", "rbxassetid://107665368823185", "rbxassetid://95020137072033", "rbxassetid://92862258103959"}
    };

    const int k_skybox_count = sizeof(k_skybox_presets) / sizeof(k_skybox_presets[0]);

    const char* const k_skybox_names[] = {
        "Piss", "Peach", "Saku", "Purple", "Retro", "Space", "Sea", "Night V2",
        "Dark", "Anime", "Beach", "Space V2", "Pink", "Rainbow", "Forest", "Night",
        "Lava", "Rainy", "Green", "Volcanic", "Minecraft", "Lucid", "Nebulous"
    };

    static void cleanup_allocations() {
        our_allocations.clear();
    }

    static bool write_string_safe(uintptr_t string_addr, const std::string& str) {
        if (!string_addr || !is_valid_address(string_addr)) return false;

        int str_len = static_cast<int>(str.length());
        if (str_len == 0) return false;

        if (!is_valid_address(string_addr + 0x18)) return false;
        if (!is_valid_address(string_addr + Offsets::Misc::StringLength)) return false;

        int current_capacity = read<int>(string_addr + 0x18);

        char* buffer = new (std::nothrow) char[str_len + 1];
        if (!buffer) return false;

        std::memset(buffer, 0, str_len + 1);
        std::memcpy(buffer, str.c_str(), static_cast<size_t>(str_len));

        bool success = false;

        if (str_len < 16) {
            write_raw(string_addr, buffer, static_cast<size_t>(str_len) + 1);
            write<int>(string_addr + Offsets::Misc::StringLength, str_len);
            write<int>(string_addr + 0x18, 15);
            success = true;
        }
        else {
            uintptr_t data_addr = 0;

            if (current_capacity >= str_len) {
                data_addr = read<uintptr_t>(string_addr);
            }
            else {
                for (const auto& alloc : our_allocations) {
                    if (alloc.string_addr == string_addr && alloc.data_addr) {
                        data_addr = alloc.data_addr;
                        break;
                    }
                }

                if (!data_addr) {
                    HANDLE process = mem::roblox_h.load();
                    if (process) {
                        data_addr = reinterpret_cast<uintptr_t>(
                            VirtualAllocEx(process, nullptr, str_len + 1,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
                            );

                        if (data_addr) {
                            our_allocations.push_back({ string_addr, data_addr });
                        }
                    }
                }
            }

            if (data_addr && is_valid_address(data_addr)) {
                write_raw(data_addr, buffer, static_cast<size_t>(str_len) + 1);
                write<uintptr_t>(string_addr, data_addr);
                write<int>(string_addr + Offsets::Misc::StringLength, str_len);
                write<int>(string_addr + 0x18, str_len > current_capacity ? str_len : current_capacity);
                success = true;
            }
        }

        delete[] buffer;
        return success;
    }

    void RunSkyboxChanger() {
        if (!skybox_changer_enabled) {
            cleanup_allocations();
            skybox_written = false;
            had_players = false;
            last_skybox_type = -1;
            last_sky_address = 0;
            last_place_id = 0;
            place_just_changed = false;
            return;
        }

        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid()) {
            std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "FAIL: DataModel invalid");
            had_players = false;
            skybox_written = false;
            last_sky_address = 0;
            return;
        }

        uint64_t current_place_id = game::GetPlaceId(g_base_address);
        if (current_place_id != last_place_id) {
            if (last_place_id != 0 && current_place_id != 0) {
                std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "Place changed: %llu -> %llu", last_place_id, current_place_id);
                cleanup_allocations();
                place_change_time = std::chrono::steady_clock::now();
                place_just_changed = true;
                skybox_written = false;
                last_skybox_type = -1;
                last_sky_address = 0;
            }
            last_place_id = current_place_id;
        }

        if (place_just_changed) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - place_change_time
            ).count();

            if (elapsed < 2) {
                std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "Waiting after place change... (%lld/2s)", elapsed);
                return;
            }
            place_just_changed = false;
        }

        instance players_service = dm.read_service("Players");
        if (!players_service.is_valid()) {
            std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "Waiting for Players...");
            had_players = false;
            skybox_written = false;
            last_sky_address = 0;
            return;
        }

        auto players = players_service.get_children();
        bool has_players = !players.empty();

        if (!has_players) {
            std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "Waiting for players to join...");
            had_players = false;
            skybox_written = false;
            last_sky_address = 0;
            return;
        }

        bool type_changed = (skybox_type != last_skybox_type);
        bool players_returned = (has_players && !had_players);

        if (skybox_written && !type_changed && !players_returned && last_sky_address != 0) {
            std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "Skybox active (%s)", k_skybox_names[skybox_type]);
            had_players = has_players;
            return;
        }

        had_players = has_players;

        std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "DM ok");
        instance lighting = dm.read_service("Lighting");
        if (!lighting.is_valid()) {
            std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "FAIL: Lighting not found");
            skybox_written = false;
            last_sky_address = 0;
            return;
        }

        std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "Lighting ok");
        instance sky = read<instance>(lighting.address + Offsets::Lighting::Sky);

        if (!sky.is_valid()) {
            auto lighting_children = lighting.get_children();
            bool found_sky = false;
            for (const instance& child : lighting_children) {
                if (!child.is_valid()) continue;
                std::string class_name = child.get_class_name();
                if (class_name == "Sky") {
                    sky = child;
                    found_sky = true;
                    break;
                }
            }

            if (!found_sky) {
                std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "No Sky in game");
                skybox_written = false;
                last_sky_address = 0;
                return;
            }
        }

        if (!sky.is_valid() || !is_valid_address(sky.address)) {
            std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "Sky address invalid");
            skybox_written = false;
            last_sky_address = 0;
            return;
        }

        if (last_sky_address != 0 && last_sky_address != sky.address) {
            std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "New Sky detected, reapplying...");
            skybox_written = false;
            last_skybox_type = -1;
        }
        last_sky_address = sky.address;

        std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "Sky ok 0x%zX", (size_t)sky.address);
        uintptr_t ve = read<uintptr_t>(g_base_address + Offsets::VisualEngine::Pointer);
        if (!ve || !is_valid_address(ve)) {
            std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "FAIL: VisualEngine invalid (ve=0x%zX)", (size_t)ve);
            skybox_written = false;
            return;
        }

        std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "VE 0x%zX", (size_t)ve);
        uintptr_t render_view = read<uintptr_t>(ve + Offsets::VisualEngine::RenderView);
        if (!render_view || !is_valid_address(render_view)) {
            std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "FAIL: RenderView invalid (rv=0x%zX)", (size_t)render_view);
            skybox_written = false;
            return;
        }

        std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "RV 0x%zX", (size_t)render_view);

        int preset_idx = skybox_type;
        if (preset_idx < 0 || preset_idx >= k_skybox_count) preset_idx = 0;
        const SkyboxPreset& preset = k_skybox_presets[preset_idx];

        uintptr_t face_addrs[6] = {
            sky.address + Offsets::Sky::SkyboxUp,
            sky.address + Offsets::Sky::SkyboxDn,
            sky.address + Offsets::Sky::SkyboxFt,
            sky.address + Offsets::Sky::SkyboxBk,
            sky.address + Offsets::Sky::SkyboxLf,
            sky.address + Offsets::Sky::SkyboxRt
        };

        for (int i = 0; i < 6; i++) {
            if (!is_valid_address(face_addrs[i])) {
                std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "FAIL: Invalid face address %d", i);
                skybox_written = false;
                return;
            }
        }

        if (is_valid_address(render_view + Offsets::RenderView::SkyValid)) {
            write<bool>(render_view + Offsets::RenderView::SkyValid, false);
        }
        if (is_valid_address(render_view + Offsets::RenderView::LightingValid)) {
            write<bool>(render_view + Offsets::RenderView::LightingValid, false);
        }

        int failed_count = 0;

        if (!write_string_safe(face_addrs[0], preset.up)) failed_count++;
        if (!write_string_safe(face_addrs[1], preset.dn)) failed_count++;
        if (!write_string_safe(face_addrs[2], preset.ft)) failed_count++;
        if (!write_string_safe(face_addrs[3], preset.bk)) failed_count++;
        if (!write_string_safe(face_addrs[4], preset.lf)) failed_count++;
        if (!write_string_safe(face_addrs[5], preset.rt)) failed_count++;

        if (failed_count > 0) {
            std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "FAIL: %d faces failed to write", failed_count);
            skybox_written = false;
            return;
        }

        skybox_written = true;
        std::snprintf(skybox_debug_msg, sizeof(skybox_debug_msg), "Skybox applied (%s)", k_skybox_names[skybox_type]);

        if (is_valid_address(render_view + Offsets::RenderView::SkyValid)) {
            write<bool>(render_view + Offsets::RenderView::SkyValid, true);
        }
        if (is_valid_address(render_view + Offsets::RenderView::LightingValid)) {
            write<bool>(render_view + Offsets::RenderView::LightingValid, true);
        }

        last_skybox_type = skybox_type;
    }
}