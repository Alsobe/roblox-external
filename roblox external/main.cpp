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
#include "features/esp_preview/esp_preview.h"
#include "features/flight/flight.h"
#include "features/noclip/noclip.h"
#include "features/walkspeed/walkspeed.h"
#include "features/click_teleport/click_teleport.h"
#include "features/rage/rage.h"
#include "features/skybox_changer/skybox_changer.h"
#include "features/hitbox_expander/hitbox_expander.h"
#include "features/memorymeshchams/memorymeshchams.h"

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

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
        if (memory_mesh_chams_enabled) features::RenderMemoryMeshChams();
        if (aimbot_enabled) features::RenderFOV();

        if (g_state.menu_open) {
            if (!g_state.centered_once) {
                ImVec2 d = ImGui::GetIO().DisplaySize;
                ImGui::SetNextWindowPos(ImVec2(d.x * 0.5f, d.y * 0.5f), ImGuiCond_Once, ImVec2(0.5f, 0.5f));
                g_state.centered_once = true;
            }
            ImGui::SetNextWindowSize(ImVec2(520, 480), ImGuiCond_FirstUseEver);
            RenderMenu();
            if (esp_preview_3d) features::RenderESPPreview();
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
            if (click_teleport_enabled) features::RunClickTeleport();
            if (skybox_changer_enabled) features::RunSkyboxChanger();
            if (hitbox_expander_enabled) features::RunHitboxExpander();
        }
        Sleep(1);
    }
}


// returns false once the process we're attached to has exited
static bool RobloxStillAlive() {
    HANDLE h = mem::roblox_h.load();
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) return false;
    return code == STILL_ACTIVE;
}

static void DetachRoblox() {
    clear_instance_caches();
    g_base_address = 0;
    mem::process_id.store(0);
    HANDLE old = mem::roblox_h.exchange(nullptr);
    if (old && old != INVALID_HANDLE_VALUE) CloseHandle(old);
    g_memory.Handle = nullptr;
}

// handles the initial attach AND re-attaching after roblox is closed/reopened,
// so the overlay can be launched before roblox and just keeps working
static DWORD WINAPI AttachLoop(LPVOID) {
    bool warned_handle = false;

    while (true) {
        if (g_base_address && !RobloxStillAlive()) {
            LogLine("roblox closed - waiting for it to come back");
            DetachRoblox();
        }

        if (!g_base_address) {
            uint32_t pid = 0;
            uintptr_t base = 0;
            if (process::FindRoblox(pid, base)) {
                mem::process_id.store(pid);
                if (mem::grabroblox_h()) {
                    g_base_address = base;
                    warned_handle = false;
                    LogLine("attached to roblox - pid %u, base 0x%llx",
                            pid, (unsigned long long)base);
                } else {
                    if (!warned_handle) {
                        LogLine("found roblox but couldn't open a handle - run as administrator");
                        warned_handle = true;
                    }
                    mem::process_id.store(0);
                }
            }
        }

        Sleep(1000);
    }
    return 0;
}

_Use_decl_annotations_ int WINAPI WinMain(HINSTANCE hI, HINSTANCE hP, LPSTR lpC, int nS) {
    (void)hI; (void)hP; (void)lpC; (void)nS;

    LogLine("roblox external started");
    LogLine("waiting for roblox...");

    cache::StartThread();
    features::StartMemoryMeshChams();
    features::rage::Initialize();
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD { FeatureLoop(); return 0; }, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, AttachLoop, nullptr, 0, nullptr);

    discord_overlay::run();

    cache::StopThread();
    features::ShutdownMeshChams();
    features::ShutdownMemoryMeshChams();
    features::rage::Shutdown();
    discord_overlay::shutdown();
    return 0;
}