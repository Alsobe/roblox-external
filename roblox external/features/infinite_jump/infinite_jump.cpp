#include <Windows.h>
#include "infinite_jump.h"
#include "globals.h"
#include "memory.h"
#include "cache.h"
#include "offsets.h"

namespace features {

    // The Lua version calls humanoid:ChangeState(Jumping) on every JumpRequest.
    // We can't invoke methods from outside the process, so instead we do what
    // that state change ultimately does: apply an upward velocity impulse to the
    // root part. Fires on the rising edge of space so each tap = one jump.
    void RunInfiniteJump() {
        static bool was_down = false;

        if (!infinite_jump_enabled) { was_down = false; return; }

        bool down = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        if (!down) { was_down = false; return; }
        if (was_down) return;          // edge trigger
        was_down = true;

        const cache::LocalPlayerData& lp = cache::GetLocalPlayer();
        if (!lp.valid || !is_valid_address(lp.hrp_primitive)) return;

        // keep horizontal momentum, replace vertical with the jump impulse
        float vel[3] = {};
        read_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyLinearVelocity, vel, sizeof(vel));
        vel[1] = infinite_jump_power;
        write_raw(lp.hrp_primitive + Offsets::Primitive::AssemblyLinearVelocity, vel, sizeof(vel));

        // also re-arm the humanoid's own jump flag so animation/state stay in sync
        if (is_valid_address(lp.humanoid_address))
            write<bool>(lp.humanoid_address + Offsets::Humanoid::Jump, true);
    }
}
