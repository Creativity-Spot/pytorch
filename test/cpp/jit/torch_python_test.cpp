#include <ATen/core/ivalue.h>
#include <c10/util/Exception.h>
#include <torch/csrc/Export.h>
#include <torch/csrc/jit/api/module.h>
#include <torch/script.h>

namespace torch {
namespace jit {

#ifdef _MSC_VER
#define JIT_TEST_API
#else
#define JIT_TEST_API TORCH_API
#endif

namespace {

bool isSandcastle() {
  return (
      (std::getenv("SANDCASTLE")) ||
      (std::getenv("TW_JOB_USER") &&
       std::string(std::getenv("TW_JOB_USER")) == "sandcastle"));
}

void testEvalModeForLoadedModule() {
  if (isSandcastle())
    return; // The module file to load is not generated in Sandcastle
  std::string module_path = "dropout_model.pt";
  torch::jit::Module module = torch::jit::load(module_path);
  AT_ASSERT(module.attr("dropout").toModule().is_training());
  module.eval();
  AT_ASSERT(!module.attr("dropout").toModule().is_training());
  module.train();
  AT_ASSERT(module.attr("dropout").toModule().is_training());
}

void testSerializationInterop() {
  if (isSandcastle()) {
    // The module file to load is not generated in Sandcastle
    return;
  }

  // This should be generated by `test/cpp/jit/tests_setup.py`
  std::ifstream input_stream("ivalue.pt");
  std::vector<char> input;
  input.insert(
      input.begin(),
      std::istream_iterator<char>(input_stream),
      std::istream_iterator<char>());
  IValue ivalue = pickle_load(input);

  auto elements = ivalue.toTupleRef().elements();
  auto ones = torch::ones({2, 2});
  AT_ASSERT(ones.equal(elements.at(0).toTensor()));

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
  auto twos = torch::ones({3, 5}) * 2;
  AT_ASSERT(twos.equal(elements.at(1).toTensor()));
}

void testTorchSaveError() {
  if (isSandcastle()) {
    // The file to load is not generated in Sandcastle
    return;
  }

  // This should be generated by `test/cpp/jit/tests_setup.py`
  bool passed = true;
  try {
    torch::jit::load("eager_value.pt");
    passed = false;
  } catch (const std::exception& c) {
  }
  // Ensure torch::jit::load did not run
  AT_ASSERT(passed);
}
} // namespace

JIT_TEST_API void runJITCPPTests() {
  // TODO: this test never ran before and is broken.
  // testSerializationInterop();
  testEvalModeForLoadedModule();
  testTorchSaveError();
}
} // namespace jit
} // namespace torch
