#include <Windows.h>
#include <cmath>
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

    static bool s_fly_active = false;   // toggle state
    static bool s_key_was_down = false;

    bool IsFlying() { return s_fly_active; }

    // Mirrors the Lua BodyVelocity approach: every tick we overwrite the root
    // part's assembly velocity with exactly the movement vector we want.
    // Because we rewrite it far faster than the physics step, gravity never gets
    // a chance to accumulate - a velocity of 0 therefore hovers in place.
    //
    // Deliberately does NOT touch PlatformStand: that made the humanoid ragdoll
    // and get resolved through the floor.
    void RunFlight() {
        if (!flight_enabled || flight_keybind == 0) {
            s_fly_active = false;
            s_key_was_down = false;
            return;
        }

        bool key_down = (GetAsyncKeyState(flight_keybind) & 0x8000) != 0;

        if (flight_hold_mode) {
            s_fly_active = key_down;
        } else {
            // toggle on the rising edge, like the lua script's Toggle mode
            if (key_down && !s_key_was_down) s_fly_active = !s_fly_active;
        }
        s_key_was_down = key_down;

        if (!s_fly_active) return;

        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        if (!lp.valid || !is_valid_address(lp.hrp_primitive)) return;

        instance cam = GetCamera();
        if (!cam.is_valid()) return;

        float rot[9] = {};
        if (!read_raw(cam.address + Offsets::Camera::Rotation, rot, sizeof(rot))) return;

        FVec3 look  = { -rot[2], -rot[5], -rot[8] };
        FVec3 right = {  rot[0],  rot[3],  rot[6] };

        FVec3 dir{};
        if (GetAsyncKeyState('W')          & 0x8000) dir = dir + look;
        if (GetAsyncKeyState('S')          & 0x8000) dir = dir - look;
        if (GetAsyncKeyState('A')          & 0x8000) dir = dir - right;
        if (GetAsyncKeyState('D')          & 0x8000) dir = dir + right;
        if (GetAsyncKeyState(VK_SPACE)     & 0x8000) dir = dir + FVec3{ 0, 1, 0 };
        if (GetAsyncKeyState(VK_LCONTROL)  & 0x8000) dir = dir - FVec3{ 0, 1, 0 };
        if (GetAsyncKeyState(VK_LSHIFT)    & 0x8000) dir = dir - FVec3{ 0, 1, 0 };

        if (dir.magnitude() > 0.0f) dir = dir.normalize();

        FVec3 vel = dir * flight_value;
        float v[3] = { vel.x, vel.y, vel.z };
        float zero[3] = { 0, 0, 0 };

        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyLinearVelocity, v, sizeof(v));
        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyAngularVelocity, zero, sizeof(zero));
    }
}
