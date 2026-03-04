# Inventory 3D Fix

An F4SE VR plugin that fixes the vanilla Fallout 4 VR bug where 3D item previews in the Pipboy inventory are invisible.

## The Bug

In Fallout 4 VR, most items in the Pipboy inventory appear invisible in the 3D preview window. Only certain transparent/effect items (like Nuka Cherry bottles) display correctly. This has been a known unfixed bug since FO4VR's release.

## The Cause

Fallout 4 VR uses a simplified forward-only render path for Pipboy 3D items that only renders BSEffectShader materials (glass, glow, particles). BSLightingShader materials (weapons, armor, food, junk — the vast majority of items) are completely skipped because the shader accumulator flags are set to forward-only mode.

## The Fix

This plugin injects BSLightingShader rendering into the forward render path:

1. **Shader group registration** — Overrides the accumulator flags (`f669` and `f688`) during scene graph traversal so BSLightingShader geometry registers into the correct render groups
2. **Forward render injection** — After the normal forward render pass, additionally renders BSLightingShader groups 0, 1, and 2
3. **Alpha fixup** — Applies a content-aware D3D11 alpha correction on the offscreen render target before compositing, since BSLightingShader outputs alpha=0 which would make items invisible in the alpha-blended composite

## Requirements

- Fallout 4 VR (version 1.2.72)
- [F4SE VR](https://f4se.silverlock.org/)
- [VR Address Library](https://www.nexusmods.com/fallout4/mods/64879)

## Installation

Copy `Inventory3DFix.dll` to `Data\F4SE\Plugins\`, or install via Mod Organizer 2 using the `Data` folder structure.

The fix is active by default. No configuration needed.

## Building from Source

### Requirements
- Visual Studio 2022
- CMake 3.23+
- vcpkg with `VCPKG_ROOT` environment variable set
- [F4VRCommonFramework](https://github.com/alandtse/F4VRCommonFramework)

### Build

```powershell
cmake -B build -S .
cmake --build build --config Release
```

Output: `build/Release/Inventory3DFix.dll`

The CMakeLists.txt looks for F4VRCommonFramework at a relative path. You may need to adjust the `F4VR_COMMON_FRAMEWORK_PATH` variable to match your setup.

## License

MIT
