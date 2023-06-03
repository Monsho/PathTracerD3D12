#include "cbuffer.hlsli"
#include "math.hlsli"

struct PSInput
{
	float4	position	: SV_POSITION;
	float2	uv			: TEXCOORD;
};

struct PSOutput
{
	float4	color	: SV_TARGET0;
};

ConstantBuffer<SceneCB>	cbScene		: register(b0);
ByteAddressBuffer		rRTResult	: register(t0);

PSOutput main(PSInput In)
{
	PSOutput Out = (PSOutput)0;

	uint2 PixelPos = uint2(In.position.xy);
	uint index = PixelPos.y * uint(cbScene.screenSize.x) + PixelPos.x;
	uint address = index * 4 * 3;
	
	Out.color = float4(pow(asfloat(rRTResult.Load3(address)), 1/2.2), 1);

	return Out;
}
