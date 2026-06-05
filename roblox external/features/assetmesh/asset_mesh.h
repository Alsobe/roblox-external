#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <string>

// stub - roblox asset mesh fetching for mesh chams

namespace assetmesh {

    struct vec3 { float x, y, z; };

    struct vertex {
        vec3 position;
        vec3 normal;
    };

    struct mesh_bounds {
        vec3 center;
        vec3 size;
    };

    struct parsed_mesh {
        std::vector<vertex> vertices;
        mesh_bounds bounds{};
    };

    struct debug_stats {
        size_t cached_meshes = 0;
        size_t pending_requests = 0;
    };

    inline uint64_t get_mesh_asset_id_from_part(uintptr_t part_address) { return 0; }
    inline uint64_t get_mesh_asset_id_for_part(uint32_t user_id, const char* part_name, bool is_r15) { return 0; }
    inline uint64_t get_default_asset_for_part(const char* part_name, bool is_r15) { return 0; }
    inline void request_avatar_assets(uint32_t user_id) {}
    inline void request_mesh(uint64_t asset_id) {}
    inline std::shared_ptr<const parsed_mesh> get_mesh(uint64_t asset_id) { return nullptr; }
    inline debug_stats get_debug_stats() { return {}; }
}