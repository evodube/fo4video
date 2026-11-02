# fo4video

An F4SE plugin that plays real video files (MP4, MOV, WMV, MKV…) on in-game TVs in Fallout 4, with sound. No flipbook textures, no conversion.

**Status:** proof of concept. Built and tested against Fallout 4 OG **1.10.163** with F4SE 0.6.23.

## How it works

1. Media Foundation (built into Windows) decodes the file on background threads: one reader for video (converted to RGB32), one for audio (PCM).
2. Each screen gets a *slot*: a tiny placeholder texture `Textures\fo4video\slotNN.dds` plus our own D3D11 dynamic texture.
3. The TV's screen mesh is found (by name, texture path, or a single glowing surface), its material is cloned so only that TV changes, and its diffuse/base texture is pointed at the slot placeholder.
4. In a `Present` hook, the newest decoded frame is uploaded to the dynamic texture, and the placeholder's shader resource view is swapped for ours, so the engine samples the video.
5. Audio plays through XAudio2 and acts as the playback clock. Volume falls off with distance from the player.

## Install

Install the build zip with your mod manager. It contains:

```
Data/F4SE/Plugins/fo4video.dll
Data/F4SE/Plugins/fo4video.ini
Data/Textures/fo4video/slot00.dds ... slot15.dds
Data/Scripts/Source/User/FO4Video.psc
```

Requires F4SE and Address Library for F4SE Plugins. Put videos in `Data\Videos\`.

## Testing (hotkeys)

1. Open the console, click a TV, close the console.
2. **F10**: animated test pattern (checks screen detection + texture swap without any decoding).
3. **F9**: play the next video in `Data\Videos`.
4. **F11**: stop. **F8**: dump the selected object's meshes to the log.

Log: `Documents\My Games\Fallout4\F4SE\fo4video.log`

## Papyrus

See `FO4Video.psc` (`Play`, `Stop`, `PlayTestPattern`, `SetVolume`, …). The `.pex` isn't built yet; compile the source with the Creation Kit compiler or Caprica if you want to call it from scripts.

## Building

Windows, Visual Studio 2022 (C++ workload), [xmake](https://xmake.io) 3.0+.

```
git clone --recurse-submodules https://github.com/evodube/fo4video
cd fo4video
xmake f -m releasedbg -y
xmake build
```

GitHub Actions builds every push and attaches a zip to tagged releases.

## License

GPL-3.0 (required by CommonLibF4), see `LICENSE`.
