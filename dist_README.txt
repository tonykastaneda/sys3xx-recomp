TEKKEN 6: BLOODLINE REBELLION (arcade, System 357) - native Windows port
=========================================================================

  config.exe   settings: resolution, 4:3 / 16:9, fullscreen, VSync, Player 1 / Player 2 / system buttons
  t6.exe       the game. Double-click it (or use "Save & Play" in config.exe).

Game files
----------
t6.exe needs the extracted TeknoParrot dump of the game (the folder that holds dev_hdd0).
Either put it in a folder named "game" next to t6.exe, or set its path in t6.ini:

  [Paths]
  Game=C:\path\to\the\dump

On first start t6.exe extracts GAME_208.elf into the "cache" folder (about 8 MB) and
creates the USB key file T6.ID in the game folder.

Default controls (change them in config.exe; XInput pads work too)
-----------------------------------------------------------------
            Player 1          Player 2
  Move      W A S D           Arrow keys
  1 (LP)    U                 Numpad 4
  2 (RP)    I                 Numpad 5
  3 (LK)    J                 Numpad 1
  4 (RK)    K                 Numpad 2
  Start     Enter             Numpad +
  Coin      5                 6
  Test F2, Service F3. Pads: X=1 Y=2 A=3 B=4, Back = coin.

Video
-----
  Original / 720p   1280x720 (the arcade resolution)
  1080p / 1440p     rendered at 1.5x / 2x internally, in a window of that size
                    (full screen uses the desktop). InternalScale=0 in t6.ini renders
                    1280x720 and scales the picture instead.
  4:3               the 3D is rendered with a true 4:3 projection (camera patched), shown
                    in a 4:3 frame; the HUD keeps its layout but is drawn ~25% narrower.
  Accurate character models (on by default): draws characters only after the SPU has
                    finished skinning them (no torn/stretched meshes). Fights run ~60 fps.

Sound and movies
----------------
  Music, voices and effects play from character select on. The attract movies play,
  without their own audio.

Logs go to t6.log next to t6.exe.

Known issues
------------
  - Movie audio is not played.
  - Rarely the picture freezes if a coin is inserted during an attract movie.
  - Fights run at about 57 fps (the T6 port reaches 60).
