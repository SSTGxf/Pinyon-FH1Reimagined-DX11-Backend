// FH1 native executor: reads a depth surface and writes its depth values to an
// R32_FLOAT render target, so the transfer probe (fh1_swap_fetch_trace) can
// count the empty texels of a depth rectangle. Direct3D 11 cannot copy a
// depth-stencil resource to a staging texture, so the values are read through
// a shader resource view and landed in a colour target that can be read back.
//
// Variant: FH1_PROBE_MSAA reads a multisampled depth, taking the largest value
// of a pixel's samples - a pixel is empty when all of its samples are zero, so
// this is exactly the "empty texel" the single-sampled probe counts.

cbuffer Fh1DepthProbeConstants : register(b0) {
  uint2 fh1_probe_origin;   // Rectangle origin in the source surface.
  uint fh1_probe_samples;   // The multisampled variant's sample count.
  uint fh1_probe_unused;
};

#ifdef FH1_PROBE_MSAA
Texture2DMS<float> fh1_probe_depth : register(t0);
#else
Texture2D<float> fh1_probe_depth : register(t0);
#endif

float main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy) + int2(fh1_probe_origin);
#ifdef FH1_PROBE_MSAA
  float depth = 0.0f;
  for (uint sample = 0; sample < fh1_probe_samples; ++sample) {
    depth = max(depth, fh1_probe_depth.Load(pixel, sample));
  }
  return depth;
#else
  return fh1_probe_depth.Load(int3(pixel, 0));
#endif
}
