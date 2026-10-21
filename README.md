# Oscilloscope

VST3 multi-track, overlayable/stackable waveform viewer for macOS. Similar to Oszillos Mega Scope. Also has clipping thresholds for each track so you can see how each track clips separately. 

Uses GPU rendering through Metal for smooth waveforms.

Only tested on Ableton.

## Building and installing

Install Xcode command-line tools, CMake 3.24+, Ninja and Python 3. CMake fetches JUCE 8.0.12 with a pinned SHA-256 checksum.

```sh
scripts/package-macos.sh --mode development
```

The script builds Release with tests and development probes disabled. It creates a versioned `.pkg`, `.zip` and `SHA256SUMS` under `dist/development`, without installing anything. To reuse an existing JUCE source tree, add `--juce-path /path/to/JUCE`.

Quit the host before installing. The installer requires administrator access and installs to `/Library/Audio/Plug-Ins/VST3/Oscilloscope.vst3`. Remove any previous user-level copy from the host's scan folders to avoid duplicate versions, then restart the host and rescan VST3 plugins.

See [macOS distribution](docs/distribution.md) for packaging, upgrades and production signing, and [CI/CD](docs/ci.md) for build artifacts and draft development releases.
