# Congo Bongo / Tip Top / Zaxxon emulator (SDL3)

A small, standalone emulator of two Sega arcade games, **Congo Bongo** (1983) and **Zaxxon** (1982), written for
SDL3 in one C++ file plus a Z80 core and a few headers. The program looks at your ROMs and starts the right game.

**Congo Bongo**: the sound board runs the real sound ROM, and the analog percussion and gorilla growl are synthesized.
The rest of this paragraph is the original description of that part. The sound board runs the **real sound ROM**; the
analog bass drum, congas, rim shot and gorilla growl are **synthesized from
circuit models, so no audio samples are needed**.

You supply your own ROMs. None are included.

**Zaxxon** has no sound CPU and no samples to begin with: an 8255 switches a set of analog effect circuits (555
timers, 74123 one-shots, op-amp filters, a noise generator, a photocell and voltage-controlled attenuators). MAME's
driver notes list the discrete sound emulation as a known issue. Here the main board is emulated (see below) and every effect is
**synthesized from the schematic in the manual (sheets 11 and 12)**, again without samples. Be aware that the
Zaxxon sound is a *behavioural* model: signal paths, one-shot times and filter corners were read off the drawing and
computed from the part values, but there was no real board or recording to compare against, and the parts without
public data (the MB4391 attenuators, the CdS photocell) are modelled as ideal. Levels and timbre are therefore
approximations; the header comment of `zaxxon_synth.h` lists every effect, its source on the schematic and the
figures used, so they are easy to check and tune. Feedback from anyone who knows the real machine is very welcome.

Congo Bongo remains on the list of MAME games that still require samples. I never like that because in some versions the drums sounded really off. I fed Claude the board schematics and the arcade manual and voila, emulation of the analog sound. I think the drums are very accurate, but the gorilla sound is still kind of an approximation. Would love for someone who has played the actual real arcade game recently to try it and let me know what they think. 

## Build (Windows, MSYS2 UCRT64)

One time:

    pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-pkgconf

Then, in this folder:

    ./build.sh

(That is just `g++ -O3 -std=gnu++17 congobongo.cpp -o congobongo $(pkg-config --cflags --libs sdl3)`.)
Linux works the same way with your distro's SDL3 development package.

## Required libraries

* **SDL3** (3.2 or newer), the only library the program links to: window, renderer, audio and input.
  Windows: `SDL3.dll` (build.sh copies it next to the program). Linux: your distro's SDL3 package.
* A **C++17 compiler** (g++ or clang++) and its standard library. Nothing else: no SDL_ttf, no zlib (a small
  inflate for the ROM zips is built in), no image or audio libraries. The Z80 core, sound chips, drum and
  gorilla synthesis, CRT filter and the vector logo renderer are part of this source.

Licenses of everything that is used or borrowed are listed under "Licenses and credits" below and in
`THIRD-PARTY-NOTICES.txt`.

## Run

Put `congo.zip` (the MAME set) and/or `zaxxon.zip` in the `roms` folder, then:

    ./congobongo

or give the zip or folder explicitly: `./congobongo C:/path/to/congo.zip`.
ROMs are matched by CRC32, so renamed files are fine. The MAME `congo` (rev C)
and `tiptop` sets are supported (`tiptop.zip` needs `congo.zip` beside it).

Zaxxon: the MAME sets `zaxxon` (set 1), `zaxxon2`, `zaxxon3`, `zaxxonj` (Japan) and `zaxxonb` (bootleg) are
supported; each zip holds everything it needs. The game is detected from the ROMs. If the ROMs of both games
are present Congo Bongo starts; use `--game zaxxon`, or `--set zaxxonj` to choose one particular Zaxxon set.
The Japan and bootleg program ROMs are stored scrambled; the unscrambling (see `zaxxon_hw.h`) was measured from
the plain set 3 program rather than copied from anywhere.

### Zaxxon controls

Arrows = stick, Z / Left Ctrl / Space = fire, 5 = coin, 1 / 2 = start (a gamepad works the same way as for Congo).
**I** swaps the stick's up and down (the arcade stick is flight-style; this is a taste setting, saved in the ini file).
**F2** is the service switch: the self test (ROM and RAM test, then the screens and the **sound port test** that
plays each circuit) steps on with fire; reset (F3) leaves it. The Audio menu has MASTER VOLUME, SOUND EFFECTS and a
TEST SOUND picker (flying, homing missile, base missile, laser, battleship, small and large explosion, cannon,
enemy shot, target alarm, fuel alarm). Cheats and the RAM finder are Congo-only.

### Sound only (needs just `tip_top_rom_17.u19`)

    ./congobongo --sound-test tip_top_rom_17.u19

Arrow keys pick a sound-board command number, Enter plays it, 0 sends the
init command 30. Command 91 (hex) is the repeating gorilla growl.

## Controller

Plug in any gamepad that SDL3 knows (Xbox, PlayStation, Switch Pro, 8BitDo ...); it also works when plugged in
while the game runs.

| button | action |
|---|---|
| d-pad or left stick | move |
| A, B or X | jump |
| Back / Select (or left bumper) | insert coin |
| Start | 1 player start |
| right bumper | 2 player start |
| Y or the Guide button | options menu on / off |

In the menu: d-pad or stick move and change values (hold to repeat), A selects, B or Back goes up one level,
Start, Y or Guide close the menu.

## Keys

| key | action |
|---|---|
| arrows | move (diagonals are two keys) |
| Z, Left Ctrl or Space | jump |
| 5 | insert coin |
| 1 / 2 | 1 player / 2 player start |
| Esc | options menu on / off (it comes back on the page you left) |
| Backspace | (in the menu) back one level |
| P | pause |
| R | rotate the picture 90 degrees |
| F3 | reset |
| F4 / F5 | previous / next scene (debug) |
| F11 | fullscreen |
| F12 | screenshot (congobongo-000.bmp ...) |
| + / - | master volume |

## Options menu (Esc)

Arrows move, Left/Right change a value, Enter selects, Esc goes back.

* **Video**: fullscreen / windowed, integer scaling (sharp pixels, whole-number zoom) or fit the window,
  WINDOW SIZE (AUTO opens at the biggest whole-number zoom that fits your desktop, taskbar and title bar
  included, e.g. 4x on a 1440p screen; or pick 1x, 2x, 3x ...),
  CRT effect (off / scanlines / **CRT** flat / **curved CRT**), CRT LOOK (ARCADE, SOFT, SHARP, HEAVY or
  your CUSTOM settings), CRT TUNING, rotate view, show FPS.
  CRT TUNING opens a low panel so you can watch the picture while you adjust: CURVATURE (0 = flat), CORNERS
  (rounding), SCANLINES (the beam gets wider on bright pixels, so bright areas fill the gaps like a real
  tube), MASK (RGB phosphor stripes), GLOW (halation), VIGNETTE and BRIGHTNESS. Moving any slider switches
  the look to CUSTOM; the four presets are never overwritten. ARCADE (default) is a cabinet monitor with a
  gentle bulge, SOFT is smoother, SHARP has crisper lines and no mask, HEAVY is the strongly bulged glass.
  The curved version costs a few milliseconds per frame on one CPU core.
* **Audio**: master volume plus separate levels for music and beeps (the two SN76489 chips), all
  drums, the bass drum on its own, and the gorilla, each 0 to 200 percent (BASS DRUM 0 silences the
  bass drum but keeps every other sound and all the timing). BASS EDGE SOFT rounds the squared-off start
  of the bass-drum model, which otherwise clicks on speakers. TREBLE CUT is a speaker-style low-pass
  (off, 12, 8, 5 or 3 kHz; default 8 kHz). TEST SOUND: pick a sound-board command with Left/Right
  (91 = gorilla, 07 = a drum pattern, the rest are music) and press Enter to hear it.
* **Cheats**: see below.
* The game is frozen while the menu is open. Choices are saved in `congobongo.ini` next to the program.

### Menu look and font

The Congo Bongo logo heads every page and the text is the game's own 8x8 size. The font is the game's
character ROM: the program finds the letters and digits by comparing the ROM's tiles with a reference
font, and the start-up line tells you what it found. If it cannot find them it uses the built-in
Press Start 2P font (SIL Open Font License, Codeman38). You can also supply your own bitmap:
`font.bmp` next to the program, 128 pixels wide, 16 glyphs per row, each 8x8, white on black; 6 rows
(ASCII 32 to 127) or 8 rows (ASCII 0 to 127). `font.bmp` wins over the ROM font.

    ./congobongo --dump-font fonts.bmp     # picture of the menu font and of all 256 character-ROM tiles
    ./congobongo --font rom|builtin|auto   # force a source

If the ROM font is not recognised, `--dump-font` shows where the letters are: put the tile number of
"A" and of "0" into `congobongo.ini` as `font_alpha=0x..` and `font_digit=0x..`.

### Cheats

Built in: INFINITE CREDITS, INFINITE LIVES and INFINITE BONUS (RAM addresses from the MAME cheat collection,
congo.xml; each freezes a RAM byte so the game always reads that value, even in the middle of a frame), plus three
found by tracing the game program itself:

* **INVINCIBLE**: enemies, coconuts, snakes and so on cannot kill you, but hippos, fish, logs and lily pads still
  carry you. (Byte 840D is the player's collision result: "killed" values are ignored, "standing on something"
  values are left alone.) It does not stop you drowning if you miss a platform; that is WALK ON WATER.
* **WALK ON WATER**: falling into the water or off a ledge no longer kills you, you just keep walking. Finishing a
  scene still works.
* **NEXT SCENE / PREVIOUS SCENE**: jump to the next or previous scene (four scenes per level; after the fourth you
  are in scene 1 of the next level, with the higher difficulty). Use the menu entries, or press **F5** (next) and
  **F4** (previous) while playing. Only works while a game is being played.

INVINCIBLE and WALK ON WATER patch the game program in memory (the files on disk are never changed) and are only
offered when the loaded ROM has the code they were written for (congo rev C and tiptop do).
In `cheats.txt` an address from 0000 to 7FFF patches a program byte, 8000 to 8FFF locks a RAM byte.

Cheats > RAM FINDER works like MAME's cheat search: NEW SEARCH remembers all of the game's RAM, close the
menu and play (lose a life, let the timer run), reopen it and filter with DECREASED, INCREASED, CHANGED,
UNCHANGED or EQUALS VALUE until few bytes are left. SHOW RESULTS lists them; pick one, set the value to
lock with Left/Right and press Enter. The cheat is switched on and appended to `cheats.txt`, so it is
there next time (switched off). The file is plain text, one cheat per line, and you can edit it:

    ; Name = hex address:hex value, more pairs after commas
    *Lives = 8123:09
    Time and lives = 8456:99, 8123:09

A leading `*` switches a cheat on at start-up; addresses are 8000 to 8FFF (main RAM, the same
addresses as in MAME's debugger and cheat files). `--poke 8123:09` does the same from the command line.

## Options

    --scale N          window scale (default: the WINDOW SIZE setting, AUTO = biggest that fits)
    --no-rotate        keep the picture as scanned (256x224)
    --font MODE        menu font: auto, rom, builtin;  --dump-font FILE.bmp
    --poke ADDR:VAL    keep a RAM byte at a value (hex), like a cheat; repeatable
    --volume X         master volume 0..2 (overrides the saved setting)
    --game NAME        congo or zaxxon, when both sets of ROMs are present;  --set NAME  one Zaxxon set
    --lives N          3, 4 or 5;  --free-play (both games)
    --bonus N          10000, 20000, 30000 or 40000
    --difficulty N     0 easy ... 3 hardest (default 1)
    --headless --frames N --wav out.wav --shot out.bmp [--shot-every N]
                       run without a window, write audio and screenshots
    --coin-at N --start-at N     (headless) coin / start at a frame number
    --service-at N     (headless, Zaxxon) press the service switch;  --cmd T:N plays Zaxxon test sound N (0..10)

## What is emulated

### Zaxxon

* Main board 834-0214 (follows MAME's `zaxxon.cpp`): Z80 at 3.04125 MHz, program ROM at 0000 with the 4 KB ROM
  mirrored, RAM, character RAM, sprite RAM, the two 74LS259 control latches (coin counters, flip, interrupt enable,
  background colour and enable, scroll), DIP switches, joystick and the NMI service switch. The 256x224 picture
  has the skewed-scroll isometric background built from the tile-map ROMs, 32x32 three-bit sprites, the character
  layer coloured by the colour-code PROM and the resistor-weighted palette PROM.
* Sound: the 8255 outputs drive `zaxxon_synth.h`, one model per circuit (see above). Boot-time glitches on the
  8255 pins are ignored for the first 100 ms.
* Verified by running every one of the five sets headless (attract mode, coin, start, play, self test) and
  looking at the pictures. Not emulated: cocktail mode, gamepad rumble, save states.

### Congo Bongo

* Main board: Z80 at 3.04125 MHz, memory map, sprite DMA chip, control
  latches, background scroll/skew, 32x32 character layer, 32 sprites, palette
  PROM, vblank interrupt, coin / start / joystick / DIP inputs. Follows MAME's
  `zaxxon.cpp` driver.
* Sound board 834-5168: Z80 at 4 MHz, 8255, two SN76489A (4 MHz and 1 MHz),
  244 Hz interrupt, running the real sound ROM.
* Percussion and gorilla: modelled from the schematic (see
  `congo_perc_synth.h`, `congo_gorilla_synth.h`). The gorilla model is fitted
  to two recordings of the real sound.

Not emulated (Congo Bongo): service switch / test mode NMI, cocktail 2-player controls,
save states.

## Status and reporting problems

The Z80 core passes the standard zexdoc and zexall instruction exercisers.
For Congo Bongo the video and sound paths were tested with synthetic graphics and the real
sound ROM, but the author did not have the game's main ROMs, so that game
itself is untested. Zaxxon was run with the real ROMs of all five sets. The SDL window, menu and
audio code for Zaxxon was only compiled against a stand-in SDL3 header (no SDL3 was available while it was
written), so please report anything odd in the menus. If something looks wrong, run

    ./congobongo --headless --frames 600 --coin-at 120 --start-at 150 --shot-every 60 --shot shot --wav test.wav

and send the `shot-*.bmp` files and `test.wav` along with what you see.

## Licenses and credits

The emulator's own code (congobongo.cpp, z80.c/h, the synth headers, the Zaxxon board, the logo renderer) is original work
for this project. It borrows the following; nothing else was copied.

| what | from | license |
|---|---|---|
| hardware behaviour, memory map, ROM names and CRCs, video, palette and input logic; SN76489A behaviour; the MM5837 noise generator (17-bit LFSR) | MAME (Nicola Salmoria, Stephane Humbert and the MAME team): zaxxon, sn76496 and mm5837 sources, re-implemented, not pasted | BSD-3-Clause |
| `Inflater`, the zip decompression | "puff" by Mark Adler | public domain / zlib-style permission |
| menu font, built-in fallback (`PS2P_GLYPHS`, rasterised at 8 px) | **Press Start 2P**, Copyright 2012 The Press Start 2P Project Authors (cody@zone38.net), designed by Cody "CodeMan38" Boisclair | SIL Open Font License 1.1 |
| window, renderer, audio, input | **SDL3**, Copyright (C) 1997-2025 Sam Lantinga | zlib license |
| the built-in cheats (INFINITE CREDITS/LIVES/BONUS, INVINCIBLE, WALK ON WATER) | RAM addresses from the MAME cheat collection's `congo.xml` | data only; credit to the cheat authors |
| Zaxxon hardware behaviour, memory map, ROM names and CRCs, video, palette and input logic | MAME `zaxxon.cpp` (Nicola Salmoria and the MAME team), re-implemented, not pasted | BSD-3-Clause |
| Zaxxon sound circuits (sheets 11 and 12) | *Zaxxon* manual 420-0724: part values, signal paths and the sound port test | data only |
| menu layout idea | the Dig Dug port by TekRantGaming (MIT); no code was taken | MIT |

The Congo Bongo and Zaxxon names, games and logos belong to their owners (Sega). The logos (`congo_logo_data.h`,
converted from `Congo_Bongo_Logo.svg`, and `zaxxon_logo_data.h`, converted from `Zaxxon_logo.svg` with
`tools/svg_to_logo.py`) are used only as menu decoration, and no ROMs are included: you supply your own.
Full license texts for MAME (BSD-3-Clause), the OFL and SDL are in `THIRD-PARTY-NOTICES.txt`.
