// Native replacements for lifted T6 functions (lifted with --hook <addr>; the lifted
// bodies stay available as func_<addr>_lifted).
//
// USB key: the arcade checks a dongle-specific ID file (16 bytes, triple-DES) and a region
// key file on the USB stick. Those files are not part of the dump, so the two checks are
// answered here instead of reimplementing the cipher:
//   0x0015EB20  key loader: copy usbid into the key object, decrypt <idpath>.KEY -> region
//   0x0015EE78  "decrypted ID matches usbid" (strncmp, 12 chars)

#include "ppu_recomp.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" uint8_t* vm_base;

static uint8_t* G(uint32_t ea) { return vm_base + ea; }

// 1 = JAPAN, 2 = OVERSEAS, 3 = KOREA. T6_REGION overrides (set by t6.exe from t6.ini).
static uint32_t region()
{
    const char* e = getenv("T6_REGION");
    const int r = e ? atoi(e) : 1;
    return (r >= 1 && r <= 3) ? (uint32_t)r : 1u;
}

// void key_load(key_obj* this, const char* usbid, const char* idpath)
void func_0015EB20(ppu_context* ctx)
{
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    const uint32_t usbid = (uint32_t)ctx->gpr[4];
    char* dst = (char*)G(obj);
    memset(dst, 0, 13);
    if (usbid) strncpy(dst, (const char*)G(usbid), 12);   // snprintf(obj, 13, "%s", usbid)
    const uint32_t r = region();
    uint8_t* p = G(obj + 0x20);                             // region, big-endian u32
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = (uint8_t)r;
}

// bool id_matches(const char* usbid, const char* decrypted)
void func_0015EE78(ppu_context* ctx)
{
    ctx->gpr[3] = 1;
}
