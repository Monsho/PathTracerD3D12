#ifndef CBUFFER_HLSLI
#define CBUFFER_HLSLI

#ifdef USE_IN_CPP
#	define		float4x4		DirectX::XMFLOAT4X4
#	define		float4			DirectX::XMFLOAT4
#	define		float3			DirectX::XMFLOAT3
#	define		float2			DirectX::XMFLOAT2
#	define		uint			UINT
#endif

struct SceneCB
{
	float4x4	mtxWorldToProj;
	float4x4	mtxWorldToView;
	float4x4	mtxViewToProj;
	float4x4	mtxProjToWorld;
	float4x4	mtxViewToWorld;
	float4x4	mtxProjToView;
	float4x4	mtxProjToPrevProj;
	float4x4	mtxPrevViewToProj;
	float4		eyePosition;
	float2		screenSize;
	float2		invScreenSize;
	float2		nearFar;
};

struct LightCB
{
	float3		ambientSky;
	int			pad0;
	float3		ambientGround;
	float		ambientIntensity;
	float3		directionalVec;
	int			pad1;
	float3		directionalColor;
	int			pad2;
};

struct PathTraceCB
{
	int			sampleCount;
	int			depthMax;
};

struct SubmeshOffsetCB
{
	uint	position;
	uint	normal;
	uint	tangent;
	uint	texcoord;
	uint	index;
};

struct DebugCB
{
	uint	displayMode;
};

struct InstanceData
{
	float4x4	mtxLocalToWorld;
	float4x4	mtxWorldToLocal;
};

struct SubmeshData
{
	uint	materialIndex;
	uint	posOffset;
	uint	normalOffset;
	uint	tangentOffset;
	uint	uvOffset;
	uint	indexOffset;
};

struct DrawCallData
{
	uint	instanceIndex;
	uint	submeshIndex;
};

#define CLASSIFY_TILE_WIDTH (64)
#define CLASSIFY_THREAD_WIDTH (16)
#define CLASSIFY_MATERIAL_MAX (256)
#define CLASSIFY_DEPTH_RANGE (CLASSIFY_MATERIAL_MAX * 32)

#define SHADOW_TYPE 0

#endif // CBUFFER_HLSLI
//  EOF