#include <Windows.h>
#include <chrono>
#include "walkspeed.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"

namespace features {

    static float cached_walkspeed = 16.0f;
    static bool walkspeed_keybind_held = false;
    static std::chrono::steady_clock::time_point last_walkspeed_write = std::chrono::steady_clock::now();

    void RunWalkspeed() {
        if (walkspeed_enabled && walkspeed_keybind > 0) {
            bool key_down = (GetAsyncKeyState(walkspeed_keybind) & 0x8000) != 0;
            const cache::LocalPlayerData& lp = cache::GetLocalPlayer();

            if (key_down && lp.valid && is_valid_address(lp.humanoid_address)) {
                if (!walkspeed_keybind_held) {
                    cached_walkspeed = read<float>(lp.humanoid_address + Offsets::Humanoid::Walkspeed);
                    if (cached_walkspeed <= 0 || cached_walkspeed > 200) {
                        cached_walkspeed = 16.0f;
                    }
                    walkspeed_keybind_held = true;
                }

                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_walkspeed_write).count();
                if (elapsed >= 0) {
                    write<float>(lp.humanoid_address + Offsets::Humanoid::Walkspeed, walkspeed_value);
                    write<float>(lp.humanoid_address + Offsets::Humanoid::WalkspeedCheck, walkspeed_value);
                    last_walkspeed_write = now;
                }
            }
            else if (!key_down && walkspeed_keybind_held) {
                if (lp.valid && is_valid_address(lp.humanoid_address)) {
                    write<float>(lp.humanoid_address + Offsets::Humanoid::Walkspeed, cached_walkspeed);
                    write<float>(lp.humanoid_address + Offsets::Humanoid::WalkspeedCheck, cached_walkspeed);
                }
                walkspeed_keybind_held = false;
            }
        }
        else {
            if (walkspeed_keybind_held) {
                const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
                if (lp.valid && is_valid_address(lp.humanoid_address)) {
                    write<float>(lp.humanoid_address + Offsets::Humanoid::Walkspeed, cached_walkspeed);
                    write<float>(lp.humanoid_address + Offsets::Humanoid::WalkspeedCheck, cached_walkspeed);
                }
                walkspeed_keybind_held = false;
            }
        }
    }
}

