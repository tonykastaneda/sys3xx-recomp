// Tekken 6 (arcade, Namco System 357). Everything core/ needs to know about this game.
// The build includes the profile of the game it is building (tools/build.sh t6).
#pragma once

#define GAME_ID          "t6"
#define GAME_EXE         "t6"                    /* t6.exe, t6.ini, t6.log */
#define GAME_EXE_W       L"t6"
#define GAME_TITLE       "TEKKEN 6"              /* window title */
#define GAME_NAME        "Tekken 6 (arcade)"
#define GAME_NAME_W      L"Tekken 6 (arcade)"

/* dev_hdd0/game/SCEEXE000/USRDIR/<GAME_SELF>.SELF, extracted to cache/<GAME_SELF>.elf */
#define GAME_SELF        "GAME_113"

/* Boot arguments (arg table at GAME_113 0x11ea7c; see NOTES.md). */
#define GAME_ARGV        "ver=105;romver=B23;livemon=0;usbid=76C0D0000000;usbnum=1;" \
                         "idpath=/dev_usb000/T6.ID;datainfo=DINF_106.BIN"

/* USB key: the serial the emulated stick reports, and the ID file it carries
 * (/dev_usb000 maps to the game root; any 16 bytes, patches.cpp skips the check). */
#define GAME_USB_SERIAL  "76C0D0000000"
#define GAME_ID_FILE     "T6.ID"

/* 4:3: the camera's 16/9 and the 16:9 entry of the screen-mode table (both set to 4/3). */
#define GAME_ASPECT_EAS  0x72B194u, 0x72E090u

/* Runtime switches this game needs, as {"NAME", "value"}, pairs (an env var set by the
 * user wins). None for T6. */
#define GAME_ENV_DEFAULTS
