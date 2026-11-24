# ds3-jumper

A tiny side-scrolling platformer made to play with a **PS3 DualShock 3** controller.
Written in C99 with SDL2 — runs on **Linux, macOS and other Unix systems**.

![screenshot](docs/screenshot.png)

Run, jump over gaps, grab coins and stomp five kinds of enemies in an endless, procedurally
generated world that gets harder the further you go. The controller rumbles when
you get hit, grab coins or stomp enemies, and LED 1 lights up (no more blinking).
Retro sound effects and an original chiptune soundtrack are synthesized in code.

## Build & run

Install SDL2 and a C compiler:

```sh
# Debian / Ubuntu
sudo apt install build-essential libsdl2-dev
# Fedora
sudo dnf install gcc make SDL2-devel
# Arch
sudo pacman -S base-devel sdl2
# macOS (Homebrew)
brew install sdl2
```

Then:

```sh
make start      # build and play (fullscreen)
make windowed   # build and play in a window
make stats      # show your saved stats
```

Or run the binary directly: `./ds3-jumper [--windowed] [--stats]`.

## Enemies

| Enemy | Looks like | How to beat it | Points |
|---|---|---|---|
| Blob | purple blob | stomp it | 50 |
| Hopper | green frog that jumps | stomp it (time it between hops) | 75 |
| Flyer | bee flying in a wave, can cross gaps | stomp it or run under it | 100 |
| Spiky | red hedgehog with spikes | **can't be stomped**, jump over it | — |
| Armored | beetle with a steel helmet | stomp twice: the helmet flies off, then it gets angry and fast | 25 + 150 |

New kinds show up the further you run.

## Difficulty

Pick it on the title screen (**DIFFICULTY**, left/right). Each difficulty has its
own best score.

| | Lives | Enemies | New enemy kinds | Gets hard by |
|---|---|---|---|---|
| EASY | 5 | fewer, slower | late | ~4000 m |
| MEDIUM | 3 | normal | gradually | ~2500 m |
| HARD | 2 | more, faster | almost right away | ~1400 m |

## Settings

**SETTINGS** on the title screen or the pause menu: character colours (hair,
skin, shirt, pants, scarf), sound effects, music, volume, rumble and an
**FPS limit** of 30, 60, 90 or 120 (default 60). The difficulty is saved there too. Everything is saved to
`settings.txt` next to the save file (see below), e.g. `fps = 90`.

The game can't show more frames than your screen refreshes: on a 60 Hz monitor
90 and 120 still look like 60 (the settings screen tells you). The physics always
runs at a fixed 120 steps per second, so the game plays the same at every limit.

## Saved stats

After every game (game over, restart, or quitting mid-run) your stats are saved
to a plain text file:

| OS | Save file |
|---|---|
| Linux / BSD | `~/.local/share/danidomi/ds3-jumper/save.txt` |
| macOS | `~/Library/Application Support/danidomi/ds3-jumper/save.txt` |

Set `DS3_JUMPER_SAVE=/some/file.txt` to use a different file. It looks like:

```ini
# ds3-jumper save file
best_score = 1520
best_score_easy = 0
best_score_medium = 1520
best_score_hard = 0
last_score = 830
games_played = 12
coins_total = 214
best_coins = 41
enemies_stomped = 37
distance_total = 9450
play_seconds = 1260
last_played = 2026-11-24 15:55
```

Your best score and totals are shown on the title screen, and printed in the
terminal when the game starts and when you quit:

```text
Best scores: EASY 0   MEDIUM 2615   HARD 0  (14 games played)
...
Last score: 1830 (MEDIUM)   Best on MEDIUM: 2615
```

Delete the file to reset.

## Dev mode

Press **SELECT** (or **F3**) any time to toggle a one-line performance overlay
in the top-left corner:

```text
FPS 60 | FRAME 16.7MS (MAX 17.2) | CPU 6% | UPDATE 0.12MS | DRAW 1.90MS | MEM 41MB | PHYS 2/F | OBJ 58 | PAD WIRED
```

FPS and frame time (average / worst in the last 0.5 s), process CPU usage,
time spent in game logic and in drawing per frame, peak memory, physics steps
per frame, live object count and controller status. It's green at 55+ FPS,
yellow at 30+, red below. Start with it on using `./ds3-jumper --dev`.

## Controller notes

- **macOS:** plug the DS3 in with a USB cable. macOS has no DS3 driver, but SDL's
  own HIDAPI driver activates it, so no extra tools (`ds3activate`) are needed.
  Wireless DS3 doesn't work on macOS 12+ — see
  [ds3pair-macos](https://github.com/danidomi/ds3pair-macos) for why.
- **Linux:** works over USB out of the box (kernel `hid-sony` driver), and over
  Bluetooth after pairing it once with BlueZ (plug it in via USB, accept the
  pairing prompt in `bluetoothctl`, unplug, press PS). If the controller isn't
  detected, check you have read access to `/dev/input/event*` / `/dev/hidraw*`.
- Other controllers supported by SDL (DualShock 4, DualSense, Xbox, …) work too.

## Development

```sh
make                                # build
make start                          # build and play
./ds3-jumper --screenshot shot.bmp  # headless: simulate 4 s of play, save a frame
./ds3-jumper --screenshot s.bmp --screen settings   # title | play | pause | settings | over
make clean
```

Everything is drawn with code (rectangles and a built-in 3×5 pixel font) and all
sound is synthesized (`src/audio.c`) — no image, font or audio assets.
