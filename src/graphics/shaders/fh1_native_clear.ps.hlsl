cbuffer Fh1NativeClearConstants : register(b0) {
  uint4 fh1_clear_value;
};

#if FH1_CLEAR_DEPTH
float main() : SV_Depth {
  return asfloat(fh1_clear_value.x);
}
#elif FH1_CLEAR_UINT
uint4 main() : SV_Target {
  return fh1_clear_value;
}
#else
float4 main() : SV_Target {
  return asfloat(fh1_clear_value);
}
#endif
