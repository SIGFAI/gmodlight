#include "overlay.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstring>

#include "MinHook.h"
#include "log.h"
#include "shm.h"

namespace gml::overlay {
namespace {

using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

Present_t oPresent = nullptr;
Present1_t oPresent1 = nullptr;
ResizeBuffers_t oResizeBuffers = nullptr;
Callbacks gCb;
std::atomic<bool> gVisible{true};
std::atomic<bool> gSuppressed{true};
std::atomic<bool> gReproject{true};
std::atomic<bool> gParallax{true};
std::atomic<int> gMaxFps{0};  // Dying Light frame cap, see LimitFrameRate
constexpr float kDepthMax = 25.0f;  // meters at alpha 254: GML.DEPTH_MAX in the addon

struct State {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11SamplerState* pointSampler = nullptr;  // for the distances in alpha: never blended
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState* depth = nullptr;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT texW = 0, texH = 0;
    uint32_t lastSeq = ~0u;
    ID3D11Buffer* cb = nullptr;
    FrameCamera frameCam{};  // camera GMod rendered the uploaded frame with
    float rect[4] = {};      // part of the uploaded frame GMod drew, in uv
    bool empty = true;       // GMod drew nothing in it: skip compositing
    bool failed = false;
    // Scene brightness: the middle of DL's frame, mipped down to one pixel.
    ID3D11Texture2D* lumTex = nullptr;
    ID3D11ShaderResourceView* lumSrv = nullptr;
    ID3D11Texture2D* lumStaging[3] = {};
    UINT lumW = 0, lumH = 0, lumMips = 0;
    DXGI_FORMAT lumFmt = DXGI_FORMAT_UNKNOWN;
    uint32_t lumFrame = 0;
    bool lumFailed = false;
} s;

// Re-projection constants: current DL camera and the camera of GMod's frame.
struct alignas(16) ReprojCB {
    float curF[4], curU[4], curL[4];
    float frmF[4], frmU[4], frmL[4];
    float tanY, aspect, enabled, pad;
    float rect[4];  // drawn part of the frame, in uv
    float curPos[4], frmPos[4];  // eye positions (DL meters) of the current camera and GMod's frame
    float depthMax, parallax, pad2, pad3;  // alpha 254 = depthMax meters; parallax on/off
};

const char* kShader = R"(
Texture2D frameTex : register(t0);
SamplerState samp : register(s0);
SamplerState pointSamp : register(s1);
cbuffer Reproj : register(b0) {
    float4 curF, curU, curL;
    float4 frmF, frmU, frmL;
    float tanY, aspect, enabled, pad;
    float4 rect;
    float4 curPos, frmPos;
    float depthMax, parallax, pad2, pad3;
};
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
// GMod clears to magenta K. A pixel GMod drew with coverage a over it is
// C = a*F + (1-a)*K. How much magenta is in it (red and blue above green) gives
// 1-a; taking that much K back out recovers F, so antialiased edges, glows and
// beams keep their own colour instead of turning pink.
//
// GMod's frame is a frame or two older than DL's. Each pixel's view direction
// in DL's current camera is looked up in the camera GMod rendered with, so
// GMod's things stay put on the world while turning (rotation only).
float4 PS(VSOut i) : SV_Target {
    float2 uv = i.uv;
    if (enabled > 0.5) {
        float tanX = tanY * aspect;
        float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
        float3 dir = curF.xyz - ndc.x * tanX * curL.xyz + ndc.y * tanY * curU.xyz;
        float z = dot(dir, frmF.xyz);
        clip(z - 0.001);
        float2 ndc0 = float2(-dot(dir, frmL.xyz), dot(dir, frmU.xyz)) / (z * float2(tanX, tanY));
        // The eye moved too. Where GMod knows how far a pixel is (alpha 1-254 =
        // distance along its view; 0 and 255 = unknown or camera-attached, like the
        // gun), find the frame pixel whose 3D point lands on this one now: start
        // from the rotation-only guess and correct it a few times (as TAA does).
        if (parallax > 0.5) {
            [unroll] for (int it = 0; it < 4; ++it) {
                float2 uv0 = float2(ndc0.x * 0.5 + 0.5, 0.5 - ndc0.y * 0.5);
                if (any(uv0 < rect.xy) || any(uv0 > rect.zw)) break;
                float a8 = frameTex.SampleLevel(pointSamp, uv0, 0).a * 255.0;
                if (a8 < 7.5 || a8 > 254.5) break;  // 8..254 = 0..depthMax m (GML.EncodeDepth)
                float zf = (a8 - 8.0) / 246.0 * depthMax;
                float3 P = frmPos.xyz + zf * (frmF.xyz - ndc0.x * tanX * frmL.xyz + ndc0.y * tanY * frmU.xyz);
                float3 v = P - curPos.xyz;
                float zc = dot(v, curF.xyz);
                if (zc < 0.05) break;
                float2 ndcNow = float2(-dot(v, curL.xyz), dot(v, curU.xyz)) / (zc * float2(tanX, tanY));
                ndc0 -= ndcNow - ndc;
            }
        }
        uv = float2(ndc0.x * 0.5 + 0.5, 0.5 - ndc0.y * 0.5);
        clip(float4(uv, 1 - uv));
    }
    // Outside the part GMod drew (and copied) the texture is stale: key.
    clip(float4(uv - rect.xy, rect.zw - uv));
    float3 c = frameTex.Sample(samp, uv).rgb;
    float k = saturate(min(c.r, c.b) - c.g);
    float a = 1 - k;
    clip(a - 0.02);
    float3 f = saturate((c - k * float3(1, 0, 1)) / a);
    return float4(f, a);
}
)";

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool CreateResources() {
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "gmodlight", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "gmodlight", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, &err))) {
        LOGE("shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        SafeRelease(err); SafeRelease(vsb); SafeRelease(psb);
        return false;
    }
    s.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s.vs);
    s.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s.ps);
    vsb->Release(); psb->Release();

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.dev->CreateSamplerState(&sd, &s.sampler);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s.dev->CreateSamplerState(&sd, &s.pointSampler);

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    s.dev->CreateBlendState(&bd, &s.blend);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    s.dev->CreateRasterizerState(&rd, &s.raster);

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    s.dev->CreateDepthStencilState(&dd, &s.depth);

    D3D11_BUFFER_DESC bd2{};
    bd2.ByteWidth = sizeof(ReprojCB);
    bd2.Usage = D3D11_USAGE_DYNAMIC;
    bd2.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd2.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    s.dev->CreateBuffer(&bd2, nullptr, &s.cb);
    return s.vs && s.ps && s.sampler && s.pointSampler && s.blend && s.raster && s.depth && s.cb;
}

bool EnsureTexture(UINT w, UINT h) {
    if (s.tex && s.texW == w && s.texH == h) return true;
    SafeRelease(s.srv);
    SafeRelease(s.tex);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;  // updated a rectangle at a time (UpdateSubresource)
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(s.dev->CreateTexture2D(&td, nullptr, &s.tex))) return false;
    if (FAILED(s.dev->CreateShaderResourceView(s.tex, nullptr, &s.srv))) return false;
    s.texW = w;
    s.texH = h;
    return true;
}

// Copy GMod's newest frame into our texture. Returns true if we have something to draw.
bool UploadFrame() {
    Header* hdr = shm::Get();
    if (!hdr || !hdr->gmodReady) return false;
    FrameSlots& f = hdr->frame;
    uint32_t seq = f.seq;
    if (seq == s.lastSeq) return s.tex != nullptr;
    uint32_t slot = f.latest;
    if (slot >= kFrameBuffers) return s.tex != nullptr;
    f.reading = slot;
    MemoryBarrier();
    if (f.latest != slot) slot = f.reading = f.latest;  // writer published again; follow it
    uint32_t w = f.w, h = f.h, stride = f.stride;
    bool ok = false;
    if (w && h && w <= kMaxFrameW && h <= kMaxFrameH && EnsureTexture(w, h)) {
        // Only the part GMod drew was copied into the slot; upload just that.
        uint32_t x0 = f.rect[slot][0], y0 = f.rect[slot][1], x1 = std::min(f.rect[slot][2], w),
                 y1 = std::min(f.rect[slot][3], h);
        if (x1 > x0 && y1 > y0) {
            D3D11_BOX box{x0, y0, 0, x1, y1, 1};
            s.ctx->UpdateSubresource(s.tex, 0, &box, FramePtr(hdr, slot) + size_t(y0) * stride + x0 * 4, stride, 0);
            // Clip half a texel inside the edge so filtering never reaches stale texels
            // (the 2 px border GMod adds is key anyway).
            s.rect[0] = (x0 + 1.0f) / w; s.rect[1] = (y0 + 1.0f) / h;
            s.rect[2] = (x1 - 1.0f) / w; s.rect[3] = (y1 - 1.0f) / h;
            s.empty = false;
        } else {
            s.empty = true;
        }
        s.frameCam = f.cam[slot];
        ok = true;
    }
    MemoryBarrier();
    f.reading = ~0u;
    s.lastSeq = seq;
    return ok || s.tex != nullptr;
}

// D3D11 has no state blocks; save exactly what we change.
struct SavedState {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11BlendState* blend = nullptr; FLOAT blendFactor[4]{}; UINT sampleMask = 0;
    ID3D11DepthStencilState* depth = nullptr; UINT stencilRef = 0;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr; ID3D11HullShader* hs = nullptr; ID3D11DomainShader* ds = nullptr;
    ID3D11ShaderResourceView* srv = nullptr; ID3D11SamplerState* samp[2] = {};
    ID3D11InputLayout* layout = nullptr; D3D11_PRIMITIVE_TOPOLOGY topo{};

    void Save(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->RSGetViewports(&vpCount, vp);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->RSGetState(&raster);
        c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr); c->HSGetShader(&hs, nullptr, nullptr); c->DSGetShader(&ds, nullptr, nullptr);
        c->PSGetShaderResources(0, 1, &srv); c->PSGetSamplers(0, 2, samp);
        c->IAGetInputLayout(&layout); c->IAGetPrimitiveTopology(&topo);
    }
    void Restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->RSSetViewports(vpCount, vp);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(depth, stencilRef);
        c->RSSetState(raster);
        c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0); c->HSSetShader(hs, nullptr, 0); c->DSSetShader(ds, nullptr, 0);
        c->PSSetShaderResources(0, 1, &srv); c->PSSetSamplers(0, 2, samp);
        c->IASetInputLayout(layout); c->IASetPrimitiveTopology(topo);
        for (auto*& r : rtv) SafeRelease(r);
        SafeRelease(dsv); SafeRelease(blend); SafeRelease(depth); SafeRelease(raster);
        SafeRelease(vs); SafeRelease(ps); SafeRelease(gs); SafeRelease(hs); SafeRelease(ds);
        SafeRelease(srv); SafeRelease(samp[0]); SafeRelease(samp[1]); SafeRelease(layout);
    }
};

// ---------- scene light ----------
// GMod's weapons are lit by gm_flatgrass's sun; at night in Dying Light they
// glowed. A few times a second the middle of DL's picture (before GMod is drawn
// on it) is averaged on the GPU and handed to GMod, which lights its viewmodel to
// match. Read back two samples later, so nothing waits on the GPU.

bool DecodePixel(DXGI_FORMAT f, const void* p, float rgb[3]) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            for (int i = 0; i < 3; ++i) rgb[i] = b[i] / 255.0f;
            return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            rgb[0] = b[2] / 255.0f; rgb[1] = b[1] / 255.0f; rgb[2] = b[0] / 255.0f;
            return true;
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
            uint32_t v;
            memcpy(&v, p, 4);
            for (int i = 0; i < 3; ++i) rgb[i] = ((v >> (10 * i)) & 1023) / 1023.0f;
            return true;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            uint16_t h[3];
            memcpy(h, p, 6);
            for (int i = 0; i < 3; ++i) {
                uint32_t sign = (h[i] >> 15) & 1, e = (h[i] >> 10) & 31, m = h[i] & 1023;
                float v = e == 0 ? m / 1024.0f / 16384.0f : std::ldexp(1.0f + m / 1024.0f, int(e) - 15);
                rgb[i] = sign ? 0.0f : std::min(v, 4.0f);
            }
            return true;
        }
        default:
            return false;
    }
}

void SampleScene(IDXGISwapChain* sc) {
    Header* hdr = shm::Get();
    if (s.lumFailed || !hdr || !hdr->gmodReady || (s.lumFrame++ % 6) != 0) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    float probe[3];
    if (bd.SampleDesc.Count != 1 || !DecodePixel(bd.Format, "\0\0\0\0\0\0\0\0", probe)) {
        LOGW("scene light: back buffer format %d x%u not supported; GMod weapons keep their own lighting", bd.Format,
             bd.SampleDesc.Count);
        s.lumFailed = true;
        bb->Release();
        return;
    }
    // The middle half of the screen: what's around the weapon, without most of the HUD.
    UINT w = std::max(1u, bd.Width / 2), h = std::max(1u, bd.Height / 2);
    if (!s.lumTex || s.lumW != w || s.lumH != h || s.lumFmt != bd.Format) {
        SafeRelease(s.lumSrv);
        SafeRelease(s.lumTex);
        for (auto*& t : s.lumStaging) SafeRelease(t);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 0;  // full chain
        td.ArraySize = 1;
        td.Format = bd.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        bool ok = SUCCEEDED(s.dev->CreateTexture2D(&td, nullptr, &s.lumTex)) &&
                  SUCCEEDED(s.dev->CreateShaderResourceView(s.lumTex, nullptr, &s.lumSrv));
        D3D11_TEXTURE2D_DESC got{};
        if (ok) s.lumTex->GetDesc(&got);
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = sd.Height = 1;
        sd.MipLevels = sd.ArraySize = 1;
        sd.Format = bd.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto*& t : s.lumStaging) ok = ok && SUCCEEDED(s.dev->CreateTexture2D(&sd, nullptr, &t));
        if (!ok) {
            LOGW("scene light: could not create textures (format %d); GMod weapons keep their own lighting", bd.Format);
            s.lumFailed = true;
            bb->Release();
            return;
        }
        s.lumW = w;
        s.lumH = h;
        s.lumFmt = bd.Format;
        s.lumMips = got.MipLevels;
        LOGI("scene light: sampling %ux%u of the %ux%u back buffer (format %d, %u mips)", w, h, bd.Width, bd.Height,
             bd.Format, s.lumMips);
    }
    UINT slot = (s.lumFrame / 6) % 3;
    D3D11_BOX box{bd.Width / 4, bd.Height / 4, 0, bd.Width / 4 + w, bd.Height / 4 + h, 1};
    s.ctx->CopySubresourceRegion(s.lumTex, 0, 0, 0, 0, bb, 0, &box);
    bb->Release();
    s.ctx->GenerateMips(s.lumSrv);
    s.ctx->CopySubresourceRegion(s.lumStaging[slot], 0, 0, 0, 0, s.lumTex, s.lumMips - 1, nullptr);
    // The oldest copy has had two samples' time to finish.
    static int samples = 0;
    ID3D11Texture2D* old = ++samples >= 3 ? s.lumStaging[(slot + 1) % 3] : nullptr;
    D3D11_MAPPED_SUBRESOURCE map;
    if (old && SUCCEEDED(s.ctx->Map(old, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map))) {
        float rgb[3];
        if (DecodePixel(s.lumFmt, map.pData, rgb)) {
            hdr->scene.r = rgb[0];
            hdr->scene.g = rgb[1];
            hdr->scene.b = rgb[2];
            MemoryBarrier();
            hdr->scene.seq = hdr->scene.seq + 1;
            static DWORD lastLog = 0;
            if (GetTickCount() - lastLog > 30000) {
                lastLog = GetTickCount();
                LOGI("scene light: %.3f %.3f %.3f", rgb[0], rgb[1], rgb[2]);
            }
        }
        s.ctx->Unmap(old, 0);
    }
}

IDXGISwapChain* gCurrentSwapChain = nullptr;
void Composite(const DXGI_SWAP_CHAIN_DESC& desc);
void CaptureIfRequested(IDXGISwapChain* sc);

void Draw(IDXGISwapChain* sc) {
    gCurrentSwapChain = sc;
    if (s.failed) return;
    if (!s.dev) {
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&s.dev)))) {
            LOGE("swap chain is not D3D11");
            s.failed = true;
            return;
        }
        s.dev->GetImmediateContext(&s.ctx);
        if (!CreateResources()) { s.failed = true; return; }
        LOGI("overlay ready");
    }
    DXGI_SWAP_CHAIN_DESC desc;
    sc->GetDesc(&desc);
    // Timed, to tell a stutter caused here from one in the game itself.
    LARGE_INTEGER q[5], f;
    QueryPerformanceCounter(&q[0]);
    if (gCb.onFrame) gCb.onFrame(desc.OutputWindow, desc.BufferDesc.Width, desc.BufferDesc.Height);
    QueryPerformanceCounter(&q[1]);
    if (!gSuppressed) SampleScene(sc);
    QueryPerformanceCounter(&q[2]);
    bool have = gVisible && !gSuppressed && UploadFrame() && !s.empty;
    QueryPerformanceCounter(&q[3]);
    if (have) Composite(desc);
    CaptureIfRequested(sc);
    QueryPerformanceCounter(&q[4]);
    QueryPerformanceFrequency(&f);
    auto ms = [&](LONGLONG d) { return double(d) * 1000.0 / double(f.QuadPart); };
    // A frame much longer than the cap's (or 25 ms uncapped): log what we spent in it.
    static LARGE_INTEGER last{};
    double frame = last.QuadPart ? ms(q[0].QuadPart - last.QuadPart) : 0;
    last = q[0];
    int cap = gMaxFps.load(std::memory_order_relaxed);
    double limit = cap > 0 ? 1500.0 / cap : 25.0;
    static int hitches = 0;
    static DWORD lastSummary = 0;
    if (frame > limit && !gSuppressed) {
        ++hitches;
        static int logs = 0;
        if (logs++ < 40)
            LOGW("hitch: %.1f ms between frames (ours this frame: world %.1f, scene light %.1f, upload %.1f, composite %.1f ms)",
                 frame, ms(q[1].QuadPart - q[0].QuadPart), ms(q[2].QuadPart - q[1].QuadPart),
                 ms(q[3].QuadPart - q[2].QuadPart), ms(q[4].QuadPart - q[3].QuadPart));
    }
    if (GetTickCount() - lastSummary > 10000) {
        lastSummary = GetTickCount();
        if (hitches) LOGI("hitches in the last 10 s: %d", hitches);
        hitches = 0;
    }
}

void Composite(const DXGI_SWAP_CHAIN_DESC& desc) {
    IDXGISwapChain* sc = gCurrentSwapChain;

    if (!s.rtv) {
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
        s.dev->CreateRenderTargetView(bb, nullptr, &s.rtv);
        bb->Release();
        if (!s.rtv) return;
    }

    SavedState saved;
    saved.Save(s.ctx);
    D3D11_VIEWPORT vp{0, 0, float(desc.BufferDesc.Width), float(desc.BufferDesc.Height), 0, 1};
    s.ctx->OMSetRenderTargets(1, &s.rtv, nullptr);
    s.ctx->RSSetViewports(1, &vp);
    const float bf[4] = {0, 0, 0, 0};
    s.ctx->OMSetBlendState(s.blend, bf, 0xffffffff);
    s.ctx->OMSetDepthStencilState(s.depth, 0);
    s.ctx->RSSetState(s.raster);
    s.ctx->IASetInputLayout(nullptr);
    s.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s.ctx->VSSetShader(s.vs, nullptr, 0);
    s.ctx->PSSetShader(s.ps, nullptr, 0);
    s.ctx->GSSetShader(nullptr, nullptr, 0);
    s.ctx->HSSetShader(nullptr, nullptr, 0);
    s.ctx->DSSetShader(nullptr, nullptr, 0);
    s.ctx->PSSetShaderResources(0, 1, &s.srv);
    ID3D11SamplerState* samplers[2] = {s.sampler, s.pointSampler};
    s.ctx->PSSetSamplers(0, 2, samplers);
    if (Header* hdr = shm::Get()) {
        const Camera& c = hdr->cam;
        // How old is the GMod frame on screen, and how far has the camera moved
        // since? GMod draws its frames this far ahead (GML.RenderCamera).
        if (s.frameCam.time > 0 && c.time > 0) {
            float age = float(c.time - s.frameCam.time);
            if (age >= 0 && age < 0.2f) {
                float l = hdr->latency;
                hdr->latency = l > 0 ? l + (age - l) * 0.05f : age;
                float dx = c.pos.x - s.frameCam.pos.x, dy = c.pos.y - s.frameCam.pos.y, dz = c.pos.z - s.frameCam.pos.z;
                float err = std::sqrt(dx * dx + dy * dy + dz * dz);
                static float errSum = 0, errMax = 0;
                static int errN = 0;
                static DWORD lastLog = 0;
                errSum += err; errMax = std::max(errMax, err); ++errN;
                if (GetTickCount() - lastLog > 10000 && errN > 0) {
                    lastLog = GetTickCount();
                    LOGI("GMod frame age %.1f ms; camera vs where GMod drew from: avg %.1f cm, max %.1f cm", hdr->latency * 1000,
                         errSum / errN * 100, errMax * 100);
                    errSum = errMax = 0;
                    errN = 0;
                }
            }
        }
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(s.ctx->Map(s.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            ReprojCB r{};
            auto put = [](float* d, const Vec3& v) { d[0] = v.x; d[1] = v.y; d[2] = v.z; d[3] = 0; };
            put(r.curF, c.fwd); put(r.curU, c.up); put(r.curL, c.left);
            put(r.frmF, s.frameCam.fwd); put(r.frmU, s.frameCam.up); put(r.frmL, s.frameCam.left);
            r.tanY = std::tan(c.fovDeg * 0.5f * 0.0174533f);
            r.aspect = c.aspect;
            // Needs both cameras, a sane fov, and DL's own left vector.
            float l2 = c.left.x * c.left.x + c.left.y * c.left.y + c.left.z * c.left.z;
            r.enabled = (c.valid && s.frameCam.valid && r.tanY > 0.05f && r.tanY < 10 && l2 > 0.25f && gReproject) ? 1.0f : 0.0f;
            memcpy(r.rect, s.rect, sizeof(r.rect));
            put(r.curPos, c.pos);
            put(r.frmPos, s.frameCam.pos);
            r.depthMax = kDepthMax;
            r.parallax = (gParallax && r.enabled > 0.5f && s.frameCam.time > 0) ? 1.0f : 0.0f;
            memcpy(map.pData, &r, sizeof(r));
            s.ctx->Unmap(s.cb, 0);
        }
    }
    ID3D11Buffer* savedCb = nullptr;
    s.ctx->PSGetConstantBuffers(0, 1, &savedCb);
    s.ctx->PSSetConstantBuffers(0, 1, &s.cb);
    s.ctx->Draw(3, 0);
    s.ctx->PSSetConstantBuffers(0, 1, &savedCb);
    SafeRelease(savedCb);
    saved.Restore(s.ctx);
}

// ---------- screenshots ----------
// What the player sees (DL with GMod on top), half size, as BMPs next to the
// plugin in GModLight_shots\. For debugging alignment from the user's machine.

std::atomic<bool> gShotRequested{false};
std::string gShotReason;
std::mutex gShotMutex;
int gShotCount = 0;
constexpr int kMaxShots = 60;

std::wstring ShotDir() {
    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ShotDir), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir = path;
    dir.resize(dir.find_last_of(L"\\/"));
    dir += L"\\GModLight_shots";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void WriteBmp(std::wstring path, std::vector<uint8_t> bgr, int w, int h) {
    int stride = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = DWORD(fh.bfOffBits + size_t(stride) * h);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bgr.data(), 1, bgr.size(), f);
    fclose(f);
}

void CaptureIfRequested(IDXGISwapChain* sc) {
    if (!gShotRequested.exchange(false)) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool r10 = d.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
    if (!(bgra || rgba || r10) || d.SampleDesc.Count != 1) {
        LOGW("screenshot: back buffer format %d / %u samples not supported", d.Format, d.SampleDesc.Count);
        bb->Release();
        return;
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(s.dev->CreateTexture2D(&sd, nullptr, &staging))) { bb->Release(); return; }
    s.ctx->CopyResource(staging, bb);
    bb->Release();
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(s.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) { staging->Release(); return; }
    int w = int(d.Width / 2), h = int(d.Height / 2);
    int stride = (w * 3 + 3) & ~3;
    std::vector<uint8_t> out(size_t(stride) * h);
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(map.pData) + size_t(y * 2) * map.RowPitch);
        uint8_t* o = out.data() + size_t(y) * stride;
        for (int x = 0; x < w; ++x) {
            uint32_t p = row[x * 2];
            uint8_t r, g, b;
            if (bgra) { b = p & 0xff; g = (p >> 8) & 0xff; r = (p >> 16) & 0xff; }
            else if (rgba) { r = p & 0xff; g = (p >> 8) & 0xff; b = (p >> 16) & 0xff; }
            else { r = uint8_t((p & 0x3ff) >> 2); g = uint8_t(((p >> 10) & 0x3ff) >> 2); b = uint8_t(((p >> 20) & 0x3ff) >> 2); }
            o[x * 3] = b; o[x * 3 + 1] = g; o[x * 3 + 2] = r;
        }
    }
    s.ctx->Unmap(staging, 0);
    staging->Release();

    std::string reason;
    { std::lock_guard<std::mutex> g(gShotMutex); reason = gShotReason; }
    wchar_t name[64];
    swprintf_s(name, L"\\shot_%03d_%hs.bmp", gShotCount, reason.c_str());
    std::wstring path = ShotDir() + name;
    LOGI("screenshot %d (%s)", gShotCount, reason.c_str());
    ++gShotCount;
    std::thread(WriteBmp, path, std::move(out), w, h).detach();  // don't stall the frame on disk
}

// ---------- frame limiter ----------
// At ~160 fps Dying Light kept the GPU ~95% busy, GMod's frames queued behind its
// frames at irregular times (20-40 ms old, stutter). Capped, both fit, evenly
// paced. Waits after Present, so the next frame (and its input) starts fresh: a
// high-resolution timer for most of the wait, then a short spin for precision.

void LimitFrameRate() {
    int fps = gMaxFps.load(std::memory_order_relaxed);
    if (fps <= 0) return;
    static LARGE_INTEGER freq{}, next{};
    static HANDLE timer = nullptr;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG period = freq.QuadPart / fps;
    if (!next.QuadPart || now.QuadPart - next.QuadPart > period) next.QuadPart = now.QuadPart;  // fell behind: don't catch up
    next.QuadPart += period;
    LONGLONG left = next.QuadPart - now.QuadPart;
    LONGLONG spin = freq.QuadPart / 1000;  // spin the last ~1 ms
    if (timer && left > spin) {
        LARGE_INTEGER due;
        due.QuadPart = -((left - spin) * 10000000 / freq.QuadPart);  // 100 ns units, relative
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
    }
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < next.QuadPart);
}

// If one Present implementation calls the other, draw and wait only once.
thread_local bool tInPresent = false;

HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if ((flags & DXGI_PRESENT_TEST) || tInPresent) return oPresent(sc, sync, flags);
    tInPresent = true;
    Draw(sc);
    HRESULT hr = oPresent(sc, sync, flags);
    LimitFrameRate();
    tInPresent = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if ((flags & DXGI_PRESENT_TEST) || tInPresent) return oPresent1(sc, sync, flags, p);
    tInPresent = true;
    Draw(sc);
    HRESULT hr = oPresent1(sc, sync, flags, p);
    LimitFrameRate();
    tInPresent = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE HkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    SafeRelease(s.rtv);
    return oResizeBuffers(sc, n, w, h, f, fl);
}

}  // namespace

bool Init(const Callbacks& cb) {
    gCb = cb;
    // Make a throwaway device and swap chain just to find the vtable entries.
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GModLightDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferCount = 1;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = hwnd;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        LOGE("dummy D3D11 device failed: %08lx", hr);
        DestroyWindow(hwnd);
        return false;
    }
    void** vt = *reinterpret_cast<void***>(sc);
    void* present = vt[8];
    void* resize = vt[13];
    void* present1 = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) {
        present1 = (*reinterpret_cast<void***>(sc1))[22];
        sc1->Release();
    }
    sc->Release(); ctx->Release(); dev->Release();
    DestroyWindow(hwnd);

    bool ok = MH_CreateHook(present, &HkPresent, reinterpret_cast<void**>(&oPresent)) == MH_OK &&
              MH_CreateHook(resize, &HkResizeBuffers, reinterpret_cast<void**>(&oResizeBuffers)) == MH_OK;
    if (present1) MH_CreateHook(present1, &HkPresent1, reinterpret_cast<void**>(&oPresent1));
    if (!ok || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        LOGE("could not hook swap chain");
        return false;
    }
    LOGI("swap chain hooked");
    return true;
}

void SetVisible(bool v) { gVisible = v; }
void SetSuppressed(bool v) { gSuppressed = v; }
void SetReproject(bool v) { gReproject = v; }
void SetParallax(bool v) { gParallax = v; }
void SetMaxFps(int fps) { gMaxFps = fps; }

void RequestScreenshot(const char* reason, bool force) {
    if (!force && gShotCount >= kMaxShots) return;
    {
        std::lock_guard<std::mutex> g(gShotMutex);
        gShotReason = reason;
    }
    gShotRequested = true;
}
bool Visible() { return gVisible; }

}  // namespace gml::overlay
