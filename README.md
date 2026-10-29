# t6br-arcade-recomp

Static recompilation of arcade **Tekken 6: Bloodline Rebellion** (System 357, GAME_208) to
native Windows, built on the [t6-arcade-recomp](../t6-arcade-recomp) port of Tekken 6 and the
ps3recomp runtime (submodule, local branch `t6br`).

No game data is in this repo. `game/` holds your own TeknoParrot dump (gitignored).

## Status

- Boots past the USB key check, links the I/O board, plays the attract movie with INSERT COIN,
  about 60 fps.
- Not done yet: coin-up into fights, sound (BR mixes on a SPURS taskset instead of T6's raw SPU
  thread), 4:3 aspect, config.exe and packaging.

## Build

```
tools/dself2elf.py  <dump>/GAME_208.SELF elf/GAME_208.elf   # debug SELF -> ELF
tools/lift.sh                                              # PPU -> recompiled_208/
tools/lift_spu.sh                                          # SPU images + 24 SPURS jobs -> spu/
tools/build.sh                                             # clang-cl + xwin -> build/t6.exe
tools/run.sh 60                                            # dev run, log in out/run/latest.log
```

See NOTES.md for the decisions log.
