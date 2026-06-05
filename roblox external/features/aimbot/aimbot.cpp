#include <Windows.h>
#include <cstdint>
#include <cmath>
#include <cfloat>
#include "aimbot.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "game.h"
#include "imgui/imgui.h"

namespace features {

    struct AimVec2 { float x, y; };
    struct AimVec3 { float x, y, z; };
    struct AimMatrix4 { float data[16]; };

    static bool AimReadRaw(uint64_t address, void* buffer, size_t size) {
        return read_raw(address, buffer, size);
    }

    static instance cached_camera{};
    static DWORD last_camera_lookup = 0;

    static instance GetCameraInstance() {
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

    static bool GetCameraTransform(AimVec3& pos, float rot[9]) {
        instance cam = GetCameraInstance();
        if (!cam.is_valid()) return false;
        if (!AimReadRaw(cam.address + Offsets::Camera::Position, &pos, sizeof(pos))) return false;
        if (!AimReadRaw(cam.address + Offsets::Camera::Rotation, rot, sizeof(float) * 9)) return false;
        return true;
    }

    static bool GetViewData(AimMatrix4& view, AimVec2& viewport) {
        instance ve = read<instance>(g_base_address + Offsets::VisualEngine::Pointer);
        if (!ve.is_valid()) return false;
        if (!AimReadRaw(ve.address + Offsets::VisualEngine::ViewMatrix, &view, sizeof(view))) return false;
        if (!AimReadRaw(ve.address + Offsets::VisualEngine::Dimensions, &viewport, sizeof(viewport))) return false;
        if (viewport.x <= 0.0f || viewport.y <= 0.0f) return false;
        return true;
    }

    static bool AimWorldToScreen(const AimVec3& world, AimVec2& out, const AimMatrix4& view, const AimVec2& viewport) {
        const float* m = view.data;
        float w_x = world.x * m[12] + world.y * m[13] + world.z * m[14] + m[15];
        if (w_x < 0.01f) return false;
        float screen_x = world.x * m[0] + world.y * m[1] + world.z * m[2] + m[3];
        float screen_y = world.x * m[4] + world.y * m[5] + world.z * m[6] + m[7];
        float inv_w = 1.0f / w_x;
        out.x = (viewport.x * 0.5f * screen_x * inv_w) + (viewport.x * 0.5f);
        out.y = -(viewport.y * 0.5f * screen_y * inv_w) + (viewport.y * 0.5f);
        if (out.x != out.x || out.y != out.y) return false;
        return true;
    }

    static AimVec3 AimNormalize(AimVec3 v) {
        float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        if (len < 0.0001f) return v;
        return { v.x / len, v.y / len, v.z / len };
    }

    static AimVec3 AimCross(AimVec3 a, AimVec3 b) {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }

    static void ComputeLookAt(const AimVec3& from, const AimVec3& to, float out[9]) {
        AimVec3 forward = AimNormalize({ to.x - from.x, to.y - from.y, to.z - from.z });
        AimVec3 right = AimNormalize(AimCross({ 0, 1, 0 }, forward));
        AimVec3 up = AimCross(forward, right);

        out[0] = -right.x;  out[1] = up.x;  out[2] = -forward.x;
        out[3] = right.y;   out[4] = up.y;  out[5] = -forward.y;
        out[6] = -right.z;  out[7] = up.z;  out[8] = -forward.z;
    }

    static uintptr_t locked_head = 0;
    static uintptr_t locked_player = 0;
    static bool was_key_held = false;
    static DWORD last_target_search_time = 0;
    static DWORD last_aim_update_time = 0;
    static constexpr DWORD k_target_search_interval_ms = 100;
    static constexpr DWORD k_aim_update_interval_ms = 8;

    static uintptr_t GetPartFromSkeleton(const cache::SkeletonEntity& skel, int part_index) {
        switch (part_index) {
        case 0: return skel.head;
        case 1: return skel.upper_torso;
        case 2: return skel.lower_torso;
        case 3: return skel.left_hand;
        case 4: return skel.right_hand;
        case 5: return skel.left_foot;
        case 6: return skel.right_foot;
        default: return skel.head;
        }
    }

    static bool FindClosestTarget(const AimMatrix4& view, const AimVec2& viewport, const AimVec2& fov_center, uintptr_t& out_part, uintptr_t& out_player) {
        float best_dist_sq = fov_size * fov_size;
        bool found = false;

        const auto& skeletons = cache::GetSkeletonEntities();
        for (const auto& skel : skeletons) {
            uintptr_t part = GetPartFromSkeleton(skel, aimbot_part);
            if (!part) part = skel.head;
            if (!part) continue;

            AimVec3 part_pos{};
            if (!AimReadRaw(part + Offsets::Primitive::Position, &part_pos, sizeof(part_pos))) continue;

            AimVec2 screen_pos{};
            if (!AimWorldToScreen(part_pos, screen_pos, view, viewport)) continue;

            float dx = screen_pos.x - fov_center.x;
            float dy = screen_pos.y - fov_center.y;
            float dist_sq = dx * dx + dy * dy;

            if (dist_sq < best_dist_sq) {
                best_dist_sq = dist_sq;
                out_part = part;
                out_player = skel.player_address;
                found = true;
            }
        }

        if (!found) {
            const auto& entities = cache::GetEspEntities();
            for (const auto& entity : entities) {
                if (entity.root_x == 0.0f && entity.root_y == 0.0f && entity.root_z == 0.0f) continue;
                if (entity.primitive_count == 0) continue;

                AimVec3 root_pos = { entity.root_x, entity.root_y, entity.root_z };
                AimVec2 screen_pos{};
                if (!AimWorldToScreen(root_pos, screen_pos, view, viewport)) continue;

                float dx = screen_pos.x - fov_center.x;
                float dy = screen_pos.y - fov_center.y;
                float dist_sq = dx * dx + dy * dy;

                if (dist_sq < best_dist_sq) {
                    best_dist_sq = dist_sq;
                    out_part = entity.primitives[0];
                    out_player = entity.player_address;
                    found = true;
                }
            }
        }

        return found;
    }

    static uintptr_t FindPartForPlayer(uintptr_t player_address) {
        const auto& skeletons = cache::GetSkeletonEntities();
        for (const auto& skel : skeletons) {
            if (skel.player_address != player_address) continue;
            uintptr_t part = GetPartFromSkeleton(skel, aimbot_part);
            if (part) return part;
            if (skel.head) return skel.head;
        }
        return 0;
    }

    static void AimAtPrimitive(uintptr_t head_prim) {
        AimVec3 cam_pos{};
        float cam_rot[9]{};
        if (!GetCameraTransform(cam_pos, cam_rot)) return;

        AimVec3 target_pos{};
        if (!AimReadRaw(head_prim + Offsets::Primitive::Position, &target_pos, sizeof(target_pos))) return;

        if (prediction_enabled) {
            AimVec3 velocity{};
            if (AimReadRaw(head_prim + Offsets::Primitive::AssemblyLinearVelocity, &velocity, sizeof(velocity))) {
                target_pos.x += velocity.x / prediction_x;
                target_pos.y += velocity.y / prediction_y;
                target_pos.z += velocity.z / prediction_x;
            }
        }

        float target_rot[9]{};
        ComputeLookAt(cam_pos, target_pos, target_rot);

        float factor_x = 1.0f / smoothing_x;
        float factor_y = 1.0f / smoothing_y;
        if (factor_x > 1.0f) factor_x = 1.0f;
        if (factor_y > 1.0f) factor_y = 1.0f;

        float new_rot[9]{};
        for (int i = 0; i < 9; ++i) {
            float factor = (i % 3 == 0) ? factor_x : factor_y;
            new_rot[i] = cam_rot[i] + (target_rot[i] - cam_rot[i]) * factor;
        }

        instance cam = GetCameraInstance();
        if (!cam.is_valid()) return;
        write_raw(cam.address + Offsets::Camera::Rotation, new_rot, sizeof(new_rot));
    }

    static void AimAtPrimitiveMouse(uintptr_t head_prim) {
        AimMatrix4 view{};
        AimVec2 viewport{};
        if (!GetViewData(view, viewport)) return;

        AimVec3 target_pos{};
        if (!AimReadRaw(head_prim + Offsets::Primitive::Position, &target_pos, sizeof(target_pos))) return;

        if (prediction_enabled) {
            AimVec3 velocity{};
            if (AimReadRaw(head_prim + Offsets::Primitive::AssemblyLinearVelocity, &velocity, sizeof(velocity))) {
                target_pos.x += velocity.x / prediction_x;
                target_pos.y += velocity.y / prediction_y;
                target_pos.z += velocity.z / prediction_x;
            }
        }

        AimVec2 target_screen{};
        if (!AimWorldToScreen(target_pos, target_screen, view, viewport)) return;

        static int cached_dpi_x = 0, cached_dpi_y = 0;
        static bool dpi_cached = false;

        if (!dpi_cached) {
            HDC hdc = GetDC(nullptr);
            cached_dpi_x = GetDeviceCaps(hdc, LOGPIXELSX);
            cached_dpi_y = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(nullptr, hdc);
            if (cached_dpi_x <= 0) cached_dpi_x = 96;
            if (cached_dpi_y <= 0) cached_dpi_y = 96;
            dpi_cached = true;
        }

        POINT cur;
        GetCursorPos(&cur);

        float dx = target_screen.x - (float)cur.x;
        float dy = target_screen.y - (float)cur.y;

        float factor_x = 1.0f / smoothing_x;
        float factor_y = 1.0f / smoothing_y;
        if (factor_x > 1.0f) factor_x = 1.0f;
        if (factor_y > 1.0f) factor_y = 1.0f;

        dx *= factor_x;
        dy *= factor_y;

        LONG move_x = (LONG)dx;
        LONG move_y = (LONG)dy;

        static DWORD last_mouse_aim_time = 0;
        DWORD now = GetTickCount();
        if (now - last_mouse_aim_time < 2) return;
        last_mouse_aim_time = now;

        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = move_x;
        input.mi.dy = move_y;
        SendInput(1, &input, sizeof(INPUT));
    }

    void RunAimbot() {
        if (!aimbot_enabled || aimbot_keybind == 0) {
            locked_head = 0;
            locked_player = 0;
            was_key_held = false;
            return;
        }

        bool key_held = (GetAsyncKeyState(aimbot_keybind) & 0x8000) != 0;

        if (!key_held) {
            locked_head = 0;
            locked_player = 0;
            was_key_held = false;
            return;
        }

        DWORD now_aim = GetTickCount();

        if (!sticky_aim) {
            AimMatrix4 view{};
            AimVec2 viewport{};
            if (!GetViewData(view, viewport)) return;

            POINT cur;
            GetCursorPos(&cur);
            AimVec2 fov_center = { (float)cur.x, (float)cur.y };

            uintptr_t found_part = 0;
            uintptr_t found_player = 0;
            if (!FindClosestTarget(view, viewport, fov_center, found_part, found_player)) return;

            if (aimbot_aim_type == 0)
                AimAtPrimitive(found_part);
            else
                AimAtPrimitiveMouse(found_part);
            return;
        }

        if (!was_key_held) {
            AimMatrix4 view{};
            AimVec2 viewport{};
            if (!GetViewData(view, viewport)) return;

            POINT cur;
            GetCursorPos(&cur);
            AimVec2 fov_center = { (float)cur.x, (float)cur.y };

            uintptr_t found_part = 0;
            uintptr_t found_player = 0;
            if (!FindClosestTarget(view, viewport, fov_center, found_part, found_player)) {
                was_key_held = true;
                return;
            }

            locked_head = found_part;
            locked_player = found_player;
            last_aim_update_time = now_aim;
        }

        if (locked_player != 0) {
            uintptr_t refreshed = FindPartForPlayer(locked_player);
            if (refreshed) {
                locked_head = refreshed;
            }

            if (is_valid_address(locked_head)) {
                AimVec3 test{};
                if (AimReadRaw(locked_head + Offsets::Primitive::Position, &test, sizeof(test))) {
                    bool do_aim = (now_aim - last_aim_update_time) >= k_aim_update_interval_ms;
                    if (do_aim) {
                        last_aim_update_time = now_aim;
                        if (aimbot_aim_type == 0)
                            AimAtPrimitive(locked_head);
                        else
                            AimAtPrimitiveMouse(locked_head);
                    }
                    was_key_held = true;
                    return;
                }
            }

            locked_head = 0;
        }

        was_key_held = true;
    }

    void RenderFOV() {
        if (!show_fov) return;

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw) return;

        POINT cur;
        GetCursorPos(&cur);
        draw->AddCircle(ImVec2((float)cur.x, (float)cur.y), fov_size, IM_COL32(255, 255, 255, 180), 64, 1.0f);
    }
}

