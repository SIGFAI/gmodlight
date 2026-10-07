// GModLight: Garry's Mod side binary module (gmcl_gmodlight_win64 / gmsv_gmodlight_win64).
//
// Exposes the shared memory bridge to Lua and, on the client, copies every
// finished GMod frame into shared memory for Dying Light to composite.
// Coordinates are passed through untouched (Dying Light space); the Lua addon
// converts them, so the mapping can be tweaked without rebuilding.
#include <windows.h>
#include <d3d9.h>
#include <emmintrin.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <share.h>

#include "GarrysMod/Lua/Interface.h"
#include "MinHook.h"
#include "../../shared/bridge.h"

using namespace GarrysMod::Lua;

namespace {

// garrysmod/gmodlight.log, next to console.log. Both realms append to it.
void Log(const char* fmt, ...) {
    static FILE* f = _fsopen("garrysmod/gmodlight.log", "a", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fflush(f);
}

HANDLE gMap = nullptr;
gml::Header* gHdr = nullptr;

bool Connect() {
    if (gHdr) return true;
    gMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, gml::kShmName);
    if (!gMap) return false;
    auto* hdr = static_cast<gml::Header*>(MapViewOfFile(gMap, FILE_MAP_ALL_ACCESS, 0, 0, gml::kShmSize));
    if (hdr && hdr->magic == gml::kMagic && hdr->version != gml::kVersion)
        Log("bridge version mismatch: Dying Light %u, this module %u. Reinstall GModLight.", hdr->version, gml::kVersion);
    if (!hdr || hdr->magic != gml::kMagic || hdr->version != gml::kVersion) {
        if (hdr) UnmapViewOfFile(hdr);
        CloseHandle(gMap);
        gMap = nullptr;
        return false;
    }
    hdr->gmPid = GetCurrentProcessId();
    gHdr = hdr;
    return true;
}

// ---------- frame capture (client) ----------

using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PresentEx_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using ResetEx_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
Present_t oPresent = nullptr;
PresentEx_t oPresentEx = nullptr;
Reset_t oReset = nullptr;
ResetEx_t oResetEx = nullptr;
bool gCaptureHooked = false;
volatile bool gCapturing = false;

struct Capture {
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9* rt = nullptr;    // non-multisampled copy of the back buffer
    IDirect3DSurface9* sys = nullptr;   // system memory copy we can lock
    UINT w = 0, h = 0;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
} cap;

void ReleaseCapture() {
    if (cap.rt) { cap.rt->Release(); cap.rt = nullptr; }
    if (cap.sys) { cap.sys->Release(); cap.sys = nullptr; }
    cap.w = cap.h = 0;
}

// Camera of the frame being rendered (set from Lua in RenderScene), attached to
// that frame when it's captured.
gml::FrameCamera gPendingCam{};
// The last few, by id: frames can reach Present a frame or two before or after
// the RenderScene that drew them (round 10: off by one in most frames while
// turning), so each image says which one it is (a stamp, see CaptureFrame).
constexpr uint32_t kCamRing = 16;
gml::FrameCamera gCamRing[kCamRing]{};

// Chroma key as a back buffer pixel (X8R8G8B8; the top byte is ignored).
constexpr uint32_t kKeyPixel = 0x00FF00FF;

int FirstNonKey(const uint32_t* p, int n) {
    const __m128i mask = _mm_set1_epi32(0x00FFFFFF), key = _mm_set1_epi32(int(kKeyPixel));
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i v = _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i)), mask);
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(v, key)) != 0xFFFF) break;
    }
    for (; i < n; ++i)
        if ((p[i] & 0x00FFFFFF) != kKeyPixel) return i;
    return -1;
}

int LastNonKey(const uint32_t* p, int n) {
    const __m128i mask = _mm_set1_epi32(0x00FFFFFF), key = _mm_set1_epi32(int(kKeyPixel));
    int i = n;
    for (; i - 4 >= 0; i -= 4) {
        __m128i v = _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i - 4)), mask);
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(v, key)) != 0xFFFF) break;
    }
    for (int k = i - 1; k >= 0; --k)
        if ((p[k] & 0x00FFFFFF) != kKeyPixel) return k;
    return -1;
}

uint64_t gCopiedPixels = 0, gFramePixels = 0;  // for Stats(): how much of each frame was content

void CaptureFrame(IDirect3DDevice9* dev) {
    if (!gHdr || !gCapturing) return;
    IDirect3DSurface9* bb = nullptr;
    static int logged = 0;
    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr)) {
        if (logged++ < 5) Log("capture: GetBackBuffer failed %08lx", hr);
        return;
    }
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    static bool described = false;
    if (!described) {
        // GMod's small frame queues on the GPU behind Dying Light's (~95% busy), which
        // was most of its ~30 ms frame age. A D3D9Ex device can jump the queue and keep
        // at most one frame in flight.
        IDirect3DDevice9Ex* ex = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&ex))) && ex) {
            HRESULT p = ex->SetGPUThreadPriority(7);
            HRESULT l = ex->SetMaximumFrameLatency(1);
            Log("capture: D3D9Ex device; GPU priority +7 %s, frame latency 1 %s", SUCCEEDED(p) ? "set" : "refused",
                SUCCEEDED(l) ? "set" : "refused");
            ex->Release();
        } else {
            Log("capture: plain D3D9 device (no GPU priority control)");
        }
        described = true;
        Log("capture: back buffer %ux%u format %d multisample %d", d.Width, d.Height, d.Format, d.MultiSampleType);
    }
    if (d.Width > gml::kMaxFrameW || d.Height > gml::kMaxFrameH ||
        (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8)) {
        if (logged++ < 5) Log("capture: unsupported back buffer, skipping");
        bb->Release();
        return;
    }
    if (cap.dev != dev || cap.w != d.Width || cap.h != d.Height || cap.fmt != d.Format) {
        ReleaseCapture();
        cap.dev = dev;
        cap.w = d.Width;
        cap.h = d.Height;
        cap.fmt = d.Format;
        dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &cap.rt, nullptr);
        dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &cap.sys, nullptr);
    }
    bool ok = false;
    if (cap.rt && cap.sys) {
        // StretchRect resolves multisampling; GetRenderTargetData wants a plain surface.
        if (d.MultiSampleType == D3DMULTISAMPLE_NONE)
            ok = SUCCEEDED(dev->GetRenderTargetData(bb, cap.sys));
        else
            ok = SUCCEEDED(dev->StretchRect(bb, nullptr, cap.rt, nullptr, D3DTEXF_NONE)) &&
                 SUCCEEDED(dev->GetRenderTargetData(cap.rt, cap.sys));
    }
    bb->Release();
    if (!ok) {
        if (logged++ < 5) Log("capture: copy failed (rt %p sys %p)", cap.rt, cap.sys);
        return;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(cap.sys->LockRect(&lr, nullptr, 0))) return;
    gml::FrameSlots& f = gHdr->frame;
    uint32_t latest = f.latest, reading = f.reading, slot = 0;
    while (slot == latest || slot == reading) ++slot;  // 3 slots, so one is always free
    uint8_t* dst = gml::FramePtr(gHdr, slot);
    const uint32_t stride = cap.w * 4;
    // Which RenderScene drew this image: Lua stamps the top-left corner with five
    // 4x4 blocks, red on/off: a marker (on), then the frame id's low 4 bits. Binary,
    // because GMod's colour processing shifts exact values a little. Take that
    // frame's camera, and paint the stamp over with key.
    gml::FrameCamera frameCam = gPendingCam;
    {
        const uint8_t* row1 = static_cast<const uint8_t*>(lr.pBits) + lr.Pitch;  // y = 1
        auto block = [&](int k) { return row1 + (k * 4 + 1) * 4; };             // B, G, R, X
        bool stamped = cap.w >= 20 && cap.h >= 4;
        int id = 0;
        for (int k = 0; k < 5 && stamped; ++k) {
            const uint8_t* p = block(k);
            if (p[0] > 24 || p[1] > 24) stamped = false;  // only red is ever drawn there
            bool on = p[2] > 128;
            if (k == 0) stamped = stamped && on;
            else id |= (on ? 1 : 0) << (k - 1);
        }
        static uint32_t matched = 0, missed = 0, stale = 0;
        if (stamped) {
            const gml::FrameCamera* best = nullptr;
            for (auto& c : gCamRing)
                if (c.valid && int(c.id & 15) == id && (!best || c.id > best->id)) best = &c;
            if (best) { frameCam = *best; ++matched; if (best->id != gPendingCam.id) ++stale; } else ++missed;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 20; ++x)
                    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch + x * 4) = kKeyPixel;
        } else {
            ++missed;
        }
        if ((matched + missed) % 2000 == 1000)
            Log("capture: %u frames matched to their camera by stamp (%u would have had another frame's), %u without a stamp",
                matched, stale, missed);
    }
    // Most of the frame is chroma key; copy (and let DL upload) only the box
    // around what was drawn, plus 2 px of key so filtering at its edge is clean.
    const uint8_t* src = static_cast<const uint8_t*>(lr.pBits);
    int x0 = int(cap.w), x1 = -1, y0 = int(cap.h), y1 = -1;
    for (int y = 0; y < int(cap.h); ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(src + size_t(y) * lr.Pitch);
        int first = FirstNonKey(row, int(cap.w));
        if (first < 0) continue;
        int last = LastNonKey(row, int(cap.w));
        x0 = std::min(x0, first);
        x1 = std::max(x1, last);
        if (y0 > y) y0 = y;
        y1 = y;
    }
    uint32_t* rect = f.rect[slot];
    if (x1 < 0) {
        rect[0] = rect[1] = rect[2] = rect[3] = 0;  // nothing drawn
    } else {
        x0 = std::max(0, x0 - 2);
        y0 = std::max(0, y0 - 2);
        x1 = std::min(int(cap.w), x1 + 3);
        y1 = std::min(int(cap.h), y1 + 3);
        for (int y = y0; y < y1; ++y)
            memcpy(dst + size_t(y) * stride + x0 * 4, src + size_t(y) * lr.Pitch + x0 * 4, size_t(x1 - x0) * 4);
        rect[0] = x0; rect[1] = y0; rect[2] = x1; rect[3] = y1;
    }
    cap.sys->UnlockRect();
    gCopiedPixels += uint64_t(x1 > x0 ? x1 - x0 : 0) * uint64_t(y1 > y0 ? y1 - y0 : 0);
    gFramePixels += uint64_t(cap.w) * cap.h;
    f.w = cap.w;
    f.h = cap.h;
    f.stride = stride;
    f.cam[slot] = frameCam;
    MemoryBarrier();
    f.latest = slot;
    f.seq = f.seq + 1;
    if (f.seq == 1) Log("capture: first frame published (%ux%u)", cap.w, cap.h);
}

// Once the window is off screen, presenting to it only costs time: Windows holds an
// invisible window's Present for ~50 ms. Dying Light shows our frames, so skip it.
// (CaptureFrame's read-back already keeps the GPU from running ahead.)
volatile bool gWindowHidden = false;
volatile LONG gFocusGrabsBlocked = 0;  // see HkSetForegroundWindow

// Lockstep: after each captured frame, wait for Dying Light's next frame (its event,
// set right after it publishes a camera), so GMod draws exactly one frame per DL frame,
// starting the moment the camera is there. 50 ms at most: never freeze on DL.
void WaitForDL() {
    if (!gHdr || !gWindowHidden || !gCapturing) return;
    static HANDLE evt = nullptr;
    static DWORD lastOpen = 0;
    if (!evt && GetTickCount() - lastOpen > 1000) {
        lastOpen = GetTickCount();
        evt = OpenEventW(SYNCHRONIZE, FALSE, gml::kFrameEventName);
        if (evt) Log("lockstep: waiting on Dying Light's frames");
    }
    if (!evt) return;
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    DWORD r = WaitForSingleObject(evt, 50);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    static double waited = 0;
    static uint32_t n = 0, timeouts = 0;
    waited += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
    timeouts += r == WAIT_TIMEOUT;
    if (++n == 600) {
        Log("lockstep: waited %.1f ms per frame for Dying Light (%u timeouts in %u frames)", waited / n, timeouts, n);
        waited = 0;
        n = timeouts = 0;
    }
}

HRESULT STDMETHODCALLTYPE HkPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    CaptureFrame(dev);
    WaitForDL();
    if (gWindowHidden && gCapturing) return D3D_OK;
    return oPresent(dev, a, b, c, d);
}

HRESULT STDMETHODCALLTYPE HkPresentEx(IDirect3DDevice9Ex* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d, DWORD fl) {
    CaptureFrame(dev);
    WaitForDL();
    if (gWindowHidden && gCapturing) return D3D_OK;
    return oPresentEx(dev, a, b, c, d, fl);
}

// Our render target lives in D3DPOOL_DEFAULT; it must be gone before a Reset.
HRESULT STDMETHODCALLTYPE HkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    ReleaseCapture();
    return oReset(dev, pp);
}

HRESULT STDMETHODCALLTYPE HkResetEx(IDirect3DDevice9Ex* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* m) {
    ReleaseCapture();
    return oResetEx(dev, pp, m);
}

bool HookPresent() {
    if (gCaptureHooked) return true;
    // A throwaway device gives us the vtable; the functions are shared with GMod's device.
    IDirect3D9Ex* d3d = nullptr;
    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d))) { Log("hook: Direct3DCreate9Ex failed"); return false; }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GModLightDummy9";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    // Explicit size: the dummy window's client area is too small to infer one from.
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    IDirect3DDevice9Ex* dev = nullptr;
    HRESULT hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                     &pp, nullptr, &dev);
    if (FAILED(hr)) { Log("hook: dummy CreateDeviceEx failed %08lx", hr); d3d->Release(); DestroyWindow(hwnd); return false; }
    void** vt = *reinterpret_cast<void***>(dev);
    void* present = vt[17];
    void* presentEx = vt[121];
    void* reset = vt[16];
    void* resetEx = vt[132];
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);

    MH_Initialize();  // fine if already initialized
    bool ok = MH_CreateHook(present, &HkPresent, reinterpret_cast<void**>(&oPresent)) == MH_OK;
    MH_CreateHook(presentEx, &HkPresentEx, reinterpret_cast<void**>(&oPresentEx));
    ok = ok && MH_CreateHook(reset, &HkReset, reinterpret_cast<void**>(&oReset)) == MH_OK;
    MH_CreateHook(resetEx, &HkResetEx, reinterpret_cast<void**>(&oResetEx));
    ok = ok && MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    gCaptureHooked = ok;
    Log("hook: Present %p PresentEx %p Reset %p -> %s", present, presentEx, reset, ok ? "hooked" : "FAILED");
    return ok;
}

// ---------- window ----------

struct FindWin { DWORD pid; HWND out; };

BOOL CALLBACK FindMainWindow(HWND h, LPARAM lp) {
    auto* f = reinterpret_cast<FindWin*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { f->out = h; return FALSE; }
    return TRUE;
}

HWND MainWindowOf(DWORD pid) {
    FindWin f{pid, nullptr};
    EnumWindows(FindMainWindow, reinterpret_cast<LPARAM>(&f));
    return f.out;
}

// ---------- Lua API ----------

LUA_FUNCTION(L_Connect) {
    bool was = gHdr != nullptr;
    bool ok = Connect();
    if (ok && !was) Log("connected to shared memory (DL pid %u)", gHdr->dlPid);
    LUA->PushBool(ok);
    return 1;
}

// True when started by tools/fakehost with GMODLIGHT_SELFTEST set.
LUA_FUNCTION(L_SelfTest) {
    LUA->PushBool(GetEnvironmentVariableA("GMODLIGHT_SELFTEST", nullptr, 0) > 0);
    return 1;
}

LUA_FUNCTION(L_Stats) {
    char buf[160];
    sprintf_s(buf, "frames %u, focus grabs blocked %ld, %.1f%% of each frame copied", gHdr ? gHdr->frame.seq : 0,
              gFocusGrabsBlocked, gFramePixels ? 100.0 * double(gCopiedPixels) / double(gFramePixels) : 0.0);
    LUA->PushString(buf);
    return 1;
}

LUA_FUNCTION(L_Log) {
    Log("lua: %s", LUA->CheckString(1));
    return 0;
}

// Returns true while Dying Light is alive and talking to us.
LUA_FUNCTION(L_Heartbeat) {
    static uint32_t lastDl = 0;
    static DWORD lastChange = GetTickCount();
    if (!gHdr) { LUA->PushBool(false); return 1; }
    gHdr->gmHeartbeat++;
    uint32_t hb = gHdr->dlHeartbeat;
    if (hb != lastDl) { lastDl = hb; lastChange = GetTickCount(); }
    LUA->PushBool(GetTickCount() - lastChange < 5000);
    return 1;
}

LUA_FUNCTION(L_SetReady) {
    if (gHdr) gHdr->gmodReady = LUA->GetBool(1) ? 1 : 0;
    return 0;
}

LUA_FUNCTION(L_GetMode) {
    LUA->PushNumber(gHdr ? gHdr->mode : 0);
    return 1;
}

gml::Camera gLastReadCam{};  // what Lua last rendered/aimed with

// valid, px, py, pz, fx, fy, fz, ux, uy, uz, fov, aspect, viewW, viewH, lx, ly, lz, time, latency
LUA_FUNCTION(L_GetCamera) {
    if (!gHdr) { LUA->PushBool(false); return 1; }
    gml::Camera c = gHdr->cam;
    gLastReadCam = c;
    LUA->PushBool(c.valid != 0);
    const float v[] = {c.pos.x, c.pos.y, c.pos.z, c.fwd.x, c.fwd.y, c.fwd.z, c.up.x, c.up.y, c.up.z, c.fovDeg, c.aspect};
    for (float x : v) LUA->PushNumber(x);
    LUA->PushNumber(c.viewW);
    LUA->PushNumber(c.viewH);
    LUA->PushNumber(c.left.x);
    LUA->PushNumber(c.left.y);
    LUA->PushNumber(c.left.z);
    LUA->PushNumber(c.time);
    LUA->PushNumber(gHdr->latency);
    return 19;
}

// The camera this frame is being rendered with (call from RenderScene), and the
// position it's drawn from (DL space) if that was predicted ahead:
// MarkFrameCamera([x, y, z [, fx, fy, fz, ux, uy, uz, lx, ly, lz]]).
LUA_FUNCTION(L_MarkFrameCamera) {
    if (!gHdr) return 0;
    const gml::Camera& c = gLastReadCam;
    static uint32_t frameId = 0;
    gPendingCam = {c.fwd, c.up, c.left, c.valid, c.pos, c.time, ++frameId};
    auto num = [&](int i) { return float(LUA->GetNumber(i)); };
    if (LUA->IsType(1, Type::Number)) gPendingCam.pos = {num(1), num(2), num(3)};
    // And the view, if that was predicted too: fwd, up, left (DL space).
    if (LUA->IsType(12, Type::Number)) {
        gPendingCam.fwd = {num(4), num(5), num(6)};
        gPendingCam.up = {num(7), num(8), num(9)};
        gPendingCam.left = {num(10), num(11), num(12)};
    }
    gCamRing[gPendingCam.id % kCamRing] = gPendingCam;
    LUA->PushNumber(gPendingCam.id);
    return 1;
}

LUA_FUNCTION(L_GetDebugFlags) {
    LUA->PushNumber(gHdr ? gHdr->debugFlags : 0);
    return 1;
}

// r, g, b, seq: how bright and what colour Dying Light's picture is right now.
LUA_FUNCTION(L_GetSceneLight) {
    if (!gHdr) return 0;
    const gml::SceneLight& l = gHdr->scene;
    LUA->PushNumber(l.r);
    LUA->PushNumber(l.g);
    LUA->PushNumber(l.b);
    LUA->PushNumber(l.seq);
    return 4;
}

// SetProbes({{id, fx, fy, fz, tx, ty, tz}, ...}): segments (DL space) for DL to trace.
// Returns the seq they were sent as.
LUA_FUNCTION(L_SetProbes) {
    if (!gHdr) return 0;
    LUA->CheckType(1, Type::Table);
    gml::Probes& p = gHdr->probes;
    uint32_t n = 0;
    for (int i = 1; n < gml::kMaxProbes; ++i) {
        LUA->PushNumber(i);
        LUA->GetTable(1);
        if (!LUA->IsType(-1, Type::Table)) { LUA->Pop(); break; }
        auto at = [&](int k) {
            LUA->PushNumber(k);
            LUA->GetTable(-2);
            double v = LUA->GetNumber(-1);
            LUA->Pop();
            return static_cast<float>(v);
        };
        gml::Probe& pr = p.probes[n++];
        pr.id = static_cast<uint32_t>(at(1));
        pr.from = {at(2), at(3), at(4)};
        pr.to = {at(5), at(6), at(7)};
        LUA->Pop();
    }
    p.count = n;
    MemoryBarrier();
    uint32_t seq = p.seq + 1;
    p.seq = seq;
    LUA->PushNumber(seq);
    return 1;
}

// resultSeq, {{id, dist, x, y, z, nx, ny, nz}, ...}: what DL's world was hit by the last answered probes.
LUA_FUNCTION(L_GetProbeHits) {
    if (!gHdr) { LUA->PushNumber(0); LUA->CreateTable(); return 2; }
    const gml::Probes& p = gHdr->probes;
    uint32_t seq = p.resultSeq;
    MemoryBarrier();
    uint32_t n = p.resultCount < gml::kMaxProbes ? p.resultCount : gml::kMaxProbes;
    LUA->PushNumber(seq);
    LUA->CreateTable();
    for (uint32_t i = 0; i < n; ++i) {
        const gml::ProbeHit& h = p.results[i];
        LUA->PushNumber(i + 1);
        LUA->CreateTable();
        LUA->PushNumber(h.id); LUA->SetField(-2, "id");
        LUA->PushNumber(h.dist); LUA->SetField(-2, "dist");
        LUA->PushNumber(h.pos.x); LUA->SetField(-2, "x");
        LUA->PushNumber(h.pos.y); LUA->SetField(-2, "y");
        LUA->PushNumber(h.pos.z); LUA->SetField(-2, "z");
        LUA->PushNumber(h.normal.x); LUA->SetField(-2, "nx");
        LUA->PushNumber(h.normal.y); LUA->SetField(-2, "ny");
        LUA->PushNumber(h.normal.z); LUA->SetField(-2, "nz");
        LUA->SetTable(-3);
    }
    return 2;
}

// Kill(id): GMod's health for the actor ran out.
LUA_FUNCTION(L_Kill) {
    if (!gHdr) return 0;
    gml::EventQueue& q = gHdr->events;
    uint32_t head = q.head;
    if (head - q.tail >= gml::kMaxEvents) return 0;
    gml::Event& e = q.events[head % gml::kMaxEvents];
    e = {};
    e.type = gml::kEvKill;
    e.handle = _strtoui64(LUA->CheckString(1), nullptr, 16);
    MemoryBarrier();
    q.head = head + 1;
    return 0;
}

// Damage(id, amount, px, py, pz, dx, dy, dz, dlType): GMod hurt an actor's stand-in.
LUA_FUNCTION(L_Damage) {
    if (!gHdr) return 0;
    gml::EventQueue& q = gHdr->events;
    uint32_t head = q.head;
    if (head - q.tail >= gml::kMaxEvents) return 0;  // DL not keeping up; drop
    gml::Event& e = q.events[head % gml::kMaxEvents];
    e.type = gml::kEvDamage;
    const char* id = LUA->CheckString(1);
    e.handle = _strtoui64(id, nullptr, 16);
    e.amount = static_cast<float>(LUA->CheckNumber(2));
    e.pos = {float(LUA->GetNumber(3)), float(LUA->GetNumber(4)), float(LUA->GetNumber(5))};
    e.dir = {float(LUA->GetNumber(6)), float(LUA->GetNumber(7)), float(LUA->GetNumber(8))};
    e.dlDamageType = static_cast<uint32_t>(LUA->GetNumber(9));
    MemoryBarrier();
    q.head = head + 1;
    return 0;
}

// Array of {type, a, b}; drains the queue.
LUA_FUNCTION(L_PollInput) {
    LUA->CreateTable();
    if (!gHdr) return 1;
    gml::InputQueue& q = gHdr->input;
    int i = 1;
    while (q.tail != q.head) {
        const gml::InputEvent e = q.events[q.tail % gml::kMaxInputEvents];
        MemoryBarrier();
        q.tail = q.tail + 1;
        LUA->PushNumber(i++);
        LUA->CreateTable();
        LUA->PushNumber(e.type); LUA->SetField(-2, "type");
        LUA->PushNumber(e.a); LUA->SetField(-2, "a");
        LUA->PushNumber(e.b); LUA->SetField(-2, "b");
        LUA->SetTable(-3);
    }
    return 1;
}

// seq, {{id, x, y, z, yaw, height, cls}, ...}
LUA_FUNCTION(L_GetNpcs) {
    if (!gHdr) { LUA->PushNumber(0); LUA->CreateTable(); return 2; }
    uint32_t seq = gHdr->npcSeq;
    MemoryBarrier();
    uint32_t n = gHdr->npcCount < gml::kMaxNpcs ? gHdr->npcCount : gml::kMaxNpcs;
    LUA->PushNumber(seq);
    LUA->CreateTable();
    for (uint32_t i = 0; i < n; ++i) {
        const gml::Npc& npc = gHdr->npcs[i];
        char id[24];
        sprintf_s(id, "%llx", static_cast<unsigned long long>(npc.handle));
        LUA->PushNumber(i + 1);
        LUA->CreateTable();
        LUA->PushString(id); LUA->SetField(-2, "id");
        LUA->PushNumber(npc.pos.x); LUA->SetField(-2, "x");
        LUA->PushNumber(npc.pos.y); LUA->SetField(-2, "y");
        LUA->PushNumber(npc.pos.z); LUA->SetField(-2, "z");
        LUA->PushNumber(npc.yaw); LUA->SetField(-2, "yaw");
        LUA->PushNumber(npc.height); LUA->SetField(-2, "height");
        LUA->PushString(npc.cls); LUA->SetField(-2, "cls");
        LUA->PushNumber(npc.flags); LUA->SetField(-2, "flags");
        if (npc.flags & gml::kNpcPosed) {
            LUA->PushNumber(npc.head.x); LUA->SetField(-2, "hx");
            LUA->PushNumber(npc.head.y); LUA->SetField(-2, "hy");
            LUA->PushNumber(npc.head.z); LUA->SetField(-2, "hz");
            LUA->PushNumber(npc.feet.x); LUA->SetField(-2, "fx");
            LUA->PushNumber(npc.feet.y); LUA->SetField(-2, "fy");
            LUA->PushNumber(npc.feet.z); LUA->SetField(-2, "fz");
        }
        LUA->SetTable(-3);
    }
    return 2;
}

// SetDriven({{id = "hex", x, y, z, vx, vy, vz, impact}, ...})
LUA_FUNCTION(L_SetDriven) {
    if (!gHdr) return 0;
    LUA->CheckType(1, Type::Table);
    uint32_t n = 0;
    for (int i = 1; n < gml::kMaxNpcs; ++i) {
        LUA->PushNumber(i);
        LUA->GetTable(1);
        if (!LUA->IsType(-1, Type::Table)) { LUA->Pop(); break; }
        gml::Driven& d = gHdr->driven[n++];
        auto num = [&](const char* k) {
            LUA->GetField(-1, k);
            double v = LUA->GetNumber(-1);
            LUA->Pop();
            return static_cast<float>(v);
        };
        LUA->GetField(-1, "id");
        d.handle = _strtoui64(LUA->GetString(-1) ? LUA->GetString(-1) : "0", nullptr, 16);
        LUA->Pop();
        d.pos = {num("x"), num("y"), num("z")};
        d.vel = {num("vx"), num("vy"), num("vz")};
        d.impact = num("impact");
        LUA->Pop();
    }
    gHdr->drivenCount = n;
    MemoryBarrier();
    gHdr->drivenSeq++;
    return 0;
}

LUA_FUNCTION(L_StartCapture) {
    bool ok = HookPresent();
    gCapturing = ok;
    LUA->PushBool(ok);
    return 1;
}

// Moves GMod's window off screen and out of the taskbar, and hands focus back to Dying Light.
// Moves GMod's window off screen and out of the taskbar, and hands focus back to Dying Light.
//
// Source throttles itself to ~20 fps whenever it isn't the active app, and GMod
// has no setting for that. So GMod's window never hears that it lost focus, and
// since an "active" Source game recenters and confines the mouse, it isn't
// allowed to touch the real cursor either (its input comes over the bridge).
WNDPROC gOrigWndProc = nullptr;

LRESULT CALLBACK StayActiveWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ACTIVATEAPP:
        case WM_NCACTIVATE:
            wp = TRUE;
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) wp = MAKEWPARAM(WA_ACTIVE, HIWORD(wp));
            break;
        case WM_KILLFOCUS:
            return 0;
    }
    return CallWindowProcW(gOrigWndProc, h, msg, wp, lp);
}

using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
SetCursorPos_t oSetCursorPos = nullptr;
ClipCursor_t oClipCursor = nullptr;
BOOL WINAPI HkSetCursorPos(int x, int y) { return gWindowHidden ? TRUE : oSetCursorPos(x, y); }
BOOL WINAPI HkClipCursor(const RECT* r) { return gWindowHidden ? TRUE : oClipCursor(r); }

// Opening the spawn menu makes Source pull its window to the front, which took
// focus away from Dying Light (and DL then closed the menu). Hidden GMod never
// needs the foreground.
using SetForegroundWindow_t = BOOL(WINAPI*)(HWND);
SetForegroundWindow_t oSetForegroundWindow = nullptr;
SetForegroundWindow_t oBringWindowToTop = nullptr;
BOOL WINAPI HkSetForegroundWindow(HWND h) {
    if (gWindowHidden) { InterlockedIncrement(&gFocusGrabsBlocked); return TRUE; }
    return oSetForegroundWindow(h);
}
BOOL WINAPI HkBringWindowToTop(HWND h) {
    if (gWindowHidden) { InterlockedIncrement(&gFocusGrabsBlocked); return TRUE; }
    return oBringWindowToTop(h);
}

LUA_FUNCTION(L_HideWindow) {
    HWND me = MainWindowOf(GetCurrentProcessId());
    if (!me) { LUA->PushBool(false); return 1; }
    bool wasForeground = GetForegroundWindow() == me;

    HMODULE u = GetModuleHandleW(L"user32.dll");
    MH_Initialize();
    MH_CreateHook(GetProcAddress(u, "SetCursorPos"), &HkSetCursorPos, reinterpret_cast<void**>(&oSetCursorPos));
    MH_CreateHook(GetProcAddress(u, "ClipCursor"), &HkClipCursor, reinterpret_cast<void**>(&oClipCursor));
    MH_CreateHook(GetProcAddress(u, "SetForegroundWindow"), &HkSetForegroundWindow, reinterpret_cast<void**>(&oSetForegroundWindow));
    MH_CreateHook(GetProcAddress(u, "BringWindowToTop"), &HkBringWindowToTop, reinterpret_cast<void**>(&oBringWindowToTop));
    MH_EnableHook(MH_ALL_HOOKS);
    gWindowHidden = true;
    if (!gOrigWndProc)
        gOrigWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(me, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&StayActiveWndProc)));

    ShowWindow(me, SW_HIDE);
    SetWindowLongPtrW(me, GWL_EXSTYLE, (GetWindowLongPtrW(me, GWL_EXSTYLE) | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW);
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;
    SetWindowPos(me, HWND_BOTTOM, x, 0, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(me, SW_SHOWNOACTIVATE);  // stays "visible" so Source keeps rendering

    // Give focus back. (Under tools/fakehost there's no game window; any other window does.)
    HWND dl = gHdr ? MainWindowOf(gHdr->dlPid) : nullptr;
    if (GetForegroundWindow() == me || wasForeground) oSetForegroundWindow(dl ? dl : GetShellWindow());
    Log("window hidden (was foreground %d, now foreground %d); presents skipped from now on",
        wasForeground, GetForegroundWindow() == me);
    ClipCursor(nullptr);
    LUA->PushBool(true);
    return 1;
}

void Reg(ILuaBase* L, const char* name, CFunc f) {
    L->PushCFunction(f);
    L->SetField(-2, name);
}

}  // namespace

GMOD_MODULE_OPEN() {
    Log("module loaded (GModLight %s, bridge v%u)", GMODLIGHT_VERSION, gml::kVersion);
    LUA->PushSpecial(SPECIAL_GLOB);
    LUA->CreateTable();
    Reg(LUA, "Connect", L_Connect);
    Reg(LUA, "Heartbeat", L_Heartbeat);
    Reg(LUA, "SetReady", L_SetReady);
    Reg(LUA, "GetMode", L_GetMode);
    Reg(LUA, "GetCamera", L_GetCamera);
    Reg(LUA, "PollInput", L_PollInput);
    Reg(LUA, "GetNpcs", L_GetNpcs);
    Reg(LUA, "SetDriven", L_SetDriven);
    Reg(LUA, "StartCapture", L_StartCapture);
    Reg(LUA, "HideWindow", L_HideWindow);
    Reg(LUA, "Log", L_Log);
    Reg(LUA, "SelfTest", L_SelfTest);
    Reg(LUA, "Stats", L_Stats);
    Reg(LUA, "GetDebugFlags", L_GetDebugFlags);
    Reg(LUA, "MarkFrameCamera", L_MarkFrameCamera);
    Reg(LUA, "Damage", L_Damage);
    Reg(LUA, "Kill", L_Kill);
    Reg(LUA, "GetSceneLight", L_GetSceneLight);
    Reg(LUA, "SetProbes", L_SetProbes);
    Reg(LUA, "GetProbeHits", L_GetProbeHits);
    LUA->SetField(-2, "gmodlight");
    LUA->Pop();
    return 0;
}

GMOD_MODULE_CLOSE() {
    gCapturing = false;
    if (gHdr) gHdr->gmodReady = 0;
    return 0;
}
