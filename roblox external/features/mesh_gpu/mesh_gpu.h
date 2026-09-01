#pragma once

#include <vector>
#include <cstdint>
#include <d3d11.h>
#include "../cache.h"
#include "../assetmesh/asset_mesh.h"

namespace meshgpu {

    struct ResolvedMeshDraw {
        uint64_t cache_key = 0;
        const assetmesh::parsed_mesh* mesh = nullptr;
    };

    using MeshResolver = ResolvedMeshDraw(*)(const cache::EspEntity&, size_t);

    inline void render(const std::vector<cache::EspEntity>&, float*, float, float,
                       ID3D11Device*, ID3D11DeviceContext*, const float*) {}

    inline void render_with_resolver(const std::vector<cache::EspEntity>&, float*, float, float,
                                     ID3D11Device*, ID3D11DeviceContext*, const float*, MeshResolver) {}

    inline void render_union_fill_with_resolver(const std::vector<cache::EspEntity>&, float*, float, float,
                                                ID3D11Device*, ID3D11DeviceContext*, const float*, MeshResolver) {}

    inline void render_union_outline_with_resolver(const std::vector<cache::EspEntity>&, float*, float, float,
                                                   ID3D11Device*, ID3D11DeviceContext*, const float*, MeshResolver) {}

    inline void render_wireframe_with_resolver(const std::vector<cache::EspEntity>&, float*, float, float,
                                               ID3D11Device*, ID3D11DeviceContext*, const float*, MeshResolver) {}

    inline void shutdown() {}
}