// Exact XenosRecomp translation of GTA IV's VS_Bink container
// (cache hash 0x156BAD4A9EE62726). Kept in the additive native shader table so
// the immutable base cache is not regenerated or replaced.
#include "../xenos_shader_common.h"

#ifdef __spirv__

#define gWorldViewProj(INDEX) selectWrapper((INDEX) < 248, vk::RawBufferLoad<float4>(g_PushConstants.VertexShaderConstants + (8 + min(INDEX, 247)) * 16, 0x10), 0.0)

#elif defined(__air__)

#define gWorldViewProj(INDEX) selectWrapper((INDEX) < 248, (*(reinterpret_cast<device float4*>(g_PushConstants.VertexShaderConstants + (8 + min((uint)(INDEX), (uint)247)) * 16))), 0.0)

#else

cbuffer VertexShaderConstants : register(b0, space4)
{
	float4 gWorldViewProj[4] : packoffset(c8);
#define gWorldViewProj(INDEX) selectWrapper((INDEX) < 248, gWorldViewProj[min(INDEX, 247)], 0.0)
};

cbuffer SharedConstants : register(b2, space4)
{
	DEFINE_SHARED_CONSTANTS();
};

#endif

struct VertexShaderInput
{
#ifdef __air__
	float4 iPosition0 [[attribute(0)]];
	float4 iTexCoord0 [[attribute(13)]];
#else
	[[vk::location(0)]] float4 iPosition0 : POSITION0;
	[[vk::location(13)]] float4 iTexCoord0 : TEXCOORD0;
#endif
};
struct Interpolators
{
#ifdef __air__
	float4 oPos [[position]] [[invariant]];
	float4 oTexCoord0 [[user(TEXCOORD0)]];
	float4 oTexCoord1 [[user(TEXCOORD1)]];
	float4 oTexCoord2 [[user(TEXCOORD2)]];
	float4 oTexCoord3 [[user(TEXCOORD3)]];
	float4 oTexCoord4 [[user(TEXCOORD4)]];
	float4 oTexCoord5 [[user(TEXCOORD5)]];
	float4 oTexCoord6 [[user(TEXCOORD6)]];
	float4 oTexCoord7 [[user(TEXCOORD7)]];
	float4 oTexCoord8 [[user(TEXCOORD8)]];
	float4 oTexCoord9 [[user(TEXCOORD9)]];
	float4 oTexCoord10 [[user(TEXCOORD10)]];
	float4 oTexCoord11 [[user(TEXCOORD11)]];
	float4 oTexCoord12 [[user(TEXCOORD12)]];
	float4 oTexCoord13 [[user(TEXCOORD13)]];
	float4 oTexCoord14 [[user(TEXCOORD14)]];
	float4 oTexCoord15 [[user(TEXCOORD15)]];
	float4 oColor0 [[user(COLOR0)]];
	float4 oColor1 [[user(COLOR1)]];
	float clipDistance [[clip_distance]];
#else
	precise float4 oPos : SV_Position;
	float4 oTexCoord0 : TEXCOORD0;
	float4 oTexCoord1 : TEXCOORD1;
	float4 oTexCoord2 : TEXCOORD2;
	float4 oTexCoord3 : TEXCOORD3;
	float4 oTexCoord4 : TEXCOORD4;
	float4 oTexCoord5 : TEXCOORD5;
	float4 oTexCoord6 : TEXCOORD6;
	float4 oTexCoord7 : TEXCOORD7;
	float4 oTexCoord8 : TEXCOORD8;
	float4 oTexCoord9 : TEXCOORD9;
	float4 oTexCoord10 : TEXCOORD10;
	float4 oTexCoord11 : TEXCOORD11;
	float4 oTexCoord12 : TEXCOORD12;
	float4 oTexCoord13 : TEXCOORD13;
	float4 oTexCoord14 : TEXCOORD14;
	float4 oTexCoord15 : TEXCOORD15;
	float4 oColor0 : COLOR0;
	float4 oColor1 : COLOR1;
	float clipDistance : SV_ClipDistance;
#endif
};
#ifdef __air__
[[vertex]]
#else
#if !defined(__spirv__)
[shader("vertex")]
#endif
#endif
Interpolators shaderMain(
#ifdef __air__
	constant Texture2DDescriptorHeap* g_Texture2DDescriptorHeap [[buffer(0)]],
	constant Texture2DArrayDescriptorHeap* g_Texture2DArrayDescriptorHeap [[buffer(1)]],
	constant Texture3DDescriptorHeap* g_Texture3DDescriptorHeap [[buffer(2)]],
	constant TextureCubeDescriptorHeap* g_TextureCubeDescriptorHeap [[buffer(3)]],
	constant SamplerDescriptorHeap* g_SamplerDescriptorHeap [[buffer(4)]],
	constant PushConstants& g_PushConstants [[buffer(8)]],
	VertexShaderInput input [[stage_in]]
#else
	VertexShaderInput input
#endif
)
{
#ifdef __air__
	Interpolators output = Interpolators{};
#else
	Interpolators output = (Interpolators)0;
#endif
	output.oTexCoord0 = 0.0;
	output.oTexCoord1 = 0.0;
	output.oTexCoord2 = 0.0;
	output.oTexCoord3 = 0.0;
	output.oTexCoord4 = 0.0;
	output.oTexCoord5 = 0.0;
	output.oTexCoord6 = 0.0;
	output.oTexCoord7 = 0.0;
	output.oTexCoord8 = 0.0;
	output.oTexCoord9 = 0.0;
	output.oTexCoord10 = 0.0;
	output.oTexCoord11 = 0.0;
	output.oTexCoord12 = 0.0;
	output.oTexCoord13 = 0.0;
	output.oTexCoord14 = 0.0;
	output.oTexCoord15 = 0.0;
	output.oColor0 = 0.0;
	output.oColor1 = 0.0;

	float4 r0 = 0.0;
	float4 r1 = 0.0;
	float4 r2 = 0.0;
	float4 r3 = 0.0;
	float4 r4 = 0.0;
	float4 r5 = 0.0;
	float4 r6 = 0.0;
	float4 r7 = 0.0;
	float4 r8 = 0.0;
	float4 r9 = 0.0;
	float4 r10 = 0.0;
	float4 r11 = 0.0;
	float4 r12 = 0.0;
	float4 r13 = 0.0;
	float4 r14 = 0.0;
	float4 r15 = 0.0;
	float4 r16 = 0.0;
	float4 r17 = 0.0;
	float4 r18 = 0.0;
	float4 r19 = 0.0;
	float4 r20 = 0.0;
	float4 r21 = 0.0;
	float4 r22 = 0.0;
	float4 r23 = 0.0;
	float4 r24 = 0.0;
	float4 r25 = 0.0;
	float4 r26 = 0.0;
	float4 r27 = 0.0;
	float4 r28 = 0.0;
	float4 r29 = 0.0;
	float4 r30 = 0.0;
	float4 r31 = 0.0;
	int a0 = 0;
	int aL = 0;
	bool p0 = false;
	float ps = 0.0;

	r2.xyz = (float3)((input.iPosition0)).xyz;
	r0.xy = (float2)(swapFloats(g_SwappedTexcoords, (input.iTexCoord0), 0)).xy;
	r1.xyzw = (float4)((r2.zzzz * gWorldViewProj(2).xyzw + gWorldViewProj(3).xyzw));
	r1.xyzw = (float4)((r2.yyyy * gWorldViewProj(1).xyzw + r1.xyzw));
	output.oPos.xyzw = (float4)((r2.xxxx * gWorldViewProj(0).xyzw + r1.xyzw));
	output.oTexCoord0.xy = (float2)((max(r0.xy, r0.xy)));
	if (g_ClipPlaneEnabled) output.clipDistance = dot(output.oPos, g_ClipPlane);
	output.oPos.xy += g_HalfPixelOffset * output.oPos.w;
	return output;
}
