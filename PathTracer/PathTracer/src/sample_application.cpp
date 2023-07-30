#include "sample_application.h"

#include "sl12/resource_mesh.h"
#include "sl12/string_util.h"
#include "sl12/root_signature.h"
#include "sl12/descriptor_set.h"
#include "sl12/resource_texture.h"
#include "sl12/command_queue.h"

#define NOMINMAX
#include <windowsx.h>
#include <memory>
#include <random>

#define USE_IN_CPP
#include "../shaders/cbuffer.hlsli"

#define ENABLE_DYNAMIC_RESOURCE 0

namespace
{
	static const float kFovY = 90.0f;
	
	static const char* kResourceDir = "resources";
	static const char* kShaderDir = "PathTracer/shaders";
	static const char* kShaderIncludeDir = "../SampleLib12/SampleLib12/shaders/include";

	static const sl12::u32 kShadowMapSize = 1024;

	static const int kRTMaterialTableCount = 1;

	static sl12::RenderGraphTargetDesc gRTResultDesc;
	void SetGBufferDesc(sl12::u32 width, sl12::u32 height)
	{
		gRTResultDesc.name = "RT";
		gRTResultDesc.type = sl12::RenderGraphTargetType::Buffer;
		gRTResultDesc.width = width * height * sizeof(float) * 3;
		gRTResultDesc.usage = sl12::ResourceUsage::ShaderResource | sl12::ResourceUsage::UnorderedAccess;
		gRTResultDesc.srvDescs.push_back(sl12::RenderGraphSRVDesc(0, 0, 0));
		gRTResultDesc.uavDescs.push_back(sl12::RenderGraphUAVDesc(0, 0, 0));
	}

	enum ShaderName
	{
		FullscreenVV,
		TonemapP,
		MaterialLib,
		PathTracerLib,

		MAX
	};
	static const char* kShaderFileAndEntry[] = {
		"fullscreen.vv.hlsl",				"main",
		"tonemap.p.hlsl",					"main",
		"material.lib.hlsl",				"main",
		"pathtracer.lib.hlsl",				"main",
	};

	static const sl12::RaytracingDescriptorCount kRTDescriptorCountGlobal = {
		3,	// cbv
		0,	// srv
		3,	// uav
		0,	// sampler
	};
	static const sl12::RaytracingDescriptorCount kRTDescriptorCountLocal = {
		1,	// cbv
		4,	// srv
		0,	// uav
		1,	// sampler
	};

	static const sl12::u32 kGlobalIndexCount = 6;
	static const sl12::u32 kLocalIndexCount = 6;

	static LPCWSTR kMaterialCHS = L"MaterialCHS";
	static LPCWSTR kMaterialAHS = L"MaterialAHS";
	static LPCWSTR kMaterialOpacityHG = L"MaterialOpacityHG";
	static LPCWSTR kMaterialMaskedHG = L"MaterialMaskedHG";
	static LPCWSTR kPathTracerRGS = L"PathTracerRGS";
	static LPCWSTR kPathTracerMS = L"PathTracerMS";
}

SampleApplication::SampleApplication(HINSTANCE hInstance, int nCmdShow, int screenWidth, int screenHeight, sl12::ColorSpaceType csType, const std::string& homeDir, int meshType)
	: Application(hInstance, nCmdShow, screenWidth, screenHeight, csType)
	, displayWidth_(screenWidth), displayHeight_(screenHeight)
	, meshType_(meshType)
{
	std::filesystem::path p(homeDir);
	p = std::filesystem::absolute(p);
	homeDir_ = p.string();
}

SampleApplication::~SampleApplication()
{}

bool SampleApplication::Initialize()
{
	if (!InitializeOIDN())
	{
		return false;
	}
	
	// initialize mesh manager.
	const size_t kVertexBufferSize = 512 * 1024 * 1024;		// 512MB
	const size_t kIndexBufferSize = 64 * 1024 * 1024;		// 64MB
	meshMan_ = sl12::MakeUnique<sl12::MeshManager>(&device_, &device_, kVertexBufferSize, kIndexBufferSize);
	
	// initialize resource loader.
	resLoader_ = sl12::MakeUnique<sl12::ResourceLoader>(nullptr);
	if (!resLoader_->Initialize(&device_, &meshMan_, sl12::JoinPath(homeDir_, kResourceDir)))
	{
		sl12::ConsolePrint("Error: failed to init resource loader.");
		return false;
	}

	// initialize shader manager.
	std::vector<std::string> shaderIncludeDirs;
	shaderIncludeDirs.push_back(sl12::JoinPath(homeDir_, kShaderIncludeDir));
	shaderMan_ = sl12::MakeUnique<sl12::ShaderManager>(nullptr);
	if (!shaderMan_->Initialize(&device_, &shaderIncludeDirs))
	{
		sl12::ConsolePrint("Error: failed to init shader manager.");
		return false;
	}

	// compile shaders.
	const std::string shaderBaseDir = sl12::JoinPath(homeDir_, kShaderDir);
	std::vector<sl12::ShaderDefine> shaderDefines;
	shaderDefines.push_back(sl12::ShaderDefine("ENABLE_DYNAMIC_RESOURCE", ENABLE_DYNAMIC_RESOURCE ? "1" : "0"));
	for (int i = 0; i < ShaderName::MAX; i++)
	{
		const char* file = kShaderFileAndEntry[i * 2 + 0];
		const char* entry = kShaderFileAndEntry[i * 2 + 1];
		auto handle = shaderMan_->CompileFromFile(
			sl12::JoinPath(shaderBaseDir, file),
			entry, sl12::GetShaderTypeFromFileName(file), 6, 6, nullptr, &shaderDefines);
		hShaders_.push_back(handle);
	}
	
	// load request.
	if (meshType_ == 0)
	{
		hSuzanneMesh_ = resLoader_->LoadRequest<sl12::ResourceItemMesh>("mesh/hp_suzanne/hp_suzanne.rmesh");
	}
	else
	{
		hSponzaMesh_ = resLoader_->LoadRequest<sl12::ResourceItemMesh>("mesh/sponza/sponza.rmesh");
		hSphereMesh_ = resLoader_->LoadRequest<sl12::ResourceItemMesh>("mesh/sphere/sphere.rmesh");
		hTitleMesh_ = resLoader_->LoadRequest<sl12::ResourceItemMesh>("mesh/title/title.rmesh");
	}
	hDetailTex_ = resLoader_->LoadRequest<sl12::ResourceItemTexture>("texture/detail_normal.dds");
	hDotTex_ = resLoader_->LoadRequest<sl12::ResourceItemTexture>("texture/dot_normal.dds");

	// init command list.
	mainCmdList_ = sl12::MakeUnique<CommandLists>(nullptr);
	if (!mainCmdList_->Initialize(&device_, &device_.GetGraphicsQueue()))
	{
		sl12::ConsolePrint("Error: failed to init main command list.");
		return false;
	}

	// init cbv manager.
	cbvMan_ = sl12::MakeUnique<sl12::CbvManager>(nullptr, &device_);

	// init render graph.
	renderGraph_ = sl12::MakeUnique<sl12::RenderGraph>(nullptr);

	// init bvh manager.
	bvhMan_ = sl12::MakeUnique<sl12::BvhManager>(nullptr, &device_);

	// init scene root.
	sceneRoot_ = sl12::MakeUnique<sl12::SceneRoot>(nullptr);

	// get GBuffer target descs.
	SetGBufferDesc(displayWidth_, displayHeight_);
	
	// create sampler.
	{
		linearSampler_ = sl12::MakeUnique<sl12::Sampler>(&device_);

		D3D12_SAMPLER_DESC desc{};
		desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		desc.MaxLOD = FLT_MAX;
		desc.MinLOD = 0.0f;
		desc.MipLODBias = 0.0f;
		linearSampler_->Initialize(&device_, desc);
	}
	
	// init utility command list.
	auto utilCmdList = sl12::MakeUnique<sl12::CommandList>(&device_);
	utilCmdList->Initialize(&device_, &device_.GetGraphicsQueue());
	utilCmdList->Reset();

	// init GUI.
	gui_ = sl12::MakeUnique<sl12::Gui>(nullptr);
	if (!gui_->Initialize(&device_, device_.GetSwapchain().GetTexture(0)->GetResourceDesc().Format))
	{
		sl12::ConsolePrint("Error: failed to init GUI.");
		return false;
	}
	if (!gui_->CreateFontImage(&device_, &utilCmdList))
	{
		sl12::ConsolePrint("Error: failed to create GUI font.");
		return false;
	}

	// create dummy texture.
	if (!device_.CreateDummyTextures(&utilCmdList))
	{
		return false;
	}

	// execute utility commands.
	utilCmdList->Close();
	utilCmdList->Execute();
	device_.WaitDrawDone();

	// wait compile and load.
	while (shaderMan_->IsCompiling() || resLoader_->IsLoading())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	
	// create scene meshes.
	if (meshType_ == 0)
	{
		static const int kMeshWidth = 32;
		static const float kMeshInter = 100.0f;
		static const float kMeshOrigin = -(kMeshWidth - 1) * kMeshInter * 0.5f;
		std::random_device seed_gen;
		std::mt19937 rnd(seed_gen());
		auto RandRange = [&rnd](float minV, float maxV)
		{
			sl12::u32 val = rnd();
			float v0_1 = (float)val / (float)0xffffffff;
			return minV + (maxV - minV) * v0_1;
		};
		for (int x = 0; x < kMeshWidth; x++)
		{
			for (int y = 0; y < kMeshWidth; y++)
			{
				auto mesh = std::make_shared<sl12::SceneMesh>(&device_, hSuzanneMesh_.GetItem<sl12::ResourceItemMesh>());
				DirectX::XMFLOAT3 pos(kMeshOrigin + x * kMeshInter, RandRange(-100.0f, 100.0f), kMeshOrigin + y * kMeshInter);
				DirectX::XMFLOAT4X4 mat;
				DirectX::XMMATRIX m = DirectX::XMMatrixRotationRollPitchYaw(RandRange(-DirectX::XM_PI, DirectX::XM_PI), RandRange(-DirectX::XM_PI, DirectX::XM_PI), RandRange(-DirectX::XM_PI, DirectX::XM_PI))
										* DirectX::XMMatrixTranslation(pos.x, pos.y, pos.z);
				DirectX::XMStoreFloat4x4(&mat, m);
				mesh->SetMtxLocalToWorld(mat);

				sceneMeshes_.push_back(mesh);
			}
		}
	}
	else
	{
		// sponza
		{
			auto mesh = std::make_shared<sl12::SceneMesh>(&device_, hSponzaMesh_.GetItem<sl12::ResourceItemMesh>());
			DirectX::XMFLOAT3 pos(0.0f, -300.0f, 100.0f);
			DirectX::XMFLOAT3 scl(0.02f, 0.02f, 0.02f);
			DirectX::XMFLOAT4X4 mat;
			DirectX::XMMATRIX m = DirectX::XMMatrixScaling(scl.x, scl.y, scl.z)
									* DirectX::XMMatrixTranslation(pos.x, pos.y, pos.z);
			DirectX::XMStoreFloat4x4(&mat, m);
			mesh->SetMtxLocalToWorld(mat);

			sceneMeshes_.push_back(mesh);
		}
		// title
		{
			auto mesh = std::make_shared<sl12::SceneMesh>(&device_, hTitleMesh_.GetItem<sl12::ResourceItemMesh>());
			DirectX::XMFLOAT3 pos(400.0f, 1000.0f, 40.0f);
			DirectX::XMFLOAT3 scl(2.5f, 2.5f, 2.5f);
			DirectX::XMFLOAT3 rot(0.0f, DirectX::XMConvertToRadians(90.0f), 0.0f);
			DirectX::XMFLOAT4X4 mat;
			DirectX::XMMATRIX m = DirectX::XMMatrixScaling(scl.x, scl.y, scl.z)
									* DirectX::XMMatrixRotationY(DirectX::XMConvertToRadians(90.0f))
									* DirectX::XMMatrixTranslation(pos.x, pos.y, pos.z);
			DirectX::XMStoreFloat4x4(&mat, m);
			mesh->SetMtxLocalToWorld(mat);

			sceneMeshes_.push_back(mesh);
		}
	}
	ComputeSceneAABB();

	// attach meshes to root.
	for (auto&& m : sceneMeshes_)
	{
		sceneRoot_->AttachNode(m);
	}
	
	// init root signature and pipeline state.
	rsVsPs_ = sl12::MakeUnique<sl12::RootSignature>(&device_);
	rsTonemapDR_ = sl12::MakeUnique<sl12::RootSignature>(&device_);
	psoTonemap_ = sl12::MakeUnique<sl12::GraphicsPipelineState>(&device_);
	rsVsPs_->Initialize(&device_, hShaders_[ShaderName::FullscreenVV].GetShader(), hShaders_[ShaderName::TonemapP].GetShader(), nullptr, nullptr, nullptr);
	{
		sl12::GraphicsPipelineStateDesc desc{};
		desc.pRootSignature = &rsVsPs_;
		desc.pVS = hShaders_[ShaderName::FullscreenVV].GetShader();
		desc.pPS = hShaders_[ShaderName::TonemapP].GetShader();

		desc.blend.sampleMask = UINT_MAX;
		desc.blend.rtDesc[0].isBlendEnable = false;
		desc.blend.rtDesc[0].writeMask = 0xf;

		desc.rasterizer.cullMode = D3D12_CULL_MODE_NONE;
		desc.rasterizer.fillMode = D3D12_FILL_MODE_SOLID;
		desc.rasterizer.isDepthClipEnable = true;
		desc.rasterizer.isFrontCCW = true;

		desc.depthStencil.isDepthEnable = false;
		desc.depthStencil.isDepthWriteEnable = false;

		desc.primTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		desc.numRTVs = 0;
		desc.rtvFormats[desc.numRTVs++] = device_.GetSwapchain().GetTexture(0)->GetResourceDesc().Format;
		desc.dsvFormat = DXGI_FORMAT_UNKNOWN;
		desc.multisampleCount = 1;

#if !ENABLE_DYNAMIC_RESOURCE
		if (!psoTonemap_->Initialize(&device_, desc))
		{
			sl12::ConsolePrint("Error: failed to init tonemap pso.");
			return false;
		}
#else
		rsTonemapDR_->InitializeWithDynamicResource(&device_, 0, 2, 0, 0, 0);
		desc.pRootSignature = &rsTonemapDR_;

		if (!psoTonemap_->Initialize(&device_, desc))
		{
			sl12::ConsolePrint("Error: failed to init tonemap pso.");
			return false;
		}
#endif
	}
	
	if (!CreateRaytracingPipeline())
	{
		return false;
	}

	for (auto&& t : timestamps_)
	{
		t.Initialize(&device_, 16);
	}

	cameraPos_ = DirectX::XMFLOAT3(1000.0f, 1000.0f, 0.0f);
	cameraDir_ = DirectX::XMFLOAT3(-1.0f, 0.0f, 0.0f);
	lastMouseX_ = lastMouseY_ = 0;
	return true;
}

void SampleApplication::Finalize()
{
	// wait render.
	device_.WaitDrawDone();
	device_.Present(1);

	DestroyOIDN();

	// destroy render objects.
	OffsetCBVs_.clear();
	for (auto&& t : timestamps_) t.Destroy();
	gui_.Reset();
	psoRayTracing_.Reset();
	psoPathTracer_.Reset();
	psoMaterialCollection_.Reset();
	psoTonemap_.Reset();
	rsRTGlobal_.Reset();
	rsRTLocal_.Reset();
	rsCs_.Reset();
	rsVsPs_.Reset();
	bvhMan_.Reset();
	renderGraph_.Reset();
	cbvMan_.Reset();
	mainCmdList_.Reset();
	shaderMan_.Reset();
	resLoader_.Reset();
}

bool SampleApplication::Execute()
{
	const int kSwapchainBufferOffset = 1;
	auto frameIndex = (device_.GetSwapchain().GetFrameIndex() + sl12::Swapchain::kMaxBuffer - 1) % sl12::Swapchain::kMaxBuffer;
	auto prevFrameIndex = (device_.GetSwapchain().GetFrameIndex() + sl12::Swapchain::kMaxBuffer - 2) % sl12::Swapchain::kMaxBuffer;
	auto pCmdList = &mainCmdList_->Reset();
	auto* pTimestamp = timestamps_ + timestampIndex_;

	sl12::CpuTimer now = sl12::CpuTimer::CurrentTime();
	sl12::CpuTimer delta = now - currCpuTime_;
	currCpuTime_ = now;

	// control camera.
	ControlCamera(delta.ToSecond());

	// debug menu.
	gui_->BeginNewFrame(pCmdList, displayWidth_, displayHeight_, inputData_);
	inputData_.Reset();
	{
		// path trace settings.
		if (ImGui::CollapsingHeader("Path Trace", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Denoise", &bDenoiseEnable_);
			ImGui::SliderInt("Sample Count", &ptSampleCount_, 1, 16);
			ImGui::SliderInt("Depth Max", &ptDepthMax_, 1, 16);
		}

		// light settings.
		if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::ColorEdit3("Ambient Sky Color", skyColor_);
			ImGui::ColorEdit3("Ambient Ground Color", groundColor_);
			ImGui::SliderFloat("Ambient Intensity", &ambientIntensity_, 0.0f, 10.0f);
			ImGui::SliderFloat("Directional Theta", &directionalTheta_, 0.0f, 90.0f);
			ImGui::SliderFloat("Directional Phi", &directionalPhi_, 0.0f, 360.0f);
			ImGui::ColorEdit3("Directional Color", directionalColor_);
			ImGui::SliderFloat("Directional Intensity", &directionalIntensity_, 0.0f, 10.0f);
		}
	}
	ImGui::Render();

	device_.WaitPresent();
	device_.SyncKillObjects();

	pTimestamp->Reset();
	pTimestamp->Query(pCmdList);
	device_.LoadRenderCommands(pCmdList);

	cbvMan_->BeginNewFrame();
	renderGraph_->BeginNewFrame();
	meshMan_->BeginNewFrame(pCmdList);
	sceneRoot_->BeginNewFrame(pCmdList);

	// gather mesh render commands.
	sl12::RenderCommandsList meshRenderCmds;
	{
		sceneRoot_->GatherRenderCommands(&cbvMan_, meshRenderCmds);
	}

	// add ray tracing geometries.
	for (auto&& cmd : meshRenderCmds)
	{
		if (cmd->GetType() == sl12::RenderCommandType::Mesh)
		{
			bvhMan_->AddGeometry(static_cast<sl12::MeshRenderCommand*>(cmd.get()));
		}
	}

	// build ray tracing assets.
	sl12::BvhScene* pBvhScene = nullptr;
	{
		// build BVH.
		bvhMan_->BuildGeometry(pCmdList);
		sl12::RenderCommandsTempList tmpRenderCmds;
		pBvhScene = bvhMan_->BuildScene(pCmdList, meshRenderCmds, kRTMaterialTableCount, tmpRenderCmds);
		bvhMan_->CopyCompactionInfoOnGraphicsQueue(pCmdList);

		// create ray tracing shader table.
#if !ENABLE_DYNAMIC_RESOURCE
		bool bCreateRTShaderTableSuccess = CreateRayTracingShaderTable(pCmdList, tmpRenderCmds);
		assert(bCreateRTShaderTableSuccess);
#else
		bool bCreateRTShaderTableDRSuccess = CreateRayTracingShaderTableDR(pCmdList, tmpRenderCmds);
		assert(bCreateRTShaderTableDRSuccess);
#endif
	}

	// create targets.
	sl12::RenderGraphTargetID rtResultID, rtAlbedoID, rtNormalID;
	rtResultID = renderGraph_->AddTarget(gRTResultDesc);
	rtAlbedoID = renderGraph_->AddTarget(gRTResultDesc);
	rtNormalID = renderGraph_->AddTarget(gRTResultDesc);

	// create render passes.
	{
		std::vector<sl12::RenderPass> passes;
		std::vector<sl12::RenderGraphTargetID> histories;

		// path tracing.
		sl12::RenderPass ptPass{};
		ptPass.output.push_back(rtResultID);
		ptPass.output.push_back(rtAlbedoID);
		ptPass.output.push_back(rtNormalID);
		ptPass.outputStates.push_back(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ptPass.outputStates.push_back(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ptPass.outputStates.push_back(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		passes.push_back(ptPass);
		
		// tonemap pass.
		sl12::RenderPass tonemapPass{};
		tonemapPass.input.push_back(rtResultID);
		tonemapPass.input.push_back(rtAlbedoID);
		tonemapPass.input.push_back(rtNormalID);
		tonemapPass.inputStates.push_back(D3D12_RESOURCE_STATE_GENERIC_READ);
		tonemapPass.inputStates.push_back(D3D12_RESOURCE_STATE_GENERIC_READ);
		tonemapPass.inputStates.push_back(D3D12_RESOURCE_STATE_GENERIC_READ);
		passes.push_back(tonemapPass);

		renderGraph_->CreateRenderPasses(&device_, passes, histories);
	}

	// create scene constant buffer.
	sl12::CbvHandle hSceneCB, hLightCB, hPathTraceCB;
	{
		DirectX::XMFLOAT3 upVec(0.0f, 1.0f, 0.0f);
		float Zn = 0.1f;
		auto cp = DirectX::XMLoadFloat3(&cameraPos_);
		auto dir = DirectX::XMLoadFloat3(&cameraDir_);
		auto up = DirectX::XMLoadFloat3(&upVec);
		auto mtxWorldToView = DirectX::XMMatrixLookAtRH(cp, DirectX::XMVectorAdd(cp, dir), up);
		auto mtxViewToClip = sl12::MatrixPerspectiveInfiniteInverseFovRH(DirectX::XMConvertToRadians(kFovY), (float)displayWidth_ / (float)displayHeight_, Zn);
		auto mtxWorldToClip = mtxWorldToView * mtxViewToClip;
		auto mtxClipToWorld = DirectX::XMMatrixInverse(nullptr, mtxWorldToClip);
		auto mtxViewToWorld = DirectX::XMMatrixInverse(nullptr, mtxWorldToView);
		auto mtxClipToView = DirectX::XMMatrixInverse(nullptr, mtxViewToClip);

		SceneCB cbScene;
		DirectX::XMStoreFloat4x4(&cbScene.mtxWorldToProj, mtxWorldToClip);
		DirectX::XMStoreFloat4x4(&cbScene.mtxWorldToView, mtxWorldToView);
		DirectX::XMStoreFloat4x4(&cbScene.mtxViewToProj, mtxViewToClip);
		DirectX::XMStoreFloat4x4(&cbScene.mtxProjToWorld, mtxClipToWorld);
		DirectX::XMStoreFloat4x4(&cbScene.mtxViewToWorld, mtxViewToWorld);
		DirectX::XMStoreFloat4x4(&cbScene.mtxProjToView, mtxClipToView);
		if (frameIndex_ == 0)
		{
			// first frame.
			auto U = DirectX::XMMatrixIdentity();
			DirectX::XMStoreFloat4x4(&cbScene.mtxProjToPrevProj, U);
			DirectX::XMStoreFloat4x4(&cbScene.mtxPrevViewToProj, mtxViewToClip);
		}
		else
		{
			auto mtxClipToPrevClip = mtxClipToWorld * mtxPrevWorldToClip_;
			DirectX::XMStoreFloat4x4(&cbScene.mtxProjToPrevProj, mtxClipToPrevClip);
			DirectX::XMStoreFloat4x4(&cbScene.mtxPrevViewToProj, mtxPrevViewToClip_);
		}
		cbScene.eyePosition.x = cameraPos_.x;
		cbScene.eyePosition.y = cameraPos_.y;
		cbScene.eyePosition.z = cameraPos_.z;
		cbScene.eyePosition.w = 0.0f;
		cbScene.screenSize.x = (float)displayWidth_;
		cbScene.screenSize.y = (float)displayHeight_;
		cbScene.invScreenSize.x = 1.0f / (float)displayWidth_;
		cbScene.invScreenSize.y = 1.0f / (float)displayHeight_;
		cbScene.nearFar.x = Zn;
		cbScene.nearFar.y = 0.0f;

		hSceneCB = cbvMan_->GetTemporal(&cbScene, sizeof(cbScene));

		mtxPrevWorldToView_ = mtxWorldToView;
		mtxPrevWorldToClip_ = mtxWorldToClip;
		mtxPrevViewToClip_ = mtxViewToClip;
	}
	{
		LightCB cbLight;
		
		memcpy(&cbLight.ambientSky, skyColor_, sizeof(cbLight.ambientSky));
		memcpy(&cbLight.ambientGround, groundColor_, sizeof(cbLight.ambientGround));
		cbLight.ambientIntensity = ambientIntensity_;

		auto dir = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
		auto mtxRot = DirectX::XMMatrixRotationZ(DirectX::XMConvertToRadians(directionalTheta_)) * DirectX::XMMatrixRotationY(DirectX::XMConvertToRadians(directionalPhi_));
		dir = DirectX::XMVector3TransformNormal(dir, mtxRot);
		DirectX::XMFLOAT3 dirF3;
		DirectX::XMStoreFloat3(&dirF3, dir);
		memcpy(&cbLight.directionalVec, &dirF3, sizeof(cbLight.directionalVec));
		cbLight.directionalColor.x = directionalColor_[0] * directionalIntensity_;
		cbLight.directionalColor.y = directionalColor_[1] * directionalIntensity_;
		cbLight.directionalColor.z = directionalColor_[2] * directionalIntensity_;

		hLightCB = cbvMan_->GetTemporal(&cbLight, sizeof(cbLight));
	}
	{
		PathTraceCB cbPT;

		cbPT.sampleCount = ptSampleCount_;
		cbPT.depthMax = ptDepthMax_;

		hPathTraceCB = cbvMan_->GetTemporal(&cbPT, sizeof(cbPT));
	}

	// clear swapchain.
	auto&& swapchain = device_.GetSwapchain();
	pCmdList->TransitionBarrier(swapchain.GetCurrentTexture(kSwapchainBufferOffset), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	{
		float color[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
		pCmdList->GetLatestCommandList()->ClearRenderTargetView(swapchain.GetCurrentRenderTargetView(kSwapchainBufferOffset)->GetDescInfo().cpuHandle, color, 0, nullptr);
	}

	// path tracing.
	renderGraph_->NextPass(pCmdList);
#if !ENABLE_DYNAMIC_RESOURCE
	{
		GPU_MARKER(pCmdList, 1, "PathTracing");
		
		// output barrier.
		renderGraph_->BarrierOutputsAll(pCmdList);

		// デスクリプタを設定
		sl12::DescriptorSet descSet;
		descSet.Reset();
		descSet.SetCsCbv(0, hSceneCB.GetCBV()->GetDescInfo().cpuHandle);
		descSet.SetCsCbv(1, hLightCB.GetCBV()->GetDescInfo().cpuHandle);
		descSet.SetCsCbv(2, hPathTraceCB.GetCBV()->GetDescInfo().cpuHandle);
		descSet.SetCsUav(0, renderGraph_->GetTarget(rtResultID)->uavs[0]->GetDescInfo().cpuHandle);
		descSet.SetCsUav(1, renderGraph_->GetTarget(rtAlbedoID)->uavs[0]->GetDescInfo().cpuHandle);
		descSet.SetCsUav(2, renderGraph_->GetTarget(rtNormalID)->uavs[0]->GetDescInfo().cpuHandle);

		// コピーしつつコマンドリストに積む
		D3D12_GPU_VIRTUAL_ADDRESS as_address[] = {
			pBvhScene->GetGPUAddress(),
		};
		pCmdList->SetRaytracingGlobalRootSignatureAndDescriptorSet(&rsRTGlobal_, &descSet, &rtDescMan_, as_address, ARRAYSIZE(as_address));

		// レイトレースを実行
		D3D12_DISPATCH_RAYS_DESC desc{};
		desc.HitGroupTable.StartAddress = MaterialHGTable_->GetResourceDep()->GetGPUVirtualAddress();
		desc.HitGroupTable.SizeInBytes = MaterialHGTable_->GetBufferDesc().size;
		desc.HitGroupTable.StrideInBytes = bvhShaderRecordSize_;
		desc.MissShaderTable.StartAddress = PathTracerMSTable_->GetResourceDep()->GetGPUVirtualAddress();
		desc.MissShaderTable.SizeInBytes = PathTracerMSTable_->GetBufferDesc().size;
		desc.MissShaderTable.StrideInBytes = bvhShaderRecordSize_;
		desc.RayGenerationShaderRecord.StartAddress = PathTracerRGSTable_->GetResourceDep()->GetGPUVirtualAddress();
		desc.RayGenerationShaderRecord.SizeInBytes = PathTracerRGSTable_->GetBufferDesc().size;
		desc.Width = displayWidth_;
		desc.Height = displayHeight_;
		desc.Depth = 1;
		pCmdList->GetDxrCommandList()->SetPipelineState1(psoRayTracing_->GetPSO());
		pCmdList->GetDxrCommandList()->DispatchRays(&desc);
	}
#else
	{
		GPU_MARKER(pCmdList, 1, "PathTracingDR");
		
		// output barrier.
		renderGraph_->BarrierOutputsAll(pCmdList);

		// set global resource index.
		struct GlobalIndex
		{
			uint cbScene;
			uint cbLight;
			uint cbPathTrace;
			uint rtResult;
			uint rtAlbedo;
			uint rtNormal;
		};
		std::vector<sl12::u32> globalIndices;
		globalIndices.resize(6);
		globalIndices[0] = hSceneCB.GetCBV()->GetDynamicDescInfo().index;
		globalIndices[1] = hLightCB.GetCBV()->GetDynamicDescInfo().index;
		globalIndices[2] = hPathTraceCB.GetCBV()->GetDynamicDescInfo().index;
		globalIndices[3] = renderGraph_->GetTarget(rtResultID)->uavs[0]->GetDynamicDescInfo().index;
		globalIndices[4] = renderGraph_->GetTarget(rtAlbedoID)->uavs[0]->GetDynamicDescInfo().index;
		globalIndices[5] = renderGraph_->GetTarget(rtNormalID)->uavs[0]->GetDynamicDescInfo().index;

		// load to command list.
		D3D12_GPU_VIRTUAL_ADDRESS as_address[] = {
			pBvhScene->GetGPUAddress(),
		};
		pCmdList->SetRaytracingGlobalRootSignatureAndDynamicResource(&rsRTGlobal_, as_address, ARRAYSIZE(as_address), globalIndices);

		// レイトレースを実行
		D3D12_DISPATCH_RAYS_DESC desc{};
		desc.HitGroupTable.StartAddress = MaterialHGTable_->GetResourceDep()->GetGPUVirtualAddress();
		desc.HitGroupTable.SizeInBytes = MaterialHGTable_->GetBufferDesc().size;
		desc.HitGroupTable.StrideInBytes = bvhShaderRecordSize_;
		desc.MissShaderTable.StartAddress = PathTracerMSTable_->GetResourceDep()->GetGPUVirtualAddress();
		desc.MissShaderTable.SizeInBytes = PathTracerMSTable_->GetBufferDesc().size;
		desc.MissShaderTable.StrideInBytes = bvhShaderRecordSize_;
		desc.RayGenerationShaderRecord.StartAddress = PathTracerRGSTable_->GetResourceDep()->GetGPUVirtualAddress();
		desc.RayGenerationShaderRecord.SizeInBytes = PathTracerRGSTable_->GetBufferDesc().size;
		desc.Width = displayWidth_;
		desc.Height = displayHeight_;
		desc.Depth = 1;
		pCmdList->GetDxrCommandList()->SetPipelineState1(psoRayTracing_->GetPSO());
		pCmdList->GetDxrCommandList()->DispatchRays(&desc);
	}
#endif
	renderGraph_->EndPass();

	pCmdList->SetDescriptorHeapDirty();
	
	// tonemap pass.
	pTimestamp->Query(pCmdList);
	renderGraph_->NextPass(pCmdList);
	{
		GPU_MARKER(pCmdList, 3, "TonemapPass");

		// output barrier.
		renderGraph_->BarrierOutputsAll(pCmdList);

		// copy path tracing result.
		CopyNoisyResource(pCmdList,
			&renderGraph_->GetTarget(rtResultID)->buffer,
			&renderGraph_->GetTarget(rtAlbedoID)->buffer,
			&renderGraph_->GetTarget(rtNormalID)->buffer);

		// set render targets.
		auto&& rtv = swapchain.GetCurrentRenderTargetView(kSwapchainBufferOffset)->GetDescInfo().cpuHandle;
		pCmdList->GetLatestCommandList()->OMSetRenderTargets(1, &rtv, false, nullptr);

		// set viewport.
		D3D12_VIEWPORT vp;
		vp.TopLeftX = vp.TopLeftY = 0.0f;
		vp.Width = (float)displayWidth_;
		vp.Height = (float)displayHeight_;
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		pCmdList->GetLatestCommandList()->RSSetViewports(1, &vp);

		// set scissor rect.
		D3D12_RECT rect;
		rect.left = rect.top = 0;
		rect.right = displayWidth_;
		rect.bottom = displayHeight_;
		pCmdList->GetLatestCommandList()->RSSetScissorRects(1, &rect);

		// set pipeline.
		pCmdList->GetLatestCommandList()->SetPipelineState(psoTonemap_->GetPSO());
		pCmdList->GetLatestCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

#if !ENABLE_DYNAMIC_RESOURCE
		// set descriptors.
		sl12::DescriptorSet descSet;
		descSet.Reset();
		descSet.SetPsCbv(0, hSceneCB.GetCBV()->GetDescInfo().cpuHandle);
		if (bDenoiseEnable_)
		{
			descSet.SetPsSrv(0, denoiseResultSRV_->GetDescInfo().cpuHandle);
		}
		else
		{
			descSet.SetPsSrv(0, renderGraph_->GetTarget(rtResultID)->bufferSrvs[0]->GetDescInfo().cpuHandle);
		}

		pCmdList->SetGraphicsRootSignatureAndDescriptorSet(&rsVsPs_, &descSet);
#else
		std::vector<std::vector<sl12::u32>> resIndices;
		resIndices.resize(1);
		resIndices[0].resize(2);
		resIndices[0][0] = hSceneCB.GetCBV()->GetDynamicDescInfo().index;
		resIndices[0][1] = bDenoiseEnable_ ? denoiseResultSRV_->GetDynamicDescInfo().index : renderGraph_->GetTarget(rtResultID)->bufferSrvs[0]->GetDynamicDescInfo().index;

		pCmdList->SetGraphicsRootSignatureAndDynamicResource(&rsTonemapDR_, resIndices);
#endif

		// draw fullscreen.
		pCmdList->GetLatestCommandList()->DrawInstanced(3, 1, 0, 0);
	}
	renderGraph_->EndPass();

	pCmdList->SetDescriptorHeapDirty();

	// draw GUI.
	pTimestamp->Query(pCmdList);
	gui_->LoadDrawCommands(pCmdList);
	
	// barrier swapchain.
	pCmdList->TransitionBarrier(swapchain.GetCurrentTexture(kSwapchainBufferOffset), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

	// graphics timestamp end.
	pTimestamp->Query(pCmdList);
	pTimestamp->Resolve(pCmdList);
	timestampIndex_ = 1 - timestampIndex_;

	// wait prev frame render.
	mainCmdList_->Close();
	device_.WaitDrawDone();

	// present swapchain.
	device_.Present(1);

	// kill TLAS.
	device_.KillObject(pBvhScene);

	// execute oidn denoise.
	if (bDenoiseEnable_)
		ExecuteDenoise();
	
	// execute current frame render.
	mainCmdList_->Execute();

	frameIndex_++;

	return true;
}

int SampleApplication::Input(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_LBUTTONDOWN:
		inputData_.mouseButton |= sl12::MouseButton::Left;
		return 0;
	case WM_RBUTTONDOWN:
		inputData_.mouseButton |= sl12::MouseButton::Right;
		return 0;
	case WM_MBUTTONDOWN:
		inputData_.mouseButton |= sl12::MouseButton::Middle;
		return 0;
	case WM_LBUTTONUP:
		inputData_.mouseButton &= ~sl12::MouseButton::Left;
		return 0;
	case WM_RBUTTONUP:
		inputData_.mouseButton &= ~sl12::MouseButton::Right;
		return 0;
	case WM_MBUTTONUP:
		inputData_.mouseButton &= ~sl12::MouseButton::Middle;
		return 0;
	case WM_MOUSEMOVE:
		inputData_.mouseX = GET_X_LPARAM(lParam);
		inputData_.mouseY = GET_Y_LPARAM(lParam);
		return 0;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		inputData_.key = wParam;
		inputData_.scancode = (int)LOBYTE(HIWORD(lParam));;
		inputData_.keyDown = false;
		return 0;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		inputData_.key = wParam;
		inputData_.scancode = (int)LOBYTE(HIWORD(lParam));;
		inputData_.keyDown = true;
		return 0;
	case WM_CHAR:
		inputData_.chara = (sl12::u16)wParam;
		return 0;
	}

	return 0;
}

void SampleApplication::ControlCamera(float deltaTime)
{
	const float kCameraMoveSpeed = 300.0f;
	const float kCameraRotSpeed = 10.0f;
	float x = 0.0f, y = 0.0f, z = 0.0f;
	float rx = 0.0f, ry = 0.0f;
	if (GetKeyState('W') < 0)
	{
		z = 1.0f;
	}
	else if (GetKeyState('S') < 0)
	{
		z = -1.0f;
	}
	if (GetKeyState('A') < 0)
	{
		x = -1.0f;
	}
	else if (GetKeyState('D') < 0)
	{
		x = 1.0f;
	}
	if (GetKeyState('Q') < 0)
	{
		y = -1.0f;
	}
	else if (GetKeyState('E') < 0)
	{
		y = 1.0f;
	}

	if (inputData_.mouseButton & sl12::MouseButton::Right)
	{
		rx = -(float)(inputData_.mouseY - lastMouseY_);
		ry = -(float)(inputData_.mouseX - lastMouseX_);
	}
	lastMouseX_ = inputData_.mouseX;
	lastMouseY_ = inputData_.mouseY;

	DirectX::XMFLOAT3 upVec(0.0f, 1.0f, 0.0f);
	auto cp = DirectX::XMLoadFloat3(&cameraPos_);
	auto c_forward = DirectX::XMLoadFloat3(&cameraDir_);
	auto c_right = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(c_forward, DirectX::XMLoadFloat3(&upVec)));
	auto mtxRot = DirectX::XMMatrixMultiply(DirectX::XMMatrixRotationAxis(c_right, DirectX::XMConvertToRadians(rx * kCameraRotSpeed) * deltaTime), DirectX::XMMatrixRotationY(DirectX::XMConvertToRadians(ry * kCameraRotSpeed) * deltaTime));
	c_forward = DirectX::XMVector4Transform(c_forward, mtxRot);
	cp = DirectX::XMVectorAdd(cp, DirectX::XMVectorScale(c_forward, z * kCameraMoveSpeed * deltaTime));
	cp = DirectX::XMVectorAdd(cp, DirectX::XMVectorScale(c_right, x * kCameraMoveSpeed * deltaTime));
	cp = DirectX::XMVectorAdd(cp, DirectX::XMVectorSet(0.0f, y * kCameraMoveSpeed * deltaTime, 0.0f, 0.0f));
	DirectX::XMStoreFloat3(&cameraPos_, cp);
	DirectX::XMStoreFloat3(&cameraDir_, c_forward);
}

void SampleApplication::ComputeSceneAABB()
{
	DirectX::XMFLOAT3 aabbMax(-FLT_MAX, -FLT_MAX, -FLT_MAX), aabbMin(FLT_MAX, FLT_MAX, FLT_MAX);
	for (auto&& mesh : sceneMeshes_)
	{
		auto mtx = DirectX::XMLoadFloat4x4(&mesh->GetMtxLocalToWorld());
		auto&& bound = mesh->GetParentResource()->GetBoundingInfo();
		DirectX::XMFLOAT3 pnts[] = {
			DirectX::XMFLOAT3(bound.box.aabbMax.x, bound.box.aabbMax.y, bound.box.aabbMax.z),
			DirectX::XMFLOAT3(bound.box.aabbMax.x, bound.box.aabbMax.y, bound.box.aabbMin.z),
			DirectX::XMFLOAT3(bound.box.aabbMax.x, bound.box.aabbMin.y, bound.box.aabbMax.z),
			DirectX::XMFLOAT3(bound.box.aabbMax.x, bound.box.aabbMin.y, bound.box.aabbMin.z),
			DirectX::XMFLOAT3(bound.box.aabbMin.x, bound.box.aabbMax.y, bound.box.aabbMax.z),
			DirectX::XMFLOAT3(bound.box.aabbMin.x, bound.box.aabbMax.y, bound.box.aabbMin.z),
			DirectX::XMFLOAT3(bound.box.aabbMin.x, bound.box.aabbMin.y, bound.box.aabbMax.z),
			DirectX::XMFLOAT3(bound.box.aabbMin.x, bound.box.aabbMin.y, bound.box.aabbMin.z),
		};
		for (auto pnt : pnts)
		{
			auto p = DirectX::XMLoadFloat3(&pnt);
			p = DirectX::XMVector3TransformCoord(p, mtx);
			DirectX::XMStoreFloat3(&pnt, p);

			aabbMax.x = std::max(pnt.x, aabbMax.x);
			aabbMax.y = std::max(pnt.y, aabbMax.y);
			aabbMax.z = std::max(pnt.z, aabbMax.z);
			aabbMin.x = std::min(pnt.x, aabbMin.x);
			aabbMin.y = std::min(pnt.y, aabbMin.y);
			aabbMin.z = std::min(pnt.z, aabbMin.z);
		}
	}
	sceneAABBMax_ = aabbMax;
	sceneAABBMin_ = aabbMin;
}

bool SampleApplication::CreateRaytracingPipeline()
{
	static const int kPayloadSize = 32;

	// create root signature.
	// only one fixed root signature.
	rsRTGlobal_ = sl12::MakeUnique<sl12::RootSignature>(&device_);
	rsRTLocal_ = sl12::MakeUnique<sl12::RootSignature>(&device_);
#if !ENABLE_DYNAMIC_RESOURCE
	if (!sl12::CreateRaytracingRootSignature(&device_,
		1,		// AS count
		kRTDescriptorCountGlobal,
		kRTDescriptorCountLocal,
		&rsRTGlobal_, &rsRTLocal_))
	{
		return false;
	}
#else
	if (!sl12::CreateRayTracingRootSignatureWithDynamicResource(&device_,
		1, kGlobalIndexCount, kLocalIndexCount,
		&rsRTGlobal_, &rsRTLocal_))
	{
		return false;
	}
#endif

	// create collection.
	{
		sl12::DxrPipelineStateDesc dxrDesc;

		// export shader from library.
		auto shader = hShaders_[MaterialLib].GetShader();
		D3D12_EXPORT_DESC libExport[] = {
			{ kMaterialCHS,	nullptr, D3D12_EXPORT_FLAG_NONE },
			{ kMaterialAHS,	nullptr, D3D12_EXPORT_FLAG_NONE },
		};
		dxrDesc.AddDxilLibrary(shader->GetData(), shader->GetSize(), libExport, ARRAYSIZE(libExport));

		// hit group.
		dxrDesc.AddHitGroup(kMaterialOpacityHG, true, nullptr, kMaterialCHS, nullptr);
		dxrDesc.AddHitGroup(kMaterialMaskedHG, true, kMaterialAHS, kMaterialCHS, nullptr);

		// payload size and intersection attr size.
		dxrDesc.AddShaderConfig(kPayloadSize, sizeof(float) * 2);

		// global root signature.
		dxrDesc.AddGlobalRootSignature(*(&rsRTGlobal_));

		// TraceRay recursive count.
		dxrDesc.AddRaytracinConfig(1);

		// local root signature.
		// if use only one root signature, do not need export association.
		dxrDesc.AddLocalRootSignatureAndExportAssociation(*(&rsRTLocal_), nullptr, 0);

		// PSO生成
		psoMaterialCollection_ = sl12::MakeUnique<sl12::DxrPipelineState>(&device_);
		if (!psoMaterialCollection_->Initialize(&device_, dxrDesc, D3D12_STATE_OBJECT_TYPE_COLLECTION))
		{
			return false;
		}
	}
	{
		sl12::DxrPipelineStateDesc dxrDesc;

		// export shader from library.
		auto shader = hShaders_[PathTracerLib].GetShader();
		D3D12_EXPORT_DESC libExport[] = {
			{ kPathTracerRGS,	nullptr, D3D12_EXPORT_FLAG_NONE },
			{ kPathTracerMS,	nullptr, D3D12_EXPORT_FLAG_NONE },
		};
		dxrDesc.AddDxilLibrary(shader->GetData(), shader->GetSize(), libExport, ARRAYSIZE(libExport));

		// payload size and intersection attr size.
		dxrDesc.AddShaderConfig(kPayloadSize, sizeof(float) * 2);

		// global root signature.
		dxrDesc.AddGlobalRootSignature(*(&rsRTGlobal_));

		// TraceRay recursive count.
		dxrDesc.AddRaytracinConfig(1);

		// PSO生成
		psoPathTracer_ = sl12::MakeUnique<sl12::DxrPipelineState>(&device_);
		if (!psoPathTracer_->Initialize(&device_, dxrDesc, D3D12_STATE_OBJECT_TYPE_COLLECTION))
		{
			return false;
		}
	}
	{
		sl12::DxrPipelineStateDesc dxrDesc;

		// payload size and intersection attr size.
		dxrDesc.AddShaderConfig(kPayloadSize, sizeof(float) * 2);

		// global root signature.
		dxrDesc.AddGlobalRootSignature(*(&rsRTGlobal_));

		// TraceRay recursive count.
		dxrDesc.AddRaytracinConfig(1);

		// hit group collection.
		dxrDesc.AddExistingCollection(psoMaterialCollection_->GetPSO(), nullptr, 0);
		dxrDesc.AddExistingCollection(psoPathTracer_->GetPSO(), nullptr, 0);

		// PSO生成
		psoRayTracing_ = sl12::MakeUnique<sl12::DxrPipelineState>(&device_);
		if (!psoRayTracing_->Initialize(&device_, dxrDesc))
		{
			return false;
		}
	}

	return true;
}

bool SampleApplication::CreateRayTracingShaderTable(sl12::CommandList* pCmdList, sl12::RenderCommandsTempList& tcmds)
{
	// already created.
	if (MaterialHGTable_.IsValid())
	{
		return true;
	}

	// count materials and create submesh vertex/index offset.
	sl12::u32 totalMaterialCount = 0;
	for (auto&& cmd : tcmds)
	{
		if (cmd->GetType() == sl12::RenderCommandType::Mesh)
		{
			// count materials.
			auto mcmd = static_cast<sl12::MeshRenderCommand*>(cmd);
			totalMaterialCount += (sl12::u32)mcmd->GetSubmeshCommands().size();

			// create offset cbv.
			auto res = mcmd->GetParentMesh()->GetParentResource();
			if (OffsetCBVs_.find(res) == OffsetCBVs_.end())
			{
				//MeshShapeOffset offsets;
				OffsetCBVs_[res].resize(res->GetSubmeshes().size());
				//OffsetCBVs_[res] = offsets;

				auto&& v = OffsetCBVs_[res];
				int idx = 0;
				for (auto&& submesh : res->GetSubmeshes())
				{
					SubmeshOffsetCB cb;
					cb.position = (UINT)(res->GetPositionHandle().offset + submesh.positionOffsetBytes);
					cb.normal = (UINT)(res->GetNormalHandle().offset + submesh.normalOffsetBytes);
					cb.tangent = (UINT)(res->GetTangentHandle().offset + submesh.tangentOffsetBytes);
					cb.texcoord = (UINT)(res->GetTexcoordHandle().offset + submesh.texcoordOffsetBytes);
					cb.index = (UINT)(res->GetIndexHandle().offset + submesh.indexOffsetBytes);
					
					auto h = cbvMan_->GetResident(sizeof(cb));
					cbvMan_->RequestResidentCopy(h, &cb, sizeof(cb));
					v[idx] = std::move(h);
					idx++;
				}
			}
		}
	}
	cbvMan_->ExecuteCopy(pCmdList);

	// initialize descriptor manager.
	rtDescMan_ = sl12::MakeUnique<sl12::RaytracingDescriptorManager>(&device_);
	if (!rtDescMan_->Initialize(&device_,
		1,		// Render Count
		1,		// AS Count
		kRTDescriptorCountGlobal,
		kRTDescriptorCountLocal,
		totalMaterialCount))
	{
		return false;
	}

	// create local shader resource table.
	struct LocalTable
	{
		D3D12_GPU_DESCRIPTOR_HANDLE	cbv;
		D3D12_GPU_DESCRIPTOR_HANDLE	srv;
		D3D12_GPU_DESCRIPTOR_HANDLE	sampler;
	};
	std::vector<LocalTable> material_table;
	std::vector<bool> opaque_table;
	auto view_desc_size = rtDescMan_->GetViewDescSize();
	auto sampler_desc_size = rtDescMan_->GetSamplerDescSize();
	auto local_handle_start = rtDescMan_->IncrementLocalHandleStart();
	auto FillMeshTable = [&](sl12::MeshRenderCommand* cmd)
	{
		auto pSceneMesh = cmd->GetParentMesh();
		auto pMeshItem = pSceneMesh->GetParentResource();
		auto&& submeshes = pMeshItem->GetSubmeshes();
		for (int i = 0; i < submeshes.size(); i++)
		{
			auto&& submesh = submeshes[i];
			auto&& material = pMeshItem->GetMaterials()[submesh.materialIndex];
			auto bc_srv = device_.GetDummyTextureView(sl12::DummyTex::White);
			auto orm_srv = device_.GetDummyTextureView(sl12::DummyTex::White);
			if (material.baseColorTex.IsValid())
			{
				auto pTexBC = const_cast<sl12::ResourceItemTexture*>(material.baseColorTex.GetItem<sl12::ResourceItemTexture>());
				bc_srv = &pTexBC->GetTextureView();
			}
			if (material.ormTex.IsValid())
			{
				auto pTexORM = const_cast<sl12::ResourceItemTexture*>(material.ormTex.GetItem<sl12::ResourceItemTexture>());
				orm_srv = &pTexORM->GetTextureView();
			}

			opaque_table.push_back(material.isOpaque);

			LocalTable table;

			// CBV
			D3D12_CPU_DESCRIPTOR_HANDLE cbv[] = {
				OffsetCBVs_[pMeshItem][i].GetCBV()->GetDescInfo().cpuHandle,
			};
			sl12::u32 cbv_cnt = ARRAYSIZE(cbv);
			device_.GetDeviceDep()->CopyDescriptors(
				1, &local_handle_start.viewCpuHandle, &cbv_cnt,
				cbv_cnt, cbv, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			table.cbv = local_handle_start.viewGpuHandle;
			local_handle_start.viewCpuHandle.ptr += view_desc_size * cbv_cnt;
			local_handle_start.viewGpuHandle.ptr += view_desc_size * cbv_cnt;

			// SRV
			D3D12_CPU_DESCRIPTOR_HANDLE srv[] = {
				meshMan_->GetIndexBufferSRV()->GetDescInfo().cpuHandle,
				meshMan_->GetVertexBufferSRV()->GetDescInfo().cpuHandle,
				bc_srv->GetDescInfo().cpuHandle,
				orm_srv->GetDescInfo().cpuHandle,
			};
			sl12::u32 srv_cnt = ARRAYSIZE(srv);
			device_.GetDeviceDep()->CopyDescriptors(
				1, &local_handle_start.viewCpuHandle, &srv_cnt,
				srv_cnt, srv, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			table.srv = local_handle_start.viewGpuHandle;
			local_handle_start.viewCpuHandle.ptr += view_desc_size * srv_cnt;
			local_handle_start.viewGpuHandle.ptr += view_desc_size * srv_cnt;

			// Samplerは1つ
			D3D12_CPU_DESCRIPTOR_HANDLE sampler[] = {
				linearSampler_->GetDescInfo().cpuHandle,
			};
			sl12::u32 sampler_cnt = ARRAYSIZE(sampler);
			device_.GetDeviceDep()->CopyDescriptors(
				1, &local_handle_start.samplerCpuHandle, &sampler_cnt,
				sampler_cnt, sampler, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
			table.sampler = local_handle_start.samplerGpuHandle;
			local_handle_start.samplerCpuHandle.ptr += sampler_desc_size * sampler_cnt;
			local_handle_start.samplerGpuHandle.ptr += sampler_desc_size * sampler_cnt;

			material_table.push_back(table);
		}
	};
	for (auto&& cmd : tcmds)
	{
		if (cmd->GetType() == sl12::RenderCommandType::Mesh)
		{
			FillMeshTable(static_cast<sl12::MeshRenderCommand*>(cmd));
		}
	}

	// create shader table.
	auto Align = [](UINT size, UINT align)
	{
		return ((size + align - 1) / align) * align;
	};
	UINT shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
	UINT descHandleOffset = Align(shaderIdentifierSize, sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));
	UINT shaderRecordSize = Align(descHandleOffset + sizeof(LocalTable), D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
	bvhShaderRecordSize_ = shaderRecordSize;

	auto GenShaderTable = [&](
		void* const * shaderIds,
		int tableCountPerMaterial,
		sl12::UniqueHandle<sl12::Buffer>& buffer,
		int materialCount)
	{
		buffer = sl12::MakeUnique<sl12::Buffer>(&device_);

		materialCount = (materialCount < 0) ? (int)material_table.size() : materialCount;
		sl12::BufferDesc desc{};
		desc.heap = sl12::BufferHeap::Dynamic;
		desc.size = shaderRecordSize * tableCountPerMaterial * materialCount;
		desc.usage = sl12::ResourceUsage::ShaderResource;
		desc.initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
		if (!buffer->Initialize(&device_, desc))
		{
			return false;
		}

		auto p = (char*)buffer->Map();
		for (int i = 0; i < materialCount; ++i)
		{
			for (int id = 0; id < tableCountPerMaterial; ++id)
			{
				auto start = p;

				memcpy(p, shaderIds[i * tableCountPerMaterial + id], shaderIdentifierSize);
				p += descHandleOffset;

				memcpy(p, &material_table[i], sizeof(LocalTable));

				p = start + shaderRecordSize;
			}
		}
		buffer->Unmap();

		return true;
	};
	// material shader table.
	{
		void* hg_identifier[4];
		{
			ID3D12StateObjectProperties* prop;
			psoRayTracing_->GetPSO()->QueryInterface(IID_PPV_ARGS(&prop));
			hg_identifier[0] = prop->GetShaderIdentifier(kMaterialOpacityHG);
			hg_identifier[1] = prop->GetShaderIdentifier(kMaterialMaskedHG);
			prop->Release();
		}
		std::vector<void*> hg_table;
		for (auto v : opaque_table)
		{
			hg_table.push_back(v ? hg_identifier[0] : hg_identifier[1]);
		}
		if (!GenShaderTable(hg_table.data(), kRTMaterialTableCount, MaterialHGTable_, -1))
		{
			return false;
		}
	}
	// for PathTracer.
	{
		void* rgs_identifier;
		void* ms_identifier;
		{
			ID3D12StateObjectProperties* prop;
			psoRayTracing_->GetPSO()->QueryInterface(IID_PPV_ARGS(&prop));
			rgs_identifier = prop->GetShaderIdentifier(kPathTracerRGS);
			ms_identifier = prop->GetShaderIdentifier(kPathTracerMS);
			prop->Release();
		}
		if (!GenShaderTable(&rgs_identifier, 1, PathTracerRGSTable_, 1))
		{
			return false;
		}
		if (!GenShaderTable(&ms_identifier, 1, PathTracerMSTable_, 1))
		{
			return false;
		}
	}

	return true;
}

bool SampleApplication::CreateRayTracingShaderTableDR(sl12::CommandList* pCmdList, sl12::RenderCommandsTempList& tcmds)
{
	// already created.
	if (MaterialHGTable_.IsValid())
	{
		return true;
	}

	// count materials and create submesh vertex/index offset.
	sl12::u32 totalMaterialCount = 0;
	for (auto&& cmd : tcmds)
	{
		if (cmd->GetType() == sl12::RenderCommandType::Mesh)
		{
			// count materials.
			auto mcmd = static_cast<sl12::MeshRenderCommand*>(cmd);
			totalMaterialCount += (sl12::u32)mcmd->GetSubmeshCommands().size();

			// create offset cbv.
			auto res = mcmd->GetParentMesh()->GetParentResource();
			if (OffsetCBVs_.find(res) == OffsetCBVs_.end())
			{
				OffsetCBVs_[res].resize(res->GetSubmeshes().size());

				auto&& v = OffsetCBVs_[res];
				int idx = 0;
				for (auto&& submesh : res->GetSubmeshes())
				{
					SubmeshOffsetCB cb;
					cb.position = (UINT)(res->GetPositionHandle().offset + submesh.positionOffsetBytes);
					cb.normal = (UINT)(res->GetNormalHandle().offset + submesh.normalOffsetBytes);
					cb.tangent = (UINT)(res->GetTangentHandle().offset + submesh.tangentOffsetBytes);
					cb.texcoord = (UINT)(res->GetTexcoordHandle().offset + submesh.texcoordOffsetBytes);
					cb.index = (UINT)(res->GetIndexHandle().offset + submesh.indexOffsetBytes);
					
					auto h = cbvMan_->GetResident(sizeof(cb));
					cbvMan_->RequestResidentCopy(h, &cb, sizeof(cb));
					v[idx] = std::move(h);
					idx++;
				}
			}
		}
	}
	cbvMan_->ExecuteCopy(pCmdList);

	// create local shader resource table.
	struct LocalIndex
	{
		uint cbSubmesh;
		uint Indices;
		uint Vertices;
		uint texBaseColor;
		uint texORM;
		uint texBaseColor_s;
	};
	std::vector<LocalIndex> material_table;
	std::vector<bool> opaque_table;
	auto FillMeshTable = [&](sl12::MeshRenderCommand* cmd)
	{
		auto pSceneMesh = cmd->GetParentMesh();
		auto pMeshItem = pSceneMesh->GetParentResource();
		auto&& submeshes = pMeshItem->GetSubmeshes();
		for (int i = 0; i < submeshes.size(); i++)
		{
			auto&& submesh = submeshes[i];
			auto&& material = pMeshItem->GetMaterials()[submesh.materialIndex];
			auto bc_srv = device_.GetDummyTextureView(sl12::DummyTex::White);
			auto orm_srv = device_.GetDummyTextureView(sl12::DummyTex::White);
			if (material.baseColorTex.IsValid())
			{
				auto pTexBC = const_cast<sl12::ResourceItemTexture*>(material.baseColorTex.GetItem<sl12::ResourceItemTexture>());
				bc_srv = &pTexBC->GetTextureView();
			}
			if (material.ormTex.IsValid())
			{
				auto pTexORM = const_cast<sl12::ResourceItemTexture*>(material.ormTex.GetItem<sl12::ResourceItemTexture>());
				orm_srv = &pTexORM->GetTextureView();
			}

			opaque_table.push_back(material.isOpaque);

			LocalIndex localIndex;
			localIndex.cbSubmesh = OffsetCBVs_[pMeshItem][i].GetCBV()->GetDynamicDescInfo().index;
			localIndex.Indices = meshMan_->GetIndexBufferSRV()->GetDynamicDescInfo().index;
			localIndex.Vertices = meshMan_->GetVertexBufferSRV()->GetDynamicDescInfo().index;
			localIndex.texBaseColor = bc_srv->GetDynamicDescInfo().index;
			localIndex.texORM = orm_srv->GetDynamicDescInfo().index;
			localIndex.texBaseColor_s = linearSampler_->GetDynamicDescInfo().index;
			
			material_table.push_back(localIndex);
		}
	};
	for (auto&& cmd : tcmds)
	{
		if (cmd->GetType() == sl12::RenderCommandType::Mesh)
		{
			FillMeshTable(static_cast<sl12::MeshRenderCommand*>(cmd));
		}
	}

	// create shader table.
	auto Align = [](UINT size, UINT align)
	{
		return ((size + align - 1) / align) * align;
	};
	UINT shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
	UINT descHandleOffset = Align(shaderIdentifierSize, sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));
	UINT shaderRecordSize = Align(descHandleOffset + sizeof(LocalIndex), D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
	bvhShaderRecordSize_ = shaderRecordSize;

	auto GenShaderTable = [&](
		void* const * shaderIds,
		int tableCountPerMaterial,
		sl12::UniqueHandle<sl12::Buffer>& buffer,
		int materialCount)
	{
		buffer = sl12::MakeUnique<sl12::Buffer>(&device_);

		materialCount = (materialCount < 0) ? (int)material_table.size() : materialCount;
		sl12::BufferDesc desc{};
		desc.heap = sl12::BufferHeap::Dynamic;
		desc.size = shaderRecordSize * tableCountPerMaterial * materialCount;
		desc.usage = sl12::ResourceUsage::ShaderResource;
		desc.initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
		if (!buffer->Initialize(&device_, desc))
		{
			return false;
		}

		auto p = (char*)buffer->Map();
		for (int i = 0; i < materialCount; ++i)
		{
			for (int id = 0; id < tableCountPerMaterial; ++id)
			{
				auto start = p;

				memcpy(p, shaderIds[i * tableCountPerMaterial + id], shaderIdentifierSize);
				p += descHandleOffset;

				memcpy(p, &material_table[i], sizeof(LocalIndex));

				p = start + shaderRecordSize;
			}
		}
		buffer->Unmap();

		return true;
	};
	// material shader table.
	{
		void* hg_identifier[4];
		{
			ID3D12StateObjectProperties* prop;
			psoRayTracing_->GetPSO()->QueryInterface(IID_PPV_ARGS(&prop));
			hg_identifier[0] = prop->GetShaderIdentifier(kMaterialOpacityHG);
			hg_identifier[1] = prop->GetShaderIdentifier(kMaterialMaskedHG);
			prop->Release();
		}
		std::vector<void*> hg_table;
		for (auto v : opaque_table)
		{
			hg_table.push_back(v ? hg_identifier[0] : hg_identifier[1]);
		}
		if (!GenShaderTable(hg_table.data(), kRTMaterialTableCount, MaterialHGTable_, -1))
		{
			return false;
		}
	}
	// for PathTracer.
	{
		void* rgs_identifier;
		void* ms_identifier;
		{
			ID3D12StateObjectProperties* prop;
			psoRayTracing_->GetPSO()->QueryInterface(IID_PPV_ARGS(&prop));
			rgs_identifier = prop->GetShaderIdentifier(kPathTracerRGS);
			ms_identifier = prop->GetShaderIdentifier(kPathTracerMS);
			prop->Release();
		}
		if (!GenShaderTable(&rgs_identifier, 1, PathTracerRGSTable_, 1))
		{
			return false;
		}
		if (!GenShaderTable(&ms_identifier, 1, PathTracerMSTable_, 1))
		{
			return false;
		}
	}

	return true;
}

bool SampleApplication::InitializeOIDN()
{
	int nDevices = oidn::getNumPhysicalDevices();
	for (int i = 0; i < nDevices; i++)
	{
		oidn::PhysicalDeviceRef pd(i);
		if (pd.get<oidn::DeviceType>("type") == oidn::DeviceType::CUDA)
		{
			oidnPhysicalDevice_ = pd;
			break;
		}
	}

	if (!oidnPhysicalDevice_)
	{
		return false;
	}
	
	oidnDevice_ = oidn::newDevice(oidn::DeviceType::CUDA);//oidnPhysicalDevice_.newDevice();
	// oidnDevice_.set("verbose", 4);
	oidnDevice_.commit();

	auto memTypes = oidnDevice_.get<int>("externalMemoryTypes");
	if (!(memTypes & (int)oidn::ExternalMemoryTypeFlag::OpaqueWin32))
	{
		return false;
	}

	// create buffer.
	noisySource_ = sl12::MakeUnique<sl12::Buffer>(&device_);
	albedoSource_ = sl12::MakeUnique<sl12::Buffer>(&device_);
	normalSource_ = sl12::MakeUnique<sl12::Buffer>(&device_);
	denoiseResult_ = sl12::MakeUnique<sl12::Buffer>(&device_);
	denoiseResultSRV_ = sl12::MakeUnique<sl12::BufferView>(&device_);
	{
		sl12::BufferDesc desc{};
		desc.heap = sl12::BufferHeap::Default;
		desc.size = displayWidth_ * displayHeight_ * sizeof(float) * 3;
		desc.usage = sl12::ResourceUsage::ShaderResource;
		desc.initialState = D3D12_RESOURCE_STATE_COMMON;
		desc.deviceShared = true;
		if (!noisySource_->Initialize(&device_, desc))
		{
			return false;
		}
		if (!albedoSource_->Initialize(&device_, desc))
		{
			return false;
		}
		if (!normalSource_->Initialize(&device_, desc))
		{
			return false;
		}
		if (!denoiseResult_->Initialize(&device_, desc))
		{
			return false;
		}

		if (!denoiseResultSRV_->Initialize(&device_, &denoiseResult_, 0, 0, 0))
		{
			return false;
		}
	}

	// create oidn buffer.
	HANDLE hNoisy, hAlbedo, hNormal, hDenoise;
	HRESULT hr;
	const char* errorMsg;
	hr = device_.GetDeviceDep()->CreateSharedHandle(noisySource_->GetResourceDep(), nullptr, GENERIC_ALL, nullptr, &hNoisy);
	hr = device_.GetDeviceDep()->CreateSharedHandle(albedoSource_->GetResourceDep(), nullptr, GENERIC_ALL, nullptr, &hAlbedo);
	hr = device_.GetDeviceDep()->CreateSharedHandle(normalSource_->GetResourceDep(), nullptr, GENERIC_ALL, nullptr, &hNormal);
	hr = device_.GetDeviceDep()->CreateSharedHandle(denoiseResult_->GetResourceDep(), nullptr, GENERIC_ALL, nullptr, &hDenoise);

	oidnNoisySource_ = oidnDevice_.newBuffer(oidn::ExternalMemoryTypeFlag::OpaqueWin32, hNoisy, nullptr, noisySource_->GetBufferDesc().size);
	if (oidnDevice_.getError(errorMsg) != oidn::Error::None)
	{
		sl12::ConsolePrint("%s\n", errorMsg);
		return false;
	}
	oidnAlbedoSource_ = oidnDevice_.newBuffer(oidn::ExternalMemoryTypeFlag::OpaqueWin32, hAlbedo, nullptr, albedoSource_->GetBufferDesc().size);
	if (oidnDevice_.getError(errorMsg) != oidn::Error::None)
	{
		sl12::ConsolePrint("%s\n", errorMsg);
		return false;
	}
	oidnNormalSource_ = oidnDevice_.newBuffer(oidn::ExternalMemoryTypeFlag::OpaqueWin32, hNormal, nullptr, normalSource_->GetBufferDesc().size);
	if (oidnDevice_.getError(errorMsg) != oidn::Error::None)
	{
		sl12::ConsolePrint("%s\n", errorMsg);
		return false;
	}
	oidnDenoiseResult_ = oidnDevice_.newBuffer(oidn::ExternalMemoryTypeFlag::OpaqueWin32, hDenoise, nullptr, denoiseResult_->GetBufferDesc().size);
	if (oidnDevice_.getError(errorMsg) != oidn::Error::None)
	{
		sl12::ConsolePrint("%s\n", errorMsg);
		return false;
	}

	CloseHandle(hNoisy);
	CloseHandle(hAlbedo);
	CloseHandle(hNormal);
	CloseHandle(hDenoise);

	return true;
}

void SampleApplication::DestroyOIDN()
{
	oidnNoisySource_ = oidn::BufferRef();
	oidnAlbedoSource_ = oidn::BufferRef();
	oidnNormalSource_ = oidn::BufferRef();
	oidnDenoiseResult_ = oidn::BufferRef();
	oidnDevice_ = oidn::DeviceRef();
	oidnPhysicalDevice_ = oidn::PhysicalDeviceRef();

	denoiseResultSRV_.Reset();
	denoiseResult_.Reset();
	normalSource_.Reset();
	albedoSource_.Reset();
	noisySource_.Reset();
}

void SampleApplication::CopyNoisyResource(sl12::CommandList* pCmdList, sl12::Buffer* pNoisySrc, sl12::Buffer* pAlbedoSrc, sl12::Buffer* pNormalSrc)
{
	pCmdList->GetLatestCommandList()->CopyResource(noisySource_->GetResourceDep(), pNoisySrc->GetResourceDep());
	pCmdList->GetLatestCommandList()->CopyResource(albedoSource_->GetResourceDep(), pAlbedoSrc->GetResourceDep());
	pCmdList->GetLatestCommandList()->CopyResource(normalSource_->GetResourceDep(), pNormalSrc->GetResourceDep());
}

void SampleApplication::ExecuteDenoise()
{
	oidn::FilterRef filter = oidnDevice_.newFilter("RT");
	filter.setImage("color", oidnNoisySource_, oidn::Format::Float3, displayWidth_, displayHeight_);
	filter.setImage("albedo", oidnAlbedoSource_, oidn::Format::Float3, displayWidth_, displayHeight_);
	filter.setImage("normal", oidnNormalSource_, oidn::Format::Float3, displayWidth_, displayHeight_);
	filter.setImage("output", oidnDenoiseResult_, oidn::Format::Float3, displayWidth_, displayHeight_);
	filter.set("hdr", true);
	filter.set("cleanAux", true);
	filter.commit();

	filter.execute();
	const char* errorMsg;
	if (oidnDevice_.getError(errorMsg) != oidn::Error::None)
	{
		sl12::ConsolePrint("%s\n", errorMsg);
	}
}

//	EOF
