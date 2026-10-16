#pragma once

// The reflex reticle's shaders, compiled at load. VSMain draws the lens glass for
// both eyes in one instanced call (instance & 1 = eye, each to its half of the
// double-wide target); PSMain projects the reticle art by angle from the eye, so
// it sits on the aim at infinity for either eye and moves with neither. Depth is
// written at the optic's far end so the housing hides what it should.
// CSVisibility measures the art once per texture for the visibility assist.

namespace TrueScopes::ReticleShader
{
	inline constexpr char kSource[] = R"(
cbuffer Optic : register(b0) {
    row_major float4x4 localToWorld;
    row_major float4x4 viewProjection[2];
    float4 originAdjust[2];
    float4 eye[2];          // w: 1 = this eye has a solution
    float4 aimForward[2];   // w: half-angle tangent of the art
    float4 aimRight[2];     // w: art aspect
    float4 aimUp[2];
    float4 depthPlane;
    float4 depthRange;      // min, max - min, alpha-test cutoff
    float4 tint;
    float4 uvTransform;
    float4 visibility;      // brightness (0 = off), mode (0 art, 1 mask, 2 dot), alpha scale
};
Texture2D reticle : register(t0);
SamplerState reticleSampler : register(s0);
RWStructuredBuffer<uint> visibilityStats : register(u0);
groupshared uint groupAlpha, groupBrightness, groupPeak;
[numthreads(16,16,1)]
void CSVisibility(uint3 id : SV_DispatchThreadID, uint thread : SV_GroupIndex) {
    if (thread == 0) { groupAlpha = 0; groupBrightness = 0; groupPeak = 0; }
    GroupMemoryBarrierWithGroupSync();
    uint width, height;
    reticle.GetDimensions(width, height);
    if (id.x < width && id.y < height) {
        float2 uv = uvTransform.xy + (float2(id.xy) + 0.5) / float2(width,height) * uvTransform.zw;
        float4 texel = reticle.SampleLevel(reticleSampler, uv, 0);
        float3 rgb = texel.rgb * tint.rgb;
        float peak = max(rgb.r, max(rgb.g,rgb.b));
        InterlockedAdd(groupAlpha, (uint)round(saturate(texel.a) * 255));
        InterlockedAdd(groupBrightness, (uint)round(saturate(texel.a) * saturate(peak) * 255));
        InterlockedMax(groupPeak, (uint)round(saturate(texel.a) * 65535));
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0) {
        InterlockedAdd(visibilityStats[0], groupAlpha);
        InterlockedAdd(visibilityStats[1], groupBrightness);
        InterlockedMax(visibilityStats[2], groupPeak);
    }
}
struct VertexInput {
    float4 position : POSITION;
    uint instance : SV_InstanceID;
};
struct PixelInput {
    float4 position : SV_POSITION;
    float3 world : TEXCOORD0;
    nointerpolation uint eyeIndex : TEXCOORD1;
    float edge : SV_ClipDistance0;
};
PixelInput VSMain(VertexInput input) {
    PixelInput output;
    uint e = input.instance & 1;
    float4 world = mul(float4(input.position.xyz, 1), localToWorld);
    float4 clipPos = mul(float4(world.xyz - originAdjust[e].xyz, 1), viewProjection[e]);
    output.world = world.xyz;
    output.eyeIndex = e;
    output.edge = min(clipPos.w, e == 0 ? clipPos.w - clipPos.x : clipPos.w + clipPos.x);
    output.position = clipPos;
    output.position.x = clipPos.x * 0.5 + (e == 0 ? -0.5 : 0.5) * clipPos.w;
    return output;
}
struct PixelOutput { float4 color : SV_TARGET; float depth : SV_DEPTH; };
float centerDistancePixels(float2 uv, float2 dx, float2 dy) {
    float determinant = dx.x*dy.y - dx.y*dy.x;
    float2 d = uv - 0.5;
    float2 pixels = float2(d.x*dy.y-d.y*dy.x, dx.x*d.y-dx.y*d.x) / (abs(determinant)>1e-12 ? determinant : 1e-12);
    return length(pixels);
}
PixelOutput PSMain(PixelInput input) {
    uint e = input.eyeIndex;
    clip(eye[e].w - 0.5);
    float3 ray = input.world - eye[e].xyz;
    float forwardDistance = dot(ray, aimForward[e].xyz);
    clip(forwardDistance - 0.000001);
    float2 uv = float2(dot(ray, aimRight[e].xyz) / aimRight[e].w, -dot(ray, aimUp[e].xyz));
    uv = 0.5 + uv / (forwardDistance * aimForward[e].w * 2);
    float2 dx = ddx(uv), dy = ddy(uv);
    bool inside = all(uv >= 0) && all(uv <= 1);
    // outside the art only the minimum-size assist dot may draw
    if (!inside) {
        clip(visibility.x - 0.001);
        clip(1.75 - centerDistancePixels(uv, dx, dy));
    }
    float4 texel = reticle.SampleGrad(reticleSampler, uvTransform.xy+uv*uvTransform.zw, dx*uvTransform.zw, dy*uvTransform.zw);
    float4 color = texel * tint;
    color.a *= inside ? 1 : 0;
    if (visibility.x > 0) {
        float3 red = float3(1,0.04,0.015);
        float peak = max(color.r,max(color.g,color.b));
        if (visibility.y == 1) {
            color.rgb = (peak > 0.01 ? color.rgb / peak : red) * visibility.x;
            color.a = saturate(texel.a * visibility.z) * tint.a * (inside ? 1 : 0);
        } else if (visibility.y == 2) color = 0;
        else color.rgb *= visibility.x;

        // a real centre mark lost to minification gets a 2.5 pixel dot
        float2 center = uvTransform.xy + 0.5 * uvTransform.zw;
        float4 sourceCenter = reticle.SampleLevel(reticleSampler, center, 0);
        float4 filteredCenter = reticle.SampleGrad(reticleSampler, center, dx*uvTransform.zw, dy*uvTransform.zw);
        bool tinyCenter = sourceCenter.a > 0.25 && filteredCenter.a < sourceCenter.a * 0.5;
        if (visibility.y == 2 || tinyCenter) {
            float coverage = 1-smoothstep(0.75,1.75,centerDistancePixels(uv,dx,dy));
            coverage *= tint.a;
            if (coverage > color.a) {
                float3 centerRgb = sourceCenter.rgb * tint.rgb;
                float centerPeak = max(centerRgb.r,max(centerRgb.g,centerRgb.b));
                float intensity = visibility.y == 0 && centerPeak > 0.01 ? centerPeak * visibility.x : visibility.x;
                color = float4((centerPeak>0.01 ? centerRgb/centerPeak : red)*intensity,coverage);
            }
        }
        clip(color.a - (visibility.y > 0 || tinyCenter ? 0.001 : max(0.001,depthRange.z)));
    } else clip(color.a - max(0.001,depthRange.z));
    // depth at the far end of the optic along this eye's ray
    float planeDenominator = dot(ray, depthPlane.xyz);
    clip(planeDenominator - 0.000001);
    float t = (depthPlane.w - dot(eye[e].xyz, depthPlane.xyz)) / planeDenominator;
    clip(t);
    float3 depthPoint = eye[e].xyz + t * ray;
    float4 projected = mul(float4(depthPoint - originAdjust[e].xyz, 1), viewProjection[e]);
    clip(projected.w - 0.000001);
    PixelOutput output;
    output.color = color;
    float ndcDepth = projected.z / projected.w;
    clip(ndcDepth);
    clip(1 - ndcDepth);
    output.depth = depthRange.x + ndcDepth * depthRange.y;
    return output;
}
)";
}
