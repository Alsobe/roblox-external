#include "cache.h"
#include "memory.h"
#include "offsets.h"
#include "globals.h"
#include "game.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>

struct vec3_t { float x, y, z; };

namespace cache {
    static std::mutex s_mutex;
    static std::vector<EspEntity> s_entities;
    static std::vector<SkeletonEntity> s_skeletons;
    static LocalPlayerData s_local{};
    static bool s_running = false;
    static HANDLE s_thread = nullptr;

    std::mutex& GetMutex() { return s_mutex; }

    static void scpy(char* dst, const char* src, size_t max) {
        if (!src) { dst[0] = '\0'; return; }
        strncpy_s(dst, max, src, _TRUNCATE);
    }

    static uintptr_t get_prim(instance part) {
        if (!part.is_valid()) return 0;
        return read<uintptr_t>(part.address + Offsets::BasePart::Primitive);
    }

    static bool find_part(instance ch, const char* name, uintptr_t& out) {
        for (auto& c : ch.get_children()) {
            if (!c.is_valid()) continue;
            if (c.get_name() == name) { out = get_prim(c); return true; }
        }
        return false;
    }

    static bool is_r15(instance ch) {
        for (auto& c : ch.get_children()) {
            if (!c.is_valid()) continue;
            auto n = c.get_name();
            if (n == "UpperTorso" || n == "LowerTorso" ||
                n == "LeftUpperArm" || n == "RightUpperArm" ||
                n == "LeftUpperLeg" || n == "RightUpperLeg")
                return true;
        }
        return false;
    }

    static bool build_skel(instance ch, SkeletonEntity& s) {
        if (!ch.is_valid()) return false;
        s.is_r15 = is_r15(ch);
        find_part(ch, "Head", s.head);
        find_part(ch, "UpperTorso", s.upper_torso);
        find_part(ch, "LowerTorso", s.lower_torso);
        find_part(ch, "LeftHand", s.left_hand);
        find_part(ch, "RightHand", s.right_hand);
        find_part(ch, "LeftFoot", s.left_foot);
        find_part(ch, "RightFoot", s.right_foot);
        if (s.is_r15) {
            find_part(ch, "LeftUpperArm", s.left_upper_arm);
            find_part(ch, "LeftLowerArm", s.left_lower_arm);
            find_part(ch, "RightUpperArm", s.right_upper_arm);
            find_part(ch, "RightLowerArm", s.right_lower_arm);
            find_part(ch, "LeftUpperLeg", s.left_upper_leg);
            find_part(ch, "LeftLowerLeg", s.left_lower_leg);
            find_part(ch, "RightUpperLeg", s.right_upper_leg);
            find_part(ch, "RightLowerLeg", s.right_lower_leg);
        }
        return s.head != 0 || s.upper_torso != 0;
    }

    static const char* body_parts[] = {
        "Head", "HumanoidRootPart", "UpperTorso", "LowerTorso",
        "LeftUpperArm", "LeftLowerArm", "LeftHand",
        "RightUpperArm", "RightLowerArm", "RightHand",
        "LeftUpperLeg", "LeftLowerLeg", "LeftFoot",
        "RightUpperLeg", "RightLowerLeg", "RightFoot",
        "Torso", "Left Arm", "Right Arm", "Left Leg", "Right Leg"
    };

    static bool is_body_part(const char* n) {
        for (auto bp : body_parts)
            if (strcmp(n, bp) == 0) return true;
        return false;
    }

    static uintptr_t get_player_team(instance player) {
        if (!player.is_valid()) return 0;
        return read<uintptr_t>(player.address + Offsets::Player::Team);
    }

    static std::vector<EspEntity> fetch_entities() {
        std::vector<EspEntity> result;
        if (!g_base_address) return result;
        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid() || dm.get_name().empty()) return result;
        instance plrs = dm.read_service("Players");
        if (!plrs.is_valid()) return result;
        instance local = plrs.local_player();

        uintptr_t local_team = 0;
        if (local.is_valid()) {
            local_team = get_player_team(local);
        }

        for (auto& p : plrs.get_children()) {
            if (!p.is_valid()) continue;
            if (local.is_valid() && p.address == local.address) continue;
            instance ch = p.model_instance();
            if (!ch.is_valid()) continue;

            EspEntity e{};
            e.player_address = p.address;
            scpy(e.name, p.get_name().c_str(), sizeof(e.name));
            e.user_id = read<uint32_t>(p.address + Offsets::Player::UserId);
            e.team_address = get_player_team(p);

            // fetch the child list ONCE and reuse it for humanoid, tool and parts
            // (this used to walk the same list three times per player)
            std::vector<instance> ch_children = ch.get_children();

            bool found_humanoid = false;
            bool found_tool = false;
            for (auto& c : ch_children) {
                if (!c.is_valid()) continue;
                if (found_humanoid && found_tool) break;
                auto cn = c.get_class_name();
                if (!found_humanoid && cn == "Humanoid") {
                    e.health = read<float>(c.address + Offsets::Humanoid::Health);
                    e.max_health = read<float>(c.address + Offsets::Humanoid::MaxHealth);
                    found_humanoid = true;
                } else if (!found_tool && (cn == "Tool" || cn == "BackpackItem")) {
                    scpy(e.tool_name, c.get_name().c_str(), sizeof(e.tool_name));
                    found_tool = true;
                }
            }
            e.is_r15 = is_r15(ch);
            e.character_address = ch.address;

            for (auto& pt : ch_children) {
                if (!pt.is_valid() || e.primitive_count >= 64) continue;
                auto pn = pt.get_name();
                if (!is_body_part(pn.c_str())) continue;
                uintptr_t pr = get_prim(pt);
                if (!is_valid_address(pr)) continue;
                size_t i = e.primitive_count;
                scpy(e.part_names[i], pn.c_str(), sizeof(e.part_names[i]));
                e.primitives[i] = pr;
                e.part_addresses[i] = pt.address;
                e.primitive_count++;
                if (pn == "HumanoidRootPart") {
                    vec3_t pos{};
                    if (read_raw(pr + Offsets::Primitive::Position, &pos, sizeof(pos))) {
                        e.root_x = pos.x; e.root_y = pos.y; e.root_z = pos.z;
                    }
                }
            }
            if (e.primitive_count > 0) result.push_back(e);
        }

        if (team_check && local_team != 0) {
            result.erase(
                std::remove_if(result.begin(), result.end(),
                    [local_team](const EspEntity& e) {
                        return e.team_address == local_team;
                    }),
                result.end());
        }

        return result;
    }

    static std::vector<SkeletonEntity> fetch_skeletons() {
        std::vector<SkeletonEntity> result;
        if (!g_base_address) return result;
        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid() || dm.get_name().empty()) return result;
        instance plrs = dm.read_service("Players");
        if (!plrs.is_valid()) return result;

        instance local = plrs.local_player();
        uintptr_t local_team = 0;
        if (team_check && local.is_valid()) {
            local_team = get_player_team(local);
        }

        for (auto& p : plrs.get_children()) {
            if (!p.is_valid()) continue;
            if (local.is_valid() && p.address == local.address) continue;
            instance ch = p.model_instance();
            if (!ch.is_valid()) continue;
            SkeletonEntity s{};
            s.player_address = p.address;
            s.team_address = get_player_team(p);
            if (build_skel(ch, s)) result.push_back(s);
        }

        if (team_check && local_team != 0) {
            result.erase(
                std::remove_if(result.begin(), result.end(),
                    [local_team](const SkeletonEntity& s) {
                        return s.team_address == local_team;
                    }),
                result.end());
        }

        return result;
    }

    static LocalPlayerData fetch_local() {
        LocalPlayerData lp{};
        if (!g_base_address) return lp;
        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid() || dm.get_name().empty()) return lp;
        instance plrs = dm.read_service("Players");
        if (!plrs.is_valid()) return lp;
        instance local = plrs.local_player();
        if (!local.is_valid()) return lp;
        instance ch = local.model_instance();
        if (!ch.is_valid()) return lp;
        lp.valid = true;
        // single pass over the character's children for both HRP and Humanoid
        std::vector<instance> parts = ch.get_children();
        for (auto& pt : parts) {
            if (!pt.is_valid()) continue;
            if (pt.get_name() == "HumanoidRootPart") {
                lp.hrp_primitive = get_prim(pt);
                break;
            }
        }
        if (is_valid_address(lp.hrp_primitive)) {
            vec3_t pos{};
            if (read_raw(lp.hrp_primitive + Offsets::Primitive::Position, &pos, sizeof(pos))) {
                lp.x = pos.x; lp.y = pos.y; lp.z = pos.z;
            }
        }
        for (auto& c : parts) {
            if (!c.is_valid()) continue;
            if (c.get_class_name() == "Humanoid") { lp.humanoid_address = c.address; break; }
        }

        // Prefer resolving the root part straight off the humanoid instead of by
        // name. Humanoid::HumanoidRootPart is a direct pointer, so this works even
        // if a name lookup fails - flight / infinite jump / teleport all depend on
        // this pointer being valid.
        if (is_valid_address(lp.humanoid_address)) {
            uintptr_t hrp_part = read<uintptr_t>(lp.humanoid_address + Offsets::Humanoid::HumanoidRootPart);
            if (is_valid_address(hrp_part)) {
                uintptr_t prim = read<uintptr_t>(hrp_part + Offsets::BasePart::Primitive);
                if (is_valid_address(prim)) {
                    lp.hrp_primitive = prim;
                    vec3_t pos{};
                    if (read_raw(prim + Offsets::Primitive::Position, &pos, sizeof(pos))) {
                        lp.x = pos.x; lp.y = pos.y; lp.z = pos.z;
                    }
                }
            }
        }
        return lp;
    }

    static DWORD WINAPI CacheThread(LPVOID) {
        while (s_running) {
            auto new_entities = fetch_entities();
            auto new_skeletons = fetch_skeletons();
            auto new_local = fetch_local();

            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_entities = std::move(new_entities);
                s_skeletons = std::move(new_skeletons);
                s_local = new_local;
            }
            Sleep(16);
        }
        return 0;
    }

    void StartThread() {
        s_running = true;
        s_thread = CreateThread(nullptr, 0, CacheThread, nullptr, 0, nullptr);
    }

    void StopThread() {
        s_running = false;
        if (s_thread) { WaitForSingleObject(s_thread, 2000); CloseHandle(s_thread); s_thread = nullptr; }
    }

    std::vector<EspEntity> GetEspEntities() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_entities;
    }

    std::vector<SkeletonEntity> GetSkeletonEntities() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_skeletons;
    }

    LocalPlayerData GetLocalPlayer() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_local;
    }

}