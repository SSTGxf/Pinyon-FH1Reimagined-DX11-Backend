# FH1 host shaders at shader model 5.0 — `bytecode/d3d11_5_0`

This is the third pass of `compile-fh1-native.sh`: the same HLSL sources as the
D3D12 pass, compiled a second time at the `*_5_0` profiles, for the D3D11
backend's immediate context (feature level 11_0). It is the document the
main-tree tab wires Step B (EDRAM surfaces + resolve) from.

* Build script: `src/graphics/shaders/compile-fh1-native.sh` (the
  `compile_d3d11` function; the d3d12 and vulkan passes are untouched except
  for the line ending normalisation, see "Regression" below).
* Output: `src/graphics/shaders/bytecode/d3d11_5_0/*.h`, 74 headers, one per
  host shader of the d3d12_5_1 set that has a source in this tree.
* Compiler: `fxc.exe` 10.1 (Windows SDK 10.0.26100.0), flags
  `//nologo //T <profile> //E main //O3 //D <defines> //Vn <name> //Fh <header>`.
* Entry point: `main` for every shader (the same entry point the d3d12 pass
  uses).

## What feature level 11_0 actually restricts

The Direct3D 11 feature level 11_0 format table
(<https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/format-support-for-direct3d-11-0-feature-level-hardware>)
has separate rows for a typed UAV's **store** and its **load**, and they do
not say the same thing for the 128 bit formats:

| format | Buffer (SRV) | Typed UAV | UAV typed store | UAV typed load | raw UAV/SRV |
| --- | --- | --- | --- | --- | --- |
| `R32_FLOAT` / `R32_UINT` / `R32_SINT` | required | required | required | required | — |
| `R32G32B32A32_FLOAT` / `_UINT` / `_SINT` | required | required | required | **optional** | — |
| `R32_TYPELESS` | — | — | — | — | required |

So at feature level 11_0:

* a **typed buffer SRV** of `R32G32B32A32_UINT` is legal (the "Buffer" row is
  required), which is what `Buffer<uint4> source` needs;
* a **128 bit typed UAV store** is legal (the "UAV typed store" row is
  required), so a shader that only writes `RWBuffer<uint4>` or
  `RWTexture2D<uint4>` is bindable;
* a **128 bit typed UAV load** is *not* guaranteed (the "UAV typed load" row
  is optional): it needs the D3D11.3 extension
  `D3D11_FEATURE_D3D11_OPTIONS2::TypedUAVLoadAdditionalFormats`;
* a **raw** (byte address) view is legal, and it is not the null description
  view: the buffer needs
  `D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS`, the UAV needs
  `DXGI_FORMAT_R32_TYPELESS` with `D3D11_BUFFER_UAV_FLAG_RAW` in
  `D3D11_UNORDERED_ACCESS_VIEW_DESC::Buffer`, and the SRV needs
  `D3D11_BUFFEREX_SRV_FLAG_RAW` in `D3D11_BUFFEREX_SRV_DESC::Buffer`.

`fxc` declares *every* typed UAV as four components, whatever the HLSL
element type is: `RWTexture2D<uint>` and `RWTexture2D<uint4>` both come out
as `dcl_uav_typed_texture2d (uint,uint,uint,uint)`, at shader model 5.0 and
5.1 alike. What decides the format is the store's width and the view's
format, not the declaration.

## Verdicts

| verdict | count | meaning |
| --- | --- | --- |
| clean | 65 | compiles at its `_5_0` profile and is bindable on feature level 11_0 with no wide format at all |
| wide typed UAV store | 9 | compiles and is bindable on feature level 11_0 (a 128 bit typed UAV *store* is required by the format table), but the d3d11 backend can bind a word-based form instead, which writes through the raw byte address buffer it already has |
| failed | 0 | — |
| not built here | 108 | guest (`xe_*`) bytecode, whose shader model 5.0 retarget belongs to the guest shader translator, plus three host shaders whose source is not in this tree |

No variant needed a profile guard: every one of them compiles at its `_5_0`
profile with exactly the defines the d3d12 pass uses.

The nine wide-typed-UAV variants are the six `fh1_native_resolve_image_*`
(shaders that also write the resolved words into a texture) and the three
texture cache shaders. None of them *loads* from a typed UAV, so all nine
are legal at feature level 11_0 as they are; the word-based forms below
exist because the d3d11 backend's guest memory mirror is a raw byte address
buffer, and a raw UAV can only be written by a `dcl_uav_raw` shader.

## The d3d11 backend's Step B resolve path

| role | shader to wire | notes |
| --- | --- | --- |
| resolve the guest EDRAM surface into the guest memory mirror | `fh1_native_resolve_memory_{color,depth,uint}[_ms]_cs` | `dcl_uav_raw`, legal on feature level 11_0 |
| move the resolved words into the new owner's render target | `fh1_native_transfer_{color,depth,stencil,uint}[_dms]_from_{color,depth,uint}[_ms]_ps` | writes a render target, no UAV |
| depth and stencil destinations | `fh1_native_transfer_words_{color,depth,uint}[_ms]_cs` then `fh1_native_transfer_{depth,stencil}[_dms]_from_words_ps` | `dcl_uav_raw` and a byte address buffer |
| the transfer pixel shaders' vertex stage | `fullscreen_cw_vs` | |

This is the same shape as the D3D12 backend's executor, which uses the
`fh1_native_resolve_memory_*` family and the transfer pixel shaders and does
not use the image family at all. The image family is legal at feature level
11_0 but is not needed: the D3D12 backend does not use it either, and the
transfer pixel shaders above already move the words into the render target.

## Per shader

### Resolve (12, `cs_5_0`, from `fh1_native_resolve_memory.cs.hlsl`)

| shader | defines | verdict |
| --- | --- | --- |
| `fh1_native_resolve_memory_color_cs` | — | clean |
| `fh1_native_resolve_memory_color_ms_cs` | `FH1_SOURCE_MSAA=1` | clean |
| `fh1_native_resolve_memory_depth_cs` | `FH1_SOURCE_DEPTH=1` | clean |
| `fh1_native_resolve_memory_depth_ms_cs` | `FH1_SOURCE_DEPTH=1 FH1_SOURCE_MSAA=1` | clean |
| `fh1_native_resolve_memory_uint_cs` | `FH1_SOURCE_UINT=1` | clean |
| `fh1_native_resolve_memory_uint_ms_cs` | `FH1_SOURCE_UINT=1 FH1_SOURCE_MSAA=1` | clean |
| `fh1_native_resolve_image_color_cs` | `FH1_DEST_IMAGE=1` | wide typed UAV store |
| `fh1_native_resolve_image_color_ms_cs` | `FH1_DEST_IMAGE=1 FH1_SOURCE_MSAA=1` | wide typed UAV store |
| `fh1_native_resolve_image_depth_cs` | `FH1_DEST_IMAGE=1 FH1_SOURCE_DEPTH=1` | wide typed UAV store |
| `fh1_native_resolve_image_depth_ms_cs` | `FH1_DEST_IMAGE=1 FH1_SOURCE_DEPTH=1 FH1_SOURCE_MSAA=1` | wide typed UAV store |
| `fh1_native_resolve_image_uint_cs` | `FH1_DEST_IMAGE=1 FH1_SOURCE_UINT=1` | wide typed UAV store |
| `fh1_native_resolve_image_uint_ms_cs` | `FH1_DEST_IMAGE=1 FH1_SOURCE_UINT=1 FH1_SOURCE_MSAA=1` | wide typed UAV store |

`FH1_DEST_IMAGE` adds two typed UAV images (`RWTexture2D<uint> fh1_image`,
`fh1_image2`). The 5_0 bytecode declares them as
`dcl_uav_typed_texture2d (uint,uint,uint,uint)` and stores 128 bits per
texel (`store_uav_typed U1[1].xyzw`): a typed UAV *store* of
`R32G32B32A32_UINT`, which feature level 11_0 requires, so the headers are
bindable as they are. The d3d11 backend does not need them anyway: the
word-based form is the same source without `FH1_DEST_IMAGE`, that is
`fh1_native_resolve_memory_*`, which writes the same words through
`RWByteAddressBuffer` (`dcl_uav_raw`).

The Vulkan backend, which does use the image form, needs the
`[[vk::image_format("r32ui")]]` declaration of the SPIR-V build.

### Transfer words (6, `cs_5_0`, from `fh1_native_transfer_words.cs.hlsl`)

`fh1_native_transfer_words_{color,depth,uint}[_ms]_cs` — clean. They write
the guest EDRAM words of a depth destination's rectangle once, through
`RWByteAddressBuffer` (`dcl_uav_raw`).

### Transfer (48, `ps_5_0`, from `fh1_native_transfer.ps.hlsl`)

`fh1_native_transfer_{color,depth,stencil,uint}[_dms]_from_{color,depth,uint}[_ms]_ps`
— clean. The sources are textures (`Texture2D`, `Texture2DMS`) and the
destination is a render target, so there is no UAV at all. `FH1_DEST_KIND`
selects the destination kind (0 color, 1 depth, 2 one stencil bit, 3 color
through a UINT view); the MSAA variants shade per sample
(`SV_SampleIndex`).

`FH1_DEST_KIND=4` (depth and stencil in one pass) is Vulkan only: it needs
`SV_StencilRef`, which neither shader model 5.1 nor glslang's HLSL front end
has, so it is not part of the d3d12 or d3d11 sets.

### Transfer from words (4, `ps_5_0`, from `fh1_native_transfer_from_words.ps.hlsl`)

`fh1_native_transfer_{depth,stencil}[_dms]_from_words_ps` — clean. They read
a `ByteAddressBuffer` of words computed by the transfer words pass and write
a depth value or discard on a stencil bit.

### Fullscreen vertex stage (1, `vs_5_0`, from `fullscreen_cw.vs.hlsl`)

`fullscreen_cw_vs` — clean.

### Texture cache (3, `cs_5_0`)

The three texture cache shaders are the ones that write a 128 bit typed UAV
into guest memory:

| shader | source | typed UAV it writes |
| --- | --- | --- |
| `fh1_scaled_32bpp_2x_cs` | `fh1_scaled_32bpp_2x.cs.hlsl` | `RWBuffer<uint4>`, that is a *typed* buffer UAV (`dcl_uav_typed_buffer (uint,uint,uint,uint)`, `R32G32B32A32_UINT`; `RWBuffer` is not a structured buffer, `RWStructuredBuffer` would be) |
| `fh1_scaled_32bpp_2x_texture_cs` | `fh1_scaled_32bpp_2x.cs.hlsl` with `FH1_DIRECT_TEXTURE_OUTPUT=1` | `RWTexture2D<uint4>` |
| `fh1_reflection_cube_import_cs` | `fh1_reflection_cube_import.cs.hlsl` | `RWTexture2DArray<uint4>` |

They only *store* 128 bits, so they are legal at feature level 11_0 — but
the d3d11 backend's guest memory mirror is a raw byte address buffer (the
same one the resolve path writes with `RWByteAddressBuffer`), and a raw UAV
is only writable by a `dcl_uav_raw` shader. So the d3d11 backend binds the
**word-based forms** of the three, which the d3d12 pass does not use:

| shader (d3d11_5_0) | destination | source |
| --- | --- | --- |
| `fh1_scaled_32bpp_2x_cs` | `RWByteAddressBuffer dest : register(u0)` (`dcl_uav_raw`) | `Buffer<uint4> source : register(t0)`, a typed `R32G32B32A32_UINT` buffer SRV |
| `fh1_scaled_32bpp_2x_texture_cs` | `RWByteAddressBuffer dest : register(u0)` (`dcl_uav_raw`) | same |
| `fh1_reflection_cube_import_cs` | `RWByteAddressBuffer dest : register(u0)` (`dcl_uav_raw`) | same |

They are the same sources with `FH1_WORD_DEST=1` (and
`FH1_DIRECT_TEXTURE_OUTPUT=1` for the texture form), and they write the same
bytes at the same offsets as the typed forms:

* `fh1_scaled_32bpp_2x_cs` (packed form): the typed form wrote
  `dest[output]`, that is bytes `output * 16 .. output * 16 + 15` of the
  buffer; the word form writes the four words of that uint4 at the same
  byte offsets. The constant buffer is unchanged.
* `fh1_scaled_32bpp_2x_texture_cs` (unpacked form): the typed form wrote
  the texel at `dest[uint2(x + i, y)]` of a `RWTexture2D<uint4>`, that is
  bytes `((y * row_pitch_bytes) + (x + i)) * 16 .. + 15` of the texture; the
  word form writes the same four words at
  `host_offset + y * host_pitch + (x + i) * 16`, so the backend passes the
  texture's base in bytes as `host_offset` and its row pitch in bytes as
  `host_pitch` (the typed form took both from the texture itself).
* `fh1_reflection_cube_import_cs`: the typed form wrote the texel at
  `dest[id]` of a `RWTexture2DArray<uint4>`; the word form writes the same
  four words at `id.z * dest_face_stride + id.y * dest_row_pitch +
  id.x * 16`, so the backend passes the array's row pitch and face stride in
  bytes. The constant buffer gains those two fields: `dest_row_pitch` and
  `dest_face_stride` replace the typed form's `unused_host_pitch` and
  `unused_height`, at the same offsets (register 8 and 9).

The word forms are in the same style as `fh1_native_resolve_memory_*`, which
already writes guest memory through a raw byte address buffer: a
`dcl_uav_raw` destination, a typed buffer SRV source, and the guest's own
constants in `b0`.

#### The two forms write the same bytes, on both APIs

The claim the d3d11 backend rests on is that the word-based form writes the
same bytes at the same offsets as the typed form, so that the two backends
write the same guest memory. `.local/tab3-verify/differential_probe.cpp`
proves it by running both forms on both devices and comparing the read back
destinations byte for byte:

| leg | form | API | shader |
| --- | --- | --- | --- |
| A | typed | d3d12 | `bytecode/d3d12_5_1`, shader model 5.1, the form the d3d12 backend binds |
| B | typed | d3d11 | the same source at shader model 5.0 |
| C | word | d3d11 | `bytecode/d3d11_5_0`, shader model 5.0, the form the d3d11 backend binds |
| D | word | d3d12 | the same header, on the d3d12 device |
| E | typed | d3d12 | the same source at shader model 5.0 |

On the AMD Radeon RX 5700 XT of this desktop:

* A, C and D write the same bytes for all three shaders: 1024 bytes for
  `fh1_scaled_32bpp_2x_cs`, 1024 bytes for `fh1_scaled_32bpp_2x_texture_cs`
  and 6144 bytes for `fh1_reflection_cube_import_cs`. The word form writes
  the same bytes on both devices, and they are the bytes the typed form
  writes.
* B and E write the same bytes for all three shaders: the API does not
  change the result.
* The comparison can fail. Three deliberately broken word forms (one broken
  byte offset each, built by `.local/tab3-verify/make_mutants.sh`) are run
  against the same typed results, and all three differ on both devices, so
  an equality here is not an equality that says nothing.

The probe also shows that a feature level 11_0 device refuses the shader
model 5.1 headers with `E_INVALIDARG` (`0x80070057`), which is why legs B
and E are compiled a second time at shader model 5.0.

#### fxc's shader model 5.0 backend drops a store

The typed form of `fh1_scaled_32bpp_2x_texture_cs` cannot be built at shader
model 5.0. fxc's 5.0 backend loses one of the stores of the unrolled loop,
so the shader writes 14 texels of a 16 texel row instead of 16. This is not
a difference between the forms and not a difference between the APIs: both
devices write the same 14 texels, and the word form of the same source at
the same shader model writes all 16.

It reproduces in a shader of fifteen lines that has nothing to do with this
project (`.local/tab3-verify/tex_store_canary.cs.hlsl`): a loop of four
iterations with two typed texture stores in the body.

| build | stores in the bytecode | texels written of 16 |
| --- | --- | --- |
| `cs_5_0` | 7 | 14 (the second store of the second iteration of the loop is gone, so the texel at `x + 1` stays at the sentinel in every thread) |
| `cs_5_1` | 8 | 16 |

The disassembly says the same: the `cs_5_0` listing has seven
`store_uav_typed` and never computes the address `x + 1`, the `cs_5_1`
listing has eight.

The trigger is the unrolled loop with more than one store in the body, not
the resource: the same shape over `RWBuffer<uint4>` loses the store as well,
while one store per iteration and a loop that is not unrolled both keep
all of them.

So the word form is not only the form the d3d11 backend can bind (the guest
memory mirror is a raw byte address buffer, and only a `dcl_uav_raw` shader
can write a raw UAV): for the texture form it is the only form that is
*correct* at shader model 5.0. The typed form stays the d3d12 backend's
form, at shader model 5.1, where fxc compiles it correctly.

### Not built here (108)

* **105 guest shaders** (`xe_*`): `texture_load_*` (57), `resolve_full_*` (10),
  `resolve_fast_*` (8), `apply_gamma_*` (8), `resolve_clear_*` (4),
  `discrete_*` (4), `continuous_*` (4), `host_depth_store_*` (3),
  `tessellation_*` (2), `fxaa_*` (2), `adaptive_*` (2), `clear_uint2_ps` (1).
  Their constant buffers and resources are the guest's own (`push_consts_xe`,
  `xe_resolve_dest`, `xe_fxaa_source`, ...), so they are guest bytecode that
  the guest shader translator retargets, not host HLSL in this tree. Their
  shader model 5.0 retarget belongs to that tab: a feature level 11_0 device
  rejects their shader model 5.1 form exactly like any other 5.1 bytecode
  (see "Verified on a feature level 11_0 device" below). Two of them are worth
  naming here because the guest's own resolve path writes a 128 bit typed UAV
  as well (`resolve_full_128bpp_cs` declares
  `dcl_uav_typed_buffer (uint,uint,uint,uint)`), so the guest translator has
  to solve the same typed UAV question for them.
* **3 host shaders without a source in this tree**: `float24_round_ps`,
  `float24_truncate_ps` (pixel shaders that adjust `SV_Depth` for the float24
  depth format) and `passthrough_position_xy_vs` (a four instruction vertex
  shader). They are checked-in artifacts built elsewhere, so this pass cannot
  compile them; their 5_0 forms have to be added by whoever owns their build.
  They are not needed for Step B.

## Verified on a feature level 11_0 device

On this desktop (`AMD Radeon RX 5700 XT`, device created at feature level
11_0):

* `CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2)` reports
  `TypedUAVLoadAdditionalFormats = 1`, that is, this device has the D3D11.3
  extension with the wide typed UAV format list. A GTX 570 (Fermi, feature
  level 11_0) does not have it, which is why the verdicts above only rely on
  the format table.
* Every header of `bytecode/d3d11_5_0/` loads into the feature level 11_0
  device: 74 of 74 create their shader object (`ID3D11Device::CreateComputeShader`,
  `CreatePixelShader`, `CreateVertexShader` all return `S_OK`), 0 failed.
* Every header of `bytecode/d3d12_5_1/` is refused by the same device:
  182 of 182 fail with `0x80070057` (`E_INVALIDARG`), because they are
  shader model 5.1. That is why the third pass is needed at all.
* The three word-based texture cache shaders run and write the expected
  bytes: each is dispatched on the feature level 11_0 device with a raw UAV
  (`DXGI_FORMAT_R32_TYPELESS` + `D3D11_BUFFER_UAV_FLAG_RAW` over a buffer
  with `D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS`) and a typed
  `R32G32B32A32_UINT` buffer SRV for the source, and the destination words
  are compared against the values the shader computes:
  * `fh1_scaled_32bpp_2x_cs` writes the eight packed words of the two
    source uint4s, byte swapped;
  * `fh1_scaled_32bpp_2x_texture_cs` writes the eight unpacked texels,
    16 bytes each, as the four 10 bit components of the swapped words;
  * `fh1_reflection_cube_import_cs` writes one 16 byte texel per cube
    face, the faces 1024 bytes apart in the destination and 16 bytes apart
    in the source.
  All three write nothing else: the rest of the destination keeps its
  sentinel value, and the device is not removed.

The probes are in `.local/tab3-verify/`: `load_all.cpp` (loads a whole
directory of headers), `uav_run_probe.cpp` (runs one compute shader of the
typed form and reads the result back), `word_uav_probe.cpp` (runs the
three word-based forms and checks the bytes they write) and
`differential_probe.cpp` (runs the typed and the word form of the three
texture cache shaders on both devices and compares the bytes they write).

## Regression

`compile-fh1-native.sh` still writes the d3d12_5_1 and vulkan_spirv sets
exactly as before, plus the d3d11_5_0 set. The only change to the existing
passes is that a header is normalised to line feed endings after `fxc` writes
it (`fxc` writes CRLF, the repository keeps text files in LF, see
`.gitattributes`: `* text=auto eol=lf`); without that, a fresh build differs
from the checked-in headers on the line endings alone.

Measured against the main tree (`F:\PinyonShift\work`):

* `bytecode/d3d12_5_1/`: 182 headers, byte for byte identical.
* `vulkan_spirv/`: 185 headers, byte for byte identical.
* `bytecode/d3d11_5_0/`: the main tree already holds a 71 header set of the
  same shaders. The 71 headers are identical apart from the line endings
  (the main tree's are CRLF, these are LF), and this set adds the three
  texture cache headers the main tree does not have:
  `fh1_scaled_32bpp_2x_cs`, `fh1_scaled_32bpp_2x_texture_cs` and
  `fh1_reflection_cube_import_cs`. The main tree's 71 headers all load on
  the feature level 11_0 device as well (71 of 71).

A second run of the script reproduces the set byte for byte: the 74
d3d11_5_0 headers are unchanged (same sha256 per file), and the 182
d3d12_5_1 headers are unchanged.
