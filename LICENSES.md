# Licenses and attribution

This repository contains only launcher code and build scripts. It contains no
Spider-Man game code or data. The game files come from the player's own copy
at runtime and never leave their computer.

| Component | License | Source |
|---|---|---|
| Launcher and scripts in this repository | MIT | this repository |
| Boxedwine (x86 emulation, WASM JIT, browser shell) | GPL-2.0-or-later | https://github.com/danoon2/Boxedwine, commit `8ab37e7b79737467674b40fe0675396b3d90a469` |
| Wine 11.0 filesystem `TinyCore15Wine11.0-web.zip` v23, including the WebGL WineD3D patch series | LGPL-2.1-or-later (Wine); TinyCore Linux components under their own licenses | https://boxedwine.org/v2/23/TinyCore15Wine11.0-web.zip, patches in Boxedwine `tools/d3dToWebGL/` |
| coi-serviceworker | MIT | https://github.com/gzuidhof/coi-serviceworker |
| Emscripten runtime | MIT / University of Illinois NCSA | https://github.com/emscripten-core/emscripten |

The deployed site serves Boxedwine and the Wine filesystem unmodified. Their
complete corresponding source is available at the links above, at the pinned
commit and filesystem version.

Spider-Man is a trademark of Marvel. The game was developed by Neversoft,
ported to PC by Treyarch and published by Activision. This project is not
affiliated with any of them.
