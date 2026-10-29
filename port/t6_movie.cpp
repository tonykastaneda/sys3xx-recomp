// Movies: cellPamf (reader) + cellDmux (PAMF / MPEG-PS demuxer) for T6's opening.pam and
// title.pam. The runtime's versions use a made-up PAMF layout and host pointers, so the game's
// movie player never got a frame -- and the attract sequence waits on movie time.
//
// Layouts follow RPCS3's cellPamf.h / cellDmux.h. Only video is exposed (the reader reports no
// audio streams), so movies play silently; H.264 decoding is the runtime's cellVdec (FFmpeg).
//
// Access units: AVC is split at access-unit delimiters (NAL 9), each AU taking the PTS/DTS of the
// PES packet it starts in. AUs are copied into a ring in an otherwise unused guest region and
// announced with AU_FOUND; the demuxer stalls when the ring is full, as the real one does.

#include "ppu_recomp.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

extern "C" {
extern uint8_t* vm_base;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
void t6_queue_call(uint32_t cb, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
}

namespace {

uint8_t* G(uint32_t ea) { return vm_base + ea; }
uint32_t rd32(uint32_t ea) { uint32_t v; memcpy(&v, G(ea), 4); return _byteswap_ulong(v); }
uint16_t rd16(uint32_t ea) { uint16_t v; memcpy(&v, G(ea), 2); return _byteswap_ushort(v); }
void wr32(uint32_t ea, uint32_t v) { v = _byteswap_ulong(v); memcpy(G(ea), &v, 4); }
void wr16(uint32_t ea, uint16_t v) { v = _byteswap_ushort(v); memcpy(G(ea), &v, 2); }
void wr64(uint32_t ea, uint64_t v) { v = _byteswap_uint64(v); memcpy(G(ea), &v, 8); }
#define RET(v) (ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(v))

void log(const char* fmt, ...)
{
    static int on = -1;
    if (on < 0) on = getenv("T6_MOVIE_LOG") ? 1 : 0;
    if (!on) return;
    va_list ap; va_start(ap, fmt); fprintf(stderr, "[t6-movie] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}

std::recursive_mutex g_mx;

// =============================== PAMF reader ===============================
// Header offsets (PAMF, all big-endian, unaligned):
constexpr uint32_t kSeq = 0x50;                // PamfSequenceInfo
constexpr uint32_t kSeqStartPts = 0x58, kSeqEndPts = 0x5E;
constexpr uint32_t kGroupStreamNum = 0x87;     // PamfGroup.stream_num
constexpr uint32_t kStreams = 0x88;            // PamfStreamHeader[48]
constexpr uint32_t kStreamSize = 48;

enum { PT_AVC = 0, PT_M2V = 1, PT_ATRAC3PLUS = 2, PT_LPCM = 3, PT_AC3 = 4, PT_USER = 5, PT_VIDEO = 20, PT_AUDIO = 21 };

int codingToType(uint8_t c)
{
    switch (c) {
    case 0x1B: return PT_AVC;
    case 0x02: return PT_M2V;
    case 0xDC: return PT_ATRAC3PLUS;
    case 0x80: return PT_LPCM;
    case 0x81: return PT_AC3;
    case 0xDD: return PT_USER;
    default: return -1;
    }
}
bool exposed(int t) { return t == PT_AVC || t == PT_M2V; }   // video only (see top)
bool matches(int want, int t)
{
    if (want == PT_VIDEO) return t == PT_AVC || t == PT_M2V;
    if (want == PT_AUDIO) return t == PT_ATRAC3PLUS || t == PT_LPCM || t == PT_AC3;
    return want == t;
}

struct Reader { uint32_t hdr = 0; int cur = -1; };
std::map<uint32_t, Reader> g_readers;          // by CellPamfReader EA

uint32_t streamHdr(const Reader& r, int i) { return r.hdr + kStreams + (uint32_t)i * kStreamSize; }
int streamCount(const Reader& r) { return G(r.hdr + kGroupStreamNum)[0]; }
int streamType(const Reader& r, int i) { return codingToType(G(streamHdr(r, i))[0]); }
int streamChannel(const Reader& r, int i)
{
    const uint32_t s = streamHdr(r, i);
    const uint8_t id = G(s + 4)[0], pid = G(s + 5)[0];
    return id == 0xBD ? (pid & 0x0F) : (id & 0x0F);
}

constexpr uint32_t kErrNotFound = 0x80610501u, kErrInvalidArg = 0x80610503u, kErrUnknownType = 0x80610504u;

void hReaderInitialize(ppu_context* ctx)       // (reader*, PamfHeader*, u64 fileSize, u32 attr)
{
    const uint32_t self = (uint32_t)ctx->gpr[3], hdr = (uint32_t)ctx->gpr[4];
    if (!self || !hdr || memcmp(G(hdr), "PAMF", 4) != 0) { RET(kErrUnknownType); return; }
    std::lock_guard<std::recursive_mutex> l(g_mx);
    g_readers[self] = {hdr, -1};
    // The public part of CellPamfReader: header/data size (u64 each), attribute.
    wr64(self, (uint64_t)rd32(hdr + 8) << 11);
    wr64(self + 8, (uint64_t)rd32(hdr + 12) << 11);
    wr32(self + 16, (uint32_t)ctx->gpr[6]);
    log("ReaderInitialize %08X hdr %08X: %d stream(s)", self, hdr, G(hdr + kGroupStreamNum)[0]);
    RET(0);
}
Reader* reader(ppu_context* ctx)
{
    auto it = g_readers.find((uint32_t)ctx->gpr[3]);
    return it == g_readers.end() ? nullptr : &it->second;
}
void hGetNumberOfSpecificStreams(ppu_context* ctx)   // (reader*, u8 type) -> u8 count
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Reader* r = reader(ctx);
    if (!r) { RET(0); return; }
    const int want = (int)(ctx->gpr[4] & 0xFF);
    int n = 0;
    for (int i = 0; i < streamCount(*r); i++)
        if (exposed(streamType(*r, i)) && matches(want, streamType(*r, i))) n++;
    RET(n);
}
void hSetStreamWithTypeAndIndex(ppu_context* ctx)    // (reader*, u8 type, u8 index) -> stream index
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Reader* r = reader(ctx);
    if (!r) { RET(kErrInvalidArg); return; }
    const int want = (int)(ctx->gpr[4] & 0xFF), idx = (int)(ctx->gpr[5] & 0xFF);
    int n = 0;
    for (int i = 0; i < streamCount(*r); i++)
        if (exposed(streamType(*r, i)) && matches(want, streamType(*r, i)) && n++ == idx) { r->cur = i; RET(i); return; }
    RET(kErrNotFound);
}
void hGetStreamTypeAndChannel(ppu_context* ctx)      // (reader*, u8* type, u8* ch)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Reader* r = reader(ctx);
    if (!r || r->cur < 0) { RET(kErrInvalidArg); return; }
    const uint32_t t = (uint32_t)ctx->gpr[4], c = (uint32_t)ctx->gpr[5];
    if (t) G(t)[0] = (uint8_t)streamType(*r, r->cur);
    if (c) G(c)[0] = (uint8_t)streamChannel(*r, r->cur);
    RET(0);
}
void hGetStreamInfo(ppu_context* ctx)                // (reader*, void* info, u32 size)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Reader* r = reader(ctx);
    const uint32_t out = (uint32_t)ctx->gpr[4], size = (uint32_t)ctx->gpr[5];
    if (!r || r->cur < 0 || !out) { RET(kErrInvalidArg); return; }
    const uint32_t s = streamHdr(*r, r->cur) + 16;   // specific info
    const int t = streamType(*r, r->cur);
    memset(G(out), 0, size);
    if (t == PT_AVC && size >= 0x20) {               // CellPamfAvcInfo
        const uint8_t* a = G(s);
        G(out)[0] = a[0]; G(out)[1] = a[1];
        G(out)[2] = (a[2] >> 7) & 1;                 // frameMbsOnlyFlag
        G(out)[3] = (a[2] >> 6) & 1;                 // videoSignalInfoFlag
        G(out)[4] = a[2] & 0x0F;                     // frameRateInfo
        G(out)[5] = a[3];
        wr16(out + 6, rd16(s + 4)); wr16(out + 8, rd16(s + 6));
        wr16(out + 10, (uint16_t)(a[9] * 16)); wr16(out + 12, (uint16_t)(a[11] * 16));
        wr16(out + 14, rd16(s + 12)); wr16(out + 16, rd16(s + 14)); wr16(out + 18, rd16(s + 16)); wr16(out + 20, rd16(s + 18));
        G(out)[22] = (a[20] >> 5) & 7; G(out)[23] = (a[20] >> 4) & 1;
        G(out)[24] = a[21]; G(out)[25] = a[22]; G(out)[26] = a[23];
        G(out)[27] = (a[24] >> 7) & 1; G(out)[28] = (a[24] >> 6) & 1; G(out)[29] = (a[24] >> 4) & 3; G(out)[30] = a[24] & 0xF;
        G(out)[31] = a[25];
    }
    RET(0);
}
void hTime(ppu_context* ctx, uint32_t field)         // CellCodecTimeStamp {upper, lower}
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Reader* r = reader(ctx);
    const uint32_t out = (uint32_t)ctx->gpr[4];
    if (!r || !out) { RET(kErrInvalidArg); return; }
    wr32(out, rd16(r->hdr + field));
    wr32(out + 4, rd32(r->hdr + field + 2));
    RET(0);
}
void hStartTime(ppu_context* ctx) { hTime(ctx, kSeqStartPts); }
void hEndTime(ppu_context* ctx) { hTime(ctx, kSeqEndPts); }

void hGetHeaderSize(ppu_context* ctx)                // (PamfHeader*, u64 fileSize, u64* size)
{
    const uint32_t p = (uint32_t)ctx->gpr[3], out = (uint32_t)ctx->gpr[5];
    if (!p || memcmp(G(p), "PAMF", 4) != 0) { RET(kErrUnknownType); return; }
    if (out) wr64(out, (uint64_t)rd32(p + 8) << 11);
    RET(0);
}
void hGetStreamOffsetAndSize(ppu_context* ctx)       // (PamfHeader*, u64 fileSize, u64* off, u64* size)
{
    const uint32_t p = (uint32_t)ctx->gpr[3], off = (uint32_t)ctx->gpr[5], sz = (uint32_t)ctx->gpr[6];
    if (!p || memcmp(G(p), "PAMF", 4) != 0) { RET(kErrUnknownType); return; }
    if (off) wr64(off, (uint64_t)rd32(p + 8) << 11);
    if (sz) wr64(sz, (uint64_t)rd32(p + 12) << 11);
    RET(0);
}
void hStreamTypeToEsFilterId(ppu_context* ctx)       // (u8 type, u8 ch, CellCodecEsFilterId*)
{
    const uint32_t type = (uint32_t)ctx->gpr[3] & 0xFF, ch = (uint32_t)ctx->gpr[4] & 0xFF, out = (uint32_t)ctx->gpr[5];
    if (!out) { RET(kErrInvalidArg); return; }
    uint32_t major = 0, minor = 0, s1 = 0;
    switch (type) {
    case PT_AVC: case PT_VIDEO: major = 0xE0 + ch; break;
    case PT_M2V: major = 0xE0 + ch; s1 = 1; break;
    case PT_ATRAC3PLUS: case PT_AUDIO: major = 0xBD; minor = ch; break;
    case PT_LPCM: major = 0xBD; minor = 0x40 + ch; break;
    case PT_AC3: major = 0xBD; minor = 0x30 + ch; break;
    case PT_USER: major = 0xBD; minor = 0x20 + ch; break;
    default: RET(kErrUnknownType); return;
    }
    wr32(out, major); wr32(out + 4, minor); wr32(out + 8, s1); wr32(out + 12, 0);
    RET(0);
}

// ================================ demuxer ==================================
constexpr uint32_t kErrDmuxArg = 0x80610201u, kErrDmuxEmpty = 0x80610204u, kErrDmuxFatal = 0x80610205u;
constexpr uint32_t kRingBase = 0xC8000000u, kRingSize = 0x00800000u;   // 8 MB per ES, 8 ES slots
constexpr uint32_t kInfoBase = 0xCC000000u;                            // CellDmuxAuInfo slots
constexpr uint32_t kMsgBase = 0xCC800000u;                             // CellDmux(Es)Msg slots
constexpr int kMaxEs = 8, kInfoSlots = 256;

struct Au { uint32_t addr, size; uint64_t pts, dts, user; bool rap; };
struct Es {
    bool used = false;
    int slot = 0;                    // ring/info slot index
    uint32_t dmux = 0, handle = 0;
    uint32_t major = 0, minor = 0;
    uint32_t cb = 0, arg = 0;
    std::vector<uint8_t> partial;    // AU being assembled
    uint64_t partialPts = ~0ull, partialDts = ~0ull;
    bool havePartial = false;
    std::deque<Au> ready;            // demuxed, not yet taken by GetAu
    std::deque<Au> taken;            // handed out, not yet released
    uint32_t head = 0;               // ring write offset
    uint32_t infoNext = 0;
};
struct Stream { uint32_t addr, size, pos; uint64_t user; };
struct Dmux {
    bool used = false;
    uint32_t cb = 0, arg = 0;
    std::deque<Stream> streams;      // SetStream data waiting to be demuxed
    uint64_t curPts = ~0ull, curDts = ~0ull;
};
Dmux g_dmux[4];
Es g_es[kMaxEs];
uint32_t g_msgNext = 0;

uint32_t msg(int32_t type, uint64_t info)
{
    const uint32_t at = kMsgBase + (g_msgNext++ % 512) * 16;
    wr32(at, (uint32_t)type); wr32(at + 4, 0); wr64(at + 8, info);
    return at;
}
Dmux* dmuxOf(uint32_t h) { return (h >= 1 && h <= 4 && g_dmux[h - 1].used) ? &g_dmux[h - 1] : nullptr; }
Es* esOf(uint32_t h) { return (h >= 0x100 && h < 0x100 + kMaxEs && g_es[h - 0x100].used) ? &g_es[h - 0x100] : nullptr; }

uint32_t ringUsed(const Es& e)
{
    uint32_t n = 0;
    for (auto& a : e.ready) n += (a.size + 127) & ~127u;
    for (auto& a : e.taken) n += (a.size + 127) & ~127u;
    return n;
}

// Emit the assembled AU (if any) into the ring. False when the ring has no room.
bool emitAu(Es& e)
{
    if (!e.havePartial || e.partial.empty()) { e.havePartial = false; e.partial.clear(); return true; }
    const uint32_t need = ((uint32_t)e.partial.size() + 127) & ~127u;
    if (need > kRingSize / 2) { e.partial.clear(); e.havePartial = false; return true; }   // corrupt; drop
    if (ringUsed(e) + need > kRingSize - 0x10000) return false;
    if (e.head + need > kRingSize) e.head = 0;
    const uint32_t base = kRingBase + (uint32_t)e.slot * kRingSize;
    memcpy(G(base + e.head), e.partial.data(), e.partial.size());
    Au au{base + e.head, (uint32_t)e.partial.size(), e.partialPts, e.partialDts, 0, false};
    // An IDR slice (NAL 5) makes it a random access point.
    for (size_t i = 0; i + 3 < e.partial.size(); i++)
        if (e.partial[i] == 0 && e.partial[i + 1] == 0 && e.partial[i + 2] == 1 && (e.partial[i + 3] & 0x1F) == 5) { au.rap = true; break; }
    e.head += need;
    e.ready.push_back(au);
    e.partial.clear(); e.havePartial = false;
    if (e.cb) t6_queue_call(e.cb, e.dmux, e.handle, msg(0, au.user), e.arg);   // AU_FOUND
    return true;
}

// Append ES payload bytes; split AVC at access-unit delimiters (00 00 01 09).
bool feedVideo(Es& e, const uint8_t* p, size_t n, uint64_t pts, uint64_t dts)
{
    size_t start = 0;
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1F) == 9) {
            if (e.havePartial) {
                e.partial.insert(e.partial.end(), p + start, p + i);
                if (!emitAu(e)) return false;
            }
            e.havePartial = true;
            e.partialPts = pts; e.partialDts = dts;
            pts = dts = ~0ull;                       // only the first AU starting in this PES owns its PTS
            start = i;
        }
    }
    if (e.havePartial) e.partial.insert(e.partial.end(), p + start, p + n);
    return true;
}

uint64_t ts(const uint8_t* b)
{
    return ((uint64_t)(b[0] >> 1) & 7) << 30 | (uint64_t)b[1] << 22 | (uint64_t)(b[2] >> 1) << 15 | (uint64_t)b[3] << 7 | (b[4] >> 1);
}

// Demux as much of the pending streams as the rings allow. Returns true when work was done.
bool pump(uint32_t h, Dmux& d)
{
    bool did = false;
    while (!d.streams.empty()) {
        Stream& s = d.streams.front();
        const uint8_t* buf = G(s.addr);
        bool stalled = false;
        while (s.pos + 4 <= s.size) {
            const uint8_t* p = buf + s.pos;
            if (!(p[0] == 0 && p[1] == 0 && p[2] == 1)) { s.pos++; continue; }
            const uint8_t id = p[3];
            if (id == 0xBA) {                                        // pack header
                if (s.pos + 14 > s.size) { s.pos = s.size; break; }
                s.pos += 14 + (p[13] & 7);
                continue;
            }
            if (id == 0xB9) {                                        // program end
                s.pos += 4;
                if (d.cb) t6_queue_call(d.cb, h, msg(2, s.user), d.arg, 0);
                continue;
            }
            if (s.pos + 6 > s.size) { s.pos = s.size; break; }
            const uint32_t len = (uint32_t)p[4] << 8 | p[5];
            const uint32_t end = s.pos + 6 + len;
            if (end > s.size) { s.pos = s.size; break; }
            if (id < 0xBB) { s.pos = end; continue; }
            uint64_t pts = ~0ull, dts = ~0ull;
            const uint8_t* pl = p + 6;
            uint32_t plen = len;
            if (id != 0xBE && id != 0xBF && len >= 3 && (pl[0] & 0xC0) == 0x80) {   // MPEG-2 PES header
                const uint8_t flags = pl[1], hl = pl[2];
                if ((flags & 0x80) && hl >= 5) pts = ts(pl + 3);
                if ((flags & 0x40) && hl >= 10) dts = ts(pl + 8);
                pl += 3 + hl; plen = len >= 3u + hl ? len - 3 - hl : 0;
            }
            for (auto& e : g_es) {
                if (!e.used || e.dmux != h) continue;
                if (id >= 0xE0 && id <= 0xEF && e.major == id) {
                    if (!feedVideo(e, pl, plen, pts, dts)) { stalled = true; break; }
                }
            }
            if (stalled) break;
            s.pos = end;
        }
        if (stalled) break;
        // This SetStream's data is consumed.
        const uint64_t user = s.user;
        d.streams.pop_front();
        if (d.cb) t6_queue_call(d.cb, h, msg(0, user), d.arg, 0);  // DEMUX_DONE
        did = true;
    }
    return did;
}

void hDmuxQueryAttr(ppu_context* ctx) { const uint32_t a = (uint32_t)ctx->gpr[4]; if (a) { wr32(a, 0x10000); wr32(a + 4, 1); wr32(a + 8, 0); } RET(0); }
void hDmuxQueryEsAttr(ppu_context* ctx) { const uint32_t a = (uint32_t)ctx->gpr[6]; if (a) wr32(a, 0x10000); RET(0); }

void hDmuxOpen(ppu_context* ctx)               // (type*, res*, cb*, handle*)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
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
    RET(kErrDmuxFatal);
}
void hDmuxClose(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    const uint32_t h = (uint32_t)ctx->gpr[3];
    if (Dmux* d = dmuxOf(h)) {
        d->used = false; d->streams.clear();
        for (auto& e : g_es) if (e.used && e.dmux == h) e = Es{};
    }
    RET(0);
}
void hDmuxSetStream(ppu_context* ctx)          // (handle, addr, size, discontinuity, userData)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    const uint32_t h = (uint32_t)ctx->gpr[3];
    Dmux* d = dmuxOf(h);
    if (!d) { RET(kErrDmuxArg); return; }
    if (!d->streams.empty()) { RET(0x80610203u); return; }    // CELL_DMUX_ERROR_BUSY
    d->streams.push_back({(uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], 0, ctx->gpr[7]});
    RET(0);
}
void hDmuxResetStream(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    if (Dmux* d = dmuxOf((uint32_t)ctx->gpr[3])) d->streams.clear();
    RET(0);
}
void hDmuxEnableEs(ppu_context* ctx)           // (handle, filterId*, esRes*, esCb*, specificInfo, esHandle*)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    const uint32_t h = (uint32_t)ctx->gpr[3], fid = (uint32_t)ctx->gpr[4], cb = (uint32_t)ctx->gpr[6], out = (uint32_t)ctx->gpr[8];
    if (!dmuxOf(h)) { RET(kErrDmuxArg); return; }
    for (int i = 0; i < kMaxEs; i++)
        if (!g_es[i].used) {
            Es& e = g_es[i];
            e = Es{};
            e.used = true; e.slot = i; e.dmux = h; e.handle = 0x100 + i;
            if (fid) { e.major = rd32(fid); e.minor = rd32(fid + 4); }
            if (cb) { e.cb = rd32(cb); e.arg = rd32(cb + 4); }
            if (out) wr32(out, e.handle);
            log("EnableEs dmux %u filter %02X/%02X -> %X", h, e.major, e.minor, e.handle);
            RET(0);
            return;
        }
    RET(kErrDmuxFatal);
}
void hDmuxDisableEs(ppu_context* ctx) { std::lock_guard<std::recursive_mutex> l(g_mx); if (Es* e = esOf((uint32_t)ctx->gpr[3])) *e = Es{}; RET(0); }
void hDmuxResetEs(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    if (Es* e = esOf((uint32_t)ctx->gpr[3])) { e->ready.clear(); e->taken.clear(); e->partial.clear(); e->havePartial = false; e->head = 0; }
    RET(0);
}
// (esHandle, CellDmuxAuInfo** info, void** specific) -- Get pops, Peek doesn't.
void getAu(ppu_context* ctx, bool pop)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Es* e = esOf((uint32_t)ctx->gpr[3]);
    if (!e) { RET(kErrDmuxArg); return; }
    if (e->ready.empty()) { RET(kErrDmuxEmpty); return; }
    const Au au = e->ready.front();
    const uint32_t info = kInfoBase + ((uint32_t)e->slot * kInfoSlots + (e->infoNext++ % kInfoSlots)) * 48;
    memset(G(info), 0, 48);
    wr32(info, au.addr); wr32(info + 4, au.size); wr32(info + 8, au.size);
    G(info + 12)[0] = au.rap ? 1 : 0;
    wr64(info + 16, au.user);
    wr32(info + 24, au.pts == ~0ull ? 0xFFFFFFFFu : (uint32_t)(au.pts >> 32)); wr32(info + 28, au.pts == ~0ull ? 0xFFFFFFFFu : (uint32_t)au.pts);
    wr32(info + 32, au.dts == ~0ull ? 0xFFFFFFFFu : (uint32_t)(au.dts >> 32)); wr32(info + 36, au.dts == ~0ull ? 0xFFFFFFFFu : (uint32_t)au.dts);
    if (ctx->gpr[4]) wr32((uint32_t)ctx->gpr[4], info);
    if (ctx->gpr[5]) wr32((uint32_t)ctx->gpr[5], 0);
    if (pop) { e->taken.push_back(au); e->ready.pop_front(); }
    RET(0);
}
void hDmuxGetAu(ppu_context* ctx) { getAu(ctx, true); }
void hDmuxPeekAu(ppu_context* ctx) { getAu(ctx, false); }
void hDmuxReleaseAu(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Es* e = esOf((uint32_t)ctx->gpr[3]);
    if (!e) { RET(kErrDmuxArg); return; }
    if (!e->taken.empty()) e->taken.pop_front();
    else if (!e->ready.empty()) e->ready.pop_front();
    RET(0);
}
void hDmuxFlushEs(ppu_context* ctx)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    Es* e = esOf((uint32_t)ctx->gpr[3]);
    if (!e) { RET(kErrDmuxArg); return; }
    emitAu(*e);                                   // the last AU has no delimiter after it
    if (e->cb) t6_queue_call(e->cb, e->dmux, e->handle, msg(1, 0), e->arg);   // FLUSH_DONE
    RET(0);
}

}  // namespace

// Called from the I/O worker thread every ~1 ms: demux what the rings have room for.
extern "C" void t6_movie_tick(void)
{
    std::lock_guard<std::recursive_mutex> l(g_mx);
    for (uint32_t i = 0; i < 4; i++)
        if (g_dmux[i].used) pump(i + 1, g_dmux[i]);
}

extern "C" void t6_movie_init(void)
{
    if (getenv("T6_NO_MOVIES")) return;          // keep the empty-reader fallback in t6_usio.cpp
    ps3_hle_register_ctx(0xB8436EE5u, "cellPamfReaderInitialize", hReaderInitialize);
    ps3_hle_register_ctx(0xD0230671u, "cellPamfReaderGetNumberOfSpecificStreams", hGetNumberOfSpecificStreams);
    ps3_hle_register_ctx(0x28B4E2C1u, "cellPamfReaderSetStreamWithTypeAndIndex", hSetStreamWithTypeAndIndex);
    ps3_hle_register_ctx(0x67FD273Bu, "cellPamfReaderGetStreamInfo", hGetStreamInfo);
    ps3_hle_register_ctx(0x9AB20793u, "cellPamfReaderGetStreamTypeAndChannel", hGetStreamTypeAndChannel);
    ps3_hle_register_ctx(0x4DE501B1u, "cellPamfReaderGetPresentationStartTime", hStartTime);
    ps3_hle_register_ctx(0xF61609D6u, "cellPamfReaderGetPresentationEndTime", hEndTime);
    ps3_hle_register_ctx(0x01067E22u, "cellPamfStreamTypeToEsFilterId", hStreamTypeToEsFilterId);
    ps3_hle_register_ctx(0xCA8181C1u, "cellPamfGetHeaderSize", hGetHeaderSize);
    ps3_hle_register_ctx(0x44F5C9E3u, "cellPamfGetStreamOffsetAndSize", hGetStreamOffsetAndSize);
    ps3_hle_register_ctx(0xA2D4189Bu, "cellDmuxQueryAttr", hDmuxQueryAttr);
    ps3_hle_register_ctx(0x02170D1Au, "cellDmuxQueryEsAttr", hDmuxQueryEsAttr);
    ps3_hle_register_ctx(0x68492DE9u, "cellDmuxOpen", hDmuxOpen);
    ps3_hle_register_ctx(0x8C692521u, "cellDmuxClose", hDmuxClose);
    ps3_hle_register_ctx(0x04E7499Fu, "cellDmuxSetStream", hDmuxSetStream);
    ps3_hle_register_ctx(0x7B56DC3Fu, "cellDmuxEnableEs", hDmuxEnableEs);
    ps3_hle_register_ctx(0x05371C8Du, "cellDmuxDisableEs", hDmuxDisableEs);
    ps3_hle_register_ctx(0x21D424F0u, "cellDmuxResetEs", hDmuxResetEs);
    ps3_hle_register_ctx(0x42C716B5u, "cellDmuxGetAu", hDmuxGetAu);
    ps3_hle_register_ctx(0x24EA6474u, "cellDmuxReleaseAu", hDmuxReleaseAu);
    ps3_hle_register_ctx(0xEBB3B2BDu, "cellDmuxFlushEs", hDmuxFlushEs);
}

// ---- clocks ----------------------------------------------------------------------------
// cellGcmGetLastFlipTime must be in the same microsecond base as sys_time_get_system_time
// (T6's draw manager times frames against it); the runtime returns raw host nanoseconds.
extern "C" {
int64_t ps3_guest_ticks(int64_t host_ticks);
uint64_t cellGcmGetLastFlipTime(void);
}
namespace {
LARGE_INTEGER g_qpcFreq, g_qpcBase;
void clockInit()
{
    if (!g_qpcFreq.QuadPart) { QueryPerformanceFrequency(&g_qpcFreq); QueryPerformanceCounter(&g_qpcBase); }
}
uint64_t guestUs()
{
    clockInit();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const int64_t g = ps3_guest_ticks(now.QuadPart) - ps3_guest_ticks(g_qpcBase.QuadPart);
    return g > 0 ? (uint64_t)((double)g * 1e6 / (double)g_qpcFreq.QuadPart) : 0;
}
void hSystemTime(ppu_context* ctx) { ctx->gpr[3] = guestUs(); }
void hLastFlipTime(ppu_context* ctx)
{
    clockInit();
    const uint64_t ns = cellGcmGetLastFlipTime();
    const double baseNs = (double)ps3_guest_ticks(g_qpcBase.QuadPart) * 1e9 / (double)g_qpcFreq.QuadPart;
    ctx->gpr[3] = ns > baseNs ? (uint64_t)(((double)ns - baseNs) / 1000.0) : 0;
}
}  // namespace

extern "C" void t6_clock_init(void)
{
    clockInit();
    if (!getenv("T6_FLIP_CLOCK")) return;   // experimental: breaks boot as is
    if (getenv("T6_SYS_CLOCK")) ps3_hle_register_ctx(0x8461E528u, "sys_time_get_system_time", hSystemTime);
    ps3_hle_register_ctx(0x21AC3697u, "cellGcmGetLastFlipTime", hLastFlipTime);
}
