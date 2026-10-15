#!/bin/sh
# Build the engine's headless host build (the WebAssembly back end as a
# native binary) and the bpasm / bgiscn tools on macOS.
set -e
cd "$(dirname "$0")/../../engine/openbgi"
make PLATFORM=wasmsim CC=clang VORBIS=stb LDLIBS="-lm -liconv" -j"$(sysctl -n hw.ncpu)" \
	bin/openbgi-wasmsim tools "$@" 2>&1 | grep -E "error:|warning:" || true
test -x bin/openbgi-wasmsim
