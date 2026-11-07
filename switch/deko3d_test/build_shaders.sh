#!/bin/sh
# Compila los shaders con la misma cadena que usaria el backend deko3d de RT64:
#   HLSL -> dxc -> SPIR-V -> spirv-cross -> GLSL -> uam -> DKSH
# Variables: DXC (ruta a dxc-linux), SPIRV_CROSS (por defecto spirv-cross), UAM (por defecto uam)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
: "${DXC:?Define DXC con la ruta a dxc}"
SPIRV_CROSS=${SPIRV_CROSS:-spirv-cross}
UAM=${UAM:-uam}

# Opciones SPIR-V iguales a las de RT64 (DXC_SPV_OPTS)
DXC_SPV_OPTS="-spirv -fspv-target-env=vulkan1.0 -fvk-use-dx-layout"

OUT_ROMFS="$HERE/romfs/shaders"
OUT_TMP="$HERE/build/shaders"
mkdir -p "$OUT_ROMFS" "$OUT_TMP"

# nombre  perfil  entrada  etapa-uam
build() {
    name=$1 profile=$2 entry=$3 stage=$4
    echo "== $name ($stage)"
    "$DXC" $DXC_SPV_OPTS -T "$profile" -E "$entry" "$HERE/shaders/$name.hlsl" -Fo "$OUT_TMP/$name.spv"
    # GLSL de escritorio (no Vulkan): uam exige bindings explicitos y no acepta descriptor sets
    "$SPIRV_CROSS" --version 460 --no-es "$OUT_TMP/$name.spv" --output "$OUT_TMP/$name.glsl"
    cat "$OUT_TMP/$name.glsl"
    "$UAM" -s "$stage" -o "$OUT_ROMFS/$name.dksh" "$OUT_TMP/$name.glsl"
}

build test_cs       cs_6_0 CSMain comp
build fullscreen_vs vs_6_0 VSMain vert
build color_ps      ps_6_0 PSMain frag

ls -l "$OUT_ROMFS"
