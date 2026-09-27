# RGHPCPort - design

A converter that builds a Windows port of the Wii version of Rabbids Go Home from files the user owns: the Wii game
data and the PC release of the game. It runs the complete Wii game (intro, menus, hub, all levels, saves) on the PC
release's executable.

## Inputs (all supplied by the user)
- the Wii game data folder (the main bigfile and its sound / video bigfiles), and (scripts phase) the Wii script module
- the PC release folder (executable, shaders, video library, bigfiles)
- optional: a Wii save folder to import

## Output
A ready-to-run port folder: the PC executable and its runtime files, the converted bigfile with its sound / video
bigfiles, the platform DLL and its configuration, and a `sav` folder (imported save if one was given).

## Data policy
The repository contains code, documentation and reverse-engineering metadata written as our own tables (addresses and
byte signatures of the supported PC executable, native and argument tables, structure layouts, format descriptions).
It never contains game data: no bigfile content, no converted assets, no decompiled or compiled game scripts, no files
copied from the game, no saves. Everything derived from the game is produced at convert time into a cache folder.

## Layout
    rghport/archive    bigfile reading, writing and patching; packages; record indexes
    rghport/formats    binary stream framework; record formats (worlds, objects, materials, geometry, textures, sound,
                       animation, text, effects, zones, scripts)
    rghport/walk       world and list walker that assigns a kind to every package record
    rghport/convert    Wii -> PC converters (geometry, textures, animation, sound, language, small records, Magma
                       descriptors), package builder, whole-archive build, port folder assembly
    rghport/scripts    Wii script module decompiler, PC bytecode decompiler, PC script compiler, Wii flow
                       restoration rules applied at convert time
    rghport/saves      Wii save import
    rghport/cli.py     command line
    rghport/checks.py  the setup's folder checks (rghport inspect: JSON)
    packaging/         PyInstaller build of the converter, rghport-cli.exe; build_app.bat puts the setup next to it
    platform/          sources of the platform DLL loaded by the PC executable (input, save system)
    launcher/          the window (Qt Widgets): the launcher in a game folder, the setup anywhere else; its stub;
                       build_launcher.bat
    tools/             developer tools: rghs.py, the setup music's .RGHS wrapper (Ogg Vorbis with loop points)
    web/               the download and mods pages on the site (www.joykhloe.com/RGH/wc) in the launcher's look:
                       templates, the Cloudflare Worker, make.py (build, preview, deploy)
    docs/              formats and reverse-engineering notes
    tests/             regression tests (hashes computed from the user's own files)

## Command line
    rghport convert  --wii <folder> --pc <folder> --out <folder> [--cache <folder>] [--pc-exe <file>] [--lang en|ja]
                     [--import-save <folder>] [--name RGH_wii.bf] [--no-options-page]
                     development: [--script-overrides <folder>[;<folder>]] [--strict] [--no-siblings]
                     [--twins scripts|none|all] [--prefix FFF,FEF] [--only KEY,KEY] [--limit N]
    rghport assemble --pc <folder> --out <folder> [--pc-exe <file>] [--name RGH_wii.bf] [--lang en|ja]
    rghport check    --out <folder>
    rghport kinds    --wii <folder> [--cache <folder>]
    rghport inspect  [--wii <folder>] [--pc <folder>]
    rghport import-save --wii-save <folder> --out <folder> --bigfile <file> [--slot-size T=N] [--dump] [--no-check]
                        [--lenient]

The cache defaults to `<out>.cache` next to the port folder; a cache inside the port folder is refused. `assemble`
stores the launch line in `options.ini` (`[launch] switches=`); no batch file is written. README.md lists the
options, the outputs and the exit codes.

## Scripts phase interface
`rghport/convert/hooks.py`: the build calls optional functions of `rghport.scripts` when they exist:
`add_arguments(parser)` (extra `convert` options, e.g. the Wii script module), `prepare(ctx)` (once before the build),
`convert(kind, key, body, ctx)` (package records of kind `script:*`; `None` leaves a record to the other rules) and
`convert_entry(key, name, data, ctx)` (loose entries; `None` keeps the Wii bytes). `--script-overrides` wins over
them until the scripts phase generates every script record itself.

## Conventions
- No absolute paths, drive letters or user names in code, docs, scripts or tests.
- Every input is given on the command line or in a config file; every generated file goes to the cache or the output.
- The game's executable is copied as it is and its SHA-256 reported; the platform DLL verifies byte signatures before
  patching, so another build runs unpatched rather than wrongly patched.

## Acceptance
1. A conversion into empty cache and output folders reproduces the reference build byte for byte and the port boots
   to the Wii hub with an imported save.
2. Script fixes regenerated from the user's files are byte-identical to the reference ones.
3. The repository audit finds no game data, no absolute path and no reference outside this project's scope.
