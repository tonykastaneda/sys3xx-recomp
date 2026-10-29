// Native replacements for lifted Bloodline Rebellion (GAME_208) functions (lifted with
// --hook <addr>; the lifted bodies stay available as func_<addr>_lifted).
//
// USB key: like T6, BR checks a dongle-specific ID file (16 bytes, triple-DES) and a region
// key file (<idpath> with the last 3 chars -> "KEY") on the USB stick. Neither file is in
// the dump, so the checks are answered here instead of reimplementing the cipher:
//   0x0014C2C8  key loader: copies usbid into the key object, decrypts the region file
//               (JAPAN.../OVERSEAS.../KOREA... -> obj+0x40 = 1/2/3)
//   0x0014BF90  "decrypted ID matches usbid"
// BR's usbid must start "02645" (T6: "76C0D"); the 6th digit is checked against the
// region: 0/1 = Japan, 2 = overseas, 3 = Korea (funcs 0x14C238 / 0x14C120 / 0x14C050).

#include "ppu_recomp.h"
#include <cstdint>
#include <cstdlib>

extern "C" uint8_t* vm_base;

// 1 = JAPAN, 2 = OVERSEAS, 3 = KOREA. T6_REGION overrides (set by t6.exe from t6.ini).
static uint32_t region()
{
    const char* e = getenv("T6_REGION");
    const int r = e ? atoi(e) : 1;
    return (r >= 1 && r <= 3) ? (uint32_t)r : 1u;
}

extern void func_0014C2C8_lifted(ppu_context* ctx);

// void key_load(key_obj* this, const char* usbid, const char* idpath)
void func_0014C2C8(ppu_context* ctx)
{
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    func_0014C2C8_lifted(ctx);   // formats the usbid fields; the region file is missing
    uint8_t* p = vm_base + obj + 0x40;   // region, big-endian u32
    const uint32_t r = region();
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = (uint8_t)r;
}

// bool id_matches(key_obj*, const char* decrypted)
void func_0014BF90(ppu_context* ctx)
{
    ctx->gpr[3] = 1;
}
