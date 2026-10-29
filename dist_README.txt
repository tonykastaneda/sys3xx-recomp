TEKKEN 6 (arcade, System 357) - native Windows port
====================================================

  config.exe   settings: resolution, 4:3 / 16:9, fullscreen, VSync, Player 1 / Player 2 / system buttons
  t6.exe       the game. Double-click it (or use "Save & Play" in config.exe).

Game files
----------
t6.exe needs the extracted TeknoParrot dump of the game (the folder that holds dev_hdd0).
Either put it in a folder named "game" next to t6.exe, or set its path in t6.ini:

  [Paths]
  Game=C:\path\to\the\dump

On first start t6.exe extracts GAME_113.elf into the "cache" folder (about 9 MB) and
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
  1080p / 1440p     window of that size (full screen uses the desktop)
  4:3               the 3D is rendered with a true 4:3 projection (camera patched), shown
                    in a 4:3 frame; the HUD keeps its layout but is drawn ~25% narrower.
  Accurate character models (on by default): fixes torn/stretched characters by
                    waiting for the SPU skinning; fights may then run below 60 fps.

Logs go to t6.log next to t6.exe.

Known issues
------------
  - (none for video: 1080p/1440p render at 1.5x/2x internally; InternalScale=0 in t6.ini turns that off)
  - Attract-mode movies show black.
  - Sound is not verified.
