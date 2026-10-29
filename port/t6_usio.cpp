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
#include <mutex>
#include <string>
#include <vector>

extern "C" {
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
constexpr uint32_t kCbStackTop = 0xCF7F0000u;   // below the descriptor block, above thread stacks' range start
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

uint64_t guestCall(uint32_t opd, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0)
{
    uint32_t code = 0, toc = 0;
    if (!opd || ppu_opd_resolve(opd, &code, &toc) != 0 || !code) { code = rd32(opd); toc = rd32(opd + 4); }
    const func_entry* f = findFunc(code);
    if (!f) { fprintf(stderr, "[t6-usb] callback OPD 0x%08X -> code 0x%08X not lifted\n", opd, code); return (uint64_t)-1; }
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[1] = kCbStackTop - 0x100;
    ctx.gpr[2] = toc;
    ctx.gpr[3] = a0; ctx.gpr[4] = a1; ctx.gpr[5] = a2;
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

// ---- the emulated device ----------------------------------------------------------------
constexpr int kDevId = 1;
constexpr uint32_t kDescBase = 0xCF800000u;     // guest copy of the static descriptors
uint32_t g_descEnd = kDescBase;

void buildDescriptors()
{
    static const uint8_t desc[] = {
        // device
        18, 0x01, 0x10, 0x01, 0xFF, 0x00, 0xFF, 0x08, 0x9A, 0x0B, 0x10, 0x09, 0x10, 0x09, 0x01, 0x02, 0x00, 0x01,
        // configuration (wTotalLength 39)
        9, 0x02, 39, 0, 0x01, 0x01, 0x00, 0xC0, 0x32,
        // interface: 3 endpoints, vendor class
        9, 0x04, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
        // EP 0x01 bulk OUT 64
        7, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00,
        // EP 0x82 bulk IN 64
        7, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,
        // EP 0x83 interrupt IN 8
        7, 0x05, 0x83, 0x03, 0x08, 0x00, 16,
    };
    memcpy(G(kDescBase), desc, sizeof desc);
    g_descEnd = kDescBase + sizeof desc;
}

struct Pipe { bool open = false; uint8_t endpoint = 0; };
Pipe g_pipes[16];

struct Ldd { uint32_t name = 0, probe = 0, attach = 0, detach = 0; };
Ldd g_ldd;
uint32_t g_privateData = 0;

struct Completion { uint32_t cb, arg; int32_t result, count; };
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

struct IoState { bool testOn = false, testHeld = false, coinHeld[2] = {false, false}; uint16_t coins = 0; } g_io;

bool gameFocused()
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void tekkenInputs()
{
    XINPUT_STATE st[4];
    const XINPUT_STATE* pads[4];
    for (int i = 0; i < 4; i++) pads[i] = XInputGetState(i, &st[i]) == ERROR_SUCCESS ? &st[i] : nullptr;
    const bool focus = gameFocused();

    uint64_t in = 0;
    uint16_t lm = 0;
    // Bits per player (RPCS3 usio "tekken" layout), player 2 shifted by 24.
    static const uint64_t bit[t6::kPlayerActionCount] = {
        0x200000, 0x100000, 0x80000, 0x40000,           // up down left right
        0x20000, 0x10000, 0x40000000, 0x20000000,       // 1 2 3 4
        0x800000};                                      // start
    for (int p = 0; p < 2; p++)
        for (int a = 0; a < t6::kPlayerActionCount; a++)
            if (t6::bindingHeld(g_cfg.players[p].b[a], pads, focus)) {
                in |= bit[a] << (p * 24);
                if (p == 0) {
                    static const uint16_t lmBit[t6::kPlayerActionCount] = {0x200, 0x400, 0x2000, 0x4000, 0x100, 0, 0, 0, 0x800};
                    lm |= lmBit[a];
                }
            }

    const bool test = t6::bindingHeld(g_cfg.system.b[t6::Test], pads, focus);
    if (test && !g_io.testHeld) g_io.testOn = !g_io.testOn;   // the cabinet's test switch latches
    g_io.testHeld = test;
    if (g_io.testOn) { in |= 0x80; lm |= 0x1000; }
    if (t6::bindingHeld(g_cfg.system.b[t6::Service], pads, focus)) in |= 0x4000;
    for (int c = 0; c < 2; c++) {
        const bool coin = t6::bindingHeld(g_cfg.system.b[c ? t6::Coin2 : t6::Coin1], pads, focus);
        if (coin && !g_io.coinHeld[c]) g_io.coins++;
        g_io.coinHeld[c] = coin;
    }

    g_response.assign(0x180, 0);
    memcpy(&g_response[0x100], &in, 8);                  // little-endian, as the board sends it
    memcpy(&g_response[0x110], &g_io.coins, 2);
    memcpy(&g_response[0], &lm, 2);
    g_response[2] = 0x10;                                // DIP switches
}

void usioRead(uint8_t ch, uint16_t reg, uint16_t size)
{
    g_response.clear();
    g_responseSeek = 0;
    if (ch == 0) {
        switch (reg) {
        case 0x0000: {
            static const uint8_t buf[] = {0x7E, 0xE4, 0x00, 0x00, 0x74, 0x01, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x7E, 0x00, 0x7E,
                                          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x02, 0, 0, 0, 0, 0, 0, 0x80};
            g_response.assign(buf, buf + sizeof buf);
            break;
        }
        case 0x1000:
            tekkenInputs();
            break;
        case 0x1800: case 0x1880: {
            // "NBGI.;USIO01;Ver1.00;JPN,Multipurpose with PPG." then two board ids
            std::vector<uint8_t> info(0x180, 0);
            const char* id[3] = {"NBGI.;USIO01;Ver1.00;JPN,Multipurpose with PPG.",
                                 "NBGI1;USIO01;Ver1.00;JPN,Multipurpose with PPG.",
                                 "NBGI2;USIO01;Ver1.00;JPN,Multipurpose with PPG."};
            static const uint8_t tail[] = {0x01, 0x00, 0x13, 0x00, 0x30, 0x00, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
                                           0x02, 0x03, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0, 0, 0, 0, 0x03, 0x00, 0x03, 0x00, 0x75, 0x6C};
            for (int i = 0; i < 3; i++) memcpy(&info[i * 0x80], id[i], strlen(id[i]));
            for (int i = 1; i < 3; i++) memcpy(&info[i * 0x80 + 0x30], tail, sizeof tail);
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
            const int32_t pr = (int32_t)guestCall(g_ldd.probe, kDevId);
            log("probe(%d) -> %d", kDevId, pr);
            if (pr == 0 && g_ldd.attach) {
                const int32_t ar = (int32_t)guestCall(g_ldd.attach, kDevId);
                log("attach(%d) -> %d", kDevId, ar);
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
            if (c.cb) guestCall(c.cb, (uint64_t)(int64_t)c.result, (uint64_t)(int64_t)c.count, c.arg);
        }
        if (GetTickCount() - lastSave > 3000) { saveSram(); lastSave = GetTickCount(); }
        Sleep(1);
    }
}

void startThread()
{
    if (!g_threadStarted.exchange(true)) CreateThread(nullptr, 1 << 20, usbdThread, nullptr, 0, nullptr);
}

void queueDone(uint32_t cb, uint32_t arg, int32_t result, int32_t count)
{
    std::lock_guard<std::mutex> l(g_mx);
    g_done.push_back({cb, arg, result, count});
}

// ---- HLE handlers ---------------------------------------------------------------------
#define RET(v) (ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(v))

void hInit(ppu_context* ctx) { log("Init"); startThread(); RET(0); }
void hEnd(ppu_context* ctx) { RET(0); }
void hOk(ppu_context* ctx) { RET(0); }

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
    uint32_t p = kDescBase;
    if (dev != kDevId) { RET(0); return; }
    if (ptr >= kDescBase && ptr < g_descEnd) p = ptr + G(ptr)[0];   // continue after the given one
    for (; p < g_descEnd; p += G(p)[0]) {
        if (!G(p)[0]) break;
        if (G(p)[1] == type) { RET(p); return; }
    }
    RET(0);
}

void hOpenPipe(ppu_context* ctx)
{
    const int32_t dev = (int32_t)ctx->gpr[3];
    const uint32_t ed = (uint32_t)ctx->gpr[4];
    if (dev != kDevId) { RET(-0x80110103); return; }   // CELL_USBD_ERROR_DEVICE_NOT_FOUND-ish
    const uint8_t ep = ed ? G(ed)[2] : 0;
    for (int i = 0; i < 16; i++)
        if (!g_pipes[i].open) {
            g_pipes[i] = {true, ep};
            log("OpenPipe ep 0x%02X -> %d", ep, i);
            RET(i);
            return;
        }
    RET(-0x80110104);
}

void hClosePipe(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3];
    if (id < 16) g_pipes[id].open = false;
    RET(0);
}

void hBulkTransfer(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3];
    const uint32_t buf = (uint32_t)ctx->gpr[4];
    const uint32_t len = (uint32_t)ctx->gpr[5];
    const uint32_t cb = (uint32_t)ctx->gpr[6], arg = (uint32_t)ctx->gpr[7];
    if (id >= 16 || !g_pipes[id].open) { RET(-0x80110105); return; }
    int32_t count = 0;
    if (g_pipes[id].endpoint == 0x01) count = bulkOut(G(buf), len);
    else if (g_pipes[id].endpoint == 0x82) count = bulkIn(G(buf), len);
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
    if (request == 0x06 && (type & 0x80) && buf) {        // GET_DESCRIPTOR
        const uint8_t dt = value >> 8;
        uint32_t p = 0;
        for (uint32_t q = kDescBase; q < g_descEnd; q += G(q)[0]) if (G(q)[1] == dt) { p = q; break; }
        if (p) {
            const uint32_t total = dt == 0x02 ? 39 : G(p)[0];
            count = (int32_t)std::min<uint32_t>(length, total);
            memcpy(G(buf), G(p), (size_t)count);
        }
    }
    log("ControlTransfer pipe %u type 0x%02X req 0x%02X value 0x%04X len %u -> %d", id, type, request, value, length, count);
    queueDone(cb, arg, 0, count);
    RET(0);
}

void hSetPrivateData(ppu_context* ctx) { g_privateData = (uint32_t)ctx->gpr[4]; RET(0); }
void hGetPrivateData(ppu_context* ctx) { RET(g_privateData); }

}  // namespace

// Called once from main() after the HLE tables are set up.
extern "C" void t6_io_init(const char* iniPath, const char* sramPath)
{
    g_cfg = t6::loadSettings(iniPath);
    g_sramPath = sramPath;
    loadSram();
    buildDescriptors();

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
    // Unimplemented in the runtime; nothing in T6 depends on their output.
    ps3_hle_register_ctx(0xBE5BE3BAu, "cellPadSetSensorMode", hOk);
    ps3_hle_register_ctx(0x4CEB9694u, "cellSpursGetJobChainInfo", hOk);
}

// Flush the backup SRAM (called at exit).
extern "C" void t6_io_shutdown(void) { saveSram(); }
