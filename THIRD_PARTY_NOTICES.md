# Third-party notices

## Nofrendo

`lib/nofrendo/` contains a modified Nofrendo NES emulator core derived from Espressif's `esp32-nesemu` proof of concept:

- Upstream: <https://github.com/espressif/esp32-nesemu>
- Referenced upstream revision: `693e378643dd4810665e52c0ec02bd9f64559c50`
- Original Nofrendo copyright: Copyright (c) 1998–2000 Matthew Conte and contributors listed in `lib/nofrendo/src/AUTHORS`
- License: GNU General Public License version 2

This project modifies the core for ESP32-S3, PlatformIO/Arduino integration, PSRAM-backed ROM loading, frame submission, audio, input state replacement, and clean return to the game menu. The repository history and source files provide the corresponding source for these modifications.

The GPL v2 text is included in the repository root as `LICENSE`. Existing copyright and license headers in the imported source files are retained.

## Game ROMs

No commercial game ROM is distributed by this repository. `assets/test.nes` is generated from the project-owned source in `tools/make_test_rom.py` and is provided under the repository license.
