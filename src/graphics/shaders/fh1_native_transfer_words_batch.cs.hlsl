// FH1 native executor ownership transfer, first step for depth destinations,
// batched: every rectangle of one destination/source pair in a single compute
// dispatch. Same word for the same destination sample as
// fh1_native_transfer_words.cs.hlsl, which carries one rectangle per dispatch;
// the dispatch covers the rectangles' bounding box and each thread keeps the
// rectangles it falls in, so the work moved is the same and only the number of
// dispatches differs.
//
// Variants: FH1_SOURCE_DEPTH / FH1_SOURCE_UINT / FH1_SOURCE_MSAA (see
// fh1_native_edram.hlsli).

#include "fh1_push_constants.hlsli"

// FH1_MAX_BATCH_RECTS rectangles, two dwords each (origin, size), after the
// eight header words. The host keeps the batch at or below that count.
#define FH1_MAX_BATCH_RECTS 32

FH1_PUSH_CONSTANTS cbuffer Fh1NativeTransferWordsBatchConstants FH1_CONSTANTS_REGISTER {
  uint fh1_rect_count;      // Rectangles in fh1_rects (1 to 32).
  uint fh1_dest_layout;     // Layout of the new owner, with its host sample mode.
  uint fh1_source_layout;   // Layout of the previous owner, with its host sample mode.
  uint fh1_flags;           // FH1_FLAG_*
  uint fh1_dest_width;      // Destination surface width in host pixels.
  uint fh1_dest_samples;    // Host samples per destination pixel.
  uint fh1_union;           // x | y << 16, the rectangles' bounding box.
  uint fh1_union_size;      // width | height << 16 of the bounding box.
  uint fh1_rects[FH1_MAX_BATCH_RECTS * 2];
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

uint2 Unpack(uint packed) { return uint2(packed & 0xFFFFu, packed >> 16u); }

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
  if (any(thread.xy >= Unpack(fh1_union_size))) {
    return;
  }
  uint2 pixel = Unpack(fh1_union) + thread.xy;
  // The rectangles of one pair do not overlap: the thread writes the word of
  // the one it falls in, and of no other.
  bool inside = false;
  [loop] for (uint i = 0u; i < fh1_rect_count; ++i) {
    uint2 origin = Unpack(fh1_rects[i * 2u]);
    uint2 size = Unpack(fh1_rects[i * 2u + 1u]);
    inside = inside || (all(pixel >= origin) && all(pixel < origin + size));
  }
  if (!inside) {
    return;
  }
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
