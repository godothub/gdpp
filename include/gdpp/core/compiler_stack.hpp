#pragma once

#include <cstddef>
#include <functional>

namespace gdpp {

// Godot creates script Threads with platform-default stacks (512 KiB on macOS). The compiler
// accepts parser nesting up to 128 levels and therefore owns a larger, deterministic worker stack
// instead of inheriting an embedding application's accidental thread limit.
inline constexpr std::size_t compiler_worker_stack_bytes = 16U * 1024U * 1024U;

void run_on_compiler_stack(const std::function<void()>& task);

} // namespace gdpp
