#include "nos_interface.h"
#include <iostream>
#include <string>

#include <Nodos/AppHelpers.hpp>
#include <nosFlatBuffersCommon.h>
#include <nosVulkanSubsystem/Types_generated.h>

#include <nosTrack/Track_generated.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <glm/gtx/euler_angles.hpp>

#include <nvvk/resource_allocator.hpp>

#define NOS_ENABLE_SYNC_LOGS 0

using namespace nos;

namespace vk_gaussian_splatting {

inline nos::uuid GenerateId()
{
  static std::seed_seq seed = []() {
    std::random_device rd;
    auto               seed_data = std::array<int, std::mt19937::state_size>{};
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
  catch(std::exception _)
  {
    return false;
  }
}

inline void Must(bool cond, const char* errMsg = "Unspecified")

{
  if(cond)
    return;
  std::cerr << "Error: " << errMsg << std::endl;
  std::cerr << "Details: " << ::GetLastError() << std::endl;
  throw;
}

// Helper to run a command and capture its stdout (replaces _popen)
std::string RunCommandAndCapture(const std::string& cmd)
{
  HANDLE              hRead, hWrite;
  SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
  if(!CreatePipe(&hRead, &hWrite, &sa, 0))
    return "";

  STARTUPINFOA si = {sizeof(STARTUPINFOA)};
  si.dwFlags      = STARTF_USESTDHANDLES;
  si.hStdOutput   = hWrite;
  si.hStdError    = hWrite;
  si.hStdInput    = NULL;

  PROCESS_INFORMATION pi = {};
  // CreateProcessA needs a modifiable buffer
  std::vector<char> cmdline(cmd.begin(), cmd.end());
  cmdline.push_back('\0');

  BOOL success = CreateProcessA(NULL, cmdline.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

  CloseHandle(hWrite);  // Parent doesn't need write end

  std::string output;
  if(success)
  {
    char  buffer[256];
    DWORD read;
    while(ReadFile(hRead, buffer, sizeof(buffer) - 1, &read, NULL) && read > 0)
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

// Helper to run nodos.exe and get SDK path from JSON output (manual parsing, no JSON lib)
std::optional<std::string> GetSdkPathFromNosman(const std::string& bundleRoot, const std::string& appSdkVersion)
{
  std::string nodosExe = bundleRoot + "\\nodos.exe";

  if(!FileExists(nodosExe))
    return std::nullopt;

  std::string cmd = "\"" + nodosExe + "\" --workspace \"" + bundleRoot + "\" sdk-info \"" + appSdkVersion + "\" process";
  std::string jsonStr = RunCommandAndCapture(cmd);
  if(jsonStr.empty())
    return std::nullopt;

  const char* key    = "\"path\"";
  size_t      keyPos = jsonStr.find(key);
  std::string sdkPath;
  if(keyPos != std::string::npos)
  {
    size_t colon = jsonStr.find(':', keyPos);
    if(colon != std::string::npos)
    {
      size_t firstQuote = jsonStr.find('"', colon + 1);
      if(firstQuote != std::string::npos)
      {
        size_t secondQuote = jsonStr.find('"', firstQuote + 1);
        if(secondQuote != std::string::npos)
        {
          sdkPath = jsonStr.substr(firstQuote + 1, secondQuote - firstQuote - 1);
        }
      }
    }
  }
  if(sdkPath.empty())
  {
    std::cerr << "Failed to parse SDK path from command output." << std::endl;
    return std::nullopt;
  }
  return sdkPath;
}

void ApplyTrack(nos::track::TTrack const& track, nvutils::CameraManipulator& cameraManip, Camera& camera)
{
  glm::vec3 eyePos = glm::vec3(track.location.y() / 100.0f, track.location.z() / 100.0f, -track.location.x() / 100.0f);
  glm::vec3 rotAsRad = glm::radians(glm::vec3(track.rotation.x(), track.rotation.y(), track.rotation.z()));
  glm::mat3 rotMat   = glm::yawPitchRoll(-rotAsRad.z, rotAsRad.y, rotAsRad.x);
  glm::vec3 center   = eyePos + (rotMat * glm::vec3(0, 0, -1));
  glm::vec3 up       = rotMat * glm::vec3(0, 1, 0);

  glm::vec2 k1k2 = reinterpret_cast<const glm::vec2&>(track.lens_distortion.k1k2());
  camera         = {
              .model                = CAMERA_BARREL,
              .eye                  = eyePos,
              .ctr                  = center,
              .up                   = up,
              .barrelDistortionK1K2 = k1k2,
              .fov                  = track.fov,        // field of view
              .clip                 = {0.1f, 2000.0f},  // znear, zfar
              .dofEnabled           = track.focus != 0,
              .focusDist            = track.focus_distance,  // focus distance to compute depth of field (defocus effect)
              .aperture             = track.focus,  // aperture distance to compute depth of field, 0 does no DOF effect
  };
  cameraManip.setCamera(Camera::toNvutilCamera(camera));
}

struct ExportedImage
{
  nvvk::Image                Image;
  HANDLE                     SharedHandle;
  nos::sys::vulkan::TTexture TextureDef;
};

struct ExportedSemaphore
{
  VkSemaphore Semaphore;
  HANDLE      SharedHandle;
};


struct WinProcLoader final : public nos::app::IProcLoader
{
public:
  WinProcLoader(HMODULE module)
      : Module(module)
  {
  }
  ~WinProcLoader() { ::FreeLibrary(Module); }
  ProcPtr GetProcAddress(const char* procName) override
  {
    return reinterpret_cast<ProcPtr>(::GetProcAddress(Module, procName));
  }
  HMODULE Module;
};

struct SplatAppNode : public nos::app::IAppNode
{
public:
  SplatAppNode(NodosSplatInterface::InternalState& appInterface);
  void OnImport(nos::fb::Node const& appNode) override;
  void OnRemoved() override;
  void OnPinValueChanges(std::unordered_map<nos::uuid, nos::Buffer> const& pinValues) override;
  void OnPreExecute(void* frameCtx, uint64_t frameNumber) override;
  void OnPostExecute(void* frameCtx, uint64_t frameNumber) override;

  void OnExecutionStateChanged(nos::app::ExecutionState newState, nos::app::ExecutionState oldState) override;

private:
  NodosSplatInterface::InternalState& AppInterface;
  nos::uuid                       NodeId;
  nos::uuid                       TrackPinId;
  nos::uuid                       ResolutionPinId;
  nos::uuid                                  OutForegroundPinId;
  nos::uuid                                  OutBackgroundPinId;
  nos::uuid                       InDepthPinId;
  ExportedImage                       InDepth;
  ExportedImage                       OutForeground;
  ExportedImage                       OutBackground;
  static constexpr const char*               InDepthPinName = "InDepth";
  static constexpr const char*               OutForegroundPinName = "OutForeground";
  static constexpr const char*               OutBackgroundPinName = "OutBackground";
  static constexpr const char*               TrackPinName    = "Track";
  static constexpr const char*               ResolutionPinName = "OutputResolution";

  struct SyncState
  {
    ExportedSemaphore InputSemaphore;
    ExportedSemaphore OutputSemaphore;
    std::optional<uint64_t>          LastInputFrameNumber  = std::nullopt;
    std::optional<uint64_t>          LastOutputFrameNumber = std::nullopt;
  };
  std::optional<SyncState> Sync;

  nos::track::TTrack Track{};
  VkExtent2D         OutputResolution{1920, 1080};
  bool               OutputResolutionChanged = false;

  nvutils::CameraManipulator NodosCameraManipulator{};
  Camera                     NodosCamera{};


  // Custom iteration based command logic since nvpro framework doesn't have a way
  // Input copy and output copy are different iterations
  VkDevice        Device;
  nvvk::QueueInfo Queue;
  struct FrameResources
  {
    struct WorkGroupResources
    {
      VkCommandPool   CommandPool   = VK_NULL_HANDLE;
      VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
    };
    WorkGroupResources InputCopyResources;
    WorkGroupResources OutputCopyResources;
  };
  // TODO: What should this value be?
  constexpr static size_t FramesInFlight = 3;

  std::array<FrameResources, FramesInFlight> FrameResourcesArray{};


  void WaitOrSignalSemaphore(VkSemaphore semaphore, uint64_t waitValue)
  {
    constexpr uint64_t  waitBeforeSignalNs = 100'000'000ull;  // 100 ms
    VkSemaphoreWaitInfo waitInfo           = {
                  .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                  .semaphoreCount = 1,
                  .pSemaphores    = &semaphore,
                  .pValues        = &waitValue,
    };
    VkSemaphoreSignalInfo signalInfo = {
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = semaphore,
        .value     = waitValue,
    };
    while(true)
    {
      VkResult res = vkWaitSemaphores(Device, &waitInfo, waitBeforeSignalNs);
      if(res == VK_SUCCESS)
        return;
      vkSignalSemaphore(Device, &signalInfo);
    }
  }

  FrameResources::WorkGroupResources BeginNewFrameForWorkGroupResources(SyncState& sync, uint64_t frameNumber, bool inputResources)
  {
    assert(inputResources ? (!sync.LastInputFrameNumber || *sync.LastInputFrameNumber < frameNumber) :
                            (!sync.LastOutputFrameNumber || *sync.LastOutputFrameNumber < frameNumber));
    size_t          frameIndex = frameNumber % FramesInFlight;
    FrameResources& frameRes   = FrameResourcesArray[frameIndex];

    auto workResources = inputResources ? frameRes.InputCopyResources : frameRes.OutputCopyResources;

    // Not in use
    if(frameNumber > FramesInFlight)
    {
      uint64_t    frameToWait = frameNumber - FramesInFlight;
      VkSemaphore semToWait{};
      uint64_t    waitValue{};
      if(inputResources)
      {
        semToWait = sync.InputSemaphore.Semaphore;
        waitValue = GetInputWaitValue(frameToWait);
      }
      else
      {
        semToWait = sync.OutputSemaphore.Semaphore;
        waitValue = GetOutputWaitValue(frameToWait);
      }

      WaitOrSignalSemaphore(semToWait, waitValue);

      // Reset command pool for reuse
      VkResult res = vkResetCommandPool(Device, workResources.CommandPool, 0);
      Must(res == VK_SUCCESS, "Failed to reset command pool in NodosSplatInterface");
    }
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    vkBeginCommandBuffer(workResources.CommandBuffer, &beginInfo);
    return workResources;
  }

  FrameResources::WorkGroupResources CreateWorkGroupResources()
  {
    FrameResources::WorkGroupResources workGroupRes{};
    // Create command pool
    VkCommandPoolCreateInfo poolCreateInfo = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = Queue.familyIndex,
    };
    Must(vkCreateCommandPool(Device, &poolCreateInfo, nullptr, &workGroupRes.CommandPool) == VK_SUCCESS,
         "Failed to create command pool for NodosSplatInterface");
    // Allocate command buffer
    VkCommandBufferAllocateInfo cmdAllocInfo = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = workGroupRes.CommandPool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    Must(vkAllocateCommandBuffers(Device, &cmdAllocInfo, &workGroupRes.CommandBuffer) == VK_SUCCESS,
         "Failed to allocate command buffer for NodosSplatInterface");
    return workGroupRes;
  }

  void CreateFrameResources()
  {
    for(size_t i = 0; i < FramesInFlight; i++)
    {
      FrameResources& frameRes     = FrameResourcesArray[i];
      frameRes.InputCopyResources  = CreateWorkGroupResources();
      frameRes.OutputCopyResources = CreateWorkGroupResources();
    }
  }

  void SignalAndWaitAllFrames()
  {
    if(!Sync)
      return;
    if(Sync->LastInputFrameNumber)
    {
      uint64_t firstInputFrameToWait =
          *Sync->LastInputFrameNumber >= FramesInFlight ? *Sync->LastInputFrameNumber - FramesInFlight + 1 : 0;
      uint64_t lastFrameToWait = *Sync->LastInputFrameNumber;
      for(uint64_t frameNum = firstInputFrameToWait; frameNum <= lastFrameToWait; frameNum++)
      {
        WaitOrSignalSemaphore(Sync->InputSemaphore.Semaphore, GetInputWaitValue(frameNum));
        vkResetCommandPool(Device, FrameResourcesArray[frameNum % FramesInFlight].InputCopyResources.CommandPool, 0);
      }
    }
    if(Sync->LastOutputFrameNumber)
    {
      uint64_t firstOutputFrameToWait =
          *Sync->LastOutputFrameNumber >= FramesInFlight ? *Sync->LastOutputFrameNumber - FramesInFlight + 1 : 0;
      uint64_t lastFrameToWait = *Sync->LastOutputFrameNumber;
      for(uint64_t frameNum = firstOutputFrameToWait; frameNum <= lastFrameToWait; frameNum++)
      {
        WaitOrSignalSemaphore(Sync->OutputSemaphore.Semaphore, GetOutputWaitValue(frameNum));
        vkResetCommandPool(Device, FrameResourcesArray[frameNum % FramesInFlight].OutputCopyResources.CommandPool, 0);
      }
    }
  }

  void DestroyFrameResources()
  {
    SignalAndWaitAllFrames();
    for(uint32_t i = 0; i < FramesInFlight; i++)
    {
      FrameResources& frameRes = FrameResourcesArray[i];
      vkDestroyCommandPool(Device, frameRes.InputCopyResources.CommandPool, nullptr);
      vkDestroyCommandPool(Device, frameRes.OutputCopyResources.CommandPool, nullptr);
    }
  }

  static uint64_t GetInputSignalValue(uint64_t frameNumber) { return frameNumber * 2 + 2; }
  static uint64_t GetOutputSignalValue(uint64_t frameNumber) { return frameNumber * 2 + 1; }
  static uint64_t GetInputWaitValue(uint64_t frameNumber) { return (frameNumber) * 2 + 1; }
  static uint64_t GetOutputWaitValue(uint64_t frameNumber) { return (frameNumber) * 2; }

  std::optional<ExportedSemaphore> CreateExportedSemaphore();

  std::optional<ExportedImage> CreateExportedImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspectMask);

  void DestroyExportedImage(ExportedImage& exportedImage);

  void InputCopies(VkCommandBuffer cmd, uint64_t frameCounter);

  void OutputCopies(VkCommandBuffer cmd, uint64_t frameCounter);

  struct WorkSubmitInfo
  {
    VkSemaphore WaitSemaphore;
    uint64_t    WaitValue;
    VkSemaphore SignalSemaphore;
    uint64_t    SignalValue;
  };

  WorkSubmitInfo PrepareSubmitInfo(uint64_t frameNumber, bool inputResources)
  {
    return WorkSubmitInfo{.WaitSemaphore = inputResources ? Sync->InputSemaphore.Semaphore : Sync->OutputSemaphore.Semaphore,
                          .WaitValue = inputResources ? GetInputWaitValue(frameNumber) : GetOutputWaitValue(frameNumber),
                          .SignalSemaphore = inputResources ? Sync->InputSemaphore.Semaphore : Sync->OutputSemaphore.Semaphore,
                          .SignalValue = inputResources ? GetInputSignalValue(frameNumber) : GetOutputSignalValue(frameNumber)};
  }

  void SubmitWork(FrameResources::WorkGroupResources& workGroupRes, WorkSubmitInfo submitInfo)
  {
    // End command buffer
    vkEndCommandBuffer(workGroupRes.CommandBuffer);
    VkSemaphoreSubmitInfo waitInfo = {
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = submitInfo.WaitSemaphore,
        .value     = submitInfo.WaitValue,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkSemaphoreSubmitInfo     signalInfo    = {.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                               .semaphore = submitInfo.SignalSemaphore,
                                               .value     = submitInfo.SignalValue,
                                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo cmdBufferInfo = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = workGroupRes.CommandBuffer,
    };
    VkSubmitInfo2 submitInfo2 = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount   = 1,
        .pWaitSemaphoreInfos      = &waitInfo,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cmdBufferInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signalInfo,
    };
    VkResult result = vkQueueSubmit2(Queue.queue, 1, &submitInfo2, VK_NULL_HANDLE);
    Must(result == VK_SUCCESS, "Failed to submit NodosSplatInterface work");
  }

  void DestroySemaphore(ExportedSemaphore& exportedSemaphore)
  {
    vkDestroySemaphore(Device, exportedSemaphore.Semaphore, nullptr);
    CloseHandle(exportedSemaphore.SharedHandle);
  }

  void SendSemaphoresToNodos();
};

struct NodosSplatInterface::InternalState : public nos::app::IApp
{
  InternalState(GaussianSplatting& splatCtx, nvutils::CameraManipulator& mouseCameraManip)
      : SplatContext(splatCtx)
      , MouseCameraManipulator(mouseCameraManip)
  {
  }

  GaussianSplatting& SplatContext;
  VkDevice           Device;
  nvvk::QueueInfo    Queue;

  nvutils::CameraManipulator&   MouseCameraManipulator;
  nvvk::ResourceAllocatorExport ExportResourceAllocator;

  std::string                                NodosSdkDllPath;
  std::string                                  NodosAppKey = "NV-GS";
  std::unique_ptr<WinProcLoader>             NodosProcLoader;
  std::unique_ptr<nos::app::NodosCommunicator> Nodos;

  nos::app::IAppNode& CreateAppNode_ApiThread() override { return *(new SplatAppNode(*this)); }
  void                       DestroyAppNode(nos::app::IAppNode& appNode) override
  {
    delete static_cast<SplatAppNode*>(&appNode);
  }

#pragma region Execution Logic
  bool IsFrameSynced = false;

  void PreFrame(Camera const& mouseCamera)
  {
    if(!Nodos->PreExecute(nullptr))
    {
      SplatContext.m_activeCamera = mouseCamera;
    }
  }

  void PostFrame() { Nodos->PostExecute(nullptr); }
};

NodosSplatInterface::NodosSplatInterface(nvutils::ParameterRegistry& parameterRegistry,
                                         nvutils::CameraManipulator& mouseCameraManip,
                                         GaussianSplatting&          splatCtx)
    : internalState(std::make_unique<InternalState>(splatCtx, mouseCameraManip))
    , splatContext(splatCtx)
{
  parameterRegistry.add({"nodosSdkDll", "Nodos App SDK dll path."},
                        &internalState->NodosSdkDllPath);
  parameterRegistry.add({"nodosAppKey", "Nodos App key."},
                        &internalState->NodosAppKey);
}

NodosSplatInterface::~NodosSplatInterface() = default;

void NodosSplatInterface::onAttah(nvapp::Application* app)
{
  app->setVsync(false);
  internalState->ExportResourceAllocator.init(VmaAllocatorCreateInfo{
      .flags            = 0,
      .physicalDevice   = app->getPhysicalDevice(),
      .device           = app->getDevice(),
      .instance         = app->getInstance(),
      .vulkanApiVersion = VK_API_VERSION_1_4,
  });
  internalState->Device = app->getDevice();
  // 0 is graphics queue
  internalState->Queue = app->getQueue(0);

#pragma region Nodos SDK Path Resolution
  char exePath[MAX_PATH];
  GetModuleFileNameA(nullptr, exePath, MAX_PATH);
  std::filesystem::path exeDir = std::filesystem::absolute(exePath).parent_path();
  if(internalState->NodosSdkDllPath.empty() || !FileExists(internalState->NodosSdkDllPath))
  {
    std::filesystem::path sdkDllCandidate = exeDir / "nosAppSDK.dll";
    internalState->NodosSdkDllPath        = sdkDllCandidate.string();
  }
  // Try to find nosAppSDK.dll using nodos.exe if still not found
  if(internalState->NodosSdkDllPath.empty() || !FileExists(internalState->NodosSdkDllPath))
  {
    // Try to find bundle root (assume two levels up from exeDir: Samples/nos.sample.dxapp/<version>/Binaries)
    std::filesystem::path bundleRoot = exeDir;
    for(int i = 0; i < 3; ++i)
      bundleRoot = bundleRoot.parent_path();
    // Use App SDK version, not engine version
    std::string                appSdkVersion = "20.0";  // use correct App SDK version
    std::optional<std::string> sdkPathOpt    = GetSdkPathFromNosman(bundleRoot.string(), appSdkVersion);
    if(sdkPathOpt)
    {
      std::string candidate = *sdkPathOpt + "\\Binaries\\nosAppSDK.dll";
      if(FileExists(candidate))
        internalState->NodosSdkDllPath = candidate;
    }
  }

  if(internalState->NodosSdkDllPath.empty() || !FileExists(internalState->NodosSdkDllPath))
  {
    const char* sdkDir = std::getenv("NODOS_SDK_DIR");
    if(sdkDir)
    {
      std::string candidate = std::string(sdkDir) + "/bin/nosAppSDK.dll";
      if(FileExists(candidate))
      {
        internalState->NodosSdkDllPath = candidate;
      }
    }
  }
#pragma endregion

  while(internalState->NodosSdkDllPath.empty() || !FileExists(internalState->NodosSdkDllPath))
  {
    std::cout << "Enter path to Nodos SDK DLL: ";
    std::getline(std::cin, internalState->NodosSdkDllPath);
    if(!FileExists(internalState->NodosSdkDllPath))
    {
      std::cout << "File does not exist: " << internalState->NodosSdkDllPath << std::endl;
      internalState->NodosSdkDllPath.clear();
    }
  }

  std::cout << "Using Nodos SDK DLL at: " << internalState->NodosSdkDllPath << std::endl;

  HMODULE sdkModule = LoadLibraryA(internalState->NodosSdkDllPath.c_str());
  Must(sdkModule, ("Failed to load Nodos SDK DLL: " + internalState->NodosSdkDllPath).c_str());

  internalState->NodosProcLoader = std::make_unique<WinProcLoader>(sdkModule);

  nos::app::ApplicationInfo appInfo
  {
    .AppKey = internalState->NodosAppKey.c_str(), .AppName = "Nvidia Gaussian Splatting"
  };
  auto nodosCommunicator = nos::app::NodosCommunicator::Create(*internalState, *internalState->NodosProcLoader, "localhost:50053", appInfo);

  if(auto* err = nodosCommunicator.Error())
  {
    Must(false, std::string("Failed to create Nodos Communicator: " + *err).c_str());
  }

  internalState->Nodos = std::move(*nodosCommunicator.Get());
}

void NodosSplatInterface::preFrame(Camera const& mouseCamera)
{
  internalState->PreFrame(mouseCamera);
}

void NodosSplatInterface::postFrame()
{
  internalState->PostFrame();

}  // namespace vk_gaussian_splatting

void NodosSplatInterface::onResize(VkCommandBuffer cmd, const VkExtent2D& size)
{
  if(!internalState->Nodos->IsSynced())
  {
    splatContext.onResize(cmd, size);
  }
}

void NodosSplatInterface::onDetach()
{
  internalState->Nodos.reset();
  internalState->ExportResourceAllocator.deinit();
  internalState.reset();
}
inline SplatAppNode::SplatAppNode(NodosSplatInterface::InternalState& appInterface)
    : AppInterface(appInterface)
    , Device(appInterface.Device)
    , Queue(appInterface.Queue)
{
}
inline void SplatAppNode::OnImport(nos::fb::Node const& appNode)
{
  NodeId = *appNode.id();
  std::optional<nos::fb::UUID> trackPinId, resolutionPinId, outForegroundPinId, outBackgroundPinId, inDepthPinId;
  bool                         overrideResolution = false;
  if(appNode.pins())
  {
    for(const auto& pin : *appNode.pins())
    {
      if(pin->name()->str() == TrackPinName)
      {
        trackPinId = *pin->id();
        flatbuffers::GetRoot<nos::track::Track>(pin->data()->Data())->UnPackTo(&Track);
      }
      else if(pin->name()->str() == ResolutionPinName)
      {
        resolutionPinId              = *pin->id();
        const nos::fb::vec2u& resPtr = *reinterpret_cast<const nos::fb::vec2u*>(pin->data()->Data());
        if(resPtr.x() != 0 && resPtr.y() != 0)
          OutputResolution = VkExtent2D{resPtr.x(), resPtr.y()};
        else
          overrideResolution = true;
      }
      else if(pin->name()->str() == OutForegroundPinName)
      {
        outForegroundPinId = *pin->id();
      }
      else if(pin->name()->str() == OutBackgroundPinName)
      {
        outBackgroundPinId = *pin->id();
      }
      else if(pin->name()->str() == InDepthPinName)
      {
        inDepthPinId = *pin->id();
      }
    }
  }
  // TODO: Handle failure
  OutForeground = *CreateExportedImage(OutputResolution.width, OutputResolution.height,
                                  AppInterface.SplatContext.m_nodosGBuffers.getColorFormat(),
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
  OutBackground = *CreateExportedImage(OutputResolution.width, OutputResolution.height,
                                       AppInterface.SplatContext.m_nodosGBuffers.getColorFormat(),
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
  
  InDepth = *CreateExportedImage(OutputResolution.width, OutputResolution.height,
                                  AppInterface.SplatContext.m_nodosGBuffers.getDepthFormat(),
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                  VK_IMAGE_ASPECT_DEPTH_BIT);

  flatbuffers::FlatBufferBuilder                 fbb;
  std::vector<flatbuffers::Offset<nos::fb::Pin>> pins;
  bool                                           outForegroundNew = false;
  bool                                           outBackgroundNew = false;
  bool                                           inDepthNew = false;

  std::vector<uint8_t> foregroundTexBuf = nos::Buffer::From(OutForeground.TextureDef);
  std::vector<uint8_t> backgroundTexBuf = nos::Buffer::From(OutBackground.TextureDef);
  std::vector<uint8_t> depthTexBuf = nos::Buffer::From(InDepth.TextureDef);

  if(!outForegroundPinId)
  {
    outForegroundPinId = GenerateId();
    outForegroundNew   = true;
    pins.push_back(nos::fb::CreatePinDirect(fbb, &*outForegroundPinId, OutForegroundPinName,
                                            nos::sys::vulkan::Texture::GetFullyQualifiedName(), nos::fb::ShowAs::OUTPUT_PIN,
                                            nos::fb::CanShowAs::OUTPUT_PIN_ONLY, nullptr, 0, &foregroundTexBuf));
  }
  if (!outBackgroundPinId)
  {
    outBackgroundPinId = GenerateId();
    outBackgroundNew   = true;
    pins.push_back(nos::fb::CreatePinDirect(fbb, &*outBackgroundPinId, OutBackgroundPinName,
                                            nos::sys::vulkan::Texture::GetFullyQualifiedName(), nos::fb::ShowAs::OUTPUT_PIN,
                                            nos::fb::CanShowAs::OUTPUT_PIN_ONLY, nullptr, 0, &backgroundTexBuf));
  }
  if(!inDepthPinId)
  {
    inDepthPinId = GenerateId();
    inDepthNew   = true;
    pins.push_back(nos::fb::CreatePinDirect(fbb, &*inDepthPinId, InDepthPinName,
                                            nos::sys::vulkan::Texture::GetFullyQualifiedName(), nos::fb::ShowAs::INPUT_PIN,
                                            nos::fb::CanShowAs::INPUT_PIN_ONLY, nullptr, 0, &depthTexBuf));
  }
  if(!trackPinId)
  {
    std::vector<uint8_t> trackBuf = nos::Buffer::From(Track);
    trackPinId                    = GenerateId();
    pins.push_back(nos::fb::CreatePinDirect(fbb, &*trackPinId, TrackPinName, nos::track::Track::GetFullyQualifiedName(),
                                            nos::fb::ShowAs::INPUT_PIN,
                                            nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY, nullptr, 0, &trackBuf));
  }
  if(!resolutionPinId)
  {
    std::vector<uint8_t> resBuf(reinterpret_cast<const uint8_t*>(&OutputResolution),
                                reinterpret_cast<const uint8_t*>(&OutputResolution) + sizeof(nos::fb::vec2u));
    resolutionPinId = GenerateId();
    pins.push_back(nos::fb::CreatePinDirect(fbb, &*resolutionPinId, ResolutionPinName,
                                            nos::fb::vec2u::GetFullyQualifiedName(), nos::fb::ShowAs::INPUT_PIN,
                                            nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY, nullptr, 0, &resBuf));
  }
  if(pins.size() > 0)
  {
    fbb.Finish(nos::CreatePartialNodeUpdateDirect(fbb, &NodeId, nos::ClearFlags::CLEAR_NODES, 0, &pins, 0, 0, 0, 0, 0, 0, 0,
                                                  nos::fb::CreateNodeOrphanStateDirect(fbb, nos::fb::NodeOrphanStateType::ACTIVE, "")));
    nos::Buffer update = fbb.Release();
    AppInterface.Nodos->GetClient().SendPartialNodeUpdate(*update.As<nos::PartialNodeUpdate>());
  }
  TrackPinId      = *trackPinId;
  ResolutionPinId = *resolutionPinId;
  OutForegroundPinId = *outForegroundPinId;
  OutBackgroundPinId = *outBackgroundPinId;
  InDepthPinId   = *inDepthPinId;
  if(!outForegroundNew)
  {
    AppInterface.Nodos->NotifyPinValueChanged(OutForegroundPinId, nos::Buffer::From(OutForeground.TextureDef));
  }
  if(!outBackgroundNew)
  {
    AppInterface.Nodos->NotifyPinValueChanged(OutBackgroundPinId, nos::Buffer::From(OutBackground.TextureDef));
  }
  if(!inDepthNew)
    AppInterface.Nodos->NotifyPinValueChanged(InDepthPinId, nos::Buffer::From(InDepth.TextureDef));
  if (overrideResolution)
    AppInterface.Nodos->NotifyPinValueChanged(ResolutionPinId, nos::Buffer::From(OutputResolution));
  OutputResolutionChanged = true;
  CreateFrameResources();
}
inline void SplatAppNode::OnRemoved()
{
  assert(!Sync);
  DestroyFrameResources();
  DestroyExportedImage(OutForeground);
  DestroyExportedImage(OutBackground);
  DestroyExportedImage(InDepth);
}
inline void SplatAppNode::OnPinValueChanges(std::unordered_map<nos::uuid, nos::Buffer> const& pinValues)
{
  for(const auto& [pinId, data] : pinValues)
  {
    if(pinId == ResolutionPinId)
    {
      const nos::fb::vec2u& newRes = *reinterpret_cast<const nos::fb::vec2u*>(data.Data());
      if(newRes.x() == 0 || newRes.y() == 0)
        continue;
      if(newRes.x() != OutputResolution.width || newRes.y() != OutputResolution.height)
      {
        OutputResolutionChanged = true;
        OutputResolution        = VkExtent2D{newRes.x(), newRes.y()};
      }
    }
    else if(pinId == TrackPinId)
    {
      flatbuffers::GetRoot<nos::track::Track>(data.Data())->UnPackTo(&Track);
    }
  }
}

inline void SplatAppNode::OnPreExecute(void* frameCtx, uint64_t frameCounter)
{
  if(!Sync)
  {
    assert(false);
    return;
  }
  ApplyTrack(Track, NodosCameraManipulator, NodosCamera);
  AppInterface.SplatContext.m_activeCameraManipulator = &NodosCameraManipulator;
  AppInterface.SplatContext.m_activeCamera            = NodosCamera;
  NodosCameraManipulator.setWindowSize(glm::uvec2(OutputResolution.width, OutputResolution.height));

  auto inputWorkGroupResources = BeginNewFrameForWorkGroupResources(*Sync, frameCounter, true);
  if(OutputResolutionChanged)
  {
    vkDeviceWaitIdle(Device);
    AppInterface.SplatContext.onResize(inputWorkGroupResources.CommandBuffer, OutputResolution);
    if(OutForeground.TextureDef.width != OutputResolution.width || OutForeground.TextureDef.height != OutputResolution.height)
    {
      DestroyExportedImage(OutForeground);
      OutForeground = *CreateExportedImage(OutputResolution.width, OutputResolution.height,
                                           AppInterface.SplatContext.m_nodosGBuffers.getColorFormat(),
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT);
      AppInterface.Nodos->NotifyPinValueChanged(OutForegroundPinId, nos::Buffer::From(OutForeground.TextureDef));
    }
    if(OutBackground.TextureDef.width != OutputResolution.width || OutBackground.TextureDef.height != OutputResolution.height)
    {
      DestroyExportedImage(OutBackground);
      OutBackground = *CreateExportedImage(OutputResolution.width, OutputResolution.height,
                                           AppInterface.SplatContext.m_nodosGBuffers.getColorFormat(),
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                           VK_IMAGE_ASPECT_COLOR_BIT);
      AppInterface.Nodos->NotifyPinValueChanged(OutBackgroundPinId, nos::Buffer::From(OutBackground.TextureDef));
    }
    if(InDepth.TextureDef.width != OutputResolution.width || InDepth.TextureDef.height != OutputResolution.height)
    {
      DestroyExportedImage(InDepth);
      InDepth = *CreateExportedImage(OutputResolution.width, OutputResolution.height,
                                     AppInterface.SplatContext.m_nodosGBuffers.getDepthFormat(),
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                      VK_IMAGE_ASPECT_DEPTH_BIT);
      AppInterface.Nodos->NotifyPinValueChanged(InDepthPinId, nos::Buffer::From(InDepth.TextureDef));
    }
    OutputResolutionChanged = false;
  }
  InputCopies(inputWorkGroupResources.CommandBuffer, frameCounter);
  auto submitInfo = PrepareSubmitInfo(frameCounter, true);
#if NOS_ENABLE_SYNC_LOGS
  std::cout << "Submitting input work for frame " << frameCounter << " with wait value " << submitInfo.WaitValue
            << " and signal value " << submitInfo.SignalValue << std::endl;
#endif
  SubmitWork(inputWorkGroupResources, submitInfo);
  Sync->LastInputFrameNumber = frameCounter;
}
inline void SplatAppNode::OnPostExecute(void* frameCtx, uint64_t frameCounter)
{
  if(!Sync)
  {
    assert(Sync);
    return;
  }
  auto outputWorkGroupResources = BeginNewFrameForWorkGroupResources(*Sync, frameCounter, false);
  OutputCopies(outputWorkGroupResources.CommandBuffer, frameCounter);
  auto submitInfo = PrepareSubmitInfo(frameCounter, false);
#if NOS_ENABLE_SYNC_LOGS
  std::cout << "Submitting output work for frame " << frameCounter << " with wait value " << submitInfo.WaitValue
            << " and signal value " << submitInfo.SignalValue << std::endl;
#endif
  SubmitWork(outputWorkGroupResources, submitInfo);
  Sync->LastOutputFrameNumber = frameCounter;
}

void SplatAppNode::OnExecutionStateChanged(nos::app::ExecutionState newState, nos::app::ExecutionState oldState)
{
  if(oldState == nos::app::ExecutionState::SYNCED)
  {
    assert(Sync);
    SignalAndWaitAllFrames();
    DestroySemaphore(Sync->InputSemaphore);
    DestroySemaphore(Sync->OutputSemaphore);
    Sync = std::nullopt;
  }
  if(newState == nos::app::ExecutionState::SYNCED)
  {
    Sync            = SyncState{};
    Sync->InputSemaphore  = *CreateExportedSemaphore();
    Sync->OutputSemaphore = *CreateExportedSemaphore();
    SendSemaphoresToNodos();
    OutputResolutionChanged                      = true;
    AppInterface.SplatContext.m_nosSplitRendering = true;
  }
  else
  {
    // Before next frame, set the camera back to mouse camera
    AppInterface.SplatContext.m_activeCameraManipulator = &AppInterface.MouseCameraManipulator;
    AppInterface.SplatContext.m_nosSplitRendering       = false;
  }
  AppInterface.SplatContext.reinitNextFrame();
}

inline std::optional<ExportedSemaphore> SplatAppNode::CreateExportedSemaphore()
{
  VkExportSemaphoreWin32HandleInfoKHR handleInfo = {
      .sType    = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
      .dwAccess = GENERIC_ALL,
  };

  VkExportSemaphoreCreateInfo exportInfo = {
      .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
      .pNext       = &handleInfo,
      .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
  };
  VkSemaphoreTypeCreateInfo semaphoreTypeInfo = {
      .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .pNext         = &exportInfo,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue  = 0,
  };
  VkSemaphoreCreateInfo semaphoreCreateInfo = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &semaphoreTypeInfo,
      .flags = 0,
  };
  VkSemaphore semaphore;
  if(vkCreateSemaphore(AppInterface.ExportResourceAllocator.getDevice(), &semaphoreCreateInfo, nullptr, &semaphore) != VK_SUCCESS)
  {
    return std::nullopt;
  }
  VkSemaphoreGetWin32HandleInfoKHR getHandleInfo = {
      .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
      .semaphore  = semaphore,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR,
  };
  HANDLE sharedHandle = nullptr;
  if(vkGetSemaphoreWin32HandleKHR(AppInterface.ExportResourceAllocator.getDevice(), &getHandleInfo, &sharedHandle) != VK_SUCCESS)
  {
    vkDestroySemaphore(AppInterface.ExportResourceAllocator.getDevice(), semaphore, nullptr);
    return std::nullopt;
  }
  return ExportedSemaphore{semaphore, sharedHandle};
}
inline std::optional<ExportedImage> SplatAppNode::CreateExportedImage(uint32_t           width,
                                                                             uint32_t           height,
                                                                             VkFormat           format,
                                                                             VkImageUsageFlags  usage,
                                                                             VkImageAspectFlags aspectMask)
{
  nvvk::Image             img{};
  const VkImageUsageFlags actualUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | usage;
  const VkImageCreateInfo info        = {
             .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
             .imageType   = VK_IMAGE_TYPE_2D,
             .format      = format,
             .extent      = {width, height, 1},
             .mipLevels   = 1,
             .arrayLayers = 1,
             .samples     = VK_SAMPLE_COUNT_1_BIT,
             .usage       = actualUsage,
  };
  VkImageViewCreateInfo viewInfo = {
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = format,
      .subresourceRange = {.aspectMask = aspectMask, .levelCount = 1, .layerCount = 1},
  };
  if(AppInterface.ExportResourceAllocator.createImageExport(img, info, viewInfo) != VK_SUCCESS)
  {
    return std::nullopt;
  }

  VmaAllocationInfo2 allocationInfo2{};
  vmaGetAllocationInfo2(AppInterface.ExportResourceAllocator, img.allocation, &allocationInfo2);

  VkDeviceMemory deviceMemory = allocationInfo2.allocationInfo.deviceMemory;
  HANDLE         sharedHandle = nullptr;
  if(vmaGetMemoryWin32Handle(AppInterface.ExportResourceAllocator, img.allocation, nullptr, &sharedHandle) != VK_SUCCESS)
  {
    AppInterface.ExportResourceAllocator.destroyImage(img);
    return std::nullopt;
  }

  nos::sys::vulkan::TTexture texDef{};
  texDef.resolution = nos::sys::vulkan::SizePreset::CUSTOM;
  texDef.width      = width;
  texDef.height     = height;
  texDef.format     = nos::sys::vulkan::Format(info.format);
  texDef.usage      = nos::sys::vulkan::ImageUsage(actualUsage);
  texDef.unscaled   = true;
  texDef.offset     = allocationInfo2.allocationInfo.offset;
  auto& extMem      = texDef.external_memory;
  extMem.mutate_handle((uint64_t)sharedHandle);
  extMem.mutate_handle_type(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT);
  extMem.mutate_allocation_size(allocationInfo2.allocationInfo.size);
  extMem.mutate_pid(getpid());

  return ExportedImage{img, sharedHandle, texDef};
}
inline void SplatAppNode::DestroyExportedImage(ExportedImage& exportedImage)
{
  AppInterface.ExportResourceAllocator.destroyImage(exportedImage.Image);
  CloseHandle(exportedImage.SharedHandle);
}
inline void CopyImage(VkCommandBuffer cmd, VkImage srcImg, VkImage dstImg, VkImageAspectFlags aspectMask, VkExtent2D extent)
{
  nvvk::cmdImageMemoryBarrier(cmd, nvvk::ImageMemoryBarrierParams{.image     = srcImg,
                                                                  .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                                  .subresourceRange = {aspectMask, 0, VK_REMAINING_MIP_LEVELS,
                                                                                       0, VK_REMAINING_ARRAY_LAYERS}});
  nvvk::cmdImageMemoryBarrier(cmd, nvvk::ImageMemoryBarrierParams{.image     = dstImg,
                                                                  .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                                  .subresourceRange = {aspectMask, 0, VK_REMAINING_MIP_LEVELS,
                                                                                       0, VK_REMAINING_ARRAY_LAYERS}});
  VkImageCopy copyInfo{
      .srcSubresource =
          {
              .aspectMask     = aspectMask,
              .mipLevel       = 0,
              .baseArrayLayer = 0,
              .layerCount     = 1,
          },
      .srcOffset = {0, 0, 0},
      .dstSubresource =
          {
              .aspectMask     = aspectMask,
              .mipLevel       = 0,
              .baseArrayLayer = 0,
              .layerCount     = 1,
          },
      .dstOffset = {0, 0, 0},
      .extent    = {extent.width, extent.height, 1},
  };
  vkCmdCopyImage(cmd, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyInfo);
  nvvk::cmdImageMemoryBarrier(cmd, nvvk::ImageMemoryBarrierParams{.image     = dstImg,
                                                                  .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                                  .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                                  .subresourceRange = {aspectMask, 0, VK_REMAINING_MIP_LEVELS,
                                                                                       0, VK_REMAINING_ARRAY_LAYERS}});
  nvvk::cmdImageMemoryBarrier(cmd, nvvk::ImageMemoryBarrierParams{.image     = srcImg,
                                                                  .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                                  .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                                  .subresourceRange = {aspectMask, 0, VK_REMAINING_MIP_LEVELS,
                                                                                       0, VK_REMAINING_ARRAY_LAYERS}});
}
inline void SplatAppNode::InputCopies(VkCommandBuffer cmd, uint64_t frameCounter)
{
    CopyImage(cmd, InDepth.Image.image,
            AppInterface.SplatContext.m_nodosGBuffers.getDepthImage(),
            VK_IMAGE_ASPECT_DEPTH_BIT, VkExtent2D{InDepth.Image.extent.width, InDepth.Image.extent.height});
}
inline void SplatAppNode::OutputCopies(VkCommandBuffer cmd, uint64_t frameCounter)
{
  {
    CopyImage(cmd, AppInterface.SplatContext.m_nodosGBuffers.getColorImage((uint32_t)GaussianSplatting::NodosColors::Foreground),
            OutForeground.Image.image, VK_IMAGE_ASPECT_COLOR_BIT,
            VkExtent2D{OutForeground.Image.extent.width, OutForeground.Image.extent.height});
    CopyImage(cmd, AppInterface.SplatContext.m_nodosGBuffers.getColorImage((uint32_t)GaussianSplatting::NodosColors::Background),
            OutBackground.Image.image, VK_IMAGE_ASPECT_COLOR_BIT,
            VkExtent2D{OutBackground.Image.extent.width, OutBackground.Image.extent.height});
  }
}
inline void SplatAppNode::SendSemaphoresToNodos()
{
  assert(Sync);
  uint64_t                       inputSemaphore  = (uint64_t)Sync->InputSemaphore.SharedHandle;
  uint64_t                       outputSemaphore = (uint64_t)Sync->OutputSemaphore.SharedHandle;
  flatbuffers::FlatBufferBuilder mb;
  auto                           offset =
      nos::CreateAppEventOffset(mb, nos::app::CreateSetSyncSemaphores(mb, &NodeId, _getpid(), inputSemaphore, outputSemaphore));
  mb.Finish(offset);
  auto buf  = mb.Release();
  auto root = flatbuffers::GetRoot<nos::app::AppEvent>(buf.data());
  AppInterface.Nodos->GetClient().Send(*root);
}
}  // namespace vk_gaussian_splatting
