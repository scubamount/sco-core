#pragma once
// Shared by the src/vfs files; not public.
#include "sco/vfs.h"

namespace sco::vfs::detail {

// Compose's validation without building anything: each splice removes or adds something, sorted
// and non-overlapping, `old` absent or exactly `removed` bytes, at + removed <= baseSize.
Result CheckSplices(std::span<const Splice> splices, uint64_t baseSize);

// Sum of the splices' replacement bytes.
uint64_t AddedBytes(std::span<const Splice> splices);

}  // namespace sco::vfs::detail
