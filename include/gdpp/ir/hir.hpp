#pragma once

#include "gdpp/ir/typed_program.hpp"

// HIR is the compiler frontend's view of the shared typed program model. MIR takes ownership of
// the same model at the lowering boundary, so neither representation needs to retain pointers to
// the other's storage.
namespace gdpp {
namespace ir = typed;
}
