#pragma once

#include <d3d12.h>
#include <directx/d3dx12.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nos::dxapp
{

struct Camera
{
	DirectX::XMFLOAT3 Position = {0.0f, 2.0f, -5.0f};
	DirectX::XMFLOAT3 Target = {0.0f, 0.0f, 0.0f};
	DirectX::XMFLOAT3 Up = {0.0f, 1.0f, 0.0f};
	float FovY = DirectX::XM_PIDIV4;
	float AspectRatio = 16.0f / 9.0f;
	float NearPlane = 0.1f;
	float FarPlane = 100.0f;

	DirectX::XMMATRIX GetViewMatrix() const;
	DirectX::XMMATRIX GetProjectionMatrix() const;
};

struct DirectionalLight
{
	DirectX::XMFLOAT3 Direction = {-0.5f, -1.0f, 0.5f};
	DirectX::XMFLOAT3 Color = {1.0f, 1.0f, 1.0f};
	float Intensity = 1.0f;
};

struct SceneObject
{
	enum class Type
	{
		Cube,
		Plane,
		TexturedQuad
	};

	Type ObjectType = Type::Cube;
	DirectX::XMFLOAT3 Position = {0.0f, 0.0f, 0.0f};
	DirectX::XMFLOAT3 Rotation = {0.0f, 0.0f, 0.0f};
	DirectX::XMFLOAT3 Scale = {1.0f, 1.0f, 1.0f};
	DirectX::XMFLOAT4 Color = {1.0f, 1.0f, 1.0f, 1.0f};

	DirectX::XMMATRIX GetWorldMatrix() const;
};

struct Vertex
{
	DirectX::XMFLOAT3 Position;
	DirectX::XMFLOAT3 Normal;
	DirectX::XMFLOAT4 Color;
	DirectX::XMFLOAT2 TexCoord;
};

struct ConstantBufferData
{
	DirectX::XMMATRIX WorldViewProj;
	DirectX::XMMATRIX World;
	DirectX::XMFLOAT3 LightDirection;
	float Padding1;
	DirectX::XMFLOAT3 LightColor;
	float LightIntensity;
	DirectX::XMFLOAT3 CameraPosition;
	float Padding2;
	DirectX::XMFLOAT4 ObjectColor;
};

class SceneRenderer
{
public:
	SceneRenderer();
	~SceneRenderer();

	void Initialize(ID3D12Device* device, DXGI_FORMAT outputFormat, uint32_t width, uint32_t height);
	void Render(ID3D12GraphicsCommandList* cmdList);
	void Resize(uint32_t width, uint32_t height);

	// Camera control
	Camera& GetCamera() { return m_Camera; }
	void SetCameraPosition(const DirectX::XMFLOAT3& position);
	void SetCameraTarget(const DirectX::XMFLOAT3& target);

	// Light control
	DirectionalLight& GetLight() { return m_Light; }
	void SetLightDirection(const DirectX::XMFLOAT3& direction);
	void SetLightColor(const DirectX::XMFLOAT3& color, float intensity);

	// Object management
	size_t AddCube(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& scale = {1.0f, 1.0f, 1.0f}, const DirectX::XMFLOAT4& color = {1.0f, 1.0f, 1.0f, 1.0f});
	size_t AddPlane(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& scale = {1.0f, 1.0f, 1.0f}, const DirectX::XMFLOAT4& color = {0.8f, 0.8f, 0.8f, 1.0f});
	size_t AddTexturedQuad(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& scale = {1.0f, 1.0f, 1.0f}, const DirectX::XMFLOAT4& color = {1.0f, 1.0f, 1.0f, 1.0f});
	SceneObject& GetObject(size_t index);
	void ClearObjects();

	// Texture input from Nodos
	void SetInputTexture(ID3D12Resource* texture);
	void SetTexturedQuadCorners(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, const DirectX::XMFLOAT3& p2, const DirectX::XMFLOAT3& p3);

	// Output texture access
	ID3D12Resource* GetOutputTexture() const { return m_OutputTexture.Get(); }
	ID3D12Resource* GetDepthStencilBuffer() const { return m_DepthStencilBuffer.Get(); }

private:
	void CreateRootSignature(ID3D12Device* device);
	void CreatePipelineState(ID3D12Device* device, DXGI_FORMAT outputFormat);
	void CreateTexturedPipelineState(ID3D12Device* device, DXGI_FORMAT outputFormat);
	void CreateGeometryBuffers(ID3D12Device* device);
	void CreateConstantBuffer(ID3D12Device* device);
	void CreateOutputTexture(ID3D12Device* device, uint32_t width, uint32_t height);
	void CreateDepthStencilBuffer(ID3D12Device* device, uint32_t width, uint32_t height);
	void CreateSRVHeap(ID3D12Device* device);
	void UpdateQuadVertexBuffer();

	void UpdateConstantBuffer(size_t objectIndex);

	ComPtr<ID3D12Device> m_Device;
	ComPtr<ID3D12RootSignature> m_RootSignature;
	ComPtr<ID3D12PipelineState> m_PipelineState;
	ComPtr<ID3D12PipelineState> m_TexturedPipelineState;

	// Geometry buffers
	ComPtr<ID3D12Resource> m_CubeVertexBuffer;
	ComPtr<ID3D12Resource> m_CubeIndexBuffer;
	ComPtr<ID3D12Resource> m_PlaneVertexBuffer;
	ComPtr<ID3D12Resource> m_PlaneIndexBuffer;
	ComPtr<ID3D12Resource> m_QuadVertexBuffer;
	ComPtr<ID3D12Resource> m_QuadIndexBuffer;

	D3D12_VERTEX_BUFFER_VIEW m_CubeVertexBufferView{};
	D3D12_INDEX_BUFFER_VIEW m_CubeIndexBufferView{};
	D3D12_VERTEX_BUFFER_VIEW m_PlaneVertexBufferView{};
	D3D12_INDEX_BUFFER_VIEW m_PlaneIndexBufferView{};
	D3D12_VERTEX_BUFFER_VIEW m_QuadVertexBufferView{};
	D3D12_INDEX_BUFFER_VIEW m_QuadIndexBufferView{};

	uint32_t m_CubeIndexCount = 0;
	uint32_t m_PlaneIndexCount = 0;
	uint32_t m_QuadIndexCount = 0;

	// Constant buffers (one per object for simplicity)
	static constexpr size_t MAX_OBJECTS = 256;
	ComPtr<ID3D12Resource> m_ConstantBuffer;
	uint8_t* m_ConstantBufferData = nullptr;
	uint32_t m_ConstantBufferSize = 0;

	ComPtr<ID3D12Resource> m_OutputTexture;
	ComPtr<ID3D12Resource> m_DepthStencilBuffer;
	ComPtr<ID3D12DescriptorHeap> m_RTVHeap;
	ComPtr<ID3D12DescriptorHeap> m_DSVHeap;
	ComPtr<ID3D12DescriptorHeap> m_SRVHeap;

	// Input texture from Nodos
	ID3D12Resource* m_InputTexture = nullptr;

	// Textured quad corners (for projection rendering)
	DirectX::XMFLOAT3 m_QuadP0 = {-1.5f, 1.0f, -2.0f};
	DirectX::XMFLOAT3 m_QuadP1 = {-1.5f, -1.0f, -2.0f};
	DirectX::XMFLOAT3 m_QuadP2 = {1.5f, -1.0f, -2.0f};
	DirectX::XMFLOAT3 m_QuadP3 = {1.5f, 1.0f, -2.0f};
	bool m_QuadCornersChanged = false;

	Camera m_Camera;
	DirectionalLight m_Light;
	std::vector<SceneObject> m_Objects;

	uint32_t m_Width = 1280;
	uint32_t m_Height = 720;
	DXGI_FORMAT m_OutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
};

}