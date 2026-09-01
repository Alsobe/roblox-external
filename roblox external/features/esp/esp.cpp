#include <Windows.h>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <unordered_set>
#include "esp.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "overlay.h"
#include "../mesh_gpu/mesh_gpu.h"
#include "../assetmesh/asset_mesh.h"
#include "imgui/imgui.h"
#include "../avatarmesh/avatar_mesh.h"
#include <clipper2/clipper.h>

namespace features {

    struct Vec2 {
        float x;
        float y;
    };

    struct Vec3 {
        float x;
        float y;
        float z;
    };

    struct Matrix4 {
        float data[16];
    };

    struct Box2D {
        float min_x;        float min_y;

        float max_x;
        float max_y;
        bool valid;
    };

    enum R15ChainId {
        ChainNone = -1,
        ChainLeftArm = 0,
        ChainRightArm = 1,
        ChainLeftLeg = 2,
        ChainRightLeg = 3,
        ChainCount = 4,
    };

    static std::unordered_set<uint64_t> g_logged_missing_mesh_asset;
    static std::unordered_set<uint64_t> g_logged_missing_mesh_data;

    static bool ReadRaw(uint64_t address, void* buffer, size_t size) {
        return read_raw(address, buffer, size);
    }

    static bool ReadVec2(uint64_t address, Vec2& out) {
        return ReadRaw(address, &out, sizeof(out));
    }

    static bool ReadVec3(uint64_t address, Vec3& out) {
        return ReadRaw(address, &out, sizeof(out));
    }

    static bool ReadMatrix(uint64_t address, Matrix4& out) {
        return ReadRaw(address, &out, sizeof(out));
    }

    static float Dot(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static float LengthSq(const Vec3& v) {
        return v.x * v.x + v.y * v.y + v.z * v.z;
    }

    static Vec3 Normalize(const Vec3& v) {
        float len_sq = LengthSq(v);
        if (len_sq <= 0.000001f) return { 0.0f, 0.0f, 0.0f };
        float inv = 1.0f / sqrtf(len_sq);
        return { v.x * inv, v.y * inv, v.z * inv };
    }

    static Vec3 Sub(const Vec3& a, const Vec3& b) {
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    }

    static Vec3 ToLocal(const float* m, const Vec3& world_delta) {
        return {
            m[0] * world_delta.x + m[3] * world_delta.y + m[6] * world_delta.z,
            m[1] * world_delta.x + m[4] * world_delta.y + m[7] * world_delta.z,
            m[2] * world_delta.x + m[5] * world_delta.y + m[8] * world_delta.z
        };
    }

    static void RotateYVariant(int variant, float in_x, float in_y, float in_z, float& out_x, float& out_y, float& out_z) {
        switch (variant & 3) {
        case 1: out_x = -in_x; out_y = in_y; out_z = -in_z; break;
        case 2: out_x = in_z; out_y = in_y; out_z = -in_x; break;
        case 3: out_x = -in_z; out_y = in_y; out_z = in_x; break;
        default: out_x = in_x; out_y = in_y; out_z = in_z; break;
        }
    }

    static int FindEntityPartIndex(const cache::EspEntity& entity, const char* part_name) {
        for (size_t i = 0; i < entity.primitive_count; ++i) {
            if (strcmp(entity.part_names[i], part_name) == 0) return (int)i;
        }
        return -1;
    }

    static const char* GetR15ChainNames(R15ChainId chain_id, const char*& anchor, const char*& upper, const char*& lower, const char*& end) {
        switch (chain_id) {
        case ChainLeftArm:
            anchor = "UpperTorso"; upper = "LeftUpperArm"; lower = "LeftLowerArm"; end = "LeftHand"; return upper;
        case ChainRightArm:
            anchor = "UpperTorso"; upper = "RightUpperArm"; lower = "RightLowerArm"; end = "RightHand"; return upper;
        case ChainLeftLeg:
            anchor = "LowerTorso"; upper = "LeftUpperLeg"; lower = "LeftLowerLeg"; end = "LeftFoot"; return upper;
        case ChainRightLeg:
            anchor = "LowerTorso"; upper = "RightUpperLeg"; lower = "RightLowerLeg"; end = "RightFoot"; return upper;
        default:
            anchor = nullptr; upper = nullptr; lower = nullptr; end = nullptr; return nullptr;
        }
    }

    static R15ChainId GetR15ChainId(const char* part_name) {
        if (strcmp(part_name, "LeftUpperArm") == 0 || strcmp(part_name, "LeftLowerArm") == 0 || strcmp(part_name, "LeftHand") == 0) return ChainLeftArm;
        if (strcmp(part_name, "RightUpperArm") == 0 || strcmp(part_name, "RightLowerArm") == 0 || strcmp(part_name, "RightHand") == 0) return ChainRightArm;
        if (strcmp(part_name, "LeftUpperLeg") == 0 || strcmp(part_name, "LeftLowerLeg") == 0 || strcmp(part_name, "LeftFoot") == 0) return ChainLeftLeg;
        if (strcmp(part_name, "RightUpperLeg") == 0 || strcmp(part_name, "RightLowerLeg") == 0 || strcmp(part_name, "RightFoot") == 0) return ChainRightLeg;
        return ChainNone;
    }

    static int DetermineR15ChainVariant(const cache::EspEntity& entity, const avatarmesh::avatar_mesh* avatar, R15ChainId chain_id) {
        const char* anchor_name;
        const char* upper_name;
        const char* lower_name;
        const char* end_name;
        if (!GetR15ChainNames(chain_id, anchor_name, upper_name, lower_name, end_name)) return 0;

        const avatarmesh::mesh_part* anchor_mesh = avatarmesh::find_part(avatar, anchor_name, true);
        const avatarmesh::mesh_part* upper_mesh = avatarmesh::find_part(avatar, upper_name, true);
        const avatarmesh::mesh_part* lower_mesh = avatarmesh::find_part(avatar, lower_name, true);
        const avatarmesh::mesh_part* end_mesh = avatarmesh::find_part(avatar, end_name, true);
        if (!anchor_mesh || !upper_mesh || !lower_mesh || !end_mesh) return 0;

        int anchor_idx = FindEntityPartIndex(entity, anchor_name);
        int upper_idx = FindEntityPartIndex(entity, upper_name);
        int lower_idx = FindEntityPartIndex(entity, lower_name);
        int end_idx = FindEntityPartIndex(entity, end_name);
        if (anchor_idx < 0 || upper_idx < 0 || lower_idx < 0 || end_idx < 0) return 0;

        uintptr_t anchor_prim = entity.primitives[anchor_idx];
        uintptr_t upper_prim = entity.primitives[upper_idx];
        uintptr_t lower_prim = entity.primitives[lower_idx];
        uintptr_t end_prim = entity.primitives[end_idx];
        if (!is_valid_address(anchor_prim) || !is_valid_address(upper_prim) || !is_valid_address(lower_prim) || !is_valid_address(end_prim)) return 0;

        struct { float rot[9]; Vec3 pos; } anchor_rp{};
        if (!ReadRaw(anchor_prim + Offsets::Primitive::Rotation, &anchor_rp, sizeof(anchor_rp))) return 0;
        Vec3 upper_pos{};
        Vec3 lower_pos{};
        Vec3 end_pos{};
        if (!ReadVec3(upper_prim + Offsets::Primitive::Position, upper_pos)) return 0;
        if (!ReadVec3(lower_prim + Offsets::Primitive::Position, lower_pos)) return 0;
        if (!ReadVec3(end_prim + Offsets::Primitive::Position, end_pos)) return 0;

        Vec3 live_a = ToLocal(anchor_rp.rot, Sub(upper_pos, anchor_rp.pos));
        Vec3 live_b = ToLocal(anchor_rp.rot, Sub(lower_pos, upper_pos));
        Vec3 live_c = ToLocal(anchor_rp.rot, Sub(end_pos, lower_pos));
        live_a = Normalize(live_a);
        live_b = Normalize(live_b);
        live_c = Normalize(live_c);

        Vec3 obj_a = Normalize({ upper_mesh->center.x - anchor_mesh->center.x, upper_mesh->center.y - anchor_mesh->center.y, upper_mesh->center.z - anchor_mesh->center.z });
        Vec3 obj_b = Normalize({ lower_mesh->center.x - upper_mesh->center.x, lower_mesh->center.y - upper_mesh->center.y, lower_mesh->center.z - upper_mesh->center.z });
        Vec3 obj_c = Normalize({ end_mesh->center.x - lower_mesh->center.x, end_mesh->center.y - lower_mesh->center.y, end_mesh->center.z - lower_mesh->center.z });

        int best_variant = 0;
        float best_score = -1e9f;
        for (int variant = 0; variant < 4; ++variant) {
            float ax, ay, az, bx, by, bz, cx, cy, cz;
            RotateYVariant(variant, obj_a.x, obj_a.y, obj_a.z, ax, ay, az);
            RotateYVariant(variant, obj_b.x, obj_b.y, obj_b.z, bx, by, bz);
            RotateYVariant(variant, obj_c.x, obj_c.y, obj_c.z, cx, cy, cz);
            float score =
                Dot(Normalize({ ax, ay, az }), live_a) * 2.0f +
                Dot(Normalize({ bx, by, bz }), live_b) +
                Dot(Normalize({ cx, cy, cz }), live_c);
            if (score > best_score) {
                best_score = score;
                best_variant = variant;
            }
        }
        return best_variant;
    }

    static bool GetCamera(Matrix4& view, Vec2& viewport) {
        instance ve = read<instance>(g_base_address + Offsets::VisualEngine::Pointer);
        if (!ve.is_valid()) return false;
        if (!ReadMatrix(ve.address + Offsets::VisualEngine::ViewMatrix, view)) return false;
        if (!ReadVec2(ve.address + Offsets::VisualEngine::Dimensions, viewport)) return false;
        if (viewport.x <= 0.0f || viewport.y <= 0.0f) return false;
        return true;
    }

    static bool WorldToScreen(const Vec3& world, Vec2& out, const Matrix4& view, const Vec2& viewport) {
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

    static bool ComputeBoxForPrimitives(const cache::EspEntity& entity, const Matrix4& view, const Vec2& viewport, Box2D& out_box) {
        if (entity.primitive_count == 0) {
            out_box.valid = false;
            return false;
        }

        bool has_point = false;
        float min_x = 0.0f;
        float min_y = 0.0f;
        float max_x = 0.0f;
        float max_y = 0.0f;

        for (size_t i = 0; i < entity.primitive_count; ++i) {
            uintptr_t primitive = entity.primitives[i];
            if (!is_valid_address(primitive)) continue;
            Vec3 pos{};
            Vec3 size{};
            if (!ReadVec3(primitive + Offsets::Primitive::Position, pos)) continue;
            if (!ReadVec3(primitive + Offsets::Primitive::Size, size)) continue;

            float hx = size.x * 0.5f;
            float hy = size.y * 0.5f;
            float hz = size.z * 0.5f;

            // apply the part's 3x3 rotation matrix, otherwise rotated/tilted parts
            // produce a box that is far too big and drifts as the player turns
            float rot[9] = {};
            bool have_rot = ReadRaw(primitive + Offsets::Primitive::Rotation, rot, sizeof(rot));

            static const float local[8][3] = {
                {-1,-1,-1},{-1,-1, 1},{-1, 1,-1},{-1, 1, 1},
                { 1,-1,-1},{ 1,-1, 1},{ 1, 1,-1},{ 1, 1, 1}
            };

            Vec3 corners[8];
            for (int c = 0; c < 8; ++c) {
                float lx = local[c][0] * hx;
                float ly = local[c][1] * hy;
                float lz = local[c][2] * hz;

                if (have_rot) {
                    corners[c] = {
                        pos.x + rot[0] * lx + rot[1] * ly + rot[2] * lz,
                        pos.y + rot[3] * lx + rot[4] * ly + rot[5] * lz,
                        pos.z + rot[6] * lx + rot[7] * ly + rot[8] * lz
                    };
                } else {
                    corners[c] = { pos.x + lx, pos.y + ly, pos.z + lz };
                }
            }

            for (int c = 0; c < 8; ++c) {
                Vec2 pt{};
                if (WorldToScreen(corners[c], pt, view, viewport)) {
                    if (!has_point) {
                        min_x = max_x = pt.x;
                        min_y = max_y = pt.y;
                        has_point = true;
                    } else {
                        if (pt.x < min_x) min_x = pt.x;
                        if (pt.x > max_x) max_x = pt.x;
                        if (pt.y < min_y) min_y = pt.y;
                        if (pt.y > max_y) max_y = pt.y;
                    }
                }
            }
        }

        if (!has_point) {
            out_box.valid = false;
            return false;
        }

        float w = max_x - min_x;
        float h = max_y - min_y;
        if (w <= 1.0f || h <= 1.0f) {
            out_box.valid = false;
            return false;
        }

        out_box.min_x = min_x;
        out_box.min_y = min_y;
        out_box.max_x = max_x;
        out_box.max_y = max_y;
        out_box.valid = true;
        return true;
    }



    static bool ReadPos(uintptr_t primitive, Vec3& out) {
        if (!is_valid_address(primitive)) return false;
        return ReadVec3(primitive + Offsets::Primitive::Position, out);
    }

    static bool PartToScreen(uintptr_t primitive, const Matrix4& view, const Vec2& viewport, Vec2& out) {
        if (!primitive) return false;
        Vec3 pos{};
        if (!ReadPos(primitive, pos)) return false;
        return WorldToScreen(pos, out, view, viewport);
    }

    static void DrawSkeletonLine(ImDrawList* draw, const Vec2& a, const Vec2& b, ImU32 color) {
        draw->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), IM_COL32(0, 0, 0, 255), 3.0f);
        draw->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), color, 1.0f);
    }

    static void DrawBone(ImDrawList* draw, uintptr_t from, uintptr_t to, const Matrix4& view, const Vec2& viewport, ImU32 color) {
        if (!from || !to) return;
        Vec2 a{}, b{};
        if (!PartToScreen(from, view, viewport, a)) return;
        if (!PartToScreen(to, view, viewport, b)) return;
        DrawSkeletonLine(draw, a, b, color);
    }

    void RenderAimViewer() {
        if (!aimviewer) return;

        Matrix4 view{};
        Vec2 viewport{};
        if (!GetCamera(view, viewport)) return;

        auto skeletons_snap = cache::GetSkeletonSnapshot();
        const auto& skeletons = *skeletons_snap;
        if (skeletons.empty()) return;

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw) return;

        for (const cache::SkeletonEntity& skel : skeletons) {
            if (!skel.head) continue;

            Vec3 head_pos{};
            if (!ReadVec3(skel.head + Offsets::Primitive::Position, head_pos)) continue;

            Vec3 look_direction{ 0.0f, 0.0f, -1.0f };
            bool has_valid_direction = false;

            float head_rot[9]{};
            if (ReadRaw(skel.head + Offsets::Primitive::Rotation, head_rot, sizeof(head_rot))) {
                look_direction = { -head_rot[2], -head_rot[5], -head_rot[8] };
                float len_sq = LengthSq(look_direction);
                if (len_sq > 0.0001f) {
                    look_direction = Normalize(look_direction);
                    has_valid_direction = true;
                }
            }

            if (!has_valid_direction && skel.upper_torso) {
                Vec3 torso_pos{};
                if (ReadVec3(skel.upper_torso + Offsets::Primitive::Position, torso_pos)) {
                    look_direction = Normalize(Sub(head_pos, torso_pos));
                    has_valid_direction = LengthSq(look_direction) > 0.0001f;
                }
            }

            if (!has_valid_direction) continue;

            Vec3 ray_end = {
                head_pos.x + look_direction.x * 40.0f,
                head_pos.y + look_direction.y * 40.0f,
                head_pos.z + look_direction.z * 40.0f
            };

            Vec2 screen_start{};
            Vec2 screen_end{};
            if (!WorldToScreen(head_pos, screen_start, view, viewport)) continue;
            if (!WorldToScreen(ray_end, screen_end, view, viewport)) continue;

            draw->AddLine(
                ImVec2(screen_start.x, screen_start.y),
                ImVec2(screen_end.x, screen_end.y),
                IM_COL32(255, 0, 0, 255),
                2.0f);
        }
    }

    void RenderSkeletonESP() {
        Matrix4 view{};
        Vec2 viewport{};
        if (!GetCamera(view, viewport)) return;

        auto skeletons_snap = cache::GetSkeletonSnapshot();
        const auto& skeletons = *skeletons_snap;
        if (skeletons.empty()) return;

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw) return;

        ImU32 color = IM_COL32(
            (int)(skeleton_color[0] * 255.0f),
            (int)(skeleton_color[1] * 255.0f),
            (int)(skeleton_color[2] * 255.0f),
            (int)(skeleton_color[3] * 255.0f)
        );
        
        ImU32 black = IM_COL32(0, 0, 0, 200);

        // Pre-allocate position cache to reduce ReadProcessMemory calls
        static Vec3 bone_positions[32];
        
        for (const cache::SkeletonEntity& skel : skeletons) {
            if (!skel.head || !skel.upper_torso) continue;

            if (skel.is_r15) {
                DrawBone(draw, skel.head, skel.upper_torso, view, viewport, color);
                DrawBone(draw, skel.upper_torso, skel.lower_torso, view, viewport, color);
                DrawBone(draw, skel.upper_torso, skel.left_upper_arm, view, viewport, color);
                DrawBone(draw, skel.left_upper_arm, skel.left_lower_arm, view, viewport, color);
                DrawBone(draw, skel.left_lower_arm, skel.left_hand, view, viewport, color);
                DrawBone(draw, skel.upper_torso, skel.right_upper_arm, view, viewport, color);
                DrawBone(draw, skel.right_upper_arm, skel.right_lower_arm, view, viewport, color);
                DrawBone(draw, skel.right_lower_arm, skel.right_hand, view, viewport, color);
                DrawBone(draw, skel.lower_torso, skel.left_upper_leg, view, viewport, color);
                DrawBone(draw, skel.left_upper_leg, skel.left_lower_leg, view, viewport, color);
                DrawBone(draw, skel.left_lower_leg, skel.left_foot, view, viewport, color);
                DrawBone(draw, skel.lower_torso, skel.right_upper_leg, view, viewport, color);
                DrawBone(draw, skel.right_upper_leg, skel.right_lower_leg, view, viewport, color);
                DrawBone(draw, skel.right_lower_leg, skel.right_foot, view, viewport, color);
            }
            else {
                DrawBone(draw, skel.head, skel.upper_torso, view, viewport, color);
                DrawBone(draw, skel.upper_torso, skel.left_hand, view, viewport, color);
                DrawBone(draw, skel.upper_torso, skel.right_hand, view, viewport, color);
                DrawBone(draw, skel.upper_torso, skel.left_foot, view, viewport, color);
                DrawBone(draw, skel.upper_torso, skel.right_foot, view, viewport, color);
            }
        }
    }

    static bool OnScreen(const Vec2& pt, const Vec2& viewport) {
        return pt.x >= 0.0f && pt.x <= viewport.x && pt.y >= 0.0f && pt.y <= viewport.y;
    }

    void RenderChinaHatESP() {
        if (!chinahat) return;

        Matrix4 view{};
        Vec2 viewport{};
        if (!GetCamera(view, viewport)) return;

        auto entities_snap = cache::GetEspSnapshot();
        const auto& entities = *entities_snap;
        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        if (entities.empty()) return;

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw) return;

        ImU32 color = IM_COL32(
            (int)(chinahat_color[0] * 255.0f),
            (int)(chinahat_color[1] * 255.0f),
            (int)(chinahat_color[2] * 255.0f),
            (int)(chinahat_color[3] * 255.0f * 0.42f)
        );

        const float hat_height = 1.0f;
        const float hat_radius = 1.5f;
        const int segments = 48;

        for (const cache::EspEntity& entity : entities) {
            int head_idx = FindEntityPartIndex(entity, "Head");
            if (head_idx < 0) continue;

            uintptr_t head_prim = entity.primitives[head_idx];
            if (!is_valid_address(head_prim)) continue;

            if (lp.valid && esp_render_distance > 0.0f) {
                float dx = entity.root_x - lp.x;
                float dy = entity.root_y - lp.y;
                float dz = entity.root_z - lp.z;
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                if (dist > esp_render_distance) continue;
            }

            Vec3 head_pos{};
            if (!ReadVec3(head_prim + Offsets::Primitive::Position, head_pos)) continue;

            Vec3 apex_pos = { head_pos.x, head_pos.y + hat_height + 0.15f, head_pos.z };

            std::vector<Vec3> base_points;
            base_points.reserve(segments);
            for (int i = 0; i < segments; ++i) {
                float angle = (2.0f * 3.14159f * i) / segments;
                base_points.emplace_back(Vec3{
                    head_pos.x + hat_radius * cosf(angle),
                    head_pos.y + 0.2f,
                    head_pos.z + hat_radius * sinf(angle)
                });
            }

            Vec2 apex_screen{};
            if (!WorldToScreen(apex_pos, apex_screen, view, viewport)) continue;

            std::vector<ImVec2> base_screen;
            base_screen.reserve(segments);
            bool any_on_screen = OnScreen(apex_screen, viewport);
            bool all_projected = true;
            for (const auto& point : base_points) {
                Vec2 screen_pos{};
                if (WorldToScreen(point, screen_pos, view, viewport)) {
                    base_screen.emplace_back(screen_pos.x, screen_pos.y);
                    any_on_screen |= OnScreen(screen_pos, viewport);
                } else {
                    all_projected = false;
                    break;
                }
            }
            if (!all_projected || !any_on_screen) continue;

            draw->Flags |= ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines;

            const float apex_soft_radius = 2.0f;
            for (size_t i = 0; i < segments; ++i) {
                size_t next = (i + 1) % segments;
                ImVec2 apex_offset = ImVec2(
                    apex_screen.x + cosf((2.0f * 3.14159f * i) / segments) * apex_soft_radius,
                    apex_screen.y + sinf((2.0f * 3.14159f * i) / segments) * apex_soft_radius
                );
                draw->AddTriangleFilled(
                    apex_offset,
                    base_screen[i],
                    base_screen[next],
                    color
                );
            }

            ImU32 base_color = IM_COL32(
                (int)(chinahat_color[0] * 255.0f),
                (int)(chinahat_color[1] * 255.0f),
                (int)(chinahat_color[2] * 255.0f),
                (int)(chinahat_color[3] * 255.0f * 0.6f * 0.42f)
            );
            draw->AddConvexPolyFilled(base_screen.data(), segments, base_color);

            ImU32 outline_color = IM_COL32(0, 0, 0, 100);
            for (size_t i = 0; i < segments; ++i) {
                size_t next = (i + 1) % segments;
                draw->AddLine(base_screen[i], base_screen[next], outline_color, 1.2f);
            }

            draw->Flags &= ~(ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines);
        }
    }

    static ImFont* GetEspFont() {
        ImGuiIO& io = ImGui::GetIO();
        if (io.Fonts && io.Fonts->Fonts.Size > 1)
            return io.Fonts->Fonts[1];
        if (io.Fonts && io.Fonts->Fonts.Size > 0)
            return io.Fonts->Fonts[0];
        return nullptr;
    }

    static void DrawTextWithShadow(ImDrawList* draw, float font_size, const ImVec2& position, ImU32 color, const char* text) {
        if (!draw || !text) return;
        ImFont* font = GetEspFont();
        ImU32 shadow = IM_COL32(0, 0, 0, 255);
        if (font) {
            for (int i = -1; i <= 1; i++) {
                for (int j = -1; j <= 1; j++) {
                    if (i == 0 && j == 0) continue;
                    draw->AddText(font, font_size, ImVec2(position.x + i, position.y + j), shadow, text);
                }
            }
            draw->AddText(font, font_size, position, color, text);
        } else {
            for (int i = -1; i <= 1; i++) {
                for (int j = -1; j <= 1; j++) {
                    if (i == 0 && j == 0) continue;
                    draw->AddText(nullptr, font_size, ImVec2(position.x + i, position.y + j), shadow, text);
                }
            }
            draw->AddText(nullptr, font_size, position, color, text);
        }
    }

    static void RenderHealthBar(ImDrawList* draw, float x1, float y1, float x2, float y2, float health, float max_health) {
        float health_percent = (max_health > 0.0f) ? (health / max_health) : 0.0f;
        health_percent = (std::max)(0.0f, (std::min)(1.0f, health_percent));

        float box_height = y2 - y1;
        float bar_gap = (std::max)(2.0f, (std::min)(7.0f, box_height * 0.035f));
        float bar_x = x1 - bar_gap;

        ImU32 outline = IM_COL32(0, 0, 0, 255);
        ImU32 background = IM_COL32(45, 45, 45, 220);

        draw->AddRectFilled(ImVec2(bar_x, y1 - 1), ImVec2(bar_x + 1, y2 + 1), outline);
        draw->AddRectFilled(ImVec2(bar_x + 2, y1 - 1), ImVec2(bar_x + 3, y2 + 1), outline);
        draw->AddRectFilled(ImVec2(bar_x, y1 - 1), ImVec2(bar_x + 3, y1), outline);
        draw->AddRectFilled(ImVec2(bar_x, y2), ImVec2(bar_x + 3, y2 + 1), outline);

        draw->AddRectFilled(ImVec2(bar_x + 1, y1), ImVec2(bar_x + 2, y2), background);

        float fill_height = box_height * health_percent;
        int r = (int)((1.0f - health_percent) * 255.0f);
        int g = (int)(health_percent * 255.0f);
        ImU32 bar_color = IM_COL32(r, g, 0, 255);

        draw->AddRectFilled(
            ImVec2(bar_x + 1, floorf(y2 - fill_height)),
            ImVec2(bar_x + 2, ceilf(y2)),
            bar_color
        );
    }

    static void RenderName(ImDrawList* draw, float x1, float y1, float x2, float y2, const char* name) {
        if (!name || name[0] == '\0') return;
        const float font_size = 12.0f;
        ImFont* font = GetEspFont();
        ImVec2 text_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, name) : ImGui::CalcTextSize(name);
        float center_x = (x1 + x2) * 0.5f;
        float x_pos = floorf(center_x - text_size.x * 0.5f + 0.5f);
        float y_pos = floorf(y1 - text_size.y - 2.0f + 0.5f);
        ImU32 col = IM_COL32(
            (int)(name_color[0] * 255.0f),
            (int)(name_color[1] * 255.0f),
            (int)(name_color[2] * 255.0f),
            (int)(name_color[3] * 255.0f)
        );
        DrawTextWithShadow(draw, font_size, ImVec2(x_pos, y_pos), col, name);
    }

    static void RenderHealthText(ImDrawList* draw, float x1, float y1, float x2, float y2, float health) {
        char buf[32];
        sprintf_s(buf, "[%.0f]", health);
        const float font_size = 12.0f;
        ImFont* font = GetEspFont();
        ImVec2 text_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, buf) : ImGui::CalcTextSize(buf);
        float x_pos = x1 - 6.0f - text_size.x;
        if (healthbar) {
            x_pos -= 7.0f;
        }
        float y_pos = floorf(y1 - 3.0f + 0.5f);
        ImU32 col = IM_COL32(
            (int)(health_text_color[0] * 255.0f),
            (int)(health_text_color[1] * 255.0f),
            (int)(health_text_color[2] * 255.0f),
            (int)(health_text_color[3] * 255.0f)
        );
        DrawTextWithShadow(draw, font_size, ImVec2(x_pos, y_pos), col, buf);
    }

    static void RenderRigType(ImDrawList* draw, float x1, float y1, float x2, float y2, bool is_r15) {
        const char* text = is_r15 ? "[R15]" : "[R6]";
        const float font_size = 12.0f;
        ImFont* font = GetEspFont();
        ImVec2 text_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text) : ImGui::CalcTextSize(text);
        float x_pos = floorf(x2 + 5.0f + 0.5f);
        float y_pos = floorf(y1 - text_size.y + 10.0f + 0.5f);
        ImU32 col = IM_COL32(
            (int)(rig_type_color[0] * 255.0f),
            (int)(rig_type_color[1] * 255.0f),
            (int)(rig_type_color[2] * 255.0f),
            (int)(rig_type_color[3] * 255.0f)
        );
        DrawTextWithShadow(draw, font_size, ImVec2(x_pos, y_pos), col, text);
    }

    static void RenderTool(ImDrawList* draw, float x1, float y1, float x2, float y2, const char* tool_name, bool has_distance) {
        char buf[96];
        if (!tool_name || tool_name[0] == '\0') {
            buf[0] = '['; buf[1] = 'N'; buf[2] = 'o'; buf[3] = 'n'; buf[4] = 'e'; buf[5] = ']'; buf[6] = '\0';
        } else {
            buf[0] = '[';
            int i = 1;
            for (const char* p = tool_name; *p && i < 92; ++p) {
                if (*p != '[' && *p != ']') buf[i++] = *p;
            }
            buf[i++] = ']';
            buf[i] = '\0';
        }
        const float font_size = 12.0f;
        ImFont* font = GetEspFont();
        ImVec2 text_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, buf) : ImGui::CalcTextSize(buf);
        float center_x = (x1 + x2) * 0.5f;
        float x_pos = floorf(center_x - text_size.x * 0.5f + 0.5f);
        float y_pos = floorf(y2 + (has_distance ? 18.0f : 3.0f) + 0.5f);
        ImU32 col = IM_COL32(
            (int)(tool_color[0] * 255.0f),
            (int)(tool_color[1] * 255.0f),
            (int)(tool_color[2] * 255.0f),
            (int)(tool_color[3] * 255.0f)
        );
        DrawTextWithShadow(draw, font_size, ImVec2(x_pos, y_pos), col, buf);
    }

    static void RenderDistance(ImDrawList* draw, float x1, float y1, float x2, float y2, float distance) {
        char buf[32];
        sprintf_s(buf, "%.0f studs", distance);
        const float font_size = 12.0f;
        ImFont* font = GetEspFont();
        ImVec2 text_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, buf) : ImGui::CalcTextSize(buf);
        float center_x = (x1 + x2) * 0.5f;
        float x_pos = floorf(center_x - text_size.x * 0.5f + 0.5f);
        float y_pos = floorf(y2 + 2.0f + 0.5f);
        ImU32 col = IM_COL32(
            (int)(distance_color[0] * 255.0f),
            (int)(distance_color[1] * 255.0f),
            (int)(distance_color[2] * 255.0f),
            (int)(distance_color[3] * 255.0f)
        );
        DrawTextWithShadow(draw, font_size, ImVec2(x_pos, y_pos), col, buf);
    }

    static int ComputeConvexHull(ImVec2* pts, int n) {
        if (n < 3) return n;
        std::sort(pts, pts + n, [](const ImVec2& a, const ImVec2& b) {
            return a.x < b.x || (a.x == b.x && a.y < b.y);
        });
        static ImVec2 hull[256];
        int k = 0;
        for (int i = 0; i < n; i++) {
            while (k >= 2 && (hull[k-1].x - hull[k-2].x) * (pts[i].y - hull[k-2].y)
                            - (hull[k-1].y - hull[k-2].y) * (pts[i].x - hull[k-2].x) <= 0.0f)
                k--;
            hull[k++] = pts[i];
        }
        int lower = k + 1;
        for (int i = n - 2; i >= 0; i--) {
            while (k >= lower && (hull[k-1].x - hull[k-2].x) * (pts[i].y - hull[k-2].y)
                                - (hull[k-1].y - hull[k-2].y) * (pts[i].x - hull[k-2].x) <= 0.0f)
                k--;
            hull[k++] = pts[i];
        }
        k--;
        for (int i = 0; i < k; i++) pts[i] = hull[i];
        return k;
    }

    void RenderChams() {
        Matrix4 view{};
        Vec2 viewport{};
        if (!GetCamera(view, viewport)) return;

        auto entities_snap = cache::GetEspSnapshot();
        const auto& entities = *entities_snap;
        if (entities.empty()) return;

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw) return;

        ImU32 fill = IM_COL32(
            (int)(chams_color[0] * 255.0f),
            (int)(chams_color[1] * 255.0f),
            (int)(chams_color[2] * 255.0f),
            (int)(chams_color[3] * 255.0f)
        );

        static const int faces[6][4] = {
            {0, 1, 3, 2},
            {4, 6, 7, 5},
            {0, 4, 5, 1},
            {2, 3, 7, 6},
            {0, 2, 6, 4},
            {1, 5, 7, 3},
        };

        for (const cache::EspEntity& entity : entities) {
            for (size_t i = 0; i < entity.primitive_count; ++i) {
                uintptr_t prim = entity.primitives[i];
                if (!is_valid_address(prim)) continue;

                struct { float rot[9]; Vec3 pos; } rp{};
                if (!ReadRaw(prim + Offsets::Primitive::Rotation, &rp, sizeof(rp))) continue;

                Vec3 sz{};
                if (!ReadVec3(prim + Offsets::Primitive::Size, sz)) continue;

                float hx = sz.x * 0.5f, hy = sz.y * 0.5f, hz = sz.z * 0.5f;

                float lx[8] = {-hx, -hx, -hx, -hx, hx, hx, hx, hx};
                float ly[8] = {-hy, -hy, hy, hy, -hy, -hy, hy, hy};
                float lz[8] = {-hz, hz, -hz, hz, -hz, hz, -hz, hz};

                Vec3 corners[8];
                for (int c = 0; c < 8; ++c) {
                    corners[c].x = rp.rot[0] * lx[c] + rp.rot[1] * ly[c] + rp.rot[2] * lz[c] + rp.pos.x;
                    corners[c].y = rp.rot[3] * lx[c] + rp.rot[4] * ly[c] + rp.rot[5] * lz[c] + rp.pos.y;
                    corners[c].z = rp.rot[6] * lx[c] + rp.rot[7] * ly[c] + rp.rot[8] * lz[c] + rp.pos.z;
                }

                Vec2 screen[8];
                bool ok[8];
                for (int c = 0; c < 8; ++c)
                    ok[c] = WorldToScreen(corners[c], screen[c], view, viewport);

                for (int f = 0; f < 6; ++f) {
                    int a = faces[f][0], b = faces[f][1], ci = faces[f][2], d = faces[f][3];
                    if (!ok[a] || !ok[b] || !ok[ci] || !ok[d]) continue;
                    ImVec2 pts[4] = {
                        {screen[a].x, screen[a].y},
                        {screen[b].x, screen[b].y},
                        {screen[ci].x, screen[ci].y},
                        {screen[d].x, screen[d].y},
                    };
                    draw->AddConvexPolyFilled(pts, 4, fill);
                }
            }
        }
    }

    static constexpr double CLIPPER_SCALE = 100.0;

    static void DrawMergedPoly(const Clipper2Lib::Path64& poly, ImDrawList* draw, ImU32 col) {
        int n = (int)poly.size();
        if (n < 3 || n > 256) return;

        static ImVec2 verts[256];
        static int idx[256];

        for (int i = 0; i < n; i++)
            verts[i] = {(float)(poly[i].x / CLIPPER_SCALE), (float)(poly[i].y / CLIPPER_SCALE)};

        float area = 0.0f;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            area += verts[i].x * verts[j].y - verts[j].x * verts[i].y;
        }
        for (int i = 0; i < n; i++) idx[i] = (area > 0.0f) ? i : (n - 1 - i);

        static ImVec2 tris[768];
        int tri_count = 0;
        int nv = n;
        int safety = nv * nv;
        int vi = 0;

        while (nv > 2 && safety-- > 0) {
            int u = vi % nv;
            int v = (vi + 1) % nv;
            int w = (vi + 2) % nv;
            const ImVec2& pu = verts[idx[u]];
            const ImVec2& pv = verts[idx[v]];
            const ImVec2& pw = verts[idx[w]];
            float cross = (pv.x - pu.x) * (pw.y - pu.y) - (pv.y - pu.y) * (pw.x - pu.x);
            if (cross > 0.0f) {
                bool ear = true;
                for (int k2 = 0; k2 < nv; k2++) {
                    if (k2 == u || k2 == v || k2 == w) continue;
                    const ImVec2& pt = verts[idx[k2]];
                    float d1 = (pt.x - pv.x) * (pu.y - pv.y) - (pu.x - pv.x) * (pt.y - pv.y);
                    float d2 = (pt.x - pw.x) * (pv.y - pw.y) - (pv.x - pw.x) * (pt.y - pw.y);
                    float d3 = (pt.x - pu.x) * (pw.y - pu.y) - (pw.x - pu.x) * (pt.y - pu.y);
                    if (!((d1 < 0) || (d2 < 0) || (d3 < 0)) || !((d1 > 0) || (d2 > 0) || (d3 > 0))) {
                        ear = false;
                        break;
                    }
                }
                if (ear) {
                    tris[tri_count * 3] = pu;
                    tris[tri_count * 3 + 1] = pv;
                    tris[tri_count * 3 + 2] = pw;
                    tri_count++;
                    for (int k2 = v; k2 < nv - 1; k2++) idx[k2] = idx[k2 + 1];
                    nv--;
                    if (vi > 0) vi--;
                    safety = nv * nv;
                    continue;
                }
            }
            vi++;
        }

        if (tri_count == 0) return;
        ImVec2 uv = ImGui::GetIO().Fonts->TexUvWhitePixel;
        draw->PrimReserve(tri_count * 3, tri_count * 3);
        for (int i = 0; i < tri_count; i++) {
            ImDrawIdx base = (ImDrawIdx)draw->_VtxCurrentIdx;
            draw->PrimWriteIdx(base);
            draw->PrimWriteIdx(base + 1);
            draw->PrimWriteIdx(base + 2);
            draw->PrimWriteVtx(tris[i * 3], uv, col);
            draw->PrimWriteVtx(tris[i * 3 + 1], uv, col);
            draw->PrimWriteVtx(tris[i * 3 + 2], uv, col);
        }
    }

    void RenderMeshChams() {
        Matrix4 view{};
        Vec2 viewport{};
        if (!GetCamera(view, viewport)) return;

        auto entities_snap = cache::GetEspSnapshot();
        const auto& entities = *entities_snap;
        if (entities.empty()) return;

        static DWORD last_stats_print = 0;
        DWORD now = GetTickCount();
        if (now - last_stats_print >= 5000) {
            last_stats_print = now;
            assetmesh::debug_stats stats = assetmesh::get_debug_stats();
        }

        if (union_chams) {
            ImDrawList* draw = ImGui::GetBackgroundDrawList();
            if (!draw) return;

            ImU32 fill = IM_COL32(
                (int)(mesh_chams_color[0] * 255.0f),
                (int)(mesh_chams_color[1] * 255.0f),
                (int)(mesh_chams_color[2] * 255.0f),
                (int)(mesh_chams_color[3] * 255.0f)
            );

            static ImVec2 mesh_pts[512];
            static Clipper2Lib::Paths64 mesh_polys;

            for (const cache::EspEntity& entity : entities) {
                mesh_polys.clear();
                mesh_polys.reserve(entity.primitive_count);

                for (size_t i = 0; i < entity.primitive_count; ++i) {
                    uint64_t asset_id = assetmesh::get_mesh_asset_id_from_part(entity.part_addresses[i]);
                    if (!asset_id && entity.user_id != 0) {
                        assetmesh::request_avatar_assets(entity.user_id);
                        asset_id = assetmesh::get_mesh_asset_id_for_part(entity.user_id, entity.part_names[i], entity.is_r15);
                        if (!asset_id) {
                            asset_id = assetmesh::get_default_asset_for_part(entity.part_names[i], entity.is_r15);
                        }
                    }
                    if (!asset_id) {
                        uint64_t key = (static_cast<uint64_t>(entity.user_id) << 32) | static_cast<uint64_t>(i);
                        g_logged_missing_mesh_asset.insert(key);
                    }

                    std::shared_ptr<const assetmesh::parsed_mesh> mesh;
                    if (asset_id) {
                        assetmesh::request_mesh(asset_id);
                        mesh = assetmesh::get_mesh(asset_id);
                        if (!mesh || mesh->vertices.empty()) {
                            uint64_t key = (static_cast<uint64_t>(entity.user_id) << 32) | (static_cast<uint64_t>(i) ^ 0x80000000ull);
                            g_logged_missing_mesh_data.insert(key);
                            mesh.reset();
                        }
                    }

                    uintptr_t prim_addr = entity.primitives[i];
                    if (!is_valid_address(prim_addr)) continue;

                    struct { float rot[9]; Vec3 pos; } rp{};
                    if (!ReadRaw(prim_addr + Offsets::Primitive::Rotation, &rp, sizeof(rp))) continue;

                    Vec3 sz{};
                    if (!ReadVec3(prim_addr + Offsets::Primitive::Size, sz)) continue;

                    int pt_count = 0;

                    if (mesh) {
                        const auto& b = mesh->bounds;
                        float sx = b.size.x > 0.001f ? sz.x / b.size.x : 1.0f;
                        float sy = b.size.y > 0.001f ? sz.y / b.size.y : 1.0f;
                        float sz_scale = b.size.z > 0.001f ? sz.z / b.size.z : 1.0f;

                        size_t step = (std::max)((size_t)1, mesh->vertices.size() / 64);
                        for (size_t vi = 0; vi < mesh->vertices.size() && pt_count < 500; vi += step) {
                            const auto& v = mesh->vertices[vi].position;
                            float lx = (v.x - b.center.x) * sx;
                            float ly = (v.y - b.center.y) * sy;
                            float lz = (v.z - b.center.z) * sz_scale;
                            Vec3 world{
                                rp.rot[0] * lx + rp.rot[1] * ly + rp.rot[2] * lz + rp.pos.x,
                                rp.rot[3] * lx + rp.rot[4] * ly + rp.rot[5] * lz + rp.pos.y,
                                rp.rot[6] * lx + rp.rot[7] * ly + rp.rot[8] * lz + rp.pos.z
                            };
                            Vec2 scr;
                            if (WorldToScreen(world, scr, view, viewport))
                                mesh_pts[pt_count++] = { scr.x, scr.y };
                        }
                    } else if (entity.user_id != 0) {
                        avatarmesh::request_avatar_mesh(entity.user_id, entity.is_r15);
                        const avatarmesh::avatar_mesh* avatar = avatarmesh::get_avatar_mesh(entity.user_id, entity.is_r15);
                        if (avatar) {
                            const avatarmesh::mesh_part* part = avatarmesh::find_part(avatar, entity.part_names[i], entity.is_r15);
                            if (part && !part->vertices.empty()) {
                                float sx = part->size.x > 0.001f ? sz.x / part->size.x : 1.0f;
                                float sy = part->size.y > 0.001f ? sz.y / part->size.y : 1.0f;
                                float sz_scale = part->size.z > 0.001f ? sz.z / part->size.z : 1.0f;
                                size_t step = (std::max)((size_t)1, part->vertices.size() / 64);
                                for (size_t vi = 0; vi < part->vertices.size() && pt_count < 500; vi += step) {
                                    const auto& v = part->vertices[vi].position;
                                    float lx = (v.x - part->center.x) * sx;
                                    float ly = (v.y - part->center.y) * sy;
                                    float lz = (v.z - part->center.z) * sz_scale;
                                    Vec3 world{
                                        rp.rot[0] * lx + rp.rot[1] * ly + rp.rot[2] * lz + rp.pos.x,
                                        rp.rot[3] * lx + rp.rot[4] * ly + rp.rot[5] * lz + rp.pos.y,
                                        rp.rot[6] * lx + rp.rot[7] * ly + rp.rot[8] * lz + rp.pos.z
                                    };
                                    Vec2 scr;
                                    if (WorldToScreen(world, scr, view, viewport))
                                        mesh_pts[pt_count++] = { scr.x, scr.y };
                                }
                            }
                        }
                    }

                    if (pt_count >= 3) {
                        int hn = ComputeConvexHull(mesh_pts, pt_count);
                        if (hn >= 3) {
                            Clipper2Lib::Path64 path;
                            path.reserve((size_t)hn);
                            for (int h = 0; h < hn; h++)
                                path.push_back({ (int64_t)(mesh_pts[h].x * CLIPPER_SCALE), (int64_t)(mesh_pts[h].y * CLIPPER_SCALE) });
                            if (Clipper2Lib::Area(path) > 1.0)
                                mesh_polys.push_back(std::move(path));
                        }
                    }
                }

                if (!mesh_polys.empty()) {
                    Clipper2Lib::Paths64 merged = Clipper2Lib::Union(mesh_polys, Clipper2Lib::FillRule::Positive);
                    ImU32 outline_col = outline_chams ? IM_COL32((int)(outline_chams_color[0] * 255.0f), (int)(outline_chams_color[1] * 255.0f), (int)(outline_chams_color[2] * 255.0f), (int)(outline_chams_color[3] * 255.0f)) : 0;
                    for (const auto& path : merged) {
                        if (!Clipper2Lib::IsPositive(path)) continue;
                        DrawMergedPoly(path, draw, fill);
                        if (outline_chams && path.size() >= 3 && path.size() <= 256) {
                            ImVec2 ov[256];
                            for (size_t vi = 0; vi < path.size(); vi++)
                                ov[vi] = {(float)(path[vi].x / CLIPPER_SCALE), (float)(path[vi].y / CLIPPER_SCALE)};
                            draw->AddPolyline(ov, (int)path.size(), outline_col, ImDrawFlags_Closed, 2.5f);
                        }
                    }
                }
            }
        } else {
            ID3D11Device* device = overlay::GetDevice();
            ID3D11DeviceContext* context = overlay::GetContext();
            if (!device || !context) return;
            meshgpu::render(entities, view.data, viewport.x, viewport.y, device, context, mesh_chams_color);
        }
    }

    void ShutdownMeshChams() {
        meshgpu::shutdown();
    }

    void RenderExpandedHitbox() {
        if (!render_expanded_hitbox) return;
        if (!hitbox_expander_enabled) return;

        Matrix4 view{};
        Vec2 viewport{};
        if (!GetCamera(view, viewport)) return;

        auto entities_snap = cache::GetEspSnapshot();
        const auto& entities = *entities_snap;
        if (entities.empty()) return;

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw) return;

        ImU32 color = IM_COL32(255, 50, 50, 255);

        const float size = hitbox_expander_value;
        const float hx = size * 0.5f;
        const float hy = size * 0.5f;
        const float hz = size * 0.5f;

        static const int faces[6][4] = {
            {0, 1, 3, 2},
            {4, 6, 7, 5},
            {0, 4, 5, 1},
            {2, 3, 7, 6},
            {0, 2, 6, 4},
            {1, 5, 7, 3},
        };

        for (const cache::EspEntity& entity : entities) {
            if (!entity.character_address) continue;

            instance character{ entity.character_address };
            if (!character.is_valid()) continue;

            uintptr_t hrp_prim = 0;
            for (const instance& child : character.get_children()) {
                if (!child.is_valid()) continue;
                if (child.get_name() != "HumanoidRootPart") continue;
                hrp_prim = read<uintptr_t>(child.address + Offsets::BasePart::Primitive);
                break;
            }

            if (!is_valid_address(hrp_prim)) continue;

            struct { float rot[9]; Vec3 pos; } rp{};
            if (!ReadRaw(hrp_prim + Offsets::Primitive::Rotation, &rp, sizeof(rp))) continue;

            float lx[8] = {-hx, -hx, -hx, -hx, hx, hx, hx, hx};
            float ly[8] = {-hy, -hy, hy, hy, -hy, -hy, hy, hy};
            float lz[8] = {-hz, hz, -hz, hz, -hz, hz, -hz, hz};

            Vec3 corners[8];
            for (int c = 0; c < 8; ++c) {
                corners[c].x = rp.rot[0] * lx[c] + rp.rot[1] * ly[c] + rp.rot[2] * lz[c] + rp.pos.x;
                corners[c].y = rp.rot[3] * lx[c] + rp.rot[4] * ly[c] + rp.rot[5] * lz[c] + rp.pos.y;
                corners[c].z = rp.rot[6] * lx[c] + rp.rot[7] * ly[c] + rp.rot[8] * lz[c] + rp.pos.z;
            }

            Vec2 screen[8];
            bool ok[8];
            for (int c = 0; c < 8; ++c)
                ok[c] = WorldToScreen(corners[c], screen[c], view, viewport);

            for (int f = 0; f < 6; ++f) {
                int a = faces[f][0], b = faces[f][1], ci = faces[f][2], d = faces[f][3];
                if (!ok[a] || !ok[b] || !ok[ci] || !ok[d]) continue;
                draw->AddLine(ImVec2(screen[a].x, screen[a].y), ImVec2(screen[b].x, screen[b].y), color, 1.5f);
                draw->AddLine(ImVec2(screen[b].x, screen[b].y), ImVec2(screen[ci].x, screen[ci].y), color, 1.5f);
                draw->AddLine(ImVec2(screen[ci].x, screen[ci].y), ImVec2(screen[d].x, screen[d].y), color, 1.5f);
                draw->AddLine(ImVec2(screen[d].x, screen[d].y), ImVec2(screen[a].x, screen[a].y), color, 1.5f);
            }
        }
    }

    void RenderESP() {
        Matrix4 view{};
        Vec2 viewport{};
        if (!GetCamera(view, viewport)) return;

        auto entities_snap = cache::GetEspSnapshot();
        const auto& entities = *entities_snap;
        if (entities.empty()) return;

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw) return;

        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();

        for (const cache::EspEntity& entity : entities) {
            if (esp_render_distance > 0.0f && lp.valid) {
                float dx = entity.root_x - lp.x;
                float dy = entity.root_y - lp.y;
                float dz = entity.root_z - lp.z;
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                if (dist > esp_render_distance) continue;
            }

            Box2D box{};
            if (!ComputeBoxForPrimitives(entity, view, viewport, box)) continue;
            if (!box.valid) continue;

            float x1 = floorf(box.min_x);
            float y1 = floorf(box.min_y);
            float x2 = floorf(box.max_x);
            float y2 = floorf(box.max_y);

            if (box_esp) {
                ImU32 black = IM_COL32(0, 0, 0, 255);
                ImU32 white = IM_COL32(
                    (int)(box_esp_color[0] * 255.0f),
                    (int)(box_esp_color[1] * 255.0f),
                    (int)(box_esp_color[2] * 255.0f),
                    (int)(box_esp_color[3] * 255.0f)
                );

                float X1 = x1 - 1.0f, Y1 = y1 - 1.0f, X2 = x2 + 1.0f, Y2 = y2 + 1.0f;
                if (box_esp_type == 1) {
                    float box_w = X2 - X1;
                    float box_h = Y2 - Y1;
                    float len = (std::min)((std::min)(box_w, box_h) * 0.25f, 50.0f);
                    len = (std::min)(len, (std::min)(box_w, box_h) * 0.5f - 1.0f);
                    float X1L = X1 + len, Y1L = Y1 + len, X2L = X2 - len, Y2L = Y2 - len;

                    if (box_fill) {
                        ImU32 fill_c = IM_COL32((int)(box_fill_top[0] * 255), (int)(box_fill_top[1] * 255), (int)(box_fill_top[2] * 255), (int)(box_fill_top[3] * 255));
                        if (box_fill_gradient && box_fill_gradient_rotate) {
                            float t = (float)ImGui::GetTime() * 2.0f;
                            float s = sinf(t), c = cosf(t);
                            ImU32 c1 = IM_COL32((int)(box_fill_top[0] * 255), (int)(box_fill_top[1] * 255), (int)(box_fill_top[2] * 255), (int)(box_fill_top[3] * 255));
                            ImU32 c2 = IM_COL32((int)(box_fill_bottom[0] * 255), (int)(box_fill_bottom[1] * 255), (int)(box_fill_bottom[2] * 255), (int)(box_fill_bottom[3] * 255));
                            ImVec4 v1 = ImGui::ColorConvertU32ToFloat4(c1);
                            ImVec4 v2 = ImGui::ColorConvertU32ToFloat4(c2);
                            ImU32 c_tl = ImGui::ColorConvertFloat4ToU32(ImVec4(v1.x + (v2.x - v1.x) * ((s + 1.0f) * 0.5f), v1.y + (v2.y - v1.y) * ((s + 1.0f) * 0.5f), v1.z + (v2.z - v1.z) * ((s + 1.0f) * 0.5f), v1.w + (v2.w - v1.w) * ((s + 1.0f) * 0.5f)));
                            ImU32 c_tr = ImGui::ColorConvertFloat4ToU32(ImVec4(v1.x + (v2.x - v1.x) * ((c + 1.0f) * 0.5f), v1.y + (v2.y - v1.y) * ((c + 1.0f) * 0.5f), v1.z + (v2.z - v1.z) * ((c + 1.0f) * 0.5f), v1.w + (v2.w - v1.w) * ((c + 1.0f) * 0.5f)));
                            ImU32 c_br = ImGui::ColorConvertFloat4ToU32(ImVec4(v1.x + (v2.x - v1.x) * ((-s + 1.0f) * 0.5f), v1.y + (v2.y - v1.y) * ((-s + 1.0f) * 0.5f), v1.z + (v2.z - v1.z) * ((-s + 1.0f) * 0.5f), v1.w + (v2.w - v1.w) * ((-s + 1.0f) * 0.5f)));
                            ImU32 c_bl = ImGui::ColorConvertFloat4ToU32(ImVec4(v1.x + (v2.x - v1.x) * ((-c + 1.0f) * 0.5f), v1.y + (v2.y - v1.y) * ((-c + 1.0f) * 0.5f), v1.z + (v2.z - v1.z) * ((-c + 1.0f) * 0.5f), v1.w + (v2.w - v1.w) * ((-c + 1.0f) * 0.5f)));
                            draw->AddRectFilledMultiColor(ImVec2(X1 + 2, Y1 + 2), ImVec2(X2 - 2, Y2 - 2), c_tl, c_tr, c_br, c_bl);
                        } else if (box_fill_gradient) {
                            ImU32 c1 = IM_COL32((int)(box_fill_top[0] * 255), (int)(box_fill_top[1] * 255), (int)(box_fill_top[2] * 255), (int)(box_fill_top[3] * 255));
                            ImU32 c2 = IM_COL32((int)(box_fill_bottom[0] * 255), (int)(box_fill_bottom[1] * 255), (int)(box_fill_bottom[2] * 255), (int)(box_fill_bottom[3] * 255));
                            draw->AddRectFilledMultiColor(ImVec2(X1 + 2, Y1 + 2), ImVec2(X2 - 2, Y2 - 2), c1, c1, c2, c2);
                        } else {
                            draw->AddRectFilled(ImVec2(X1 + 2, Y1 + 2), ImVec2(X2 - 2, Y2 - 2), fill_c);
                        }
                    }

                    draw->AddRectFilled(ImVec2(X1 - 1, Y1 - 1), ImVec2(X1L + 1, Y1 + 1), black);
                    draw->AddRectFilled(ImVec2(X1 - 1, Y1 - 1), ImVec2(X1 + 1, Y1L + 1), black);
                    draw->AddRectFilled(ImVec2(X2L - 1, Y1 - 1), ImVec2(X2 + 1, Y1 + 1), black);
                    draw->AddRectFilled(ImVec2(X2 - 1, Y1 - 1), ImVec2(X2 + 1, Y1L + 1), black);
                    draw->AddRectFilled(ImVec2(X1 - 1, Y2 - 1), ImVec2(X1L + 1, Y2 + 1), black);
                    draw->AddRectFilled(ImVec2(X1 - 1, Y2L - 1), ImVec2(X1 + 1, Y2 + 1), black);
                    draw->AddRectFilled(ImVec2(X2L - 1, Y2 - 1), ImVec2(X2 + 1, Y2 + 1), black);
                    draw->AddRectFilled(ImVec2(X2 - 1, Y2L - 1), ImVec2(X2 + 1, Y2 + 1), black);

                    draw->AddRectFilled(ImVec2(X1 + 1, Y1 + 1), ImVec2(X1L + 1, Y1 + 2), white);
                    draw->AddRectFilled(ImVec2(X1 + 1, Y1 + 1), ImVec2(X1 + 2, Y1L + 1), white);
                    draw->AddRectFilled(ImVec2(X2L - 1, Y1 + 1), ImVec2(X2 - 1, Y1 + 2), white);
                    draw->AddRectFilled(ImVec2(X2 - 2, Y1 + 1), ImVec2(X2 - 1, Y1L + 1), white);
                    draw->AddRectFilled(ImVec2(X1 + 1, Y2 - 2), ImVec2(X1L + 1, Y2 - 1), white);
                    draw->AddRectFilled(ImVec2(X1 + 1, Y2L - 1), ImVec2(X1 + 2, Y2 - 1), white);
                    draw->AddRectFilled(ImVec2(X2L - 1, Y2 - 2), ImVec2(X2 - 1, Y2 - 1), white);
                    draw->AddRectFilled(ImVec2(X2 - 2, Y2L - 1), ImVec2(X2 - 1, Y2 - 1), white);
                } else {
                    if (box_fill) {
                        ImU32 fill_c = IM_COL32((int)(box_fill_top[0] * 255), (int)(box_fill_top[1] * 255), (int)(box_fill_top[2] * 255), (int)(box_fill_top[3] * 255));
                        if (box_fill_gradient && box_fill_gradient_rotate) {
                            float t = (float)ImGui::GetTime() * 2.0f;
                            float s = sinf(t), c = cosf(t);
                            ImU32 c1 = IM_COL32((int)(box_fill_top[0] * 255), (int)(box_fill_top[1] * 255), (int)(box_fill_top[2] * 255), (int)(box_fill_top[3] * 255));
                            ImU32 c2 = IM_COL32((int)(box_fill_bottom[0] * 255), (int)(box_fill_bottom[1] * 255), (int)(box_fill_bottom[2] * 255), (int)(box_fill_bottom[3] * 255));
                            ImVec4 v1 = ImGui::ColorConvertU32ToFloat4(c1);
                            ImVec4 v2 = ImGui::ColorConvertU32ToFloat4(c2);
                            ImU32 c_tl = ImGui::ColorConvertFloat4ToU32(ImVec4(v1.x + (v2.x - v1.x) * ((s + 1.0f) * 0.5f), v1.y + (v2.y - v1.y) * ((s + 1.0f) * 0.5f), v1.z + (v2.z - v1.z) * ((s + 1.0f) * 0.5f), v1.w + (v2.w - v1.w) * ((s + 1.0f) * 0.5f)));
                            ImU32 c_tr = ImGui::ColorConvertFloat4ToU32(ImVec4(v1.x + (v2.x - v1.x) * ((c + 1.0f) * 0.5f), v1.y + (v2.y - v1.y) * ((c + 1.0f) * 0.5f), v1.z + (v2.z - v1.z) * ((c + 1.0f) * 0.5f), v1.w + (v2.w - v1.w) * ((c + 1.0f) * 0.5f)));
                            ImU32 c_br = ImGui::ColorConvertFloat4ToU32(ImVec4(v1.x + (v2.x - v1.x) * ((-s + 1.0f) * 0.5f), v1.y + (v2.y - v1.y) * ((-s + 1.0f) * 0.5f), v1.z + (v2.z - v1.z) * ((-s + 1.0f) * 0.5f), v1.w + (v2.w - v1.w) * ((-s + 1.0f) * 0.5f)));
                            ImU32 c_bl = ImGui::ColorConvertFloat4ToU32(ImVec4(v1.x + (v2.x - v1.x) * ((-c + 1.0f) * 0.5f), v1.y + (v2.y - v1.y) * ((-c + 1.0f) * 0.5f), v1.z + (v2.z - v1.z) * ((-c + 1.0f) * 0.5f), v1.w + (v2.w - v1.w) * ((-c + 1.0f) * 0.5f)));
                            draw->AddRectFilledMultiColor(ImVec2(x1 + 1, y1 + 1), ImVec2(x2 - 1, y2 - 1), c_tl, c_tr, c_br, c_bl);
                        } else if (box_fill_gradient) {
                            ImU32 c1 = IM_COL32((int)(box_fill_top[0] * 255), (int)(box_fill_top[1] * 255), (int)(box_fill_top[2] * 255), (int)(box_fill_top[3] * 255));
                            ImU32 c2 = IM_COL32((int)(box_fill_bottom[0] * 255), (int)(box_fill_bottom[1] * 255), (int)(box_fill_bottom[2] * 255), (int)(box_fill_bottom[3] * 255));
                            draw->AddRectFilledMultiColor(ImVec2(x1 + 1, y1 + 1), ImVec2(x2 - 1, y2 - 1), c1, c1, c2, c2);
                        } else {
                            draw->AddRectFilled(ImVec2(x1 + 1, y1 + 1), ImVec2(x2 - 1, y2 - 1), fill_c);
                        }
                    }
                    draw->AddRectFilled(ImVec2(x1 - 1, y1 - 1), ImVec2(x2 + 1, y1), black);
                    draw->AddRectFilled(ImVec2(x1 - 1, y2), ImVec2(x2 + 1, y2 + 1), black);
                    draw->AddRectFilled(ImVec2(x1 - 1, y1), ImVec2(x1, y2), black);
                    draw->AddRectFilled(ImVec2(x2, y1), ImVec2(x2 + 1, y2), black);
                    draw->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y1 + 1), white);
                    draw->AddRectFilled(ImVec2(x1, y2 - 1), ImVec2(x2, y2), white);
                    draw->AddRectFilled(ImVec2(x1, y1 + 1), ImVec2(x1 + 1, y2 - 1), white);
                    draw->AddRectFilled(ImVec2(x2 - 1, y1 + 1), ImVec2(x2, y2 - 1), white);
                    draw->AddRectFilled(ImVec2(x1 + 1, y1 + 1), ImVec2(x2 - 1, y1 + 2), black);
                    draw->AddRectFilled(ImVec2(x1 + 1, y2 - 2), ImVec2(x2 - 1, y2 - 1), black);
                    draw->AddRectFilled(ImVec2(x1 + 1, y1 + 2), ImVec2(x1 + 2, y2 - 2), black);
                    draw->AddRectFilled(ImVec2(x2 - 2, y1 + 2), ImVec2(x2 - 1, y2 - 2), black);
                }
            }

            if (healthbar)
                RenderHealthBar(draw, x1, y1, x2, y2, entity.health, entity.max_health);
            if (health_text)
                RenderHealthText(draw, x1, y1, x2, y2, entity.health);
            if (name)
                RenderName(draw, x1, y1, x2, y2, entity.name);

            if (distance && lp.valid) {
                float dx = entity.root_x - lp.x;
                float dy = entity.root_y - lp.y;
                float dz = entity.root_z - lp.z;
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                RenderDistance(draw, x1, y1, x2, y2, dist);
            }
            if (rig_type)
                RenderRigType(draw, x1, y1, x2, y2, entity.is_r15);
            if (tool_esp)
                RenderTool(draw, x1, y1, x2, y2, entity.tool_name, distance && lp.valid);
        }
    }
}
