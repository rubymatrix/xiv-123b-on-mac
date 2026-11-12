# Android port

FINAL FANTASY XIV 1.23b on Android phones and tablets, built on the player's computer from their own
game, as on iOS (docs/ios-port.md). Started 2026-11-12 with a Pixel 6a.

## The test device

- Pixel 6a: Google Tensor G1 (gs101), two Cortex-X1 cores at 2.8 GHz, two A76, four A55; Mali-G78
  GPU with Vulkan; 5.7 GB of RAM; Android 17; kernel 6.1.
- 48-bit address space for apps (the stack sits near 0xf635_0000_0000), so the Mac's fixed guest
  window at 0x20000000000 fits. Phones with a 39-bit space (512 GB) would need another base or none
  (XIV_ANDROID_GUEST_BASE).
- adb can write into the app's own folder under /sdcard/Android/data on Android 17.
- Mali GPUs do not sample BC (DXT) textures: the renderer has to decode them when they are uploaded,
  which costs memory (xi-on-anything does the same).

## What it builds on

- xi-on-anything's Android build (rubymatrix): an SDL3 host, a Vulkan renderer for its D3D8 interface
  (gfx.h), HLSL compiled to SPIR-V with glslang, and APK packaging without Gradle. This port reuses
  the toolchain and packaging (tools/android_toolchain.py, android_deps.py) and writes the rest for
  FFXIV's D3D9 interface (gfx9.h).
- The Android SDK, NDK 27.2, its CMake and Ninja, and a Java (Android Studio's) on the build machine.

## Milestones

1. The game on Android without graphics. tools/build_xiv_android.py builds the translation, the
   runtime and tests/xivboot_android.c into libmain.so with the NDK (ARM64, API 29, -ffixed-x18,
   16 KB pages), packages an APK with SDL's Java, installs it and pushes the game with adb. The
   renderer is runtime/portable/gfx9_null.c, which draws nothing. This proves the CPU side: the guest
   window, files, sign-in, the lobby and the world, and the speed.
2. The Vulkan renderer: gfx9_vulkan behind gfx9.h, and a translator from D3D9 shader models 2 and 3
   to GLSL, compiled to SPIR-V with glslang. Developed on the Mac through MoltenVK first, where each
   try takes seconds, then on the phone.
3. Android polish: the touch controls (tests/ios_touch.c is SDL and mostly carries over), a sign-in
   screen, the frame cap from heat and battery, and the keyboard for chat.
4. A cross-platform installer (Tauri) for Windows, Linux and macOS that fetches what it needs (Python,
   the NDK, the SDK, a Java), walks the player through USB debugging, builds, installs and pushes the
   game. FINAL FANTASY XI comes in through the same tool contract as on the Mac
   (docs/adding-a-game.md).

## Where it stands

Milestone 1 works on the Pixel 6a: the game signs in, goes through the lobby and character select,
and enters the world, with sound, all without drawing anything. The title and the lobby run at the
60 frames a second the null renderer paces to; in the world the game makes about 19 a second, so
the CPU side is the limit there and wants measuring before the renderer adds its own cost.

Things found on the way:

- SDL sets the activity's orientation when it makes the window and overrides the manifest. Without
  SDL_HINT_ORIENTATIONS it chose portrait, and the game's clicks were mapped through a 1080x2400
  window. xivboot_android.c sets the hint to landscape.
- Starting SDL text input brings up the screen keyboard, so on Android, as on iOS, it is on only
  while one of the game's text fields has the keyboard (user32_text_field).
- adb push of the game's 215,000 files stalls; push-game streams a tar into the app's folder.
- A sign-in session is good for one entry into the world. A scripted run needs a new one
  (tools/xiv_login.py) each time, or it stops at character select.
- XIV_DISCORD=log writes the game's state ([state] lines) to the log, and XIV_NETLOG=1 traces each
  connection, which together show how far a run got without a picture.
