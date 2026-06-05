#include <Windows.h>
#include <cstring>
#include <cstdio>
#include "imgui/imgui.h"
#include "globals.h"
#include "features/skybox_changer/skybox_changer.h"

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
            if (flight_enabled) ImGui::SliderFloat("fly speed", &flight_value, 0.0f, 200.0f);
            keybind_button("flight key", flight_keybind);
            ImGui::Separator();
            ImGui::Checkbox("korblox", &korblox_enabled);
            ImGui::Separator();
            ImGui::Checkbox("rivals skin changer", &rivals_skin_changer_enabled);
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

        if (ImGui::BeginTabItem("blade ball")) {
            ImGui::Checkbox("auto parry", &blade_ball_auto_parry);
            ImGui::Checkbox("ball esp", &blade_ball_ball_esp);
            ImGui::SliderFloat("parry dist", &blade_ball_parry_distance, 4.0f, 30.0f, "%.1f");
            ImGui::SliderFloat("parry height", &blade_ball_parry_height, 2.0f, 15.0f, "%.1f");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}