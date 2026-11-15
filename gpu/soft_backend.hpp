#pragma once
#include "gpu_backend.hpp"

#include <memory>

namespace p3pios::gpu {

// CPU implementation of Backend, for tests (soft_backend.cpp).
std::unique_ptr<Backend> make_soft_backend();

} // namespace p3pios::gpu
