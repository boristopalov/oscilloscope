#!/bin/bash
# Build and package only. This script never installs the plugin or publishes it.
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'USAGE'
Usage: scripts/package-macos.sh [options]
  --mode development|release  Default: development (ad-hoc, not notarized)
  --build-dir DIR             Default: build/macos-MODE
  --output-dir DIR            Default: dist/MODE
  --juce-path DIR             Optional existing JUCE source tree
  --parallel N               Build jobs (default: 4)
  --help

Release mode requires these environment variables:
  OSCILLOSCOPE_APPLICATION_SIGNING_IDENTITY  Developer ID Application identity
  OSCILLOSCOPE_INSTALLER_SIGNING_IDENTITY    Developer ID Installer identity
  OSCILLOSCOPE_NOTARY_KEYCHAIN_PROFILE       Stored notarytool credential profile
Optional: OSCILLOSCOPE_SIGNING_KEYCHAIN, OSCILLOSCOPE_NOTARY_KEYCHAIN.
USAGE
}

die() { printf 'Error: %s\n' "$*" >&2; exit 1; }
mode=development
build_dir=
output_dir=
juce_path=
parallel=4
while [ "$#" -gt 0 ]; do
    case "$1" in
        --help) usage; exit 0 ;;
        --mode|--build-dir|--output-dir|--juce-path|--parallel)
            [ "$#" -ge 2 ] || die "$1 requires a value"
            case "$1" in
                --mode) mode=$2 ;;
                --build-dir) build_dir=$2 ;;
                --output-dir) output_dir=$2 ;;
                --juce-path) juce_path=$2 ;;
                --parallel) parallel=$2 ;;
            esac
            shift 2 ;;
        *) die "Unknown argument: $1" ;;
    esac
done
case "$mode" in development|release) ;; *) die "Mode must be development or release" ;; esac
case "$parallel" in ''|*[!0-9]*|0) die "--parallel must be a positive integer" ;; esac
[ "$(uname -s)" = Darwin ] || die "macOS is required"
for tool in cmake ninja python3 codesign pkgbuild productbuild pkgutil ditto shasum lipo; do
    command -v "$tool" >/dev/null || die "Missing required tool: $tool"
done

script_dir=$(cd "$(dirname "$0")" && pwd)
project_dir=$(cd "$script_dir/.." && pwd)
build_dir=${build_dir:-"$project_dir/build/macos-$mode"}
output_dir=${output_dir:-"$project_dir/dist/$mode"}

# Validate all release inputs before building or producing an artifact. Never
# substitute an ad-hoc signature for missing production credentials.
if [ "$mode" = release ]; then
    [ -n "${OSCILLOSCOPE_APPLICATION_SIGNING_IDENTITY:-}" ] || die "Set OSCILLOSCOPE_APPLICATION_SIGNING_IDENTITY"
    [ -n "${OSCILLOSCOPE_INSTALLER_SIGNING_IDENTITY:-}" ] || die "Set OSCILLOSCOPE_INSTALLER_SIGNING_IDENTITY"
    [ -n "${OSCILLOSCOPE_NOTARY_KEYCHAIN_PROFILE:-}" ] || die "Set OSCILLOSCOPE_NOTARY_KEYCHAIN_PROFILE"
    [ "$OSCILLOSCOPE_APPLICATION_SIGNING_IDENTITY" != - ] || die "Release mode requires a Developer ID Application certificate"
    command -v xcrun >/dev/null || die "Xcode command-line tools are required for notarization"
    xcrun --find notarytool >/dev/null
    xcrun --find stapler >/dev/null
fi

mkdir -p "$build_dir" "$output_dir"
build_dir=$(cd "$build_dir" && pwd)
output_dir=$(cd "$output_dir" && pwd)
cmake_args=(-S "$project_dir" -B "$build_dir" -G Ninja
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
    -DBUILD_TESTING=OFF -DOSCILLOSCOPE_DEV_PROBES=OFF)
if [ -n "$juce_path" ]; then
    juce_path=$(cd "$juce_path" && pwd)
    cmake_args+=("-DOSCILLOSCOPE_JUCE_PATH=$juce_path")
fi
cmake "${cmake_args[@]}"
cmake --build "$build_dir" --target Oscilloscope_VST3 --parallel "$parallel"

# Use one version/identity source for CMake, signing and package receipts. Reject
# mismatched or stale build products before they can become release artifacts.
version=$(python3 - "$project_dir/VERSION" <<'PY'
import pathlib, re, sys
version = pathlib.Path(sys.argv[1]).read_text().strip()
if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version):
    raise SystemExit('VERSION must contain major.minor.patch without leading zeroes')
print(version)
PY
)
identity_path="$project_dir/packaging/plugin-identity.json"
product_name=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["product_name"])' "$identity_path")
installer_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["installer_identifier"])' "$identity_path")
source_bundle="$build_dir/Oscilloscope_artefacts/Release/VST3/$product_name.vst3"
[ -d "$source_bundle" ] || die "Built VST3 bundle is missing"
python3 - "$source_bundle/Contents/Info.plist" "$identity_path" "$version" <<'PY'
import json, plistlib, sys
with open(sys.argv[1], 'rb') as source:
    info = plistlib.load(source)
with open(sys.argv[2]) as source:
    identity = json.load(source)
expected = {'CFBundleIdentifier': identity['bundle_identifier'],
            'CFBundleShortVersionString': sys.argv[3], 'CFBundleVersion': sys.argv[3]}
for key, value in expected.items():
    if info.get(key) != value:
        raise SystemExit(f'Built plugin metadata mismatch: {key}')
PY
binary="$source_bundle/Contents/MacOS/$product_name"
[ "$(lipo -archs "$binary")" = arm64 ] || die "Package must contain only an arm64 plugin"

work_dir=$(mktemp -d "$build_dir/package.XXXXXX")
cleanup() { rm -rf "$work_dir"; }
trap cleanup EXIT
payload="$work_dir/payload"
bundle_relative="Library/Audio/Plug-Ins/VST3/$product_name.vst3"
bundle="$payload/$bundle_relative"
mkdir -p "$(dirname "$bundle")"
# Copy data and file modes only. ditto's ownership/metadata restoration can be
# denied in managed workspaces, even when resource/extended attributes are off.
cp -R -X "$source_bundle" "$bundle"

codesign_args=(--force --sign -)
installer_sign_args=()
notary_args=()
if [ "$mode" = release ]; then
    codesign_args=(--force --sign "$OSCILLOSCOPE_APPLICATION_SIGNING_IDENTITY" --options runtime --timestamp)
    installer_sign_args=(--sign "$OSCILLOSCOPE_INSTALLER_SIGNING_IDENTITY" --timestamp)
    notary_args=(--keychain-profile "$OSCILLOSCOPE_NOTARY_KEYCHAIN_PROFILE")
    if [ -n "${OSCILLOSCOPE_SIGNING_KEYCHAIN:-}" ]; then
        codesign_args+=(--keychain "$OSCILLOSCOPE_SIGNING_KEYCHAIN")
        installer_sign_args+=(--keychain "$OSCILLOSCOPE_SIGNING_KEYCHAIN")
    fi
    if [ -n "${OSCILLOSCOPE_NOTARY_KEYCHAIN:-}" ]; then
        notary_args+=(--keychain "$OSCILLOSCOPE_NOTARY_KEYCHAIN")
    fi
fi
# The VST3 contains one Mach-O executable and no nested executable bundles.
# Sign the complete bundle after JUCE's module manifest has been generated.
codesign "${codesign_args[@]}" "$bundle"
codesign --verify --deep --strict "$bundle"
if [ "$mode" = release ]; then
    # Check the certificate class without printing its identity into the log.
    codesign -d --verbose=4 "$bundle" 2>&1 | python3 -c '
import sys
text = sys.stdin.read()
if "Authority=Developer ID Application:" not in text or "flags=" not in text or "runtime" not in text:
    raise SystemExit("Release plugin must use Developer ID Application signing with hardened runtime")
if "Timestamp=" not in text:
    raise SystemExit("Release plugin must include a trusted timestamp")
'
fi

# Fixed location and atomic replacement prevent Installer from finding a moved
# development copy or leaving obsolete files inside an upgraded bundle.
python3 - "$work_dir/components.plist" "$bundle_relative" <<'PY'
import plistlib, sys
components = [{'RootRelativeBundlePath': sys.argv[2], 'BundleIsRelocatable': False,
               'BundleIsVersionChecked': True, 'BundleHasStrictIdentifier': True,
               'BundleOverwriteAction': 'upgrade'}]
with open(sys.argv[1], 'wb') as output:
    plistlib.dump(components, output)
PY
pkgbuild --quiet --root "$payload" --component-plist "$work_dir/components.plist" \
    --identifier "$installer_id" --version "$version" \
    --install-location / --ownership recommended "$work_dir/component.pkg"
(
    cd "$work_dir"
    productbuild --quiet --synthesize --product "$project_dir/packaging/macos/requirements.plist" \
        --package component.pkg distribution.xml
)
python3 - "$work_dir/distribution.xml" "$product_name" <<'PY'
import sys, xml.etree.ElementTree as ET
tree = ET.parse(sys.argv[1])
root = tree.getroot()
title = ET.Element('title')
title.text = sys.argv[2]
root.insert(0, title)
domains = ET.SubElement(root, 'domains')
domains.set('enable_anywhere', 'false')
domains.set('enable_currentUserHome', 'false')
domains.set('enable_localSystem', 'true')
tree.write(sys.argv[1], encoding='utf-8', xml_declaration=True)
PY
suffix=
[ "$mode" != development ] || suffix=-development
artifact_name="$product_name-$version-macOS-arm64$suffix"
archive_dir="$work_dir/$artifact_name"
mkdir -p "$archive_dir"
package="$archive_dir/$artifact_name.pkg"
productbuild_args=(--quiet --distribution "$work_dir/distribution.xml" --package-path "$work_dir"
    --identifier "$installer_id" --version "$version")
# macOS ships Bash 3.2, where an empty-array expansion trips `set -u`.
if [ "$mode" = release ]; then
    productbuild_args+=("${installer_sign_args[@]}")
fi
productbuild "${productbuild_args[@]}" "$package"

if [ "$mode" = release ]; then
    pkgutil --check-signature "$package" | python3 -c '
import sys
if "Developer ID Installer:" not in sys.stdin.read():
    raise SystemExit("Release installer must use a Developer ID Installer certificate")
'
    # Apple receives the exact signed package we ship. ZIP contains this stapled
    # installer rather than an unstapleable raw plugin or a second binary build.
    xcrun notarytool submit "$package" "${notary_args[@]}" --wait --output-format json > "$work_dir/notary-result.json"
    python3 - "$work_dir/notary-result.json" <<'PY'
import json, sys
with open(sys.argv[1]) as source:
    result = json.load(source)
if result.get('status') != 'Accepted':
    raise SystemExit('Apple notarization did not accept the installer; inspect the notarytool submission history/log')
PY
    xcrun stapler staple "$package"
    xcrun stapler validate "$package"
    spctl --assess --type install "$package"
fi

cat > "$archive_dir/INSTALL.txt" <<EOF
$product_name $version — macOS 12 or later, Apple Silicon (arm64), VST3

1. Quit your audio host before installing.
2. Open $artifact_name.pkg and follow Installer. Administrator access is required.
3. Restart your host, enable the system VST3 folder and rescan plugins if needed.

The installer writes /Library/Audio/Plug-Ins/VST3/$product_name.vst3.
It replaces earlier versions at that location. If you previously installed a
copy in your user Library, move that copy out of the host's scan folders first
to avoid duplicate plugin versions. Keep a backup if you may need to roll back.

To uninstall, quit your host and move that system plugin bundle to the Trash.
The plugin does not change audio gain; the display clip threshold is visual only.
EOF
if [ "$mode" = development ]; then
    cat >> "$archive_dir/INSTALL.txt" <<'EOF'

DEVELOPMENT BUILD: The plugin is ad-hoc signed. The installer is unsigned and
has not been notarized by Apple. This artifact is for local development, not a
public release. Gatekeeper may block it after download or transfer.
EOF
else
    printf '\nThe plugin and installer are Developer ID signed; the installer is notarized and stapled.\n' >> "$archive_dir/INSTALL.txt"
fi
(
    cd "$archive_dir"
    shasum -a 256 "$artifact_name.pkg" > SHA256SUMS
)
zip_path="$work_dir/$artifact_name.zip"
ditto -c -k --norsrc --noextattr --noqtn --noacl --keepParent "$archive_dir" "$zip_path"
# Publish to the local artifact directory only after all required signing and
# notarization steps succeed. Raw credentials and notary responses stay private.
cp "$package" "$output_dir/$artifact_name.pkg"
cp "$zip_path" "$output_dir/$artifact_name.zip"
(
    cd "$output_dir"
    shasum -a 256 "$artifact_name.pkg" "$artifact_name.zip" > SHA256SUMS
)
printf 'Packaged %s (%s): %s\n' "$version" "$mode" "$output_dir"
