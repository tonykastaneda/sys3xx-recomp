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
- Sound (2026-10-29): bnusCore is a SPURS task (image spu_0000, entry 0x3070) with a PPU->SPU
  command queue (q 0x420CB080) and an SPU->PPU reply queue (0x420CAC80), 16-byte elements.
  Fixes: (1) its command handlers are reached through tables in the image's data segment, so
  lift_spu.sh passes code addresses found there as --extra-funcs (0x15088 was the first miss);
  (2) SPURS_JC_DONE_EVENTS=0 -- the runtime broadcast job-chain completions to every queue
  attached to SPURS, and the decoder server thread read them as its own events (null call in
  func_00470FE8); (3) SPURS_QUEUE_LAYOUT=2 -- BR's SPU library reads size at +0x08 and the
  buffer at +0x10; (4) SPURS_QUEUE_PUSH=1 and a new cellSpursQueuePopBody. The PPU sends "STAT"
  and gets "END "; music and effects come out from character select on (AUDIO_PEAK, AUDIO_WAV).
  All three env vars are launcher defaults.
- 4:3 (2026-10-29): camera aspect 0x7016F8 and screen-mode entry 0x7042EC (matched by the
  bytes around T6's). Package: tools/package.sh -> dist/T6BR (verified a launcher-started fight).
- Coin-during-movie "hang" (2026-10-30): actually a crash (exit 0xC0000005 in avcodec, found
  with out/exitcode.ps1 which keeps the full exit code). cellVdecGetPicture decodes held-back
  AUs on the picture thread while the player's EndSeq/Close flush or free the FFmpeg context on
  another. Fix: one lock over the cellVdec entry points, guest callbacks deferred to after the
  outermost unlock. 25/25 runs clean (was ~1 in 13).
- Ghosting + flicker (2026-10-30): BR draws a velocity buffer (0x01690000, its own Z/stencil
  at 0x01C90000) and blurs the scene into s5 (0x00E50000) with a stencil-limited pass
  (fp d63844612a4fced4). Outside the characters s5 must already hold the scene: the game blits
  it there every frame with NV3089 (s2 0x00B50000 -> s5, plus 512x384 downsamples into
  0x01510000/0x01450000). The draw engine ignored those blits (only the live-draw renderer
  mirrored them), so s5 and the downsample targets kept old frames: stair-stepped ghosts of
  earlier camera shots, and exposure/bloom flipping between two levels. Fix in ps3recomp:
  rsx_draw_engine_blit + D3D12 ENG_REC_BLIT. Verified with bursts: no ghosts, brightness
  steady (75-83 vs 121/172 flips), real motion blur on fast moves, ~57-60 fps.
  Tools: out/ghostrun.sh, out/burst.sh (frame bursts), RSX_SURF_DUMP_*, RSX_TRACE_AT_OFF.
- Fight fps (2026-10-30): tools/run.sh leaves PS3_VERBOSE on, which costs a few fps; measure with
  tools/fightbench.sh (PS3_VERBOSE=0, like the launcher). Profile (T6_PROF=drain): nv3089_blit's
  guest-memory copy was 14% of the drain thread -- now skipped when the GPU blit ran. Fight mean
  60.2 fps (slowest 5 s window 59.8).
- Rare mid-fight freeze: 1 in ~20 bench runs (1 of 21 since the ring fixes). SPURS_WATCHDOG=1
  (launcher default) dumps chains/guards/flags to the log when frames stall 4 s. The ring halves
  are guarded (JobGuardNotify after the PPU resets the next half), so it is not the stale-half
  race; next time it happens the watchdog output says where.
- Open: fights ~56-58 fps (T6 is ~61), unresolved sysPrxForUser 0xE76964F5 / 0x45FE2FCE and
  cellSpurs 0xA121A224 (harmless so far), 4:3 aspect constants, config.exe / package.
