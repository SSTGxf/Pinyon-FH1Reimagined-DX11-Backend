// FH1 native executor ownership transfer, first step for depth destinations:
// computes the previous owner's guest EDRAM word for every destination sample
// of a rectangle once, so the depth pass and the eight stencil-bit passes
// (fh1_native_transfer_from_words.ps.hlsl) only load it.
//
// Variants: FH1_SOURCE_DEPTH / FH1_SOURCE_UINT / FH1_SOURCE_MSAA (see
// fh1_native_edram.hlsli).

#include "fh1_push_constants.hlsli"

FH1_PUSH_CONSTANTS cbuffer Fh1NativeTransferWordsConstants FH1_CONSTANTS_REGISTER {
  uint fh1_rect_origin;     // x | y << 16, destination host pixels
  uint fh1_rect_size;       // width | height << 16
  uint fh1_dest_layout;     // Layout of the new owner, with its host sample mode.
  uint fh1_source_layout;   // Layout of the previous owner, with its host sample mode.
  uint fh1_flags;           // FH1_FLAG_*
  uint fh1_dest_width;      // Destination surface width in host pixels.
  uint fh1_dest_samples;    // Host samples per destination pixel.
  uint fh1_unused;
};

#include "fh1_native_edram.hlsli"

// Feature level 11_0 (the d3d11 backend) binds the words through typed buffer
// views, one element per guest EDRAM word: RAW_VIEWS is fragile on Fermi
// (raw stores land only at low offsets, and a sub-range raw view cannot be
// created), while a typed R32_UINT buffer needs no raw view at all and one
// word per element keeps every store independent of its neighbours.
#ifdef FH1_WORD_TYPED
RWBuffer<uint> fh1_words : register(u0);
void StoreWord(uint index, uint value) { fh1_words[index] = value; }
#else
RWByteAddressBuffer fh1_words : register(u0);
void StoreWord(uint index, uint value) { fh1_words.Store(index * 4u, value); }
#endif

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
  uint2 rect_size = uint2(fh1_rect_size & 0xFFFFu, fh1_rect_size >> 16u);
  if (any(thread.xy >= rect_size)) {
    return;
  }
  uint2 pixel = uint2(fh1_rect_origin & 0xFFFFu, fh1_rect_origin >> 16u) + thread.xy;
  fh1_fixed16_scale = (fh1_flags & FH1_FLAG_FIXED16_FULL_RANGE) != 0u ? 32.0f : 1.0f;
  uint base = (pixel.y * fh1_dest_width + pixel.x) * fh1_dest_samples;
  for (uint host_sample = 0u; host_sample < fh1_dest_samples; ++host_sample) {
    uint guest_sample = GuestSample(host_sample, LayoutMsaa(fh1_dest_layout),
                                    LayoutHostSampleMode(fh1_dest_layout));
    uint2 guest_pixel = pixel;
    SetScaledPixel(guest_pixel, ((fh1_flags >> FH1_FLAG_SCALE_SHIFT) & 3u) + 1u);
    StoreWord(base + host_sample,
              LoadSourceWord(fh1_dest_layout, guest_pixel, guest_sample, 0u,
                             fh1_source_layout, fh1_flags));
  }
}
