#include <Windows.h>
#include <unordered_map>
#include "noclip.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"
#include "game.h"

namespace features {

    static std::unordered_map<uintptr_t, uint8_t> s_original_flags;
    static uintptr_t s_last_character = 0;

    static void RestoreCollision() {
        if (s_original_flags.empty()) return;
        if (!g_base_address) return;

        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid()) return;
        instance players = dm.read_service("Players");
        if (!players.is_valid()) return;
        instance local = players.local_player();
        if (!local.is_valid()) return;
        instance character = local.model_instance();
        if (!character.is_valid()) return;

        for (const instance& child : character.get_children()) {
            if (!child.is_valid()) continue;
            uintptr_t prim = read<uintptr_t>(child.address + Offsets::BasePart::Primitive);
            if (!is_valid_address(prim)) continue;
            auto it = s_original_flags.find(child.address);
            if (it != s_original_flags.end()) {
                write<uint8_t>(prim + Offsets::Primitive::Flags, it->second);
            }
        }
        s_original_flags.clear();
        s_last_character = 0;
    }

    void RunNoclip() {
        if (!noclip_enabled) {
            RestoreCollision();
            return;
        }

        bool key_held = noclip_keybind > 0 && (GetAsyncKeyState(noclip_keybind) & 0x8000) != 0;

        if (!key_held) {
            RestoreCollision();
            return;
        }

        if (!g_base_address) return;
        instance dm = game::ReadDatamodel(g_base_address);
        if (!dm.is_valid()) return;
        instance players = dm.read_service("Players");
        if (!players.is_valid()) return;
        instance local = players.local_player();
        if (!local.is_valid()) return;
        instance character = local.model_instance();
        if (!character.is_valid()) return;

        if (character.address != s_last_character) {
            s_original_flags.clear();
            s_last_character = character.address;
        }

        for (const instance& child : character.get_children()) {
            if (!child.is_valid()) continue;
            uintptr_t prim = read<uintptr_t>(child.address + Offsets::BasePart::Primitive);
            if (!is_valid_address(prim)) continue;

            if (s_original_flags.find(child.address) == s_original_flags.end()) {
                s_original_flags[child.address] = read<uint8_t>(prim + Offsets::Primitive::Flags);
            }

            uint8_t flags = read<uint8_t>(prim + Offsets::Primitive::Flags);
            flags &= ~static_cast<uint8_t>(Offsets::PrimitiveFlags::CanCollide);
            write<uint8_t>(prim + Offsets::Primitive::Flags, flags);
        }
    }
}
