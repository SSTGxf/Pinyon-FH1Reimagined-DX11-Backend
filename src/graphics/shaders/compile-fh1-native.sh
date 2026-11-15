#!/usr/bin/env bash
# Compiles the FH1 native executor and texture cache shaders into
# bytecode/d3d12_5_1 headers and, from the same HLSL, into vulkan_spirv
# SPIR-V headers, and (third pass) into bytecode/d3d11_5_0 headers for the
# d3d11 backend's immediate context (SM 5.0, feature level 11_0).
#
# Third pass: the d3d11 backend's immediate context is feature level 11_0,
# so every host shader of the d3d12 set is compiled a second time at the
# matching *_5_0 profile into bytecode/d3d11_5_0. It compiles the same
# sources with the same defines and only adds files, so the d3d12_5_1 and
# vulkan_spirv outputs stay byte for byte the same. The shaders the guest
# brings (the xe_* library) are not part of it: their SM 5.0 retarget
# belongs to the guest shader translator, not to this build.
#
# Feature level 11_0 limits typed unordered access view *loads* to R32_FLOAT,
# R32_UINT and R32_SINT (the wider format list needs D3D11.3 or D3D12); a
# 128 bit typed UAV *store* is required by the feature level 11_0 format
# table, so the variants that only store are bindable as they are. The
# d3d11 backend writes the guest memory mirror through a raw byte address
# buffer, and only a dcl_uav_raw shader can write a raw UAV, so the three
# texture cache shaders are compiled a second time with FH1_WORD_DEST, which
# makes them write through a raw byte address buffer instead of a typed UAV.
# The variants and their word based forms are listed in VARIANTS.md next to
# this script.
#
# SPIR-V comes from the vendored glslang's HLSL front end, so no DXC with
# SPIR-V code generation is needed. Build glslangValidator once from
# thirdparty/glslang with ENABLE_HLSL and ENABLE_GLSLANG_BINARIES and point
# GLSLANG at it; without GLSLANG only the DXBC headers are written.
#
# Vulkan: the constant buffers are push constants (FH1_SPIRV, see
# fh1_push_constants.hlsli), and resources are in descriptor set 0 by register
# class whatever their type (--hlsl-iomap): t<n> is binding 16 + n and u<n> is
# binding 32 + n.
set -euo pipefail
cd "$(dirname "$0")"
FXC="${FXC:-/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe}"
GLSLANG="${GLSLANG:-}"
# fxc writes its headers with CRLF line endings; the repository keeps text
# files in LF (.gitattributes: * text=auto eol=lf), so a header is normalized
# after it is written. Without it a fresh build differs from the checked-in
# headers byte for byte, and the byte-compare of the d3d12_5_1 pass against
# the pre-existing set fails on the line endings alone.
to_lf() {  # header
  local header=$1
  local tmp="$header.lf"
  tr -d '\r' < "$header" > "$tmp" && mv "$tmp" "$header"
}
compile_spirv() {  # profile file name defines...
  local profile=$1 file=$2 name=$3
  shift 3
  [[ -n $GLSLANG ]] || return 0
  local args=()
  local define
  for define in "$@"; do args+=("-D$define"); done
  local stage=frag
  [[ $profile == cs_* ]] && stage=comp
  "$GLSLANG" -D -V -S "$stage" -e main -DFH1_SPIRV=1 "${args[@]}" --hlsl-iomap \
    --shift-texture-binding 16 --shift-UAV-binding 32 \
    --vn "$name" -o "vulkan_spirv/$name.h" "$file" > /dev/null
}
d3d11_count=0
compile_d3d11() {  # profile file name defines... (SM 5.0 third pass)
  local profile=$1 file=$2 name=$3
  shift 3
  # The d3d11 backend is feature level 11_0: the same sources at *_5_0.
  [[ $profile == *_5_1 ]] || return 0
  local profile_5_0="${profile%5_1}5_0"
  local args=()
  local define
  for define in "$@"; do args+=(//D "$define"); done
  mkdir -p bytecode/d3d11_5_0
  # fxc writes its diagnostics to stdout: keep them for the failure case, so
  # a rejected variant names itself instead of vanishing from the set.
  local log
  if ! log="$("$FXC" //nologo //T "$profile_5_0" //E main //O3 "${args[@]}" \
      //Vn "$name" //Fh "bytecode/d3d11_5_0/$name.h" "$file" 2>&1)"; then
    echo "fxc failed for $name ($profile_5_0, $file):" >&2
    echo "$log" >&2
    return 1
  fi
  to_lf "bytecode/d3d11_5_0/$name.h"
  d3d11_count=$((d3d11_count + 1))
}
compile() {  # profile file name defines...
  local profile=$1 file=$2 name=$3
  shift 3
  local args=()
  local define
  for define in "$@"; do args+=(//D "$define"); done
  "$FXC" //nologo //T "$profile" //E main //O3 "${args[@]}" //Vn "$name" \
    //Fh "bytecode/d3d12_5_1/$name.h" "$file" > /dev/null
  to_lf "bytecode/d3d12_5_1/$name.h"
  compile_spirv "$profile" "$file" "$name" "$@"
  compile_d3d11 "$profile" "$file" "$name" "$@"
}
for source_kind in color depth uint; do
  source_defines=()
  [[ $source_kind == depth ]] && source_defines+=(FH1_SOURCE_DEPTH=1)
  [[ $source_kind == uint ]] && source_defines+=(FH1_SOURCE_UINT=1)
  for source_msaa in "" _ms; do
    msaa_defines=("${source_defines[@]}")
    [[ -n $source_msaa ]] && msaa_defines+=(FH1_SOURCE_MSAA=1)
    compile cs_5_1 fh1_native_resolve_memory.cs.hlsl \
      "fh1_native_resolve_memory_${source_kind}${source_msaa}_cs" "${msaa_defines[@]}"
    compile cs_5_1 fh1_native_resolve_memory.cs.hlsl       "fh1_native_resolve_image_${source_kind}${source_msaa}_cs" "${msaa_defines[@]}"       FH1_DEST_IMAGE=1
    compile cs_5_1 fh1_native_transfer_words.cs.hlsl \
      "fh1_native_transfer_words_${source_kind}${source_msaa}_cs" "${msaa_defines[@]}"
    dest_index=0
    for dest_kind in color depth stencil uint; do
      for dest_msaa in "" _dms; do
        dest_defines=("${msaa_defines[@]}" "FH1_DEST_KIND=$dest_index")
        [[ -n $dest_msaa ]] && dest_defines+=(FH1_DEST_MSAA=1)
        compile ps_5_1 fh1_native_transfer.ps.hlsl \
          "fh1_native_transfer_${dest_kind}${dest_msaa}_from_${source_kind}${source_msaa}_ps" \
          "${dest_defines[@]}"
      done
      dest_index=$((dest_index + 1))
    done
  done
done
# Depth destinations: depth and stencil-bit passes from precomputed words.
kind=1
for dest_kind in depth stencil; do
  for dest_msaa in "" _dms; do
    defines=("FH1_DEST_KIND=$kind")
    [[ -n $dest_msaa ]] && defines+=(FH1_DEST_MSAA=1)
    compile ps_5_1 fh1_native_transfer_from_words.ps.hlsl \
      "fh1_native_transfer_${dest_kind}${dest_msaa}_from_words_ps" "${defines[@]}"
  done
  kind=2
done
# Depth destinations through typed word buffers: the d3d11 backend binds no raw
# view at all (feature level 11_0 / Fermi: raw stores land only at low offsets
# and a sub-range raw view cannot be created), so the same sources are compiled
# with FH1_WORD_TYPED, one R32_UINT element per guest EDRAM word.
for source_kind in color depth uint; do
  source_defines=()
  [[ $source_kind == depth ]] && source_defines+=(FH1_SOURCE_DEPTH=1)
  [[ $source_kind == uint ]] && source_defines+=(FH1_SOURCE_UINT=1)
  for source_msaa in "" _ms; do
    msaa_defines=("${source_defines[@]}" FH1_WORD_TYPED=1)
    [[ -n $source_msaa ]] && msaa_defines+=(FH1_SOURCE_MSAA=1)
    compile_d3d11 cs_5_1 fh1_native_transfer_words.cs.hlsl \
      "fh1_native_transfer_words_${source_kind}${source_msaa}_typed_cs" "${msaa_defines[@]}"
  done
done
kind=1
for dest_kind in depth stencil; do
  for dest_msaa in "" _dms; do
    defines=("FH1_DEST_KIND=$kind" FH1_WORD_TYPED=1)
    [[ -n $dest_msaa ]] && defines+=(FH1_DEST_MSAA=1)
    compile_d3d11 ps_5_1 fh1_native_transfer_from_words.ps.hlsl \
      "fh1_native_transfer_${dest_kind}${dest_msaa}_from_words_typed_ps" "${defines[@]}"
  done
  kind=2
done
# The same words pass batched: one dispatch for every rectangle of one
# destination/source pair. Built for this backend only - the Direct3D 12
# executor passes one rectangle per dispatch (fh1_native_transfer_words.cs).
for source_kind in color depth uint; do
  source_defines=()
  [[ $source_kind == depth ]] && source_defines+=(FH1_SOURCE_DEPTH=1)
  [[ $source_kind == uint ]] && source_defines+=(FH1_SOURCE_UINT=1)
  for source_msaa in "" _ms; do
    msaa_defines=("${source_defines[@]}" FH1_WORD_TYPED=1)
    [[ -n $source_msaa ]] && msaa_defines+=(FH1_SOURCE_MSAA=1)
    compile_d3d11 cs_5_1 fh1_native_transfer_words_batch.cs.hlsl \
      "fh1_native_transfer_words_batch_${source_kind}${source_msaa}_typed_cs" "${msaa_defines[@]}"
  done
done
# Depth readback for the transfer probe (a depth-stencil resource cannot be
# stage-copied on Direct3D 11).
compile_d3d11 ps_5_1 fh1_native_depth_probe.ps.hlsl fh1_native_depth_probe_ps
compile_d3d11 ps_5_1 fh1_native_depth_probe.ps.hlsl fh1_native_depth_probe_ms_ps FH1_PROBE_MSAA=1
# Depth and stencil in one pass straight from the source, for Vulkan devices
# with shader stencil export (FH1_DEST_KIND 4). glslang's HLSL front end has no
# SV_StencilRef (nor has Direct3D 12's shader model 5.1): the shader writes the
# value to an int target at location 7, which spirv_stencil_export.py turns
# into the FragStencilRefEXT built-in.
if [[ -n $GLSLANG ]]; then
  PYTHON="${PYTHON:-python3}"
  spirv_temp="$(mktemp -d)"
  for source_kind in color depth uint; do
    for source_msaa in "" _ms; do
      for dest_msaa in "" _dms; do
        defines=(-DFH1_SPIRV=1 -DFH1_DEST_KIND=4)
        [[ $source_kind == depth ]] && defines+=(-DFH1_SOURCE_DEPTH=1)
        [[ $source_kind == uint ]] && defines+=(-DFH1_SOURCE_UINT=1)
        [[ -n $source_msaa ]] && defines+=(-DFH1_SOURCE_MSAA=1)
        [[ -n $dest_msaa ]] && defines+=(-DFH1_DEST_MSAA=1)
        name="fh1_native_transfer_depth_stencil${dest_msaa}_from_${source_kind}${source_msaa}_ps"
        "$GLSLANG" -D -V -S frag -e main "${defines[@]}" --hlsl-iomap \
          --shift-texture-binding 16 --shift-UAV-binding 32 \
          -o "$spirv_temp/$name.spv" fh1_native_transfer.ps.hlsl > /dev/null
        "$PYTHON" spirv_stencil_export.py "$spirv_temp/$name.spv" "vulkan_spirv/$name.h" "$name"
      done
    done
  done
  rm -r "$spirv_temp"
fi
# The fullscreen triangle vertex shader of the transfer pixel shader path.
# Its d3d12_5_1 header was built with other fxc flags and is kept as it is;
# the d3d11 pass takes the shader from the source checked in beside it.
compile_d3d11 vs_5_1 fullscreen_cw.vs.hlsl fullscreen_cw_vs
compile_d3d11 ps_5_1 fh1_native_clear.ps.hlsl fh1_native_clear_color_ps
compile_d3d11 ps_5_1 fh1_native_clear.ps.hlsl fh1_native_clear_uint_ps FH1_CLEAR_UINT=1
compile_d3d11 ps_5_1 fh1_native_clear.ps.hlsl fh1_native_clear_depth_ps FH1_CLEAR_DEPTH=1
# Texture cache: the scaled 32-bpp resolve buffer and reflection cube imports.
# Their DXBC headers were built with other fxc flags and are kept as they are.
compile_spirv cs_5_1 fh1_scaled_32bpp_2x.cs.hlsl fh1_scaled_32bpp_2x_cs
compile_spirv cs_5_1 fh1_reflection_cube_import.cs.hlsl fh1_reflection_cube_import_cs
# The same two texture cache shaders for the d3d11 backend, from the same
# sources. All three write a 128 bit typed UAV (RWBuffer<uint4> is a typed
# buffer UAV of R32G32B32A32_UINT, not a structured one), which feature level
# 11_0 can store but not load; the d3d11 backend's guest memory mirror is a
# raw byte address buffer, so the form it binds writes through one
# (FH1_WORD_DEST) instead of through the typed UAV.
#
# The typed form of the texture cache must not be built at shader model 5.0:
# fxc's 5.0 backend loses one of the stores of an unrolled loop that stores
# more than once, and the texture form has such a loop, so the 5.0 build
# writes 14 texels of a 16 texel row. The word form is correct at 5.0 (the
# four word stores of a texel become one 128 bit store_raw), and the typed
# form is correct at 5.1, which is the model the d3d12 backend uses. The
# difference reproduces in a shader of fifteen lines; see VARIANTS.md.
compile_d3d11 cs_5_1 fh1_scaled_32bpp_2x.cs.hlsl fh1_scaled_32bpp_2x_cs \
  FH1_WORD_DEST=1
compile_d3d11 cs_5_1 fh1_scaled_32bpp_2x.cs.hlsl fh1_scaled_32bpp_2x_texture_cs \
  FH1_DIRECT_TEXTURE_OUTPUT=1 FH1_WORD_DEST=1
compile_d3d11 cs_5_1 fh1_reflection_cube_import.cs.hlsl fh1_reflection_cube_import_cs \
  FH1_WORD_DEST=1
# Texture cache: buffer-to-image copies by compute (GLSL), one per texel size
# in 32-bit words.
if [[ -n $GLSLANG ]]; then
  for words in 1 2 4; do
    "$GLSLANG" -V -S comp -DWORDS=$words --vn "texture_copy_buffer_image_${words}w_cs"       -o "vulkan_spirv/texture_copy_buffer_image_${words}w_cs.h" texture_copy_buffer_image.comp       > /dev/null
  done
fi
echo "compiled, $d3d11_count d3d11 SM 5.0 shaders into bytecode/d3d11_5_0/"
