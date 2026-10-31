# sys3xx-recomp

Native Windows ports of **Namco System 357 / 369** arcade games (the PS3-based boards), made by
static recompilation: the game's PPU and SPU code is lifted to C/C++ and built against the
[ps3recomp](https://github.com/soyjxck/ps3recomp) runtime, plus a layer for what the arcade
board adds around the PS3 (I/O board, USB key, operator settings, the TeknoParrot dump layout).

No game data is included or downloaded. You need your own dump of each game.

## Games

| Game | id | Status |
|---|---|---|
| Tekken 6 | `t6` | Playable: attract, movies, coin-up, fights at ~61 fps, sound, 4:3, config tool |
| Tekken 6: Bloodline Rebellion | `t6br` | Playable: attract, movies, fights at 52-61 fps, sound, motion blur, 4:3, config tool |
| Tekken Tag Tournament 2 / Unlimited | | Not started |
| Dragon Ball Zenkai Battle Royale | | Not started |
| Razing Storm, Deadstorm Pirates Special, Dark Escape 4D, Sailor Zombie | | Not started (light-gun I/O) |
| Taiko no Tatsujin (all colours), Wadaiko Master | | Not started (drum I/O) |

Per-game details, decisions and known issues: `games/<id>/NOTES.md` and `games/<id>/README.txt`.

## Layout

```
ps3recomp/        the PS3 runtime (vendored copy, see Credits): recompilers, lv2, SPURS, RSX, codecs
core/             the arcade layer, shared by every game: launcher, I/O board (USIO/cellUsbd),
                  movies (cellPamf/cellDmux), settings, main loop, profiler
config/           config.exe (resolution, 4:3, controls, controllers)
games/<id>/       one profile per game:
  game_profile.h    exe, title, SELF, boot args, USB serial, 4:3 addresses, runtime switches
  game.env          what the scripts need: code end, lift hooks, SPU job source, bench input
  patches.cpp       native replacements for a few lifted functions (USB key checks)
  local.env         (yours, not committed) GAME_DUMP=<folder that holds dev_hdd0>
tools/            per-game scripts: lift, lift_spu, build, run, fightbench, package
work/<id>/        (generated) extracted ELF, lifted code, build, logs
```

## Building a game

Windows, with clang-cl + [xwin](https://github.com/Jake-Shadle/xwin) (no Visual Studio), CMake,
Ninja, Python 3, and the FFmpeg shared build (for movies). Paths are set in `tools/build.sh`.

```
git clone --recursive <this repo>
python -m venv .venv
echo GAME_DUMP="D:/dumps/Tekken6" > games/t6/local.env    # the folder with dev_hdd0

tools/lift.sh t6         # PPU -> work/t6/recompiled   (extracts the ELF from the dump first)
tools/lift_spu.sh t6     # SPU thread images + SPURS jobs -> work/t6/spu
tools/build.sh t6        # -> work/t6/build/t6.exe + config.exe
tools/run.sh t6 60       # dev run, log in work/t6/out/run/latest.log
tools/package.sh t6      # -> dist/t6/: t6.exe, config.exe, FFmpeg DLLs, README
```

The packaged `<id>.exe` needs no arguments: it finds the dump (a `game` folder next to it, or
`[Paths] Game=` in `<id>.ini`), extracts the ELF into `cache/`, and starts.

## Adding a game

Copy a profile (`games/t6br/` is the most complete), then fill it in as you go:
the SELF name and boot args (from the TeknoParrot profile or the game's argument table), the
code end and any USB-key hooks (`game.env`, `patches.cpp`), the SPURS job binaries
(`SPU_JOBS`), and runtime switches the game needs (`GAME_ENV_DEFAULTS`). The notes of the two
Tekken 6 ports record how each of those was found.

## Credits and license

`ps3recomp/` is a copy of [ps3recomp](https://github.com/soyjxck/ps3recomp) by Ned Heller
(sp00nznet), soyjxck and its other contributors, taken at upstream commit `018024d`, plus the
changes this project needed (SPURS job-chain parking and queues, GPU-side NV3089 blits, the
cellVdec lock, the SPURS watchdog). It stays under its own MIT license, `ps3recomp/LICENSE`.

The I/O board details and several library function names come from
[RPCS3](https://github.com/RPCS3/rpcs3) (GPLv2). Everything else in this repository is
released under the GNU General Public License v2, see `LICENSE`.
