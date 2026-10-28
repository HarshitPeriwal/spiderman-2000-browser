# Track B: static recompilation of Spider-Man (2000) PC

Goal: a high-performance local build that runs Spider-Man's original x86 code,
translated to C and compiled to WebAssembly, with a hand-written Windows layer
on browser APIs. The method follows
[cricket07-browser](https://github.com/Dhaiwat10/cricket07-browser) (MIT),
whose translator and runtime are reused with attribution. The Windows layer
is specific to Spider-Man.

**Distribution rule:** the translated C and WASM are derived from Activision's
code. They are generated on the player's machine from their own `SpideyPC.exe`
and `binkw32.dll`, and are never committed or deployed. The repository holds
only tools, the Windows layer and the launcher. Each player builds and plays
at `localhost`.

## Inputs (player's own copy)

| File | Role | SHA-256 (tested copy) |
|---|---|---|
| `SpideyPC.exe` | game code, PE32, linked 2001-08-15, ~4,400 functions | `e4e221b1…3eec86` |
| `binkw32.dll` | RAD Bink video decoder, x86 | `02d40384d5b56bc2…` |

## Windows surface to provide (from REA import evidence)

`SpideyPC.exe` (`ev_f8da1b73…`): 70 KERNEL32, 26 USER32, 1 GDI32,
3 ADVAPI32 (registry reads), `DirectDrawCreateEx`/`DirectDrawEnumerateExA`
(then COM: IDirectDraw7, IDirectDrawSurface7, IDirect3D7, IDirect3DDevice7),
`DirectInput8Create`, `DirectSoundCreate8`, 11 WINMM (multimedia timers,
mmio WAV parsing, MCI), 16 binkw32.

`binkw32.dll` (`ev_8bf438e0…`): 60 KERNEL32 (threads, events, mutexes, QPC),
27 USER32, 12 GDI32 (DIB sections, StretchBlt), 10 WINMM (`waveOut*`,
`timeGetTime`). Bink can also output through the game's DirectSound
(`BinkOpenDirectSound`).

Compared with Cricket 07 (D3D8, 31,547 functions), the code is about 7×
smaller, but the graphics API differs: DirectDraw 7 surfaces, flips, Blt and
Lock, plus Direct3D 7 immediate-mode fixed-function rendering, must be written
new on WebGPU.

## Stages

1. **Function export.** Ghidra 12 headless over `SpideyPC.exe` and
   `binkw32.dll` (`ghidra/export.sh`). Spidey-decomp's `tools/names.json` can
   name functions.
2. **Translation.** `x86toc.py` → C. Check unsupported instructions and fix
   the translator generically, not per game.
3. **Equivalence tests.** `difftest.py` compares every translated function
   against a CPU emulator on random inputs (registers and touched memory).
   This is the evidence that translation changes nothing.
4. **Windows layer, in order of need:** kernel32/user32 (reused), registry,
   WinMM timers (`timeSetEvent` drives the game clock), DirectInput8 (reused,
   including gamepads, so the game's own joystick path works), DirectSound
   (reused), DirectDraw7 + D3D7 → WebGPU (new), Bink's waveOut/GDI paths.
5. **Native first, then browser.** Get the native build to the front end and
   then gameplay, comparing frames with the original under the same inputs,
   then build for the web with Emscripten.
6. **Launcher.** Copy the player's game folder into the browser's private
   storage (OPFS) once, as Cricket 07 does.

## Status (2026-10-28)

**Working:** the translated game boots, plays its intro movies through RAD's
own translated Bink decoder, and renders in Chrome through WebGPU.

| Stage | State |
|---|---|
| Ghidra export | `SpideyPC.exe`: 3,771 functions. `binkw32.dll`: 395 functions |
| Translation | exe: 7,006 entries, 456,559 instructions. Bink: 787 entries, 68,322 instructions. Every Ghidra-found function translates; the unsupported instructions left are only in pointer/gap entries that decode data as code |
| Translator fixes (generic, not game-specific) | x87 `fnstenv/fldenv/fnsave/frstor`; `lock`-prefixed `inc/dec` (Capstone 5 mnemonic); segment-register `push/pop` in the flat model; fixpoint discovery of direct-call targets that Ghidra folded into neighbours; immediates and base-relocated dwords pointing at unowned code; `--prefix` for DLLs |
| Runtime | loads guest DLLs at their own base, binds the exe's imports to their translated exports, keeps DLL ranges out of `VirtualAlloc`, runs `DllMain` |
| Windows layer, new for Spider-Man | `host/ddraw7.c` (DirectDraw 7 + Direct3D 7 on the shared WebGPU renderer); `host/spidey.c` (GetVersion, MessageBeep, WinMM mmio + MCI, Bink's kernel32/user32/gdi32/waveOut imports); DirectInput 8 and DirectSound 8 interfaces added to the shared `dinput.c`/`dsound.c`; `WaitForMultipleObjects` |
| Headless Node run | boots through CRT, WinMain, window, DirectDraw (640×480×16 exclusive, flip chain, 16-bit z-buffer chosen by the game), DirectSound (44.1 kHz), DirectInput 8 (keyboard, mouse, gamepad with the game's own −1000…1000 range), PKR archives, texture uploads, the Bink intros (Bink's DirectSound stream and worker threads), then holds 60 fps at one draw per frame (title screen, waiting for input). No traps in 150 s |
| Browser (Chrome, WebGPU) | game files in OPFS, intro movie frames rendered. Speed not measured: the automation window used for testing limits every page to about 8 `requestAnimationFrame` ticks per second, and the renderer replays one frame per tick |

**Not verified yet:** gameplay past the title screen, 3D rendering of levels
(render states, fog, alpha, color keys), sound output in the browser, saves,
real gamepad hardware, browser frame rate in a normal window, and the
function-by-function equivalence tests (need a native C toolchain:
`sudo apt install build-essential` in WSL).

## Building (track B)

Inside WSL, with Emscripten 6.0.13 (`~/tools/emsdk`) and the game files from
your own copy:

```sh
# once per game version: Ghidra exports (Windows, see ghidra/export.sh), then translate
python3 build.py translate     "<game>/SpideyPC.exe" work/spidey/functions.json
python3 build.py translate-dll "<game>/binkw32.dll"  work/bink/functions.json bink
bash run_node.sh "<game folder>" 120     # headless test run, log in ~/spidey-build/run.log
python3 build.py web                     # ~/spidey-build/www/recomp.{js,wasm}
```

Serve `recomp/web/` with `build/` pointing at that output, using cross-origin
isolation headers (`web/serve.py`), then open it in Chrome or Edge on
`localhost`. `work/` and every build output stay local and are git-ignored.
