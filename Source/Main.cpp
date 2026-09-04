// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
// Simplified Main - Just renders with SceneRenderer to swapchain

#define NOMINMAX 1
#include <dxgiformat.h>
#include <dxgi1_6.h>
#include <directx/d3dx12.h>
#include <DirectXMath.h>
#include <d3dcompiler.h>

#define SDL_MAIN_HANDLED 1
#include <SDL2/SDL.h>
#include <SDL_syswm.h>
#include <wrl/client.h>
#include <comdef.h>
using Microsoft::WRL::ComPtr;

#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>

#include "SceneRenderer.h"
#include "NodosSceneInterface.h"

#define DX12_ENABLE_DEBUG_LAYER

#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif

inline void Must(bool cond, const char* errMsg = "Unspecified")
{
	if (cond)
		return;
	std::cerr << "Error: " << errMsg << std::endl;
	std::cerr << "Details: " << GetLastError() << std::endl;
	throw;
}

inline void Must(HRESULT res, const char* errMsg = "Unspecified")
{
	if (S_OK == res)
		return;
	std::cerr << "Error: " << errMsg << std::endl;
	std::cerr << "Details: " << GetLastError() << std::endl;
	_com_error err(res);
	std::cerr << err.ErrorMessage() << std::endl;
	throw;
}

using namespace DirectX;

struct SimpleApp
{
	static constexpr int BACK_BUFFER_COUNT = 3;

	struct
	{
		int Width = 1280;
		int Height = 720;
		HWND Handle = nullptr;
	} Window;

	D3D12_VIEWPORT Viewport;
	D3D12_RECT ScissorRect;
	ComPtr<ID3D12Device2> Device = nullptr;
	ComPtr<ID3D12CommandAllocator> CmdAllocators[BACK_BUFFER_COUNT]{};
	ComPtr<ID3D12CommandQueue> CmdQueue = nullptr;
	ComPtr<ID3D12GraphicsCommandList> CmdList = nullptr;

	ComPtr<IDXGISwapChain3> SwapChain = nullptr;
	ComPtr<ID3D12Resource> SwapChainRTResources[BACK_BUFFER_COUNT] = {};

	ComPtr<ID3D12DescriptorHeap> RTVHeap = nullptr;
	uint32_t RTVDescriptorSize = 0;

	ComPtr<ID3D12Fence> Fence = nullptr;
	HANDLE FenceEvent = nullptr;
	UINT64 FenceValues[BACK_BUFFER_COUNT]{};
	uint32_t SwapChainFrameIndex = 0;

	bool EnableVsync = true;

	// Scene Renderer
	std::unique_ptr<nos::dxapp::SceneRenderer> SceneRenderer;
	std::unique_ptr<nos::dxapp::NodosSceneInterface> NodosInterface;
	float Time = 0.0f;
	bool NodosFrame = false;

	SimpleApp(HWND windowHandle, int width, int height, bool vsyncEnabled, std::optional<uint32_t> gpuIndex, const std::string& sdkDllPath) :
		Window{width, height, windowHandle},
		Viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)},
		ScissorRect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)},
		EnableVsync(vsyncEnabled)
	{
#ifdef DX12_ENABLE_DEBUG_LAYER
		ComPtr<ID3D12Debug> pdx12Debug = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pdx12Debug))))
			pdx12Debug->EnableDebugLayer();
#endif

		ComPtr<IDXGIFactory6> dxgiFactory;
		Must(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)), "Failed to create DXGIFactory");

		std::vector<ComPtr<IDXGIAdapter1>> adapters;
		ComPtr<IDXGIAdapter1> adapter;
		UINT index = 0;

		while (dxgiFactory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND)
		{
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			{
				++index;
				continue;
			}

			std::wcout << L"[" << index << L"] " << desc.Description << std::endl;
			adapters.push_back(adapter);
			++index;
		}

		Must(!adapters.empty(), "No suitable adapter found.");

		uint32_t selectedAdapter = 0;
		if (adapters.size() > 1)
		{
			if (gpuIndex)
			{
				selectedAdapter = *gpuIndex;
			}
			else
			{
				// Automatically select the first discrete GPU (highest dedicated video memory)
				size_t maxDedicatedMemory = 0;
				for (uint32_t i = 0; i < adapters.size(); ++i)
				{
					DXGI_ADAPTER_DESC1 desc;
					adapters[i]->GetDesc1(&desc);
					if (desc.DedicatedVideoMemory > maxDedicatedMemory)
					{
						maxDedicatedMemory = desc.DedicatedVideoMemory;
						selectedAdapter = i;
					}
				}
				std::wcout << L"Auto-selected discrete GPU: " << selectedAdapter << std::endl;
			}
		}

		std::wcout << L"Selected GPU: " << selectedAdapter << std::endl;

		Must(!(selectedAdapter < 0 || selectedAdapter >= static_cast<uint32_t>(adapters.size())), "Invalid adapter selection.");

		Must(D3D12CreateDevice(adapters[selectedAdapter].Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device)),
			"Unable to create D3D12 Device");

#ifdef DX12_ENABLE_DEBUG_LAYER
		if (pdx12Debug != nullptr)
		{
			ComPtr<ID3D12InfoQueue> pInfoQueue = nullptr;
			Device->QueryInterface(IID_PPV_ARGS(&pInfoQueue));
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
			pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false);
		}
#endif

		D3D12_COMMAND_QUEUE_DESC commandQueueDesc = {};
		commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		Must(Device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&CmdQueue)),
			 "Unable to create CommandQueue");

		CreateRTVHeap();
		CreateSwapChain();
		CreateCommandAllocatorsAndList();
		CreateFence();
		CreateScene(sdkDllPath);
	}

	void CreateRTVHeap()
	{
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = BACK_BUFFER_COUNT;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		Must(Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&RTVHeap)), "Unable to create RTV DescriptorHeap");
		RTVDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}

	void CreateSwapChain()
	{
		DXGI_SWAP_CHAIN_DESC1 sd{};
		sd.BufferCount = BACK_BUFFER_COUNT;
		sd.Width = Window.Width;
		sd.Height = Window.Height;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.Flags = 0;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
		sd.Scaling = DXGI_SCALING_STRETCH;
		sd.Stereo = FALSE;

		ComPtr<IDXGISwapChain1> swapChain1 = nullptr;
		ComPtr<IDXGIFactory2> dxgiFactory;
		
		uint32_t dxgiFactoryCreateFlags = 0;
#ifdef DX12_ENABLE_DEBUG_LAYER
		dxgiFactoryCreateFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
		Must(CreateDXGIFactory2(dxgiFactoryCreateFlags, IID_PPV_ARGS(&dxgiFactory)), "Unable to create DXGIFactory2");
		Must(dxgiFactory->CreateSwapChainForHwnd(CmdQueue.Get(), Window.Handle, &sd, nullptr, nullptr, &swapChain1));
		Must(swapChain1->QueryInterface(IID_PPV_ARGS(&SwapChain)));
		SwapChainFrameIndex = SwapChain->GetCurrentBackBufferIndex();

		// Create RTVs for swap chain
		auto rtvHandle = RTVHeap->GetCPUDescriptorHandleForHeapStart();
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		
		for (int i = 0; i < BACK_BUFFER_COUNT; i++)
		{
			Must(SwapChain->GetBuffer(i, IID_PPV_ARGS(&SwapChainRTResources[i])));
			Device->CreateRenderTargetView(SwapChainRTResources[i].Get(), &rtvDesc, rtvHandle);
			rtvHandle.ptr += RTVDescriptorSize;
		}
	}

	void CreateCommandAllocatorsAndList()
	{
		for (int i = 0; i < BACK_BUFFER_COUNT; i++)
		{
			Must(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&CmdAllocators[i])));
		}

		Must(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CmdAllocators[SwapChainFrameIndex].Get(),
			nullptr, IID_PPV_ARGS(&CmdList)), "Failed to create command list");
		Must(CmdList->Close());
	}

	void CreateFence()
	{
		Must(Device->CreateFence(FenceValues[SwapChainFrameIndex], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
		FenceValues[SwapChainFrameIndex]++;

		FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (FenceEvent == nullptr)
		{
			Must(HRESULT_FROM_WIN32(GetLastError()));
		}

		WaitForGpu();
	}

	void CreateScene(const std::string& sdkDllPath)
	{
		SceneRenderer = std::make_unique<nos::dxapp::SceneRenderer>();
		SceneRenderer->Initialize(Device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, Window.Width, Window.Height);

		// Set up camera
		SceneRenderer->SetCameraPosition({0.0f, 3.0f, -7.0f});
		SceneRenderer->SetCameraTarget({0.0f, 0.0f, 0.0f});

		// Set up light
		SceneRenderer->SetLightDirection({-0.5f, -1.0f, 0.5f});
		SceneRenderer->SetLightColor({1.0f, 0.95f, 0.9f}, 1.2f);

		// Add a ground plane
		SceneRenderer->AddPlane(
			{0.0f, 0.0f, 0.0f},
			{10.0f, 1.0f, 10.0f},
			{0.7f, 0.7f, 0.7f, 1.0f}
		);

		// Add some cubes
		SceneRenderer->AddCube(
			{-2.0f, 0.5f, 0.0f},
			{1.0f, 1.0f, 1.0f},
			{1.0f, 0.3f, 0.3f, 1.0f}  // Red
		);

		SceneRenderer->AddCube(
			{2.0f, 0.5f, 0.0f},
			{1.0f, 1.0f, 1.0f},
			{0.3f, 1.0f, 0.3f, 1.0f}  // Green
		);

		SceneRenderer->AddCube(
			{0.0f, 0.5f, 2.0f},
			{1.0f, 1.0f, 1.0f},
			{0.3f, 0.3f, 1.0f, 1.0f}  // Blue
		);

		// Add a textured quad (billboard) - will display input texture from Nodos
		SceneRenderer->AddTexturedQuad(
			{0.0f, 2.5f, -2.0f},  // Position above the ground, behind center
			{3.0f, 2.0f, 1.0f},   // Scale (3x2 aspect ratio)
			{1.0f, 1.0f, 1.0f, 1.0f}  // White with slight transparency
		);

		// Initialize Nodos interface
		try
		{
			NodosInterface = std::make_unique<nos::dxapp::NodosSceneInterface>(*SceneRenderer);
			if (!sdkDllPath.empty())
			{
				NodosInterface->SetSdkDllPath(sdkDllPath);
			}
			NodosInterface->Initialize(Device.Get(), CmdQueue.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
			std::cout << "Nodos interface initialized successfully" << std::endl;
		}
		catch (const std::exception& e)
		{
			std::cerr << "Failed to initialize Nodos interface: " << e.what() << std::endl;
			NodosInterface.reset();
		}
	}

	void WaitForGpu()
	{
		Must(CmdQueue->Signal(Fence.Get(), FenceValues[SwapChainFrameIndex]));
		Must(Fence->SetEventOnCompletion(FenceValues[SwapChainFrameIndex], FenceEvent));
		WaitForSingleObjectEx(FenceEvent, INFINITE, FALSE);
		FenceValues[SwapChainFrameIndex]++;
	}

	void MoveToNextFrame()
	{
		const UINT64 currentFenceValue = FenceValues[SwapChainFrameIndex];
		Must(CmdQueue->Signal(Fence.Get(), currentFenceValue));

		SwapChainFrameIndex = SwapChain->GetCurrentBackBufferIndex();

		if (Fence->GetCompletedValue() < FenceValues[SwapChainFrameIndex])
		{
			Must(Fence->SetEventOnCompletion(FenceValues[SwapChainFrameIndex], FenceEvent));
			WaitForSingleObjectEx(FenceEvent, INFINITE, FALSE);
		}

		FenceValues[SwapChainFrameIndex] = currentFenceValue + 1;
	}

	// Nodos PreFrame: while synced this blocks until Nodos requests a frame, and applies the
	// track to the camera.
	void BeginFrame()
	{
		NodosFrame = NodosInterface && NodosInterface->PreFrame();
	}

	// While Nodos drives the app the scene's time belongs to Nodos: each frame it requests
	// stands for the delta it supplied, and a repaint it did not request stands for nothing.
	// Without a fixed delta (free run) or without Nodos, wall-clock time is used.
	float FrameDeltaSeconds(float wallClockDeltaSeconds) const
	{
		if (!NodosInterface || !NodosInterface->IsSynced())
			return wallClockDeltaSeconds;
		if (!NodosFrame)
			return 0.0f;
		return NodosInterface->GetFixedDeltaSeconds().value_or(wallClockDeltaSeconds);
	}

	void UpdateScene(float deltaTime)
	{
		Time += deltaTime;

		// Rotate camera around scene (only if Nodos is not synced)
		if (!NodosInterface || !NodosInterface->IsSynced())
		{
			float radius = 7.0f;
			float camX = radius * sin(Time * 0.5f);
			float camZ = radius * cos(Time * 0.5f);
			SceneRenderer->SetCameraPosition({camX, 3.0f, camZ});
			SceneRenderer->SetCameraTarget({0.0f, 0.0f, 0.0f});
		}

		// Animate the red cube (index 1)
		auto& redCube = SceneRenderer->GetObject(1);
		redCube.Rotation.y = Time;
		redCube.Position.y = 0.5f + 0.5f * sin(Time * 2.0f);

		// Animate the green cube (index 2)
		auto& greenCube = SceneRenderer->GetObject(2);
		greenCube.Rotation.x = Time * 0.7f;
		greenCube.Rotation.z = Time * 0.3f;

		// Animate the blue cube (index 3)
		auto& blueCube = SceneRenderer->GetObject(3);
		blueCube.Scale.x = 1.0f + 0.3f * sin(Time * 1.5f);
		blueCube.Scale.y = 1.0f + 0.3f * sin(Time * 1.5f + 1.0f);
		blueCube.Scale.z = 1.0f + 0.3f * sin(Time * 1.5f + 2.0f);
	}

	void Render()
	{
		// If not synced with Nodos, ensure renderer is at swapchain resolution
		if (!NodosInterface || !NodosInterface->IsSynced())
		{
			// Check if renderer needs to be resized to match swapchain
			auto rendererDesc = SceneRenderer->GetOutputTexture()->GetDesc();
			if (rendererDesc.Width != Window.Width || rendererDesc.Height != Window.Height)
			{
				WaitForGpu();
				SceneRenderer->Resize(Window.Width, Window.Height);
			}
		}

		// Reset command allocator and list
		Must(CmdAllocators[SwapChainFrameIndex]->Reset());
		Must(CmdList->Reset(CmdAllocators[SwapChainFrameIndex].Get(), nullptr));

		// Render scene to SceneRenderer's internal texture
		SceneRenderer->Render(CmdList.Get());

		// Check if we can copy to swapchain (dimensions must match)
		auto rendererDesc = SceneRenderer->GetOutputTexture()->GetDesc();
		bool canCopyToSwapchain = (rendererDesc.Width == Window.Width && rendererDesc.Height == Window.Height);

		if (canCopyToSwapchain)
		{
			// Copy from SceneRenderer output to swapchain
			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				SceneRenderer->GetOutputTexture(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_COPY_SOURCE
			);
			CmdList->ResourceBarrier(1, &barrier);

			barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				SwapChainRTResources[SwapChainFrameIndex].Get(),
				D3D12_RESOURCE_STATE_PRESENT,
				D3D12_RESOURCE_STATE_COPY_DEST
			);
			CmdList->ResourceBarrier(1, &barrier);

			// Copy the texture
			CmdList->CopyResource(SwapChainRTResources[SwapChainFrameIndex].Get(), SceneRenderer->GetOutputTexture());

			// Transition resources back
			barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				SceneRenderer->GetOutputTexture(),
				D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET
			);
			CmdList->ResourceBarrier(1, &barrier);

			barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				SwapChainRTResources[SwapChainFrameIndex].Get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_PRESENT
			);
			CmdList->ResourceBarrier(1, &barrier);
		}
		else
		{
			// Dimensions don't match - use shader blit to resize
			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				SceneRenderer->GetOutputTexture(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
			);
			CmdList->ResourceBarrier(1, &barrier);

			barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				SwapChainRTResources[SwapChainFrameIndex].Get(),
				D3D12_RESOURCE_STATE_PRESENT,
				D3D12_RESOURCE_STATE_RENDER_TARGET
			);
			CmdList->ResourceBarrier(1, &barrier);

			// Get RTV handle for swapchain backbuffer
			CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(RTVHeap->GetCPUDescriptorHandleForHeapStart(), SwapChainFrameIndex, RTVDescriptorSize);

			// Blit with resize
			SceneRenderer->BlitToRenderTarget(CmdList.Get(), rtvHandle, Window.Width, Window.Height);

			// Transition resources back
			barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				SceneRenderer->GetOutputTexture(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET
			);
			CmdList->ResourceBarrier(1, &barrier);

			barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				SwapChainRTResources[SwapChainFrameIndex].Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PRESENT
			);
			CmdList->ResourceBarrier(1, &barrier);
		}

		// Execute command list
		Must(CmdList->Close());
		ID3D12CommandList* ppCommandLists[] = {CmdList.Get()};
		CmdQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

		// Present
		Must(SwapChain->Present(EnableVsync ? 1 : 0, 0));

		MoveToNextFrame();

		// Nodos PostFrame (handles output sync)
		if (NodosInterface)
			NodosInterface->PostFrame();
	}

	void Destroy()
	{
		WaitForGpu();
		
		if (NodosInterface)
		{
			NodosInterface->Shutdown();
			NodosInterface.reset();
		}
		
		CloseHandle(FenceEvent);
	}
};

int SimpleAppMain(int windowWidth, int windowHeight, std::optional<uint32_t> gpuIndex, bool vsync, const std::string& sdkDllPath)
{
	SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);

	SDL_Init(SDL_INIT_VIDEO);
	SDL_Window* window = SDL_CreateWindow(
		"Simple DX12 Scene Renderer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		windowWidth, windowHeight, window_flags);
	
	if (!window)
	{
		auto error = SDL_GetError();
		std::cout << "Failed to create window: " << error << std::endl;
		return 1;
	}

	HWND windowHandle = nullptr;
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	SDL_GetWindowWMInfo(window, &wmInfo);
	windowHandle = wmInfo.info.win.window;

	SimpleApp app(windowHandle, windowWidth, windowHeight, vsync, gpuIndex, sdkDllPath);

	// Main loop
	SDL_Event event;
	bool running = true;
	auto lastTime = std::chrono::high_resolution_clock::now();

	while (running)
	{
		auto currentTime = std::chrono::high_resolution_clock::now();
		float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
		lastTime = currentTime;

		SDL_PumpEvents();
		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_QUIT)
			{
				running = false;
				break;
			}
		}

		app.BeginFrame();
		app.UpdateScene(app.FrameDeltaSeconds(deltaTime));
		app.Render();
	}

	app.Destroy();

	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}

int main(int argc, char** argv)
{
	std::optional<uint32_t> gpuIndex;
	int windowWidth = 1280;
	int windowHeight = 720;
	bool vsync = false;
	std::string sdkDllPath;

	if (argc > 1)
	{
		for (int i = 1; i < argc; i++)
		{
			if (strcmp(argv[i], "--gpu") == 0 && i + 1 < argc)
				gpuIndex = std::atoi(argv[++i]);
			else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
				windowWidth = std::atoi(argv[++i]);
			else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
				windowHeight = std::atoi(argv[++i]);
			else if (strcmp(argv[i], "--sdk-dll") == 0 && i + 1 < argc)
				sdkDllPath = argv[++i];
			else
				std::cerr << "Unknown argument: " << argv[i] << std::endl;
		}
	}

	auto ret = SimpleAppMain(windowWidth, windowHeight, gpuIndex, vsync, sdkDllPath);

#ifdef DX12_ENABLE_DEBUG_LAYER
	if (ComPtr<IDXGIDebug1> pDebug = nullptr; SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug))))
		pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
#endif

	return ret;
}
