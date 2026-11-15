#pragma once
#include "gpu_backend.hpp"

#include <memory>
#include <string>

namespace p3pios::gpu {

// Metal implementation of Backend (metal_backend.mm; iOS and macOS). Returns
// nullptr and fills `error` when the device has no usable GPU or the shaders
// do not build. Every call must come from one thread.
// `scale` (1..4): render targets are scale x the PSP's resolution; read_target
// still returns 480 x 272 (block means). `layer`: a CAMetalLayer (as void *)
// that present() draws into, or nullptr.
std::unique_ptr<Backend> make_metal_backend(std::string &error, void *layer = nullptr, int scale = 1);

} // namespace p3pios::gpu
