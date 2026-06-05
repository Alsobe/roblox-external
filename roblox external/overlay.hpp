#pragma once

#include <Windows.h>
#include <process.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <chrono>

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
    };

    inline State g_state;

    constexpr int TOGGLE_KEY = VK_INSERT;

    inline DWORD WINAPI input_thread(LPVOID)
    {
        while (true)
        {
            if (GetAsyncKeyState(TOGGLE_KEY) & 1)
            {
                g_state.menu_open = !g_state.menu_open;

                if (g_state.menu_open)
                {
                    SetWindowLong(g_state.window, GWL_EXSTYLE, WS_EX_LAYERED | WS_EX_TOOLWINDOW);
                    SetForegroundWindow(g_state.window);
                }
                else
                {
                    SetWindowLong(g_state.window, GWL_EXSTYLE, WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW);
                }
            }

            ImGuiIO& io = ImGui::GetIO();

            POINT cursor{};
            GetCursorPos(&cursor);
            io.MousePos = ImVec2(static_cast<float>(cursor.x), static_cast<float>(cursor.y));

            io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

            Sleep(1);
        }
        return 0;
    }

    inline bool create_overlay()
    {
        g_state.window = FindWindowA("Chrome_WidgetWin_1", "Discord Overlay");
        if (!g_state.window)
            return false;

        SetWindowLong(g_state.window, GWL_EXSTYLE, WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW);
        SetLayeredWindowAttributes(g_state.window, RGB(0, 0, 0), 255, LWA_ALPHA);

        MARGINS margin = { -1 };
        DwmExtendFrameIntoClientArea(g_state.window, &margin);

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

    inline bool init_imgui()
    {
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.Fonts->AddFontDefault();

        ImGui_ImplWin32_Init(g_state.window);
        ImGui_ImplDX11_Init(g_device, g_context);

        return true;
    }

    // render_ui is defined in main.cpp
    void render_ui();

    inline void run()
    {
        while (!create_overlay())
            Sleep(2000);

        if (!init_device()) return;
        if (!init_imgui())  return;

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
    }
}