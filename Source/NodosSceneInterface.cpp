#include "NodosSceneInterface.h"
#include "SceneRenderer.h"

#include <iostream>
#include <string>
#include <filesystem>
#include <random>
#include <array>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <process.h>

#include <Nodos/AppHelpers.hpp>
#include <nosFlatBuffersCommon.h>
#include <nosVulkanSubsystem/Types_generated.h>
#include <nosTrack/Track_generated.h>
#include <nosVulkanSubsystem/nosVulkanSubsystem.h>

#define NOS_ENABLE_SYNC_LOGS 0

using namespace nos;

namespace nos::dxapp
{

inline nos::uuid GenerateId()
{
	static std::seed_seq seed = []() {
		std::random_device rd;
		auto seed_data = std::array<int, std::mt19937::state_size>{};
		std::generate(std::begin(seed_data), std::end(seed_data), std::ref(rd));
		return std::seed_seq(std::begin(seed_data), std::end(seed_data));
	}();
	static std::mt19937 generator(seed);
	static uuids::uuid_random_generator uuidGen(generator);
	return nos::uuid(uuidGen());
}

bool FileExists(const std::string& filename)
{
	try
	{
		return std::filesystem::exists(filename);
	}
	catch (std::exception const&)
	{
		return false;
	}
}

inline void Must(bool cond, const char* errMsg = "Unspecified")
{
	if (cond)
		return;
	std::cerr << "Error: " << errMsg << std::endl;
	std::cerr << "Details: " << ::GetLastError() << std::endl;
	throw std::runtime_error(errMsg);
}

// Helper to run a command and capture its stdout
std::string RunCommandAndCapture(const std::string& cmd)
{
	HANDLE hRead, hWrite;
	SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
	if (!CreatePipe(&hRead, &hWrite, &sa, 0))
		return "";

	STARTUPINFOA si = {sizeof(STARTUPINFOA)};
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = hWrite;
	si.hStdError = hWrite;
	si.hStdInput = NULL;

	PROCESS_INFORMATION pi = {};
	std::vector<char> cmdline(cmd.begin(), cmd.end());
	cmdline.push_back('\0');

	BOOL success = CreateProcessA(NULL, cmdline.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

	CloseHandle(hWrite);

	std::string output;
	if (success)
	{
		char buffer[256];
		DWORD read;
		while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &read, NULL) && read > 0)
		{
			buffer[read] = '\0';
			output += buffer;
		}
		WaitForSingleObject(pi.hProcess, INFINITE);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	CloseHandle(hRead);
	return output;
}

std::optional<std::string> GetSdkPathFromNosman(const std::string& bundleRoot, const std::string& appSdkVersion)
{
	std::string nodosExe = bundleRoot + "\\nodos.exe";

	if (!FileExists(nodosExe))
		return std::nullopt;

	std::string cmd = "\"" + nodosExe + "\" --workspace \"" + bundleRoot + "\" sdk-info \"" + appSdkVersion + "\" process";
	std::string jsonStr = RunCommandAndCapture(cmd);
	if (jsonStr.empty())
		return std::nullopt;

	const char* key = "\"path\"";
	size_t keyPos = jsonStr.find(key);
	std::string sdkPath;
	if (keyPos != std::string::npos)
	{
		size_t colon = jsonStr.find(':', keyPos);
		if (colon != std::string::npos)
		{
			size_t firstQuote = jsonStr.find('"', colon + 1);
			if (firstQuote != std::string::npos)
			{
				size_t secondQuote = jsonStr.find('"', firstQuote + 1);
				if (secondQuote != std::string::npos)
				{
					sdkPath = jsonStr.substr(firstQuote + 1, secondQuote - firstQuote - 1);
				}
			}
		}
	}
	if (sdkPath.empty())
	{
		std::cerr << "Failed to parse SDK path from command output." << std::endl;
		return std::nullopt;
	}
	return sdkPath;
}

struct ExportedTexture
{
	ComPtr<ID3D12Resource> Resource;
	HANDLE SharedHandle;
	nos::sys::vulkan::TTexture TextureDef;  // Nodos still uses Vulkan-style definitions
	D3D12_RESOURCE_STATES CurrentState;
};

struct ExportedFence
{
	ComPtr<ID3D12Fence> Fence;
	HANDLE SharedHandle;
};

struct WinProcLoader final : public nos::app::IProcLoader
{
public:
	WinProcLoader(HMODULE module) : Module(module) {}
	~WinProcLoader() { ::FreeLibrary(Module); }
	ProcPtr GetProcAddress(const char* procName) override
	{
		return reinterpret_cast<ProcPtr>(::GetProcAddress(Module, procName));
	}
	HMODULE Module;
};

struct SceneAppNode : public nos::app::IAppNode
{
public:
	SceneAppNode(NodosSceneInterface::InternalState& appInterface);
	void OnImport(nos::fb::Node const& appNode) override;
	void OnRemoved() override;
	void OnPinValueChanges(std::unordered_map<nos::uuid, nos::Buffer> const& pinValues) override;
	void OnPreExecute(void* frameCtx, uint64_t frameNumber) override;
	void OnPostExecute(void* frameCtx, uint64_t frameNumber) override;
	void OnExecutionStateChanged(nos::app::ExecutionState newState, nos::app::ExecutionState oldState) override;

private:
	NodosSceneInterface::InternalState& AppInterface;
	nos::uuid NodeId;
	nos::uuid TrackPinId;
	nos::uuid ResolutionPinId;
	nos::uuid OutColorPinId;
	nos::uuid OutDepthPinId;
	nos::uuid InColorPinId;
	nos::uuid QuadP0PinId;
	nos::uuid QuadP1PinId;
	nos::uuid QuadP2PinId;
	nos::uuid QuadP3PinId;
	
	ExportedTexture InColor;
	ExportedTexture OutColor;
	ExportedTexture OutDepth;
	
	static constexpr const char* InColorPinName = "InColor";
	static constexpr const char* OutColorPinName = "OutColor";
	static constexpr const char* OutDepthPinName = "OutDepth";
	static constexpr const char* TrackPinName = "Track";
	static constexpr const char* ResolutionPinName = "OutputResolution";
	static constexpr const char* QuadP0PinName = "QuadP0";
	static constexpr const char* QuadP1PinName = "QuadP1";
	static constexpr const char* QuadP2PinName = "QuadP2";
	static constexpr const char* QuadP3PinName = "QuadP3";

	struct SyncState
	{
		ExportedFence InputFence;
		ExportedFence OutputFence;
		std::optional<uint64_t> LastInputFrameNumber = std::nullopt;
		std::optional<uint64_t> LastOutputFrameNumber = std::nullopt;
	};
	std::optional<SyncState> Sync;

	nos::track::TTrack Track{};
	uint32_t OutputWidth = 1920;
	uint32_t OutputHeight = 1080;
	bool OutputResolutionChanged = false;

	// Quad corner positions in world space
	nos::fb::vec3 QuadP0{-1.5f, 1.0f, -2.0f};
	nos::fb::vec3 QuadP1{-1.5f, -1.0f, -2.0f};
	nos::fb::vec3 QuadP2{1.5f, -1.0f, -2.0f};
	nos::fb::vec3 QuadP3{1.5f, 1.0f, -2.0f};

	// DX12 resources
	ComPtr<ID3D12Device> Device;
	ComPtr<ID3D12CommandQueue> CommandQueue;
	
	struct FrameResources
	{
		struct WorkGroupResources
		{
			ComPtr<ID3D12CommandAllocator> CommandAllocator;
			ComPtr<ID3D12GraphicsCommandList> CommandList;
		};
		WorkGroupResources InputCopyResources;
		WorkGroupResources OutputCopyResources;
	};
	
	constexpr static size_t FramesInFlight = 3;
	std::array<FrameResources, FramesInFlight> FrameResourcesArray{};

	void WaitOrSignalFence(ID3D12Fence* fence, uint64_t waitValue)
	{
		constexpr uint64_t waitBeforeSignalMs = 100;  // 100 ms
		while (true)
		{
			uint64_t currentValue = fence->GetCompletedValue();
			if (currentValue >= waitValue)
				return;
			
			// Try to signal if it's stuck
			HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
			fence->SetEventOnCompletion(waitValue, event);
			DWORD result = WaitForSingleObject(event, waitBeforeSignalMs);
			CloseHandle(event);
			
			if (result == WAIT_OBJECT_0)
				return;
				
			// Signal manually if needed
			fence->Signal(waitValue);
		}
	}

	FrameResources::WorkGroupResources BeginNewFrameForWorkGroupResources(SyncState& sync, uint64_t frameNumber, bool inputResources)
	{
		size_t frameIndex = frameNumber % FramesInFlight;
		FrameResources& frameRes = FrameResourcesArray[frameIndex];
		auto& workResources = inputResources ? frameRes.InputCopyResources : frameRes.OutputCopyResources;

		if (frameNumber >= FramesInFlight)
		{
			uint64_t frameToWait = frameNumber - FramesInFlight;
			ID3D12Fence* fenceToWait = inputResources ? sync.InputFence.Fence.Get() : sync.OutputFence.Fence.Get();
			uint64_t waitValue = inputResources ? GetInputWaitValue(frameToWait) : GetOutputWaitValue(frameToWait);

			WaitOrSignalFence(fenceToWait, waitValue);
			Must(SUCCEEDED(workResources.CommandAllocator->Reset()), "Failed to reset command allocator");
		}
		
		Must(SUCCEEDED(workResources.CommandList->Reset(workResources.CommandAllocator.Get(), nullptr)), 
			"Failed to reset command list");
		return workResources;
	}

	FrameResources::WorkGroupResources CreateWorkGroupResources()
	{
		FrameResources::WorkGroupResources workGroupRes{};
		
		Must(SUCCEEDED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, 
			IID_PPV_ARGS(&workGroupRes.CommandAllocator))),
			"Failed to create command allocator");
		
		Must(SUCCEEDED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, 
			workGroupRes.CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&workGroupRes.CommandList))),
			"Failed to create command list");
		
		Must(SUCCEEDED(workGroupRes.CommandList->Close()), "Failed to close command list");
		
		return workGroupRes;
	}

	void CreateFrameResources()
	{
		for (size_t i = 0; i < FramesInFlight; i++)
		{
			FrameResources& frameRes = FrameResourcesArray[i];
			frameRes.InputCopyResources = CreateWorkGroupResources();
			frameRes.OutputCopyResources = CreateWorkGroupResources();
		}
	}

	void SignalAndWaitAllFrames()
	{
		if (!Sync)
			return;
			
		if (Sync->LastInputFrameNumber)
		{
			uint64_t firstInputFrameToWait = *Sync->LastInputFrameNumber >= FramesInFlight ? 
				*Sync->LastInputFrameNumber - FramesInFlight + 1 : 0;
			uint64_t lastFrameToWait = *Sync->LastInputFrameNumber;
			for (uint64_t frameNum = firstInputFrameToWait; frameNum <= lastFrameToWait; frameNum++)
			{
				WaitOrSignalFence(Sync->InputFence.Fence.Get(), GetInputWaitValue(frameNum));
			}
		}
		
		if (Sync->LastOutputFrameNumber)
		{
			uint64_t firstOutputFrameToWait = *Sync->LastOutputFrameNumber >= FramesInFlight ? 
				*Sync->LastOutputFrameNumber - FramesInFlight + 1 : 0;
			uint64_t lastFrameToWait = *Sync->LastOutputFrameNumber;
			for (uint64_t frameNum = firstOutputFrameToWait; frameNum <= lastFrameToWait; frameNum++)
			{
				WaitOrSignalFence(Sync->OutputFence.Fence.Get(), GetOutputWaitValue(frameNum));
			}
		}
	}

	void DestroyFrameResources()
	{
		SignalAndWaitAllFrames();
		for (uint32_t i = 0; i < FramesInFlight; i++)
		{
			FrameResourcesArray[i] = {};
		}
	}

	static uint64_t GetInputSignalValue(uint64_t frameNumber) { return frameNumber * 2 + 2; }
	static uint64_t GetOutputSignalValue(uint64_t frameNumber) { return frameNumber * 2 + 1; }
	static uint64_t GetInputWaitValue(uint64_t frameNumber) { return frameNumber * 2 + 1; }
	static uint64_t GetOutputWaitValue(uint64_t frameNumber) { return frameNumber * 2; }

	std::optional<ExportedFence> CreateExportedFence();
	std::optional<ExportedTexture> CreateExportedTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, 
		D3D12_RESOURCE_FLAGS flags, bool isDepth);
	void DestroyExportedTexture(ExportedTexture& exportedTexture);
	void InputCopies(ID3D12GraphicsCommandList* cmd, uint64_t frameCounter);
	void OutputCopies(ID3D12GraphicsCommandList* cmd, uint64_t frameCounter);

	struct WorkSubmitInfo
	{
		ID3D12Fence* WaitFence;
		uint64_t WaitValue;
		ID3D12Fence* SignalFence;
		uint64_t SignalValue;
	};

	WorkSubmitInfo PrepareSubmitInfo(uint64_t frameNumber, bool inputResources)
	{
		return WorkSubmitInfo{
			.WaitFence = inputResources ? Sync->InputFence.Fence.Get() : Sync->OutputFence.Fence.Get(),
			.WaitValue = inputResources ? GetInputWaitValue(frameNumber) : GetOutputWaitValue(frameNumber),
			.SignalFence = inputResources ? Sync->InputFence.Fence.Get() : Sync->OutputFence.Fence.Get(),
			.SignalValue = inputResources ? GetInputSignalValue(frameNumber) : GetOutputSignalValue(frameNumber)
		};
	}

	void SubmitWork(FrameResources::WorkGroupResources& workGroupRes, WorkSubmitInfo submitInfo)
	{
		Must(SUCCEEDED(workGroupRes.CommandList->Close()), "Failed to close command list");
		
		// Wait on fence before executing
		Must(SUCCEEDED(CommandQueue->Wait(submitInfo.WaitFence, submitInfo.WaitValue)),
			"Failed to wait on fence");
		
		ID3D12CommandList* cmdLists[] = {workGroupRes.CommandList.Get()};
		CommandQueue->ExecuteCommandLists(1, cmdLists);
		
		// Signal after execution
		Must(SUCCEEDED(CommandQueue->Signal(submitInfo.SignalFence, submitInfo.SignalValue)),
			"Failed to signal fence");
	}

	void DestroyFence(ExportedFence& exportedFence)
	{
		exportedFence.Fence.Reset();
		if (exportedFence.SharedHandle)
			CloseHandle(exportedFence.SharedHandle);
	}

	void SendSemaphoresToNodos();
};

struct NodosSceneInterface::InternalState : public nos::app::IApp
{
	InternalState(SceneRenderer& renderer) : Renderer(renderer) {}

	SceneRenderer& Renderer;
	ComPtr<ID3D12Device> Device;
	ComPtr<ID3D12CommandQueue> CommandQueue;

	std::string NodosSdkDllPath;
	std::string NodosAppKey = "DX-SceneRenderer";
	std::unique_ptr<WinProcLoader> NodosProcLoader;
	std::unique_ptr<nos::app::NodosCommunicator> Nodos;

	nos::app::IAppNode& CreateAppNode_ApiThread() override { return *(new SceneAppNode(*this)); }
	void DestroyAppNode(nos::app::IAppNode& appNode) override
	{
		delete static_cast<SceneAppNode*>(&appNode);
	}

	bool IsNodosCameraActive = false;
	DirectX::XMFLOAT3 StoredCameraPosition;
	DirectX::XMFLOAT3 StoredCameraTarget;

	void PreFrame()
	{
		if (!Nodos)
			return;

		// PreExecute returns false when not synced
		if (!Nodos->PreExecute(nullptr))
		{
			// Not synced - restore camera if was using Nodos camera
			if (IsNodosCameraActive)
			{
				Renderer.SetCameraPosition(StoredCameraPosition);
				Renderer.SetCameraTarget(StoredCameraTarget);
				IsNodosCameraActive = false;
			}
		}
		else
		{
			// Synced - store current camera if not already stored
			if (!IsNodosCameraActive)
			{
				StoredCameraPosition = Renderer.GetCamera().Position;
				StoredCameraTarget = Renderer.GetCamera().Target;
				IsNodosCameraActive = true;
			}
		}
	}

	void PostFrame() 
	{ 
		if (Nodos)
		{
			Nodos->PostExecute(nullptr); 
		}
	}
};

NodosSceneInterface::NodosSceneInterface(SceneRenderer& renderer)
	: m_InternalState(std::make_unique<InternalState>(renderer))
	, m_Renderer(renderer)
{
}

NodosSceneInterface::~NodosSceneInterface() = default;

void NodosSceneInterface::SetSdkDllPath(const std::string& path)
{
	m_InternalState->NodosSdkDllPath = path;
}

void NodosSceneInterface::Initialize(ID3D12Device* device, ID3D12CommandQueue* commandQueue, DXGI_FORMAT outputFormat)
{
	m_InternalState->Device = device;
	m_InternalState->CommandQueue = commandQueue;

#pragma region Nodos SDK Path Resolution
	char exePath[MAX_PATH];
	GetModuleFileNameA(nullptr, exePath, MAX_PATH);
	std::filesystem::path exeDir = std::filesystem::absolute(exePath).parent_path();
	
	if (m_InternalState->NodosSdkDllPath.empty() || !FileExists(m_InternalState->NodosSdkDllPath))
	{
		std::filesystem::path sdkDllCandidate = exeDir / "nosAppSDK.dll";
		m_InternalState->NodosSdkDllPath = sdkDllCandidate.string();
	}
	
	if (m_InternalState->NodosSdkDllPath.empty() || !FileExists(m_InternalState->NodosSdkDllPath))
	{
		std::filesystem::path bundleRoot = exeDir;
		for (int i = 0; i < 3; ++i)
			bundleRoot = bundleRoot.parent_path();
		std::string appSdkVersion = "20.0";
		std::optional<std::string> sdkPathOpt = GetSdkPathFromNosman(bundleRoot.string(), appSdkVersion);
		if (sdkPathOpt)
		{
			std::string candidate = *sdkPathOpt + "\\Binaries\\nosAppSDK.dll";
			if (FileExists(candidate))
				m_InternalState->NodosSdkDllPath = candidate;
		}
	}

	if (m_InternalState->NodosSdkDllPath.empty() || !FileExists(m_InternalState->NodosSdkDllPath))
	{
		const char* sdkDir = std::getenv("NODOS_SDK_DIR");
		if (sdkDir)
		{
			std::string candidate = std::string(sdkDir) + "/bin/nosAppSDK.dll";
			if (FileExists(candidate))
			{
				m_InternalState->NodosSdkDllPath = candidate;
			}
		}
	}
#pragma endregion

	while (m_InternalState->NodosSdkDllPath.empty() || !FileExists(m_InternalState->NodosSdkDllPath))
	{
		std::cout << "Enter path to Nodos SDK DLL: ";
		std::getline(std::cin, m_InternalState->NodosSdkDllPath);
		if (!FileExists(m_InternalState->NodosSdkDllPath))
		{
			std::cout << "File does not exist: " << m_InternalState->NodosSdkDllPath << std::endl;
			m_InternalState->NodosSdkDllPath.clear();
		}
	}

	std::cout << "Using Nodos SDK DLL at: " << m_InternalState->NodosSdkDllPath << std::endl;

	HMODULE sdkModule = LoadLibraryA(m_InternalState->NodosSdkDllPath.c_str());
	Must(sdkModule, ("Failed to load Nodos SDK DLL: " + m_InternalState->NodosSdkDllPath).c_str());

	m_InternalState->NodosProcLoader = std::make_unique<WinProcLoader>(sdkModule);

	nos::app::ApplicationInfo appInfo{
		.AppKey = m_InternalState->NodosAppKey.c_str(), 
		.AppName = "DX12 Scene Renderer"
	};
	
	auto nodosCommunicator = nos::app::NodosCommunicator::Create(*m_InternalState, 
		*m_InternalState->NodosProcLoader, "localhost:50053", appInfo);

	if (auto* err = nodosCommunicator.Error())
	{
		Must(false, std::string("Failed to create Nodos Communicator: " + *err).c_str());
	}

	m_InternalState->Nodos = std::move(*nodosCommunicator.Get());
}

void NodosSceneInterface::PreFrame()
{
	m_InternalState->PreFrame();
}

void NodosSceneInterface::PostFrame()
{
	m_InternalState->PostFrame();
}

void NodosSceneInterface::OnResize(uint32_t width, uint32_t height)
{
	if (!IsSynced())
	{
		m_Renderer.Resize(width, height);
	}
}

void NodosSceneInterface::Shutdown()
{
	m_InternalState->Nodos.reset();
	m_InternalState.reset();
}

bool NodosSceneInterface::IsSynced() const
{
	return m_InternalState->Nodos && m_InternalState->Nodos->IsSynced();
}

// SceneAppNode implementation
inline SceneAppNode::SceneAppNode(NodosSceneInterface::InternalState& appInterface)
	: AppInterface(appInterface)
	, Device(appInterface.Device)
	, CommandQueue(appInterface.CommandQueue)
{
}

inline void SceneAppNode::OnImport(nos::fb::Node const& appNode)
{
	NodeId = *appNode.id();
	std::optional<nos::fb::UUID> trackPinId, resolutionPinId, outColorPinId, outDepthPinId, inColorPinId;
	std::optional<nos::fb::UUID> quadP0PinId, quadP1PinId, quadP2PinId, quadP3PinId;
	bool overrideResolution = false;
	
	if (appNode.pins())
	{
		for (const auto& pin : *appNode.pins())
		{
			if (pin->name()->str() == TrackPinName)
			{
				trackPinId = *pin->id();
				flatbuffers::GetRoot<nos::track::Track>(pin->data()->Data())->UnPackTo(&Track);
			}
			else if (pin->name()->str() == ResolutionPinName)
			{
				resolutionPinId = *pin->id();
				const nos::fb::vec2u& resPtr = *reinterpret_cast<const nos::fb::vec2u*>(pin->data()->Data());
				if (resPtr.x() != 0 && resPtr.y() != 0)
				{
					OutputWidth = resPtr.x();
					OutputHeight = resPtr.y();
				}
				else
					overrideResolution = true;
			}
			else if (pin->name()->str() == OutColorPinName)
			{
				outColorPinId = *pin->id();
			}
			else if (pin->name()->str() == OutDepthPinName)
			{
				outDepthPinId = *pin->id();
			}
			else if (pin->name()->str() == InColorPinName)
			{
				inColorPinId = *pin->id();
			}
			else if (pin->name()->str() == QuadP0PinName)
			{
				quadP0PinId = *pin->id();
				QuadP0 = *reinterpret_cast<const nos::fb::vec3*>(pin->data()->Data());
			}
			else if (pin->name()->str() == QuadP1PinName)
			{
				quadP1PinId = *pin->id();
				QuadP1 = *reinterpret_cast<const nos::fb::vec3*>(pin->data()->Data());
			}
			else if (pin->name()->str() == QuadP2PinName)
			{
				quadP2PinId = *pin->id();
				QuadP2 = *reinterpret_cast<const nos::fb::vec3*>(pin->data()->Data());
			}
			else if (pin->name()->str() == QuadP3PinName)
			{
				quadP3PinId = *pin->id();
				QuadP3 = *reinterpret_cast<const nos::fb::vec3*>(pin->data()->Data());
			}
		}
	}

	// Create exported textures
	OutColor = *CreateExportedTexture(OutputWidth, OutputHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, false);
	OutDepth = *CreateExportedTexture(OutputWidth, OutputHeight, DXGI_FORMAT_D32_FLOAT,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, true);
	InColor = *CreateExportedTexture(OutputWidth, OutputHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_NONE, false);

	flatbuffers::FlatBufferBuilder fbb;
	std::vector<flatbuffers::Offset<nos::fb::Pin>> pins;
	bool outColorNew = false;
	bool outDepthNew = false;
	bool inColorNew = false;

	std::vector<uint8_t> colorTexBuf = nos::Buffer::From(OutColor.TextureDef);
	std::vector<uint8_t> depthTexBuf = nos::Buffer::From(OutDepth.TextureDef);
	std::vector<uint8_t> inColorTexBuf = nos::Buffer::From(InColor.TextureDef);

	if (!outColorPinId)
	{
		outColorPinId = GenerateId();
		outColorNew = true;
		pins.push_back(nos::fb::CreatePinDirect(fbb, &*outColorPinId, OutColorPinName,
			nos::sys::vulkan::Texture::GetFullyQualifiedName(), nos::fb::ShowAs::OUTPUT_PIN,
			nos::fb::CanShowAs::OUTPUT_PIN_ONLY, nullptr, 0, &colorTexBuf));
	}
	
	if (!outDepthPinId)
	{
		outDepthPinId = GenerateId();
		outDepthNew = true;
		pins.push_back(nos::fb::CreatePinDirect(fbb, &*outDepthPinId, OutDepthPinName,
			nos::sys::vulkan::Texture::GetFullyQualifiedName(), nos::fb::ShowAs::OUTPUT_PIN,
			nos::fb::CanShowAs::OUTPUT_PIN_ONLY, nullptr, 0, &depthTexBuf));
	}
	
	if (!inColorPinId)
	{
		inColorPinId = GenerateId();
		inColorNew = true;
		pins.push_back(nos::fb::CreatePinDirect(fbb, &*inColorPinId, InColorPinName,
			nos::sys::vulkan::Texture::GetFullyQualifiedName(), nos::fb::ShowAs::INPUT_PIN,
			nos::fb::CanShowAs::INPUT_PIN_ONLY, nullptr, 0, &inColorTexBuf));
	}
	
	if (!trackPinId)
	{
		std::vector<uint8_t> trackBuf = nos::Buffer::From(Track);
		trackPinId = GenerateId();
		pins.push_back(nos::fb::CreatePinDirect(fbb, &*trackPinId, TrackPinName, 
			nos::track::Track::GetFullyQualifiedName(),
			nos::fb::ShowAs::INPUT_PIN, nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY, nullptr, 0, &trackBuf));
	}
	
	if (!resolutionPinId)
	{
		nos::fb::vec2u resolution{OutputWidth, OutputHeight};
		std::vector<uint8_t> resBuf(reinterpret_cast<const uint8_t*>(&resolution),
			reinterpret_cast<const uint8_t*>(&resolution) + sizeof(nos::fb::vec2u));
		resolutionPinId = GenerateId();
		pins.push_back(nos::fb::CreatePinDirect(fbb, &*resolutionPinId, ResolutionPinName,
			nos::fb::vec2u::GetFullyQualifiedName(), nos::fb::ShowAs::INPUT_PIN,
			nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY, nullptr, 0, &resBuf));
	}

	// Create quad corner pins
	if (!quadP0PinId)
	{
		std::vector<uint8_t> vec3Buf(reinterpret_cast<const uint8_t*>(&QuadP0),
			reinterpret_cast<const uint8_t*>(&QuadP0) + sizeof(nos::fb::vec3));
		quadP0PinId = GenerateId();
		pins.push_back(nos::fb::CreatePinDirect(fbb, &*quadP0PinId, QuadP0PinName,
			nos::fb::vec3::GetFullyQualifiedName(), nos::fb::ShowAs::INPUT_PIN,
			nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY, nullptr, 0, &vec3Buf));
	}

	if (!quadP1PinId)
	{
		std::vector<uint8_t> vec3Buf(reinterpret_cast<const uint8_t*>(&QuadP1),
			reinterpret_cast<const uint8_t*>(&QuadP1) + sizeof(nos::fb::vec3));
		quadP1PinId = GenerateId();
		pins.push_back(nos::fb::CreatePinDirect(fbb, &*quadP1PinId, QuadP1PinName,
			nos::fb::vec3::GetFullyQualifiedName(), nos::fb::ShowAs::INPUT_PIN,
			nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY, nullptr, 0, &vec3Buf));
	}

	if (!quadP2PinId)
	{
		std::vector<uint8_t> vec3Buf(reinterpret_cast<const uint8_t*>(&QuadP2),
			reinterpret_cast<const uint8_t*>(&QuadP2) + sizeof(nos::fb::vec3));
		quadP2PinId = GenerateId();
		pins.push_back(nos::fb::CreatePinDirect(fbb, &*quadP2PinId, QuadP2PinName,
			nos::fb::vec3::GetFullyQualifiedName(), nos::fb::ShowAs::INPUT_PIN,
			nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY, nullptr, 0, &vec3Buf));
	}

	if (!quadP3PinId)
	{
		std::vector<uint8_t> vec3Buf(reinterpret_cast<const uint8_t*>(&QuadP3),
			reinterpret_cast<const uint8_t*>(&QuadP3) + sizeof(nos::fb::vec3));
		quadP3PinId = GenerateId();
		pins.push_back(nos::fb::CreatePinDirect(fbb, &*quadP3PinId, QuadP3PinName,
			nos::fb::vec3::GetFullyQualifiedName(), nos::fb::ShowAs::INPUT_PIN,
			nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY, nullptr, 0, &vec3Buf));
	}

	if (pins.size() > 0)
	{
		fbb.Finish(nos::CreatePartialNodeUpdateDirect(fbb, &NodeId, nos::ClearFlags::CLEAR_NODES, 0, &pins, 
			0, 0, 0, 0, 0, 0, 0,
			nos::fb::CreateNodeOrphanStateDirect(fbb, nos::fb::NodeOrphanStateType::ACTIVE, "")));
		nos::Buffer update = fbb.Release();
		AppInterface.Nodos->GetClient().SendPartialNodeUpdate(*update.As<nos::PartialNodeUpdate>());
	}

	TrackPinId = *trackPinId;
	ResolutionPinId = *resolutionPinId;
	OutColorPinId = *outColorPinId;
	OutDepthPinId = *outDepthPinId;
	InColorPinId = *inColorPinId;
	QuadP0PinId = *quadP0PinId;
	QuadP1PinId = *quadP1PinId;
	QuadP2PinId = *quadP2PinId;
	QuadP3PinId = *quadP3PinId;

	if (!outColorNew)
		AppInterface.Nodos->NotifyPinValueChanged(OutColorPinId, nos::Buffer::From(OutColor.TextureDef));
	if (!outDepthNew)
		AppInterface.Nodos->NotifyPinValueChanged(OutDepthPinId, nos::Buffer::From(OutDepth.TextureDef));
	if (!inColorNew)
		AppInterface.Nodos->NotifyPinValueChanged(InColorPinId, nos::Buffer::From(InColor.TextureDef));
	if (overrideResolution)
	{
		nos::fb::vec2u resolution{OutputWidth, OutputHeight};
		AppInterface.Nodos->NotifyPinValueChanged(ResolutionPinId, nos::Buffer::From(resolution));
	}

	OutputResolutionChanged = true;
	CreateFrameResources();
}

inline void SceneAppNode::OnRemoved()
{
	assert(!Sync);
	DestroyFrameResources();
	DestroyExportedTexture(OutColor);
	DestroyExportedTexture(OutDepth);
	DestroyExportedTexture(InColor);
}

inline void SceneAppNode::OnPinValueChanges(std::unordered_map<nos::uuid, nos::Buffer> const& pinValues)
{
	for (const auto& [pinId, data] : pinValues)
	{
		if (pinId == ResolutionPinId)
		{
			const nos::fb::vec2u& newRes = *reinterpret_cast<const nos::fb::vec2u*>(data.Data());
			if (newRes.x() == 0 || newRes.y() == 0)
				continue;
			if (newRes.x() != OutputWidth || newRes.y() != OutputHeight)
			{
				OutputResolutionChanged = true;
				OutputWidth = newRes.x();
				OutputHeight = newRes.y();
			}
		}
		else if (pinId == TrackPinId)
		{
			flatbuffers::GetRoot<nos::track::Track>(data.Data())->UnPackTo(&Track);
		}
		else if (pinId == QuadP0PinId)
		{
			QuadP0 = *reinterpret_cast<const nos::fb::vec3*>(data.Data());
		}
		else if (pinId == QuadP1PinId)
		{
			QuadP1 = *reinterpret_cast<const nos::fb::vec3*>(data.Data());
		}
		else if (pinId == QuadP2PinId)
		{
			QuadP2 = *reinterpret_cast<const nos::fb::vec3*>(data.Data());
		}
		else if (pinId == QuadP3PinId)
		{
			QuadP3 = *reinterpret_cast<const nos::fb::vec3*>(data.Data());
		}
	}
}

inline void SceneAppNode::OnPreExecute(void* frameCtx, uint64_t frameCounter)
{
	if (!Sync)
	{
		assert(false);
		return;
	}

	// Apply track data to camera
	using namespace DirectX;
	XMFLOAT3 eyePos = XMFLOAT3(-Track.location.y() / 100.0f, Track.location.z() / 100.0f, -Track.location.x() / 100.0f);
	
	// Convert rotation from degrees to radians and build rotation matrix
	float pitch = XMConvertToRadians(Track.rotation.y());
	float yaw = XMConvertToRadians(Track.rotation.z());
	float roll = XMConvertToRadians(Track.rotation.x());
	
	XMMATRIX rotMat = XMMatrixRotationRollPitchYaw(pitch, yaw, roll);
	XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, -1, 0), rotMat);
	XMVECTOR upVec = XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), rotMat);
	
	XMVECTOR eyePosVec = XMLoadFloat3(&eyePos);
	XMVECTOR centerVec = XMVectorAdd(eyePosVec, forward);
	
	XMFLOAT3 center, up;
	XMStoreFloat3(&center, centerVec);
	XMStoreFloat3(&up, upVec);
	
	AppInterface.Renderer.SetCameraPosition(eyePos);
	AppInterface.Renderer.SetCameraTarget(center);
	AppInterface.Renderer.GetCamera().FovY = XMConvertToRadians(Track.fov);

	auto inputWorkGroupResources = BeginNewFrameForWorkGroupResources(*Sync, frameCounter, true);
	
	if (OutputResolutionChanged)
	{
		// Wait for GPU to finish all work
		ComPtr<ID3D12Fence> waitFence;
		Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&waitFence));
		CommandQueue->Signal(waitFence.Get(), 1);
		HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
		waitFence->SetEventOnCompletion(1, event);
		WaitForSingleObject(event, INFINITE);
		CloseHandle(event);
		
		AppInterface.Renderer.Resize(OutputWidth, OutputHeight);
		
		// Recreate textures if needed
		if (OutColor.TextureDef.width != OutputWidth || OutColor.TextureDef.height != OutputHeight)
		{
			DestroyExportedTexture(OutColor);
			OutColor = *CreateExportedTexture(OutputWidth, OutputHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
				D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, false);
			AppInterface.Nodos->NotifyPinValueChanged(OutColorPinId, nos::Buffer::From(OutColor.TextureDef));
		}
		
		if (OutDepth.TextureDef.width != OutputWidth || OutDepth.TextureDef.height != OutputHeight)
		{
			DestroyExportedTexture(OutDepth);
			OutDepth = *CreateExportedTexture(OutputWidth, OutputHeight, DXGI_FORMAT_D32_FLOAT,
				D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, true);
			AppInterface.Nodos->NotifyPinValueChanged(OutDepthPinId, nos::Buffer::From(OutDepth.TextureDef));
		}
		
		if (InColor.TextureDef.width != OutputWidth || InColor.TextureDef.height != OutputHeight)
		{
			DestroyExportedTexture(InColor);
			InColor = *CreateExportedTexture(OutputWidth, OutputHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
				D3D12_RESOURCE_FLAG_NONE, false);
			AppInterface.Nodos->NotifyPinValueChanged(InColorPinId, nos::Buffer::From(InColor.TextureDef));
		}
		
		OutputResolutionChanged = false;
	}

	// Set input texture for the renderer
	AppInterface.Renderer.SetInputTexture(InColor.Resource.Get());

	// Set quad corners from Nodos pins (convert from cm to meters and apply track coordinate mapping)
	// Track mapping: X→-Z, Y→-X, Z→Y
	DirectX::XMFLOAT3 p0(-QuadP0.y() / 100.0f, QuadP0.z() / 100.0f, -QuadP0.x() / 100.0f);
	DirectX::XMFLOAT3 p1(-QuadP1.y() / 100.0f, QuadP1.z() / 100.0f, -QuadP1.x() / 100.0f);
	DirectX::XMFLOAT3 p2(-QuadP2.y() / 100.0f, QuadP2.z() / 100.0f, -QuadP2.x() / 100.0f);
	DirectX::XMFLOAT3 p3(-QuadP3.y() / 100.0f, QuadP3.z() / 100.0f, -QuadP3.x() / 100.0f);
	AppInterface.Renderer.SetTexturedQuadCorners(p0, p1, p2, p3);

	InputCopies(inputWorkGroupResources.CommandList.Get(), frameCounter);
	auto submitInfo = PrepareSubmitInfo(frameCounter, true);
	
#if NOS_ENABLE_SYNC_LOGS
	std::cout << "Submitting input work for frame " << frameCounter << " with wait value " << submitInfo.WaitValue
		<< " and signal value " << submitInfo.SignalValue << std::endl;
#endif
	
	SubmitWork(inputWorkGroupResources, submitInfo);
	Sync->LastInputFrameNumber = frameCounter;
}

inline void SceneAppNode::OnPostExecute(void* frameCtx, uint64_t frameCounter)
{
	if (!Sync)
	{
		assert(Sync);
		return;
	}

	auto outputWorkGroupResources = BeginNewFrameForWorkGroupResources(*Sync, frameCounter, false);
	OutputCopies(outputWorkGroupResources.CommandList.Get(), frameCounter);
	auto submitInfo = PrepareSubmitInfo(frameCounter, false);
	
#if NOS_ENABLE_SYNC_LOGS
	std::cout << "Submitting output work for frame " << frameCounter << " with wait value " << submitInfo.WaitValue
		<< " and signal value " << submitInfo.SignalValue << std::endl;
#endif
	
	SubmitWork(outputWorkGroupResources, submitInfo);
	Sync->LastOutputFrameNumber = frameCounter;
}

void SceneAppNode::OnExecutionStateChanged(nos::app::ExecutionState newState, nos::app::ExecutionState oldState)
{
	if (oldState == nos::app::ExecutionState::SYNCED)
	{
		assert(Sync);
		SignalAndWaitAllFrames();
		DestroyFence(Sync->InputFence);
		DestroyFence(Sync->OutputFence);
		Sync = std::nullopt;
	}
	
	if (newState == nos::app::ExecutionState::SYNCED)
	{
		Sync = SyncState{};
		Sync->InputFence = *CreateExportedFence();
		Sync->OutputFence = *CreateExportedFence();
		SendSemaphoresToNodos();
		OutputResolutionChanged = true;
	}
}

inline std::optional<ExportedFence> SceneAppNode::CreateExportedFence()
{
	ComPtr<ID3D12Fence> fence;
	if (FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))))
		return std::nullopt;

	HANDLE sharedHandle = nullptr;
	if (FAILED(Device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle)))
		return std::nullopt;

	return ExportedFence{fence, sharedHandle};
}

inline std::optional<ExportedTexture> SceneAppNode::CreateExportedTexture(uint32_t width, uint32_t height, 
	DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, bool isDepth)
{
	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Flags = flags;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	ComPtr<ID3D12Resource> resource;
	D3D12_RESOURCE_STATES initialState = isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COMMON;
	
	if (FAILED(Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &desc, 
		initialState, nullptr, IID_PPV_ARGS(&resource))))
		return std::nullopt;

	HANDLE sharedHandle = nullptr;
	if (FAILED(Device->CreateSharedHandle(resource.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle)))
		return std::nullopt;

	// Create Nodos texture definition (uses Vulkan-style format)
	nos::sys::vulkan::TTexture texDef{};
	texDef.resolution = nos::sys::vulkan::SizePreset::CUSTOM;
	texDef.width = width;
	texDef.height = height;

	switch (format)
	{
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			texDef.format = nos::sys::vulkan::Format::R8G8B8A8_UNORM; break;
	case DXGI_FORMAT_D32_FLOAT:
		texDef.format = nos::sys::vulkan::Format::D32_SFLOAT; break;
		default: assert(false && "Unsupported format"); break;
	}

	texDef.unscaled = true;
	texDef.offset = 0;
	
	auto& extMem = texDef.external_memory;
	extMem.mutate_handle((uint64_t)sharedHandle);
	extMem.mutate_handle_type(
		NOS_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE); // VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
	auto resourceDesc = resource->GetDesc();
	extMem.mutate_allocation_size(Device->GetResourceAllocationInfo(0, 1, &resourceDesc).SizeInBytes);  // DX12 doesn't expose allocation size the same way
	extMem.mutate_pid(_getpid());

	return ExportedTexture{resource, sharedHandle, texDef, initialState};
}

inline void SceneAppNode::DestroyExportedTexture(ExportedTexture& exportedTexture)
{
	exportedTexture.Resource.Reset();
	if (exportedTexture.SharedHandle)
		CloseHandle(exportedTexture.SharedHandle);
}

inline void SceneAppNode::InputCopies(ID3D12GraphicsCommandList* cmd, uint64_t frameCounter)
{
	// Transition InColor for reading if needed
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = InColor.Resource.Get();
	barrier.Transition.StateBefore = InColor.CurrentState;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	
	if (InColor.CurrentState != D3D12_RESOURCE_STATE_COPY_SOURCE)
	{
		cmd->ResourceBarrier(1, &barrier);
		InColor.CurrentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
	}
	
	// Input texture copy could be implemented here if needed for compositing
	// For now, we just ensure it's in the correct state
}

inline void SceneAppNode::OutputCopies(ID3D12GraphicsCommandList* cmd, uint64_t frameCounter)
{
	// Get renderer's output textures
	ID3D12Resource* rendererColor = AppInterface.Renderer.GetOutputTexture();
	ID3D12Resource* rendererDepth = AppInterface.Renderer.GetDepthStencilBuffer();
	
	if (rendererColor)
	{
		// Transition renderer output to copy source
		D3D12_RESOURCE_BARRIER barriers[2] = {};
		barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[0].Transition.pResource = rendererColor;
		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		
		// Transition OutColor to copy dest
		barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[1].Transition.pResource = OutColor.Resource.Get();
		barriers[1].Transition.StateBefore = OutColor.CurrentState;
		barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		
		cmd->ResourceBarrier(2, barriers);
		
		// Copy color texture
		cmd->CopyResource(OutColor.Resource.Get(), rendererColor);
		
		// Transition back
		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		
		cmd->ResourceBarrier(2, barriers);
		OutColor.CurrentState = D3D12_RESOURCE_STATE_COMMON;
	}
	
	if (rendererDepth)
	{
		// Transition depth buffer to copy source
		D3D12_RESOURCE_BARRIER barriers[2] = {};
		barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[0].Transition.pResource = rendererDepth;
		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		
		// Transition OutDepth to copy dest
		barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[1].Transition.pResource = OutDepth.Resource.Get();
		barriers[1].Transition.StateBefore = OutDepth.CurrentState;
		barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		
		cmd->ResourceBarrier(2, barriers);
		
		// Copy depth texture
		cmd->CopyResource(OutDepth.Resource.Get(), rendererDepth);
		
		// Transition back
		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		
		cmd->ResourceBarrier(2, barriers);
		OutDepth.CurrentState = D3D12_RESOURCE_STATE_COMMON;
	}
}

inline void SceneAppNode::SendSemaphoresToNodos()
{
	assert(Sync);
	uint64_t inputSemaphore = (uint64_t)Sync->InputFence.SharedHandle;
	uint64_t outputSemaphore = (uint64_t)Sync->OutputFence.SharedHandle;
	
	flatbuffers::FlatBufferBuilder mb;
	auto offset = nos::CreateAppEventOffset(mb, nos::app::CreateSetSyncSemaphores(mb, &NodeId, _getpid(), 
		inputSemaphore, outputSemaphore));
	mb.Finish(offset);
	auto buf = mb.Release();
	auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
	AppInterface.Nodos->GetClient().Send(*root);
}

}  // namespace nos::dxapp
