# Platform DLL (wiimote.dll): Wii remote input and Wii save layer for the RGH PC executable

Source of truth: the retail RGH PC executable (2010 build, PE32, image base 0x00400000, SHA-256
`208054AF049A1E72EBEA3ADC7FA9BC5FABAD162C47861E52FE81B86FACEED502`), studied through a Ghidra export. Code missing
from the export (small virtuals, callbacks, thunks) was disassembled with capstone straight from the file. All
addresses below are virtual addresses in that executable; Wii addresses are those of the retail Wii executable (with
symbols).

**Sources in `platform/`**:
* DLL: `wiimote.h wm_main.cpp wm_input.cpp wm_proto.cpp wm_patch.cpp wm_sav.cpp wm_ctl.cpp wm_video.cpp wm_afx.cpp
  wm_timing.cpp wm_script.cpp wm_options.cpp wm_gfx.cpp wm_hang.cpp wm_fixes.cpp wiimote.def`.
* Configuration template: `wiimote.ini`.
* Offline test: `wmtest.cpp`.
* `build.bat`: finds MSVC with vswhere and builds `wiimote.dll`, `wmtest.exe` and a copy of `wiimote.ini` into
  `platform/build/`.

**Test files**, all written next to `wmtest.exe`:
* `wmtest` writes `test\wmtest.ini`.
* `wmtest sav` writes `test\wmsav.ini` and `test\savtest\sav\`.
* `wmtest ctl` writes `test\wmctl.ini`.
* `wmtest video` and `wmtest timing` write `test\wmvideo.ini` and `test\wmtiming.ini`.

The Wii save converter is `rghport import-save` (§12.7).

**Save layer: wiimote.dll also implements the Wii save natives (SAV_*) on PC save files in `<exe dir>\sav\`.
See §12.**

**Controls mode: `[controls] mode=pc` turns the virtual remote off and serves the Wii pointer and motion natives from
the engine's mouse, for PC-native controls. See §13. Analyses (RE only): frame timing §14, Bink videos §15.**

**Video natives and video log: `[video] natives=1` gives the ten stubbed K3D_Bink current-video and preload natives
their Wii behaviour, and `[video] log=1` logs every played video and video script call. See §16.**

**Frame pacing: `[timing] fps_cap=60` (default) waits inside the engine's frame time, so the game runs at 60 Hz with a
1/60 DT. See §17.**

**Wii after effects: `[video] afx=1` (default) draws the levels' colour grade, soft glow and tint like the Wii, and
`rghport assemble` gives the shaders the Wii's lighting and glow arithmetic. See §19.**

**Options screen: with `[controls] mode=pc` and `[options] enabled=1` (default) the pause menu's OPTIONS entry (added
by the converter) opens a screen with control bindings (up to three inputs per action), four volumes and graphics
settings (display mode, window size, VSync, high detail, texture filtering, Wii effects), saved in `options.ini`.
See §20.**

**Always on: widescreen (the picture fills the window instead of a centred 1280x720 box) and no controller rescans
every 2 s (the stutter), each only when the executable matches. See §21.**

---

## 0. Short answers

* **The PC executable has three Wii remote paths. The game's scripts use only one of them, and it is not the wiimote.dll exports.**
  1. *wiimote.dll exports* (`Wrap*`): Gear's `Gear::Input::GamePadWii` devices call them. The game's input layer
     **ignores those devices**: its connect callback and its Gear backend accept only devices that are
     `IsKindOf("GamePadWiiDevkit")`, and the type chain of GamePadWii is
     GamePadWii → VibrationEnabledGamePad → HardwareGamePad → … (§1.2).
  2. *Devkit proxy pipe* `\\.\pipe\WiimoteProxyPcPipe` → `Gear::Input::GamepadWiiDevkit` devices. The game accepts
     these, but only copies one status per frame and never sets the sample count (§1.3).
  3. *KPAD compiled into the exe on top of a WPAD layer that is a TCP client of 127.0.0.1:4242* (§1.4). When no
     devkit pad exists, the game's Wii remote backend reads this path every frame: up to 16 KPAD samples, sample count,
     pointer, horizon, distance, both accelerometers, the Nunchuk stick. It feeds script ids 20.. (slot type 2).
* **What the DLL does**: its WPAD server on 127.0.0.1:4242 streams one virtual Wii remote + Nunchuk (channel 0) at
  200 Hz. The raw data is synthesised so that the exe's own KPAD code outputs the intended pointer, horizon, distance,
  acceleration, stick and buttons, from mouse, keyboard and XInput. The server starts during the Gear init that loads
  wiimote.dll, which runs before the engine's KPADInit connects. All 46 `Wrap*` exports are also implemented
  (KPADStatus 0xF0, cdecl), behind `gear_pad=1` (off by default, the game ignores them).
* **Build**: MSVC x86 (found with vswhere), `/MT`, PE32 i386, 46 engine export names plus nine test-only exports.
* **Offline tests**:

  | test | result | section |
  |---|---|---|
  | `wmtest` | 31/31 PASS; it plays the engine's side of the protocol exactly and decodes the samples with a port of the executable's KPAD code | §8 |
  | `wmtest sav` | 84/84 + 21/21 PASS | §12.9 |
  | `wmtest ctl` | 20/20 + 21/21 PASS | §13.6 |
  | `wmtest video` | 44/44 + 16/16 PASS | §16.6 |
  | `wmtest timing` | 8/8 + 11/11 PASS | §17.5 |

* **In game**: the Wii menus and the save layer run. The pc controls mode, the video natives and the frame pacing are
  not tested in game yet.
* **Open questions**: §11.

---

## 1. How the engine reaches a Wii remote

### 1.1 wiimote.dll loading

| what | address |
|---|---|
| `Gear::Input::InteractiveDeviceManager::Shadow` ctor, calls the loader with ECX = Shadow+0x38 | `FUN_007EC750` |
| loader: `LoadLibraryA(*009D9BA4 = "wiimote.dll")`, then 46 `GetProcAddress` | `FUN_007B7110` |
| table pointer (= Shadow+0x38) | `DAT_00A9A528` |
| Gear input manager init: Shadow ctor, `DirectInput8Create`, then 3 DLL calls | `FUN_007E18D0` |

* **When**: during Gear input manager init, before the game's IO init. The game's `FUN_006F66E0` asserts "Gear called before
  initialization", and KPADInit's tail `FUN_007F1F80` calls through the table. The first calls are
  `WrapWPADRegisterAllocator(0,0)`, `WrapKPADInit()` and `WrapKPADEnableAimingMode(0)`. They are skipped if
  `DirectInput8Create` fails.
* **Missing DLL or export**: the table slot is NULL. Every wrapper (`007B7580..007B7AC0`) and direct call tests for
  NULL and then returns 0, -2 or 0.0 or does nothing, so the engine keeps running. Side effect: the Gear device scan
  `FUN_007DF330` skips its "0 samples" test when the read pointer is NULL. It then checks bytes of an
  **uninitialised** stack buffer and may create phantom GamePadWii devices. A stub DLL (void functions leaving
  garbage in EAX) has the same effect. Both are harmless for the game.
* **Calling convention**: cdecl throughout (the caller cleans the stack). Floats go on the stack as 4-byte values.
  `WrapKMPLSIsEnable*` return a float in ST0; the engine tests `>= 0.0` as "enabled".

Table layout (offset from `DAT_00A9A528`):

| off | export | off | export | off | export |
|---|---|---|---|---|---|
| +00 | HMODULE | +40 | KPADDisableMpls(chan) | +80 | KMPLSDisableZeroDrift |
| +04 | KPADRead(chan, buf, len) → s32 | +44 | KMPLSIsEnableZeroPlay(chan) → f32 | +84 | KMPLSSetZeroDriftParam(chan,f,f,f) |
| +08 | KPADReadEx(chan, buf, len, s32 *err) | +48 | KMPLSIsEnableZeroDrift | +88 | KMPLSInitZeroDriftParam |
| +0C | KPADInit() | +4C | KMPLSIsEnableDirRevise | +8C | KMPLSEnableDirRevise |
| +10 | KPADEnableAimingMode(chan) | +50 | KMPLSIsEnableAccRevise | +90 | KMPLSDisableDirRevise |
| +14 | KPADSetPosParam(chan, f play, f sens) | +54 | KMPLSIsEnableDpdRevise | +94 | KMPLSSetDirReviseParam(chan,f) |
| +18 | KPADSetDistParam(chan,f,f) | +58 | KMPLSGetZeroPlayParam(chan, f*) | +98 | KMPLSInitDirReviseParam |
| +1C | KPADSetAccParam(chan,f,f) | +5C | KMPLSGetZeroDriftParam(chan, f*, f*, f*) | +9C | KMPLSEnableAccRevise |
| +20 | KPADSetHoriParam(chan,f,f) | +60 | KMPLSGetDirReviseParam(chan, f*) | +A0 | KMPLSDisableAccRevise |
| +24 | WPADRegisterAllocator(a, f) | +64 | KMPLSGetAccReviseParam(chan, f*, f*) | +A4 | KMPLSSetAccReviseParam(chan,f,f) |
| +28 | WPADControlMotor(chan, cmd) | +68 | KMPLSGetDpdReviseParam(chan, f*) | +A8 | KMPLSInitAccReviseParam |
| +2C | WPADShutdown() | +6C | KMPLSEnableZeroPlay(chan) | +AC | KMPLSEnableDpdRevise |
| +30 | WPADGetSensorBarPosition() → u8 | +70 | KMPLSDisableZeroPlay | +B0 | KMPLSDisableDpdRevise |
| +34 | KMPLSRead | +74 | KMPLSSetZeroPlayParam(chan, f) | +B4 | KMPLSSetDpdReviseParam(chan,f) |
| +38 | KPADGetUnifiedWpadStatus | +78 | KMPLSInitZeroPlayParam | +B8 | KMPLSInitDpdReviseParam |
| +3C | KPADEnableMpls(chan, u8 mode) | +7C | KMPLSEnableZeroDrift | | |

No caller was found for +2C, +34 or +38.

### 1.2 Gear::Input::GamePadWii (the wiimote.dll path)

* **Scan** `FUN_007DF330`, every frame from `FUN_007E0110`: for each channel 0..3 without a device it calls
  `KPADRead(chan, local 0xF0 buffer, 1)`. With count ≠ 0, `wpad_err` (+0x5D) ≠ -1 and `dev_type` (+0x5C) ≠ 0xFD, it
  creates `Gear::Input::InteractiveDeviceManager::Pad` (0x218, ctor `FUN_007DC9B0`, vtables 008E9888 / 008E9858).
  The ctor allocates 16 × 0xF0 "KPADStatus" plus a 0x74-byte MotionPlus parameter block.
* **Update**, interface vtable +0x28 = `007D6810`, per device per frame:
  `WPADGetSensorBarPosition()` → `KPADRead(chan, buf, 16)` → `KPADReadEx(chan, buf, 16, &err)`.
  Then the MotionPlus state machine: `KPADEnableMpls(chan, 5)` / `KPADDisableMpls`, the five `KMPLSIsEnable*`, the
  five `Get*Param`, and Enable/Disable/Set/Init on request. Then `KPADSet{Pos,Dist,Acc,Hori}Param(chan, play, sens)`
  whenever the stored values are non-zero. It counts a disconnect after more than 5 empty reads. Conversion into
  Gear fields: `007C3CF0` (hold bits → button bytes, acc at +0x0C, …). Motor: `FUN_007BC1C0` → `WPADControlMotor`.
* **Why the game never sees it**: the game's Gear connect callback `006F5A50` and its Gear backend `FUN_006F6300` call
  `IsKindOf(TypeId("GamePadWiiDevkit"))`. TypeIds are interned strings (`FUN_007C58A0`). GamePadWii's IsKindOf
  (`007EBA10`) walks "GamePadWii" → `007EB960` "VibrationEnabledGamePad" → `007EB850` "HardwareGamePad" →
  `007EADB0` … and never meets "GamePadWiiDevkit".

### 1.3 Devkit proxy (named pipe)

`FUN_007EBC20` always creates `Gear::Input::WiimoteDevkitManager` (`FUN_007D7320`, 0xD5C bytes). Its thread
(`007C02C0`) retries `CreateFileA("\\.\pipe\WiimoteProxyPcPipe")` every 5 ms. Once connected it loops
`ReadFile(pipe, manager+0xD4C, 0x400)` and then writes a request (`FUN_007BC470`, "GEARPADR" serializer `FUN_007CEF20`).
`FUN_007DCC00` creates `Gear::Input::GamepadWiiDevkit` (ctor `FUN_007DCB40`, vtables 008E9A10 / 008E99DC,
IsKindOf `007EBAE0` "GamePadWiiDevkit"). The game accepts these. `FUN_006F6300` then fills only record 0 of the channel
(`FUN_006F60E0`: dev_type 6, format 0x10) and never updates the sample count, so the sample natives return nothing.
Not used by this DLL.

### 1.4 KPAD in the exe + WPAD over TCP (the path the DLL serves)

* The game's Wii remote backends form a chain (`DAT_00A99DEC`): object 009D8F34 (vtable 008CD524, update `FUN_006F6300` =
  Gear devkit pads), then object 009D8F28 (vtable 008CD500, init `FUN_006F52C0`, update `FUN_006F5220`). Update
  `FUN_006F5BE0` stops at the first backend that returns non-zero. Without devkit pads the Gear backend returns 0 and
  the internal backend runs every frame.
* Init `FUN_006F52C0` → **KPADInit `FUN_007F2050`** → `FUN_007F2940`. That allocates the WPAD object
  (`DAT_00A9F69C`, CRT operator new, not zeroed, ctor `FUN_007F3880`: 4 × 0xA0 channel structs, dev_type 0xFD) and
  `FUN_007F40C0(0x1092)` starts thread `FUN_007F3CD0`. KPADInit then loops on WPADGetStatus (`0059D3B0`, always 3),
  clears the KPAD channel blocks (`00AAB2E0 + chan*0x57C`) and sets the defaults: DPD enabled +0x564 = 1, centre
  +0xBC = 0, +0xC0 = ±0.2 from the sensor bar byte, … At the end, `FUN_007F1F80` calls
  `WrapWPADControlMotor(chan, 0)` for chans 3..0 through the DLL table.
* **The thread connects once**: `WSASocket` → `SO_REUSEADDR` + `TCP_NODELAY` → `WSAConnect(127.0.0.1:4242)`. If that
  fails the thread exits and there is no retry for the rest of the session. After connecting: `WSAEventSelect`
  (non-blocking) and a loop on {socket event, send event, stop event}.
* The game's IO init `FUN_006ED260` (called from `FUN_006CC700`): `FUN_006F66E0` (backend init, so KPADInit and the
  connect) → **`Sleep(100)`** → DirectInput create → backend update → `FUN_006F53B0` (Wii remote slots) → XInput poll
  → DirectInput poll. **A remote that is connected within those 100 ms gets slot 0**, before any pad.

---

## 2. WPAD TCP protocol (engine = client, DLL = server, little-endian, raw struct bytes)

### 2.1 Server → engine

The engine reads **one message per FD_READ event**: 1 type byte, then fixed-size `recv` calls on a non-blocking
socket. **Any short read or FD_CLOSE closes the socket and ends the thread for good.** So every message must reach
the engine whole; the DLL sends each message with a single `send()` on loopback.

| type | handler | payload after the type byte |
|---|---|---|
| 1 hello | `FUN_007F3970` | `u8 sensorBarPos` (obj+0x29C) · `s32 motorEnabled` (==1, else the engine sends motor-stop for all 4 channels) |
| 2 connect | `FUN_007F31B0` | channel info · clears the sampling and extension callbacks, calls the connect callback(chan, 0) |
| 3 disconnect | `FUN_007F3210` | channel info (dev_type 0xFD) · connect callback(chan, -1) |
| 4 extension | `FUN_007F3270` | channel info · extension callback(chan, dev_type) |
| 5 info result | `FUN_007F32C0` | channel info · copies ch+0x18..+0x2F to the WPADGetInfoAsync buffer, callback(chan, 0) |
| 6 control done | `FUN_007F3340` | channel info · callback ch+0x70 (the WPADControlDpd completion `FUN_007F2000`) |
| 7 info update | `FUN_007F3390` | channel info only |
| 8 sample | `FUN_007F3A30` | `s32 chan` · if ch.dev_type ∈ {0,1}: WPADStatus (below) · if dev_type == 1: FS extra · queue, then call the sampling callback |

**Channel info** (`FUN_007F2D50`, 76 bytes, written into the WPAD channel struct `ch = obj + chan*0xA0`):

| bytes | field | ch+ | DLL sends |
|---|---|---|---|
| 4 | s32 chan | — | 0 |
| 6 | BD address | +0x04 | 00:19:1D:57:49:49 |
| 4 | s32 dev_type | +0x0C | 1 FREESTYLE (0 without Nunchuk, 0xFD disconnected) |
| 4 | s32 data_format | +0x10 | 0, then what the engine requests |
| 4 | u8 DPD enabled + 3 | +0x14 | 0, then 1 after ControlDpd |
| 5×4 | s32 dpd, speaker, attach, lowBat, nearEmpty (stored as `== 1`) | +0x18..+0x28 | 0/0/1/0/0 |
| 4×1 | battery, led, protocol, firmware | +0x2C..+0x2F | 4, 1<<chan, 0, 0 |
| 6×2 | s16 acc zero x,y,z | +0x30 | 0,0,0 |
| 6×2 | s16 acc 1g x,y,z | +0x36 | 25,25,25 → gravity unit 100 |
| 12×2 | s16 ×6 (not read by this KPAD) | +0x3C | 0,0,0,50,50,50 |
| 6×1 | u8 ×6 (not read by this KPAD) | +0x48 | 0 |

**Sample** (queued as WPADFSStatus, 50 bytes; wire order):
`u16 button` · `s16 accX, accY, accZ` · 4 × (`s16 x`, `s16 y`, `u16 size`, `u8 traceId`, 7 bytes, no padding
byte) · `u8 dev` · `s8 err` · [FS] `s16 fsAccX, fsAccY, fsAccZ` · `s8 fsStickX, fsStickY`.
That is 43 bytes after the type byte for core, 51 with the Nunchuk. `size == 0` means no IR object.

### 2.2 Engine → server commands

12 bytes `{u32 cmd, u32 chan, u32 arg}`, queued by `FUN_007F3920` and sent by the thread:

| cmd | sender | meaning | DLL answer |
|---|---|---|---|
| 3 | `FUN_007F29E0` WPADSetDataFormat | format wanted by KPAD | store, send type 7 |
| 4 | `FUN_007F2AE0` WPADControlDpd(chan, cmd, cb `FUN_007F2000`) | DPD mode | store DPD enabled = (arg≠0), send type 6 |
| 5 | `FUN_007F2B40` WPADControlMotor (only if hello said motor enabled) | 1 rumble / 0 stop | XInput vibration |

### 2.3 Handshake driven by KPAD's sampling callback (`007F21D0`, runs on the WPAD thread for each sample)

It takes the probe dev type (`FUN_007F2990`: ch+0x0C ≠ 0xFD) → index 0 core / 2 FS / 4 classic, +1 while KPAD DPD is
enabled (+0x564, set to 1 by KPADInit) → table `009DA8EC` (DPD cmd, format) =
(0,1) (3,2) (0,4) **(1,5)** (0,7) (1,8). While `(ch.dpdEnabled ? last cmd : 0)` ≠ wanted, it sends ControlDpd once
(busy flag +0x563 cleared by message 6). Otherwise, while the sample's format (ch+0x10) ≠ wanted, it sends
SetDataFormat. With the Nunchuk the result is **DPD 1, format 5 (FREESTYLE_ACC_DPD)**. KPAD ignores the IR data
before format 2/5/8, the Nunchuk accelerometer before 4/5, and the stick before 3/4/5.
WPADRead (`FUN_007F2C30`) pops the newest queued sample.

---

## 3. KPAD model (what the synthesis inverts)

KPADRead `FUN_007F2360(chan, buf, len)` walks the KPAD ring buffer (16 × 0x3C at channel +0x114; write index +0x112,
count +0x113, cleared per read) oldest first. It writes up to `len` KPADStatus of **0x88 bytes**, newest at index 0,
and returns the count.

| step | function | rule |
|---|---|---|
| gravity unit | `FUN_007F2A30` | remote: ((1g−zero)×4) per axis, z uses the y calibration; any 0 → default scale 0.01. Nunchuk: always the default 0.005 |
| remote acc | `FUN_007F0990` | KPAD (x,y,z) = (−accX, −accZ, +accY) × scale, clamp ±3.4 g |
| Nunchuk acc | `FUN_007F0990` | needs err 0, dev 1, format 4/5: (−fsX, −fsZ, +fsY) × 0.005, clamp ±2.1 g |
| stick | `FUN_007F1DD0` → `FUN_007F1C90` | needs dev 1, format 3/4/5: per axis \|raw\| ≤ 15 → 0, ≥ 71 → 1, linear between, sign kept, vector clamped to length 1 |
| buttons | `FUN_007F0540` | hold = (C/Z 0x6000 from the last valid extension sample) \| (core & 0x9FFF); trig/release |
| IR objects | `FUN_007F0D40` | size≠0 → kobj = (x/512 − 0.9990234375, y/512 − 0.7490234375) |
| window | `FUN_007F0DB0` | usable only while −0.95 < x < 0.95 and −0.7 < y < 0.7 |
| gate | `FUN_007F19E0` | DPD only for format 2/5/8 and acc_vertical.x = √(ax²+ay²)/\|a\| > 0.7 |
| acc horizon | `FUN_007F0620` | target (−ay, −ax)/√(ax²+ay²), smoothed 0.05 per sample |
| pairing | `FUN_007F0F10` / `FUN_007F1090` | pair distance d → est = K/d with **K = 0.2 / 0.38386398553848267 = 0.5210** (0.2 = obj interval `009DA898`); needs K < est < 3.0 and \|dir·acc horizon\| > 0.9 |
| agreement | `FUN_007F19E0` | invalid if acc horizon · DPD horizon ≤ 0.9 (skipped for 100 samples after the acc moves) |
| output | `FUN_007F14D0` / `FUN_007F1590` | horizon h = (dir.x, −dir.y); dist = K/d; mid = (p1+p2)/2; pos.x = (0 − (mid.x·h.x − mid.y·h.y))·S; pos.y = (cy − (h.x·mid.y + mid.x·h.y))·S; then smoothing with the pos/hori/dist play radius and sensitivity (+0x88..+0x9C) |
| scale | `FUN_007F0100` | **S = √1.5625 / min(1−\|cx\|, 0.75−\|cy\|) = 1.25/0.55 = 2.2727** |
| centre | KPADInit | cx = 0, **cy = +0.2 if the WPAD sensor bar byte ≠ 1, else −0.2**. The byte is read before any message can arrive and is uninitialised heap memory. The DLL reads the real value at `00AAB2E0 + chan*0x57C + 0xC0` (exe signature checked at 007F2050), else uses +0.2 |

KPAD pos (−1,−1) is the top-left of the screen (Wii `Bunnies_Pointer_Pos_Convert`: (p+1)/2 → Magma cursor).
Rest (level, buttons up) is acc (0,−1,0), horizon (1,0). The Wii cursor code gives the same angle from
`-acc` and from the horizon when acc = (−sin r, −cos r, 0) and horizon = (cos r, sin r).

**Inverse used by the DLL** (`WpadFromState`):
d = K/dist, u = −pos.x/S, v = cy − pos.y/S, mid = (c·u + s·v, −s·u + c·v) with (c,s) = (cos r, sin r),
dots = mid ∓ (c, −s)·d/2, raw = (k + 0.9990234375, k + 0.7490234375) × 512, size 3.
Reachable at dist 2 m: pos.x ±1.86, pos.y −1.13…+2.0, so the whole screen.
Acc raw = (−x, +z, −y) × 100, Nunchuk (−x, +z, −y) × 200, stick raw = ±(15 + \|v\|·56).

---

## 4. The game's controller layer (script natives)

* Slots (10) `DAT_00A97980` type, `DAT_00A979F8` device index, `DAT_00A979D0` used. Ids 0..9 are slots; ids 20..23
  go through `DAT_00A97A20[20+chan]` (set by `FUN_006F5420`). `FUN_006EC590(2, chan)` registers a remote; the button
  remap is initialised in `FUN_006EC480` → `FUN_006F6530`.
* Internal backend update **`FUN_006F5220`** (every frame): for chan 0..3,
  `DAT_00A97B20[chan] = (probe == 0)`, `DAT_00A97B50[chan] = dev_type`,
  `DAT_00A97B40[chan] = KPADRead(chan, 00A97BEC + chan*0x880, 16)`, `DAT_00A97BE0 = sensor bar`.
* Per frame `FUN_006ED470` (called from `FUN_006CC740`): backend update; **every 2 s** `FUN_006F53B0`
  registers/unregisters slots from `DAT_00A97B20`; then per slot `FUN_006EC890` (buttons: `FUN_006F5450` hold →
  remap), `FUN_006F5F30` (rumble), `FUN_006ED3A0` (stick 0 = `FUN_006F5520`), `FUN_006F55E0` (acc 0 remote, 1
  Nunchuk), `FUN_006F5CA0` (pointer), `FUN_006F5500` (count), `FUN_006F5580`/`5640`/`5D00` (16 stick/acc/pointer
  samples).
* KPADStatus fields the game reads (0x88 layout): hold +0x00, acc +0x0C, pos +0x20, horizon +0x34, dist +0x48,
  dpd_valid_fg +0x5E, data_format +0x5F, fs.stick +0x60, fs.acc +0x68. Probe dev_type 1/6 → 2 acc sensors and 1 stick.

| native | internal | Wii remote result |
|---|---|---|
| `IO_JoystickHere_C` 005ECE40 | slot connected | |
| `IO_JoystickTypeGet_C` 005ED160 | slot type | 2 |
| `IO_JoystickExtensionGet_C` 005ED180 | dev_type ≠ 0 | 2 with Nunchuk |
| `IO_JoystickButtonPressed/JustPressed/JustReleased_C` 005ECE50/70/B0 | `FUN_006EC990/9D0/A10` | remapped bits (§5) |
| `IO_JoystickMove_C`/`StickGet` 005ECEF0 | `FUN_006ECC10` | Nunchuk stick (stick 0) |
| `IO_JoystickAccelGet_C` 005ECFE0 | `FUN_006ECD90` | sensor 0 remote, 1 Nunchuk (type 2 only) |
| `IO_JoystickIsAccelEnable_C` / `AccelMaxGet_C` | | sensor < 2 / 3.4 g remote, 2.1 g Nunchuk |
| `IO_JoystickGetPointer_C` 005ED000 | `FUN_006ECDF0` | (pos.x, pos.y + shift, dist). The shift is added twice (`FUN_006F5CA0` and here); 0 unless a script sets it |
| `IO_JoystickDPDValidGet_C` 005ED020 | `FUN_006F56B0` | dpd_valid_fg if format ∈ {2,5,8,9,16}, else −1 |
| `IO_JoystickGetPointerState_C` 005ED040 | `FUN_006F5710` | 1 ok; −1 no DPD format; −2 not valid; −3 dist < 0.7; −4 dist > 5.0 |
| `IO_JoystickHorizonGet_C` 005ED050 | `FUN_006F5840` | (horizon.x, horizon.y, dist) |
| `IO_JoystickSampleNumberGet_C` 005ED0D0 | `DAT_00A96878` | KPADRead count (≈3–4 per frame at 60 fps) |
| `IO_JoystickStick/Accel/PointerSampleGet_C` 005ED0E0/100/120 | | per-sample values |
| `IO_JoystickSensorBarPositionGet_C` / `PointerShiftSet_C` | `DAT_00A97BE0` / `FUN_006F5890` | |
| `IO_JoystickWiimoteSensibilitySet_C` 005ED0A0 | `FUN_006F5D90(id, which, play, sens)` | which 0 pos `007F0080`, 1 dist `007F00C0`, 2 acc `007F00E0`, 3 horizon `007F00A0` |
| `IO_JoystickRumbleSet_C` 005ED070 | `FUN_006ED1A0` → `FUN_006F5F30` → `FUN_006F5390` | WPAD command 5 |
| `IO_JoystickBatteryStateGet_C` | | constant 0x10003 |

---

## 5. Button masks

| Wii | WPAD/KPAD bit (hold) | script mask (`FUN_006F6530`) |
|---|---|---|
| A | 0x0800 | 0x0001 |
| B | 0x0400 | 0x0002 |
| 1 | 0x0200 | 0x0004 |
| 2 | 0x0100 | 0x0008 |
| Z (Nunchuk) | 0x2000 | 0x0040 |
| C (Nunchuk) | 0x4000 | 0x0080 |
| − (Minus) | 0x1000 | 0x0100 |
| + (Plus) | 0x0010 | 0x0200 |
| D-pad up | 0x0008 | 0x1000 |
| D-pad right | 0x0002 | 0x2000 |
| D-pad down | 0x0004 | 0x4000 |
| D-pad left | 0x0001 | 0x8000 |
| HOME | 0x8000 | 0x40000 |

(D-pad names follow WPAD, i.e. the remote held vertically.)

---

## 6. The DLL

```
PC input (wm_input.cpp) --WmState--> WpadFromState (wm_proto.cpp) --WPAD TCP--> KPAD in the PC executable --> game slots
                               \--> Wrap* exports (wm_main.cpp, gear_pad=1) --> Gear GamePadWii (unused by the game)
```

* **Start**: `DllMain` pins the module (FreeLibrary never unmaps the running thread) and starts a thread that runs
  the one-time init. Every export also runs it (InitOnce). Init loads the ini, binds and listens on 127.0.0.1:port
  synchronously (`SO_EXCLUSIVEADDRUSE`), then starts the I/O thread.
* **I/O thread**: `timeBeginPeriod(1)`, a 200 Hz schedule (catch-up capped, resync after 0.25 s). Per sample: poll
  PC input → WmState → on accept send hello + connect → sample. Commands 3/4/5 are answered (§2.2). A sample is
  dropped rather than queued behind a partially sent message.
* **PC input**: the game window is the largest visible top-level window of the process. Keyboard, mouse and pad are
  read only while a window of the process has focus (`require_focus`). Pointer: the mouse while inside the client
  area, or a virtual pointer driven by the right stick (always valid; mouse movement takes back control).
  Tilt keys ramp the roll at 90°/s so KPAD's smoothed accelerometer horizon keeps agreeing with the dots. Shake: a
  sine burst `shake_g` × sin(2π·f·t) along (0, 0.24, 0.97), finished to whole cycles.
* **Gear exports**: KPADStatus 0xF0 (hold/trig/release derived once per new sample so Read and ReadEx agree, acc,
  pos/vec/speed, horizon, dist, acc_vertical, dev 1, err 0, dpd 2/0, format 5, Nunchuk stick/acc, MotionPlus fields
  zero). Channel ≠ configured, or `gear_pad=0`: dev 0xFD, err −1, count 0. Also: `ReadEx *err`, the MotionPlus
  parameter state (IsEnable → −1.0 disabled), sensor bar from the ini, motor → XInput.
* **Log** `wiimote_log.txt` next to the DLL (`log=1`; `log_verbose=1` adds a state line every 2 s with `sent` and
  `dropped` counters).
* **Hang watch and crash log** (`wm_hang.cpp`, with `log=1`): `hang_watch=1` logs where the main thread is when it
  finishes no frame for 4 s (registers and the return addresses on its stack, by module), again every 2 s while it
  stays stuck. `crash_log=1` (the default) is a vectored exception handler that sees a fatal exception (an access
  violation, an illegal instruction, a stack overflow, on any thread) before any handler and logs `CRASH: exception
  <code> at <module>!<address>` with the registers and the return addresses from the faulting frame up, then writes
  a minidump `crash_<time>.dmp` beside the log and lets the exception go on. Without it a crash looked like a hang:
  the game's own unhandled-exception filter (`00409340`, calling the reporter `00507230`) spins for five seconds
  trying to write a report into a `LyNcrash` folder the port does not have, then dies in its own error path, and the
  hang sample showed only that reporter.
* **Test-only**: env `WIIMOTE_INI` (ini path) and `WIIMOTE_TEST=1` (input only from the export
  `WiimoteTestInject`). The engine never sets these.

---

## 7. Default mapping (`wiimote.ini`)

| Wii | keyboard / mouse | XInput |
|---|---|---|
| IR pointer | mouse inside the game window | right stick (`pad_speed` 1.4 widths/s); LS click = centre |
| A | Space, Enter, left mouse button | A |
| B | Left Shift, right mouse button | B, RT |
| Z | Left Ctrl | LT |
| C | C | LB |
| 1 / 2 | 1 / 2 | RB / Y |
| + / − | Tab / Esc | Start / Back |
| HOME | H | RS click |
| D-pad ↑ ↓ ← → | I K J L, Numpad 8 2 4 6 | D-pad |
| Nunchuk stick | W A S D, arrows | left stick (radial dead zone 0.25) |
| shake | F, middle mouse button | X |
| tilt (roll) ∓35° | Q / E | — |
| connect toggle | (unbound) | — |

Other keys: `channel`, `nunchuk`, `rate_hz`, `dist` (0.8–2.9 m), `sensor_bar`, `kpad_center_y`,
`shake_g/shake_hz/shake_target`, `tilt_deg/tilt_speed_deg`, `rumble`, `gear_pad`, `[xinput] pad/deadzone`.
Bindings accept up to 6 names in a comma list. PC-code models also read the keyboard, mouse and pads directly, so the
defaults keep the meaning of the PC keys (Space dash, Shift boost, Ctrl protect, Tab swap, Esc pause).

---

## 8. Build and offline test

`build.bat` builds `wiimote.dll` and `wmtest.exe` into `platform/build/`. It locates Visual Studio with vswhere,
resets PATH and calls vcvars32, then compiles with `/MT /O2`, linking ws2_32, winmm and user32. Result: PE32
i386, 190 KB, no CRT dependency. The export names were checked against the strings in the PC executable: 46 of 46, plus
`WiimoteTestInject`.

`wmtest.exe` loads the DLL (port 42424, test ini in `test\`), calls the exports in the engine's order and connects the
way `FUN_007F3CD0` does. It enforces one message per FD_READ and treats a short read as fatal. It answers like the
KPAD sampling callback and decodes every sample with a port of the KPAD code in §3 (first acquisition, no smoothing).
Result **31/31 PASS**:
hello; connect; channels 1..3 absent; gravity unit 100; DPD/format handshake reaching format 5 / DPD 1; 200.1 Hz
(the DLL's own counter says 400 samples every 2 s, 0 dropped); rest acc (0,−1,0) for both sensors; pointer centre
(0, −0.0004) with horizon (1,0) and dist 2.006; a 5×5 grid (−1..1) with worst error 0.002; roll ±25° with horizon
angle ±24.84°; invalid pointer gives no objects and DPD 0; all 13 buttons; 8 stick vectors within 0.02; shake peak
3.38 g with 76 samples above 1.45 g and the Nunchuk untouched; motor commands; Gear exports.
The test leaves `wiimote_log.txt` next to the DLL. Delete it before copying the DLL.

---

## 9. In-game test plan (input)

Deployment: copy `wiimote.dll` and `wiimote.ini` **next to the PC executable that is run** (LoadLibrary finds the
executable's folder first). Set `log=1`, `log_verbose=1` for the first runs.

1. **Connection** (log): `WPAD server listening on 127.0.0.1:4242`, `engine WPAD client connected`,
   `-> connect: channel 0 dev_type 1`, `<- ControlDpd(0, 1)`, `<- SetDataFormat(0, 5)`,
   `KPAD DPD centre: read from the PC executable`, then state lines with `fmt=5 dpd=1`, `sent` +400 every 2 s, `dropped=0`.
   With no `client connected` line, the engine never reached KPADInit or something else holds port 4242.
2. **Slot**: `IO_JoystickHere(20) == 1`, `IO_JoystickTypeGet(0) == 2` (slot 0 is the remote, also with a pad
   plugged in), `IO_JoystickExtensionGet(20) == 2`, `IO_JoystickSampleNumberGet(20)` 2–5.
   MMB_Init must pick the Wii scheme (`ai_groups_Joy[0] = 0`, control type 2).
3. **Pointer**: mouse in the window → `IO_JoystickDPDValidGet(0) == 2`, `IO_JoystickGetPointerState(0) == 1`;
   corners → `IO_JoystickGetPointer(0)` ≈ (±1, ±1), with y = −1 at the top; outside the window → valid 0.
   `IO_JoystickHorizonGet(0)` ≈ (1, 0, 2.0). Q/E → horizon angle ±35° while the pointer stays valid. Right stick
   moves the pointer; LS click centres it.
4. **Buttons**: `IO_JoystickButtonPressed(20, mask)` for each row of §5.
5. **Stick**: WASD / left stick → `IO_JoystickMove(20, 0)` unit vectors, +y = up.
6. **Motion**: `IO_JoystickAccelGet(20, 0)` and `(20, 1)` ≈ (0, −1, 0). F / MMB / pad X → samples reach about 3 g
   (`IO_JoystickAccelSampleGet`), and the protect action (`Joy_ShakePAD`) fires. Tune `shake_g`/`shake_hz` if a
   minigame needs a different gesture.
7. **Menus**: a Wii Magma page driven by `Bunnies_UpdateMagmaCursor` (language, hub) follows the mouse and rotates
   with Q/E; left click = A.
8. **Rumble**: `IO_JoystickRumbleSet(20, 0, 1, t)` vibrates the XInput pad (`log_verbose` shows ControlMotor).
9. **Negative cases**: `enabled=0` → the PC build's behaviour (no id 20). `require_focus=1` → alt-tab stops input.
   Unplugging and replugging the pad works.
10. **Conflicts**: with a pad connected the game also sees it as a type-3 controller (ids 10..). If PC-code models react
    twice, set `[xinput] enabled=0`.

---

## 10. Real Wii remote over Bluetooth HID (later)

The TCP server takes raw WPAD samples, so a HID source can feed `WpadRaw` directly and skip the inverse model:

* `wm_hid.cpp` + `[general] source=pc|hid|auto`. Enumerate with SetupAPI/`HidD_GetAttributes`: VID 0x057E,
  PID 0x0306 (RVL-CNT-01) or 0x0330 (-TR). Open overlapped. Send output reports with `WriteFile` (22 bytes on the
  Microsoft stack; `HidD_SetOutputReport` as fallback).
* Setup: status 0x15, LEDs 0x11. Read calibration from EEPROM 0x0016 (report 0x17; zero/1g are 8-bit + 2 LSBs).
  IR on (0x13/0x1A, register 0xB00030 = 0x08, sensitivity blocks 0xB00000/0xB0001A, mode 0xB00033 = 3). Nunchuk
  init: 0xA400F0 = 0x55, 0xA400FB = 0x00, id at 0xA400FA. Reporting mode 0x37 (buttons + accel + IR 10 bytes +
  extension 6 bytes), continuous.
* Mapping: buttons already use the WPAD bit order. accX/Y/Z = raw10 − zero10, with channel-info calibration set so
  that (1g − zero)×4 equals the remote's counts per g. IR x,y as camera pixels, size 3 in basic mode, traceId = index.
  Nunchuk: stick = raw − centre (KPAD maps \|v\| 15..71), acc = raw10 − zero10 (≈200 counts/g, matching KPAD's
  default 0.005), C/Z bits inverted into 0x4000/0x2000.
* Forward each 100 Hz report as one sample (KPAD accepts any rate) or repeat to 200 Hz. Engine commands: motor → the
  rumble bit in every output report; ControlDpd → IR camera on/off; SetDataFormat → reporting mode 0x31/0x33/0x37.
  Extension status report 0x20 → message 4 with the new dev_type; loss of the device → message 3. Pairing is left to
  Windows (1+2 sync).

---

## 11. Open questions / risks

1. **In-game coverage.** The Wii menus (pointer, buttons) run in game. The offline test proves framing and the KPAD
   math as read from the decompile; gestures, KPAD's one-dot estimation and the real timing of the engine thread
   need more in-game testing.
2. **One connection per session**: the engine's WPAD thread never reconnects. If the DLL is disabled at start, port
   4242 is taken, or the connection drops, restart the game.
3. **Pointer centre**: KPAD's cy (±0.2) comes from an uninitialised byte. The DLL reads the engine's value when the exe
   matches the signature at 007F2050. On a patched exe it falls back to +0.2; with a wrong centre the pointer is off by
   0.45 vertically, so set `kpad_center_y` by hand.
4. **Slot order**: if the remote connects later than the 100 ms sleep of the IO init, an XInput/DirectInput pad can
   take slot 0. Wii code that uses id 0 (e.g. `Bunnies_UpdateMagmaCursor`) would then read the pad. Check test plan
   step 2.
5. **Gesture tuning**: the exact shake rules of `Shake_LIB`/`Wii_LIB` (sample windows, axes) were not reverse
   engineered; defaults give a 3 g peak at 6 Hz. The sign of the tilt keys (which way is "right") is unverified.
   Pointing up or down (acc_vertical) is not modelled, so the remote is always level.
6. **Double input** from PC-code models that read the pad or keys directly (§7, §9.10).
7. The Gear path (`gear_pad`) and the devkit pipe are unused by the game. The devkit protocol (0x400-byte blob,
   "GEARPADR" requests) was not decoded.

---

## 12. Wii save layer (`wm_sav.cpp`)

Source of truth: the PC executable (Ghidra export plus capstone on the file) and the Wii executable (symbols;
disassembled with capstone). Script usage: the save natives the Wii scripts call and the GST_SaveManager script,
decompiled from the game's own bigfile; the converted bigfile runs the PC bytecode of GST_SaveManager.

### 12.1 Why

The RGH PC build ships the Wii save layer as stubs:

| native (word) | PC handler | PC behaviour |
|---|---|---|
| SAV_InitSystem (1E26) | 005D6C90 | pops n, calls 006945B0 = `mov eax,1; ret` (SAV_InitSystem_C 005D5BE0 jumps there) |
| SAV_Step (1E29) | 005D6D90 | pushes the 1 of 006945B0 |
| SAV_GetSlotDataEx (1E20) | 005D64B0 | pops 3, pushes 0 (005EC610 = `xor eax,eax; ret`) |
| SAV_IsEnabled / IsFirstBoot (1E2C / 1E3E) | 005D6DE0 / 005D6E60 | return 0x00A93764 / 0x00A93768; nobody ever sets them to 1 |
| SAV_SaveSlotEx (1E1D) | 005D5CD0 | `return 1` in both branches: **no full-slot writer exists on PC** |
| SAV_SetSlotUserDataEx (1E21), SetSlotUserBufferEx (1E35) | 005D6630, 005D6AC0 | no-op (00458420 = `ret`) |
| SAV_UpdateValid (1E18) | 005D5930 | FUN_006eb790: a loop calling `ret` |
| SAV_IsChannelInstalled (1E3C) | 005ECA50 | pushes 0: the Wii "install the Rabbids Channel?" prompt on New Game |
| SAV_ReadSlotValidity (1E45) | 005D6460 | FUN_006f4270 writes valid 1 into the table |

**Nothing allocates the slot tables** 0x00A93734[6]. The only users are FUN_006ebc10 (zeroes them from
ViD::b_Create), FUN_006ebcf0 (frees them in ViD::Destroy), and readers and writers that assume they exist. The crash
seen when clicking a profile was `SAV_GetSlotNameW` (handler 005D69B0, return address 005D69D7) → FUN_006eb920 on a
NULL table.

The real PC pieces are the readers FUN_006f4910/006f4ac0 (header) and 006f4830/006f49a0 (slot), the per-slot header
writer FUN_006f4690, delete FUN_006eba00/006eba40 → FUN_006f4290/006f4390, FUN_006eb950 (GetSlotUserBuffer),
SAV_SetStructureMaxSize/SetUserBufferSize/SetSlotsMerge, and SAV_ValidateEx/UnvalidateEx (the tmp buffer).

### 12.2 PC save globals = the Wii SAV structures

| PC | Wii executable | meaning |
|---|---|---|
| 0x00A93698 | SAV_gt_Config+0 (805BE348) | n of SAV_InitSystem(n) |
| 0x00A9369C[t] | Config+0x04 | declared slot count (SetStructureMaxSize) |
| 0x00A936B4[t] | Config+0x1C | slot size S_t = SCR::SAVComputeSlotSize(t) (004F83E0: saved variables + 0x2020 if user buffer, FUN_006ebba0) |
| 0x00A936E4[t] | Config+0x4C | merged flag (SetSlotsMerge) |
| 0x00A93704[t] | Config+0x6C | allocated count: set only when merged or count ≤ 32 (FUN_006ebaa0 = SAV_b_SetStructureMaxSize 80071668) |
| 0x00A9371C[t] | Config+0x84 | user buffer size; set only while init-done is 0 (FUN_006ebbc0 = SAV_SetUserBufferSize 800717E0) |
| 0x00A93734[t] | SAV_gapt_Slots (805BE330) | slot tables, count × 0xC0 |
| 0x00A9374C / 50 / 54 | SAV_gt_SaveContext+0/+4/+8 | tmp buffer pointer / capacity / size (SAV_ValidateEx output, SAV_ReadSlotEx input) |
| 0x00A93758 / 5C / 60 | SaveContext+0xC/+0x10/+0x14 | instance buffer (object entries; not saved on either platform) |
| 0x00A93764 | SaveContext+0x18 | init done (FUN_006ebbe0 = SAV_IsEnabled_C; cleared by SCR_DestroyUniverse/ReloadUniverse via FUN_006eb7b0 = SAV_ReinitSystem) |
| 0x00A93768 | (RVL_NandHelper+0x88) | first boot (PC global read by SAV_IsFirstBoot_C) |

Slot record (0xC0, identical fields on both platforms, big-endian on the Wii): +00 u16 0x0067, +04 u32 valid,
+08 u8 sec, +09 min, +0A hour, +0B day, +0C month, +0E u16 year, +10 u32[10] user data (fields 100..109),
+38 UTF-16 name (64 units), +B8 u32 data size, +BC runtime user-buffer pointer. The user buffer is 0x2020 bytes:
u32 −1, u32 −1, u32 0x2014, then data (Wii SAV_SetSlotUserBuffer 80071374).

### 12.3 Script VM and native table

* Handler `void* __cdecl h(void* ip)` returns ip+4. VM globals: entry index 0x00A7E188, data offset 0x00A7E18C,
  data base cell 0x00A7E198, entry-pointer array cell 0x00A7E19C.
  * **Pop** (last argument first): index−1, offset−4 (0xC for a SCR_tt_FixedBuffer_ {data, cur, size}), value = `*ptrs[index]`.
  * **Push**: `data[offset] = v`, `ptrs[index] = data+offset`, index+1, offset+4.
* Native table TOOsarray at 0x00A718AC: {data, element size 12, capacity, count at +0xC}, elements {word, handler,
  word}, sorted.
  * SCR::ResolveNodes/SCR_u32_GetRankID store the rank in the bytecode.
  * SCR::ApplyCommon (00516030) calls `*(data + rank*12 + 4)` **at every call**, so replacing an element's handler
    works at once.
* Order (gameChecks 00409970): ViD::b_Create → SCR_b_Init (creates the table) + ViD::RegisterScript →
  SCR::RegisterScript → RegisterFunctions 0050CE40. Then bigfile, window, Load_World, ViD::SecondInit → IO init.
  wiimote.dll is loaded by the Gear input init, after the table is built.

### 12.4 Patches (DllMain, `[save] enabled=1`)

1. **Verify first, patch only if everything matches**; otherwise log the FAIL lines and change nothing.
   * CRC32 of every replaced handler body.
   * CRC32 of 32 engine functions the layer calls or relies on.
   * The RegisterFunctions imm32 of each handler, with its `add eax, word<<16`.
   * The 8 call sites.
   * `L"Empty"` at 0x008CC128.
2. **JMP (E9) at the handler entry**, for handlers used by their SAV native only. `wmtest sav` counts the imm32
   references in the code section: exactly the registrations, and no call/jmp.

| word | native | handler | registration imm32 |
|---|---|---|---|
| 1E26 | SAV_InitSystem | 005D6C90 | 0050FF84 |
| 1E29 | SAV_Step | 005D6D90 | 0051004A |
| 1E20 | SAV_GetSlotDataEx | 005D64B0 | 0050FDF8 |
| 1E2C | SAV_IsEnabled | 005D6DE0 | 00510110 |
| 1E3E | SAV_IsFirstBoot | 005D6E60 | 005105B4 |
| 1E41 | SAV_NeedFirstBoot | 005D6EB0 | 0051067A |
| 1E45 | SAV_ReadSlotValidity | 005D6460 | 00510782 |
| 1E46/1E47 | SAV_ForceProgressMessage_Start/End | 005D5CC0 | 005107C4, 00510806 |
| 1E18 | SAV_UpdateValid | 005D5930 | 0050FBE8 |
| 1E1D | SAV_SaveSlotEx | 005D5CD0 | 0050FD32 |
| 1E21 | SAV_SetSlotUserDataEx | 005D6630 | 0050FE3A |
| 1E35 | SAV_SetSlotUserBufferEx | 005D6AC0 | 00510362 |
| 1E30 | SAV_GetSlotNameW | 005D69B0 | 00510218 |
| 1E32 | SAV_GetSlotNameWEx | 005D6930 | 00510289 |
| 1E31 | SAV_SetSlotNameWEx | 005D6780 | 0051025A |
| 1E36 | SAV_GetSlotUserBufferEx | 005D6B80 | 005103A4 |
| 1E1E | SAV_ReadSlotEx | 005D5E70 | 0050FD74 |
| 1E3B | SAV_ReadSlotHeaderEx | 005D5F40 | 005104EE |
| 1E43 | SAV_SaveSlotHeaderEx | 005D5DA0 | 005106FE |
| 1E28 | SAV_DeleteSlotEx | 005D6CC0 | 00510008 |

3. **Per-word replacement** for natives whose body is shared (a JMP would change unrelated natives).
   * The element in the native table is replaced at DllMain and re-checked at every SAV_InitSystem.
   * The RegisterFunctions imm32 is also replaced, in case registration runs again.

| word | native | shared body (references) | registration imm32 |
|---|---|---|---|
| 1E3C | SAV_IsChannelInstalled | 005ECA50 "push 0" (11) | 00510530 |
| 1E2A | SAV_ReturnCommand | 005D6430 pop + no-op (3, incl. 1E38 SAV_UpdateValidEx) | 0051008C |
| 1E2E | SAV_InstallChannel | 00560580 no-op (22) | 00510194 |

Not replaced: 1E38 SAV_UpdateValidEx (no Wii script calls it; stays a pop + no-op), 1E44 SAV_ChannelInstallNbBlocks
(pushes 0x80; only shown by the install prompt).

4. **Save folder**: the `call STD_IOGetExecPath` (006D4BD0) in the eight save functions now calls
   `SavExecPathForSaves`, which returns `<STD_IOGetExecPath()>/sav` and creates the folder on first use. The format
   strings stay as they are (`/slt_%d_%d.sav`, `/slt_xx_%d.sav`, `/` + `slt_...`):

| call site | function |
|---|---|
| 006F4297 | FUN_006f4290 delete write |
| 006F4398 | FUN_006f4390 merged delete write |
| 006F46B8 | FUN_006f4690 header write |
| 006F47C4 | FUN_006f47b0 NeedFirstBoot (slt_0_0.sav) |
| 006F4858 | FUN_006f4830 slot read |
| 006F4938 | FUN_006f4910 header read |
| 006F49C6 | FUN_006f49a0 merged slot read |
| 006F4AE6 | FUN_006f4ac0 merged header read |

### 12.5 Semantics (Wii evidence → PC implementation)

| native | Wii executable | wiimote.dll |
|---|---|---|
| SAV_InitSystem(n) | SAV_b_InitSystem 80071AC8: every NULL table gets Config+0x6C[t] × 0xC0 (p_AllocExt, memset, u16 0x67 per record); Config+0 = n; RVL_NandHelper::Init(1); g_poNandFlow = oBoot, Init; SaveContext+0x18 = 1 | Same allocation through MEM::p_Alloc(MEM_gpo_Main) (freed by the engine's FUN_006ebcf0). Count 0 → no table (the Wii allocates 0 bytes); if structures 0..2 were never declared, 3/90/1 (SAVE_STRUCTURE_InitAll). n stored; IsFirstBoot = no `slt_*.sav` in sav\; every declared slot header read from disk through FUN_006f4910/006f4ac0 (missing file or region → invalid; bad magic → record reset); init done = 1. Log: folder, first boot, per-structure counts, slot size, user buffer |
| SAV_Step | SAV_i_Step 80071BDC: 1 when no flow is pending, 0 while flow->Step runs | always 1 (every PC operation completes inside its native). The scripts only wait for `== 1`; nothing needs a 0 first |
| SAV_GetSlotDataEx(slot, t, f) | SAV_u32_GetSlotData 80071170: f 0 u32 +4; 1..5 u8 +8..+C; 6 u16 +E; 100..109 u32 +10+4(f−100); else 0; no checks | same mapping; 0 for t ≥ 6, table not allocated, slot ≥ allocated count |
| SAV_IsEnabled | SAV_b_IsEnabled 80072DD0: init done && RVL_BootUpFlow::IsInitialized | init done (0x00A93764) && our InitSystem ran |
| SAV_IsFirstBoot | bIsFirstBoot (NandHelper+0x88), set by RVL_BootUpFlow::Step (8045300C) after b_CreatingFirstBootStep created the save | 0x00A93768, set by InitSystem: 1 when sav\ had no slt_*.sav |
| SAV_NeedFirstBoot | bNeedFirstBoot 804543B8: no `<home>/banner.bin` and no `<home>/share/banner.bin` (the game never created its save); read by mode-1 managers only | 0 once InitSystem ran, else 1 when sav\ has no slt_*.sav |
| SAV_IsChannelInstalled | SAV_b_IsChannelInstalled → RVL_Nand::bChannelExists | `[save] channel_installed` (default 1). PROFIL_SELECT phase 0 then never switches to State_Install_WIIChannel, and PROFIL_SELECT/INFO hide the channel button |
| SAV_InstallChannel | g_poNandFlow = oChannelInstall, Init | marks the channel installed; Install_WIIChannel step 4 sees SAV_Step() == 1 at once, so it cannot hang |
| SAV_ReturnCommand(c) | SAV_v_ReturnCommand 80071C4C: flow->ProcessCommand(c) if a flow is pending | pop, log (no prompt is ever raised: callback 1005 events are never sent) |
| SAV_ForceProgressMessage_Start/End | NandHelper SetForceProgressMessage / HideProgressMessage | log only (no NAND message on PC) |
| SAV_ReadSlotValidity(slot, t) | SAV_UpdateValidityEx 80071924: valid = index header valid | valid = header on disk (magic 0x67) valid, else 0 |
| SAV_UpdateValid | SAV_ScanIfExists 80070A44: all headers from the index (keeps +BC) | every declared slot re-read from disk as in InitSystem |
| SAV_SaveSlotEx(slot, t) | SAV_b_WriteSlot 80072480 / SeekWriteSlot 8007291C (merged): magic, valid 1, time (STD localtime), +B8 = tmp size; no user buffer while the structure needs one → header restored, nothing written; writes [user buffer][tmp buffer] | same header update with the PC STD_u8_TimeGetCurrent* functions; writes the PC layout (§12.6). A merged payload larger than S_t, or a write error, restores the header and writes nothing |
| SAV_SaveSlotHeaderEx | SAV_b_WriteSlotHeader: header + user buffer | per-slot: the PC writer FUN_006f4690; merged (PC handler was a no-op): the same blocks at the merged region |
| SAV_ReadSlotEx / ReadSlotHeaderEx | SAV_b_ReadSlot / ReadSlotHeader: index header, magic check, user buffer, tmp buffer | the PC readers after bounds, file and region checks; bad magic → record reset |
| SAV_DeleteSlotEx | SAV_b_DeleteSlot → SAV_Delete | the PC SAV_DeleteSlotEx_C (header with valid 0 written to the file) after bounds checks |
| SAV_SetSlotUserDataEx(slot, t, f, v) | SAV_SetSlotUserData 800712DC: f 100..109 only | same, with bounds |
| SAV_SetSlotUserBufferEx / GetSlotUserBufferEx | SAV_SetSlotUserBuffer 80071374 (size ≤ 0x2014 and == user buffer size, allocate 0x2020, {−1,−1,0x2014}, zero, copy) / SAV_GetSlotUserBuffer 80071490 = PC FUN_006eb950 | Set: the Wii code. Get: FUN_006eb950 in bounds, else its "no data" branch (size 0, buffer freed) |
| SAV_GetSlotNameW / WEx, SetSlotNameWEx | SAV_pz_GetSlotName: name if valid else @10552 L"Empty"; SAV_SetSlotName: wcsncpy 0x80 | L"Empty" (engine string 008CC128) for invalid or missing slots; the copy is bounded to 63 units + NUL |

### 12.6 PC save files (`<exe dir>\sav\`)

The PC build has no full-slot writer. The layout is therefore the one its readers expect:

* **Per-slot file**: `slt_<slot>_<t>.sav` = [0xC0 header][0x2020 user buffer if the structure has one][header+B8 bytes
  of data]. Read by FUN_006f4830 → FUN_006f44b0, then FUN_006eb7c0 and a read into the tmp buffer.
  wiimote.dll writes it whole (CREATE_ALWAYS), with +BC = 0.
* **Merged file**: `slt_xx_<t>.sav`. Slot k is the region at k × (0xC0 + S_t) = [header][user buffer][data]
  (FUN_006f49a0 / FUN_006f4ac0 / FUN_006f4390). Regions are updated in place, and gaps read as zeros (bad magic →
  invalid).
* **Data**: the SAV tmp buffer, entries {u32 key, u32 id, u32 len, len bytes}.
  * key 0 = universe.
  * id = variable offset.
  * Values are in PC byte order.
* **Header only**: the per-slot writer FUN_006f4690 writes [header][user buffer] in place.
* **Delete**: FUN_006f4290 / FUN_006f4390 write the header with valid 0. The file stays.
* **Wii structures** (SAVE_STRUCTURE_InitAll):

| structure | content | slots | user buffer | merged |
|---|---|---|---|---|
| 0 | progression | 3 | none | no |
| 1 | figurines | 90 | 6144 | yes |
| 2 | sticker album | 1 | none | no |

### 12.7 Importing a Wii save (`rghport import-save`)

`rghport import-save --wii-save <folder> --out <exe dir>\sav --bigfile <Wii or converted bigfile> [--slot-size 1=N]
[--dump] [--no-check] [--lenient]` (also `python -m rghport.saves import-save ...`).

* The Wii save folder (index.dat, slot files) is only read; an output folder inside it is refused.
* The type of every saved variable comes from the universe script model, entry 72002B9C of the given bigfile (the Wii
  bigfile and a converted one hold the same bytes).
* After writing, the files are read back the way the PC readers read them and compared with the Wii source.

| | Wii (NAND, big-endian) | PC |
|---|---|---|
| headers | `index.dat` only: 6 × 90 × 0xC0, record t×90+slot (RVL_NandHelper::pGetIndexHeader) | first 0xC0 bytes of the file or region |
| per-slot file | [user buffer][data], padded to 32 bytes | [header][user buffer][data] |
| merged file | region k at k × S_t = [user buffer][data] | region k at k × (0xC0+S_t) |
| values | big-endian | swapped per variable type |

* **Typing**: the entries follow SCR::SAVValidateVar (PC 004F7530, Wii 8012218C).
  * Structures go member by member, arrays element by element, fixed buffers as raw bytes; buffers are skipped.
  * Types come from the saved universe variables: structure 0 has 612 leaves (464 int, 115 float, 30 vector,
    3 fixed buffers), structure 1 has 33, structure 2 has 48 fixed buffers.
* **S_1 = 0x2027C**, from three agreeing sources: the Wii region offsets, the figurine data size 0x1E25C + 0x2020,
  and the universe variable sizes (with the GST_SaveManager FBUF_InitAll buffer sizes). The layer logs the runtime value
  at SAV_InitSystem.
* **Checks**: a real Wii save converted this way reads back with every entry equal to its Wii source, loads in game
  (profile page, PLAY) and is written back by the game at the same size.

### 12.8 Configuration and log

* `[save] enabled=1` (0: nothing is verified or patched). `[save] channel_installed=1` (0: the Wii install prompts;
  InstallChannel then marks the channel installed for the session).
* DllMain reads `[save]` with plain file I/O and its log lines are buffered until the config is loaded.
* Log lines:
  * patch result: "PC executable verified: 20/20 SAV handlers redirected, 8/8 save path call sites → `<exe dir>`/sav,
    per-word natives …", or "differs … NOT installed" followed by the FAIL lines;
  * per-word table replacements;
  * SAV_InitSystem with per-structure counts, allocation, merged flag, slot size and user buffer;
  * every save written;
  * with `log_verbose=1`, every SAV call with its arguments and result (identical consecutive calls such as
    SAV_Step polling are counted).

### 12.9 Tests (offline)

* `wmtest sav <PC executable>`:
  * **Executable file: 84/84 PASS.** 20 handler CRCs + registrations, 3 per-word registrations, 32 engine function CRCs,
    8 call sites, L"Empty", and 20 exclusivity counts.
  * **Self-test: 21/21 PASS.** The real handlers run on a fake VM (VM stack balance checked), with a fake engine whose
    file functions follow the PC decompile. Covered:
    * NULL-table safety;
    * InitSystem allocation and flags;
    * the full GetSlotDataEx mapping and out-of-range values;
    * New Game (name, feet, data) written and read back through the PC reader;
    * a merged figurine with user buffer at 3 × (0xC0+S);
    * GetSlotUserBufferEx including the missing-slot branch;
    * delete, ReadSlotValidity, UpdateValid, ReturnCommand.
* `wmtest` (WPAD) still 31/31 PASS.
* Build: `build.bat`, MSVC 18 x86 `/MT`; two test-only exports (`WiimoteSavCheckExe`, `WiimoteSavSelfTest`).

### 12.10 In-game test plan

1. Copy `wiimote.dll` next to the PC executable; `wiimote.ini` with `[save] enabled=1`, `log=1`, `log_verbose=1`.
2. **Start**: the log shows the "verified … 20/20 … 8/8" line and three per-word table replacements.
3. **PRETITLE**: "SAV_InitSystem(1): folder …\sav, no save file (first boot)". Structure 1 must show "slot size
   0x2027C". Default figurines are created (sav\slt_xx_1.sav, slt_0_2.sav).
4. **CHOOSE A FILE → New Game**:
   * no Rabbids Channel dialog, name entry;
   * `SAV_SaveSlotEx(<slot>, 0) -> written`, and sav\slt_<slot>_0.sav appears;
   * the profile page shows the name.
5. **Restart**: IsFirstBoot 0, the profile shows the name, loading it restores the progression.
6. **Imported Wii save**: run `rghport import-save` into `<exe dir>\sav\` and start. The profile page shows the Wii
   profiles; with the album and figurines 0–9 valid, PRETITLE goes straight to MAINTITLE; loading reaches the saved
   progress. If the logged structure 1 slot size is not 0x2027C, rerun the import with `--slot-size 1=<value>`.
7. **Negative**: `[save] enabled=0` gives the old behaviour. `channel_installed=0` shows the install prompt; "yes"
   returns to the profile page.

### 12.11 Open risks

1. **Not run in the game yet.** Offline checks prove the bytes, the VM protocol and the file layout as read from the
   decompile.
2. **PRETITLE vs GST_SaveManager_Track_INIT ordering.** SAV_Step is 1 at once, and the Wii pauses the other objects
   during InitSystem. If PRETITLE phase 1 ran before InitSystem, it would see IsFirstBoot 0 and empty tables, and try
   to create figurines while saves are refused (logged). The Wii has the same order and no NULL checks, so this is
   not expected.
3. **Instance buffer**: object entries (0x00A93758) are not persisted. The Wii writer stores only the tmp buffer too.
4. **Delete** keeps the file with valid 0 (PC behaviour; the Wii deletes the file). IsFirstBoot therefore also sees
   deleted slots as existing save data.
5. **No NAND error events** (callback 1005) are raised. A write error only logs and keeps the old header, so the
   scripts see an invalid slot.
6. **Time fields** come from the PC STD_u8_TimeGetCurrent* functions (localtime, month as they return it). Wii saves
   carry broken years (1959–1973) and are copied as they are.
7. **The import** takes the variable types from the universe model in the given bigfile (the Wii bigfile and a
   converted one hold the same model). An invalid slot whose data does not convert gets its header with data size 0.

### 12.12 In-game status and the next paths

**Verified in game** with an imported Wii save:
* The log shows "20/20 SAV handlers redirected, 8/8 save path call sites".
* SAV_InitSystem(1) reports the valid slots and structure 1 slot size 0x2027C (the import value).
* CHOOSE A FILE shows the imported profile with its figurine portrait; the profile page opens without a crash.
* PLAY loads the progression into the Baltimore hub, and saving there writes `slt_0_0.sav` back at the imported size
  (379460 bytes).

**PLAY = load progression** (PC GST_SaveManager bytecode):

1. `GST_SM_LoadProgressionFromSlot(slot)` needs `mi_UnitsToProcess == 0 && @u i_SaveIsEnable`. It sets mode 0 and
   action 2, then State_PROFILES.
2. State_PROFILES uses `ma_SlotsInfosProgressions[slot].bValid` (= SAV_GetSlotDataEx(slot,0,0) from InitTab) and goes
   to State_LOAD.
3. State_LOAD: `SAV_ReadSlotEx(slot, 0)`.
   * The layer checks that the header is on disk, then calls FUN_006f4830: header, no user buffer (0x00A9371C[0] = 0),
     FUN_006eb7c0(+B8 = 0x5C984), which reallocates the tmp buffer, then 0x5C984 bytes are read.
   * Log: `SAV_ReadSlotEx(0, 0) -> 1 (0x5C984 bytes of data)`.
4. State_WAIT (`SAV_Step() -> 1`), then State_UNVALIDATE.
   * GST_SM_CB_ErorSend → `SAV_ReadSlotValidity(slot,0): valid 1` and `SAV_GetSlotDataEx(slot,0,0) -> 1`, so there is
     no error.
   * `SAV_UnvalidateEx(0)` runs the PC SAV_UnvalidateEx_C (not patched):
     * every tmp-buffer entry goes to SCR::SAVUnValidate(universe 0x00A718C8); only key 0 (= universe +0x15C) is taken,
       and the variable is found by offset (id);
     * then the instance buffer is applied to the objects (object entries from the last SAV_ValidateEx, same as on
       the Wii).
5. SCR::SAVUnValidateVar 004F78F0:
   * Plain variables: `memcpy(universe + id, data, len)`.
   * Buffers (type 0x640000): skipped.
   * **Fixed buffers (type 0x6A0000)** are only taken when the script buffer's capacity (+8) is non-zero **and equal
     to the entry length**. An all-zero entry frees it; otherwise it is allocated if needed, copied, and cur = data + len.
     The customised-rabbit JPEGs therefore need FBUF_InitAll (GST_SaveManager_Track_INIT, before SAV_InitSystem) to
     give the same capacities as the Wii entries. The import found them equal.
6. `i_lastSlotUsed = i_loadedSlot = slot`, then State_LOAD with no unit left, then State_IDLE. MapStartup continues
   (LOADMAP).
7. A universe reload calls FUN_006eb7b0 (init done = 0). The next GST_SaveManager_Track_INIT then runs
   SAVE_STRUCTURE_InitAll + SAV_InitSystem(1) again: the tables are kept, headers are re-read, and first boot is 0.

**Save progression** (GST_SM_SaveProgressionInSlot → State_SAVE, mode 0):

* `SAV_SetSlotUserDataEx(slot,0,105,@u i_collecte_StackToTheMoon)`.
* `SAV_ValidateEx(0)` (PC, fills the tmp buffer, expected 0x5C984 bytes).
* `SAV_SaveSlotEx(slot,0)` → `slot N of structure 0 saved to …\sav/slt_N_0.sav (offset 0x0, 0x5C984 bytes of data)`.
  The file is replaced whole: header (valid 1, PC time) + data = 379460 bytes, the size of the imported file.
* Then State_WAIT → State_SAVE → State_IDLE.
* A name change goes through SAV_SetSlotNameWEx and GST_SM_SAVE_ProgressionName → State_SAVE_HEADER →
  `SAV_SaveSlotHeaderEx(slot,0)`, i.e. FUN_006f4690: the header is rewritten in place and the data is kept.

**Figurines / In Ze Wiimote** (structure 1, merged, user buffer 6144):

* Header or slot reads use region k at k × (0xC0 + 0x2027C): [header][0x2020 user buffer][0x1E25C data].
  * FUN_006f4580 always reads a 0x2020 block (allocated through MEM::p_Alloc when +BC is 0).
  * FUN_006eb950 copies 6144 bytes into the script buffer only if that buffer's capacity is ≥ 6144. Otherwise it sets
    the capacity to 0 and frees the script buffer. The portrait on CHOOSE A FILE shows that this path works.
* Saves:
  * State_SAVE mode 1 calls SAV_SetSlotUserBufferEx before SAV_SaveSlotEx. The payload 0x2020 + tmp size must be
    ≤ 0x2027C (a larger one is refused and logged).
  * GST_BM_LIB_SaveCustoInFigurineSlot saves without SetSlotUserBufferEx. It works for slots whose header was read
    (user buffer allocated); on a slot without one, the Wii and the layer both refuse ("structure has a user buffer
    but the slot has none").

**Delete** (PROFIL_INFO DELETE, TRC 18, −50):

* `SAV_DeleteSlotEx(slot,0)` writes the header with valid 0 into sav\slt_slot_0.sav. The data stays, but the slot is
  "New game" from then on.
* Then SAV_UpdateValid (94 headers re-read) and InitTab.
* Running the import again restores an imported profile.

**Log signatures of trouble**:

| log line | meaning |
|---|---|
| `no such slot` | script slot or structure outside the allocated tables |
| `do not fit the 0x…-byte region` | merged payload larger than S_t |
| `structure has a user buffer but the slot has none` | figurine save without a user buffer (Wii-identical refusal) |
| `cannot open … for writing` | file error; the header is restored and the slot keeps its old state |
| `SAV_ReadSlotEx(…) -> 0` on a valid slot | header missing or bad magic → State_UNVALIDATE reports load error 1 |

---

## 13. Controls mode (`wm_ctl.cpp`, `[controls] mode=pc`)

### 13.1 What it does

* `mode=wii`: the virtual Wii remote + Nunchuk of §6 drives every controller read.
* `mode=pc` (the default since 2026-09-17; before that `wii` was): **PC-native controls.**
  1. **No virtual remote.** The one-time init skips the PC input source and the WPAD server. The engine's KPAD
     client finds no server, exactly as in the retail PC game without the DLL, so no Wii remote slot is ever
     registered. No button, stick, shake or tilt input comes from the DLL. The `Wrap*` exports report no device and
     never forward rumble. `[general] enabled`, `[keys]`, `[xinput]` and `[motion]` are unused; `[pointer] dist` is
     used.
  2. **The 11 Wii-only pointer and motion natives are replaced per word** (§13.2). This covers the native table
     element (the table is built by ViD::b_Create before the DLL loads, and SCR::ApplyCommon reads the handler at every
     call) and the `mov [esp+x], handler` imm32 of the entry in ViD::RegisterScript `005428E0`. The engine's handler
     bodies stay intact.
  3. **Verification first**: 11 handler CRCs with their registration sites (`C7 44 24 xx` / `C7 04 24` next to
     `push key`), and 8 engine functions (§13.3, §13.4). Any difference means nothing is replaced, the log shows the
     FAIL lines, and the virtual remote stays off.
* The pointer is the engine's own mouse position, the value `IO_MousePosGet` returns, in KPAD units. The motion reads
  (accelerometers, samples, horizon) come from the Options bindings (§22); until the first frame, and without the
  Options layer, they describe a remote held level at rest.
* `[controls] mode` is read in DllMain with plain file I/O (like `[save]`). The shared patch helpers (PE reader, CRC,
  code writes, native table replacement, ini reader) are in `wm_patch.cpp`.

### 13.2 Replaced natives

Every controller id gets the same answer. Pops are ints, last argument first; results are pushed like the engine
handlers (int +4, vector +0xC). "Inside" means the mouse is inside the 3D viewport while `IO_Mouse` ≠ 0 (§13.3).

| key | native | engine handler → internal | arguments | pc mode result |
|---|---|---|---|---|
| `0098FFFF` | IO_JoystickPointerGet | `005EDE50` → `FUN_006ECDF0` | id | (2x−1, 2y−1, dist); outside: last inside position |
| `0085FFFF` | IO_JoystickGetPointer | `005EDEE0` (jmp `005EDE50`) | id | same |
| `0099FFFF` | IO_JoystickPointerStateGet | `005EDF60` → `FUN_006ECE90` | id | 1 inside, −2 outside |
| `0086FFFF` | IO_JoystickGetPointerState | `005EE040` (jmp `005EDF60`) | id | same |
| `00B2FFFF` | IO_JoystickDPDValidGet | `005EDEF0` → `FUN_006ECE60` | id | 2 inside, 0 outside |
| `00A6FFFF` | IO_JoystickHorizonGet | `005EE050` → `FUN_006ECEC0` | id | (1, 0, dist) |
| `0097FFFF` | IO_JoystickAccelGet | `005EDD90` → `FUN_006ECD90` | id, sensor | (0, −1, 0) for sensor 0/1, else 0 |
| `009DFFFF` | IO_JoystickAccelSampleGet | `005EE330` → `FUN_006ECFB0` | id, sample, sensor | (0, −1, 0) for sensor 0/1, else 0 |
| `009EFFFF` | IO_JoystickPointerSampleGet | `005EE3F0` → `FUN_006ED020` | id, sample | the pointer |
| `009FFFFF` | IO_JoystickPointerStateSampleGet | `005EE4A0` → `FUN_006ED0A0` | id, sample | the pointer state |
| `009BFFFF` | IO_JoystickSampleNumberGet | `005EE200` → `FUN_006ECF10` | id | 1 |
| `008CFFFF` | IO_JoystickStickGet | `005ED7C0` → `FUN_006ECC10` | id, stick | controller 0, stick 0: the movement actions while §22.4 applies; any other read is the engine's handler |

x, y = (cursor − viewport origin) / viewport size, the formula of `FUN_006CC490`. KPAD (−1, −1) is the top-left,
and the Wii `Bunnies_Pointer_Pos_Convert` computes (p + 1) / 2, so a Wii cursor lands exactly where
`IO_MousePosGet` puts a PC cursor. The rest values are those the virtual remote produces (§3). dist = `[pointer]
dist` (0.8–2.9, default 2.0), inside the 0.7–5.0 range of the pointer state.

Deliberate differences from a real remote:
* The pointer shift of `IO_JoystickPointerShiftSet` is not added (the engine adds it twice for a remote).
* A single sample per frame.
* All ids share the mouse (there is no second pointer for a second player).

Not replaced, because they read no remote state or behave correctly without one: IO_JoystickHere, TypeGet,
ExtensionGet, buttons, the other stick natives, RumbleSet, BatteryStateGet, SensorBarPositionGet, WiimoteSensibilitySet,
PointerShiftSet, IsAccelEnable, AccelMaxGet.

### 13.3 The engine's mouse and keyboard (what pc mode reads, what the scripts can call)

| item | address | behaviour |
|---|---|---|
| mouse init | `FUN_006CC1D0` (from `FUN_006CC6D0`, `FUN_006CC750`) | `IO_Mouse` `00A90F28` = 1, sensitivity = default, button mask `00A90EFC` = 0xFFFFFFFF, cursor read |
| per frame | `FUN_006CC740` | keyboard `FUN_006EDBB0` (GetKeyboardState), controllers `FUN_006ED470`, then `Mouse_On_Game` `006CC2A0` |
| Mouse_On_Game | `006CC2A0` | cursor = GetCursorPos − client origin (`00A90EB8/EBC`) → `00A90F18/F1C`; buttons from GetAsyncKeyState: 1 left, 2 right, 4 middle, wheel 8 up / 0x10 down → current `00A90F04`, previous `00A90F00` |
| IO_MousePosGet_C | `005ECF20` → `FUN_006CC490` | ((x − `00A90ED4`) / `00A90ECC`, (y − `00A90ED0`) / `00A90EC8`, 1 inside [0,1]² else 0); all 0 while `IO_Mouse` = 0 |
| IO_MouseButtonPressed_C / JustPressed_C / JustReleased_C | `005ECF80` / `005ECF90` / `005ECFA0` → `FUN_006CC5E0` / `FUN_006CC600` / `FUN_006CC630` | current & mask & bits; just pressed = now and not before; just released = before and not now |
| IO_MouseEnable_C | `005ECF70` → `006CC6C0` | sets `IO_Mouse` (the only writer besides the init) |
| IO_MouseButtonMaskSet_C / IO_MouseMoveGet_C / IO_MouseSensitivitySet_C | `005ECF50` / `005ECF10` / `005ECF30` | button mask / movement divided by the sensitivity / sensitivity |
| IO_KeyboardKeyPressed_C / JustPressed_C / JustReleased_C | `005ECE10` / `005ECE20` / `005ECE30` | virtual-key codes from the per-frame keyboard state |

No script is needed to turn the mouse on: the engine's own init already sets `IO_Mouse` = 1 and the full button
mask.

### 13.4 Magma menus with the mouse

* **Cursor position.** The scripts place the Magma cursor with `ViD_MenuGameCursorMove` (handler `005EB9C0` → MGM
  manager vtable +0x88 (cursor, x, y)). Both script paths work in pc mode:
  1. PC BUNNIES_LIB `Bunnies_UpdateMagmaCursor`: `IO_MousePosGet` while `IO_JoystickHere(0)` is 0, else the pad
     stick.
  2. Wii: `IO_JoystickPointerGet` → `Bunnies_Pointer_Pos_Convert` → the same x, y, because pc mode returns
     p = 2x − 1.
* **Clicks.** `FUN_007FD0A0` runs per cursor. When `FUN_006EC950(cursor, 0)` finds no controller in that slot, the
  Magma buttons come from the mouse:
  * bouton 1 (+0x134) ← left button;
  * bouton 4 (+0x138) ← middle button;
  * bouton 2 (+0x13C) ← right button;
  * press through `FUN_006CC600`, release through `FUN_006CC630`.

  Otherwise it uses the joystick masks bound with `MGM_BindMagmaBoutonToJoystick`. The mouse test sits inside the
  "binding ≠ 0" test, so a bouton unbound with `MGM_BindMagmaBoutonToJoystick(b, 0)` gets no mouse click either.
  Keep bouton 1 bound; the Wii flow only clears the bindings of boutons 2 and 4.
* **With a pad in slot 0** (XInput or DirectInput), `FUN_006EC950` is true. Magma then takes the pad's bound buttons,
  and path 1 moves the cursor with the stick (PC-build behaviour). Path 2 still follows the mouse.
* `FUN_0051E470` (called by `FUN_0051E7B0`) is a separate PC UI mouse dispatcher: it forwards the cursor and the
  left button through `FUN_0071AF50`. It is not part of the Magma path.

### 13.5 What the scripts must do in pc mode

1. **Buttons and sticks**: no controller input exists. Read `IO_KeyboardKey*` and `IO_MouseButton*` (for example
   Enter confirm, left click select or shoot, Shift run).
2. **Pointer**: nothing to change. The Wii pointer reads follow the mouse, and `IO_MousePosGet` gives the same
   position.
3. **Motion**: every accelerometer read is at rest, so no shake and no tilt ever fire. Actions triggered by a shake
   (for example `Joy_ShakePAD`) need a key.
4. **Presence**: `IO_JoystickHere(0)`, `IO_JoystickTypeGet(0)`, `IO_JoystickExtensionGet` and
   `IO_JoystickBatteryStateGet` see no Wii remote. The pc profile must skip:
   * the pad-disconnect / Nunchuk TRCs (`i_NoPadTRC`, `GST_PAD_CB`);
   * the control scheme detection that uses `IO_JoystickTypeGet(0)` (`MMB_Init`).
5. Keep `IO_Mouse` on (no `IO_MouseEnable(0)`) and keep Magma bouton 1 bound.
6. A left click in a Magma menu is also a mouse button press for gameplay reads. Gate gameplay clicks while a menu
   is open.

### 13.6 Tests (offline)

`wmtest ctl <PC executable>` (writes `test\wmctl.ini` with `mode=pc`, `dist=1.5`):
* **Executable file: 20/20 PASS.** 12 handler CRCs + ViD::RegisterScript registration sites, and 8 engine functions:
  `FUN_006CC490`, `Mouse_On_Game`, `FUN_006CC1D0`, `FUN_006CC600`, `FUN_006CC630`, `FUN_006EC950`, `FUN_007FD0A0`
  and the `ViD_MenuGameCursorMove` handler.
* **Self-test: 21/21 PASS.** The real handlers run on a fake script stack and fake mouse globals, with the stack
  effect checked (also with an older entry below the arguments). Covered:
  * dist from the ini;
  * distinct words, with the aliases sharing handlers;
  * centre, corners, a letterboxed viewport;
  * outside the viewport (DPD 0, state −2, position held);
  * samples and the sample count;
  * `IO_Mouse` 0;
  * an empty viewport;
  * horizon;
  * accelerometer sensors 0/1/2, accelerometer samples;
  * `IO_JoystickStickGet(0, 0)`: the engine's answer before a script reads the accelerometers, the movement
    actions after; stick 1 and controller 10 stay the engine's;
  * the native table replacement on a fake TOOsarray: replaced, repeated, a foreign handler kept, the save-layer word
    mask, a missing key.
* `wmtest` 31/31 and `wmtest sav` 84/84 + 21/21 still pass; the save layer now uses the shared helpers.

### 13.7 In-game test plan

1. `[controls] mode=pc`, `log=1`. Expected log:
   * `CTL: mode=pc: PC executable verified; 11/11 pointer/motion natives replaced in the native table, 11/11
     registration entries; the virtual Wii remote is off`;
   * `CTL: controls mode=pc: no virtual Wii remote …`;
   * no WPAD server or connection lines.
2. The first use of each native logs `CTL: first call: <native>(<id>)`, which shows the natives and ids the scripts
   use.
3. With no pad connected, Wii Magma menus: the cursor follows the mouse (if the menu script reads the pointer or
   `IO_MousePosGet`), and a left click selects.
4. Outside the viewport: DPD 0, pointer state −2. The Wii scripts hide or freeze the cursor as with a remote pointing
   away.
5. `mode=wii` gives back the virtual remote unchanged.

---

## 14. Frame timing (walking lag analysis, RE only)

### 14.1 DT on both platforms

**PC**
* `TIME_GetDT_C` `005D8A20` returns `00A928BC`; `TIME_Get_C` `005D8A10` returns `00A92890`.
* TIM_UpdateBeforeFrame `006D5AB0` sets DT to the last measured value (`00A928C4`).
* TIM_UpdateAfterFrame `006D5B80` times the ViD::OneFrame body with QueryPerformanceCounter:
  * floor 0.001 (`008A0970`);
  * ForceDT `00A928C8` when non-zero (nothing writes it);
  * cap MaxDT `00A928CC` (config `TIM_MaxDT`, 0.48 in every default.cfg);
  * factor `009D8DA8`;
  * then DT = 0 until the next frame.
* The main loop `00409370` is PeekMessage + OneFrame, with no limiter and no Sleep.

**Vsync**
* The command line `/vsync[:N]` sets `00A72378` (default 0). Its only writer is the parser at `00500F73`.
* `FUN_00492BB0` maps 0 → D3DPRESENT_INTERVAL_IMMEDIATE, 1 → ONE, 2 → TWO.
* `005CD8E8` does Sleep(1) for the loading thread only when vsync is off.

**Wii**
* Same TIM logic, timed with OSGetTime.
* The flip waits for the retrace inside the render, so the frame rate is bound to 50/60 Hz.

### 14.2 Hypotheses, most likely first

1. **Uncapped frame pacing.** Without `/vsync` the port presents immediately. Every frame moves by the previous
   frame's measured time, so jitter shows as judder, where the Wii is locked to the retrace.
   * Test: run with `/vsync` (60 Hz) or `/vsync:2`.
   * Confirm: log the QPC time between calls of `006D5AB0` and DT at `00A928BC`.
2. **Per-frame (not DT-scaled) terms in the rabbid scripts**, correct only near 60 Hz:
   * `PJ_Rabbidz_Culbuto` divides by `60 * TIME_GetDT()` and adds `tv_side` per frame;
   * `f_stretch_coef += (1 − cur) * 0.5` per frame;
   * constant blends 0.3 and 0.1666667 in the group object.

   The follow state and the DYN integration are DT-correct. Fix: cap at 60 Hz (hypothesis 1), or a script rule
   scaling those terms by 60·DT.
3. **Ruled out: a Present wait outside the timed window.** The main loop `00409370` only pumps messages, reads the
   window rectangle (`FUN_006CC1A0`) and calls ViD::OneFrame. Inside OneFrame the window opens at
   TIM_UpdateBeforeFrame, after only the input poll `FUN_006CC740`, and closes at TIM_UpdateAfterFrame, the last
   call. The call just before it (`00458420`) is an empty method. Rendering and any flip therefore happen inside the
   window, so DT covers the whole frame except the message pump and the input poll.
4. **Animation conversion** (low). Root motion is used as `ACT_MBMoveGet() / TIME_GetDT()`, which does not depend
   on the frame rate.

**Measured and decided**:
* The title screen runs at DT ~1 ms without `/vsync` and ~5 ms with it, so vsync is not enough.
* The frame pacer (§17, `[timing] fps_cap=60`, on by default) is the fix for hypothesis 1. It also makes the per-frame
  terms of hypothesis 2 run at the Wii's rate.
* A DLL-side vsync switch is still not proposed: the present parameters are built when the device is created or reset
  (`00487860`, reset handler `FUN_00488090`), and the device may already exist when the DLL loads.

---

## 15. Bink videos playing or skipping fast (analysis, RE only)

### 15.1 Excluded

* **Video files**: the 51 PC videos are byte-identical to the Wii copies, and the port's 83 videos are byte copies of
  the Wii ones (640×448, about 30 fps, one audio track).
* **Configuration**: no Bink, vsync, refresh or fixed-DT key in either default.cfg.
* **Texture and BVO data**: the Wii loader `803322EC` and the PC loaders `0047B5D0`/`0045D150` read the same layout.
* **Wrong playback rate**: the PC player cannot play a file at the wrong rate.
  * It opens with `BinkOpen(handle, 0x900400)` (`0043EB33`), without the frame-rate override flag 0x1000.
  * It never calls its rate method (`0045CF80`, vtable +0x34).
  * Its clock is Bink's own wall clock (`BinkWait`), corrected against the engine's DirectSound device; game time
    does not drive it.
* **Observed length**: the hub loading video (id 0, 14.7 s) lasted its normal length in two logged sessions.

### 15.2 How videos run

* **PC update**: ViD::OneFrame → Render → K3D::BeforeFrame `00418D00` → UpdateProcedural → `b_Update` `0047B890`.
  * The update is skipped while the display flag at K3D+0x550 is set, or while DT is 0 and the texture flag is 0.
  * Decode step `0045CC40`: BinkWait → wait for the asynchronous decode → `while BinkShouldSkip { BinkNextFrame;
    decode }` → next frame.
  * At the last frame the option bits select stop, pause or loop.
* **Wii update**: `b_Update` `8033358C` updates only the current video.
  * Every frame it sets the rate to the video fps × TIM DT factor (`80332ACC`), so Wii videos follow
    `TIME_SetFactor` (slow motion, game speed).
  * Decoding is synchronous.
* **Loading-screen scripts** (`Test_LB_Play`, `Univers_Loop`, identical in both flows):
  * `BVO_Run(id)` starts the video on the loading-screen object; it ends when `BVO_IsRunning` is false.
  * Skipping needs `i_ReadyToFlush` and `(IO_JoystickButtonJustPressed(0,1) || @u i_Bink_AutoSkip) && id < 65 &&
    ai_Blink_Skipable[id]`.
  * The Wii flow plays the old map's exit video and the new map's enter video (`Map_Load_ES_WithBink(map,1,-1,2)`).
    Hub enter is id 0; ids 14, 30–35, 63 and 64 are skipable. The PC flow's mission videos are made non-skipable.
  * `i_Bink_AutoSkip` is set after a load of map 5107 (IZW, Wii only) and is never cleared.
* **Stubbed video natives on PC** (handlers bound in ViD::RegisterScript; on the Wii these are real):

| words | handler | behaviour |
|---|---|---|
| K3D_BinkIsRunning `0134FFFF`, PauseCurrentVideo `0130FFFF`, StopCurrentVideo `0133FFFF`, IsPreloading `0138FFFF`, GetPreloadedBinkKey `0139FFFF` | `005ECA50` | push 0 |
| GotoCurrentVideo `0131FFFF`, Preload `0136FFFF` | `005D6F00` | pop 1, push 0 |
| LengthGetCurrentVideo `0132FFFF` | `005EC990` | push 0.0 |
| CancelPreload `0135FFFF` | `00560580` | nothing |
| IsPreloadFinished `0137FFFF` | `005225F0` | push 1 |

The other K3D_Bink words (Play, Goto, Pause, Switch, SwitchToKey, StopIfRunning, LengthGet) and the BVO words are real.

### 15.3 Hypotheses, most likely first

1. **Catch-up after main-thread stalls.**
   * **Mechanism**: the video update runs only inside Render. During a stall (`LoadList_AsyncPost` after Render,
     texture creation, heavier Wii data) or while DT is 0, the Bink clock and the audio keep running. The next update
     skips frames to catch up, which looks like fast-forward while the total length stays normal.
   * **Confirm**: at `0045CC40`, log the time, the frame numbers (+0xC, +0x10 of the Bink handle) and the number of
     BinkNextFrame calls per update. Gaps over 100 ms followed by multi-frame jumps confirm it.
   * **Fix (platform DLL)**: pause the running Bink handles around the `LoadList_AsyncPost` call in OneFrame, and let
     `0047B890` update a playing loading video even when DT is 0.
2. **Skipable Wii videos cut on input or AutoSkip.**
   * **Mechanism**:
     * In wii controls mode, script button 1 of pad 0 is the virtual remote's A (Space, Enter, left click). One click
       during a skipable video ends it as soon as loading completes.
     * After any IZW load, `i_Bink_AutoSkip` stays 1 and cuts every later skipable video without input.
     * In `[controls] mode=pc` there is no remote, so only AutoSkip remains.
   * **Confirm**: log `i_Bink_AutoSkip`, `i_ReadyToFlush`, `i_Univ_binkToPlay[0..1]` and `BVO_CursorGet()` when
     `i_played` becomes 1; a cut video ends with the cursor below 1.
   * **Fix (script rule)**: clear `@u i_Bink_AutoSkip` in the flush branch of `Univers_Loop` (or in `Test_LB_Init`),
     and use AutoSkip only for id 64.
3. **Game speed factor ignored on PC.** During slow motion or a speed change, Wii videos change speed; PC videos stay
   at 1×.
   * **Confirm**: log `009D8DA8` (written by `TIME_SetFactor_C` `005D8AE0`) while a video plays; this applies only if
     it is not 1.0.
   * **Fix**: call the rate method each frame with fps × `009D8DA8`, and open with 0x901400 so Bink honours the
     override.
4. **Stubbed video natives used by Wii-only scripts** (table above).
   * **Mechanism**: a "wait while `K3D_BinkIsRunning`" loop ends at once, and Pause, Stop and Goto on the current
     video do nothing.
   * **Confirm**: first install logging handlers for these words, the per-word native table replacement of §13.
   * **Fix**: real handlers built on the PC video object:
     * IsRunning → `0047B640` (running and not paused, as on the Wii);
     * PauseCurrentVideo → `0047B6B0`;
     * StopCurrentVideo → vtable +0x30;
     * GotoCurrentVideo → vtable +0x3C;
     * LengthGetCurrentVideo → `0047B6E0`;
     * the current video is tracked from Play, ForcePlay, Switch and SwitchToKey.
5. **Title demo exits at once.** The Wii exit rule is `IO_JoystickButtonPressed(0,-1)` (any held button) or a stick
   above 0.1, so a held mapped key or pad drift ends the demo. Fix: `JustReleased` in the title-screen override, or a
   larger dead zone.

**Status**:
* Hypothesis 4 is implemented: `[video] natives=1` (§16).
* One run with `[video] log=1` confirms or rules out hypotheses 1–3 (§16.4).

---

## 16. Video natives and video log (`wm_video.cpp`, `[video]`)

### 16.1 What it does

* **`natives=1` (default)**:
  * **Replaced**: the ten K3D_Bink "current video" and preload words that are stubs in the PC executable (§15.2) get
    handlers with the Wii semantics.
  * **How**: each word is replaced on its own, in the native table element and in the imm32 of its entry in
    ViD::RegisterScript `005428E0`. The shared stub bodies stay intact.
  * **Tracking**: five real words are wrapped to track the current video (§16.3).
* **`log=1`** (together with `[general] log=1`):
  * **Call log**: the nine K3D_Bink words that exist on PC and nine BVO words are wrapped.
  * **Player hooks**: two slots of the Bink player vtable are replaced by logging wrappers that call the originals.
  * **Output**: each played video and each video script call is logged (§16.4).
* **Verification first** (§16.5): any check needed by the enabled options fails → nothing from this module is
  installed, and the log shows the FAIL lines.

### 16.2 Wii semantics and PC mapping

Keys were checked against the Wii ViD::RegisterScript, where each handler is registered as `lis rX, word+1; addi rX,
rX, -1`. For example 0x0135 - 1 gives `0134FFFF` = `K3D_BinkIsRunning__FPUl`, and 0x012D - 1 gives `012CFFFF` =
`K3D_BinkPause__FPUl`. The stack effects match the PC stubs.

| key | word | Wii | PC handler |
|---|---|---|---|
| `0134FFFF` | K3D_BinkIsRunning | `IsRunning` 8032F8D0: mpo_Current && b_IsRunning (+0x1D bit 0 set, bit 1 clear) | current && `FUN_0047B640` |
| `0130FFFF` | K3D_BinkPauseCurrentVideo | `PauseCurrent` 8032F52C: current with option +0x1C bit 0 → Pause(), 1; else 0 | current && option +0x0C bit 0 → `FUN_0047B6B0`, 1 |
| `0133FFFF` | K3D_BinkStopCurrentVideo | `StopCurrent` 8032F2F0: Stop(0), which clears mpo_Current → 1 | vtable +0x30 Stop(0), tracking cleared, 1 |
| `0131FFFF` | K3D_BinkGotoCurrentVideo(frame) | `GotoCurrent` 8032F788: GoToFrame(frame), 1 | vtable +0x3C Goto(frame), 1 |
| `0132FFFF` | K3D_BinkLengthGetCurrentVideo | `GetLengthCurrent` 8032F884: HBINK Frames / fps (the float is uninitialised without a current video) | `FUN_0047B6E0` (frames / fps); 0.0 without a video, Bink handle or fps |
| `0136FFFF` | K3D_BinkPreload(key) | `b_Preload(key, 1)` 8032EB2C: 0 for key 0, 1 when the key is already preloaded; else stores the key, clears "finished", starts the asynchronous read, 1 | key 0 → 0; else stores the key, finished at once, 1 |
| `0138FFFF` | K3D_BinkIsPreloading | `msb_IsPreloading` (.sbss, 0) | 0 |
| `0137FFFF` | K3D_BinkIsPreloadFinished | `msb_IsPreloadFinished` (.sdata, 1) | 1 |
| `0139FFFF` | K3D_BinkGetPreloadedBinkKey | `msh_PreloadBinkFile` (the key stored by b_Preload) | the stored key |
| `0135FFFF` | K3D_BinkCancelPreload | `FinishPreload(opened, 1)` 803334E8: only while mpo_Current is set; clears the key, finished 1, preloading 0 | same condition and effect |

* **Why preload is instant on PC**: the Wii preloads a video file into memory so that SwitchToKey opens it from the
  cache (DVD streaming). The PC opens videos straight from the bigfile, so there is nothing to wait for.
* **Script effect**: a script that waits on `K3D_BinkIsPreloadFinished` goes on at once, as with the PC stub.

### 16.3 The current video on PC

* **Wii**: one static current video.
  * `mpo_Current` is set by Run, ForcePlay, SetBinkKey, Reinit and b_Update (the running video it updates).
  * It is cleared by Stop and by b_Update at the last frame when the "stop" option is set.
  * Only the current video is updated.
* **PC**: every Bink player updates itself (update `0047B890`, called from K3D::BeforeFrame). The only engine-side
  "current" is the background video `K3D+0x684B4`: it is set by K3D::BVO_SetBgVideO and is what K3D_BinkStopIfRunning
  (`0086D370`) compares with.
* **Replacement**:
  * **Tracked texture**: the current video is the player of the texture key last passed to K3D_BinkPlay `012EFFFF`,
    ForcePlay `013AFFFF`, Switch `013DFFFF` or SwitchToKey `013BFFFF`, while its state bit 0 (running) is set.
  * **Fallback**: otherwise it is the background video, if running.
  * **Clearing**: StopCurrentVideo clears the tracking, and so does K3D_BinkStopIfRunning `013CFFFF` returning 1 for
    that texture.
  * **No stored pointer**: the player is looked up again at every call through `FUN_0086D2A0` (texture key → player,
    or NULL once unloaded).
* **PC Bink player** (K3D_texpro_bink, vtable `008A17A4`):

  | offset | field | Wii offset |
  |---|---|---|
  | +0x08 | update even while DT is 0 | |
  | +0x0C | options: bit 0 stop at the last frame, bit 3 pause there | +0x1C |
  | +0x0D | state: bit 0 running, bit 1 paused, bit 2 last frame reached | +0x1D |
  | +0x14 | fps | +0x18 |
  | +0x34 | Bink file key | +0x0C |
  | +0x44 | HBINK: Frames +8, FrameNum +0xC, LastFrameNum +0x10 | +0x38 |

  Vtable slots: +0x1C update, +0x2C run, +0x30 stop, +0x3C goto, +0x40 decode step `0045CC40`, +0x54 frame count,
  +0x58 / +0x5C BinkPause on / off.

### 16.4 Video log (`log=1`)

* **Script calls** (all prefixed `VIDEO:`):
  * Words logged: the ten replaced words, and wrappers for the K3D_Bink words Play, ForcePlay, Switch, SwitchToKey,
    StopIfRunning, Pause, Goto, LengthGet and IsInPause.
  * BVO words: BVO_IsRunning, BVO_Run, BVO_Pause, BVO_Stop, BVO_IsPaused, BVO_CursorGet, plus the length, frame and
    play words.
  * BVO keys are module-relative, so their table elements are matched by handler.
  * Each word logs its first call (`first call: BVO_Run(<object>, 0) -> 1`) and then each change of arguments or
    result. Floats are compared to 0.1; at most 200 lines per word.
  * Replaced words also show the current video's Bink key.
* **Played videos**: vtable slot +0x40 (decode step) and +0x1C (update) are wrapped.
  * `video starts, Bink key K: N frames at F fps (S s), frame n, time factor f`.
  * `end of video, Bink key K (reason): frames a..b of N (file S s at F fps); X s of video in W s wall (xR); U updates,
    V shown, K skipped in C catch-ups (largest jump J), L loops or gotos; longest gap between updates G ms, H gaps over
    100 ms; updates blocked: D by DT == 0, P by the display flag; time factor min..max`.
  * The reason is "no update for 1 s" (stopped, finished or no longer drawn) or "the texture switched to another
    video".
  * "Advanced" is the FrameNum difference across one decode step. More than one frame is a catch-up.
* **Reading it for §15.3**:
  1. Catch-ups right after gaps over 100 ms, with ×R close to 1 over the whole video → hypothesis 1 (stalls).
     Updates blocked by DT == 0 point at the DT gate of `0047B890`.
  2. Last frame b below N with BVO_IsRunning turning 0 and BVO_CursorGet below 1.0 → cut (hypothesis 2); the BVO
     and K3D_Bink lines show which call ended it.
  3. Time factor not 1.000 → hypothesis 3.
  4. First calls of the replaced words show whether Wii-only scripts use them (hypothesis 4).

### 16.5 Verification (44 checks)

* **28 words**: handler CRC and registration.
  * 19 in ViD::RegisterScript (`C7 44 24 xx` / `C7 04 24` near `push key`).
  * 9 in the BVO registrar `00604940` (`C7 44 24 xx` with `movzx eax, word [esi+0Ah]` and `add eax, n<<16` nearby).
* **Engine functions for `natives`**:

  | function | role |
  |---|---|
  | `0086D2A0` | texture key → player |
  | `0047B640` | running, not paused |
  | `0047B6B0` | pause |
  | `0047B6E0` | length |
  | `0045CBD0` | stop |
  | `0045CC00` | goto |
  | `0045CD10` | frame count |
  | `0086D370` | StopIfRunning; pins K3D+0x684B4 and the display pointer `009DE178` |

* **Engine functions for `log`**:

  | function | role |
  |---|---|
  | `0045CC40` | decode step |
  | `0047B890` | update; pins DT `00A928BC` and the display flag +0x550 |
  | `006D5B80` | TIM_UpdateAfterFrame; pins the time factor `009D8DA8` |

* **5 vtable slots** of `008A17A4`: +0x30, +0x3C, +0x54 (natives), +0x1C, +0x40 (log).

### 16.6 Tests (offline)

`wmtest video <PC executable>` writes `test\wmvideo.ini`.
* **Executable file: 44/44 PASS.**
* **Self-test: 16/16 PASS.** The real handlers run on a fake script stack, fake players and a fake display, with an
  older stack entry kept below the arguments. Covered:
  * the word table;
  * no current video (the same stack effect as the stubs);
  * a K3D_BinkPlay wrapper making a texture current;
  * length 14.7 s for 441 frames at 30 fps;
  * PauseCurrentVideo with and without option bit 0;
  * GotoCurrentVideo(12);
  * StopCurrentVideo clearing the tracking;
  * the background-video fallback for an unloaded texture;
  * Switch plus StopIfRunning;
  * the Wii initial preload state;
  * Preload(0), Preload(k) and a repeated Preload(k);
  * CancelPreload ignored without a current video, and clearing the key with one;
  * the decode accounting (a stall, an 8-frame jump, a loop, the time factor);
  * the call log (4 calls with one change → 2 lines).

### 16.7 In-game test plan

1. `[general] log=1`, `[video] log=1`. The start of the log shows `VIDEO: PC executable verified: natives=1 log=1;
   28/28 words in the native table (28 registration entries), 2/2 player hooks`. If the BVO module registers later,
   the count is below 28 and a later line reports the words installed after start-up.
2. Play into the hub. Look at the loading video's `video starts` / `end of video` pair and the BVO_Run, BVO_IsRunning
   and BVO_CursorGet lines around it.
3. A tutorial with a video: the first calls of K3D_BinkPreload, K3D_BinkIsPreloadFinished and K3D_BinkIsRunning.
4. `[video] natives=0` brings back the stubs; `log=0` removes all hooks.

---

## 17. Frame pacing (`wm_timing.cpp`, `[timing] fps_cap`)

### 17.1 Measurement and decision

The title screen was measured by sampling DT at `00A928BC` for 5 s:
* **Without `/vsync`**: median 1.0 ms (~1000 fps), max 9 ms.
* **With `/vsync`**: median 5.0 ms (~200 fps), min 1 ms, max 20 ms.

Vsync alone does not give the Wii's 60 Hz, so frame pacing is part of the port: **`fps_cap=60` is the default**, and
0 turns it off.

### 17.2 Hook site

| code | role |
|---|---|
| main loop `00409370` | GetFocus, PeekMessage / Translate / Dispatch pump, GetWindowRect → `FUN_006CC1A0`, then `call ViD::OneFrame` at `00409418`, the only call of OneFrame. No limiter, no Sleep |
| ViD::OneFrame `00503750` | input poll `FUN_006CC740`, reinit check, `call TIM_UpdateBeforeFrame` at `00503781` (only call), …, Render, …, `call TIM_UpdateAfterFrame` at `00503A15` (only call and the last call before `ret`) |
| TIM_UpdateBeforeFrame `006D5AB0` | depth counter `00A928B0`; DT `00A928BC` = the last measured value `00A928C4`; frame start = QueryPerformanceCounter → `00A92898`; paused time `00A928B4` = 0 |
| TIM_UpdateAfterFrame `006D5B80` | elapsed = QPC − start − paused, minimum 0.001, ForceDT `00A928C8`, TIM_MaxDT `00A928CC`; `00A928C0` = dt, `00A928C4` = dt × time factor `009D8DA8`; DT `00A928BC` = 0 until the next frame |

A wait placed just before the call at `00503A15` is inside the measured window, so the engine's DT includes it. A wait
in the main loop or before TIM_UpdateBeforeFrame would not be counted, and DT would stay at ~1 ms. `00503A15` is
therefore the only safe hook that gives the engine a 1/60 DT.

### 17.3 Algorithm

* **Hook**: the call at `00503A15` is redirected to the pacer (cdecl, no arguments). At depth 1 with a cap it waits,
  then calls TIM_UpdateAfterFrame:

  `wait = clamp(period + paused − (now − frame start), 0, period)`

* **Wait**: Sleep in whole milliseconds under `timeBeginPeriod(1)` until 1.5 ms remain, then spin with
  `YieldProcessor` up to the deadline.
* **Result**:
  * DT is the period exactly; the pacing self-test measures a 16.667 ms median.
  * The frame rate is 1 / (period + the time outside the window: message pump and input poll, ~0.1–0.5 ms), just
    below the cap. The Wii's measured window leaves out the same work.
* **No catch-up**: every deadline is relative to that frame's own start. A long frame (loading) is not waited on and
  is followed by normal frames, never by a burst of short ones.
* **Vsync**: pacing works with and without `/vsync`, because a Present that blocks inside Render is part of the elapsed
  time.
* **`fps_cap` values**: Hz as a float. 60 is the default, 59.94 is exact NTSC, 50 is PAL. Values below 10 mean off;
  the maximum is 1000. With `fps_cap=0` and `[general] log=1` the hook only measures.
* **Live changes**: the pacer reads the period at every frame; the Options screen's FRAME RATE (§20.4) sets it
  (`TIMING: frame rate cap 144.00 Hz (Options screen)`, `TIMING: frame rate uncapped (Options screen)`). The hook must
  be installed, so `fps_cap=0` needs `[general] log=1` for the setting to work.
* **Log** (`log=1`) every 10 s: `TIMING: fps_cap 60.00: N frames in 10.00 s = X fps, frame period median P ms, engine
  DT median D ms (min, max), average wait W ms, K frames over budget`.

### 17.4 Verification (8 checks)

* The three calls above: E8 target plus CRC32 of the 0x20 bytes around each.
* The CRCs of TIM_UpdateBeforeFrame and TIM_UpdateAfterFrame.
* In the file only: exactly one call/jmp reference each to TIM_UpdateAfterFrame, TIM_UpdateBeforeFrame and
  ViD::OneFrame, so no other path opens or closes the frame window.

### 17.5 Tests (offline)

`wmtest timing <PC executable>` writes `test\wmtiming.ini`.
* **Executable file: 8/8 PASS.**
* **Self-test: 11/11 PASS.** Covered:
  * wait values (1 ms of work, a frame over the period, paused time, the bounds);
  * the sleep plan (Sleep(14) then spin);
  * the median helper;
  * a simulated engine loop of 600 frames with 1 ms of work, 0.3 ms outside the window and one 50 ms stall: DT median
    16.6667 ms, the stall frame 50 ms, the next frame 16.6667 ms, no short frames, wall rate 58.75 fps;
  * 5 ms frames with 2 ms paused (DT 16.6667 ms);
  * 25 ms frames (not slowed);
  * a real-clock run of 30 paced frames: period median 16.667 ms.

### 17.6 In-game check

1. `log=1`. The start of the log shows `TIMING: PC executable verified: the call to TIM_UpdateAfterFrame at 00503A15
   paces frames to 60.00 Hz`, then a TIMING line every 10 s.
2. Title screen: DT at `00A928BC` has a median of 0.0167, with and without `/vsync`. The TIMING line gives the engine
   DT median and the achieved rate.
3. `fps_cap=0` gives back the uncapped behaviour of §17.1.

## 18. Script natives answered like the Wii (`wm_script.cpp`, `[script]`)

### 18.1 Why

The natives the Wii script module imports (855) were compared with the PC executable's implementations. Most are
real. The empty ones are debug output (`DBG_*`), Wii system prompts (`IO_JoystickRequired`,
`IO_JoystickExtensionRequired`, `IO_JoystickDisconnectionAlert`, `IO_JoystickEnableHomeButton`, harmless with an
always-connected remote) and three whose answers change the game:

* **`ViD_PlatformCurrentGet`**: the PC handler `00545E40` calls `CFG_e_GetCurrentPlatform` `005EC610`
  (`xor eax, eax`), so the PC answers 0; the Wii answers 1. The scripts both releases share branch on it:
  * `PJ_BunniesGroupObject` sets `ai_control_type[0..1] = 2` on 1, the rabbid groups' remote + Nunchuk control type;
  * `GST_MapManager_Bunnies` and `GST_Global_Bunnies` pick the groups' controller ids on 1;
  * the Wii-only `CUSTO_LIB` and `MUSICMAKER_LIB` take their Wii paths on 1.

  With the virtual Wii remote, the PC answer ran the rabbids with the PC control type.
* **`IO_JoystickPointerStateSet(id, state)`**: the PC handler `005EE5E0` pops both arguments and calls an empty
  function (the same handler serves `IO_JoystickEnableAimingMode`). The Wii scripts switch the pointer off while a
  video or cinematic runs (`GST_Global_Bunnies`) and back on (`activator_event`, `GST_Photo`).
* **`WII_ReturnToMenu`**: the shared no-op handler `00560580`. The Wii leaves the game for the system menu.

The file natives (`IO_FileOpenRead` .. `IO_FileOpen`, words `00B4FFFF`..`00BEFFFF`) go the other way: the PC's are
real (fopen / fgetc / fwrite / fclose) and the Wii's never reach a disk. On the Wii `IO_FileOpenRead_C` `800C4700` and
`IO_FileGetI_C` `800C4B80` answer -1, the GetC / GetF / EOF wrappers -1 / -1.0 / 1, a read `IO_FileOpen` -1, and the
writes go to the development kit's host over HIO2 (`LogFileHandlerRequest` `8001F82C`, `LogFileWrite` `8001F9A4`),
nothing on a retail console. The PC handlers pass a failed fopen's NULL on to the CRT, whose invalid-parameter check
ends the game (exception `c000000d` in `msvcr80!fwrite`): `CUSTO_LIB_SetJpeg` (the CUSTO_LIB of the PC's
`PNJ_CannonBall`, called when the paint mode is left, a texture is applied or a figurine is saved in the Wii remote)
dumps the rabbid's JPEG to `C:\Documents and Settings\All Users\Bureau\Custo.jpg` whenever `ViD_PlatformCurrentGet()`
is not 1, which it is not with `platform=pc`. The only PC script that uses these natives is that dump.

`ViD_PauseAllButMe` looked stubbed in the export scan but is real (its handler `00545F80` calls `ViD::PauseAll`
`00503390`). `ViD_IsRetailVersion` answers 1 on the PC, the right value for the retail game.

### 18.2 Patches

| Word | Native | Engine handler (length, CRC32) | Registration entry | Replacement |
|---|---|---|---|---|
| `0770FFFF` | ViD_PlatformCurrentGet | `00545E40` (0x45, `6AFB1F4F`) | `00544630` (`mov [esp+disp32]`) | pushes the configured platform |
| `00A3FFFF` | IO_JoystickPointerStateSet | `005EE5E0` (0x45, `5E09E42C`) | `005432F1` (`mov [esp+disp8]`) | keeps the state per controller 0..3 |
| `0915FFFF` | WII_ReturnToMenu | `00560580` (0x08, `BAC82FF4`) | `005458F3` (`mov [esp+disp32]`) | closes the game's main window |
| `00B4FFFF` | IO_FileOpenRead | `005EE6A0` (0x6E, `B59404E7`) | `0054349B` (`mov [esp+disp8]`) | pops the path, pushes -1 |
| `00B5FFFF` | IO_FileClose | `005EE830` (0x31, `1079FADB`) | `005434CF` | pops the handle |
| `00B6FFFF` | IO_FileGetC | `005EE870` (0x73, `33A759A2`) | `00543503` | pushes -1 |
| `00B7FFFF` | IO_FileOpenWrite | `005EE710` (0x6E, `2AE878D5`) | `00543537` | pops the path, pushes -1 |
| `00B8FFFF` | IO_FilePutC | `005EE8F0` (0x46, `61F0DEC5`) | `0054356B` | pops handle and value |
| `00B9FFFF` | IO_FilePutI | `005EE9C0` (0x53, `AFF635E9`) | `0054359F` | pops handle and value |
| `00BAFFFF` | IO_FileGetI | `005EE940` (0x77, `DE6EC238`) | `005435D3` | pushes -1 |
| `00BBFFFF` | IO_FilePutF | `005EEAA0` (0x60, `8DC4F56A`) | `00543607` | pops handle and value |
| `00BCFFFF` | IO_FileGetF | `005EEA20` (0x77, `C39DB3FC`) | `0054363B` | pushes -1.0 |
| `00BDFFFF` | IO_FileEOF | `005EEB00` (0x82, `FF7970C5`) | `0054366F` | pushes 1 |
| `00BEFFFF` | IO_FileOpen | `005EE780` (0xAC, `CFCFE39A`) | `005436A3` | pops 4 arguments, pushes -1 |

* Every wanted word is verified first (handler CRC32 and its `ViD::RegisterScript` entry: the `mov` opcode form, the
  handler and `push word` nearby); on any difference nothing is patched.
* The registration entries are rewritten in DllMain, so the table the engine builds afterwards holds the new
  handlers; the table entries are also patched in place when they already exist.
* While controller n's pointer is off, its IR pointer reads as not valid: the virtual remote's samples in
  `[controls] mode=wii` (`wm_input.cpp`), the pointer natives in `mode=pc` (`wm_ctl.cpp`, controller 0).
* The main window is closed with WM_CLOSE (visible top-level window of the process without an owner); the engine
  ends the process on it.

### 18.3 Configuration

    [script]
    enabled=1              ; 0 = the PC executable's own answers (the file natives included)
    platform=auto          ; wii (1), pc (0), auto = wii with [controls] mode=wii, pc with mode=pc
    pointer_state=1        ; IO_JoystickPointerStateSet works
    return_to_menu=quit    ; quit or none

With `platform=pc` the shared scripts use their PC control types, the basis of PC-native controls (`[controls]
mode=pc`).

### 18.4 Tests (offline)

`wmtest script <PC executable>` writes `test\wmscript.ini`.
* **Executable file: 14/14 PASS** (the rows above).
* **Self-test: 16/16 PASS**: `ViD_PlatformCurrentGet` pushes 1 (wii) and 0 (pc); `IO_JoystickPointerStateSet` pops two
  arguments and turns controller 0 off, on, and ignores ids outside 0..3; `pointer_state=0` keeps the pointer on;
  `WII_ReturnToMenu` has no stack effect; the disp32 registration check matches and rejects another word or handler;
  the file natives pop their arguments and push the Wii's answers (opens -1, `IO_FileGetI` -1, `IO_FileGetF` -1.0,
  `IO_FileEOF` 1; `IO_FilePutI` and `IO_FileClose` push nothing).

### 18.5 In-game check

1. `log=1`. The start of the log shows `SCRIPT: PC executable verified: platform 1 (Wii, auto), pointer_state=1,
   return_to_menu=quit; 0/14 words in the native table, 14 registration entries`: the table is built after DllMain,
   so the rewritten registration entries fill it (the video module reports the same).
2. Boot and PLAY: `SCRIPT: IO_JoystickPointerStateSet(0, 0): pointer off` while the videos run, `... pointer on`
   afterwards; `first call: ViD_PlatformCurrentGet() -> 1 (Wii)` when the hub town loads.
3. The rabbids push the cart and follow the stick as before.
4. In the Wii remote with `platform=pc`: paint the rabbid and leave the paint mode, apply a texture, save a figurine.
   The log shows `SCRIPT: IO_FileOpenWrite("C:\Documents and Settings\All Users\Bureau\Custo.jpg") -> -1: the Wii's
   file natives open no file` and the game goes on (it used to end with `c000000d` in `msvcr80!fwrite`).

## 19. Wii after effects (`wm_afx.cpp`, `[video] afx`)

### 19.1 Why

Most levels carry an AFX modifier: an after effect the Wii executable draws over the 3D image every frame. The records
are the same in both releases (53 in the Wii archive: 39 OldMovie, 6 Remanence, 5 ColorCorrection, 2 DepthBlur,
1 BigBlur). The PC executable registers them like the Wii (`AFX::ApplyAlways` `00567B40` fills the display's table at
`+0x68444`, count `+0x684A4`) but never draws them:

* its switch, the global `009DE174`, is set to 0 by the display constructor (`00418636`) and changed only by a
  PC-only script native (id `0x232A`, handler `005226B0`) that the Wii scripts never call. `K3D::AFX_User`
  (`004451A0`) returns at once while it is 0. The Wii turns its switch on for good in `K3D_S::AFX_Init`
  (`+0x1AF50 = 1`); only the debug console's AFX command turns it off.
* even with the switch on, the PC draws an effect only through the HLSL model its instance names, and the release
  has none: `K3D_HLSL_Instance::Execute` (`0046A540`) returns when the model is NULL.

The result is the PC look of the levels: no colour grade (saturation, contrast gain up to 1.5, brightness offset),
no soft glow and no tint.

### 19.2 Hook

`K3D::Render2D` calls `AFX_User(0)` right after the 3D image (before the 3D strings and the 2D layers) and
`AFX_User(1)` at its end (`0086A424`, `0086A5AB`), like the Wii. `AFX_User` is replaced by a `jmp` to the DLL, which
keeps its checks (`u32_GetDrawGlobalMask` bit `0x400000`, display flags `+0x4DC` bit `0x100000`, the entry's mode and
viewport mask against `u32_GetEngineViewNum`) but not the switch, and draws each registered effect with Direct3D 9
on the engine's render target and viewport. The device's state is captured in a state block before and applied after;
`IDirect3DDevice9::Reset` is hooked to release the DLL's render targets first.

Verified before patching (otherwise nothing is installed): `AFX_User` (length 0x1AE, CRC32 `FF4FD461`),
`u32_GetDrawGlobalMask` (`903F7692`), `u32_GetEngineViewNum` (`6512C3EB`), both calls in `Render2D` and the table
write in `K3D::AFX_SetHLSL` (`0086A9E8`).

### 19.3 Parameters

The Wii builds an effect from its instance's model variables and then its saved variables (`K3D_S::AFX_b_Build`),
each class matching variable names by case-insensitive substring (`SetParamsFromVars`). The PC instance holds the same
lists (`+0x14`/`+0x18` model, `+0x1C`/`+0x20` saved; 24-byte entries: name, flags, type, size, data). Defaults are the
constructors'.

| Type | Class | Names matched (first match wins) | Defaults |
|---|---|---|---|
| 10 | OldMovie | CircleRatio, BlurFactor (at most 0.003125), RemanenceFactor, CenterWidth, Saturation, Contrast, Brightness; vec3 vBorderColor, else Color (the centre colour) | 0.7, 0.005, 0.1, 0.7, 0.5, 1, 1; (0,0,0.5), (0.5,0.5,0) |
| 4 | Remanence | glowfactor, bigblurfactor | 0.5, 0.085 |
| 3 | ColorCorrection | vec3 vcoloradd; colorbrightness, factorbrightness, colorsaturation, factorsaturation, colorcontrast, factorcontrast, coloraddcoef | 0.5, 1, 0.5, 1, 0.5, 1, 0 |
| 1 | BigBlur | BigBlurFactor | 0 |
| 2 | DepthBlur | zend, zstart, bigblurfactor | not drawn (logged) |

### 19.4 Drawing (the GX arithmetic)

Konst factors are bytes: `k = trunc(255 f) & 0xFF`, `k' = k + (k >> 7)`. A TEV lerp or scale by a konst weighs
`k'/256`; ONE scaled by a konst adds `(255 k') >> 8`; a konst scaled by the source adds about `k/255` of it; a konst
added as is adds `k/255`. Every stage clamps to [0, 1]. The grey of the saturation stages is the red channel.

* **BigBlur** (`AFX_S::BigBlur`): a 2x box-filtered copy of the frame at 320x240 (the PC frame is first scaled to
  640x480, the Wii's image), then passes at UV offset `f = min(2 factor, 0.0125)`, halved per pass, drawn only while
  the next half is above 1/640. A pass is a 4-tap cross: `+u`, lerp 1/2 `-v`, lerp 3/8 `-u`, lerp 2/8 `+v`. The result
  is stretched back bilinearly.
* **OldMovie** (TEV chain over the frame copy S): saturation below 1 `lerp(S.rrr, S, k)`, above 1
  `S + max(S - S.rrr, 0) k`; contrast above 1 `X (1 + k)`, below 1 `lerp(X, 128/255, k)`; brightness `X ± k`;
  remanence (when > 0) `X + k B`, B the BigBlur of S (copied back at half size with the box filter, as the Wii copies
  the stretched blur) or S itself without a blur factor; tint `X + trunc(25.5 colour)/255` when a centre colour
  component is above 0. CircleRatio, CenterWidth and vBorderColor are read but not drawn, as on the Wii.
* **Remanence**: `S + a B`, `a = (k k') >> 8` (glow factor squared in 8 bits), B the stretched BigBlur.
* **ColorCorrection** (blended quads on the Wii, one shader here): add colour (ONE / INVSRCALPHA), saturation below 1
  lerp to the red grey by `byte(1 - s)`, above 1 `X + max(X - X.rrr, 0) k`; contrast above 1 one or two gains
  `X (1 + k/255)`, below 1 lerp to 127/255; brightness `X ± byte(d)/255`.
* An effect that leaves the image unchanged (e.g. OldMovie with only a blur factor) is skipped.

### 19.5 Variables: the fades and the black-and-white mode

Scripts change a built effect with `MDF_Setf` / `MDF_Setv` (variable index `(b << 16) | a`). The global fade
(`GST_GlobalFade` in `GST_Global_Bunnies`, the same in both releases) runs every screen fade this way: modifier
(29, 13) is the universe's full-screen ColorCorrection (mode 1, drawn over the 2D too); the script writes variable 5
`vColorAdd` (the fade colour), 6 `fColorAddCoef` (the fade amount) and 3 `fColorSaturation` (0 for its
black-and-white mode), and blends variable 2 of modifier (29, 12).

* **Wii**: `SetParamsFromVars` binds each model variable it matches to its field (the StoreVar's pointer at `+0x18`);
  `AFX::IFVSetf` / `IFVSetv` write through that pointer and `IFVGetf` / `IFVGetv` read through it; a variable
  without a field, or an effect not built yet (built the first time `AFX_User` draws it), reads 0 and ignores writes.
  Saved variables are copied at the build without binding, so a later write replaces them. Nothing resets the fields
  until the instance is destroyed.
* **PC**: the natives (`MDF_Setf_C` `00540A30`, ... ) call the AFX vtable slots `+0x8C` `+0x90` `+0x98` `+0xA0`
  (`006232A0` `00623360` `006232E0` `00623320`), which store into the instance's variable buffer; nothing draws it, so
  the PC release never showed these fades.
* **DLL**: the four slots are replaced. Setf/Setv call the PC's function and then write the bound field of the DLL's
  built effect (kept per instance: parameters and the variable -> field binding of 19.3); Getf/Getv read the field
  (0, or a zero vector instead of NULL). The instance's state is dropped at its destruction: the two calls of
  `K3D_HLSL_Instance::Destroy` (`0086A9B0`, from `AFX::Destroy` `00567ACE` and `EVE::AFXEvent_Destroy` `005E15FA`)
  go through the DLL.

### 19.6 Configuration

    [video]
    afx=1          ; 0 = the PC look (AFX_User and the variable slots untouched)
    afx_dump=0     ; 1 = afx_NN_before.png / afx_NN_after.png next to the DLL when the settings change (at most once
                   ; a second; 20 pairs, or N pairs with afx_dump=N)

With `[general] log=1` the log shows each effect set (a changing one at most once a second), e.g. `AFX: mode 0 effect
1/1: OldMovie saturation 1.200 contrast 1.100 brightness 1.040 remanence 0.300 blur 0.00313 tint (0.00 0.00 0.00)`,
and changed variable writes (each variable at most every 2 s), e.g. `AFX: MDF_Setf: ColorCorrection variable 6
(fColorAddCoef) = 0.960`.

### 19.7 Shaders (`rghport assemble`)

The PC release lights the Wii materials in HLSL (`include/WIICommon.fxh`, `core/Wii_MaterialsTemplates.fx`) with
lights packed like the Wii's (`v0` = a0 a1 a2 k0, `v1` = direction k1, `v2` = colour k2, `v3` = position and a flag
for attenuation) and blends the glow in `core/Basic_Glow.fx`. `assemble` changes the port folder's copy where that
HLSL differs from the Wii (`rghport/convert/shaders.py`; each change replaces an exact piece of the release's text):

* spot cosine: `max(0, dot(L, -direction))` (`GXInitLightDir` stores the negated direction; the HLSL took
  `abs(dot(L, direction))` and lit a mirrored cone behind spot lights); angle attenuation not capped at 4;
* every light's distance attenuation starts from 1 (the HLSL reused the previous light's for a light without terms);
* normals: the direction of the inverse transpose of the world matrix, normalised (the Wii loads
  `PSMTXInvXpose(model-view)` as the normal matrix and lights unit normals; the HLSL used the world matrix, so scaled
  objects were lit brighter or darker);
* the lightmap coordinates keep UV2 (the vertex shader overwrote them with the lit colour, the cartoon ramp lookup);
* glow: the frame plus the blurred glow scaled by the glow factor (`AFX_S_Glow::Apply`); the HLSL used the brighter of
  the blurred and the unblurred glow image.

`check` reports whether the changes are present.

### 19.8 Tests (offline)

`wmtest afx <PC executable>` writes `test\wmafx.ini`.
* **Executable file: 17/17 PASS**: `AFX_User`, `u32_GetDrawGlobalMask`, `u32_GetEngineViewNum`, the four variable
  functions and `K3D_HLSL_Instance::Destroy` (CRC32); the two `Render2D` calls, the `AFX_SetHLSL` table write and the
  two `Instance::Destroy` calls; the four AFX vtable slots.
* **Self-test: 20/20 PASS**: OldMovie defaults, model then saved variables (substring match, BlurFactor clamp), the
  binding of model variables (not saved ones) and a write after the build, the OldMovie konsts of a level record
  (saturation k 51, contrast k 25, brightness adds 9/255, remanence k 76), the identity case, the tint konst, Remanence
  alpha (0.55 -> 77), ColorCorrection names, the fade effect (neutral until written; variables 3, 5, 6 bound; a white
  fade and black-and-white draw), konst byte wrap, the BigBlur schedule for 0.003125, 0.085 and 0.0056 and the no-blur
  cases.

### 19.9 In-game check

1. `log=1`, `afx_dump=1`. The start of the log shows `AFX: PC executable verified: K3D::AFX_User replaced (...);
   effect variables (MDF_Setf/Setv/Getf/Getv) kept like the Wii`.
2. Boot: `AFX: mode 1 effect 1/1: ColorCorrection ... x 0.000` (the fade effect), `MDF_Setf: ColorCorrection variable 3
   (fColorSaturation) = 1.000`, `MDF_Setv: ... variable 5 (vColorAdd) = (0.000 0.000 0.000)`.
3. Load a level: the fade to black and back (`... x 0.960`, `x 0.999`, `x 0.081`; the dumps' after images go black);
   the level's own effect set; its `afx_NN_before/after.png` pair shows the grade (level A1: mean brightness 33
   before, 56 after).
4. The frame rate stays at the cap (the effects cost about 2 ms per frame at 1280x720).

## 20. Options screen (`wm_options.cpp`, `wm_gfx.cpp`, `[options]`)

### 20.1 The screen

The converter adds the screen to the user's own Magma blobs (`rghport/convert/menus.py`; nothing is shipped):

* **Pause menu** (`InGame.mgb`, page `IGM_P_E3`): CONTINUE / RESTART / SUBTITLES / **OPTIONS** / EXIT. OPTIONS is a
  copy of the EXIT label and mouse area (mouse mask 64 through `INGAME_OverElement`); EXIT moves one row down and
  appears one step later, and the page's appear sounds get one more whoosh. The pause script
  (`InGame_MagmaMenu_State_EXEC`, script override) pushes the Options page when OPTIONS is chosen and waits while it
  is the top page. It pushes the page only while key `0x07` reads pressed (the DLL's layer flag, written with
  `[controls] mode=pc`, §20.2), so OPTIONS does nothing without the DLL or in `mode=wii`.
* **Options page** (`Common.mgb`, page `PCOPT_P_Options`): the pause menu's swirl vignette, dimmed screen, blue label
  buttons (a copy of the in-game label area, `PCOPT_A_Label`) and yellow selection arrows (`PCOPT_A_Arrows`). Three
  tabs: CONTROLS, AUDIO, GRAPHICS; DEFAULTS and BACK at the bottom. Every element the DLL drives has a generic object
  `PCOPT_G_<name>` (names and layout in `menus.py`; the DLL's hit tests and arrow positions use the same numbers).

The DLL notices the page on top (`MGM_GetTopLevelPageID_C` `005EB9A0`) once per frame, in its call at `0050386B`
(`ViD::OneFrame`, right before the Magma update), and drives it through the Magma script exports:
`MGM_SetIntProperty_C` (`005EB5E0`: frame 77, visibility 115, position 32, size 33), `MGM_SetFloatProperty_C`
(`005EB610`: scale 34), `MGM_SetWStringProperty_C` (`005EB670`: text 21), `MGM_PopMouseCursor_C` and
`MGM_PopPage_C` on BACK. Only changed values are sent (a frame index sent again restarts the label animation). Labels
use the label area's timeline: frame 0 selected, 10 normal, 40 / 50 greyed.

Input on the page: arrows / D-pad / left stick (repeat after 0.4 s), Enter / Space / pad A, Esc / Backspace / pad B /
Start / right click (back), Delete / pad X (clear a binding), Tab / Shift+Tab / LB / RB (tabs), mouse (hover, click,
drag the volume bars). While the page is up, and afterwards until every input is released (at most 2 s), the game
sees no keys, mouse buttons or pad buttons.

### 20.2 Controls (`[controls] mode=pc`)

Nineteen actions, each with three slots: MOVE FORWARD / BACK / LEFT / RIGHT, DASH, RUN, SCREAM, THROW, GRAB, SUPER
BOOST, SWAP PLAYERS, PAUSE, and the seven motion rows of §22 (SHAKE LEFT/RIGHT, SHAKE UP/DOWN, MARACA SHAKE, TILT LEFT,
TILT RIGHT, SHAKE CLOCKWISE, SHAKE COUNTER-CW; the list shows eight rows and scrolls). A slot holds a keyboard key, a
mouse button, a pad button (XInput names; `PAD_BIT<n>` for other pads), a stick direction, a fast mouse wiggle
(`MOUSE_SHAKE_X`, `MOUSE_SHAKE_Y`), the mouse moved in circles (`MOUSE_CIRCLE_CW`, `MOUSE_CIRCLE_CCW`) or the scroll
wheel (`WHEEL`, `WHEEL_UP`, `WHEEL_DOWN`; §22.3). Defaults: the PC release's keys and the buttons of its pad mapping. Shift, Ctrl and
Alt are one binding each for both sides (`SHIFT`, `CONTROL`, `ALT`: the keyboard state's combined `VK_SHIFT` /
`VK_CONTROL` / `VK_MENU`, which either key sets; the PC scripts read both sides too). `LSHIFT`, `RALT` and the other
side-specific names or codes load as the combined key, and an input listed twice for one action keeps one slot. When
a binding is captured, Alt is taken before Ctrl (AltGr presses both).

The PC scripts read keys and pads directly. The port's PC-controls script overrides read one virtual key per action
instead whenever key `0x07` reads pressed, in the form `(layer && virtual key) || (!layer && original read)`:
`0x88`-`0x8B` movement, `0x8C` dash, `0x8D` run (keyboard), `0x8E` run (pad, held), `0x8F` scream, `0x97` throw,
`0x98` grab, `0x99` boost, `0x9A` swap, `0x9B` pause (codes the engine's keyboard state never receives from Windows).
In single player the bunnies' scripts read both player slots; the layer applies to a slot without its own pad, so
player 1's inputs work in either slot.

Right after the engine's input poll (`0050375A` -> `006CC740`: `GetKeyboardState`, controllers, mouse) the DLL
evaluates every action from its bindings: keys and mouse buttons from that keyboard state (`[00A97B00]`), pad buttons
from the raw button word the controller update read (its calls at `006EC8C7`, `006EC8E3`, `006EC8FF` are wrapped),
sticks through the engine's reader and dead zones (`006FCDF0`, `006ECA50`). It writes the virtual keys and the layer
flag into the keyboard state; with a pad connected the movement actions also become the pad's left stick of
controller 0. Menus keep their own keys.

### 20.3 Volumes

MASTER, MUSIC, EFFECTS, VOICES, 0-100 % in steps of 5. The sound engine keeps an offset per group in millibels
(`SND_GroupOffsetSet_C` `006286A0`, stored clamped to [-10000, +600] by `0049CE10`); a group's volume is its offset
plus its base volume plus its parent's volume (`0049C3E0`). The mix of the Wii archive (group <- parent): `10` SFX,
`11` AMB, `13` DIALOG, `14` HUD <- `1` USER_SFX; `12` MUSIC <- `2` USER_MUSIC; `39`-`41` (radio) <- `12`; `1`, `2`
<- `0` MASTER. The sliders set: master `0`, music `2` (or `12` when the mix has no USER_MUSIC above it), effects
`10`, `11`, `14`, voices `13`. A slider at p % sets 40 log10(p / 100) dB (the gain squared: 50 % is -12 dB, 10 %
-40 dB); 0 % is -100 dB.

The mix files put some sound sets in the group of another kind (jingles in MASTER, radio songs in the ambience
groups). The converter moves them (`rghport/convert/sound.py`, `regroup_set`): a set whose streamed samples are music
(names starting `MUS_`, `RADIO_`, `CUSTO_ZIK`, or containing `MUSIC`) goes to MUSIC unless it is already below
USER_MUSIC; an effects set in MASTER goes to SFX. A set moves only when the summed base volumes of the two groups are
equal, so nothing sounds louder or quieter at 100 %. The counts are in the conversion summary
(`sound_sets_regrouped`).

### 20.4 Graphics (`wm_gfx.cpp`)

| Row | Values | How |
|---|---|---|
| DISPLAY | WINDOW, BORDERLESS | a popup window over the monitor (style and rectangle restored for WINDOW) |
| RESOLUTION | NATIVE (the window's size), 360, 480, 540, 720, 900, 1080, 1440, 2160 lines (the width follows the window's shape) | the game renders at that size and the picture is stretched over the window (below) |
| VSYNC | ON, OFF | the renderer's configuration `+0x20` and one device reset |
| FRAME RATE | 30, 60, 120, 144, 165, 240 FPS, UNCAPPED | the frame pacer's cap (§17), changed live; the default is `[timing] fps_cap`. Above 60 the game's DT is shorter than the Wii's 1/60 s, which the scripts written for 60 Hz do not all expect (uncapped, the rabbids following the player lagged behind) |
| HIGH DETAIL | ON, OFF | `K3D_ForceNoLod_C` (`005EC770`) stores 1 at display `+0x69410`: every model at its best level of detail; written every frame while on |
| TEXTURES | BILINEAR, ANISO 2X-16X | `IDirect3DDevice9::SetSamplerState` (vtable slot 69) wrapped: a linear minification filter becomes anisotropic (capped at the device's `MaxAnisotropy`) |
| WII EFFECTS | ON, OFF | the after effects of §19 (row greyed without `[video] afx=1`) |

Resolution. The display draws at its K3D screen width and height (display `+0x948` / `+0x94C`: `K3D::SetScreenWidth`
/ `SetScreenHeight`, which `ViD::RenderOneViewpoint` sets for each view). At the start of every frame the display
(`0041ABC0`) reads the client area (`0041ABF3`: `call [GetClientRect]`); when it changed it derives them from it (the
viewport fitted to `K3D_ScreenRatio` / `K3D_ScreenVSize`, §21) and stores the size in the renderer (`0041A2D0`), whose
Present shows that part of the back buffer on the same part of the window (`00486EF0`: source = destination). The DLL
replaces that call: the display reads the chosen height and the width of the window's shape, so the game renders at
that size, and `IDirect3DDevice9::Present` (vtable slot 17, wrapped) gets no destination rectangle, which stretches
the rendered part over the whole window. The mouse follows: the executable's imports of `GetCursorPos` /
`SetCursorPos` (`0089D4A0` / `0089D4D8`, used by the mouse update `006CC2A0` and `IO_MouseSetPos` `006CC540`) convert
between window and render pixels around the window's origin. NATIVE leaves every call as it was.

Back buffer. When the rendered size grows past the back buffer the display forces a device reset (`0041B023`:
`00488090` with force 1). The reset builds the present parameters from the renderer's configuration (`00487860`: size
`+0x10` / `+0x14`), which keeps the size the game started with, so a larger window or resolution got a back buffer
that was still too small. The DLL's call at `0041ABD0` (the device check with force 0, right before the client area
is read) raises the configured size first: to the monitor's size, or the rendered size when that is larger (at most
8000, the display compares 13-bit sizes). Growing a window therefore costs one reset, and windows can be dragged to
any size. The renderer object is the one in `[00A6E830]` and `[00A6E834]` (`00484E27`). With `/fullscreen`
(`00A72370`) the display keeps a fixed size: DISPLAY and RESOLUTION are greyed.

VSync. The same call passes force 1 once after a change. Each reset attempt waited 1000 ms before
`IDirect3DDevice9::Reset` (`00487140`, `push 1000` at `00487151`); the wait is 50 ms. At start-up the saved value is
also written to the `/vsync` option (`00A72378`), which the device creation reads (`0041A709`); a `/vsync` on the
command line is parsed later (`00500F1D`) and overrides it, so the saved value is applied again, with a reset, when
they differ. The DEFAULTS button returns VSync to the command line's value.

The saved settings are applied 30 frames after start, once the window and the renderer exist.

### 20.5 options.ini

Written next to the DLL when the page closes with changed settings (a temporary file, then a rename):

    [controls]
    move_forward=W,UP,PAD_LS_UP
    dash=SPACE,PAD_A
    ...
    shake_sideways=MOUSE_SHAKE_X
    shake_updown=MOUSE_SHAKE_Y
    shake_maraca=WHEEL
    tilt_left=MOUSE1
    tilt_right=MOUSE2
    shake_clockwise=MOUSE_CIRCLE_CW
    shake_counterclockwise=MOUSE_CIRCLE_CCW
    [audio]
    master=100
    music=100
    effects=100
    voices=100
    [graphics]
    display=window        ; window or borderless
    resolution=0          ; the height rendered, stretched over the window; 0 = the window's size
    frame_rate=-1         ; the cap in Hz, 0 = uncapped; -1 = [timing] fps_cap
    vsync=-1              ; 1 / 0; -1 = the command line's
    high_detail=0
    texture_filter=0      ; 0 (the game's filters) or 2 / 4 / 8 / 16
    wii_effects=-1        ; 1 / 0; -1 = [video] afx

Unknown input names are logged and skipped. Settings files of earlier versions load too (their `width` / `height`
keys are ignored).

### 20.6 Configuration

    [options]
    enabled=1      ; 0 = no Options screen, bindings, volumes or graphics settings

With `[general] log=1` the log shows the verification, the sound group tree and the slider groups (`OPTIONS: volume
groups: master:0 music:2 effects:10,11,14 voices:13`), each binding change, saves, and the graphics changes
(`GFX: resolution 720 lines`, `GFX: rendering at 1280x720, stretched over the 1920x1080 window`, `GFX: back buffer
1280x720 -> 1920x1080 for 1920x1080`, `GFX: VSync on`, `GFX: device reset (settings changed)`, `GFX: device hooks:
texture filtering installed (anisotropy up to 16), present installed`).

### 20.7 Verification (49 checks)

Nothing is installed when one differs: the five wrapped calls (`0050375A`, `0050386B`, `006EC8C7`, `006EC8E3`,
`006EC8FF`; target and CRC32 of the 32 bytes around each) and 28 functions (input poll, keyboard and mouse natives,
the mouse update and its wheel message copy,
controller buttons, sticks, `MGM_Interface::GetManager`, the seven Magma exports, the three sound group functions).
The graphics settings also need 13 code ranges (device check, reset loop, present parameters, display frame start
and its `/fullscreen` test and grow reset, windowed present, VSync interval, `K3D_ForceNoLod_C`, `createMainWindow`,
device creation, mouse update, `IO_MouseSetPos`), the calls at `0041ABD0` and `0041ABF3` and the 1000 ms wait; the
resolution also needs the three imports (`GetClientRect`, `GetCursorPos`, `SetCursorPos`) to be user32's.

### 20.8 Tests (offline)

`wmtest options <PC executable>`:
* **Executable file: 49/49 PASS** (§20.7).
* **Self-test: 35/35 PASS**: input names (a side-specific Shift / Alt loads as the combined key; the wiggle and wheel
  names); defaults and the motion rows' defaults; mouse wiggles (§22.3: 4 Hz at 60 and 240 fps, stopping, 2 Hz, a slow
  wave and a single flick never count), mouse circles (3 turns a second either way at 60 and 240 fps with no x / y
  wiggle beside them, stopping, a flat ellipse stays a wiggle, half a turn a second never counts) and the wheel; a mouse button's tilt (held still, kept while shaking, never
  while dragging; a key at once); the mouse snapshot; capture of the wheel and of mouse circles on a motion row; the
  `options.ini` round trip (cleared slot, stick binding, a volume, every graphics setting); a list with an unknown name
  and four inputs; actions on a fake engine (several bindings at once, a rebound action, a stick the scripts switched
  off, no focus, the input mask); the volume curve; the page (open, idle frame, navigation, capture, scrolling, audio
  tab, mouse click on a bar, graphics tab rows and values, tab wrap, close); the frame rate presets (60, 120, back to
  30, below 30 uncapped); LALT,RALT,PAD_B,RSHIFT keeping ALT once.

`menus.py` on the Wii archive: the three `Common.mgb` blobs get the page (77 elements, 74 new generic objects) and
the three `InGame.mgb` blobs the OPTIONS entry; each result parses back and rebuilds byte for byte.

### 20.9 In-game check

1. Pause (Esc), OPTIONS: the page opens with the pause menu's look; BACK or Esc returns to the pause menu.
2. CONTROLS: bind a key to the third slot of an action (Enter, then the key); leave the page and use the key in a
   level.
3. AUDIO: each slider changes only its kind of sound; 0 % silences it.
4. GRAPHICS: BORDERLESS and back; RESOLUTION 480 (blurry, fills the window; the mouse still hits the menu items),
   2160 and NATIVE; drag the window larger (one reset, no black borders); VSync off and on; HIGH DETAIL on (distant
   models keep their detail); ANISO 16X (floor textures sharp at a distance).
5. Restart: the settings come back from `options.ini`.

## 21. Fixes that are always on (`wm_fixes.cpp`)

Both follow the "Controller Stutter + RGH Widescreen Fix" DLL (the widescreen fix was found by Duckymomo). Each is
installed only when its code in the PC executable matches; the log says `FIXES: PC executable verified: widescreen on,
controller scan stutter fix on`.

### 21.1 Widescreen

The configuration (`default.cfg`: `K3D_ScreenRatio = 3`, 16:9; `K3D_ScreenVSize = 720`; the same in the Wii and the
converted archive) gives the display a virtual screen height of 720 lines (`00501C8F` -> display `+0x930`). Each time
the client area changes, the display (`0041AC51`) fits a viewport of that ratio and at most that height into it and
centres it: width = VSize x ratio, height = width / ratio, width = height x ratio, each clamped to the client area. A
window larger than 1280x720 therefore showed a centred 1280x720 picture with black borders. The DLL sets the height to
10000, so the viewport is the largest 16:9 area of the window (all of a 16:9 window), and clears the display's stored
client width (`+0x958`) so it fits the viewport again at the next frame. A watcher thread keeps the value (the
configuration is read after the DLL starts).

### 21.2 Controller scan stutter

The controller update (`006ED470`, every frame from the input poll) enumerates the controllers again every 2 s (KPAD,
XInput and DirectInput: `006F53B0`, `006FCC90`, `006FC690`); each enumeration stalls a frame. After the first scan
(the last scan time `00A97AEC` is set) the timer test is made to skip them (`jbe` at `006ED4BE` -> `jmp`). The watcher
counts the game controllers in the raw input device list (HID usage page 1, usage 4 joystick / 5 game pad) every
second; when they change, one scan runs (the `jbe` is back until the last scan time changes, at most 3 s): `FIXES: game
controllers changed (1 connected): the game scans them again`.

### 21.3 Tests (offline)

`wmtest fixes <PC executable>`:
* **Executable file: 2/2 PASS**: the display's viewport code (`0041AC51`, length 0x76) and the controller update
  (`006ED470`, length 0x6C, with the `jbe`).
* **Self-test: 6/6 PASS**: the viewport for a 1920x1080 window with height 720 (centred 1280x720) and 10000 (the whole
  window), 1280x720 and 1024x768 windows (a 16:9 band), an 854x480 window with 720 (853x480); the raw input controller
  count read twice.

## 22. Wii remote motion in controls mode pc (`wm_motion.cpp`)

### 22.1 Why

Two parts of the Wii version read the remote's accelerometers and have no PC replacement:

* **The controls check that opens a new game** (world group `IZW_TRC`, track `CTRC_STATE_UPDATE`): shake the Nunchuk,
  shake the remote (`MTH_VecNorm(IO_JoystickAccelGet(0, sensor)) > 3`, and the page's own tests `|accel.y| > 1.3` /
  `1.5`), press A, then shake the rabbid inside the remote until its shake counter reaches the page's number.
* **Inside Zee Wiimote**: the rabbid inside the remote follows its shakes and its roll. `WII_LIB_MOVE` (procedure list
  `60000C18`) keeps the last six extremums of the accelerometer samples per axis (x = -accel.x, y = accel.z along the
  remote, z = accel.y) and averages them: a sideways (1), up and down (2), along the remote (3, a maraca) or circular
  (4 / 5) shake of 3.5 to 10 g (4.5 along the remote), 0.5 to 10 Hz, is the immediate move; it becomes the "smart"
  move the rabbid follows while a new extremum comes every 0.2 s. `CWII_LIB_GetJoyBanking` = (-x, z, -y) of the
  acceleration is the remote's orientation (12 reference orientations).

In controls mode pc there is no virtual remote (§13): `wm_ctl.cpp` answered these natives with a remote at rest.

### 22.2 The motion

`wm_ctl.cpp`'s `IO_JoystickAccelGet`, `IO_JoystickAccelSampleGet`, `IO_JoystickSampleNumberGet` and
`IO_JoystickHorizonGet` now answer from `wm_motion.cpp`, which builds one frame of motion right after the input poll
from the actions of the Options bindings (§20.2):

| Action | Motion |
|---|---|
| SCREAM (the attack) | the remote and the Nunchuk shake up and down (KPAD y) |
| SHAKE LEFT/RIGHT | the remote shakes along its x axis (WII_LIB_MOVE move 1) |
| SHAKE UP/DOWN | along its y axis (move 2) |
| MARACA SHAKE | along its length, z (move 3) |
| TILT LEFT / TILT RIGHT | the remote rolls 90 degrees while held, both: it turns over (180 degrees); 450 degrees a second, the short way back on release, nothing for the first 0.1 s |
| SHAKE CLOCKWISE / SHAKE COUNTER-CW | the remote is stirred in a circle facing the screen: x and y in quadrature (WII_LIB_MOVE moves 4 / 5) |

A shake is a 3.4 g sine (6.8 g peak to peak; WII_LIB_MOVE's averages read 8.6 g; some scripts want more than 3 g of
one axis, the paint page's sticker shake) at 5 Hz, or at the rhythm of the mouse wiggle, circles or wheel that drives
it, kept between 3.5 and 7 Hz (slower, the extremums come further apart than the 0.2 s
the smart move needs). It starts from rest, fades in over 0.06 s and out over 0.12 s. Each frame carries samples at
200 Hz like the remote (1 to 12), newest first, spaced DT / count as the scripts expect. KPAD frame as in §3: at rest
(0, -1, 0); a roll r reads (-sin r, -cos r, 0), horizon (cos r, sin r). +x is the remote's left side and a linear
acceleration a reads -a, so a remote stirred clockwise as the player sees it (position R (sin wt, cos wt) in right / up)
reads x = -A sin wt, y = -1 + A cos wt, and x = +A sin wt the other way round: WII_LIB_MOVE answers 4 and 5, and the
Shaker challenge's clockwise arrow is move 4 (checked in game: clockwise mouse circles score on it). While the Options page has the input the
remote settles. With `[general] log=1` each input is named as it starts and stops (`OPTIONS: motion: SHAKE LEFT/RIGHT
on (input rhythm 4.9 Hz)`).

### 22.3 Mouse wiggles, the wheel, mouse buttons

* **`MOUSE_SHAKE_X` / `MOUSE_SHAKE_Y`**: the engine's per-frame mouse movement (`[00A90F0C]`, `[00A90F10]` times the
  scale `[00A90F08]`, mouse update `006CC2A0`; it also works while the cursor is held in place) in heights of the 3D
  view. A stroke is movement in one direction; a turn ends a fast stroke when the stroke covered 4 % of the height and
  peaked at 0.8 heights a second. Two such turns in a row within 0.45 s start the wiggle; it stops when no turn comes
  for twice the time between turns. The shake takes the wiggle's rhythm.
* **`MOUSE_CIRCLE_CW` / `MOUSE_CIRCLE_CCW`**: the direction of the mouse's movement (frames slower than 0.5 heights a
  second are not counted) keeps turning the same way: 0.85 of a turn within 0.9 s, on a path whose smaller extent is
  at least 0.45 of the larger, starts the circle (clockwise on the screen = clockwise for the player); it takes the
  circles' rhythm and ends when the direction reverses (a change of more than 2 radians in a frame), the path goes
  flat or the mouse rests for 0.12 s. From 0.4 of a turn on, a circle's x and y strokes are not wiggles, and the
  wiggles need two new turns after it.
* **`WHEEL`, `WHEEL_UP`, `WHEEL_DOWN`**: the wheel bits of the mouse buttons word (`[00A90F04]` bits `08` / `10`, from
  the `WM_MOUSEWHEEL` message `006CC280` keeps). A motion row stays on for 0.3 s after a notch and takes the notches'
  rhythm; any other action sees one frame per notch.
* **A mouse button on a tilt row** also clicks and drags (Inside Zee Wiimote: left click = A, right click = B, §22.4):
  it tilts only when held with the mouse still (0.25 s with less than 3 % of the height of movement), then until it is
  released, so a tilted remote can be shaken with the mouse. Keys and pad buttons tilt at once.

### 22.4 The Nunchuk stick from the movement actions

The Wii-only scripts read controller 0's Nunchuk stick with `IO_JoystickStickGet(0, 0)` (the paint view's scroll, the
challenges' pause page, the music maker's instrument choice, the remote-control tool). Without a controller the
handler answers MOVE RIGHT - MOVE LEFT, MOVE FORWARD - MOVE BACK (length 1 at most) while such scripts run: one of the
accelerometer sample natives (`IO_JoystickSampleNumberGet`, `IO_JoystickAccelSampleGet`) was called within the last
0.5 s (Inside Zee Wiimote's shake detection calls them every frame; the levels' scripts only read
`IO_JoystickAccelGet` and handle the keyboard themselves under `!IO_JoystickHere(id)`). Any
other read (another controller or stick, a connected controller, no such script running) goes to the engine's handler
`005ED7C0`.

### 22.5 What the scripts need (the converter's script overrides)

The DLL only produces the motion. The Wii-only scripts that use it are the scripts phase's (rule 3); the development
pipeline's overrides hold them:

* Inside Zee Wiimote had no bytecode on the Wii (compiled C++): `WII_LIB_MOVE`, the rabbid's states and libraries, the
  Magma menu (`CUSTO_MagmaMenu`: its states, libraries and the page callback), the customisation manager, the drag
  and drop manager, the tools' rope and power cable, the hair chains, the figurines and the three challenges are
  written from that code.
* A state change keeps its track in a temporary (`t = "CR_STATE_SHAKED"; ... SCR_TrackCurChange(t)`): it has to compile
  to a track constant. A string node there gives the native a pointer to the characters (access violation at
  `00516350` reading the text as a pointer, at the rabbid's first detected shake).
* Without a controller the keyboard and the mouse answer the buttons these scripts read from controller 0: A = Enter /
  left click, B = Backspace / right click, 1 / 2, - / +, the D-pad = the arrow keys, Z / C, HOME = Home.

### 22.6 Tests (offline)

`wmtest motion <PC executable>`: **24/24 PASS**. The module's frames run through a C++ copy of WII_LIB_MOVE (the
list's initial values and `CWII_LIB_SetShakeAssistant`'s parameters): at 60, 144 and 500 frames a second SCREAM peaks
at 4 g on both sensors and reads as move 2, the three shake rows as moves 1, 2 and 3 within half a second and for as
long as they are held; wiggles of 2, 3.5, 6 and 12 Hz shake at 3.5, 3.5, 6 and 7 Hz; nothing held and a tilt alone
detect no move; tilt left, both (banking z = -1: turned over), right, release, an 83 ms click; samples per frame and
their spacing; the Options page stops a shake; SHAKE CLOCKWISE / COUNTER-CW read as moves 4 / 5 at the three frame
rates, circles of 2 and 12 turns a second stir at 3.5 and 7 Hz; the movement actions as the Nunchuk stick (not before
a script reads the accelerometers, (0.707, 0.707) for forward + right, never with a controller, 0.4 s after the last
read but not 0.6 s).

### 22.7 In-game check

1. New game: at "Shake your Nunchuk" hold SCREAM (three boxes, OK), again for the remote (the scripted "foreign
   object" error), Enter or left click for each "Press A", shake the mouse at "You can communicate with your Rabbid",
   Enter to start the adventure.
2. Inside Zee Wiimote: Enter, hold left click on a rabbid; inside, wiggle the mouse sideways and up and down, scroll
   the wheel (the rabbid is thrown about each way), hold left or right click without moving (the remote lies on its
   side), both (upside down).
3. Inside Zee Wiimote, MENU: Tools (a tool from the browser goes on its stand; drag it onto the rabbid: the vise's
   head is squashed by sideways wiggles and stretched by up and down ones), Paints (hold left click to spray, the
   movement keys scroll the view), Challenges: the Shaker asks for the five moves (sideways, up and down, the wheel,
   mouse circles either way; clockwise circles score on the clockwise arrow), the other asks for A / B / the D-pad
   (Enter / Backspace / the arrow keys).
4. Options, CONTROLS: the list scrolls to the seven motion rows; Enter on a slot, then scroll, shake or circle the
   mouse.

---

## 23. Mods (`wm_mods.cpp`, `wm_lua.cpp`, `[mods]`)

What a mod is and what it may contain is in [mods.md](mods.md). This section is the engine side.

### 23.1 Loose files instead of the archive

The engine reads its archive by key: `BIG::i64_KeySearchPos` (006E9DB0, `this` = the archive object at 00A70B9C)
answers `{hi, lo}` of the entry (`{-1, -1}` for none); the file layer seeks there (`FIL_Seek` 006D50C0 ->
`SetFilePointer(h, lo, &hi, FILE_BEGIN)`) and reads the 32-byte file header `{length, user length, reference bytes,
flags}` and the payload (`FIL_Read` 006D4ED0 -> `ReadFile`); the per-world bins go the same way through the BIG stream
(vtable +0x40 open / +0x4C read). Sizes come from the header inside the file, not from the file table.

The loader keeps its own copy of the file table (read from the archive named on the command line), detours
`i64_KeySearchPos` (its first 10 bytes run in a trampoline) and hooks the executable's imports `SetFilePointer`,
`ReadFile` and `CloseHandle`. A key a mod provides answers with a virtual position `{0x4D4F4400 + n, 0}` (real
positions have hi 0: the archive is under 4 GB); a seek to it puts the handle in virtual mode and the reads come
from the virtual entry - a synthesized header (raw, no shortcut), the mod's payload, the original entry's reference
table - until a seek to a real position. A virtual package is the original package with the overridden record
bodies swapped in, built the first time the engine asks for it. Handles are forgotten at `CloseHandle`, so a reused
handle value never inherits virtual mode.

Verified sites: `i64_KeySearchPos` (006E9DB0, 0x40 bytes) and the `b_Open` call in gameChecks (00409ACB, 0x14).
The imports must hold the kernel32 functions (another hook there: the loader stays out).

### 23.2 Lua

`wm_lua.cpp` embeds Lua 5.4.7 (`platform/lua/`, MIT). `LuaFrame` runs from the Magma update hook (once per frame,
main thread): the first call starts every enabled mod's `main.lua` in its own state, later calls run the frame
callbacks and timers. `wc.native` replaces an element of the native table (`PatchNativeTable`, §12.3) with one
handler for every hooked word, which finds the word from the rank at `ip`; the Lua function gets a `vm` userdata
over the VM globals (pop / push as in §12.3; `arg_*` peek at `ptrs[index - 1 - i]`). After a hook that neither
called the original nor moved the stack, the loader pops the arguments and pushes a zero result of the native's
result size (the sizes are in `wm_natives_table.cpp`, generated from the Wii executable's registration tables: 2168
natives, words identical on the PC). `wc.call` pushes the arguments, calls the handler with a node `{rank, 0}` and
pops the result.

The overlay (`wc.text`, `wc.screenshot`): `wm_gfx.cpp` patches the renderer's present routine (00486EF0: its two
`mov edx, [vtable+44h]` become calls of stubs that put `PresentHook` in edx), because the device's vtable is a
per-object copy that d3d9 replaces at every `Reset` (a vtable hook lasted one reset; the sampler and Reset vtable
hooks are put back whenever the copy changes). `PresentHook` draws the texts on the back buffer inside a scene of
its own (full viewport, no scissor, no depth, the state restored from a state block) at the presented rectangle's
origin, with an `ID3DXFont` from the game's own `d3dx9_37.dll`, then calls the vtable's Present.

### 23.3 Tests

`wmtest mods <PC executable>`: the two sites, package record replacement (a record that grows), a package without
the record, a truncated package, the synthesized header, key file names, the enabled list. In game: the log lines
`MODS: FFF003C8 served from ... (1 record)` at the title screen with a record of the title package; `LUA ...` lines
and the demo mod's texts over the picture.

## 24. Super Mario 64's Mario (`wm_sm64.cpp`, `[sm64]`)

`libsm64` (the SM64 decompilation built as a library) runs Mario's movement, collision, animation and camera-less
state machine on a set of collision surfaces and hands back his position and the triangles of his model every 30 Hz
tick. `sm64.dll` sits next to the executable, or in the mod's own folder (the downloaded mod carries one); `[sm64] rom=` names the player's own Super Mario 64 (USA) `.z64`,
because Mario's texture and animations are read out of it at startup. Nothing of the ROM is shipped.

The level provides the collision: a level mod carries `mods\<mod>\sm64\surfaces.bin` ("SM64", version, the scale
and offset that take SM64's units to the game's, Mario's start, then the surfaces), written by the level builder
from the same geometry it converts. Mario is drawn into the 3D image right after the engine's own geometry and
before the after effects (`AfxSetSceneDraw`, §19), so he is graded, fogged and depth-sorted like the level.

### 24.1 The script drives him

Nothing ticks Mario on its own. A level's script model - compiled for the engine's own script VM, the same as any
of the game's behaviours - calls the `SM64_*` natives the module registers into the engine's native table (words
`0x7D00FFFF`..`0x7D0FFFFF`, unused by the game; registered from a hook on the call the engine makes right after it
builds and sorts that table, before any script is loaded, with a per-frame fallback):

| native | result | what it does |
| --- | --- | --- |
| `SM64_Ready()` | int | libsm64 runs with the ROM and the level's surfaces are loaded |
| `SM64_MarioCreate(vector pos)` | int | Mario at a point of the game's world; -1 if there is no floor there |
| `SM64_MarioInput(float x, float y, int buttons, vector look)` | | the stick, A/B/Z (1/2/4) and the camera's facing |
| `SM64_MarioTick(float dt)` | int | the ticks owed to `dt` at 30 Hz |
| `SM64_MarioPosGet()` / `SM64_MarioPosSet(vector)` | vector | his place in the game's units |
| `SM64_MarioStateGet(int what)` | int | 0 action, 1 health, 2 animation, 3 flags, 4 particles, 5 speed x1000 |
| `SM64_MarioDamage(int damage, vector source)` | | hurt him from a direction |
| `SM64_ControlsGet()` | vector | the stick (x, y) and the buttons (z), from the keyboard and an XInput pad |
| `SM64_MouseLookGet()` | vector | the mouse's motion since the last call; asking for it captures the cursor |
| `SM64_ToggleGet()` | int | 1 on the frame **M** was pressed |
| `SM64_HudSet(int on)` | | 1 = SM64's HUD is drawn over the picture |
| `SM64_MarioPosSmooth()` | vector | where he is drawn: between the last two 30 Hz steps (the camera and the cart hang off this) |
| `SM64_MarioLaunch(vector v, int flip)` | | throw him at `v` game units a second; 1 = in his front flip, kept turning until he lands |
| `SM64_MarioFlip()` | | keep him in the flip with no motion of his own, for when the script is carrying him |
| `SM64_MarioFaceGet()` | vector | the way he faces, a unit direction in the game's plane |
| `SM64_MarioWind(vector accel)` | | a fan blows on him: an acceleration in game units a second each second, given every frame it blows; he rises in SM64's own vertical-wind action |

Collision of the game's own levels: a mod's `collision\<world>.rghc` (or `sm64\`) holds a world's ground as triangles in
the game's units, minus what the cart drives through (a material whose `crossable` word shares a bit with the cart's
`DYN_CrossableSet(2048 | 16)`); libsm64 gets the triangles within `WINDOW` of Mario, measured from a centre that
moves with him. `<world>.rghd` holds the same collision *object by object*, in each object's own space with the
object's key and collision-modifier rank; when a world has one, its baked triangles are only used for the
level's extent, and every frame the module finds each live object (`LOA_pt_KeySearchRealAddress`, what
`SCR_ObjByKeyGet` calls), reads its axes and position through the `OBJ_*Get_C` natives, checks the object's and
the modifier's apply bits, and hands libsm64 the placed triangles as a surface object, made again only when the
object has moved or the window has. So a barrier lowered by its script, a gate one script raises through
another object's design structure, a lift on its way and an object switched off are all where the game has
them. The five engine routines are checksummed at attach; if they are not where expected the bake is used as it
is. They are called under a fault guard (`DynProbe`): the axis getters validate the object's hierarchy first
(`OBJ::HieValidate`, walking the father link), and an object the engine is part-way through building or tearing
down faults in there - such an object is logged once and left alone until its key stops resolving, and axes that
come back as nonsense (NaN, absurd lengths) are not placed either. **K** (with `log=1`) writes what is around
Mario to the log: his place, the window, and every movable within twenty units with whether it was found, is
switched on, where it is and the SM64 bounds it was placed with; without Mario it says so.

Keyboard: W forward, S back, A/D or the arrows to the sides, Space jump (A), left mouse or F punch (B), Ctrl or
right mouse crouch/dive (Z); the mouse turns the camera. Pad: the left stick, A, X = B, B or a trigger = Z, the
right stick to look.

### 24.2 The M toggle

Mario is a mod of his own: one world holding the script model, its tracks, an instance of it and the camera object
it steers. Nothing in it belongs to a particular level, so any level can have him by listing that world's key beside
its own, the way the game's levels all list the shared HUD world. The level still provides the collision he walks on
(`sm64\surfaces.bin`). On **M** the script:
creates Mario where the cart stands, facing the way the camera already looks; stops the cart, its rabbids and its
stack being drawn with the cart's own `PJ_BUGROB_ControlRenderSet(0)` - the call the game's cinematics use - and
takes the stick away from them with `IO_JoystickStickEnable(0, 0, 0)`; stops the level's HUD object rendering; and
turns SM64's HUD on. While Mario plays, the hidden cart is kept on him every frame, so the game's player systems
and its fight zone travel with him: whenever SM64 reports `ACT_FLAG_ATTACKING`, the script runs the cart's own
`DL_HitCheck`, so what Mario hits is hit by the cart's attack. **M** again deletes Mario, leaves the cart where he
stood, and puts the rendering, the stick and the HUD back.

### 24.3 The HUD

SM64's own HUD pictures come out of the ROM: two MIO0 blocks hold them, the 16x16 glyphs (digits, the multiply
sign, the coin, Mario's head, the star) in segment 2 and the power meter's eight 32x32 wedges with the level's
textures. They are decoded once at startup into BGRA and drawn as point-sampled sprites over the picture through a
second overlay slot (`GfxSetHud`, drawn after the Lua overlay). What it shows is what is actually true: Mario's
head with the life count, which a death costs one of (he starts the level again after the fall), and the power
meter for the health libsm64 reports, which appears only when he has lost some.

### 24.4 Building `sm64.dll`

`platform\sm64\build.bat` builds libsm64 as a 32-bit DLL with clang and lld against a mingw32 sysroot (the
library's sources use GCC attributes MSVC does not take): each `.c` with `--target=i686-w64-mingw32`, `-O2`,
`-DSM64_LIB_EXPORT -DGBI_FLOATS -DVERSION_US -DNO_SEGMENTED_MEMORY`, linked `-fuse-ld=lld -shared -static`.
It is built with `-g` and frame pointers on purpose: a crash inside it is logged by the platform as
`sm64.dll+offset`, and `llvm-symbolizer --obj=sm64.dll --relative-address <offset>` names the function and line.

### 24.5 The object engine and the T menu

libsm64 as published carries Mario alone; everything else in SM64 - the enemies, the bosses, the coins and caps,
the platforms - is an "object" run by a behaviour script, and the library stubs that engine out.  This build puts
it back in from the SM64 decompilation:

- `platform\sm64\import-objects.py <sm64-master>` copies the decompilation's object engine and every behaviour
  into `libsm64-master\src\decomp\`, the actors (their pictures replaced by descriptors: the pixels are decoded
  out of the player's own ROM at `sm64_global_init`, from the places `assets.json` gives, so nothing of the game
  ships), the level collision plates and race paths the behaviours name, and two generated tables: the model
  bindings of every level script (`actors\model_table.c` - SM64 numbers its models per level, so 0x56 is the
  Bully in one level and King Bob-omb in another, and each object looks its models up in the table of the level
  it believes it is in) and the menu (`actors\menu_table.c`, from the curated `MENU` list in the script: name,
  category, behaviour, model or geo layout, behaviour parameters, the level the thing believes it is in - Bowser
  fights differently in each of his three arenas - and an entry spawned first at the same place, the Monty Mole's
  hole).  Run it again after changing the list; `build.bat` compiles what it writes.
- The glue is hand-written: `gbi_libsm64.h` (a display list is records of eight words, so the actors' static
  lists and the lists behaviours build at run time read the same), `gfx_adapter.c` (two sinks: Mario's buffers as
  before, and the objects' `SM64ObjectGeometryBuffers` with a picture, flags, drawing layer and object slot per
  triangle), `objects.c` (spawning with a `SpawnInfo`, a home and respawn info exactly as a level places an
  object; clearing; shifting when the host recentres its window), `libsm64_stubs.c` (the camera the host gives,
  used for billboards and for placing sounds; a conversation is over the moment it is asked for; a cutscene lasts a
  frame; the save file has every star so MIPS and Yoshi come out), `surface_load.c` (a Thwomp's or a Whomp's
  collision, placed each frame as a dynamic surface group; the host's own surface objects get a stand-in Object so
  platform displacement works for both), and the render root, which walks the whole pool with Mario among them.
- New exports: `sm64_set_camera`, `sm64_set_object_geometry`, `sm64_menu_count/name/category`,
  `sm64_object_spawn`, `sm64_objects_clear/count/shift`, `sm64_texture_count/size/rgba`, `sm64_object_info`.
  `wm_sm64.cpp` binds them as optional: an older `sm64.dll` still runs Mario, with the menu off.
- `platform\sm64\smoke_test.c` (`build_smoke_test.bat`) runs the library without the game: a flat floor, Mario,
  every menu entry in turn, a crash handler that prints `sm64.dll+offset` frames.  Run it in `build\` with the
  ROM's path after any change to the engine.

In the game, **T** opens the menu while Mario has the level (`MenuFrame` / `MenuDraw` in `wm_sm64.cpp`): the
arrows or WASD move through the list, Left and Right change the category, Enter or Space spawns the thing three
units in front of him facing him, Backspace clears everything spawned, T closes it; his controls and the mouse
look are held while it is open.  The list is drawn with the ROM's own HUD font (the US ROM has no J, Q, V, X or Z:
those, the dash and the full stop are drawn by the module).  The objects are drawn right after Mario with the
same camera, lighting and step blending, batched by drawing layer, picture and material (SM64's combiner is
reduced to: the picture times the corner colour, the picture's alpha, or the picture laid over the colour by its
alpha), the see-through layers last without writing depth.  Each object's world position is measured from the
collision window's middle like Mario's, so a recentre shifts them all (`sm64_objects_shift`).

Not there: shadows under the objects, the text boxes, the level's own platforms, and anything whose behaviour
reaches for its level's coordinates (Klepto's targets, the race paths).

The crash hunt (`debug=` in the mod's `config.ini`, handed to `sm64_set_debug` before `sm64_global_init`): 1 puts
every block the renderer and the objects allocate at the end of its own page with a page nobody may touch after
it, so a write past a block's end is an access violation at the very line that did it (the crash logger names
the frame; `llvm-symbolizer --obj=sm64.dll --relative-address` turns it into a line); 2 validates the process's
heaps at the start of a step, after the object updates and after the render, and the platform checks again after
a step and after drawing the objects, logging `!! the heap is corrupt: first seen ...` once; 4 checks before every
object's behaviour too and names the object (slow).  Without the pages every pool block carries a canary checked
when its pool is freed.  `SM64_DEBUG=7` in the environment does the same for the smoke test.  The smoke test's
sixth argument (1) runs a grab test instead of the sweep: the entry spawned facing away, Mario walks up behind
it, punches to grab it and throws it (arguments 7-9: the frame of the punch, of the throw, the total), and a
tenth adds what the game does around a step (1 the camera turning, 2 the collision window recentred, 4 the audio
ticked; add them up).

The text box.  SM64's objects ask for one - an NPC talking, a boss's opening line, a sign - and the library keeps
it as a countdown (`libsm64_stubs.c`: five seconds, one box at a time; an object that asked through
`cur_obj_update_dialog*` waits for its box as it would for the text, a cutscene's box just shows, and the answer
to a question is yes).  `sm64_dialog_state` gives the frames left and the SM64 dialog id, `sm64_dialog_close`
ends it early.  `wm_sm64.cpp` draws the box low on the picture with the HUD font (`kDialogText` is the one line it
says; `!` was added to the module's own glyphs) and closes it on Enter or Space.

Two things learned the hard way: the display list pool is emptied at the start of a tick, not of the render -
`obj_orient_graph` (anything on a slope: Bob-ombs, coins, MIPS, Yoshi) allocates its matrix during the behaviour
update, and emptying the pool at the render's start freed it before it was drawn, so those objects jumped about
every frame.  And `platform\build.bat` now links `wiimote.dll` with a PDB (`build\wiimote.pdb`), and the module
logs both DLLs' load addresses at start (`<the sm64.dll it loaded> at ..., wiimote.dll at ...`), so a crash's return addresses
can be turned into lines without a dump: `llvm-symbolizer --obj=sm64.dll --relative-address <address - base>`,
and `llvm-symbolizer --obj=wiimote.dll` the same way with the PDB beside it.

Stars and coins.  Every star is the 100-coin star's kind: `interact_star_or_key` takes it as a no-exit star
(generator patch in `import-objects.py`), and `general_star_dance_handler` in libsm64's `mario_actions_cutscene.c`
is back - the celebration star, `play_course_clear` (the level music ducks under the jingle and comes back), the
peace sign, time stopped for the others, and Mario his own again; the star's own "save?" box (dialogs 13 and 14)
is the one box never shown.  `sm64_hud_counts` gives his coin count (`numCoins`) and the stars caught this session
(`save_file_collect_star_or_key` stub); the module adds both to the counters the script sets with `SM64_CountsSet`.

The level's people fear his enemies.  `sm64_threats` lists where the ENEMIES and BOSSES of the menu stand;
`SM64_ThreatGet(k)` / `SM64_ThreatCount()` hand that to the script in the game's units (a point 5000 below the
world when there is no k-th one), and the `SM64_Mario` script keeps eight *threat objects* (keys 7E0F2100..2107,
made by `build_mario_mod.py` in Mario's world) on them every frame.  The humans' own reflex track
`PNJ_Humans_Track_Reflex` (record EF0015D5, shared by the Verminators who inherit the model) is recompiled by
`work\pcport\mods\humans_flee.py` with one change - the thing feared is the nearest of the cart and those
objects (`v_Caddie_Pos`, and `s_Flee.o_Flee_Actor` for the navmesh flee) - and shipped as the mod's record
override `records\EF0015D5.bin` (wm_mods.cpp swaps it into every package holding the key).  Everything after
that is their own AI: noticing, suspicion, running, hiding.  The recompile is exact: the unchanged track compiles
back to the archive's bytes (scrc_check's PC round trip).


## 25. Sound: the sequence-set fix, diagnostics and the Doppler speed filter (`wm_audio.cpp`, `[audio]`)

With the defaults (`keep_pcm=1`, `doppler=engine`, `doppler_log=0`) only the sequence-set fix (25.1) is patched in:
`AUDIO: PC executable verified: samples that sets play one after the other keep their decoded sound (keep_pcm=1)` and
`AUDIO: [audio] doppler=engine: the sources' velocities are left to the engine`.

### 25.1 Sequence sets keep their decoded sound (`keep_pcm=1`)

The buzz near the taxi, the helicopter and the bombs in hub 3 (and the taxi's horn that never sounded) was the
engine's, not the conversion's. A level's sounds come from its sound bins through the load stream (binary loading).
A RAM sample's header is read with its file in state 2 (`004A5C60`: the data is in the stream, the file's read handle
is never opened); its data block (`LoadSounds` -> `004A7000` -> `0049A860` -> `SLib_file_S` slot 10, `004BBD10`)
decodes it (`004A6630`: the PCM at file `+0x6C`), fills its static DirectSound buffer and frees the PCM (`004A5890`,
called at `004BC042`). After the stream is closed, the resolve pass (`004996B0` -> `SLib_set` slot 7 `004C8910` ->
`004C8700`) gives the sets of type 2, 3 and 8 - samples played one after the other through a 48 kHz ring, e.g. the
taxi `85004891`: horn `850045AE` twice, engine start `85000572`, engine loop `85000574` - their samples' PCM again.
The PCM is gone, so each sample is decoded a second time, and its reads (`004A6790` -> `BIG_S_P4::b_ReadDynAccess`
with the zeroed handle) come from byte 0 of the first shadow BF, `RGH_WC.wii.sns.bf`: its header and file table.
Measured from a dump of a user session (2026-09-27): all 24 such decodes of hub 3 equal the ADPCM decode of the
sns.bf's first bytes (`85000574`: from offset 164); samples under ~9200 PCM samples came out silent, the longer ones as
full-scale noise that the ring looped. The samples exist only inside the bins, so there is nothing to read them from.

The fix: the call at `004BC042` goes to `FreeThunk` (`push esi; push ecx; call FreeAfterStatic; ret`), which keeps
the PCM when a set of type 2, 3 or 8 in the manager's object list (`[00A6E88C] + 0x9AC`) has the sample as a child -
with the type the resolve will give the set (its template's, `+0xF4`, unless override bit 7 of `+0xF8` is set) - or
when no static buffer could be made (`SLib_file_S +0x14` 0: the ring plays it); every other sample is freed as
before. The resolve then finds the PCM decoded from the stream, as on the Wii where the sample stays in memory. A
decode that would still read through an unopened handle (state 2, no load stream, handle position 0, no prefetch) is
answered with silence (the ngcadpcm vtable slot 4) and logged once per sample. The three resolve calls (`004EBA74`
`LoadSounds`, `004EC93C` `LoadList_End`, `004ED2DA` `LoadList_Thread`) log one line per load, e.g. hub 3:
`AUDIO: sound load: 17 sets play their samples one after the other (types 2, 3, 8; 43 samples loaded): 24 samples
kept their decoded sound from the load (1023 KB, 0 of them without a static buffer), 0 decoded again by the resolve,
0 answered with silence`. The PCM stays until the engine releases the sample, as it would after the second decode.
Limits: a set that meets its samples in a later load than theirs, or plays a freed sample through the ring at play
time, gets silence instead of noise (the `answered with silence` count). `keep_pcm=0` leaves the engine alone.

### 25.2 The Doppler speed

SLib measures each sound source's velocity once per game frame, (position - previous position) / DT, in the source
update (`004ACE90`, the block `004ACF74..004AD041`). The Doppler insert (`004C03B0`, called from `004C0620`) turns it
into a pitch factor, 1 + scale x global scale (manager `+0x9E0`, 2.0) x approach speed / (300 - listener speed),
clamped to the insert's range, and multiplies the voice's pitch (`+0xA4`) by it; "user" inserts with input 2 read the
same velocity. The listener's velocity has an acceleration limit (`0049D2E0`, manager `+0x9EC`), a source's has none.
Measured in hub 3 on 2026-09-26 at 220-250 fps: the factors move by 0.1-0.7 % from one frame to the next, which is
not audible, so the filter is off by default. `doppler=wii` replaces the block with a call that measures over at least
1/`doppler_rate` s of game time (90 % of it, so every 60 Hz frame is a measurement and gives the engine's result) and
treats a measurement faster than 150 m/s as a jump (a new source's first position is the world origin, the next one
its object's; the engine turns that into one frame at the insert's maximum factor).

### 25.3 The log (`doppler_log=1`)

Once per second: `AUDIO: sources:` sources updated / started, the listener, starts per sound set; `near:` the
sources within 25 m; `doppler:` the Doppler sources with the largest frame-to-frame factor steps, and `doppler X:`
every factor over 1.25 or under 0.8 with the source's and listener's position and velocity; `adpcm codec` the
ngcadpcm decode calls of streams (`004C7F30`, vtable `008A7108` slot 4: PCM offset and length in 28-byte units per
channel, contiguity); `dsound:` the DirectSound calls SLib makes (the secondary buffers' vtable is patched once SLib
created its device `00A6E920`): SetVolume / SetFrequency / SetPan / SetCurrentPosition / Play / Stop per buffer with
the buffer's sound set, and `fast:` frequencies of 60 kHz or more with the buffer's format.

What it showed (hub 3, left side near the taxi and the gas-mask rabbid): every voice is a 10472-byte 32 kHz mono
DirectSound buffer SLib streams into; streams are decoded in whole 686-frame chunks (no misalignment); frequencies of
60-100 kHz are the sets' own pitch (the footsteps' pitch inserts have f0 1.78, +10 semitones) apart from one frame at
the start of each Doppler sound (the jump above). The Wii ADPCM samples decode like the PC release's Ogg copies of the
same sounds (same sample counts; the gas-mask rabbid's loop `850016FA`, a ~350 Hz buzzing tone by design, matches at
a median 29.6 dB). What the PC executable does not do that the Wii does: the aux sends (reverb / echo) and the filter
insert's low-pass are only applied through EAX (`DAT_00A6E924 & 4`), which current Windows does not have.
`doppler_log=2` also writes every buffer write, decode output and buffer event to `dump_dir` (`index.csv` +
`data.bin`, at most 400 MB): the dump that found 25.1. It is diagnostics only: its memory probes fault and are caught,
and each fault makes the crash log (`[general] crash_log`, section 6) write a 74 MB minidump, so use it with
`crash_log=0`.

### 25.4 Tests (offline)

`wmtest audio <PC executable>`:
* **Executable file: 24/24 PASS**: for 25.1 the static-buffer function, the PCM free, the decode-all, the file read,
  the file header load, the set prepare and resolve, the resolve loop, the `SLib_set` / `SLib_file_S` / ngcadpcm
  vtables, `ENG_Binary_Mode` and the ngcadpcm decode (CRC), the call at `004BC042` and the three resolve calls; for the
  Doppler the source update, its velocity block, the Doppler insert update and factor, the ngcadpcm decode and vtable,
  and the call at `004C06B0`.
* **Self-test: 13/13 PASS**: a source moving at 20 m/s at 60 / 500 / 1900 fps, moving every frame or advancing at 60 or
  30 Hz (frame-to-frame Doppler steps 0.27 engine / 0.006 filtered at 500 fps with 60 Hz steps); 60 fps with frames of
  1/60 and 1/62 s identical to the engine; a new source 40 m from the origin gives velocity 0; DT 0 gives the engine's
  result; the replacement code's bytes; for 25.1 a fake object list (a type 2 set's sample kept, a type 1 set's freed
  also when a resolved type 1 set has a type 2 template, a sample of a set inheriting type 8 kept, a stream child
  ignored, an empty list), the sequence-set count, the stale-read test (state 2 without the load stream and a handle
  only) and the thunk's bytes.

In game (2026-09-27, hub 3 via `/wog2A01F77F`): the 24 samples that had decoded from the sns.bf header kept their
PCM, none was decoded again; the user heard no more buzzing.
