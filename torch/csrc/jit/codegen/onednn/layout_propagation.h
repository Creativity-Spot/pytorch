#pragma once

#include <torch/csrc/jit/ir/ir.h>

namespace torch {
namespace jit {
namespace fuser {
namespace onednn {

void PropagateLayout(const std::shared_ptr<Graph>& graph);

} // namespace onednn
} // namespace fuser
} // namespace jit
} // namespace torch
