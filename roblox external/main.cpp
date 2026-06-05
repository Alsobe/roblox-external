#include <Windows.h>
#include <cstdio>
#include "memory.h"
#include "process.h"
#include "game.h"
#include "offsets.h"
#include "globals.h"
#include "cache.h"
#include "overlay.hpp"

#include "features/aimbot/aimbot.h"
#include "features/esp/esp.h"
#include "features/flight/flight.h"
#include "features/noclip/noclip.h"
#include "features/walkspeed/walkspeed.h"

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include <cstdio>

void RenderMenu();
void TickKeybinds();

namespace discord_overlay {
    void render_ui() {
        TickKeybinds();

        {
            static float fps_timer = 0;
            static int frame_count = 0;
            static float current_fps = 0;
            frame_count++;
            float now = (float)ImGui::GetTime();
            if (now - fps_timer >= 1.0f) {
                current_fps = (float)frame_count / (now - fps_timer);
                frame_count = 0;
                fps_timer = now;
            }
            char fps_buf[32];
            sprintf_s(fps_buf, "fps: %.0f", current_fps);
            ImGui::GetBackgroundDrawList()->AddText(ImVec2(10, 10), IM_COL32(255, 255, 255, 255), fps_buf);
        }

        if (esp_enabled) {
            features::RenderESP();
            if (skeleton_esp) features::RenderSkeletonESP();
            if (chinahat) features::RenderChinaHatESP();
            if (aimviewer) features::RenderAimViewer();
            if (chams_enabled) features::RenderChams();
            if (mesh_chams_enabled) features::RenderMeshChams();
            if (render_expanded_hitbox) features::RenderExpandedHitbox();
        }
        if (aimbot_enabled) features::RenderFOV();

        if (g_state.menu_open) {
            if (!g_state.centered_once) {
                ImVec2 d = ImGui::GetIO().DisplaySize;
                ImGui::SetNextWindowPos(ImVec2(d.x * 0.5f, d.y * 0.5f), ImGuiCond_Once, ImVec2(0.5f, 0.5f));
                g_state.centered_once = true;
            }
            ImGui::SetNextWindowSize(ImVec2(520, 480), ImGuiCond_FirstUseEver);
            RenderMenu();
        }
    }
}

static void FeatureLoop() {
    while (true) {
        if (g_base_address) {
            if (aimbot_enabled) features::RunAimbot();
            if (flight_enabled) features::RunFlight();
            if (noclip_enabled) features::RunNoclip();
            if (walkspeed_enabled) features::RunWalkspeed();
        }
        Sleep(1);
    }
}

static void WaitForRoblox() {
    printf("waiting for roblox...\n");
    uint32_t pid = 0;
    uintptr_t base = 0;
    while (!process::FindRoblox(pid, base)) Sleep(2000);
    printf("found roblox - pid: %u, base: 0x%llx\n", pid, (unsigned long long)base);
    mem::process_id.store(pid);
    if (!mem::grabroblox_h()) { printf("failed to open handle\n"); return; }
    g_base_address = base;
}

_Use_decl_annotations_ int WINAPI WinMain(HINSTANCE hI, HINSTANCE hP, LPSTR lpC, int nS) {
    (void)hI; (void)hP; (void)lpC; (void)nS;
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONIN$", "r", stdin);

    printf("=== roblox external ===\n");
    WaitForRoblox();

    if (!g_base_address) { printf("couldnt find roblox, exiting\n"); Sleep(3000); return 1; }

    cache::StartThread();
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD { FeatureLoop(); return 0; }, nullptr, 0, nullptr);

    printf("launching overlay - insert to toggle menu\n");
    discord_overlay::run();

    cache::StopThread();
    features::ShutdownMeshChams();
    discord_overlay::shutdown();
    if (fp) fclose(fp);
    FreeConsole();
    return 0;
}