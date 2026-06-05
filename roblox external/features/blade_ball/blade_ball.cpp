#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cfloat>
#include <cstdio>
#include <cmath>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "blade_ball.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "game.h"
#include "imgui/imgui.h"

namespace features {
    namespace {
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

        struct BallInfo {
            uintptr_t primitive = 0;
            unsigned int engagement_id = 0;
            uintptr_t target_character = 0;
            Vec3 position{};
            Vec3 velocity{};
            Vec3 filtered_velocity{};
            Vec3 acceleration{};
            Vec3 predicted_impact_position{};
            bool targeting_local = false;
            float horizontal_distance = 0.0f;
            float vertical_distance = 0.0f;
            float total_distance = 0.0f;
            float speed = 0.0f;
            float filtered_speed = 0.0f;
            float time_to_reach = 0.0f;  // Time in seconds until ball reaches player
            float time_to_intercept = 999.0f;
            float time_to_closest_approach = 999.0f;
            float approach_factor = 0.0f;  // How directly ball is moving toward player (-1 to 1)
            float closest_distance = FLT_MAX;
            float closest_horizontal_distance = FLT_MAX;
            float closest_vertical_distance = FLT_MAX;
            float closing_speed = 0.0f;
            float curve_strength = 0.0f;
            float close_range_factor = 0.0f;
            float parry_distance_threshold = 0.0f;
            float parry_height_threshold = 0.0f;
            float prediction_confidence = 0.0f;
            bool moving_toward_player = false;
            bool predicted_to_intercept = false;
        };

        struct BallTrack {
            Vec3 last_position{};
            Vec3 last_velocity{};
            Vec3 filtered_velocity{};
            Vec3 acceleration{};
            Vec3 last_velocity_direction{};
            float last_approach_factor = 0.0f;
            float last_total_distance = FLT_MAX;
            bool last_targeting_local = false;
            unsigned int engagement_id = 0;
            int away_frame_count = 0;
            std::chrono::steady_clock::time_point last_update{};
            bool valid = false;
        };

        static std::chrono::steady_clock::time_point g_last_parry = std::chrono::steady_clock::now();
        static std::chrono::steady_clock::time_point g_last_in_range = std::chrono::steady_clock::now();
        static std::chrono::steady_clock::time_point g_last_ball_snapshot = std::chrono::steady_clock::time_point{};
        static std::chrono::steady_clock::time_point g_parry_release_at = std::chrono::steady_clock::time_point{};
        static uintptr_t g_last_parried_ball = 0;
        static unsigned int g_last_parried_engagement_id = 0;
        static std::unordered_map<uintptr_t, BallTrack> g_ball_tracks;
        static std::vector<BallInfo> g_cached_balls;
        static Vec3 g_cached_local_pos{};
        static bool g_parry_input_down = false;

        static Vec3 operator+(const Vec3& lhs, const Vec3& rhs) {
            return { lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z };
        }

        static Vec3 operator-(const Vec3& lhs, const Vec3& rhs) {
            return { lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z };
        }

        static Vec3 operator*(const Vec3& vec, float scalar) {
            return { vec.x * scalar, vec.y * scalar, vec.z * scalar };
        }

        static Vec3 operator/(const Vec3& vec, float scalar) {
            if (std::fabs(scalar) < 0.0001f) return { 0.0f, 0.0f, 0.0f };
            return { vec.x / scalar, vec.y / scalar, vec.z / scalar };
        }

        static float Dot(const Vec3& lhs, const Vec3& rhs) {
            return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
        }

        static float Length(const Vec3& vec) {
            return std::sqrt(Dot(vec, vec));
        }

        static Vec3 Normalize(const Vec3& vec) {
            float len = Length(vec);
            if (len < 0.0001f) return { 0.0f, 0.0f, 0.0f };
            return { vec.x / len, vec.y / len, vec.z / len };
        }

        static float HorizontalLength(const Vec3& vec) {
            return std::sqrt((vec.x * vec.x) + (vec.z * vec.z));
        }

        static bool IsFiniteFloat(float value) {
            return std::isfinite(value) != 0;
        }

        static bool IsFiniteVec3(const Vec3& vec) {
            return IsFiniteFloat(vec.x) && IsFiniteFloat(vec.y) && IsFiniteFloat(vec.z);
        }

        static Vec3 SanitizeVec3(const Vec3& vec) {
            Vec3 sanitized = vec;
            if (!IsFiniteFloat(sanitized.x)) sanitized.x = 0.0f;
            if (!IsFiniteFloat(sanitized.y)) sanitized.y = 0.0f;
            if (!IsFiniteFloat(sanitized.z)) sanitized.z = 0.0f;
            return sanitized;
        }

        static float ClampFloat(float value, float min_value, float max_value) {
            return std::clamp(value, min_value, max_value);
        }

        static Vec3 LerpVec3(const Vec3& from, const Vec3& to, float alpha) {
            const float t = ClampFloat(alpha, 0.0f, 1.0f);
            return from + (to - from) * t;
        }

        static Vec3 PredictPosition(const Vec3& position, const Vec3& velocity, const Vec3& acceleration, float time_seconds) {
            const float t = ClampFloat(time_seconds, 0.0f, FLT_MAX);
            return position + (velocity * t) + (acceleration * (0.5f * t * t));
        }

        static void PruneOldBallTracks(const std::chrono::steady_clock::time_point& now) {
            constexpr float stale_seconds = 2.0f;
            for (auto it = g_ball_tracks.begin(); it != g_ball_tracks.end();) {
                const float age = std::chrono::duration<float>(now - it->second.last_update).count();
                if (!it->second.valid || age > stale_seconds) {
                    it = g_ball_tracks.erase(it);
                } else {
                    ++it;
                }
            }
        }

        static void UpdateBallTrack(uintptr_t primitive, const Vec3& position, const Vec3& velocity, BallInfo& info, const std::chrono::steady_clock::time_point& now) {
            BallTrack& track = g_ball_tracks[primitive];
            const Vec3 safe_position = SanitizeVec3(position);
            const Vec3 safe_velocity = SanitizeVec3(velocity);

            if (!track.valid) {
                track.last_position = safe_position;
                track.last_velocity = safe_velocity;
                track.filtered_velocity = safe_velocity;
                track.acceleration = { 0.0f, 0.0f, 0.0f };
                track.last_velocity_direction = Normalize(safe_velocity);
                track.last_update = now;
                track.valid = true;
            } else {
                const float dt = std::chrono::duration<float>(now - track.last_update).count();
                if (dt > 0.0005f) {
                    const Vec3 measured_velocity = SanitizeVec3((safe_position - track.last_position) / dt);
                    const Vec3 blended_velocity = SanitizeVec3(LerpVec3(safe_velocity, measured_velocity, 0.6f));
                    track.filtered_velocity = LerpVec3(track.filtered_velocity, blended_velocity, 0.45f);

                    const Vec3 raw_acceleration = SanitizeVec3((track.filtered_velocity - track.last_velocity) / dt);
                    track.acceleration = LerpVec3(track.acceleration, raw_acceleration, 0.35f);
                    const float acceleration_magnitude = Length(track.acceleration);
                    if (!IsFiniteFloat(acceleration_magnitude)) {
                        track.acceleration = { 0.0f, 0.0f, 0.0f };
                    } else if (acceleration_magnitude > 450.0f) {
                        track.acceleration = Normalize(track.acceleration) * 450.0f;
                    }
                }

                track.filtered_velocity = SanitizeVec3(track.filtered_velocity);
                track.acceleration = SanitizeVec3(track.acceleration);
                track.last_position = safe_position;
                track.last_velocity = track.filtered_velocity;
                track.last_velocity_direction = Normalize(track.filtered_velocity);
                track.last_update = now;
            }

            info.filtered_velocity = track.filtered_velocity;
            info.acceleration = track.acceleration;
            info.filtered_speed = Length(track.filtered_velocity);
        }

        static void AnalyzeBallTrajectory(BallInfo& info, const Vec3& local_pos) {
            info.position = SanitizeVec3(info.position);
            info.velocity = SanitizeVec3(info.velocity);
            info.filtered_velocity = SanitizeVec3(info.filtered_velocity);
            info.acceleration = SanitizeVec3(info.acceleration);
            if (!IsFiniteVec3(local_pos)) {
                return;
            }

            const Vec3 relative_position = info.position - local_pos;
            info.closest_distance = info.total_distance;
            info.closest_horizontal_distance = info.horizontal_distance;
            info.closest_vertical_distance = info.vertical_distance;
            info.predicted_impact_position = info.position;
            info.time_to_reach = 999.0f;
            info.time_to_intercept = 999.0f;
            info.time_to_closest_approach = 999.0f;
            info.predicted_to_intercept = false;
            info.prediction_confidence = 0.0f;
            info.closing_speed = 0.0f;
            info.curve_strength = 0.0f;
            info.close_range_factor = 0.0f;
            info.parry_distance_threshold = blade_ball_parry_distance;
            info.parry_height_threshold = blade_ball_parry_height;
            info.moving_toward_player = false;
            info.approach_factor = 0.0f;

            if (info.filtered_speed < 1.0f || info.total_distance < 0.05f) {
                return;
            }

            const Vec3 toward_local = Normalize(local_pos - info.position);
            const Vec3 velocity_direction = Normalize(info.filtered_velocity);
            info.approach_factor = Dot(velocity_direction, toward_local);
            info.closing_speed = ClampFloat(Dot(info.filtered_velocity, toward_local), 0.0f, FLT_MAX);
            const float lateral_acceleration = Length(info.acceleration - (toward_local * Dot(info.acceleration, toward_local)));
            info.curve_strength = ClampFloat(lateral_acceleration / 220.0f, 0.0f, 1.0f);
            info.close_range_factor = 1.0f - ClampFloat(info.total_distance / 18.0f, 0.0f, 1.0f);
            info.moving_toward_player = info.approach_factor > 0.15f && info.closing_speed > 1.0f;
            const float fast_ball_factor = ClampFloat((info.filtered_speed - 140.0f) / 180.0f, 0.0f, 1.0f);
            const float closing_factor = ClampFloat((info.closing_speed - 45.0f) / 120.0f, 0.0f, 1.0f);
            info.parry_distance_threshold =
                blade_ball_parry_distance +
                (fast_ball_factor * 7.0f) +
                (closing_factor * 4.0f);
            info.parry_height_threshold =
                blade_ball_parry_height +
                (fast_ball_factor * 2.5f) +
                (closing_factor * 1.5f);

            const float speed_factor = ClampFloat(info.filtered_speed / 200.0f, 0.72f, 2.2f);
            const float prediction_horizon = 1.35f * speed_factor;
            constexpr int prediction_steps = 110;
            bool found_intercept = false;

            for (int step = 1; step <= prediction_steps; ++step) {
                const float t = prediction_horizon * (static_cast<float>(step) / static_cast<float>(prediction_steps));
                const Vec3 predicted = PredictPosition(info.position, info.filtered_velocity, info.acceleration, t);
                if (!IsFiniteVec3(predicted)) continue;
                const Vec3 delta = predicted - local_pos;
                const float horizontal = HorizontalLength(delta);
                const float vertical = std::fabs(delta.y);
                const float total = Length(delta);
                if (!IsFiniteFloat(horizontal) || !IsFiniteFloat(vertical) || !IsFiniteFloat(total)) continue;

                if (total < info.closest_distance) {
                    info.closest_distance = total;
                    info.closest_horizontal_distance = horizontal;
                    info.closest_vertical_distance = vertical;
                    info.time_to_closest_approach = t;
                    info.predicted_impact_position = predicted;
                }

                if (!found_intercept &&
                    horizontal <= info.parry_distance_threshold &&
                    vertical <= info.parry_height_threshold) {
                    found_intercept = true;
                    info.predicted_to_intercept = true;
                    info.time_to_intercept = t;
                    info.time_to_reach = t;
                    info.predicted_impact_position = predicted;
                }
            }

            if (info.time_to_closest_approach == 999.0f) {
                const float speed_sq = Dot(info.filtered_velocity, info.filtered_velocity);
                if (speed_sq > 1.0f) {
                    const float closest_t = ClampFloat(-Dot(relative_position, info.filtered_velocity) / speed_sq, 0.0f, prediction_horizon);
                    info.time_to_closest_approach = closest_t;
                    const Vec3 predicted = PredictPosition(info.position, info.filtered_velocity, info.acceleration, closest_t);
                    const Vec3 delta = predicted - local_pos;
                    info.closest_distance = Length(delta);
                    info.closest_horizontal_distance = HorizontalLength(delta);
                    info.closest_vertical_distance = std::fabs(delta.y);
                    info.predicted_impact_position = predicted;
                }
            }

            if (!found_intercept && info.closing_speed > 1.0f) {
                info.time_to_reach = info.total_distance / info.closing_speed;
            }

            const float intercept_score =
                (info.predicted_to_intercept ? 0.65f : 0.0f) +
                ClampFloat(info.approach_factor, 0.0f, 1.0f) * 0.18f +
                ClampFloat(info.closing_speed / 220.0f, 0.0f, 1.0f) * 0.12f +
                info.curve_strength * 0.05f;
            info.prediction_confidence = ClampFloat(intercept_score, 0.0f, 1.0f);
        }

        static void UpdateBallEngagement(BallInfo& info) {
            auto track_it = g_ball_tracks.find(info.primitive);
            if (track_it == g_ball_tracks.end()) return;

            BallTrack& track = track_it->second;
            const bool newly_targeted = info.targeting_local && !track.last_targeting_local;
            const bool bounced_back_toward_local =
                info.moving_toward_player &&
                track.last_approach_factor < -0.20f &&
                track.away_frame_count >= 1;
            const bool regained_after_away =
                info.targeting_local &&
                track.away_frame_count >= 2 &&
                info.approach_factor > 0.20f &&
                info.predicted_to_intercept;

            const bool moving_away_now =
                !info.targeting_local ||
                info.approach_factor < -0.05f ||
                (!info.predicted_to_intercept && info.total_distance > track.last_total_distance + 0.5f);

            if (moving_away_now) {
                track.away_frame_count = (std::min)(track.away_frame_count + 1, 5);
            } else {
                track.away_frame_count = 0;
            }

            if ((newly_targeted || bounced_back_toward_local || regained_after_away) &&
                (info.predicted_to_intercept || info.moving_toward_player)) {
                ++track.engagement_id;
            } else if (track.engagement_id == 0 &&
                       info.targeting_local &&
                       (info.predicted_to_intercept || info.moving_toward_player)) {
                track.engagement_id = 1;
            }

            info.engagement_id = track.engagement_id;
            track.last_targeting_local = info.targeting_local;
            track.last_approach_factor = info.approach_factor;
            track.last_total_distance = info.total_distance;
        }

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

        static bool IsBallTargetingLocalByAttribute(const instance& ball) {
            if (!ball.is_valid()) return false;

            // Get local player character
            instance dm = game::ReadDatamodel(g_base_address);
            if (!dm.is_valid()) return false;

            instance players = dm.read_service("Players");
            if (!players.is_valid()) return false;

            instance local_player = players.local_player();
            if (!local_player.is_valid()) return false;

            instance character = local_player.model_instance();
            if (!character.is_valid()) return false;

            // Check if ball has a "Target" child pointing to our character
            instance target = ball.read_child("Target");
            if (target.is_valid()) {
                // Read the ObjectValue - it should point to our character
                uintptr_t target_value = read<uintptr_t>(target.address + Offsets::Misc::Value);
                if (target_value == character.address) {
                    return true;
                }
            }

            // Also check if our character has a Highlight (alternative method)
            instance char_highlight = character.read_child("Highlight");
            if (char_highlight.is_valid()) {
                return true;
            }

            return false;
        }

        static uintptr_t GetBallTargetCharacterAddress(const instance& ball, uintptr_t local_character_address, bool& out_targeting_local) {
            out_targeting_local = false;
            if (!ball.is_valid()) return 0;

            instance target = ball.read_child("Target");
            if (target.is_valid()) {
                uintptr_t target_value = read<uintptr_t>(target.address + Offsets::Misc::Value);
                if (is_valid_address(target_value)) {
                    out_targeting_local = (target_value == local_character_address);
                    return target_value;
                }
            }

            if (is_valid_address(local_character_address)) {
                instance local_character{ local_character_address };
                instance char_highlight = local_character.read_child("Highlight");
                if (char_highlight.is_valid()) {
                    out_targeting_local = true;
                    return local_character_address;
                }
            }

            return 0;
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
            float w = world.x * m[12] + world.y * m[13] + world.z * m[14] + m[15];
            if (w < 0.01f) return false;
            float screen_x = world.x * m[0] + world.y * m[1] + world.z * m[2] + m[3];
            float screen_y = world.x * m[4] + world.y * m[5] + world.z * m[6] + m[7];
            float inv_w = 1.0f / w;
            out.x = (viewport.x * 0.5f * screen_x * inv_w) + (viewport.x * 0.5f);
            out.y = -(viewport.y * 0.5f * screen_y * inv_w) + (viewport.y * 0.5f);
            return out.x == out.x && out.y == out.y;
        }

        static bool IsNumericName(const std::string& name) {
            return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            });
        }

        static uintptr_t GetPrimitiveFromInstance(const instance& inst) {
            if (!inst.is_valid()) return 0;

            uintptr_t primitive = read<uintptr_t>(inst.address + Offsets::BasePart::Primitive);
            if (is_valid_address(primitive)) return primitive;

            for (const instance& child : inst.get_children()) {
                if (!child.is_valid()) continue;
                primitive = read<uintptr_t>(child.address + Offsets::BasePart::Primitive);
                if (is_valid_address(primitive)) return primitive;
            }

            return 0;
        }

        static std::vector<instance> GetBladeBallCandidates() {
            std::vector<instance> balls;
            balls.reserve(8);

            instance dm = game::ReadDatamodel(g_base_address);
            if (!dm.is_valid()) return balls;

            instance workspace = dm.read_service("Workspace");
            if (!workspace.is_valid()) return balls;

            instance balls_folder = workspace.read_child("Balls");
            if (balls_folder.is_valid()) {
                for (const instance& child : balls_folder.get_children()) {
                    if (child.is_valid()) {
                        balls.push_back(child);
                    }
                }
            }

            if (!balls.empty()) return balls;

            for (const instance& child : workspace.get_children()) {
                if (!child.is_valid()) continue;
                if (IsNumericName(child.get_name())) {
                    balls.push_back(child);
                }
            }

            return balls;
        }

        static bool GetLocalPlayerAndCharacter(instance& out_player, instance& out_character) {
            out_player = {};
            out_character = {};

            instance dm = game::ReadDatamodel(g_base_address);
            if (!dm.is_valid()) return false;

            instance players = dm.read_service("Players");
            if (!players.is_valid()) return false;

            out_player = players.local_player();
            if (!out_player.is_valid()) return false;

            out_character = out_player.model_instance();
            return out_character.is_valid();
        }

        static bool ReadLocalPosition(Vec3& out_pos) {
            const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
            if (!lp.valid) return false;
            out_pos = { lp.x, lp.y, lp.z };
            return true;
        }

        static bool GatherBallInfo(std::vector<BallInfo>& out_balls, Vec3& local_pos) {
            out_balls.clear();
            if (!ReadLocalPosition(local_pos)) return false;
            if (!IsFiniteVec3(local_pos)) return false;

            instance local_player{};
            instance local_character{};
            uintptr_t local_character_address = 0;
            if (GetLocalPlayerAndCharacter(local_player, local_character)) {
                local_character_address = local_character.address;
            }

            const auto now = std::chrono::steady_clock::now();
            PruneOldBallTracks(now);

            std::vector<instance> candidates = GetBladeBallCandidates();

            for (const instance& ball : candidates) {
                uintptr_t primitive = GetPrimitiveFromInstance(ball);
                if (!is_valid_address(primitive)) continue;

                Vec3 ball_pos{};
                if (!ReadVec3(primitive + Offsets::Primitive::Position, ball_pos)) continue;
                ball_pos = SanitizeVec3(ball_pos);
                if (ball_pos.x == 0.0f && ball_pos.y == 0.0f && ball_pos.z == 0.0f) continue;

                Vec3 ball_vel{};
                ReadVec3(primitive + Offsets::Primitive::AssemblyLinearVelocity, ball_vel);
                ball_vel = SanitizeVec3(ball_vel);

                float dx = ball_pos.x - local_pos.x;
                float dy = ball_pos.y - local_pos.y;
                float dz = ball_pos.z - local_pos.z;

                BallInfo info{};
                info.primitive = primitive;
                info.position = ball_pos;
                info.velocity = ball_vel;
                info.horizontal_distance = std::sqrt(dx * dx + dz * dz);
                info.vertical_distance = std::fabs(dy);
                info.total_distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                info.speed = std::sqrt(ball_vel.x * ball_vel.x + ball_vel.y * ball_vel.y + ball_vel.z * ball_vel.z);
                info.target_character = GetBallTargetCharacterAddress(ball, local_character_address, info.targeting_local);
                UpdateBallTrack(primitive, ball_pos, ball_vel, info, now);
                AnalyzeBallTrajectory(info, local_pos);
                UpdateBallEngagement(info);

                out_balls.push_back(info);
            }

            return !out_balls.empty();
        }

        static void UpdateParryInputState(const std::chrono::steady_clock::time_point& now) {
            if (g_parry_input_down && now >= g_parry_release_at) {
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                g_parry_input_down = false;
                g_parry_release_at = std::chrono::steady_clock::time_point{};
            }
        }

        static void SendParryInput(const std::chrono::steady_clock::time_point& now) {
            UpdateParryInputState(now);
            if (g_parry_input_down) {
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                g_parry_input_down = false;
            }
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            g_parry_input_down = true;
            g_parry_release_at = now + std::chrono::milliseconds(50);
        }

    }

    void RunBladeBallAutoParry() {
        if (!blade_ball_auto_parry) return;

        const auto now = std::chrono::steady_clock::now();
        UpdateParryInputState(now);

        Vec3 local_pos{};
        std::vector<BallInfo> balls;
        if (!GatherBallInfo(balls, local_pos)) {
            const float seconds_since_seen = std::chrono::duration<float>(now - g_last_in_range).count();
            if (seconds_since_seen > 0.20f) {
                g_last_parried_ball = 0;
                g_last_parried_engagement_id = 0;
            }
            return;
        }

        g_cached_balls = balls;
        g_cached_local_pos = local_pos;
        g_last_ball_snapshot = now;

        const float seconds_since_last_parry = std::chrono::duration<float>(now - g_last_parry).count();
        constexpr float rearm_grace_period = 0.18f;

        BallInfo* most_dangerous = nullptr;
        float best_score = -FLT_MAX;

            for (size_t i = 0; i < balls.size(); i++) {
                BallInfo& ball = balls[i];

                if (!ball.targeting_local) continue;
                if (!ball.moving_toward_player && !ball.predicted_to_intercept) continue;

                const bool currently_in_range =
                    ball.horizontal_distance <= ball.parry_distance_threshold &&
                    ball.vertical_distance <= ball.parry_height_threshold;
                const bool predicted_threat =
                    ball.predicted_to_intercept ||
                    (ball.closest_horizontal_distance <= ball.parry_distance_threshold * 0.85f &&
                     ball.closest_vertical_distance <= ball.parry_height_threshold);

                if (!currently_in_range && !predicted_threat) continue;

            const float time_score = ball.predicted_to_intercept ? ball.time_to_intercept : ball.time_to_closest_approach;
            const float danger_score =
                (ball.predicted_to_intercept ? 140.0f : 0.0f) +
                (ball.prediction_confidence * 80.0f) +
                (ball.closing_speed * 0.20f) +
                (ball.filtered_speed * 0.08f) -
                (time_score * 55.0f) -
                (ball.closest_distance * 1.5f);

            if (danger_score > best_score) {
                best_score = danger_score;
                most_dangerous = &ball;
            }
        }

        if (most_dangerous != nullptr) {
            g_last_in_range = now;
            const float intercept_time =
                most_dangerous->predicted_to_intercept
                ? most_dangerous->time_to_intercept
                : most_dangerous->time_to_reach;

            float reaction_buffer = 0.038f;
            reaction_buffer += ClampFloat(most_dangerous->filtered_speed / 280.0f, 0.0f, 1.0f) * 0.022f;
            reaction_buffer += ClampFloat(most_dangerous->filtered_speed / 180.0f, 0.0f, 1.0f) * 0.018f;
            reaction_buffer += ClampFloat(Length(most_dangerous->acceleration) / 320.0f, 0.0f, 1.0f) * 0.010f;
            reaction_buffer += most_dangerous->curve_strength * 0.012f;
            reaction_buffer += (1.0f - ClampFloat(most_dangerous->approach_factor, 0.0f, 1.0f)) * 0.010f;
            reaction_buffer -= ClampFloat(most_dangerous->prediction_confidence, 0.0f, 1.0f) * 0.008f;
            reaction_buffer -= ClampFloat((120.0f - most_dangerous->filtered_speed) / 120.0f, 0.0f, 1.0f) * 0.016f;
            reaction_buffer = ClampFloat(reaction_buffer, 0.034f, 0.090f);
            constexpr float single_fire_cooldown = 0.20f;
            const float min_confidence =
                (most_dangerous->filtered_speed > 180.0f && most_dangerous->closing_speed > 60.0f)
                    ? 0.28f
                    : ((most_dangerous->filtered_speed < 120.0f && most_dangerous->closing_speed < 45.0f)
                        ? 0.52f
                        : 0.45f);
            const float min_intercept_time =
                ClampFloat((120.0f - most_dangerous->filtered_speed) / 120.0f, 0.0f, 1.0f) * 0.028f;
            const bool same_ball_as_last_parry = g_last_parried_ball == most_dangerous->primitive;
            const bool same_engagement_as_last_parry = g_last_parried_engagement_id == most_dangerous->engagement_id;
            const bool ball_has_passed =
                !most_dangerous->moving_toward_player ||
                most_dangerous->approach_factor < -0.10f ||
                (seconds_since_last_parry > 0.35f && !most_dangerous->predicted_to_intercept);
            const bool should_rearm_for_new_pass =
                same_ball_as_last_parry &&
                same_engagement_as_last_parry &&
                ball_has_passed;

            if (should_rearm_for_new_pass) {
                g_last_parried_ball = 0;
                g_last_parried_engagement_id = 0;
            }

            const bool should_parry =
                most_dangerous->predicted_to_intercept &&
                intercept_time <= reaction_buffer &&
                intercept_time >= min_intercept_time &&
                most_dangerous->prediction_confidence >= min_confidence &&
                most_dangerous->closing_speed >= 12.0f &&
                seconds_since_last_parry >= single_fire_cooldown &&
                !(same_ball_as_last_parry && same_engagement_as_last_parry);

            if (should_parry) {
                SendParryInput(now);
                g_last_parry = now;
                g_last_parried_ball = most_dangerous->primitive;
                g_last_parried_engagement_id = most_dangerous->engagement_id;
                return;
            }
        } else {
            const float seconds_since_seen = std::chrono::duration<float>(now - g_last_in_range).count();
            if (seconds_since_seen > rearm_grace_period) {
                g_last_parried_ball = 0;
                g_last_parried_engagement_id = 0;
            }
        }
    }

    void RenderBladeBallESP() {
        if (!blade_ball_auto_parry) return;

        const auto now = std::chrono::steady_clock::now();
        if (!blade_ball_ball_esp) return;

        Matrix4 view{};
        Vec2 viewport{};
        if (!GetCamera(view, viewport)) return;

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw) return;

        Vec3 local_pos = g_cached_local_pos;
        const std::vector<BallInfo>* balls = &g_cached_balls;
        const float snapshot_age = (g_last_ball_snapshot.time_since_epoch().count() == 0)
            ? 999.0f
            : std::chrono::duration<float>(now - g_last_ball_snapshot).count();

        std::vector<BallInfo> fresh_balls;
        if (snapshot_age > 0.03f || balls->empty() || !IsFiniteVec3(local_pos)) {
            if (!GatherBallInfo(fresh_balls, local_pos)) return;
            balls = &fresh_balls;
            g_cached_balls = fresh_balls;
            g_cached_local_pos = local_pos;
            g_last_ball_snapshot = now;
        }

        if (blade_ball_ball_esp) {
            Vec2 local_screen{};
            if (!WorldToScreen(local_pos, local_screen, view, viewport)) return;

            const BallInfo* display_ball = nullptr;
            float best_display_score = -FLT_MAX;
            for (const BallInfo& ball : *balls) {
                if (!ball.targeting_local) continue;
                const float display_score =
                    (ball.predicted_to_intercept ? 100.0f : 0.0f) +
                    (ball.prediction_confidence * 60.0f) +
                    (ball.closing_speed * 0.15f) -
                    (ball.time_to_intercept * 45.0f);

                if (display_score > best_display_score) {
                    best_display_score = display_score;
                    display_ball = &ball;
                }
            }
            const float display_horizontal_envelope =
                (display_ball != nullptr)
                ? display_ball->parry_distance_threshold
                : 12.0f;

            constexpr int segments = 32;
            ImVec2 ring_points[segments];
            int ring_count = 0;

            for (int i = 0; i < segments; ++i) {
                float angle = (2.0f * 3.14159265f * (float)i) / (float)segments;
                Vec3 point{
                    local_pos.x + std::cos(angle) * display_horizontal_envelope,
                    local_pos.y,
                    local_pos.z + std::sin(angle) * display_horizontal_envelope
                };

                Vec2 screen{};
                if (WorldToScreen(point, screen, view, viewport)) {
                    ring_points[ring_count++] = ImVec2(screen.x, screen.y);
                }
            }

            if (ring_count >= 3) {
                draw->AddPolyline(ring_points, ring_count, IM_COL32(0, 255, 180, 180), ImDrawFlags_Closed, 2.0f);
            }

            for (const BallInfo& ball : *balls) {
                Vec2 ball_screen{};
                if (!WorldToScreen(ball.position, ball_screen, view, viewport)) continue;

                bool in_range =
                    ball.horizontal_distance <= ball.parry_distance_threshold &&
                    ball.vertical_distance <= ball.parry_height_threshold;
                ImU32 color = IM_COL32(255, 255, 0, 170);
                if (ball.targeting_local) {
                    color = IM_COL32(255, 64, 64, 255);
                } else if (in_range) {
                    color = IM_COL32(255, 180, 0, 220);
                }

                draw->AddLine(ImVec2(local_screen.x, local_screen.y), ImVec2(ball_screen.x, ball_screen.y), color, 2.0f);
                draw->AddCircleFilled(ImVec2(ball_screen.x, ball_screen.y), 4.0f, color);

                if (ball.targeting_local) {
                    if (ball.predicted_to_intercept) {
                        Vec2 predicted_screen{};
                        if (WorldToScreen(ball.predicted_impact_position, predicted_screen, view, viewport)) {
                            draw->AddLine(ImVec2(ball_screen.x, ball_screen.y), ImVec2(predicted_screen.x, predicted_screen.y), IM_COL32(0, 255, 120, 180), 1.5f);
                            draw->AddCircle(ImVec2(predicted_screen.x, predicted_screen.y), 12.0f, IM_COL32(255, 255, 255, 180), 24, 1.5f);
                        }
                    }
                }
            }
        }

    }
}
