#include "esp_preview.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cache.h"
#include "game.h"
#include "globals.h"
#include "memory.h"
#include "offsets.h"
#include "features/avatarmesh/avatar_mesh.h"
#include "imgui/imgui.h"
#include "clipper2/clipper.h"

namespace features {

namespace {

static constexpr double k_clipper_scale = 1000.0;

struct vec2_t {
    float x;
    float y;
};

struct vec3_t {
    float x;
    float y;
    float z;
};

struct primitive_pose {
    uintptr_t primitive = 0;
    const char* name = nullptr;
    vec3_t pos{};
    vec3_t size{};
    float rot[9]{};
    bool valid = false;
};

struct projected_point {
    ImVec2 point{};
    float depth = 0.0f;
    bool valid = false;
};

struct canvas_state {
    ImVec2 min{};
    ImVec2 max{};
    ImVec2 center{};
    float width = 0.0f;
    float height = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float focal = 0.0f;
    float cam_dist = 0.0f;
};

struct preview_target {
    const cache::EspEntity* entity = nullptr;
    const avatarmesh::avatar_mesh* mesh = nullptr;
};

static vec3_t add(const vec3_t& a, const vec3_t& b);
static vec3_t sub(const vec3_t& a, const vec3_t& b);
static vec3_t rotate_local(const float rot[9], const vec3_t& local);
static bool project_point(const vec3_t& point, const canvas_state& canvas, projected_point& out);

static bool match_preview_part_name(const std::string& name) {
    return name == "Head" ||
        name == "UpperTorso" ||
        name == "LowerTorso" ||
        name == "Torso" ||
        name == "LeftHand" ||
        name == "RightHand" ||
        name == "LeftFoot" ||
        name == "RightFoot" ||
        name == "Left Arm" ||
        name == "Right Arm" ||
        name == "Left Leg" ||
        name == "Right Leg" ||
        name == "LeftUpperArm" ||
        name == "RightUpperArm" ||
        name == "LeftLowerArm" ||
        name == "RightLowerArm" ||
        name == "LeftUpperLeg" ||
        name == "RightUpperLeg" ||
        name == "LeftLowerLeg" ||
        name == "RightLowerLeg";
}

static bool collect_local_preview_entity(instance local_player, cache::EspEntity& entity_out) {
    if (!local_player.is_valid()) {
        return false;
    }

    instance local_char = local_player.model_instance();
    if (!local_char.is_valid()) {
        return false;
    }

    cache::EspEntity entity{};
    entity.player_address = local_player.address;
    entity.character_address = local_char.address;
    entity.user_id = read<int64_t>(local_player.address + Offsets::Player::UserId);
    entity.health = 0.0f;
    entity.max_health = 100.0f;

    bool has_humanoid = false;
    bool has_upper_torso = false;
    bool has_torso = false;
    uintptr_t hrp_primitive = 0;

    for (const instance& child : local_char.get_children()) {
        if (!child.is_valid()) {
            continue;
        }

        const std::string child_name = child.get_name();
        if (child_name == "Humanoid") {
            has_humanoid = true;
            entity.health = read<float>(child.address + Offsets::Humanoid::Health);
            entity.max_health = read<float>(child.address + Offsets::Humanoid::MaxHealth);
            if (entity.max_health <= 0.0f) {
                entity.max_health = 100.0f;
            }
            continue;
        }

        if (child_name == "HumanoidRootPart") {
            hrp_primitive = read<uintptr_t>(child.address + Offsets::BasePart::Primitive);
            if (is_valid_address(hrp_primitive)) {
                entity.root_x = read<float>(hrp_primitive + Offsets::Primitive::Position);
                entity.root_y = read<float>(hrp_primitive + Offsets::Primitive::Position + 4);
                entity.root_z = read<float>(hrp_primitive + Offsets::Primitive::Position + 8);
            }
        }

        if (!match_preview_part_name(child_name) || entity.primitive_count >= cache::k_esp_primitive_capacity) {
            continue;
        }

        uintptr_t primitive = read<uintptr_t>(child.address + Offsets::BasePart::Primitive);
        if (!is_valid_address(primitive)) {
            continue;
        }

        entity.primitives[entity.primitive_count] = primitive;
        entity.part_addresses[entity.primitive_count] = child.address;
        const size_t copy_len = (std::min)(child_name.size(), size_t(31));
        std::memcpy(entity.part_names[entity.primitive_count], child_name.c_str(), copy_len);
        entity.part_names[entity.primitive_count][copy_len] = '\0';
        entity.primitive_count += 1;

        if (child_name == "UpperTorso") {
            has_upper_torso = true;
        }
        if (child_name == "Torso") {
            has_torso = true;
        }
    }

    if (!has_humanoid || entity.primitive_count == 0 || !is_valid_address(hrp_primitive)) {
        return false;
    }

    entity.is_r15 = has_upper_torso || !has_torso;

    std::string player_name = local_player.get_name();
    const size_t player_copy = (std::min)(player_name.size(), size_t(63));
    std::memcpy(entity.name, player_name.c_str(), player_copy);
    entity.name[player_copy] = '\0';

    uintptr_t display_name_ptr = read<uintptr_t>(local_player.address + Offsets::Player::DisplayName);
    if (is_valid_address(display_name_ptr)) {
        std::string display_name = fetchstring(display_name_ptr);
        const size_t display_copy = (std::min)(display_name.size(), size_t(63));
        std::memcpy(entity.display_name, display_name.c_str(), display_copy);
        entity.display_name[display_copy] = '\0';
    }

    for (const instance& child : local_char.get_children()) {
        if (!child.is_valid()) {
            continue;
        }
        if (child.get_class_name() == "Tool") {
            std::string tool_name = child.get_name();
            const size_t tool_copy = (std::min)(tool_name.size(), size_t(63));
            std::memcpy(entity.tool_name, tool_name.c_str(), tool_copy);
            entity.tool_name[tool_copy] = '\0';
            break;
        }
    }

    entity_out = entity;
    return true;
}

static float clampf(float value, float min_v, float max_v) {
    return (std::max)(min_v, (std::min)(value, max_v));
}

static ImU32 color_from_array(const float color[4], float alpha_scale = 1.0f) {
    return IM_COL32(
        (int)(clampf(color[0], 0.0f, 1.0f) * 255.0f),
        (int)(clampf(color[1], 0.0f, 1.0f) * 255.0f),
        (int)(clampf(color[2], 0.0f, 1.0f) * 255.0f),
        (int)(clampf(color[3] * alpha_scale, 0.0f, 1.0f) * 255.0f));
}

static ImU32 scale_alpha(ImU32 color, float alpha_scale) {
    ImVec4 rgba = ImGui::ColorConvertU32ToFloat4(color);
    rgba.w = clampf(rgba.w * alpha_scale, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(rgba);
}

static Clipper2Lib::Path64 make_triangle_path(const ImVec2& a, const ImVec2& b, const ImVec2& c) {
    Clipper2Lib::Path64 path;
    path.push_back({ (int64_t)(a.x * k_clipper_scale), (int64_t)(a.y * k_clipper_scale) });
    path.push_back({ (int64_t)(b.x * k_clipper_scale), (int64_t)(b.y * k_clipper_scale) });
    path.push_back({ (int64_t)(c.x * k_clipper_scale), (int64_t)(c.y * k_clipper_scale) });
    return path;
}

static float cross_2d(const ImVec2& a, const ImVec2& b, const ImVec2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static Clipper2Lib::Path64 make_convex_hull_path(std::vector<ImVec2> points) {
    if (points.size() < 3) {
        return {};
    }

    std::sort(points.begin(), points.end(), [](const ImVec2& left, const ImVec2& right) {
        if (left.x == right.x) {
            return left.y < right.y;
        }
        return left.x < right.x;
    });

    std::vector<ImVec2> hull;
    hull.reserve(points.size() * 2);

    for (const ImVec2& point : points) {
        while (hull.size() >= 2 && cross_2d(hull[hull.size() - 2], hull[hull.size() - 1], point) <= 0.0f) {
            hull.pop_back();
        }
        hull.push_back(point);
    }

    const size_t lower_size = hull.size();
    for (size_t i = points.size(); i-- > 0;) {
        const ImVec2& point = points[i];
        while (hull.size() > lower_size && cross_2d(hull[hull.size() - 2], hull[hull.size() - 1], point) <= 0.0f) {
            hull.pop_back();
        }
        hull.push_back(point);
    }

    if (!hull.empty()) {
        hull.pop_back();
    }

    if (hull.size() < 3) {
        return {};
    }

    Clipper2Lib::Path64 path;
    path.reserve(hull.size());
    for (const ImVec2& point : hull) {
        path.push_back({ (int64_t)(point.x * k_clipper_scale), (int64_t)(point.y * k_clipper_scale) });
    }
    return path;
}

static void draw_merged_path(ImDrawList* draw, const Clipper2Lib::Path64& path, ImU32 fill, ImU32 outline, bool draw_outline) {
    if (path.size() < 3 || path.size() > 512) {
        return;
    }

    ImVec2 points[512];
    for (size_t i = 0; i < path.size(); ++i) {
        points[i] = ImVec2((float)(path[i].x / k_clipper_scale), (float)(path[i].y / k_clipper_scale));
    }

    draw->AddConcavePolyFilled(points, (int)path.size(), fill);
    if (draw_outline) {
        draw->AddPolyline(points, (int)path.size(), IM_COL32(0, 0, 0, 180), ImDrawFlags_Closed, 2.8f);
        draw->AddPolyline(points, (int)path.size(), outline, ImDrawFlags_Closed, 1.35f);
    }
}

static vec3_t make_vec3(float x, float y, float z) {
    return { x, y, z };
}

static vec3_t add(const vec3_t& a, const vec3_t& b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

static vec3_t sub(const vec3_t& a, const vec3_t& b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

static vec3_t mul_scalar(const vec3_t& a, float scalar) {
    return { a.x * scalar, a.y * scalar, a.z * scalar };
}

static vec3_t mul_components(const vec3_t& a, const vec3_t& b) {
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

static float dot(const vec3_t& a, const vec3_t& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static vec3_t cross(const vec3_t& a, const vec3_t& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static vec3_t normalize(const vec3_t& value) {
    const float len_sq = dot(value, value);
    if (len_sq <= 0.000001f) {
        return {};
    }
    const float inv_len = 1.0f / sqrtf(len_sq);
    return mul_scalar(value, inv_len);
}

static vec3_t rotate_local(const float rot[9], const vec3_t& local) {
    return {
        rot[0] * local.x + rot[1] * local.y + rot[2] * local.z,
        rot[3] * local.x + rot[4] * local.y + rot[5] * local.z,
        rot[6] * local.x + rot[7] * local.y + rot[8] * local.z
    };
}

static bool read_vec3(uintptr_t address, vec3_t& out) {
    return read_raw(address, &out, sizeof(out));
}

static bool read_rotation(uintptr_t address, float out[9]) {
    return read_raw(address, out, sizeof(float) * 9);
}

static int find_pose_index(const std::vector<primitive_pose>& poses, const char* part_name) {
    for (size_t i = 0; i < poses.size(); ++i) {
        if (poses[i].name && std::strcmp(poses[i].name, part_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static preview_target select_preview_target() {
    preview_target target{};
    static cache::EspEntity local_preview_entity{};
    const auto& entities = cache::GetEspEntities();
    instance local_player{};
    instance dm = game::ReadDatamodel(g_base_address);
    if (dm.is_valid()) {
        instance players_service = dm.read_service("Players");
        if (players_service.is_valid()) {
            local_player = players_service.local_player();
        }
    }

    if (collect_local_preview_entity(local_player, local_preview_entity)) {
        avatarmesh::request_avatar_mesh(local_preview_entity.user_id, local_preview_entity.is_r15);
        target.entity = &local_preview_entity;
        target.mesh = avatarmesh::get_avatar_mesh(local_preview_entity.user_id, local_preview_entity.is_r15);
        return target;
    }

    for (const cache::EspEntity& entity : entities) {
        if (!local_player.is_valid() || entity.player_address != local_player.address) {
            continue;
        }
        if (entity.user_id <= 0 || entity.primitive_count == 0) {
            continue;
        }

        avatarmesh::request_avatar_mesh(entity.user_id, entity.is_r15);
        const avatarmesh::avatar_mesh* mesh = avatarmesh::get_avatar_mesh(entity.user_id, entity.is_r15);
        target.entity = &entity;
        target.mesh = mesh;
        return target;
    }

    return target;
}

static std::vector<primitive_pose> collect_poses(const cache::EspEntity& entity) {
    std::vector<primitive_pose> poses;
    poses.reserve(entity.primitive_count);

    for (size_t i = 0; i < entity.primitive_count; ++i) {
        uintptr_t primitive = entity.primitives[i];
        if (!is_valid_address(primitive)) {
            continue;
        }

        primitive_pose pose{};
        pose.primitive = primitive;
        pose.name = entity.part_names[i];
        pose.valid =
            read_vec3(primitive + Offsets::Primitive::Position, pose.pos) &&
            read_vec3(primitive + Offsets::Primitive::Size, pose.size) &&
            read_rotation(primitive + Offsets::Primitive::Rotation, pose.rot);

        if (pose.valid) {
            poses.push_back(pose);
        }
    }

    return poses;
}

static vec3_t compute_model_center(const std::vector<primitive_pose>& poses, const vec3_t& origin) {
    if (poses.empty()) {
        return {};
    }

    vec3_t min_v{ FLT_MAX, FLT_MAX, FLT_MAX };
    vec3_t max_v{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (const primitive_pose& pose : poses) {
        vec3_t rel = sub(pose.pos, origin);
        vec3_t half = mul_scalar(pose.size, 0.5f);
        min_v.x = (std::min)(min_v.x, rel.x - half.x);
        min_v.y = (std::min)(min_v.y, rel.y - half.y);
        min_v.z = (std::min)(min_v.z, rel.z - half.z);
        max_v.x = (std::max)(max_v.x, rel.x + half.x);
        max_v.y = (std::max)(max_v.y, rel.y + half.y);
        max_v.z = (std::max)(max_v.z, rel.z + half.z);
    }

    const float height = (std::max)(0.1f, max_v.y - min_v.y);
    return {
        (min_v.x + max_v.x) * 0.5f,
        min_v.y + height * 0.48f,
        (min_v.z + max_v.z) * 0.5f
    };
}

static float compute_model_radius(const std::vector<primitive_pose>& poses, const vec3_t& origin, const vec3_t& center) {
    float radius = 2.0f;
    for (const primitive_pose& pose : poses) {
        vec3_t rel = sub(sub(pose.pos, origin), center);
        radius = (std::max)(radius, fabsf(rel.x) + pose.size.x);
        radius = (std::max)(radius, fabsf(rel.y) + pose.size.y);
        radius = (std::max)(radius, fabsf(rel.z) + pose.size.z);
    }
    return radius;
}

static bool project_point(const vec3_t& point, const canvas_state& canvas, projected_point& out) {
    const float cy = cosf(canvas.yaw);
    const float sy = sinf(canvas.yaw);
    const float cp = cosf(canvas.pitch);
    const float sp = sinf(canvas.pitch);

    const float x1 = point.x * cy - point.z * sy;
    const float z1 = point.x * sy + point.z * cy;
    const float y2 = point.y * cp - z1 * sp;
    const float z2 = point.y * sp + z1 * cp + canvas.cam_dist;

    if (z2 <= 0.1f) {
        return false;
    }

    const float inv_z = canvas.focal / z2;
    out.point = ImVec2(canvas.center.x + x1 * inv_z, canvas.center.y - y2 * inv_z);
    out.depth = z2;
    out.valid = true;
    return true;
}

static vec3_t to_camera_space(const vec3_t& point, const canvas_state& canvas) {
    const float cy = cosf(canvas.yaw);
    const float sy = sinf(canvas.yaw);
    const float cp = cosf(canvas.pitch);
    const float sp = sinf(canvas.pitch);

    const float x1 = point.x * cy - point.z * sy;
    const float z1 = point.x * sy + point.z * cy;
    const float y2 = point.y * cp - z1 * sp;
    const float z2 = point.y * sp + z1 * cp;
    return { x1, y2, z2 };
}

static void draw_canvas_background(ImDrawList* draw, const ImVec2& min, const ImVec2& max) {
    draw->AddRectFilledMultiColor(
        min, max,
        IM_COL32(20, 18, 24, 255),
        IM_COL32(24, 21, 28, 255),
        IM_COL32(14, 13, 18, 255),
        IM_COL32(18, 16, 22, 255));

    const float width = max.x - min.x;
    const float height = max.y - min.y;
    const float step = 26.0f;
    const ImU32 grid = IM_COL32(45, 40, 55, 80);
    for (float x = min.x + fmodf(ImGui::GetTime() * 18.0f, step) - step; x < max.x + step; x += step) {
        draw->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), grid, 1.0f);
    }
    for (float y = min.y + fmodf(ImGui::GetTime() * 9.0f, step) - step; y < max.y + step; y += step) {
        draw->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), grid, 1.0f);
    }

    draw->AddRect(min, max, IM_COL32(58, 52, 70, 160), 0.0f, 0, 1.0f);
}

static void draw_label(ImDrawList* draw, const ImVec2& pos, ImU32 color, const char* text, bool centered = false) {
    if (!text || !text[0]) {
        return;
    }
    ImVec2 size = ImGui::CalcTextSize(text);
    ImVec2 p = pos;
    if (centered) {
        p.x -= size.x * 0.5f;
    }
    draw->AddText(ImVec2(p.x + 1.0f, p.y + 1.0f), IM_COL32(0, 0, 0, 180), text);
    draw->AddText(p, color, text);
}

static void draw_health_bar(ImDrawList* draw, float x1, float y1, float y2, float health, float max_health) {
    if (max_health <= 0.0f) {
        return;
    }

    const float ratio = clampf(health / max_health, 0.0f, 1.0f);
    const float bar_x1 = x1 - 8.0f;
    const float bar_x2 = x1 - 4.0f;
    const float bar_y1 = y1;
    const float bar_y2 = y2;
    const float fill_y = bar_y2 - (bar_y2 - bar_y1) * ratio;

    draw->AddRectFilled(ImVec2(bar_x1 - 1.0f, bar_y1 - 1.0f), ImVec2(bar_x2 + 1.0f, bar_y2 + 1.0f), IM_COL32(0, 0, 0, 220));
    draw->AddRectFilled(ImVec2(bar_x1, bar_y1), ImVec2(bar_x2, bar_y2), IM_COL32(30, 28, 35, 210));
    draw->AddRectFilled(ImVec2(bar_x1, fill_y), ImVec2(bar_x2, bar_y2), color_from_array(healthbar_color));
}

static void draw_box(ImDrawList* draw, float x1, float y1, float x2, float y2) {
    const ImU32 outline = IM_COL32(0, 0, 0, 220);
    const ImU32 main = color_from_array(box_esp_color);

    if (box_fill) {
        if (box_fill_gradient) {
            ImU32 top = color_from_array(box_fill_top);
            ImU32 bottom = color_from_array(box_fill_bottom);
            if (box_fill_gradient_rotate) {
                const float t = (float)ImGui::GetTime() * 1.8f;
                const float s = (sinf(t) + 1.0f) * 0.5f;
                const float c = (cosf(t) + 1.0f) * 0.5f;
                ImVec4 a = ImGui::ColorConvertU32ToFloat4(top);
                ImVec4 b = ImGui::ColorConvertU32ToFloat4(bottom);
                auto mix = [&](float weight) {
                    return ImGui::ColorConvertFloat4ToU32(ImVec4(
                        a.x + (b.x - a.x) * weight,
                        a.y + (b.y - a.y) * weight,
                        a.z + (b.z - a.z) * weight,
                        a.w + (b.w - a.w) * weight));
                };
                draw->AddRectFilledMultiColor(ImVec2(x1 + 1.0f, y1 + 1.0f), ImVec2(x2 - 1.0f, y2 - 1.0f), mix(s), mix(c), mix(1.0f - s), mix(1.0f - c));
            } else {
                draw->AddRectFilledMultiColor(ImVec2(x1 + 1.0f, y1 + 1.0f), ImVec2(x2 - 1.0f, y2 - 1.0f), top, top, bottom, bottom);
            }
        } else {
            draw->AddRectFilled(ImVec2(x1 + 1.0f, y1 + 1.0f), ImVec2(x2 - 1.0f, y2 - 1.0f), color_from_array(box_fill_top));
        }
    }

    if (box_esp_type == 1) {
        const float box_w = x2 - x1;
        const float box_h = y2 - y1;
        const float len = (std::min)((std::min)(box_w, box_h) * 0.24f, 28.0f);
        const std::array<ImVec2, 8> starts = {
            ImVec2(x1, y1), ImVec2(x1, y1), ImVec2(x2 - len, y1), ImVec2(x2, y1),
            ImVec2(x1, y2), ImVec2(x1, y2 - len), ImVec2(x2 - len, y2), ImVec2(x2, y2 - len)
        };
        const std::array<ImVec2, 8> ends = {
            ImVec2(x1 + len, y1), ImVec2(x1, y1 + len), ImVec2(x2, y1), ImVec2(x2, y1 + len),
            ImVec2(x1 + len, y2), ImVec2(x1, y2), ImVec2(x2, y2), ImVec2(x2, y2)
        };
        for (size_t i = 0; i < starts.size(); ++i) {
            draw->AddLine(starts[i], ends[i], outline, 3.0f);
            draw->AddLine(starts[i], ends[i], main, 1.5f);
        }
    } else {
        draw->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), outline, 0.0f, 0, 3.0f);
        draw->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), main, 0.0f, 0, 1.2f);
    }
}

static void draw_china_hat(ImDrawList* draw, const primitive_pose& head, const vec3_t& origin, const vec3_t& model_center, const canvas_state& canvas) {
    static constexpr float k_pi = 3.14159265358979323846f;
    const vec3_t rel_head = sub(sub(head.pos, origin), model_center);
    const vec3_t up = make_vec3(head.rot[1], head.rot[4], head.rot[7]);
    const vec3_t right = make_vec3(head.rot[0], head.rot[3], head.rot[6]);
    const vec3_t forward = make_vec3(head.rot[2], head.rot[5], head.rot[8]);
    const float radius = (std::max)(head.size.x, head.size.z) * 0.85f;

    projected_point tip{};
    if (!project_point(add(rel_head, mul_scalar(up, head.size.y * 0.95f)), canvas, tip)) {
        return;
    }

    ImVector<ImVec2> ring;
    ring.reserve(20);
    for (int i = 0; i < 18; ++i) {
        const float angle = (float)i / 18.0f * k_pi * 2.0f;
        vec3_t offset = add(mul_scalar(right, cosf(angle) * radius), mul_scalar(forward, sinf(angle) * radius));
        projected_point pt{};
        if (project_point(add(add(rel_head, offset), mul_scalar(up, head.size.y * 0.5f)), canvas, pt)) {
            ring.push_back(pt.point);
        }
    }

    if (ring.Size < 3) {
        return;
    }

    const ImU32 fill = color_from_array(chinahat_color);
    const ImU32 line = scale_alpha(fill, 0.85f);
    draw->AddConvexPolyFilled(ring.Data, ring.Size, scale_alpha(fill, 0.35f));
    draw->AddPolyline(ring.Data, ring.Size, line, ImDrawFlags_Closed, 1.5f);
    for (int i = 0; i < ring.Size; i += 3) {
        draw->AddLine(tip.point, ring[i], line, 1.0f);
    }
}

static void draw_skeleton(ImDrawList* draw, const std::vector<primitive_pose>& poses, const vec3_t& origin, const vec3_t& model_center, const canvas_state& canvas, bool is_r15) {
    std::vector<projected_point> part_points(poses.size());
    for (size_t i = 0; i < poses.size(); ++i) {
        project_point(sub(sub(poses[i].pos, origin), model_center), canvas, part_points[i]);
    }

    auto draw_bone = [&](const char* a_name, const char* b_name) {
        const int a = find_pose_index(poses, a_name);
        const int b = find_pose_index(poses, b_name);
        if (a < 0 || b < 0 || !part_points[a].valid || !part_points[b].valid) {
            return;
        }
        const ImU32 color = color_from_array(skeleton_color);
        draw->AddLine(part_points[a].point, part_points[b].point, IM_COL32(0, 0, 0, 220), 3.0f);
        draw->AddLine(part_points[a].point, part_points[b].point, color, 1.5f);
    };

    if (is_r15) {
        draw_bone("Head", "UpperTorso");
        draw_bone("UpperTorso", "LowerTorso");
        draw_bone("UpperTorso", "LeftUpperArm");
        draw_bone("LeftUpperArm", "LeftLowerArm");
        draw_bone("LeftLowerArm", "LeftHand");
        draw_bone("UpperTorso", "RightUpperArm");
        draw_bone("RightUpperArm", "RightLowerArm");
        draw_bone("RightLowerArm", "RightHand");
        draw_bone("LowerTorso", "LeftUpperLeg");
        draw_bone("LeftUpperLeg", "LeftLowerLeg");
        draw_bone("LeftLowerLeg", "LeftFoot");
        draw_bone("LowerTorso", "RightUpperLeg");
        draw_bone("RightUpperLeg", "RightLowerLeg");
        draw_bone("RightLowerLeg", "RightFoot");
    } else {
        draw_bone("Head", "Torso");
        draw_bone("Torso", "Left Arm");
        draw_bone("Torso", "Right Arm");
        draw_bone("Torso", "Left Leg");
        draw_bone("Torso", "Right Leg");
    }
}

static void draw_preview_model(
    ImDrawList* draw,
    const cache::EspEntity& entity,
    const avatarmesh::avatar_mesh* mesh,
    const std::vector<primitive_pose>& poses,
    const canvas_state& canvas,
    float x1,
    float y1,
    float x2,
    float y2) {

    const vec3_t origin = make_vec3(entity.root_x, entity.root_y, entity.root_z);
    const vec3_t model_center = compute_model_center(poses, origin);

    struct tri_cmd {
        ImVec2 a;
        ImVec2 b;
        ImVec2 c;
        float depth;
        ImU32 color;
    };

    std::vector<tri_cmd> tris;
    tris.reserve(1400);
    Clipper2Lib::Paths64 union_paths;

    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;

    auto register_point = [&](const projected_point& pt) {
        if (!pt.valid) {
            return;
        }
        min_x = (std::min)(min_x, pt.point.x);
        min_y = (std::min)(min_y, pt.point.y);
        max_x = (std::max)(max_x, pt.point.x);
        max_y = (std::max)(max_y, pt.point.y);
    };

    const bool use_union_body = mesh_chams_enabled && union_chams;
    const ImU32 base_fill = mesh_chams_enabled ? color_from_array(mesh_chams_color) :
        (chams_enabled ? color_from_array(chams_color) : IM_COL32(124, 118, 142, 214));
    const ImU32 base_line = outline_chams ? color_from_array(outline_chams_color) : IM_COL32(180, 174, 194, 100);

    if (mesh && mesh->valid) {
        for (const primitive_pose& pose : poses) {
            const avatarmesh::mesh_part* part = avatarmesh::find_part(mesh, pose.name, entity.is_r15);
            if (!part || part->vertices.empty() || part->faces.empty()) {
                continue;
            }

            const vec3_t scale = {
                pose.size.x / (std::max)(part->size.x, 0.001f),
                pose.size.y / (std::max)(part->size.y, 0.001f),
                pose.size.z / (std::max)(part->size.z, 0.001f)
            };

            std::vector<vec3_t> local_vertices(part->vertices.size());
            std::vector<projected_point> proj_vertices(part->vertices.size());
            std::vector<vec3_t> camera_vertices(part->vertices.size());
            for (size_t i = 0; i < part->vertices.size(); ++i) {
                const avatarmesh::mesh_vertex& vertex = part->vertices[i];
                const vec3_t local = {
                    (vertex.position.x - part->center.x) * scale.x,
                    (vertex.position.y - part->center.y) * scale.y,
                    (vertex.position.z - part->center.z) * scale.z
                };
                local_vertices[i] = local;
                const vec3_t world_local = sub(add(rotate_local(pose.rot, local), sub(pose.pos, origin)), model_center);
                camera_vertices[i] = to_camera_space(world_local, canvas);
                project_point(world_local, canvas, proj_vertices[i]);
                register_point(proj_vertices[i]);
            }

            float winding_accum = 0.0f;
            for (size_t i = 0; i < part->faces.size(); ++i) {
                const avatarmesh::mesh_face& face = part->faces[i];
                if (face.a >= local_vertices.size() || face.b >= local_vertices.size() || face.c >= local_vertices.size()) {
                    continue;
                }
                const vec3_t& la = local_vertices[face.a];
                const vec3_t& lb = local_vertices[face.b];
                const vec3_t& lc = local_vertices[face.c];
                const vec3_t local_normal = cross(sub(lb, la), sub(lc, la));
                const vec3_t tri_center = mul_scalar(add(add(la, lb), lc), 1.0f / 3.0f);
                winding_accum += dot(local_normal, tri_center);
            }
            const float winding_sign = winding_accum >= 0.0f ? 1.0f : -1.0f;

            for (size_t i = 0; i < part->faces.size(); ++i) {
                const avatarmesh::mesh_face& face = part->faces[i];
                if (face.a >= proj_vertices.size() || face.b >= proj_vertices.size() || face.c >= proj_vertices.size()) {
                    continue;
                }
                const projected_point& a = proj_vertices[face.a];
                const projected_point& b = proj_vertices[face.b];
                const projected_point& c = proj_vertices[face.c];
                if (!a.valid || !b.valid || !c.valid) {
                    continue;
                }

                const float area = (b.point.x - a.point.x) * (c.point.y - a.point.y) - (b.point.y - a.point.y) * (c.point.x - a.point.x);
                if (fabsf(area) < 0.25f) {
                    continue;
                }

                const vec3_t cam_a = camera_vertices[face.a];
                const vec3_t cam_b = camera_vertices[face.b];
                const vec3_t cam_c = camera_vertices[face.c];
                vec3_t normal = normalize(cross(sub(cam_b, cam_a), sub(cam_c, cam_a)));
                if (fabsf(normal.z) < 0.0001f || normal.z * winding_sign >= 0.0f) {
                    continue;
                }
                float light = clampf(dot(normal, normalize(make_vec3(-0.45f, 0.7f, -1.0f))) * 0.5f + 0.5f, 0.18f, 1.0f);
                const float shade = 0.80f + light * 0.22f;
                ImVec4 base = ImGui::ColorConvertU32ToFloat4(base_fill);
                ImVec4 lit = ImVec4(
                    clampf(base.x * shade, 0.0f, 1.0f),
                    clampf(base.y * shade, 0.0f, 1.0f),
                    clampf(base.z * (shade + 0.02f), 0.0f, 1.0f),
                    clampf(base.w, 0.0f, 1.0f));

                tris.push_back({
                    a.point,
                    b.point,
                    c.point,
                    (a.depth + b.depth + c.depth) / 3.0f,
                    ImGui::ColorConvertFloat4ToU32(lit)
                });

            }

            if (use_union_body) {
                std::vector<ImVec2> hull_points;
                hull_points.reserve(proj_vertices.size());
                for (const projected_point& vertex : proj_vertices) {
                    if (vertex.valid) {
                        hull_points.push_back(vertex.point);
                    }
                }
                Clipper2Lib::Path64 hull = make_convex_hull_path(std::move(hull_points));
                if (hull.size() >= 3) {
                    union_paths.push_back(std::move(hull));
                }
            }
        }
    }

    std::sort(tris.begin(), tris.end(), [](const tri_cmd& left, const tri_cmd& right) {
        return left.depth > right.depth;
    });

    if (use_union_body && !union_paths.empty()) {
        Clipper2Lib::Paths64 merged = Clipper2Lib::Union(union_paths, Clipper2Lib::FillRule::Positive);
        const ImU32 union_fill = color_from_array(mesh_chams_color);
        const ImU32 union_outline = outline_chams ? color_from_array(outline_chams_color) : union_fill;
        for (const auto& path : merged) {
            if (!Clipper2Lib::IsPositive(path)) {
                continue;
            }
            draw_merged_path(draw, path, union_fill, union_outline, outline_chams);
        }
    } else {
        for (const tri_cmd& tri : tris) {
            draw->AddTriangleFilled(tri.a, tri.b, tri.c, tri.color);
        }

        if (outline_chams && !tris.empty()) {
            const size_t edge_step = (std::max)((size_t)1, tris.size() / 220);
            for (size_t i = 0; i < tris.size(); i += edge_step) {
                const tri_cmd& tri = tris[i];
                draw->AddTriangle(tri.a, tri.b, tri.c, scale_alpha(base_line, 0.35f), 0.8f);
            }
        }
    }

    for (const primitive_pose& pose : poses) {
        projected_point center{};
        if (project_point(sub(sub(pose.pos, origin), model_center), canvas, center)) {
            register_point(center);
        }
    }

    if (skeleton) {
        draw_skeleton(draw, poses, origin, model_center, canvas, entity.is_r15);
    }

    if (chinahat) {
        const int head_idx = find_pose_index(poses, "Head");
        if (head_idx >= 0) {
            draw_china_hat(draw, poses[head_idx], origin, model_center, canvas);
        }
    }

    const bool box_valid = min_x < max_x && min_y < max_y;
    if (box_valid && box_esp) {
        draw_box(draw, min_x, min_y, max_x, max_y);
    }

    if (box_valid && healthbar) {
        draw_health_bar(draw, min_x, min_y, max_y, entity.health, entity.max_health);
    }

    if (box_valid && health_text) {
        char health_buf[32];
        std::snprintf(health_buf, sizeof(health_buf), "%.0f", entity.health);
        draw_label(draw, ImVec2(min_x - 26.0f, min_y - 14.0f), color_from_array(health_text_color), health_buf);
    }

    if (box_valid && name) {
        const char* preview_name = entity.display_name[0] ? entity.display_name : entity.name;
        draw_label(draw, ImVec2((min_x + max_x) * 0.5f, min_y - 18.0f), color_from_array(name_color), preview_name, true);
    }

    if (box_valid && distance) {
        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        float dist = 0.0f;
        if (lp.valid) {
            const float dx = entity.root_x - lp.x;
            const float dy = entity.root_y - lp.y;
            const float dz = entity.root_z - lp.z;
            dist = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        char dist_buf[32];
        std::snprintf(dist_buf, sizeof(dist_buf), "%.0f studs", dist);
        draw_label(draw, ImVec2((min_x + max_x) * 0.5f, max_y + 6.0f), color_from_array(distance_color), dist_buf, true);
    }

    if (box_valid && rig_type) {
        draw_label(draw, ImVec2(max_x + 8.0f, min_y), color_from_array(rig_type_color), entity.is_r15 ? "R15" : "R6");
    }

    if (tool_esp && entity.tool_name[0]) {
        const ImVec2 tool_pos = box_valid ?
            ImVec2(max_x + 8.0f, min_y + 16.0f) :
            ImVec2(x1 + 10.0f, y1 + 28.0f);
        draw_label(draw, tool_pos, color_from_array(tool_color), entity.tool_name);
    }

    const char* header_name = entity.display_name[0] ? entity.display_name : entity.name;
    draw_label(draw, ImVec2(x1 + 10.0f, y1 + 8.0f), IM_COL32(210, 205, 220, 255), header_name);
}

}

void RenderESPPreview() {
    if (!esp_preview_3d) {
        return;
    }
    if (menu_alpha <= 0.0f) {
        return;
    }

    static float drag_x = 0.0f;
    static float drag_y = 0.0f;
    static bool dragging = false;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 window_size(430.0f, 380.0f);
    const float topbar_h = theme::topbar_h;
    const float border = theme::out_thick;

    const float base_x = display.x * 0.5f + theme::menu_size[0] * 0.5f + 24.0f;
    const float base_y = display.y * 0.5f - window_size.y * 0.5f;

    ImGui::SetNextWindowPos(ImVec2(base_x + drag_x, base_y + drag_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, menu_alpha);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse;

    if (ImGui::Begin("3D ESP Preview", nullptr, flags)) {
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        ImDrawList* draw = ImGui::GetWindowDrawList();

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool in_title = mouse.x >= wp.x && mouse.x <= wp.x + ws.x && mouse.y >= wp.y && mouse.y <= wp.y + topbar_h;
        if (ImGui::IsMouseReleased(0)) {
            dragging = false;
        }
        if (in_title && ImGui::IsMouseClicked(0)) {
            dragging = true;
        }
        if (dragging && ImGui::IsMouseDown(0)) {
            drag_x += ImGui::GetIO().MouseDelta.x;
            drag_y += ImGui::GetIO().MouseDelta.y;
        }

        draw->AddRectFilledMultiColor(
            wp,
            ImVec2(wp.x + ws.x, wp.y + topbar_h),
            theme::col_alpha(theme::topbar_grad_t, 255),
            theme::col_alpha(theme::topbar_grad_t, 255),
            theme::col_alpha(theme::topbar_grad_b, 255),
            theme::col_alpha(theme::topbar_grad_b, 255));
        draw->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + topbar_h), theme::col_alpha(theme::topbar_inline, 255), 0.0f, 0, border);
        draw->AddLine(ImVec2(wp.x, wp.y + topbar_h), ImVec2(wp.x + ws.x, wp.y + topbar_h), theme::col_alpha(theme::accent_border, 255), border);
        draw->AddText(ImVec2(wp.x + 10.0f, wp.y + 5.0f), theme::col_alpha(theme::menu_text, 255), "3d esp preview");

        const ImVec2 content_min(wp.x + 10.0f, wp.y + topbar_h + 10.0f);
        const ImVec2 content_max(wp.x + ws.x - 10.0f, wp.y + ws.y - 10.0f);
        draw_canvas_background(draw, content_min, content_max);

        preview_target target = select_preview_target();
        if (!target.entity) {
            draw_label(draw, ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.5f + 2.0f), IM_COL32(220, 216, 228, 255), "load into game", true);
        } else {
            std::vector<primitive_pose> poses = collect_poses(*target.entity);
            if (poses.empty()) {
                draw_label(draw, ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.5f + 2.0f), IM_COL32(220, 216, 228, 255), "load into game", true);
            } else {
                const vec3_t origin = make_vec3(target.entity->root_x, target.entity->root_y, target.entity->root_z);
                const vec3_t center = compute_model_center(poses, origin);
                const float radius = compute_model_radius(poses, origin, center);

                canvas_state canvas{};
                canvas.min = content_min;
                canvas.max = content_max;
                canvas.width = content_max.x - content_min.x;
                canvas.height = content_max.y - content_min.y;
                canvas.center = ImVec2((content_min.x + content_max.x) * 0.5f, content_min.y + canvas.height * 0.53f);
                canvas.yaw = (float)ImGui::GetTime() * 0.55f;
                canvas.pitch = -0.24f;
                canvas.focal = (std::min)(canvas.width, canvas.height) * 1.26f;
                canvas.cam_dist = radius * 2.55f + 3.5f;

                draw_preview_model(draw, *target.entity, target.mesh, poses, canvas, content_min.x, content_min.y, content_max.x, content_max.y);
            }
        }

        draw->AddRect(ImVec2(wp.x - border, wp.y - border), ImVec2(wp.x + ws.x + border, wp.y + ws.y + border), theme::col_alpha(theme::accent_border, 255), 0.0f, 0, border);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

}
