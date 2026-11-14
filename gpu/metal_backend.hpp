#pragma once
#include "gpu_backend.hpp"

#include <memory>
#include <string>

namespace p3pios::gpu {

// Metal implementation of Backend (metal_backend.mm; iOS and macOS). Returns
// nullptr and fills `error` when the device has no usable GPU or the shaders
// do not build. Every call must come from one thread.
std::unique_ptr<Backend> make_metal_backend(std::string &error);

} // namespace p3pios::gpu
