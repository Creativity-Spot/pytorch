#pragma once

#include <c10/macros/Export.h>

namespace c10 {
struct TensorImpl;
}

namespace at {
class TensorBase;

// MemOverlap: Whether or not there is memory overlap
//
// NO: Absolutely no memory overlap
// YES: Absolutely yes memory overlap
// TOO_HARD: There might be memory overlap, but it was too expensive to compute.
//
// NB: Please update the python test for these if you renumber them.
enum class MemOverlap { NO, YES, TOO_HARD };

enum class MemOverlapStatus { FULL, PARTIAL, NO, TOO_HARD };

TORCH_API MemOverlap has_internal_overlap(const TensorBase& t);
TORCH_API MemOverlap has_internal_overlap(c10::TensorImpl* t);

TORCH_API void assert_no_internal_overlap(const TensorBase& t);
TORCH_API void assert_no_internal_overlap(c10::TensorImpl* t);

TORCH_API MemOverlapStatus
get_overlap_status(const TensorBase& a, const TensorBase& b);
TORCH_API MemOverlapStatus
get_overlap_status(c10::TensorImpl* a, c10::TensorImpl* b);

TORCH_API void assert_no_partial_overlap(
    const TensorBase& a,
    const TensorBase& b);
void assert_no_partial_overlap(c10::TensorImpl* a, c10::TensorImpl* b);

TORCH_API void assert_no_overlap(const TensorBase& a, const TensorBase& b);
TORCH_API void assert_no_overlap(c10::TensorImpl* a, c10::TensorImpl* b);

} // namespace at
