#include "gdpp/ir/mir.hpp"

#include <iomanip>
#include <sstream>
#include <string_view>

namespace gdpp {
namespace {

std::string_view function_role_name(const mir::FunctionRole role) {
    switch (role) {
    case mir::FunctionRole::method:
        return "method";
    case mir::FunctionRole::getter:
        return "getter";
    case mir::FunctionRole::setter:
        return "setter";
    case mir::FunctionRole::lambda:
        return "lambda";
    }
    return "invalid";
}

std::string_view instruction_kind_name(const mir::InstructionKind kind) {
    switch (kind) {
    case mir::InstructionKind::evaluate:
        return "evaluate";
    case mir::InstructionKind::declare_variable:
        return "declare_variable";
    case mir::InstructionKind::assign:
        return "assign";
    case mir::InstructionKind::assert_condition:
        return "assert_condition";
    case mir::InstructionKind::debug_breakpoint:
        return "debug_breakpoint";
    case mir::InstructionKind::loop_test:
        return "loop_test";
    case mir::InstructionKind::match_test:
        return "match_test";
    case mir::InstructionKind::suspend_value:
        return "suspend_value";
    }
    return "invalid";
}

std::string_view terminator_kind_name(const mir::TerminatorKind kind) {
    switch (kind) {
    case mir::TerminatorKind::invalid:
        return "invalid";
    case mir::TerminatorKind::jump:
        return "jump";
    case mir::TerminatorKind::branch:
        return "branch";
    case mir::TerminatorKind::return_value:
        return "return";
    case mir::TerminatorKind::stop:
        return "stop";
    case mir::TerminatorKind::suspend:
        return "suspend";
    }
    return "invalid";
}

std::string_view branch_role_name(const mir::BranchRole role) {
    switch (role) {
    case mir::BranchRole::none:
        return "none";
    case mir::BranchRole::condition:
        return "condition";
    case mir::BranchRole::iterator_protocol:
        return "iterator_protocol";
    case mir::BranchRole::match_pattern:
        return "match_pattern";
    case mir::BranchRole::match_guard:
        return "match_guard";
    case mir::BranchRole::assertion:
        return "assertion";
    }
    return "invalid";
}

std::string_view expression_kind_name(const ir::ExpressionKind kind) {
    switch (kind) {
    case ir::ExpressionKind::literal:
        return "literal";
    case ir::ExpressionKind::identifier:
        return "identifier";
    case ir::ExpressionKind::unary:
        return "unary";
    case ir::ExpressionKind::await_expression:
        return "await";
    case ir::ExpressionKind::binary:
        return "binary";
    case ir::ExpressionKind::call:
        return "call";
    case ir::ExpressionKind::member:
        return "member";
    case ir::ExpressionKind::subscript:
        return "subscript";
    case ir::ExpressionKind::conditional:
        return "conditional";
    case ir::ExpressionKind::node_reference:
        return "node_reference";
    case ir::ExpressionKind::array_literal:
        return "array";
    case ir::ExpressionKind::dictionary_literal:
        return "dictionary";
    case ir::ExpressionKind::lambda:
        return "lambda";
    }
    return "invalid";
}

std::string_view literal_kind_name(const ir::LiteralKind kind) {
    switch (kind) {
    case ir::LiteralKind::none:
        return "none";
    case ir::LiteralKind::nil:
        return "nil";
    case ir::LiteralKind::boolean:
        return "boolean";
    case ir::LiteralKind::integer:
        return "integer";
    case ir::LiteralKind::floating:
        return "floating";
    case ir::LiteralKind::string:
        return "string";
    case ir::LiteralKind::string_name:
        return "string_name";
    case ir::LiteralKind::node_path:
        return "node_path";
    }
    return "invalid";
}

std::string_view resolution_kind_name(const ir::ResolutionKind kind) {
    switch (kind) {
    case ir::ResolutionKind::none:
        return "none";
    case ir::ResolutionKind::godot_method:
        return "godot_method";
    case ir::ResolutionKind::godot_property:
        return "godot_property";
    case ir::ResolutionKind::godot_constructor:
        return "godot_constructor";
    case ir::ResolutionKind::godot_singleton:
        return "godot_singleton";
    case ir::ResolutionKind::external_singleton:
        return "external_singleton";
    case ir::ResolutionKind::godot_type:
        return "godot_type";
    case ir::ResolutionKind::external_type:
        return "external_type";
    case ir::ResolutionKind::script_type:
        return "script_type";
    case ir::ResolutionKind::script_autoload:
        return "script_autoload";
    case ir::ResolutionKind::script_constant:
        return "script_constant";
    case ir::ResolutionKind::local_constant:
        return "local_constant";
    case ir::ResolutionKind::script_enum_type:
        return "script_enum_type";
    case ir::ResolutionKind::script_resource:
        return "script_resource";
    case ir::ResolutionKind::script_constructor:
        return "script_constructor";
    case ir::ResolutionKind::external_constructor:
        return "external_constructor";
    case ir::ResolutionKind::external_static_method:
        return "external_static_method";
    case ir::ResolutionKind::external_super_method:
        return "external_super_method";
    case ir::ResolutionKind::external_callable:
        return "external_callable";
    case ir::ResolutionKind::external_signal:
        return "external_signal";
    case ir::ResolutionKind::inner_constructor:
        return "inner_constructor";
    case ir::ResolutionKind::inner_type:
        return "inner_type";
    case ir::ResolutionKind::script_super:
        return "script_super";
    case ir::ResolutionKind::script_signal:
        return "script_signal";
    case ir::ResolutionKind::script_callable:
        return "script_callable";
    case ir::ResolutionKind::script_static_callable:
        return "script_static_callable";
    case ir::ResolutionKind::script_static_field:
        return "script_static_field";
    case ir::ResolutionKind::script_runtime_static_field:
        return "script_runtime_static_field";
    case ir::ResolutionKind::script_free:
        return "script_free";
    case ir::ResolutionKind::enum_member:
        return "enum_member";
    case ir::ResolutionKind::script_property:
        return "script_property";
    case ir::ResolutionKind::dynamic_method:
        return "dynamic_method";
    case ir::ResolutionKind::dynamic_property:
        return "dynamic_property";
    case ir::ResolutionKind::utility_function:
        return "utility_function";
    case ir::ResolutionKind::global_constant:
        return "global_constant";
    case ir::ResolutionKind::global_enum_type:
        return "global_enum_type";
    case ir::ResolutionKind::global_enum_value:
        return "global_enum_value";
    case ir::ResolutionKind::builtin_constant:
        return "builtin_constant";
    case ir::ResolutionKind::intrinsic:
        return "intrinsic";
    }
    return "invalid";
}

std::string_view type_kind_name(const TypeKind kind) {
    switch (kind) {
    case TypeKind::unknown:
        return "unknown";
    case TypeKind::variant:
        return "variant";
    case TypeKind::nil:
        return "nil";
    case TypeKind::boolean:
        return "boolean";
    case TypeKind::integer:
        return "integer";
    case TypeKind::floating:
        return "floating";
    case TypeKind::string:
        return "string";
    case TypeKind::string_name:
        return "string_name";
    case TypeKind::array:
        return "array";
    case TypeKind::dictionary:
        return "dictionary";
    case TypeKind::enumeration:
        return "enumeration";
    case TypeKind::script_resource:
        return "script_resource";
    case TypeKind::builtin:
        return "builtin";
    case TypeKind::object:
        return "object";
    case TypeKind::void_type:
        return "void";
    }
    return "invalid";
}

std::string_view intrinsic_name(const IntrinsicKind kind) {
    if (kind == IntrinsicKind::none)
        return "none";
    const auto* feature = IntrinsicRegistry::latest().find(kind);
    return feature ? feature->name : "invalid";
}

std::string_view ownership_name(const OwnershipKind kind) {
    switch (kind) {
    case OwnershipKind::value:
        return "value";
    case OwnershipKind::shared_container:
        return "shared_container";
    case OwnershipKind::object_reference:
        return "object_reference";
    case OwnershipKind::dynamic:
        return "dynamic";
    }
    return "invalid";
}

void write_span(std::ostream& output, const SourceSpan& span) {
    output << span.begin.offset << ':' << span.begin.line << ':' << span.begin.column << '-'
           << span.end.offset << ':' << span.end.line << ':' << span.end.column;
}

template <typename Id>
void write_ids(std::ostream& output, const std::vector<Id>& values, const char prefix) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0)
            output << ',';
        output << prefix << values[index];
    }
    output << ']';
}

} // namespace

std::string MirSerializer::serialize(const mir::Module& module) const {
    std::ostringstream output;
    output << "GDPP_MIR " << module.format_version << '\n';
    output << "functions " << module.functions.size() << '\n';
    for (const auto& function : module.functions) {
        output << "function f" << function.id << " role " << function_role_name(function.role)
               << " name " << std::quoted(function.name) << " entry b" << function.entry
               << " suspends " << (function.suspends ? 1 : 0) << " span ";
        write_span(output, function.span);
        output << '\n';
        output << "values " << function.values.size() << '\n';
        for (const auto& value : function.values) {
            output << "  value v" << value.id << " kind " << expression_kind_name(value.kind)
                   << " literal " << literal_kind_name(value.literal_kind) << " type "
                   << type_kind_name(value.type.kind) << ':'
                   << std::quoted(value.type.display_name()) << " storage "
                   << type_kind_name(value.storage_type.kind) << ':'
                   << std::quoted(value.storage_type.display_name()) << " assignment "
                   << type_kind_name(value.assignment_type.kind) << ':'
                   << std::quoted(value.assignment_type.display_name()) << " ownership "
                   << ownership_name(value.ownership) << " non_null " << (value.non_null ? 1 : 0)
                   << " resolution " << resolution_kind_name(value.resolution) << " intrinsic "
                   << intrinsic_name(value.intrinsic) << " symbol " << value.symbol_identity
                   << " coroutine " << (value.coroutine_call ? 1 : 0) << " direct "
                   << (value.direct_access ? 1 : 0) << " argument " << value.indexed_argument
                   << " payload " << std::quoted(value.payload) << " owner "
                   << std::quoted(value.resolved_owner) << " getter " << std::quoted(value.getter)
                   << " setter " << std::quoted(value.setter) << " operands ";
            write_ids(output, value.operands, 'v');
            output << " span ";
            write_span(output, value.span);
            if (value.call_contract) {
                output << " call required " << value.call_contract->required_arguments << " vararg "
                       << (value.call_contract->is_vararg ? 1 : 0) << " parameters [";
                for (std::size_t index = 0; index < value.call_contract->parameters.size();
                     ++index) {
                    if (index != 0)
                        output << ',';
                    const auto& parameter = value.call_contract->parameters[index];
                    output << type_kind_name(parameter.kind) << ':'
                           << std::quoted(parameter.display_name());
                }
                output << ']';
            } else {
                output << " call none";
            }
            output << '\n';
        }
        output << "blocks " << function.blocks.size() << '\n';
        for (const auto& block : function.blocks) {
            output << "  block b" << block.id << " predecessors ";
            write_ids(output, block.predecessors, 'b');
            output << '\n';
            for (const auto& instruction : block.instructions) {
                output << "    operation o" << instruction.id << " instruction "
                       << instruction_kind_name(instruction.kind) << " effects "
                       << static_cast<unsigned>(instruction.effects) << " inputs ";
                write_ids(output, instruction.inputs, 'v');
                output << " span ";
                write_span(output, instruction.span);
                output << '\n';
            }
            const auto& terminator = block.terminator;
            output << "    operation o" << terminator.id << " terminator "
                   << terminator_kind_name(terminator.kind) << " role "
                   << branch_role_name(terminator.branch_role) << " condition ";
            if (terminator.condition_value == mir::invalid_value)
                output << "none";
            else
                output << 'v' << terminator.condition_value;
            output << " targets ";
            write_ids(output, terminator.targets, 'b');
            output << " span ";
            write_span(output, terminator.span);
            output << '\n';
        }
        output << "end_function\n";
    }
    return output.str();
}

} // namespace gdpp
