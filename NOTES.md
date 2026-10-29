# T6 arcade recomp: working notes

Goal: arcade Tekken 6 (Namco System 357, ROM from TeknoParrot pack) statically recompiled and running natively on Windows,
plus an external config app (P1/P2 button mapping; resolution Original/720p/1080p/1440p and 4:3 variants).

Inputs
- ROM: ~/Downloads/Tekken 6 (2007)[Namco System 357][TP].zip (dev_hdd0/game/SCEEXE000, BOOT*.SELF + DATA*.BIN)
- TeknoParrot: E:\SYSTEMS\TEKKNOPAROT (has RPCS3 fork + GameProfiles/Tekken6.xml) - reference for keys, I/O emulation
- Toolchain: ps3recomp (dod3 fork) submodule, LLVM-MinGW, REA MCP
- References: undecuple/project-helghast, whompay/inFAMOUSreborn, soyjxck/dod3-recomp

## Plan
- [ ] Extract ROM, identify which BOOT SELF TeknoParrot launches, confirm it runs in TP's RPCS3 (baseline + screenshots)
- [ ] Decrypt SELF -> ELF, inventory modules/imports/SPU
- [ ] Study helghast / inFAMOUSreborn / dod3 pipelines; pick approach
- [ ] Port ps3recomp build to MinGW (or install needed toolchain if unavoidable)
- [ ] Lift PPU code, build runtime, stub arcade I/O + security (follow TP's 357 handling)
- [ ] Boot to first frame natively
- [ ] Input mapping (P1/P2) + config file
- [ ] RE render/video path: backbuffer size, viewport, aspect -> 720/1080/1440 + 4:3
- [ ] Config UI app (writes config file the game reads)

## Decisions log
