#include "cbuffer.hlsli"
#include "payload.hlsli"
#include "vertex_factory.hlsli"

#if !ENABLE_DYNAMIC_RESOURCE

// local
ConstantBuffer<SubmeshOffsetCB>	cbSubmesh		: register(b0, space1);
ByteAddressBuffer				Indices			: register(t0, space1);
ByteAddressBuffer				Vertices		: register(t1, space1);
Texture2D						texBaseColor	: register(t2, space1);
Texture2D						texORM			: register(t3, space1);
SamplerState					texBaseColor_s	: register(s0, space1);

#else

struct LocalIndex
{
	uint cbSubmesh;
	uint Indices;
	uint Vertices;
	uint texBaseColor;
	uint texORM;
	uint texBaseColor_s;
};

ConstantBuffer<LocalIndex>	cbLocalIndices	: register(b1, space0);

#endif

[shader("closesthit")]
void MaterialCHS(inout MaterialPayload payload : SV_RayPayload, in BuiltInTriangleIntersectionAttributes attr : SV_IntersectionAttributes)
{
#if ENABLE_DYNAMIC_RESOURCE
	// get dynamic resources.
	ConstantBuffer<SubmeshOffsetCB> cbSubmesh = ResourceDescriptorHeap[cbLocalIndices.cbSubmesh];
	ByteAddressBuffer Indices = ResourceDescriptorHeap[cbLocalIndices.Indices];
	ByteAddressBuffer Vertices = ResourceDescriptorHeap[cbLocalIndices.Vertices];
	Texture2D texBaseColor = ResourceDescriptorHeap[cbLocalIndices.texBaseColor];
	Texture2D texORM = ResourceDescriptorHeap[cbLocalIndices.texORM];
	SamplerState texBaseColor_s = SamplerDescriptorHeap[cbLocalIndices.texBaseColor_s];
#endif

	uint3 indices = GetVertexIndices32(Indices, cbSubmesh.index, PrimitiveIndex());

	float2 uvs[3] = {
		GetVertexTexcoord(Vertices, cbSubmesh.texcoord, indices.x),
		GetVertexTexcoord(Vertices, cbSubmesh.texcoord, indices.y),
		GetVertexTexcoord(Vertices, cbSubmesh.texcoord, indices.z),
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
		GetVertexNormal(Vertices, cbSubmesh.normal, indices.x),
		GetVertexNormal(Vertices, cbSubmesh.normal, indices.y),
		GetVertexNormal(Vertices, cbSubmesh.normal, indices.z),
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
#if ENABLE_DYNAMIC_RESOURCE
	// get dynamic resources.
	ConstantBuffer<SubmeshOffsetCB> cbSubmesh = ResourceDescriptorHeap[cbLocalIndices.cbSubmesh];
	ByteAddressBuffer Indices = ResourceDescriptorHeap[cbLocalIndices.Indices];
	ByteAddressBuffer Vertices = ResourceDescriptorHeap[cbLocalIndices.Vertices];
	Texture2D texBaseColor = ResourceDescriptorHeap[cbLocalIndices.texBaseColor];
	Texture2D texORM = ResourceDescriptorHeap[cbLocalIndices.texORM];
	SamplerState texBaseColor_s = SamplerDescriptorHeap[cbLocalIndices.texBaseColor_s];
#endif

	uint3 indices = GetVertexIndices32(Indices, cbSubmesh.index, PrimitiveIndex());

	float2 uvs[3] = {
		GetVertexTexcoord(Vertices, cbSubmesh.texcoord, indices.x),
		GetVertexTexcoord(Vertices, cbSubmesh.texcoord, indices.y),
		GetVertexTexcoord(Vertices, cbSubmesh.texcoord, indices.z),
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
