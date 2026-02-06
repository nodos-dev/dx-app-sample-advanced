// Example usage of SceneRenderer
// This demonstrates how to use the SceneRenderer class

#include "SceneRenderer.h"
#include <d3d12.h>

namespace nos::dxapp
{

// Example usage in your application:
void ExampleSceneRendererUsage(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
	// 1. Create and initialize the renderer
	SceneRenderer renderer;
	renderer.Initialize(device, DXGI_FORMAT_R8G8B8A8_UNORM, 1280, 720);

	// 2. Set up the camera
	renderer.SetCameraPosition({0.0f, 3.0f, -7.0f});
	renderer.SetCameraTarget({0.0f, 0.0f, 0.0f});
	
	// Or get direct access to camera for more control:
	auto& camera = renderer.GetCamera();
	camera.FovY = DirectX::XM_PIDIV4;
	camera.NearPlane = 0.1f;
	camera.FarPlane = 100.0f;

	// 3. Set up the directional light
	renderer.SetLightDirection({-0.5f, -1.0f, 0.5f});
	renderer.SetLightColor({1.0f, 0.95f, 0.9f}, 1.2f);
	
	// Or get direct access to light:
	auto& light = renderer.GetLight();
	light.Direction = {-0.3f, -1.0f, -0.2f};
	light.Intensity = 1.0f;

	// 4. Add objects to the scene
	
	// Add a ground plane
	size_t groundId = renderer.AddPlane(
		{0.0f, 0.0f, 0.0f},           // Position
		{10.0f, 1.0f, 10.0f},         // Scale
		{0.7f, 0.7f, 0.7f, 1.0f}      // Color (gray)
	);

	// Add some cubes
	size_t cube1 = renderer.AddCube(
		{-2.0f, 0.5f, 0.0f},          // Position
		{1.0f, 1.0f, 1.0f},           // Scale
		{1.0f, 0.3f, 0.3f, 1.0f}      // Color (red)
	);

	size_t cube2 = renderer.AddCube(
		{2.0f, 0.5f, 0.0f},           // Position
		{1.0f, 1.0f, 1.0f},           // Scale
		{0.3f, 1.0f, 0.3f, 1.0f}      // Color (green)
	);

	size_t cube3 = renderer.AddCube(
		{0.0f, 0.5f, 2.0f},           // Position
		{1.0f, 1.0f, 1.0f},           // Scale
		{0.3f, 0.3f, 1.0f, 0.8f}      // Color (blue with some transparency)
	);

	// 5. Modify objects after adding them
	auto& cube1Obj = renderer.GetObject(cube1);
	cube1Obj.Rotation = {0.0f, 0.785f, 0.0f}; // Rotate 45 degrees
	cube1Obj.Scale = {1.5f, 1.5f, 1.5f};      // Make it bigger

	// 6. Render the scene
	// You need to provide an RTV handle to render to
	// Example:
	// D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = ...; // Your RTV handle
	// ID3D12Resource* outputTexture = renderer.GetOutputTexture();
	// renderer.Render(cmdList, outputTexture, rtvHandle);

	// 7. Get the output texture for further use
	ID3D12Resource* outputTexture = renderer.GetOutputTexture();
	// Use this texture as input to other passes, export it, etc.

	// 8. Handle resize
	// renderer.Resize(1920, 1080);

	// 9. Clear all objects and start fresh
	// renderer.ClearObjects();
}

// Example animation loop:
void ExampleAnimationLoop(SceneRenderer& renderer, float time)
{
	// Rotate camera around the scene
	float radius = 7.0f;
	float camX = radius * sin(time);
	float camZ = radius * cos(time);
	renderer.SetCameraPosition({camX, 3.0f, camZ});
	renderer.SetCameraTarget({0.0f, 0.0f, 0.0f});

	// Animate a cube (assuming it's at index 0)
	if (renderer.GetObject(0).ObjectType == SceneObject::Type::Cube)
	{
		auto& cube = renderer.GetObject(0);
		cube.Rotation.y = time; // Rotate over time
		cube.Position.y = 0.5f + 0.5f * sin(time * 2.0f); // Bounce up and down
	}
}

}
