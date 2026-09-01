#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <mutex>
#include <memory>

namespace cache {
    static constexpr size_t k_esp_primitive_capacity = 64;

    struct EspEntity {
        uintptr_t primitives[64]{};
        char part_names[64][24]{};   // longest body part name is 16 chars
        uintptr_t part_addresses[64]{};
        size_t primitive_count = 0;
        uintptr_t character_address = 0;
        uintptr_t player_address = 0;
        uint32_t user_id = 0;
        uintptr_t team_address = 0;
        bool is_r15 = false;
        float root_x = 0, root_y = 0, root_z = 0;
        float health = 0, max_health = 0;
        char name[64]{};
        char display_name[64]{};
        char tool_name[64]{};
    };

    struct SkeletonEntity {
        uintptr_t head = 0;
        uintptr_t upper_torso = 0;
        uintptr_t lower_torso = 0;
        uintptr_t left_hand = 0, right_hand = 0;
        uintptr_t left_foot = 0, right_foot = 0;
        uintptr_t left_upper_arm = 0, left_lower_arm = 0;
        uintptr_t right_upper_arm = 0, right_lower_arm = 0;
        uintptr_t left_upper_leg = 0, left_lower_leg = 0;
        uintptr_t right_upper_leg = 0, right_lower_leg = 0;
        uintptr_t player_address = 0;
        uintptr_t team_address = 0;
        bool is_r15 = false;
    };

    struct LocalPlayerData {
        bool valid = false;
        float x = 0, y = 0, z = 0;
        uintptr_t hrp_primitive = 0;
        uintptr_t humanoid_address = 0;
    };

    // Zero-copy snapshots. The cache thread publishes a new immutable vector each
    // pass; readers hold a shared_ptr so the data stays alive while they use it.
    // (The old by-value getters deep-copied ~2.7KB per player on every call, from
    // 9 call sites per frame.)
    using EspSnapshot  = std::shared_ptr<const std::vector<EspEntity>>;
    using SkelSnapshot = std::shared_ptr<const std::vector<SkeletonEntity>>;

    EspSnapshot  GetEspSnapshot();
    SkelSnapshot GetSkeletonSnapshot();

    std::mutex& GetMutex();
    std::vector<EspEntity> GetEspEntities();
    std::vector<SkeletonEntity> GetSkeletonEntities();
    LocalPlayerData GetLocalPlayer();
    void StartThread();
    void StopThread();
}