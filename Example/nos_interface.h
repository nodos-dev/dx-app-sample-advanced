#ifndef _NOS_INTERFACE_H_
#define _NOS_INTERFACE_H_

#include <nvvk/resources.hpp>

#include <nvutils/parameter_registry.hpp>
#include <nvutils/camera_manipulator.hpp>

#include <nvapp/application.hpp>

#include "gaussian_splatting.h"

namespace vk_gaussian_splatting {

class NodosSplatInterface
{
public:
  NodosSplatInterface(nvutils::ParameterRegistry& parameterRegistry, nvutils::CameraManipulator& mouseCameraManip, GaussianSplatting& splatCtx);
  ~NodosSplatInterface();
  void onAttah(nvapp::Application* app);
  void preFrame(Camera const& mouseCamera);
  void postFrame();
  void onResize(VkCommandBuffer cmd, const VkExtent2D& size);
  void onDetach();

  struct InternalState;
private:
  std::unique_ptr<InternalState> internalState;
  GaussianSplatting&             splatContext;
  std::unique_ptr<nvvk::Image> outImage{};
  std::unique_ptr<VkImageView> outImageView{};
};
}  // namespace vk_gaussian_splatting

#endif