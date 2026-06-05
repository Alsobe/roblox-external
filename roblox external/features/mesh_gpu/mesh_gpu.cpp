#include "mesh_gpu.h"

#include "../assetmesh/asset_mesh.h"
#include "../globals.h"
#include "../memory.h"
#include "../offsets.h"

#include <d3dcompiler.h>
#include <cfloat>
#include <cstring>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace meshgpu {

struct Vec3 {
    float x;
    float y;
    float z;
};

struct PrimitiveState {
    float rot[9];
    Vec3 pos;
};

struct GpuVertex {
    float x;
    float y;
    float z;
};

struct FrameCB {
    float view0[4];
    float view1[4];
    float view2[4];
    float view3[4];
};

struct ObjectCB {
    float rot0[4];
    float rot1[4];
    float rot2[4];
    float pos[4];
    float mesh_center[4];
    float mesh_scale[4];
    float color[4];
};

struct FullscreenCB {
    float fill_color[4];
    float outline_color[4];
    float texel_size[2];
    float padding[2];
};

struct GpuMesh {
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    UINT index_count = 0;
};

struct SnapshotDraw {
    uint64_t cache_key = 0;
    const assetmesh::parsed_mesh* mesh = nullptr;
    PrimitiveState prim{};
    Vec3 size{};
    size_t entity_index = 0;
};

static ID3D11Device* g_device = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_ps = nullptr;
static ID3D11PixelShader* g_mask_ps = nullptr;
static ID3D11VertexShader* g_fullscreen_vs = nullptr;
static ID3D11PixelShader* g_fullscreen_fill_ps = nullptr;
static ID3D11PixelShader* g_fullscreen_outline_ps = nullptr;
static ID3D11InputLayout* g_layout = nullptr;
static ID3D11Buffer* g_frame_cb = nullptr;
static ID3D11Buffer* g_object_cb = nullptr;
static ID3D11Buffer* g_fullscreen_cb = nullptr;
static ID3D11BlendState* g_blend = nullptr;
static ID3D11BlendState* g_no_color_blend = nullptr;
static ID3D11RasterizerState* g_rasterizer = nullptr;
static ID3D11RasterizerState* g_wireframe_rasterizer = nullptr;
static ID3D11RasterizerState* g_no_cull_rasterizer = nullptr;
static ID3D11DepthStencilState* g_depth_read = nullptr;
static ID3D11DepthStencilState* g_stencil_write = nullptr;
static ID3D11DepthStencilState* g_stencil_fill = nullptr;
static ID3D11DepthStencilState* g_stencil_outline = nullptr;
static ID3D11SamplerState* g_point_sampler = nullptr;
static ID3D11Texture2D* g_union_mask_texture = nullptr;
static ID3D11RenderTargetView* g_union_mask_rtv = nullptr;
static ID3D11ShaderResourceView* g_union_mask_srv = nullptr;
static UINT g_union_mask_width = 0;
static UINT g_union_mask_height = 0;
static std::unordered_map<uint64_t, GpuMesh> g_gpu_meshes;
static std::unordered_map<uintptr_t, uint64_t> g_part_mesh_keys;
static size_t g_last_drawn_entities = 0;

static ResolvedMeshDraw resolve_asset_mesh(const cache::EspEntity& entity, size_t primitive_index) {
    const uintptr_t part_address = entity.part_addresses[primitive_index];
    if (!part_address) {
        return {};
    }

    auto stable_it = g_part_mesh_keys.find(part_address);
    if (stable_it != g_part_mesh_keys.end()) {
        assetmesh::request_mesh(stable_it->second);
        auto mesh = assetmesh::get_mesh(stable_it->second);
        if (!mesh) {
            return {};
        }
        ResolvedMeshDraw resolved{};
        resolved.cache_key = stable_it->second;
        resolved.mesh = mesh.get();
        return resolved;
    }

    uint64_t asset_id = assetmesh::get_mesh_asset_id_from_part(part_address);
    if (!asset_id && entity.user_id != 0) {
        assetmesh::request_avatar_assets(entity.user_id);
        asset_id = assetmesh::get_mesh_asset_id_for_part(entity.user_id, entity.part_names[primitive_index], entity.is_r15);
        if (!asset_id) {
            asset_id = assetmesh::get_default_asset_for_part(entity.part_names[primitive_index], entity.is_r15);
        }
    }
    if (!asset_id) {
        return {};
    }

    assetmesh::request_mesh(asset_id);
    auto mesh = assetmesh::get_mesh(asset_id);
    if (!mesh) {
        return {};
    }

    g_part_mesh_keys.emplace(part_address, asset_id);

    ResolvedMeshDraw resolved{};
    resolved.cache_key = asset_id;
    resolved.mesh = mesh.get();
    return resolved;
}

static bool read_raw(uint64_t address, void* buffer, size_t size) {
    return ::read_raw(address, buffer, size);
}

static bool read_vec3(uint64_t address, Vec3& out) {
    return read_raw(address, &out, sizeof(out));
}

static bool primitive_intersects_screen(const PrimitiveState& prim, const Vec3& size, const float view[16]) {
    const float hx = size.x * 0.5f;
    const float hy = size.y * 0.5f;
    const float hz = size.z * 0.5f;
    static const float corners[8][3] = {
        {-1.0f, -1.0f, -1.0f}, { 1.0f, -1.0f, -1.0f },
        {-1.0f,  1.0f, -1.0f}, { 1.0f,  1.0f, -1.0f },
        {-1.0f, -1.0f,  1.0f}, { 1.0f, -1.0f,  1.0f },
        {-1.0f,  1.0f,  1.0f}, { 1.0f,  1.0f,  1.0f }
    };

    bool has_front_point = false;
    float min_ndc_x = FLT_MAX;
    float max_ndc_x = -FLT_MAX;
    float min_ndc_y = FLT_MAX;
    float max_ndc_y = -FLT_MAX;

    for (const auto& corner : corners) {
        const float lx = corner[0] * hx;
        const float ly = corner[1] * hy;
        const float lz = corner[2] * hz;
        const Vec3 world = {
            prim.pos.x + prim.rot[0] * lx + prim.rot[1] * ly + prim.rot[2] * lz,
            prim.pos.y + prim.rot[3] * lx + prim.rot[4] * ly + prim.rot[5] * lz,
            prim.pos.z + prim.rot[6] * lx + prim.rot[7] * ly + prim.rot[8] * lz
        };

        const float clip_x = world.x * view[0] + world.y * view[1] + world.z * view[2] + view[3];
        const float clip_y = world.x * view[4] + world.y * view[5] + world.z * view[6] + view[7];
        const float clip_w = world.x * view[12] + world.y * view[13] + world.z * view[14] + view[15];
        if (clip_w <= 0.01f) continue;

        has_front_point = true;
        const float ndc_x = clip_x / clip_w;
        const float ndc_y = clip_y / clip_w;
        min_ndc_x = (std::min)(min_ndc_x, ndc_x);
        max_ndc_x = (std::max)(max_ndc_x, ndc_x);
        min_ndc_y = (std::min)(min_ndc_y, ndc_y);
        max_ndc_y = (std::max)(max_ndc_y, ndc_y);

        if (ndc_x >= -1.0f && ndc_x <= 1.0f && ndc_y >= -1.0f && ndc_y <= 1.0f) {
            return true;
        }
    }

    if (!has_front_point) {
        return false;
    }

    return max_ndc_x >= -1.0f && min_ndc_x <= 1.0f &&
        max_ndc_y >= -1.0f && min_ndc_y <= 1.0f;
}

static void release_union_mask() {
    if (g_union_mask_srv) { g_union_mask_srv->Release(); g_union_mask_srv = nullptr; }
    if (g_union_mask_rtv) { g_union_mask_rtv->Release(); g_union_mask_rtv = nullptr; }
    if (g_union_mask_texture) { g_union_mask_texture->Release(); g_union_mask_texture = nullptr; }
    g_union_mask_width = 0;
    g_union_mask_height = 0;
}

static void release_resources() {
    for (auto& [asset_id, mesh] : g_gpu_meshes) {
        (void)asset_id;
        if (mesh.vb) mesh.vb->Release();
        if (mesh.ib) mesh.ib->Release();
    }
    g_gpu_meshes.clear();
    g_part_mesh_keys.clear();
    release_union_mask();
    if (g_point_sampler) { g_point_sampler->Release(); g_point_sampler = nullptr; }
    if (g_stencil_outline) { g_stencil_outline->Release(); g_stencil_outline = nullptr; }
    if (g_stencil_fill) { g_stencil_fill->Release(); g_stencil_fill = nullptr; }
    if (g_stencil_write) { g_stencil_write->Release(); g_stencil_write = nullptr; }
    if (g_depth_read) { g_depth_read->Release(); g_depth_read = nullptr; }
    if (g_wireframe_rasterizer) { g_wireframe_rasterizer->Release(); g_wireframe_rasterizer = nullptr; }
    if (g_no_cull_rasterizer) { g_no_cull_rasterizer->Release(); g_no_cull_rasterizer = nullptr; }
    if (g_rasterizer) { g_rasterizer->Release(); g_rasterizer = nullptr; }
    if (g_no_color_blend) { g_no_color_blend->Release(); g_no_color_blend = nullptr; }
    if (g_blend) { g_blend->Release(); g_blend = nullptr; }
    if (g_fullscreen_cb) { g_fullscreen_cb->Release(); g_fullscreen_cb = nullptr; }
    if (g_object_cb) { g_object_cb->Release(); g_object_cb = nullptr; }
    if (g_frame_cb) { g_frame_cb->Release(); g_frame_cb = nullptr; }
    if (g_layout) { g_layout->Release(); g_layout = nullptr; }
    if (g_fullscreen_outline_ps) { g_fullscreen_outline_ps->Release(); g_fullscreen_outline_ps = nullptr; }
    if (g_fullscreen_fill_ps) { g_fullscreen_fill_ps->Release(); g_fullscreen_fill_ps = nullptr; }
    if (g_fullscreen_vs) { g_fullscreen_vs->Release(); g_fullscreen_vs = nullptr; }
    if (g_mask_ps) { g_mask_ps->Release(); g_mask_ps = nullptr; }
    if (g_ps) { g_ps->Release(); g_ps = nullptr; }
    if (g_vs) { g_vs->Release(); g_vs = nullptr; }
    g_device = nullptr;
}

static bool ensure_union_mask_target(ID3D11Device* device, UINT width, UINT height) {
    if (!device || width == 0 || height == 0) return false;
    if (g_union_mask_texture && g_union_mask_width == width && g_union_mask_height == height) {
        return true;
    }

    release_union_mask();

    D3D11_TEXTURE2D_DESC tex_desc{};
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_R8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&tex_desc, nullptr, &g_union_mask_texture);
    if (FAILED(hr)) return false;
    hr = device->CreateRenderTargetView(g_union_mask_texture, nullptr, &g_union_mask_rtv);
    if (FAILED(hr)) return false;
    hr = device->CreateShaderResourceView(g_union_mask_texture, nullptr, &g_union_mask_srv);
    if (FAILED(hr)) return false;

    g_union_mask_width = width;
    g_union_mask_height = height;
    return true;
}

static bool ensure_pipeline(ID3D11Device* device) {
    if (!device) return false;
    if (g_device == device &&
        g_vs && g_ps && g_mask_ps && g_fullscreen_vs && g_fullscreen_fill_ps && g_fullscreen_outline_ps &&
        g_layout && g_frame_cb && g_object_cb && g_fullscreen_cb &&
        g_blend && g_no_color_blend &&
        g_rasterizer && g_wireframe_rasterizer && g_no_cull_rasterizer &&
        g_depth_read && g_stencil_write && g_stencil_fill && g_stencil_outline &&
        g_point_sampler) {
        return true;
    }

    release_resources();
    g_device = device;

    static const char* vs_src =
        "cbuffer FrameCB : register(b0) {"
        " float4 view0;"
        " float4 view1;"
        " float4 view2;"
        " float4 view3;"
        "};"
        "cbuffer ObjectCB : register(b1) {"
        " float4 rot0;"
        " float4 rot1;"
        " float4 rot2;"
        " float4 objPos;"
        " float4 meshCenter;"
        " float4 meshScale;"
        " float4 meshColor;"
        "};"
        "struct VSInput { float3 pos : POSITION; };"
        "struct VSOutput { float4 pos : SV_POSITION; float4 color : COLOR0; };"
        "VSOutput main(VSInput input) {"
        " VSOutput o;"
        " float3 local = (input.pos - meshCenter.xyz) * meshScale.xyz;"
        " float3 world;"
        " world.x = objPos.x + dot(local, rot0.xyz);"
        " world.y = objPos.y + dot(local, rot1.xyz);"
        " world.z = objPos.z + dot(local, rot2.xyz);"
        " o.pos.x = world.x * view0.x + world.y * view0.y + world.z * view0.z + view0.w;"
        " o.pos.y = world.x * view1.x + world.y * view1.y + world.z * view1.z + view1.w;"
        " o.pos.z = world.x * view2.x + world.y * view2.y + world.z * view2.z + view2.w;"
        " o.pos.w = world.x * view3.x + world.y * view3.y + world.z * view3.z + view3.w;"
        " o.color = meshColor;"
        " return o;"
        "}";

    static const char* ps_src =
        "struct PSInput { float4 pos : SV_POSITION; float4 color : COLOR0; };"
        "float4 main(PSInput input) : SV_TARGET {"
        " return input.color;"
        "}";

    static const char* mask_ps_src =
        "float4 main() : SV_TARGET {"
        " return float4(1.0, 1.0, 1.0, 1.0);"
        "}";

    static const char* fullscreen_vs_src =
        "struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
        "VSOutput main(uint vertex_id : SV_VertexID) {"
        " VSOutput o;"
        " float2 pos;"
        " if (vertex_id == 0) pos = float2(-1.0, -1.0);"
        " else if (vertex_id == 1) pos = float2(-1.0, 3.0);"
        " else pos = float2(3.0, -1.0);"
        " o.pos = float4(pos, 0.0, 1.0);"
        " o.uv = float2((pos.x + 1.0) * 0.5, 1.0 - ((pos.y + 1.0) * 0.5));"
        " return o;"
        "}";

    static const char* fullscreen_fill_ps_src =
        "cbuffer FullscreenCB : register(b0) {"
        " float4 fillColor;"
        " float4 outlineColor;"
        " float2 texelSize;"
        " float2 padding;"
        "};"
        "float4 main() : SV_TARGET {"
        " return fillColor;"
        "}";

    static const char* fullscreen_outline_ps_src =
        "Texture2D maskTex : register(t0);"
        "SamplerState pointSampler : register(s0);"
        "cbuffer FullscreenCB : register(b0) {"
        " float4 fillColor;"
        " float4 outlineColor;"
        " float2 texelSize;"
        " float2 padding;"
        "};"
        "struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
        "float sample_mask(float2 uv) { return maskTex.SampleLevel(pointSampler, uv, 0).r; }"
        "float4 main(PSInput input) : SV_TARGET {"
        " if (sample_mask(input.uv) > 0.5) discard;"
        " float2 offsets[8] = {"
        "   float2(texelSize.x, 0.0), float2(-texelSize.x, 0.0),"
        "   float2(0.0, texelSize.y), float2(0.0, -texelSize.y),"
        "   float2(texelSize.x, texelSize.y), float2(-texelSize.x, texelSize.y),"
        "   float2(texelSize.x, -texelSize.y), float2(-texelSize.x, -texelSize.y)"
        " };"
        " float found = 0.0;"
        " [unroll] for (int i = 0; i < 8; ++i) {"
        "   found = max(found, sample_mask(input.uv + offsets[i]));"
        "   found = max(found, sample_mask(input.uv + offsets[i] * 2.0));"
        " }"
        " if (found <= 0.5) discard;"
        " return outlineColor;"
        "}";

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* mask_ps_blob = nullptr;
    ID3DBlob* fullscreen_vs_blob = nullptr;
    ID3DBlob* fullscreen_fill_blob = nullptr;
    ID3DBlob* fullscreen_outline_blob = nullptr;
    ID3DBlob* error_blob = nullptr;

    HRESULT hr = D3DCompile(vs_src, std::strlen(vs_src), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vs_blob, &error_blob);
    if (FAILED(hr)) {
        if (error_blob) error_blob->Release();
        return false;
    }
    hr = D3DCompile(ps_src, std::strlen(ps_src), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &ps_blob, &error_blob);
    if (FAILED(hr)) {
        if (vs_blob) vs_blob->Release();
        if (error_blob) error_blob->Release();
        return false;
    }
    hr = D3DCompile(mask_ps_src, std::strlen(mask_ps_src), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &mask_ps_blob, &error_blob);
    if (FAILED(hr)) {
        if (vs_blob) vs_blob->Release();
        if (ps_blob) ps_blob->Release();
        if (error_blob) error_blob->Release();
        return false;
    }
    hr = D3DCompile(fullscreen_vs_src, std::strlen(fullscreen_vs_src), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &fullscreen_vs_blob, &error_blob);
    if (FAILED(hr)) {
        if (vs_blob) vs_blob->Release();
        if (ps_blob) ps_blob->Release();
        if (mask_ps_blob) mask_ps_blob->Release();
        if (error_blob) error_blob->Release();
        return false;
    }
    hr = D3DCompile(fullscreen_fill_ps_src, std::strlen(fullscreen_fill_ps_src), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &fullscreen_fill_blob, &error_blob);
    if (FAILED(hr)) {
        if (vs_blob) vs_blob->Release();
        if (ps_blob) ps_blob->Release();
        if (mask_ps_blob) mask_ps_blob->Release();
        if (fullscreen_vs_blob) fullscreen_vs_blob->Release();
        if (error_blob) error_blob->Release();
        return false;
    }
    hr = D3DCompile(fullscreen_outline_ps_src, std::strlen(fullscreen_outline_ps_src), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &fullscreen_outline_blob, &error_blob);
    if (FAILED(hr)) {
        if (vs_blob) vs_blob->Release();
        if (ps_blob) ps_blob->Release();
        if (mask_ps_blob) mask_ps_blob->Release();
        if (fullscreen_vs_blob) fullscreen_vs_blob->Release();
        if (fullscreen_fill_blob) fullscreen_fill_blob->Release();
        if (error_blob) error_blob->Release();
        return false;
    }

    hr = device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_vs);
    if (FAILED(hr)) return false;
    hr = device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &g_ps);
    if (FAILED(hr)) return false;
    hr = device->CreatePixelShader(mask_ps_blob->GetBufferPointer(), mask_ps_blob->GetBufferSize(), nullptr, &g_mask_ps);
    if (FAILED(hr)) return false;
    hr = device->CreateVertexShader(fullscreen_vs_blob->GetBufferPointer(), fullscreen_vs_blob->GetBufferSize(), nullptr, &g_fullscreen_vs);
    if (FAILED(hr)) return false;
    hr = device->CreatePixelShader(fullscreen_fill_blob->GetBufferPointer(), fullscreen_fill_blob->GetBufferSize(), nullptr, &g_fullscreen_fill_ps);
    if (FAILED(hr)) return false;
    hr = device->CreatePixelShader(fullscreen_outline_blob->GetBufferPointer(), fullscreen_outline_blob->GetBufferSize(), nullptr, &g_fullscreen_outline_ps);
    if (FAILED(hr)) return false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device->CreateInputLayout(layout, 1, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &g_layout);
    vs_blob->Release();
    ps_blob->Release();
    mask_ps_blob->Release();
    fullscreen_vs_blob->Release();
    fullscreen_fill_blob->Release();
    fullscreen_outline_blob->Release();
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb_desc.ByteWidth = sizeof(FrameCB);
    hr = device->CreateBuffer(&cb_desc, nullptr, &g_frame_cb);
    if (FAILED(hr)) return false;
    cb_desc.ByteWidth = sizeof(ObjectCB);
    hr = device->CreateBuffer(&cb_desc, nullptr, &g_object_cb);
    if (FAILED(hr)) return false;
    cb_desc.ByteWidth = sizeof(FullscreenCB);
    hr = device->CreateBuffer(&cb_desc, nullptr, &g_fullscreen_cb);
    if (FAILED(hr)) return false;

    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&blend_desc, &g_blend);
    if (FAILED(hr)) return false;
    blend_desc.RenderTarget[0].BlendEnable = FALSE;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = 0;
    hr = device->CreateBlendState(&blend_desc, &g_no_color_blend);
    if (FAILED(hr)) return false;

    D3D11_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_BACK;
    rasterizer_desc.DepthClipEnable = TRUE;
    rasterizer_desc.SlopeScaledDepthBias = 1.0f;
    rasterizer_desc.DepthBias = 100;
    hr = device->CreateRasterizerState(&rasterizer_desc, &g_rasterizer);
    if (FAILED(hr)) return false;
    rasterizer_desc.SlopeScaledDepthBias = 0.0f;
    rasterizer_desc.DepthBias = 0;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    hr = device->CreateRasterizerState(&rasterizer_desc, &g_no_cull_rasterizer);
    if (FAILED(hr)) return false;
    rasterizer_desc.FillMode = D3D11_FILL_WIREFRAME;
    hr = device->CreateRasterizerState(&rasterizer_desc, &g_wireframe_rasterizer);
    if (FAILED(hr)) return false;

    D3D11_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr = device->CreateDepthStencilState(&depth_desc, &g_depth_read);
    if (FAILED(hr)) return false;

    D3D11_DEPTH_STENCIL_DESC stencil_desc{};
    stencil_desc.DepthEnable = TRUE;
    stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    stencil_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    stencil_desc.StencilEnable = TRUE;
    stencil_desc.StencilReadMask = 0xFF;
    stencil_desc.StencilWriteMask = 0xFF;
    stencil_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    stencil_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    stencil_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    stencil_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    stencil_desc.BackFace = stencil_desc.FrontFace;
    hr = device->CreateDepthStencilState(&stencil_desc, &g_stencil_write);
    if (FAILED(hr)) return false;

    stencil_desc.DepthEnable = FALSE;
    stencil_desc.StencilWriteMask = 0;
    stencil_desc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    stencil_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    stencil_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    stencil_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    stencil_desc.BackFace = stencil_desc.FrontFace;
    hr = device->CreateDepthStencilState(&stencil_desc, &g_stencil_fill);
    if (FAILED(hr)) return false;

    stencil_desc.FrontFace.StencilFunc = D3D11_COMPARISON_NOT_EQUAL;
    stencil_desc.BackFace = stencil_desc.FrontFace;
    hr = device->CreateDepthStencilState(&stencil_desc, &g_stencil_outline);
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampler_desc, &g_point_sampler);
    if (FAILED(hr)) return false;

    return true;
}

static bool ensure_gpu_mesh(ID3D11Device* device, uint64_t cache_key, const assetmesh::parsed_mesh& mesh) {
    auto it = g_gpu_meshes.find(cache_key);
    if (it != g_gpu_meshes.end()) return true;

    std::vector<GpuVertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for (const auto& vertex : mesh.vertices) {
        vertices.push_back({ vertex.position.x, vertex.position.y, vertex.position.z });
    }

    D3D11_BUFFER_DESC vb_desc{};
    vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vb_desc.ByteWidth = (UINT)(vertices.size() * sizeof(GpuVertex));
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vb_data{};
    vb_data.pSysMem = vertices.data();

    D3D11_BUFFER_DESC ib_desc{};
    ib_desc.Usage = D3D11_USAGE_IMMUTABLE;
    ib_desc.ByteWidth = (UINT)(mesh.indices.size() * sizeof(uint32_t));
    ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ib_data{};
    ib_data.pSysMem = mesh.indices.data();

    GpuMesh gpu_mesh{};
    HRESULT hr = device->CreateBuffer(&vb_desc, &vb_data, &gpu_mesh.vb);
    if (FAILED(hr)) return false;
    hr = device->CreateBuffer(&ib_desc, &ib_data, &gpu_mesh.ib);
    if (FAILED(hr)) {
        gpu_mesh.vb->Release();
        return false;
    }

    gpu_mesh.index_count = (UINT)mesh.indices.size();
    g_gpu_meshes[cache_key] = gpu_mesh;
    return true;
}

static bool upload_frame_cb(ID3D11DeviceContext* context, const float view[16]) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context->Map(g_frame_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;
    FrameCB cb{};
    std::memcpy(cb.view0, view + 0, sizeof(float) * 4);
    std::memcpy(cb.view1, view + 4, sizeof(float) * 4);
    std::memcpy(cb.view2, view + 8, sizeof(float) * 4);
    std::memcpy(cb.view3, view + 12, sizeof(float) * 4);
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    context->Unmap(g_frame_cb, 0);
    return true;
}

static bool upload_object_cb(ID3D11DeviceContext* context, const PrimitiveState& prim, const assetmesh::mesh_bounds& bounds, const Vec3& size, const float color[4]) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context->Map(g_object_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;
    ObjectCB cb{};
    cb.rot0[0] = prim.rot[0]; cb.rot0[1] = prim.rot[1]; cb.rot0[2] = prim.rot[2];
    cb.rot1[0] = prim.rot[3]; cb.rot1[1] = prim.rot[4]; cb.rot1[2] = prim.rot[5];
    cb.rot2[0] = prim.rot[6]; cb.rot2[1] = prim.rot[7]; cb.rot2[2] = prim.rot[8];
    cb.pos[0] = prim.pos.x; cb.pos[1] = prim.pos.y; cb.pos[2] = prim.pos.z; cb.pos[3] = 1.0f;
    cb.mesh_center[0] = bounds.center.x; cb.mesh_center[1] = bounds.center.y; cb.mesh_center[2] = bounds.center.z;
    cb.mesh_scale[0] = bounds.size.x > 0.001f ? size.x / bounds.size.x : 1.0f;
    cb.mesh_scale[1] = bounds.size.y > 0.001f ? size.y / bounds.size.y : 1.0f;
    cb.mesh_scale[2] = bounds.size.z > 0.001f ? size.z / bounds.size.z : 1.0f;
    cb.color[0] = color[0];
    cb.color[1] = color[1];
    cb.color[2] = color[2];
    cb.color[3] = color[3];
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    context->Unmap(g_object_cb, 0);
    return true;
}

static bool upload_fullscreen_cb(ID3D11DeviceContext* context, const float fill_color[4], const float outline_color[4], float viewport_width, float viewport_height) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context->Map(g_fullscreen_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;
    FullscreenCB cb{};
    if (fill_color) std::memcpy(cb.fill_color, fill_color, sizeof(float) * 4);
    if (outline_color) std::memcpy(cb.outline_color, outline_color, sizeof(float) * 4);
    cb.texel_size[0] = viewport_width > 0.0f ? 1.0f / viewport_width : 0.0f;
    cb.texel_size[1] = viewport_height > 0.0f ? 1.0f / viewport_height : 0.0f;
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    context->Unmap(g_fullscreen_cb, 0);
    return true;
}

static void build_snapshot(const std::vector<cache::EspEntity>& entities, const float view[16], MeshResolver resolver, std::vector<SnapshotDraw>& out_items, size_t& out_entity_count) {
    out_items.clear();
    out_entity_count = 0;
    if (!resolver) return;

    for (size_t entity_index = 0; entity_index < entities.size(); ++entity_index) {
        const auto& entity = entities[entity_index];
        std::vector<SnapshotDraw> entity_items;
        entity_items.reserve(entity.primitive_count);
        bool entity_visible = false;
        for (size_t i = 0; i < entity.primitive_count; ++i) {
            const ResolvedMeshDraw resolved = resolver(entity, i);
            if (!resolved.cache_key || !resolved.mesh) continue;

            const uintptr_t prim_addr = entity.primitives[i];
            if (!is_valid_address(prim_addr)) continue;

            PrimitiveState prim{};
            if (!read_raw(prim_addr + Offsets::Primitive::Rotation, &prim, sizeof(prim))) continue;
            Vec3 size{};
            if (!read_vec3(prim_addr + Offsets::Primitive::Size, size)) continue;

            SnapshotDraw item{};
            item.cache_key = resolved.cache_key;
            item.mesh = resolved.mesh;
            item.prim = prim;
            item.size = size;
            item.entity_index = entity_index;
            entity_items.push_back(item);
            entity_visible = entity_visible || primitive_intersects_screen(prim, size, view);
        }
        if (entity_visible && !entity_items.empty()) {
            out_items.insert(out_items.end(), entity_items.begin(), entity_items.end());
            ++out_entity_count;
        }
    }
}

static void setup_mesh_pipeline(ID3D11DeviceContext* context, const float view[16], float viewport_width, float viewport_height, ID3D11RasterizerState* rasterizer, ID3D11DepthStencilState* depth_state, UINT stencil_ref, ID3D11BlendState* blend_state, ID3D11PixelShader* pixel_shader) {
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = viewport_width;
    vp.Height = viewport_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);
    context->IASetInputLayout(g_layout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_vs, nullptr, 0);
    context->PSSetShader(pixel_shader, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &g_frame_cb);
    context->VSSetConstantBuffers(1, 1, &g_object_cb);
    context->OMSetBlendState(blend_state, nullptr, 0xffffffff);
    context->RSSetState(rasterizer);
    context->OMSetDepthStencilState(depth_state, stencil_ref);
    upload_frame_cb(context, view);
}

static void draw_snapshot_meshes(ID3D11Device* device, ID3D11DeviceContext* context, const std::vector<SnapshotDraw>& items, const float color[4]) {
    for (const SnapshotDraw& item : items) {
        if (!item.mesh) continue;
        if (!ensure_gpu_mesh(device, item.cache_key, *item.mesh)) continue;
        if (!upload_object_cb(context, item.prim, item.mesh->bounds, item.size, color)) continue;

        auto it = g_gpu_meshes.find(item.cache_key);
        if (it == g_gpu_meshes.end()) continue;
        UINT stride = sizeof(GpuVertex);
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &it->second.vb, &stride, &offset);
        context->IASetIndexBuffer(it->second.ib, DXGI_FORMAT_R32_UINT, 0);
        context->DrawIndexed(it->second.index_count, 0, 0);
    }
}

static void draw_fullscreen(ID3D11DeviceContext* context, float viewport_width, float viewport_height, ID3D11DepthStencilState* depth_state, UINT stencil_ref, ID3D11PixelShader* pixel_shader, const float fill_color[4], const float outline_color[4], ID3D11ShaderResourceView* mask_srv) {
    if (!upload_fullscreen_cb(context, fill_color, outline_color, viewport_width, viewport_height)) {
        return;
    }

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_fullscreen_vs, nullptr, 0);
    context->PSSetShader(pixel_shader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &g_fullscreen_cb);
    context->RSSetState(g_no_cull_rasterizer);
    context->OMSetBlendState(g_blend, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(depth_state, stencil_ref);

    ID3D11ShaderResourceView* null_srv = nullptr;
    if (mask_srv) {
        context->PSSetShaderResources(0, 1, &mask_srv);
        context->PSSetSamplers(0, 1, &g_point_sampler);
    }

    context->Draw(3, 0);

    if (mask_srv) {
        context->PSSetShaderResources(0, 1, &null_srv);
    }
}

static size_t render_impl(const std::vector<cache::EspEntity>& entities, const float view[16], float viewport_width, float viewport_height, ID3D11Device* device, ID3D11DeviceContext* context, const float color[4], MeshResolver resolver, bool wireframe) {
    g_last_drawn_entities = 0;
    if (!device || !context || !resolver) return 0;

    std::vector<SnapshotDraw> snapshot;
    size_t entity_count = 0;
    build_snapshot(entities, view, resolver, snapshot, entity_count);
    if (snapshot.empty()) return 0;

    if (!ensure_pipeline(device)) return 0;

    setup_mesh_pipeline(
        context,
        view,
        viewport_width,
        viewport_height,
        wireframe ? g_wireframe_rasterizer : g_rasterizer,
        g_depth_read,
        0,
        g_blend,
        g_ps);
    draw_snapshot_meshes(device, context, snapshot, color);

    g_last_drawn_entities = entity_count;
    return entity_count;
}

size_t render_with_resolver(const std::vector<cache::EspEntity>& entities, const float view[16], float viewport_width, float viewport_height, ID3D11Device* device, ID3D11DeviceContext* context, const float color[4], MeshResolver resolver) {
    return render_impl(entities, view, viewport_width, viewport_height, device, context, color, resolver, false);
}

size_t render_wireframe_with_resolver(const std::vector<cache::EspEntity>& entities, const float view[16], float viewport_width, float viewport_height, ID3D11Device* device, ID3D11DeviceContext* context, const float color[4], MeshResolver resolver) {
    return render_impl(entities, view, viewport_width, viewport_height, device, context, color, resolver, true);
}

size_t render_union_fill_with_resolver(const std::vector<cache::EspEntity>& entities, const float view[16], float viewport_width, float viewport_height, ID3D11Device* device, ID3D11DeviceContext* context, const float color[4], MeshResolver resolver) {
    g_last_drawn_entities = 0;
    if (!device || !context || !resolver) return 0;

    std::vector<SnapshotDraw> snapshot;
    size_t entity_count = 0;
    build_snapshot(entities, view, resolver, snapshot, entity_count);
    if (snapshot.empty()) return 0;

    if (!ensure_pipeline(device)) return 0;

    ID3D11RenderTargetView* current_rtv = nullptr;
    ID3D11DepthStencilView* current_dsv = nullptr;
    context->OMGetRenderTargets(1, &current_rtv, &current_dsv);
    if (!current_rtv || !current_dsv) {
        if (current_rtv) current_rtv->Release();
        if (current_dsv) current_dsv->Release();
        return 0;
    }

    context->ClearDepthStencilView(current_dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
    context->OMSetRenderTargets(1, &current_rtv, current_dsv);
    setup_mesh_pipeline(context, view, viewport_width, viewport_height, g_no_cull_rasterizer, g_stencil_write, 1, g_no_color_blend, g_mask_ps);
    static const float kMaskColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    draw_snapshot_meshes(device, context, snapshot, kMaskColor);

    context->OMSetRenderTargets(1, &current_rtv, current_dsv);
    draw_fullscreen(context, viewport_width, viewport_height, g_stencil_fill, 1, g_fullscreen_fill_ps, color, nullptr, nullptr);

    current_rtv->Release();
    current_dsv->Release();

    g_last_drawn_entities = entity_count;
    return entity_count;
}

size_t render_union_outline_with_resolver(const std::vector<cache::EspEntity>& entities, const float view[16], float viewport_width, float viewport_height, ID3D11Device* device, ID3D11DeviceContext* context, const float color[4], MeshResolver resolver) {
    g_last_drawn_entities = 0;
    if (!device || !context || !resolver) return 0;

    std::vector<SnapshotDraw> snapshot;
    size_t entity_count = 0;
    build_snapshot(entities, view, resolver, snapshot, entity_count);
    if (snapshot.empty()) return 0;

    if (!ensure_pipeline(device)) return 0;
    if (!ensure_union_mask_target(device, (UINT)viewport_width, (UINT)viewport_height)) return 0;

    ID3D11RenderTargetView* current_rtv = nullptr;
    ID3D11DepthStencilView* current_dsv = nullptr;
    context->OMGetRenderTargets(1, &current_rtv, &current_dsv);
    if (!current_rtv || !current_dsv) {
        if (current_rtv) current_rtv->Release();
        if (current_dsv) current_dsv->Release();
        return 0;
    }

    const float clear_mask[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearRenderTargetView(g_union_mask_rtv, clear_mask);
    context->ClearDepthStencilView(current_dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);

    context->OMSetRenderTargets(1, &g_union_mask_rtv, current_dsv);
    setup_mesh_pipeline(context, view, viewport_width, viewport_height, g_no_cull_rasterizer, g_stencil_write, 1, nullptr, g_mask_ps);
    static const float kMaskColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    draw_snapshot_meshes(device, context, snapshot, kMaskColor);

    context->OMSetRenderTargets(1, &current_rtv, current_dsv);
    static const float kTransparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    draw_fullscreen(context, viewport_width, viewport_height, g_stencil_outline, 1, g_fullscreen_outline_ps, kTransparent, color, g_union_mask_srv);

    current_rtv->Release();
    current_dsv->Release();

    g_last_drawn_entities = entity_count;
    return entity_count;
}

size_t render(const std::vector<cache::EspEntity>& entities, const float view[16], float viewport_width, float viewport_height, ID3D11Device* device, ID3D11DeviceContext* context, const float color[4]) {
    return render_with_resolver(entities, view, viewport_width, viewport_height, device, context, color, &resolve_asset_mesh);
}

size_t get_last_drawn_entities() {
    return g_last_drawn_entities;
}

void shutdown() {
    release_resources();
}

} // namespace meshgpu
