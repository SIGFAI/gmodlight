// Stand-in for Dying Light, for testing the Garry's Mod side alone.
// Creates the bridge, feeds a slowly turning camera, launches GMod the same way
// the plugin does, and reports what comes back. Saves GMod's frames as BMPs.
//
//   fakehost.exe [seconds] [--menu-at N] [--physgun-at N] [--spin]
//
// Sets GMODLIGHT_SELFTEST, so the addon spawns a few props 4 m in front of the
// camera. A fake zombie stands 3 m ahead; --physgun-at grabs and pushes it.
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../dl_plugin/src/shm.h"
#include "../shared/launch.h"

using namespace gml;

static double QpcSeconds() {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return double(q.QuadPart) / double(f.QuadPart);
}

// Does each frame's image match the camera recorded with it? The green debug
// box's centroid is compared with where the frame's camera projects the box's
// middle. A frame tagged with the wrong camera (e.g. the next frame's, under
// queued rendering) is off by the turn between them. Returns pixels, or NaN.
static float BoxReprojectionError(Header* hdr, Vec3 target, float vfovDeg, int* idDelta = nullptr) {
    FrameSlots& f = hdr->frame;
    uint32_t slot = f.latest;
    if (slot >= kFrameBuffers || !f.w) return NAN;
    f.reading = slot;
    MemoryBarrier();
    uint32_t w = f.w, h = f.h, stride = f.stride;
    const uint32_t* r = f.rect[slot];
    FrameCamera fc = f.cam[slot];
    const uint8_t* px = FramePtr(hdr, slot);
    // The self-test stamps its frame id (low 8 bits) into pixel (1, 1)'s red.
    if (idDelta) *idDelta = r[0] == 0 && r[1] == 0 ? int8_t(uint8_t(px[stride + 4 + 2] - uint8_t(fc.id))) : 999;
    double sx = 0;
    size_t n = 0;
    for (uint32_t y = r[1]; y < r[3] && y < h; ++y)
        for (uint32_t x = r[0]; x < r[2] && x < w; ++x) {
            const uint8_t* p = px + size_t(y) * stride + x * 4;
            if (p[1] > 150 && p[0] < 110 && p[2] < 110) { sx += x; ++n; }
        }
    f.reading = ~0u;
    if (n < 50 || !fc.valid) return NAN;
    Vec3 d{target.x - fc.pos.x, target.y - fc.pos.y, target.z - fc.pos.z};
    float z = d.x * fc.fwd.x + d.y * fc.fwd.y + d.z * fc.fwd.z;
    if (z < 0.5f) return NAN;
    float tanY = std::tan(vfovDeg * 0.5f * 0.0174533f), tanX = tanY * float(w) / float(h);
    float ndcX = -(d.x * fc.left.x + d.y * fc.left.y + d.z * fc.left.z) / (z * tanX);
    float expect = (ndcX * 0.5f + 0.5f) * float(w);
    if (expect < 60 || expect > float(w) - 60) return NAN;  // half off screen: centroid is biased
    return float(sx / n) - expect;
}

// A frame as captured, with the stale part outside its drawn rectangle keyed out.
struct Snap {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;  // BGRA
    FrameCamera cam{};
};

static Snap TakeSnap(Header* hdr) {
    Snap s;
    FrameSlots& f = hdr->frame;
    uint32_t slot = f.latest;
    if (slot >= kFrameBuffers || !f.w) return s;
    f.reading = slot;
    MemoryBarrier();
    s.w = f.w; s.h = f.h; s.cam = f.cam[slot];
    s.px.resize(size_t(s.w) * s.h * 4);
    memcpy(s.px.data(), FramePtr(hdr, slot), s.px.size());
    const uint32_t* r = f.rect[slot];
    f.reading = ~0u;
    for (uint32_t y = 0; y < s.h; ++y)
        for (uint32_t x = 0; x < s.w; ++x)
            if (x < r[0] || x >= r[2] || y < r[1] || y >= r[3]) {
                uint8_t* p = &s.px[(size_t(y) * s.w + x) * 4];
                p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 0;
            }
    return s;
}

static bool Drawn(const uint8_t* p) { return !(p[2] > 200 && p[1] < 60 && p[0] > 200); }

// The composite shader's re-projection, on the CPU: which pixel of `src` shows at
// (x, y) of a frame seen from `cur`. depth = false: rotation only (the old way).
static const uint8_t* Warp(const Snap& src, const FrameCamera& cur, float vfovDeg, int x, int y, bool depth) {
    auto dot = [](Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    float tanY = std::tan(vfovDeg * 0.5f * 0.0174533f), tanX = tanY * float(src.w) / float(src.h);
    float nx = (x + 0.5f) / src.w * 2 - 1, ny = 1 - (y + 0.5f) / src.h * 2;
    Vec3 dir{cur.fwd.x - nx * tanX * cur.left.x + ny * tanY * cur.up.x, cur.fwd.y - nx * tanX * cur.left.y + ny * tanY * cur.up.y,
             cur.fwd.z - nx * tanX * cur.left.z + ny * tanY * cur.up.z};
    const FrameCamera& fr = src.cam;
    float z = dot(dir, fr.fwd);
    if (z < 0.001f) return nullptr;
    float n0x = -dot(dir, fr.left) / (z * tanX), n0y = dot(dir, fr.up) / (z * tanY);
    auto at = [&](float ax, float ay) -> const uint8_t* {
        int px = int((ax * 0.5f + 0.5f) * src.w), py = int((0.5f - ay * 0.5f) * src.h);
        if (px < 0 || py < 0 || px >= int(src.w) || py >= int(src.h)) return nullptr;
        return &src.px[(size_t(py) * src.w + px) * 4];
    };
    if (depth)
        for (int it = 0; it < 4; ++it) {
            const uint8_t* p = at(n0x, n0y);
            if (!p || p[3] < 8 || p[3] == 255) break;
            float zf = (p[3] - 8) / 246.0f * 25.0f;
            Vec3 P{fr.pos.x + zf * (fr.fwd.x - n0x * tanX * fr.left.x + n0y * tanY * fr.up.x),
                   fr.pos.y + zf * (fr.fwd.y - n0x * tanX * fr.left.y + n0y * tanY * fr.up.y),
                   fr.pos.z + zf * (fr.fwd.z - n0x * tanX * fr.left.z + n0y * tanY * fr.up.z)};
            Vec3 v{P.x - cur.pos.x, P.y - cur.pos.y, P.z - cur.pos.z};
            float zc = dot(v, cur.fwd);
            if (zc < 0.05f) break;
            n0x -= -dot(v, cur.left) / (zc * tanX) - nx;
            n0y -= dot(v, cur.up) / (zc * tanY) - ny;
        }
    return at(n0x, n0y);
}

// How well `a` re-projected to `b`'s camera matches what GMod drew from there:
// intersection over union of the drawn (non-key) pixels.
static float WarpIoU(const Snap& a, const Snap& b, bool depth) {
    size_t both = 0, either = 0;
    for (uint32_t y = 0; y < b.h; y += 2)
        for (uint32_t x = 0; x < b.w; x += 2) {
            bool truth = Drawn(&b.px[(size_t(y) * b.w + x) * 4]);
            const uint8_t* w = Warp(a, b.cam, 50.0f, int(x), int(y), depth);
            bool got = w && Drawn(w);
            both += truth && got;
            either += truth || got;
        }
    return either ? float(both) / float(either) : 0.0f;
}

static void SaveBmp(const char* path, Header* hdr) {
    FrameSlots& f = hdr->frame;
    uint32_t slot = f.latest;
    if (slot >= kFrameBuffers || !f.w) return;
    f.reading = slot;
    MemoryBarrier();
    uint32_t w = f.w, h = f.h, stride = f.stride;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    memcpy(px.data(), FramePtr(hdr, slot), px.size());
    // Only the drawn rectangle is fresh; the rest of the slot is old frames.
    const uint32_t* r = f.rect[slot];
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            if (x < r[0] || x >= r[2] || y < r[1] || y >= r[3]) {
                uint8_t* p = &px[(size_t(y) * w + x) * 4];
                p[0] = 255; p[1] = 0; p[2] = 255;
            }
    printf("  drawn rect (%u, %u)-(%u, %u) = %.1f%% of the frame\n", r[0], r[1], r[2], r[3],
           r[2] > r[0] ? 100.0 * (r[2] - r[0]) * (r[3] - r[1]) / (double(w) * h) : 0.0);
    f.reading = ~0u;
    // The alpha channel (distances, see GML.DrawDepthAlpha) as its own grey image.
    {
        std::vector<uint8_t> ga(px.size());
        for (size_t i = 0; i < px.size(); i += 4) ga[i] = ga[i + 1] = ga[i + 2] = px[i + 3], ga[i + 3] = 255;
        std::string ap = std::string(path);
        ap.insert(ap.size() - 4, "_alpha");
        BITMAPFILEHEADER afh{};
        BITMAPINFOHEADER aih{};
        aih.biSize = sizeof(aih); aih.biWidth = LONG(w); aih.biHeight = -LONG(h); aih.biPlanes = 1; aih.biBitCount = 32;
        afh.bfType = 0x4D42; afh.bfOffBits = sizeof(afh) + sizeof(aih); afh.bfSize = DWORD(afh.bfOffBits + ga.size());
        FILE* ao = nullptr;
        if (!fopen_s(&ao, ap.c_str(), "wb") && ao) {
            fwrite(&afh, sizeof(afh), 1, ao); fwrite(&aih, sizeof(aih), 1, ao); fwrite(ga.data(), 1, ga.size(), ao); fclose(ao);
        }
    }
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = LONG(w);
    ih.biHeight = -LONG(h);  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = DWORD(fh.bfOffBits + px.size());
    FILE* out = nullptr;
    if (fopen_s(&out, path, "wb") || !out) return;
    fwrite(&fh, sizeof(fh), 1, out);
    fwrite(&ih, sizeof(ih), 1, out);
    fwrite(px.data(), 1, px.size(), out);
    fclose(out);
    // Fraction of the frame that is chroma key, as a sanity check.
    size_t key = 0;
    for (size_t i = 0; i < px.size(); i += 4)
        key += px[i + 2] > 200 && px[i + 1] < 60 && px[i] > 200;
    // Where the green debug box is, to check the projection against the camera.
    double gx = 0; size_t green = 0;
    for (size_t i = 0; i < px.size(); i += 4)
        if (px[i + 1] > 180 && px[i] < 90 && px[i + 2] < 90) { gx += double((i / 4) % w); ++green; }
    printf("  saved %s (%ux%u, %.1f%% chroma key, debug box x %.0f from %zu px)\n", path, w, h,
           100.0 * key / (size_t(w) * h), green ? gx / green : -1.0, green);
}

int main(int argc, char** argv) {
    int seconds = 90, menuAt = -1, physgunAt = -1;
    bool spin = false;
    float spinRate = 0.3f;  // rad/s; --spin-rate R
    // --real: numbers like a real Dying Light session. Main menu camera far away
    // for the first 6 s, then the player ~500 m from the origin. (The bridge
    // carries the corrected camera, so the basis is the same as without --real.)
    bool real = false;
    // --fovtest: level camera, zombie 3 m ahead and 1.5 m aside. With the 50 deg
    // vertical fov at 1600x669 its box should be centred ~359 px off the middle.
    bool fovTest = false;
    unsigned width = 1600;
    float latencyMs = 20;  // --latency MS
    bool night = false;  // --night: tell GMod the scene is dark blue, like Dying Light at night
    bool lying = false;  // --lying: the zombie lies on the ground
    float parallaxMove = 0.4f;  // --parallax-move M
    bool parallaxTest = false;  // --parallaxtest: depth-aware re-projection vs GMod's own render
    std::wstring gmodArgs;  // --gmodargs "...": extra GMod command line (e.g. "+mat_queue_mode 0")
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--menu-at") && i + 1 < argc) menuAt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--physgun-at") && i + 1 < argc) physgunAt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spin")) spin = true;
        else if (!strcmp(argv[i], "--spin-rate") && i + 1 < argc) { spin = true; spinRate = float(atof(argv[++i])); }
        else if (!strcmp(argv[i], "--real")) real = true;
        else if (!strcmp(argv[i], "--fovtest")) fovTest = true;
        else if (!strcmp(argv[i], "--night")) night = true;
        else if (!strcmp(argv[i], "--latency") && i + 1 < argc) latencyMs = float(atof(argv[++i]));
        else if (!strcmp(argv[i], "--lying")) lying = true;
        else if (!strcmp(argv[i], "--parallaxtest")) parallaxTest = true;
        else if (!strcmp(argv[i], "--parallax-move") && i + 1 < argc) { parallaxTest = true; parallaxMove = float(atof(argv[++i])); }
        else if (!strcmp(argv[i], "--gmodargs") && i + 1 < argc) { std::string a = argv[++i]; gmodArgs.assign(a.begin(), a.end()); }
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = unsigned(atoi(argv[++i]));
        else seconds = atoi(argv[i]);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    timeBeginPeriod(1);  // Sleep(16) is ~16 ms, not 31: a 60 Hz "Dying Light"
    if (!shm::Create()) { printf("shared memory failed: %lu\n", GetLastError()); return 1; }
    Header* hdr = shm::Get();
    // Like the plugin: tell GMod each time there's a new camera (lockstep).
    HANDLE frameEvent = CreateEventW(nullptr, FALSE, FALSE, kFrameEventName);

    const std::wstring dir = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\GarrysMod";
    // Same as the real run (3440x1440 scaled to MaxWidth); --width picks another width.
    const unsigned W = width, H = unsigned(width * 1440.0 / 3440.0 + 0.5);
    std::wstring line = GModCommandLine(dir + L"\\bin\\win64\\gmod.exe", W, H, gmodArgs);
    std::vector<wchar_t> cmd(line.begin(), line.end());
    cmd.push_back(0);

    SetEnvironmentVariableW(L"GMODLIGHT_SELFTEST", L"1");  // inherited by gmod
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi)) {
        printf("could not start gmod: %lu\n", GetLastError());
        return 1;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    printf("started gmod pid %lu\n", pi.dwProcessId);

    DWORD start = GetTickCount();
    const double qStart = QpcSeconds();
    DWORD lastReport = 0;
    bool savedFirst = false;
    uint32_t mode = kModePlay;
    for (;;) {
        DWORD now = GetTickCount() - start;
        float t = now / 1000.0f;
        if (t > seconds) break;

        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE) {
            printf("gmod EXITED after %.1fs with code 0x%08lx\n", t, code);
            return 2;
        }

        // Standing 1.7 m up looking down +X a little (or turning with --spin).
        Vec3 o{0, 0, 0};
        if (real) o = t < 6 ? Vec3{245.68f - 0.0f, 138.64f - 1.7f, 87.70f} : Vec3{489.6f, 139.4f, 165.3f};
        auto camAt = [&](double q) {
            bool moved = parallaxTest && q - qStart >= 12.0;  // --parallaxtest: step 40 cm aside and turn a little
            float yaw = (spin ? float(q - qStart) * spinRate : 0.0f) + (moved ? 0.05f : 0.0f), pitch = fovTest ? 0.0f : -0.25f;
            Camera c{};
            c.pos = {o.x, o.y + 1.7f, o.z + (moved ? parallaxMove : 0.0f)};
            c.fwd = {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
            c.up = {-std::sin(pitch) * std::cos(yaw), std::cos(pitch), -std::sin(pitch) * std::sin(yaw)};
            // Left-handed world (x right, y up, z forward): left = forward x up.
            c.left = {c.fwd.y * c.up.z - c.fwd.z * c.up.y, c.fwd.z * c.up.x - c.fwd.x * c.up.z, c.fwd.x * c.up.y - c.fwd.y * c.up.x};
            return c;
        };
        double q = QpcSeconds();
        Camera c = camAt(q);
        c.fovDeg = 50;
        c.aspect = float(W) / H;
        c.viewW = 3440;
        c.viewH = 1440;
        c.valid = 1;
        c.seq = hdr->cam.seq + 1;
        c.time = q;
        hdr->latency = latencyMs / 1000.0f;  // as if DL showed frames this long after the camera sample
        hdr->cam = c;
        hdr->dlHeartbeat++;
        if (frameEvent) SetEvent(frameEvent);
        // How old is GMod's newest frame now (what DL would composite)?
        {
            static double ageSum = 0, ageMax = 0;
            static int ageN = 0;
            uint32_t sl = hdr->frame.latest;
            if (sl < kFrameBuffers && hdr->frame.cam[sl].time > 0) {
                double age = (q - hdr->frame.cam[sl].time) * 1000.0;
                if (age >= 0 && age < 500) { ageSum += age; ageMax = std::max(ageMax, age); ++ageN; }
            }
            if (ageN >= 300) {
                printf("[%5.1fs] GMod frame age when used: avg %.1f ms, max %.1f ms\n", t, ageSum / ageN, ageMax);
                ageSum = ageMax = 0;
                ageN = 0;
            }
        }
        if (night) { hdr->scene.r = 0.04f; hdr->scene.g = 0.05f; hdr->scene.b = 0.08f; hdr->scene.seq++; }

        uint32_t want = kModePlay;
        if (menuAt >= 0 && t >= menuAt && t < menuAt + 15) want = kModeSpawnMenu;
        if (physgunAt >= 0 && t >= physgunAt && t < physgunAt + 22) want = kModeGMod;
        if (want != mode) {
            mode = want;
            hdr->mode = mode;
            printf("[%5.1fs] mode -> %u\n", t, mode);
            if (mode == kModeSpawnMenu) shm::Push(kInMouseMove, W / 2, H / 2);
        }
        // Wiggle the menu cursor so we can see it move in the frames.
        if (mode == kModeSpawnMenu && (now / 16) % 10 == 0)
            shm::Push(kInMouseMove, int(W / 2 + 200 * std::cos(t)), int(H / 2 + 100 * std::sin(t)));

        // One fake zombie 3 m ahead, unless GMod is moving it.
        Npc& z = hdr->npcs[0];
        z.handle = 0x1234;
        bool driven = false;
        for (uint32_t i = 0; i < hdr->drivenCount; ++i)
            if (hdr->driven[i].handle == z.handle) { driven = true; z.pos = hdr->driven[i].pos; }
        if (!driven) z.pos = {o.x + 3, o.y, o.z + (fovTest ? 1.5f : 0.0f)};
        static DWORD lastDrivenPrint = 0;
        if (driven && now - lastDrivenPrint >= 1000) {
            lastDrivenPrint = now;
            printf("[%5.1fs]   zombie driven to (%.2f, %.2f, %.2f)\n", t, z.pos.x - o.x, z.pos.y - o.y, z.pos.z - o.z);
        }
        z.height = 1.8f;
        z.flags = kNpcAlive | kNpcPosed;
        // Skeleton: standing, or with --lying flat on the ground pointing away to the side.
        z.feet = {z.pos.x, z.pos.y + 0.08f, z.pos.z};
        z.head = lying ? Vec3{z.pos.x, z.pos.y + 0.15f, z.pos.z + 1.55f} : Vec3{z.pos.x, z.pos.y + 1.62f, z.pos.z};
        strcpy_s(z.cls, "FakeZombie");
        hdr->npcCount = 1;
        hdr->npcSeq++;

        // GMod hands, like a player would: scroll the weapon wheel and click to pick,
        // press 1, then grab the zombie with the physgun and push it away with the wheel.
        if (mode == kModeGMod) {
            float pt = t - physgunAt;
            static int step = 0;
            struct Act { float at; uint32_t type; int a; const char* what; };
            static const Act acts[] = {
                {2.0f, kInMouseDown, 0, "physgun: grab"},
                {4.0f, kInWheel, 1, "push"}, {4.4f, kInWheel, 1, nullptr}, {4.8f, kInWheel, 1, nullptr},
                {5.2f, kInWheel, 1, nullptr}, {5.6f, kInWheel, 1, nullptr},
                {6.5f, 0, 1, "screenshot physgun"},
                {8.0f, kInMouseUp, 0, "physgun: drop"},
                {11.0f, kInWheel, -1, "wheel down"},
                {11.5f, kInWheel, -1, "wheel down"},
                {12.0f, 0, 0, "screenshot weapon selection"},
                {12.5f, kInMouseDown, 0, "click to pick"},
                {12.7f, kInMouseUp, 0, nullptr},
                {15.0f, kInKeyDown, '2', "press 2 (pistol slot)"},
                {15.2f, kInKeyUp, '2', nullptr},
                {15.6f, kInMouseDown, 0, "click to pick"},
                {15.8f, kInMouseUp, 0, nullptr},
                {17.0f, kInMouseDown, 0, "shoot the zombie"}, {17.1f, kInMouseUp, 0, nullptr},
                {17.6f, kInMouseDown, 0, nullptr}, {17.7f, kInMouseUp, 0, nullptr},
                {18.2f, kInMouseDown, 0, nullptr}, {18.3f, kInMouseUp, 0, nullptr},
                {18.25f, 0, 1, "screenshot shooting"},
                {18.8f, kInMouseDown, 0, nullptr}, {18.9f, kInMouseUp, 0, nullptr},
                {19.4f, kInMouseDown, 0, nullptr}, {19.5f, kInMouseUp, 0, nullptr},
                {20.0f, kInMouseDown, 0, nullptr}, {20.1f, kInMouseUp, 0, nullptr},
                {20.6f, kInMouseDown, 0, nullptr}, {20.7f, kInMouseUp, 0, nullptr},
                {21.2f, kInMouseDown, 0, nullptr}, {21.3f, kInMouseUp, 0, nullptr},
                {21.8f, kInMouseDown, 0, nullptr}, {21.9f, kInMouseUp, 0, nullptr},
            };
            while (step < int(sizeof(acts) / sizeof(acts[0])) && pt >= acts[step].at) {
                const Act& a = acts[step++];
                if (a.what) printf("[%5.1fs] %s\n", t, a.what);
                if (a.type) shm::Push(a.type, a.a);
                else SaveBmp(a.a ? "fakehost_physgun.bmp" : "fakehost_weapons.bmp", hdr);
            }
        }

        // DL's world as far as probes go: ground where the zombie stands, and a wall
        // 12 m ahead (x = o.x + 12, facing back toward the camera).
        {
            static uint32_t lastProbeSeq = 0;
            Probes& p = hdr->probes;
            if (p.seq != lastProbeSeq) {
                lastProbeSeq = p.seq;
                uint32_t n = std::min(p.count, kMaxProbes), w = 0;
                for (uint32_t i = 0; i < n; ++i) {
                    const Probe& pr = p.probes[i];
                    Vec3 d{pr.to.x - pr.from.x, pr.to.y - pr.from.y, pr.to.z - pr.from.z};
                    float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                    float best = 2, u;
                    Vec3 normal{};
                    if (d.y < 0 && (u = (o.y - pr.from.y) / d.y) >= 0 && u <= 1 && u < best) { best = u; normal = {0, 1, 0}; }
                    if (d.x > 0 && (u = (o.x + 12 - pr.from.x) / d.x) >= 0 && u <= 1 && u < best) { best = u; normal = {-1, 0, 0}; }
                    if (best > 1) continue;
                    ProbeHit& h = p.results[w++];
                    h.id = pr.id;
                    h.dist = best * len;
                    h.pos = {pr.from.x + d.x * best, pr.from.y + d.y * best, pr.from.z + d.z * best};
                    h.normal = normal;
                    printf("[%5.1fs] probe %u hit %s at (%.2f, %.2f, %.2f)\n", t, h.id, normal.y > 0 ? "ground" : "wall",
                           h.pos.x - o.x, h.pos.y - o.y, h.pos.z - o.z);
                }
                p.resultCount = w;
                MemoryBarrier();
                p.resultSeq = p.seq;
            }
        }

        // Image vs recorded camera, a few times a second (see BoxReprojectionError).
        static double errSum = 0, errMax = 0;
        static int errN = 0;
        static DWORD lastErrCheck = 0;
        if (!lying && now - lastErrCheck >= 50) {
            lastErrCheck = now;
            int idDelta = 999;
            float e = BoxReprojectionError(hdr, {z.pos.x, z.pos.y + 0.9f, z.pos.z}, 50.0f, &idDelta);
            static int idHist[5] = {};
            if (idDelta >= -2 && idDelta <= 2) idHist[idDelta + 2]++;
            static DWORD lastIdLog = 0;
            if (now - lastIdLog > 5000) {
                lastIdLog = now;
                printf("[%5.1fs] image's frame id minus its camera's: -2:%d -1:%d 0:%d +1:%d +2:%d\n", t, idHist[0], idHist[1], idHist[2], idHist[3], idHist[4]);
                memset(idHist, 0, sizeof(idHist));
            }
            if (e == e) { errSum += std::fabs(e); errMax = std::max(errMax, double(std::fabs(e))); ++errN; }
        }
        if (errN >= 20) {
            printf("[%5.1fs] box vs its frame's camera: avg %.1f px, max %.1f px (%d frames)\n", t, errSum / errN, errMax, errN);
            errSum = errMax = 0;
            errN = 0;
        }

        if (parallaxTest) {
            static Snap before;
            static bool tookBefore = false, done = false;
            if (!tookBefore && t >= 10.5f) { before = TakeSnap(hdr); tookBefore = true; printf("[%5.1fs] parallax test: frame from the first camera taken\n", t); }
            if (!done && t >= 17.0f && tookBefore) {
                done = true;
                Snap after = TakeSnap(hdr);
                float moved = std::sqrt(std::pow(after.cam.pos.x - before.cam.pos.x, 2.0f) + std::pow(after.cam.pos.z - before.cam.pos.z, 2.0f));
                printf("[%5.1fs] parallax test: camera moved %.2f m; GMod's old frame re-projected vs what it draws from the new camera (IoU of drawn pixels):\n", t, moved);
                printf("    rotation only %.3f, with distances %.3f\n", WarpIoU(before, after, false), WarpIoU(before, after, true));
            }
        }

        // What DL would act out.
        while (hdr->events.tail != hdr->events.head) {
            const Event& e = hdr->events.events[hdr->events.tail % kMaxEvents];
            if (e.type == kEvKill) {
                printf("[%5.1fs] EVENT kill %llx\n", t, (unsigned long long)e.handle);
                hdr->events.tail = hdr->events.tail + 1;
                continue;
            }
            printf("[%5.1fs] EVENT damage %.1f type %u to %llx at (%.2f, %.2f, %.2f) dir (%.2f, %.2f, %.2f)\n", t, e.amount, e.dlDamageType,
                   (unsigned long long)e.handle, e.pos.x, e.pos.y, e.pos.z, e.dir.x, e.dir.y, e.dir.z);
            hdr->events.tail = hdr->events.tail + 1;
        }

        if (!savedFirst && hdr->frame.seq > 0) {
            savedFirst = true;
            printf("[%5.1fs] first frame\n", t);
            SaveBmp("fakehost_first.bmp", hdr);
        }
        if (now - lastReport >= 5000) {
            lastReport = now;
            // The camera GMod drew the newest frame with, against where this camera
            // really points when DL would show it (sample + latency).
            // Alpha of what GMod drew (depth, if it writes depth to dest alpha).
            {
                uint32_t sl = hdr->frame.latest;
                if (sl < kFrameBuffers) {
                    const uint32_t* rr = hdr->frame.rect[sl];
                    const uint8_t* fp = FramePtr(hdr, sl);
                    int hist[8] = {}, keyA[8] = {};
                    for (uint32_t y = rr[1]; y < rr[3]; y += 2)
                        for (uint32_t x = rr[0]; x < rr[2]; x += 2) {
                            const uint8_t* p = fp + size_t(y) * hdr->frame.stride + x * 4;
                            bool key = p[2] > 200 && p[1] < 60 && p[0] > 200;
                            (key ? keyA : hist)[p[3] / 32]++;
                        }
                    printf("[%5.1fs] alpha of drawn pixels by 32s: %d %d %d %d %d %d %d %d | key: %d %d %d %d %d %d %d %d\n", t, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], keyA[0], keyA[1], keyA[2], keyA[3], keyA[4], keyA[5], keyA[6], keyA[7]);
                }
            }
            uint32_t ls = hdr->frame.latest;
            if (ls < kFrameBuffers && hdr->frame.cam[ls].time > 0) {
                const FrameCamera& fc = hdr->frame.cam[ls];
                Camera shown = camAt(fc.time + hdr->latency), sampled = camAt(fc.time);
                auto deg = [](Vec3 a, Vec3 b) {
                    float d = a.x * b.x + a.y * b.y + a.z * b.z;
                    return std::acos(std::max(-1.0f, std::min(1.0f, d))) * 57.2958f;
                };
                printf("[%5.1fs] frame view vs shown: %.3f deg (unpredicted %.3f deg), up %.3f deg, left %.3f deg\n", t,
                       deg(fc.fwd, shown.fwd), deg(sampled.fwd, shown.fwd), deg(fc.up, shown.up), deg(fc.left, shown.left));
            }
            static uint32_t lastFrames = 0;
            printf("[%5.1fs] gmod ready %u, %u fps, driven %u, zombie at (%.2f, %.2f, %.2f)\n", t, hdr->gmodReady,
                   (hdr->frame.seq - lastFrames) / 5, hdr->drivenCount, hdr->npcs[0].pos.x, hdr->npcs[0].pos.y,
                   hdr->npcs[0].pos.z);
            lastFrames = hdr->frame.seq;
            if (mode == kModeSpawnMenu) SaveBmp("fakehost_menu.bmp", hdr);
            if (mode == kModePlay && t > 15) SaveBmp("fakehost_play.bmp", hdr);
        }
        Sleep(16);
    }
    SaveBmp("fakehost_last.bmp", hdr);
    printf("done; closing gmod\n");
    return 0;
}
