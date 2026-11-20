# KytyPS5-nhl

**Play EA Sports NHL 27 on PC.** This is a fork of [KytyPS5](https://github.com/KytyPS5/KytyPS5) focused on one game: **NHL 27** (PPSA34063, tested with v01.003.001).

> Not affiliated with Sony Interactive Entertainment, PlayStation or Electronic Arts. No game files or system software are distributed. Use only game files you have obtained legally.

## Download

**Current build — #208 (main line):**
https://github.com/duy64/KytyPS5-nhl/releases/tag/KytyPS5-2026-11-19-main-208

All builds are on the [releases page](https://github.com/duy64/KytyPS5-nhl/releases), including the macOS test build (#209) and the diagnostic builds used to trace the visual glitches below.

## What works

- Boots to the menus and loads into matches; Play Now, Franchise and Be A Pro are playable
- Player and scene lighting fixed (players and ice were dark before this fork's fix)
- Commentary and game audio working; saves and profiles working
- FSR 4 upscaling on AMD cards (renders at 2560×1440, upscales to 4K); the game's own frame pacing is untouched
- A launcher: point it at your game folder once, pick a build, press play
- macOS on Apple Silicon: boots to the title screen; getting into the menus is in testing now (#209)

## Get started

1. Download the Windows zip from the current release and extract it anywhere you can write to.
2. Start the launcher and add your NHL 27 folder — the one that holds `eboot.bin`.
3. Press play. The first launch compiles shaders, which stutters once; after that the cache is reused.

**What you need**

- Windows 10 or 11, 64-bit
- A CPU with AVX2 (Intel Haswell / AMD Zen or newer)
- A current GPU driver and the Microsoft Visual C++ Redistributable (x64)
- Your own dump of NHL 27: PPSA34063, tested with 01.003.001

## Known issues (2026-11-19)

- **Visual glitches during play:** brief pixelation, or a green or dark flash over the ice. They usually clear on their own within seconds; opening Instant Replay for a moment clears most of them. This family is under active investigation — it is the fork's top open problem.
- **Frame-rate dips** in some matches.
- **Team logos missing** in some overlay graphics (scores and standings show without them in places).
- **Celebration audio** plays much louder than the rest of the game.
- **Rare GPU device-loss crashes.**
- Cosmetic, inherited from upstream: jersey cloth glitches, some skin tones off in cut-scenes, blurry facial hair.

## Builds

- **main** — the playable line. Current release: #208.
- **macOS test line** — the MoltenVK subgroup fix for Apple Silicon. Current release: #209.
- **diagnostic line** — capture builds for the glitch investigation. Current release: #210.

Older experiment lines are retired and their releases removed, so the releases page only ever holds builds that are current.

## What this fork adds

Over upstream KytyPS5, this fork carries the NHL 27 work:

- Boot and playability fixes for NHL 27 (front-face lighting, Opus commentary decoding, a Be A Pro tessellation crash, a module-list boot crash)
- The FSR 4 upscaling bridge, plus DLSS and XeSS build lines
- Performance tuning aimed at NHL 27's frame cost (write-tracking defaults; lazy shared writes measured and turned off)
- An F2 frame-capture tool, so a glitch can be reported with the exact frame that showed it
- macOS fixes (an AGC boot blocker, and the subgroup shader lowering in the test line)

## Credits

- [KytyPS5](https://github.com/KytyPS5/KytyPS5) and the original Kyty — the emulator this fork is built on
- Cryan: the front-face lighting fix and testing
- Petr and Toucann: performance development
- AnyPS5 developers, for comparison data on the lighting defect
- Matus Riedl ([nhl27-pc-faceoff](https://github.com/MatusRiedl/nhl27-pc-faceoff)), whose project builds on this fork and whose releases are audited here for source-level picks

Development on this fork is AI-assisted; all changes are reviewed and tested in-game by the maintainer before release.

## License and building

GPL-2.0, same as upstream. To build from source, follow the [upstream KytyPS5 instructions](https://github.com/KytyPS5/KytyPS5) — this fork builds the same way.
