#!/bin/bash
# Build the SweetGL GLEngine for iOS 16 arm64 (macOS + Xcode).
#   out/GLEngine  the engine (glengine.c + encoder.c), installed as GLEngine.bundle/SweetGLEngine
#                 or sent by the host viewer into the guest's engine blob
#   out/SweetGL   the loader EAGL opens (loader.c): trampolines into the engine
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
python3 gen.py gl-dispatch-slots.txt out/stubs.c
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 \
	-O2 -Wall -Wno-unused-parameter -fvisibility=hidden -dynamiclib \
	-install_name /System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle/SweetGLEngine \
	-o out/GLEngine glengine.c encoder.c out/stubs.c -framework IOKit -framework IOSurface -framework CoreFoundation
codesign -f -s - out/GLEngine
nm -gU out/GLEngine | grep " _gli" | wc -l
xcrun --sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 \
	-O2 -Wall -fvisibility=hidden -dynamiclib \
	-install_name /System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle/SweetGL \
	-o out/SweetGL loader.c -framework IOKit -framework CoreFoundation
codesign -f -s - out/SweetGL
nm -gU out/SweetGL | grep -c " _gli"
shasum -a 256 out/GLEngine out/SweetGL > out/SHA256SUMS
