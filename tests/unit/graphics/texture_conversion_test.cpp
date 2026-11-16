/**
 * @file        graphics/texture_conversion_test.cpp
 * @brief       Unit tests for the CPU texture untile/convert load path
 *
 *              Drives the device-free conversion layer of the Direct3D 11
 *              texture upload path (Phase 2): the per-format CPU converters
 *              and the CopyGuestLevelSlice level driver (tiled and linear,
 *              2D and 3D, packed mip tail windows, run grouping, endianness)
 *              against hand-built and round-tripped guest layouts, with no
 *              graphics device involved.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

using rex::graphics::texture_conversion::ConvertRunDepth24FloatToR32;
using rex::graphics::texture_conversion::ConvertRunDepth24UnormToR32;

namespace {

using rex::graphics::texture_conversion::ConvertRunCallback;
using rex::graphics::texture_conversion::ConvertRunCopy;
using rex::graphics::texture_conversion::ConvertRunCTX1ToR8G8;
using rex::graphics::texture_conversion::ConvertRunDXT3AToR8;
using rex::graphics::texture_conversion::ConvertRunR5G5B5A1ToB5G5R5A1;
using rex::graphics::texture_conversion::ConvertRunR5G6B5ToB5G6R5;
using rex::graphics::texture_conversion::ConvertRunRGBA4ToBGRA4;
using rex::graphics::texture_conversion::CopyGuestLevelSlice;
using rex::graphics::texture_conversion::GuestLevelSlice;
namespace texture_util = rex::graphics::texture_util;
using rex::graphics::xenos::Endian;

constexpr uint32_t kLog2Bpp(uint32_t bytes_per_block) {
  uint32_t log2 = 0;
  while ((UINT32_C(1) << log2) < bytes_per_block) {
    ++log2;
  }
  return log2;
}

// Copies a guest block run into a 2D host image through the driver.
GuestLevelSlice MakeLinear2DSlice(const std::vector<uint8_t>& source, uint8_t* destination,
                                  uint32_t width_blocks, uint32_t height_blocks,
                                  uint32_t pitch_blocks, uint32_t source_block_bytes,
                                  uint32_t destination_pitch, ConvertRunCallback convert,
                                  Endian endian = Endian::kNone) {
  GuestLevelSlice slice = {};
  slice.source = source.data();
  slice.tiled = false;
  slice.tiled_3d = false;
  slice.pitch_blocks = pitch_blocks;
  slice.height_blocks_storage = height_blocks;
  slice.width_blocks = width_blocks;
  slice.height_blocks = height_blocks;
  slice.depth = 1;
  slice.z_slice_stride_block_rows = height_blocks;
  slice.source_block_bytes = source_block_bytes;
  slice.destination = destination;
  slice.destination_pitch_bytes = destination_pitch;
  slice.destination_rows_per_block_row = 1;
  slice.destination_block_row_bytes = source_block_bytes;
  slice.endian = endian;
  slice.convert = convert;
  return slice;
}

// Reference endianness swap for whole 32-bit words plus a byte tail.
void ReferenceSwap(Endian endian, uint8_t* output, const uint8_t* input, uint32_t bytes) {
  uint32_t offset = 0;
  for (; offset + 4 <= bytes; offset += 4) {
    switch (endian) {
      case Endian::k8in16:
        output[offset] = input[offset + 1];
        output[offset + 1] = input[offset];
        output[offset + 2] = input[offset + 3];
        output[offset + 3] = input[offset + 2];
        break;
      case Endian::k8in32:
        output[offset] = input[offset + 3];
        output[offset + 1] = input[offset + 2];
        output[offset + 2] = input[offset + 1];
        output[offset + 3] = input[offset];
        break;
      case Endian::k16in32:
        output[offset] = input[offset + 2];
        output[offset + 1] = input[offset + 3];
        output[offset + 2] = input[offset];
        output[offset + 3] = input[offset + 1];
        break;
      default:
        std::memcpy(output + offset, input + offset, 4);
        break;
    }
  }
  for (; offset < bytes; ++offset) {
    output[offset] = input[offset];
  }
}

}  // namespace

TEST_CASE("convert run copy preserves bytes without endianness") {
  std::vector<uint8_t> input(16);
  for (uint32_t i = 0; i < uint32_t(input.size()); ++i) {
    input[i] = uint8_t(i * 7 + 1);
  }
  std::vector<uint8_t> output(16, 0xAA);
  ConvertRunCopy(Endian::kNone, output.data(), 16, input.data(), uint32_t(input.size()));
  CHECK(std::memcmp(output.data(), input.data(), input.size()) == 0);
}

TEST_CASE("convert run copy swaps whole words for each endianness mode") {
  std::vector<uint8_t> input(16);
  for (uint32_t i = 0; i < uint32_t(input.size()); ++i) {
    input[i] = uint8_t(i);
  }
  for (Endian endian : {Endian::k8in16, Endian::k8in32, Endian::k16in32}) {
    std::vector<uint8_t> expected(16, 0);
    ReferenceSwap(endian, expected.data(), input.data(), uint32_t(input.size()));
    std::vector<uint8_t> output(16, 0xAA);
    ConvertRunCopy(endian, output.data(), 16, input.data(), uint32_t(input.size()));
    INFO("endian mode " << uint32_t(endian));
    CHECK(std::memcmp(output.data(), expected.data(), expected.size()) == 0);
  }
}

TEST_CASE("convert run copy keeps trailing bytes of partial words") {
  // 6 bytes: one swapped 32-bit word, then 2 raw tail bytes.
  const uint8_t input[6] = {1, 2, 3, 4, 5, 6};
  uint8_t output[6] = {};
  ConvertRunCopy(Endian::k8in32, output, 6, input, 6);
  CHECK(output[0] == 4);
  CHECK(output[1] == 3);
  CHECK(output[2] == 2);
  CHECK(output[3] == 1);
  CHECK(output[4] == 5);
  CHECK(output[5] == 6);
}

TEST_CASE("convert run 565-family shuffles swap red and blue") {
  SECTION("r5g5b5a1 to b5g5r5a1") {
    // Guest A1R5G5B5: R in 14:10, B in 4:0.
    const uint16_t input = uint16_t((1 << 15) | (31 << 10) | (7 << 5) | 3);
    uint8_t output[2] = {};
    ConvertRunR5G5B5A1ToB5G5R5A1(Endian::kNone, output, 2, &input, 2);
    uint16_t result = uint16_t(output[0]) | (uint16_t(output[1]) << 8);
    CHECK(result == uint16_t((1 << 15) | (3 << 10) | (7 << 5) | 31));
  }
  SECTION("r5g6b5 to b5g6r5") {
    // Guest R5G6B5: R in 15:11, B in 4:0.
    const uint16_t input = uint16_t((31 << 11) | (21 << 5) | 3);
    uint8_t output[2] = {};
    ConvertRunR5G6B5ToB5G6R5(Endian::kNone, output, 2, &input, 2);
    uint16_t result = uint16_t(output[0]) | (uint16_t(output[1]) << 8);
    CHECK(result == uint16_t((3 << 11) | (21 << 5) | 31));
  }
  SECTION("rgba4 to bgra4") {
    // Guest R4G4B4A4: R in 15:12, B in 7:4.
    const uint16_t input = uint16_t((13 << 12) | (6 << 8) | (3 << 4) | 2);
    uint8_t output[2] = {};
    ConvertRunRGBA4ToBGRA4(Endian::kNone, output, 2, &input, 2);
    uint16_t result = uint16_t(output[0]) | (uint16_t(output[1]) << 8);
    CHECK(result == uint16_t((3 << 12) | (6 << 8) | (13 << 4) | 2));
  }
}

TEST_CASE("convert run dxt3a expands 2-bit texels to r8 rows") {
  // Two blocks in one run: all four 2-bit values in block 0 (rows major),
  // a uniform block 1.
  uint64_t bits0 = 0;
  for (uint32_t texel = 0; texel < 16; ++texel) {
    bits0 |= uint64_t(texel & 3) << (texel * 2);
  }
  uint8_t block0[8];
  for (uint32_t byte = 0; byte < 8; ++byte) {
    block0[byte] = uint8_t(bits0 >> (byte * 8));
  }
  uint8_t block1[8] = {0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89};
  // Value 3 everywhere.
  std::memset(block1, 0xFF, sizeof(block1));

  uint8_t input[16];
  std::memcpy(input, block0, 8);
  std::memcpy(input + 8, block1, 8);

  uint8_t output[2 * 4 * 4];
  std::memset(output, 0xAA, sizeof(output));
  ConvertRunDXT3AToR8(Endian::kNone, output, 2 * 4, input, 16);

  const uint8_t kExpand[4] = {0, 85, 170, 255};
  for (uint32_t y = 0; y < 4; ++y) {
    for (uint32_t x = 0; x < 4; ++x) {
      CHECK(output[y * (2 * 4) + x] == kExpand[(y * 4 + x) & 3]);
      CHECK(output[y * (2 * 4) + 4 + x] == 255);
    }
  }
}

TEST_CASE("convert run ctx1 interpolates red and green endpoints") {
  // Block: g0=0, r0=30, g1=12, r1=6, all texels index 3.
  uint8_t input[8] = {0, 30, 12, 6, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t output[2 * 4 * 4];
  std::memset(output, 0xAA, sizeof(output));
  ConvertRunCTX1ToR8G8(Endian::kNone, output, 2 * 4, input, 8);

  // Index 3 = 1/3 * r0 + 2/3 * r1: cr = 1/3 * 30 + 2/3 * 6 = 14,
  // cg = 1/3 * 0 + 2/3 * 12 = 8.
  for (uint32_t y = 0; y < 4; ++y) {
    for (uint32_t x = 0; x < 4; ++x) {
      CHECK(output[y * (2 * 4) + x * 2 + 0] == 14);
      CHECK(output[y * (2 * 4) + x * 2 + 1] == 8);
    }
  }
}

TEST_CASE("convert run depth24 unorm extracts depth above stencil") {
  // Packed D24S8 words: depth in bits 31:8, stencil in 7:0.
  const uint32_t words[] = {
      0x00000000,              // Depth 0.
      0x000000AB,              // Stencil only.
      0xFFFFFF00U | 0x5A,      // Max depth, stencil set.
      0x80000000U,             // Half depth.
  };
  uint8_t input[sizeof(words)];
  std::memcpy(input, words, sizeof(words));
  uint8_t output[sizeof(words)] = {};
  ConvertRunDepth24UnormToR32(Endian::kNone, output, sizeof(input), input, sizeof(input));
  float results[4];
  std::memcpy(results, output, sizeof(results));
  CHECK(results[0] == 0.0f);
  CHECK(results[1] == 0.0f);
  CHECK(results[2] == 1.0f);
  // 0x800000 rounds up (true division by 0xFFFFFF is just above the midpoint):
  // float(0x800001) * 2^-24, the float bit pattern 0x3F000001.
  uint32_t half_bits;
  std::memcpy(&half_bits, &results[3], sizeof(half_bits));
  CHECK(half_bits == UINT32_C(0x3F000001));
}

TEST_CASE("convert run depth24 float decodes 20e4 guest depth") {
  // Build 20e4 encodings with the guest-side encoder, then pack them as the
  // depth surface does (bits 31:8) with stencil noise in 7:0.
  const float samples[4] = {0.0f, 0.5f, 1.0f, 0.25f};
  uint8_t input[4 * 4];
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t packed =
        (rex::graphics::xenos::Float32To20e4(samples[i], true) << 8) | (i * 0x11);
    for (uint32_t byte = 0; byte < 4; ++byte) {
      input[i * 4 + byte] = uint8_t(packed >> (byte * 8));
    }
  }
  uint8_t output[sizeof(input)] = {};
  ConvertRunDepth24FloatToR32(Endian::kNone, output, sizeof(input), input, sizeof(input));
  float results[4];
  std::memcpy(results, output, sizeof(results));
  for (uint32_t i = 0; i < 4; ++i) {
    INFO("sample " << i);
    CHECK(results[i] == samples[i]);
  }
}

TEST_CASE("convert run depth24 applies endianness before extraction") {
  // k8in32: the guest big-endian word stored as bytes 01 02 03 04 is the
  // value 0x01020304 - depth is 0x010203, not the raw little-endian read.
  const uint8_t input[4] = {1, 2, 3, 4};
  uint8_t output[4] = {};
  ConvertRunDepth24UnormToR32(Endian::k8in32, output, 4, input, 4);
  float result;
  std::memcpy(&result, output, sizeof(result));
  CHECK(result == rex::graphics::xenos::UNorm24To32(0x010203));
}

TEST_CASE("guest level slice copies linear rows past pitch padding") {
  // 3 blocks of 4 bytes per row, pitch 5 blocks (20 bytes of padding per row).
  constexpr uint32_t kBlockBytes = 4;
  constexpr uint32_t kWidth = 3;
  constexpr uint32_t kHeight = 2;
  constexpr uint32_t kPitch = 5;
  std::vector<uint8_t> source(kPitch * kHeight * kBlockBytes, 0x5A);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      for (uint32_t byte = 0; byte < kBlockBytes; ++byte) {
        source[(y * kPitch + x) * kBlockBytes + byte] =
            uint8_t(1 + y * 16 + x * 4 + byte);
      }
    }
  }
  std::vector<uint8_t> destination(kWidth * kHeight * kBlockBytes, 0xAA);
  GuestLevelSlice slice =
      MakeLinear2DSlice(source, destination.data(), kWidth, kHeight, kPitch, kBlockBytes,
                        kWidth * kBlockBytes, ConvertRunCopy);
  CopyGuestLevelSlice(slice);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      for (uint32_t byte = 0; byte < kBlockBytes; ++byte) {
        CHECK(destination[(y * kWidth + x) * kBlockBytes + byte] ==
              uint8_t(1 + y * 16 + x * 4 + byte));
      }
    }
  }
}

TEST_CASE("guest level slice applies endianness word swaps on linear rows") {
  constexpr uint32_t kBlockBytes = 4;
  constexpr uint32_t kWidth = 4;
  std::vector<uint8_t> source(kWidth * kBlockBytes);
  for (uint32_t i = 0; i < uint32_t(source.size()); ++i) {
    source[i] = uint8_t(i * 5 + 3);
  }
  std::vector<uint8_t> destination(source.size(), 0xAA);
  GuestLevelSlice slice =
      MakeLinear2DSlice(source, destination.data(), kWidth, 1, kWidth, kBlockBytes,
                        kWidth * kBlockBytes, ConvertRunCopy, Endian::k8in32);
  CopyGuestLevelSlice(slice);
  std::vector<uint8_t> expected(source.size());
  ReferenceSwap(Endian::k8in32, expected.data(), source.data(), uint32_t(source.size()));
  CHECK(std::memcmp(destination.data(), expected.data(), expected.size()) == 0);
}

TEST_CASE("guest level slice gathers tiled 2d blocks") {
  // 8x5 blocks of 2 bytes, tiled with a 32-block pitch.
  constexpr uint32_t kBlockBytes = 2;
  constexpr uint32_t kWidth = 8;
  constexpr uint32_t kHeight = 5;
  constexpr uint32_t kPitch = 32;
  const uint32_t log2_bpp = kLog2Bpp(kBlockBytes);
  // Upper bound of the tiled address space touched, from the tile-aligned origin.
  std::vector<uint8_t> source(64 * 1024, 0);
  std::vector<uint8_t> pattern(kWidth * kHeight * kBlockBytes);
  for (uint32_t i = 0; i < uint32_t(pattern.size()); ++i) {
    pattern[i] = uint8_t(i + 1);
  }
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      int32_t offset = texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), kPitch, log2_bpp);
      REQUIRE(offset >= 0);
      REQUIRE(size_t(offset) + kBlockBytes <= source.size());
      std::memcpy(source.data() + offset,
                  pattern.data() + (y * kWidth + x) * kBlockBytes, kBlockBytes);
    }
  }
  std::vector<uint8_t> destination(pattern.size(), 0xAA);
  GuestLevelSlice slice = MakeLinear2DSlice(source, destination.data(), kWidth, kHeight, kPitch,
                                            kBlockBytes, kWidth * kBlockBytes, ConvertRunCopy);
  slice.tiled = true;
  CopyGuestLevelSlice(slice);
  CHECK(std::memcmp(destination.data(), pattern.data(), pattern.size()) == 0);
}

TEST_CASE("guest level slice gathers packed mip tail windows") {
  // A 16x16 block sublevel at a (4, 8) block origin inside its storage.
  constexpr uint32_t kBlockBytes = 4;
  constexpr uint32_t kWidth = 16;
  constexpr uint32_t kHeight = 16;
  constexpr uint32_t kPitch = 32;
  constexpr uint32_t kOffsetX = 4;
  constexpr uint32_t kOffsetY = 8;
  const uint32_t log2_bpp = kLog2Bpp(kBlockBytes);
  std::vector<uint8_t> source(256 * 1024, 0);
  std::vector<uint8_t> pattern(kWidth * kHeight * kBlockBytes);
  for (uint32_t i = 0; i < uint32_t(pattern.size()); ++i) {
    pattern[i] = uint8_t(i * 3 + 7);
  }
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      int32_t offset = texture_util::GetTiledOffset2D(int32_t(x + kOffsetX),
                                                      int32_t(y + kOffsetY), kPitch, log2_bpp);
      REQUIRE(offset >= 0);
      REQUIRE(size_t(offset) + kBlockBytes <= source.size());
      std::memcpy(source.data() + offset,
                  pattern.data() + (y * kWidth + x) * kBlockBytes, kBlockBytes);
    }
  }
  std::vector<uint8_t> destination(pattern.size(), 0xAA);
  GuestLevelSlice slice = MakeLinear2DSlice(source, destination.data(), kWidth, kHeight, kPitch,
                                            kBlockBytes, kWidth * kBlockBytes, ConvertRunCopy);
  slice.tiled = true;
  slice.offset_x_blocks = kOffsetX;
  slice.offset_y_blocks = kOffsetY;
  CopyGuestLevelSlice(slice);
  CHECK(std::memcmp(destination.data(), pattern.data(), pattern.size()) == 0);
}

TEST_CASE("guest level slice keeps tiled run grouping out of endianness swaps") {
  // 1-byte blocks with 8-in-32 endianness: the swap unit is a contiguous
  // tiled run, never a single block and never across a hole. Reference: split
  // each row's scattered block offsets into maximal contiguous runs and swap
  // each run as 32-bit words.
  constexpr uint32_t kBlockBytes = 1;
  constexpr uint32_t kWidth = 12;
  constexpr uint32_t kHeight = 3;
  constexpr uint32_t kPitch = 32;
  const uint32_t log2_bpp = kLog2Bpp(kBlockBytes);
  std::vector<uint8_t> source(64 * 1024, 0);
  std::vector<uint8_t> pattern(kWidth * kHeight * kBlockBytes);
  for (uint32_t i = 0; i < uint32_t(pattern.size()); ++i) {
    pattern[i] = uint8_t(i * 13 + 5);
  }
  std::vector<uint8_t> expected(pattern.size(), 0xAA);
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::vector<uint32_t> offsets(kWidth);
    for (uint32_t x = 0; x < kWidth; ++x) {
      offsets[x] = uint32_t(
          texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), kPitch, log2_bpp));
      source[offsets[x]] = pattern[y * kWidth + x];
    }
    for (uint32_t x = 0; x < kWidth;) {
      uint32_t run_last = x;
      while (run_last + 1 < kWidth &&
             offsets[run_last + 1] == offsets[x] + (run_last + 1 - x) * kBlockBytes) {
        ++run_last;
      }
      uint32_t run_blocks = run_last - x + 1;
      ReferenceSwap(Endian::k8in32, expected.data() + y * kWidth + x,
                    source.data() + offsets[x], run_blocks * kBlockBytes);
      x = run_last + 1;
    }
  }
  std::vector<uint8_t> destination(pattern.size(), 0xAA);
  GuestLevelSlice slice =
      MakeLinear2DSlice(source, destination.data(), kWidth, kHeight, kPitch, kBlockBytes,
                        kWidth * kBlockBytes, ConvertRunCopy, Endian::k8in32);
  slice.tiled = true;
  CopyGuestLevelSlice(slice);
  CHECK(std::memcmp(destination.data(), expected.data(), expected.size()) == 0);
}

TEST_CASE("guest level slice walks linear 3d z slices at the slice stride") {
  constexpr uint32_t kBlockBytes = 4;
  constexpr uint32_t kWidth = 3;
  constexpr uint32_t kHeight = 2;
  constexpr uint32_t kDepth = 3;
  constexpr uint32_t kPitch = 4;
  constexpr uint32_t kZStride = 8;  // Block rows between Z slices.
  std::vector<uint8_t> source(kPitch * kZStride * kDepth * kBlockBytes, 0x33);
  std::vector<uint8_t> pattern(kWidth * kHeight * kDepth * kBlockBytes);
  for (uint32_t i = 0; i < uint32_t(pattern.size()); ++i) {
    pattern[i] = uint8_t(i + 1);
  }
  for (uint32_t z = 0; z < kDepth; ++z) {
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        std::memcpy(source.data() + ((z * kZStride + y) * kPitch + x) * kBlockBytes,
                    pattern.data() +
                        ((z * kHeight + y) * kWidth + x) * kBlockBytes,
                    kBlockBytes);
      }
    }
  }
  std::vector<uint8_t> destination(pattern.size(), 0xAA);
  GuestLevelSlice slice = MakeLinear2DSlice(source, destination.data(), kWidth, kHeight, kPitch,
                                            kBlockBytes, kWidth * kBlockBytes, ConvertRunCopy);
  slice.depth = kDepth;
  slice.z_slice_stride_block_rows = kZStride;
  CopyGuestLevelSlice(slice);
  CHECK(std::memcmp(destination.data(), pattern.data(), pattern.size()) == 0);
}

TEST_CASE("guest level slice gathers tiled 3d volumes") {
  constexpr uint32_t kBlockBytes = 2;
  constexpr uint32_t kWidth = 8;
  constexpr uint32_t kHeight = 6;
  constexpr uint32_t kDepth = 3;
  constexpr uint32_t kPitch = 32;
  constexpr uint32_t kHeightStorage = 32;
  const uint32_t log2_bpp = kLog2Bpp(kBlockBytes);
  std::vector<uint8_t> source(256 * 1024, 0);
  std::vector<uint8_t> pattern(kWidth * kHeight * kDepth * kBlockBytes);
  for (uint32_t i = 0; i < uint32_t(pattern.size()); ++i) {
    pattern[i] = uint8_t(i * 11 + 1);
  }
  for (uint32_t z = 0; z < kDepth; ++z) {
    for (uint32_t y = 0; y < kHeight; ++y) {
      for (uint32_t x = 0; x < kWidth; ++x) {
        int32_t offset = texture_util::GetTiledOffset3D(int32_t(x), int32_t(y), int32_t(z),
                                                        kPitch, kHeightStorage, log2_bpp);
        REQUIRE(offset >= 0);
        REQUIRE(size_t(offset) + kBlockBytes <= source.size());
        std::memcpy(source.data() + offset,
                    pattern.data() + ((z * kHeight + y) * kWidth + x) * kBlockBytes,
                    kBlockBytes);
      }
    }
  }
  std::vector<uint8_t> destination(pattern.size(), 0xAA);
  GuestLevelSlice slice = MakeLinear2DSlice(source, destination.data(), kWidth, kHeight, kPitch,
                                            kBlockBytes, kWidth * kBlockBytes, ConvertRunCopy);
  slice.tiled = true;
  slice.tiled_3d = true;
  slice.depth = kDepth;
  slice.height_blocks_storage = kHeightStorage;
  slice.z_slice_stride_block_rows = kHeightStorage;
  CopyGuestLevelSlice(slice);
  CHECK(std::memcmp(destination.data(), pattern.data(), pattern.size()) == 0);
}

TEST_CASE("guest level slice writes multi-row host units across the pitch") {
  // Two DXT3A blocks per row: the converter writes 4 host rows of 4 bytes per
  // block, and adjacent guest blocks advance 4 bytes within a host row.
  constexpr uint32_t kBlockBytes = 8;
  constexpr uint32_t kWidthBlocks = 2;
  constexpr uint32_t kHeightBlocks = 2;
  constexpr uint32_t kPitchBlocks = 4;
  constexpr uint32_t kRows = 4;
  constexpr uint32_t kBlockRowBytes = 4;
  constexpr uint32_t kDestinationPitch = kWidthBlocks * kBlockRowBytes + 8;  // Padding.
  std::vector<uint8_t> source(kPitchBlocks * kHeightBlocks * kBlockBytes, 0);
  // Value pattern: every texel of block (bx, by) is (by * 2 + bx).
  for (uint32_t by = 0; by < kHeightBlocks; ++by) {
    for (uint32_t bx = 0; bx < kWidthBlocks; ++bx) {
      uint64_t bits = 0;
      for (uint32_t texel = 0; texel < 16; ++texel) {
        bits |= uint64_t((by * 2 + bx) & 3) << (texel * 2);
      }
      for (uint32_t byte = 0; byte < 8; ++byte) {
        source[(by * kPitchBlocks + bx) * kBlockBytes + byte] =
            uint8_t(bits >> (byte * 8));
      }
    }
  }
  std::vector<uint8_t> destination(kDestinationPitch * kRows * kHeightBlocks, 0xAA);
  GuestLevelSlice slice = MakeLinear2DSlice(
      source, destination.data(), kWidthBlocks, kHeightBlocks, kPitchBlocks, kBlockBytes,
      kDestinationPitch, ConvertRunDXT3AToR8);
  slice.destination_rows_per_block_row = kRows;
  slice.destination_block_row_bytes = kBlockRowBytes;
  CopyGuestLevelSlice(slice);

  const uint8_t kExpand[4] = {0, 85, 170, 255};
  for (uint32_t by = 0; by < kHeightBlocks; ++by) {
    for (uint32_t row = 0; row < kRows; ++row) {
      for (uint32_t x = 0; x < kWidthBlocks * kBlockRowBytes; ++x) {
        uint8_t expected = kExpand[(by * 2 + x / kBlockRowBytes) & 3];
        CHECK(destination[(by * kRows + row) * kDestinationPitch + x] == expected);
      }
      // Pitch padding stays untouched.
      CHECK(destination[(by * kRows + row) * kDestinationPitch +
                        kWidthBlocks * kBlockRowBytes] == 0xAA);
    }
  }
}
