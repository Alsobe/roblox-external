#include <Windows.h>
#include <cmath>
#include "flight.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "game.h"

namespace features {

    struct FlightVec3 {
        float x, y, z;

        FlightVec3 operator+(const FlightVec3& other) const {
            return { x + other.x, y + other.y, z + other.z };
        }

        FlightVec3 operator-(const FlightVec3& other) const {
            return { x - other.x, y - other.y, z - other.z };
        }

        FlightVec3 operator*(float scalar) const {
            return { x * scalar, y * scalar, z * scalar };
        }

        float magnitude() const {
            return sqrtf(x * x + y * y + z * z);
        }

        FlightVec3 normalize() const {
            float mag = magnitude();
            if (mag < 0.0001f) return { 0, 0, 0 };
            return { x / mag, y / mag, z / mag };
        }
    };

    static instance cached_camera{};
    static DWORD last_camera_lookup = 0;

    static instance GetCamera() {
        DWORD now = GetTickCount();
        if (cached_camera.is_valid() && (now - last_camera_lookup) < 2000)
            return cached_camera;

        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid()) return instance{};
        instance workspace = dm.read_service("Workspace");
        if (!workspace.is_valid()) return instance{};
        cached_camera = read<instance>(workspace.address + Offsets::Workspace::CurrentCamera);
        last_camera_lookup = now;
        return cached_camera;
    }

    static bool GetCameraRotation(float rot[9]) {
        instance cam = GetCamera();
        if (!cam.is_valid()) return false;
        return read_raw(cam.address + Offsets::Camera::Rotation, rot, sizeof(float) * 9);
    }

    static FlightVec3 GetLookVector(const float rot[9]) {
        return { -rot[2], -rot[5], -rot[8] };
    }

    static FlightVec3 GetRightVector(const float rot[9]) {
        return { rot[0], rot[3], rot[6] };
    }

    void RunFlight() {
        if (!flight_enabled || flight_keybind == 0) return;

        bool key_down = (GetAsyncKeyState(flight_keybind) & 0x8000) != 0;
        if (!key_down) return;

        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        if (!lp.valid || !is_valid_address(lp.hrp_primitive)) return;

        float rot[9] = {};
        if (!GetCameraRotation(rot)) return;

        FlightVec3 look = GetLookVector(rot);
        FlightVec3 right = GetRightVector(rot);

        FlightVec3 direction = { 0, 0, 0 };

        if (GetAsyncKeyState('W') & 0x8000) {
            direction = direction + look;
        }
        if (GetAsyncKeyState('S') & 0x8000) {
            direction = direction - look;
        }
        if (GetAsyncKeyState('A') & 0x8000) {
            direction = direction - right;
        }
        if (GetAsyncKeyState('D') & 0x8000) {
            direction = direction + right;
        }

        if (direction.magnitude() > 0.01f) {
            direction = direction.normalize();
        }

        FlightVec3 velocity = direction * flight_value;

        write<float>(lp.hrp_primitive + Offsets::Primitive::AssemblyLinearVelocity, velocity.x);
        write<float>(lp.hrp_primitive + Offsets::Primitive::AssemblyLinearVelocity + 4, velocity.y);
        write<float>(lp.hrp_primitive + Offsets::Primitive::AssemblyLinearVelocity + 8, velocity.z);
    }
}

