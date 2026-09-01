#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

namespace avatarmesh {

    struct vec3 { float x, y, z; };

    struct mesh_vertex {
        vec3 position;
        vec3 normal;
    };

    struct mesh_face {
        uint32_t a, b, c;
    };

    struct mesh_part {
        char name[64]{};
        vec3 center{};
        vec3 size{};
        std::vector<mesh_vertex> vertices;
        std::vector<mesh_face> faces;
    };

    struct avatar_mesh {
        uint32_t user_id = 0;
        bool is_r15 = false;
        bool valid = false;
        std::vector<mesh_part> parts;
    };

    inline void request_avatar_mesh(uint32_t user_id, bool is_r15) {}
    inline const avatar_mesh* get_avatar_mesh(uint32_t user_id, bool is_r15) { return nullptr; }
    inline const mesh_part* find_part(const avatar_mesh* mesh, const char* name, bool /*is_r15*/) {
        if (!mesh) return nullptr;
        for (auto& p : mesh->parts) {
            if (strcmp(p.name, name) == 0) return &p;
        }
        return nullptr;
    }
}