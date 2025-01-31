#if defined(USE_CUDA)
#include <gtest/gtest.h>

#include <torch/csrc/jit/codegen/cuda/arith.h>
#include <torch/csrc/jit/codegen/cuda/codegen.h>
#include <torch/csrc/jit/codegen/cuda/disjoint_set.h>
#include <torch/csrc/jit/codegen/cuda/executor.h>
#include <torch/csrc/jit/codegen/cuda/executor_launch_params.h>
#include <torch/csrc/jit/codegen/cuda/expr_evaluator.h>
#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/fusion_segmenter.h>
#include <torch/csrc/jit/codegen/cuda/interface.h>
#include <torch/csrc/jit/codegen/cuda/ir_all_nodes.h>
#include <torch/csrc/jit/codegen/cuda/ir_graphviz.h>
#include <torch/csrc/jit/codegen/cuda/ir_iostream.h>
#include <torch/csrc/jit/codegen/cuda/ir_printer.h>
#include <torch/csrc/jit/codegen/cuda/ir_utils.h>
#include <torch/csrc/jit/codegen/cuda/iter_visitor.h>
#include <torch/csrc/jit/codegen/cuda/kernel_cache.h>
#include <torch/csrc/jit/codegen/cuda/kernel_expr_evaluator.h>
#include <torch/csrc/jit/codegen/cuda/kernel_ir.h>
#include <torch/csrc/jit/codegen/cuda/lower2device.h>
#include <torch/csrc/jit/codegen/cuda/mma_type.h>
#include <torch/csrc/jit/codegen/cuda/mutator.h>
#include <torch/csrc/jit/codegen/cuda/ops/all_ops.h>
#include <torch/csrc/jit/codegen/cuda/root_domain_map.h>
#include <torch/csrc/jit/codegen/cuda/scheduler/all_schedulers.h>
#include <torch/csrc/jit/codegen/cuda/scheduler/reduction_utils.h>
#include <torch/csrc/jit/codegen/cuda/scheduler/utils.h>
#include <torch/csrc/jit/codegen/cuda/test/test_gpu_validator.h>
#include <torch/csrc/jit/codegen/cuda/transform_replay.h>
#include <torch/csrc/jit/codegen/cuda/transform_rfactor.h>

// fuser and IR parser
#include <ATen/cuda/CUDAContext.h>
#include <ATen/cuda/Exceptions.h>
#include <c10/cuda/CUDAStream.h>

#include <algorithm>
#include <iostream>

// Tests go in torch::jit
namespace torch {
namespace jit {

using namespace torch::jit::fuser::cuda;
using namespace at::indexing;

namespace {

// Make a tensor that is known to be fully contiguous of dimensionality=ndims,
// but unknown sizes
TensorView* makeContigTensor(size_t ndims, DataType dtype = DataType::Float) {
  return TensorViewBuilder()
      .ndims(ndims)
      .dtype(dtype)
      .contiguity(std::vector<bool>(ndims, true))
      .build();
}

// Make a tensor that is known to be non-contiguous of dimensionality=ndims,
// but unknown sizes
TensorView* makeSymbolicTensor(size_t ndims, DataType dtype = DataType::Float) {
  return TensorViewBuilder().ndims(ndims).dtype(dtype).build();
}

// Make a non-contiguous tensor of compile-time known sizes
TensorView* makeConcreteTensor(
    std::vector<int64_t> shape,
    DataType dtype = DataType::Float) {
  return TensorViewBuilder().shape(shape).dtype(dtype).build();
}

void checkIntValue(
    ExpressionEvaluator& evaluator,
    Val* val,
    Int::ScalarType expected_value) {
  TORCH_CHECK(val->isAnInt());
  const auto actual_value = evaluator.evaluate(val);
  TORCH_CHECK(actual_value.has_value());
  TORCH_CHECK(actual_value.value() == expected_value);
}

void checkIntValue(
    kir::ExpressionEvaluator& evaluator,
    const Val* val,
    Int::ScalarType expected_value) {
  const auto actual_value = evaluator.evaluate(val);
  TORCH_CHECK(actual_value.has_value());
  TORCH_CHECK(actual_value.value() == expected_value);
}

bool cudaArchGuardShouldSkip(int required_major, int required_minor) {
  int capability_major = at::cuda::getCurrentDeviceProperties()->major;
  int capability_minor = at::cuda::getCurrentDeviceProperties()->minor;

  if (capability_major < required_major ||
      (capability_major == required_major &&
       capability_minor < required_minor)) {
    return true;
  }
  return false;
}

#define NVFUSER_TEST_CUDA_ARCH_GUARD(REQUIRED_MAJOR, REQUIRED_MINOR)          \
  if (cudaArchGuardShouldSkip(REQUIRED_MAJOR, REQUIRED_MINOR)) {              \
    GTEST_SKIP() << "Requires GPU capability above " << REQUIRED_MAJOR << "." \
                 << REQUIRED_MINOR << " to run.\n";                           \
  }

#define NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(                                \
    REQUIRED_MAJOR, REQUIRED_MINOR, COMPILE_FUSION)                          \
  if (cudaArchGuardShouldSkip(REQUIRED_MAJOR, REQUIRED_MINOR)) {             \
    ASSERT_ANY_THROW(COMPILE_FUSION);                                        \
    GTEST_SKIP() << "(Lowered Only) Requires GPU capability above "          \
                 << REQUIRED_MAJOR << "." << REQUIRED_MINOR << " to run.\n"; \
  } else {                                                                   \
    COMPILE_FUSION;                                                          \
  }

} // namespace

// MMA unit test for a single instruction tile. VoltaTT
TEST_F(NVFuserTest, FusionVoltaMMATT_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // [M,K]
  auto tv0 = makeConcreteTensor({16, 4}, DataType::Half);
  // [K,N]
  auto tv1 = makeConcreteTensor({4, 16}, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,K,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {true, false, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {1});

  fusion.addOutput(tv2);

  // TODO: should be able to completely remove it
  //  in a follow up.
  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(16, 16, 4);
  gemm_tile.warp_tile = GemmTile(16, 16, 4);
  gemm_tile.instruction_tile = GemmTile(16, 16, 4);

  auto mma_builder = MmaBuilder(MmaOptions::MacroType::Volta_16_16_4, gemm_tile)
                         .layout(MmaOptions::MmaInputLayout::TT);
  mma_builder.configureMma(tv2);

  // Write A to smem
  auto tv0cw = tv0b->cacheAfter();
  // Read A from smem
  auto tv0cr = tv0cw->cacheAfter();

  // Write B to smem
  auto tv1cw = tv1b->cacheAfter();

  // Read B from smem
  auto tv1cr = tv1cw->cacheAfter();

  // Register accumulator
  auto tv2c = tv2->cacheBefore();

  // [M,K,N]->[M,N,K]
  tv0cr->reorder({{-2, -1}, {-1, -2}});

  // Schedule the instruction tile loops, which is the only
  //  part we have in this unit test.
  // Assumes last 3 dims are mnk
  // The innermost loops are dictated by the type of mma used,
  //   the scheduler needs to use mma_util::WarpMmaSwizzler to
  //   get the right thread swizzle. Currently this is the only
  //   method allowed to schedule the 3/2 inner most loops of
  //   mma input/output.
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  // [M,K,N]->[M,N,K]
  tv1cr->reorder({{-2, -1}, {-1, -2}});
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // [M,K,N]->[M,N,K]
  tv2c->reorder({{-2, -1}, {-1, -2}});

  // Schedule the output instruction tile.
  // Assumes last 3 dims are mnk
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  // Set memory type.
  tv0cw->setMemoryType(MemoryType::Shared);
  tv1cw->setMemoryType(MemoryType::Shared);

  at::manual_seed(0);
  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({16, 4}, options);
  auto t1 = at::randn({4, 16}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 0, fe.compileFusion(&fusion, {t0, t1}));
  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.to(at::kFloat).matmul(t1.to(at::kFloat));

  testValidate(&fusion, cg_outputs, {t0, t1}, {tref}, __LINE__, __FILE__);
}

// MMA unit test for a single instruction tile. VoltaTN
TEST_F(NVFuserTest, FusionVoltaMMATN_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // [M,K]
  auto tv0 = makeConcreteTensor({16, 4}, DataType::Half);
  // [N,K]
  auto tv1 = makeConcreteTensor({16, 4}, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,N,K]
  auto tv0b = broadcast(tv0, {false, true, false});
  auto tv1b = broadcast(tv1, {true, false, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {2});

  fusion.addOutput(tv2);

  // TODO: should be able to completely remove it
  //  in a follow up.
  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(16, 16, 4);
  gemm_tile.warp_tile = GemmTile(16, 16, 4);
  gemm_tile.instruction_tile = GemmTile(16, 16, 4);

  auto mma_builder = MmaBuilder(MmaOptions::MacroType::Volta_16_16_4, gemm_tile)
                         .layout(MmaOptions::MmaInputLayout::TN);

  mma_builder.configureMma(tv2);

  auto tv0cw = tv0b->cacheAfter();
  auto tv0cr = tv0cw->cacheAfter();
  auto tv1cw = tv1b->cacheAfter();
  auto tv1cr = tv1cw->cacheAfter();
  auto tv2c = tv2->cacheBefore();

  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  tv0cw->setMemoryType(MemoryType::Shared);
  tv1cw->setMemoryType(MemoryType::Shared);

  at::manual_seed(0);
  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({16, 4}, options);
  auto t1 = at::randn({16, 4}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 0, fe.compileFusion(&fusion, {t0, t1}));
  auto cg_outputs = fe.runFusion({t0, t1});
  auto tref = t0.to(at::kFloat).matmul(t1.t().to(at::kFloat));
  testValidate(&fusion, cg_outputs, {t0, t1}, {tref}, __LINE__, __FILE__);
}

// MMA unit test for a single instruction tile. VoltaNT
TEST_F(NVFuserTest, FusionVoltaMMANT_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // [K,M]
  auto tv0 = makeConcreteTensor({4, 16}, DataType::Half);
  // [K,N]
  auto tv1 = makeConcreteTensor({4, 16}, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [K,M,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {false, true, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {0});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(16, 16, 4);
  gemm_tile.warp_tile = GemmTile(16, 16, 4);
  gemm_tile.instruction_tile = GemmTile(16, 16, 4);

  auto mma_builder = MmaBuilder(MmaOptions::MacroType::Volta_16_16_4, gemm_tile)
                         .layout(MmaOptions::MmaInputLayout::NT);

  mma_builder.configureMma(tv2);

  auto tv0cw = tv0b->cacheAfter();
  auto tv0cr = tv0cw->cacheAfter();
  auto tv1cw = tv1b->cacheAfter();
  auto tv1cr = tv1cw->cacheAfter();
  auto tv2c = tv2->cacheBefore();

  // To MNK
  tv0cr->reorder({{0, 2}, {1, 0}, {2, 1}});
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  // To MNK
  tv1cr->reorder({{0, 2}, {1, 0}, {2, 1}});
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  tv2c->reorder({{0, 2}, {1, 0}, {2, 1}});
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv0cw->setMemoryType(MemoryType::Shared);
  tv1cw->setMemoryType(MemoryType::Shared);

  at::manual_seed(0);
  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({4, 16}, options);
  auto t1 = at::randn({4, 16}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 0, fe.compileFusion(&fusion, {t0, t1}));
  auto cg_outputs = fe.runFusion({t0, t1});
  auto tref = t0.t().to(at::kFloat).matmul(t1.to(at::kFloat));
  testValidate(&fusion, cg_outputs, {t0, t1}, {tref}, __LINE__, __FILE__);
}

// Gemm test for Volta MMA: TT
//  This is the only example that is fully manual,
//    the rest of them are facilitated by gemm utils.
TEST_F(NVFuserTest, FusionVoltaMatMulTT_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // Keep multiples of 8 to keep vectorizable.
  int M = 264, N = 120, K = 248;

  // [M,K]
  auto tv0 = makeContigTensor(2, DataType::Half);
  // [K,N]
  auto tv1 = makeContigTensor(2, DataType::Half);

  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,K,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {true, false, false});

  auto tv2 = fusedMultiplySum(tv0b, tv1b, {1});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(128, 128, 32);
  gemm_tile.warp_tile = GemmTile(64, 64, 32);
  gemm_tile.instruction_tile = GemmTile(16, 16, 4);

  auto mma_builder = MmaBuilder(MmaOptions::MacroType::Volta_16_16_4, gemm_tile)
                         .layout(MmaOptions::MmaInputLayout::TT);

  mma_builder.configureMma(tv2);

  auto tv0r = tv0->cacheAfter();
  auto tv1r = tv1->cacheAfter();
  auto tv0cw = tv0b->cacheAfter();
  auto tv0cr = tv0cw->cacheAfter();
  auto tv1cw = tv1b->cacheAfter();
  auto tv1cr = tv1cw->cacheAfter();
  auto tv2c = tv2->cacheBefore();

  // Make a CTA tile
  // ------------------------------------------------------------------
  // [M,N]
  tv2->split(-2, gemm_tile.cta_tile.m);
  tv2->split(-1, gemm_tile.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv2->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  tv0->computeAt(tv2, 2);
  tv1->computeAt(tv2, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv2c->split(-1, gemm_tile.cta_tile.k);
  tv2c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0r->computeAt(tv2c, 3);
  tv1r->computeAt(tv2c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------

  //       -3   -2  -1
  //[...    M,   N,  K]
  // Distribute warp tile: accumulator reg
  tv2c->split(-3, gemm_tile.warp_tile.m);
  tv2c->split(-2, gemm_tile.warp_tile.n);

  //  -5   -4   -3   -2   -1
  // [Mwo  Mw  Nwo   Nw   K]
  tv2c->split(-4, gemm_tile.instruction_tile.m);
  tv2c->split(-2, gemm_tile.instruction_tile.n);
  tv2c->split(-1, gemm_tile.instruction_tile.k);

  //   -8  -7 -6 -5 -4 -3 -2 -1
  // [Mwo Mw Mi Nwo Nw Ni Ko Ki]
  tv2c->reorder({{-7, -5}, {-6, -3}, {-5, -7}, {-3, -2}, {-2, -6}});
  //   -8  -7  -6 -5 -4 -3 -2 -1
  // [Mwo  Nwo Ko Mw Nw Mi Ni Ki]

  // Distribute warp tile: output tensor
  tv2->split(-2, gemm_tile.warp_tile.m);
  tv2->split(-1, gemm_tile.warp_tile.n);

  //  -4   -3   -2   -1
  // [Mwo  Mw  Nwo   Nw ]
  tv2->split(-3, gemm_tile.instruction_tile.m);
  tv2->split(-1, gemm_tile.instruction_tile.n);

  //  -6 -5  -4 -3 -2 -1
  // [Mwo Mw Mi Nwo Nw Ni]
  tv2->reorder({{-5, -4}, {-4, -2}, {-3, -5}, {-2, -3}});
  //  -6 -5  -4 -3 -2 -1
  // [Mwo Nwo Mw Nw Mi Ni]

  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]

  tv0cr->computeAt(tv2c, -4);
  tv1cr->computeAt(tv2c, -4);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,No,Ko,M,N,K]
  tv0cw->reorder({
      {-3, -2},
      {-2, -3},
  });
  // [Mo,No,Ko,N,M,K]
  tv0cw->merge(-2);
  tv0r->merge(-2);
  auto warp_dims = gemm_tile.cta_tile / gemm_tile.warp_tile;
  int num_of_thread = warp_dims.m * warp_dims.n * warp_dims.k * 32;
  int vector_word = 8;

  // Smem write
  tv0cw->split(-1, num_of_thread * vector_word);
  tv0cw->split(-1, 8);
  // [..., thread, vec]
  // distribute to warp:
  tv0cw->split(-2, 32);
  tv0cw->split(-3, warp_dims.n * warp_dims.k);

  tv0cw->axis(-1)->parallelize(ParallelType::Vectorize);
  tv0cw->axis(-2)->parallelize(ParallelType::TIDx);
  tv0cw->axis(-3)->parallelize(ParallelType::TIDy);
  tv0cw->axis(-4)->parallelize(ParallelType::TIDz);

  // Gmem read (reg staging)
  tv0r->split(-1, num_of_thread * vector_word);
  tv0r->split(-1, 8);
  // [..., thread, vec]
  // distribute to warp:
  tv0r->split(-2, 32);
  tv0r->split(-3, warp_dims.n * warp_dims.k);

  tv0r->axis(-1)->parallelize(ParallelType::Vectorize);
  tv0r->axis(-2)->parallelize(ParallelType::TIDx);
  tv0r->axis(-3)->parallelize(ParallelType::TIDy);
  tv0r->axis(-4)->parallelize(ParallelType::TIDz);

  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [Mo,No,Ko,M,N,K]
  tv1r->reorder({
      {-1, -2},
      {-2, -1},
  });
  tv1cw->reorder({
      {-1, -2},
      {-2, -1},
  });
  // [Mo,No,Ko,M,K,N]
  tv1cw->merge(-2);
  tv1r->merge(-2);
  // [Mo,No,Ko,i,wy,wx,v]
  tv1r->split(-1, num_of_thread * vector_word);
  tv1r->split(-1, 8);
  // [..., thread, vec]
  // distribute to warp:
  tv1r->split(-2, 32);
  tv1r->split(-3, warp_dims.n * warp_dims.k);

  tv1r->axis(-1)->parallelize(ParallelType::Vectorize);
  tv1r->axis(-2)->parallelize(ParallelType::TIDx);
  tv1r->axis(-3)->parallelize(ParallelType::TIDy);
  tv1r->axis(-4)->parallelize(ParallelType::TIDz);

  tv1cw->split(-1, num_of_thread * vector_word);
  tv1cw->split(-1, 8);
  // [..., thread, vec]
  // distribute to warp:
  tv1cw->split(-2, 32);
  tv1cw->split(-3, warp_dims.n * warp_dims.k);

  tv1cw->axis(-1)->parallelize(ParallelType::Vectorize);
  tv1cw->axis(-2)->parallelize(ParallelType::TIDx);
  tv1cw->axis(-3)->parallelize(ParallelType::TIDy);
  tv1cw->axis(-4)->parallelize(ParallelType::TIDz);

  tv1cw->setMemoryType(MemoryType::Shared);

  // Schedule mma input
  // ---------------------------------------------------------------------------

  // Use WarpMmaSwizzler for the innermost instruction tile.(Mi, Ni, Ki)
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  // Use WarpMmaSwizzler for the innermost instruction tile (Mi,Ni, Ki) on
  // output
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  //  -6 -5  -4 -3 -2 -1
  // [Mwo Nwo Mw Nw Mi Ni]
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  // Inline broadcast with smem write.
  tv0b->computeAt(tv0cw, -2);
  tv1b->computeAt(tv1cw, -2);

  // Vectorize smem read
  tv0cr->axis(-1)->parallelize(ParallelType::Vectorize);
  tv1cr->axis(-1)->parallelize(ParallelType::Vectorize);

  // Parallelize
  //  0   1  2  3    4   5  6  7  8  9  10
  // [Mo No Ko Mwo  Nwo Kw Mw Nw (Mi Ni Ki)]
  tv2c->axis(3)->parallelize(ParallelType::TIDz);
  tv2c->axis(4)->parallelize(ParallelType::TIDy);

  // Parallelize
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  tv2->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::BIDy);
  tv2->axis(2)->parallelize(ParallelType::TIDz);
  tv2->axis(3)->parallelize(ParallelType::TIDy);

  at::manual_seed(0);
  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({M, K}, options);
  auto t1 = at::randn({K, N}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 0, fe.compileFusion(&fusion, {t0, t1}));
  auto cg_outputs = fe.runFusion({t0, t1});
  auto tref = t0.to(at::kFloat).matmul(t1.to(at::kFloat));

  TORCH_CHECK(cg_outputs[0].allclose(tref, 0.0001, 0.0001));
}

// Gemm test for Volta MMA: TN
TEST_F(NVFuserTest, FusionVoltaMatMulTN_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);
  int M = 120, N = 264, K = 56;

  // [M,K]
  auto tv0 = makeContigTensor(2, DataType::Half);
  // [N,K]
  auto tv1 = makeContigTensor(2, DataType::Half);

  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,N,K]
  auto tv0b = broadcast(tv0, {false, true, false});
  auto tv1b = broadcast(tv1, {true, false, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {2});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(128, 128, 32);
  gemm_tile.warp_tile = GemmTile(64, 64, 32);
  gemm_tile.instruction_tile = GemmTile(16, 16, 4);

  auto mma_builder = MmaBuilder(MmaOptions::MacroType::Volta_16_16_4, gemm_tile)
                         .layout(MmaOptions::MmaInputLayout::TN);

  mma_builder.configureMma(tv2);

  auto tv0r = tv0->cacheAfter();
  auto tv1r = tv1->cacheAfter();
  auto tv0cw = tv0b->cacheAfter();
  auto tv0cr = tv0cw->cacheAfter();
  auto tv1cw = tv1b->cacheAfter();
  auto tv1cr = tv1cw->cacheAfter();
  auto tv2c = tv2->cacheBefore();

  // Make a CTA tile
  // ------------------------------------------------------------------
  // [M,N]
  tv2->split(-2, gemm_tile.cta_tile.m);
  tv2->split(-1, gemm_tile.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv2->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  tv0->computeAt(tv2, 2);
  tv1->computeAt(tv2, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv2c->split(-1, gemm_tile.cta_tile.k);
  tv2c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0r->computeAt(tv2c, 3);
  tv1r->computeAt(tv2c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(tv2c, gemm_tile);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv2, gemm_tile);
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv0cr->computeAt(tv2c, -4);
  tv1cr->computeAt(tv2c, -4);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,No,Ko,M,N,K]
  tv0cw->reorder({
      {-3, -2},
      {-2, -3},
  });
  // [Mo,No,Ko,N,M,K]
  tv0cw->merge(-2);
  tv0r->merge(-2);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0r, gemm_tile, 8);
  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [Mo,No,Ko,M,N,K]
  tv1cw->merge(-2);
  tv1r->merge(-2);
  // [Mo,No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1r, gemm_tile, 8);
  tv1cw->setMemoryType(MemoryType::Shared);
  // Schedule mma input
  // ---------------------------------------------------------------------------
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  tv0b->computeAt(tv0cw, -2);
  tv1b->computeAt(tv1cw, -2);

  tv0cr->axis(-1)->parallelize(ParallelType::Vectorize);
  tv1cr->axis(-1)->parallelize(ParallelType::Vectorize);
  // Parallelize
  //  0   1  2  3    4   5  6  7  8  9  10
  // [Mo No Ko Mwo  Nwo Kw Mw Nw (Mi Ni Ki)]
  tv2c->axis(3)->parallelize(ParallelType::TIDz);
  tv2c->axis(4)->parallelize(ParallelType::TIDy);

  // Parallelize
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  tv2->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::BIDy);
  tv2->axis(2)->parallelize(ParallelType::TIDz);
  tv2->axis(3)->parallelize(ParallelType::TIDy);

  at::manual_seed(0);
  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({M, K}, options);
  auto t1 = at::randn({N, K}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 0, fe.compileFusion(&fusion, {t0, t1}));
  auto cg_outputs = fe.runFusion({t0, t1});
  auto tref = t0.to(at::kFloat).matmul(t1.to(at::kFloat).t());
  TORCH_CHECK(cg_outputs[0].allclose(tref, 0.0001, 0.0001));
}

// Gemm test for Volta MMA: NT
TEST_F(NVFuserTest, FusionVoltaMatMulNT_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);
  int M = 240, N = 320, K = 136;

  // [K,M]
  auto tv0 = makeContigTensor(2, DataType::Half);
  // [K,N]
  auto tv1 = makeContigTensor(2, DataType::Half);

  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [K,M,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {false, true, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {0});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(128, 128, 32);
  gemm_tile.warp_tile = GemmTile(64, 64, 32);
  gemm_tile.instruction_tile = GemmTile(16, 16, 4);

  auto mma_builder = MmaBuilder(MmaOptions::MacroType::Volta_16_16_4, gemm_tile)
                         .layout(MmaOptions::MmaInputLayout::NT);

  mma_builder.configureMma(tv2);

  auto tv0r = tv0->cacheAfter();
  auto tv1r = tv1->cacheAfter();
  auto tv0cw = tv0b->cacheAfter();
  auto tv0cr = tv0cw->cacheAfter();
  auto tv1cw = tv1b->cacheAfter();
  auto tv1cr = tv1cw->cacheAfter();
  auto tv2c = tv2->cacheBefore();

  // Make a CTA tile
  // ------------------------------------------------------------------
  // [M,N]
  tv2->split(-2, gemm_tile.cta_tile.m);
  tv2->split(-1, gemm_tile.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv2->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  tv0->computeAt(tv2, 2);
  tv1->computeAt(tv2, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv2c->split(-1, gemm_tile.cta_tile.k);
  tv2c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0r->computeAt(tv2c, 3);
  tv1r->computeAt(tv2c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(tv2c, gemm_tile);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv2, gemm_tile);
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv0cr->computeAt(tv2c, -4);
  tv1cr->computeAt(tv2c, -4);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,No,Ko,M,N,K]
  tv0cw->reorder({{-3, -1}, {-2, -3}, {-1, -2}});
  // [Mo,No,Ko,N,K,M]
  tv0cw->merge(-2);

  // [Mo,No,M,K]
  tv0r->reorder({{-2, -1}, {-1, -2}});
  // [Mo,No,K,M]
  tv0r->merge(-2);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0r, gemm_tile, 8);
  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [Mo,No,Ko,M,N,K]
  tv1cw->reorder({{-2, -1}, {-1, -2}});
  tv1r->reorder({{-2, -1}, {-1, -2}});
  // [Mo,No,Ko,M,K,N]
  tv1cw->merge(-2);
  tv1r->merge(-2);
  // [Mo,No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1r, gemm_tile, 8);
  tv1cw->setMemoryType(MemoryType::Shared);
  // Schedule mma input
  // ---------------------------------------------------------------------------
  // [...M,N,K]
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  tv0b->computeAt(tv0cw, -2);
  tv1b->computeAt(tv1cw, -2);

  tv0cr->axis(-1)->parallelize(ParallelType::Vectorize);
  tv1cr->axis(-1)->parallelize(ParallelType::Vectorize);
  // Parallelize
  //  0   1  2  3    4   5  6  7  8  9  10
  // [Mo No Ko Mwo  Nwo Kw Mw Nw (Mi Ni Ki)]
  tv2c->axis(3)->parallelize(ParallelType::TIDz);
  tv2c->axis(4)->parallelize(ParallelType::TIDy);

  // Parallelize
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  tv2->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::BIDy);
  tv2->axis(2)->parallelize(ParallelType::TIDz);
  tv2->axis(3)->parallelize(ParallelType::TIDy);

  at::manual_seed(0);
  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({K, M}, options);
  auto t1 = at::randn({K, N}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 0, fe.compileFusion(&fusion, {t0, t1}));
  auto cg_outputs = fe.runFusion({t0, t1});
  auto tref = t0.to(at::kFloat).t().matmul(t1.to(at::kFloat));

  TORCH_CHECK(cg_outputs[0].allclose(tref, 0.0001, 0.0001));
}

// MMA unit test on Ampere
TEST_F(NVFuserTest, FusionAmpereMMATN_CUDA) {
  NVFUSER_TEST_CUDA_ARCH_GUARD(8, 0);

  Fusion fusion;
  FusionGuard fg(&fusion);

  // [M,K]
  auto tv0 = makeConcreteTensor({16, 16}, DataType::Half);
  // [N,K]
  auto tv1 = makeConcreteTensor({8, 16}, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,N,K]
  auto tv0b = broadcast(tv0, {false, true, false});
  auto tv1b = broadcast(tv1, {true, false, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {2});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(16, 8, 16);
  gemm_tile.warp_tile = GemmTile(16, 8, 16);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Ampere_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::TN);

  mma_builder.configureMma(tv2);

  auto tv0cw = tv0b->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1b->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());

  auto tv2c = tv2->cacheBefore();

  // [M,N,K] -> [N,M,K]
  tv0cr->reorder({{-2, -3}, {-3, -2}});
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  tv0cw->setMemoryType(MemoryType::Shared);
  tv1cw->setMemoryType(MemoryType::Shared);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({16, 16}, options);
  auto t1 = at::randn({8, 16}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      8, 0, fe.compileFusion(&fusion, {t0, t1}));
  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.to(at::kFloat).matmul(t1.t().to(at::kFloat));

  testValidate(&fusion, cg_outputs, {t0, t1}, {tref}, __LINE__, __FILE__);
}

// MMA unit test on Ampere
TEST_F(NVFuserTest, FusionAmpereMMATT_CUDA) {
  NVFUSER_TEST_CUDA_ARCH_GUARD(8, 0);

  Fusion fusion;
  FusionGuard fg(&fusion);

  // [M,K]
  auto tv0 = makeConcreteTensor({16, 16}, DataType::Half);
  // [K,N]
  auto tv1 = makeConcreteTensor({16, 8}, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,K,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {true, false, false});

  auto tv2 = fusedMultiplySum(tv0b, tv1b, {1});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(16, 8, 16);
  gemm_tile.warp_tile = GemmTile(16, 8, 16);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Ampere_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::TT);

  mma_builder.configureMma(tv2);

  auto tv0cw = tv0b->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1b->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());

  auto tv2c = tv2->cacheBefore();

  // [M,K,N] -> [N,M,K]
  tv0cr->reorder({{-3, -2}, {-2, -1}, {-1, -3}});
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  // [M,K,N] -> [M,N,K]
  tv1cr->reorder({{-2, -1}, {-1, -2}});
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // [M,K,N] -> [M,N,K]
  tv2c->reorder({{-2, -1}, {-1, -2}});
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  tv0cw->setMemoryType(MemoryType::Shared);
  tv1cw->setMemoryType(MemoryType::Shared);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({16, 16}, options);
  auto t1 = at::randn({16, 8}, options);

  FusionExecutor fe;

  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      8, 0, fe.compileFusion(&fusion, {t0, t1}));

  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.to(at::kFloat).matmul(t1.to(at::kFloat));

  testValidate(&fusion, cg_outputs, {t0, t1}, {tref}, __LINE__, __FILE__);
}

// MMA unit test on Ampere
TEST_F(NVFuserTest, FusionAmpereMMANT_CUDA) {
  NVFUSER_TEST_CUDA_ARCH_GUARD(8, 0);

  Fusion fusion;
  FusionGuard fg(&fusion);

  // [K,M]
  auto tv0 = makeConcreteTensor({16, 16}, DataType::Half);
  // [K,N]
  auto tv1 = makeConcreteTensor({16, 8}, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [K,M,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {false, true, false});
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {0});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(16, 8, 16);
  gemm_tile.warp_tile = GemmTile(16, 8, 16);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Ampere_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::NT);

  mma_builder.configureMma(tv2);

  auto tv0cw = tv0b->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1b->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());

  auto tv2c = tv2->cacheBefore();

  // [K,M,N] -> [N,M,K]
  tv0cr->reorder({{-3, -1}, {-1, -3}});
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  // [K,M,N] -> [M,N,K]
  tv1cr->reorder({
      {-3, -1},
      {-2, -3},
      {-1, -2},
  });
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // [K,M,N] -> [M,N,K]
  tv2c->reorder({{-3, -1}, {-2, -3}, {-1, -2}});
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  tv0cw->setMemoryType(MemoryType::Shared);
  tv1cw->setMemoryType(MemoryType::Shared);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({16, 16}, options);
  auto t1 = at::randn({16, 8}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      8, 0, fe.compileFusion(&fusion, {t0, t1}));
  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.t().to(at::kFloat).matmul(t1.to(at::kFloat));

  testValidate(&fusion, cg_outputs, {t0, t1}, {tref}, __LINE__, __FILE__);
}

// Matmul test on Ampere
TEST_F(NVFuserTest, FusionAmpereMatmulTN_CUDA) {
  NVFUSER_TEST_CUDA_ARCH_GUARD(8, 0);

  Fusion fusion;
  FusionGuard fg(&fusion);
  int M = 511, N = 257, K = 88;

  // [M,K]
  auto tv0 = makeContigTensor(2, DataType::Half);
  // [N,K]
  auto tv1 = makeContigTensor(2, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,N,K]
  auto tv0b = broadcast(tv0, {false, true, false});
  auto tv1b = broadcast(tv1, {true, false, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {2});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(128, 128, 32);
  gemm_tile.warp_tile = GemmTile(64, 64, 32);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Ampere_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::TN);

  mma_builder.configureMma(tv2);

  auto tv0r = tv0->cacheAfter();
  auto tv1r = tv1->cacheAfter();
  auto tv0cw = tv0r->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1r->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());
  auto tv2c = tv2->cacheBefore();

  // Make a CTA tile
  // ------------------------------------------------------------------
  // [M,N]
  tv2->split(-2, gemm_tile.cta_tile.m);
  tv2->split(-1, gemm_tile.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv2->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  tv0->computeAt(tv2, 2);
  tv1->computeAt(tv2, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv2c->split(-1, gemm_tile.cta_tile.k);
  tv2c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0r->computeAt(tv2c, 3);
  tv1r->computeAt(tv2c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(tv2c, gemm_tile);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv2, gemm_tile);
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv0cr->computeAt(tv2c, -4);
  tv1cr->computeAt(tv2c, -4);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,Ko,M,K]
  tv0cw->merge(-2);
  tv0r->merge(-2);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0r, gemm_tile, 8);
  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [No,Ko,N,K]
  tv1cw->merge(-2);
  tv1r->merge(-2);
  // [No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1r, gemm_tile, 8);
  tv1cw->setMemoryType(MemoryType::Shared);
  // Schedule mma input
  // ---------------------------------------------------------------------------
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  // [... Mi, Ni, Ki] want [Ni, Mi, Ki]
  tv0b->reorder({{-2, -3}, {-3, -2}});
  tv0b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());
  tv1b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  // Parallelize
  //  0   1  2  3    4   5  6  7  8  9  10
  // [Mo No Ko Mwo  Nwo Kw Mw Nw (Mi Ni Ki)]
  tv2c->axis(3)->parallelize(ParallelType::TIDz);
  tv2c->axis(4)->parallelize(ParallelType::TIDy);

  // Parallelize
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  tv2->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::BIDy);
  tv2->axis(2)->parallelize(ParallelType::TIDz);
  tv2->axis(3)->parallelize(ParallelType::TIDy);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({M, K}, options);
  auto t1 = at::randn({N, K}, options);

  FusionExecutor fe;

  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      8, 0, fe.compileFusion(&fusion, {t0, t1}));

  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.to(at::kFloat).matmul(t1.t().to(at::kFloat));

  TORCH_CHECK(cg_outputs[0].allclose(tref, 0.0001, 0.0001));
}

// Matmul test on Ampere
TEST_F(NVFuserTest, FusionAmpereMatmulTT_CUDA) {
  NVFUSER_TEST_CUDA_ARCH_GUARD(8, 0);

  Fusion fusion;
  FusionGuard fg(&fusion);
  int M = 512, N = 256, K = 128;

  // [M,K]
  auto tv0 = makeContigTensor(2, DataType::Half);
  // [K,N]
  auto tv1 = makeContigTensor(2, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,K,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {true, false, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {1});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(128, 128, 32);
  gemm_tile.warp_tile = GemmTile(64, 64, 32);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Ampere_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::TT);

  mma_builder.configureMma(tv2);

  auto tv0r = tv0->cacheAfter();
  auto tv1r = tv1->cacheAfter();
  auto tv0cw = tv0r->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1r->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());
  auto tv2c = tv2->cacheBefore();

  // Make a CTA tile
  // ------------------------------------------------------------------
  // [M,N]
  tv2->split(-2, gemm_tile.cta_tile.m);
  tv2->split(-1, gemm_tile.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv2->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  tv0->computeAt(tv2, 2);
  tv1->computeAt(tv2, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv2c->split(-1, gemm_tile.cta_tile.k);
  tv2c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0r->computeAt(tv2c, 3);
  tv1r->computeAt(tv2c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(tv2c, gemm_tile);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv2, gemm_tile);
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv0cr->computeAt(tv2c, -4);
  tv1cr->computeAt(tv2c, -4);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,Ko,M,K]
  tv0cw->merge(-2);
  tv0r->merge(-2);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0r, gemm_tile, 8);
  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [No,Ko,N,K] -> [No,Ko,K,N]
  tv1cw->reorder({{-2, -1}, {-1, -2}});
  tv1r->reorder({{-2, -1}, {-1, -2}});
  tv1cw->merge(-2);
  tv1r->merge(-2);
  // [No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1r, gemm_tile, 8);
  tv1cw->setMemoryType(MemoryType::Shared);
  // Schedule mma input
  // ---------------------------------------------------------------------------
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());
  // [... Mi, Ni, Ki] want [Ni, Mi, Ki]
  tv0b->reorder({{-2, -3}, {-3, -2}});
  tv0b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());
  tv1b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  // Parallelize
  //  0   1  2  3    4   5  6  7  8  9  10
  // [Mo No Ko Mwo  Nwo Kw Mw Nw (Mi Ni Ki)]
  tv2c->axis(3)->parallelize(ParallelType::TIDz);
  tv2c->axis(4)->parallelize(ParallelType::TIDy);

  // Parallelize
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  tv2->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::BIDy);
  tv2->axis(2)->parallelize(ParallelType::TIDz);
  tv2->axis(3)->parallelize(ParallelType::TIDy);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({M, K}, options);
  auto t1 = at::randn({K, N}, options);

  FusionExecutor fe;

  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      8, 0, fe.compileFusion(&fusion, {t0, t1}));

  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.to(at::kFloat).matmul(t1.to(at::kFloat));

  TORCH_CHECK(cg_outputs[0].allclose(tref, 0.0001, 0.0001));
}

// Matmul test on Ampere
TEST_F(NVFuserTest, FusionAmpereMatmulNT_CUDA) {
  NVFUSER_TEST_CUDA_ARCH_GUARD(8, 0);

  Fusion fusion;
  FusionGuard fg(&fusion);
  int M = 512, N = 256, K = 128;

  // [K,M]
  auto tv0 = makeContigTensor(2, DataType::Half);
  // [K,N]
  auto tv1 = makeContigTensor(2, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [K,M,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {false, true, false});

  auto tv2 = fusedMultiplySum(tv0b, tv1b, {0});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(128, 128, 32);
  gemm_tile.warp_tile = GemmTile(64, 64, 32);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Ampere_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::NT);

  mma_builder.configureMma(tv2);

  auto tv0r = tv0->cacheAfter();
  auto tv1r = tv1->cacheAfter();
  auto tv0cw = tv0r->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1r->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());
  auto tv2c = tv2->cacheBefore();

  // Make a CTA tile
  // ------------------------------------------------------------------
  // [M,N]
  tv2->split(-2, gemm_tile.cta_tile.m);
  tv2->split(-1, gemm_tile.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv2->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  tv0->computeAt(tv2, 2);
  tv1->computeAt(tv2, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv2c->split(-1, gemm_tile.cta_tile.k);
  tv2c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0r->computeAt(tv2c, 3);
  tv1r->computeAt(tv2c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(tv2c, gemm_tile);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv2, gemm_tile);
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv0cr->computeAt(tv2c, -4);
  tv1cr->computeAt(tv2c, -4);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,Ko,M,K] -> [..., K,M]
  tv0cw->reorder({{-2, -1}, {-1, -2}});
  tv0r->reorder({{-2, -1}, {-1, -2}});
  tv0cw->merge(-2);
  tv0r->merge(-2);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0r, gemm_tile, 8);
  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [No,Ko,N,K] -> [No,Ko,K,N]
  tv1cw->reorder({{-2, -1}, {-1, -2}});
  tv1r->reorder({{-2, -1}, {-1, -2}});
  tv1cw->merge(-2);
  tv1r->merge(-2);
  // [No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1r, gemm_tile, 8);
  tv1cw->setMemoryType(MemoryType::Shared);
  // Schedule mma input
  // ---------------------------------------------------------------------------
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  // [... Mi, Ni, Ki] want [Ni, Mi, Ki]
  tv0b->reorder({{-2, -3}, {-3, -2}});
  tv0b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());
  tv1b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  // Parallelize
  //  0   1  2  3    4   5  6  7  8  9  10
  // [Mo No Ko Mwo  Nwo Kw Mw Nw (Mi Ni Ki)]
  tv2c->axis(3)->parallelize(ParallelType::TIDz);
  tv2c->axis(4)->parallelize(ParallelType::TIDy);

  // Parallelize
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  tv2->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::BIDy);
  tv2->axis(2)->parallelize(ParallelType::TIDz);
  tv2->axis(3)->parallelize(ParallelType::TIDy);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({K, M}, options);
  auto t1 = at::randn({K, N}, options);

  FusionExecutor fe;

  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      8, 0, fe.compileFusion(&fusion, {t0, t1}));

  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.t().to(at::kFloat).matmul(t1.to(at::kFloat));

  TORCH_CHECK(cg_outputs[0].allclose(tref, 0.0001, 0.0001));
}

// Matmul-Matmul fusion test on Ampere
TEST_F(NVFuserTest, FusionMatmulMatmulAmpere_CUDA) {
  NVFUSER_TEST_CUDA_ARCH_GUARD(8, 0);

  Fusion fusion;
  FusionGuard fg(&fusion);
  int M = 512, N = 256, K1 = 128, K2 = 128;

  // Fusion definition (Both gemms are TN)
  // [M,K1]
  auto tv0 = makeConcreteTensor({M, K1}, DataType::Half);
  // [K2,K1]
  auto tv1 = makeConcreteTensor({K2, K1}, DataType::Half);
  // [N,K2]
  auto tv2 = makeConcreteTensor({N, K2}, DataType::Half);

  fusion.addInput(tv0);
  fusion.addInput(tv1);
  fusion.addInput(tv2);

  // [M,N,K]
  auto tv0b = broadcast(tv0, {false, true, false});
  auto tv1b = broadcast(tv1, {true, false, false});
  auto tv2b = broadcast(tv2, {true, false, false});

  // [M,K2,R]
  auto tv3 = fusedMultiplySum(tv0b, tv1b, {2});

  auto tv3h = castOp(DataType::Half, tv3);
  auto tv3b = broadcast(tv3h, {false, true, false});

  auto tv4 = fusedMultiplySum(tv3b, tv2b, {2});

  fusion.addOutput(tv4);

  // Fusion:
  //  Gemm(M,K2,K1) x Gemm(M,N,K2)

  MatMulTileOptions gemm_tile1, gemm_tile2;

  // cta tile:
  //  To save register, n of cta tile 1
  //  matches k of cta tile2
  gemm_tile1.cta_tile = GemmTile(128, 64, 32);
  gemm_tile2.cta_tile = GemmTile(128, 32, 64);

  // Distribute to 2x2 warps
  gemm_tile1.warp_tile = GemmTile(64, 32, 32);
  gemm_tile2.warp_tile = GemmTile(64, 16, 64);

  // Using Ampere mma macro
  gemm_tile2.instruction_tile = GemmTile(16, 8, 16);
  gemm_tile2.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder1 =
      MmaBuilder(MmaOptions::MacroType::Ampere_16_8_16, gemm_tile1)
          .layout(MmaOptions::MmaInputLayout::TN);

  auto mma_builder2 =
      MmaBuilder(MmaOptions::MacroType::Ampere_16_8_16, gemm_tile2)
          .layout(MmaOptions::MmaInputLayout::TN);

  mma_builder1.configureMma(tv3);
  mma_builder2.configureMma(tv4);

  // Global read for gemm 1
  auto tv0r = tv0->cacheAfter();
  auto tv1r = tv1->cacheAfter();

  // Global read for gemm 2
  auto tv2r = tv2->cacheAfter();

  // Gemm 1 main loop read
  auto tv0cw = tv0r->cacheAfter();
  auto tv0cr = tv0cw->cacheAfter(LoadStoreOpType::LdMatrix);
  auto tv1cw = tv1r->cacheAfter();
  auto tv1cr = tv1cw->cacheAfter(LoadStoreOpType::LdMatrix);

  // Gemm 1 accumulator reg
  auto tv3c = tv3->cacheBefore();

  // Gemm 2 main loop read
  auto tv3cw = tv3h->cacheAfter();
  auto tv3cr = tv3cw->cacheAfter(LoadStoreOpType::LdMatrix);

  auto tv2cw = tv2r->cacheAfter();
  auto tv2cr = tv2cw->cacheAfter(LoadStoreOpType::LdMatrix);

  // Gemm 2 accumulator reg
  auto tv4c = tv4->cacheBefore();

  // General idea is inlining gemm1's main loop inside gemm2's

  // Schedule gemm 2:
  // ------------------------------------------------------------------
  tv4->split(-2, gemm_tile2.cta_tile.m);
  tv4->split(-1, gemm_tile2.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv4->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  tv2->computeAt(tv4, 2);
  tv3->computeAt(tv4, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv4c->split(-1, gemm_tile2.cta_tile.k);
  tv4c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv3->computeAt(tv4c, 3); // Implicitly defines cta tile of gemm1
  tv2r->computeAt(tv4c, 3);

  // Make warp tile
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(
      tv4c, gemm_tile2);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv4, gemm_tile2);
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv3cr->computeAt(tv4c, -4);
  tv2cr->computeAt(tv4c, -4);

  // Schedule tv2 gmem read and smem write:
  // ----------------------------------------------------------------
  // [No,Ko,N,K]
  tv2cw->merge(-2);
  tv2r->merge(-2);

  // [No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv2cw, gemm_tile2, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv2r, gemm_tile2, 8);
  tv2cw->setMemoryType(MemoryType::Shared);

  // Schedule tv2 gmem read and smem write:
  // ----------------------------------------------------------------

  // Schedule gemm 2 mma input
  // ---------------------------------------------------------------------------
  tv3cr->applyMmaSwizzle(mma_builder2.operand(MmaOptions::Operand::A).build());

  // [... Mi, Ni, Ki] want [Ni, Mi, Ki]
  tv3b->reorder({{-2, -3}, {-3, -2}});
  tv3b->applyMmaSwizzle(mma_builder2.operand(MmaOptions::Operand::A).build());

  tv2cr->applyMmaSwizzle(mma_builder2.operand(MmaOptions::Operand::B).build());
  tv2b->applyMmaSwizzle(mma_builder2.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv4c->applyMmaSwizzle(
      mma_builder2.operand(MmaOptions::Operand::Accumulator).build());
  tv4->applyMmaSwizzle(
      mma_builder2.operand(MmaOptions::Operand::Accumulator).build());

  // Schedule gemm 1:
  // ------------------------------------------------------------------

  // CTA tile:
  tv0->computeAt(tv3, 2);
  tv1->computeAt(tv3, 2);

  // Schedule K dim for gemm 1:

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv3c->split(-1, gemm_tile1.cta_tile.k);
  tv3c->reorder({{2, 3}, {3, 4}, {4, 2}});
  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0r->computeAt(tv3c, 3);
  tv1r->computeAt(tv3c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(
      tv3c, gemm_tile1);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv3cw, gemm_tile1);

  tv0cr->computeAt(tv3c, -4);
  tv1cr->computeAt(tv3c, -4);

  tv3->computeAt(tv3cw, -3);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,Ko,M,K]
  tv0cw->merge(-2);
  tv0r->merge(-2);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0cw, gemm_tile1, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0r, gemm_tile1, 8);
  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [No,Ko,N,K]
  tv1cw->merge(-2);
  tv1r->merge(-2);
  // [No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1cw, gemm_tile1, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1r, gemm_tile1, 8);
  tv1cw->setMemoryType(MemoryType::Shared);

  // Schedule mma input
  // ---------------------------------------------------------------------------
  tv0cr->applyMmaSwizzle(mma_builder1.operand(MmaOptions::Operand::A).build());
  // [... Mi, Ni, Ki] want [Ni, Mi, Ki]
  tv0b->reorder({{-2, -3}, {-3, -2}});
  tv0b->applyMmaSwizzle(mma_builder1.operand(MmaOptions::Operand::A).build());

  tv1cr->applyMmaSwizzle(mma_builder1.operand(MmaOptions::Operand::B).build());
  tv1b->applyMmaSwizzle(mma_builder1.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv3c->applyMmaSwizzle(
      mma_builder1.operand(MmaOptions::Operand::Accumulator).build());
  tv3cw->applyMmaSwizzle(
      mma_builder1.operand(MmaOptions::Operand::Accumulator).build());
  tv3h->applyMmaSwizzle(
      mma_builder1.operand(MmaOptions::Operand::Accumulator).build());
  tv3->applyMmaSwizzle(
      mma_builder1.operand(MmaOptions::Operand::Accumulator).build());
  tv3cw->setMemoryType(MemoryType::Shared);

  // Parallelize
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  // Gemm 1
  tv3c->axis(3)->parallelize(ParallelType::TIDz);
  tv3c->axis(4)->parallelize(ParallelType::TIDy);

  tv3->computeAt(tv3cw, -2);
  tv3cw->axis(2)->parallelize(ParallelType::TIDz);
  tv3cw->axis(3)->parallelize(ParallelType::TIDy);

  // Gemm 2
  tv4->axis(2)->parallelize(ParallelType::TIDz);
  tv4->axis(3)->parallelize(ParallelType::TIDy);
  tv4c->axis(3)->parallelize(ParallelType::TIDz);
  tv4c->axis(4)->parallelize(ParallelType::TIDy);

  tv4->axis(0)->parallelize(ParallelType::BIDx);
  tv4->axis(1)->parallelize(ParallelType::BIDy);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({M, K1}, options);
  auto t1 = at::randn({K2, K1}, options);
  auto t2 = at::randn({N, K2}, options);

  auto tref = t0.to(at::kFloat)
                  .matmul(t1.t().to(at::kFloat))
                  .matmul(t2.t().to(at::kFloat));

  FusionExecutor fe;

  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      8, 0, fe.compileFusion(&fusion, {t0, t1, t2}));

  auto cg_outputs = fe.runFusion({t0, t1, t2});

  // relaxed check for now, err accumulation is significant.
  TORCH_CHECK(cg_outputs[0].allclose(tref, 0.1, 0.1));
}

// Simplified Matmul-Softmax-Matmul test on Ampere
//   (To be extended in follow ups)
TEST_F(NVFuserTest, FusionMatmulSoftmaxMatmulAmpere_CUDA) {
  NVFUSER_TEST_CUDA_ARCH_GUARD(8, 0);

  Fusion fusion;
  FusionGuard fg(&fusion);

  // Omitting outer dimensions and pointwise ops

  const int seql_q = 32;
  const int seql_k = 128;
  const int hidden_size = 1024;
  const int num_heads = 16;
  const int head_dim = hidden_size / num_heads;

  // Gemm 1:
  // (80, 80, 64)
  const int M1 = seql_q, N1 = seql_k, K1 = head_dim;
  // (80, 64, 80)
  const int M2 = seql_q, N2 = head_dim, K2 = seql_k;

  // Fusion definition (Both gemms are TN)
  // [M,K1]
  auto inp = makeConcreteTensor({M1, K1}, DataType::Half);
  // Query matrix
  auto qk = makeConcreteTensor({N1, K1}, DataType::Half);
  // Second linear matrix
  auto acc = makeConcreteTensor({N2, K2}, DataType::Half);

  fusion.addInput(inp);
  fusion.addInput(qk);
  fusion.addInput(acc);

  // [M,N,K]
  auto tv0b = broadcast(inp, {false, true, false});
  auto tv1b = broadcast(qk, {true, false, false});
  auto tv2b = broadcast(acc, {true, false, false});

  // [M,K2,R]
  auto tv3 = fusedMultiplySum(tv0b, tv1b, {2});

  // Inline define softmax for now for scheduling
  auto x = tv3;
  const int kReductionAxis = 1;
  const int kNumberOfDims = 2;
  std::vector<bool> broadcast_mask(kNumberOfDims, false);
  broadcast_mask[kReductionAxis] = true;

  auto max_val = max(x, {kReductionAxis});
  auto bcast_max = broadcast(max_val, broadcast_mask);
  auto x_max_sub = sub(x, bcast_max);
  auto exp_val = exp(x_max_sub);
  auto sum_exp = sum(exp_val, {kReductionAxis});
  auto bcast_sum = broadcast(sum_exp, broadcast_mask);
  auto recip = reciprocal(bcast_sum);
  auto tv3sfm = mul(exp_val, recip);

  auto tv3h = castOp(DataType::Half, tv3sfm);
  auto tv3b = broadcast(tv3h, {false, true, false});
  auto tv4 = fusedMultiplySum(tv3b, tv2b, {2});

  fusion.addOutput(tv4);

  // Fusion:
  //  Gemm(M,K2,K1) x Gemm(M,N,K2)
  MatMulTileOptions gemm_tile;

  // TODO: use very small tiles for now since
  //  alias pass is not re-using smem. Fix later.
  gemm_tile.cta_tile = GemmTile(32, 128, 32);

  // Distribute to 2x2 warps
  gemm_tile.warp_tile = GemmTile(16, 64, 32);

  // Using Ampere mma macro
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder1 =
      MmaBuilder(MmaOptions::MacroType::Ampere_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::TN);

  auto mma_builder2 =
      MmaBuilder(MmaOptions::MacroType::Ampere_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::TN);

  mma_builder1.configureMma(tv3);
  mma_builder2.configureMma(tv4);

  // Global read for gemm 1
  auto tv0r = inp->cacheAfter();
  auto tv1r = qk->cacheAfter();

  // Global read for gemm 2
  auto tv2r = acc->cacheAfter();

  // Gemm 1 main loop read
  auto tv0cw = tv0r->cacheAfter();
  auto tv0cr = tv0cw->cacheAfter(LoadStoreOpType::LdMatrix);
  auto tv1cw = tv1r->cacheAfter();
  auto tv1cr = tv1cw->cacheAfter(LoadStoreOpType::LdMatrix);

  // Gemm 1 accumulator reg
  auto tv3c = tv3->cacheBefore();

  // Softmax conversion:
  auto tv3ccr = tv3->cacheAfter();

  // tv3ccr -> tv3h : softmax

  // Gemm 2 main loop read
  // auto tv3cw = tv3h->cacheAfter();
  auto tv3cr = tv3h->cacheAfter(LoadStoreOpType::LdMatrix);

  auto tv2cw = tv2r->cacheAfter();
  auto tv2cr = tv2cw->cacheAfter(LoadStoreOpType::LdMatrix);

  // Gemm 2 accumulator reg
  auto tv4c = tv4->cacheBefore();

  // Schedule gemm 2:
  // ------------------------------------------------------------------
  tv4->split(-2, gemm_tile.cta_tile.m);
  tv4->split(-1, gemm_tile.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv4->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  acc->computeAt(tv4, 2);
  tv3->computeAt(tv4, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv4c->split(-1, gemm_tile.cta_tile.k);
  tv4c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv3->computeAt(tv4c, 2);
  tv2r->computeAt(tv4c, 3);

  // Make warp tile
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(tv4c, gemm_tile);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv4, gemm_tile);
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv3cr->computeAt(tv4c, -4);
  tv2cr->computeAt(tv4c, -4);

  // Schedule tv2 gmem read and smem write:
  // ----------------------------------------------------------------
  // [No,Ko,N,K]
  tv2cw->merge(-2);
  tv2r->merge(-2);

  // [No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv2cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv2r, gemm_tile, 8);
  tv2cw->setMemoryType(MemoryType::Shared);

  // Schedule tv2 gmem read and smem write:
  // ----------------------------------------------------------------

  // Schedule gemm 2 mma input
  // ---------------------------------------------------------------------------
  tv3cr->applyMmaSwizzle(mma_builder2.operand(MmaOptions::Operand::A).build());
  // [... Mi, Ni, Ki] want [Ni, Mi, Ki]
  tv3b->reorder({{-2, -3}, {-3, -2}});
  tv3b->applyMmaSwizzle(mma_builder2.operand(MmaOptions::Operand::A).build());

  tv2cr->applyMmaSwizzle(mma_builder2.operand(MmaOptions::Operand::B).build());
  tv2b->applyMmaSwizzle(mma_builder2.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv4c->applyMmaSwizzle(
      mma_builder2.operand(MmaOptions::Operand::Accumulator).build());
  tv4->applyMmaSwizzle(
      mma_builder2.operand(MmaOptions::Operand::Accumulator).build());

  // Schedule gemm 1:
  // ------------------------------------------------------------------

  // CTA tile:
  // [Mo, Mi128, N80]

  tv3->split(-1, gemm_tile.cta_tile.n);
  // [Mo, Mi128, No, Ni128]

  tv3->reorder({{1, 2}, {2, 1}});

  // [Mo, No, Mi128, Ni128]
  inp->computeAt(tv3, 2);
  qk->computeAt(tv3, 2);

  // Schedule K dim for gemm 1:

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv3c->split(-1, gemm_tile.cta_tile.k);
  tv3c->reorder({{2, 3}, {3, 4}, {4, 2}});
  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0r->computeAt(tv3c, 3);
  tv1r->computeAt(tv3c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(tv3c, gemm_tile);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv3, gemm_tile);

  tv0cr->computeAt(tv3c, -4);
  tv1cr->computeAt(tv3c, -4);

  // tv3->computeAt(tv3cw,-3);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,Ko,M,K]
  tv0cw->merge(-2);
  tv0r->merge(-2);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0r, gemm_tile, 8);
  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [No,Ko,N,K]
  tv1cw->merge(-2);
  tv1r->merge(-2);
  // [No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1r, gemm_tile, 8);
  tv1cw->setMemoryType(MemoryType::Shared);

  // Schedule mma input
  // ---------------------------------------------------------------------------
  tv0cr->applyMmaSwizzle(mma_builder1.operand(MmaOptions::Operand::A).build());
  // [... Mi, Ni, Ki] want [Ni, Mi, Ki]
  tv0b->reorder({{-2, -3}, {-3, -2}});
  tv0b->applyMmaSwizzle(mma_builder1.operand(MmaOptions::Operand::A).build());

  tv1cr->applyMmaSwizzle(mma_builder1.operand(MmaOptions::Operand::B).build());
  tv1b->applyMmaSwizzle(mma_builder1.operand(MmaOptions::Operand::B).build());

  // // Schedule mma output
  // //
  // ---------------------------------------------------------------------------
  tv3c->applyMmaSwizzle(
      mma_builder1.operand(MmaOptions::Operand::Accumulator).build());
  tv3->applyMmaSwizzle(
      mma_builder1.operand(MmaOptions::Operand::Accumulator).build());

  // mma_util::WarpMmaSwizzler::scheduleMmaWarpOutput(tv3ccw,
  // mma_builder1.build());

  // Put tv3 result in smem
  tv3->setMemoryType(MemoryType::Shared);

  // schedule a reg persistent softmax: from tv3
  // [Mo, M128, RN]
  max_val->split(-1, 128);
  // [Mo, M128, RN1, RN128]
  max_val->split(-1, 4);
  // Map to warp (2x2)
  max_val->split(-4, 4);
  max_val->split(-4, 2);

  // [Mo, Mo32, My2, Mx2, RN1, RNo32, RNi4]
  auto max_rf = max_val->rFactor({-1});
  // [Mo, Mo32, My2, Mx2, RN1, I32, RNi4]

  // [Mo, M128, RN]
  sum_exp->split(-1, 128);
  // [Mo, M128, RN1, RN128]
  sum_exp->split(-1, 4);
  // Map to warp (2x2)
  sum_exp->split(-4, 4);
  sum_exp->split(-4, 2);

  // [Mo, Mo32, My2, Mx2, RN1, RNo32, RNi4]
  auto sum_exp_rf = sum_exp->rFactor({-1});
  // [Mo, Mo32, My2, Mx2, RN1, I32, RNi4]

  exp_val->computeAt(sum_exp_rf, 4);
  exp_val->split(-1, 128);
  exp_val->split(-1, 4);
  bcast_max->computeAt(exp_val, -2);

  // [Mo, Mo32, My2, Mx2, IN1, I32, INi4]

  // Read from smem
  tv3ccr->computeAt(max_rf, 4);
  // [Mo, Mo32, My2, Mx2, N80]
  tv3ccr->split(-1, 128);
  tv3ccr->split(-1, 4);
  // [Mo, Mo32, My2, Mx2, IN1, I32, INi4]

  // Write to second gemm
  tv3h->split(-1, 128);
  tv3h->split(-1, 4);
  // Map to warp (2x2)
  tv3h->split(-4, 4);
  tv3h->split(-4, 2);

  bcast_sum->computeAt(tv3h, -2);

  tv3h->setMemoryType(MemoryType::Shared);

  // Parallelize
  tv4->axis(0)->parallelize(ParallelType::BIDx);
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  // Gemm 1
  tv3c->axis(3)->parallelize(ParallelType::TIDz);
  tv3c->axis(4)->parallelize(ParallelType::TIDy);
  tv3->axis(2)->parallelize(ParallelType::TIDz);
  tv3->axis(3)->parallelize(ParallelType::TIDy);

  auto parallelize_non_reduced_val = [](TensorView* tv) {
    tv->axis(-2)->parallelize(ParallelType::TIDx);
    tv->axis(2)->parallelize(ParallelType::TIDz);
    tv->axis(3)->parallelize(ParallelType::TIDy);
  };

  auto parallelize_reduced_val = [](TensorView* tv) {
    tv->axis(-1)->parallelize(ParallelType::TIDx);
    tv->axis(2)->parallelize(ParallelType::TIDz);
    tv->axis(3)->parallelize(ParallelType::TIDy);
  };

  parallelize_non_reduced_val(tv3h);
  parallelize_non_reduced_val(max_rf);
  parallelize_non_reduced_val(bcast_max);
  parallelize_non_reduced_val(exp_val);
  parallelize_non_reduced_val(sum_exp_rf);
  parallelize_non_reduced_val(bcast_sum);
  parallelize_non_reduced_val(recip);

  parallelize_reduced_val(max_val);
  parallelize_reduced_val(sum_exp);

  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  // Gemm 2
  tv4->axis(2)->parallelize(ParallelType::TIDz);
  tv4->axis(3)->parallelize(ParallelType::TIDy);
  tv4c->axis(3)->parallelize(ParallelType::TIDz);
  tv4c->axis(4)->parallelize(ParallelType::TIDy);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({M1, K1}, options);
  auto t1 = at::randn({N1, K1}, options);
  auto t2 = at::randn({N2, K2}, options);

  FusionExecutor fe;

  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      8, 0, fe.compileFusion(&fusion, {t0, t1, t2}));

  auto cg_outputs = fe.runFusion({t0, t1, t2});

  auto g1 = t0.to(at::kFloat).matmul(t1.t().to(at::kFloat));
  auto sg1 = at::_softmax(g1, -1, false);
  auto gsg1 = sg1.matmul(t2.t().to(at::kFloat));

  TORCH_CHECK(cg_outputs[0].allclose(gsg1, 0.001, 0.001));
}

// MMA unit test on Turing
TEST_F(NVFuserTest, FusionTuringMMATN_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // [M,K]
  auto tv0 = makeConcreteTensor({16, 16}, DataType::Half);
  // [N,K]
  auto tv1 = makeConcreteTensor({8, 16}, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,N,K]
  auto tv0b = broadcast(tv0, {false, true, false});
  auto tv1b = broadcast(tv1, {true, false, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {2});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(16, 8, 16);
  gemm_tile.warp_tile = GemmTile(16, 8, 16);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Turing_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::TN);

  mma_builder.configureMma(tv2);

  auto tv0cw = tv0b->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1b->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());

  auto tv2c = tv2->cacheBefore();

  // [M,N,K] -> [N,M,K]
  tv0cr->reorder({{-2, -3}, {-3, -2}});
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  tv0cw->setMemoryType(MemoryType::Shared);
  tv1cw->setMemoryType(MemoryType::Shared);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({16, 16}, options);
  auto t1 = at::randn({8, 16}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 5, fe.compileFusion(&fusion, {t0, t1}));

  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.to(at::kFloat).matmul(t1.t().to(at::kFloat));

  testValidate(&fusion, cg_outputs, {t0, t1}, {tref}, __LINE__, __FILE__);
}

// MMA unit test on Turing
TEST_F(NVFuserTest, FusionTuringMMATT_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // [M,K]
  auto tv0 = makeConcreteTensor({16, 16}, DataType::Half);
  // [K,N]
  auto tv1 = makeConcreteTensor({16, 8}, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,K,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {true, false, false});

  auto tv2 = fusedMultiplySum(tv0b, tv1b, {1});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(16, 8, 16);
  gemm_tile.warp_tile = GemmTile(16, 8, 16);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Turing_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::TT);

  mma_builder.configureMma(tv2);

  auto tv0cw = tv0b->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1b->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());

  auto tv2c = tv2->cacheBefore();

  // [M,K,N] -> [N,M,K]
  tv0cr->reorder({{-3, -2}, {-2, -1}, {-1, -3}});
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  // [M,K,N] -> [M,N,K]
  tv1cr->reorder({{-2, -1}, {-1, -2}});
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // [M,K,N] -> [M,N,K]
  tv2c->reorder({{-2, -1}, {-1, -2}});
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  tv0cw->setMemoryType(MemoryType::Shared);
  tv1cw->setMemoryType(MemoryType::Shared);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({16, 16}, options);
  auto t1 = at::randn({16, 8}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 5, fe.compileFusion(&fusion, {t0, t1}));

  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.to(at::kFloat).matmul(t1.to(at::kFloat));

  testValidate(&fusion, cg_outputs, {t0, t1}, {tref}, __LINE__, __FILE__);
}

// MMA unit test on Turing
TEST_F(NVFuserTest, FusionTuringMMANT_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // [K,M]
  auto tv0 = makeConcreteTensor({16, 16}, DataType::Half);
  // [K,N]
  auto tv1 = makeConcreteTensor({16, 8}, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [K,M,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {false, true, false});
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {0});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(16, 8, 16);
  gemm_tile.warp_tile = GemmTile(16, 8, 16);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Turing_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::NT);

  mma_builder.configureMma(tv2);

  auto tv0cw = tv0b->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1b->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());

  auto tv2c = tv2->cacheBefore();

  // [K,M,N] -> [N,M,K]
  tv0cr->reorder({{-3, -1}, {-1, -3}});
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  // [K,M,N] -> [M,N,K]
  tv1cr->reorder({
      {-3, -1},
      {-2, -3},
      {-1, -2},
  });
  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // [K,M,N] -> [M,N,K]
  tv2c->reorder({{-3, -1}, {-2, -3}, {-1, -2}});
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  tv0cw->setMemoryType(MemoryType::Shared);
  tv1cw->setMemoryType(MemoryType::Shared);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({16, 16}, options);
  auto t1 = at::randn({16, 8}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 5, fe.compileFusion(&fusion, {t0, t1}));

  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.t().to(at::kFloat).matmul(t1.to(at::kFloat));

  testValidate(&fusion, cg_outputs, {t0, t1}, {tref}, __LINE__, __FILE__);
}

// Matmul test on Turing
TEST_F(NVFuserTest, FusionTuringMatmulTN_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);
  int M = 511, N = 257, K = 88;

  // [M,K]
  auto tv0 = makeContigTensor(2, DataType::Half);
  // [N,K]
  auto tv1 = makeContigTensor(2, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,N,K]
  auto tv0b = broadcast(tv0, {false, true, false});
  auto tv1b = broadcast(tv1, {true, false, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {2});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(128, 128, 32);
  gemm_tile.warp_tile = GemmTile(64, 64, 32);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Turing_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::TN);

  mma_builder.configureMma(tv2);

  auto tv0r = tv0->cacheAfter();
  auto tv1r = tv1->cacheAfter();
  auto tv0cw = tv0r->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1r->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());
  auto tv2c = tv2->cacheBefore();

  // Make a CTA tile
  // ------------------------------------------------------------------
  // [M,N]
  tv2->split(-2, gemm_tile.cta_tile.m);
  tv2->split(-1, gemm_tile.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv2->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  tv0->computeAt(tv2, 2);
  tv1->computeAt(tv2, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv2c->split(-1, gemm_tile.cta_tile.k);
  tv2c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0r->computeAt(tv2c, 3);
  tv1r->computeAt(tv2c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(tv2c, gemm_tile);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv2, gemm_tile);
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv0cr->computeAt(tv2c, -4);
  tv1cr->computeAt(tv2c, -4);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,Ko,M,K]
  tv0cw->merge(-2);
  tv0r->merge(-2);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0r, gemm_tile, 8);
  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [No,Ko,N,K]
  tv1cw->merge(-2);
  tv1r->merge(-2);
  // [No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1r, gemm_tile, 8);
  tv1cw->setMemoryType(MemoryType::Shared);
  // Schedule mma input
  // ---------------------------------------------------------------------------
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  // [... Mi, Ni, Ki] want [Ni, Mi, Ki]
  tv0b->reorder({{-2, -3}, {-3, -2}});
  tv0b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());
  tv1b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  // Parallelize
  //  0   1  2  3    4   5  6  7  8  9  10
  // [Mo No Ko Mwo  Nwo Kw Mw Nw (Mi Ni Ki)]
  tv2c->axis(3)->parallelize(ParallelType::TIDz);
  tv2c->axis(4)->parallelize(ParallelType::TIDy);

  // Parallelize
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  tv2->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::BIDy);
  tv2->axis(2)->parallelize(ParallelType::TIDz);
  tv2->axis(3)->parallelize(ParallelType::TIDy);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({M, K}, options);
  auto t1 = at::randn({N, K}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 5, fe.compileFusion(&fusion, {t0, t1}));

  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.to(at::kFloat).matmul(t1.t().to(at::kFloat));

  TORCH_CHECK(cg_outputs[0].allclose(tref, 0.0001, 0.0001));
}

// Matmul test on Turing
TEST_F(NVFuserTest, FusionTuringMatmulTT_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);
  int M = 512, N = 256, K = 128;

  // [M,K]
  auto tv0 = makeContigTensor(2, DataType::Half);
  // [K,N]
  auto tv1 = makeContigTensor(2, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,K,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {true, false, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {1});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(128, 128, 32);
  gemm_tile.warp_tile = GemmTile(64, 64, 32);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Turing_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::TT);

  mma_builder.configureMma(tv2);

  auto tv0r = tv0->cacheAfter();
  auto tv1r = tv1->cacheAfter();
  auto tv0cw = tv0r->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1r->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());
  auto tv2c = tv2->cacheBefore();

  // Make a CTA tile
  // ------------------------------------------------------------------
  // [M,N]
  tv2->split(-2, gemm_tile.cta_tile.m);
  tv2->split(-1, gemm_tile.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv2->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  tv0->computeAt(tv2, 2);
  tv1->computeAt(tv2, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv2c->split(-1, gemm_tile.cta_tile.k);
  tv2c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0r->computeAt(tv2c, 3);
  tv1r->computeAt(tv2c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(tv2c, gemm_tile);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv2, gemm_tile);
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv0cr->computeAt(tv2c, -4);
  tv1cr->computeAt(tv2c, -4);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,Ko,M,K]
  tv0cw->merge(-2);
  tv0r->merge(-2);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0r, gemm_tile, 8);
  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [No,Ko,N,K] -> [No,Ko,K,N]
  tv1cw->reorder({{-2, -1}, {-1, -2}});
  tv1r->reorder({{-2, -1}, {-1, -2}});
  tv1cw->merge(-2);
  tv1r->merge(-2);
  // [No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1r, gemm_tile, 8);
  tv1cw->setMemoryType(MemoryType::Shared);
  // Schedule mma input
  // ---------------------------------------------------------------------------
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());
  // [... Mi, Ni, Ki] want [Ni, Mi, Ki]
  tv0b->reorder({{-2, -3}, {-3, -2}});
  tv0b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());
  tv1b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  // Parallelize
  //  0   1  2  3    4   5  6  7  8  9  10
  // [Mo No Ko Mwo  Nwo Kw Mw Nw (Mi Ni Ki)]
  tv2c->axis(3)->parallelize(ParallelType::TIDz);
  tv2c->axis(4)->parallelize(ParallelType::TIDy);

  // Parallelize
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  tv2->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::BIDy);
  tv2->axis(2)->parallelize(ParallelType::TIDz);
  tv2->axis(3)->parallelize(ParallelType::TIDy);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({M, K}, options);
  auto t1 = at::randn({K, N}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 5, fe.compileFusion(&fusion, {t0, t1}));

  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.to(at::kFloat).matmul(t1.to(at::kFloat));

  TORCH_CHECK(cg_outputs[0].allclose(tref, 0.0001, 0.0001));
}

// Matmul test on Turing
TEST_F(NVFuserTest, FusionTuringMatmulNT_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);
  int M = 512, N = 256, K = 128;

  // [K,M]
  auto tv0 = makeContigTensor(2, DataType::Half);
  // [K,N]
  auto tv1 = makeContigTensor(2, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [K,M,N]
  auto tv0b = broadcast(tv0, {false, false, true});
  auto tv1b = broadcast(tv1, {false, true, false});

  auto tv2 = fusedMultiplySum(tv0b, tv1b, {0});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(128, 128, 32);
  gemm_tile.warp_tile = GemmTile(64, 64, 32);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Turing_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::NT);

  mma_builder.configureMma(tv2);

  auto tv0r = tv0->cacheAfter();
  auto tv1r = tv1->cacheAfter();
  auto tv0cw = tv0r->cacheAfter();
  auto tv0cr =
      tv0cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::A).ldMatrix());
  auto tv1cw = tv1r->cacheAfter();
  auto tv1cr =
      tv1cw->cacheAfter(mma_builder.operand(MmaOptions::Operand::B).ldMatrix());
  auto tv2c = tv2->cacheBefore();

  // Make a CTA tile
  // ------------------------------------------------------------------
  // [M,N]
  tv2->split(-2, gemm_tile.cta_tile.m);
  tv2->split(-1, gemm_tile.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv2->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  tv0->computeAt(tv2, 2);
  tv1->computeAt(tv2, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv2c->split(-1, gemm_tile.cta_tile.k);
  tv2c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0r->computeAt(tv2c, 3);
  tv1r->computeAt(tv2c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(tv2c, gemm_tile);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv2, gemm_tile);
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv0cr->computeAt(tv2c, -4);
  tv1cr->computeAt(tv2c, -4);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,Ko,M,K] -> [..., K,M]
  tv0cw->reorder({{-2, -1}, {-1, -2}});
  tv0r->reorder({{-2, -1}, {-1, -2}});
  tv0cw->merge(-2);
  tv0r->merge(-2);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0r, gemm_tile, 8);
  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [No,Ko,N,K] -> [No,Ko,K,N]
  tv1cw->reorder({{-2, -1}, {-1, -2}});
  tv1r->reorder({{-2, -1}, {-1, -2}});
  tv1cw->merge(-2);
  tv1r->merge(-2);
  // [No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1cw, gemm_tile, 8);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1r, gemm_tile, 8);
  tv1cw->setMemoryType(MemoryType::Shared);
  // Schedule mma input
  // ---------------------------------------------------------------------------
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  // [... Mi, Ni, Ki] want [Ni, Mi, Ki]
  tv0b->reorder({{-2, -3}, {-3, -2}});
  tv0b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());
  tv1b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  // Parallelize
  //  0   1  2  3    4   5  6  7  8  9  10
  // [Mo No Ko Mwo  Nwo Kw Mw Nw (Mi Ni Ki)]
  tv2c->axis(3)->parallelize(ParallelType::TIDz);
  tv2c->axis(4)->parallelize(ParallelType::TIDy);

  // Parallelize
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  tv2->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::BIDy);
  tv2->axis(2)->parallelize(ParallelType::TIDz);
  tv2->axis(3)->parallelize(ParallelType::TIDy);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({K, M}, options);
  auto t1 = at::randn({K, N}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      7, 5, fe.compileFusion(&fusion, {t0, t1}));

  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.t().to(at::kFloat).matmul(t1.to(at::kFloat));

  TORCH_CHECK(cg_outputs[0].allclose(tref, 0.0001, 0.0001));
}

// Matmul test on ampere, using ampere memory ops
TEST_F(NVFuserTest, FusionAmpereMatmulTNcpAsync_CUDA) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  int M = 255, N = 511, K = 88;

  // [M,K]
  auto tv0 = makeContigTensor(2, DataType::Half);
  // [N,K]
  auto tv1 = makeContigTensor(2, DataType::Half);
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // [M,N,K]
  auto tv0b = broadcast(tv0, {false, true, false});
  auto tv1b = broadcast(tv1, {true, false, false});

  // Leaving both sets of mma inputs for volta outside
  //  currently since they need to be swizzled.
  auto tv2 = fusedMultiplySum(tv0b, tv1b, {2});

  fusion.addOutput(tv2);

  MatMulTileOptions gemm_tile;
  gemm_tile.cta_tile = GemmTile(128, 128, 32);
  gemm_tile.warp_tile = GemmTile(64, 64, 32);
  gemm_tile.instruction_tile = GemmTile(16, 8, 16);

  auto mma_builder =
      MmaBuilder(MmaOptions::MacroType::Ampere_16_8_16, gemm_tile)
          .layout(MmaOptions::MmaInputLayout::TN);

  mma_builder.configureMma(tv2);

  auto tv0cw = tv0->cacheAfter(LoadStoreOpType::CpAsync);
  auto tv0cr = tv0cw->cacheAfter(LoadStoreOpType::LdMatrix);
  auto tv1cw = tv1->cacheAfter(LoadStoreOpType::CpAsync);
  auto tv1cr = tv1cw->cacheAfter(LoadStoreOpType::LdMatrix);
  auto tv2c = tv2->cacheBefore();

  // Make a CTA tile
  // ------------------------------------------------------------------
  // [M,N]
  tv2->split(-2, gemm_tile.cta_tile.m);
  tv2->split(-1, gemm_tile.cta_tile.n);

  //  0   1    2   3
  // [Mo,M128, No, N128]
  tv2->reorder({{1, 2}, {2, 1}});

  //  0   1    2   3
  // [Mo,No, M128, N128]
  tv0->computeAt(tv2, 2);
  tv1->computeAt(tv2, 2);

  // Order K
  //  0   1    2   3     4    5
  // [Mo,No, M128, N128, Ko, K32]
  tv2c->split(-1, gemm_tile.cta_tile.k);
  tv2c->reorder({{2, 3}, {3, 4}, {4, 2}});

  //  0   1  2   3     4    5
  // [Mo,No, Ko M128, N128, K32]
  tv0cw->computeAt(tv2c, 3);
  tv1cw->computeAt(tv2c, 3);

  // Make warp tile:
  // -------------------------------------------------------------------------
  scheduler_utils::matmul_utils::scheduleWarpTileWithReduction(tv2c, gemm_tile);
  scheduler_utils::matmul_utils::scheduleWarpTileWithNoReduction(
      tv2, gemm_tile);
  //           -8   -7  -6 -5 -4 -3 -2 -1
  // [Mo No Ko Mwo  Nwo Kwo Mw Nw Mi Ni Ki]
  tv0cr->computeAt(tv2c, -4);
  tv1cr->computeAt(tv2c, -4);

  // Schedule gmem read and smem write:
  // ---------------------------------------------------------------------------
  // [Mo,Ko,M,K]
  tv0cw->merge(-2);
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv0cw, gemm_tile, 8);
  tv0cw->setMemoryType(MemoryType::Shared);
  // [Mo,Ko,i,wy,wx,v]

  // [No,Ko,N,K]
  tv1cw->merge(-2);
  // [No,Ko,i,wy,wx,v]
  scheduler_utils::matmul_utils::scheduleContiguousVectorLoad(
      tv1cw, gemm_tile, 8);
  tv1cw->setMemoryType(MemoryType::Shared);
  // Schedule mma input
  // ---------------------------------------------------------------------------
  tv0cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());
  // [... Mi, Ni, Ki]
  tv0b->reorder({{-2, -3}, {-3, -2}});
  tv0b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::A).build());

  tv1cr->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());
  tv1b->applyMmaSwizzle(mma_builder.operand(MmaOptions::Operand::B).build());

  // Schedule mma output
  // ---------------------------------------------------------------------------
  tv2c->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());
  tv2->applyMmaSwizzle(
      mma_builder.operand(MmaOptions::Operand::Accumulator).build());

  // Parallelize
  //  0   1  2  3    4   5  6  7  8  9  10
  // [Mo No Ko Mwo  Nwo Kw Mw Nw (Mi Ni Ki)]
  tv2c->axis(3)->parallelize(ParallelType::TIDz);
  tv2c->axis(4)->parallelize(ParallelType::TIDy);

  // Parallelize
  //  0  1  2   3   4   5  6  7
  // [Mo No Mwo Nwo Mw Nw (Mi Ni)]
  tv2->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::BIDy);
  tv2->axis(2)->parallelize(ParallelType::TIDz);
  tv2->axis(3)->parallelize(ParallelType::TIDy);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
  auto t0 = at::randn({M, K}, options);
  auto t1 = at::randn({N, K}, options);

  FusionExecutor fe;
  NVFUSER_TEST_CUDA_ARCH_COMPILE_CHECK(
      8, 0, fe.compileFusion(&fusion, {t0, t1}));

  auto cg_outputs = fe.runFusion({t0, t1});

  auto tref = t0.to(at::kFloat).matmul(t1.t().to(at::kFloat));

  TORCH_CHECK(cg_outputs[0].allclose(tref, 0.0001, 0.0001));
}

#undef NVFUSER_TEST_CUDA_ARCH_GUARD

} // namespace jit
} // namespace torch

#endif
