# Spider-Man (2000) PC in the browser

Runs the **original, unmodified** Windows PC release of Spider-Man (2000) in a
web browser. No game code is rewritten: the original `SpideyPC.exe` runs on
[Boxedwine](https://github.com/danoon2/Boxedwine) (an x86 emulator with a
WebAssembly JIT) on top of Wine 11. Wine's DirectDraw/Direct3D is translated
to WebGL by Boxedwine's patched WineD3D. Gameplay, physics, AI, timing,
levels, cutscenes and bugs are the original binary's.

**No game files are in this repository or on the deployed site.** Each player
supplies their own copy, which stays on their computer.

## Status

| Part | State |
|---|---|
| Version identification of the supplied copy | Done (below) |
| Choice of approach | Done: emulate the original exe (priority 1 in the brief) |
| Launcher (`site/`) | Loads in Chrome, detects an installed copy. The folder is read with `<input webkitdirectory>` because Chrome's File System Access API hides `.dll` files from directory listings (this is why `binkw32.dll` showed as missing) |
| Runtime build script and GitHub Pages workflow | Builds and deploys (runs 40677629485, 40680459285) |
| Runtime starts a program from the browser-storage `D:` drive | **Tested 2026-10-28:** Boxedwine's `ski32.exe` sample, written in the launcher's IndexedDB format, ran from `D:` in Chrome (single-threaded JIT, about 70–110 MIPS) |
| Synthetic key events (gamepad bridge method) reach the Windows program | **Tested:** Left arrow events turned and stopped the SkiFree skier |
| Game running under Boxedwine WebGL | **Runs (2026-10-28, Chrome, single-threaded JIT):** Activision, Neversoft and Treyarch Bink intros play, title screen shows, Enter (sent the gamepad bridge's way) is accepted, and the opening story cutscene renders in real time through the WebGL WineD3D. No D3D/DirectDraw errors logged. Sound was off for the test. Gameplay itself not yet reached |
| Gameplay equivalence tests against native | **Not started.** Needs the runtime working first |

## 1. Identified version

All facts come from the files in the install folder. REA Evidence IDs refer to
`docs/evidence/`.

- **Platform:** Windows PC, the Treyarch port published by Activision (`TREYARCH.bik`
  and `NEVERSOFT.bik` intros; source path `C:\backup\SpideyPC\SpideyPC\D3d\` in the exe).
- **Region/language:** US English (readme: "US English Language version").
- **Executable:** `SpideyPC.exe`, PE32 i386, 1,507,328 bytes,
  SHA-256 `e4e221b168ccf52130c8c56a1535289afacc1906fb55d54545e33d50e43eec86`,
  linker timestamp 2001-08-15 08:23:03 UTC. No Authenticode table, no TLS
  callbacks, no load config (`ev_7f87f336…`, `ev_3ebb60d7…`). The file's
  modification date is 2020-08-23, so it may be a later-patched copy (for
  example no-CD). Not yet compared with a known retail hash.
- **Data:** `data.pkr` (166,992,524 B) and `media.pkr` (290,302,112 B), both
  Neversoft `PKR3` archives (WAV audio and Bink video inside), plus `texture.dat`.
- **Imports** (`ev_f8da1b73…`): `DDRAW!DirectDrawCreateEx/DirectDrawEnumerateExA`
  (DirectDraw 7 / Direct3D 7 immediate mode), `DINPUT8!DirectInput8Create`,
  `DSOUND` ordinal 11 (`DirectSoundCreate8`), RAD `binkw32.dll` (16 functions),
  WinMM multimedia timers (`timeBeginPeriod`, `timeSetEvent`, `timeKillEvent`)
  and `mciSendCommandA`, plus ADVAPI32 registry reads and `CreateThread`.
- **Third-party additions in this install, not part of the game:** dgVoodoo 2
  wrappers `DDraw.dll`, `D3D8.dll`, `D3DImm.dll`, `dgVoodoo.conf`, `dgVoodooCpl.exe` (2022).
- **No CD needed when `Media.pkr` is in the working directory.** The CD scan
  (reconstructed in spidey-decomp `PCMovie.cpp`) looks for `<drive>\Setup\SpideyPC.exe`
  on CD-ROM drives, then opens `Media.pkr` from the current directory. Saves go
  to `save\` in the game folder.

## 2. Approaches evaluated

- **spidey-decomp** ([krystalgamer/spidey-decomp](https://github.com/krystalgamer/spidey-decomp),
  checked at `f8a0392`): an incremental decompilation that builds as a
  `binkw32.dll` proxy, hooking functions inside the running original exe.
  About 2,400 functions are tagged `@Ok`/`@Matching`, out of about 4,400 in the
  original. Roughly 410 are still `TODO` stubs, including about 70 in
  `spidey.cpp` (Spider-Man's own controller) and `Web_CollideWithSuper`. It
  can't run on its own and has no license file. **Not usable for a source port
  yet.** It's still the best reference for how the exe works.
- **Boxedwine 26R1 release:** its web build explicitly does not support
  Direct3D. Not usable.
- **Boxedwine master (26R2 development, `8ab37e7`):** adds a WASM JIT and
  Direct3D 9-and-lower → WebGL through a patched WineD3D (Wine 11 web
  filesystem v23). **Selected.** It runs the original binary, so nothing is
  reimplemented.

## 3. How it works

1. `site/index.html` checks the browser, reads the player's install folder
   (read-only), identifies the build from its PE header and checks the PKR3
   archives.
2. It installs the game once into the browser's IndexedDB: the database that
   Boxedwine mounts as `D:` (`/d_drive/key/spiderman2000`, Emscripten IDBFS
   format), leaving out DirectX wrapper DLLs and installer files. Wine loads
   DLLs from the game folder before system32, so dgVoodoo's `DDraw.dll` would
   replace the WebGL one. Nothing is written to the player's disk. An earlier
   version copied to a second folder through the File System Access API, and
   Chrome's Safe Browsing checks on `.dll` writes could drop `binkw32.dll`.
3. It opens Boxedwine with
   `root=TinyCore15Wine11.0-web&storage=indexeddb&storageKey=spiderman2000&p=SpideyPC.exe&w=d:/&sound=true`.
   Saves land in the game's `save\` folder inside that storage. Later visits
   skip steps 1 and 2.

The build adds `IDBFS` back to the JIT targets' exported runtime methods.
Without it, the pinned Boxedwine commit fails at startup with "IDBFS is not
defined" (found in testing on 2026-10-28).

## 4. Building and running locally

Requires Linux or WSL with git, python3, curl and GNU make. Emscripten 6.0.13
is installed automatically if `emcc` isn't on PATH.

```sh
bash scripts/build-runtime.sh          # builds dist/ (both runtimes, about 100 MB filesystem)
node scripts/serve.mjs 8080 dist       # http://localhost:8080/
```

Open it in Chrome or Edge 137+ (folder access is Chromium-only).

## 5. Deploying to GitHub Pages

1. Push this folder to a GitHub repository on branch `main`.
2. Repository → Settings → Pages → Source: **GitHub Actions**.
3. The workflow `.github/workflows/pages.yml` builds Boxedwine, downloads and
   verifies the Wine filesystem (SHA-256 pinned), and deploys `dist/`. The
   build script refuses to emit a site containing game files.

The Wine filesystem is 99.5 MB, under GitHub's 100 MiB per-file limit. Whether
the Pages artifact upload accepts it hasn't been verified yet.

## 6. Supplying game files

Install the game from your CD as usual, or use any complete install folder.
In the launcher, choose that folder, then an empty folder for the browser copy.
Required: `SpideyPC.exe`, `binkw32.dll`, `data.pkr`, `media.pkr`, `texture.dat`.

## 7. Original systems preserved

All of them, by construction: the game's own exe performs simulation, physics,
web-swinging, combat, AI, camera, triggers, saves, menus, cutscenes (original
Bink decoder) and audio. The port only supplies Windows APIs (Wine) and a CPU
(Boxedwine). This holds only if the emulation is correct. See section 9.

## 8. Test results

None yet. Nothing has been run in a browser, and no comparison against native
play exists. Planned tests: identical input sequences recorded natively and in
the browser (Boxedwine has a recorder/automation build), comparing frame
hashes and positions for walking, jumping, web-swinging, wall crawling,
combat, cutscenes and save/load.

## 9. Known risks and limitations

- **Graphics translation:** Boxedwine's D3D→WebGL path is unreleased. D3D7
  immediate-mode games may hit untested paths (fog, W-buffer, palettized
  textures, `texture.dat` formats).
- **Speed:** emulated x86 in WASM is several times slower than native. The
  game is timed by WinMM multimedia timers (`timeSetEvent`). If the emulator
  can't keep up, the game slows down rather than changing its logic, but frame
  pacing may differ. The multi-threaded runtime is likely to matter here.
- **Memory:** the host-folder mount loads the whole game folder (about 457 MB)
  into WASM memory, out of a 3 GB maximum.
- **Browser support:** Chromium only (File System Access API).
- **Controllers:** Boxedwine doesn't pass browser gamepads to Wine, so the
  game's own DirectInput joystick path is unused. `site/gamepad.js` presses the
  game's default keyboard keys instead (table below). This matches the
  original's own controller handling, which reduces the stick to digital
  directions at 250/1000 (`PCINPUT_GetControllerDirections` in spidey-decomp).
  Differences from native joystick play: the in-game "change button
  configuration" screen won't see the pad, and if you change the *keyboard*
  configuration in the game you must update the bridge mapping
  (`localStorage["spidey.padmap"]`) to match. Untested with real hardware.

## Controls

Keyboard: the game's own defaults (arrows, numpad 2/4/6/8/7/9/3, Enter, Tab).
Change them in the game's options as on PC.

Controller (any pad with the browser's standard mapping), by position:

| Pad (Xbox / PlayStation) | Sends | Game default action slot |
|---|---|---|
| Left stick, d-pad | Arrow keys | Directions |
| A / Cross (bottom) | Numpad 2 | action `0x10` |
| B / Circle (right) | Numpad 6 | action `0x20` |
| X / Square (left) | Numpad 4 | action `0x40` |
| Y / Triangle (top) | Numpad 8 | action `0x80` |
| LB / L1 | Numpad 7 | action `0x100` |
| RB / R1 | Numpad 9 | action `0x400` |
| RT / R2 | Numpad 3 | action `0x200` |
| Start | Enter | action `0x1000` |
| Back / Select | Tab | action `0x2000` |

Action slots come from the exe's default mapping table. Their names (jump,
punch, kick, web and so on) are shown in the game's keyboard configuration
screen. Mapping slot numbers to names from the code is still an open item.
