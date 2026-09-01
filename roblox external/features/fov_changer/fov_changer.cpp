#include <Windows.h>
#include <cmath>
#include "fov_changer.h"
#include "globals.h"
#include "memory.h"
#include "offsets.h"
#include "game.h"

namespace features {

    static instance fov_camera{};
    static DWORD fov_last_lookup = 0;

    static instance GetCamera() {
        DWORD now = GetTickCount();
        if (fov_camera.is_valid() && (now - fov_last_lookup) < 1000)
            return fov_camera;

        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid()) return instance{};
        instance ws = dm.read_service("Workspace");
        if (!ws.is_valid()) return instance{};
        fov_camera = read<instance>(ws.address + Offsets::Workspace::CurrentCamera);
        fov_last_lookup = now;
        return fov_camera;
    }

    // IMPORTANT: roblox stores FieldOfView internally in RADIANS, not degrees.
    // Writing the raw degree value (e.g. 70) meant writing 70 radians (~4010 deg),
    // which wrapped the projection - that's why the fov went the wrong way and
    // the screen ended up upside down. Convert first.
    void RunFovChanger() {
        if (!fov_changer_enabled) return;

        instance cam = GetCamera();
        if (!cam.is_valid()) return;

        float deg = fov_value;
        if (deg < 1.0f)   deg = 1.0f;
        if (deg > 120.0f) deg = 120.0f;

        const float kPi = 3.14159265358979323846f;
        float want = deg * kPi / 180.0f;

        float current = read<float>(cam.address + Offsets::Camera::FieldOfView);
        if (current < 0.0f || current > 3.2f) return;   // not a sane radian fov - bail

        if (fabsf(current - want) > 0.0005f)
            write<float>(cam.address + Offsets::Camera::FieldOfView, want);
    }
}
