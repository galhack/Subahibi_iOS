#!/bin/bash
# Build the Mac application (Mac Catalyst), signed to run locally, and zip it.
#
#   tools/apple/build_mac.sh
#
# The result is build/Subahibi-mac.zip with Subahibi.app inside. It includes
# the attributed community afterstory delta; players supply the full game
# and base patch. The app is ad-hoc signed: on another Mac, open it the first
# time with right-click > Open (or allow it in System Settings > Privacy &
# Security), since it is not notarised.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
"$ROOT/tools/apple/gen_project.sh" > /dev/null
xcodebuild -project "$ROOT/app/Subahibi.xcodeproj" -scheme Subahibi -configuration Release \
	-destination 'generic/platform=macOS,variant=Mac Catalyst' -derivedDataPath "$ROOT/build/dd-mac" build |
	grep -E "error:|BUILD"
APP="$ROOT/build/dd-mac/Build/Products/Release-maccatalyst/Subahibi.app"
rm -f "$ROOT/build/Subahibi-mac.zip"
ditto --norsrc -c -k --keepParent "$APP" "$ROOT/build/Subahibi-mac.zip"
echo "wrote build/Subahibi-mac.zip ($APP)"
