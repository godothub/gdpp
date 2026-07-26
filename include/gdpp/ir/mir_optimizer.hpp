#pragma once

#include "gdpp/ir/mir.hpp"

#include <cstddef>
#include <limits>

namespace gdpp {

struct MirOptimizationBudget {
    std::size_t max_functions{4096};
    std::size_t max_blocks{1U << 20U};
    std::size_t max_operations{1U << 22U};
};

struct MirOptimizationStats {
    std::size_t branches_simplified{0};
    std::size_t blocks_removed{0};
    std::size_t instructions_removed{0};
    std::size_t functions_visited{0};
    std::size_t blocks_before{0};
    std::size_t blocks_after{0};
    std::size_t operations_before{0};
    std::size_t operations_after{0};
    bool precondition_verified{false};
    bool postcondition_verified{false};
    bool budget_exhausted{false};
};

// Performs semantics-preserving CFG transforms after MIR verification. Every transform must
// rebuild dense block IDs and predecessor metadata so the result remains a valid codegen input.
// The diagnostic overload is transactional: invalid input is left untouched, candidate functions
// are committed only after post-verification, and an exceeded optimization budget safely preserves
// the verified unoptimized module.
class MirOptimizer final {
  public:
    [[nodiscard]] MirOptimizationStats optimize(mir::Module& module) const;
    [[nodiscard]] MirOptimizationStats optimize(mir::Module& module, DiagnosticBag& diagnostics,
                                                const MirOptimizationBudget& budget = {}) const;
};

} // namespace gdpp
