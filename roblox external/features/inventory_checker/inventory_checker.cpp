#include <Windows.h>
#include <cmath>
#include <string>
#include <vector>
#include "inventory_checker.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "game.h"
#include "process.h"
#include "imgui/imgui.h"

namespace features {

    struct IVec2 { float x = 0, y = 0; };

    struct InvCache {
        uintptr_t player = 0;
        DWORD stamp = 0;
        std::vector<std::string> items;
        std::string owner;
    };

    static InvCache s_cache;

    static bool WorldToScreenLocal(const float world[3], IVec2& out,
                                   const float* m, const IVec2& viewport) {
        float w = world[0] * m[12] + world[1] * m[13] + world[2] * m[14] + m[15];
        if (w < 0.01f) return false;
        float sx = world[0] * m[0] + world[1] * m[1] + world[2] * m[2] + m[3];
        float sy = world[0] * m[4] + world[1] * m[5] + world[2] * m[6] + m[7];
        float inv = 1.0f / w;
        out.x = (viewport.x * 0.5f * sx * inv) + (viewport.x * 0.5f);
        out.y = -(viewport.y * 0.5f * sy * inv) + (viewport.y * 0.5f);
        return (out.x == out.x && out.y == out.y);
    }

    // reads the tools sitting in a player's Backpack, plus whatever they're
    // currently holding in their character
    static void ReadBackpack(uintptr_t player_address, std::vector<std::string>& out) {
        out.clear();
        instance player{ player_address };
        if (!player.is_valid()) return;

        for (auto& c : player.get_children()) {
            if (!c.is_valid()) continue;
            if (c.get_class_name() != "Backpack") continue;

            for (auto& tool : c.get_children()) {
                if (!tool.is_valid()) continue;
                std::string cn = tool.get_class_name();
                if (cn != "Tool" && cn != "HopperBin" && cn != "BackpackItem") continue;
                std::string n = tool.get_name();
                if (!n.empty() && out.size() < 40) out.push_back(n);
            }
            break;
        }

        // equipped tool lives under the character, not the backpack
        instance ch = player.model_instance();
        if (ch.is_valid()) {
            for (auto& c : ch.get_children()) {
                if (!c.is_valid()) continue;
                std::string cn = c.get_class_name();
                if (cn != "Tool" && cn != "BackpackItem") continue;
                std::string n = c.get_name();
                if (!n.empty() && out.size() < 40) out.push_back(n + "  (equipped)");
            }
        }
    }

    void RenderInventoryChecker() {
        if (!inventory_checker_enabled || inventory_checker_keybind == 0) return;
        if (!(GetAsyncKeyState(inventory_checker_keybind) & 0x8000)) return;
        if (!g_base_address) return;

        instance ve = read<instance>(g_base_address + Offsets::VisualEngine::Pointer);
        if (!ve.is_valid()) return;

        float view[16] = {};
        float dims[2] = {};
        if (!read_raw(ve.address + Offsets::VisualEngine::ViewMatrix, view, sizeof(view))) return;
        if (!read_raw(ve.address + Offsets::VisualEngine::Dimensions, dims, sizeof(dims))) return;
        if (dims[0] < 1.0f || dims[1] < 1.0f) return;

        IVec2 viewport{ dims[0], dims[1] };

        POINT cursor{};
        if (!GetCursorPos(&cursor)) return;

        // the viewport is roblox's CLIENT area - if the game is windowed, the desktop
        // cursor position is offset from it, which threw the ray off. convert first.
        if (HWND rbx = process::GetRobloxWindow())
            ScreenToClient(rbx, &cursor);

        // pick whichever player is closest to the cursor on screen
        auto entities_snap = cache::GetEspSnapshot();
        const auto& entities = *entities_snap;
        const cache::EspEntity* best = nullptr;
        IVec2 best_screen{};
        float best_dist = 250.0f; // px - must be reasonably close to count as "on them"

        for (const cache::EspEntity& e : entities) {
            float root[3] = { e.root_x, e.root_y, e.root_z };
            if (root[0] == 0.0f && root[1] == 0.0f && root[2] == 0.0f) continue;

            IVec2 scr{};
            if (!WorldToScreenLocal(root, scr, view, viewport)) continue;

            float dx = scr.x - (float)cursor.x;
            float dy = scr.y - (float)cursor.y;
            float d = sqrtf(dx * dx + dy * dy);
            if (d < best_dist) { best_dist = d; best = &e; best_screen = scr; }
        }

        if (!best) return;

        // refresh at most ~4x a second per player: walking the backpack is expensive
        DWORD now = GetTickCount();
        if (s_cache.player != best->player_address || (now - s_cache.stamp) > 250) {
            s_cache.player = best->player_address;
            s_cache.stamp = now;
            s_cache.owner = best->name[0] ? best->name : "player";
            ReadBackpack(best->player_address, s_cache.items);
        }

        // ---- draw the panel next to them ----
        ImDrawList* draw = ImGui::GetBackgroundDrawList();

        const float pad = 8.0f;
        const float line_h = ImGui::GetTextLineHeight();
        float width = 190.0f;

        char header[96];
        snprintf(header, sizeof(header), "%s  [%d]", s_cache.owner.c_str(), (int)s_cache.items.size());

        for (const std::string& it : s_cache.items) {
            ImVec2 sz = ImGui::CalcTextSize(it.c_str());
            if (sz.x + pad * 2.0f > width) width = sz.x + pad * 2.0f;
        }
        ImVec2 hsz = ImGui::CalcTextSize(header);
        if (hsz.x + pad * 2.0f > width) width = hsz.x + pad * 2.0f;

        float rows = (float)(s_cache.items.empty() ? 1 : s_cache.items.size());
        float height = pad * 2.0f + line_h * (rows + 1.0f) + 6.0f;

        float px = best_screen.x + 24.0f;
        float py = best_screen.y - height * 0.5f;
        if (px + width > viewport.x) px = best_screen.x - width - 24.0f;
        if (py < 0.0f) py = 0.0f;

        ImU32 bg     = IM_COL32(12, 12, 16, 220);
        ImU32 border = IM_COL32(220, 33, 61, 200);
        ImU32 text   = IM_COL32(240, 240, 245, 255);
        ImU32 dim    = IM_COL32(160, 160, 170, 255);

        draw->AddRectFilled(ImVec2(px, py), ImVec2(px + width, py + height), bg, 6.0f);
        draw->AddRect(ImVec2(px, py), ImVec2(px + width, py + height), border, 6.0f, 0, 1.5f);

        float ty = py + pad;
        draw->AddText(ImVec2(px + pad, ty), border, header);
        ty += line_h + 4.0f;
        draw->AddLine(ImVec2(px + pad, ty), ImVec2(px + width - pad, ty), border, 1.0f);
        ty += 2.0f;

        if (s_cache.items.empty()) {
            draw->AddText(ImVec2(px + pad, ty), dim, "empty");
        } else {
            for (const std::string& it : s_cache.items) {
                draw->AddText(ImVec2(px + pad, ty), text, it.c_str());
                ty += line_h;
            }
        }

        // little tether so it's obvious who it belongs to
        draw->AddLine(ImVec2(best_screen.x, best_screen.y),
                      ImVec2(px < best_screen.x ? px + width : px, py + height * 0.5f),
                      border, 1.0f);
    }
}
