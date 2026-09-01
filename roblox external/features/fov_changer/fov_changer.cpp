#include <Windows.h>
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

    // Roblox clamps FieldOfView to 1..120 itself, and most games rewrite it every
    // frame from their camera script, so we keep re-applying it every tick.
    void RunFovChanger() {
        if (!fov_changer_enabled) return;

        instance cam = GetCamera();
        if (!cam.is_valid()) return;

        float want = fov_value;
        if (want < 1.0f) want = 1.0f;
        if (want > 120.0f) want = 120.0f;

        float current = read<float>(cam.address + Offsets::Camera::FieldOfView);
        if (current != want)
            write<float>(cam.address + Offsets::Camera::FieldOfView, want);
    }
}
