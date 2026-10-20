#!/bin/bash
# Build the SweetGL GLEngine for iOS 16 arm64 (macOS + Xcode).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
python3 gen.py gl-dispatch-slots.txt out/stubs.c
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 \
	-O2 -Wall -Wno-unused-parameter -fvisibility=hidden -dynamiclib \
	-install_name /System/Library/Frameworks/OpenGLES.framework/GLEngine.bundle/SweetGL \
	-o out/GLEngine glengine.c encoder.c out/stubs.c -framework IOKit -framework IOSurface -framework CoreFoundation
codesign -f -s - out/GLEngine
nm -gU out/GLEngine | grep " _gli" | wc -l
shasum -a 256 out/GLEngine > out/SHA256SUMS
