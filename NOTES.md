# Notes (Bloodline Rebellion)

Started from t6-arcade-recomp (port/, tools/, config/, ps3recomp branch t6). What changed for BR:

- 2026-10-29: GAME_208.elf: entry 0x6d6f18, .text 0x10230+0x58c128, stubs end 0x59e87c,
  TOC 0x7069d0. Data segment VA 0x6c0000 = file 0x6b0000. 24 SPURS job binaries in named
  `.bin_objs.<name>` sections (kkMob, njCalcBone, nuClling, VertexBlendSkinJob, inflate_zlib, ...),
  same job format as T6. tools/lift_spu.sh wraps every such section.
- Boot args: same names as T6. ver=206 (VER.206), romver=B23, datainfo=DINF_004.BIN.
- USB key: usbid must start "02645" (T6: "76C0D"); the 6th digit must match the region from the
  key file: 0/1 Japan (0x14C238), 2 overseas (0x14C120), 3 Korea (0x14C050). Hooks in
  port/t6_patches.cpp: 0x14C2C8 key loader (lifted body, then region at obj+0x40) and 0x14BF90
  ID compare -> match. Failure path was exit(0) -> sys_prx_exitspawn_with_level (no-op here),
  then null writes.
- SPURS: BR creates each job chain as a single NEXT-to-itself, runs it once, then appends jobs
  by overwriting that command, with no Run/Kick. ps3recomp cellSpurs now parks the walk on a
  self-loop and a poller restarts it when the word changes.
- I/O board: BR's LDD probe (0x4C0D50) requires idProduct == 0x0900 (T6's took any). Emulated
  USIO now reports 0x0900; bcdDevice stays 0x0910 ("USJ PCB version 0910").
- Guest scratch moved out of RSX VRAM (0xC0000000+), which BR fills: USB descriptors and
  callback stack (0x6F7F0000/0x6F800000), dmux msgs (0x6F820000), movie AU rings/info/msgs
  (0x60000000/0x64000000/0x64800000). The descriptors at 0xCF800000 were being overwritten, so
  the probe saw no device. (T6 has the same latent problem.)
- Movies: BR also calls cellPamfGetHeaderSize2 and cellDmuxGetAuEx (= GetAu). Attract movie plays.
- Round-start freeze (2026-10-29): the game's ring manager (func_00405CD8, object 0x43E10780:
  flags A/B/C at +0x200/+0x280/+0x300, ring chain at +0x80, send-event job at +0x580) waits on
  the A/B flag of the next ring half. Restarting the walk the instant the parked word changed
  raced the PPU's multi-word update, the walk parked where no work came, and flag B was never
  set. The poller now waits 200 us before restarting: 4/4 runs fine (was 2/2 frozen). Logging
  (SPURS_SPIN_LOG) also hid it. Coin-up -> char select -> fights works; ~56-58 fps in fights.
- Open: sound (bnusCore on a SPURS taskset + cellSpursQueue; cellSpursQueuePopBody 0x91066667
  missing; task image 1 branches into unlifted LS 0x15088), null call in PS3AIO init
  (func_00470FE8), unresolved sysPrxForUser 0xE76964F5, aspect constants for 4:3, config/package.
