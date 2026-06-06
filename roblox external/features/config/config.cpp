#include "config.h"
#include "../../globals.h"
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace config {

    std::string GetConfigDir() {
        return "C:\\config";
    }

    void EnsureConfigDir() {
        std::string dir = GetConfigDir();
        CreateDirectoryA(dir.c_str(), nullptr);
    }

    static std::string BoolStr(bool v) { return v ? "1" : "0"; }
    static std::string FloatStr(float v) {
        char buf[64];
        sprintf_s(buf, "%f", v);
        return buf;
    }
    static std::string IntStr(int v) { return std::to_string(v); }

    static bool ParseBool(const std::string& s) { return s == "1" || s == "true"; }
    static float ParseFloat(const std::string& s) {
        try { return std::stof(s); } catch (...) { return 0.0f; }
    }
    static int ParseInt(const std::string& s) {
        try { return std::stoi(s); } catch (...) { return 0; }
    }

    static void WriteEntry(std::ofstream& f, const char* key, bool val) {
        f << key << "=" << BoolStr(val) << "\n";
    }
    static void WriteEntry(std::ofstream& f, const char* key, int val) {
        f << key << "=" << IntStr(val) << "\n";
    }
    static void WriteEntry(std::ofstream& f, const char* key, float val) {
        f << key << "=" << FloatStr(val) << "\n";
    }
    static void WriteEntry(std::ofstream& f, const char* key, const float val[4]) {
        f << key << "=" << FloatStr(val[0]) << "," << FloatStr(val[1]) << "," << FloatStr(val[2]) << "," << FloatStr(val[3]) << "\n";
    }

    static bool ReadLine(const std::string& line, const char* key, bool& out) {
        std::string prefix = std::string(key) + "=";
        if (line.find(prefix) != 0) return false;
        out = ParseBool(line.substr(prefix.size()));
        return true;
    }
    static bool ReadLine(const std::string& line, const char* key, int& out) {
        std::string prefix = std::string(key) + "=";
        if (line.find(prefix) != 0) return false;
        out = ParseInt(line.substr(prefix.size()));
        return true;
    }
    static bool ReadLine(const std::string& line, const char* key, float& out) {
        std::string prefix = std::string(key) + "=";
        if (line.find(prefix) != 0) return false;
        out = ParseFloat(line.substr(prefix.size()));
        return true;
    }
    static bool ReadLine(const std::string& line, const char* key, float out[4]) {
        std::string prefix = std::string(key) + "=";
        if (line.find(prefix) != 0) return false;
        std::string val = line.substr(prefix.size());
        std::stringstream ss(val);
        std::string token;
        for (int i = 0; i < 4 && std::getline(ss, token, ','); ++i) {
            out[i] = ParseFloat(token);
        }
        return true;
    }

    bool Save(const std::string& config_name) {
        if (config_name.empty()) return false;
        EnsureConfigDir();
        std::string path = GetConfigDir() + "\\" + config_name + ".cfg";
        std::ofstream f(path);
        if (!f.is_open()) return false;

        // aimbot
        WriteEntry(f, "aimbot_enabled", aimbot_enabled);
        WriteEntry(f, "aimbot_aim_type", aimbot_aim_type);
        WriteEntry(f, "aimbot_keybind", aimbot_keybind);
        WriteEntry(f, "aimbot_part", aimbot_part);
        WriteEntry(f, "sticky_aim", sticky_aim);
        WriteEntry(f, "prediction_enabled", prediction_enabled);
        WriteEntry(f, "prediction_x", prediction_x);
        WriteEntry(f, "prediction_y", prediction_y);
        WriteEntry(f, "smoothing_x", smoothing_x);
        WriteEntry(f, "smoothing_y", smoothing_y);
        WriteEntry(f, "fov_size", fov_size);
        WriteEntry(f, "show_fov", show_fov);

        // esp
        WriteEntry(f, "esp_enabled", esp_enabled);
        WriteEntry(f, "box_esp", box_esp);
        WriteEntry(f, "box_esp_type", box_esp_type);
        WriteEntry(f, "healthbar", healthbar);
        WriteEntry(f, "health_text", health_text);
        WriteEntry(f, "name_esp", name);
        WriteEntry(f, "distance", distance);
        WriteEntry(f, "rig_type", rig_type);
        WriteEntry(f, "tool_esp", tool_esp);
        WriteEntry(f, "aimviewer", aimviewer);
        WriteEntry(f, "skeleton_esp", skeleton_esp);
        WriteEntry(f, "chinahat", chinahat);
        WriteEntry(f, "render_expanded_hitbox", render_expanded_hitbox);
        WriteEntry(f, "hitbox_expander_enabled", hitbox_expander_enabled);
        WriteEntry(f, "hitbox_expander_value", hitbox_expander_value);
        WriteEntry(f, "esp_render_distance", esp_render_distance);
        WriteEntry(f, "chams_enabled", chams_enabled);
        WriteEntry(f, "mesh_chams_enabled", mesh_chams_enabled);
        WriteEntry(f, "union_chams", union_chams);
        WriteEntry(f, "outline_chams", outline_chams);
        WriteEntry(f, "box_fill", box_fill);
        WriteEntry(f, "box_fill_gradient", box_fill_gradient);
        WriteEntry(f, "box_fill_gradient_rotate", box_fill_gradient_rotate);
        WriteEntry(f, "team_check", team_check);
        WriteEntry(f, "esp_preview_3d", esp_preview_3d);
        WriteEntry(f, "skeleton", skeleton);

        // colors
        WriteEntry(f, "box_esp_color", box_esp_color);
        WriteEntry(f, "name_color", name_color);
        WriteEntry(f, "distance_color", distance_color);
        WriteEntry(f, "health_text_color", health_text_color);
        WriteEntry(f, "rig_type_color", rig_type_color);
        WriteEntry(f, "tool_color", tool_color);
        WriteEntry(f, "skeleton_color", skeleton_color);
        WriteEntry(f, "chinahat_color", chinahat_color);
        WriteEntry(f, "chams_color", chams_color);
        WriteEntry(f, "mesh_chams_color", mesh_chams_color);
        WriteEntry(f, "outline_chams_color", outline_chams_color);
        WriteEntry(f, "box_fill_top", box_fill_top);
        WriteEntry(f, "box_fill_bottom", box_fill_bottom);
        WriteEntry(f, "healthbar_color", healthbar_color);

        // misc
        WriteEntry(f, "walkspeed_enabled", walkspeed_enabled);
        WriteEntry(f, "walkspeed_keybind", walkspeed_keybind);
        WriteEntry(f, "walkspeed_value", walkspeed_value);
        WriteEntry(f, "flight_enabled", flight_enabled);
        WriteEntry(f, "flight_keybind", flight_keybind);
        WriteEntry(f, "flight_value", flight_value);
        WriteEntry(f, "noclip_enabled", noclip_enabled);
        WriteEntry(f, "noclip_keybind", noclip_keybind);

        // blade ball
        WriteEntry(f, "blade_ball_auto_parry", blade_ball_auto_parry);
        WriteEntry(f, "blade_ball_ball_esp", blade_ball_ball_esp);
        WriteEntry(f, "blade_ball_parry_distance", blade_ball_parry_distance);
        WriteEntry(f, "blade_ball_parry_height", blade_ball_parry_height);

        // misc features
        WriteEntry(f, "korblox_enabled", korblox_enabled);
        WriteEntry(f, "skybox_changer_enabled", skybox_changer_enabled);
        WriteEntry(f, "skybox_type", skybox_type);
        WriteEntry(f, "rivals_skin_changer_enabled", rivals_skin_changer_enabled);

        // memory chams
        WriteEntry(f, "memory_mesh_chams_enabled", memory_mesh_chams_enabled);
        WriteEntry(f, "memory_union_chams", memory_union_chams);
        WriteEntry(f, "memory_outline_chams", memory_outline_chams);
        WriteEntry(f, "memory_mesh_chams_color", memory_mesh_chams_color);
        WriteEntry(f, "memory_outline_chams_color", memory_outline_chams_color);

        f.close();
        return true;
    }

    bool Load(const std::string& config_name) {
        if (config_name.empty()) return false;
        std::string path = GetConfigDir() + "\\" + config_name + ".cfg";
        std::ifstream f(path);
        if (!f.is_open()) return false;

        std::string line;
        while (std::getline(f, line)) {
            // aimbot
            if (ReadLine(line, "aimbot_enabled", aimbot_enabled)) continue;
            if (ReadLine(line, "aimbot_aim_type", aimbot_aim_type)) continue;
            if (ReadLine(line, "aimbot_keybind", aimbot_keybind)) continue;
            if (ReadLine(line, "aimbot_part", aimbot_part)) continue;
            if (ReadLine(line, "sticky_aim", sticky_aim)) continue;
            if (ReadLine(line, "prediction_enabled", prediction_enabled)) continue;
            if (ReadLine(line, "prediction_x", prediction_x)) continue;
            if (ReadLine(line, "prediction_y", prediction_y)) continue;
            if (ReadLine(line, "smoothing_x", smoothing_x)) continue;
            if (ReadLine(line, "smoothing_y", smoothing_y)) continue;
            if (ReadLine(line, "fov_size", fov_size)) continue;
            if (ReadLine(line, "show_fov", show_fov)) continue;

            // esp
            if (ReadLine(line, "esp_enabled", esp_enabled)) continue;
            if (ReadLine(line, "box_esp", box_esp)) continue;
            if (ReadLine(line, "box_esp_type", box_esp_type)) continue;
            if (ReadLine(line, "healthbar", healthbar)) continue;
            if (ReadLine(line, "health_text", health_text)) continue;
            if (ReadLine(line, "name_esp", name)) continue;
            if (ReadLine(line, "rig_type", rig_type)) continue;
            if (ReadLine(line, "tool_esp", tool_esp)) continue;
            if (ReadLine(line, "aimviewer", aimviewer)) continue;
            if (ReadLine(line, "skeleton_esp", skeleton_esp)) continue;
            if (ReadLine(line, "chinahat", chinahat)) continue;
            if (ReadLine(line, "render_expanded_hitbox", render_expanded_hitbox)) continue;
            if (ReadLine(line, "hitbox_expander_enabled", hitbox_expander_enabled)) continue;
            if (ReadLine(line, "hitbox_expander_value", hitbox_expander_value)) continue;
            if (ReadLine(line, "esp_render_distance", esp_render_distance)) continue;
            if (ReadLine(line, "chams_enabled", chams_enabled)) continue;
            if (ReadLine(line, "mesh_chams_enabled", mesh_chams_enabled)) continue;
            if (ReadLine(line, "union_chams", union_chams)) continue;
            if (ReadLine(line, "outline_chams", outline_chams)) continue;
            if (ReadLine(line, "box_fill", box_fill)) continue;
            if (ReadLine(line, "box_fill_gradient", box_fill_gradient)) continue;
            if (ReadLine(line, "box_fill_gradient_rotate", box_fill_gradient_rotate)) continue;
            if (ReadLine(line, "team_check", team_check)) continue;
            if (ReadLine(line, "esp_preview_3d", esp_preview_3d)) continue;
            if (ReadLine(line, "skeleton", skeleton)) continue;

            // colors
            if (ReadLine(line, "box_esp_color", box_esp_color)) continue;
            if (ReadLine(line, "name_color", name_color)) continue;
            if (ReadLine(line, "distance_color", distance_color)) continue;
            if (ReadLine(line, "health_text_color", health_text_color)) continue;
            if (ReadLine(line, "rig_type_color", rig_type_color)) continue;
            if (ReadLine(line, "tool_color", tool_color)) continue;
            if (ReadLine(line, "skeleton_color", skeleton_color)) continue;
            if (ReadLine(line, "chinahat_color", chinahat_color)) continue;
            if (ReadLine(line, "chams_color", chams_color)) continue;
            if (ReadLine(line, "mesh_chams_color", mesh_chams_color)) continue;
            if (ReadLine(line, "outline_chams_color", outline_chams_color)) continue;
            if (ReadLine(line, "box_fill_top", box_fill_top)) continue;
            if (ReadLine(line, "box_fill_bottom", box_fill_bottom)) continue;
            if (ReadLine(line, "healthbar_color", healthbar_color)) continue;

            // misc
            if (ReadLine(line, "walkspeed_enabled", walkspeed_enabled)) continue;
            if (ReadLine(line, "walkspeed_keybind", walkspeed_keybind)) continue;
            if (ReadLine(line, "walkspeed_value", walkspeed_value)) continue;
            if (ReadLine(line, "flight_enabled", flight_enabled)) continue;
            if (ReadLine(line, "flight_keybind", flight_keybind)) continue;
            if (ReadLine(line, "flight_value", flight_value)) continue;
            if (ReadLine(line, "noclip_enabled", noclip_enabled)) continue;
            if (ReadLine(line, "noclip_keybind", noclip_keybind)) continue;

            // blade ball
            if (ReadLine(line, "blade_ball_auto_parry", blade_ball_auto_parry)) continue;
            if (ReadLine(line, "blade_ball_ball_esp", blade_ball_ball_esp)) continue;
            if (ReadLine(line, "blade_ball_parry_distance", blade_ball_parry_distance)) continue;
            if (ReadLine(line, "blade_ball_parry_height", blade_ball_parry_height)) continue;

            // misc features
            if (ReadLine(line, "korblox_enabled", korblox_enabled)) continue;
            if (ReadLine(line, "skybox_changer_enabled", skybox_changer_enabled)) continue;
            if (ReadLine(line, "skybox_type", skybox_type)) continue;
            if (ReadLine(line, "rivals_skin_changer_enabled", rivals_skin_changer_enabled)) continue;

            // memory chams
            if (ReadLine(line, "memory_mesh_chams_enabled", memory_mesh_chams_enabled)) continue;
            if (ReadLine(line, "memory_union_chams", memory_union_chams)) continue;
            if (ReadLine(line, "memory_outline_chams", memory_outline_chams)) continue;
            if (ReadLine(line, "memory_mesh_chams_color", memory_mesh_chams_color)) continue;
            if (ReadLine(line, "memory_outline_chams_color", memory_outline_chams_color)) continue;
        }

        f.close();
        return true;
    }

    bool Delete(const std::string& config_name) {
        if (config_name.empty()) return false;
        std::string path = GetConfigDir() + "\\" + config_name + ".cfg";
        return DeleteFileA(path.c_str()) != 0;
    }

    bool Rename(const std::string& old_config_name, const std::string& new_config_name) {
        if (old_config_name.empty() || new_config_name.empty()) return false;
        std::string old_path = GetConfigDir() + "\\" + old_config_name + ".cfg";
        std::string new_path = GetConfigDir() + "\\" + new_config_name + ".cfg";
        return MoveFileA(old_path.c_str(), new_path.c_str()) != 0;
    }

    std::vector<std::string> GetConfigList() {
        std::vector<std::string> list;
        EnsureConfigDir();
        std::string dir = GetConfigDir() + "\\*.cfg";
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(dir.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return list;
        do {
            std::string fname = fd.cFileName;
            // strip .cfg extension
            if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".cfg") {
                list.push_back(fname.substr(0, fname.size() - 4));
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
        std::sort(list.begin(), list.end());
        return list;
    }

    void OpenConfigFolder() {
        EnsureConfigDir();
        std::string dir = GetConfigDir();
        ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWDEFAULT);
    }
}