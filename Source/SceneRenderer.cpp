#include "SceneRenderer.h"
#include <stdexcept>
#include <d3dcompiler.h>

namespace nos::dxapp
{

// Camera implementation
DirectX::XMMATRIX Camera::GetViewMatrix() const
{
	using namespace DirectX;
	XMVECTOR pos = XMLoadFloat3(&Position);
	XMVECTOR target = XMLoadFloat3(&Target);
	XMVECTOR up = XMLoadFloat3(&Up);
	return XMMatrixLookAtLH(pos, target, up);
}

DirectX::XMMATRIX Camera::GetProjectionMatrix() const
{
	return DirectX::XMMatrixPerspectiveFovLH(FovY, AspectRatio, NearPlane, FarPlane);
}

// SceneObject implementation
DirectX::XMMATRIX SceneObject::GetWorldMatrix() const
{
	using namespace DirectX;
	XMMATRIX translation = XMMatrixTranslation(Position.x, Position.y, Position.z);
	XMMATRIX rotation = XMMatrixRotationRollPitchYaw(Rotation.x, Rotation.y, Rotation.z);
	XMMATRIX scale = XMMatrixScaling(Scale.x, Scale.y, Scale.z);
	return scale * rotation * translation;
}

// Shader code
const char* g_VertexShader = R"(
cbuffer ConstantBuffer : register(b0)
{
	float4x4 WorldViewProj;
	float4x4 World;
	float3 LightDirection;
	float Padding1;
	float3 LightColor;
	float LightIntensity;
	float3 CameraPosition;
	float Padding2;
	float4 ObjectColor;
};

struct VSInput
{
	float3 Position : POSITION;
	float3 Normal : NORMAL;
	float4 Color : COLOR;
};

struct PSInput
{
	float4 Position : SV_POSITION;
	float3 WorldPos : POSITION;
	float3 Normal : NORMAL;
	float4 Color : COLOR;
};

PSInput main(VSInput input)
{
	PSInput output;
	output.Position = mul(float4(input.Position, 1.0), WorldViewProj);
	output.WorldPos = mul(float4(input.Position, 1.0), World).xyz;
	output.Normal = normalize(mul(float4(input.Normal, 0.0), World).xyz);
	output.Color = input.Color * ObjectColor;
	return output;
}
)";

const char* g_PixelShader = R"(
cbuffer ConstantBuffer : register(b0)
{
	float4x4 WorldViewProj;
	float4x4 World;
	float3 LightDirection;
	float Padding1;
	float3 LightColor;
	float LightIntensity;
	float3 CameraPosition;
	float Padding2;
	float4 ObjectColor;
};

struct PSInput
{
	float4 Position : SV_POSITION;
	float3 WorldPos : POSITION;
	float3 Normal : NORMAL;
	float4 Color : COLOR;
};

float4 main(PSInput input) : SV_TARGET
{
	float3 normal = normalize(input.Normal);
	float3 lightDir = normalize(-LightDirection);
	
	// Ambient
	float3 ambient = 0.2 * LightColor;
	
	// Diffuse
	float diff = max(dot(normal, lightDir), 0.0);
	float3 diffuse = diff * LightColor * LightIntensity;
	
	// Specular
	float3 viewDir = normalize(CameraPosition - input.WorldPos);
	float3 halfDir = normalize(lightDir + viewDir);
	float spec = pow(max(dot(normal, halfDir), 0.0), 32.0);
	float3 specular = spec * LightColor * LightIntensity * 0.5;
	
	float3 result = (ambient + diffuse + specular) * input.Color.rgb;
	return float4(result, input.Color.a);
}
)";

// SceneRenderer implementation
SceneRenderer::SceneRenderer()
{
}

SceneRenderer::~SceneRenderer()
{
	if (m_ConstantBufferData)
	{
		m_ConstantBuffer->Unmap(0, nullptr);
		m_ConstantBufferData = nullptr;
	}
}

void SceneRenderer::Initialize(ID3D12Device* device, DXGI_FORMAT outputFormat, uint32_t width, uint32_t height)
{
	m_Device = device;
	m_OutputFormat = outputFormat;
	m_Width = width;
	m_Height = height;

	m_Camera.AspectRatio = static_cast<float>(width) / static_cast<float>(height);

	CreateRootSignature(device);
	CreatePipelineState(device, outputFormat);
	CreateGeometryBuffers(device);
	CreateConstantBuffer(device);
	CreateOutputTexture(device, width, height);
	CreateDepthStencilBuffer(device, width, height);
}

void SceneRenderer::CreateRootSignature(ID3D12Device* device)
{
	D3D12_ROOT_PARAMETER rootParameter{};
	rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	rootParameter.Descriptor.ShaderRegister = 0;
	rootParameter.Descriptor.RegisterSpace = 0;
	rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
	rootSignatureDesc.NumParameters = 1;
	rootSignatureDesc.pParameters = &rootParameter;
	rootSignatureDesc.NumStaticSamplers = 0;
	rootSignatureDesc.pStaticSamplers = nullptr;
	rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> signature;
	ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr))
	{
		if (error)
		{
			OutputDebugStringA((char*)error->GetBufferPointer());
		}
		throw std::runtime_error("Failed to serialize root signature");
	}

	hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature));
	if (FAILED(hr))
		throw std::runtime_error("Failed to create root signature");
}

void SceneRenderer::CreatePipelineState(ID3D12Device* device, DXGI_FORMAT outputFormat)
{
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;
	ComPtr<ID3DBlob> error;

	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;

	HRESULT hr = D3DCompile(g_VertexShader, strlen(g_VertexShader), nullptr, nullptr, nullptr,
		"main", "vs_5_0", compileFlags, 0, &vertexShader, &error);
	if (FAILED(hr))
	{
		if (error)
		{
			OutputDebugStringA((char*)error->GetBufferPointer());
		}
		throw std::runtime_error("Failed to compile vertex shader");
	}

	hr = D3DCompile(g_PixelShader, strlen(g_PixelShader), nullptr, nullptr, nullptr,
		"main", "ps_5_0", compileFlags, 0, &pixelShader, &error);
	if (FAILED(hr))
	{
		if (error)
		{
			OutputDebugStringA((char*)error->GetBufferPointer());
		}
		throw std::runtime_error("Failed to compile pixel shader");
	}

	D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
	psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
	psoDesc.pRootSignature = m_RootSignature.Get();
	psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
	
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
	psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = outputFormat;
	psoDesc.SampleDesc.Count = 1;

	hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_PipelineState));
	if (FAILED(hr))
		throw std::runtime_error("Failed to create pipeline state");
}

void SceneRenderer::CreateGeometryBuffers(ID3D12Device* device)
{
	using namespace DirectX;

	// Cube vertices (with normals)
	Vertex cubeVertices[] = {
		// Front face
		{ XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		// Back face
		{ XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		// Top face
		{ XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		// Bottom face
		{ XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		// Right face
		{ XMFLOAT3( 0.5f, -0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f,  0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f,  0.5f,  0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f, -0.5f,  0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		// Left face
		{ XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) }
	};

	UINT16 cubeIndices[] = {
		0, 1, 2, 0, 2, 3,       // Front
		4, 5, 6, 4, 6, 7,       // Back
		8, 9, 10, 8, 10, 11,    // Top
		12, 13, 14, 12, 14, 15, // Bottom
		16, 17, 18, 16, 18, 19, // Right
		20, 21, 22, 20, 22, 23  // Left
	};

	m_CubeIndexCount = _countof(cubeIndices);

	// Create cube vertex buffer
	{
		UINT vertexBufferSize = sizeof(cubeVertices);
		auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
		
		device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_CubeVertexBuffer));

		void* pData;
		m_CubeVertexBuffer->Map(0, nullptr, &pData);
		memcpy(pData, cubeVertices, vertexBufferSize);
		m_CubeVertexBuffer->Unmap(0, nullptr);

		m_CubeVertexBufferView.BufferLocation = m_CubeVertexBuffer->GetGPUVirtualAddress();
		m_CubeVertexBufferView.StrideInBytes = sizeof(Vertex);
		m_CubeVertexBufferView.SizeInBytes = vertexBufferSize;
	}

	// Create cube index buffer
	{
		UINT indexBufferSize = sizeof(cubeIndices);
		auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
		
		device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_CubeIndexBuffer));

		void* pData;
		m_CubeIndexBuffer->Map(0, nullptr, &pData);
		memcpy(pData, cubeIndices, indexBufferSize);
		m_CubeIndexBuffer->Unmap(0, nullptr);

		m_CubeIndexBufferView.BufferLocation = m_CubeIndexBuffer->GetGPUVirtualAddress();
		m_CubeIndexBufferView.Format = DXGI_FORMAT_R16_UINT;
		m_CubeIndexBufferView.SizeInBytes = indexBufferSize;
	}

	// Plane vertices (Y-up plane)
	Vertex planeVertices[] = {
		{ XMFLOAT3(-0.5f, 0.0f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3(-0.5f, 0.0f,  0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f, 0.0f,  0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) },
		{ XMFLOAT3( 0.5f, 0.0f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) }
	};

	UINT16 planeIndices[] = {
		0, 1, 2, 0, 2, 3
	};

	m_PlaneIndexCount = _countof(planeIndices);

	// Create plane vertex buffer
	{
		UINT vertexBufferSize = sizeof(planeVertices);
		auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
		
		device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_PlaneVertexBuffer));

		void* pData;
		m_PlaneVertexBuffer->Map(0, nullptr, &pData);
		memcpy(pData, planeVertices, vertexBufferSize);
		m_PlaneVertexBuffer->Unmap(0, nullptr);

		m_PlaneVertexBufferView.BufferLocation = m_PlaneVertexBuffer->GetGPUVirtualAddress();
		m_PlaneVertexBufferView.StrideInBytes = sizeof(Vertex);
		m_PlaneVertexBufferView.SizeInBytes = vertexBufferSize;
	}

	// Create plane index buffer
	{
		UINT indexBufferSize = sizeof(planeIndices);
		auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
		
		device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_PlaneIndexBuffer));

		void* pData;
		m_PlaneIndexBuffer->Map(0, nullptr, &pData);
		memcpy(pData, planeIndices, indexBufferSize);
		m_PlaneIndexBuffer->Unmap(0, nullptr);

		m_PlaneIndexBufferView.BufferLocation = m_PlaneIndexBuffer->GetGPUVirtualAddress();
		m_PlaneIndexBufferView.Format = DXGI_FORMAT_R16_UINT;
		m_PlaneIndexBufferView.SizeInBytes = indexBufferSize;
	}
}

void SceneRenderer::CreateConstantBuffer(ID3D12Device* device)
{
	m_ConstantBufferSize = (sizeof(ConstantBufferData) + 255) & ~255;
	UINT totalSize = m_ConstantBufferSize * MAX_OBJECTS;

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(totalSize);
	
	device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_ConstantBuffer));

	CD3DX12_RANGE readRange(0, 0);
	m_ConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_ConstantBufferData));
}

void SceneRenderer::CreateOutputTexture(ID3D12Device* device, uint32_t width, uint32_t height)
{
	D3D12_RESOURCE_DESC textureDesc = {};
	textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	textureDesc.Width = width;
	textureDesc.Height = height;
	textureDesc.DepthOrArraySize = 1;
	textureDesc.MipLevels = 1;
	textureDesc.Format = m_OutputFormat;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.SampleDesc.Quality = 0;
	textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = m_OutputFormat;
	clearValue.Color[0] = 0.0f;
	clearValue.Color[1] = 0.0f;
	clearValue.Color[2] = 0.0f;
	clearValue.Color[3] = 0.0f;

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	
	device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&textureDesc,
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		&clearValue,
		IID_PPV_ARGS(&m_OutputTexture));

	// Create RTV heap for output texture
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = 1;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_RTVHeap));

	// Create RTV for output texture
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = m_OutputFormat;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(m_OutputTexture.Get(), &rtvDesc, m_RTVHeap->GetCPUDescriptorHandleForHeapStart());
}

void SceneRenderer::CreateDepthStencilBuffer(ID3D12Device* device, uint32_t width, uint32_t height)
{
	// Create descriptor heap for DSV
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	
	HRESULT hr = device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_DSVHeap));
	if (FAILED(hr))
		throw std::runtime_error("Failed to create DSV descriptor heap");

	// Create depth stencil texture
	D3D12_RESOURCE_DESC depthStencilDesc = {};
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Width = width;
	depthStencilDesc.Height = height;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;
	depthStencilDesc.SampleDesc.Count = 1;
	depthStencilDesc.SampleDesc.Quality = 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE depthClearValue = {};
	depthClearValue.Format = DXGI_FORMAT_D32_FLOAT;
	depthClearValue.DepthStencil.Depth = 1.0f;
	depthClearValue.DepthStencil.Stencil = 0;

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	
	hr = device->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&depthClearValue,
		IID_PPV_ARGS(&m_DepthStencilBuffer));
	if (FAILED(hr))
		throw std::runtime_error("Failed to create depth stencil buffer");

	// Create depth stencil view
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.Texture2D.MipSlice = 0;
	
	device->CreateDepthStencilView(m_DepthStencilBuffer.Get(), &dsvDesc, m_DSVHeap->GetCPUDescriptorHandleForHeapStart());
}

void SceneRenderer::Render(ID3D12GraphicsCommandList* cmdList)
{
	using namespace DirectX;

	// Set viewport and scissor rect
	D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(m_Width), static_cast<float>(m_Height), 0.0f, 1.0f };
	D3D12_RECT scissorRect = { 0, 0, static_cast<LONG>(m_Width), static_cast<LONG>(m_Height) };
	
	cmdList->RSSetViewports(1, &viewport);
	cmdList->RSSetScissorRects(1, &scissorRect);

	// Get RTV handle for our internal output texture
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RTVHeap->GetCPUDescriptorHandleForHeapStart();

	// Clear render target
	float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
	cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

	// Clear depth stencil
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_DSVHeap->GetCPUDescriptorHandleForHeapStart();
	cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// Set render target and depth stencil
	cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

	// Set pipeline state
	cmdList->SetPipelineState(m_PipelineState.Get());
	cmdList->SetGraphicsRootSignature(m_RootSignature.Get());
	cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Get view and projection matrices
	XMMATRIX view = m_Camera.GetViewMatrix();
	XMMATRIX proj = m_Camera.GetProjectionMatrix();
	XMMATRIX viewProj = view * proj;

	// Render each object
	for (size_t i = 0; i < m_Objects.size(); ++i)
	{
		const SceneObject& obj = m_Objects[i];
		
		// Update constant buffer for this object
		XMMATRIX world = obj.GetWorldMatrix();
		XMMATRIX worldViewProj = world * viewProj;

		ConstantBufferData cbData{};
		XMStoreFloat4x4((XMFLOAT4X4*)&cbData.WorldViewProj, XMMatrixTranspose(worldViewProj));
		XMStoreFloat4x4((XMFLOAT4X4*)&cbData.World, XMMatrixTranspose(world));
		
		XMVECTOR lightDir = XMLoadFloat3(&m_Light.Direction);
		lightDir = XMVector3Normalize(lightDir);
		XMStoreFloat3(&cbData.LightDirection, lightDir);
		
		cbData.LightColor = m_Light.Color;
		cbData.LightIntensity = m_Light.Intensity;
		cbData.CameraPosition = m_Camera.Position;
		cbData.ObjectColor = obj.Color;

		// Copy to constant buffer
		memcpy(m_ConstantBufferData + i * m_ConstantBufferSize, &cbData, sizeof(ConstantBufferData));

		// Set constant buffer view
		D3D12_GPU_VIRTUAL_ADDRESS cbvAddress = m_ConstantBuffer->GetGPUVirtualAddress() + i * m_ConstantBufferSize;
		cmdList->SetGraphicsRootConstantBufferView(0, cbvAddress);

		// Set vertex and index buffers based on object type
		if (obj.ObjectType == SceneObject::Type::Cube)
		{
			cmdList->IASetVertexBuffers(0, 1, &m_CubeVertexBufferView);
			cmdList->IASetIndexBuffer(&m_CubeIndexBufferView);
			cmdList->DrawIndexedInstanced(m_CubeIndexCount, 1, 0, 0, 0);
		}
		else if (obj.ObjectType == SceneObject::Type::Plane)
		{
			cmdList->IASetVertexBuffers(0, 1, &m_PlaneVertexBufferView);
			cmdList->IASetIndexBuffer(&m_PlaneIndexBufferView);
			cmdList->DrawIndexedInstanced(m_PlaneIndexCount, 1, 0, 0, 0);
		}
	}
}

void SceneRenderer::Resize(uint32_t width, uint32_t height)
{
	if (width == m_Width && height == m_Height)
		return;

	m_Width = width;
	m_Height = height;
	m_Camera.AspectRatio = static_cast<float>(width) / static_cast<float>(height);

	if (m_OutputTexture)
	{
		m_OutputTexture.Reset();
		CreateOutputTexture(m_Device.Get(), width, height);
	}
	
	if (m_DepthStencilBuffer)
	{
		m_DepthStencilBuffer.Reset();
		m_DSVHeap.Reset();
		CreateDepthStencilBuffer(m_Device.Get(), width, height);
	}
}

void SceneRenderer::SetCameraPosition(const DirectX::XMFLOAT3& position)
{
	m_Camera.Position = position;
}

void SceneRenderer::SetCameraTarget(const DirectX::XMFLOAT3& target)
{
	m_Camera.Target = target;
}

void SceneRenderer::SetLightDirection(const DirectX::XMFLOAT3& direction)
{
	m_Light.Direction = direction;
}

void SceneRenderer::SetLightColor(const DirectX::XMFLOAT3& color, float intensity)
{
	m_Light.Color = color;
	m_Light.Intensity = intensity;
}

size_t SceneRenderer::AddCube(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& scale, const DirectX::XMFLOAT4& color)
{
	SceneObject obj;
	obj.ObjectType = SceneObject::Type::Cube;
	obj.Position = position;
	obj.Scale = scale;
	obj.Color = color;
	
	m_Objects.push_back(obj);
	return m_Objects.size() - 1;
}

size_t SceneRenderer::AddPlane(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& scale, const DirectX::XMFLOAT4& color)
{
	SceneObject obj;
	obj.ObjectType = SceneObject::Type::Plane;
	obj.Position = position;
	obj.Scale = scale;
	obj.Color = color;
	
	m_Objects.push_back(obj);
	return m_Objects.size() - 1;
}

SceneObject& SceneRenderer::GetObject(size_t index)
{
	return m_Objects[index];
}

void SceneRenderer::ClearObjects()
{
	m_Objects.clear();
}

void SceneRenderer::UpdateConstantBuffer(size_t objectIndex)
{
	// This method can be extended for per-frame updates if needed
}

}
