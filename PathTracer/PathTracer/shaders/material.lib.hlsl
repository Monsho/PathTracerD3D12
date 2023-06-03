#include "cbuffer.hlsli"
#include "payload.hlsli"

// local
ConstantBuffer<SubmeshOffsetCB>	cbSubmesh		: register(b0, space1);
ByteAddressBuffer				Indices			: register(t0, space1);
ByteAddressBuffer				Vertices		: register(t1, space1);
Texture2D						texBaseColor	: register(t2, space1);
Texture2D						texORM			: register(t3, space1);
SamplerState					texBaseColor_s	: register(s0, space1);

uint3 GetTriangleIndices2byte(uint offset, ByteAddressBuffer indexBuffer)
{
	uint alignedOffset = offset & ~0x3;
	uint2 indices4 = indexBuffer.Load2(alignedOffset);

	uint3 ret;
	if (alignedOffset == offset)
	{
		ret.x = indices4.x & 0xffff;
		ret.y = (indices4.x >> 16) & 0xffff;
		ret.z = indices4.y & 0xffff;
	}
	else
	{
		ret.x = (indices4.x >> 16) & 0xffff;
		ret.y = indices4.y & 0xffff;
		ret.z = (indices4.y >> 16) & 0xffff;
	}

	return ret;
}

uint3 Get16bitIndices(uint primIdx, uint startOffset, ByteAddressBuffer indexBuffer)
{
	uint indexOffset = startOffset + primIdx * 2 * 3;
	return GetTriangleIndices2byte(indexOffset, indexBuffer);
}

uint3 Get32bitIndices(uint primIdx, uint startOffset, ByteAddressBuffer indexBuffer)
{
	uint indexOffset = startOffset + primIdx * 4 * 3;
	uint3 ret = indexBuffer.Load3(indexOffset);
	return ret;
}

float3 GetFloat3Attribute(uint vertIdx, uint startOffset, ByteAddressBuffer vertexBuffer)
{
	uint offset = startOffset + vertIdx * 4 * 3;
	float3 ret = asfloat(vertexBuffer.Load3(offset));
	return ret;
}

float2 GetFloat2Attribute(uint vertIdx, uint startOffset, ByteAddressBuffer vertexBuffer)
{
	uint offset = startOffset + vertIdx * 4 * 2;
	float2 ret = asfloat(vertexBuffer.Load2(offset));
	return ret;
}

[shader("closesthit")]
void MaterialCHS(inout MaterialPayload payload : SV_RayPayload, in BuiltInTriangleIntersectionAttributes attr : SV_IntersectionAttributes)
{
	uint3 indices = Get32bitIndices(PrimitiveIndex(), cbSubmesh.index, Indices);

	float2 uvs[3] = {
		GetFloat2Attribute(indices.x, cbSubmesh.texcoord, Vertices),
		GetFloat2Attribute(indices.y, cbSubmesh.texcoord, Vertices),
		GetFloat2Attribute(indices.z, cbSubmesh.texcoord, Vertices),
	};
	float2 uv = uvs[0] +
		attr.barycentrics.x * (uvs[1] - uvs[0]) +
		attr.barycentrics.y * (uvs[2] - uvs[0]);

	MaterialParam param = (MaterialParam)0;
	param.hitT = RayTCurrent();

	param.baseColor = texBaseColor.SampleLevel(texBaseColor_s, uv, 0.0);
	float4 orm = texORM.SampleLevel(texBaseColor_s, uv, 0.0);
	param.roughness = max(0.01, orm.g);
	param.metallic = orm.b;

	param.emissive = 0.0;

	float3 ns[3] = {
		GetFloat3Attribute(indices.x, cbSubmesh.normal, Vertices),
		GetFloat3Attribute(indices.y, cbSubmesh.normal, Vertices),
		GetFloat3Attribute(indices.z, cbSubmesh.normal, Vertices),
	};
	param.normal = ns[0] +
		attr.barycentrics.x * (ns[1] - ns[0]) +
		attr.barycentrics.y * (ns[2] - ns[0]);

	param.flag = 0;
	param.flag |= (HitKind() == HIT_KIND_TRIANGLE_BACK_FACE) ? kFlagBackFaceHit : 0;

	EncodeMaterialPayload(param, payload);
}

[shader("anyhit")]
void MaterialAHS(inout MaterialPayload payload : SV_RayPayload, in BuiltInTriangleIntersectionAttributes attr : SV_IntersectionAttributes)
{
	uint3 indices = Get32bitIndices(PrimitiveIndex(), cbSubmesh.index, Indices);

	float2 uvs[3] = {
		GetFloat2Attribute(indices.x, cbSubmesh.texcoord, Vertices),
		GetFloat2Attribute(indices.y, cbSubmesh.texcoord, Vertices),
		GetFloat2Attribute(indices.z, cbSubmesh.texcoord, Vertices),
	};
	float2 uv = uvs[0] +
		attr.barycentrics.x * (uvs[1] - uvs[0]) +
		attr.barycentrics.y * (uvs[2] - uvs[0]);

	float opacity = texBaseColor.SampleLevel(texBaseColor_s, uv, 0.0).a;
	if (opacity < 0.33)
	{
		IgnoreHit();
	}
}


// EOF
