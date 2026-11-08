# glOSports-gtalcs — GTA Liberty City Stories (Android), native, on the GA36-MB

Grand Theft Auto: Liberty City Stories through Rockstar's Android build
(War Drum Studios, 2016, built from the PSP code): a loader (`loader/`) maps
the game's own `libGTALcs.so` (armeabi-v7a, ARM code), resolves its imports
to goodluckOS's glibc, ARM's Mali GLES 2.0 driver (from the folder's `glos/`,
see `goodluckOS-mali-driver`), OpenAL Soft and SDL's pad, answers its Java calls
through a fake JNI, and runs it. The loader core is carried over from
`glOSports-gtavc`. (`glOSports-lcsrecomp` runs the PSP game as a
recompilation instead.) The console stays stock goodluckOS.

Status: runs on the GA36-MB: straight to the main menu, new
game and Resume, Android's loading screen. In-game about 18 fps in the
safehouse at High graphic detail, 23–27 at Low. Sound, saves and the in-game
controls are not checked in detail.

## Game files

From your own install of the Android game (package
`com.rockstargames.gtalcs`), into `/home/player/ports/gtalcs/game/`:

| From | Version | In `game/` as |
|---|---|---|
| the `.apk` | version 2.2 (`libGTALcs.so` of 2016-07-15, md5 `9c1fd961…`) | `libGTALcs.so` (from `lib/armeabi-v7a/`) and `assets/` |
| `main.17.com.rockstargames.gtalcs.obb` and `patch.15.com.rockstargames.gtalcs.obb` | 17, 15 | `files/`: unpacked (below) |
| the `.apk`'s `res/raw/intro.m4v` | | `Movies/LCS_INTRO_1080.m4v`, converted (`tools/movies.sh intro.m4v`, below; optional) |
| the `.apk`'s `res/raw/` | | `loading_screen_1.png` … `_4.png`, `hud_loadbar.png`, `hud_loadbar_small.png`, `hud_loadbarprogress.png`, `hud_loadbarprogress_small.png` (the loading screen) |

The OBBs are in `/sdcard/Android/obb/com.rockstargames.gtalcs/` on the phone.
They are the game's own encrypted WAD archives (not zips). Unpacked once into
`game/files/`, the game needs no OBB: the loader opens
`files/<name>` for every file it asks for (`patch_unpacked_files` in
`loader/game.c`; the console's HOME is vfat, so names match in any case):

    tools/unpack.py files libGTALcs.so tools/lcs-names.txt -- main.17.….obb patch.15.….obb

The OBBs keep hashes only; `unpack.py` recovers the names (the texture
mapping, `libGTALcs.so`'s strings and formats, names in the unpacked files,
the names list; 9,739 of 10,135) and puts the rest in `files/_byhash/<hash>`,
where the loader looks by the same hash. Without `files/` the loader still
mounts `main.obb` and `patch.obb` (`GTALCS_FILELOG=1` logs every name opened).

## The game folder on the console (`/home/player/ports/gtalcs/`)

| | |
|---|---|
| `gtalcs` | the loader (DT_RPATH `$ORIGIN/lib`) |
| `lib/` | OpenAL Soft 1.19 and its dependencies (sndio, bsd, md) |
| `alsoft.conf` | OpenAL reads it from the game folder by itself (ALSA) |
| `game/` | the game files above; settings (`gta_lcs.set`) and saves land here too |
| `glos/` | the ARM driver set from `goodluckOS-mali-driver` |
| `start.sh` | pad mapping and settings, then `exec ./glos/glos-gpu run mali ./gtalcs` |

Launcher entry: `gtalcs.puppy` ("Grand Theft Auto: Liberty City
Stories"), its icon a crop of a PS2 banner. Log: `/dev/shm/gtalcs.log`.

## Settings (`start.sh`)

| Variable | Default | |
|---|---|---|
| `GAME_STICK_SCALE`, `GAME_STICK_DEADZONE` | 1.5, 0.12 | stick shaping (radial dead zone after scaling) |
| `GAME_FPS_COUNTER` | 0 (1 in `start.sh`) | frames per second, top right |
| `GAME_FPS_CAP` | 0 | 30 caps the game at 30 fps (frames wait for their slot on a 1/30 s grid, a late one starts the grid again); 0 no cap |
| `GAME_AUDIO_RATE` | 44100 | OpenAL mixing rate |

## How it runs, and what the loader does (`loader/game.c`)

Unlike the older War Drum ports there is no NVEvent main loop: on Android,
Java's GLSurfaceView renderer drives the game, and Java hands it paths and
device facts through `GTAJNIlib`'s natives. The loader makes those calls:

- **Startup:** `JNI_OnLoad` (it looks up its Java methods, so the fake
  `FindClass` returns a class, not NULL), then `setAssetManager`,
  `setGameFilesDir` with the main and the patch OBB (mounted in that order
  by `_LogicalFS_Init`), `setPrivateFilesDir` (`game/`, with the trailing
  slash it appends names to), `setDeviceInfo` (1.2 GHz: the lowest detail
  index), OS version, no vibrator, not a TV.
- **Frames:** a render thread with the GL context calls `viewOnInit`,
  `setDisplaySize` (which runs `InitGame`), `viewOnResume`, then
  `viewOnDrawFrame(ms)` every frame, and swaps.
- **Java methods** (`CommonAPI`): language English, version, quit. No
  videos (the intro movie counts as played), music playlist, Social Club or
  loading-screen overlay.
- **Legal screen and rating:** the legal screen and "tap to continue" count
  as shown (`shownLegalScreen`), so the game opens on the main menu; it
  never asks for a rating (`CheckForAppRate`).
- **Social Club:** the startup waits for `gbSocialClubStarted`, which Java's
  Social Club views set; the loader sets it. The main menu's Rockstar
  button and sign-out (`SocialServices::InvokedFromIngame`, `SignOut`) do
  nothing, as they need the Social Club object those views make.
- **Pad:** buttons go in as Java sends them, `onJoyButtonDown` /
  `onJoyButtonUp` with War Drum's button index, which set the array that
  `WarGamepad_GetGamepadButtons(button)` reads (one button, not a mask as in
  the older games) and queue the button events the menus act on. Sticks and
  triggers answer `WarGamepad_GetGamepadAxis`; the pad is reported as an
  Xbox 360 pad. The game switches to the pad (prompts, menu navigation) when
  the title screen sees pad input, which the legal skip leaves out:
  `GTouchscreen`'s pad flag (+8) is set every frame.
- **Lock-on** (as in `glOSports-gtavc`, after GTA SA): the PSP code took
  only peds in front of the player within 40 m
  (`CPed::OurPedCanSeeThisOne`) and scored them by distance plus 5 × the
  angle off the player's facing. Now `EvaluateTarget` scores every
  candidate in weapon range by SA's rules: off the player's facing and,
  when on screen, off the camera's heading (from the camera to the
  player), the better counting; 1 − 0.785 × the angle off over the fourth
  root of the distance, times 0.8 for aiming priorities and the player's
  threats and 0.1 for the rest; only in line of sight (the game's own test,
  buildings and objects). The two searches' `OurPedCanSeeThisOne` calls
  answer yes. The next target (`EvaluateNeighbouringTarget`) was already
  SA's rule, the nearest angle on that side within 50 degrees; it also
  needs line of sight now. `ShiftTargetLeft/RightJustDown` (the PSP's
  D-pad left / right with the target button held) also answer a
  right-stick flick past half.
- **Loading screen** (`loading.c`): on Android Java draws it over the game:
  a random one of the four pictures, stretched; with the bar
  (`hud_loadbar*`, cut to the progress, centred at 88% of the height, the
  small pair as the big one is wider than 640) and its fake progress
  (every 250 ms 2.5% of what is left, up to 95%) when the game asks for
  that. The loader draws the same into the game's GL context and swaps,
  since a load runs inside one frame, and keeps it up until the game hides
  it. Two differences: none during the startup (states 0–7 of
  `OS_ApplicationTick`, which the intro movie covers on Android; here it
  only flashed before the menu), and the random picture stays for the whole
  load (Java switches to `loading_screen_3` at the first progress, so the
  first one only flashed).
- **HUD:** placed as in `glOSports-gtavc` / `glOSports-gta3` (after GTA
  SA's Xbox 360 layout): the radar bottom left, its ring centred 76 in and
  93 up, about 100 across (`CHud::GetRadarPosX/Y`, `GetRadarSizeX/Y`
  answer it; the game draws it 25 pixels below the centre they give); the
  stats block's button (`GTouchscreen` +0x78) 30 pixels in from the right
  and 36 down, the help box's (+0x7c) at 32, 36, set every frame in layout
  units (+680 / +684 scale them to pixels).
- **Graphic detail:** renders the 3D view at 50% (Low), 75% (Medium) or 100%
  (High) of the screen, but `GetOffscreenResScale` kept at least 720 lines,
  so on 480 every setting was full size. That minimum (a literal 720.0f) is
  0: the setting applies here as on bigger screens.
- **Game detail:** picks a row (Low, Medium, High) of three tables in
  `gDeviceDetailSettings` (`C_DeviceDetailSettings`): world and model
  culling (within a distance everything is drawn, beyond it an object has
  to cover a part of the screen that grows with distance:
  `CalcScreenSize`), and LOD (vehicle and ped LOD and fade distances, max
  peds, max moving and parked cars, population radii, as the game's
  `dvLODSettings<level>_*`). The game's rows differ little (the same world
  and model limits on all three), so Low and Medium did little. High stays
  the game's; Low and Medium take 0.5 and 0.75 of the game's own rows, as
  Graphic detail's resolution steps: distances and ped and car counts
  times the factor, the screen parts divided by it. The car and ped fade
  distances stay at least the spawn radius (they would appear in view), the
  spawn and removal radii stay the game's. Low: 10 peds, 5 moving and 3
  parked cars, cars fade at 40 (High 30, 30, 10, 160). Set every frame, so
  a change in the menu applies at once (peds and cars already there stay
  until they go).
- **Frame Limiter:** the game only stores `m_PrefsFrameLimiter`, and the
  mobile menu has no entry for it: the cap is `GAME_FPS_CAP` in `start.sh`
  instead, as in `glOSports-maxpayne`. The menu's "mobile effects" value is
  likewise only stored, never used.

In the shared loader (`imports.c`, `openal.c`):

- **Assets:** `AAssetManager_open` reads `game/assets/`, and absolute paths
  (the OBBs). `Platform::File` asks for its position as length minus
  remaining on every read, so an asset keeps its length and reads through a
  64 KB buffer (seeking to the end each time made a level load take
  minutes).
- **`glGetString`** hands out writable copies: `LgGl::InitGLExtensions`
  runs `strtok` over the extension string, ARM's is read-only.
- **OpenAL:** this build imports OpenAL instead of carrying its own; the
  imports go to the folder's, the float setters through softfp wrappers.
- **C++ exceptions:** the `_Unwind_*` imports come from libgcc_s.

## Performance (measured 2026-11-02)

The safehouse after Resume, Graphic detail Low (50%), lowest settings, no
cap. One run each before the flip-wait test: 18 fps at High (100%), 22–27
at Low.

- **What limits it:** the GPU. Over 10 s the render thread is busy 54% of
  a core (29% of its samples in ARM's GL driver, 7% in the game, the rest
  waiting for the game thread, the flip, or the GPU in the driver's
  ioctls), the game's tick thread 62% (game code), the driver's thread
  waits on GPU jobs. Neither CPU thread is saturated.
- **Game tick thread:** already on and running in parallel with the render
  thread (`gGameTickThread` bound, 4 cores seen): no lever there.
- **Flip wait** (`GAME_VSYNC`, A/B alternating, 30 s each): off 27.6 and
  28.0 fps, on 26.9 and 24.1; swap 0.4 ms against 15–18 ms a frame. Off is
  about 2 fps better and steadier, at the cost of tearing.
- **GL work per frame** (counted in the world): the 3D view into an
  offscreen framebuffer (fully cleared, ~630 draw calls), then the screen
  (one draw copying it up, a depth clear, ~28 HUD draws). No glFinish,
  glFlush, readbacks, texture uploads or mipmap rebuilds; ~16 kB of buffer
  updates. High against Low (18 against ~26 fps) puts the pixel work at Low
  near 6 ms of a ~38 ms frame: the rest is per-vertex and per-draw work on
  the GPU, which only fewer or simpler draws would cut.
- **Depth/stencil discard** (glDiscardFramebufferEXT when leaving the
  offscreen framebuffer and before the swap, A/B): 23.4 and 24.5 fps with,
  24.4 without (the second run without stayed in the menu and doesn't
  count). A tie with no counter to confirm it: dropped.
- **Vertex shaders** (dumped through `glShaderSource`): one shader pair
  draws 392 of the ~630 draws and ~172k of the ~320k indices a frame. The
  world shaders multiply each vertex by the view matrix only for the fog
  distance (and, in the lit and skinned ones, for projection * view). A test
  without that multiply (fog wrong) gave 26.9 and 26.1 fps against 24.5 and
  24.3. The same picture with one multiply less (fog distance to the camera
  position, `projection * view` as one matrix, both computed when the game
  uploads its matrices; 45 shaders rewritten, the dropped uniforms kept in
  the game's list at locations of the loader's) then measured, over ~190 s
  of play each: 26.0 and 26.5 fps against 25.8 and 26.4 (vsync on, both at
  its 27.5 step most of the time), and with vsync off 28.3 against 29.4. No
  gain; the earlier test's shorter window had caught a dip. Dropped.
- **Game detail** (above): the lever that is left, at the cost of
  distant detail and traffic; 0.5 and 0.75 still look right outdoors.
- **Not touched:** the effect switches (blur, fog) by choice; the menu's
  "mobile effects" and `m_PrefsLOD` values, which only the menu code reads.

## Video

The intro is the APK's
`res/raw/intro.m4v`, 1920x1080 MPEG-4 Part 2 at 3 Mbit/s: too heavy to
decode in time, and a 4:3 picture between black side bars.
`tools/movies.sh` converts it on the Mac (the 1440x1080 picture cropped out,
640x480 H.264 baseline, AAC 44.1 kHz stereo) to `gamedata/movies/Movies/`; it goes
into `game/Movies/LCS_INTRO_1080.m4v`, the name the game asks for. On Android
a Java player shows it while `OS_ApplicationTick`'s startup polls
`OS_MovieIsPlaying` and goes on drawing; here `OS_MoviePlay` and
`OS_MovieIsPlaying` go to `loader/movie.c` (the player of `glOSports-gtactw`):
the game's swaps show the newest picture in place of its frame (the poll and
a render thread's sleeps would too, when they hold the screen). A or Start
skips; without the file the game goes straight on.

## Build

```
./build.sh      # loader/ -> build/gtalcs (Docker, Debian bookworm armhf)
./package.sh    # -> out/ports/gtalcs/ and out/.local/share/applications/gtalcs.puppy
```

`build.sh` runs `tools/build-ffmpeg.sh` first when `build/ffmpeg/` is
missing: a static FFmpeg 6.1.2 (MP4 demuxer, H.264 and AAC decoders, NEON),
from ffmpeg.org's `ffmpeg-6.1.2.tar.xz` (sha256 `3b624649…`) unpacked in
`upstream/ffmpeg-6.1.2`.

Test tools are shared: `../goodluckOS-tools/testing-sync.sh gtalcs` puts them
on the console (`glos/testing/`), `testing-clean.sh gtalcs` takes them off,
`shot.sh gtalcs file.png` takes a screenshot (see `goodluckOS-tools`).
`stackscan` there gives the return addresses into the game on each thread's
stack: where each thread waits.

## Licence

The port's own code is MIT (`LICENSE`); `package.sh` puts a copy into the game folder's
`licenses/`. Third-party code it carries keeps its own licence (below).

## Third-party components

Code carried over from the Vita loaders it names keeps their MIT notices;
those, FFmpeg (LGPL-2.1, linked statically for the videos), OpenAL Soft and
the other bundled libraries, and ARM's Mali driver are listed with their
licences in `licenses/`, which `package.sh` copies into the game folder. The
game itself is not included.
