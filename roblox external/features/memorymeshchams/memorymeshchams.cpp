#include "memorymeshchams.h"

#include "../../cache.h"
#include "../../game.h"
#include "../../globals.h"
#include "../../memory.h"
#include "../../offsets.h"
#include "../../overlay.h"
#include "../assetmesh/asset_mesh.h"
#include "../mesh_gpu/mesh_gpu.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace features {
namespace {

struct Vec2 { float x, y; };
struct Matrix4 { float data[16]; };
struct LiveMeshRecord { std::string url; assetmesh::parsed_mesh parsed; };
struct PartMeshBinding {
    std::shared_ptr<LiveMeshRecord> record;
    uint64_t gpu_cache_key = 0;
    DWORD last_refresh_tick = 0;
};
struct CapturedRecord {
    std::shared_ptr<LiveMeshRecord> record;
    std::vector<std::string> keys;
};

static constexpr uint64_t kVertexStride = 40;
static constexpr uint64_t kFaceStride = 12;
static constexpr uint32_t kVertexBatch = 25;
static constexpr uint32_t kFaceBatch = 85;
static constexpr DWORD kScanIntervalMs = 2000;
static constexpr DWORD kBindingRefreshMs = 0; // No caching - always refresh

static std::unordered_map<std::string, std::shared_ptr<LiveMeshRecord>> g_lookup;
static std::unordered_set<std::string> g_cached_urls;
static std::unordered_map<uintptr_t, PartMeshBinding> g_part_binding_cache;
static std::mutex g_cache_mutex;
static std::atomic<bool> g_scan_running = false;
static HANDLE g_scan_thread = nullptr;
static uint64_t g_next_gpu_cache_key = 1;

// Camera frame buffering to prevent jitter
static Matrix4 g_prev_view{};
static Vec2 g_prev_viewport{};
static bool g_has_prev_camera = false;

static bool read_safe(uint64_t addr, void* out, size_t size) {
    return is_valid_address((uintptr_t)addr) && read_raw(addr, out, size);
}

template <typename T>
static bool read_t(uint64_t addr, T& out) {
    return read_safe(addr, &out, sizeof(out));
}

static std::string trim_copy(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::string lower_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back((char)std::tolower((unsigned char)c));
    return out;
}

static uint64_t extract_asset_id(const std::string& text) {
    const std::string lower = lower_copy(text);
    const std::array<const char*, 4> markers = { "rbxassetid://", "rbxassetid:", "rbxasset://", "?id=" };
    for (const char* marker : markers) {
        const size_t pos = lower.find(marker);
        if (pos == std::string::npos) continue;
        const size_t start = pos + std::strlen(marker);
        size_t end = start;
        while (end < lower.size() && lower[end] >= '0' && lower[end] <= '9') ++end;
        if (end == start) continue;
        try { return std::stoull(lower.substr(start, end - start)); }
        catch (...) { return 0; }
    }
    return 0;
}

static std::vector<std::string> build_keys(const std::string& raw) {
    std::vector<std::string> keys;
    const std::string trimmed = trim_copy(raw);
    if (trimmed.empty()) return keys;
    keys.push_back(lower_copy(trimmed));
    const uint64_t asset_id = extract_asset_id(trimmed);
    if (asset_id != 0) keys.push_back("assetid:" + std::to_string(asset_id));
    return keys;
}

static const char* builtin_asset_alias(uint64_t asset_id) {
    switch (asset_id) {
    case 1365230: return "rbxasset://avatar/heads/head.mesh";
    case 1365219: return "rbxasset://avatar/meshes/torso.mesh";
    case 1365224: return "rbxasset://avatar/meshes/leftarm.mesh";
    case 1365223: return "rbxasset://avatar/meshes/rightarm.mesh";
    case 1365226: return "rbxasset://avatar/meshes/leftleg.mesh";
    case 1365225: return "rbxasset://avatar/meshes/rightleg.mesh";
    default: return nullptr;
    }
}

static std::string read_std_string(uint64_t base) {
    if (!is_valid_address((uintptr_t)base)) return "";
    uint64_t size = 0;
    if (!read_t(base + 0x18, size)) return "";
    uint64_t src = base;
    if (size >= 16 && !read_t(base, src)) return "";
    if (!is_valid_address((uintptr_t)src)) return "";
    const size_t len = (size_t)(std::min<uint64_t>)(size, 511);
    if (len == 0) return "";
    char buf[512] = {};
    if (!read_safe(src, buf, len)) return "";
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)buf[i];
        if (c == 0) break;
        if (c >= 32 && c < 127) out.push_back((char)c);
    }
    return out;
}

static void compute_bounds(assetmesh::parsed_mesh& mesh) {
    if (mesh.vertices.empty()) {
        mesh.bounds.valid = false;
        return;
    }
    mesh.bounds.min = { mesh.vertices[0].position.x, mesh.vertices[0].position.y, mesh.vertices[0].position.z };
    mesh.bounds.max = mesh.bounds.min;
    for (const auto& v : mesh.vertices) {
        mesh.bounds.min.x = (std::min)(mesh.bounds.min.x, v.position.x);
        mesh.bounds.min.y = (std::min)(mesh.bounds.min.y, v.position.y);
        mesh.bounds.min.z = (std::min)(mesh.bounds.min.z, v.position.z);
        mesh.bounds.max.x = (std::max)(mesh.bounds.max.x, v.position.x);
        mesh.bounds.max.y = (std::max)(mesh.bounds.max.y, v.position.y);
        mesh.bounds.max.z = (std::max)(mesh.bounds.max.z, v.position.z);
    }
    mesh.bounds.center = {
        (mesh.bounds.min.x + mesh.bounds.max.x) * 0.5f,
        (mesh.bounds.min.y + mesh.bounds.max.y) * 0.5f,
        (mesh.bounds.min.z + mesh.bounds.max.z) * 0.5f
    };
    mesh.bounds.size = {
        (std::max)(0.001f, mesh.bounds.max.x - mesh.bounds.min.x),
        (std::max)(0.001f, mesh.bounds.max.y - mesh.bounds.min.y),
        (std::max)(0.001f, mesh.bounds.max.z - mesh.bounds.min.z)
    };
    mesh.bounds.valid = true;
}

static bool read_vertices(uint64_t first, uint32_t count, std::vector<assetmesh::mesh_vertex>& out) {
    out.clear();
    out.reserve((size_t)count);
    uint64_t addr = first;
    uint32_t left = count;
    while (left > 0) {
        const uint32_t batch = (std::min)(kVertexBatch, left);
        std::vector<unsigned char> buf((size_t)(batch * kVertexStride));
        if (!read_safe(addr, buf.data(), buf.size())) return false;
        for (uint32_t j = 0; j < batch; ++j) {
            const size_t b = (size_t)(j * kVertexStride);
            assetmesh::mesh_vertex v{};
            std::memcpy(&v.position.x, buf.data() + b, sizeof(float));
            std::memcpy(&v.position.y, buf.data() + b + 4, sizeof(float));
            std::memcpy(&v.position.z, buf.data() + b + 8, sizeof(float));
            out.push_back(v);
        }
        left -= batch;
        addr += batch * kVertexStride;
    }
    return true;
}

static bool read_faces(uint64_t first, uint32_t count, std::vector<uint32_t>& out) {
    out.clear();
    out.reserve((size_t)count * 3);
    uint64_t addr = first;
    uint32_t left = count;
    while (left > 0) {
        const uint32_t batch = (std::min)(kFaceBatch, left);
        std::vector<unsigned char> buf((size_t)(batch * kFaceStride));
        if (!read_safe(addr, buf.data(), buf.size())) return false;
        for (uint32_t j = 0; j < batch; ++j) {
            const size_t b = (size_t)(j * kFaceStride);
            uint32_t a = 0, c = 0, d = 0;
            std::memcpy(&a, buf.data() + b, sizeof(uint32_t));
            std::memcpy(&c, buf.data() + b + 4, sizeof(uint32_t));
            std::memcpy(&d, buf.data() + b + 8, sizeof(uint32_t));
            out.push_back(a);
            out.push_back(c);
            out.push_back(d);
        }
        left -= batch;
        addr += batch * kFaceStride;
    }
    return true;
}

static uint32_t read_faces_max_index(uint64_t first, uint32_t count) {
    uint64_t addr = first;
    uint32_t left = count;
    uint32_t max_idx = 0;
    while (left > 0) {
        const uint32_t batch = (std::min)(kFaceBatch, left);
        std::vector<unsigned char> buf((size_t)(batch * kFaceStride));
        if (!read_safe(addr, buf.data(), buf.size())) return 0;
        for (uint32_t j = 0; j < batch; ++j) {
            const size_t b = (size_t)(j * kFaceStride);
            for (int k = 0; k < 3; ++k) {
                uint32_t idx = 0;
                std::memcpy(&idx, buf.data() + b + (k * 4), sizeof(uint32_t));
                if (idx > max_idx) max_idx = idx;
            }
        }
        left -= batch;
        addr += batch * kFaceStride;
    }
    return max_idx;
}

static uint32_t get_lod0_face_count(uint64_t fmd, uint32_t total) {
    for (uint64_t off = 0xA8; off <= 0xD0; off += 0x08) {
        uint64_t lf = 0, ll = 0;
        if (!read_t(fmd + off, lf) || !read_t(fmd + off + 0x08, ll) || ll <= lf) continue;
        const uint64_t lc = (ll - lf) / 4;
        if (lc < 2 || lc > 20) continue;
        uint32_t faces = 0;
        if (read_t(lf + 4, faces) && faces > 0 && faces <= total) return faces;
    }
    return total;
}

static std::vector<uint64_t> get_nodes(uint64_t cache_ptr) {
    std::vector<uint64_t> nodes;
    uint64_t sentinel = 0, node = 0;
    if (!read_t(cache_ptr + 0x08, sentinel) || !is_valid_address((uintptr_t)sentinel)) return nodes;
    if (!read_t(sentinel, node) || !is_valid_address((uintptr_t)node) || node == sentinel) {
        if (!read_t(sentinel + 0x08, node) || !is_valid_address((uintptr_t)node)) return nodes;
    }
    int walked = 0;
    while (is_valid_address((uintptr_t)node) && node != sentinel && walked < 10000) {
        nodes.push_back(node);
        ++walked;
        uint64_t next = 0;
        if (!read_t(node, next) || !is_valid_address((uintptr_t)next) || next == node) break;
        node = next;
    }
    return nodes;
}

static bool looks_like_cache_group(uint64_t base_ptr, uint64_t& evict, uint64_t& pinned) {
    uint64_t first = 0;
    if (!read_t(base_ptr, first)) return false;
    if ((first & 0xffffffffull) != 0x02000000ull || (first >> 32) != 0) return false;
    if (!read_t(base_ptr + 0x20, evict) || !read_t(base_ptr + 0x28, pinned)) return false;
    return is_valid_address((uintptr_t)evict) && is_valid_address((uintptr_t)pinned);
}

static bool find_cache(uint64_t& evict, uint64_t& pinned) {
    evict = 0;
    pinned = 0;
    instance dm = game::ReadDatamodel(g_base_address);
    if (!dm.is_valid()) return false;
    instance svc = dm.read_service("MeshContentProvider");
    if (!svc.is_valid()) return false;
    const std::array<uint64_t, 3> scan_bases = { read<uint64_t>(svc.address + 0x08), read<uint64_t>(svc.address), svc.address };
    for (uint64_t scan_base : scan_bases) {
        if (!is_valid_address((uintptr_t)scan_base)) continue;
        for (uint64_t off = 0; off <= 0x3000; off += 0x08) {
            uint64_t candidate = 0;
            if (!read_t(scan_base + off, candidate) || !is_valid_address((uintptr_t)candidate)) continue;
            uint64_t ce = 0, cp = 0;
            if (!looks_like_cache_group(candidate, ce, cp)) continue;
            if (get_nodes(ce).empty() && get_nodes(cp).empty()) continue;
            evict = ce;
            pinned = cp;
            return true;
        }
    }
    return false;
}

static bool capture_node(uint64_t node, std::unordered_set<std::string>& known_urls, std::vector<CapturedRecord>& out_records) {
    const std::string url = read_std_string(node + 0x10);
    if (url.empty() || known_urls.find(url) != known_urls.end()) return false;

    uint64_t ci = 0, fmd = 0, v_first = 0, v_last = 0, f_first = 0, f_last = 0;
    if (!read_t(node + 0x38, ci) || !read_t(ci + 0x28, fmd) || !read_t(fmd + 0x00, v_first) ||
        !read_t(fmd + 0x08, v_last) || !read_t(fmd + 0x30, f_first) || !read_t(fmd + 0x38, f_last)) {
        return false;
    }
    if (!is_valid_address((uintptr_t)v_first) || !is_valid_address((uintptr_t)f_first) || v_last <= v_first || f_last <= f_first) return false;

    const uint64_t v_diff = v_last - v_first;
    const uint64_t f_diff = f_last - f_first;
    if ((v_diff % kVertexStride) != 0 || (f_diff % kFaceStride) != 0) return false;

    const uint32_t total_verts = (uint32_t)(v_diff / kVertexStride);
    const uint32_t total_faces = (uint32_t)(f_diff / kFaceStride);
    if (total_verts == 0 || total_faces == 0 || total_verts > 500000 || total_faces > 500000) return false;

    const uint32_t lod0_faces = get_lod0_face_count(fmd, total_faces);
    const uint32_t vert_count = (std::min)(read_faces_max_index(f_first, lod0_faces) + 1, total_verts);
    if (vert_count == 0) return false;

    auto record = std::make_shared<LiveMeshRecord>();
    record->url = url;
    if (!read_vertices(v_first, vert_count, record->parsed.vertices)) return false;
    if (!read_faces(f_first, lod0_faces, record->parsed.indices)) return false;
    compute_bounds(record->parsed);
    if (!record->parsed.bounds.valid) return false;

    std::vector<std::string> keys = build_keys(url);
    if (keys.empty()) return false;

    known_urls.insert(url);
    out_records.push_back({ record, std::move(keys) });
    return true;
}

static void perform_scan_once() {
    uint64_t evict = 0, pinned = 0;
    if (!find_cache(evict, pinned)) {
        return;
    }

    std::unordered_set<std::string> known_urls;
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        known_urls = g_cached_urls;
    }

    std::vector<CapturedRecord> captured;
    const std::vector<uint64_t> evict_nodes = get_nodes(evict);
    const std::vector<uint64_t> pinned_nodes = get_nodes(pinned);
    captured.reserve(evict_nodes.size() + pinned_nodes.size());
    for (uint64_t node : evict_nodes) capture_node(node, known_urls, captured);
    for (uint64_t node : pinned_nodes) capture_node(node, known_urls, captured);

    if (captured.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    for (auto& entry : captured) {
        g_cached_urls.insert(entry.record->url);
        for (const std::string& key : entry.keys) {
            g_lookup[key] = entry.record;
        }
    }
}

static DWORD WINAPI scan_thread_main(LPVOID) {
    while (g_scan_running.load()) {
        perform_scan_once();
        for (int i = 0; i < (int)(kScanIntervalMs / 50) && g_scan_running.load(); ++i) {
            Sleep(50);
        }
    }
    return 0;
}

static bool get_camera(Matrix4& view, Vec2& viewport) {
    instance ve = read<instance>(g_base_address + Offsets::VisualEngine::Pointer);
    if (!ve.is_valid()) {
        if (g_has_prev_camera) {
            view = g_prev_view;
            viewport = g_prev_viewport;
            return true;
        }
        return false;
    }
    
    Matrix4 new_view{};
    Vec2 new_viewport{};
    
    if (!read_safe(ve.address + Offsets::VisualEngine::ViewMatrix, &new_view, sizeof(new_view))) {
        if (g_has_prev_camera) {
            view = g_prev_view;
            viewport = g_prev_viewport;
            return true;
        }
        return false;
    }
    
    if (!read_safe(ve.address + Offsets::VisualEngine::Dimensions, &new_viewport, sizeof(new_viewport))) {
        if (g_has_prev_camera) {
            view = g_prev_view;
            viewport = g_prev_viewport;
            return true;
        }
        return false;
    }
    
    if (new_viewport.x <= 0.0f || new_viewport.y <= 0.0f) {
        if (g_has_prev_camera) {
            view = g_prev_view;
            viewport = g_prev_viewport;
            return true;
        }
        return false;
    }
    
    // Validate matrix values to detect partial updates
    bool valid_matrix = true;
    for (int i = 0; i < 16; ++i) {
        const float val = new_view.data[i];
        if (!std::isfinite(val) || std::abs(val) > 1e6f) {
            valid_matrix = false;
            break;
        }
    }
    
    if (!valid_matrix) {
        if (g_has_prev_camera) {
            view = g_prev_view;
            viewport = g_prev_viewport;
            return true;
        }
        return false;
    }
    
    // Use new camera data directly - no caching, no smoothing
    g_prev_view = new_view;
    g_prev_viewport = new_viewport;
    g_has_prev_camera = true;
    
    view = new_view;
    viewport = new_viewport;
    
    return true;
}

static meshgpu::ResolvedMeshDraw resolve_live_mesh_gpu(const cache::EspEntity& entity, size_t part_index) {
    const uintptr_t part_address = entity.part_addresses[part_index];
    if (!part_address) {
        return {};
    }

    const DWORD now = GetTickCount();
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_part_binding_cache.find(part_address);
        if (it != g_part_binding_cache.end() && it->second.record &&
            (now - it->second.last_refresh_tick) < kBindingRefreshMs) {
            meshgpu::ResolvedMeshDraw resolved{};
            resolved.cache_key = it->second.gpu_cache_key;
            resolved.mesh = &it->second.record->parsed;
            return resolved;
        }
    }

    std::vector<std::string> keys;
    const std::string mesh_id = assetmesh::get_mesh_id_string_from_part(part_address);
    if (!mesh_id.empty()) {
        const auto mesh_keys = build_keys(mesh_id);
        keys.insert(keys.end(), mesh_keys.begin(), mesh_keys.end());
    }

    uint64_t asset_id = extract_asset_id(mesh_id);
    if (asset_id == 0 && entity.user_id != 0) {
        asset_id = assetmesh::get_mesh_asset_id_for_part(entity.user_id, entity.part_names[part_index], entity.is_r15);
        if (asset_id == 0) asset_id = assetmesh::get_default_asset_for_part(entity.part_names[part_index], entity.is_r15);
    }
    if (asset_id != 0) keys.push_back("assetid:" + std::to_string(asset_id));
    if (asset_id != 0) {
        const char* alias = builtin_asset_alias(asset_id);
        if (alias) keys.push_back(alias);
    }

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    PartMeshBinding& binding = g_part_binding_cache[part_address];
    if (binding.record && (now - binding.last_refresh_tick) < kBindingRefreshMs) {
        meshgpu::ResolvedMeshDraw resolved{};
        resolved.cache_key = binding.gpu_cache_key;
        resolved.mesh = &binding.record->parsed;
        return resolved;
    }

    for (const std::string& key : keys) {
        auto it = g_lookup.find(key);
        if (it == g_lookup.end()) continue;
        const bool record_changed = binding.record.get() != it->second.get();
        binding.record = it->second;
        binding.last_refresh_tick = now;
        if (binding.gpu_cache_key == 0 || record_changed) {
            binding.gpu_cache_key = reinterpret_cast<uint64_t>(binding.record.get());
            if (binding.gpu_cache_key == 0) {
                binding.gpu_cache_key = g_next_gpu_cache_key++;
            }
        }
        meshgpu::ResolvedMeshDraw resolved{};
        resolved.cache_key = binding.gpu_cache_key;
        resolved.mesh = &binding.record->parsed;
        return resolved;
    }

    if (binding.record) {
        binding.last_refresh_tick = now;
        if (binding.gpu_cache_key == 0) {
            binding.gpu_cache_key = reinterpret_cast<uint64_t>(binding.record.get());
            if (binding.gpu_cache_key == 0) {
                binding.gpu_cache_key = g_next_gpu_cache_key++;
            }
        }
        meshgpu::ResolvedMeshDraw resolved{};
        resolved.cache_key = binding.gpu_cache_key;
        resolved.mesh = &binding.record->parsed;
        return resolved;
    }

    return {};
}

} // namespace

void StartMemoryMeshChams() {
    bool expected = false;
    if (!g_scan_running.compare_exchange_strong(expected, true)) {
        return;
    }
    g_scan_thread = CreateThread(nullptr, 0, &scan_thread_main, nullptr, 0, nullptr);
    if (!g_scan_thread) {
        g_scan_running.store(false);
    }
}

void RenderMemoryMeshChams() {
    Matrix4 view{};
    Vec2 viewport{};
    if (!get_camera(view, viewport)) {
        return;
    }

    ID3D11Device* device = overlay::GetDevice();
    ID3D11DeviceContext* context = overlay::GetContext();
    if (!device || !context) {
        return;
    }

    auto entities_snap = cache::GetEspSnapshot();
    const auto& entities = *entities_snap;
    if (entities.empty()) {
        return;
    }

    if (memory_union_chams) {
        meshgpu::render_union_fill_with_resolver(
            entities,
            view.data,
            viewport.x,
            viewport.y,
            device,
            context,
            memory_mesh_chams_color,
            &resolve_live_mesh_gpu);
        if (memory_outline_chams) {
            meshgpu::render_union_outline_with_resolver(
                entities,
                view.data,
                viewport.x,
                viewport.y,
                device,
                context,
                memory_outline_chams_color,
                &resolve_live_mesh_gpu);
        }
        return;
    }

    meshgpu::render_with_resolver(
        entities,
        view.data,
        viewport.x,
        viewport.y,
        device,
        context,
        memory_mesh_chams_color,
        &resolve_live_mesh_gpu);

    if (memory_outline_chams) {
        meshgpu::render_wireframe_with_resolver(
            entities,
            view.data,
            viewport.x,
            viewport.y,
            device,
            context,
            memory_outline_chams_color,
            &resolve_live_mesh_gpu);
    }
}

void ShutdownMemoryMeshChams() {
    g_scan_running.store(false);
    if (g_scan_thread) {
        WaitForSingleObject(g_scan_thread, INFINITE);
        CloseHandle(g_scan_thread);
        g_scan_thread = nullptr;
    }

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_lookup.clear();
    g_cached_urls.clear();
    g_part_binding_cache.clear();
    g_next_gpu_cache_key = 1;
    g_has_prev_camera = false;
}

} // namespace features
