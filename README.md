# Oscilloscope

A compact, multi-track waveform viewer for macOS audio hosts. Insert an Oscilloscope VST3 on up to eight stereo tracks, then open one editor to compare their waveforms. Audio passes through unchanged with zero added latency.

The viewer offers stacked and overlay layouts, independent freeze controls, L/R and M/S projection, source colors, peak-preserving rendering, and clip threshold and headroom knobs. Metal renders the waveforms with a CPU fallback.

## Requirements and limits

- Apple Silicon Mac running macOS 12 or later.
- A stereo VST3 host; development has focused on Ableton Live.
- 48 kHz sessions with a fixed tempo, up to eight sources and 16 seconds of history per source.
- Settings and source identities are runtime-only.

This is an early development version. Public Developer ID signing and notarization are not yet provisioned. CI artifacts are explicitly marked **development**; their plugins use ad-hoc signatures and their installers are unsigned. macOS may block downloaded development packages.

## Using the viewer

Start playback and insert the plugin on the tracks you want to compare. Open an editor, choose the visible sources and beat window, then select Stacked or Overlay. All admitted instances contribute while an editor is open.

Choose an active track as the shared reference. It supplies timing rather than routing or latency compensation. Each editor can freeze independently; **Reset capture** resets all live viewers. Tempo changes, seeks or reference loss hold the live view until Reset. Verified transport loops continue without Reset.

Use the channel selector for Left, Right, Stereo L/R, Mono (Mid), Side or Mid/Side. **Headroom** adjusts the display from 0 to 24 dB above full scale. Each source's **Clip dBFS** knob sets its visual warning threshold from -60 to +24 dBFS; it does not change gain. Double-click either knob to restore its default. Source names and colors can be overridden in the editor.

## Building and installing

Install Xcode command-line tools, CMake 3.24+, Ninja and Python 3. CMake fetches JUCE 8.0.12 with a pinned SHA-256 checksum.

```sh
scripts/package-macos.sh --mode development
```

The script builds Release with tests and development probes disabled. It creates a versioned `.pkg`, `.zip` and `SHA256SUMS` under `dist/development`, without installing anything. To reuse an existing JUCE source tree, add `--juce-path /path/to/JUCE`.

Quit the host before installing. The installer requires administrator access and installs to `/Library/Audio/Plug-Ins/VST3/Oscilloscope.vst3`. Remove any previous user-level copy from the host's scan folders to avoid duplicate versions, then restart the host and rescan VST3 plugins.

See [macOS distribution](docs/distribution.md) for packaging, upgrades and production signing, and [CI/CD](docs/ci.md) for build artifacts and draft development releases.

## Licensing

A project license has not been selected. Public source availability does not grant a project license. JUCE is fetched as a separate dependency and is subject to its [JUCE 8 licensing terms](https://juce.com/legal/juce-8-licence/). Resolve the applicable project and dependency licensing before redistributing builds.
