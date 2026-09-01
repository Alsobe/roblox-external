#include <Windows.h>
#include <chrono>
#include "infinite_jump.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"

namespace features {

    // Velocity writes get clobbered by the humanoid state machine, so instead of
    // asking the engine to jump we run the arc ourselves: on each space press we
    // start an upward launch and integrate it against roblox's gravity, writing
    // only the Y component so the game keeps full control of horizontal movement.
    // The arc ends as soon as we've fallen back to where we started.

    static bool  s_space_was_down = false;
    static bool  s_arc_active = false;
    static float s_vel_y = 0.0f;
    static float s_start_y = 0.0f;
    static std::chrono::steady_clock::time_point s_last{};

    static constexpr float kGravity = 196.2f;   // roblox default studs/s^2

    void RunInfiniteJump() {
        if (!infinite_jump_enabled) {
            s_arc_active = false;
            s_space_was_down = false;
            return;
        }

        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        if (!lp.valid || !is_valid_address(lp.hrp_primitive)) {
            s_arc_active = false;
            return;
        }

        bool down = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        bool rising_edge = down && !s_space_was_down;
        s_space_was_down = down;

        auto now = std::chrono::steady_clock::now();

        float pos[3] = {};
        if (!read_raw(lp.hrp_primitive + Offsets::Primitive::Position, pos, sizeof(pos))) return;

        // every fresh press restarts the arc, mid-air or not - that's the "infinite" part
        if (rising_edge) {
            s_arc_active = true;
            s_vel_y = infinite_jump_power;
            s_start_y = pos[1];
            s_last = now;
            return;
        }

        if (!s_arc_active) return;

        float dt = std::chrono::duration<float>(now - s_last).count();
        s_last = now;
        if (dt <= 0.0f) dt = 0.001f;
        if (dt > 0.05f) dt = 0.05f;

        s_vel_y -= kGravity * dt;
        pos[1] += s_vel_y * dt;

        // hand control back once we're descending past the launch height
        if (s_vel_y < 0.0f && pos[1] <= s_start_y) {
            s_arc_active = false;
            return;
        }

        write_raw(lp.hrp_primitive + Offsets::Primitive::Position, pos, sizeof(pos));
    }
}
