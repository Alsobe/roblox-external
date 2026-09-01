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

    static bool s_flying = false;
    static uintptr_t s_flying_humanoid = 0;

    static void StopFlying() {
        // let the humanoid drive itself again
        if (s_flying && is_valid_address(s_flying_humanoid)) {
            write<bool>(s_flying_humanoid + Offsets::Humanoid::PlatformStand, false);
        }
        s_flying = false;
        s_flying_humanoid = 0;
    }

    // Writing Primitive::Position directly made the physics solver resolve the
    // resulting collision and shove the character through the floor. Instead we
    // put the humanoid into PlatformStand (which switches off its state machine,
    // so it stops trying to walk/stand/fall) and then drive AssemblyLinearVelocity.
    // Rewriting the velocity every tick is what cancels gravity - a velocity of
    // exactly zero therefore hovers perfectly in place.
    void RunFlight() {
        if (!flight_enabled || flight_keybind == 0) { StopFlying(); return; }

        bool key_down = (GetAsyncKeyState(flight_keybind) & 0x8000) != 0;
        if (!key_down) { StopFlying(); return; }

        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        if (!lp.valid || !is_valid_address(lp.hrp_primitive)) { StopFlying(); return; }

        if (!s_flying) {
            s_flying = true;
            s_flying_humanoid = lp.humanoid_address;
            if (is_valid_address(s_flying_humanoid))
                write<bool>(s_flying_humanoid + Offsets::Humanoid::PlatformStand, true);
        } else if (s_flying_humanoid != lp.humanoid_address) {
            // respawned mid-flight - re-apply to the new humanoid
            s_flying_humanoid = lp.humanoid_address;
            if (is_valid_address(s_flying_humanoid))
                write<bool>(s_flying_humanoid + Offsets::Humanoid::PlatformStand, true);
        }

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

        if (dir.magnitude() > 0.01f) dir = dir.normalize();

        FVec3 vel = dir * flight_value;
        float v[3] = { vel.x, vel.y, vel.z };
        float zero[3] = { 0, 0, 0 };

        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyLinearVelocity, v, sizeof(v));
        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyAngularVelocity, zero, sizeof(zero));
    }
}
