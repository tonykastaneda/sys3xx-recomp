# T6 arcade recomp: working notes

Goal: arcade Tekken 6 (Namco System 357, ROM from TeknoParrot pack) statically recompiled and running natively on Windows,
plus an external config app (P1/P2 button mapping; resolution Original/720p/1080p/1440p and 4:3 variants).

Inputs
- ROM: ~/Downloads/Tekken 6 (2007)[Namco System 357][TP].zip (dev_hdd0/game/SCEEXE000, BOOT*.SELF + DATA*.BIN)
- TeknoParrot: E:\SYSTEMS\TEKKNOPAROT (has RPCS3 fork + GameProfiles/Tekken6.xml) - reference for keys, I/O emulation
- Toolchain: ps3recomp (dod3 fork) submodule, LLVM-MinGW, REA MCP
- References: undecuple/project-helghast, whompay/inFAMOUSreborn, soyjxck/dod3-recomp

## Deliverables (user, 2026-10-29)
- `t6.exe`: launches the game properly (native recomp).
- `config.exe`: all options (P1/P2 buttons; resolution Original/720p/1080p/1440p + 4:3 variants), writes a config file t6.exe reads.

## Plan
- [x] Extract ROM (game/ in repo, ignored). TP launches USRDIR/EBOOT.BIN (= loader). Chain: EBOOT/BOOT.SELF -> BOOT_xxx.SELF -> GAME_xxx.SELF per VER.105
- [ ] Baseline in TP RPCS3 (booted BOOT.SELF via CLI; precompiling; GAME_100 loaded)
- [x] SELF -> ELF: all are *debug* SELFs (key rev 0x8000) with the original ELF verbatim at header_len; tools/dself2elf.py. Target elf/GAME_113.elf
- [ ] Study helghast / inFAMOUSreborn / dod3 pipelines; pick approach
- [ ] Port ps3recomp build to MinGW (or install needed toolchain if unavoidable)
- [ ] Lift PPU code, build runtime, stub arcade I/O + security (follow TP's 357 handling)
- [ ] Boot to first frame natively
- [ ] Input mapping (P1/P2) + config file
- [ ] RE render/video path: backbuffer size, viewport, aspect -> 720/1080/1440 + 4:3
- [ ] Config UI app (writes config file the game reads)

## Decisions log
- 2026-10-29: Target = GAME_113.SELF (final, romver=B23, cabinet type Station, args `romver=B23 livemon=0`, datainfo=DINF_106.BIN). Boot program for Station = BOOT_202.
- 2026-10-29: Firmware 4.93 installed into TP RPCS3 (was missing) from scoop ps3-system-software PUP.
- 2026-10-29: Desktop-wide screenshots come back black (headless Apollo virtual display); use .claude/hooks/win.ps1 (PrintWindow) for window shots.

## Facts: GAME_113.elf
- entry 0x704110 (OPD). .text 0x10240..0x661898 (6.4 MB). .rodata 0x10000000, .data 0x100c0000, .bss 0x10127600 (5.6 MB).
- .spu_image 0x1005e280 (0x54380) + 11 SPURS job binaries in .bin_objs.jobElement/* (SendEvent, njSnowField*, njParticleSpu/*, njCalcBone, kkMob).
- 43 sections, symbols stripped (no .symtab), .eh_frame present (good for function bounds).
