#!/bin/bash
# Builds SamovStreamer-Installer-macOS.pkg
#   build-pkg.sh <path to "Samov Streamer.vst3"> <version> <output dir>
set -euo pipefail
BUNDLE="$1"; VERSION="$2"; OUT="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"

mkdir -p "$WORK/root/Library/Audio/Plug-Ins/VST3" "$OUT"
cp -R "$BUNDLE" "$WORK/root/Library/Audio/Plug-Ins/VST3/"

# Never let Installer "relocate" the bundle to wherever an old copy with the same id lives.
pkgbuild --analyze --root "$WORK/root" "$WORK/component.plist"
PB=/usr/libexec/PlistBuddy
i=0
while $PB -c "Print :$i" "$WORK/component.plist" >/dev/null 2>&1; do
  $PB -c "Set :$i:BundleIsRelocatable false" "$WORK/component.plist" 2>/dev/null \
    || $PB -c "Add :$i:BundleIsRelocatable bool false" "$WORK/component.plist"
  i=$((i + 1))
done
echo "component plist ($i entries):"; cat "$WORK/component.plist"

pkgbuild --root "$WORK/root" \
         --component-plist "$WORK/component.plist" \
         --scripts "$HERE/scripts" \
         --identifier com.samopospisil.samovstreamer.vst3 \
         --version "$VERSION" \
         --install-location / \
         "$WORK/component.pkg"

cat > "$WORK/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>Samov Streamer ${VERSION}</title>
  <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <domains enable_localSystem="true"/>
  <pkg-ref id="com.samopospisil.samovstreamer.vst3"/>
  <choices-outline><line choice="default"><line choice="vst3"/></line></choices-outline>
  <choice id="default"/>
  <choice id="vst3" visible="false"><pkg-ref id="com.samopospisil.samovstreamer.vst3"/></choice>
  <pkg-ref id="com.samopospisil.samovstreamer.vst3" version="${VERSION}" onConclusion="none">component.pkg</pkg-ref>
</installer-gui-script>
XML

productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" \
             "$OUT/SamovStreamer-Installer-macOS.pkg"
pkgutil --payload-files "$OUT/SamovStreamer-Installer-macOS.pkg" | head -20
rm -rf "$WORK"
