#pragma once
#include "overlay.hpp"

namespace overlay {
    inline ID3D11Device* GetDevice() { return discord_overlay::g_device; }
    inline ID3D11DeviceContext* GetContext() { return discord_overlay::g_context; }
}