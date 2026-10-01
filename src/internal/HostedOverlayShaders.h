// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

namespace ImGuiVRHelper::Internal::HostedOverlayShaders
{
	struct DrawConstants
	{
		float model[16]{};
		float trackingToColorClip[16]{};
		float trackingToDepthClip[16]{};
		float trackingToDepthMetres[4]{};
		float uvTransform[4]{ 1.0f, 1.0f, 0.0f, 0.0f };
		float depthRect[4]{};
		float depthDecode[4]{};
		float options[4]{};
	};
	static_assert(sizeof(DrawConstants) == 272);

	inline constexpr char source[] = R"(
cbuffer DrawConstants : register(b0)
{
	row_major float4x4 model;
	row_major float4x4 trackingToColorClip;
	row_major float4x4 trackingToDepthClip;
	float4 trackingToDepthMetres;
	float4 uvTransform;
	float4 depthRect;
	float4 depthDecode; // scale, offset, bias metres, encoding (0 native, 1 linear)
	float4 options; // depth test, output flip X, output flip Y, linear output
};
Texture2D<float4> panel : register(t0);
Texture2D<float> sceneDepth : register(t1);
SamplerState panelSampler : register(s0);
struct VertexOutput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 depthClip : TEXCOORD1;
	float depthMetres : TEXCOORD2;
};
VertexOutput VSMain(uint vertex : SV_VertexID)
{
	const float2 corners[6] = {
		float2(-0.5, 0.5), float2(0.5, 0.5), float2(0.5, -0.5),
		float2(-0.5, 0.5), float2(0.5, -0.5), float2(-0.5, -0.5)
	};
	float4 tracking = mul(float4(corners[vertex], 0, 1), model);
	VertexOutput output;
	output.position = mul(tracking, trackingToColorClip);
	output.position.xy *= float2(options.y != 0 ? -1 : 1, options.z != 0 ? -1 : 1);
	output.uv = (float2(corners[vertex].x, -corners[vertex].y) + 0.5) * uvTransform.xy + uvTransform.zw;
	output.depthClip = mul(tracking, trackingToDepthClip);
	output.depthMetres = dot(tracking, trackingToDepthMetres);
	return output;
}
bool Occluded(VertexOutput input)
{
	if (!all(isfinite(input.depthClip)) || input.depthClip.w <= 0 ||
		!isfinite(input.depthMetres) || input.depthMetres <= 0)
		return false;
	float3 ndc = input.depthClip.xyz / input.depthClip.w;
	if (!all(isfinite(ndc)) || ndc.x < -1 || ndc.x >= 1 || ndc.y <= -1 || ndc.y > 1 || ndc.z < 0 || ndc.z > 1)
		return false;
	float2 pixel = floor(depthRect.xy + float2(ndc.x + 1, 1 - ndc.y) * 0.5 * depthRect.zw);
	if (any(pixel < depthRect.xy) || any(pixel >= depthRect.xy + depthRect.zw))
		return false;
	float sample = sceneDepth.Load(int3(int2(pixel), 0));
	if (!isfinite(sample))
		return false;
	float metres;
	if (depthDecode.w == 0) {
		if (sample < 0 || sample > 1 || depthDecode.y - sample <= 0)
			return false;
		metres = depthDecode.x / (depthDecode.y - sample);
	} else {
		if (sample <= 0)
			return false;
		metres = depthDecode.x * sample;
	}
	float limit = metres + depthDecode.z;
	return isfinite(metres) && metres > 0 && isfinite(limit) && input.depthMetres > limit;
}
float4 PSMain(VertexOutput input) : SV_Target
{
	float4 color = panel.Sample(panelSampler, input.uv);
	if (options.x != 0 && Occluded(input))
		discard;
	if (options.w != 0) {
		float3 gamma = max(color.rgb, 0);
		color.rgb = lerp(gamma / 12.92, pow((gamma + 0.055) / 1.055, 2.4), step(0.04045, gamma));
	}
	return color;
}
)";
}
