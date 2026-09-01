#include <Windows.h>
#include <cmath>
#include <chrono>
#include <unordered_map>
#include "flight.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "game.h"

namespace features {

    // ---------------------------------------------------------------------
    // Why this is written the way it is:
    //
    //   attempt 1 - write AssemblyLinearVelocity  -> the humanoid state machine
    //               and the assembly solver overwrite it every physics step.
    //   attempt 2 - write Position                -> the collision solver sees the
    //               character intersecting geometry and resolves it downward,
    //               which is the "bugged into the floor" behaviour.
    //   attempt 3 - PlatformStand + velocity      -> ragdolls, still falls.
    //
    // So: drive the position ourselves AND turn the character's collisions off
    // while flying, so there is nothing for the solver to resolve. Collisions are
    // restored the moment you stop. This is fully under our control and does not
    // depend on the engine cooperating.
    // ---------------------------------------------------------------------

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
    static DWORD    last_camera_lookup = 0;

    static instance GetCamera() {
        DWORD now = GetTickCount();
        if (cached_camera.is_valid() && (now - last_camera_lookup) < 1000)
            return cached_camera;

        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid()) return instance{};
        instance ws = dm.read_service("Workspace");
        if (!ws.is_valid()) return instance{};
        cached_camera = read<instance>(ws.address + Offsets::Workspace::CurrentCamera);
        last_camera_lookup = now;
        return cached_camera;
    }

    static bool s_active = false;
    static bool s_key_was_down = false;
    static bool s_have_pos = false;
    static float s_pos[3] = { 0, 0, 0 };
    static std::chrono::steady_clock::time_point s_last_tick{};

    // saved collision flags, keyed by part instance
    static std::unordered_map<uintptr_t, uint8_t> s_saved_flags;

    bool IsFlying() { return s_active; }

    static instance LocalCharacter() {
        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid()) return instance{};
        instance players = dm.read_service("Players");
        if (!players.is_valid()) return instance{};
        instance local = players.local_player();
        if (!local.is_valid()) return instance{};
        return local.model_instance();
    }

    static void SetCharacterCollision(bool enable_collision) {
        instance ch = LocalCharacter();
        if (!ch.is_valid()) return;

        for (const instance& child : ch.get_children()) {
            if (!child.is_valid()) continue;
            uintptr_t prim = read<uintptr_t>(child.address + Offsets::BasePart::Primitive);
            if (!is_valid_address(prim)) continue;

            if (!enable_collision) {
                uint8_t flags = read<uint8_t>(prim + Offsets::Primitive::Flags);
                if (s_saved_flags.find(child.address) == s_saved_flags.end())
                    s_saved_flags[child.address] = flags;
                write<uint8_t>(prim + Offsets::Primitive::Flags,
                               (uint8_t)(flags & ~Offsets::PrimitiveFlags::CanCollide));
            } else {
                auto it = s_saved_flags.find(child.address);
                if (it != s_saved_flags.end())
                    write<uint8_t>(prim + Offsets::Primitive::Flags, it->second);
            }
        }

        if (enable_collision) s_saved_flags.clear();
    }

    static void StopFlying() {
        if (s_active || !s_saved_flags.empty()) SetCharacterCollision(true);
        s_active = false;
        s_have_pos = false;
    }

    void RunFlight() {
        if (!flight_enabled || flight_keybind == 0) {
            StopFlying();
            s_key_was_down = false;
            return;
        }

        bool key_down = (GetAsyncKeyState(flight_keybind) & 0x8000) != 0;

        bool want = s_active;
        if (flight_hold_mode) {
            want = key_down;
        } else if (key_down && !s_key_was_down) {
            want = !s_active;
        }
        s_key_was_down = key_down;

        if (!want) { StopFlying(); return; }

        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        if (!lp.valid || !is_valid_address(lp.hrp_primitive)) { StopFlying(); return; }

        auto now = std::chrono::steady_clock::now();

        if (!s_active) {
            s_active = true;
            s_have_pos = false;
            s_last_tick = now;
            s_saved_flags.clear();
            SetCharacterCollision(false);   // ghost mode while flying
        }

        float cur[3] = {};
        if (!read_raw(lp.hrp_primitive + Offsets::Primitive::Position, cur, sizeof(cur))) return;

        if (!s_have_pos) {
            s_pos[0] = cur[0]; s_pos[1] = cur[1]; s_pos[2] = cur[2];
            s_have_pos = true;
            s_last_tick = now;
        }

        float dt = std::chrono::duration<float>(now - s_last_tick).count();
        s_last_tick = now;
        if (dt <= 0.0f) dt = 0.001f;
        if (dt > 0.05f) dt = 0.05f;

        // if something teleported us (respawn, game tp) resync instead of snapping back
        float ddx = cur[0] - s_pos[0], ddy = cur[1] - s_pos[1], ddz = cur[2] - s_pos[2];
        if (sqrtf(ddx * ddx + ddy * ddy + ddz * ddz) > 30.0f) {
            s_pos[0] = cur[0]; s_pos[1] = cur[1]; s_pos[2] = cur[2];
        }

        instance cam = GetCamera();
        if (!cam.is_valid()) return;

        float rot[9] = {};
        if (!read_raw(cam.address + Offsets::Camera::Rotation, rot, sizeof(rot))) return;

        FVec3 look  = { -rot[2], -rot[5], -rot[8] };
        FVec3 right = {  rot[0],  rot[3],  rot[6] };

        FVec3 dir{};
        if (GetAsyncKeyState('W')         & 0x8000) dir = dir + look;
        if (GetAsyncKeyState('S')         & 0x8000) dir = dir - look;
        if (GetAsyncKeyState('A')         & 0x8000) dir = dir - right;
        if (GetAsyncKeyState('D')         & 0x8000) dir = dir + right;
        if (GetAsyncKeyState(VK_SPACE)    & 0x8000) dir = dir + FVec3{ 0, 1, 0 };
        if (GetAsyncKeyState(VK_LCONTROL) & 0x8000) dir = dir - FVec3{ 0, 1, 0 };
        if (GetAsyncKeyState(VK_LSHIFT)   & 0x8000) dir = dir - FVec3{ 0, 1, 0 };

        if (dir.magnitude() > 0.0f) {
            dir = dir.normalize();
            s_pos[0] += dir.x * flight_value * dt;
            s_pos[1] += dir.y * flight_value * dt;
            s_pos[2] += dir.z * flight_value * dt;
        }

        write_raw(lp.hrp_primitive + Offsets::Primitive::Position, s_pos, sizeof(s_pos));

        // kill any momentum the engine tries to build up underneath us
        float zero[3] = { 0, 0, 0 };
        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyLinearVelocity, zero, sizeof(zero));
        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyAngularVelocity, zero, sizeof(zero));
    }
}
