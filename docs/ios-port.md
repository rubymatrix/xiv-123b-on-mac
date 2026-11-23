# iPad and iPhone port: starting notes

These notes were written on 2026-11-11 on a Windows machine after reading this repo and
xi-on-anything. Nothing here has been compiled or run. Each item says whether it was read in the
source or is a guess.

The goal is FINAL FANTASY XIV 1.23b on an iPad Air with an M-series chip (8 GB of RAM), then on an
iPhone. The Mac is a MacBook Pro with Xcode and a paid Apple Developer account. The server is
Project Meteor.

## Rules for this work

- Nothing is posted to rubymatrix's repos (issues, pull requests, comments) without the operator's
  explicit OK for that post. Fetching is fine.
- No Square Enix code or data is committed. `generated/` and the game folder stay out of git, as
  upstream already arranges.
- Commits carry no Co-Authored-By or "Generated with" lines. Commit and push only when asked.
- The game's behaviour is not patched to make the port easier. Changes go in the runtime and host.
- Docs use plain sentences, no em dashes and no bold labels.

## Step 0: the Mac build has to work first

The operator has the stock Mac build installed but it does not run properly yet. The cause is not
known here. Fix that before anything else, because the iOS build uses the same translated game
code, the same patched 1.23b game folder and the same sign-in.

Things to establish on the Mac:

- which step fails: install, patching to 1.23b, the build, sign-in, lobby, or entering the world
- that `tools/xiv_login.py --meteor --server <site root> --lobby <host>` reaches the character list
  against the operator's Meteor server
- how much memory the game uses in a busy zone (Activity Monitor, or the runtime's own log). This
  number decides how the iPad build has to treat memory.

## Step 0 findings (on the Mac, 2026-11-11)

- The stock app failed at patching. The disc's ffxivboot asks ver01.ffxiv.com:54996 for its
  version check, the runtime sends that to the server, and prod has nothing on 54996. The request
  times out, the boot throws a C++ exception the runtime cannot dispatch yet ("guest SEH dispatch
  is not supported yet") and the process ends. The install was still at the disc's 2010.07.10.
- The disc's ffxivboot downloads patches only by BitTorrent, and only torrents whose signature
  checks out against the certificate in its resource 111. Prod's patch server (Kodama, port 17742)
  serves plain HTTP downloads with no signature, so the stock patcher cannot use it. Support for
  the official patcher on prod (Project-Meteor-Web) is being added separately.
- Kodama's boot check is GET /http/<channel>/<boot version>. Its game check is
  POST /http/<channel>/<game version>/<any id> and always lists all 51 game patches with their
  SHA-1, so filter by version. Both need User-Agent: FFXIV PATCH CLIENT. The game check hashes
  every patch on the server, so call it once and keep the reply.
- `tools/xiv_patch.py` applies the downloaded patches without the game's updater. In a 1.x patch
  every ETRY block that carries data holds the whole new file, so each file is checked against the
  entry's SHA-1 before and after it is written. Applied to the disc install it gives boot
  2010.09.18.0000 and game 2012.09.19.0001, and the four programs match the retail 1.23b SHA-256
  values in `tools/xiv_e2e.sh`.
- Prod's login is not Meteor's PHP page. `tools/xiv_login.py --kodama --server
  http://<host>:17741` posts to /oauth/ffxiv/login/login.send and reads the session id.
- The first config.sys asked for a 2112x1188 window, which is larger than a 14 inch MacBook Pro's
  screen (1512x982 points), and the window cannot be resized. The first window is now the largest
  16:9 size within nine tenths of the main display's usable area. Existing installs keep their
  config.sys; `tools/xiv_config.py --resolution WxH` changes it while the game is closed. Wanted
  later: a settings window for players on the Mac (window size, full screen and the rest of
  config.sys), since `xiv_config.py` is command line only.
- The Mac's internal disk is nearly full, so the patches, the patched game folder and this
  checkout's build/ and generated/ (symlinks) live on an external drive.

## First device run (iPhone 17 Pro, iOS 27.3, 2026-11-11)

- Built with `XIV_IOS_GUEST_BASE=none` and signed with the team's wildcard development profile, so
  without the extended addressing and increased memory entitlements. iOS placed the 4 GB guest
  window at 0x7000000000. The fixed 2 TB base of the Mac build would not have worked; a fixed base
  near 0x7000000000 is the next thing to try for speed.
- Metal on the A19 Pro: title, character list and the world at a steady 60 fps at 1560x720, 40 to
  50 fps while a zone streams in. The game reached the world and its opening cutscene against prod.
- Memory on the Mac: 656 MB at the character list, 1,294 MB at most in the world (not yet measured
  in a busy area, nor on the phone).
- Three iOS bugs found and fixed: file names (the phone's storage is case-sensitive, so the
  runtime now matches names ignoring case), touch (SDL's global mouse position on iOS is the
  window's), and the interface size (the game draws it at a fixed pixel size, so it now renders at
  720 lines and is scaled up to the screen).
- The patched game folder goes onto the phone with `xcrun devicectl device copy to` into the app's
  data container (12 GB, about 8 minutes over USB). A 64 GB iPad without room for it needs a game
  folder on an external drive, chosen with a folder picker. Not written yet.

## What xi-on-anything already has for iOS (read in its source)

- `tools/build_ios.py` builds an .app with Xcode's clang for `arm64-apple-ios17.0`, for the
  simulator and for devices. It signs, installs with `devicectl`, and runs on the simulator with
  `simctl`. It is the model for the script this repo needs.
- `third_party/sdl3-ios` is SDL 3.4.16's UIKit tree with a `manifest.json`. `tools/vendor.py
  sdl3-ios` vendors it.
- `host/ios_main.c` is the entry point. `host/ios_touch.c` draws a touch gamepad through SDL's
  virtual gamepad. `host/display_fit.c` sizes the game for the screen and safe area.
- Game files go in the app's Documents folder, shared through Finder and the Files app
  (`UIFileSharingEnabled`). Arguments come from `Documents/run.args`. The log goes to
  `Documents/host64.log`.
- The build asks for two entitlements: `com.apple.developer.kernel.extended-virtual-addressing`
  and `com.apple.developer.kernel.increased-memory-limit`. It drops any the profile does not grant.
- On iOS the guest window is 2 GB instead of 4 GB, so that it might work without the extended
  addressing entitlement. Its notes say this was not tried on a device.
- Its Metal back end decodes DXT textures to BGRA8 when `supportsBCTextureCompression` is false,
  and it skips `displaySyncEnabled`, which iOS does not have.
- `docs/ios.md` and `docs/platforms/ios.md` there describe all of this.

## How this repo differs from xi-on-anything (read in the source)

- There is no `host/` folder. The game's entry is `tests/xivboot.c`. The build is
  `tools/build_xiv.py`, which calls plain `clang` with `-mcpu=apple-m1` and ThinLTO.
- The guest window is fixed: `-DRT_GUEST_FIXED=0x20000000000ull`, 4 GB plus a guard page, placed by
  `plat_reserve_at` in `runtime/portable/plat_posix.c` with an address hint and no `MAP_FIXED`.
  `runtime/portable/gwin.c` fails start-up if the window lands anywhere else.
- Graphics are Direct3D 9 on Metal only: `runtime/portable/d3d9.c`, `d3dx9.c`, `sm_msl.c` (shader
  bytecode to Metal Shading Language) and `gfx9_metal.m`. MetalFX is linked weakly for upscaling.
- SDL3 is vendored as the Cocoa tree only (`third_party/sdl3`).
- AppKit is used in `ui_appkit.m`, `webbrowser.m` and `app_icon.m`. The first two are only linked
  into the launcher programs (`EXE_HOST` in `build_xiv.py`), which the iOS build does not need.
  `app_icon.m` is in `PORTABLE` and is called from `tests/xivboot.c`, so it needs an iOS stub.
- `tests/xivboot.c` includes `libproc.h` and `pthread/qos.h` under `__APPLE__`. `libproc.h` is not
  in the iOS SDK.
- Sign-in is done outside the game by `tools/xiv_login.py`: it posts the account to the server's
  web pages, gets a 56 hex digit session id, and starts `xivboot` with launch arguments built by
  `tools/xiv_args.py` (Blowfish, keyed to the tick count the host starts the game clock at). An
  iPad has no Python, so this has to exist in C inside the app, or the app needs its own sign-in
  screen as xi-on-anything's `host/signin.c` has. It now has one (`tests/ios_signin.m`). It posts
  username and password to the 1.23b login page as ffxivlogin.exe uses it,
  `http://<server>/account/content/ffxivlogin`, the page Meteor-PatchLogin-Server serves, and takes
  the session id from the `<x-sqexauth sid="...">` tag in the answer. An answer without the tag is a
  refusal, and the page's error message is shown. The player enters the server address (host, or
  host:port); the lobby is the same host unless another one is set. The page is plain http, so
  `build_xiv_ios.py --login-host <host>` lets App Transport Security allow plain http to that host
  only, besides the local network.
- `runtime/portable/xiv_direct_login.c` is a developer login that skips the title and lobby
  (`XIV_DIRECT_LOGIN=<character id>`). It may be a quick way to get the first frame in the world
  on a device before the real sign-in is ported. Meteor's support for that ticket login is not
  checked.
- The first-run `config.sys` written by `tests/xivboot.c` asks for a 2112x1188 window. iOS needs a
  size taken from the screen.

## Risks, most serious first

1. Memory. This is a guess from the PC requirements (1.5 GB of RAM minimum plus a 512 MB video
   card), not a measurement. iOS ends an app that passes its memory limit. On an 8 GB iPad the
   limit is higher with the increased memory limit entitlement, which a paid account can enable.
   Measure the Mac build first. Texture memory matters too: if the iPad's GPU cannot sample BC
   textures, decoding DXT to BGRA8 multiplies texture memory by 4 to 8. Apple silicon iPads report
   BC support on recent iPadOS, but check `supportsBCTextureCompression` on the device instead of
   trusting this.
2. The guest window address. `0x20000000000` is 2 TB up. Whether an iOS process can map there,
   with or without extended virtual addressing, is not known. If it cannot, pick a lower fixed
   address or build without `RT_GUEST_FIXED` (slower, see the comment in `runtime/guest.h`).
   Whether the game needs more than 2 GB of address space has not been checked. Read the
   large-address-aware bit in `ffxivgame.exe`'s PE header and watch the "top" of guest memory on
   the Mac.
3. Shaders. `sm_msl.c` output already compiles on the Mac's Metal. iOS Metal on Apple GPUs should
   accept the same source, but features such as texture formats, depth formats and sampler modes
   differ between the Mac and iOS feature sets. Expect a round of fixes in `gfx9_metal.m`.
4. Speed. An M-series iPad has the Mac's cores, so the translation should run about as it does on
   a MacBook Air. An iPhone is slower and gets hot. The guest-wide lock (one guest thread at a
   time, `runtime/guest.h`) is the same on both.
5. Controls and text. The game expects a gamepad or keyboard and mouse. Start with a paired
   controller. Touch controls and the on-screen keyboard for chat come from xi-on-anything later.

## Suggested order

1. Get the stock Mac build working against the operator's server, and record memory use.
2. Vendor SDL3's UIKit tree (`third_party/sdl3-ios`, copied from xi-on-anything with its manifest)
   and check `tools/thirdparty.py` can build it for an iOS target.
3. Write `tools/build_xiv_ios.py` from xi-on-anything's `tools/build_ios.py`: same compile and
   bundle steps, this repo's source lists (`PORTABLE`, `HOST`, `generated/ffxivgame`), no LuaJIT,
   imgui or mbedtls unless something here needs them. Keep `-flto=thin` for the translation.
4. Add an iOS entry (`tests/xivboot.c` with `main` renamed, or a small `ios_main.c`): game folder
   from `Documents/FINAL FANTASY XIV`, arguments from `Documents/run.args`, log to a file.
5. Make it build and start in the iOS simulator on the Mac. The simulator reads the Mac's files, so
   the game folder does not need copying yet. Fix `app_icon.m`, `libproc.h`, the window size and
   whatever `gfx9_metal.m` uses that iOS lacks.
6. Sign-in: port `xiv_login.py`'s Meteor path and `xiv_args.py` to C, or pass a session made on
   the Mac through `run.args` for the first tests.
7. Device build on the iPad with both entitlements. Copy the patched game folder into the app's
   Documents with Finder. Watch the log for the guest window placement and memory.
8. Paired controller first, then touch controls and the keyboard from xi-on-anything.
9. iPhone last.

## Where things are

- Upstream: https://github.com/rubymatrix/xiv-123b-on-mac and
  https://github.com/rubymatrix/xi-on-anything
- The operator's server notes live in the FFXIV project on the Windows machine
  (`docs/1.0-SERVER.md`, `ffxiv-server/README.md`). Meteor's login site and lobby host come from
  there.
