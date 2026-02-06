#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

namespace nos
{
namespace dxapp
{

class SceneRenderer;

class NodosSceneInterface
{
public:
	NodosSceneInterface(SceneRenderer& renderer);
	~NodosSceneInterface();
	
	// Set SDK DLL path before Initialize (optional)
	void SetSdkDllPath(const std::string& path);
	
	// Initialize with DX12 device and command queue
	void Initialize(ID3D12Device* device, ID3D12CommandQueue* commandQueue, DXGI_FORMAT outputFormat);
	
	// Frame lifecycle
	void PreFrame();
	void PostFrame();
	
	// Resize handling
	void OnResize(uint32_t width, uint32_t height);
	
	// Cleanup
	void Shutdown();
	
	// Check if synced with Nodos
	bool IsSynced() const;

	struct InternalState;
private:
	std::unique_ptr<InternalState> m_InternalState;
	SceneRenderer& m_Renderer;
};

}  // namespace dxapp
}  // namespace nos
