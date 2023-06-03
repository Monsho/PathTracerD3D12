#include "math.hlsli"
#include "payload.hlsli"
#include "cbuffer.hlsli"
#include "pbr.hlsli"

#define RayTMax			10000.0

// global
ConstantBuffer<SceneCB>				cbScene			: register(b0, space0);
ConstantBuffer<LightCB>				cbLight			: register(b1, space0);
ConstantBuffer<PathTraceCB>			cbPathTrace		: register(b2, space0);

RaytracingAccelerationStructure		TLAS			: register(t0, space0);

RWByteAddressBuffer					rtResult		: register(u0, space0);
RWByteAddressBuffer					rtAlbedo		: register(u1, space0);
RWByteAddressBuffer					rtNormal		: register(u2, space0);

uint Hash32(uint x)
{
	x ^= x >> 16;
	x *= uint(0x7feb352d);
	x ^= x >> 15;
	x *= uint(0x846ca68b);
	x ^= x >> 16;
	return x;
}
float Hash32ToFloat(uint hash)
{ 
	return hash / 4294967296.0;
}
uint Hash32Combine(const uint seed, const uint value)
{
	return seed ^ (Hash32(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}
float2 Noise(uint2 pixPos, uint temporalIndex)
{
	uint baseHash = Hash32(pixPos.x + (pixPos.y << 15));
	baseHash = Hash32Combine(baseHash, temporalIndex);
	return float2(Hash32ToFloat(baseHash), Hash32ToFloat(Hash32(baseHash)));
}

float3 HemisphereSampleUniform(float u, float v)
{
	float phi = v * 2.0 * PI;
	float cosTheta = 1.0 - u;
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float3 HemisphereSampleCos(float u, float v)
{
	float phi = v * 2.0 * PI;
	float cosTheta = sqrt(1.0 - u);
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float4 QuatMul(float4 q1, float4 q2)
{
	return float4(
		q2.xyz * q1.w + q1.xyz * q2.w + cross(q1.xyz, q2.xyz),
		q1.w * q2.w - dot(q1.xyz, q2.xyz)
		);
}

float4 QuatFromAngleAxis(float angle, float3 axis)
{
	float sn = sin(angle * 0.5);
	float cs = cos(angle * 0.5);
	return float4(axis * sn, cs);
}

float4 QuatFromTwoVector(float3 v1, float3 v2)
{
	float4 q;
	float d = dot(v1, v2);
	if (d < -0.999999)
	{
		float3 right = float3(1, 0, 0);
		float3 up = float3(0, 1, 0);
		float3 tmp = cross(right, v1);
		if (length(tmp) < 0.000001)
		{
			tmp = cross(up, v1);
		}
		tmp = normalize(tmp);
		q = QuatFromAngleAxis(PI, tmp);
	}
	else if (d > 0.999999)
	{
		q = float4(0, 0, 0, 1);
	}
	else
	{
		q.xyz = cross(v1, v2);
		q.w = 1 + d;
		q = normalize(q);
	}
	return q;
}

float3 QuatRotVector(float3 v, float4 r)
{
	float4 r_c = r * float4(-1, -1, -1, 1);
	return QuatMul(r, QuatMul(float4(v, 0), r_c)).xyz;
}

float3 SkyLight(float3 dir)
{
	float t = dir.y * 0.5 + 0.5;
	return lerp(cbLight.ambientGround, cbLight.ambientSky, t) * cbLight.ambientIntensity;
}

[shader("raygeneration")]
void PathTracerRGS()
{
	uint2 PixelPos = DispatchRaysIndex().xy;
	float2 xy = (float2)PixelPos + 0.5;
	float2 clipSpacePos = xy / float2(DispatchRaysDimensions().xy) * float2(2, -2) + float2(-1, 1);

	float4 worldPos = mul(cbScene.mtxProjToWorld, float4(clipSpacePos, 1, 1));
	worldPos.xyz /= worldPos.w;

	float3 origin = cbScene.eyePosition.xyz;
	float3 direction = normalize(worldPos.xyz - origin);

	const int kSampleCount = cbPathTrace.sampleCount;
	const int kDepth = cbPathTrace.depthMax;
	const int N = kSampleCount * kDepth;

	float3 color = 0;
	float3 albedo = 0;
	float3 normal = float3(0, 0, 1);
	for (int sample = 0; sample < kSampleCount; sample++)
	{
		// primary ray.
		RayDesc ray = { origin, 0.0, direction, RayTMax };
		MaterialPayload payload = (MaterialPayload)0;

		float3 reflectivity = 1;
		for (int depth = 0; depth < kDepth; depth++)
		{
			TraceRay(TLAS, RAY_FLAG_NONE, ~0, 0, 1, 0, ray, payload);
			if (payload.hitT >= 0.0)
			{
				MaterialParam matParam;
				DecodeMaterialPayload(payload, matParam);

				// shadow ray.
				float3 hitP = ray.Origin + ray.Direction * payload.hitT + matParam.normal * 1e-3;
				RayDesc sray = { hitP, 0.0, cbLight.directionalVec, RayTMax };
				TraceRay(TLAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, ~0, 0, 1, 0, sray, payload);
				float shadowMask = payload.hitT < 0 ? 1.0 : 0.0;

				float3 diffuse = lerp(matParam.baseColor, 0, matParam.metallic);
				float3 specular = lerp(0.04, matParam.baseColor, matParam.metallic);
				float3 directLight = BrdfGGX(diffuse, specular, matParam.roughness, matParam.normal, cbLight.directionalVec, -ray.Direction);
				color += reflectivity * (matParam.emissive + directLight * cbLight.directionalColor * shadowMask);
				reflectivity *= diffuse;

				ray.Origin = hitP;
				uint i = PixelPos.x + PixelPos.y * sample + depth;
				float2 uv = Noise(PixelPos, sample * kDepth + depth);
				float3 localDir = HemisphereSampleUniform(uv.x, uv.y);
				float4 qRot = QuatFromTwoVector(float3(0, 0, 1), matParam.normal);
				ray.Direction = QuatRotVector(localDir, qRot);

				if (depth == 0)
				{
					albedo = matParam.baseColor;
					normal = matParam.normal;
				}
			}
			else
			{
				color += reflectivity * SkyLight(ray.Direction);
				break;
			}
		}
	}
	color *= (1.0 / (float)kSampleCount);

	uint index = PixelPos.y * DispatchRaysDimensions().x + PixelPos.x;
	uint address = index * 4/* sizeof(float) */ * 3;
	rtResult.Store3(address, asuint(color));
	rtAlbedo.Store3(address, asuint(albedo));
	rtNormal.Store3(address, asuint(normal));
}

[shader("miss")]
void PathTracerMS(inout MaterialPayload payload : SV_RayPayload)
{
	payload.hitT = -1.0;
}

// EOF
