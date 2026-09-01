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

    // implemented in mesh_gpu.cpp (now part of the build)
    size_t render(const std::vector<cache::EspEntity>& entities, const float view[16],
                  float viewport_width, float viewport_height,
                  ID3D11Device* device, ID3D11DeviceContext* context, const float color[4]);

    size_t render_with_resolver(const std::vector<cache::EspEntity>& entities, const float view[16],
                                float viewport_width, float viewport_height,
                                ID3D11Device* device, ID3D11DeviceContext* context,
                                const float color[4], MeshResolver resolver);

    size_t render_union_fill_with_resolver(const std::vector<cache::EspEntity>& entities, const float view[16],
                                           float viewport_width, float viewport_height,
                                           ID3D11Device* device, ID3D11DeviceContext* context,
                                           const float color[4], MeshResolver resolver);

    size_t render_union_outline_with_resolver(const std::vector<cache::EspEntity>& entities, const float view[16],
                                              float viewport_width, float viewport_height,
                                              ID3D11Device* device, ID3D11DeviceContext* context,
                                              const float color[4], MeshResolver resolver);

    size_t render_wireframe_with_resolver(const std::vector<cache::EspEntity>& entities, const float view[16],
                                          float viewport_width, float viewport_height,
                                          ID3D11Device* device, ID3D11DeviceContext* context,
                                          const float color[4], MeshResolver resolver);

    size_t get_last_drawn_entities();
    void shutdown();
}