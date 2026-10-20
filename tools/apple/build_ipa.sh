#!/bin/bash
# Build an unsigned .ipa for Sideloadly / AltStore, which sign it with your
# own Apple ID when they install it.
#
#   tools/apple/build_ipa.sh [ios|tvos|visionos]     (default ios)
#
# Includes only the attributed community afterstory delta; players supply the full game and base patch.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PLATFORM="${1:-ios}"
case "$PLATFORM" in
	ios) DEST="generic/platform=iOS"; PRODUCTS="Release-iphoneos" ;;
	tvos) DEST="generic/platform=tvOS"; PRODUCTS="Release-appletvos" ;;
	visionos) DEST="generic/platform=visionOS"; PRODUCTS="Release-xros" ;;
	*) echo "usage: $0 [ios|tvos|visionos]" >&2; exit 2 ;;
esac
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
"$ROOT/tools/apple/gen_project.sh" > /dev/null
xcodebuild -project "$ROOT/app/Subahibi.xcodeproj" -scheme Subahibi -configuration Release \
	-destination "$DEST" -derivedDataPath "$ROOT/build/dd-ipa" \
	CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= build | grep -E "error:|BUILD"
STAGE="$ROOT/build/ipa-$PLATFORM"
rm -rf "$STAGE"
mkdir -p "$STAGE/Payload"
cp -R "$ROOT/build/dd-ipa/Build/Products/$PRODUCTS/Subahibi.app" "$STAGE/Payload/"
(cd "$STAGE" && rm -f "$ROOT/build/Subahibi-$PLATFORM.ipa" && zip -qry "$ROOT/build/Subahibi-$PLATFORM.ipa" Payload)
echo "wrote build/Subahibi-$PLATFORM.ipa"
