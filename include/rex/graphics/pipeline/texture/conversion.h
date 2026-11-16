#pragma once
/**
 *******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 *******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 *******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <functional>
#include <memory>

#include <rex/assert.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::texture_conversion {

typedef std::function<void(xenos::Endian, void*, const void*, size_t)> CopyBlockCallback;

void CopySwapBlock(xenos::Endian endian, void* output, const void* input, size_t length);
void ConvertTexelCTX1ToR8G8(xenos::Endian endian, void* output, const void* input, size_t length);
void ConvertTexelDXT3AToDXT3(xenos::Endian endian, void* output, const void* input, size_t length);

typedef std::function<void(void*, const void*, size_t)> UntileCopyBlockCallback;

typedef struct UntileInfo {
  uint32_t offset_x;
  uint32_t offset_y;
  uint32_t width;
  uint32_t height;
  uint32_t input_pitch;
  uint32_t output_pitch;
  const FormatInfo* input_format_info;
  const FormatInfo* output_format_info;
  UntileCopyBlockCallback copy_callback;
} UntileInfo;

void Untile(uint8_t* output_buffer, const uint8_t* input_buffer, const UntileInfo* untile_info);

// ---------------------------------------------------------------------------
// CPU guest block -> host unit conversions (the Direct3D 11 load path).
//
// Each call converts a run of adjacent guest blocks whose source bytes are
// contiguous in memory (one block for block-compressed formats, several
// texels for 1x1-block formats - runs are at most 16 source bytes when
// untiling, whole rows when linear). The guest endianness is applied to the
// source as a whole first, so 32-bit-word swap modes are exact as long as the
// run is a whole number of 32-bit words, which the driver guarantees.
//
// The output of one run is `bytes / source block bytes` host units wide and
// `rows` host rows tall (1 for formats whose host unit is a full guest block
// or a single texel, the guest block height for uncompressed host formats of
// compressed guest blocks), written at `output + row * output_pitch`.
// ---------------------------------------------------------------------------

// Converts one run of guest blocks. `input_bytes` is a whole number of source
// blocks; the callback writes exactly as many host rows as one guest block
// row covers (1 unless the callback documents otherwise).
typedef void (*ConvertRunCallback)(xenos::Endian endian, void* output,
                                   uint32_t output_pitch, const void* input,
                                   uint32_t input_bytes);

// Raw copy with the endianness swap (block sizes preserved).
void ConvertRunCopy(xenos::Endian endian, void* output, uint32_t output_pitch,
                    const void* input, uint32_t input_bytes);
// Guest R5G5B5A1 texels to host B5G5R5A1 (red and blue swapped).
void ConvertRunR5G5B5A1ToB5G5R5A1(xenos::Endian endian, void* output,
                                  uint32_t output_pitch, const void* input,
                                  uint32_t input_bytes);
// Guest R5G6B5 texels to host B5G6R5 (red and blue swapped).
void ConvertRunR5G6B5ToB5G6R5(xenos::Endian endian, void* output,
                              uint32_t output_pitch, const void* input,
                              uint32_t input_bytes);
// Guest R4G4B4A4 texels to host B4G4R4A4 (red and blue swapped).
void ConvertRunRGBA4ToBGRA4(xenos::Endian endian, void* output,
                            uint32_t output_pitch, const void* input,
                            uint32_t input_bytes);// Guest DXT3A 4x4 blocks (2 bits per texel) to host R8_UNORM 4x4 texels.
// Writes 4 host rows per guest block row.
void ConvertRunDXT3AToR8(xenos::Endian endian, void* output, uint32_t output_pitch,
                         const void* input, uint32_t input_bytes);
// Guest CTX1 4x4 blocks (2 bits per texel, interpolated red/green endpoints)
// to host R8G8_UNORM 4x4 texels. Writes 4 host rows per guest block row.
void ConvertRunCTX1ToR8G8(xenos::Endian endian, void* output, uint32_t output_pitch,
                          const void* input, uint32_t input_bytes);
// Guest D24S8 words (24-bit unorm depth in bits 31:8, 8-bit stencil in 7:0)
// to host R32_FLOAT depth texels (stencil dropped - the host format is
// single-channel, matching the Direct3D 12 load shader).
void ConvertRunDepth24UnormToR32(xenos::Endian endian, void* output, uint32_t output_pitch,
                                 const void* input, uint32_t input_bytes);
// Guest D24FS8 words (20e4 float depth in bits 31:8) to host R32_FLOAT depth
// texels, decoding the float24 as the guest fetch unit would return it (the
// host depth buffer's 0...0.5 remap is a render-target-only convention and is
// not applied here).
void ConvertRunDepth24FloatToR32(xenos::Endian endian, void* output, uint32_t output_pitch,
                                 const void* input, uint32_t input_bytes);

// One array slice (or one 3D texture level) of one guest texture level,
// ready to be copied into a row-major host image.
struct GuestLevelSlice {
  // First byte of the level's guest storage (the array slice for 2D).
  const uint8_t* source;
  bool tiled;
  // 3D (volume) tiling - also used for 2D-as-3D wrapper loads.
  bool tiled_3d;
  // Guest row pitch in blocks (tiled) - a multiple of 32 for tiled textures.
  uint32_t pitch_blocks;
  // Height of the level's storage in blocks - the height argument of the 3D
  // tiled address function.
  uint32_t height_blocks_storage;
  // Origin of this sublevel within its storage in blocks (the packed mip tail
  // places smaller levels at non-zero origins).
  uint32_t offset_x_blocks;
  uint32_t offset_y_blocks;
  uint32_t offset_z;
  // Extent of the sublevel itself in blocks (1x1 texel formats: texels).
  uint32_t width_blocks;
  uint32_t height_blocks;
  uint32_t depth;
  // Distance between Z slices of the level in block rows (linear storage).
  uint32_t z_slice_stride_block_rows;
  uint32_t source_block_bytes;

  // Row-major host image; the driver writes each guest block row through
  // `convert`, which places its rows at output + row * destination_pitch_bytes.
  // The driver advances the destination by this many rows per guest block row.
  uint8_t* destination;
  uint32_t destination_pitch_bytes;
  uint32_t destination_rows_per_block_row;
  // Distance between the outputs of two adjacent guest blocks within a host
  // row (the host unit's width along X).
  uint32_t destination_block_row_bytes;

  xenos::Endian endian;
  ConvertRunCallback convert;
};

// Copies one guest level slice (tiled or linear, 2D or 3D) into a row-major
// host image, converting every guest block run through slice.convert.
void CopyGuestLevelSlice(const GuestLevelSlice& slice);

}  // namespace rex::graphics::texture_conversion
