# macOS distribution

`VERSION` contains the numeric semantic version shared by the VST3, installer and artifact names. The next version is **0.2.0**: the compact knob UI and production-default resource improvements. Increment the patch for compatible fixes, the minor for new features, and the major for incompatible product behavior. Tag a release with the matching `v` prefix, such as `v0.2.0`.

`packaging/plugin-identity.json` preserves the existing bundle identifier, manufacturer code and plugin code. These values identify the device in existing DAW sessions; retain them when shipping upgrades. The `local` bundle identifier is inherited from the original plugin. Renaming it for branding requires a deliberate migration decision. Certificate names, team identifiers and credentials belong in the local keychain or CI secrets, never in this file.

## Build a local development installer

Requires macOS, Xcode command-line tools, CMake 3.24+, Ninja and Python 3. The build targets macOS 12+, Apple Silicon only. JUCE 8.0.12 is downloaded with its pinned checksum, or supply an existing source tree with `--juce-path`.

```sh
scripts/package-macos.sh --mode development
```

The script configures a Release build with `BUILD_TESTING=OFF` and `OSCILLOSCOPE_DEV_PROBES=OFF`, builds the VST3 and stages a separate copy for packaging. It never installs or publishes anything. Development artifacts use an ad-hoc plugin signature and an unsigned installer; names include `-development` and the enclosed instructions identify their status.

Options: `--build-dir DIR`, `--output-dir DIR`, `--juce-path DIR`, `--parallel N` (default 4). Default build folders are `build/macos-development` and `build/macos-release`; artifact folders are `dist/development` and `dist/release`. Relative paths resolve from the directory where the script is invoked.

Both modes produce a versioned `.pkg`, a `.zip` containing that same installer plus installation instructions/checksum, and `SHA256SUMS` covering the package and archive. Only completed artifacts reach the output directory. Build intermediates, credentials and notarization responses are excluded. Keep build and distribution folders out of source control.

## Provision a public release

Public signing requires membership in the Apple Developer Program, a **Developer ID Application** certificate/private key, a **Developer ID Installer** certificate/private key and access to Apple's notary service. Import the certificates into a keychain outside this repository. Store notary credentials using `xcrun notarytool store-credentials`; its interactive prompts avoid putting passwords in shell history. A keychain profile can hold either Apple ID credentials or an App Store Connect API key. Consult Apple's [custom notarization workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow) for account/profile setup.

Set these values in your shell or protected CI environment; the example uses existing environment values so no identity or password is written into a tracked file:

```sh
export OSCILLOSCOPE_APPLICATION_SIGNING_IDENTITY
export OSCILLOSCOPE_INSTALLER_SIGNING_IDENTITY
export OSCILLOSCOPE_NOTARY_KEYCHAIN_PROFILE
scripts/package-macos.sh --mode release
```

The identity variables select the imported Application and Installer certificates. `OSCILLOSCOPE_NOTARY_KEYCHAIN_PROFILE` selects the stored credential profile. Optional `OSCILLOSCOPE_SIGNING_KEYCHAIN` selects the certificate keychain; optional `OSCILLOSCOPE_NOTARY_KEYCHAIN` selects the notary credential keychain. Leave both unset to use the default keychain search configuration. In CI, import certificates and establish the profile in a temporary unlocked keychain, then delete that keychain after packaging. Do not enable shell tracing around secret setup.

Release mode fails if credentials are missing. It signs the complete VST3 with hardened runtime and a trusted timestamp, verifies the signature and certificate class, creates/signs the installer, submits that installer with `notarytool`, requires an Accepted result, staples/validates the ticket and checks Gatekeeper's installer assessment. The ZIP contains the same stapled installer, making the installer the download and offline verification entry point. See Apple's [notarization requirements](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).

Signing exposes the certificate holder's public identity in the distributed binary and installer, as required by Developer ID. The source tree contains no certificate identity or account information. A successful development package does not establish that production signing/notarization is configured; run release mode after provisioning.

## Installation and upgrades

The installer requires administrator access and writes only `/Library/Audio/Plug-Ins/VST3/Oscilloscope.vst3`. It supports macOS 12+ and arm64, uses a fixed installation location and replaces the whole bundle when upgrading. It refuses to overwrite a newer bundle version. Close the audio host before installing, then restart/rescan the system VST3 folder.

Previous local builds may exist under the user's Library. Move a previous user-level copy out of plugin scan folders before using the installer to avoid loading two versions. The installer deliberately does not remove files from a user's Library. Retain an old plugin outside scan folders if rollback is needed. To uninstall, close the host and move the system VST3 bundle to the Trash.

Host compatibility and audio/graphics tests are separate from packaging. They were intentionally skipped for this work. Before a public launch, settle the project's/JUCE's distribution license and validate a signed, notarized release in supported hosts on a clean Mac.
