#include <Windows.h>
#include <cmath>
#include <chrono>
#include "flight.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "game.h"

namespace features {

    struct FVec3 {
        float x = 0, y = 0, z = 0;

        FVec3 operator+(const FVec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        FVec3 operator-(const FVec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        FVec3 operator*(float s)        const { return { x * s, y * s, z * s }; }

        float magnitude() const { return sqrtf(x * x + y * y + z * z); }

        FVec3 normalize() const {
            float m = magnitude();
            if (m < 0.0001f) return { 0, 0, 0 };
            return { x / m, y / m, z / m };
        }
    };

    static instance cached_camera{};
    static DWORD last_camera_lookup = 0;

    static instance GetCamera() {
        DWORD now = GetTickCount();
        if (cached_camera.is_valid() && (now - last_camera_lookup) < 1000)
            return cached_camera;

        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid()) return instance{};
        instance workspace = dm.read_service("Workspace");
        if (!workspace.is_valid()) return instance{};
        cached_camera = read<instance>(workspace.address + Offsets::Workspace::CurrentCamera);
        last_camera_lookup = now;
        return cached_camera;
    }

    // flight is position based rather than velocity based. writing AssemblyLinearVelocity
    // fights the physics solver (gravity keeps re-applying between our writes and the
    // humanoid state machine clamps it), so instead we move the root part ourselves
    // every tick and hold it there. that also makes hovering exact.
    void RunFlight() {
        static bool was_flying = false;
        static std::chrono::steady_clock::time_point last_tick{};
        static float hold[3] = { 0, 0, 0 };

        if (!flight_enabled || flight_keybind == 0) { was_flying = false; return; }

        bool key_down = (GetAsyncKeyState(flight_keybind) & 0x8000) != 0;
        if (!key_down) { was_flying = false; return; }

        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        if (!lp.valid || !is_valid_address(lp.hrp_primitive)) { was_flying = false; return; }

        float cur[3] = {};
        if (!read_raw(lp.hrp_primitive + Offsets::Primitive::Position, cur, sizeof(cur))) return;

        auto now = std::chrono::steady_clock::now();

        // first frame of a new flight: latch onto where we currently are
        if (!was_flying) {
            hold[0] = cur[0]; hold[1] = cur[1]; hold[2] = cur[2];
            last_tick = now;
            was_flying = true;
        }

        float dt = std::chrono::duration<float>(now - last_tick).count();
        last_tick = now;
        if (dt <= 0.0f) dt = 0.001f;
        if (dt > 0.1f)  dt = 0.1f;   // don't lurch after a stall

        // if the game moved us a long way (teleport, respawn, seat) resync
        float drift = sqrtf((cur[0] - hold[0]) * (cur[0] - hold[0]) +
                            (cur[1] - hold[1]) * (cur[1] - hold[1]) +
                            (cur[2] - hold[2]) * (cur[2] - hold[2]));
        if (drift > 25.0f) { hold[0] = cur[0]; hold[1] = cur[1]; hold[2] = cur[2]; }

        instance cam = GetCamera();
        if (!cam.is_valid()) return;

        float rot[9] = {};
        if (!read_raw(cam.address + Offsets::Camera::Rotation, rot, sizeof(rot))) return;

        FVec3 look  = { -rot[2], -rot[5], -rot[8] };
        FVec3 right = {  rot[0],  rot[3],  rot[6] };

        FVec3 dir{};
        if (GetAsyncKeyState('W')       & 0x8000) dir = dir + look;
        if (GetAsyncKeyState('S')       & 0x8000) dir = dir - look;
        if (GetAsyncKeyState('A')       & 0x8000) dir = dir - right;
        if (GetAsyncKeyState('D')       & 0x8000) dir = dir + right;
        if (GetAsyncKeyState(VK_SPACE)  & 0x8000) dir = dir + FVec3{ 0, 1, 0 };
        if (GetAsyncKeyState(VK_LSHIFT) & 0x8000) dir = dir - FVec3{ 0, 1, 0 };

        if (dir.magnitude() > 0.01f) {
            dir = dir.normalize();
            hold[0] += dir.x * flight_value * dt;
            hold[1] += dir.y * flight_value * dt;
            hold[2] += dir.z * flight_value * dt;
        }

        // pin the root part to our tracked position, and keep velocity dead so the
        // solver doesn't accumulate gravity while we're holding it
        write_raw(lp.hrp_primitive + Offsets::Primitive::Position, hold, sizeof(hold));

        float zero[3] = { 0, 0, 0 };
        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyLinearVelocity, zero, sizeof(zero));
        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyAngularVelocity, zero, sizeof(zero));
    }
}
