#pragma once

#include <Windows.h>
#include <process.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <chrono>
#include <cstdio>

#include "memory.h"
#include "globals.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace discord_overlay
{
    inline ID3D11Device*           g_device        = nullptr;
    inline ID3D11DeviceContext*    g_context       = nullptr;
    inline ID3D11RenderTargetView* g_render_target = nullptr;

    struct State
    {
        HWND                  window      = nullptr;
        IDXGISwapChain*       swap_chain  = nullptr;
        ID3D11RenderTargetView* rtv       = nullptr;
        D3D_FEATURE_LEVEL     feature_level{};
        bool                  menu_open   = false;
        bool                  centered_once = false;
        HWND                  prev_foreground = nullptr;
    };

    inline State g_state;

    // standalone overlay window bookkeeping
    inline HINSTANCE   g_hinstance   = nullptr;
    inline ATOM        g_class_atom  = 0;
    inline const char* g_class_name  = "RBXOverlayWnd";
    inline const char* g_window_name = "RBXOverlay";

    // menu toggle key lives in globals (menu_toggle_keybind) so the keybinds tab
    // can rebind it at runtime. defaults to VK_HOME.

    // finds roblox's main window so we can hand input back to it
    struct FindWndData { DWORD pid; HWND result; };

    inline BOOL CALLBACK find_wnd_proc(HWND hwnd, LPARAM lparam)
    {
        FindWndData* d = reinterpret_cast<FindWndData*>(lparam);
        DWORD wnd_pid = 0;
        GetWindowThreadProcessId(hwnd, &wnd_pid);
        if (wnd_pid == d->pid && GetWindow(hwnd, GW_OWNER) == nullptr && IsWindowVisible(hwnd))
        {
            d->result = hwnd;
            return FALSE;
        }
        return TRUE;
    }

    inline HWND find_roblox_window()
    {
        DWORD pid = (DWORD)mem::process_id.load();
        if (!pid) return nullptr;
        FindWndData d{ pid, nullptr };
        EnumWindows(find_wnd_proc, reinterpret_cast<LPARAM>(&d));
        return d.result;
    }

    // when click_through is true the overlay ignores the mouse entirely so you can
    // keep playing. when the menu is open we drop WS_EX_TRANSPARENT so it can be used.
    inline void set_clickthrough(bool click_through)
    {
        if (!g_state.window) return;

        LONG_PTR ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
        if (click_through) ex |= WS_EX_TRANSPARENT;

        SetWindowLongPtr(g_state.window, GWL_EXSTYLE, ex);
        SetWindowPos(g_state.window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    inline int VkToImGuiKey(int vk) {
        if (vk >= '0' && vk <= '9') return vk;
        if (vk >= 'A' && vk <= 'Z') return vk;
        if (vk == VK_BACK) return ImGuiKey_Backspace;
        if (vk == VK_DELETE) return ImGuiKey_Delete;
        if (vk == VK_RETURN) return ImGuiKey_Enter;
        if (vk == VK_ESCAPE) return ImGuiKey_Escape;
        if (vk == VK_TAB) return ImGuiKey_Tab;
        if (vk == VK_LEFT) return ImGuiKey_LeftArrow;
        if (vk == VK_RIGHT) return ImGuiKey_RightArrow;
        if (vk == VK_UP) return ImGuiKey_UpArrow;
        if (vk == VK_DOWN) return ImGuiKey_DownArrow;
        if (vk == VK_HOME) return ImGuiKey_Home;
        if (vk == VK_END) return ImGuiKey_End;
        if (vk == VK_SPACE) return ImGuiKey_Space;
        if (vk == VK_DECIMAL) return ImGuiKey_KeypadDecimal;
        if (vk == VK_OEM_MINUS) return ImGuiKey_Minus;
        if (vk == VK_OEM_PLUS) return ImGuiKey_Equal;
        if (vk == VK_OEM_1) return ImGuiKey_Semicolon;
        if (vk == VK_OEM_7) return ImGuiKey_Apostrophe;
        if (vk == VK_OEM_COMMA) return ImGuiKey_Comma;
        if (vk == VK_OEM_PERIOD) return ImGuiKey_Period;
        if (vk == VK_OEM_2) return ImGuiKey_Slash;
        if (vk == VK_OEM_4) return ImGuiKey_LeftBracket;
        if (vk == VK_OEM_5) return ImGuiKey_Backslash;
        if (vk == VK_OEM_6) return ImGuiKey_RightBracket;
        if (vk == VK_OEM_3) return ImGuiKey_GraveAccent;
        if (vk == VK_LSHIFT || vk == VK_RSHIFT) return ImGuiKey_ModShift;
        return ImGuiKey_None;
    }

    inline bool IsVkPrintable(int vk) {
        if (vk >= '0' && vk <= '9') return true;
        if (vk >= 'A' && vk <= 'Z') return true;
        if (vk == VK_SPACE) return true;
        if (vk == VK_OEM_MINUS || vk == VK_OEM_PLUS || vk == VK_OEM_1) return true;
        if (vk == VK_OEM_7 || vk == VK_OEM_COMMA || vk == VK_OEM_PERIOD) return true;
        if (vk == VK_OEM_2 || vk == VK_OEM_3 || vk == VK_OEM_4) return true;
        if (vk == VK_OEM_5 || vk == VK_OEM_6) return true;
        return false;
    }

    inline char VkToChar(int vk, bool shift) {
        if (vk >= '0' && vk <= '9') {
            if (!shift) return (char)vk;
            const char* shifted = ")!@#$%^&*(";
            return shifted[vk - '0'];
        }
        if (vk >= 'A' && vk <= 'Z') {
            char c = (char)(vk + 32);
            if (shift) c = (char)vk;
            return c;
        }
        if (vk == VK_SPACE) return ' ';
        if (shift) {
            if (vk == VK_OEM_MINUS) return '_';
            if (vk == VK_OEM_PLUS) return '+';
            if (vk == VK_OEM_1) return ':';
            if (vk == VK_OEM_7) return '"';
            if (vk == VK_OEM_COMMA) return '<';
            if (vk == VK_OEM_PERIOD) return '>';
            if (vk == VK_OEM_2) return '?';
            if (vk == VK_OEM_3) return '~';
            if (vk == VK_OEM_4) return '{';
            if (vk == VK_OEM_5) return '|';
            if (vk == VK_OEM_6) return '}';
        } else {
            if (vk == VK_OEM_MINUS) return '-';
            if (vk == VK_OEM_PLUS) return '=';
            if (vk == VK_OEM_1) return ';';
            if (vk == VK_OEM_7) return '\'';
            if (vk == VK_OEM_COMMA) return ',';
            if (vk == VK_OEM_PERIOD) return '.';
            if (vk == VK_OEM_2) return '/';
            if (vk == VK_OEM_3) return '`';
            if (vk == VK_OEM_4) return '[';
            if (vk == VK_OEM_5) return '\\';
            if (vk == VK_OEM_6) return ']';
        }
        return 0;
    }

    inline DWORD WINAPI input_thread(LPVOID)
    {
        static bool s_prev_keys[256] = {};

        while (true)
        {
            if (menu_toggle_keybind != 0 && (GetAsyncKeyState(menu_toggle_keybind) & 1))
            {
                g_state.menu_open = !g_state.menu_open;

                if (g_state.menu_open)
                {
                    // remember whatever the user was using so we can hand focus back
                    HWND prev = GetForegroundWindow();
                    if (prev && prev != g_state.window)
                        g_state.prev_foreground = prev;

                    SetForegroundWindow(g_state.window);
                }
                else
                {
                    // give focus back to roblox (or whatever was focused before),
                    // otherwise the game keeps ignoring your mouse and keyboard
                    HWND restore = g_state.prev_foreground;
                    if (!restore || !IsWindow(restore))
                        restore = find_roblox_window();

                    if (restore && IsWindow(restore))
                    {
                        SetForegroundWindow(restore);
                        SetActiveWindow(restore);
                    }
                    g_state.prev_foreground = nullptr;
                }
            }

            ImGuiIO& io = ImGui::GetIO();

            POINT cursor{};
            GetCursorPos(&cursor);
            io.MousePos = ImVec2(static_cast<float>(cursor.x), static_cast<float>(cursor.y));

            io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

            if (g_state.menu_open) {
                bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

                io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
                io.AddKeyEvent(ImGuiMod_Shift, shift);

                for (int vk = 1; vk < 256; ++vk) {
                    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == menu_toggle_keybind) continue;
                    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    bool was_down = s_prev_keys[vk];
                    if (down != was_down) {
                        ImGuiKey imgui_key = (ImGuiKey)VkToImGuiKey(vk);
                        if (imgui_key != ImGuiKey_None) {
                            io.AddKeyEvent(imgui_key, down);
                        }
                        if (down && !was_down && IsVkPrintable(vk)) {
                            char c = VkToChar(vk, shift);
                            if (ctrl && (c == 'c' || c == 'C' || c == 'v' || c == 'V' || c == 'a' || c == 'A' || c == 'x' || c == 'X')) {
                                // let ctrl shortcuts pass through
                            } else if (c != 0) {
                                io.AddInputCharacter((unsigned int)c);
                            }
                        }
                    }
                    s_prev_keys[vk] = down;
                }
            }

            Sleep(1);
        }
        return 0;
    }

    inline LRESULT WINAPI wnd_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return true;

        switch (msg)
        {
        case WM_SIZE:
            if (g_state.swap_chain && wParam != SIZE_MINIMIZED)
            {
                if (g_state.rtv) { g_state.rtv->Release(); g_state.rtv = nullptr; g_render_target = nullptr; }

                g_state.swap_chain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                                  DXGI_FORMAT_UNKNOWN, 0);

                ID3D11Texture2D* back_buffer = nullptr;
                g_state.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
                if (back_buffer)
                {
                    g_device->CreateRenderTargetView(back_buffer, nullptr, &g_state.rtv);
                    g_render_target = g_state.rtv;
                    back_buffer->Release();
                }
            }
            return 0;

        case WM_DESTROY:
            g_state.window = nullptr;
            PostQuitMessage(0);
            return 0;

        // never let the overlay steal focus by being clicked on
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        }

        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }

    // creates our own transparent click-through overlay window covering every monitor.
    // no discord required.
    inline bool create_overlay()
    {
        // match roblox's physical pixels, otherwise windows scales our window on
        // high-dpi displays and the esp lands in the wrong place
        SetProcessDPIAware();

        g_hinstance = GetModuleHandleA(nullptr);

        WNDCLASSEXA wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = wnd_proc;
        wc.hInstance     = g_hinstance;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = g_class_name;

        g_class_atom = RegisterClassExA(&wc);
        if (!g_class_atom)
            return false;

        // cover the primary monitor starting at 0,0 - this matches the geometry the
        // discord overlay had, so the esp screen-space math stays correct.
        // (if roblox lives on a second monitor, switch these to SM_XVIRTUALSCREEN /
        //  SM_YVIRTUALSCREEN / SM_CXVIRTUALSCREEN / SM_CYVIRTUALSCREEN)
        int vx = 0;
        int vy = 0;
        int vw = GetSystemMetrics(SM_CXSCREEN);
        int vh = GetSystemMetrics(SM_CYSCREEN);

        g_state.window = CreateWindowExA(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            g_class_name, g_window_name,
            WS_POPUP,
            vx, vy, vw, vh,
            nullptr, nullptr, g_hinstance, nullptr);

        if (!g_state.window)
            return false;

        SetLayeredWindowAttributes(g_state.window, RGB(0, 0, 0), 255, LWA_ALPHA);

        // extend the dwm frame across the whole client area so the swapchain's
        // alpha=0 clear actually shows through as real transparency
        MARGINS margin = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(g_state.window, &margin);

        ShowWindow(g_state.window, SW_SHOWNOACTIVATE);
        UpdateWindow(g_state.window);
        SetWindowPos(g_state.window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        return true;
    }

    inline bool init_device()
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.RefreshRate.Numerator   = 60;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        desc.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count                   = 1;
        desc.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount                        = 2;
        desc.OutputWindow                       = g_state.window;
        desc.Windowed                           = TRUE;
        desc.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;
        desc.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

        D3D_FEATURE_LEVEL requested_levels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0
        };

        D3D_FEATURE_LEVEL achieved_level{};
        ID3D11Device* device  = nullptr;
        ID3D11DeviceContext* context = nullptr;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            requested_levels, _countof(requested_levels),
            D3D11_SDK_VERSION, &desc, &g_state.swap_chain,
            &device, &achieved_level, &context
        );

        if (FAILED(hr))
            return false;

        g_state.feature_level = achieved_level;
        g_device  = device;
        g_context = context;

        ID3D11Texture2D* back_buffer = nullptr;
        g_state.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));

        if (back_buffer)
        {
            device->CreateRenderTargetView(back_buffer, nullptr, &g_state.rtv);
            g_render_target = g_state.rtv;
            back_buffer->Release();
        }

        return true;
    }

    // glassy red theme - translucent panels so the game shows through, red accents
    inline void apply_theme()
    {
        ImGuiStyle& s = ImGui::GetStyle();

        s.WindowRounding    = 10.0f;
        s.ChildRounding     = 8.0f;
        s.FrameRounding     = 6.0f;
        s.PopupRounding     = 8.0f;
        s.ScrollbarRounding = 8.0f;
        s.GrabRounding      = 6.0f;
        s.TabRounding       = 8.0f;

        s.WindowBorderSize  = 1.0f;
        s.ChildBorderSize   = 1.0f;
        s.PopupBorderSize   = 1.0f;
        s.FrameBorderSize   = 1.0f;
        s.TabBorderSize     = 0.0f;

        s.WindowPadding     = ImVec2(16, 16);
        s.FramePadding      = ImVec2(14, 8);   // wider tabs so labels aren't clipped
        s.ItemSpacing       = ImVec2(12, 10);
        s.ItemInnerSpacing  = ImVec2(10, 8);
        s.CellPadding       = ImVec2(8, 6);
        s.ScrollbarSize     = 14.0f;
        s.GrabMinSize       = 12.0f;
        s.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
        s.SeparatorTextBorderSize = 2.0f;
        s.SeparatorTextPadding    = ImVec2(20, 6);

        ImVec4* c = s.Colors;

        const ImVec4 red        = ImVec4(0.86f, 0.13f, 0.24f, 1.00f);
        const ImVec4 red_dim    = ImVec4(0.86f, 0.13f, 0.24f, 0.55f);
        const ImVec4 red_soft   = ImVec4(0.86f, 0.13f, 0.24f, 0.28f);
        const ImVec4 glass      = ImVec4(0.05f, 0.05f, 0.07f, 0.82f); // translucent = glassy
        const ImVec4 glass_deep = ImVec4(0.03f, 0.03f, 0.05f, 0.90f);
        const ImVec4 glass_soft = ImVec4(0.10f, 0.10f, 0.13f, 0.60f);

        c[ImGuiCol_Text]                 = ImVec4(0.94f, 0.94f, 0.96f, 1.00f);
        c[ImGuiCol_TextDisabled]         = ImVec4(0.45f, 0.45f, 0.50f, 1.00f);
        c[ImGuiCol_WindowBg]             = glass;
        c[ImGuiCol_ChildBg]              = ImVec4(0.08f, 0.08f, 0.10f, 0.45f);
        c[ImGuiCol_PopupBg]              = glass_deep;
        c[ImGuiCol_Border]               = ImVec4(0.86f, 0.13f, 0.24f, 0.35f);
        c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

        c[ImGuiCol_FrameBg]              = glass_soft;
        c[ImGuiCol_FrameBgHovered]       = red_soft;
        c[ImGuiCol_FrameBgActive]        = red_dim;

        c[ImGuiCol_TitleBg]              = glass_deep;
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.16f, 0.04f, 0.07f, 0.95f);
        c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.05f, 0.05f, 0.07f, 0.60f);
        c[ImGuiCol_MenuBarBg]            = glass_deep;

        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.03f, 0.03f, 0.05f, 0.40f);
        c[ImGuiCol_ScrollbarGrab]        = red_soft;
        c[ImGuiCol_ScrollbarGrabHovered] = red_dim;
        c[ImGuiCol_ScrollbarGrabActive]  = red;

        c[ImGuiCol_CheckMark]            = red;
        c[ImGuiCol_SliderGrab]           = red_dim;
        c[ImGuiCol_SliderGrabActive]     = red;

        c[ImGuiCol_Button]               = glass_soft;
        c[ImGuiCol_ButtonHovered]        = red_soft;
        c[ImGuiCol_ButtonActive]         = red_dim;

        c[ImGuiCol_Header]               = red_soft;
        c[ImGuiCol_HeaderHovered]        = red_dim;
        c[ImGuiCol_HeaderActive]         = red;

        c[ImGuiCol_Separator]            = ImVec4(0.86f, 0.13f, 0.24f, 0.25f);
        c[ImGuiCol_SeparatorHovered]     = red_dim;
        c[ImGuiCol_SeparatorActive]      = red;

        c[ImGuiCol_ResizeGrip]           = red_soft;
        c[ImGuiCol_ResizeGripHovered]    = red_dim;
        c[ImGuiCol_ResizeGripActive]     = red;

        c[ImGuiCol_Tab]                  = ImVec4(0.10f, 0.05f, 0.07f, 0.80f);
        c[ImGuiCol_TabHovered]           = red_dim;
        c[ImGuiCol_TabActive]            = ImVec4(0.35f, 0.07f, 0.13f, 0.95f);
        c[ImGuiCol_TabUnfocused]         = ImVec4(0.08f, 0.04f, 0.06f, 0.80f);
        c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.22f, 0.05f, 0.09f, 0.90f);

        c[ImGuiCol_PlotLines]            = red;
        c[ImGuiCol_PlotLinesHovered]     = ImVec4(1.00f, 0.35f, 0.45f, 1.00f);
        c[ImGuiCol_PlotHistogram]        = red;
        c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.35f, 0.45f, 1.00f);

        c[ImGuiCol_TextSelectedBg]       = red_dim;
        c[ImGuiCol_DragDropTarget]       = red;
        c[ImGuiCol_NavHighlight]         = red;
        c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.45f);
    }

    inline bool init_imgui()
    {
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        // load a modern system font instead of imgui's tiny bitmap default.
        // segoe ui ships with every supported version of windows; fall back to
        // tahoma, then the built-in font, so this can never leave us fontless.
        {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 2;
            cfg.PixelSnapH  = true;

            const char* candidates[] = {
                "C:\\Windows\\Fonts\\segoeui.ttf",
                "C:\\Windows\\Fonts\\tahoma.ttf",
                "C:\\Windows\\Fonts\\arial.ttf",
            };

            ImFont* loaded = nullptr;
            for (const char* path : candidates) {
                // check the file exists first - AddFontFromFileTTF asserts in debug builds
                DWORD attrs = GetFileAttributesA(path);
                if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                loaded = io.Fonts->AddFontFromFileTTF(path, 18.0f, &cfg);
                if (loaded) break;
            }
            if (!loaded) io.Fonts->AddFontDefault();
        }

        apply_theme();

        ImGui_ImplWin32_Init(g_state.window);
        ImGui_ImplDX11_Init(g_device, g_context);

        return true;
    }

    // render_ui is defined in main.cpp
    void render_ui();

    inline void run()
    {
        if (!create_overlay()) { LogLine("failed to create overlay window"); return; }
        if (!init_device())    { LogLine("failed to create d3d11 device"); return; }
        if (!init_imgui())     { LogLine("failed to init imgui"); return; }

        LogLine("overlay ready");

        CreateThread(nullptr, 0, input_thread, nullptr, 0, nullptr);

        while (g_state.window)
        {
            MSG msg{};
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) return;
            }

            if (!g_state.window) break;

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            render_ui();

            ImGui::Render();

            // only capture the mouse where the menu actually is. everywhere else stays
            // click-through so roblox and your other apps keep working while it's open.
            {
                static bool s_click_through = true;
                bool want_mouse = g_state.menu_open && ImGui::GetIO().WantCaptureMouse;
                bool desired = !want_mouse;
                if (desired != s_click_through)
                {
                    set_clickthrough(desired);
                    s_click_through = desired;
                }
            }

            constexpr float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            g_context->OMSetRenderTargets(1, &g_state.rtv, nullptr);
            g_context->ClearRenderTargetView(g_state.rtv, clear);

            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_state.swap_chain->Present(0, 0);
        }
    }

    inline void shutdown()
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        if (g_state.rtv)        g_state.rtv->Release();
        if (g_state.swap_chain) g_state.swap_chain->Release();
        if (g_context)          g_context->Release();
        if (g_device)           g_device->Release();

        if (g_state.window) { DestroyWindow(g_state.window); g_state.window = nullptr; }
        if (g_class_atom)   { UnregisterClassA(g_class_name, g_hinstance); g_class_atom = 0; }
    }
}