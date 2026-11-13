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

## Milestone 2: the Vulkan renderer

runtime/portable/gfx9_vulkan.c is the Vulkan 1.1 back end behind gfx9.h, with gfx9_metal.m's
behaviour; its shaders are sm_glsl.c's GLSL, compiled to SPIR-V with glslang's C interface when a draw
first needs them. On the Mac it runs on MoltenVK: `XIV_GFX=vulkan .venv/bin/python tools/build_xiv.py
xivboot --exe .../ffxivgame.exe` builds it in place of Metal, linked with Homebrew's vulkan-loader,
molten-vk and glslang (`brew install molten-vk vulkan-loader vulkan-headers glslang`).

What it does the way Metal does: the formats (16-bit ones converted to BGRA8 on upload), the lazy render
passes with whole-target clears as load operations, the pipeline key, shader variants per draw
(integer attributes, the varyings a pixel shader reads, alpha test, depth samplers), background
compiles for the 3D scene's draws, the half-pixel offset, clockwise front faces, the default (0, 0, 0, 1)
attribute, sRGB reads through a second view, and the present scaled into the window with its aspect
kept. Like Metal it ignores D3DRS_SRGBWRITEENABLE (the Metal back end keys its pipelines by it but
draws to the plain target).

Settings: XIV_VK_VALIDATE=1 turns the validation layer on (with Homebrew's vulkan-validationlayers,
VK_LAYER_PATH set to its share/vulkan/explicit_layer.d and DYLD_LIBRARY_PATH=/opt/homebrew/lib, since
its manifest names the library without a folder), XIV_VK_NOBC=1 decodes DXT and ATI textures on upload as on a
Mali, XIV_VK_PROF=1 logs where the render thread waits and the GPU time of a frame (2: by render pass
size), XIV_VK_DUMP=<dir> writes every GLSL source, XIV_VK_CACHE=<dir> moves the SPIR-V and pipeline
cache (empty: none), and XIV_GFXTRACE=<dir> with a `req` file there dumps the render targets a frame
samples, as the Metal back end does.

On the Mac (M1 Pro, MoltenVK 1.4.2, 2026-11-12) it draws the title, the lobby, character select and the
world. The title and lobby match the Metal back end's frames to within a level of rounding, character
select differs only where things move, and the world's scene, captured with XIV_GFXTRACE in both back
ends minutes apart, differs only by the light of the hour and what moves. (On prod that day both back
ends stayed on "Now Loading..." over the drawn world, the zone's data received and nothing more coming,
so the scene was compared from the render target the game draws it into.) It holds the same 60 frames a
second as Metal in the Gridania forest, the GPU taking about 13.5 ms a frame. Getting there took three
things: MoltenVK's own submit thread and no Metal argument buffers (environment settings the back end
sets on Apple, MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS and MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS), render
pass dependencies that cover fragment work only, and sm_glsl.c zeroing its register arrays with
constructors instead of loops, so spirv-opt can keep them in registers. With XIV_VK_NOBC=1 the textures
decoded on upload look the same as the BC ones.

Not there yet:

- The Android build. tools/build_xiv_android.py still links gfx9_null.c. It needs glslang for the NDK
  (glslang's CMake builds with the NDK's toolchain, as android_deps.py builds SDL; or shaderc from the
  NDK's sources/third_party), libvulkan from the NDK, and gfx9_vulkan.c and sm_glsl.c in place of
  gfx9_null.c. SDL for Android is built with its Vulkan support, so the surface comes from
  SDL_Vulkan_CreateSurface there.
- What the phone needs from its driver: Vulkan 1.1, VK_KHR_swapchain, a graphics queue that presents,
  and B8G8R8A8_UNORM as a vertex format (D3DCOLOR; the log says when it is missing). Used where present:
  textureCompressionBC (else the decode), shaderClipDistance (else SM_NO_CLIP_DISTANCE), samplerAnisotropy,
  independentBlend, fillModeNonSolid, largePoints, VK_KHR_shader_float_controls with
  shaderSignedZeroInfNanPreserveFloat32 (NaN and infinities kept, as Metal's safe math),
  VK_KHR_sampler_mirror_clamp_to_edge (D3DTADDRESS_MIRRORONCE, else clamp) and VK_KHR_image_format_list.
  D32_SFLOAT_S8_UINT falls back to D24_UNORM_S8_UINT, D32_SFLOAT to X8_D24.
- The swapchain on a phone in landscape asks for the identity transform and lets the compositor rotate;
  rendering pre-rotated would save the compositor a pass.
- Decoded DXT textures take four times the memory of BC ones (one in BC1's case eight).
- Stream frequencies above one instance (D3DSTREAMSOURCE_INSTANCEDATA | n) want
  VK_EXT_vertex_attribute_divisor; the game has not been seen to use them.
- No run with the validation layer yet (the first try lacked DYLD_LIBRARY_PATH and the layer did not
  load). It is the next thing to do, and the best check of what a Mali driver will hold the back end to.
- Metal always restarts strips at index 0xFFFF (0xFFFFFFFF for 32-bit indices), which D3D9 does not;
  MoltenVK says so once per pipeline. Harmless unless the game puts that index in a strip.
- A depth buffer sampled while it is the bound depth target, or a texture sampled while it is a bound
  render target, is not given the layouts Vulkan asks for (MoltenVK does not care; the validation layer
  would).
