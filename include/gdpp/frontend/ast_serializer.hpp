#pragma once

#include "gdpp/frontend/ast.hpp"

#include <string>

namespace gdpp {

// Stable, versioned and address-free AST representation used by compatibility gates and
// diagnostics. Every AST field and SourceSpan is serialized so parser changes cannot silently
// alter source ownership or recovery boundaries.
class AstSerializer final {
  public:
    [[nodiscard]] std::string serialize(const ast::Script& script) const;
};

} // namespace gdpp
