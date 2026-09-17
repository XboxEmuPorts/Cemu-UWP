# Cemu UWP Host

Cemu UWP Host is an experimental Universal Windows Platform front end for running a modified Cemu Wii U emulator on Windows and Xbox Series S in Developer Mode. The host embeds Cemu through the `CemuEmbed` C API and presents its output through a XAML `SwapChainPanel`. Direct3D 11 remains the stable renderer; Direct3D 12 is available as an explicitly experimental in-tree backend.

The primary development and validation target is **Xbox Series S**. Desktop x64 builds are useful for diagnostics, but they do not reproduce the Xbox memory limit, D3D11On12 driver behavior, storage broker, controller projection, or performance characteristics.

The modified Cemu runtime used by this project is maintained at [rodrigoandrigo/Cemu](https://github.com/rodrigoandrigo/Cemu).

> [!IMPORTANT]
> This port is under active development. Its Direct3D 11 renderer does not yet provide the same compatibility as the upstream desktop Vulkan renderer. Some games, shaders, Graphic Packs, geometry effects, and long sessions can still expose rendering or stability problems.

## Current status

Implemented and currently available:

- Embedded Cemu lifecycle without creating the desktop wxWidgets interface.
- Native Direct3D 11 rendering inside the UWP application.
- Experimental internal Vulkan-to-Direct3D 12 backend. It keeps Cemu's Vulkan renderer front-end and redirects its Vulkan dispatch into an in-process D3D12 translation layer. Mesa/Dozen is design reference only and is not a runtime or build dependency.
- Xbox controller input, automatic player-one profile, selectable Wii U controller type, and in-game virtual mouse.
- Installed-game library with base game, update, DLC, region, version, and Graphic Pack information.
- Direct launch of WUD, WUX, ISO, WUA, WUHB, RPX, and ELF files.
- Installation of extracted base games, updates, and DLC.
- Persistent `GamesToInstall` folder for Xbox users without external storage.
- `keys.txt` import and validation.
- Secure community Graphic Pack download through the Windows HTTP stack, manual pack import, and per-game activation controls.
- Native LEGO Dimensions Toy Pad emulation with persistent virtual tags.
- Performance metrics toggle and a Settings tab backed by Cemu's embedded settings ABI.
- Xbox Series S memory guard and a 512 MB disk-backed D3D11 shader-cache budget.
- Full-client game presentation with the command bar and option tabs hidden while a title runs.
- Direct execution from external storage through brokered streams without copying the selected game into internal storage.
- Persistent external-library authorization through the UWP `FutureAccessList`, with automatic restoration, combined rescanning, and an explicit command to forget only the folders registered by this host.
- Settings saved immediately when changed; no separate Apply button is required.
- Arcade Botânico frontend with porcelain surfaces, Circuit Green focus, sage secondary planes, recolored package assets, and full-height tool pages adapted to retain the current Graphic Packs and Xbox-specific controls.
- Square, wrapping game cards with compact metadata and the title-provided `meta/iconTex.tga` artwork used by Cemu's desktop game list; the Start game action appears in the ready-to-play system panel only after a game is selected.

## Xbox Series S memory policy

Xbox Developer Mode gives the packaged application a constrained process-memory budget. Testing on Series S observed a limit close to 5120 MB, so the renderer intentionally operates below it and leaves headroom for D3D11On12, the Xbox shader compiler, XAML, audio, and system allocations.

| Process commit | Renderer behavior |
| --- | --- |
| Below **3840 MB** | Deferred shader and pipeline work may resume. |
| **3968 MB** | Hysteresis release threshold for leaving memory-pressure state. |
| **4096 MB** | New shader/driver work is deferred and the memory guard runs. |

These thresholds are the current tested policy and must remain unchanged until new Series S measurements demonstrate safe additional headroom.

Memory-related renderer work includes:

- A reusable 4 MB dynamic index-upload ring with GPU retirement before wrapping.
- Reusable uniform, copy, and scratch buffers instead of retaining one allocation per operation.
- Texture trimming and release of reconstructible transient resources under pressure.
- GPU retirement, DXGI trimming, and heap compaction during heavy recovery.
- Serialized shader translation and native-pipeline creation to avoid concurrent allocation spikes.
- Retryable shader failures when creation is deferred by the memory guard.
- A 128 MB stack reserve for both Debug x64 and Release x64. Stack pages remain demand-committed; the reserve prevents the Xbox driver compiler from reaching the stack guard during complex pipeline creation.

## Rendering and shader status

The custom renderer translates Wii U shader programs through GLSL/SPIR-V/HLSL compatibility paths and creates Direct3D 11 shaders suitable for the Xbox D3D11On12 environment. The entire D3D11 backend requires exactly **DirectX 11 Feature Level 11.0**; higher or lower feature levels are rejected instead of selecting alternate renderer paths.

Current graphics work includes:

- 512 MB maximum budget for the persistent D3D11 shader cache.
- Native shader materialization on first use to reduce title startup work.
- Atomic temporary-file replacement and cache validation before loading cached data.
- Reduced shader-progress logging to avoid repeated output affecting performance.
- Input-layout caching and pre-reserved native state caches.
- Runtime sampler-swizzle handling and format compatibility paths.
- Geometry-shader translation and fallbacks for supported layouts.
- Stream-output validation and raster-shader preservation when native stream output is unavailable.
- Hardened index decoding, little-endian index conversion, and programmable primitive restart.
- Vertex-range validation and protection against stale buffer references.
- GPU-side buffer-cache copies where supported.
- Resource-creation recovery that preserves valid renderer state.

These changes reduce missing geometry, stale-buffer corruption, long stretched polygons, and repeated shader work, but do not guarantee complete compatibility.

Areas still under validation:

- Arbitrary Wii U geometry shaders and uncommon interface layouts.
- Stream-output-heavy effects.
- Rare GX2 primitives, vertex formats, mutable aliases, and sampler combinations.
- Title-specific texture or geometry artifacts in previously unseen areas.
- Long-session stability while new native shaders and D3D11On12 pipelines continue to appear.
- Graphic Packs designed specifically around Vulkan behavior.

First-time traversal of a scene can still stutter while new shaders are translated and compiled. Revisiting the same scene should use the cache unless its input, renderer version, or native format changed.

## Features

### Game library and content

- Installed-title discovery from the persistent MLC.
- Game name, Title ID, region, version, update, DLC, and Graphic Pack status.
- Automatic association of updates and DLC with their base title.
- Automatic mounting of the installed base game, highest applicable update, and matching DLC.
- Direct launch of supported single-file and executable formats.
- Brokered file and folder access without `broadFileSystemAccess`.
- Direct reads from external storage on Windows and Xbox; games are not staged or copied to internal storage before launch.
- Recursive scanning of the local installation folder.
- Persistent confirmed selection: moving focus with the D-pad does not replace the game confirmed with `A`.

### Supported input formats

The host currently recognizes:

| Format | Typical use |
| --- | --- |
| `.wud` | Wii U optical-disc image |
| `.wux` | Compressed WUD image |
| `.iso` | Disc image accepted by Cemu's title loader |
| `.wua` | Wii U Archive |
| `.wuhb` | Wii U homebrew bundle |
| `.rpx` | Wii U executable |
| `.elf` | Homebrew or development executable |
| `code` + `content` + `meta` | Extracted base game, update, or DLC |
| `title.tmd` content | NUS-style title folder accepted by the Cemu loader |

An RPX or ELF title may require adjacent RPL modules and supporting content. Recognition does not guarantee that every title is compatible with the current D3D11 backend.

### Storage and installation

- Installed base games, updates, DLC, keys, and manually imported Graphic Packs are copied only when the user explicitly requests installation or import.
- External standalone games and extracted game folders run directly through the storage broker on every supported platform.
- Platform-assisted `StorageFile.CopyAsync` is preferred for operations that genuinely install content.
- Providers that cannot perform an installation copy use a bounded sequential-buffer fallback.
- Storage metadata requests are batched to reduce broker IPC overhead.
- Large files and Graphic Packs are copied in chunks.
- Successfully processed extracted content receives a `cemu-installed.txt` marker.
- Successfully processed Graphic Pack sources receive a `cemu-graphic-pack-installed.txt` marker.

### Xbox controls

- Xbox-compatible controllers are discovered through `Windows.Gaming.Input::Gamepad` on the XAML apartment.
- Only plain input snapshots cross into Cemu worker threads; apartment-affine WinRT objects are retained by the host.
- A player-one Wii U GamePad profile is created automatically when required.
- The top-bar controller and account indicators are compact icons suitable for television layouts.
- The system controller cursor is disabled while the emulator is running.

### Virtual mouse

While a game is running:

- Press `L + R` together to enable or disable the virtual mouse.
- Move the pointer with the left thumbstick.
- Press or hold `A` for the left mouse button.
- The cursor is displayed as a single arrow.
- Mouse controls are withheld from the emulated controller while the mouse is active.
- Pointer events are sent to Cemu's render surface so the virtual mouse can interact with supported in-game pointer and keyboard screens.

### Audio

- XAudio 2.8 output for UWP and Xbox.
- Automatic fallback to XAudio 2.8 when a backend stored in `settings.xml` is unavailable in the UWP sandbox.
- Configurable latency, channel mode, TV/GamePad/input volume, and portal volume through the Settings tab.

### Performance metrics

The top bar includes a toggle for Cemu's native overlay. The embedded preset can display:

- FPS;
- draw calls;
- total and per-core CPU usage;
- RAM usage;
- VRAM usage.

The top-bar button owns the performance-overlay preset and visibility.

## User interface

The interface uses English text and a dark green/blue theme designed for controller navigation and less experienced users.

Available tabs:

- **My games**: content installation, direct file/folder launch, `keys.txt`, local scan, Graphic Packs, and installed games.
- **Toy Pad**: native LEGO Dimensions figure and tag management.
- **Graphic packs**: four-column per-game catalog with pack name, category, description, and an individual enable/disable switch.
- **Settings**: global scalar settings exposed by the embedded Cemu runtime.
- **Help and errors**: diagnostics and actionable user-facing errors.

The **Getting started** guide always opens collapsed and can be expanded temporarily when needed.

While no game is running, every tab extends from the command bar to the bottom of the interface. Long settings, diagnostics, libraries, and Graphic Pack collections scroll inside that full-height area. The host does not request a second fullscreen transition on launch; Xbox already supplies the app window at the correct display size. When a game starts, both the top command bar and the option tabs disappear and the emulator surface expands across the complete client area.

## Settings tab

The backend exposes a versioned `CemuEmbedSettings` structure. The Settings tab reads the relevant scalar global options from that ABI and saves each change immediately. There is no Apply settings button. Performance metrics are controlled separately by the top-bar button:

- CPU mode and console language.
- Boot sound and screen-saver behavior.
- Fixed Direct3D 11 backend information.
- VSync, asynchronous shader compilation, GX2DrawDone synchronization, and upside-down rendering.
- Upscale/downscale filters, fullscreen scaling, game gamma, and display gamma.
- Notification position, scale, controller profile, battery, shader compilation, and friends notices.
- Audio backend, delay, channel modes, and volumes.
- Skylanders portal, Disney Infinity base, and LEGO Dimensions Toy Pad emulation.

Renderer selection is host-owned. Direct3D 11 is the default; Direct3D 12 is marked experimental in Settings and takes effect after restarting the application. Paths, accounts, Graphic Packs, controller profiles, and installed content use dedicated host APIs rather than the scalar settings structure. USB-device and startup-only changes also apply after restarting the application.

## LEGO Dimensions Toy Pad

The Toy Pad tab integrates Cemu's native `nsyshid/Dimensions` emulation rather than running a separate external emulator.

It supports:

- Native character, vehicle, and gadget catalog enumeration.
- Seven virtual Toy Pad positions.
- Placement, replacement, removal, and movement of tags.
- Persistent tag files in application data.
- Saving upgrades and other tag changes written by the game.
- Optional Toy Pad activation from Settings.

Host options remain hidden while a title is running so the emulator owns the complete client area. Stop or leave the running title before returning to the tabs.

Select both a figure and a Toy Pad position before placing it. The Toy Pad must be enabled before Cemu initializes; changing its USB setting therefore requires an application restart.

## Repository layout

The default project expects the repositories to be siblings:

```text
Projects/
|-- Cemu/
|-- Cemu-UWP-Host/
`-- SDL3-uwp/
```

| MSBuild property | Default | Purpose |
| --- | --- | --- |
| `CemuBuildDir` | `..\Cemu\bin` | Cemu DLL/import library, resources, and game profiles |
| `CemuIncludeDir` | `..\Cemu\src` | Directory containing `Cemu/CemuEmbed.h` |

## Requirements

- A Windows x64 development PC.
- Windows 10 version 1903 (`10.0.18362.0`) or newer for the project minimum.
- Windows SDK `10.0.26100.0`.
- Visual Studio 18 2026 with the MSVC v145 C++ toolchain and UWP C++ tools.
- CMake 3.21.1 or newer.
- A local checkout of [rodrigoandrigo/Cemu](https://github.com/rodrigoandrigo/Cemu).
- A local checkout of [rodrigoandrigo/SDL3_UWP](https://github.com/rodrigoandrigo/SDL3_UWP).
- Xbox Series S with Developer Mode for target validation and deployment.

Use the native Visual Studio/CMake tools. Do not configure the MSVC build with an MSYS2 CMake executable.

The project retains inherited non-x64 configurations, but the embedded runtime, package payload, testing, and bundle are currently configured for **x64**.

## Building

Run these commands from **Developer PowerShell for Visual Studio 18**.

### 1. Configure the embedded Cemu runtime

```powershell
Set-Location C:\path\to\Cemu

cmake -S . -B build-msvc-d3d11 `
  -G "Visual Studio 18 2026" `
  -A x64 `
  -DCEMU_UWP=ON `
  -DENABLE_D3D11=ON `
  -DENABLE_D3D12=ON `
  -DSDL3_UWP_SOURCE_DIR="C:\path\to\SDL3-uwp"
```

`CEMU_UWP=ON` enables both the stable Direct3D 11 path and the experimental internal Vulkan-to-Direct3D 12 path. The Mesa tree used during its design is reference-only and is neither linked, copied nor packaged. Initial configuration can take time while vcpkg builds static dependencies.

### 2. Build the Cemu runtime

```powershell
cmake --build build-msvc-d3d11 `
  --config Release `
  --target CemuBin `
  --parallel
```

The host currently links and packages the Release runtime for both Debug and Release host configurations. The Cemu build must produce:

```text
Cemu\bin\Cemu_release.dll
Cemu\bin\Cemu_release.lib
Cemu\bin\resources\
Cemu\bin\gameProfiles\
```

`resources\sharedFonts\CafeCn.ttf` is required. The host project stops with an explanatory error if the runtime DLL or this shared font is missing.

### 3. Build the UWP host

```powershell
Set-Location C:\path\to\Cemu-UWP-Host

& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  .\Cemu-UWP-Host.vcxproj `
  /restore /m `
  /p:Configuration=Debug `
  /p:Platform=x64
```

For a Release package, replace `Configuration=Debug` with `Configuration=Release`.

For a non-default repository layout:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  .\Cemu-UWP-Host.vcxproj `
  /restore /m `
  /p:Configuration=Release `
  /p:Platform=x64 `
  /p:CemuBuildDir="C:\path\to\Cemu\bin" `
  /p:CemuIncludeDir="C:\path\to\Cemu\src"
```

The project produces an x64 application bundle and uses `Cemu-UWP-Host_TemporaryKey.pfx` for development signing. Install or trust the corresponding certificate when required by the deployment method. Replace the development signing identity and the project-specific `AppInstallerUri` before distributing a package.

The current package manifest version is **1.0.8.0**.

## Using the application

### First launch

1. Start the application and wait until Cemu reports that it is ready.
2. Import a valid `keys.txt` if your legally dumped encrypted game formats require it.
3. Install extracted content, open a supported file/folder, or copy content to `GamesToInstall`.
4. Select **Scan local folder** to process local content.
5. Focus a game and press `A` to confirm it.
6. Select **Start game**.

### Install extracted base games, updates, and DLC

1. Open **My games**.
2. Select **Install content**.
3. Choose a directory containing `code`, `content`, and `meta`.
4. Wait for the brokered copy and installation to finish.
5. Refresh the library if necessary.

The same command accepts base games, updates, and DLC. Metadata determines the content type and target MLC location. An update or DLC must match an installed base Title ID to be mounted with that game.

### Open a game file or folder

- Use **Open game file** for WUD, WUX, ISO, WUA, WUHB, RPX, or ELF.
- Use **Open game folder** for extracted titles, supported NUS-style content, or executable layouts requiring adjacent files.

Files and folders opened through a picker use brokered UWP access. The selected game is read directly from its source storage on both Windows and Xbox and is not copied into `LocalState`. Installed content deliberately copied into application storage remains available independently of the original source.

### Use `GamesToInstall` without external storage

The application creates a persistent `LocalState\GamesToInstall` directory. Copy supported content there through the Xbox Device Portal or another available package-data transfer method, then select **Scan local folder**.

The scan recursively detects:

- WUD, WUX, ISO, WUA, WUHB, RPX, and ELF files;
- extracted base games, updates, and DLC;
- Cemu Graphic Packs containing `rules.txt`.

Single-file games remain listed while their source files remain in `GamesToInstall`. Extracted content is installed into the MLC. Processing markers prevent the same extracted title or Graphic Pack from being imported on every refresh. Delete the appropriate marker only when intentionally requesting reprocessing.

### Import `keys.txt`

1. Stop any running game.
2. Open **My games**.
3. Select **Import keys.txt**.
4. Choose a text file containing valid Cemu-format 128-bit keys.

The backend validates the file, replaces the application-data `keys.txt`, and reloads the key cache immediately. Invalid input is rejected. Some encrypted WUD, WUX, ISO, and WUA titles cannot be identified without the correct keys from hardware and software you legally own.

This project does not provide title keys, common keys, games, firmware, account data, or copyrighted console files.

### Download and manage Graphic Packs

1. Open **Graphic packs**.
2. Select **Download community packs** to retrieve the latest official collection, or select **Install packs** to import a local directory.
3. Choose an installed game.
4. Enable or disable each compatible pack individually. Changes are saved immediately and used on the next launch.

Community downloads use `Windows::Web::Http::HttpClient`, the Windows certificate store, GitHub's current release metadata, a 256 MiB safety limit, path-safe ZIP extraction, and transactional staging/backup directories. Local imports scan for valid `rules.txt` files and copy only the selected pack directories into persistent application data. Downloading, importing, refreshing, or launching does not automatically enable every compatible pack.

Graphic Packs can change shaders and executable behavior. If a game develops artifacts or crashes, retest without title-specific packs before reporting a renderer regression.

### Select and start a game with a controller

1. Use the D-pad to move focus through the library.
2. Press `A` on the intended game to confirm it.
3. Navigate to **Start game** and press `A`.

Focus and confirmed selection are deliberately separate. D-pad navigation does not change the confirmed title. Press `A` on another title to replace the selection.

## Application data

The host maps writable Cemu paths into UWP package storage:

| Package location | Contents |
| --- | --- |
| `LocalState` | `settings.xml`, `keys.txt`, MLC, saves, accounts, `GamesToInstall`, imported Graphic Packs, Toy Pad tags, logs, and persistent D3D11 driver/native caches |
| `LocalCache` | Transferable/precompiled Cemu shader caches and temporary brokered-copy staging data |
| `InstalledLocation` | Read-only packaged DLL, resources, game profiles, and application assets |

On a Windows diagnostic installation, the package root is normally below:

```text
%LOCALAPPDATA%\Packages\<package-family-name>\
```

Resetting or uninstalling the package can remove both `LocalState` and `LocalCache`. Back up important saves, tags, settings, and legally obtained keys before resetting the application. Native driver caches and transferable shader caches are different formats and must not be treated as interchangeable files.

## Troubleshooting

### Cemu does not initialize

- Confirm that `Cemu_release.dll` was packaged.
- Confirm that `resources\sharedFonts\CafeCn.ttf` and `gameProfiles` exist under the packaged runtime payload.
- Check **Help and errors** and `LocalState\log.txt`.

### An encrypted game is not recognized

- Import a valid Cemu-format `keys.txt` before scanning.
- Confirm that the file contains the correct keys for the title you legally dumped.
- Rescan `GamesToInstall` after importing the file.

### A selected game changes while navigating

Move focus to the intended game and press `A` once. The highlighted focus can continue moving, but the confirmed game displayed by the host should remain selected until another title is confirmed with `A`.

### Controller detected but game input is unavailable

- Wait until the host has created the Wii U GamePad profile.
- Reconnect the controller if initialization was interrupted.
- Disable the virtual mouse with `L + R`; while it is active, `A`, both shoulder buttons, and the left stick are reserved for mouse control.

### Audio backend warning

The UWP sandbox does not expose every desktop audio API. The host automatically selects XAudio 2.8 when the saved backend is unavailable. This fallback is expected.

### First visit to a scene stutters

New shaders and native D3D11 pipelines can be compiled on first use. Revisit the same area to distinguish expected first-use compilation from recurring stutter. Preserve the shader caches between tests unless cache corruption is specifically being investigated.

### Rendering artifacts

1. Record whether the artifact appears only with a Graphic Pack.
2. Revisit the same area to determine whether it is shader-first-use related.
3. Capture a screenshot or video.
4. Save the complete `LocalState\log.txt`.
5. Report the approximate runtime, game/version, area, and whether the scene had been visited before.

The Direct3D 11 backend remains experimental. Unsupported geometry shaders, stream output, rare vertex formats, format aliases, and title-specific shader assumptions may still render differently from Vulkan.

### Memory-pressure or long-session failure

Do not increase the 3840/3968/4096 MB policy values. Record the performance overlay values and preserve the full log. A useful Series S report includes runtime before failure, RAM/VRAM shown by the overlay, whether new shaders were compiling, and whether the same area is stable immediately after relaunch.

## Testing guidance

For renderer changes, test on the Xbox Series S using a repeatable route:

1. Start from a cold application launch.
2. Record time to the title screen.
3. Enter a shader-heavy area that has not yet been cached.
4. Revisit the same area.
5. Leave the title running for an extended period.
6. Compare FPS, stutter, RAM, VRAM, artifacts, audio, and input.

Do not compare a warm desktop cache directly with a cold Xbox cache. Keep game version, update, DLC, Graphic Packs, resolution, and settings identical between regression tests.

## Project structure

- `App.xaml` / `App.xaml.cpp`: application lifecycle, controller-pointer policy, suspension, and resume.
- `DirectXPage.xaml`: full-height tabs, top bar, four-column Graphic Pack grid, theme, library, Toy Pad, Settings, diagnostics, and render surface.
- `DirectXPage.xaml.cpp`: UI behavior, immediate settings persistence, controller navigation, direct external-storage launch, storage scanning, installation, keys, Graphic Packs, and Toy Pad actions.
- `Cemu_UWP_HostMain.cpp`: C++/CX adapter between UWP storage objects and the `CemuEmbed` C ABI.
- `Common/DeviceResources.cpp`: Direct3D device and XAML swap-chain setup.
- `Package.appxmanifest`: UWP identity, capabilities, and visual assets.
- `Cemu/src/Cemu/CemuEmbed.h`: public embedded-runtime ABI in the adjacent modified Cemu tree.
- `Cemu/src/Cafe/HW/Latte/Renderer/D3D11/`: custom D3D11 renderer and Series S policies.
- `Cemu/src/Cafe/HW/Latte/Renderer/D3D12/`: experimental in-process translation layer used by the Vulkan renderer front-end.
- `LICENSE`: Apache License 2.0 terms for this host repository.

## Security and legal notice

Use only games, updates, DLC, keys, firmware, saves, and account data that you are legally authorized to use. This repository does not distribute Nintendo content, title keys, console secrets, games, firmware, or online-service credentials.

Developer Mode packages are development software. Review the package capabilities, signing identity, deployment destination, and storage contents before redistribution.

## License

Cemu UWP Host is licensed under the Apache License 2.0; see [`LICENSE`](LICENSE).

Cemu is separate software distributed under the Mozilla Public License 2.0; see `LICENSE.txt` in the adjacent Cemu source tree. SDL and all other dependencies retain their own licenses and notices.
