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

## Status

Not started. Needs Ghidra 12, Python with `capstone` and `pefile`, and
Emscripten (already set up in WSL), all run locally on the player's machine.
