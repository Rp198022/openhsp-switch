# OpenHSP for Nintendo Switch

This is a Nintendo Switch port of the [OpenHSP](https://github.com/onitama/OpenHSP)
runtime (the engine behind Hot Soup Processor / HSP3).  It makes HSP3 games -
Elona+ among them - run on Switch hardware.

The upstream README is kept untouched at [README.md](README.md); it covers the
Windows / Linux / Raspberry Pi builds.

---

## What is in this repository

| Path | What it is |
|---|---|
| `src/hsp3/switch/` | Port layer for the **console** backend (`hsp3switch`) |
| `src/hsp3dish/switch/` | Port layer for the **graphics** backend (`hsp3dish`), SDL2 + OpenGL ES |
| `makefile.switch` | Build rules for both backends |
| `.github/workflows/switch.yml` | CI - compiles both NROs on every push / PR |
| `PORT_NOTES.md` | Development log: what was changed and why, per revision |

## Building

### Via CI (easiest)

Fork the repository (or push a commit).  GitHub Actions runs
`.github/workflows/switch.yml` and publishes three artifacts:

- `hsp3switch-nro` - console backend
- `hsp3dish-nro` - graphics backend, plus `.elf` / `.map` for crash triage
- `hsp3dish-nro-only` - just the `.nro`, much smaller

### Locally

You need devkitPro (devkitA64 + libnx) and these packages:

```
switch-dev devkitA64 libnx switch-tools
switch-sdl2 switch-sdl2_image switch-sdl2_mixer switch-sdl2_ttf
switch-mesa switch-libdrm_nouveau switch-pkg-config
```

Then:

```bash
make -f makefile.switch -j$(nproc)        # console backend
make -f makefile.switch dish -j$(nproc)   # graphics backend
```

## Running a game

Layout on the SD card:

```
/switch/hsp3dish.nro          the port itself
/switch/openhsp/              the game directory
```

Launch `hsp3dish` from the Homebrew Menu.  The game directory needs:

| File | Why |
|---|---|
| the game itself (e.g. Elona+'s `start.ax` and its data folders) | not shipped here |
| `ipaexg.ttf` | font. **A CJK-complete font is required** - the port draws a Chinese key-hint column, and the IPAex font Elona ships has no Simplified Chinese glyphs.  Noto Sans (SIL OFL) works. |
| `timidity.cfg` + `timidity/instruments/` | only if you want BGM (SDL_mixer renders MIDI through TiMidity) |
| `config.txt` | `music` must be `"2"` (SDL audio); `"1"` selects a Windows-only backend and is silent here |

Optionally, a `charset.txt` containing just `gbk` switches text decoding from
CP932 (Japanese) to GBK (Simplified Chinese).  Do not keep both a GBK title and
a CP932 title in the same directory - they share that one setting.

## Features

- SDL2 + OpenGL ES rendering, controller input, audio
- Chinese / Japanese text via the `charset.txt` switch (GBK / CP932)
- A key-hint column down both black borders, in Chinese (toggle: R + right
  stick click)
- BGM through SDL_mixer + TiMidity
- Save-file I/O

## Known issues

- **A freshly created character pauses on a blank screen for about half a
  minute** before the opening scene appears.  Only happens straight after character
  creation; loading a save is instant.  Known, left as is.
- **Message log text overlaps** - real hardware only; the same NRO and the same
  GL call sequence render correctly in the Eden emulator.  Suspected Tegra TBDR
  behaviour around read-while-write on one texture.  Unresolved; see
  `PORT_NOTES.md` appendix H.
- **One story scene renders its text misplaced** (two layouts at once).
- **Entering a Chinese character name can crash.**  Use a Latin or kana name.
- Do not resize the window.

## License

The port follows upstream OpenHSP's BSD-3-Clause license; see [LICENSE](LICENSE).

Game content (Elona, Elona+, ...) belongs to its respective authors.  This
repository contains **no game files and no translation content** of any kind.
