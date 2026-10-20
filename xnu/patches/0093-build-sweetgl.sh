#!/bin/bash
# Not a kernel change: dxb runs patches/*.sh on the macOS runner (Xcode is
# set up by then), so this also builds the SweetGL GLEngine (iOS arm64
# userland dylib, sweetgl/) and drops it into the uploaded xnu-kernel
# artifact. A failure here never stops the kernel build.
[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
ws=${GITHUB_WORKSPACE:-}
[[ -n "$ws" && -f "$ws/sweetgl/build.sh" ]] || exit 0
if bash "$ws/sweetgl/build.sh"; then
    mkdir -p "$ws/out"
    cp "$ws/sweetgl/out/GLEngine" "$ws/out/GLEngine"
    cp "$ws/sweetgl/out/SweetGL" "$ws/out/SweetGL" || true
    (cd "$ws/out" && shasum -a 256 GLEngine SweetGL > GLEngine.sha256)
    echo "SweetGL GLEngine built"
else
    echo "SweetGL GLEngine build FAILED (kernel build continues)" >&2
fi
exit 0
