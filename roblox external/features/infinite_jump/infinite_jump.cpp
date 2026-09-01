#include <Windows.h>
#include "infinite_jump.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"

namespace features {

    // Setting Humanoid::Jump re-arms the jump regardless of whether the humanoid
    // thinks it's airborne, so holding space keeps launching you upward.
    void RunInfiniteJump() {
        if (!infinite_jump_enabled) return;

        // space is the game's own jump key, so we piggyback on it
        if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) return;

        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        if (!lp.valid || !is_valid_address(lp.humanoid_address)) return;

        write<bool>(lp.humanoid_address + Offsets::Humanoid::Jump, true);
    }
}
