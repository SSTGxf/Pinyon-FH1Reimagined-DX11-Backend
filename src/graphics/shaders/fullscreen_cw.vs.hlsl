// Fullscreen clockwise triangle from SV_VertexID, in Direct3D screen
// coordinates (the third vertex shader needed by the FH1 native transfer
// pixel shaders on top of the guest vertex shaders).
//
// Vertex 0 covers the top-left of the viewport, vertex 1 the top-right,
// vertex 2 the bottom-left; the triangle covers the whole viewport and more,
// which is why it is drawn clockwise.

void main(uint vertex_id : SV_VertexID, out float4 position : SV_Position) {
  float2 coords = float2(float(vertex_id & 1), float((vertex_id >> 1) & 1));
  position = float4(coords * float2(4.0, -4.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
