#include <ATen/native/SegmentReduce.h>

#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/NumericUtils.h>
#include <c10/util/irange.h>

namespace at {
namespace native {

DEFINE_DISPATCH(_segment_reduce_stub);
DEFINE_DISPATCH(_segment_reduce_backward_stub);

namespace {

SegmentReductionType get_reduction_enum(const c10::string_view& reduce) {
  if (reduce == "max") {
    return SegmentReductionType::MAX;
  } else if (reduce == "mean") {
    return SegmentReductionType::MEAN;
  } else if (reduce == "min") {
    return SegmentReductionType::MIN;
  } else if (reduce == "sum") {
    return SegmentReductionType::SUM;
  } else if (reduce == "prod") {
    return SegmentReductionType::PROD;
  } else {
    TORCH_CHECK(false, "unsupported reduction given! ", reduce);
  }
}

template <typename T>
void _segment_reduce_cpu_kernel1(
    SegmentReductionType reduction,
    const Tensor& data,
    const T* lengths_data,
    int64_t axis,
    const c10::optional<Scalar>& initial,
    Tensor& output,
    int64_t segment_count,
    int64_t lengths_stride_axis) {
  // outer_offset is the size of the outer dimensions of output (before axis)
  // inner_offset is the size of the inner dimensions of output (after axis)
  int64_t outer_offset = 1, inner_offset = 1;
  for (int64_t d = 0; d < axis; d++)
      outer_offset *= output.size(d);
  for (int64_t d = axis + 1; d < output.dim(); d++)
      inner_offset *= output.size(d);
  AT_DISPATCH_FLOATING_TYPES_AND2(
      kBFloat16, kHalf, data.scalar_type(), "_segment_reduce_cpu", [&]() {
        auto* output_data = output.data_ptr<scalar_t>();
        const auto* values_data = data.data_ptr<scalar_t>();
        for (const auto outer_idx : c10::irange(outer_offset)) {
          int64_t lengths_cum_sum = 0;
          for (const auto dim_idx : c10::irange(segment_count)) {
            int64_t segment_length = lengths_data[outer_idx * lengths_stride_axis * segment_count + dim_idx];
            for (const auto inner_idx : c10::irange(inner_offset)) {
              // ===== step1: initialize starting value
              scalar_t initial_value;
              if (initial.has_value()) {
                initial_value = initial.value().to<scalar_t>();
              } else if (reduction == SegmentReductionType::MAX) {
                initial_value = -std::numeric_limits<scalar_t>::infinity();
              } else if (
                  reduction == SegmentReductionType::MEAN ||
                  reduction == SegmentReductionType::SUM) {
                initial_value = 0;
              } else if (reduction == SegmentReductionType::MIN) {
                initial_value = std::numeric_limits<scalar_t>::infinity();
              } else if (reduction == SegmentReductionType::PROD) {
                initial_value = 1;
              }

              // ===== step2: apply reduction
              for (const auto j : c10::irange(segment_length)) {
                int64_t data_index = outer_idx * data.stride(axis) * data.size(axis)
                                     + (lengths_cum_sum + j) * data.stride(axis) + inner_idx;
                const auto val = values_data[data_index];
                if (reduction == SegmentReductionType::MAX) {
                  initial_value = at::_isnan(val)
                      ? val
                      : std::max<scalar_t>(initial_value, val);
                } else if (
                    reduction == SegmentReductionType::MEAN ||
                    reduction == SegmentReductionType::SUM) {
                  initial_value = initial_value + val;
                } else if (reduction == SegmentReductionType::MIN) {
                  initial_value = at::_isnan(val)
                      ? val
                      : std::min<scalar_t>(initial_value, val);
                } else if (reduction == SegmentReductionType::PROD) {
                  initial_value = initial_value * val;
                }
              }

              // ===== step3: finalize reduction
              TORCH_CHECK(segment_length >= 0);

              if (segment_length == 0 && !initial.has_value() &&
                  reduction == SegmentReductionType::MEAN) {
                initial_value = static_cast<scalar_t>(NAN);
              } else if (
                  reduction == SegmentReductionType::MEAN &&
                  segment_length > 0 && !at::_isnan(initial_value)) {
                initial_value = initial_value / segment_length;
              }
              int64_t output_index = outer_idx * output.stride(axis) * output.size(axis)
                                     + dim_idx * output.stride(axis) + inner_idx;
              output_data[output_index] = initial_value;
            }
            lengths_cum_sum += segment_length;
          }
        }
      });
}

Tensor _segment_reduce_cpu_kernel(
    SegmentReductionType reduction,
    const Tensor& data,
    const Tensor& lengths,
    int64_t axis,
    const c10::optional<Scalar>& initial) {
  // data and lengths should be contiguous from the call to .contiguous in segment_reduce_kernel
  TORCH_CHECK(data.is_contiguous(), "Expected data to be contiguous.");
  TORCH_CHECK(lengths.is_contiguous(), "Expected lengths to be contiguous.");
  // reduction axis should always be the last dimension of lengths
  axis = lengths.dim() - 1;
  int64_t segment_count = lengths.size(axis);
  int64_t lengths_stride_axis = lengths.stride(axis);
  auto output_shape = data.sizes().vec();
  output_shape[axis] = segment_count;
  auto output = at::empty(output_shape, data.options());

  AT_DISPATCH_INDEX_TYPES(lengths.scalar_type(), "_segment_reduce_cpu_kernel1", [&]() {
    const auto* lengths_data = lengths.data_ptr<index_t>();
    _segment_reduce_cpu_kernel1(
        reduction, data, lengths_data, axis, initial, output, segment_count, lengths_stride_axis);
  });

  return output;
}

template <typename T>
void _segment_reduce_cpu_backward_kernel1(
    const Tensor& grad_contig,
    const Tensor& output_contig,
    const Tensor& data_contig,
    SegmentReductionType reduction,
    const T* lengths_data,
    int64_t axis,
    const c10::optional<Scalar>& initial,
    Tensor& grad_input,
    int64_t segment_count,
    int64_t lengths_stride_axis) {
  // outer_offset is the size of the outer dimensions of output (before axis)
  // inner_offset is the size of the inner dimensions of output (after axis)
  int64_t outer_offset = 1, inner_offset = 1;
  for (int64_t d = 0; d < axis; d++)
      outer_offset *= output_contig.size(d);
  for (int64_t d = axis + 1; d < output_contig.dim(); d++)
      inner_offset *= output_contig.size(d);
  // TODO: Swtich to TensorIterator for better maintainablility and
  // readability
  AT_DISPATCH_FLOATING_TYPES_AND2(
      kBFloat16,
      kHalf,
      data_contig.scalar_type(),
      "_segment_reduce_cpu",
      [&]() {
        auto* output_data = output_contig.data_ptr<scalar_t>();
        auto* grad_data = grad_contig.data_ptr<scalar_t>();
        auto* grad_input_data = grad_input.data_ptr<scalar_t>();
        const auto* values_data = data_contig.data_ptr<scalar_t>();
        // Used to calculate exclusive prod
        scalar_t initial_prod_value;
        if (reduction == SegmentReductionType::PROD) {
          if (initial.has_value()) {
            initial_prod_value = initial.value().to<scalar_t>();
          } else {
            initial_prod_value = 1;
          }
        }

        for (const auto outer_idx : c10::irange(outer_offset)) {
          int64_t lengths_cum_sum = 0;
          for (const auto dim_idx : c10::irange(segment_count)) {
            int64_t segment_length = lengths_data[outer_idx * lengths_stride_axis * segment_count + dim_idx];
            if (segment_length == 0) {
              continue;
            }
            for (const auto inner_idx : c10::irange(inner_offset)) {
              int64_t output_index = outer_idx * output_contig.stride(axis) * output_contig.size(axis)
                                     + dim_idx * output_contig.stride(axis) + inner_idx;
              if (reduction == SegmentReductionType::MAX ||
                  reduction == SegmentReductionType::MIN) {
                int64_t counter = 0;
                for (const auto j : c10::irange(segment_length)) {
                  int64_t data_index = outer_idx * data_contig.stride(axis) * data_contig.size(axis)
                                       + (lengths_cum_sum + j) * data_contig.stride(axis) + inner_idx;
                  if (at::_isnan(values_data[data_index]) ||
                      values_data[data_index] == output_data[output_index]) {
                    grad_input_data[data_index] = grad_data[output_index];
                    counter++;
                  }
                }
                // Average gradient based on number of maximum elements in
                // the segment
                if (counter < 2) {
                  continue;
                }
                for (const auto j : c10::irange(segment_length)) {
                  int64_t data_index = outer_idx * data_contig.stride(axis) * data_contig.size(axis)
                                       + (lengths_cum_sum + j) * data_contig.stride(axis) + inner_idx;
                  if (grad_input_data[data_index] > 0) {
                    grad_input_data[data_index] =
                        grad_input_data[data_index] / counter;
                  }
                }
              } else if (reduction == SegmentReductionType::MEAN) {
                auto grad_val = grad_data[output_index] / segment_length;
                for (const auto j : c10::irange(segment_length)) {
                  int64_t data_index = outer_idx * data_contig.stride(axis) * data_contig.size(axis)
                                       + (lengths_cum_sum + j) * data_contig.stride(axis) + inner_idx;
                  grad_input_data[data_index] = grad_val;
                }
              } else if (reduction == SegmentReductionType::SUM) {
                const auto& grad_val = grad_data[output_index];
                for (const auto j : c10::irange(segment_length)) {
                  int64_t data_index = outer_idx * data_contig.stride(axis) * data_contig.size(axis)
                                       + (lengths_cum_sum + j) * data_contig.stride(axis) + inner_idx;
                  grad_input_data[data_index] = grad_val;
                }
              } else if (reduction == SegmentReductionType::PROD) {
                const auto& grad_val = grad_data[output_index] * output_data[output_index];
                for (const auto j : c10::irange(segment_length)) {
                  int64_t data_index = outer_idx * data_contig.stride(axis) * data_contig.size(axis)
                                       + (lengths_cum_sum + j) * data_contig.stride(axis) + inner_idx;
                  if (at::_isnan(values_data[data_index]) ||
                      values_data[data_index] == 0) {
                    // explicitly compute exclusive prod
                    scalar_t exclusive_prod = initial_prod_value;
                    int64_t idx;
                    for (const auto k : c10::irange(segment_length)) {
                      if (k != j) {
                        idx = outer_idx * data_contig.stride(axis) * data_contig.size(axis)
                              + (lengths_cum_sum + k) * data_contig.stride(axis) + inner_idx;
                        exclusive_prod *= values_data[idx];
                      }
                    }
                    grad_input_data[data_index] = grad_data[output_index] * exclusive_prod;
                  } else {
                    grad_input_data[data_index] = grad_val / values_data[data_index];
                  }
                }
              }
            }
            lengths_cum_sum += segment_length;
          }
        }
      });
}

Tensor _segment_reduce_cpu_backward_kernel(
    const Tensor& grad_contig,
    const Tensor& output_contig,
    const Tensor& data_contig,
    SegmentReductionType reduction,
    const Tensor& lengths_contig,
    int64_t axis,
    const c10::optional<Scalar>& initial) {
  axis = lengths_contig.dim() - 1;
  int64_t segment_count = lengths_contig.size(axis);
  int64_t lengths_stride_axis = lengths_contig.stride(axis);
  auto grad_input = at::zeros({data_contig.sizes()}, grad_contig.options());

  AT_DISPATCH_INDEX_TYPES(
      lengths_contig.scalar_type(), "_segment_reduce_cpu_backward_kernel1", [&] {
        const auto* lengths_data = lengths_contig.data_ptr<index_t>();
        _segment_reduce_cpu_backward_kernel1(
            grad_contig,
            output_contig,
            data_contig,
            reduction,
            lengths_data,
            axis,
            initial,
            grad_input,
            segment_count,
            lengths_stride_axis);
      });

  return grad_input;
}

} // namespace

Tensor segment_reduce_kernel(
    const Tensor& data,
    c10::string_view reduce,
    const c10::optional<Tensor>& lengths,
    const c10::optional<Tensor>& indices,
    int64_t axis,
    bool unsafe,
    const c10::optional<Scalar>& initial) {
  axis = maybe_wrap_dim(axis, data.ndimension());
  TORCH_CHECK(data.numel() > 0);

  // length related checks
  TORCH_CHECK(
      lengths.has_value() && !indices.has_value(),
      "Currently only lengths based reduction is supported!")
  const auto& lengths_value = lengths.value();
  TORCH_CHECK(data.get_device() == lengths_value.get_device());
  TORCH_CHECK(data.dim() >= lengths_value.dim());
  TORCH_CHECK(axis == lengths_value.dim() - 1, "Expected axis to be equal to lengths.ndim() - 1 but got ", axis, ".");

  if (!unsafe) {
    auto min_length = lengths_value.min().item<int64_t>();
    TORCH_CHECK((min_length >= 0), "lengths contains negative value!");
    TORCH_CHECK(all(lengths_value.sum({-1}) == data.size(axis)).item<bool>(),
                "Expected all rows of lengths to sum to data.size(lengths.dim()-1) when unsafe=False");
  }

  auto reduction = get_reduction_enum(reduce);
  const auto data_contig = data.contiguous();
  const auto lengths_contig = lengths_value.contiguous();

  return _segment_reduce_stub(
      data_contig.device().type(),
      reduction,
      data_contig,
      lengths_contig,
      axis,
      initial);
}

REGISTER_ARCH_DISPATCH(
    _segment_reduce_stub,
    DEFAULT,
    &_segment_reduce_cpu_kernel);
REGISTER_AVX2_DISPATCH(_segment_reduce_stub, &_segment_reduce_cpu_kernel);
REGISTER_AVX512_DISPATCH(_segment_reduce_stub, &_segment_reduce_cpu_kernel);
REGISTER_VSX_DISPATCH(_segment_reduce_stub, &_segment_reduce_cpu_kernel);
REGISTER_ZVECTOR_DISPATCH(_segment_reduce_stub, &_segment_reduce_cpu_kernel);

// Currently some computation is being duplicated across forward and backward.
// TODO: Cache indices in forward pass to re-use in backward
Tensor _segment_reduce_backward_kernel(
    const Tensor& grad,
    const Tensor& output,
    const Tensor& data,
    c10::string_view reduce,
    const c10::optional<Tensor>& lengths,
    int64_t axis,
    const c10::optional<Scalar>& initial) {
  axis = maybe_wrap_dim(axis, data.ndimension());
  TORCH_CHECK(
      lengths.has_value(),
      "Currently only lengths based reduction is supported!")
  const auto& lengths_value = lengths.value();

  const auto grad_contig = grad.contiguous();
  const auto output_contig = output.contiguous();
  const auto data_contig = data.contiguous();
  const auto lengths_contig = lengths_value.contiguous();

  auto reduction = get_reduction_enum(reduce);
  return _segment_reduce_backward_stub(
      grad_contig.device().type(),
      grad_contig,
      output_contig,
      data_contig,
      reduction,
      lengths_contig,
      axis,
      initial);
}

REGISTER_ARCH_DISPATCH(
    _segment_reduce_backward_stub,
    DEFAULT,
    &_segment_reduce_cpu_backward_kernel);
REGISTER_AVX512_DISPATCH(
    _segment_reduce_backward_stub,
    &_segment_reduce_cpu_backward_kernel);
REGISTER_AVX2_DISPATCH(
    _segment_reduce_backward_stub,
    &_segment_reduce_cpu_backward_kernel);
REGISTER_VSX_DISPATCH(
    _segment_reduce_backward_stub,
    &_segment_reduce_cpu_backward_kernel);
REGISTER_ZVECTOR_DISPATCH(
    _segment_reduce_backward_stub,
    &_segment_reduce_cpu_backward_kernel);

} // namespace native
} // namespace at
