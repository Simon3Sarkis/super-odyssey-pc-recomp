#!/bin/bash
# macOS release disk image: build/dist/Super Odyssey PC Recomp-<release tag>-macOS.dmg holding "Super Odyssey PC Recomp.app" next to an
# Applications shortcut (drag the app onto it to install - how Mac users expect to install). Run after `make app`.
set -e
cd "$(dirname "$0")/.."
APP="build/Super Odyssey PC Recomp.app"
[ -d "$APP" ] || { echo "no $APP - run make app first" >&2; exit 1; }
VERSION=$(sed -n 's/.*"ProductVersion", "\(.*\)"/\1/p' runtime/platform/windows.rc)
OUT="build/dist/Super Odyssey PC Recomp-$VERSION-macOS.dmg"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
ditto "$APP" "$STAGE/Super Odyssey PC Recomp.app"
ln -s /Applications "$STAGE/Applications"
mkdir -p build/dist
rm -f "$OUT"
hdiutil create -volname "Super Odyssey PC Recomp $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 \
    -ov "$OUT" >/dev/null
echo "$OUT ($(du -h "$OUT" | cut -f1))"
