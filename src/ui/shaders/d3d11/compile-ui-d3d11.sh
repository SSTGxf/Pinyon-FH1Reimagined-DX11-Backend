#!/usr/bin/env bash
# Compiles the UI guest-output shaders for the d3d11 backend into
# ../bytecode/d3d11_5_0 headers (SM 5.0, feature level 11_0), following the
# same conventions as src/graphics/shaders/compile-fh1-native.sh: fxc /Fh
# headers are checked in, and the FXC environment variable can override the
# compiler path.
set -euo pipefail
cd "$(dirname "$0")"
FXC="${FXC:-/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe}"
mkdir -p ../bytecode/d3d11_5_0
compile() {  # profile entry name [defines...]
  local profile=$1 entry=$2 name=$3
  shift 3
  local args=()
  local define
  for define in "$@"; do args+=(//D "$define"); done
  "$FXC" //nologo //T "$profile" //E "$entry" //O3 "${args[@]}" //Vn "$name" \
    //Fh "../bytecode/d3d11_5_0/$name.h" guest_output.hlsl > /dev/null
}
compile vs_5_0 guest_output_triangle_strip_rect_vs guest_output_triangle_strip_rect_vs GUEST_OUTPUT_VS=1
compile ps_5_0 guest_output_bilinear_ps guest_output_bilinear_ps
compile ps_5_0 guest_output_bilinear_dither_ps guest_output_bilinear_dither_ps
echo "d3d11 UI shaders compiled into ../bytecode/d3d11_5_0/"
