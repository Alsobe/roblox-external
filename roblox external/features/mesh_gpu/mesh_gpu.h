#pragma once

#include <vector>
#include <cstdint>
#include <d3d11.h>
#include "../cache.h"

namespace meshgpu {
    inline void render(const std::vector<cache::EspEntity>& /*entities*/, float* /*view_matrix*/, float /*vp_x*/, float /*vp_y*/,
                       ID3D11Device* /*device*/, ID3D11DeviceContext* /*ctx*/, const float* /*color*/) {
        // stub - needs real implementation
    }

    inline void shutdown() {
        // stub
    }
}