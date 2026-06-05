#include "asset_mesh.h"
#include "../../memory.h"
#include "../../offsets.h"
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")

namespace assetmesh {

    static std::unordered_map<uint64_t, std::shared_ptr<parsed_mesh>> g_mesh_cache;
    static std::unordered_set<uint64_t> g_pending_assets;
    static std::unordered_map<uint64_t, ULONGLONG> g_retry_after;
    static std::queue<uint64_t> g_request_queue;
    static std::mutex g_state_mutex;
    static std::condition_variable g_queue_cv;
    static std::vector<HANDLE> g_workers;
    static std::atomic<bool> g_running{ false };
    static std::atomic<size_t> g_fetch_ok{ 0 };
    static std::atomic<size_t> g_fetch_fail{ 0 };
    static std::atomic<size_t> g_fail_api{ 0 };
    static std::atomic<size_t> g_fail_download{ 0 };
    static std::atomic<size_t> g_fail_parse{ 0 };
    static char g_last_error[128] = {};
    static std::atomic<int> g_last_http_status{ 0 };
    static std::atomic<uint64_t> g_last_failed_asset_id{ 0 };
    static std::atomic<ULONGLONG> g_last_request_tick{ 0 };
    static constexpr size_t THREAD_POOL_SIZE = 4;
    static constexpr DWORD REQUEST_DELAY_MS = 120;

    static std::unordered_map<int64_t, std::unordered_map<std::string, uint64_t>> g_avatar_asset_cache;
    static std::unordered_set<int64_t> g_avatar_pending;
    static std::unordered_map<int64_t, ULONGLONG> g_avatar_retry_after;
    static std::mutex g_avatar_mutex;

    static void set_error(const char* msg) {
        strncpy_s(g_last_error, msg, sizeof(g_last_error) - 1);
        g_last_error[sizeof(g_last_error) - 1] = '\0';
    }

    static float read_float_le(const uint8_t* data, size_t& offset) {
        float value{};
        std::memcpy(&value, data + offset, sizeof(float));
        offset += sizeof(float);
        return value;
    }

    static uint32_t read_uint32_le(const uint8_t* data, size_t& offset) {
        uint32_t value{};
        std::memcpy(&value, data + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        return value;
    }

    static uint16_t read_uint16_le(const uint8_t* data, size_t& offset) {
        uint16_t value{};
        std::memcpy(&value, data + offset, sizeof(uint16_t));
        offset += sizeof(uint16_t);
        return value;
    }

    static uint8_t read_uint8(const uint8_t* data, size_t& offset) {
        return data[offset++];
    }

    static int8_t read_int8(const uint8_t* data, size_t& offset) {
        return static_cast<int8_t>(data[offset++]);
    }

    static bool download_from_url(const std::wstring& host, const std::wstring& path, std::vector<uint8_t>& data, int* out_status = nullptr) {
        HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return false;

        DWORD timeout = 15000;
        WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            return false;
        }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
        LPCWSTR headers = L"Accept: */*\r\nAccept-Encoding: gzip, deflate\r\nReferer: https://www.roblox.com/\r\n";
        WinHttpAddRequestHeaders(hRequest, headers, (DWORD)wcslen(headers), WINHTTP_ADDREQ_FLAG_ADD);

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }
        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        DWORD status = 0;
        DWORD status_len = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &status_len, nullptr);
        if (out_status) *out_status = (int)status;
        g_last_http_status.store((int)status);
        if (status != 200) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        data.clear();
        DWORD avail = 0;
        do {
            if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
            size_t old_size = data.size();
            data.resize(old_size + avail);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, data.data() + old_size, avail, &read)) {
                data.resize(old_size);
                break;
            }
            if (read < avail) data.resize(old_size + read);
        } while (true);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return !data.empty();
    }

    static bool fetch_avatar_json(int64_t user_id, std::string& out_json) {
        std::wstring path = L"/v1/users/" + std::to_wstring(user_id) + L"/avatar";
        std::vector<uint8_t> data;
        int status = 0;
        if (!download_from_url(L"avatar.roblox.com", path, data, &status)) return false;
        out_json.assign(reinterpret_cast<const char*>(data.data()), data.size());
        return true;
    }

    static void parse_avatar_assets(const std::string& json, std::unordered_map<std::string, uint64_t>& out_map) {
        out_map.clear();
        size_t pos = 0;
        while ((pos = json.find("\"assetType\"", pos)) != std::string::npos) {
            size_t name_start = json.find("\"name\"", pos);
            if (name_start == std::string::npos || name_start > pos + 50) { pos++; continue; }
            size_t colon = json.find(':', name_start);
            if (colon == std::string::npos) { pos++; continue; }
            size_t q = json.find('"', colon);
            if (q == std::string::npos) { pos++; continue; }
            size_t name_end = json.find('"', q + 1);
            if (name_end == std::string::npos) { pos++; continue; }
            std::string type_name = json.substr(q + 1, name_end - q - 1);
            size_t id_pos = json.rfind("\"id\"", pos);
            if (id_pos == std::string::npos) { pos++; continue; }
            size_t id_colon = json.find(':', id_pos);
            if (id_colon == std::string::npos) { pos++; continue; }
            try {
                size_t num_end = id_colon + 1;
                while (num_end < json.size() && (json[num_end] == ' ' || json[num_end] == '\t')) num_end++;
                size_t num_start = num_end;
                while (num_end < json.size() && json[num_end] >= '0' && json[num_end] <= '9') num_end++;
                if (num_end > num_start) {
                    uint64_t asset_id = std::stoull(json.substr(num_start, num_end - num_start));
                    out_map[type_name] = asset_id;
                }
            }
            catch (...) {}
            pos = name_end + 1;
        }
    }

    static DWORD WINAPI avatar_fetch_thread(LPVOID param) {
        int64_t uid = static_cast<int64_t>(reinterpret_cast<uintptr_t>(param));
        try {
            std::string json;
            if (!fetch_avatar_json(uid, json)) {
                std::lock_guard<std::mutex> lock(g_avatar_mutex);
                g_avatar_pending.erase(uid);
                g_avatar_retry_after[uid] = GetTickCount64() + 15000;
                return 0;
            }

            std::unordered_map<std::string, uint64_t> asset_map;
            parse_avatar_assets(json, asset_map);
            {
                std::lock_guard<std::mutex> lock(g_avatar_mutex);
                g_avatar_pending.erase(uid);
                g_avatar_asset_cache[uid] = asset_map;
            }
            for (const auto& kv : asset_map) {
                if (kv.second != 0) request_mesh(kv.second);
            }
            return 0;
        } catch (...) {
            std::lock_guard<std::mutex> lock(g_avatar_mutex);
            g_avatar_pending.erase(uid);
            g_avatar_retry_after[uid] = GetTickCount64() + 15000;
        }
        return 0;
    }

    static uint64_t get_default_asset_for_part_impl(const char* part_name, bool is_r15) {
        if (is_r15) {
            if (strcmp(part_name, "Head") == 0) return 7430070993;
            if (strcmp(part_name, "UpperTorso") == 0) return 7430071038;
            if (strcmp(part_name, "LowerTorso") == 0) return 7430071109;
            if (strcmp(part_name, "LeftUpperArm") == 0) return 7430071044;
            if (strcmp(part_name, "RightUpperArm") == 0) return 7430071041;
            if (strcmp(part_name, "LeftLowerArm") == 0) return 7430071005;
            if (strcmp(part_name, "RightLowerArm") == 0) return 7430071013;
            if (strcmp(part_name, "LeftHand") == 0) return 7430070991;
            if (strcmp(part_name, "RightHand") == 0) return 7430070997;
            if (strcmp(part_name, "LeftUpperLeg") == 0) return 7430071065;
            if (strcmp(part_name, "RightUpperLeg") == 0) return 7430071119;
            if (strcmp(part_name, "LeftLowerLeg") == 0) return 7430071049;
            if (strcmp(part_name, "RightLowerLeg") == 0) return 7430071105;
            if (strcmp(part_name, "LeftFoot") == 0) return 7430071039;
            if (strcmp(part_name, "RightFoot") == 0) return 7430071082;
        }
        else {
            if (strcmp(part_name, "Head") == 0) return 1365230;
            if (strcmp(part_name, "Torso") == 0) return 1365219;
            if (strcmp(part_name, "Left Arm") == 0) return 1365224;
            if (strcmp(part_name, "Right Arm") == 0) return 1365223;
            if (strcmp(part_name, "Left Leg") == 0) return 1365226;
            if (strcmp(part_name, "Right Leg") == 0) return 1365225;
        }
        return 0;
    }

    static const char* part_name_to_asset_type(const char* part_name) {
        if (strcmp(part_name, "Head") == 0) return "DynamicHead";
        if (strcmp(part_name, "Torso") == 0) return "Torso";
        if (strcmp(part_name, "UpperTorso") == 0) return "Torso";
        if (strcmp(part_name, "LowerTorso") == 0) return "Torso";
        if (strcmp(part_name, "Left Arm") == 0) return "LeftArm";
        if (strcmp(part_name, "Right Arm") == 0) return "RightArm";
        if (strcmp(part_name, "Left Leg") == 0) return "LeftLeg";
        if (strcmp(part_name, "Right Leg") == 0) return "RightLeg";
        if (strcmp(part_name, "LeftUpperArm") == 0) return "LeftArm";
        if (strcmp(part_name, "RightUpperArm") == 0) return "RightArm";
        if (strcmp(part_name, "LeftLowerArm") == 0) return "LeftArm";
        if (strcmp(part_name, "RightLowerArm") == 0) return "RightArm";
        if (strcmp(part_name, "LeftHand") == 0) return "LeftArm";
        if (strcmp(part_name, "RightHand") == 0) return "RightArm";
        if (strcmp(part_name, "LeftUpperLeg") == 0) return "LeftLeg";
        if (strcmp(part_name, "RightUpperLeg") == 0) return "RightLeg";
        if (strcmp(part_name, "LeftLowerLeg") == 0) return "LeftLeg";
        if (strcmp(part_name, "RightLowerLeg") == 0) return "RightLeg";
        if (strcmp(part_name, "LeftFoot") == 0) return "LeftLeg";
        if (strcmp(part_name, "RightFoot") == 0) return "RightLeg";
        return nullptr;
    }

    static std::string extract_json_string(const std::string& json, const char* key) {
        std::string search = "\"";
        search += key;
        search += "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos);
        if (pos == std::string::npos) return "";
        pos = json.find('"', pos);
        if (pos == std::string::npos) return "";
        size_t start = pos + 1;
        size_t end = json.find('"', start);
        if (end == std::string::npos) return "";
        return json.substr(start, end - start);
    }

    static bool split_url(const std::string& url, std::wstring& host, std::wstring& path) {
        size_t proto = url.find("://");
        if (proto == std::string::npos) return false;
        std::string rest = url.substr(proto + 3);
        size_t path_pos = rest.find('/');
        if (path_pos == std::string::npos) return false;
        std::string host_str = rest.substr(0, path_pos);
        std::string path_str = rest.substr(path_pos);
        if (host_str.empty() || path_str.empty()) return false;
        host.assign(host_str.begin(), host_str.end());
        path.assign(path_str.begin(), path_str.end());
        return true;
    }

    static uint64_t extract_asset_id_from_text(const std::string& text) {
        size_t id_pos = text.find("rbxassetid://");
        if (id_pos != std::string::npos) {
            size_t start = id_pos + 13;
            size_t end = start;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') end++;
            if (end > start) {
                try { return std::stoull(text.substr(start, end - start)); }
                catch (...) { return 0; }
            }
        }
        id_pos = text.find("?id=");
        if (id_pos != std::string::npos) {
            size_t start = id_pos + 4;
            size_t end = start;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') end++;
            if (end > start) {
                try { return std::stoull(text.substr(start, end - start)); }
                catch (...) { return 0; }
            }
        }
        return 0;
    }

    static std::string get_mesh_version(const std::vector<uint8_t>& data) {
        if (data.size() < 12) return "";
        std::string version(reinterpret_cast<const char*>(data.data()), 12);
        if (version.rfind("version ", 0) != 0) return "";
        version = version.substr(8);
        while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' ')) version.pop_back();
        return version;
    }

    static void compute_mesh_bounds(parsed_mesh& mesh) {
        if (mesh.vertices.empty()) {
            mesh.bounds.valid = false;
            return;
        }
        mesh.bounds.min = { FLT_MAX, FLT_MAX, FLT_MAX };
        mesh.bounds.max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (const auto& vertex : mesh.vertices) {
            mesh.bounds.min.x = (std::min)(mesh.bounds.min.x, vertex.position.x);
            mesh.bounds.min.y = (std::min)(mesh.bounds.min.y, vertex.position.y);
            mesh.bounds.min.z = (std::min)(mesh.bounds.min.z, vertex.position.z);
            mesh.bounds.max.x = (std::max)(mesh.bounds.max.x, vertex.position.x);
            mesh.bounds.max.y = (std::max)(mesh.bounds.max.y, vertex.position.y);
            mesh.bounds.max.z = (std::max)(mesh.bounds.max.z, vertex.position.z);
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

    static bool parse_mesh_v1(const std::vector<uint8_t>& data, size_t offset, parsed_mesh& mesh, float scale, bool invert_uv) {
        std::string data_str(reinterpret_cast<const char*>(data.data() + offset), data.size() - offset);
        size_t newline_pos = data_str.find('\n');
        if (newline_pos == std::string::npos) return false;
        uint32_t num_faces = 0;
        try {
            num_faces = static_cast<uint32_t>(std::stoul(data_str.substr(0, newline_pos)));
        }
        catch (...) {
            return false;
        }
        size_t start_pos = data_str.find('[');
        if (start_pos == std::string::npos) return false;

        std::vector<std::vector<float>> all_vectors;
        all_vectors.reserve(num_faces * 9);
        std::string remaining = data_str.substr(start_pos);
        size_t pos = 0;
        while (pos < remaining.size()) {
            size_t open = remaining.find('[', pos);
            if (open == std::string::npos) break;
            size_t close = remaining.find(']', open);
            if (close == std::string::npos) break;
            std::string vec_str = remaining.substr(open + 1, close - open - 1);
            std::vector<float> vec;
            std::istringstream iss(vec_str);
            std::string token;
            while (std::getline(iss, token, ',')) {
                try {
                    vec.push_back(std::stof(token));
                }
                catch (...) {
                }
            }
            if (vec.size() == 3) all_vectors.push_back(std::move(vec));
            pos = close + 1;
        }
        if (all_vectors.size() != num_faces * 9) return false;

        mesh.vertices.clear();
        mesh.indices.clear();
        mesh.vertices.reserve(num_faces * 3);
        mesh.indices.reserve(num_faces * 3);
        for (size_t i = 0; i < all_vectors.size(); i += 3) {
            if (i + 2 >= all_vectors.size()) return false;
            mesh_vertex vertex{};
            vertex.position = {
                all_vectors[i][0] * scale,
                all_vectors[i][1] * scale,
                all_vectors[i][2] * scale
            };
            if (invert_uv) {
                (void)invert_uv;
            }
            mesh.vertices.push_back(vertex);
        }
        for (uint32_t i = 0; i < num_faces; ++i) {
            mesh.indices.push_back(i * 3);
            mesh.indices.push_back(i * 3 + 1);
            mesh.indices.push_back(i * 3 + 2);
        }
        return true;
    }

    static bool parse_mesh_v2(const std::vector<uint8_t>& data, size_t offset, parsed_mesh& mesh) {
        size_t pos = offset;
        if (pos + 12 > data.size()) return false;
        uint16_t cb_size = read_uint16_le(data.data(), pos);
        if (cb_size != 12) return false;
        uint8_t cb_vertices_stride = read_uint8(data.data(), pos);
        uint8_t cb_face_stride = read_uint8(data.data(), pos);
        uint32_t num_vertices = read_uint32_le(data.data(), pos);
        uint32_t num_faces = read_uint32_le(data.data(), pos);
        if (num_vertices == 0 || num_faces == 0) return false;
        bool has_rgba = (cb_vertices_stride == 40);
        size_t vertex_size = cb_vertices_stride;
        if (vertex_size < 36) return false;

        mesh.vertices.clear();
        mesh.indices.clear();
        mesh.vertices.reserve(num_vertices);
        mesh.indices.reserve(num_faces * 3);
        for (uint32_t i = 0; i < num_vertices; ++i) {
            if (pos + vertex_size > data.size()) return false;
            mesh_vertex vertex{};
            vertex.position.x = read_float_le(data.data(), pos);
            vertex.position.y = read_float_le(data.data(), pos);
            vertex.position.z = read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            if (has_rgba) {
                read_uint8(data.data(), pos);
                read_uint8(data.data(), pos);
                read_uint8(data.data(), pos);
                read_uint8(data.data(), pos);
            }
            if (vertex_size > (has_rgba ? 40 : 36)) pos += (vertex_size - (has_rgba ? 40 : 36));
            mesh.vertices.push_back(vertex);
        }
        for (uint32_t i = 0; i < num_faces; ++i) {
            if (cb_face_stride == 6) {
                if (pos + 6 > data.size()) return false;
                mesh.indices.push_back(read_uint16_le(data.data(), pos));
                mesh.indices.push_back(read_uint16_le(data.data(), pos));
                mesh.indices.push_back(read_uint16_le(data.data(), pos));
            }
            else {
                if (pos + 12 > data.size()) return false;
                mesh.indices.push_back(read_uint32_le(data.data(), pos));
                mesh.indices.push_back(read_uint32_le(data.data(), pos));
                mesh.indices.push_back(read_uint32_le(data.data(), pos));
            }
        }
        return true;
    }

    static bool parse_mesh_v3(const std::vector<uint8_t>& data, size_t offset, parsed_mesh& mesh) {
        size_t pos = offset;
        if (pos + 16 > data.size()) return false;
        uint16_t cb_size = read_uint16_le(data.data(), pos);
        if (cb_size != 16) return false;
        uint8_t cb_vertices_stride = read_uint8(data.data(), pos);
        uint8_t cb_face_stride = read_uint8(data.data(), pos);
        uint16_t sizeof_lod = read_uint16_le(data.data(), pos);
        uint16_t num_lods = read_uint16_le(data.data(), pos);
        uint32_t num_vertices = read_uint32_le(data.data(), pos);
        uint32_t num_faces = read_uint32_le(data.data(), pos);
        (void)sizeof_lod;
        if (num_vertices == 0 || num_faces == 0) return false;
        size_t vertex_size = cb_vertices_stride;
        if (vertex_size < 40) return false;

        mesh.vertices.clear();
        mesh.indices.clear();
        mesh.vertices.reserve(num_vertices);
        mesh.indices.reserve(num_faces * 3);
        for (uint32_t i = 0; i < num_vertices; ++i) {
            if (pos + vertex_size > data.size()) return false;
            mesh_vertex vertex{};
            vertex.position.x = read_float_le(data.data(), pos);
            vertex.position.y = read_float_le(data.data(), pos);
            vertex.position.z = read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            read_uint8(data.data(), pos);
            read_uint8(data.data(), pos);
            read_uint8(data.data(), pos);
            read_uint8(data.data(), pos);
            if (vertex_size > 40) pos += (vertex_size - 40);
            mesh.vertices.push_back(vertex);
        }
        size_t face_start = pos;
        for (uint32_t i = 0; i < num_faces; ++i) {
            if (cb_face_stride == 6) {
                if (pos + 6 > data.size()) return false;
                mesh.indices.push_back(read_uint16_le(data.data(), pos));
                mesh.indices.push_back(read_uint16_le(data.data(), pos));
                mesh.indices.push_back(read_uint16_le(data.data(), pos));
            }
            else {
                if (pos + 12 > data.size()) return false;
                mesh.indices.push_back(read_uint32_le(data.data(), pos));
                mesh.indices.push_back(read_uint32_le(data.data(), pos));
                mesh.indices.push_back(read_uint32_le(data.data(), pos));
            }
        }
        std::vector<uint32_t> lods;
        lods.reserve(num_lods);
        for (uint16_t i = 0; i < num_lods; ++i) {
            if (pos + 4 > data.size()) break;
            lods.push_back(read_uint32_le(data.data(), pos));
        }
        if (lods.size() > 1 && lods[1] * 3 < mesh.indices.size()) {
            mesh.indices.resize(lods[1] * 3);
        }
        else if (pos == face_start) {
            return false;
        }
        return true;
    }

    static bool parse_mesh_v4(const std::vector<uint8_t>& data, size_t offset, parsed_mesh& mesh) {
        size_t pos = offset;
        if (pos + 24 > data.size()) return false;
        uint16_t header_size = read_uint16_le(data.data(), pos);
        if (header_size != 24) return false;
        uint16_t lod_type = read_uint16_le(data.data(), pos);
        uint32_t num_vertices = read_uint32_le(data.data(), pos);
        uint32_t num_faces = read_uint32_le(data.data(), pos);
        uint16_t num_lods = read_uint16_le(data.data(), pos);
        uint16_t num_bones = read_uint16_le(data.data(), pos);
        uint32_t bone_names_size = read_uint32_le(data.data(), pos);
        uint16_t num_subsets = read_uint16_le(data.data(), pos);
        uint8_t num_hq_lods = read_uint8(data.data(), pos);
        uint8_t unused = read_uint8(data.data(), pos);
        (void)lod_type;
        (void)bone_names_size;
        (void)num_subsets;
        (void)num_hq_lods;
        (void)unused;
        if (num_vertices == 0 || num_faces == 0) return false;

        mesh.vertices.clear();
        mesh.indices.clear();
        mesh.vertices.reserve(num_vertices);
        mesh.indices.reserve(num_faces * 3);
        for (uint32_t i = 0; i < num_vertices; ++i) {
            if (pos + 40 > data.size()) return false;
            mesh_vertex vertex{};
            vertex.position.x = read_float_le(data.data(), pos);
            vertex.position.y = read_float_le(data.data(), pos);
            vertex.position.z = read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            read_uint8(data.data(), pos);
            read_uint8(data.data(), pos);
            read_uint8(data.data(), pos);
            read_uint8(data.data(), pos);
            mesh.vertices.push_back(vertex);
        }
        if (num_bones > 0) {
            size_t skip = (size_t)num_vertices * 8;
            if (pos + skip > data.size()) return false;
            pos += skip;
        }
        for (uint32_t i = 0; i < num_faces; ++i) {
            if (pos + 12 > data.size()) return false;
            mesh.indices.push_back(read_uint32_le(data.data(), pos));
            mesh.indices.push_back(read_uint32_le(data.data(), pos));
            mesh.indices.push_back(read_uint32_le(data.data(), pos));
        }
        std::vector<uint32_t> lods;
        lods.reserve(num_lods);
        for (uint16_t i = 0; i < num_lods; ++i) {
            if (pos + 4 > data.size()) break;
            lods.push_back(read_uint32_le(data.data(), pos));
        }
        if (lods.size() > 1 && lods[1] * 3 < mesh.indices.size()) {
            mesh.indices.resize(lods[1] * 3);
        }
        return true;
    }

    static bool parse_mesh_v5(const std::vector<uint8_t>& data, size_t offset, parsed_mesh& mesh) {
        size_t pos = offset;
        if (pos + 32 > data.size()) return false;
        uint16_t header_size = read_uint16_le(data.data(), pos);
        if (header_size != 32) return false;
        uint16_t lod_type = read_uint16_le(data.data(), pos);
        uint32_t num_vertices = read_uint32_le(data.data(), pos);
        uint32_t num_faces = read_uint32_le(data.data(), pos);
        uint16_t num_lods = read_uint16_le(data.data(), pos);
        uint16_t num_bones = read_uint16_le(data.data(), pos);
        uint32_t bone_names_size = read_uint32_le(data.data(), pos);
        uint16_t num_subsets = read_uint16_le(data.data(), pos);
        uint8_t num_hq_lods = read_uint8(data.data(), pos);
        uint8_t unused_padding = read_uint8(data.data(), pos);
        uint32_t facs_data_format = read_uint32_le(data.data(), pos);
        uint32_t facs_data_size = read_uint32_le(data.data(), pos);
        (void)lod_type;
        (void)bone_names_size;
        (void)num_subsets;
        (void)num_hq_lods;
        (void)unused_padding;
        (void)facs_data_format;
        if (num_vertices == 0 || num_faces == 0) return false;

        mesh.vertices.clear();
        mesh.indices.clear();
        mesh.vertices.reserve(num_vertices);
        mesh.indices.reserve(num_faces * 3);
        for (uint32_t i = 0; i < num_vertices; ++i) {
            if (pos + 40 > data.size()) return false;
            mesh_vertex vertex{};
            vertex.position.x = read_float_le(data.data(), pos);
            vertex.position.y = read_float_le(data.data(), pos);
            vertex.position.z = read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_float_le(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            read_int8(data.data(), pos);
            read_uint8(data.data(), pos);
            read_uint8(data.data(), pos);
            read_uint8(data.data(), pos);
            read_uint8(data.data(), pos);
            mesh.vertices.push_back(vertex);
        }
        if (num_bones > 0) {
            size_t skip = (size_t)num_vertices * 8;
            if (pos + skip > data.size()) return false;
            pos += skip;
        }
        for (uint32_t i = 0; i < num_faces; ++i) {
            if (pos + 12 > data.size()) return false;
            mesh.indices.push_back(read_uint32_le(data.data(), pos));
            mesh.indices.push_back(read_uint32_le(data.data(), pos));
            mesh.indices.push_back(read_uint32_le(data.data(), pos));
        }
        std::vector<uint32_t> lods;
        lods.reserve(num_lods);
        for (uint16_t i = 0; i < num_lods; ++i) {
            if (pos + 4 > data.size()) break;
            lods.push_back(read_uint32_le(data.data(), pos));
        }
        if (lods.size() > 1 && lods[1] * 3 < mesh.indices.size()) {
            mesh.indices.resize(lods[1] * 3);
        }
        size_t skip = (size_t)num_bones * 60 + bone_names_size + (size_t)num_subsets * 72 + facs_data_size;
        if (pos + skip > data.size()) return false;
        return true;
    }

    static bool parse_mesh_from_data(const std::vector<uint8_t>& data, parsed_mesh& mesh) {
        if (data.size() < 12) return false;
        std::string version = get_mesh_version(data);
        if (version.empty()) return false;
        mesh.version = version;
        size_t offset = 0;
        for (size_t i = 0; i < data.size(); ++i) {
            if (data[i] == '\n') {
                offset = i + 1;
                break;
            }
        }
        bool success = false;
        if (version == "1.00") success = parse_mesh_v1(data, offset, mesh, 0.5f, true);
        else if (version == "1.01") success = parse_mesh_v1(data, offset, mesh, 1.0f, false);
        else if (version == "2.00") success = parse_mesh_v2(data, offset, mesh);
        else if (version == "3.00" || version == "3.01") success = parse_mesh_v3(data, offset, mesh);
        else if (version == "4.00" || version == "4.01") success = parse_mesh_v4(data, offset, mesh);
        else if (version == "5.00" || version == "5.01" || version == "6.00" || version == "7.00") success = parse_mesh_v5(data, offset, mesh);
        if (!success) return false;
        if (mesh.vertices.empty() || mesh.indices.empty()) return false;
        compute_mesh_bounds(mesh);
        return mesh.bounds.valid;
    }

    static bool download_mesh_from_asset_id(uint64_t asset_id, std::vector<uint8_t>& mesh_data, int* out_status = nullptr, int depth = 0) {
        if (asset_id == 0) return false;
        if (depth > 2) return false;
        std::vector<uint8_t> api_response;
        int http_status = 0;
        std::wstring path = L"/v2/asset/?id=" + std::to_wstring(asset_id);
        bool ok = download_from_url(L"assetdelivery.roblox.com", path, api_response, &http_status);
        if (!ok) {
            path = L"/v1/asset/?id=" + std::to_wstring(asset_id);
            ok = download_from_url(L"assetdelivery.roblox.com", path, api_response, &http_status);
        }
        if (out_status) *out_status = http_status;
        if (!ok) return false;
        if (api_response.size() >= 12) {
            std::string header(reinterpret_cast<const char*>(api_response.data()), 12);
            if (header.rfind("version ", 0) == 0) {
                mesh_data = std::move(api_response);
                return true;
            }
        }

        std::string response_str(reinterpret_cast<const char*>(api_response.data()), api_response.size());
        uint64_t nested_asset_id = extract_asset_id_from_text(response_str);
        if (nested_asset_id != 0 && nested_asset_id != asset_id) {
            return download_mesh_from_asset_id(nested_asset_id, mesh_data, out_status, depth + 1);
        }

        std::string location_url = extract_json_string(response_str, "location");
        if (location_url.empty()) location_url = extract_json_string(response_str, "url");
        if (location_url.empty()) {
            size_t http_start = response_str.find("http");
            if (http_start != std::string::npos) {
                size_t http_end = response_str.find('"', http_start);
                if (http_end != std::string::npos) location_url = response_str.substr(http_start, http_end - http_start);
            }
        }
        if (location_url.empty()) return false;

        std::wstring host;
        std::wstring location_path;
        if (!split_url(location_url, host, location_path)) return false;
        bool downloaded = download_from_url(host, location_path, mesh_data, &http_status);
        if (out_status) *out_status = http_status;
        if (!downloaded) return false;
        if (mesh_data.size() >= 12) {
            std::string header(reinterpret_cast<const char*>(mesh_data.data()), 12);
            if (header.rfind("version ", 0) == 0) return true;
        }
        std::string nested_text(reinterpret_cast<const char*>(mesh_data.data()), mesh_data.size());
        nested_asset_id = extract_asset_id_from_text(nested_text);
        if (nested_asset_id != 0 && nested_asset_id != asset_id) {
            return download_mesh_from_asset_id(nested_asset_id, mesh_data, out_status, depth + 1);
        }
        return true;
    }

    static std::string read_string_property(uint64_t address) {
        uintptr_t value_ptr = read<uintptr_t>(address);
        std::string value;
        if (is_valid_address(value_ptr)) value = fetchstring(value_ptr);
        if (value.empty() || value == "str_error" || value == "Unknown") value = fetchstring(address);
        if (value == "str_error" || value == "Unknown") return "";
        return value;
    }

    static int map_character_mesh_body_part(const std::string& part_name) {
        if (part_name == "Head") return 0;
        if (part_name == "Torso" || part_name == "UpperTorso" || part_name == "LowerTorso") return 1;
        if (part_name == "Left Arm" || part_name == "LeftUpperArm" || part_name == "LeftLowerArm" || part_name == "LeftHand") return 2;
        if (part_name == "Right Arm" || part_name == "RightUpperArm" || part_name == "RightLowerArm" || part_name == "RightHand") return 3;
        if (part_name == "Left Leg" || part_name == "LeftUpperLeg" || part_name == "LeftLowerLeg" || part_name == "LeftFoot") return 4;
        if (part_name == "Right Leg" || part_name == "RightUpperLeg" || part_name == "RightLowerLeg" || part_name == "RightFoot") return 5;
        return -1;
    }

    static std::string get_mesh_id_string(uint64_t part_address) {
        if (part_address == 0) return "";
        instance part{ part_address };
        const std::string class_name = part.get_class_name();
        if (class_name == "MeshPart") {
            return read_string_property(part_address + Offsets::MeshPart::MeshId);
        }

        for (const instance& child : part.get_children()) {
            if (!child.is_valid()) continue;
            const std::string child_class = child.get_class_name();
            if (child_class == "SpecialMesh" || child_class == "FileMesh") {
                std::string mesh_id = read_string_property(child.address + Offsets::SpecialMesh::MeshId);
                if (!mesh_id.empty()) return mesh_id;
            }
        }

        instance parent = read<instance>(part_address + Offsets::Instance::Parent);
        if (parent.is_valid()) {
            const int target_body_part = map_character_mesh_body_part(part.get_name());
            if (target_body_part >= 0) {
                for (const instance& sibling : parent.get_children()) {
                    if (!sibling.is_valid()) continue;
                    if (sibling.get_class_name() != "CharacterMesh") continue;
                    int body_part = read<int>(sibling.address + Offsets::CharacterMesh::BodyPart);
                    if (body_part != target_body_part) continue;
                    std::string mesh_id = read_string_property(sibling.address + Offsets::CharacterMesh::MeshId);
                    if (!mesh_id.empty()) return mesh_id;
                }
            }
        }

        return "";
    }

    static uint64_t extract_asset_id(const std::string& mesh_id_string) {
        if (mesh_id_string.rfind("rbxassetid://", 0) == 0) {
            try { return std::stoull(mesh_id_string.substr(13)); }
            catch (...) { return 0; }
        }
        if (mesh_id_string.rfind("rbxasset://", 0) == 0) {
            try { return std::stoull(mesh_id_string.substr(11)); }
            catch (...) { return 0; }
        }
        size_t id_pos = mesh_id_string.find("?id=");
        if (id_pos != std::string::npos) {
            std::string id_str = mesh_id_string.substr(id_pos + 4);
            size_t end_pos = id_str.find_first_of("& \n\r\t");
            if (end_pos != std::string::npos) id_str = id_str.substr(0, end_pos);
            try { return std::stoull(id_str); }
            catch (...) { return 0; }
        }
        return 0;
    }

    static DWORD WINAPI worker_thread_proc(LPVOID) {
        while (g_running.load()) {
            try {
                uint64_t asset_id = 0;
                {
                    std::unique_lock<std::mutex> lock(g_state_mutex);
                    g_queue_cv.wait_for(lock, std::chrono::milliseconds(100), [] {
                        return !g_request_queue.empty() || !g_running.load();
                        });
                    if (!g_running.load()) break;
                    if (g_request_queue.empty()) continue;
                    asset_id = g_request_queue.front();
                    g_request_queue.pop();
                }

                ULONGLONG now = GetTickCount64();
                ULONGLONG last = g_last_request_tick.load();
                if (last != 0 && now - last < REQUEST_DELAY_MS) {
                    Sleep((DWORD)(REQUEST_DELAY_MS - (now - last)));
                }
                g_last_request_tick.store(GetTickCount64());

                std::vector<uint8_t> mesh_data;
                int http_status = 0;
                if (!download_mesh_from_asset_id(asset_id, mesh_data, &http_status)) {
                    g_fetch_fail++;
                    g_fail_api++;
                    g_last_failed_asset_id.store(asset_id);
                    {
                        std::lock_guard<std::mutex> lock(g_state_mutex);
                        g_pending_assets.erase(asset_id);
                        ULONGLONG retry_ms = (http_status == 401 || http_status == 403) ? 30000ull : 10000ull;
                        g_retry_after[asset_id] = GetTickCount64() + retry_ms;
                    }
                    set_error("assetdelivery fetch failed");
                    continue;
                }

                auto mesh = std::make_shared<parsed_mesh>();
                if (!parse_mesh_from_data(mesh_data, *mesh)) {
                    g_fetch_fail++;
                    g_fail_parse++;
                    g_last_failed_asset_id.store(asset_id);
                    {
                        std::lock_guard<std::mutex> lock(g_state_mutex);
                        g_pending_assets.erase(asset_id);
                        g_retry_after[asset_id] = GetTickCount64() + 60000ull;
                    }
                    set_error("mesh parse failed");
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(g_state_mutex);
                    g_mesh_cache[asset_id] = mesh;
                    g_pending_assets.erase(asset_id);
                    g_retry_after.erase(asset_id);
                }
                g_fetch_ok++;
            } catch (...) {
                set_error("mesh worker crashed");
                Sleep(100);
            }
        }
        return 0;
    }

    void initialize() {
        if (g_running.load()) return;
        g_running.store(true);
        g_workers.reserve(THREAD_POOL_SIZE);
        for (size_t i = 0; i < THREAD_POOL_SIZE; ++i) {
            HANDLE worker = CreateThread(nullptr, 0, worker_thread_proc, nullptr, 0, nullptr);
            if (worker) g_workers.push_back(worker);
        }
    }

    void shutdown() {
        g_running.store(false);
        g_queue_cv.notify_all();
        for (HANDLE worker : g_workers) {
            WaitForSingleObject(worker, INFINITE);
            CloseHandle(worker);
        }
        g_workers.clear();
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_mesh_cache.clear();
        g_pending_assets.clear();
        g_retry_after.clear();
        std::queue<uint64_t> empty;
        std::swap(g_request_queue, empty);
        {
            std::lock_guard<std::mutex> av_lock(g_avatar_mutex);
            g_avatar_asset_cache.clear();
            g_avatar_pending.clear();
            g_avatar_retry_after.clear();
        }
    }

    uint64_t get_mesh_asset_id_from_part(uintptr_t part_address) {
        if (part_address == 0) return 0;
        return extract_asset_id(get_mesh_id_string(part_address));
    }

    std::string get_mesh_id_string_from_part(uintptr_t part_address) {
        if (part_address == 0) return "";
        return get_mesh_id_string(part_address);
    }

    void request_mesh(uint64_t asset_id) {
        if (asset_id == 0) return;
        if (!g_running.load()) initialize();
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_mesh_cache.find(asset_id) != g_mesh_cache.end()) return;
        auto retry_it = g_retry_after.find(asset_id);
        if (retry_it != g_retry_after.end()) {
            ULONGLONG now = GetTickCount64();
            if (now < retry_it->second) return;
            g_retry_after.erase(retry_it);
        }
        if (g_pending_assets.find(asset_id) != g_pending_assets.end()) return;
        g_pending_assets.insert(asset_id);
        g_request_queue.push(asset_id);
        g_queue_cv.notify_one();
    }

    std::shared_ptr<const parsed_mesh> get_mesh(uint64_t asset_id) {
        if (asset_id == 0) return {};
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_mesh_cache.find(asset_id);
        if (it == g_mesh_cache.end()) return {};
        return it->second;
    }

    void request_avatar_assets(int64_t user_id) {
        if (user_id == 0) return;
        {
            std::lock_guard<std::mutex> lock(g_avatar_mutex);
            if (g_avatar_asset_cache.find(user_id) != g_avatar_asset_cache.end()) return;
            auto retry_it = g_avatar_retry_after.find(user_id);
            if (retry_it != g_avatar_retry_after.end()) {
                if (GetTickCount64() < retry_it->second) return;
                g_avatar_retry_after.erase(retry_it);
            }
            if (g_avatar_pending.find(user_id) != g_avatar_pending.end()) return;
            g_avatar_pending.insert(user_id);
        }
        CreateThread(nullptr, 0, avatar_fetch_thread, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(user_id)), 0, nullptr);
    }

    uint64_t get_default_asset_for_part(const char* part_name, bool is_r15) {
        return get_default_asset_for_part_impl(part_name, is_r15);
    }

    uint64_t get_mesh_asset_id_for_part(int64_t user_id, const char* part_name, bool is_r15) {
        if (user_id == 0 || !part_name) return 0;
        const char* asset_type = part_name_to_asset_type(part_name);
        if (!asset_type) return 0;
        std::lock_guard<std::mutex> lock(g_avatar_mutex);
        auto it = g_avatar_asset_cache.find(user_id);
        if (it == g_avatar_asset_cache.end()) return 0;
        auto type_it = it->second.find(asset_type);
        if (type_it != it->second.end()) return type_it->second;
        if (strcmp(part_name, "Head") == 0) {
            type_it = it->second.find("Head");
            if (type_it != it->second.end()) return type_it->second;
        }
        return 0;
    }

    debug_stats get_debug_stats() {
        debug_stats stats{};
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            stats.cache_count = g_mesh_cache.size();
            stats.pending_count = g_pending_assets.size();
            stats.queue_size = g_request_queue.size();
            stats.retry_after_count = g_retry_after.size();
            stats.fetch_ok = g_fetch_ok.load();
            stats.fetch_fail = g_fetch_fail.load();
            stats.fail_api = g_fail_api.load();
            stats.fail_download = g_fail_download.load();
            stats.fail_parse = g_fail_parse.load();
            strncpy_s(stats.last_error, g_last_error, sizeof(stats.last_error) - 1);
            stats.last_http_status = g_last_http_status.load();
            stats.last_failed_asset_id = g_last_failed_asset_id.load();
        }
        {
            std::lock_guard<std::mutex> lock(g_avatar_mutex);
            stats.avatar_cache_count = g_avatar_asset_cache.size();
            stats.avatar_pending_count = g_avatar_pending.size();
        }
        return stats;
    }

}
