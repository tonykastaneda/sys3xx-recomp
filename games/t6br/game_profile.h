// Tekken 6: Bloodline Rebellion (arcade, Namco System 357). Everything core/ needs to know
// about this game. The build includes the profile of the game it is building.
#pragma once

#define GAME_ID          "t6br"
#define GAME_EXE         "t6br"                  /* t6br.exe, t6br.ini, t6br.log */
#define GAME_EXE_W       L"t6br"
#define GAME_TITLE       "TEKKEN 6 BLOODLINE REBELLION"
#define GAME_NAME        "Tekken 6 Bloodline Rebellion (arcade)"
#define GAME_NAME_W      L"Tekken 6 Bloodline Rebellion (arcade)"

/* dev_hdd0/game/SCEEXE000/USRDIR/<GAME_SELF>.SELF, extracted to cache/<GAME_SELF>.elf */
#define GAME_SELF        "GAME_208"

#define GAME_ARGV        "ver=206;romver=B23;livemon=0;usbid=026450000000;usbnum=1;" \
                         "idpath=/dev_usb000/T6.ID;datainfo=DINF_004.BIN"

/* BR's usbid must start "02645"; the 6th digit selects the region check (patches.cpp). */
#define GAME_USB_SERIAL  "026450000000"
#define GAME_ID_FILE     "T6.ID"

/* 4:3: camera aspect and screen-mode table entry (matched by the bytes around T6's). */
#define GAME_ASPECT_EAS  0x7016F8u, 0x7042ECu

/* BR's sound core (bnusCore) runs as a SPURS task fed through CellSpursQueue: the queue
 * header layout its SPU library uses, the PPU push, and no job-chain completion events
 * on the queues it attached (they reached its decoder thread). */
#define GAME_ENV_DEFAULTS { "SPURS_QUEUE_LAYOUT", "2" }, { "SPURS_QUEUE_PUSH", "1" }, \
                          { "SPURS_JC_DONE_EVENTS", "0" },
