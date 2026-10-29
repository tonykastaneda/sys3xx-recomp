# Tekken 6 Arcade Recomp (local)

Static recompilation of the arcade Tekken 6 (Namco System 357, PS3-based) to a
native Windows executable, built on [ps3recomp](https://github.com/soyjxck/ps3recomp)
and modelled on [dod3-recomp](https://github.com/soyjxck/dod3-recomp).

Local-only project. No game files belong in this repository.

## Layout

| Path | What it is |
|---|---|
| `ps3recomp/` | ps3recomp toolkit (submodule, the dod3-recomp fork at the commit dod3 uses) |
| `third_party/zlib/` | zlib 1.3.1 (submodule), built from source instead of vcpkg |
| `reference/dod3-recomp/` | Drakengard 3 port, used as the template (ignored, cloned separately) |
| `reference/ps3recomp-upstream/` | Upstream ps3recomp, for comparison (ignored) |
| `.venv/` | Python tools for lifting PPU/SPU code (ignored) |

## Toolchain (kept light on purpose)

- LLVM-MinGW (scoop `mingw-mstorsjo-llvm-msvcrt`) instead of Visual Studio + clang-cl
- CMake, Ninja, Python (scoop)
- No vcpkg: zlib is built from source; SDL2 is skipped (controllers use XInput)
- RPCS3 + PS3 firmware (scoop) for studying the arcade game, not for running the port

dod3-recomp targets MSVC, so its Windows build needs porting to MinGW
(structured exception handling in 5 files, `_MSC_VER` checks, CMake changes).

## Setup

```sh
git submodule update --init
python -m venv .venv
.venv/Scripts/pip install -r ps3recomp/requirements.txt
git clone --recurse-submodules https://github.com/soyjxck/dod3-recomp reference/dod3-recomp
git clone https://github.com/sp00nznet/ps3recomp reference/ps3recomp-upstream
```

## Status

- [x] Project and tools set up
- [ ] Arcade dump obtained; main executable decrypted and loaded
- [ ] Run in RPCS3 to see which system libraries and SPU programs the game uses
- [ ] dod3 build ported to LLVM-MinGW
- [ ] PPU and SPU code lifted for Tekken 6
- [ ] Arcade I/O, coin/card and security checks replaced with native code
