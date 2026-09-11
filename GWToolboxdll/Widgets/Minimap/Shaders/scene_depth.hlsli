// What counts as sky in the camera depth buffer.
//
// Shared, because the answer has to be the SAME in every pass that reads it. This is the whole
// of a bug that showed as a pale blue rim along distant ridges at night:
//
//   - the sky quad is drawn at z == 1.0 with LESSEQUAL, so it covers only pixels whose depth is
//     EXACTLY 1.0 - the cleared far value, where nothing was drawn;
//   - every pass that tints or fogs the world discarded anything with depth >= 0.999999,
//     treating it as sky and leaving it alone.
//
// Those two disagree over the band [0.999999, 1.0) - about 17 units of a 24-bit depth buffer.
// Pixels in it are geometry, so our sky does not cover them, and they are skipped by the tint,
// so they keep GW's own daylight colouring while everything around them is taken down to night.
// Only the very farthest sliver of the scene lands in that band, which is why it read as an
// outline along the skyline rather than as a region.
//
// The threshold is a HAIR below 1.0 rather than exactly at it, and that margin does a second
// job. On a silhouette the resolved depth is not purely geometry or purely sky: a pixel that is
// mostly sky can come back fractionally short of the far value, so a test for "exactly far"
// classifies it as geometry. Our sky then does not paint it, every world pass tints it as if it
// were ground, and GW's colour survives there - as a rim on the SKY side of the outline, which
// is why repairing inwards from the geometry never reached it.
//
// The margin is bounded by GW's own clip planes (near 46.875, far 48000): 0.9999 corresponds to
// geometry 43,545 units away, which is past the fog and effectively at the far plane, so
// nothing real is lost to it.
#ifndef REBIRTH_SCENE_DEPTH_INCLUDED
#define REBIRTH_SCENE_DEPTH_INCLUDED

#define REBIRTH_SKY_DEPTH 0.9999

// Screen UV -> depth-texture UV. The INTZ copy is sized from the DEPTH SURFACE, which need not
// match the viewport we sample with; RESZ does not scale, it lands 1:1 from the origin. Sampling
// by raw screen UV then reads a texel that is right at one corner and progressively wrong across
// the screen - the diagonal artifact. Worst where the depth gradient is steepest, i.e. the sea
// horizon, where being a texel out swaps sky for geometry entirely.
float2 depth_uv_scale : register(c30);

float2 SceneDepthUV(const float2 screen_uv)
{
    return screen_uv * depth_uv_scale;
}

bool IsSkyDepth(const float depth)
{
    return depth >= REBIRTH_SKY_DEPTH;
}

// The compass terrain disc to leave alone: (centre.x, centre.y, radius, aspect). Radius and
// centre are in UV, with the radius normalised by viewport WIDTH; aspect is height/width, so the
// y term below rescales back to a true circle instead of an ellipse. Radius 0 disables it.
// Shared register across every fullscreen pass so one upload serves them all.
float4 excluded_circle : register(c31);

// True inside a region that is NOT ours to paint.
//
// GW's compass renders 3D into its own sub-viewport, and its geometry sits in the same GR command
// stream as the world - so the flush that materialises the world materialises the compass too,
// and a fullscreen overlay afterwards paints straight over it. It writes no depth of its own, so
// the overlay tests against whatever world depth happens to lie behind that corner: sky there and
// the compass is replaced by sky, our water plane and it is replaced by water, land and only the
// tint applies, which is too subtle to notice. Hence "it only breaks over water" - the ordering
// is wrong everywhere, but only some cases are visible.
bool IsExcludedPixel(const float2 uv)
{
    if (excluded_circle.z <= 0.0) return false;
    float2 offset = uv - excluded_circle.xy;
    offset.y *= excluded_circle.w;
    return dot(offset, offset) <= excluded_circle.z * excluded_circle.z;
}

#endif
