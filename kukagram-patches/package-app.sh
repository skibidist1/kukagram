#!/bin/bash
set -euo pipefail
umask 077

repo="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${KUKAGRAM_BUILD_DIR:-$HOME/Developer/kukagram-build}"
brew_prefix="${KUKAGRAM_BREW_PREFIX:-/Volumes/KG}"
input="${1:-$build_dir/out-release/kukagram.app}"
output_dir="${2:-$build_dir/packages.noindex}"
entitlements="$repo/Telegram/Telegram/Telegram.entitlements"

if [[ ! -x "$input/Contents/MacOS/kukagram" ]]; then
	printf 'Missing built Kukagram executable in %s\n' "$input" >&2
	exit 1
fi
if [[ ! -x "$brew_prefix/bin/macdeployqt" ]]; then
	printf 'Missing macdeployqt in %s/bin\n' "$brew_prefix" >&2
	exit 1
fi
if [[ "$(/usr/bin/lipo -archs "$input/Contents/MacOS/kukagram")" != arm64 ]]; then
	printf 'Expected an arm64 Kukagram executable\n' >&2
	exit 1
fi
for resource in Icon.icns Telegram.rcc lib_ui.rcc lib_spellcheck.rcc external_microtex_bundled.rcc; do
	if [[ ! -s "$input/Contents/Resources/$resource" ]]; then
		printf 'Missing build resource: %s\n' "$resource" >&2
		exit 1
	fi
done

mkdir -p "$output_dir"
stage="$(mktemp -d "$output_dir/kukagram-release.XXXXXXXX")"
app="$stage/kukagram.app"
/usr/bin/ditto --norsrc --noextattr "$input/Contents" "$app/Contents"
mkdir -p "$app/Contents/Frameworks" "$app/Contents/Resources/Licenses/Telegram-Desktop"
/bin/cp "$repo/LEGAL" "$repo/LICENSE" "$app/Contents/Resources/Licenses/Telegram-Desktop/"
/usr/bin/xcrun strip -S -x "$app/Contents/MacOS/kukagram"

/usr/bin/xcrun swift-stdlib-tool --copy --platform macosx \
	--scan-executable "$app/Contents/MacOS/kukagram" \
	--destination "$app/Contents/Frameworks"

export PATH="$brew_prefix/bin:/usr/bin:/bin:/usr/sbin:/sbin"
"$brew_prefix/bin/macdeployqt" "$app" -no-strip -no-codesign -verbose=1 \
	-libpath="$brew_prefix/lib" \
	-libpath="$brew_prefix/opt/openssl@3/lib" \
	-libpath="$brew_prefix/opt/ffmpeg@6/lib" \
	-libpath="$brew_prefix/opt/openal-soft/lib"

/usr/bin/python3 "$repo/kukagram-patches/finalize-mac-bundle.py" "$app" "$entitlements" "$brew_prefix"
/usr/bin/codesign --verify --deep --strict "$app"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$app" "$stage/kukagram-arm64.zip"
/usr/bin/unzip -tq "$stage/kukagram-arm64.zip"
printf 'Signed app: %s\nArchive: %s\n' "$app" "$stage/kukagram-arm64.zip"
