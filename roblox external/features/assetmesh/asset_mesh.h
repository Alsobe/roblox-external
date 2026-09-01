#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <string>

namespace assetmesh {

    struct vec3 { float x, y, z; };

    struct mesh_vertex {
        vec3 position;
        vec3 normal;
    };

    struct mesh_bounds {
        vec3 min{};
        vec3 max{};
        vec3 center{};
        vec3 size{};
        bool valid = false;
    };

    struct parsed_mesh {
        std::vector<mesh_vertex> vertices;
        std::vector<uint32_t> indices;
        mesh_bounds bounds{};
    };

    struct debug_stats {
        size_t cached_meshes = 0;
        size_t pending_requests = 0;
    };

    // implemented in asset_mesh.cpp (now part of the build)
    uint64_t get_mesh_asset_id_from_part(uintptr_t part_address);
    uint64_t get_mesh_asset_id_for_part(int64_t user_id, const char* part_name, bool is_r15);
    uint64_t get_default_asset_for_part(const char* part_name, bool is_r15);
    std::string get_mesh_id_string_from_part(uintptr_t part_address);
    void request_avatar_assets(int64_t user_id);
    void request_mesh(uint64_t asset_id);
    std::shared_ptr<const parsed_mesh> get_mesh(uint64_t asset_id);
    debug_stats get_debug_stats();
}