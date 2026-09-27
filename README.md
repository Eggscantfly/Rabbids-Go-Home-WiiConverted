# Rabbids Go Home WiiConverted

A Converter/Launcher to play the Wii Version of Rabbids Go Home on the 16 Level PC Ports exe.

**Download the setup and the mods: https://www.joykhloe.com/RGH/wc/home**

## rghport

Builds a Windows port of the Wii version of Rabbids Go Home from files you own: the Wii game data and the PC release
of the game. The Wii game runs on the PC release's executable. A platform DLL built from `platform/` provides the Wii
remote (keyboard, mouse or pad) and the Wii save system.

The repository holds code and reverse-engineering notes only. Everything derived from your game files is generated
when you convert, into a cache folder and into the port folder.

## Requirements

- Windows: the port runs the PC release's 32-bit executable.
- Python 3.10 or later, with `numpy`: `pip install numpy`, or `pip install .` in this folder, which also installs the
  `rghport` command. The bundled Pybind11/miniLZO module decompresses the archives when built; without it a pure-Python
  decoder is used, which is far too slow for a whole conversion.
- The platform DLL: `platform\build.bat` builds `platform\build\wiimote.dll` with Visual Studio 2017 or later and the
  C++ x86 build tools (see `docs/platform.md`).
- Disk space: about 2.5 GB for the port folder and 15 MB for the cache. A conversion takes a few minutes.

## Inputs

| Input | Contents | Option |
|---|---|---|
| Wii game data | the folder with the Wii disc's main bigfile (`RGH.BF`) and its sibling bigfiles (`RGH.wii.sns.BF` streamed sounds, `RGH.$hd$.bik.BF` videos) | `--wii` |
| PC release | the installed or copied PC release: game executable, `shaders` folder, `binkw32.dll`, main bigfile | `--pc` |
| Another executable | optional: an executable file to copy instead of the PC folder's game program | `--pc-exe` |
| Wii save | optional: the game's save data folder copied from a Wii or an emulator NAND (`index.dat`, `slt_*.sav`); only read | `--import-save` |

Two builds of the Wii game data are accepted:

| Build | Archive | What the converter does differently |
|---|---|---|
| the US disc (game ID `RGWE41`) | 8710 entries; packages, texture banks, Magma blobs and language bins named by their key alone (`fff09e7b`); English, French and Spanish texts; the Latin Rabbids font and Arial | its `default.cfg` key swaps stay under their language guard (the packages hold the original fonts and pages) |
| the later build with Japanese texts | 4330 entries, bins named `<key>.bin`; English and Japanese texts; packages built with the Japanese fonts and pages swapped in | its `default.cfg` key swaps are made unconditional (the packages hold only the swap targets) |

The language bins are found through the archive's language indexes, not by their keys (the binarizer's key counter
differs per build). A script model taken from the PC release names its tracks and lists by key and the loader reads
them from the package in that order; when a Wii package lacks one (the US disc has no WiiWare save state track,
`0E009029`), the record is added from the PC release right after the record of the model's nearest earlier reference,
and the conversion summary lists it under `pc_twin_records_added`. Without it the loader searches past the end of
the package and the game stops responding at start-up.

The game's executable is the PC folder's `LyN_f.exe` (else the folder's only program, launchers and uninstallers
aside). It is copied as it is, and its SHA-256 is reported; the release build the platform DLL was written for is

    208054AF049A1E72EBEA3ADC7FA9BC5FABAD162C47861E52FE81B86FACEED502

Another build is a warning, not a stop: the DLL verifies byte signatures before every patch, so it leaves alone what
it does not recognise. `--pc-exe FILE` copies another executable file instead of the PC folder's, with `convert` or
later with `assemble`.

The port folder's copy of the executable gets the moon icon (`rghport/data/moon.ico`) in its main icon group, so
Explorer and the game window show it (`rghport/convert/peicon.py`, pure Python); `--icon FILE` gives it another
`.ico` file's images, `--icon none` keeps the release's own. A copy with another icon is still recognised as the
release build by the SHA-256 of its code sections (`SUPPORTED_CODE`). The PC release's own file is never changed.

## Setup and launcher

`dist\RGHPort\WiiConverted Setup.exe` is the setup: the launcher window (below) in setup mode - the same dark window, whose
sidebar lists the steps that build the game folder (welcome, the Wii game data folder, the PC release folder, the
destination, the conversion, done). It needs no Python on the machine that runs it; the whole `dist\RGHPort`
folder is the app: the window with its Qt runtime, `rghport-cli.exe` - the same command line as `python -m rghport`,
which the window runs for every conversion and for the folder checks (`rghport inspect`) - and its `_internal`
folder with the platform DLL and the launcher. It holds no game data.

    build_app.bat                         builds dist\RGHPort (needs: pip install pyinstaller numpy capstone, and
                                          platform\build.bat and build_launcher.bat run before)
    platform\build\launcher\launcher\RGHLauncher.exe
                                          runs the setup from the repository (it finds python -m rghport there)

- Every folder is checked as you pick it (the archives found, the game's executable, free space). A game folder that
  already holds a converted archive is refreshed instead of converted again: its executable, launcher, DLL and
  shaders are copied anew (`rghport assemble`), the archive is kept; delete `RGH_WC.bf` there to convert again.
- The destination step asks one thing: whether to add the Options page to the game's pause menu (off unless you
  turn it on; `--no-options-page` otherwise).
- When the game folder is built, Go Home turns the window into the folder's launcher: Play is on its Home page.

The Convert step's meter is the game's own end-of-level count: the tower of pipes that fills up, the reward icons that
light up as the fill passes them, the splash on the surface and the tag with the tally. `Assets\GUI\meter\` holds
the page's sprite sheet as the game ships it and `layout.json`, the page's areas, elements, keyframes, mask modes and
blend modes as its menu data holds them; `launcher\meter.cpp` renders that data the way the game's menu renderer
does (masks as depth levels, rotations about each element's pivot, the fill multiplied over the pipes) with the
conversion's progress as the
tower's frame (the count of archive entries drives the fill; the tag counts to 1000 as in the game). The scene's
sounds come with it, decoded from the game's sound banks into `Assets\Audio\sfx\` (`launcher\sfx.cpp` plays
them): the transfer loop while the fill rises, its pitch climbing with the fill as its sound set prescribes, and a
ring for every reward.

The setup plays music while it is up: `Assets\Audio\setup.rghs`, the Wii remote's looped tune, compiled into the
window. An `.RGHS` file is a 32-byte header (sample rate, channels, the loop's first frame and the frame after its
last, the frame count) followed by an Ogg Vorbis stream; `tools\rghs.py` writes one from a WAV (`from-wav`, with
ffmpeg) or an Ogg file (`wrap`) and reads the header back (`info`). The window decodes it with stb_vorbis and plays
it through waveOut (`launcher\music.cpp`), looping between the two frames, and fades it out when Go Home hands over
to the launcher, which never plays it.
- The setup converts with the defaults: the archive is named `RGH_WC.bf`, the PC release's animation lists are used
  (`--twin-kinds spec:trl`), the cache goes next to the game folder, the executable gets the moon icon, the game starts in
  English (the launcher changes the language), and the script record folders are those of the `script_overrides`
  folder next to the app (or next to one of the three folders above it), in the order of its `layers.txt` (one
  folder per line, later folders win; without the file: `wii_records`, then `overrides`). The command line has the
  options for anything else.
- The folders picked last time are kept per user in `RGHPort.ini` (under the user's application data folder).

The game folder gets the launcher: **`WiiConverted Launcher.exe`** (a small stub that carries the WC icon and starts
the window) and a `launcher` folder with `RGHLauncher.exe`, a Qt Widgets application, and its runtime (Qt6Core / Gui /
Widgets, the Windows platform plugin, the C++ runtime; about 26 MB). The window is one program: in a game folder it
is the launcher, anywhere else the setup. Sources in `launcher\` (`ui.cpp` the window, `setup.cpp` the setup's
steps, `settings.cpp` the files it reads and writes, `stub\` the stub); `build_launcher.bat` builds both with CMake, Ninja
and a Qt 6 MSVC 64-bit kit (`QT_DIR`, else the newest under `C:\Qt`) and deploys the tree into
`platform\build\launcher\`, which `rghport assemble` copies into every game folder. The art (the logo, the "Wii
converted" wordmark, the moon, the rabbid icons and the Rabbids font) is compiled in from `Assets\` next to the
repository.

A dark window in the style of a game launcher: a sidebar with the pages (Home, Mods, Settings), a top bar with the
language, the window size and PLAY, a banner with the game's art on the Home page, cards. Settings: display mode,
window size, frame rate cap, language, VSync, Wii picture effects, FPS counter. It saves them where the game reads
them - `[graphics]` of `options.ini` (the platform DLL's Options screen writes the same file) and the game's command
line, `[launch] switches=` of the same file (`/lang/xx`, `/res<W>x<H>`, `/fps`); a switch you added to that line
yourself is kept. `--icon` is for the game's executable; the launcher keeps its own WC icon.

The language box lists the languages the game has texts and voices for: those the conversion summary
(`<bigfile>.json`, `"languages"`) names - the Wii disc's own (English, French and Spanish on the US disc; English and
Japanese on the later build). The PC release's launcher offered `en fr de it es nl` the same way (`/lang/<xx>`); a
game folder without a summary (an older conversion) gets those six, all showing English.

## Mods

A mod is a folder under the game folder's `mods\` with a `mod.json` (name, author, version, description, icon,
screenshots) that the launcher's Mods page shows and switches on. The game reads enabled mods from their folders
(the archive is never changed): `entries\<KEY>.<ext>` replaces whole archive entries, `records\<KEY>.bin` replaces
records of the world packages (compiled native scripts drop in as they are), and `main.lua` runs in the game with
the `wc` API (frame callbacks, keys, text over the picture, memory, engine natives to hook or call). See
[docs/mods.md](docs/mods.md).

## Converting

    python -m rghport convert --wii <Wii folder> --pc <PC folder> --out <port folder> --name RGH_WC.bf [--import-save <Wii save folder>]

The `script_overrides` folder next to this repository (the patched script records, in the order of its `layers.txt`)
is used automatically; the game does not start without it. The repository does not carry it (the records come from
the game, see DESIGN.md's data policy): it comes with the setup download, next to `WiiConverted Setup.exe`. Pass
`--script-overrides` yourself only when testing a different override set.

Run it from this folder, or as `rghport convert ...` after `pip install .`. Then start the game from `WiiConverted Launcher.exe` in the port folder.

| Option | |
|---|---|
| `--wii DIR` | the Wii game data folder |
| `--pc DIR` | the PC release folder |
| `--out DIR` | the port folder to write (created when missing) |
| `--cache DIR` | the folder for the data generated from the game files. Default: `<port folder>.cache`, next to the port folder. A cache inside the port folder is refused. |
| `--pc-exe FILE` | an executable file to copy instead of the PC folder's game program |
| `--lang <xx>` | the language the launcher starts the game in: `en`, `fr`, `de`, `it`, `es`, `nl` or `ja` (default: the one the port folder already has, else `en`). A language the Wii disc has no texts for shows English (the summary and `check` say which the disc has). |
| `--import-save DIR` | Wii save data folder to import into `<out>/sav` |
| `--icon FILE` | an `.ico` file for the port folder's executable (default: the moon; `none` keeps the release's own icon) |
| `--name NAME` | file name of the converted bigfile (default `RGH_WC.bf`): `<base>.bf`, starting with `RGH` |
| `--no-options-page` | leave the pause menu as the Wii had it: no OPTIONS entry and no Options page (the platform DLL's control bindings, volumes and graphics screen, `docs/platform.md` section 20). The script records a layer's `needs.txt` ties to the page (`<KEY> options_page` lines) are left out too, so the earlier layer's record of that key stays. |

Development options:

- `--script-overrides DIR[;DIR]`: folders of PC script records `<KEY>.bin`, each with an `entries` folder of loose
  bigfile entries `<KEY>.bin`. They replace what the build produces; later folders win. Default: the
  `script_overrides` folder next to the repository (or next to the packaged converter), in the order of its `layers.txt`.
- `--strict`: stop at a record without a converter instead of keeping its Wii bytes.
- `--no-siblings`: do not copy the sibling bigfiles.
- `--twins scripts|none|all`: which records are taken from the PC release when it has the same key.
- `--prefix FFF,FEF`, `--only KEY,KEY`, `--limit N`: test runs. A test run writes an incomplete bigfile and skips the
  rest of the port folder.

The cache is reused while the input bigfiles stay the same and is rebuilt when they change; it can be deleted at any
time.

### The port folder

    <port folder>/
      <PC executable>         copy of the game's program (LyN_f.exe), with the moon icon
      shaders/                the PC release's shaders, with the Wii's lighting and glow arithmetic
      binkw32.dll             the video library
      wiimote.dll             the platform DLL (from platform/build)
      wiimote.ini             its configuration (kept when the port folder already has one)
      options.ini             the game's settings: the Options screen's (written by the game) and the launch line
      RGH_WC.bf               the converted Wii archive
      RGH_WC.wii.sns.bf       copy of the Wii streamed sounds
      RGH_WC.$hd$.bik.bf      copy of the Wii videos
      RGH_WC.bf.json          conversion summary: counts, record sources, errors
      sav/                    the saves (the imported Wii save, if any)
    <port folder>.cache/      record indexes, record kinds and skin index of the input bigfiles

A sibling bigfile already in the port folder is kept only when it has the same size and the same bytes as the Wii
file; otherwise it is copied again.

### The launch line

The launcher starts the executable itself, on the converted bigfile, with the switches the PC release uses; no batch
file is written. The switches are stored in `options.ini`, `[launch] switches=`, which `assemble` writes and the
launcher's Settings page keeps up to date:

    <PC executable> RGH_WC.bf /binload/fe /lang/en

| Argument | What the PC executable does with it |
|---|---|
| the bigfile | opens it as the game archive. A bigfile whose file name starts with `RGH` gets BIG flag 1, as the release's own bigfile does, so `--name` must keep that prefix. |
| `/binload/fe` | loading mode 3: binary loading, in which each world loads from its per-world bins |
| `/lang/en`, `/lang/fr`, ... | the current language: the two letters are looked up in the executable's language table (`fr` 0, `en` 1, `nl` 3, `de` 5, `it` 6, `es` 7, `ja` 12, ...). The PC release's launcher wrote `/binload/fe /lang/<xx> [/fullscreen] [/vsync] /res<mode> /versionIndex:<n>` with `xx` one of `en fr de it es nl`; `/versionIndex:5` is left off here, because with it the game calls itself "Rabbids Go Home - DVD" after the disc release it came from. |

The converted archive's language indexes and text groups get one slot per PC language (`fr en nl de it es`, and `ja`
when the disc has Japanese): a slot names the disc's own texts and voices for that language when it has them, and the
English ones otherwise, so `/lang/de` on the US disc shows English rather than nothing (the executable loads no world
texts at all for a language that an index does not list). The summary's `"languages"` lists the languages with their
own texts; `assemble` and `check` warn when the launch line starts the game in another one.

## Other commands

    python -m rghport assemble --pc <PC folder> --out <port folder> [--pc-exe FILE] [--name NAME] [--lang <xx>]

Copies the executable, shaders, video library and platform files into an existing port folder, creates `sav` and
stores the launch line in `options.ini` (keeping the language it has unless `--lang` says otherwise) and removes a
`play.cmd` an earlier setup wrote. Use it to put a newly built platform DLL or
launcher into a game folder without converting again; the setup does this by itself for a folder that already holds
a converted archive.
The copied shaders get the Wii's lighting and glow arithmetic where the release's HLSL differs from it (spot cosine,
per-light attenuation, normal matrix, lightmap coordinates, glow blend; see docs/platform.md §19.7).

    python -m rghport check --out <port folder>

Verifies a port folder: the game's executable (its SHA-256 reported), the `shaders` folder and its Wii lighting
changes,
`binkw32.dll`, `wiimote.dll` and `wiimote.ini`, the converted bigfile and both siblings named after it, the `sav`
folder and the launch line. Every item is listed as `ok`, `warning` or `PROBLEM`.

    python -m rghport import-save --wii-save <Wii save folder> --out <port folder>/sav --bigfile <port folder>/RGH_WC.bf

Imports a Wii save on its own (see Saves).

    python -m rghport kinds --wii <Wii folder> [--cache DIR]

Generates the record kinds of the Wii archive into a cache folder; `convert` does this by itself.

    python -m rghport inspect [--wii <Wii folder>] [--pc <PC folder>]

Describes the folders for the setup, as JSON: the archives found and the disc's languages; the game's executable.

## Exit codes

| Code | `convert` | `assemble` | `check` | `import-save` |
|---|---|---|---|---|
| 0 | the port folder is complete | the port folder is complete | the port folder is complete | imported, and the read-back check passed |
| 1 | error | error | not a folder | error, or the read-back check found problems |
| 2 | bad or refused option | bad option | bad option | refused: the output folder is inside the Wii save folder |
| 3 | converted, but the port folder is incomplete (the problems are listed) | the port folder is incomplete | the port folder is incomplete | |
| 4 | the port folder was built, but the save import failed | | | |

## Saves

The platform DLL keeps the game's saves in the `sav` folder next to the executable, in the PC layout
(`slt_<slot>_<structure>.sav` and `slt_xx_<structure>.sav`). A Wii save is imported with `--import-save` during
`convert`, or later with `rghport import-save`:

- the input is the game's save data folder from a Wii or an emulator NAND (`index.dat` and the slot files;
  `banner.bin` is not needed). It is only read.
- The type of every saved variable comes from the universe script model (bigfile entry `72002B9C`).
- After writing, the files are read back the way the PC executable reads them and compared with the Wii save.

In `wiimote.ini`, `[save] enabled=1` turns the save layer on and `channel_installed=1` skips the Rabbids Channel
install prompts. Details: `docs/platform.md`, section 12.

## Regression test

After a conversion you trust, record the hashes of its output; after converting again, compare them:

    python tests/regression.py record --out <port folder> --reference <file outside the repository>
    python tests/regression.py check  --out <port folder> --reference <the same file>
