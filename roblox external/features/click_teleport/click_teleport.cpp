#include <Windows.h>
#include <cmath>
#include "click_teleport.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "game.h"
#include "process.h"

namespace features {

    static instance ct_cached_camera{};
    static DWORD ct_last_camera_lookup = 0;
    static bool ct_key_held = false;

    static instance GetCamera() {
        DWORD now = GetTickCount();
        if (ct_cached_camera.is_valid() && (now - ct_last_camera_lookup) < 1000)
            return ct_cached_camera;

        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid()) return instance{};
        instance workspace = dm.read_service("Workspace");
        if (!workspace.is_valid()) return instance{};
        ct_cached_camera = read<instance>(workspace.address + Offsets::Workspace::CurrentCamera);
        ct_last_camera_lookup = now;
        return ct_cached_camera;
    }

    // teleports to wherever the mouse cursor is pointing.
    //
    // the old version just used the camera's forward vector, which is only correct
    // when the cursor sits dead centre (first person). in third person the cursor is
    // usually off-centre, so we now rebuild the actual ray through the cursor using
    // the camera basis + fov, which behaves correctly in both modes.
    void RunClickTeleport() {
        if (!click_teleport_enabled || click_teleport_keybind == 0) {
            ct_key_held = false;
            return;
        }

        bool key_down = (GetAsyncKeyState(click_teleport_keybind) & 0x8000) != 0;

        // edge trigger - holding the key shouldn't teleport every tick
        if (!key_down) { ct_key_held = false; return; }
        if (ct_key_held) return;
        ct_key_held = true;

        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        if (!lp.valid || !is_valid_address(lp.hrp_primitive)) return;

        instance cam = GetCamera();
        if (!cam.is_valid()) return;

        float campos[3] = {};
        float rot[9] = {};
        if (!read_raw(cam.address + Offsets::Camera::Position, campos, sizeof(campos))) return;
        if (!read_raw(cam.address + Offsets::Camera::Rotation, rot, sizeof(rot))) return;

        float fov = read<float>(cam.address + Offsets::Camera::FieldOfView);
        if (fov < 1.0f || fov > 170.0f) fov = 70.0f;

        // viewport dimensions straight from the renderer
        instance ve = read<instance>(g_base_address + Offsets::VisualEngine::Pointer);
        if (!ve.is_valid()) return;
        float dims[2] = {};
        if (!read_raw(ve.address + Offsets::VisualEngine::Dimensions, dims, sizeof(dims))) return;
        if (dims[0] < 1.0f || dims[1] < 1.0f) return;

        POINT cursor{};
        if (!GetCursorPos(&cursor)) return;

        // the viewport is roblox's CLIENT area - if the game is windowed, the desktop
        // cursor position is offset from it, which threw the ray off. convert first.
        if (HWND rbx = process::GetRobloxWindow())
            ScreenToClient(rbx, &cursor);

        // camera basis vectors out of the roblox rotation matrix
        float rx = rot[0], ry = rot[3], rz = rot[6];   // right
        float ux = rot[1], uy = rot[4], uz = rot[7];   // up
        float lx = -rot[2], ly = -rot[5], lz = -rot[8]; // forward

        // normalised device coords for the cursor, -1..1
        float ndc_x = (2.0f * (float)cursor.x / dims[0]) - 1.0f;
        float ndc_y = 1.0f - (2.0f * (float)cursor.y / dims[1]);

        const float kPi = 3.14159265358979323846f;
        float tan_half = tanf((fov * 0.5f) * kPi / 180.0f);
        float aspect = dims[0] / dims[1];

        float sx = ndc_x * tan_half * aspect;
        float sy = ndc_y * tan_half;

        // ray through the cursor = forward + right*sx + up*sy
        float dx = lx + rx * sx + ux * sy;
        float dy = ly + ry * sx + uy * sy;
        float dz = lz + rz * sx + uz * sy;

        float mag = sqrtf(dx * dx + dy * dy + dz * dz);
        if (mag < 0.0001f) return;
        dx /= mag; dy /= mag; dz /= mag;

        float target[3] = {
            campos[0] + dx * click_teleport_distance,
            campos[1] + dy * click_teleport_distance,
            campos[2] + dz * click_teleport_distance
        };

        write_raw(lp.hrp_primitive + Offsets::Primitive::Position, target, sizeof(target));

        float zero[3] = { 0.0f, 0.0f, 0.0f };
        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyLinearVelocity, zero, sizeof(zero));
        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyAngularVelocity, zero, sizeof(zero));
    }
}
