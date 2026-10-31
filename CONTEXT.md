# How sys3xx-recomp works

This file explains the moving parts: what each layer does, how a game goes from a dump to an
exe, the mechanisms that took the most work to get right, and the tools for debugging. Read it
before changing the runtime or starting a new game. The README covers the commands;
`games/<id>/NOTES.md` covers the details of each game.

## The idea

A Namco System 357/369 board is a PS3 with an arcade I/O board, a USB dongle and an operator
menu. The game is an ordinary PS3 executable (a SELF). Instead of emulating the PS3, we:

1. extract the ELF from the SELF in the TeknoParrot dump,
2. translate every PowerPC (PPU) function into C, and every SPU program the game runs into C,
3. compile that C together with a runtime that implements the PS3 OS (lv2), its libraries
   (SPURS, cellGcm, audio, file system, codecs) and the RSX GPU on top of D3D12,
4. add the arcade layer: the I/O board, the USB key, the operator settings, the boot arguments.

The result is one native Windows exe per game. Guest memory is a flat 4 GB reservation and
guest addresses are offsets into it (big-endian, so every load and store byte-swaps).

## Three layers

| Layer | Folder | Shared by | What lives there |
|---|---|---|---|
| Runtime | `ps3recomp/` (submodule, branch `sys3xx`) | every PS3 game | PPU/SPU lifters, lv2 syscalls, HLE libraries, SPURS, the RSX renderer, FFmpeg codecs |
| Arcade core | `core/` | every System 3xx game | launcher, USIO I/O board over cellUsbd, movie player (cellPamf/cellDmux), settings/ini, main loop, profiler |
| Game | `games/<id>/` | one game | profile, script settings, native patches, notes |

Rule of thumb: a fix goes in the lowest layer it is true for. A SPURS bug goes into ps3recomp
even if only one game hits it. If a game needs a fix that would break another game, put it
behind an env switch and turn the switch on in that game's `GAME_ENV_DEFAULTS`.

## From dump to exe

```
dumps/<id>/dev_hdd0/.../GAME_xxx.SELF
   │ tools/game.sh need_elf → tools/dself2elf.py
   ▼
work/<id>/elf/GAME_xxx.elf
   │ tools/lift.sh: ppu_loader.py (function list + imports) → ppu_lifter.py
   ▼
work/<id>/recompiled/*.c          one C function per PPU function, 16 chunks
   │ tools/lift_spu.sh: extract_spu_images, wrap_spu_elf, build_spu_workloads
   ▼
work/<id>/spu/                    SPU thread images + SPURS job binaries as C, plus a registry
   │ tools/build.sh: cmake -S core -DGAME=<id> (clang-cl + xwin, Ninja)
   ▼
work/<id>/build/<exe>.exe, <exe>dbg.exe, config.exe
   │ tools/package.sh
   ▼
dist/<id>/                        exe + config.exe + FFmpeg DLLs + README + <exe>.ini
```

Everything under `work/` is generated and can be deleted. A lift only needs redoing when the
lifter, `CODE_END` or `LIFT_HOOKS` change; most work is a rebuild only.

**Lifting.** `ppu_lifter.py` turns each function into C that operates on a register context.
`--code-end` stops it at the end of the game's code (past it is data that looks like code).
`--hook <addr>` replaces a function with one supplied natively, from `games/<id>/patches.cpp`.
Imports (NIDs) become calls to the runtime's HLE implementations; an unimplemented NID logs
once and returns 0.

**SPU code.** Games run SPU code two ways: raw SPU threads (an ELF image loaded into an SPU) and
SPURS (Sony's SPU job/task scheduler). Thread images are found in the ELF and lifted whole.
SPURS jobs are small binaries the game points at by address, so each game lists them in
`game.env`: `SPU_JOBS` as `va:size` pairs (T6) or as ELF sections (BR's `.bin_objs`).
`SPU_TABLE_ENTRIES=1` also lifts entries found through jump tables that normal flow analysis
misses (BR's sound needed this).

## A game profile

`games/<id>/game_profile.h` (compiled in):

| Key | Example (t6br) | Meaning |
|---|---|---|
| `GAME_EXE` | `t6br` | exe, ini and log name |
| `GAME_TITLE` | `TEKKEN 6 BLOODLINE REBELLION` | window title (`<title> - 00.00 FPS`) |
| `GAME_SELF` | `GAME_208` | the SELF in the dump |
| `GAME_ARGV` | `ver=206;romver=B23;…;usbid=026450000000;…` | the boot arguments the arcade BIOS passes |
| `GAME_USB_SERIAL` | `026450000000` | serial the emulated USB key reports |
| `GAME_ID_FILE` | `T6.ID` | key file the game reads from `/dev_usb000` |
| `GAME_ASPECT_EAS` | `0x7016F8u, 0x7042ECu` | float constants patched for the 4:3 option |
| `GAME_ENV_DEFAULTS` | `{ "SPURS_QUEUE_LAYOUT", "2" }, …` | runtime switches this game needs (the user's env still wins) |

`games/<id>/game.env` (for the scripts): `GAME_EXE`, `GAME_SELF`, `CODE_END`, `LIFT_HOOKS`,
`SPU_JOBS`, `SPU_TABLE_ENTRIES`, `BENCH_INPUT`.

`games/<id>/local.env` (yours, gitignored): `GAME_DUMP` (the folder holding `dev_hdd0`) and
optionally `DEV_FLASH` (an RPCS3 `dev_flash`, for the system fonts).

## Boot

`core/launch.cpp` runs first. It sets `PS3_TITLE`, `PS3_BOOT_PATH`, `PS3_ARGV`,
`SPURS_WATCHDOG=1` and the game's `GAME_ENV_DEFAULTS` (without overriding anything already set).
It reads `<exe>.ini`, finds the dump (`game/` next to the exe, or `[Paths] Game=`), extracts the
ELF into `cache/` if needed, opens `<exe>.log`, then hands over to the runtime, which maps the
ELF, starts the PPU main thread at the entry point and creates the window.

Closing the window ends the process, audio included. The window title shows the measured
frame rate once a second.

## The arcade parts

**USB key.** The game reads an ID file from the dongle and checks it against `usbid` from the
boot arguments, then against the key's USB serial. The checks differ per game, so each game
hooks its own loader and compare functions (`LIFT_HOOKS` + `patches.cpp`). BR's serial must
start with `02645`; with the wrong prefix the game calls `exitspawn` and quits.

**I/O board (USIO).** `core/usio.cpp` emulates Namco's USIO board behind cellUsbd: the game
probes the USB bus, finds VID 0x0B9A / PID 0x0900, and then polls switches, coins and analog
inputs through bulk transfers. Keyboard and XInput are mapped onto it (config.exe sets the
bindings). The game's operator data lives in the board's backup memory, saved as
`usio_backup.bin` next to the exe.

**Movies.** Attract and intro movies are PAMF files (AVC video + ATRAC3plus audio).
`core/movie.cpp` implements cellPamf/cellDmux natively, and the runtime's cellVdec decodes
with FFmpeg (avcodec/avutil/swresample DLLs). Games use different entry points: BR also
needs `hGetHeaderSize2` (NID 0x90FC9A59) and `cellDmuxGetAuEx` (0x2C9A5857). Without them, BR
runs with no sound. Movie audio is not played yet.

**4:3.** The game hard-codes its aspect ratio as float constants; the 4:3 option rewrites
them (`GAME_ASPECT_EAS`) and the presenter letterboxes.

## Mechanisms that took the most work

These are the parts most likely to break a new game. Each has an env switch, so a game can
opt in or out.

**SPURS job chains.** A job chain is a list of commands (RUN job, NEXT, SYNC, GUARD, END) that
SPUs walk. The runtime runs jobs on host worker threads. Games often park a chain on a `NEXT`
that points to itself and later rewrite it to start new work. Instead of spinning on it, the
runtime parks the chain and a poller re-checks it (self-loop parking). After the PPU restarts
a walk, the runtime waits 200 µs to let the PPU finish writing the list; without that, BR froze
at round start.

**Job guards and event flags.** BR's job ring (`func_00405CD8`) splits its work into two halves.
It tracks progress with three event flags at obj+0x200/0x280/0x300 and two guards at
obj+0x380/0x400, which the PPU notifies after resetting a half. Waking guards in the wrong order
deadlocks. `SPURS_WATCHDOG=1` detects a stalled chain and kicks it. A rare mid-fight freeze
(about 1 in 20 fights) still exists in BR; the watchdog recovers most of them.

**SPURS queues.** BR's sound engine (bnusCore) runs as a SPURS task fed through CellSpursQueue.
Its queue struct uses a different layout from the one the runtime assumed: size at +0x08,
depth at +0x0C, buffer at +0x10. That is `SPURS_QUEUE_LAYOUT=2`; `SPURS_QUEUE_PUSH=1` makes
pushes wake the task. `SPURS_JC_DONE_EVENTS=0` stops job-chain completion events from being
broadcast to a port BR's decoder thread also listens on.

**The RSX renderer.** The game writes RSX command buffers exactly as on a PS3. The runtime
decodes them into a register file and a draw engine records draws for D3D12. Surfaces
(render targets) live in guest VRAM addresses and are tracked by address.
- *NV3089 blits.* Games copy and scale surfaces with the NV3089 scaled-image method; BR's motion
  blur copies the scene (0x00B50000 → 0x00E50000) and makes two 512×384 downsamples every frame.
  These blits must also happen on the GPU (`ENG_REC_BLIT`), or the next draw samples stale
  copies: that was BR's ghosting and flicker. When the GPU blit succeeds, the CPU copy is skipped
  (it cost 14% of the frame thread); `RSX_BLIT_CPU_TOO=1` brings it back.
- *Waiting on SPU work.* The GPU must not draw from a buffer the SPUs are still skinning.
  `RSX_WAIT_JOBS` / `RSX_WAIT_JOBS_MODE` control how the frame thread waits, and `RSX_VCHECK`
  counts torn vertex buffers.
- *Scratch memory.* Runtime scratch buffers must stay outside guest VRAM, or the game's
  surfaces overwrite them. They are at 0x6F7F0000, 0x6F800000 and 0x6F820000, with movie rings
  at 0x60000000, 0x64000000 and 0x64800000.

**cellVdec.** FFmpeg's decoder is not reentrant per context. Decoding takes a lock, and
callbacks into the game are deferred until the lock is released. Before that fix, inserting
a coin during a movie crashed with an access violation inside avcodec.

**Frame rate.** Both games target 60 fps. T6 holds 61. BR varies 52-61 by matchup, because some
characters make the frame thread wait longer on SPU skinning. That variance exists on all
builds so far.

## Debugging

Logs: dev runs write to `work/<id>/out/run/latest.log`. The packaged exe writes `<exe>.log` next
to itself. `PS3_VERBOSE=1` makes the runtime chatty (and slow); the scripts default it to 0.

| Tool / switch | Use |
|---|---|
| `tools/run.sh <id> [secs] [log]` | timed dev run |
| `tools/fightbench.sh <id>` | scripted coin → start → fight; prints mean and slowest fps. Use it to measure every perf change. |
| `T6_INPUT_SCRIPT="40:coin1,45:start1,…"` | scripted inputs (`seconds:button`) |
| `T6_FPS_LOG=1` | frametime lines every 5 s |
| `T6_PROF=drain`, `T6_PROF_TOP=N` | sampling profiler for the frame/drain thread |
| `T6_NO_MOVIES=1`, `T6_MOVIE_LOG=1` | skip or trace movies |
| `T6_USB_LOG=1` | trace USIO / USB key traffic |
| `RSX_SURF_DUMP_DIR`, `RSX_SURF_DUMP_FROM`, `RSX_SURF_DUMP_EVERY` | dump render targets to PNG |
| `RSX_DRAW_TRACE_FRAME=N`, `RSX_TRACE_AT_OFF` | full draw trace of one frame (targets, textures, state) |
| `RSX_CLEAR_LOG`, `RSX_PUT_HIST` | clears, and the command-buffer PUT history |
| `SPURS_JC_LOG`, `SPURS_SPIN_LOG`, `SPURS_JOB_STATS` | job-chain walks, spinning chains, per-job timing |
| `SPURS_WATCHDOG=0` | turn the stall watchdog off (to catch the freeze rather than recover) |
| `tools/shot_game.sh <id> out.png [delay]` | screenshot the game window |
| `tools/sym.py`, `tools/xref.py`, `tools/callers.py`, `tools/t6dis.py` | look up, cross-reference and disassemble guest functions (they default to T6 paths) |

When a run dies with no message, start it from PowerShell with `Start-Process -PassThru` and
read the full `ExitCode`: 0xC0000005 is an access violation, which usually points at a native
library (FFmpeg, D3D12) rather than lifted code.

The `T6_` prefix on the core switches and the `t6::` namespace in `core/` are historical; they
apply to every game.

## Adding a game

1. Get the dump in the TeknoParrot layout and set `GAME_DUMP` in `games/<id>/local.env`.
2. Copy `games/t6br/` to `games/<id>/`. Set `GAME_SELF`, `GAME_EXE`, `GAME_TITLE` and
   `GAME_ARGV` from the TeknoParrot profile (or the game's own argument table).
3. `tools/lift.sh <id>`. Set `CODE_END` to the end of the game's text; if the lifter emits
   garbage functions past it, lower it.
4. `tools/lift_spu.sh <id>`: find the SPURS job binaries (search the ELF for `bin_objs`
   sections or job descriptors) and list them in `SPU_JOBS`.
5. `tools/build.sh <id>`, then `tools/run.sh <id>`. Expect it to stop at the USB key check:
   find the ID loader and compare functions, hook them (`LIFT_HOOKS`, `patches.cpp`), and work
   out the serial format.
6. Work through what is left in order: I/O board detection, sound, movies, rendering. Look in
   `games/t6/NOTES.md` and `games/t6br/NOTES.md`: most problems have happened before.
7. Write `games/<id>/NOTES.md` as you go, and add the game to the README table.

The light-gun games (Razing Storm, Deadstorm Pirates, Dark Escape 4D) and the drum games
(Taiko, Wadaiko Master) use different I/O hardware, which `core/usio.cpp` does not emulate yet.
