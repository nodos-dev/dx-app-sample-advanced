# SceneRenderer - Simple DX12 3D Scene Renderer

A lightweight DirectX 12 scene renderer with support for basic 3D objects, camera control, and directional lighting.

## Features

- **Camera System**: Controllable camera position and target with configurable FOV and clip planes
- **Geometric Primitives**: Support for cubes and planes with independent transforms
- **Directional Lighting**: Simple Blinn-Phong shading with ambient, diffuse, and specular components
- **Alpha Support**: Full alpha channel support in output textures
- **Per-Object Control**: Individual position, rotation, scale, and color for each object

## Quick Start

### Initialization

```cpp
#include "SceneRenderer.h"

nos::dxapp::SceneRenderer renderer;
renderer.Initialize(device, DXGI_FORMAT_R8G8B8A8_UNORM, 1280, 720);
```

### Camera Control

```cpp
// Set camera position and target
renderer.SetCameraPosition({0.0f, 3.0f, -7.0f});
renderer.SetCameraTarget({0.0f, 0.0f, 0.0f});

// Or get direct access for more control
auto& camera = renderer.GetCamera();
camera.FovY = DirectX::XM_PIDIV4;  // 45 degrees
camera.NearPlane = 0.1f;
camera.FarPlane = 100.0f;
```

### Lighting

```cpp
// Set light direction and color
renderer.SetLightDirection({-0.5f, -1.0f, 0.5f});
renderer.SetLightColor({1.0f, 0.95f, 0.9f}, 1.2f);  // Warm white, intensity 1.2

// Or get direct access
auto& light = renderer.GetLight();
light.Direction = {-0.3f, -1.0f, -0.2f};
light.Color = {1.0f, 1.0f, 1.0f};
light.Intensity = 1.0f;
```

### Adding Objects

```cpp
// Add a ground plane
size_t groundId = renderer.AddPlane(
    {0.0f, 0.0f, 0.0f},           // Position
    {10.0f, 1.0f, 10.0f},         // Scale
    {0.7f, 0.7f, 0.7f, 1.0f}      // Color (RGBA)
);

// Add a cube
size_t cubeId = renderer.AddCube(
    {0.0f, 0.5f, 0.0f},           // Position
    {1.0f, 1.0f, 1.0f},           // Scale
    {1.0f, 0.3f, 0.3f, 1.0f}      // Color (red)
);

// Modify the cube after creation
auto& cube = renderer.GetObject(cubeId);
cube.Rotation = {0.0f, 0.785f, 0.0f};  // Rotate 45 degrees around Y
cube.Position.y = 1.0f;                 // Move up
```

### Rendering

```cpp
// Render to your output texture
D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = ...;  // Your RTV handle
renderer.Render(cmdList, outputTexture, rtvHandle);

// Get the output texture for further processing
ID3D12Resource* outputTexture = renderer.GetOutputTexture();
```

### Resizing

```cpp
renderer.Resize(1920, 1080);
```

## API Reference

### SceneRenderer Class

#### Initialization
- `Initialize(ID3D12Device* device, DXGI_FORMAT outputFormat, uint32_t width, uint32_t height)` - Initialize the renderer
- `Resize(uint32_t width, uint32_t height)` - Resize the output texture

#### Rendering
- `Render(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* outputTexture, const D3D12_CPU_DESCRIPTOR_HANDLE& rtvHandle)` - Render the scene
- `GetOutputTexture()` - Get the internal output texture resource

#### Camera Control
- `GetCamera()` - Get direct access to camera
- `SetCameraPosition(const DirectX::XMFLOAT3& position)` - Set camera position
- `SetCameraTarget(const DirectX::XMFLOAT3& target)` - Set camera look-at target

#### Lighting
- `GetLight()` - Get direct access to directional light
- `SetLightDirection(const DirectX::XMFLOAT3& direction)` - Set light direction
- `SetLightColor(const DirectX::XMFLOAT3& color, float intensity)` - Set light color and intensity

#### Object Management
- `AddCube(position, scale, color)` - Add a cube to the scene, returns object ID
- `AddPlane(position, scale, color)` - Add a plane to the scene, returns object ID
- `GetObject(size_t index)` - Get reference to object for modification
- `ClearObjects()` - Remove all objects from the scene

### Camera Structure

```cpp
struct Camera
{
    DirectX::XMFLOAT3 Position;     // Camera position
    DirectX::XMFLOAT3 Target;       // Look-at target
    DirectX::XMFLOAT3 Up;           // Up vector
    float FovY;                      // Field of view (radians)
    float AspectRatio;              // Width / Height
    float NearPlane;                // Near clip plane
    float FarPlane;                 // Far clip plane
};
```

### DirectionalLight Structure

```cpp
struct DirectionalLight
{
    DirectX::XMFLOAT3 Direction;    // Light direction (normalized)
    DirectX::XMFLOAT3 Color;        // Light color (RGB)
    float Intensity;                // Light intensity multiplier
};
```

### SceneObject Structure

```cpp
struct SceneObject
{
    enum class Type { Cube, Plane };
    
    Type ObjectType;                // Cube or Plane
    DirectX::XMFLOAT3 Position;     // World position
    DirectX::XMFLOAT3 Rotation;     // Euler angles (radians)
    DirectX::XMFLOAT3 Scale;        // Scale
    DirectX::XMFLOAT4 Color;        // RGBA color
};
```

## Shading Model

The renderer uses a simple Blinn-Phong shading model with:
- **Ambient**: 20% of light color
- **Diffuse**: Lambertian diffuse (N·L)
- **Specular**: Blinn-Phong specular with shininess = 32

The lighting is calculated per-pixel in the pixel shader.

## Limitations

- Maximum 256 objects per scene
- No depth buffer (objects rendered in order added)
- No shadows
- No texture mapping
- Simple geometry primitives only
- No frustum culling

## Example: Animated Scene

```cpp
void AnimateScene(nos::dxapp::SceneRenderer& renderer, float time)
{
    // Orbit camera
    float radius = 7.0f;
    float camX = radius * sin(time);
    float camZ = radius * cos(time);
    renderer.SetCameraPosition({camX, 3.0f, camZ});
    
    // Rotate cube
    auto& cube = renderer.GetObject(0);
    cube.Rotation.y = time;
    cube.Position.y = 0.5f + 0.5f * sin(time * 2.0f);
}
```

## Notes

- The renderer uses left-handed coordinate system (DirectX convention)
- Colors are in linear space
- Alpha blending is enabled (back-to-front rendering recommended)
- Coordinate system: +Y is up, +Z is forward, +X is right
