# Mods

A mod is a folder under the game folder's `mods\`. The launcher lists the folders (Mods page), shows each one's
`mod.json`, and the enable switch writes the folder names into `options.ini`:

    [mods]
    enabled=Hub text mod|Lua hello

The game reads the enabled mods at start (`wiimote.dll`, `[mods]` of `wiimote.ini`), straight from their folders: the
archive is never rewritten, and disabling a mod is just leaving it out of the list. Later mods in the list win when
two give the same file.

```
mods\My mod\
  mod.json           name, author, version, description, icon, screenshots (for the launcher); "switches":
                     command line switches the game gets while the mod is on (a test world: ["/wog08105F25"])
  icon.png           the picture on the Mods page (any picture file; "icon" in mod.json names another)
  screenshots\       pictures shown on the mod's page (or a "screenshots" list in mod.json)
  entries\           whole archive entries (see below)
  records\           records of the world packages (native scripts and other records)
  main.lua           a Lua 5.4 script that runs in the game (and any file it require()s)
```

## Files the game reads instead of the archive's

**`entries\<KEY>.<ext>`** replaces a whole entry of the game archive: a per-world bin (`FFF0xxxx` package,
`FEF0xxxx` texture bank, `FDF0xxxx` sound bin, `FCF0xxxx` language index, `FBF0xxxx` Magma blob, a language bin) or
a loose file (`.wol`, `.wog`, `.fct`, `.cfg`, `.smx`, ...). `KEY` is the entry's key, 8 hex digits, in either case;
a loose file can be named by its file name instead (`entries\default.cfg`). The file holds the raw entry as the
converter stores it (no header). A key the archive lacks can be added the same way. The entry's reference table
(what a `.wol` world list or `.wog` world group names: 12-byte references `{key, flags, user}`) comes from
`entries\<file>.refs` when that file exists, else it stays the archive's.

**New worlds.** A level is a `.wog` group naming `.wol` lists naming world keys; a world is its package `FFF<X>`
(the world record first, then its records), texture bank `FEF<X>`, sound bin `FDF<X>` and language index `FCF<X>`,
where `X` is the low 20 bits of the world key. Rebuilt worlds under new keys go into `entries\` with a new list
(`.wol` + `.refs`) and a group that names it; the "Test world" example (level A4 rebuilt with its scenery stripped)
is built by `build_test_world.py` from the converted archive and hooks itself into the hub's A4 exit by replacing
`A4.wog`'s references.

**`records\<KEY>.bin`** replaces one record inside the world packages, in every package that holds the key: the body
that follows the record's `{key, length}` words. These are exactly the files of the converter's `script_overrides`
layers, so a compiled script record (a model, a procedure list, a rule list, a data set) drops in as it is - this is
how a **native-script mod** ships: the records compiled by the scripts tooling from the game's own script language.
Replacing only; a record the package lacks cannot be added yet.

The loader logs what it serves in `wiimote_log.txt` (`[general] log=1`): `FFF01D10 served from Hub text mod (1 record)`.

What is loaded before the DLL is (the very first world) cannot be replaced; every world loaded after that can.

## Lua

`main.lua` runs once, on the game's main thread, at the first frame the DLL sees (the title screen is not up yet).
Every mod has its own Lua state; errors go to `wiimote_log.txt` and never stop the game. The standard libraries are
open; `require("name")` looks for `name.lua` in the mod's folder. `print` writes to the log.

The `wc` table:

| Call | What it does |
|---|---|
| `wc.log(...)` | a line in `wiimote_log.txt` (`LUA <mod>: ...`) |
| `wc.on("frame", fn)` | `fn(dt)` every frame, `dt` in seconds |
| `wc.every(seconds, fn)` | `fn()` at that period |
| `wc.time()` | seconds since the game started |
| `wc.key(vk)`, `wc.pressed(vk)` | the keyboard while the game has the focus: held / went down this frame. `wc.vk.SPACE`, `wc.vk.F5`, `wc.vk.A`, `wc.vk.N1` ... hold the codes |
| `wc.text(id, x, y, str [, argb])` | a line of text over the picture until `wc.text(id)` removes it; `x, y` in pixels of the picture (`wc.screen()` gives its size); `id` any string |
| `wc.screen()` | the picture's width and height |
| `wc.screenshot(path)` | saves the picture, texts included, at the next frame (`.png`, `.jpg`, `.bmp`) |
| `wc.mem.u8/u16/u32/i32/f32(addr)` | reads the process memory (`nil` when the address is not readable) |
| `wc.mem.write_u8/u16/u32/i32/f32(addr, v)` | writes it (`false` when not writable); `wc.mem.patch(addr, bytes)` writes code |
| `wc.mem.string(addr [, max])` | a C string |
| `wc.game.fps_cap([hz])` | the frame rate cap (get / set); `wc.game.afx([on])` the Wii picture effects; `wc.game.high_detail(on)` |
| `wc.natives[name]` | the word of an engine script native (2168 names) |
| `wc.native(name, fn)` | `fn(vm)` runs in place of the native each time a script calls it (below) |
| `wc.call(name, ...)`, `wc.callf(name, ...)` | calls an engine script native from Lua; the 4-byte result as an integer / a float, a vector result as three numbers `x, y, z`, `nil` for none. Arguments: numbers, booleans, strings for pointer arguments, `{x, y, z}` tables for vector arguments, given in the order of the native's C signature (e.g. `wc.call("CAM_ViewPosGet", 0)` gives the camera position, `wc.call("ViD_SectoByPosGet", {x, y, z})` the sector at a point, `wc.call("RAY_CastWorld", {x, y, z}, {0, 0, -1}, flags)` casts a ray) |
| `wc.mod`, `wc.dir` | the mod's name and folder |

**Native hooks.** The game's scripts call engine functions ("natives": `IO_JoystickButtonPressed`, `SND_PlaySound`,
`MGM_SetIntProperty` ...). `wc.native(name, fn)` puts `fn` in the engine's native table for that name; the `vm`
object it receives is the script stack: `vm:arg_int(i)`, `vm:arg_float(i)`, `vm:arg_string(i)` look at argument `i`
(0 = the first) without taking it; `vm:pop_int()` / `vm:pop_float()` take the arguments, last first, as the engine
does; `vm:push_int(v)` / `vm:push_float(v)` / `vm:push_string(s)` give the result; `vm:original()` runs the engine's
own handler with the arguments still on the stack. A hook that only looks (no pop, no `original()`) makes the call a
no-op returning 0. `vm:name()` and `vm:word()` say which native this is when one function serves several.

```lua
-- count the button reads and let the engine answer
local reads = 0
wc.native("IO_JoystickButtonPressed", function(vm)
    reads = reads + 1
    vm:original()
end)

-- answer a native yourself: IO_JoystickButtonPressed(id, button) -> 1 while F9 is held
wc.native("IO_JoystickButtonPressed", function(vm)
    local button = vm:pop_int()      -- last argument first
    local id = vm:pop_int()
    vm:push_int(wc.key(wc.vk.F9) and 1 or 0)
end)

wc.on("frame", function(dt)
    wc.text("hud", 20, 20, string.format("%d button reads, cap %d fps", reads, wc.game.fps_cap()))
    reads = 0
end)
```

The native names and their argument counts come from the game's own tables (`platform/wm_natives_table.cpp`); a
call with the wrong number of arguments is refused with the count.

## Settings (`wiimote.ini`)

    [mods]
    enabled=1     ; 0 = mods off
    lua=1         ; 0 = the scripts do not run
    log=1         ; what the loader serves and what the scripts log

## Making the files

* Entries and records come out of the converter's tooling (`rghport`): a converted archive's entries can be extracted,
  changed and dropped into `entries\`; script records are compiled from script sources into `records\` (the
  `script_overrides` layers are made the same way).
* Everything a mod contains is read from disk at every start: edit, restart the game, see.
