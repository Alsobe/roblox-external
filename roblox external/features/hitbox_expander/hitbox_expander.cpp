#include <Windows.h>
#include <chrono>
#include <cstdio>
#include "hitbox_expander.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "game.h"

namespace features {

    static std::chrono::steady_clock::time_point last_hitbox_write = std::chrono::steady_clock::now();
    static constexpr long long k_write_interval_ms = 2000;

    void RunHitboxExpander() {
        if (!hitbox_expander_enabled) return;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hitbox_write).count();
        if (elapsed < k_write_interval_ms) return;
        last_hitbox_write = now;

        if (!g_base_address) return;

        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid()) return;

        instance players_service = dm.read_service("Players");
        if (!players_service.is_valid()) return;

        instance local = players_service.local_player();
        int expanded_count = 0;

        for (const instance& player : players_service.get_children()) {
            if (!player.is_valid()) continue;
            if (!is_valid_address(player.address)) continue;
            if (local.is_valid() && player.address == local.address) continue;
            if (player.get_class_name() != "Player") continue;

            instance character = player.model_instance();
            if (!character.is_valid()) continue;
            if (!is_valid_address(character.address)) continue;

            for (const instance& child : character.get_children()) {
                if (!child.is_valid()) continue;
                if (!is_valid_address(child.address)) continue;
                if (child.get_name() != "HumanoidRootPart") continue;

                uintptr_t part_addr = child.address;

                uintptr_t prim = read<uintptr_t>(part_addr + Offsets::BasePart::Primitive);
                if (!is_valid_address(prim)) break;

                float size_val = hitbox_expander_value;
                write<float>(prim + Offsets::Primitive::Size, size_val);
                write<float>(prim + Offsets::Primitive::Size + 4, size_val);
                write<float>(prim + Offsets::Primitive::Size + 8, size_val);

                uint8_t current_flags = read<uint8_t>(prim + Offsets::Primitive::Flags);
                uint8_t new_flags = current_flags & ~static_cast<uint8_t>(Offsets::PrimitiveFlags::CanCollide);
                write<uint8_t>(prim + Offsets::Primitive::Flags, new_flags);

                expanded_count++;
                break;
            }
        }
    }
}

