#include <Windows.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include "imgui/imgui.h"
#include "globals.h"
#include "memory.h"
#include "game.h"
#include "cache.h"
#include "offsets.h"
#include "features/skybox_changer/skybox_changer.h"
#include "features/config/config.h"

static int* s_waiting_key_ptr = nullptr;

static const char* KeyName(int key) {
    if (key == 0) return "none";
    if (key == VK_LBUTTON) return "lmb";
    if (key == VK_RBUTTON) return "rmb";
    if (key == VK_MBUTTON) return "mmb";
    if (key == VK_XBUTTON1) return "mouse4";
    if (key == VK_XBUTTON2) return "mouse5";
    LONG lp = (MapVirtualKeyA(key, 0) << 16) | 1;
    char buf[64]{};
    if (GetKeyNameTextA(lp, buf, sizeof(buf)) > 0) return buf;
    return "???";
}

void TickKeybinds() {
    if (!s_waiting_key_ptr) return;

    for (int vk = 1; vk < 256; ++vk) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON) continue;
        if (GetAsyncKeyState(vk) & 0x8000) {
            while (GetAsyncKeyState(vk) & 0x8000) Sleep(1);
            *s_waiting_key_ptr = vk;
            s_waiting_key_ptr = nullptr;
            return;
        }
    }

    POINT pt;
    GetCursorPos(&pt);
    HWND hw = WindowFromPoint(pt);
    if (hw) {
        WPARAM wp = 0;
        if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) wp = XBUTTON1;
        else if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) wp = XBUTTON2;
        if (wp) {
            while ((GetAsyncKeyState(VK_XBUTTON1) & 0x8000) || (GetAsyncKeyState(VK_XBUTTON2) & 0x8000)) Sleep(1);
            *s_waiting_key_ptr = (wp == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            s_waiting_key_ptr = nullptr;
            return;
        }
    }
}

static bool keybind_button(const char* label, int& key) {
    if (s_waiting_key_ptr == &key) {
        if (ImGui::Button("...", ImVec2(-1, 0))) s_waiting_key_ptr = nullptr;
        return false;
    }
    char buf[64];
    sprintf_s(buf, "%s: [%s]", label, KeyName(key));
    if (ImGui::Button(buf, ImVec2(-1, 0))) { s_waiting_key_ptr = &key; return true; }
    return false;
}

void RenderMenu() {
    ImGui::Begin("roblox external");
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("aimbot")) {
            ImGui::Checkbox("enabled", &aimbot_enabled);
            ImGui::Combo("aim type", &aimbot_aim_type, "camera\0mouse\0");
            ImGui::Combo("target bone", &aimbot_part, "head\0upper torso\0lower torso\0left hand\0right hand\0left foot\0right foot\0");
            ImGui::Checkbox("sticky aim", &sticky_aim);
            ImGui::Checkbox("prediction", &prediction_enabled);
            if (prediction_enabled) {
                ImGui::SliderFloat("pred x", &prediction_x, 1.0f, 50.0f);
                ImGui::SliderFloat("pred y", &prediction_y, 1.0f, 50.0f);
            }
            ImGui::SliderFloat("smooth x", &smoothing_x, 2.0f, 20.0f, "%.1f");
            ImGui::SliderFloat("smooth y", &smoothing_y, 2.0f, 20.0f, "%.1f");
            ImGui::SliderFloat("fov size", &fov_size, 10.0f, 500.0f);
            ImGui::Checkbox("show fov", &show_fov);
            keybind_button("aimbot key", aimbot_keybind);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("esp")) {
            ImGui::Checkbox("enabled", &esp_enabled);
            ImGui::Separator();
            ImGui::Checkbox("box", &box_esp);
            if (box_esp) {
                ImGui::Combo("box style", &box_esp_type, "full\0corners\0");
                ImGui::Checkbox("fill", &box_fill);
                if (box_fill) {
                    ImGui::Checkbox("gradient", &box_fill_gradient);
                    if (box_fill_gradient) {
                        ImGui::Checkbox("rotate", &box_fill_gradient_rotate);
                        ImGui::ColorEdit4("fill top", box_fill_top);
                        ImGui::ColorEdit4("fill bottom", box_fill_bottom);
                    }
                    ImGui::ColorEdit4("fill color", box_fill_top);
                }
                ImGui::ColorEdit4("box color", box_esp_color);
            }
            ImGui::Checkbox("health bar", &healthbar);
            if (healthbar) ImGui::ColorEdit4("health bar color", healthbar_color);
            ImGui::Checkbox("health text", &health_text);
            if (health_text) ImGui::ColorEdit4("health text color", health_text_color);
            ImGui::Checkbox("name", &name);
            if (name) ImGui::ColorEdit4("name color", name_color);
            ImGui::Checkbox("distance", &distance);
            if (distance) ImGui::ColorEdit4("distance color", distance_color);
            ImGui::Checkbox("rig type", &rig_type);
            if (rig_type) ImGui::ColorEdit4("rig type color", rig_type_color);
            ImGui::Checkbox("tool", &tool_esp);
            if (tool_esp) ImGui::ColorEdit4("tool color", tool_color);
            ImGui::SliderFloat("render dist", &esp_render_distance, 0.0f, 2000.0f, "%.0f");
            ImGui::Checkbox("team check", &team_check);
            ImGui::Separator();
            ImGui::Checkbox("skeleton", &skeleton_esp);
            if (skeleton_esp) ImGui::ColorEdit4("skeleton color", skeleton_color);
            ImGui::Checkbox("aim viewer", &aimviewer);
            ImGui::Checkbox("china hat", &chinahat);
            if (chinahat) ImGui::ColorEdit4("hat color", chinahat_color);
            ImGui::Separator();
            ImGui::Checkbox("3d esp preview", &esp_preview_3d);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("chams")) {
            ImGui::Checkbox("chams", &chams_enabled);
            if (chams_enabled) ImGui::ColorEdit4("chams color", chams_color);
            ImGui::Separator();
            ImGui::Checkbox("mesh chams", &mesh_chams_enabled);
            if (mesh_chams_enabled) {
                ImGui::ColorEdit4("mesh color", mesh_chams_color);
                ImGui::Checkbox("union (2d)", &union_chams);
                ImGui::Checkbox("outline", &outline_chams);
                if (outline_chams) ImGui::ColorEdit4("outline color", outline_chams_color);
            }
            ImGui::Separator();
            ImGui::Checkbox("memory mesh chams", &memory_mesh_chams_enabled);
            if (memory_mesh_chams_enabled) {
                ImGui::ColorEdit4("mem chams color", memory_mesh_chams_color);
                ImGui::Checkbox("mem union", &memory_union_chams);
                ImGui::Checkbox("mem outline", &memory_outline_chams);
                if (memory_outline_chams) ImGui::ColorEdit4("mem outline color", memory_outline_chams_color);
            }
            ImGui::Separator();
            ImGui::Checkbox("expanded hitbox", &render_expanded_hitbox);
            if (render_expanded_hitbox) {
                ImGui::Checkbox("hitbox expander", &hitbox_expander_enabled);
                ImGui::SliderFloat("hitbox size", &hitbox_expander_value, 1.0f, 50.0f);
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("misc")) {
            ImGui::Checkbox("noclip", &noclip_enabled);
            keybind_button("noclip key", noclip_keybind);
            ImGui::Separator();
            ImGui::Checkbox("walkspeed", &walkspeed_enabled);
            if (walkspeed_enabled) ImGui::SliderFloat("speed", &walkspeed_value, 0.0f, 200.0f);
            keybind_button("speed key", walkspeed_keybind);
            ImGui::Separator();
            ImGui::Checkbox("flight", &flight_enabled);
            if (flight_enabled) {
                ImGui::SliderFloat("fly speed", &flight_value, 0.0f, 200.0f);
                ImGui::TextDisabled("hold key + wasd, space = up, lshift = down");
            }
            keybind_button("flight key", flight_keybind);
            ImGui::Separator();
            ImGui::Checkbox("click teleport", &click_teleport_enabled);
            if (click_teleport_enabled) ImGui::SliderFloat("tp distance", &click_teleport_distance, 5.0f, 200.0f, "%.0f studs");
            keybind_button("teleport key", click_teleport_keybind);
            ImGui::Separator();
            ImGui::Checkbox("korblox", &korblox_enabled);

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("world")) {
            ImGui::Checkbox("skybox changer", &skybox_changer_enabled);
            if (skybox_changer_enabled) {
                ImGui::Combo("skybox", &skybox_type,
                    "Piss\0Peach\0Saku\0Purple\0Retro\0Space\0Sea\0Night V2\0"
                    "Dark\0Anime\0Beach\0Space V2\0Pink\0Rainbow\0Forest\0Night\0"
                    "Lava\0Rainy\0Green\0Volcanic\0Minecraft\0Lucid\0Nebulous\0");
                if (skybox_debug_msg[0]) {
                    ImGui::TextWrapped("%s", skybox_debug_msg);
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("debug")) {
            ImGui::TextWrapped("if a feature does nothing, check these values. "
                               "0x0 or 'INVALID' means that offset is wrong for your client version.");
            ImGui::Separator();

            ImGui::Text("base address   : 0x%llX", (unsigned long long)g_base_address);

            instance ve = read<instance>(g_base_address + Offsets::VisualEngine::Pointer);
            ImGui::Text("visual engine  : 0x%llX %s", (unsigned long long)ve.address,
                        ve.is_valid() ? "" : "<- INVALID");

            instance dm = game::ReadDatamodel(g_base_address);
            ImGui::Text("datamodel      : 0x%llX %s", (unsigned long long)dm.address,
                        dm.is_valid() ? "" : "<- INVALID");

            if (dm.is_valid()) {
                ImGui::Text("game name      : %s", dm.get_name().c_str());
                uint64_t place_id = read<uint64_t>(dm.address + Offsets::DataModel::PlaceId);
                bool loaded = read<bool>(dm.address + Offsets::DataModel::GameLoaded);
                ImGui::Text("place id       : %llu", (unsigned long long)place_id);
                ImGui::Text("game loaded    : %s", loaded ? "yes" : "no");

                if (place_id == 0) {
                    ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                        "you are not in a game yet (home page / menu).\n"
                        "join an actual experience - there is no world or\n"
                        "player list to read until then.");
                }
            }

            ImGui::Separator();

            // world-to-screen inputs - esp/aimbot/chams all depend on these
            if (ve.is_valid()) {
                float view[16]{};
                float dims[2]{};
                read_raw(ve.address + Offsets::VisualEngine::ViewMatrix, view, sizeof(view));
                read_raw(ve.address + Offsets::VisualEngine::Dimensions, dims, sizeof(dims));

                ImGui::Text("viewport       : %.0f x %.0f %s", dims[0], dims[1],
                            (dims[0] > 0.0f && dims[1] > 0.0f) ? "" : "<- INVALID (Dimensions offset wrong)");
                ImGui::Text("view matrix    : %.2f %.2f %.2f %.2f", view[0], view[1], view[2], view[3]);
                ImGui::Text("                 %.2f %.2f %.2f %.2f", view[4], view[5], view[6], view[7]);

                bool all_zero = true;
                for (int i = 0; i < 16; ++i) if (view[i] != 0.0f) { all_zero = false; break; }
                if (all_zero) ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                                                 "view matrix is all zero - ViewMatrix offset is wrong");
            }

            ImGui::Separator();

            const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
            ImGui::Text("local player   : %s", lp.valid ? "ok" : "INVALID");
            ImGui::Text("humanoid       : 0x%llX", (unsigned long long)lp.humanoid_address);
            ImGui::Text("hrp primitive  : 0x%llX", (unsigned long long)lp.hrp_primitive);
            ImGui::Text("local pos      : %.1f, %.1f, %.1f", lp.x, lp.y, lp.z);

            std::vector<cache::EspEntity> ents = cache::GetEspEntities();
            ImGui::Text("cached players : %d", (int)ents.size());
            if (ents.empty())
                ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "no other players cached (join a populated server)");

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("config")) {
            static char config_name_buf[128] = "";
            static char rename_buf[128] = "";
            static std::vector<std::string> config_list = config::GetConfigList();
            static int selected_config = -1;

            ImGui::InputText("config name", config_name_buf, sizeof(config_name_buf));

            if (ImGui::Button("Save", ImVec2(-1, 0))) {
                if (config_name_buf[0] != '\0') {
                    config::Save(config_name_buf);
                    config_list = config::GetConfigList();
                }
            }

            if (ImGui::Button("Load", ImVec2(-1, 0))) {
                if (config_name_buf[0] != '\0') {
                    config::Load(config_name_buf);
                }
            }

            if (ImGui::Button("Delete", ImVec2(-1, 0))) {
                if (config_name_buf[0] != '\0') {
                    config::Delete(config_name_buf);
                    config_list = config::GetConfigList();
                    selected_config = -1;
                }
            }

            ImGui::Separator();
            ImGui::InputText("rename to", rename_buf, sizeof(rename_buf));
            if (ImGui::Button("Rename", ImVec2(-1, 0))) {
                if (config_name_buf[0] != '\0' && rename_buf[0] != '\0') {
                    config::Rename(config_name_buf, rename_buf);
                    config_list = config::GetConfigList();
                    strncpy_s(config_name_buf, rename_buf, sizeof(config_name_buf) - 1);
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Open Config Folder", ImVec2(-1, 0))) {
                config::OpenConfigFolder();
            }

            ImGui::Separator();
            ImGui::Text("saved configs:");

            if (ImGui::Button("Refresh List", ImVec2(-1, 0))) {
                config_list = config::GetConfigList();
                selected_config = -1;
            }

            ImGui::BeginChild("config_list", ImVec2(-1, 150), true);
            for (int i = 0; i < (int)config_list.size(); ++i) {
                bool is_selected = (selected_config == i);
                if (ImGui::Selectable(config_list[i].c_str(), is_selected)) {
                    selected_config = i;
                    strncpy_s(config_name_buf, config_list[i].c_str(), sizeof(config_name_buf) - 1);
                }
            }
            ImGui::EndChild();

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}