// config.exe: settings for the T6 Bloodline Rebellion arcade port. Writes t6.ini next to itself; t6.exe reads it.
// Plain Win32 + XInput, no other dependencies.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <xinput.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "../port/t6_settings.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "xinput.lib")

using namespace t6;

static HINSTANCE g_inst;
static HWND g_main, g_tab, g_status;
static HFONT g_font;
static Settings g_cfg;
static std::wstring g_ini;

// ---- page controls --------------------------------------------------------
static std::vector<HWND> g_pages[4];          // Video, Player 1, Player 2, System
static HWND g_res, g_aspect, g_full, g_vsync, g_skin, g_resinfo;
static HWND g_ctrl[2];                        // per-player controller dropdown

struct BindRow { int player; int action; HWND key; HWND pad; };
static std::vector<BindRow> g_rows;           // player 0/1 = P1/P2, 2 = system

// capture state: which row/column is waiting for input
static int g_capRow = -1, g_capCol = -1;      // col 0 key, col 1 pad
static WORD g_padPrev[4];

static Binding& bindingFor(int player, int action)
{
    return player < 2 ? g_cfg.players[player].b[action] : g_cfg.system.b[action];
}

static std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

static void refreshRow(const BindRow& r)
{
    Binding& b = bindingFor(r.player, r.action);
    SetWindowTextW(r.key, widen(keyName(b.key)).c_str());
    std::string pn = padName(b.padIndex, b.padButton);
    if (b.padIndex < 0 && b.padButton) pn = padName(0, b.padButton).substr(7) + "  (no controller)";
    SetWindowTextW(r.pad, widen(pn).c_str());
}

// Fill a controller dropdown: None, then Controller 1-4 marked when connected.
static void fillControllers(int p)
{
    HWND c = g_ctrl[p];
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"None (keyboard only)");
    for (int i = 0; i < 4; i++) {
        XINPUT_STATE st{};
        const bool on = XInputGetState(i, &st) == ERROR_SUCCESS;
        wchar_t buf[64];
        swprintf(buf, 64, L"Controller %d%s", i + 1, on ? L"  (connected)" : L"");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessageW(c, CB_SETCURSEL, g_cfg.controller[p] + 1, 0);
}

static void setStatus(const wchar_t* s) { SetWindowTextW(g_status, s); }

static void updateResInfo()
{
    int w, h;
    resolutionSize(g_cfg.video.resolution, g_cfg.video.aspect, w, h);
    wchar_t buf[128];
    if (g_cfg.video.resolution == Resolution::Original && g_cfg.video.aspect == Aspect::Wide)
        swprintf(buf, 128, L"Render size: %dx%d (the game's own)", w, h);
    else
        swprintf(buf, 128, L"Render size: %dx%d", w, h);
    SetWindowTextW(g_resinfo, buf);
}

// Layout is written in 96-DPI units and scaled to the system DPI.
static int g_dpi = 96;
static int g_startPage = 0;   // --page N (for testing)
constexpr int kClientW = 540, kClientH = 480;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

// x, y, w, h are in 96-DPI units, except negative x/y which mean "already in pixels" (-x).
static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, int page)
{
    x = x < 0 ? -x : S(x); y = y < 0 ? -y : S(y);
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, x, y, S(w), S(h), g_main, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (page >= 0) g_pages[page].push_back(c);
    return c;
}

enum {
    ID_TAB = 100, ID_RES, ID_ASPECT, ID_FULL, ID_VSYNC, ID_SAVE, ID_PLAY, ID_DEFAULTS, ID_CTRL1, ID_CTRL2,
    ID_BIND0 = 1000,   // ID_BIND0 + row*2 + col
};

static void showPage(int p)
{
    for (int i = 0; i < 4; i++)
        for (HWND c : g_pages[i]) ShowWindow(c, i == p ? SW_SHOW : SW_HIDE);
}

static void buildBindPage(int page, int player, const char* const* names, int count, int x0, int y0)
{
    if (player < 2) {   // which controller this player uses
        mk(L"STATIC", L"Controller", SS_LEFT, x0, y0 + 4, 120, 20, -1, page);
        g_ctrl[player] = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST, x0 + 130, y0, 330, 200, ID_CTRL1 + player, page);
        y0 += 40;
    }
    mk(L"STATIC", L"Action", SS_LEFT, x0, y0, 120, 20, -1, page);
    mk(L"STATIC", L"Keyboard", SS_LEFT, x0 + 130, y0, 150, 20, -1, page);
    mk(L"STATIC", L"Controller", SS_LEFT, x0 + 290, y0, 170, 20, -1, page);
    for (int a = 0; a < count; a++) {
        int y = y0 + 26 + a * 30;
        mk(L"STATIC", widen(names[a]).c_str(), SS_LEFT, x0, y + 4, 120, 20, -1, page);
        int row = (int)g_rows.size();
        BindRow r{player, a, nullptr, nullptr};
        r.key = mk(L"BUTTON", L"", BS_PUSHBUTTON, x0 + 130, y, 150, 26, ID_BIND0 + row * 2, page);
        r.pad = mk(L"BUTTON", L"", BS_PUSHBUTTON, x0 + 290, y, 170, 26, ID_BIND0 + row * 2 + 1, page);
        g_rows.push_back(r);
        refreshRow(r);
    }
    mk(L"STATIC", L"Click a box, then press a key or controller button. Right-click a box to clear it.",
       SS_LEFT, x0, y0 + 30 + count * 30, 470, 20, -1, page);
}

static void loadUi()
{
    SendMessageW(g_res, CB_SETCURSEL, (int)g_cfg.video.resolution, 0);
    SendMessageW(g_aspect, CB_SETCURSEL, (int)g_cfg.video.aspect, 0);
    SendMessageW(g_full, BM_SETCHECK, g_cfg.video.fullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_vsync, BM_SETCHECK, g_cfg.video.vsync ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_skin, BM_SETCHECK, g_cfg.video.skinSync ? BST_CHECKED : BST_UNCHECKED, 0);
    for (int p = 0; p < 2; p++) if (g_ctrl[p]) fillControllers(p);
    for (auto& r : g_rows) refreshRow(r);
    updateResInfo();
}

static void buildUi()
{
    const RECT rc{0, 0, kClientW, kClientH};   // 96-DPI units; mk() scales
    g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                            S(8), S(8), S(rc.right - 16), S(rc.bottom - 60), g_main, (HMENU)ID_TAB, g_inst, nullptr);
    SendMessageW(g_tab, WM_SETFONT, (WPARAM)g_font, TRUE);
    const wchar_t* tabs[] = {L"Video", L"Player 1", L"Player 2", L"System"};
    for (int i = 0; i < 4; i++) {
        TCITEMW it{}; it.mask = TCIF_TEXT; it.pszText = (LPWSTR)tabs[i];
        SendMessageW(g_tab, TCM_INSERTITEMW, i, (LPARAM)&it);
    }
    const int x0 = 28, y0 = 48;

    // Video
    mk(L"STATIC", L"Resolution", SS_LEFT, x0, y0 + 4, 120, 20, -1, 0);
    g_res = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, x0 + 130, y0, 200, 200, ID_RES, 0);
    for (const char* n : {"Original", "720p", "1080p", "1440p"})
        SendMessageW(g_res, CB_ADDSTRING, 0, (LPARAM)widen(n).c_str());
    mk(L"STATIC", L"Aspect ratio", SS_LEFT, x0, y0 + 40, 120, 20, -1, 0);
    g_aspect = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST, x0 + 130, y0 + 36, 200, 200, ID_ASPECT, 0);
    SendMessageW(g_aspect, CB_ADDSTRING, 0, (LPARAM)L"16:9 (widescreen)");
    SendMessageW(g_aspect, CB_ADDSTRING, 0, (LPARAM)L"4:3");
    g_resinfo = mk(L"STATIC", L"", SS_LEFT, x0 + 130, y0 + 70, 300, 20, -1, 0);
    g_full = mk(L"BUTTON", L"Fullscreen", BS_AUTOCHECKBOX, x0, y0 + 104, 200, 24, ID_FULL, 0);
    g_vsync = mk(L"BUTTON", L"VSync", BS_AUTOCHECKBOX, x0, y0 + 134, 200, 24, ID_VSYNC, 0);
    g_skin = mk(L"BUTTON", L"Accurate character models (fixes torn/stretched characters; fights may run below 60 fps)",
                BS_AUTOCHECKBOX | BS_MULTILINE, x0, y0 + 164, 460, 40, ID_VSYNC + 50, 0);
    mk(L"STATIC", L"4:3 renders the 3D in true 4:3 (taller view, nothing cropped); the HUD is drawn slightly narrower.",
       SS_LEFT, x0, y0 + 220, 470, 36, -1, 0);

    buildBindPage(1, 0, kPlayerActionNames, kPlayerActionCount, x0, y0);
    buildBindPage(2, 1, kPlayerActionNames, kPlayerActionCount, x0, y0);
    buildBindPage(3, 2, kSystemActionNames, kSystemActionCount, x0, y0);

    int by = rc.bottom - 44;
    mk(L"BUTTON", L"Defaults", BS_PUSHBUTTON, 8, by, 90, 32, ID_DEFAULTS, -1);
    mk(L"BUTTON", L"Save", BS_PUSHBUTTON, rc.right - 8 - 200 - 8, by, 100, 32, ID_SAVE, -1);
    mk(L"BUTTON", L"Save && Play", BS_DEFPUSHBUTTON, rc.right - 8 - 100, by, 100, 32, ID_PLAY, -1);
    g_status = mk(L"STATIC", L"", SS_LEFT, 108, by + 8, rc.right - 330, 20, -1, -1);
    for (HWND c : {GetDlgItem(g_main, ID_DEFAULTS), GetDlgItem(g_main, ID_SAVE), GetDlgItem(g_main, ID_PLAY), g_status})
        ShowWindow(c, SW_SHOW);
    loadUi();
    SendMessageW(g_tab, TCM_SETCURSEL, g_startPage, 0);
    showPage(g_startPage);
}

static void readUi()
{
    g_cfg.video.resolution = (Resolution)SendMessageW(g_res, CB_GETCURSEL, 0, 0);
    g_cfg.video.aspect = (Aspect)SendMessageW(g_aspect, CB_GETCURSEL, 0, 0);
    g_cfg.video.fullscreen = SendMessageW(g_full, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_cfg.video.vsync = SendMessageW(g_vsync, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_cfg.video.skinSync = SendMessageW(g_skin, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static bool save()
{
    readUi();
    if (!saveSettings(g_ini.c_str(), g_cfg)) { setStatus(L"Could not write t6.ini"); return false; }
    setStatus(L"Saved.");
    return true;
}

static void endCapture()
{
    if (g_capRow >= 0) refreshRow(g_rows[g_capRow]);
    g_capRow = g_capCol = -1;
    KillTimer(g_main, 1);
}

static void startCapture(int row, int col)
{
    endCapture();
    g_capRow = row; g_capCol = col;
    SetWindowTextW(col == 0 ? g_rows[row].key : g_rows[row].pad,
                   col == 0 ? L"press a key... (Esc cancels)" : L"press a button...");
    for (int i = 0; i < 4; i++) {
        XINPUT_STATE st{};
        g_padPrev[i] = XInputGetState(i, &st) == ERROR_SUCCESS ? st.Gamepad.wButtons : 0;
    }
    SetTimer(g_main, 1, 16, nullptr);
    SetFocus(g_main);
}

static void pollPadCapture()
{
    for (int i = 0; i < 4; i++) {
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
        WORD btn = st.Gamepad.wButtons;
        // triggers count as buttons too
        if (st.Gamepad.bLeftTrigger > 128) btn |= kPadLeftTrigger;
        if (st.Gamepad.bRightTrigger > 128) btn |= kPadRightTrigger;
        WORD pressed = btn & ~g_padPrev[i];
        g_padPrev[i] = btn;
        if (!pressed) continue;
        for (int bit = 0; bit < 16; bit++) {
            if (pressed & (1u << bit)) {
                const int pl = g_rows[g_capRow].player;
                Binding& b = bindingFor(pl, g_rows[g_capRow].action);
                b.padIndex = i; b.padButton = (uint16_t)(1u << bit);
                if (pl < 2) {   // the pad pressed becomes this player's controller
                    g_cfg.controller[pl] = i;
                    applyController(g_cfg, pl);
                    fillControllers(pl);
                    for (auto& r : g_rows) if (r.player == pl) refreshRow(r);
                }
                endCapture();
                return;
            }
        }
    }
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        g_main = h;
        buildUi();
        return 0;
    case WM_NOTIFY:
        if (((NMHDR*)l)->idFrom == ID_TAB && ((NMHDR*)l)->code == TCN_SELCHANGE) {
            endCapture();
            showPage((int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0));
        }
        return 0;
    case WM_TIMER:
        if (w == 1 && g_capRow >= 0 && g_capCol == 1) pollPadCapture();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id >= ID_BIND0) {
            int row = (id - ID_BIND0) / 2, col = (id - ID_BIND0) % 2;
            if (row < (int)g_rows.size()) startCapture(row, col);
            return 0;
        }
        if ((id == ID_RES || id == ID_ASPECT) && HIWORD(w) == CBN_SELCHANGE) { readUi(); updateResInfo(); }
        if ((id == ID_CTRL1 || id == ID_CTRL2) && HIWORD(w) == CBN_SELCHANGE) {
            const int pl = id - ID_CTRL1;
            g_cfg.controller[pl] = (int)SendMessageW(g_ctrl[pl], CB_GETCURSEL, 0, 0) - 1;
            applyController(g_cfg, pl);
            for (auto& r : g_rows) if (r.player == pl) refreshRow(r);
        }
        if ((id == ID_CTRL1 || id == ID_CTRL2) && HIWORD(w) == CBN_DROPDOWN) {   // refresh "connected"
            const int pl = id - ID_CTRL1;
            fillControllers(pl);
        }
        if (id == ID_DEFAULTS) { std::string gp = g_cfg.gamePath; g_cfg = defaultSettings(); g_cfg.gamePath = gp; loadUi(); setStatus(L"Defaults loaded (not saved yet)."); }
        if (id == ID_SAVE) save();
        if (id == ID_PLAY && save()) {
            wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring dir(exe); dir = dir.substr(0, dir.find_last_of(L"\\/"));
            std::wstring game = dir + L"\\t6.exe";
            if ((INT_PTR)ShellExecuteW(h, L"open", game.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL) > 32)
                PostQuitMessage(0);
            else
                setStatus(L"Could not start t6.exe (is it next to config.exe?)");
        }
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (g_capRow >= 0 && g_capCol == 0) {
            if (w != VK_ESCAPE) bindingFor(g_rows[g_capRow].player, g_rows[g_capRow].action).key = (uint16_t)w;
            endCapture();
            return 0;
        }
        break;
    case WM_CONTEXTMENU: {
        HWND c = (HWND)w;
        for (size_t i = 0; i < g_rows.size(); i++) {
            Binding& b = bindingFor(g_rows[i].player, g_rows[i].action);
            if (c == g_rows[i].key) { b.key = 0; refreshRow(g_rows[i]); }
            if (c == g_rows[i].pad) { b.padIndex = -1; b.padButton = 0; refreshRow(g_rows[i]); }
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Buttons swallow WM_KEYDOWN; route keys to the main window while capturing.
static bool preTranslate(MSG& msg)
{
    if (g_capRow >= 0 && g_capCol == 0 && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) {
        wndProc(g_main, msg.message, msg.wParam, msg.lParam);
        return true;
    }
    return false;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int show)
{
    g_inst = inst;
    if (const wchar_t* p = wcsstr(cmd, L"--page ")) g_startPage = std::max(0, std::min(3, _wtoi(p + 7)));
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    g_dpi = (int)GetDpiForSystem();

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_ini = exe; g_ini = g_ini.substr(0, g_ini.find_last_of(L"\\/")) + L"\\t6.ini";
    g_cfg = loadSettings(g_ini.c_str());

    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"T6Config";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    RegisterClassExW(&wc);
    RECT r{0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Tekken 6 Bloodline Rebellion (arcade) - Settings",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, inst, nullptr);
    ShowWindow(h, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (preTranslate(msg)) continue;
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}
