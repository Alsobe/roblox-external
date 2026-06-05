#pragma once
#include <cstdint>

inline bool aimbot_enabled = false;
inline int aimbot_aim_type = 1;
inline int aimbot_keybind = 0;
inline int aimbot_part = 0;
inline bool sticky_aim = false;
inline bool prediction_enabled = false;
inline float prediction_x = 10.0f;
inline float prediction_y = 10.0f;
inline float smoothing_x = 4.0f;
inline float smoothing_y = 4.0f;
inline float fov_size = 150.0f;
inline bool show_fov = false;

inline bool esp_enabled = false;
inline bool box_esp = false;
inline int box_esp_type = 0;
inline bool healthbar = false;
inline bool health_text = false;
inline bool name = false;
inline bool distance = false;
inline bool rig_type = false;
inline bool tool_esp = false;
inline bool aimviewer = false;
inline bool skeleton_esp = false;
inline bool chinahat = false;
inline bool render_expanded_hitbox = false;
inline bool hitbox_expander_enabled = false;
inline float hitbox_expander_value = 10.0f;
inline float esp_render_distance = 0.0f;
inline bool chams_enabled = false;
inline bool mesh_chams_enabled = false;
inline bool union_chams = true;
inline bool outline_chams = false;

inline bool box_fill = false;
inline bool box_fill_gradient = false;
inline bool box_fill_gradient_rotate = false;

inline uintptr_t g_base_address = 0;

inline float box_esp_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
inline float name_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
inline float distance_color[4] = { 0.7f, 0.7f, 0.7f, 1.0f };
inline float health_text_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
inline float rig_type_color[4] = { 0.8f, 0.8f, 0.8f, 1.0f };
inline float tool_color[4] = { 0.9f, 0.9f, 0.5f, 1.0f };
inline float skeleton_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
inline float chinahat_color[4] = { 0.0f, 0.8f, 1.0f, 1.0f };
inline float chams_color[4] = { 1.0f, 0.0f, 0.0f, 0.6f };
inline float mesh_chams_color[4] = { 0.0f, 1.0f, 0.0f, 0.6f };
inline float outline_chams_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
inline float box_fill_top[4] = { 0.0f, 0.0f, 0.0f, 0.4f };
inline float box_fill_bottom[4] = { 0.5f, 0.0f, 0.5f, 0.4f };
inline float healthbar_color[4] = { 0.0f, 1.0f, 0.0f, 1.0f };

inline bool walkspeed_enabled = false;
inline int walkspeed_keybind = 0;
inline float walkspeed_value = 30.0f;

inline bool flight_enabled = false;
inline int flight_keybind = 0;
inline float flight_value = 50.0f;

inline bool noclip_enabled = false;
inline int noclip_keybind = 0;

inline bool blade_ball_auto_parry = false;
inline bool blade_ball_ball_esp = false;
inline float blade_ball_parry_distance = 12.0f;
inline float blade_ball_parry_height = 6.0f;

inline bool korblox_enabled = false;

inline bool skybox_changer_enabled = false;
inline int skybox_type = 0;
inline char skybox_debug_msg[256]{};

inline bool rivals_skin_changer_enabled = false;

inline bool memory_mesh_chams_enabled = false;
inline float memory_mesh_chams_color[4] = { 1.0f, 0.0f, 0.0f, 0.6f };
inline bool memory_union_chams = true;
inline bool memory_outline_chams = false;
inline float memory_outline_chams_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

inline bool esp_preview_3d = false;
inline bool skeleton = false;
inline float menu_alpha = 1.0f;

inline bool menu_open = false;

struct {
    bool centered_once = false;
} inline g_state;

namespace theme {
    inline float menu_size[2] = { 520.0f, 480.0f };
    inline float topbar_h = 30.0f;
    inline float out_thick = 1.0f;
    inline unsigned int topbar_grad_t = 0xFF1A1A2E;
    inline unsigned int topbar_grad_b = 0xFF16162B;
    inline unsigned int topbar_inline = 0xFF2A2A4A;
    inline unsigned int accent_border = 0xFF6C63FF;
    inline unsigned int menu_text = 0xFFD4D4E8;
    inline unsigned int col_alpha(unsigned int c, int a) {
        return (c & 0x00FFFFFF) | (((unsigned int)a << 24));
    }
}

inline bool box_fade = false;
inline float box_fade_timer = 10.0f;

inline float health_bar_width = 3.0f;
inline float health_bar_gap = 3.0f;

inline float text_spacing = 2.0f;

inline int text_font = 0;

inline int max_distance = 2000;

inline bool team_check = false;

inline bool alive_check = true;

inline bool show_name = true;

inline bool show_distance = true;

inline bool show_health = true;

inline bool show_tool = true;

inline bool show_rig = true;

inline bool show_skeleton = true;

inline bool show_box = true;

inline bool show_chams = true;

inline bool show_mesh_chams = true;

inline bool show_chinahat = true;

inline bool show_aimbot_fov = true;

inline bool show_expanded_hitbox = true;

inline bool show_aim_viewer = true;