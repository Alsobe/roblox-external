#include <Windows.h>
#include <cmath>
#include "click_teleport.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "game.h"

namespace features {

    static instance ct_cached_camera{};
    static DWORD ct_last_camera_lookup = 0;
    static bool ct_key_held = false;

    static instance GetCamera() {
        DWORD now = GetTickCount();
        if (ct_cached_camera.is_valid() && (now - ct_last_camera_lookup) < 2000)
            return ct_cached_camera;

        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid()) return instance{};
        instance workspace = dm.read_service("Workspace");
        if (!workspace.is_valid()) return instance{};
        ct_cached_camera = read<instance>(workspace.address + Offsets::Workspace::CurrentCamera);
        ct_last_camera_lookup = now;
        return ct_cached_camera;
    }

    // teleports the character to the point you are aiming at, `distance` studs
    // out along the camera's look vector.
    void RunClickTeleport() {
        if (!click_teleport_enabled || click_teleport_keybind == 0) {
            ct_key_held = false;
            return;
        }

        bool key_down = (GetAsyncKeyState(click_teleport_keybind) & 0x8000) != 0;

        // edge trigger only - otherwise holding the key teleports every single tick
        if (!key_down) {
            ct_key_held = false;
            return;
        }
        if (ct_key_held) return;
        ct_key_held = true;

        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        if (!lp.valid || !is_valid_address(lp.hrp_primitive)) return;

        instance cam = GetCamera();
        if (!cam.is_valid()) return;

        float pos[3] = {};
        float rot[9] = {};
        if (!read_raw(cam.address + Offsets::Camera::Position, pos, sizeof(pos))) return;
        if (!read_raw(cam.address + Offsets::Camera::Rotation, rot, sizeof(rot))) return;

        // third column of the rotation matrix, negated, is the forward vector
        float lx = -rot[2], ly = -rot[5], lz = -rot[8];
        float mag = sqrtf(lx * lx + ly * ly + lz * lz);
        if (mag < 0.0001f) return;
        lx /= mag; ly /= mag; lz /= mag;

        float target[3] = {
            pos[0] + lx * click_teleport_distance,
            pos[1] + ly * click_teleport_distance,
            pos[2] + lz * click_teleport_distance
        };

        write_raw(lp.hrp_primitive + Offsets::Primitive::Position, target, sizeof(target));

        // zero velocity so the engine doesn't fling or ragdoll us on arrival
        float zero[3] = { 0.0f, 0.0f, 0.0f };
        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyLinearVelocity, zero, sizeof(zero));
        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyAngularVelocity, zero, sizeof(zero));
    }
}
