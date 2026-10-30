// T6 arcade I/O: cellUsbd + an emulated Namco USIO board ("PS3A-USJ", VID 0x0B9A PID 0x0910).
//
// The System 357 build talks to its I/O board through libusbd: it registers a logical device
// driver (LDD), waits for the board to be probed and attached, then polls it with bulk transfers.
// The board protocol follows RPCS3's usb_device_usio (rpcs3/Emu/Io/usio.cpp):
//   EP 0x01 OUT: 6-byte command {op|channel, ~reg_hi&0xF0, reg_lo/hi, len_lo/hi}, then data for writes
//   EP 0x82 IN : the response of the last read
//   channel 0  : registers (0x1000 = Tekken inputs, 0x1800 = firmware id, ...)
//   channel 2+ : backup SRAM pages (16 x 64 KB), persisted to usio_backup.bin
//
// Inputs come from t6.ini (written by config.exe): keyboard + XInput.

#include "ppu_recomp.h"
#include "t6_settings.h"

#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
void t6_movie_tick(void);
void t6_movie_init(void);
void t6_prof_start(void);
void t6_clock_init(void);
extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
int ppu_opd_resolve(uint32_t opd, uint32_t* code, uint32_t* toc);
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
}

namespace {

// ---- guest memory (big-endian) ---------------------------------------------------------
uint8_t* G(uint32_t ea) { return vm_base + ea; }
uint32_t rd32(uint32_t ea) { uint32_t v; memcpy(&v, G(ea), 4); return _byteswap_ulong(v); }
void wr32(uint32_t ea, uint32_t v) { v = _byteswap_ulong(v); memcpy(G(ea), &v, 4); }

void log(const char* fmt, ...)
{
    static int on = -1;
    if (on < 0) on = getenv("T6_USB_LOG") ? 1 : 0;
    if (!on) return;
    va_list ap; va_start(ap, fmt); fprintf(stderr, "[t6-usb] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}

// ---- calling guest code from the USB thread -------------------------------------------
// A private guest stack: ppu_guest_call's scratch stack is shared by every host thread.
constexpr uint32_t kCbStackTop = 0x6F7F0000u;   // guest scratch outside every PS3 region (not RSX VRAM)
constexpr uint32_t kTlsTp = 0x10F07000u;        // PPU_TLS_TP (main thread TLS pointer)

const func_entry* findFunc(uint32_t code)
{
    static std::vector<const func_entry*> sorted;
    static std::once_flag once;
    std::call_once(once, [] {
        sorted.reserve((size_t)function_table_count);
        for (uint64_t i = 0; i < function_table_count; i++) sorted.push_back(&function_table[i]);
        std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a->addr < b->addr; });
    });
    auto it = std::lower_bound(sorted.begin(), sorted.end(), (uint64_t)code,
                               [](const func_entry* e, uint64_t a) { return e->addr < a; });
    return it != sorted.end() && (*it)->addr == code ? *it : nullptr;
}

uint64_t guestCall(uint32_t opd, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0)
{
    uint32_t code = 0, toc = 0;
    if (!opd || ppu_opd_resolve(opd, &code, &toc) != 0 || !code) { code = rd32(opd); toc = rd32(opd + 4); }
    const func_entry* f = findFunc(code);
    if (!f) { fprintf(stderr, "[t6-usb] callback OPD 0x%08X -> code 0x%08X not lifted\n", opd, code); return (uint64_t)-1; }
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[1] = kCbStackTop - 0x100;
    ctx.gpr[2] = toc;
    ctx.gpr[3] = a0; ctx.gpr[4] = a1; ctx.gpr[5] = a2; ctx.gpr[6] = a3;
    ctx.gpr[13] = kTlsTp;
    ctx.cia = code;
    ctx.thread_id = 0x7FFE0001ull;     // a host thread, outside the guest thread table
    ppu_context* saved = g_active_ctx;
    g_active_ctx = &ctx;
    f->func(&ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&ctx); }
    g_active_ctx = saved;
    return ctx.gpr[3];
}

// ---- the emulated devices ------------------------------------------------------------
// 1: the USIO I/O board.  2: the USB key (a USB stick; the game identifies it by its serial).
struct Device {
    int id;
    uint32_t descBase, descEnd;
    uint16_t configTotal;
    std::vector<std::string> strings;   // string descriptors 1..n
    uint32_t privateData;
};
constexpr int kUsio = 1, kKey = 2;
constexpr uint32_t kDescBase = 0x6F800000u;     // guest copy of the static descriptors
Device g_devs[2];

Device* findDev(int id)
{
    for (auto& d : g_devs) if (d.id == id) return &d;
    return nullptr;
}

void putDesc(Device& d, uint32_t at, const uint8_t* p, size_t n, uint16_t total)
{
    memcpy(G(at), p, n);
    d.descBase = at; d.descEnd = at + (uint32_t)n; d.configTotal = total;
}

void buildDevices()
{
    static const uint8_t usio[] = {
        18, 0x01, 0x10, 0x01, 0xFF, 0x00, 0xFF, 0x08, 0x9A, 0x0B, 0x10, 0x09, 0x10, 0x09, 0x01, 0x02, 0x00, 0x01,
        9, 0x02, 39, 0, 0x01, 0x01, 0x00, 0xC0, 0x32,                // config, wTotalLength 39
        9, 0x04, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,           // interface, 3 endpoints
        7, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00,                       // EP 0x01 bulk OUT
        7, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,                       // EP 0x82 bulk IN
        7, 0x05, 0x83, 0x03, 0x08, 0x00, 16,                         // EP 0x83 interrupt IN
    };
    static const uint8_t key[] = {
        18, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x93, 0x06, 0x26, 0x00, 0x26, 0x00, 0x01, 0x02, 0x03, 0x01,
        9, 0x02, 0x20, 0, 0x01, 0x01, 0x00, 0x80, 0x32,              // config, wTotalLength 32
        9, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,           // mass storage, SCSI, bulk-only
        7, 0x05, 0x81, 0x02, 0x00, 0x02, 0xFF,
        7, 0x05, 0x02, 0x02, 0x00, 0x02, 0xFF,
    };
    g_devs[0] = {kUsio, 0, 0, 0, {}, 0};
    g_devs[1] = {kKey, 0, 0, 0, {"SMI Corporation", "USB DISK", "76C0D0000000"}, 0};
    putDesc(g_devs[0], kDescBase, usio, sizeof usio, 39);
    putDesc(g_devs[1], kDescBase + 0x100, key, sizeof key, 0x20);
}

struct Pipe { bool open = false; int dev = 0; uint8_t endpoint = 0; };
Pipe g_pipes[32];

struct Ldd { uint32_t name = 0, probe = 0, attach = 0, detach = 0; };
Ldd g_ldd;

// A guest callback to run on the worker thread: cb(a0, a1, a2, a3).
struct Completion { uint32_t cb; uint64_t a0, a1, a2, a3; };
std::mutex g_mx;
std::deque<Completion> g_done;
std::atomic<bool> g_attachPending{false};
std::atomic<bool> g_threadStarted{false};

// ---- USIO ---------------------------------------------------------------------------
t6::Settings g_cfg;
std::vector<uint8_t> g_sram(16 * 0x10000);
bool g_sramDirty = false;
std::string g_sramPath;
std::vector<uint8_t> g_response;
size_t g_responseSeek = 0;

struct IoState { bool testOn = false, testHeld = false, coinHeld[2] = {false, false}; uint16_t coins[2] = {0, 0}; } g_io;

bool gameFocused()
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// T6_INPUT_SCRIPT="10:coin1,12:start1,14.5:lp1:500" -- for unattended tests: at <sec> after
// the first input poll, hold <action> for <ms> (default 150). Actions: up/down/left/right,
// lp/rp/lk/rk, start (suffix 1 or 2 for the player), coin1, coin2, test, service.
struct ScriptStep { double at; std::string what; double ms; };
std::vector<ScriptStep> g_script;
ULONGLONG g_scriptT0 = 0;
void loadScript()
{
    const char* e = getenv("T6_INPUT_SCRIPT");
    if (!e) return;
    std::string all(e);
    size_t i = 0;
    while (i < all.size()) {
        size_t j = all.find(',', i); if (j == std::string::npos) j = all.size();
        std::string tok = all.substr(i, j - i);
        double at = 0, ms = 150; char what[32] = {};
        if (sscanf(tok.c_str(), "%lf:%31[^:]:%lf", &at, what, &ms) >= 2) g_script.push_back({at, what, ms});
        i = j + 1;
    }
}
bool scripted(const char* what)
{
    if (g_script.empty()) return false;
    const double t = (GetTickCount64() - g_scriptT0) / 1000.0;
    for (auto& st : g_script)
        if (st.what == what && t >= st.at && t < st.at + st.ms / 1000.0) return true;
    return false;
}

void tekkenInputs()
{
    if (!g_scriptT0) { g_scriptT0 = GetTickCount64(); loadScript(); }
    XINPUT_STATE st[4];
    const XINPUT_STATE* pads[4];
    for (int i = 0; i < 4; i++) pads[i] = XInputGetState(i, &st[i]) == ERROR_SUCCESS ? &st[i] : nullptr;
    const bool focus = gameFocused();

    // T6 reads the board block at 0x100 in JVS switch order (found by bisection on the
    // title screen; RPCS3's "tekken" layout is TTT2's):
    //   0x100 system: 0x80 test
    //   0x101 P1: 0x80 start, 0x40 service, 0x20 up, 0x10 down, 0x08 left, 0x04 right, 0x02 1, 0x01 2
    //   0x102 P1: 0x80 3, 0x40 4
    //   0x103/0x104: P2, same
    //   0x110/0x112: coin counters (LE u16), slot 1 / slot 2
    static const uint8_t mask1[t6::kPlayerActionCount] = {0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0, 0, 0x80};
    static const uint8_t mask2[t6::kPlayerActionCount] = {0, 0, 0, 0, 0, 0, 0x80, 0x40, 0};
    static const char* const sname[t6::kPlayerActionCount] = {"up", "down", "left", "right", "lp", "rp", "lk", "rk", "start"};
    uint8_t sw[5] = {};
    for (int p = 0; p < 2; p++)
        for (int a = 0; a < t6::kPlayerActionCount; a++) {
            char sn[16]; snprintf(sn, sizeof sn, "%s%d", sname[a], p + 1);
            if (t6::bindingHeld(g_cfg.players[p].b[a], pads, focus) || scripted(sn)) {
                sw[1 + p * 2] |= mask1[a];
                sw[2 + p * 2] |= mask2[a];
            }
        }
    // A cabinet stick cannot hold opposite directions; drop both if a pad/keyboard does.
    for (int p = 0; p < 2; p++) {
        uint8_t& d = sw[1 + p * 2];
        if ((d & 0x30) == 0x30) d &= ~0x30;
        if ((d & 0x0C) == 0x0C) d &= ~0x0C;
    }

    const bool test = t6::bindingHeld(g_cfg.system.b[t6::Test], pads, focus) || scripted("test");
    if (test && !g_io.testHeld) g_io.testOn = !g_io.testOn;   // the cabinet's test switch latches
    g_io.testHeld = test;
    if (g_io.testOn) sw[0] |= 0x80;
    if (t6::bindingHeld(g_cfg.system.b[t6::Service], pads, focus) || scripted("service")) sw[1] |= 0x40;
    for (int c = 0; c < 2; c++) {
        const bool coin = t6::bindingHeld(g_cfg.system.b[c ? t6::Coin2 : t6::Coin1], pads, focus) || scripted(c ? "coin2" : "coin1");
        if (coin && !g_io.coinHeld[c]) g_io.coins[c] = (uint16_t)((g_io.coins[c] + 1) & 0x3FFF);
        g_io.coinHeld[c] = coin;
    }

    g_response.assign(0x180, 0);
    memcpy(&g_response[0x100], sw, sizeof sw);
    memcpy(&g_response[0x110], &g_io.coins[0], 2);
    memcpy(&g_response[0x112], &g_io.coins[1], 2);
    g_response[2] = 0x10;                                // DIP switches
    // Script action "raw<hexoff>x<hexmask>": OR raw bits into the report (input bisection).
    if (!g_script.empty()) {
        const double t = (GetTickCount64() - g_scriptT0) / 1000.0;
        for (auto& st : g_script) {
            unsigned off = 0, mask = 0;
            if (t >= st.at && t < st.at + st.ms / 1000.0 && sscanf(st.what.c_str(), "raw%xx%x", &off, &mask) == 2 && off < g_response.size())
                g_response[off] |= (uint8_t)mask;
        }
    }
}

void usioRead(uint8_t ch, uint16_t reg, uint16_t size)
{
    {   // T6_USB_LOG: which registers the game polls, once every ~5 s
        static std::map<uint32_t, unsigned> hist; static ULONGLONG last = 0;
        hist[(uint32_t)ch << 16 | reg]++;
        if (GetTickCount64() - last > 5000) {
            last = GetTickCount64();
            for (auto& kv : hist) log("read ch%u reg 0x%04X x%u (coins=%u/%u test=%d)", kv.first >> 16, kv.first & 0xFFFF, kv.second, g_io.coins[0], g_io.coins[1], g_io.testOn);
            hist.clear();
        }
    }
    g_response.clear();
    g_responseSeek = 0;
    if (ch == 0) {
        switch (reg) {
        case 0x0000: {
            static const uint8_t buf[] = {0x7E, 0xE4, 0x00, 0x00, 0x74, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            g_response.assign(buf, buf + sizeof buf);
            break;
        }
        case 0x1000:
            tekkenInputs();
            break;
        case 0x1800: case 0x1880: {
            // Firmware id + per-board capability blocks, verbatim from RPCS3 (usio.cpp):
            // "NBGI.;USIO01;Ver1.00;JPN,Multipurpose with PPG." and two board blocks.
            static const uint8_t kInfo[0x180] = {
        0x4E, 0x42, 0x47, 0x49, 0x2E, 0x3B, 0x55, 0x53, 0x49, 0x4F, 0x30, 0x31, 0x3B, 0x56, 0x65, 0x72,
        0x31, 0x2E, 0x30, 0x30, 0x3B, 0x4A, 0x50, 0x4E, 0x2C, 0x4D, 0x75, 0x6C, 0x74, 0x69, 0x70, 0x75,
        0x72, 0x70, 0x6F, 0x73, 0x65, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20, 0x50, 0x50, 0x47, 0x2E, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x4E, 0x42, 0x47, 0x49, 0x31, 0x3B, 0x55, 0x53, 0x49, 0x4F, 0x30, 0x31, 0x3B, 0x56, 0x65, 0x72,
        0x31, 0x2E, 0x30, 0x30, 0x3B, 0x4A, 0x50, 0x4E, 0x2C, 0x4D, 0x75, 0x6C, 0x74, 0x69, 0x70, 0x75,
        0x72, 0x70, 0x6F, 0x73, 0x65, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20, 0x50, 0x50, 0x47, 0x2E, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x13, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x03, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00,
        0x75, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x4E, 0x42, 0x47, 0x49, 0x32, 0x3B, 0x55, 0x53, 0x49, 0x4F, 0x30, 0x31, 0x3B, 0x56, 0x65, 0x72,
        0x31, 0x2E, 0x30, 0x30, 0x3B, 0x4A, 0x50, 0x4E, 0x2C, 0x4D, 0x75, 0x6C, 0x74, 0x69, 0x70, 0x75,
        0x72, 0x70, 0x6F, 0x73, 0x65, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20, 0x50, 0x50, 0x47, 0x2E, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x13, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x03, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00,
        0x75, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            };
            std::vector<uint8_t> info(kInfo, kInfo + sizeof kInfo);
            g_response.assign(info.begin() + (reg - 0x1800), info.end());
            break;
        }
        default:
            log("unhandled read ch0 reg 0x%04X size 0x%X", reg, size);
            break;
        }
    } else if (ch >= 2) {
        const size_t page = ch - 2, end = (size_t)reg + size;
        if (page < 16 && end <= 0x10000) {
            const uint8_t* src = &g_sram[page * 0x10000 + reg];
            g_response.assign(src, src + size);
        }
    }
    g_response.resize(size);
}

void usioWrite(uint8_t ch, uint16_t reg, const std::vector<uint8_t>& data)
{
    if (ch >= 2) {
        const size_t page = ch - 2, end = (size_t)reg + data.size();
        if (page < 16 && end <= 0x10000 && !data.empty()) {
            std::lock_guard<std::mutex> l(g_mx);
            memcpy(&g_sram[page * 0x10000 + reg], data.data(), data.size());
            g_sramDirty = true;
        }
        return;
    }
    if (ch == 0 && reg == 0x000A && data.size() == 2 && data[0] == 0x66 && data[1] == 0x66) {
        std::lock_guard<std::mutex> l(g_mx);
        std::fill(g_sram.begin(), g_sram.end(), 0);
        g_sramDirty = true;
    }
    log("write ch%u reg 0x%04X len %zu", ch, reg, data.size());
}

// The command/data state machine on EP 0x01.
struct { bool expectData = false; std::vector<uint8_t> data; uint8_t ch = 0; uint16_t reg = 0, len = 0; } g_cmd;

int32_t bulkOut(const uint8_t* buf, uint32_t n)
{
    if (g_cmd.expectData) {
        g_cmd.data.insert(g_cmd.data.end(), buf, buf + n);
        g_cmd.len = (uint16_t)(g_cmd.len > n ? g_cmd.len - n : 0);
        if (!g_cmd.len) { g_cmd.expectData = false; usioWrite(g_cmd.ch, g_cmd.reg, g_cmd.data); }
        return (int32_t)n;
    }
    if (n != 6) { log("expected a 6-byte command, got %u", n); return (int32_t)n; }
    g_cmd.ch = buf[0] & 0xF;
    g_cmd.reg = (uint16_t)(buf[2] | buf[3] << 8);
    g_cmd.len = (uint16_t)(buf[4] | buf[5] << 8);
    if ((buf[0] & 0x90) == 0x90) { g_cmd.expectData = g_cmd.len != 0; g_cmd.data.clear(); }
    else if ((buf[0] & 0x10) == 0x10) usioRead(g_cmd.ch, g_cmd.reg, g_cmd.len);
    else if ((buf[0] & 0xA0) == 0xA0) {
        if (g_cmd.ch == 0 && g_cmd.reg == 0x000A) { std::lock_guard<std::mutex> l(g_mx); std::fill(g_sram.begin(), g_sram.end(), 0); g_sramDirty = true; }
    } else log("unexpected command 0x%02X", buf[0]);
    return (int32_t)n;
}

int32_t bulkIn(uint8_t* buf, uint32_t n)
{
    const size_t left = g_response.size() > g_responseSeek ? g_response.size() - g_responseSeek : 0;
    const uint32_t k = (uint32_t)std::min<size_t>(n, left);
    if (k) memcpy(buf, &g_response[g_responseSeek], k);
    g_responseSeek += k;
    return (int32_t)k;
}

void loadSram()
{
    if (FILE* f = fopen(g_sramPath.c_str(), "rb")) {
        if (fread(g_sram.data(), 1, g_sram.size(), f) != g_sram.size()) std::fill(g_sram.begin(), g_sram.end(), 0);
        fclose(f);
    }
}

void saveSram()
{
    std::vector<uint8_t> copy;
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (!g_sramDirty) return;
        g_sramDirty = false;
        copy = g_sram;
    }
    std::string tmp = g_sramPath + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        fwrite(copy.data(), 1, copy.size(), f);
        fclose(f);
        MoveFileExA(tmp.c_str(), g_sramPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

// ---- usbd worker: attach + completion callbacks ---------------------------------------
DWORD WINAPI usbdThread(LPVOID)
{
    DWORD lastSave = GetTickCount();
    for (;;) {
        if (g_attachPending.exchange(false) && g_ldd.probe) {
            Sleep(50);   // let the registering thread return first, as the real usbd thread would
            for (auto& d : g_devs) {
                const int32_t pr = (int32_t)guestCall(g_ldd.probe, (uint64_t)d.id);
                log("probe(%d) -> %d", d.id, pr);
                if (pr == 0 && g_ldd.attach) {
                    const int32_t ar = (int32_t)guestCall(g_ldd.attach, (uint64_t)d.id);
                    log("attach(%d) -> %d", d.id, ar);
                }
            }
        }
        for (;;) {
            Completion c;
            {
                std::lock_guard<std::mutex> l(g_mx);
                if (g_done.empty()) break;
                c = g_done.front();
                g_done.pop_front();
            }
            if (c.cb) guestCall(c.cb, c.a0, c.a1, c.a2, c.a3);
        }
        t6_movie_tick();
        if (GetTickCount() - lastSave > 3000) { saveSram(); lastSave = GetTickCount(); }
        Sleep(1);
    }
}

void startThread()
{
    if (!g_threadStarted.exchange(true)) CreateThread(nullptr, 1 << 20, usbdThread, nullptr, 0, nullptr);
}

void queueCall(uint32_t cb, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0)
{
    std::lock_guard<std::mutex> l(g_mx);
    g_done.push_back({cb, a0, a1, a2, a3});
}
// USB transfer completion: cb(s32 result, s32 count, void* arg)
void queueDone(uint32_t cb, uint32_t arg, int32_t result, int32_t count)
{
    queueCall(cb, (uint64_t)(int64_t)result, (uint64_t)(int64_t)count, arg);
}

// ---- HLE handlers ---------------------------------------------------------------------
#define RET(v) (ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(v))

void hInit(ppu_context* ctx) { log("Init"); startThread(); RET(0); }
void hEnd(ppu_context* ctx) { RET(0); }
void hOk(ppu_context* ctx) { RET(0); }
extern "C" void ppu_dump_guest_stack(ppu_context*, const char*);
void hExitSpawnLevel(ppu_context* ctx)   // diagnostics: who is exiting?
{
    fprintf(stderr, "[t6] sys_prx_exitspawn_with_level called\n");
    ppu_dump_guest_stack(ctx, "exit");
    RET(0);
}

void hRegisterLdd(ppu_context* ctx)
{
    const uint32_t ops = (uint32_t)ctx->gpr[3];
    g_ldd = {rd32(ops), rd32(ops + 4), rd32(ops + 8), rd32(ops + 12)};
    const char* name = g_ldd.name ? (const char*)G(g_ldd.name) : "?";
    log("RegisterLdd '%s' probe=0x%08X attach=0x%08X detach=0x%08X", name, g_ldd.probe, g_ldd.attach, g_ldd.detach);
    startThread();
    g_attachPending = true;
    RET(0);
}

void hUnregisterLdd(ppu_context* ctx) { g_ldd = {}; RET(0); }

void hScanStaticDescriptor(ppu_context* ctx)
{
    const int32_t dev = (int32_t)ctx->gpr[3];
    const uint32_t ptr = (uint32_t)ctx->gpr[4];
    const uint8_t type = (uint8_t)ctx->gpr[5];
    Device* d = findDev(dev);
    if (!d) { RET(0); return; }
    uint32_t p = d->descBase;
    if (ptr >= d->descBase && ptr < d->descEnd) p = ptr + G(ptr)[0];   // continue after the given one
    for (; p < d->descEnd; p += G(p)[0]) {
        if (!G(p)[0]) break;
        if (G(p)[1] == type) { RET(p); return; }
    }
    RET(0);
}

void hOpenPipe(ppu_context* ctx)
{
    const int32_t dev = (int32_t)ctx->gpr[3];
    const uint32_t ed = (uint32_t)ctx->gpr[4];
    if (!findDev(dev)) { RET(-0x80110103); return; }   // CELL_USBD_ERROR_DEVICE_NOT_FOUND-ish
    const uint8_t ep = ed ? G(ed)[2] : 0;
    for (int i = 0; i < 32; i++)
        if (!g_pipes[i].open) {
            g_pipes[i] = {true, dev, ep};
            log("OpenPipe dev %d ep 0x%02X -> %d", dev, ep, i);
            RET(i);
            return;
        }
    RET(-0x80110104);
}

void hClosePipe(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3];
    if (id < 32) g_pipes[id].open = false;
    RET(0);
}

void hBulkTransfer(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3];
    const uint32_t buf = (uint32_t)ctx->gpr[4];
    const uint32_t len = (uint32_t)ctx->gpr[5];
    const uint32_t cb = (uint32_t)ctx->gpr[6], arg = (uint32_t)ctx->gpr[7];
    if (id >= 32 || !g_pipes[id].open) { RET(-0x80110105); return; }
    int32_t count = 0;
    if (g_pipes[id].dev == kUsio) {
        if (g_pipes[id].endpoint == 0x01) count = bulkOut(G(buf), len);
        else if (g_pipes[id].endpoint == 0x82) {
            if (g_cmd.reg == 0x1000) { static int n; if (n < 6 && ++n) log("input read into guest buf 0x%08X len %u", buf, len); }
            count = bulkIn(G(buf), len);
        }
    } else {
        log("bulk on USB key ep 0x%02X len %u (ignored)", g_pipes[id].endpoint, len);
    }
    queueDone(cb, arg, 0, count);
    RET(0);
}

void hControlTransfer(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3];
    const uint32_t req = (uint32_t)ctx->gpr[4];
    const uint32_t buf = (uint32_t)ctx->gpr[5];
    const uint32_t cb = (uint32_t)ctx->gpr[6], arg = (uint32_t)ctx->gpr[7];
    const uint8_t* r = G(req);
    const uint8_t type = r[0], request = r[1];
    // The guest builds the setup packet with native (big-endian) u16 fields.
    const uint16_t value = (uint16_t)(r[2] << 8 | r[3]), length = (uint16_t)(r[6] << 8 | r[7]);
    int32_t count = 0;
    Device* d = id < 32 ? findDev(g_pipes[id].dev) : nullptr;
    if (d && request == 0x06 && (type & 0x80) && buf) {   // GET_DESCRIPTOR
        const uint8_t dt = value >> 8, index = value & 0xFF;
        if (dt == 0x03) {                                 // STRING (UTF-16LE)
            uint8_t sd[2 + 2 * 64] = {};
            size_t n = 0;
            if (index == 0) { sd[2] = 0x09; sd[3] = 0x04; n = 4; }           // English (US)
            else if (index <= d->strings.size()) {
                const std::string& str = d->strings[index - 1];
                for (size_t i = 0; i < str.size() && i < 64; i++) sd[2 + i * 2] = (uint8_t)str[i];
                n = 2 + str.size() * 2;
            }
            if (n) {
                sd[0] = (uint8_t)n; sd[1] = 0x03;
                count = (int32_t)std::min<size_t>(length, n);
                memcpy(G(buf), sd, (size_t)count);
            }
        } else {
            uint32_t p = 0;
            for (uint32_t q = d->descBase; q < d->descEnd; q += G(q)[0]) if (G(q)[1] == dt) { p = q; break; }
            if (p) {
                const uint32_t total = dt == 0x02 ? d->configTotal : G(p)[0];
                count = (int32_t)std::min<uint32_t>(length, total);
                memcpy(G(buf), G(p), (size_t)count);
            }
        }
    }
    log("ControlTransfer dev %d pipe %u type 0x%02X req 0x%02X value 0x%04X len %u -> %d", d ? d->id : -1, id, type, request, value, length, count);
    queueDone(cb, arg, 0, count);
    RET(0);
}

void hSetPrivateData(ppu_context* ctx)
{
    if (Device* d = findDev((int)ctx->gpr[3])) d->privateData = (uint32_t)ctx->gpr[4];
    RET(0);
}
void hGetPrivateData(ppu_context* ctx)
{
    Device* d = findDev((int)ctx->gpr[3]);
    RET(d ? d->privateData : 0);
}

// ---- USB key (dongle) identity -------------------------------------------------------
// The game asks lv2 which USB device is mounted at /dev_usbXXX through sys_fs_fcntl
// 0xC0000015 (vid/pid) and 0xC000001C (vid/pid/serial), and checks it against its key
// file. Report the TeknoParrot dump's key: /dev_usb000 = 0693:0026 serial 76C0D0000000.
typedef int64_t (*Lv2Fn)(ppu_context*);
Lv2Fn g_prevFcntl = nullptr;
std::string g_usbSerial = "76C0D0000000";
uint16_t g_usbVid = 0x0693, g_usbPid = 0x0026;

int64_t sysFsFcntl(ppu_context* ctx)
{
    const uint32_t op = (uint32_t)ctx->gpr[4], arg = (uint32_t)ctx->gpr[5];
    if ((op == 0xC0000015u || op == 0xC000001Cu) && arg) {
        const uint32_t pathSize = rd32(arg + 0x0C), path = rd32(arg + 0x10);
        std::string vpath((const char*)G(path), strnlen((const char*)G(path), pathSize));
        const bool key = vpath.rfind("/dev_usb000", 0) == 0;
        log("fcntl 0x%08X path '%s' -> %s", op, vpath.c_str(), key ? "USB key" : "not mounted");
        if (key) {
            G(arg + 0x14)[0] = g_usbVid >> 8; G(arg + 0x14)[1] = g_usbVid & 0xFF;
            G(arg + 0x16)[0] = g_usbPid >> 8; G(arg + 0x16)[1] = g_usbPid & 0xFF;
            if (op == 0xC000001Cu) {
                memset(G(arg + 0x20), 0, 64);
                for (size_t i = 0; i < g_usbSerial.size() && i < 32; i++) {
                    G(arg + 0x20 + i * 2)[0] = 0;                   // UTF-16 big-endian
                    G(arg + 0x20 + i * 2)[1] = (uint8_t)g_usbSerial[i];
                }
            }
            wr32(arg + 0x18, 0);
        } else {
            wr32(arg + 0x18, 0x8001003Au);                           // CELL_ENOTMOUNTED
        }
        return 0;
    }
    return g_prevFcntl ? g_prevFcntl(ctx) : 0;
}

// ---- small HLE gaps ---------------------------------------------------------------------
void wr64(uint32_t ea, uint64_t v) { v = _byteswap_uint64(v); memcpy(G(ea), &v, 8); }

// cellPamfGetHeaderSize(PamfHeader* p, u64 fileSize, u64* size)
void hPamfGetHeaderSize(ppu_context* ctx)
{
    const uint32_t p = (uint32_t)ctx->gpr[3], out = (uint32_t)ctx->gpr[5];
    if (!p || memcmp(G(p), "PAMF", 4) != 0) { RET(0x80610504); return; }   // CELL_PAMF_ERROR_UNKNOWN_TYPE
    if (out) wr64(out, (uint64_t)rd32(p + 8) << 11);
    RET(0);
}
// cellPamfGetStreamOffsetAndSize(PamfHeader* p, u64 fileSize, u64* offset, u64* size)
void hPamfGetStreamOffsetAndSize(ppu_context* ctx)
{
    const uint32_t p = (uint32_t)ctx->gpr[3], off = (uint32_t)ctx->gpr[5], sz = (uint32_t)ctx->gpr[6];
    if (!p || memcmp(G(p), "PAMF", 4) != 0) { RET(0x80610504); return; }
    if (off) wr64(off, (uint64_t)rd32(p + 8) << 11);
    if (sz) wr64(sz, getenv("T6_MOVIE_TINY") ? 0x800 : (uint64_t)rd32(p + 12) << 11);
    log("PamfGetStreamOffsetAndSize hdr=0x%08X off=0x%llX size=0x%llX fileSize=0x%llX", p,
        (unsigned long long)rd32(p + 8) << 11, (unsigned long long)rd32(p + 12) << 11, (unsigned long long)ctx->gpr[4]);
    RET(0);
}
// ---- movies: an empty PAMF reader --------------------------------------------------------
// The runtime has no working PAMF/Dmux/AVC pipeline (its cellPamf uses a made-up layout and
// host pointers). The game's movie player falls back cleanly when a file has no stream it
// wants, so report every PAMF file as having none: the attract movies are skipped.
constexpr uint32_t kPamfStreamNotFound = 0x80610501u;
void hPamfReaderInitialize(ppu_context* ctx) { RET(0); }
void hPamfZeroCount(ppu_context* ctx) { RET(0); }                       // GetNumberOfSpecificStreams
void hPamfNotFound(ppu_context* ctx) { RET(kPamfStreamNotFound); }     // SetStream*/GetStreamInfo/TypeAndChannel
void hPamfTime(ppu_context* ctx)                                         // Get{Start,End}Time(reader, CellCodecTimeStamp*)
{
    const uint32_t out = (uint32_t)ctx->gpr[4];
    if (out) { wr32(out, 0); wr32(out + 4, 0); }
    RET(0);
}
// cellPamfStreamTypeToEsFilterId(u8 type, u8 ch, CellCodecEsFilterId* out) (RPCS3's table)
void hPamfStreamTypeToEsFilterId(ppu_context* ctx)
{
    const uint32_t type = (uint32_t)ctx->gpr[3] & 0xFF, ch = (uint32_t)ctx->gpr[4] & 0xFF, out = (uint32_t)ctx->gpr[5];
    if (!out) { RET(0x80610503u); return; }
    uint32_t major = 0, minor = 0, s1 = 0, s2 = 0;
    switch (type) {
    case 0: case 20: major = 0xE0 + ch; break;                  // AVC / video
    case 1: major = 0xE0 + ch; s1 = 1; break;                   // M2V
    case 2: case 21: major = 0xBD; minor = ch; break;           // ATRAC3+ / audio
    case 3: major = 0xBD; minor = 0x40 + ch; break;             // LPCM
    case 4: major = 0xBD; minor = 0x30 + ch; break;             // AC3
    case 5: major = 0xBD; minor = 0x20 + ch; break;             // user data
    default: RET(0x80610504u); return;
    }
    wr32(out, major); wr32(out + 4, minor); wr32(out + 8, s1); wr32(out + 12, s2);
    RET(0);
}

// ---- movies: a demuxer that finds nothing ------------------------------------------------
// Paired with the empty PAMF reader: every stream handed in is reported consumed
// (DEMUX_DONE), no access unit is ever produced, and flushes complete at once.
constexpr uint32_t kDmuxMsgBase = 0x6F820000u;   // guest scratch for CellDmuxMsg / CellDmuxEsMsg
struct DmuxEs { uint32_t cb = 0, arg = 0; bool used = false; };
struct Dmux { uint32_t cb = 0, arg = 0; bool used = false; DmuxEs es[8]; } g_dmux[4];
std::atomic<uint32_t> g_dmuxMsgSlot{0};

uint32_t dmuxMsg(int32_t type, uint64_t info)
{
    const uint32_t at = kDmuxMsgBase + (g_dmuxMsgSlot++ % 256) * 16;
    wr32(at, (uint32_t)type); wr32(at + 4, 0); wr64(at + 8, info);
    return at;
}
void hDmuxQueryAttr(ppu_context* ctx)       // (type*, attr*)
{
    const uint32_t attr = (uint32_t)ctx->gpr[4];
    if (attr) { wr32(attr, 0x10000); wr32(attr + 4, 1); wr32(attr + 8, 0); }
    RET(0);
}
void hDmuxQueryEsAttr(ppu_context* ctx)     // (type*, filterId*, specificInfo, esAttr*)
{
    const uint32_t attr = (uint32_t)ctx->gpr[6];
    if (attr) wr32(attr, 0x10000);
    RET(0);
}
void hDmuxOpen(ppu_context* ctx)            // (type*, res*, cb*, handle*)
{
    const uint32_t cb = (uint32_t)ctx->gpr[5], out = (uint32_t)ctx->gpr[6];
    for (uint32_t i = 0; i < 4; i++)
        if (!g_dmux[i].used) {
            g_dmux[i] = {};
            g_dmux[i].used = true;
            if (cb) { g_dmux[i].cb = rd32(cb); g_dmux[i].arg = rd32(cb + 4); }
            if (out) wr32(out, i + 1);
            log("DmuxOpen -> %u", i + 1);
            RET(0);
            return;
        }
    RET(0x80610205u);
}
Dmux* dmuxOf(uint32_t h) { return (h >= 1 && h <= 4 && g_dmux[h - 1].used) ? &g_dmux[h - 1] : nullptr; }
void hDmuxClose(ppu_context* ctx) { if (Dmux* d = dmuxOf((uint32_t)ctx->gpr[3])) d->used = false; RET(0); }
void hDmuxSetStream(ppu_context* ctx)       // (handle, addr, size, discontinuity, userData)
{
    const uint32_t h = (uint32_t)ctx->gpr[3];
    Dmux* d = dmuxOf(h);
    if (!d) { RET(0x80610201u); return; }
    if (d->cb) {
        queueCall(d->cb, h, dmuxMsg(0, ctx->gpr[7]), d->arg);            // DEMUX_DONE
        queueCall(d->cb, h, dmuxMsg(2, ctx->gpr[7]), d->arg);            // PROG_END_CODE: the movie is over
    }
    RET(0);
}
void hDmuxEnableEs(ppu_context* ctx)        // (handle, filterId*, esRes*, esCb*, specificInfo, esHandle*)
{
    Dmux* d = dmuxOf((uint32_t)ctx->gpr[3]);
    const uint32_t cb = (uint32_t)ctx->gpr[6], out = (uint32_t)ctx->gpr[8];
    if (!d) { RET(0x80610201u); return; }
    for (uint32_t i = 0; i < 8; i++)
        if (!d->es[i].used) {
            d->es[i].used = true;
            if (cb) { d->es[i].cb = rd32(cb); d->es[i].arg = rd32(cb + 4); }
            if (out) wr32(out, ((uint32_t)(d - g_dmux) + 1) << 8 | (i + 1));
            RET(0);
            return;
        }
    RET(0x80610205u);
}
DmuxEs* esOf(uint32_t h, uint32_t* dmuxHandle)
{
    Dmux* d = dmuxOf(h >> 8);
    const uint32_t i = (h & 0xFF) - 1;
    if (!d || i >= 8 || !d->es[i].used) return nullptr;
    if (dmuxHandle) *dmuxHandle = h >> 8;
    return &d->es[i];
}
void hDmuxDisableEs(ppu_context* ctx) { if (DmuxEs* e = esOf((uint32_t)ctx->gpr[3], nullptr)) e->used = false; RET(0); }
void hDmuxGetAu(ppu_context* ctx) { RET(0x80610204u); }                 // CELL_DMUX_ERROR_EMPTY
void hDmuxFlushEs(ppu_context* ctx)
{
    const uint32_t esh = (uint32_t)ctx->gpr[3];
    uint32_t dh = 0;
    DmuxEs* e = esOf(esh, &dh);
    if (!e) { RET(0x80610201u); return; }
    if (e->cb) queueCall(e->cb, dh, esh, dmuxMsg(1, 0), e->arg);         // ES FLUSH_DONE
    RET(0);
}

// int inet_aton(const char* cp, in_addr* out)
void hInetAton(ppu_context* ctx)
{
    const uint32_t cp = (uint32_t)ctx->gpr[3], out = (uint32_t)ctx->gpr[4];
    unsigned a, b, c, d;
    if (cp && sscanf((const char*)G(cp), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
        if (out) wr32(out, a << 24 | b << 16 | c << 8 | d);
        RET(1);
        return;
    }
    RET(0);
}

}  // namespace

// Called once from main() after the HLE tables are set up.
extern "C" void t6_io_init(const char* iniPath, const char* sramPath)
{
    g_cfg = t6::loadSettings(iniPath);
    g_sramPath = sramPath;
    loadSram();
    buildDevices();

    t6_prof_start();
    t6_clock_init();
    t6_movie_init();   // before the empty PAMF/Dmux fallbacks below (first registration wins)
    startThread();     // the worker also runs the demuxer and movie callbacks
    ps3_hle_register_ctx(0xD0E766FEu, "cellUsbdInit", hInit);
    ps3_hle_register_ctx(0x35F22AC3u, "cellUsbdEnd", hEnd);
    ps3_hle_register_ctx(0x5C832BD7u, "cellUsbdSetThreadPriority2", hOk);
    ps3_hle_register_ctx(0x359BEFBAu, "cellUsbdRegisterLdd", hRegisterLdd);
    ps3_hle_register_ctx(0x64951AC7u, "cellUsbdUnregisterLdd", hUnregisterLdd);
    ps3_hle_register_ctx(0x2FB08E1Eu, "cellUsbdScanStaticDescriptor", hScanStaticDescriptor);
    ps3_hle_register_ctx(0x254289ACu, "cellUsbdOpenPipe", hOpenPipe);
    ps3_hle_register_ctx(0x9763E962u, "cellUsbdClosePipe", hClosePipe);
    ps3_hle_register_ctx(0xAC77EB78u, "cellUsbdBulkTransfer", hBulkTransfer);
    ps3_hle_register_ctx(0x97CF128Eu, "cellUsbdControlTransfer", hControlTransfer);
    ps3_hle_register_ctx(0x63BFDB97u, "cellUsbdSetPrivateData", hSetPrivateData);
    ps3_hle_register_ctx(0x5DE3AF36u, "cellUsbdGetPrivateData", hGetPrivateData);
    // The board's watchdog (System 357 only): nothing to feed.
    ps3_hle_register_ctx(0x9E0623B5u, "sys_game_watchdog_start", hOk);
    ps3_hle_register_ctx(0x6E05231Du, "sys_game_watchdog_stop", hOk);
    ps3_hle_register_ctx(0xACAD8FB6u, "sys_game_watchdog_clear", hOk);
    if (getenv("T6_EXIT_BT")) ps3_hle_register_ctx(0xA2C7BA64u, "sys_prx_exitspawn_with_level", hExitSpawnLevel);
    // Unimplemented in the runtime; nothing in T6 depends on their output.
    ps3_hle_register_ctx(0xBE5BE3BAu, "cellPadSetSensorMode", hOk);
    ps3_hle_register_ctx(0x4CEB9694u, "cellSpursGetJobChainInfo", hOk);
    ps3_hle_register_ctx(0xCA8181C1u, "cellPamfGetHeaderSize", hPamfGetHeaderSize);
    ps3_hle_register_ctx(0x44F5C9E3u, "cellPamfGetStreamOffsetAndSize", hPamfGetStreamOffsetAndSize);
    ps3_hle_register_ctx(0xA9A079E0u, "inet_aton", hInetAton);
    ps3_hle_register_ctx(0xB8436EE5u, "cellPamfReaderInitialize", hPamfReaderInitialize);
    ps3_hle_register_ctx(0xD0230671u, "cellPamfReaderGetNumberOfSpecificStreams", hPamfZeroCount);
    ps3_hle_register_ctx(0x28B4E2C1u, "cellPamfReaderSetStreamWithTypeAndIndex", hPamfNotFound);
    ps3_hle_register_ctx(0x67FD273Bu, "cellPamfReaderGetStreamInfo", hPamfNotFound);
    ps3_hle_register_ctx(0x9AB20793u, "cellPamfReaderGetStreamTypeAndChannel", hPamfNotFound);
    ps3_hle_register_ctx(0x4DE501B1u, "cellPamfReaderGetPresentationStartTime", hPamfTime);
    ps3_hle_register_ctx(0xF61609D6u, "cellPamfReaderGetPresentationEndTime", hPamfTime);
    ps3_hle_register_ctx(0x01067E22u, "cellPamfStreamTypeToEsFilterId", hPamfStreamTypeToEsFilterId);
    ps3_hle_register_ctx(0xA2D4189Bu, "cellDmuxQueryAttr", hDmuxQueryAttr);
    ps3_hle_register_ctx(0x02170D1Au, "cellDmuxQueryEsAttr", hDmuxQueryEsAttr);
    ps3_hle_register_ctx(0x68492DE9u, "cellDmuxOpen", hDmuxOpen);
    ps3_hle_register_ctx(0x8C692521u, "cellDmuxClose", hDmuxClose);
    ps3_hle_register_ctx(0x04E7499Fu, "cellDmuxSetStream", hDmuxSetStream);
    ps3_hle_register_ctx(0x7B56DC3Fu, "cellDmuxEnableEs", hDmuxEnableEs);
    ps3_hle_register_ctx(0x05371C8Du, "cellDmuxDisableEs", hDmuxDisableEs);
    ps3_hle_register_ctx(0x21D424F0u, "cellDmuxResetEs", hOk);
    ps3_hle_register_ctx(0x42C716B5u, "cellDmuxGetAu", hDmuxGetAu);
    ps3_hle_register_ctx(0x24EA6474u, "cellDmuxReleaseAu", hOk);
    ps3_hle_register_ctx(0xEBB3B2BDu, "cellDmuxFlushEs", hDmuxFlushEs);
}

// Flush the backup SRAM (called at exit).
extern "C" void t6_io_shutdown(void) { saveSram(); }

// Called from main() after lv2_init_syscalls() (it rebuilds the table).
extern "C" char g_lv2_syscalls[];   // lv2_syscall_table { Lv2Fn handlers[1024]; }
extern "C" void t6_io_init_syscalls(void)
{
    Lv2Fn* table = reinterpret_cast<Lv2Fn*>(g_lv2_syscalls);
    g_prevFcntl = table[817];        // sys_fs_fcntl
    table[817] = sysFsFcntl;
}

extern "C" void t6_queue_call(uint32_t cb, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    queueCall(cb, a0, a1, a2, a3);
}
