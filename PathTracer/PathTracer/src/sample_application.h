#include "sl12/application.h"
#include "sl12/resource_loader.h"
#include "sl12/shader_manager.h"
#include "sl12/command_list.h"
#include "sl12/gui.h"
#include "sl12/root_signature.h"
#include "sl12/pipeline_state.h"
#include "sl12/unique_handle.h"
#include "sl12/cbv_manager.h"
#include "sl12/render_graph.h"
#include "sl12/indirect_executer.h"
#include "sl12/bvh_manager.h"
#include "sl12/scene_root.h"

#include <memory>
#include <vector>

#include "sl12/scene_mesh.h"
#include "sl12/timestamp.h"

#include "OpenImageDenoise/oidn.hpp"


class SampleApplication
	: public sl12::Application
{
	template <typename T> using UniqueHandle = sl12::UniqueHandle<T>;
	
	struct WorkMaterial
	{
		sl12::ResourceItemMesh::Material	resource;
		int									psoType;
	};

	typedef std::vector<sl12::CbvHandle> MeshShapeOffset;

public:
	SampleApplication(HINSTANCE hInstance, int nCmdShow, int screenWidth, int screenHeight, sl12::ColorSpaceType csType, const std::string& homeDir, int meshType);
	virtual ~SampleApplication();

	// virtual
	virtual bool Initialize() override;
	virtual bool Execute() override;
	virtual void Finalize() override;
	virtual int Input(UINT message, WPARAM wParam, LPARAM lParam) override;

private:
	void ControlCamera(float deltaTime = 1.0f / 60.0f);

	void ComputeSceneAABB();

	bool CreateRaytracingPipeline();
	bool CreateRayTracingShaderTable(sl12::CommandList* pCmdList, sl12::RenderCommandsTempList& tcmds);

	bool InitializeOIDN();
	void DestroyOIDN();
	void CopyNoisyResource(sl12::CommandList* pCmdList, sl12::Buffer* pNoisySrc, sl12::Buffer* pAlbedoSrc, sl12::Buffer* pNormalSrc);
	void ExecuteDenoise();

private:
	static const int kBufferCount = sl12::Swapchain::kMaxBuffer;

	struct CommandLists
	{
		sl12::CommandList	cmdLists[kBufferCount];
		int					index = 0;

		CommandLists()
		{}
		~CommandLists()
		{
			Destroy();
		}

		bool Initialize(sl12::Device* pDev, sl12::CommandQueue* pQueue)
		{
			for (auto&& v : cmdLists)
			{
				if (!v.Initialize(pDev, pQueue, true))
				{
					return false;
				}
			}
			index = 0;
			return true;
		}

		void Destroy()
		{
			for (auto&& v : cmdLists) v.Destroy();
		}

		sl12::CommandList& Reset()
		{
			index = (index + 1) % kBufferCount;
			auto&& ret = cmdLists[index];
			ret.Reset();
			return ret;
		}

		void Close()
		{
			cmdLists[index].Close();
		}

		void Execute()
		{
			cmdLists[index].Execute();
		}

		sl12::CommandQueue* GetParentQueue()
		{
			return cmdLists[index].GetParentQueue();
		}
	};	// struct CommandLists

private:
	std::string		homeDir_;
	
	UniqueHandle<sl12::ResourceLoader>	resLoader_;
	UniqueHandle<sl12::ShaderManager>	shaderMan_;
	UniqueHandle<sl12::MeshManager>		meshMan_;
	UniqueHandle<CommandLists>			mainCmdList_;
	UniqueHandle<sl12::CbvManager>		cbvMan_;
	UniqueHandle<sl12::RenderGraph>		renderGraph_;
	UniqueHandle<sl12::BvhManager>		bvhMan_;
	UniqueHandle<sl12::SceneRoot>		sceneRoot_;

	// root sig & pso.
	UniqueHandle<sl12::RootSignature>			rsVsPs_;
	UniqueHandle<sl12::RootSignature>			rsCs_;
	UniqueHandle<sl12::RootSignature>			rsRTGlobal_, rsRTLocal_;
	UniqueHandle<sl12::GraphicsPipelineState>	psoTonemap_;
	UniqueHandle<sl12::DxrPipelineState>		psoMaterialCollection_, psoPathTracer_, psoRayTracing_;

	UniqueHandle<sl12::Sampler>				linearSampler_;

	UniqueHandle<sl12::Gui>		gui_;
	sl12::InputData				inputData_{};

	// resources.
	sl12::ResourceHandle	hSuzanneMesh_;
	sl12::ResourceHandle	hSponzaMesh_;
	sl12::ResourceHandle	hSphereMesh_;
	sl12::ResourceHandle	hDetailTex_;
	sl12::ResourceHandle	hDotTex_;

	// shaders.
	std::vector<sl12::ShaderHandle>	hShaders_;

	// history.
	DirectX::XMMATRIX		mtxPrevWorldToView_, mtxPrevViewToClip_, mtxPrevWorldToClip_;

	std::vector<std::shared_ptr<sl12::SceneMesh>>	sceneMeshes_;
	DirectX::XMFLOAT3		sceneAABBMax_, sceneAABBMin_;

	// ray tracing.
	UniqueHandle<sl12::RaytracingDescriptorManager>	rtDescMan_;
	UniqueHandle<sl12::Buffer>	PathTracerRGSTable_;
	UniqueHandle<sl12::Buffer>	PathTracerMSTable_;
	UniqueHandle<sl12::Buffer>	MaterialHGTable_;
	sl12::u32	bvhShaderRecordSize_;
	std::map<const sl12::ResourceItemMesh*, MeshShapeOffset>	OffsetCBVs_;

	sl12::Timestamp			timestamps_[2];
	sl12::u32				timestampIndex_ = 0;
	sl12::CpuTimer			currCpuTime_;

	// camera parameters.
	DirectX::XMFLOAT3		cameraPos_;
	DirectX::XMFLOAT3		cameraDir_;
	int						lastMouseX_, lastMouseY_;

	// light parameters.
	float					skyColor_[3] = {0.565f, 0.843f, 0.925f};
	float					groundColor_[3] = {0.639f, 0.408f, 0.251f};
	float					ambientIntensity_ = 0.1f;
	float					directionalTheta_ = 30.0f;
	float					directionalPhi_ = 45.0f;
	float					directionalColor_[3] = {1.0f, 1.0f, 1.0f};
	float					directionalIntensity_ = 3.0f;

	// path trace parameters.
	bool					bDeiseEnable_ = true;
	int						ptSampleCount_ = 1;
	int						ptDepthMax_ = 4;

	// OIDN.
	oidn::PhysicalDeviceRef			oidnPhysicalDevice_;
	oidn::DeviceRef					oidnDevice_;
	UniqueHandle<sl12::Buffer>		noisySource_;
	UniqueHandle<sl12::Buffer>		albedoSource_;
	UniqueHandle<sl12::Buffer>		normalSource_;
	UniqueHandle<sl12::Buffer>		denoiseResult_;
	UniqueHandle<sl12::BufferView>	denoiseResultSRV_;
	oidn::BufferRef					oidnNoisySource_;
	oidn::BufferRef					oidnAlbedoSource_;
	oidn::BufferRef					oidnNormalSource_;
	oidn::BufferRef					oidnDenoiseResult_;

	int	displayWidth_, displayHeight_;
	int meshType_;
	sl12::u64	frameIndex_ = 0;
};	// class SampleApplication

//	EOF
