// <game>.ini: shared by config.exe (writes it) and <game>.exe (reads it). Header-only, Win32.
//
// [Video]
// Resolution=1080p        Original | 720p | 1080p | 1440p
// Aspect=16:9             16:9 | 4:3
// Fullscreen=1
// VSync=1
// [Player1] / [Player2] / [System]
// <Action>=key:0x57 pad:0:0x1000     (key = Win32 virtual-key, pad = XInput index:button mask; either may be absent)
#pragma once
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace t6 {

enum class Resolution : int { Original = 0, R720 = 1, R1080 = 2, R1440 = 3 };
enum class Aspect : int { Wide = 0, Narrow = 1 };   // 16:9, 4:3

// The arcade game renders 1280x720 (TeknoParrot's profile: "default resolution of 1280x720").
constexpr int kNativeWidth = 1280, kNativeHeight = 720;

inline void resolutionSize(Resolution r, Aspect a, int& w, int& h)
{
    switch (r) {
    case Resolution::R1080: h = 1080; break;
    case Resolution::R1440: h = 1440; break;
    default: h = 720; break;                       // Original and 720p are both 720 lines
    }
    w = a == Aspect::Narrow ? h * 4 / 3 : h * 16 / 9;
}

// Player inputs of a fighting-game cabinet (one stick + 4 buttons + start per side).
enum PlayerAction { Up, Down, Left, Right, LP, RP, LK, RK, Start, kPlayerActionCount };
inline const char* const kPlayerActionNames[kPlayerActionCount] = {
    "Up", "Down", "Left", "Right", "1  Left Punch", "2  Right Punch", "3  Left Kick", "4  Right Kick", "Start"};
inline const char* const kPlayerActionKeys[kPlayerActionCount] = {
    "Up", "Down", "Left", "Right", "LP", "RP", "LK", "RK", "Start"};

enum SystemAction { Test, Service, Coin1, Coin2, kSystemActionCount };
inline const char* const kSystemActionNames[kSystemActionCount] = {"Test", "Service", "Coin (P1)", "Coin (P2)"};
inline const char* const kSystemActionKeys[kSystemActionCount] = {"Test", "Service", "Coin1", "Coin2"};

// Trigger pseudo-buttons in the two XInput wButtons bits nothing uses (0x0400, 0x0800).
constexpr uint16_t kPadLeftTrigger = 0x0400, kPadRightTrigger = 0x0800;

struct Binding {
    uint16_t key = 0;        // virtual-key, 0 = none
    int padIndex = -1;       // XInput user index, -1 = none
    uint16_t padButton = 0;  // XINPUT_GAMEPAD_* mask (or a trigger pseudo-button)
};
struct PlayerBinds { Binding b[kPlayerActionCount]; };
struct SystemBinds { Binding b[kSystemActionCount]; };

struct VideoSettings {
    Resolution resolution = Resolution::Original;
    Aspect aspect = Aspect::Wide;
    bool fullscreen = false;
    bool vsync = true;
    bool skinSync = true;   // draw characters only once the SPU has written them (no torn meshes)
};

struct Settings {
    VideoSettings video;
    PlayerBinds players[2];
    SystemBinds system;
    int controller[2] = {0, 1};   // XInput pad per player (0..3), -1 = none (keyboard only)
    std::string gamePath;   // [Paths] Game=, empty = the "game" folder next to the exe
};

// Point every pad binding of a player at that player's controller (button kept).
inline void applyController(Settings& s, int p)
{
    for (auto& b : s.players[p].b)
        if (b.padButton) b.padIndex = s.controller[p];
    Binding& coin = s.system.b[p ? Coin2 : Coin1];   // each side's coin follows its pad
    if (coin.padButton) coin.padIndex = s.controller[p];
}

inline Binding bind(uint16_t key, int pad, uint16_t btn) { Binding b; b.key = key; b.padIndex = pad; b.padButton = btn; return b; }

inline Settings defaultSettings()
{
    Settings s;
    // P1: WASD + U I J K (arcade layout 1 2 / 3 4), Enter; first controller
    // P2: arrows + numpad 4 5 1 2, numpad +; second controller
    // Face buttons follow the Tekken pad layout: X=1, Y=2, A=3, B=4.
    const uint16_t keys[2][kPlayerActionCount] = {
        {'W', 'S', 'A', 'D', 'U', 'I', 'J', 'K', VK_RETURN},
        {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD1, VK_NUMPAD2, VK_ADD}};
    const uint16_t pad[kPlayerActionCount] = {
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_START};
    for (int p = 0; p < 2; p++)
        for (int a = 0; a < kPlayerActionCount; a++)
            s.players[p].b[a] = bind(keys[p][a], p, pad[a]);
    s.system.b[Test] = bind(VK_F2, -1, 0);
    s.system.b[Service] = bind(VK_F3, -1, 0);
    s.system.b[Coin1] = bind('5', 0, XINPUT_GAMEPAD_BACK);
    s.system.b[Coin2] = bind('6', 1, XINPUT_GAMEPAD_BACK);
    return s;
}

// ---- names -----------------------------------------------------------------
inline std::string keyName(uint16_t vk)
{
    if (!vk) return "-";
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    switch (vk) {   // extended keys need the extended bit for a correct name
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
    case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
    case VK_RCONTROL: case VK_RMENU:
        sc |= 0x100; break;
    }
    char buf[64] = {};
    if (GetKeyNameTextA((LONG)(sc << 16), buf, sizeof buf) > 0) return buf;
    snprintf(buf, sizeof buf, "Key 0x%02X", vk);
    return buf;
}

inline std::string padName(int idx, uint16_t btn)
{
    if (idx < 0 || !btn) return "-";
    static const struct { uint16_t m; const char* n; } names[] = {
        {XINPUT_GAMEPAD_DPAD_UP, "D-pad Up"}, {XINPUT_GAMEPAD_DPAD_DOWN, "D-pad Down"},
        {XINPUT_GAMEPAD_DPAD_LEFT, "D-pad Left"}, {XINPUT_GAMEPAD_DPAD_RIGHT, "D-pad Right"},
        {XINPUT_GAMEPAD_START, "Start"}, {XINPUT_GAMEPAD_BACK, "Back"},
        {XINPUT_GAMEPAD_LEFT_THUMB, "L3"}, {XINPUT_GAMEPAD_RIGHT_THUMB, "R3"},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, "LB"}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB"},
        {kPadLeftTrigger, "LT"}, {kPadRightTrigger, "RT"},
        {XINPUT_GAMEPAD_A, "A"}, {XINPUT_GAMEPAD_B, "B"}, {XINPUT_GAMEPAD_X, "X"}, {XINPUT_GAMEPAD_Y, "Y"}};
    for (auto& n : names)
        if (n.m == btn) { char b[48]; snprintf(b, sizeof b, "Pad %d  %s", idx + 1, n.n); return b; }
    char b[48]; snprintf(b, sizeof b, "Pad %d  0x%04X", idx + 1, btn); return b;
}

// ---- ini -------------------------------------------------------------------
inline const char* resolutionName(Resolution r)
{
    switch (r) { case Resolution::R720: return "720p"; case Resolution::R1080: return "1080p";
                 case Resolution::R1440: return "1440p"; default: return "Original"; }
}

inline std::string bindingText(const Binding& b)
{
    char buf[64]; buf[0] = 0;
    std::string s;
    if (b.key) { snprintf(buf, sizeof buf, "key:0x%02X", b.key); s += buf; }
    if (b.padButton) {
        snprintf(buf, sizeof buf, "%spad:%d:0x%04X", s.empty() ? "" : " ", b.padIndex, b.padButton);
        s += buf;
    }
    return s;
}

inline Binding parseBinding(const char* t)
{
    Binding b;
    if (const char* k = strstr(t, "key:")) b.key = (uint16_t)strtoul(k + 4, nullptr, 0);
    if (const char* p = strstr(t, "pad:")) {
        char* end = nullptr;
        b.padIndex = (int)strtol(p + 4, &end, 10);
        if (end && *end == ':') b.padButton = (uint16_t)strtoul(end + 1, nullptr, 0);
        if (b.padIndex > 3 || !b.padButton) { b.padIndex = -1; b.padButton = 0; }
        if (b.padIndex < 0) b.padIndex = -1;   // button kept: the player has no controller
    }
    return b;
}

inline std::string iniNarrow(const wchar_t* w) { char b[MAX_PATH * 3]; WideCharToMultiByte(CP_ACP, 0, w, -1, b, sizeof b, nullptr, nullptr); return b; }
inline std::string iniNarrow(const char* s) { return s; }

template <class P>
inline Settings loadSettings(P path_)
{
    std::string path = iniNarrow(path_);
    Settings s = defaultSettings();
    char v[128];
    auto get = [&](const char* sec, const char* key) -> const char* {
        GetPrivateProfileStringA(sec, key, "\x01", v, sizeof v, path.c_str());
        return v[0] == 1 ? nullptr : v;
    };
    if (const char* r = get("Video", "Resolution")) {
        if (!_stricmp(r, "720p")) s.video.resolution = Resolution::R720;
        else if (!_stricmp(r, "1080p")) s.video.resolution = Resolution::R1080;
        else if (!_stricmp(r, "1440p")) s.video.resolution = Resolution::R1440;
        else s.video.resolution = Resolution::Original;
    }
    if (const char* a = get("Video", "Aspect")) s.video.aspect = strstr(a, "4:3") ? Aspect::Narrow : Aspect::Wide;
    if (const char* f = get("Video", "Fullscreen")) s.video.fullscreen = atoi(f) != 0;
    if (const char* f = get("Video", "VSync")) s.video.vsync = atoi(f) != 0;
    if (const char* f = get("Video", "SkinSync")) s.video.skinSync = atoi(f) != 0;
    for (int p = 0; p < 2; p++)
        for (int a = 0; a < kPlayerActionCount; a++)
            if (const char* t = get(p ? "Player2" : "Player1", kPlayerActionKeys[a])) s.players[p].b[a] = parseBinding(t);
    for (int a = 0; a < kSystemActionCount; a++)
        if (const char* t = get("System", kSystemActionKeys[a])) s.system.b[a] = parseBinding(t);
    for (int p = 0; p < 2; p++) {
        if (const char* c = get(p ? "Player2" : "Player1", "Controller"))
            s.controller[p] = (c[0] >= '1' && c[0] <= '4') ? c[0] - '1' : -1;
        applyController(s, p);
    }
    char g[MAX_PATH] = {};
    GetPrivateProfileStringA("Paths", "Game", "", g, sizeof g, path.c_str());
    s.gamePath = g;
    return s;
}

template <class P>
inline bool saveSettings(P path_, const Settings& s)
{
    std::string path = iniNarrow(path_);
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    fprintf(f, "; Settings. Edit with config.exe.\n\n[Video]\n");
    fprintf(f, "SkinSync=%d\n", s.video.skinSync ? 1 : 0);
    fprintf(f, "Resolution=%s\nAspect=%s\nFullscreen=%d\nVSync=%d\n",
            resolutionName(s.video.resolution), s.video.aspect == Aspect::Narrow ? "4:3" : "16:9",
            s.video.fullscreen ? 1 : 0, s.video.vsync ? 1 : 0);
    for (int p = 0; p < 2; p++) {
        fprintf(f, "\n[Player%d]\n", p + 1);
        if (s.controller[p] >= 0) fprintf(f, "Controller=%d\n", s.controller[p] + 1);
        else fprintf(f, "Controller=None\n");
        for (int a = 0; a < kPlayerActionCount; a++)
            fprintf(f, "%s=%s\n", kPlayerActionKeys[a], bindingText(s.players[p].b[a]).c_str());
    }
    fprintf(f, "\n[System]\n");
    for (int a = 0; a < kSystemActionCount; a++)
        fprintf(f, "%s=%s\n", kSystemActionKeys[a], bindingText(s.system.b[a]).c_str());
    fprintf(f, "\n[Paths]\n; Folder with the extracted game (it holds dev_hdd0). Empty = the \"game\" folder next to the game exe.\nGame=%s\n",
            s.gamePath.c_str());
    fclose(f);
    return true;
}

// Is a binding held right now? pads[] = XInput state per user index (nullptr if not connected).
inline bool bindingHeld(const Binding& b, const XINPUT_STATE* const pads[4], bool keyboardFocus)
{
    if (b.key && keyboardFocus && (GetAsyncKeyState(b.key) & 0x8000)) return true;
    if (b.padIndex >= 0 && b.padIndex < 4 && b.padButton && pads[b.padIndex]) {
        const XINPUT_GAMEPAD& g = pads[b.padIndex]->Gamepad;
        if (b.padButton == kPadLeftTrigger) return g.bLeftTrigger > 128;
        if (b.padButton == kPadRightTrigger) return g.bRightTrigger > 128;
        if (g.wButtons & b.padButton) return true;
        // the left stick also drives the d-pad directions
        const int dz = 16000;
        if (b.padButton == XINPUT_GAMEPAD_DPAD_UP && g.sThumbLY > dz) return true;
        if (b.padButton == XINPUT_GAMEPAD_DPAD_DOWN && g.sThumbLY < -dz) return true;
        if (b.padButton == XINPUT_GAMEPAD_DPAD_LEFT && g.sThumbLX < -dz) return true;
        if (b.padButton == XINPUT_GAMEPAD_DPAD_RIGHT && g.sThumbLX > dz) return true;
    }
    return false;
}

}  // namespace t6
