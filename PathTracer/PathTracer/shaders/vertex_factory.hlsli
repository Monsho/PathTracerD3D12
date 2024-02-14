#ifndef VERTEX_FACTORY_HLSLI
#define VERTEX_FACTORY_HLSLI

uint3 GetTriangleIndices2byte(ByteAddressBuffer indexBuffer, uint offset)
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

uint3 GetVertexIndices16(in ByteAddressBuffer rIndexBuffer, in uint startOffset, in uint triIndex)
{
	uint address = startOffset + triIndex * 2 * 3;
	return GetTriangleIndices2byte(rIndexBuffer, address);
}

uint3 GetVertexIndices32(in ByteAddressBuffer rIndexBuffer, in uint startOffset, in uint triIndex)
{
	uint address = startOffset + triIndex * 3 * 4;
	return rIndexBuffer.Load3(address);
}

float SNormToFloat(int v, float scale)
{
	float scaledV = (float)v * scale;
	return max(scaledV, -1.0);
}

float4 SNorm8ToFloat32_Vector(uint v)
{
	const float kScale = 1.0 / 127.0;
	float4 ret = float4(
		SNormToFloat(asint(v << 24) >> 24, kScale),
		SNormToFloat(asint((v << 16) & 0xff000000) >> 24, kScale),
		SNormToFloat(asint((v << 8) & 0xff000000) >> 24, kScale),
		SNormToFloat(asint(v & 0xff000000) >> 24, kScale)
		);
	return ret;
}

float3 GetVertexPosition(in ByteAddressBuffer rVertexBuffer, in uint startOffset, in uint index)
{
	const float kScale = 1.0 / 32767.0;
	uint address = startOffset + index * 8;
	uint2 up = rVertexBuffer.Load2(address);
	float3 ret = float3(
		SNormToFloat(asint(up.x << 16) >> 16, kScale),
		SNormToFloat(asint(up.x & 0xffff0000) >> 16, kScale),
		SNormToFloat(asint(up.y << 16) >> 16, kScale)
		);
	return ret;
}

float3 GetVertexNormal(in ByteAddressBuffer rVertexBuffer, in uint startOffset, in uint index)
{
	uint address = startOffset + index * 4;
	uint up = rVertexBuffer.Load(address);
	return SNorm8ToFloat32_Vector(up).xyz;
}

float4 GetVertexTangent(in ByteAddressBuffer rVertexBuffer, in uint startOffset, in uint index)
{
	uint address = startOffset + index * 4;
	uint up = rVertexBuffer.Load(address);
	return SNorm8ToFloat32_Vector(up);
}

float2 GetVertexTexcoord(in ByteAddressBuffer rVertexBuffer, in uint startOffset, in uint index)
{
	uint address = startOffset + index * 4;
	uint up = rVertexBuffer.Load(address);
	float2 ret = float2(
		f16tof32(up),
		f16tof32(up >> 16)
		);
	return ret;
}


#endif // VERTEX_FACTORY_HLSLI
