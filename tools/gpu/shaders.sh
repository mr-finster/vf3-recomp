#!/bin/bash
# The GPU's 3D shaders for SDL_GPU (src/gpu/r3d.hlsl), compiled with Microsoft's DXC into SPIR-V (Vulkan: Linux,
# the Steam Deck, Windows without Direct3D 12) and DXIL (Direct3D 12: Windows), and from the SPIR-V into MSL
# (Metal: Macs) with SPIRV-Cross, into src/gpu/r3d_shaders.h, which is committed: the builds don't need the tools.
# Run after changing r3d.hlsl.
#   DXC=/path/to/dxc SPIRV_CROSS=/path/to/spirv-cross tools/gpu/shaders.sh
# (DXC's Linux release has libdxil.so beside it, which signs the DXIL as Direct3D 12 wants it.)
set -e
cd "$(dirname "$0")/../.."
OUT=build/shaders
mkdir -p $OUT
DXC=${DXC:-dxc}
SPIRV_CROSS=${SPIRV_CROSS:-spirv-cross}
NAMES=""
for s in vs_poly fs_opq fs_tr vs_quad fs_fog fs_out fs_colour; do
	case $s in
		vs_poly) D="-D VS_POLY"; P=vs_6_0;;
		fs_opq) D="-D FS_POLY -D FS_LAYER=0"; P=ps_6_0;;
		fs_tr) D="-D FS_POLY -D FS_LAYER=1"; P=ps_6_0;;
		vs_quad) D="-D VS_QUAD"; P=vs_6_0;;
		fs_fog) D="-D FS_FOG"; P=ps_6_0;;
		fs_out) D="-D FS_OUT"; P=ps_6_0;;
		fs_colour) D="-D FS_COLOUR"; P=ps_6_0;;
	esac
	$DXC -T $P -E main -HV 2021 -O3 $D -spirv -fspv-target-env=vulkan1.0 -fvk-use-dx-position-w -Fo $OUT/$s.spv src/gpu/r3d.hlsl
	$DXC -T $P -E main -HV 2021 -O3 $D -Fo $OUT/$s.dxil src/gpu/r3d.hlsl
	$SPIRV_CROSS --msl --msl-version 20100 --msl-decoration-binding --rename-entry-point main main0 \
		$( [ ${s:0:2} = vs ] && echo vert || echo frag ) $OUT/$s.spv --output $OUT/$s.msl
	NAMES="$NAMES $s"
done
python3 - "$OUT" $NAMES > src/gpu/r3d_shaders.h <<'PY'
import sys, hashlib
out, names = sys.argv[1], sys.argv[2:]
print("// Made by tools/gpu/shaders.sh from src/gpu/r3d.hlsl (DXC, SPIRV-Cross): don't edit.")
print("#pragma once")
print("#include <cstddef>")
print("#include <cstdint>")
for k in names:
    for ext in ("spv", "dxil", "msl"):
        data = open(f"{out}/{k}.{ext}", "rb").read()
        if ext == "msl": data += b"\0"
        print(f"alignas(4) static const uint8_t r3d_{k}_{ext}[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            print("\t" + ",".join(str(b) for b in data[i:i + 24]) + ",")
        print("};")
src = open("src/gpu/r3d.hlsl", "rb").read()
print(f'static const char r3d_shaders_source_sha1[] = "{hashlib.sha1(src).hexdigest()}";')
PY
ls -la src/gpu/r3d_shaders.h
