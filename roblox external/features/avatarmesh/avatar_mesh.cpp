#include "avatar_mesh.h"
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <array>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <queue>
#include <unordered_set>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")

namespace avatarmesh {

static std::unordered_map<int64_t, avatar_mesh> g_mesh_cache;
static std::unordered_map<int64_t, DWORD> g_retry_after;
static std::unordered_set<int64_t> g_pending_ids;
static std::queue<std::pair<int64_t, bool>> g_request_queue;
static std::mutex g_cache_mutex;
static std::atomic<size_t> g_fetch_ok{ 0 };
static std::atomic<size_t> g_fetch_fail{ 0 };
static std::atomic<size_t> g_fail_api{ 0 };
static std::atomic<size_t> g_fail_imageurl{ 0 };
static std::atomic<size_t> g_fail_meta{ 0 };
static std::atomic<size_t> g_fail_obj_hash{ 0 };
static std::atomic<size_t> g_fail_cdn{ 0 };
static std::atomic<size_t> g_fail_parse{ 0 };
static char g_last_error[128] = {};
static std::atomic<int> g_last_http_status{ 0 };
static constexpr int THREAD_POOL_SIZE = 1;
static std::vector<HANDLE> g_workers;
static std::atomic<bool> g_running{ false };

static std::string cdn_url_from_hash(const std::string& hash) {
    int i = 31;
    size_t len = (std::min)(hash.size(), size_t(38));
    for (size_t t = 0; t < len; t++)
        i ^= static_cast<unsigned char>(hash[t]);
    return "https://t" + std::to_string(i % 8) + ".rbxcdn.com/" + hash;
}

static void set_retry_after(int64_t user_id, DWORD delay_ms) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_retry_after[user_id] = GetTickCount() + delay_ms;
}

static bool http_get(const std::wstring& host, const std::wstring& path, std::string& out, int* out_status = nullptr) {
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    DWORD timeout = 20000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    LPCWSTR headers = L"Accept: */*\r\nAccept-Encoding: gzip, deflate\r\nReferer: https://www.roblox.com/\r\n";
    WinHttpAddRequestHeaders(hRequest, headers, (DWORD)wcslen(headers), WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }
    DWORD status = 0, statusLen = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &statusLen, NULL);
    if (out_status) *out_status = (int)status;
    g_last_http_status.store((int)status);
    if (status != 200) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }
    out.clear();
    DWORD avail = 0;
    do {
        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) break;
        out.append(buf.data(), read);
    } while (true);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return !out.empty();
}

static bool fetch_url(const std::string& url, std::string& out, int* out_status = nullptr) {
    size_t proto = url.find("://");
    if (proto == std::string::npos) return false;
    std::string rest = url.substr(proto + 3);
    size_t path_start = rest.find('/');
    if (path_start == std::string::npos) return false;
    std::string host_s = rest.substr(0, path_start);
    std::string path_s = rest.substr(path_start);
    std::wstring host(host_s.begin(), host_s.end());
    std::wstring path(path_s.begin(), path_s.end());
    return http_get(host, path, out, out_status);
}

static std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
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

static std::string extract_hash_from_url(const std::string& url) {
    size_t rbxcdn = url.find("rbxcdn.com/");
    if (rbxcdn != std::string::npos) {
        size_t slash = url.find('/', rbxcdn);
        if (slash != std::string::npos) {
            size_t start = slash + 1;
            size_t end = url.find_first_of("/\" \t\n\r?&", start);
            if (end == std::string::npos) end = url.size();
            std::string hash = url.substr(start, end - start);
            if (hash.size() >= 16 && hash.size() <= 128) return hash;
        }
    }
    return "";
}

static std::string extract_obj_hash_from_meta(const std::string& meta) {
    const char* keys[] = { "obj", "objHash", "object", "model", "mesh" };
    for (const char* k : keys) {
        std::string v = extract_json_string(meta, k);
        if (v.empty()) continue;
        std::string hash = extract_hash_from_url(v);
        if (!hash.empty()) return hash;
        if (v.size() >= 16 && v.size() <= 128) {
            bool ok = true;
            for (char c : v) {
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-') continue;
                ok = false;
                break;
            }
            if (ok) return v;
        }
    }
    size_t rbxcdn = meta.find("rbxcdn.com/");
    if (rbxcdn != std::string::npos) {
        size_t slash = meta.find('/', rbxcdn);
        if (slash != std::string::npos) {
            size_t start = slash + 1;
            size_t end = meta.find_first_of("\" \t\n\r?&", start);
            if (end == std::string::npos) end = meta.size();
            std::string hash = meta.substr(start, end - start);
            if (hash.size() >= 16 && hash.size() <= 128) return hash;
        }
    }
    return "";
}

static int parse_face_index(const char* token, size_t slash, int vertex_count) {
    std::string vpart = (slash == std::string::npos) ? token : std::string(token, slash);
    if (vpart.empty()) return -1;
    int idx = 0;
    try { idx = std::stoi(vpart); } catch (...) { return -1; }
    if (idx > 0) return idx - 1;
    if (idx < 0) return vertex_count + idx;
    return -1;
}

static bool is_player_group_name(const std::string& name) {
    if (name.size() < 7) return false;
    if (name.compare(0, 6, "Player") != 0) return false;
    for (size_t i = 6; i < name.size(); i++) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

static void assign_label(mesh_part& part, const char* label) {
    part.label = label;
}

static void label_r6_parts(avatar_mesh& mesh, const std::vector<size_t>& player_parts) {
    if (player_parts.size() < 6) return;
    std::vector<size_t> ids = player_parts;
    std::sort(ids.begin(), ids.end(), [&](size_t a, size_t b) {
        return mesh.parts[a].center.y > mesh.parts[b].center.y;
    });
    assign_label(mesh.parts[ids[0]], "Head");
    size_t torso_idx = ids[1];
    float best_abs_x = fabsf(mesh.parts[torso_idx].center.x);
    for (size_t i = 1; i < ids.size(); i++) {
        float ax = fabsf(mesh.parts[ids[i]].center.x);
        if (ax < best_abs_x) {
            best_abs_x = ax;
            torso_idx = ids[i];
        }
    }
    assign_label(mesh.parts[torso_idx], "Torso");
    std::vector<size_t> remain;
    for (size_t idx : ids) {
        if (idx != ids[0] && idx != torso_idx) remain.push_back(idx);
    }
    std::vector<size_t> left_side;
    std::vector<size_t> right_side;
    for (size_t idx : remain) {
        if (mesh.parts[idx].center.x < 0.0f) left_side.push_back(idx);
        else right_side.push_back(idx);
    }
    if (left_side.size() == 2) {
        std::sort(left_side.begin(), left_side.end(), [&](size_t a, size_t b) {
            return mesh.parts[a].center.y > mesh.parts[b].center.y;
        });
        assign_label(mesh.parts[left_side[0]], "Left Arm");
        assign_label(mesh.parts[left_side[1]], "Left Leg");
    }
    if (right_side.size() == 2) {
        std::sort(right_side.begin(), right_side.end(), [&](size_t a, size_t b) {
            return mesh.parts[a].center.y > mesh.parts[b].center.y;
        });
        assign_label(mesh.parts[right_side[0]], "Right Arm");
        assign_label(mesh.parts[right_side[1]], "Right Leg");
    }
}

static void label_r15_side(avatar_mesh& mesh, const std::vector<size_t>& side_parts, const char* upper_leg, const char* lower_leg, const char* foot, const char* upper_arm, const char* lower_arm, const char* hand) {
    if (side_parts.size() < 6) return;
    std::vector<size_t> ids = side_parts;
    std::sort(ids.begin(), ids.end(), [&](size_t a, size_t b) {
        return fabsf(mesh.parts[a].center.x) > fabsf(mesh.parts[b].center.x);
    });
    std::vector<size_t> arm_chain(ids.begin(), ids.begin() + 3);
    std::vector<size_t> leg_chain(ids.begin() + 3, ids.begin() + 6);
    std::sort(arm_chain.begin(), arm_chain.end(), [&](size_t a, size_t b) {
        return mesh.parts[a].center.y > mesh.parts[b].center.y;
    });
    std::sort(leg_chain.begin(), leg_chain.end(), [&](size_t a, size_t b) {
        return mesh.parts[a].center.y > mesh.parts[b].center.y;
    });
    assign_label(mesh.parts[arm_chain[0]], upper_arm);
    assign_label(mesh.parts[arm_chain[1]], lower_arm);
    assign_label(mesh.parts[arm_chain[2]], hand);
    assign_label(mesh.parts[leg_chain[0]], upper_leg);
    assign_label(mesh.parts[leg_chain[1]], lower_leg);
    assign_label(mesh.parts[leg_chain[2]], foot);
}

static void label_r15_parts(avatar_mesh& mesh, const std::vector<size_t>& player_parts) {
    if (player_parts.size() < 15) return;
    std::vector<size_t> ids = player_parts;
    std::sort(ids.begin(), ids.end(), [&](size_t a, size_t b) {
        return mesh.parts[a].center.y > mesh.parts[b].center.y;
    });
    assign_label(mesh.parts[ids[0]], "Head");
    std::vector<size_t> remain;
    for (size_t i = 1; i < ids.size(); i++) remain.push_back(ids[i]);
    std::sort(remain.begin(), remain.end(), [&](size_t a, size_t b) {
        return fabsf(mesh.parts[a].center.x) < fabsf(mesh.parts[b].center.x);
    });
    if (remain.size() < 14) return;
    size_t torso_a = remain[0];
    size_t torso_b = remain[1];
    if (mesh.parts[torso_a].center.y > mesh.parts[torso_b].center.y) {
        assign_label(mesh.parts[torso_a], "UpperTorso");
        assign_label(mesh.parts[torso_b], "LowerTorso");
    } else {
        assign_label(mesh.parts[torso_b], "UpperTorso");
        assign_label(mesh.parts[torso_a], "LowerTorso");
    }
    std::vector<size_t> side_parts;
    for (size_t idx : remain) {
        if (idx != torso_a && idx != torso_b) side_parts.push_back(idx);
    }
    if (side_parts.size() < 12) return;
    std::sort(side_parts.begin(), side_parts.end(), [&](size_t a, size_t b) {
        return mesh.parts[a].center.x < mesh.parts[b].center.x;
    });
    std::vector<size_t> left_side(side_parts.begin(), side_parts.begin() + 6);
    std::vector<size_t> right_side(side_parts.end() - 6, side_parts.end());
    label_r15_side(mesh, left_side, "LeftUpperLeg", "LeftLowerLeg", "LeftFoot", "LeftUpperArm", "LeftLowerArm", "LeftHand");
    label_r15_side(mesh, right_side, "RightUpperLeg", "RightLowerLeg", "RightFoot", "RightUpperArm", "RightLowerArm", "RightHand");
}

static void label_mesh_parts(avatar_mesh& mesh) {
    std::vector<size_t> player_parts;
    for (size_t i = 0; i < mesh.parts.size(); i++) {
        mesh.parts[i].label.clear();
        if (is_player_group_name(mesh.parts[i].name)) player_parts.push_back(i);
    }
    if (player_parts.size() >= 15) label_r15_parts(mesh, player_parts);
    else if (player_parts.size() >= 6) label_r6_parts(mesh, player_parts);
}

static bool parse_obj(const std::string& data, avatar_mesh& mesh) {
    if (data.size() < 10) return false;
    std::vector<vec3_t> global_verts;
    struct temp_part { std::string name; std::vector<std::array<int, 3>> faces; };
    std::vector<temp_part> temp_parts;
    temp_part* current = nullptr;
    std::istringstream iss(data);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line[0] == ' ' || line[0] == '\t')) line.erase(0, 1);
        if (line.empty() || line[0] == '#') continue;
        if (line.size() > 1 && line[0] == 'v' && line[1] == ' ') {
            float x, y, z;
            if (sscanf_s(line.c_str() + 2, "%f %f %f", &x, &y, &z) == 3)
                global_verts.push_back({ x, y, z });
        } else if ((line[0] == 'o' || line[0] == 'g') && line.size() > 1 && (line[1] == ' ' || line[1] == '\t')) {
            std::string name = line.substr(2);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            temp_parts.push_back({});
            current = &temp_parts.back();
            current->name = name.empty() ? "unnamed" : name;
        } else if (line[0] == 'f' && line.size() > 1 && (line[1] == ' ' || line[1] == '\t')) {
            if (!current) {
                temp_parts.push_back({});
                current = &temp_parts.back();
                current->name = "default";
            }
            std::vector<int> poly;
            const char* p = line.c_str() + 2;
            while (*p) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;
                const char* start = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                std::string tok(start, p);
                size_t slash = tok.find('/');
                int idx = parse_face_index(tok.c_str(), slash, (int)global_verts.size());
                if (idx >= 0 && (size_t)idx < global_verts.size()) poly.push_back(idx);
            }
            for (size_t i = 1; i + 1 < (int)poly.size(); i++) {
                std::array<int, 3> fa = { poly[0], poly[i], poly[i + 1] };
                current->faces.push_back(fa);
            }
        }
    }
    mesh.parts.clear();
    for (const auto& tp : temp_parts) {
        if (tp.faces.empty()) continue;
        mesh_part mp;
        mp.name = tp.name;
        std::vector<int> remap(global_verts.size(), -1);
        for (const auto& f : tp.faces) {
            uint32_t lf[3] = {};
            int valid = 0;
            for (int i = 0; i < 3; i++) {
                int gi = f[i];
                if (gi < 0 || (size_t)gi >= global_verts.size()) continue;
                int li = remap[gi];
                if (li < 0) {
                    li = (int)mp.vertices.size();
                    remap[gi] = li;
                    mesh_vertex mv{};
                    mv.position = global_verts[gi];
                    mp.vertices.push_back(mv);
                }
                lf[valid++] = (uint32_t)li;
            }
            if (valid >= 3) mp.faces.push_back({ lf[0], lf[1], lf[2] });
        }
        if (mp.vertices.empty() || mp.faces.empty()) continue;
        float minx = 1e9f, miny = 1e9f, minz = 1e9f;
        float maxx = -1e9f, maxy = -1e9f, maxz = -1e9f;
        for (const auto& v : mp.vertices) {
            minx = (std::min)(minx, v.position.x);
            miny = (std::min)(miny, v.position.y);
            minz = (std::min)(minz, v.position.z);
            maxx = (std::max)(maxx, v.position.x);
            maxy = (std::max)(maxy, v.position.y);
            maxz = (std::max)(maxz, v.position.z);
        }
        mp.center.x = (minx + maxx) * 0.5f;
        mp.center.y = (miny + maxy) * 0.5f;
        mp.center.z = (minz + maxz) * 0.5f;
        mp.size.x = (std::max)(0.001f, maxx - minx);
        mp.size.y = (std::max)(0.001f, maxy - miny);
        mp.size.z = (std::max)(0.001f, maxz - minz);
        mesh.parts.push_back(std::move(mp));
    }
    label_mesh_parts(mesh);
    mesh.valid = !mesh.parts.empty();
    return mesh.valid;
}

static const char* r15_part_to_obj_name(const char* roblox_name) {
    if (strcmp(roblox_name, "Head") == 0) return "Player1";
    if (strcmp(roblox_name, "UpperTorso") == 0) return "Player3";
    if (strcmp(roblox_name, "LowerTorso") == 0) return "Player2";
    if (strcmp(roblox_name, "LeftUpperLeg") == 0) return "Player6";
    if (strcmp(roblox_name, "LeftLowerLeg") == 0) return "Player5";
    if (strcmp(roblox_name, "LeftFoot") == 0) return "Player4";
    if (strcmp(roblox_name, "RightUpperLeg") == 0) return "Player9";
    if (strcmp(roblox_name, "RightLowerLeg") == 0) return "Player8";
    if (strcmp(roblox_name, "RightFoot") == 0) return "Player7";
    if (strcmp(roblox_name, "LeftUpperArm") == 0) return "Player12";
    if (strcmp(roblox_name, "LeftLowerArm") == 0) return "Player11";
    if (strcmp(roblox_name, "LeftHand") == 0) return "Player10";
    if (strcmp(roblox_name, "RightUpperArm") == 0) return "Player15";
    if (strcmp(roblox_name, "RightLowerArm") == 0) return "Player14";
    if (strcmp(roblox_name, "RightHand") == 0) return "Player13";
    return nullptr;
}

static const char* r6_part_to_obj_name(const char* roblox_name) {
    if (strcmp(roblox_name, "Head") == 0) return "Player6";
    if (strcmp(roblox_name, "Torso") == 0) return "Player5";
    if (strcmp(roblox_name, "Left Arm") == 0) return "Player4";
    if (strcmp(roblox_name, "Right Arm") == 0) return "Player3";
    if (strcmp(roblox_name, "Left Leg") == 0) return "Player2";
    if (strcmp(roblox_name, "Right Leg") == 0) return "Player1";
    return nullptr;
}

static void set_error(const char* msg) {
    strncpy_s(g_last_error, msg, sizeof(g_last_error) - 1);
    g_last_error[sizeof(g_last_error) - 1] = '\0';
}

static bool looks_like_obj(const std::string& s) {
    if (s.size() < 4) return false;
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && s[first] == '{') return false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == 'v' && i + 1 < s.size() && (s[i+1] == ' ' || s[i+1] == '\t')) return true;
        if (c == 'o' && i + 1 < s.size() && (s[i+1] == ' ' || s[i+1] == '\t')) return true;
        if (c == 'g' && i + 1 < s.size() && (s[i+1] == ' ' || s[i+1] == '\t')) return true;
        if (c == 'f' && i + 1 < s.size() && (s[i+1] == ' ' || s[i+1] == '\t')) return true;
        if (c == '#') return true;
        return false;
    }
    return false;
}

static void fetch_avatar_worker(int64_t user_id, bool is_r15) {
    (void)is_r15;
    int http_status = 0;
    std::string api_resp;
    for (int retry = 0; retry < 3; retry++) {
        if (fetch_url("https://thumbnails.roblox.com/v1/users/avatar-3d?userId=" + std::to_string(user_id), api_resp, &http_status)) {
            break;
        }
        if (http_status == 429 && retry < 2) {
            Sleep(10000);
            continue;
        }
        g_fetch_fail++; g_fail_api++;
        set_retry_after(user_id, http_status == 429 ? 30000 : 15000);
        char buf[64];
        snprintf(buf, sizeof(buf), "avatar-3d API failed HTTP %d", http_status);
        set_error(buf);
        return;
    }
    std::string state = extract_json_string(api_resp, "state");
    if (state == "Error" || state == "Blocked") {
        g_fetch_fail++; g_fail_imageurl++;
        set_retry_after(user_id, 30000);
        set_error("avatar state not Completed");
        return;
    }
    std::string image_url = extract_json_string(api_resp, "imageUrl");
    if (image_url.empty()) {
        if (state == "Pending" || state == "TemporarilyUnavailable" || state == "InReview" || state == "Blocked" || state == "Error") {
            set_retry_after(user_id, 10000);
            set_error("avatar thumbnail pending");
            return;
        }
        g_fetch_fail++; g_fail_imageurl++;
        set_retry_after(user_id, 15000);
        set_error("no imageUrl in response");
        return;
    }
    std::string meta_resp;
    for (int retry = 0; retry < 3; retry++) {
        if (fetch_url(image_url, meta_resp, &http_status))
            break;
        if (http_status == 429 && retry < 2) {
            Sleep(10000);
            continue;
        }
        const size_t obj_pos = image_url.find("-Obj");
        if (obj_pos != std::string::npos) {
            const std::string base_url = image_url.substr(0, obj_pos);
            if (fetch_url(base_url, meta_resp, &http_status)) {
                break;
            }
        }
        g_fetch_fail++; g_fail_meta++;
        set_retry_after(user_id, http_status == 429 ? 30000 : 15000);
        char buf[64];
        snprintf(buf, sizeof(buf), "imageUrl fetch failed HTTP %d", http_status);
        set_error(buf);
        return;
    }
    std::string obj_hash = extract_obj_hash_from_meta(meta_resp);
    std::string obj_data;
    if (!obj_hash.empty()) {
        bool got_obj = false;
        int obj_status = 0;
        std::string primary_url = cdn_url_from_hash(obj_hash);
        if (fetch_url(primary_url, obj_data, &obj_status) && !obj_data.empty() && looks_like_obj(obj_data)) {
            got_obj = true;
        } else {
            for (int attempt = 0; attempt < 8; attempt++) {
                std::string obj_url = "https://t" + std::to_string(attempt) + ".rbxcdn.com/" + obj_hash;
                if (obj_url == primary_url) continue;
                std::string attempt_data;
                if (fetch_url(obj_url, attempt_data, &obj_status) && !attempt_data.empty() && looks_like_obj(attempt_data)) {
                    obj_data = std::move(attempt_data);
                    got_obj = true;
                    break;
                }
            }
        }
        if (!got_obj || obj_data.empty()) {
            g_fetch_fail++; g_fail_cdn++;
            set_retry_after(user_id, 15000);
            set_error("OBJ CDN fetch failed");
            return;
        }
    } else if (looks_like_obj(meta_resp)) {
        obj_data = meta_resp;
    } else {
        g_fetch_fail++; g_fail_obj_hash++;
        set_retry_after(user_id, 15000);
        set_error("no obj hash in meta");
        return;
    }
    avatar_mesh mesh;
    if (!parse_obj(obj_data, mesh)) {
        g_fetch_fail++; g_fail_parse++;
        set_retry_after(user_id, 15000);
        set_error("OBJ parse failed");
        return;
    }
    g_fetch_ok++;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_retry_after.erase(user_id);
    g_mesh_cache[user_id] = std::move(mesh);
}

static DWORD WINAPI worker_thread(LPVOID) {
    while (g_running.load()) {
        try {
            int64_t user_id = 0;
            bool is_r15 = true;
            {
                std::lock_guard<std::mutex> lock(g_cache_mutex);
                if (!g_request_queue.empty()) {
                    auto p = g_request_queue.front();
                    g_request_queue.pop();
                    user_id = p.first;
                    is_r15 = p.second;
                    g_pending_ids.erase(user_id);
                }
            }
            if (user_id != 0) {
                fetch_avatar_worker(user_id, is_r15);
                Sleep(5000);
            } else {
                Sleep(50);
            }
        } catch (...) {
            set_error("avatar worker crashed");
            Sleep(250);
        }
    }
    return 0;
}

void initialize() {
    if (g_running.load()) return;
    g_running.store(true);
    g_workers.reserve(THREAD_POOL_SIZE);
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        HANDLE h = CreateThread(nullptr, 0, worker_thread, nullptr, 0, nullptr);
        if (h) g_workers.push_back(h);
    }
}

void request_avatar_mesh(int64_t user_id, bool is_r15) {
    if (user_id <= 0) return;
    if (!g_running.load()) initialize();
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto retry_it = g_retry_after.find(user_id);
    if (retry_it != g_retry_after.end() && GetTickCount() < retry_it->second) return;
    if (g_mesh_cache.find(user_id) != g_mesh_cache.end()) return;
    if (g_pending_ids.find(user_id) != g_pending_ids.end()) return;
    g_pending_ids.insert(user_id);
    g_request_queue.push({ user_id, is_r15 });
}

const avatar_mesh* get_avatar_mesh(int64_t user_id, bool is_r15) {
    if (user_id <= 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_mesh_cache.find(user_id);
    if (it == g_mesh_cache.end()) return nullptr;
    return &it->second;
}

const mesh_part* find_part(const avatar_mesh* mesh, const char* roblox_name, bool is_r15) {
    if (!mesh || !mesh->valid) return nullptr;
    for (const auto& p : mesh->parts) {
        if (p.label == roblox_name) return &p;
        if (!p.label.empty() && _stricmp(p.label.c_str(), roblox_name) == 0) return &p;
    }
    const char* obj_name = is_r15 ? r15_part_to_obj_name(roblox_name) : r6_part_to_obj_name(roblox_name);
    if (!obj_name) return nullptr;
    for (const auto& p : mesh->parts) {
        if (p.name == obj_name) return &p;
        if (_stricmp(p.name.c_str(), obj_name) == 0) return &p;
    }
    return nullptr;
}

debug_stats get_debug_stats() {
    debug_stats s{};
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    s.cache_count = g_mesh_cache.size();
    s.pending_count = g_pending_ids.size();
    s.queue_size = g_request_queue.size();
    s.fetch_ok = g_fetch_ok.load();
    s.fetch_fail = g_fetch_fail.load();
    s.fail_api = g_fail_api.load();
    s.fail_imageurl = g_fail_imageurl.load();
    s.fail_meta = g_fail_meta.load();
    s.fail_obj_hash = g_fail_obj_hash.load();
    s.fail_cdn = g_fail_cdn.load();
    s.fail_parse = g_fail_parse.load();
    strncpy_s(s.last_error, g_last_error, sizeof(s.last_error) - 1);
    s.last_http_status = g_last_http_status.load();
    return s;
}

}
