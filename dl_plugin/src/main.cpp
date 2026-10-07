// GModLight: Dying Light side. Loaded by the ASI loader (dsound.dll).
//
// Dying Light stays the real game. This plugin:
//  - publishes DL's camera and nearby actors to shared memory,
//  - launches a hidden Garry's Mod that renders its spawn menu, physgun and props
//    from the same camera, and composites that frame over DL (overlay.cpp),
//  - routes input to GMod while the spawn menu or physgun is in use,
//  - moves DL actors that GMod is physgunning or flinging.
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "engine.h"
#include "log.h"
#include "overlay.h"
#include "shm.h"
#include "../../shared/launch.h"

namespace gml {
namespace {

// ---------- config ----------

struct Config {
    UINT keySpawnMenu = VK_F1;
    bool spawnMenuHold = true;
    UINT keyPhysgun = VK_F2;
    UINT keyOverlay = VK_F7;
    UINT keyDump = VK_F8;
    UINT keyDebugBoxes = VK_F9;
    UINT keyScreenshot = VK_F10;
    float cursorSpeed = 1.0f;
    bool gmodAutoStart = true;
    std::wstring gmodPath;   // folder containing gmod.exe; auto-detected if empty
    int gmodMaxWidth = 2560;
    std::wstring gmodArgs;
    bool npcSync = false;
    std::vector<std::string> npcClasses;  // substrings of RTTI class names to mirror
    float npcRadius = 40.0f;
    float damageScale = 3.0f;      // GMod damage -> DL damage
    bool damageAttacker = true;    // pass DL's player as the attacker (the game always has one)
    bool hideOwnModel = true;      // hide DL's own arms in GMod hands
    // Game object classes of what Kyle holds and his first-person arms.
    std::vector<std::string> hideClasses{"PlayerFppVis", "WeaponVis", "ItemVis"};
    bool hideWeaponHud = true;     // hide DL's weapon HUD (held item, durability, crosshair) too
    std::vector<std::string> hideHudClasses{"HudPrimaryWeaponIndicator", "HudSecondaryWeaponIndicator", "HudHeldItem",
                                            "HudWeaponSelector", "HudCrosshair"};
    float hideRadius = 3.0f;       // only those this close to the camera (not loot on the ground)
    int autoScreenshots = 12;
    bool boxesOnStart = false;     // zombie boxes visible from the start (F9 toggles)
    bool wallsBlockShots = false;  // drop GMod hits with Dying Light world in between
    bool thrownHitWorld = true;    // thrown actors stop at Dying Light's walls
    bool releaseFix = true;        // on release, move the AI's own idea of the actor's position too
    bool gameThreadWork = true;    // move/damage actors on the game's update thread, not in Present
    bool useViewMatrix = true;     // camera from DL's render view matrix (includes shake/head bob)
    int maxFps = 60;               // Dying Light frame cap (0 = off)
    bool lockstep = true;          // GMod renders one frame per DL frame, right after DL's camera
    bool parallax = true;          // depth-aware re-projection (GMod sends distances in alpha)
    bool useCombined = true;       // camera from the renderer's combined matrix (the frame actually drawn)
    double driveExtrapolate = 0.035;  // s: how far held/thrown actors are carried on between GMod updates
} cfg;

std::wstring gDir;  // folder the .asi lives in

UINT ParseKey(const wchar_t* s, UINT def) {
    if (!s || !*s) return def;
    if ((s[0] == L'F' || s[0] == L'f') && iswdigit(s[1])) {
        int n = _wtoi(s + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (!s[1] && iswalnum(s[0])) return towupper(s[0]);
    if (iswdigit(s[0])) return static_cast<UINT>(wcstoul(s, nullptr, 0));
    return def;
}

std::vector<std::string> SplitList(const std::wstring& w) {
    std::vector<std::string> out;
    std::string cur;
    for (wchar_t c : w + L",") {
        if (c == L',') {
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            size_t b = cur.find_first_not_of(' ');
            if (b != std::string::npos) out.push_back(cur.substr(b));
            cur.clear();
        } else {
            cur += static_cast<char>(c);
        }
    }
    return out;
}

void LoadConfig() {
    std::wstring ini = gDir + L"\\GModLight.ini";
    wchar_t buf[1024];
    auto str = [&](const wchar_t* sec, const wchar_t* key) {
        GetPrivateProfileStringW(sec, key, L"", buf, 1024, ini.c_str());
        return std::wstring(buf);
    };
    auto num = [&](const wchar_t* sec, const wchar_t* key, int def) {
        return static_cast<int>(GetPrivateProfileIntW(sec, key, def, ini.c_str()));
    };
    cfg.keySpawnMenu = ParseKey(str(L"Keys", L"SpawnMenu").c_str(), cfg.keySpawnMenu);
    cfg.spawnMenuHold = num(L"Keys", L"SpawnMenuHold", 1) != 0;
    cfg.keyPhysgun = ParseKey(str(L"Keys", L"Physgun").c_str(), cfg.keyPhysgun);  // older name
    cfg.keyPhysgun = ParseKey(str(L"Keys", L"GModHands").c_str(), cfg.keyPhysgun);
    cfg.keyOverlay = ParseKey(str(L"Keys", L"ToggleOverlay").c_str(), cfg.keyOverlay);
    cfg.keyDump = ParseKey(str(L"Keys", L"DebugDump").c_str(), cfg.keyDump);
    cfg.keyDebugBoxes = ParseKey(str(L"Keys", L"DebugBoxes").c_str(), cfg.keyDebugBoxes);
    cfg.keyScreenshot = ParseKey(str(L"Keys", L"Screenshot").c_str(), cfg.keyScreenshot);
    cfg.cursorSpeed = num(L"Keys", L"CursorSpeedPercent", 100) / 100.0f;
    cfg.gmodAutoStart = num(L"GMod", L"AutoStart", 1) != 0;
    cfg.gmodPath = str(L"GMod", L"Path");
    cfg.gmodMaxWidth = num(L"GMod", L"MaxWidth", 2560);
    cfg.gmodArgs = str(L"GMod", L"ExtraArgs");
    cfg.npcSync = num(L"Npcs", L"Enabled", 0) != 0;
    cfg.npcClasses = SplitList(str(L"Npcs", L"ClassContains"));
    cfg.npcRadius = static_cast<float>(num(L"Npcs", L"Radius", 40));
    cfg.damageScale = num(L"Npcs", L"DamagePercent", 500) / 100.0f;
    cfg.damageAttacker = num(L"Npcs", L"PlayerIsAttacker", 1) != 0;
    cfg.hideOwnModel = num(L"Hands", L"HideOwnModel", 1) != 0;
    if (!str(L"Hands", L"HideClasses").empty()) cfg.hideClasses = SplitList(str(L"Hands", L"HideClasses"));
    cfg.hideRadius = static_cast<float>(num(L"Hands", L"HideRadius", 3));
    cfg.hideWeaponHud = num(L"Hands", L"HideWeaponHud", 1) != 0;
    if (!str(L"Hands", L"HideHudClasses").empty()) cfg.hideHudClasses = SplitList(str(L"Hands", L"HideHudClasses"));
    cfg.autoScreenshots = num(L"Debug", L"AutoScreenshots", 12);
    cfg.boxesOnStart = num(L"Debug", L"BoxesOnStart", 0) != 0;
    cfg.wallsBlockShots = num(L"Npcs", L"WallsBlockShots", 0) != 0;
    cfg.thrownHitWorld = num(L"Npcs", L"ThrownHitWorld", 1) != 0;
    cfg.gameThreadWork = num(L"Advanced", L"GameThread", 1) != 0;
    cfg.releaseFix = num(L"Npcs", L"ReleaseFix", 1) != 0;
    cfg.useViewMatrix = num(L"Advanced", L"UseViewMatrix", 1) != 0;
    cfg.useCombined = num(L"Advanced", L"UseRenderCamera", 1) != 0;
    cfg.parallax = num(L"Advanced", L"Parallax", 1) != 0;
    cfg.maxFps = num(L"Performance", L"MaxFPS", 60);
    cfg.lockstep = num(L"Performance", L"Lockstep", 1) != 0;
    cfg.driveExtrapolate = num(L"Npcs", L"ExtrapolateMs", 35) / 1000.0;
    LOGI("config: spawn menu key %u (%s), physgun key %u, npc sync %d (%zu class filters)",
         cfg.keySpawnMenu, cfg.spawnMenuHold ? "hold" : "toggle", cfg.keyPhysgun, cfg.npcSync,
         cfg.npcClasses.size());
}

// ---------- launching Garry's Mod ----------

HANDLE gJob = nullptr;
HANDLE gGModProcess = nullptr;
std::atomic<bool> gLaunched{false};
int gRestarts = 0;
DWORD gFocusLockedAt = 0;  // GetTickCount when we locked the foreground, 0 if not locked
unsigned gLaunchW = 0, gLaunchH = 0;

bool ProcessRunning(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, exe)) { found = true; break; }
    CloseHandle(snap);
    return found;
}

std::wstring FindGModFolder() {
    if (!cfg.gmodPath.empty()) return cfg.gmodPath;
    wchar_t steam[MAX_PATH];
    DWORD n = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &n) != ERROR_SUCCESS)
        return L"";
    std::vector<std::wstring> libs{steam};
    // libraryfolders.vdf lists every Steam library as "path" "X:\\..."
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(steam) + L"\\steamapps\\libraryfolders.vdf").c_str(), L"r") == 0 && f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            const char* p = strstr(line, "\"path\"");
            if (!p) continue;
            p = strchr(p + 6, '"');
            if (!p) continue;
            std::string path;
            for (++p; *p && *p != '"'; ++p) {
                if (*p == '\\' && p[1] == '\\') ++p;
                path += *p;
            }
            libs.emplace_back(path.begin(), path.end());
        }
        fclose(f);
    }
    for (auto& lib : libs) {
        std::wstring dir = lib + L"\\steamapps\\common\\GarrysMod";
        if (GetFileAttributesW((dir + L"\\gmod.exe").c_str()) != INVALID_FILE_ATTRIBUTES) return dir;
    }
    return L"";
}

void LaunchGMod(unsigned dlW, unsigned dlH) {
    if (gLaunched.exchange(true) || !cfg.gmodAutoStart) return;
    gLaunchW = dlW;
    gLaunchH = dlH;
    if (ProcessRunning(L"gmod.exe")) {
        LOGW("gmod.exe is already running; using it as is (start it with the GModLight launch options or close it)");
        return;
    }
    std::wstring dir = FindGModFolder();
    if (dir.empty()) { LOGE("Garry's Mod not found; set [GMod] Path in GModLight.ini"); return; }

    // Same aspect as DL so the frames line up; capped so the copy stays cheap.
    unsigned w = dlW, h = dlH;
    if (w > unsigned(cfg.gmodMaxWidth)) { h = h * cfg.gmodMaxWidth / w; w = cfg.gmodMaxWidth; }
    w = std::min(w, kMaxFrameW); h = std::min(h, kMaxFrameH);

    // On the x86-64 branch the top-level gmod.exe is a 32-bit launcher; start the real game.
    std::wstring exe = dir + L"\\bin\\win64\\gmod.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) exe = dir + L"\\gmod.exe";

    std::wstring cmdLine = GModCommandLine(exe, w, h, cfg.gmodArgs);
    std::vector<wchar_t> cmd(cmdLine.begin(), cmdLine.end());
    cmd.push_back(0);

    if (!gJob) gJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;  // GMod closes with DL
    SetInformationJobObject(gJob, JobObjectExtendedLimitInformation, &li, sizeof(li));

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi)) {
        LOGE("could not start Garry's Mod (%lu): %ls", GetLastError(), cmdLine.c_str());
        return;
    }
    AssignProcessToJobObject(gJob, pi.hProcess);
    // Source grabs the foreground when its window opens; a fullscreen Dying Light
    // can minimize when that happens. Keep focus here until GMod has hidden itself.
    if (LockSetForegroundWindow(LSFW_LOCK)) gFocusLockedAt = GetTickCount();
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (gGModProcess) CloseHandle(gGModProcess);
    gGModProcess = pi.hProcess;
    LOGI("started Garry's Mod %ux%u: %ls", w, h, cmdLine.c_str());
}

// Unlock the foreground once GMod is up, and restart GMod if it dies.
void WatchGMod() {
    Header* hdr = shm::Get();
    if (gFocusLockedAt && ((hdr && hdr->gmodReady) || GetTickCount() - gFocusLockedAt > 120000)) {
        // Lua hides the window 3 s after connecting; give it a little longer.
        static DWORD readySince = 0;
        if (!readySince) readySince = GetTickCount();
        if (GetTickCount() - readySince > 6000 || GetTickCount() - gFocusLockedAt > 120000) {
            LockSetForegroundWindow(LSFW_UNLOCK);
            gFocusLockedAt = 0;
            readySince = 0;
        }
    }
    static DWORD lastCheck = 0;
    if (!gGModProcess || GetTickCount() - lastCheck < 1000) return;
    lastCheck = GetTickCount();
    DWORD code = 0;
    if (!GetExitCodeProcess(gGModProcess, &code) || code == STILL_ACTIVE) return;
    LOGW("Garry's Mod exited (code 0x%08lx)", code);
    CloseHandle(gGModProcess);
    gGModProcess = nullptr;
    if (hdr) { hdr->gmodReady = 0; hdr->gmPid = 0; }
    if (gRestarts < 2) {
        ++gRestarts;
        LOGI("restarting Garry's Mod (%d of 2)", gRestarts);
        gLaunched = false;
        LaunchGMod(gLaunchW, gLaunchH);
    }
}

// ---------- input routing ----------
//
// Everything here stays inside Dying Light's process; nothing hooks input
// system-wide, so nothing here can ever hold the mouse or keyboard of other apps.
//  - Our keys, mouse buttons and typing are read once per DL frame from the
//    current key state (PollInput).
//  - The spawn menu cursor follows the real Windows cursor, which DL isn't
//    allowed to re-centre or clip while the menu is open.
//  - DL is kept from reacting by filtering its own reads of raw input.

std::atomic<uint32_t> gMode{kModePlay};
uint32_t gModeBeforeMenu = kModePlay;
std::atomic<HWND> gHwnd{nullptr};
std::atomic<bool> gPaused{false};
std::atomic<bool> gDumpRequested{false};
std::atomic<int> gWheel{0};       // wheel notches seen in DL's raw input, for GMod
bool gWasDown[256] = {};
int gLastCursorX = -1, gLastCursorY = -1;

using GetKeyState_t = SHORT(WINAPI*)(int);
using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBuffer_t = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
GetKeyState_t oGetAsyncKeyState = nullptr;
GetKeyState_t oGetKeyState = nullptr;
SetCursorPos_t oSetCursorPos = nullptr;
ClipCursor_t oClipCursor = nullptr;
GetRawInputData_t oGetRawInputData = nullptr;
GetRawInputBuffer_t oGetRawInputBuffer = nullptr;

HWND RootWindow() { return gHwnd ? GetAncestor(gHwnd, GA_ROOT) : nullptr; }

// GMod only gets input while DL is in front and GMod is up.
bool InputActive() {
    HWND root = RootWindow();
    if (!root || GetForegroundWindow() != root) return false;
    Header* hdr = shm::Get();
    return hdr && hdr->gmodReady;
}

void CenterCursorOnDL() {
    RECT r;
    HWND h = gHwnd;
    if (!h || !GetClientRect(h, &r)) return;
    POINT c{(r.right - r.left) / 2, (r.bottom - r.top) / 2};
    ClientToScreen(h, &c);
    oClipCursor(nullptr);
    oSetCursorPos(c.x, c.y);
}

bool IsMenu(uint32_t m) { return m == kModeSpawnMenu || m == kModeContextMenu; }

UINT gMenuHoldKey = 0;                  // Q or C while a menu is held open from GMod hands
std::atomic<bool> gDumpNearRequested{false};

void SetMode(uint32_t m) {
    if (m != kModePlay && gPaused) m = kModePlay;  // nothing of GMod's over DL's pause menu
    uint32_t old = gMode.exchange(m);
    if (old == m) return;
    if (Header* h = shm::Get()) h->mode = m;
    if (IsMenu(m)) {
        CenterCursorOnDL();
        gLastCursorX = gLastCursorY = -1;
    }
    if (!IsMenu(m)) gMenuHoldKey = 0;
    if (m == kModeGMod) {
        // First time in GMod hands, log what's right at the camera: DL's own hands
        // and weapon are in there somewhere, and they should be hidden in this mode.
        static bool dumped = false;
        if (!dumped) { dumped = true; gDumpNearRequested = true; }
    }
    gWheel = 0;
    LOGI("mode %u -> %u", old, m);
}

bool OurKey(UINT vk) {
    return vk == cfg.keySpawnMenu || vk == cfg.keyPhysgun || vk == cfg.keyOverlay || vk == cfg.keyDump ||
           vk == cfg.keyDebugBoxes || vk == cfg.keyScreenshot;
}
bool MouseVk(UINT vk) { return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2; }
// Keys a GMod player uses that DL has to give up in GMod hands mode: weapon
// slots, reload, use, undo, and Q/C for the spawn and context menus.
bool GModKey(UINT vk) {
    return (vk >= '0' && vk <= '9') || vk == 'R' || vk == 'E' || vk == 'Z' || vk == 'Q' || vk == 'C';
}

void PushMouseButton(UINT vk, bool down) {
    int b = vk == VK_LBUTTON ? 0 : vk == VK_RBUTTON ? 1 : vk == VK_MBUTTON ? 2 : -1;
    if (b >= 0) shm::Push(down ? kInMouseDown : kInMouseUp, b);
}

void PushTyped(UINT vk) {
    BYTE state[256] = {};
    if (oGetAsyncKeyState(VK_SHIFT) & 0x8000) state[VK_SHIFT] = 0x80;
    if (oGetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 0x01;
    wchar_t ch[4];
    int n = ToUnicode(vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), state, ch, 4, 4);  // 4: keep dead-key state
    for (int i = 0; i < n; ++i)
        if (ch[i] >= 32) shm::Push(kInChar, ch[i]);
}

void ProcessKey(UINT vk, bool down) {
    uint32_t mode = gMode;
    if (vk == cfg.keySpawnMenu) {
        if (down) {
            if (mode != kModeSpawnMenu) {
                if (!IsMenu(mode)) gModeBeforeMenu = mode;
                gMenuHoldKey = 0;
                SetMode(kModeSpawnMenu);
            } else if (!cfg.spawnMenuHold) {
                SetMode(gModeBeforeMenu);
            }
        } else if (cfg.spawnMenuHold && mode == kModeSpawnMenu && !gMenuHoldKey) {
            SetMode(gModeBeforeMenu);
        }
        return;
    }
    if (vk == cfg.keyPhysgun) {
        if (down) {
            bool inGMod = mode == kModeGMod || (IsMenu(mode) && gModeBeforeMenu == kModeGMod);
            SetMode(inGMod ? kModePlay : kModeGMod);
        }
        return;
    }
    if (vk == cfg.keyOverlay) { if (down) overlay::SetVisible(!overlay::Visible()); return; }
    if (vk == cfg.keyDump) { if (down) gDumpRequested = true; return; }
    if (vk == cfg.keyDebugBoxes) {
        if (down) if (Header* h = shm::Get()) { h->debugFlags ^= kDebugProxies; LOGI("actor boxes %s", (h->debugFlags & kDebugProxies) ? "on" : "off"); }
        return;
    }
    if (vk == cfg.keyScreenshot) { if (down) overlay::RequestScreenshot("key", true); return; }

    if (mode == kModeGMod) {
        if (down && (vk == 'Q' || vk == 'C')) {
            // Held like in GMod: open while down, close on release.
            gModeBeforeMenu = kModeGMod;
            SetMode(vk == 'Q' ? kModeSpawnMenu : kModeContextMenu);
            gMenuHoldKey = vk;
            return;
        }
        if (MouseVk(vk)) PushMouseButton(vk, down);
        else if (GModKey(vk)) shm::Push(down ? kInKeyDown : kInKeyUp, int(vk));
        return;
    }
    if (IsMenu(mode)) {
        if (vk == gMenuHoldKey) { if (!down) SetMode(gModeBeforeMenu); return; }
        if (vk == VK_ESCAPE) { if (down) SetMode(gModeBeforeMenu); return; }
        if (MouseVk(vk)) { PushMouseButton(vk, down); return; }
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) return;  // L/R variants are sent instead
        shm::Push(down ? kInKeyDown : kInKeyUp, int(vk));
        if (down) PushTyped(vk);
    }
}

// Once per DL frame.
void PollInput() {
    bool now[256];
    for (int vk = 1; vk < 255; ++vk) now[vk] = (oGetAsyncKeyState(vk) & 0x8000) != 0;
    now[0] = now[255] = false;

    if (!InputActive()) {
        // Alt-tab, a popup, GMod gone: drop back to plain play and remember key
        // state so nothing fires when DL is back in front.
        if (gMode != kModePlay) {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            Header* hdr = shm::Get();
            wchar_t exe[MAX_PATH] = L"?";
            if (HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                DWORD len = MAX_PATH;
                QueryFullProcessImageNameW(ph, 0, exe, &len);
                CloseHandle(ph);
            }
            LOGW("DL lost focus to %ls (pid %lu%s) or GMod not ready (%u): back to play",
                 exe, pid, hdr && pid == hdr->gmPid ? ", Garry's Mod" : "", hdr ? hdr->gmodReady : 0);
            SetMode(kModePlay);
        }
        static int explained = 0;
        for (UINT vk = 1; vk < 255; ++vk) {
            if (now[vk] && !gWasDown[vk] && OurKey(vk) && explained++ < 5) {
                Header* hdr = shm::Get();
                LOGW("key %u ignored: foreground %p, DL window %p (root %p), gmod ready %u",
                     vk, GetForegroundWindow(), gHwnd.load(), RootWindow(), hdr ? hdr->gmodReady : 0);
            }
        }
        memcpy(gWasDown, now, sizeof(now));
        return;
    }

    for (UINT vk = 1; vk < 255; ++vk) {
        if (now[vk] == gWasDown[vk]) continue;
        gWasDown[vk] = now[vk];
        ProcessKey(vk, now[vk]);
    }

    uint32_t mode = gMode;
    if (IsMenu(mode)) {
        // Real cursor position over DL's window -> GMod frame pixels.
        Header* h = shm::Get();
        POINT p;
        RECT rc;
        if (h && h->frame.w && GetCursorPos(&p) && ScreenToClient(gHwnd, &p) && GetClientRect(gHwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
            int x = std::max(0, std::min(int(h->frame.w) - 1, int(int64_t(p.x) * h->frame.w / rc.right)));
            int y = std::max(0, std::min(int(h->frame.h) - 1, int(int64_t(p.y) * h->frame.h / rc.bottom)));
            if (x != gLastCursorX || y != gLastCursorY) {
                gLastCursorX = x;
                gLastCursorY = y;
                shm::Push(kInMouseMove, x, y);
            }
        }
    }
    if (mode != kModePlay) {
        int wheel = gWheel.exchange(0);
        for (; wheel > 0; --wheel) shm::Push(kInWheel, 1);
        for (; wheel < 0; ++wheel) shm::Push(kInWheel, -1);
    }
}

// What DL may still see of one raw input event, given the mode.
void FilterRaw(RAWINPUT* ri) {
    uint32_t mode = gMode;
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        RAWMOUSE& m = ri->data.mouse;
        if (mode != kModePlay && (m.usButtonFlags & RI_MOUSE_WHEEL))
            gWheel += SHORT(m.usButtonData) > 0 ? 1 : -1;
        if (IsMenu(mode)) {
            m.lLastX = m.lLastY = 0;
            m.usButtonFlags = 0;
        } else if (mode == kModeGMod) {
            m.usButtonFlags = 0;  // every button and the wheel are GMod's; DL keeps the look
        }
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        RAWKEYBOARD& k = ri->data.keyboard;
        bool hide = OurKey(k.VKey) || IsMenu(mode) || (mode == kModeGMod && GModKey(k.VKey));
        if (hide && InputActive()) { k.VKey = 0xFF; k.MakeCode = 0; }
    }
}

UINT WINAPI HkGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdrSize) {
    static bool logged = false;
    if (!logged) { logged = true; LOGI("DL reads raw input with GetRawInputData"); }
    UINT r = oGetRawInputData(h, cmd, data, size, hdrSize);
    if (cmd == RID_INPUT && data && r != UINT(-1) && r > 0) FilterRaw(static_cast<RAWINPUT*>(data));
    return r;
}

typedef unsigned __int64 QWORD;  // NEXTRAWINPUTBLOCK uses it; winuser.h doesn't define it

UINT WINAPI HkGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT hdrSize) {
    static bool logged = false;
    if (!logged) { logged = true; LOGI("DL reads raw input with GetRawInputBuffer"); }
    UINT n = oGetRawInputBuffer(data, size, hdrSize);
    if (data && n != UINT(-1)) {
        RAWINPUT* ri = data;
        for (UINT i = 0; i < n; ++i, ri = NEXTRAWINPUTBLOCK(ri)) FilterRaw(ri);
    }
    return n;
}

// For anything DL polls directly. Our own polling uses the originals.
bool HiddenFromDL(int vk) {
    uint32_t mode = gMode;
    if (IsMenu(mode)) return true;
    if (mode == kModeGMod && (MouseVk(UINT(vk)) || GModKey(UINT(vk)))) return true;
    return OurKey(UINT(vk)) && InputActive();
}

SHORT WINAPI HkGetAsyncKeyState(int vk) { return HiddenFromDL(vk) ? 0 : oGetAsyncKeyState(vk); }
SHORT WINAPI HkGetKeyState(int vk) { return HiddenFromDL(vk) ? 0 : oGetKeyState(vk); }

BOOL WINAPI HkSetCursorPos(int x, int y) {
    if (IsMenu(gMode)) return TRUE;  // DL re-centres every frame; not while a menu's up
    return oSetCursorPos(x, y);
}

BOOL WINAPI HkClipCursor(const RECT* r) {
    if (IsMenu(gMode)) return TRUE;
    return oClipCursor(r);
}

// The game's frame tick. ILevel::TimerUpdate turned out to run only around level
// loads, so the per-frame tick is the window thread's message pump: Dying Light
// peeks its messages once a frame on its main thread, between frames (nothing of
// the game's is mid-update there). In-process only; nothing system-wide.
void OnTick();
DWORD gWindowThread = 0;
using PeekMessage_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
PeekMessage_t oPeekMessageW = nullptr, oPeekMessageA = nullptr;

void PumpTick() {
    if (GetCurrentThreadId() != gWindowThread) return;
    static thread_local bool inTick = false;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (inTick || now - last < 3) return;  // once per frame, not per message
    last = now;
    inTick = true;
    OnTick();
    inTick = false;
}
BOOL WINAPI HkPeekMessageW(LPMSG m, HWND h, UINT a, UINT b, UINT r) { PumpTick(); return oPeekMessageW(m, h, a, b, r); }
BOOL WINAPI HkPeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT r) { PumpTick(); return oPeekMessageA(m, h, a, b, r); }

void HookInput(HWND hwnd) {
    gHwnd = hwnd;
    gWindowThread = GetWindowThreadProcessId(hwnd, nullptr);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    MH_CreateHook(GetProcAddress(u, "PeekMessageW"), &HkPeekMessageW, reinterpret_cast<void**>(&oPeekMessageW));
    MH_CreateHook(GetProcAddress(u, "PeekMessageA"), &HkPeekMessageA, reinterpret_cast<void**>(&oPeekMessageA));
    LOGI("threads: Present %lu, window %lu", GetCurrentThreadId(), gWindowThread);
    MH_CreateHook(GetProcAddress(u, "GetAsyncKeyState"), &HkGetAsyncKeyState, reinterpret_cast<void**>(&oGetAsyncKeyState));
    MH_CreateHook(GetProcAddress(u, "GetKeyState"), &HkGetKeyState, reinterpret_cast<void**>(&oGetKeyState));
    MH_CreateHook(GetProcAddress(u, "SetCursorPos"), &HkSetCursorPos, reinterpret_cast<void**>(&oSetCursorPos));
    MH_CreateHook(GetProcAddress(u, "ClipCursor"), &HkClipCursor, reinterpret_cast<void**>(&oClipCursor));
    MH_CreateHook(GetProcAddress(u, "GetRawInputData"), &HkGetRawInputData, reinterpret_cast<void**>(&oGetRawInputData));
    MH_CreateHook(GetProcAddress(u, "GetRawInputBuffer"), &HkGetRawInputBuffer, reinterpret_cast<void**>(&oGetRawInputBuffer));
    MH_EnableHook(MH_ALL_HOOKS);
    for (int vk = 1; vk < 255; ++vk) gWasDown[vk] = (oGetAsyncKeyState(vk) & 0x8000) != 0;
    LOGI("input hooked (window %p, root %p)", hwnd, RootWindow());
}

// ---------- actors ----------

uint32_t gLastDrivenSeq = 0;
int gPinnedLast = 0;  // actors pinned on the last update
HANDLE gFrameEvent = nullptr;  // kFrameEventName: set once a frame for GMod's lockstep
DWORD gLastNpcScan = 0;
std::unordered_set<uint64_t> gLiveNpcs;  // handles seen in the latest scan; only these get touched
void* gPlayer = nullptr;                 // DL's player object (PlayerDI), from the scans

bool WantedClass(const char* cls) {
    for (auto& c : cfg.npcClasses)
        if (strstr(cls, c.c_str())) return true;
    return false;
}

// An AI object (HumanAI) is the brain; what's drawn is a "...AIVis" child
// (ZombieAIVis, HumanAIVis). Positions come from the body when there is one.
void* BodyOf(void* ai) {
    static void* kids[64];
    int n = eng::Children(ai, kids, 64);
    for (int i = 0; i < n; ++i)
        if (strstr(eng::ClassName(kids[i]), "Vis@@")) return kids[i];
    return nullptr;
}

std::unordered_map<uint64_t, void*> gBodies;  // AI handle -> body, from the latest scan
std::unordered_map<uint64_t, DWORD> gBlockedAt;  // when a driven actor last hit the world
struct Released { eng::vec3 pos; DWORD at; };
std::unordered_map<uint64_t, Released> gReleased;  // let go by GMod in the last few seconds

// Every few seconds in GMod hands: where everything is relative to the camera.
void SceneReport(const Camera& cam, void* const* objs, int n) {
    static DWORD last = 0;
    static int reports = 0;
    if (gMode == kModePlay || reports >= 15 || GetTickCount() - last < 5000) return;
    last = GetTickCount();
    ++reports;
    LOGI("scene: camera (%.2f, %.2f, %.2f) fwd (%.2f, %.2f, %.2f) up (%.2f, %.2f, %.2f) left (%.2f, %.2f, %.2f) vfov %.1f aspect %.3f",
         cam.pos.x, cam.pos.y, cam.pos.z, cam.fwd.x, cam.fwd.y, cam.fwd.z, cam.up.x, cam.up.y, cam.up.z,
         cam.left.x, cam.left.y, cam.left.z, cam.fovDeg, cam.aspect);
    for (int i = 0; i < n; ++i) {
        const char* cls = eng::ClassName(objs[i]);
        if (!WantedClass(cls)) continue;
        eng::mtx34 a{}, b{};
        eng::GetWorldXform(objs[i], a);
        void* body = BodyOf(objs[i]);
        bool hasBody = body && eng::GetWorldXform(body, b);
        const eng::mtx34& p = hasBody ? b : a;
        float dx = p.m[0][3] - cam.pos.x, dy = p.m[1][3] + 1.0f - cam.pos.y, dz = p.m[2][3] - cam.pos.z;
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        float off = d > 0.01f ? std::acos(std::max(-1.0f, std::min(1.0f, (dx * cam.fwd.x + dy * cam.fwd.y + dz * cam.fwd.z) / d))) * 57.2958f : 0;
        static void* kids[64];
        int nk = eng::Children(objs[i], kids, 64);
        std::string kidNames;
        for (int k = 0; k < nk && k < 6; ++k) { kidNames += eng::ClassName(kids[k]); kidNames += ' '; }
        LOGI("  %p %s ai (%.2f, %.2f, %.2f) body %s (%.2f, %.2f, %.2f) | %.1fm, %.0f deg off crosshair | children: %s",
             objs[i], cls, a.m[0][3], a.m[1][3], a.m[2][3], hasBody ? eng::ClassName(body) : "none",
             b.m[0][3], b.m[1][3], b.m[2][3], d, off, kidNames.c_str());
    }
}

// Where we last told DL to put each driven actor, to check it stayed there.
struct DriveCheck { uint64_t handle; eng::vec3 want; DWORD at; };
DriveCheck gDriveCheck{};

void SyncNpcs(Header* hdr, const Camera& cam) {
    // Publish nearby actors ~15 times a second.
    DWORD now = GetTickCount();
    if (now - gLastNpcScan >= 66) {
        gLastNpcScan = now;
        static void* objs[1024];
        int n = eng::FindNearby({cam.pos.x, cam.pos.y, cam.pos.z}, cfg.npcRadius, objs, 1024);
        uint32_t w = 0;
        gLiveNpcs.clear();
        gBodies.clear();
        SceneReport(cam, objs, n);
        // Actors GMod is holding or has thrown stay mirrored past the radius (a long
        // throw used to drop them mid-flight). Found again near where GMod put them,
        // which also proves the pointer is still a live object.
        uint32_t nd = std::min(hdr->drivenCount, kMaxNpcs);
        for (uint32_t k = 0; k < nd && n < 1000; ++k) {
            const Driven& d = hdr->driven[k];
            bool seen = false;
            for (int i = 0; i < n && !seen; ++i) seen = reinterpret_cast<uint64_t>(objs[i]) == d.handle;
            if (seen) continue;
            static void* around[64];
            int m = eng::FindNearby({d.pos.x, d.pos.y + 1.0f, d.pos.z}, 4.0f, around, 64);
            for (int i = 0; i < m; ++i)
                if (reinterpret_cast<uint64_t>(around[i]) == d.handle) { objs[n++] = around[i]; break; }
        }
        for (int i = 0; i < n; ++i) {
            const char* cls = eng::ClassName(objs[i]);
            if (!strcmp(cls, ".?AVPlayerDI@@")) { gPlayer = objs[i]; continue; }
            if (w >= kMaxNpcs || !WantedClass(cls)) continue;
            eng::mtx34 m;
            if (!eng::GetWorldXform(objs[i], m)) continue;
            if (void* body = BodyOf(objs[i])) {
                eng::mtx34 bm;
                if (eng::GetWorldXform(body, bm)) { m = bm; gBodies[reinterpret_cast<uint64_t>(objs[i])] = body; }
            }
            Npc& npc = hdr->npcs[w++];
            npc.handle = reinterpret_cast<uint64_t>(objs[i]);
            npc.pos = {m.m[0][3], m.m[1][3], m.m[2][3]};
            npc.yaw = std::atan2(m.m[0][2], m.m[2][2]);
            npc.height = 1.8f;
            npc.flags = kNpcAlive;
            // The pose (standing, crouched, crawling, lying) shapes GMod's stand-in.
            eng::vec3 head, feet;
            if (eng::GetPose(objs[i], head, feet)) {
                npc.head = {head.x, head.y, head.z};
                npc.feet = {feet.x, feet.y, feet.z};
                npc.flags |= kNpcPosed;
            }
            auto blocked = gBlockedAt.find(npc.handle);
            if (blocked != gBlockedAt.end() && now - blocked->second < 300) npc.flags |= kNpcBlocked;
            strncpy_s(npc.cls, cls, _TRUNCATE);
            gLiveNpcs.insert(npc.handle);

            // After a release: does the AI keep it where it was let go, or snap back?
            auto rel = gReleased.find(npc.handle);
            if (rel != gReleased.end()) {
                float dx = npc.pos.x - rel->second.pos.x, dy = npc.pos.y - rel->second.pos.y, dz = npc.pos.z - rel->second.pos.z;
                float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                static int snapLogs = 0;
                if (d > 1.5f && snapLogs++ < 20)
                    LOGW("snap back? %s %llx is %.2fm from where it was released %lums ago, now at (%.2f, %.2f, %.2f)", cls,
                         npc.handle, d, now - rel->second.at, npc.pos.x, npc.pos.y, npc.pos.z);
                if (d > 1.5f || now - rel->second.at > 3000) gReleased.erase(rel);
            }
            // Did the last move stick, or did DL's AI put it back?
            if (gDriveCheck.handle == npc.handle && now - gDriveCheck.at > 100) {
                float dx = npc.pos.x - gDriveCheck.want.x, dy = npc.pos.y - gDriveCheck.want.y, dz = npc.pos.z - gDriveCheck.want.z;
                static int reports = 0;
                if (reports++ < 20)
                    LOGI("drive check: %s %llx is %.2fm from where GMod put it %lums ago",
                         cls, npc.handle, std::sqrt(dx * dx + dy * dy + dz * dz), now - gDriveCheck.at);
                gDriveCheck.handle = 0;
            }
        }
        hdr->npcCount = w;
        MemoryBarrier();
        hdr->npcSeq++;
    }

    // Move whatever GMod is holding or flinging to where GMod says it is. GMod
    // sends that 66 times a second; DL draws ~165. Between updates each actor is
    // carried on along its velocity, so a held zombie glides with the physgun
    // instead of stepping (and trailing) behind it.
    static Driven snap[kMaxNpcs];
    static uint32_t snapN = 0;
    static double snapAt = 0;
    LARGE_INTEGER qn, qf;
    QueryPerformanceCounter(&qn);
    QueryPerformanceFrequency(&qf);
    double nowSec = double(qn.QuadPart) / double(qf.QuadPart);
    uint32_t seq = hdr->drivenSeq;
    if (seq != gLastDrivenSeq) {
        gLastDrivenSeq = seq;
        snapN = std::min(hdr->drivenCount, kMaxNpcs);
        memcpy(snap, hdr->driven, snapN * sizeof(Driven));
        snapAt = nowSec;
    } else if (snapN == 0 && gPinnedLast == 0) {
        return;  // nothing driven, nothing to let go of
    }
    float ahead = float(std::min(std::max(nowSec - snapAt, 0.0), cfg.driveExtrapolate));
    uint32_t n = snapN;
    // Pin everything GMod drives so DL's own movement can't pull it back.
    static void* pinObjs[16];
    static eng::vec3 pinPos[16];
    int pins = 0;
    for (uint32_t i = 0; i < n && pins < 16; ++i) {
        const Driven& d = snap[i];
        if (!gLiveNpcs.count(d.handle)) continue;
        pinObjs[pins] = reinterpret_cast<void*>(d.handle);
        pinPos[pins++] = {d.pos.x + d.vel.x * ahead, d.pos.y + d.vel.y * ahead, d.pos.z + d.vel.z * ahead};
    }
    gPinnedLast = pins;
    // Thrown or held actors don't go through Dying Light's walls: trace at waist
    // height from where the actor is to where GMod wants it; stop short of a hit.
    if (cfg.thrownHitWorld && gPlayer) {
        for (int i = 0; i < pins; ++i) {
            eng::mtx34 cur;
            if (!eng::GetWorldXform(pinObjs[i], cur)) continue;
            eng::vec3 a{cur.m[0][3], cur.m[1][3] + 0.9f, cur.m[2][3]};
            eng::vec3 b{pinPos[i].x, pinPos[i].y + 0.9f, pinPos[i].z};
            float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
            float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len < 0.02f) continue;
            eng::RayHit r;
            if (!eng::Raytrace(gPlayer, a, b, r, pinObjs[i]) || !r.hit || r.dist < 0.05f || r.dist > len - 0.02f) continue;
            float back = std::min(0.3f, r.dist) / len;  // stay a little out of the wall
            pinPos[i] = {r.pos.x - dx * back, r.pos.y - dy * back - 0.9f, r.pos.z - dz * back};
            gBlockedAt[reinterpret_cast<uint64_t>(pinObjs[i])] = GetTickCount();
            static int logs = 0;
            if (logs++ < 20) LOGI("thrown actor %p hit the world after %.2fm of %.2fm (+18 %s)", pinObjs[i], r.dist, len,
                                  eng::ClassName(r.p18));
        }
    }
    eng::SetPins(pinObjs, pinPos, pins);
    // Released since last time: watch them for a few seconds (snap-back check).
    static std::unordered_map<void*, eng::vec3> lastPins;
    std::unordered_map<void*, eng::vec3> nowPins;
    for (int i = 0; i < pins; ++i) nowPins[pinObjs[i]] = pinPos[i];
    for (auto& [obj, p] : lastPins) {
        if (nowPins.count(obj)) continue;
        gReleased[reinterpret_cast<uint64_t>(obj)] = {p, GetTickCount()};
        // The AI kept its own idea of where the actor is and put it back there the
        // moment the pin went (round 8 log: 3-12 m snaps 50 ms after release).
        // Rewrite that idea to where GMod let go.
        eng::vec3 gamePos;
        if (cfg.releaseFix && gLiveNpcs.count(reinterpret_cast<uint64_t>(obj)) && eng::TakeGamePosition(obj, gamePos)) {
            float dx = gamePos.x - p.x, dy = gamePos.y - p.y, dz = gamePos.z - p.z;
            if (dx * dx + dy * dy + dz * dz > 0.25f) eng::MoveGameCopies(obj, gamePos, p);
        }
    }
    lastPins = std::move(nowPins);
    for (uint32_t i = 0; i < n; ++i) {
        const Driven& d = snap[i];
        if (!gLiveNpcs.count(d.handle)) continue;  // gone since the last scan
        void* obj = reinterpret_cast<void*>(d.handle);
        eng::mtx34 m;
        if (!eng::GetWorldXform(obj, m)) continue;
        eng::vec3 p{d.pos.x, d.pos.y, d.pos.z};
        for (int k = 0; k < pins; ++k)
            if (pinObjs[k] == obj) { p = pinPos[k]; break; }
        m.m[0][3] = p.x;
        m.m[1][3] = p.y;
        m.m[2][3] = p.z;
        eng::SetWorldXform(obj, m);
        auto body = gBodies.find(d.handle);
        if (body != gBodies.end()) {
            eng::mtx34 bm;
            if (eng::GetWorldXform(body->second, bm)) {
                bm.m[0][3] = d.pos.x;
                bm.m[1][3] = d.pos.y;
                bm.m[2][3] = d.pos.z;
                eng::SetWorldXform(body->second, bm);
            }
        }
        if (!gDriveCheck.handle) gDriveCheck = {d.handle, {d.pos.x, d.pos.y, d.pos.z}, now};
    }
}

// GMod -> DL probes: trace each segment through DL's world and answer with the hits.
// Actors are traced through (GMod has its own stand-ins for them); the world isn't.
void TraceProbes(Header* hdr) {
    static uint32_t lastSeq = 0;
    Probes& p = hdr->probes;
    uint32_t seq = p.seq;
    if (seq == lastSeq || !gPlayer) return;
    lastSeq = seq;
    MemoryBarrier();
    static Probe in[kMaxProbes];
    uint32_t n = std::min(p.count, kMaxProbes);
    memcpy(in, p.probes, n * sizeof(Probe));
    uint32_t w = 0;
    static int logs = 0;
    for (uint32_t i = 0; i < n; ++i) {
        eng::vec3 a{in[i].from.x, in[i].from.y, in[i].from.z}, b{in[i].to.x, in[i].to.y, in[i].to.z};
        float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 0.001f || len > 500.0f) continue;
        float travelled = 0;
        for (int tries = 0; tries < 3; ++tries) {
            eng::RayHit r;
            if (!eng::Raytrace(gPlayer, a, b, r, gPlayer) || !r.hit) break;
            if (r.p18 && WantedClass(eng::ClassName(r.p18))) {
                // An actor: carry on just past it.
                float step = r.dist + 0.05f;
                a = {a.x + dx / len * step, a.y + dy / len * step, a.z + dz / len * step};
                travelled += step;
                if (travelled >= len) break;
                continue;
            }
            ProbeHit& h = p.results[w++];
            h.id = in[i].id;
            h.dist = travelled + r.dist;
            h.pos = {r.pos.x, r.pos.y, r.pos.z};
            h.normal = {r.floats[0], r.floats[1], r.floats[2]};
            if (logs++ < 15)
                LOGI("probe %u hit the world after %.2fm of %.2fm at (%.2f, %.2f, %.2f) normal (%.2f, %.2f, %.2f)", h.id,
                     h.dist, len, h.pos.x, h.pos.y, h.pos.z, h.normal.x, h.normal.y, h.normal.z);
            break;
        }
    }
    p.resultCount = w;
    MemoryBarrier();
    p.resultSeq = seq;
}

// GMod -> DL events: damage GMod did to an actor's stand-in becomes real damage.
void ApplyEvents(Header* hdr) {
    EventQueue& q = hdr->events;
    while (q.tail != q.head) {
        Event e = q.events[q.tail % kMaxEvents];
        MemoryBarrier();
        q.tail = q.tail + 1;
        static int logged = 0;
        if (!gLiveNpcs.count(e.handle)) {
            if (logged++ < 20) LOGW("event %u for %llx skipped: not in the latest scan", e.type, e.handle);
            continue;
        }
        void* obj = reinterpret_cast<void*>(e.handle);
        static bool described = false;
        if (!described) {
            // Once: what this kind of actor offers, for the next round of work.
            described = true;
            const char* methods[] = {"Kill", "TakeDamageToKill", "DeleteAfterDie", "DisableElementsRagdoll",
                                     "Explode", "StopFighting", "Hide", "Delete"};
            std::string have;
            for (const char* m : methods)
                if (eng::HasScriptMethod(obj, m)) { have += m; have += ' '; }
            LOGI("script methods on %s: %s", eng::ClassName(obj), have.empty() ? "(none of the probed ones)" : have.c_str());
            eng::LogSkeleton(obj);
        }
        if (e.type == kEvKill) {
            // The game's own death (ragdoll, effects): try the "by damage" one first.
            bool ok = eng::CallScriptMethod(obj, "TakeDamageToKill") || eng::CallScriptMethod(obj, "Kill");
            LOGI("kill %s %llx: %s", eng::ClassName(obj), e.handle, ok ? "done" : "FAILED");
            continue;
        }
        if (e.type != kEvDamage) continue;
        float amount = e.amount * cfg.damageScale;
        if (amount <= 0) continue;
        uint32_t dlType = e.dlDamageType ? e.dlDamageType : kDLBullet;
        // Is there Dying Light world between the camera and the hit? (GMod has no
        // walls, so its bullets go through them.) Logged; enforced with WallsBlockShots.
        if (gPlayer && hdr->cam.valid && dlType != kDLBlast) {
            eng::RayHit r;
            eng::vec3 from{hdr->cam.pos.x, hdr->cam.pos.y, hdr->cam.pos.z}, to{e.pos.x, e.pos.y, e.pos.z};
            float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
            float want = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (eng::Raytrace(gPlayer, from, to, r, gPlayer)) {
                // Hitting an actor's own collision (this one or one in front) isn't a wall.
                bool blocked = r.hit && r.dist < want - 0.6f && !(r.p18 && WantedClass(eng::ClassName(r.p18)));
                static int wallLogs = 0;
                if (wallLogs++ < 30)
                    LOGI("  line of fire: target %.2fm, ray hit %d at %.2fm (+18 %s, +28 %s, +30 %s)%s", want, r.hit, r.dist,
                         eng::ClassName(r.p18), eng::ClassName(r.p28), eng::ClassName(r.p30), blocked ? " -> WALL" : "");
                if (blocked && cfg.wallsBlockShots) continue;
            }
        }
        // The attacker is the player's IGSObject (complete object + 0x28), as the game passes it.
        // The attacker is the player's IGSObject (complete object + 0x28), as the
        // game's weapon code passes it (gamedll 0x6e8680). Engine 0x13e409 reads
        // [attacker+0x20]+0x40 as its flags; logged once to check that holds.
        void* attacker = cfg.damageAttacker && gPlayer ? eng::CastTo(gPlayer, ".?AVIGSObject@@") : nullptr;
        static bool told = false;
        if (!told && attacker) {
            told = true;
            void* const* a = static_cast<void* const*>(attacker);
            LOGI("attacker %p: [+0x20] %p, player ctrl %p, [ctrl+8] %p; victim identity %p (handle %p)", attacker,
                 a[4], gPlayer, static_cast<void* const*>(gPlayer)[1], eng::DamageIdentity(obj), obj);
        }
        int bone = -1;
        bool ok = eng::TakeDamage(obj, attacker, amount, {e.pos.x, e.pos.y, e.pos.z}, {e.dir.x, e.dir.y, e.dir.z},
                                  dlType, 135.0f + amount, &bone);
        if (logged++ < 40)
            LOGI("damage %.1f (GMod %.1f) type %u to %s %llx at (%.2f, %.2f, %.2f), bone %d, attacker %p: %s", amount,
                 e.amount, dlType, eng::ClassName(obj), e.handle, e.pos.x, e.pos.y, e.pos.z, bone, attacker,
                 ok ? "applied" : "FAILED");
        if (ok) overlay::RequestScreenshot("damage");
    }
}

// ---------- own hands ----------
// In GMod hands, DL's own arms and held weapon would sit under GMod's viewmodel.

std::vector<void*> gHidden;
std::vector<void*> gHudComponents;  // Dying Light's HUD component objects, from the level walk
bool gHideTried = false;  // once per stretch of GMod hands

void LogTree(void* obj, int depth) {
    static void* kids[256];
    int n = eng::Children(obj, kids, 256);
    for (int i = 0; i < n; ++i) {
        void* k = kids[i];
        LOGI("  %*s%p %s%s", depth * 2, "", k, eng::ClassName(k), eng::CastTo(k, ".?AVIModelObject@@") ? " [model]" : "");
        if (depth < 3) LogTree(k, depth + 1);
    }
}

// Hide a model and what's attached to it (a weapon on the arms' hand bone).
void HideTree(void* model, int depth) {
    // Every pass, not just once: the game may switch them back on (weapon swaps).
    if (eng::SetRendering(model, false) && std::find(gHidden.begin(), gHidden.end(), model) == gHidden.end()) {
        gHidden.push_back(model);
        static int logs = 0;
        if (logs++ < 40) LOGI("  hid %s %p (depth %d)", eng::ClassName(model), model, depth);
    }
    if (depth >= 3) return;
    void* kids[64];
    int n = eng::Children(model, kids, 64);
    for (int i = 0; i < n; ++i)
        if (eng::CastTo(kids[i], ".?AVIModelObject@@")) HideTree(kids[i], depth + 1);
}

// Kyle's first-person arms (PlayerFppVis) and what he holds (WeaponVis, ItemVis,
// ...) are game objects, not children of the player. They're looked for among the
// objects the player object points to (cheap, every second: weapons change) and,
// once per level, in the level's object list. Only within reach of the camera:
// loot and weapons lying in the world stay visible.
void HideOwnModel(bool hide) {
    if (!hide) {
        eng::SetForceHidden(nullptr, 0);  // let the game (and us) show them again
        for (void* o : gHidden) eng::SetRendering(o, true);
        if (!gHidden.empty()) LOGI("own model shown again (%zu objects)", gHidden.size());
        gHidden.clear();
        gHideTried = false;
        return;
    }
    if (!gPlayer || !cfg.hideOwnModel) return;
    static DWORD lastPass = 0;
    DWORD nowTick = GetTickCount();
    if (gHideTried && nowTick - lastPass < 1000) return;
    bool first = !gHideTried;
    gHideTried = true;
    lastPass = nowTick;
    size_t before = gHidden.size();
    if (first) HideTree(gPlayer, 2);  // the third-person body (shadow, mirrors)

    auto wanted = [](const char* cls) {
        for (auto& c : cfg.hideClasses)
            if (strstr(cls, c.c_str())) return true;
        return false;
    };
    auto interesting = [&](const char* cls) {
        return wanted(cls) || strstr(cls, "Vis") || strstr(cls, "Fpp") || strstr(cls, "Weapon") ||
               strstr(cls, "Inventory") || strstr(cls, "Hand") || strstr(cls, "Camera");
    };
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceFrequency(&f);
    auto ms = [&] { QueryPerformanceCounter(&t1); return double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart); };

    // Once per player object: one walk of the level's game objects (only the hidden
    // classes are kept) and a log of every class name there that looks related, so
    // the next round knows the real names if these aren't them.
    static void* discoveredFor = nullptr;
    static std::vector<void*> walkFound;
    bool discover = discoveredFor != gPlayer;
    if (discover) {
        discoveredFor = gPlayer;
        walkFound.clear();
        QueryPerformanceCounter(&t0);
        static void* objs[65536];
        int n = eng::AllGameObjects(eng::CastTo(gPlayer, ".?AVIGSObject@@"), objs, 65536);
        std::unordered_map<std::string, int> related;
        gHudComponents.clear();
        for (int i = 0; i < n; ++i) {
            const char* cls = eng::ClassName(objs[i]);
            if (wanted(cls)) walkFound.push_back(objs[i]);
            if (!strncmp(cls, ".?AVHud", 7) || !strncmp(cls, ".?AVHUD", 7)) gHudComponents.push_back(objs[i]);
            if (interesting(cls) || strstr(cls, "Player")) related[cls]++;
        }
        LOGI("own model: walked %d game objects in %.2f ms; %zu of the hidden classes", n, ms(), walkFound.size());
        std::string line;
        for (auto& [cls, count] : related) line += " " + cls + "x" + std::to_string(count);
        LOGI("own model: related classes in the level:%s", line.c_str());
    }

    // Every pass: what the player object (and the related objects it holds) point
    // to. Cheap, and it follows weapon swaps.
    QueryPerformanceCounter(&t0);
    std::vector<void*> found = walkFound;
    void* complete = eng::CastTo(gPlayer, ".?AVPlayerDI@@");
    static void* l1[1024];
    static uint32_t o1[1024];
    int n1 = complete ? eng::ObjectsReferencedBy(complete, 0x8000, l1, o1, 1024) : 0;
    for (int i = 0; i < n1; ++i) {
        const char* cls = eng::ClassName(l1[i]);
        if (!interesting(cls)) continue;
        if (discover) LOGI("  player +0x%x -> %s %p", o1[i], cls, l1[i]);
        if (wanted(cls)) found.push_back(l1[i]);
        void* l2[256];
        uint32_t o2[256];
        int n2 = eng::ObjectsReferencedBy(l1[i], 0x1000, l2, o2, 256);
        for (int k = 0; k < n2; ++k) {
            const char* c2 = eng::ClassName(l2[k]);
            if (discover && interesting(c2)) LOGI("    %s +0x%x -> %s %p", cls, o2[k], c2, l2[k]);
            if (wanted(c2)) found.push_back(l2[k]);
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    if (discover) LOGI("own model: player references scanned in %.2f ms; %zu objects of the hidden classes", ms(), found.size());
    bool report = discover;

    Camera cam = shm::Get()->cam;  // copied: Present keeps writing it
    auto distance = [&](void* model) {
        eng::mtx34 m;
        if (!model || !eng::GetWorldXform(model, m)) return -1.0f;
        float dx = m.m[0][3] - cam.pos.x, dy = m.m[1][3] - cam.pos.y, dz = m.m[2][3] - cam.pos.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    for (void* gso : found) {
        const char* cls = eng::ClassName(gso);
        // A game object's model, or the object itself if it's an engine object.
        std::vector<void*> models;
        if (void* m = eng::ModelOf(gso)) models.push_back(m);
        else if (eng::CastTo(gso, ".?AVIModelObject@@")) models.push_back(eng::CastTo(gso, ".?AVIControlObject@@"));
        if (models.empty()) {
            // PlayerFppVis keeps a FakeModelObject (+0x40) standing in for its real
            // model (round 9 log): follow every pointer it holds to engine models.
            void* refs[256];
            uint32_t at[256];
            int nr = eng::ObjectsReferencedBy(gso, 0x3000, refs, at, 256);
            for (int i = 0; i < nr; ++i) {
                if (!eng::CastTo(refs[i], ".?AVIModelObject@@")) continue;
                void* ctrl = eng::CastTo(refs[i], ".?AVIControlObject@@");
                if (!ctrl) continue;
                float d = distance(ctrl);
                if (report) LOGI("    %s +0x%x -> model %s %p at %.2fm", cls, at[i], eng::ClassName(refs[i]), ctrl, d);
                models.push_back(ctrl);
            }
        }
        for (void* model : models) {
            float d = distance(model);
            bool hideIt = model && d >= 0 && d < cfg.hideRadius;
            if (report) LOGI("  own model scan: %s %p model %p at %.2fm%s", cls, gso, model, d, hideIt ? " -> hidden" : "");
            if (hideIt) HideTree(model, 0);
        }
    }
    eng::SetForceHidden(gHidden.data(), int(gHidden.size()));
    if (report) {
        LOGI("own model scan: %zu hidden now", gHidden.size());
    } else if (gHidden.size() != before) {
        LOGI("own model hidden (%zu objects)", gHidden.size());
    }
}

// Dying Light's weapon HUD (what Kyle holds, its durability, the weapon wheel,
// the crosshair) describes weapons that are hidden while GMod's hands are out.
// Each HUD component owns some UI elements; only elements owned by exactly one
// of the components to hide are hidden, so shared parents (the HUD root, with
// health and the minimap) are never touched.
std::vector<void*> gHiddenUI;

void HideWeaponHud(bool hide) {
    if (!hide) {
        eng::SetForceHiddenUI(nullptr, 0);  // always: the list can hold elements we never hid
        if (gHiddenUI.empty()) return;
        for (void* e : gHiddenUI) eng::SetUIVisible(e, true);
        LOGI("weapon HUD shown again (%zu elements)", gHiddenUI.size());
        gHiddenUI.clear();
        return;
    }
    if (!cfg.hideWeaponHud || gHudComponents.empty()) return;
    static DWORD lastPass = 0;
    static size_t lastComponents = 0;
    static std::vector<void*> candidates;
    if (GetTickCount() - lastPass < 1000) return;
    lastPass = GetTickCount();
    // Worked out once per set of components (round 12 redid this, and logged it,
    // every frame when there was nothing to hide: 7,000 log lines, a stutter).
    if (lastComponents != gHudComponents.size()) {
        // Which component owns which elements (pointers in its first 4 KB).
        lastComponents = gHudComponents.size();
        candidates.clear();
        std::unordered_map<void*, int> owners;
        std::vector<std::pair<void*, std::vector<void*>>> owned;
        for (void* comp : gHudComponents) {
            void* refs[256];
            int n = eng::ObjectsReferencedBy(comp, 0x1000, refs, nullptr, 256);
            std::vector<void*> elems;
            for (int i = 0; i < n; ++i)
                if (eng::CastTo(refs[i], ".?AVIUIElement@@")) { elems.push_back(refs[i]); owners[refs[i]]++; }
            owned.push_back({comp, std::move(elems)});
        }
        std::string line;
        for (auto& [comp, elems] : owned) {
            const char* cls = eng::ClassName(comp);
            bool target = false;
            for (auto& c : cfg.hideHudClasses) target = target || strstr(cls, c.c_str());
            if (!target) continue;
            int mine = 0;
            for (void* e : elems)
                if (owners[e] == 1) { candidates.push_back(e); ++mine; }
            char b[160];
            sprintf_s(b, " %s(%d of %zu)", cls, mine, elems.size());
            line += b;
        }
        LOGI("weapon HUD: %zu HUD components; hiding elements of:%s", gHudComponents.size(), line.c_str());
    }
    for (void* e : candidates)
        if (std::find(gHiddenUI.begin(), gHiddenUI.end(), e) == gHiddenUI.end() && eng::UIVisible(e) &&
            eng::SetUIVisible(e, false))
            gHiddenUI.push_back(e);
    // All of them stay hidden, including ones the game shows later (the crosshair);
    // only those that were visible get shown again afterwards.
    eng::SetForceHiddenUI(candidates.data(), int(candidates.size()));
}

// ---------- per frame ----------

DWORD gLastFrameLog = 0;

// Everything that changes game objects. Called with gWorldMutex held, from the
// game tick (normally) or from Present (fallback).
std::mutex gWorldMutex;
std::mutex gCamMutex;               // guards gWorldCam only
Camera gWorldCam{};                // latest camera from Present, for the tick
std::atomic<bool> gWorldLive{false};  // in the world, not paused
std::atomic<DWORD> gLastTickAt{0}, gTickThread{0};

void WorldWork(Header* hdr, const Camera& cam) {
    // A new level (loading screen: death, fast travel, quitting to the menu) frees
    // the old one's objects. Forget every pointer into it without touching them.
    static void* lastLevel = nullptr;
    void* level = eng::Level();
    if (level != lastLevel) {
        if (lastLevel) LOGI("level changed (%p -> %p): forgetting player, hidden models and actor state", lastLevel, level);
        lastLevel = level;
        gPlayer = nullptr;
        gHidden.clear();
        eng::SetForceHidden(nullptr, 0);
        gHiddenUI.clear();
        gHudComponents.clear();
        eng::SetForceHiddenUI(nullptr, 0);
        gHideTried = false;
        gReleased.clear();
        gBlockedAt.clear();
        gBodies.clear();
        gLiveNpcs.clear();
        eng::SetPins(nullptr, nullptr, 0);
    }
    // Timed: anything slow here is a stutter in the game (round 8's 0.6 s hitches).
    LARGE_INTEGER q[5], f;
    QueryPerformanceCounter(&q[0]);
    if (cfg.npcSync) SyncNpcs(hdr, cam);
    QueryPerformanceCounter(&q[1]);
    TraceProbes(hdr);
    QueryPerformanceCounter(&q[2]);
    ApplyEvents(hdr);
    QueryPerformanceCounter(&q[3]);
    // DL's own arms, weapon and weapon HUD go while GMod's hands are out.
    HideOwnModel(gMode != kModePlay);
    HideWeaponHud(gMode != kModePlay);
    QueryPerformanceCounter(&q[4]);
    QueryPerformanceFrequency(&f);
    auto ms = [&](int a, int b) { return double(q[b].QuadPart - q[a].QuadPart) * 1000.0 / double(f.QuadPart); };
    static int slowLogs = 0;
    if (ms(0, 4) > 8.0 && slowLogs < 30) {
        ++slowLogs;
        LOGW("slow frame work: %.1f ms (actors %.1f, traces %.1f, damage %.1f, hiding %.1f)", ms(0, 4), ms(0, 1), ms(1, 2),
             ms(2, 3), ms(3, 4));
    }
}

void OnTick() {
    Header* hdr = shm::Get();
    if (!hdr || !cfg.gameThreadWork) return;
    gTickThread = GetCurrentThreadId();
    if (!gWorldLive) return;
    gLastTickAt = GetTickCount() | 1;
    Camera cam;
    {
        std::lock_guard<std::mutex> c(gCamMutex);
        cam = gWorldCam;
    }
    std::lock_guard<std::mutex> g(gWorldMutex);
    WorldWork(hdr, cam);
}

void OnFrame(HWND hwnd, unsigned w, unsigned h) {
    Header* hdr = shm::Get();
    if (!hdr) return;
    if (!gHwnd && hwnd) HookInput(hwnd);
    // Start GMod at DL's main menu so it has finished loading by the time you're in the world.
    if (!gLaunched && w && h) LaunchGMod(w, h);
    WatchGMod();

    hdr->dlHeartbeat++;
    Camera cam{};
    bool haveCam = eng::ReadCamera(cam);
    bool paused = !haveCam || eng::IsPaused();
    if (paused != gPaused) {
        gPaused = paused;
        if (paused) SetMode(kModePlay);
        LOGI(paused ? "paused or no camera: overlay hidden" : "in game: overlay shown");
    }
    overlay::SetSuppressed(paused);
    PollInput();
    if (haveCam) {
        cam.viewW = w;
        cam.viewH = h;
        cam.valid = 1;
        cam.seq = hdr->cam.seq + 1;
        LARGE_INTEGER qpc, freq;
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&freq);
        cam.time = double(qpc.QuadPart) / double(freq.QuadPart);
        hdr->cam = cam;
        if (gFrameEvent) SetEvent(gFrameEvent);  // GMod: go (lockstep)
        {
            // Its own lock, held only for the copy: Present must never wait on the
            // world work, which may itself wait on the render thread (deadlock).
            std::lock_guard<std::mutex> g(gCamMutex);
            gWorldCam = cam;
        }
        gWorldLive = true;
        // Moving actors, damage and traces run on the game's update thread (OnTick)
        // once that hook is seen running; until then (or if it never does) here.
        static bool toldThreads = false;
        DWORD lastTick = gLastTickAt.load();
        bool tickActive = cfg.gameThreadWork && lastTick && int32_t(GetTickCount() - lastTick) < 250;
        if (tickActive && !toldThreads) {
            toldThreads = true;
            LOGI("world work moved to the game thread %lu (Present runs on thread %lu)", gTickThread.load(),
                 GetCurrentThreadId());
        }
        if (!tickActive) {
            std::unique_lock<std::mutex> g(gWorldMutex, std::try_to_lock);
            if (g.owns_lock()) WorldWork(hdr, cam);  // busy = the game thread is on it
        }
        // A few screenshots of what the player sees in GMod hands, for debugging.
        static DWORD lastAuto = 0;
        static int autoShots = 0;
        if (gMode == kModeGMod && autoShots < cfg.autoScreenshots && GetTickCount() - lastAuto > 8000) {
            lastAuto = GetTickCount();
            if (autoShots++ > 0) overlay::RequestScreenshot("gmodhands");  // skip the very first frame
        }
        DWORD now = GetTickCount();
        if (now - gLastFrameLog > 10000) {
            gLastFrameLog = now;
            LOGI("camera (%.2f, %.2f, %.2f) fwd (%.2f, %.2f, %.2f) fov %.2f aspect %.3f | gmod ready %u, frames %u, %u npcs",
                 cam.pos.x, cam.pos.y, cam.pos.z, cam.fwd.x, cam.fwd.y, cam.fwd.z, cam.fovDeg, cam.aspect,
                 hdr->gmodReady, hdr->frame.seq, hdr->npcCount);
            float vDeg, vCm;
            int vBad;
            eng::TakeViewMatrixStats(vDeg, vCm, vBad);
            LOGI("  view matrix vs camera object (last 10 s): up to %.2f deg, %.1f cm apart%s", vDeg, vCm,
                 vBad ? " (and it looked wrong some frames; camera object used then)" : "");
            float cDeg, cCm;
            int cUsed, cFrames;
            eng::TakeCombinedStats(cDeg, cCm, cUsed, cFrames);
            if (int blocked = eng::TakeBlockedShows())
                LOGI("  the game tried to show Kyle's hidden arms/weapons %d times (kept hidden)", blocked);
            LOGI("  renderer's camera vs view matrix: up to %.2f deg, %.1f cm apart; used in %d of %d frames", cDeg, cCm, cUsed,
                 cFrames);
        }
    } else {
        hdr->cam.valid = 0;
        gWorldLive = false;
        if (gFrameEvent) SetEvent(gFrameEvent);  // menus, loading: keep GMod ticking at DL's pace
    }
    if (gDumpRequested.exchange(false)) {
        eng::DumpNearby(30.0f);
        // Raytrace probe straight ahead: which SCollision pointer is the hit object?
        if (haveCam && gPlayer) {
            eng::RayHit r;
            eng::vec3 from{cam.pos.x, cam.pos.y, cam.pos.z};
            eng::vec3 to{cam.pos.x + cam.fwd.x * 100, cam.pos.y + cam.fwd.y * 100, cam.pos.z + cam.fwd.z * 100};
            if (eng::Raytrace(gPlayer, from, to, r, gPlayer))
                LOGI("ray ahead: hit %d at %.2fm (%.2f, %.2f, %.2f) floats %.2f %.2f %.2f / %.2f %.2f %.2f | +18 %p %s | +28 %p %s | +30 %p %s",
                     r.hit, r.dist, r.pos.x, r.pos.y, r.pos.z, r.floats[0], r.floats[1], r.floats[2], r.floats[3],
                     r.floats[4], r.floats[5], r.p18, eng::ClassName(r.p18), r.p28, eng::ClassName(r.p28), r.p30,
                     eng::ClassName(r.p30));
            else
                LOGW("ray ahead: raytrace unavailable");
        }
    }
    if (gDumpNearRequested.exchange(false)) eng::DumpNearby(2.5f);
}

DWORD WINAPI InitThread(LPVOID) {
    LOGI("GModLight %s", GMODLIGHT_VERSION);
    LoadConfig();
    if (MH_Initialize() != MH_OK) { LOGE("MinHook init failed"); return 0; }
    if (!shm::Create()) { LOGE("could not create shared memory (%lu)", GetLastError()); return 0; }
    if (cfg.boxesOnStart) shm::Get()->debugFlags |= kDebugProxies;
    if (!eng::Init()) { LOGE("engine init failed; GModLight disabled"); return 0; }
    eng::SetUseViewMatrix(cfg.useViewMatrix);
    eng::SetUseCombined(cfg.useCombined);
    overlay::SetParallax(cfg.parallax);
    overlay::SetMaxFps(cfg.maxFps);
    if (cfg.lockstep) gFrameEvent = CreateEventW(nullptr, FALSE, FALSE, kFrameEventName);
    LOGI("frame cap %d fps (0 = none); GMod lockstep %s", cfg.maxFps, gFrameEvent ? "on" : "off");
    overlay::Callbacks cb;
    cb.onFrame = OnFrame;
    if (!overlay::Init(cb)) { LOGE("overlay init failed; GModLight disabled"); return 0; }
    LOGI("ready");
    return 0;
}

}  // namespace
}  // namespace gml

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(inst, path, MAX_PATH);
        gml::gDir = path;
        gml::gDir.resize(gml::gDir.find_last_of(L"\\/"));
        char logPath[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, (gml::gDir + L"\\GModLight.log").c_str(), -1, logPath, MAX_PATH, nullptr, nullptr);
        gml::log::Open(logPath);
        LOGI("GModLight 0.1.0 loading");
        // Hooks go in from a thread: the loader lock is held here.
        CloseHandle(CreateThread(nullptr, 0, gml::InitThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
