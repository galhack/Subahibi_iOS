#!/bin/sh
# Generate app/Subahibi.xcodeproj from app/project.yml with XcodeGen.
#   SUBAHIBI_TEAM=<your team id> tools/apple/gen_project.sh
# XcodeGen comes from build/tools (downloaded on first use) or the PATH.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
XG="$(command -v xcodegen || true)"
if [ -z "$XG" ]; then
	XG="$ROOT/build/tools/xcodegen/bin/xcodegen"
	if [ ! -x "$XG" ]; then
		mkdir -p "$ROOT/build/tools"
		curl -sL -o "$ROOT/build/tools/xcodegen.zip" https://github.com/yonaskolb/XcodeGen/releases/download/2.46.0/xcodegen.zip
		unzip -q -o "$ROOT/build/tools/xcodegen.zip" -d "$ROOT/build/tools"
	fi
fi
export SUBAHIBI_TEAM="${SUBAHIBI_TEAM:-}"
"$XG" generate --spec "$ROOT/app/project.yml" --project "$ROOT/app"
