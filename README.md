# AlphaCWController

Independent Xbox/XInput controller mapper DLL for Cube World Alpha and Windows Steam Deck.

Made by DieuDeGlace with AI assistance from OpenAI ChatGPT/Codex. MIT licensed. No game executable, game assets, saves, or runtime journals are included.

## Build

Install Visual Studio 2022 Build Tools with Desktop development with C++ and
the Windows SDK. Open an **x86 Native Tools Command Prompt for VS 2022**.
These Alpha DLLs are 32-bit; an x64 build will not load into Cube.exe.

```bat
git clone https://github.com/DieuDeGlace/AlphaCWController.git
cd AlphaCWController
build.cmd
```

The DLL appears under `build/Mods/`. No toolchain or game files are bundled.
Builds use C++17, /O2 /MT /EHsc, Unicode, and the Windows SDK. clang-cl in an
x86 MSVC environment may be substituted for cl. Test programs are built into
`build/tests/`; run them with Alpha and other exclusive-fullscreen games closed.
GPU tests require a working Windows D3D9 driver and do not measure in-game FPS.

## Install

Close Alpha, extract the release ZIP into the Alpha game folder, and launch
through the Alpha mod loader. The ZIP's `Mods` folder must merge with the
game's existing `Mods` folder. Keep the included configuration/support folders.
The repository name is the project name; the established DLL filename stays
unchanged for compatibility. Remove the DLL with the game closed to uninstall.

## Controller usage



| Control | Action |
|---|---|
| Left stick | WASD movement |
| Right stick | Camera / mouse |
| RT / LT | Attack / special attack |
| A / B | Jump / dodge |
| X / Y | Interact / ultimate |
| LB / RB | Class skill / quick item |
| L3 | Walk modifier |
| R3 | Toggle menu mouse mode |
| D-pad up / down | Lamp / crafting |
| D-pad left / right | Inventory / map |
| View / Menu | Tab selection / Escape |

Press R3 when using inventory or menus. In menu mode A clicks left and B clicks right; movement is disabled. Press R3 again to return to gameplay. Menu mode is manual. Movement is digital WASD, rather than native analog movement.

Edit AlphaController.ini and restart the mapper to change bindings or sensitivity. Supported binding values: A-Z, 0-9, SPACE, SHIFT, CTRL, ALT, TAB, ESC, ENTER, LEFT_MOUSE, RIGHT_MOUSE, MIDDLE_MOUSE, NONE. If your Alpha bindings differ, match the INI to them.


Use Xbox-compatible Windows XInput. Steam Deck running Windows needs a driver exposing its controls as XInput. Inputs release on focus loss/disconnect. Do not run the standalone mapper alongside the DLL. Run `build\tests\AlphaController.exe --self-test`; controller input still needs verification with your own device.

## Source formatting

The project uses the checked-in `.clang-format` style (four-space indentation and a 100-column target). From the repository root, run `clang-format -i src/*.cpp src/*.h` on the source files that exist. Keep third-party SMAA files and embedded shader strings unchanged.

The cleanup was rebuilt with MSVC for x86 and checked against the previous revision for identical executable tokens and shader strings. Runtime settings and installed DLLs are unchanged.
