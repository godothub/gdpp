#include "gdpp/semantic/analyzer.hpp"

#include "gdpp/frontend/constant_evaluator.hpp"
#include "gdpp/semantic/conversion.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace gdpp {
namespace {

const Type unknown_type{TypeKind::unknown, "unknown"};
const Type variant_type{TypeKind::variant, "Variant"};
const Type void_type{TypeKind::void_type, "void"};

class WarningIgnoreScope final {
  public:
    WarningIgnoreScope(std::unordered_set<std::string>& active,
                       const std::vector<ast::Annotation>& annotations)
        : active_(active), previous_(active) {
        for (const auto& annotation : annotations) {
            if (annotation.name != "warning_ignore")
                continue;
            for (const auto& argument : annotation.arguments) {
                if (argument->kind() == ast::ExpressionKind::literal &&
                    argument->literal_kind() == ast::LiteralKind::string) {
                    active_.insert(argument->value());
                }
            }
        }
    }

    ~WarningIgnoreScope() { active_ = std::move(previous_); }
    WarningIgnoreScope(const WarningIgnoreScope&) = delete;
    WarningIgnoreScope& operator=(const WarningIgnoreScope&) = delete;

  private:
    std::unordered_set<std::string>& active_;
    std::unordered_set<std::string> previous_;
};

bool warning_is_ignored(const std::unordered_set<std::string>& active,
                        const std::string_view name) {
    return active.find(std::string{name}) != active.end();
}

bool annotations_ignore_warning(const std::vector<ast::Annotation>& annotations,
                                const std::string_view name) {
    return std::any_of(annotations.begin(), annotations.end(), [&](const auto& annotation) {
        if (annotation.name != "warning_ignore")
            return false;
        return std::any_of(annotation.arguments.begin(), annotation.arguments.end(),
                           [&](const auto& argument) {
                               return argument->kind() == ast::ExpressionKind::literal &&
                                      argument->literal_kind() == ast::LiteralKind::string &&
                                      argument->value() == name;
                           });
    });
}

bool is_number_literal(const ast::Expression& expression) {
    if (expression.kind() == ast::ExpressionKind::literal) {
        return expression.literal_kind() == ast::LiteralKind::integer ||
               expression.literal_kind() == ast::LiteralKind::floating;
    }
    return expression.kind() == ast::ExpressionKind::unary &&
           (expression.value() == "+" || expression.value() == "-") &&
           expression.operand_count() == 1 && is_number_literal(*expression.operand(0));
}

bool is_string_literal(const ast::Expression& expression) {
    return expression.kind() == ast::ExpressionKind::literal &&
           expression.literal_kind() == ast::LiteralKind::string;
}

bool is_shared_constant_type(const Type& type) {
    return type.kind == TypeKind::object || type.kind == TypeKind::script_resource ||
           type.kind == TypeKind::array || type.kind == TypeKind::dictionary ||
           type.is_packed_array();
}

bool has_property_annotation(const ast::VariableDeclaration& variable) {
    return std::any_of(variable.annotations.begin(), variable.annotations.end(),
                       [](const ast::Annotation& annotation) {
                           return annotation.name != "onready" &&
                                  annotation.name != "warning_ignore" &&
                                  annotation.name != "export_group" &&
                                  annotation.name != "export_subgroup" &&
                                  annotation.name != "export_category";
                       });
}

const ast::Annotation* property_annotation_of(const ast::VariableDeclaration& variable) {
    const auto found = std::find_if(
        variable.annotations.begin(), variable.annotations.end(), [](const auto& annotation) {
            return annotation.name != "onready" && annotation.name != "warning_ignore" &&
                   annotation.name != "export_group" && annotation.name != "export_subgroup" &&
                   annotation.name != "export_category";
        });
    return found == variable.annotations.end() ? nullptr : &*found;
}

bool match_pattern_contains_binding(const ast::MatchPattern& pattern) {
    if (pattern.kind() == ast::MatchPatternKind::binding)
        return true;
    return std::any_of(pattern.elements.begin(), pattern.elements.end(), [](const auto& element) {
        return match_pattern_contains_binding(*element);
    });
}

std::optional<Type> implied_export_property_type(const ast::VariableDeclaration& variable) {
    for (const auto& annotation : variable.annotations) {
        if (annotation.name == "export_enum")
            return Type{TypeKind::integer, "int"};
        if (annotation.name == "export_color_no_alpha")
            return Type{TypeKind::builtin, "Color"};
        if (annotation.name == "export_node_path")
            return Type{TypeKind::builtin, "NodePath"};
        if (annotation.name == "export_tool_button")
            return Type{TypeKind::builtin, "Callable"};
        if (annotation.name == "export_flags" || annotation.name == "export_flags_2d_render" ||
            annotation.name == "export_flags_2d_physics" ||
            annotation.name == "export_flags_2d_navigation" ||
            annotation.name == "export_flags_3d_render" ||
            annotation.name == "export_flags_3d_physics" ||
            annotation.name == "export_flags_3d_navigation" ||
            annotation.name == "export_flags_avoidance") {
            return Type{TypeKind::integer, "int"};
        }
        if (annotation.name == "export_file" || annotation.name == "export_file_path" ||
            annotation.name == "export_global_file" || annotation.name == "export_dir" ||
            annotation.name == "export_global_dir" || annotation.name == "export_multiline" ||
            annotation.name == "export_placeholder") {
            return Type{TypeKind::string, "String"};
        }
    }
    return std::nullopt;
}

Type export_property_type(const ast::VariableDeclaration& variable, const Type& declared_type,
                          const Type& initializer_type) {
    if (!variable.type && !variable.infer_type) {
        if (const auto implied = implied_export_property_type(variable))
            return *implied;
        if (const auto* annotation = property_annotation_of(variable);
            annotation && annotation->name != "export_storage" && variable.initializer) {
            return initializer_type;
        }
    }
    if (declared_type.kind == TypeKind::variant && variable.initializer) {
        if (const auto* annotation = property_annotation_of(variable);
            annotation && annotation->name == "export_enum") {
            return initializer_type;
        }
    }
    return declared_type;
}

Type exported_value_type(const Type& type) {
    if (type.is_packed_array())
        return packed_array_element_type(type);
    constexpr std::string_view prefix{"Array["};
    if (type.kind == TypeKind::array && type.name.size() > prefix.size() &&
        type.name.compare(0, prefix.size(), prefix) == 0 && type.name.back() == ']') {
        return type_from_annotation(
            type.name.substr(prefix.size(), type.name.size() - prefix.size() - 1));
    }
    return type;
}

bool expression_contains_await(const ast::Expression& expression) {
    if (expression.kind() == ast::ExpressionKind::await_expression)
        return true;
    for (std::size_t index = 0; index < expression.operand_count(); ++index) {
        if (expression_contains_await(*expression.operand(index)))
            return true;
    }
    return false;
}

bool contains_await_syntax(const std::vector<ast::Statement>& statements) {
    for (const auto& statement : statements) {
        if ((statement.expression() && expression_contains_await(*statement.expression())) ||
            (statement.condition() && expression_contains_await(*statement.condition()))) {
            return true;
        }
        if (contains_await_syntax(statement.body()) ||
            contains_await_syntax(statement.else_body())) {
            return true;
        }
        for (const auto& branch : statement.match_branches()) {
            if ((branch.guard && expression_contains_await(*branch.guard)) ||
                contains_await_syntax(branch.body)) {
                return true;
            }
        }
    }
    return false;
}

struct ProjectEnumLookup {
    const ScriptClassSymbol* owner{nullptr};
    const ScriptInnerClassSymbol* inner_owner{nullptr};
    const ScriptEnumSymbol* enumeration{nullptr};
    std::string native_owner;
};

ProjectEnumLookup find_project_enum(const ScriptSymbolTable* symbols, const std::string& name) {
    if (!symbols)
        return {};
    if (const auto separator = name.rfind("::"); separator != std::string::npos) {
        const auto owner_name = name.substr(0, separator);
        const auto enum_name = name.substr(separator + 2);
        if (const auto* owner = symbols->find_native_class(owner_name))
            return {owner, nullptr, symbols->find_enum(*owner, enum_name), owner_name};
        if (const auto* inner = symbols->find_inner_native(owner_name)) {
            const auto enumeration = std::find_if(
                inner->enums.begin(), inner->enums.end(),
                [&](const ScriptEnumSymbol& candidate) { return candidate.name == enum_name; });
            return {symbols->owner_of(*inner), inner,
                    enumeration == inner->enums.end() ? nullptr : &*enumeration, owner_name};
        }
        return {};
    }
    const auto separator = name.find('.');
    if (separator == std::string::npos || name.find('.', separator + 1) != std::string::npos)
        return {};
    const auto* owner = symbols->find_class(name.substr(0, separator));
    if (!owner)
        return {};
    return {owner, nullptr, symbols->find_enum(*owner, name.substr(separator + 1)),
            owner->native_class_name};
}

struct ExternalEnumLookup {
    const ExternalClassSymbol* owner{nullptr};
    const ScriptEnumSymbol* enumeration{nullptr};
};

ExternalEnumLookup find_external_enum(const ScriptSymbolTable* symbols, const std::string& name) {
    if (!symbols)
        return {};
    const auto separator = name.find('.');
    if (separator == std::string::npos || name.find('.', separator + 1) != std::string::npos)
        return {};
    const auto* owner = symbols->find_external(name.substr(0, separator));
    if (!owner)
        return {};
    return {owner, symbols->find_external_enum(*owner, name.substr(separator + 1))};
}

std::string builtin_operator_type(const Type& type) {
    switch (type.kind) {
    case TypeKind::nil:
        return "Nil";
    case TypeKind::boolean:
        return "bool";
    case TypeKind::integer:
    case TypeKind::enumeration:
        return "int";
    case TypeKind::floating:
        return "float";
    case TypeKind::string:
        return "String";
    case TypeKind::string_name:
        return "StringName";
    case TypeKind::array:
        return "Array";
    case TypeKind::dictionary:
        return "Dictionary";
    case TypeKind::builtin:
        return type.name;
    case TypeKind::object:
        return "Object";
    case TypeKind::variant:
    case TypeKind::unknown:
        return "Variant";
    case TypeKind::void_type:
    case TypeKind::script_resource:
        return {};
    }
    return {};
}

} // namespace

Type SemanticModel::type_of(const ast::Expression& expression) const {
    const auto found = expression_types_.find(&expression);
    return found == expression_types_.end() ? unknown_type : found->second;
}

Type SemanticModel::storage_type_of(const ast::Expression& expression) const {
    const auto found = referenced_symbols_.find(&expression);
    if (found != referenced_symbols_.end() &&
        (found->second.kind == SymbolKind::local || found->second.kind == SymbolKind::parameter)) {
        return found->second.type;
    }
    return type_of(expression);
}

bool SemanticModel::is_non_null(const ast::Expression& expression) const noexcept {
    return non_null_expressions_.find(&expression) != non_null_expressions_.end();
}

Type SemanticModel::type_of(const ast::VariableDeclaration& declaration) const {
    const auto found = variable_types_.find(&declaration);
    return found == variable_types_.end() ? unknown_type : found->second;
}

Type SemanticModel::property_type_of(const ast::VariableDeclaration& declaration) const {
    const auto found = property_types_.find(&declaration);
    return found == property_types_.end() ? type_of(declaration) : found->second;
}

Type SemanticModel::type_of(const ast::Statement& statement) const {
    const auto found = local_types_.find(&statement);
    return found == local_types_.end() ? unknown_type : found->second;
}

IterationPlan SemanticModel::iteration_plan_of(const ast::Statement& statement) const {
    const auto found = iteration_plans_.find(&statement);
    return found == iteration_plans_.end() ? IterationPlan{} : found->second;
}

const std::vector<DebugVariable>&
SemanticModel::debug_variables_at(const ast::Statement& statement) const noexcept {
    static const std::vector<DebugVariable> empty;
    const auto found = debug_variables_.find(&statement);
    return found == debug_variables_.end() ? empty : found->second;
}

Type SemanticModel::type_of(const ast::MatchPattern& pattern) const {
    const auto found = match_pattern_types_.find(&pattern);
    return found == match_pattern_types_.end() ? unknown_type : found->second;
}

Type SemanticModel::type_of(const ast::Parameter& parameter) const {
    const auto found = parameter_types_.find(&parameter);
    return found == parameter_types_.end() ? unknown_type : found->second;
}

DefaultArgumentEvaluation
SemanticModel::default_argument_evaluation_of(const ast::Parameter& parameter) const noexcept {
    const auto found = default_argument_evaluations_.find(&parameter);
    return found == default_argument_evaluations_.end() ? DefaultArgumentEvaluation::absent
                                                        : found->second;
}

Type SemanticModel::return_type_of(const ast::FunctionDeclaration& function) const {
    const auto found = function_return_types_.find(&function);
    return found == function_return_types_.end() ? unknown_type : found->second;
}

Type SemanticModel::return_type_of(const ast::LambdaExpression& function) const {
    const auto found = lambda_return_types_.find(&function);
    return found == lambda_return_types_.end() ? unknown_type : found->second;
}

bool SemanticModel::is_coroutine(const ast::FunctionDeclaration& function) const noexcept {
    return coroutine_functions_.find(&function) != coroutine_functions_.end();
}

bool SemanticModel::is_coroutine(const ast::LambdaExpression& function) const noexcept {
    return coroutine_lambdas_.find(&function) != coroutine_lambdas_.end();
}

bool SemanticModel::is_coroutine_call(const ast::Expression& expression) const noexcept {
    return coroutine_calls_.find(&expression) != coroutine_calls_.end();
}

bool SemanticModel::owner_bound(const ast::LambdaExpression& function) const noexcept {
    return owner_bound_lambdas_.find(&function) != owner_bound_lambdas_.end();
}

const RpcConfiguration*
SemanticModel::rpc_configuration_of(const ast::FunctionDeclaration& function) const noexcept {
    const auto found = rpc_configurations_.find(&function);
    return found == rpc_configurations_.end() ? nullptr : &found->second;
}

std::int64_t SemanticModel::value_of(const ast::EnumEntry& entry) const {
    const auto found = enum_values_.find(&entry);
    return found == enum_values_.end() ? 0 : found->second;
}

std::optional<std::int64_t>
SemanticModel::constant_integer_value_of(const ast::Expression& expression) const noexcept {
    const auto found = constant_integer_values_.find(&expression);
    return found == constant_integer_values_.end() ? std::nullopt
                                                   : std::optional<std::int64_t>{found->second};
}

const Symbol* SemanticModel::symbol_of(const ast::Expression& expression) const noexcept {
    const auto found = referenced_symbols_.find(&expression);
    return found == referenced_symbols_.end() ? nullptr : &found->second;
}

const ApiResolution*
SemanticModel::api_resolution_of(const ast::Expression& expression) const noexcept {
    const auto found = api_resolutions_.find(&expression);
    return found == api_resolutions_.end() ? nullptr : &found->second;
}

const ResolvedCallContract*
SemanticModel::call_contract_of(const ast::Expression& expression) const noexcept {
    const auto found = call_contracts_.find(&expression);
    return found == call_contracts_.end() ? nullptr : &found->second;
}

void SemanticAnalyzer::declare(Symbol symbol) {
    auto& scope = scopes_.back();
    const auto existing = scope.find(symbol.name);
    if (existing != scope.end()) {
        diagnostics_.error("GDS4001", "duplicate declaration of '" + symbol.name + "'",
                           symbol.declaration);
        return;
    }
    if (symbol.identity == 0)
        symbol.identity = next_symbol_identity_++;
    scope.emplace(symbol.name, std::move(symbol));
}

const Symbol* SemanticAnalyzer::resolve(const std::string& name) const noexcept {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->find(name);
        if (found != scope->end())
            return &found->second;
    }
    return nullptr;
}

std::vector<DebugVariable> SemanticAnalyzer::visible_debug_variables() const {
    std::vector<DebugVariable> result;
    std::unordered_set<std::string> shadowed;
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        for (const auto& [name, symbol] : *scope) {
            const bool function_value =
                symbol.storage == SymbolStorage::function_local &&
                (symbol.kind == SymbolKind::parameter || symbol.kind == SymbolKind::local ||
                 symbol.kind == SymbolKind::constant);
            if (function_value && shadowed.insert(name).second)
                result.push_back({name, symbol.type, symbol.declaration});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.declaration.begin.offset != right.declaration.begin.offset)
            return left.declaration.begin.offset < right.declaration.begin.offset;
        return left.name < right.name;
    });
    return result;
}

void SemanticAnalyzer::require_assignable(const Type& target, const Type& source, SourceSpan span,
                                          const std::string& context) {
    const bool compatible_objects =
        target.kind == TypeKind::object && source.kind == TypeKind::object &&
        (object_type_inherits(source, target) || object_type_inherits(target, source));
    const bool same_project_enum =
        current_script_ && target.kind == TypeKind::enumeration &&
        source.kind == TypeKind::enumeration &&
        (target.name == current_script_->script_name + "." + source.name ||
         source.name == current_script_->script_name + "." + target.name);
    if (!is_implicitly_convertible(target, source) && !compatible_objects && !same_project_enum) {
        diagnostics_.error("GDS4002",
                           context + ": cannot assign " + source.display_name() + " to " +
                               target.display_name(),
                           span);
    }
}

void SemanticAnalyzer::require_truthy_value(const Type& type, const SourceSpan span,
                                            const std::string& context) {
    if (type.truthiness() == TruthinessKind::invalid) {
        diagnostics_.error("GDS4153", context + " cannot use a void expression", span);
    }
}

void SemanticAnalyzer::require_inferable_type(const Type& type, const SourceSpan span,
                                              const std::string& context) {
    if (type.kind == TypeKind::nil) {
        diagnostics_.error("GDS4154", context + " cannot infer a concrete type from null", span);
    } else if (!type.is_value()) {
        diagnostics_.error("GDS4154", context + " cannot infer a type from a void expression",
                           span);
    }
}

void SemanticAnalyzer::require_expression_assignable(const Type& target,
                                                     const ast::Expression& expression,
                                                     const Type& source, const SourceSpan span,
                                                     const std::string& context) {
    require_assignable(target, source, span, context);
    const bool semantically_assignable = is_implicitly_convertible(target, source);
    const auto container = describe_container_type(target);
    if (container && container->kind == ContainerTypeKind::array &&
        expression.kind() == ast::ExpressionKind::array_literal) {
        const auto element_type = type_from_name(container->arguments.front(), expression.span);
        for (std::size_t index = 0; index < expression.operand_count(); ++index) {
            const auto& element = *expression.operand(index);
            require_expression_assignable(element_type, element, model_.type_of(element),
                                          element.span,
                                          context + " array element " + std::to_string(index + 1));
        }
        model_.expression_types_[&expression] = target;
        return;
    }
    if (container && container->kind == ContainerTypeKind::dictionary &&
        expression.kind() == ast::ExpressionKind::dictionary_literal) {
        const auto key_type = type_from_name(container->arguments.at(0), expression.span);
        const auto value_type = type_from_name(container->arguments.at(1), expression.span);
        for (std::size_t index = 0; index + 1 < expression.operand_count(); index += 2) {
            const auto& key = *expression.operand(index);
            const auto& value = *expression.operand(index + 1);
            require_expression_assignable(key_type, key, model_.type_of(key), key.span,
                                          context + " dictionary key " +
                                              std::to_string(index / 2 + 1));
            require_expression_assignable(value_type, value, model_.type_of(value), value.span,
                                          context + " dictionary value " +
                                              std::to_string(index / 2 + 1));
        }
        model_.expression_types_[&expression] = target;
        return;
    }
    const auto runtime_source = runtime_storage_type_of(expression);
    if (semantically_assignable && !is_runtime_storage_compatible(target, runtime_source) &&
        !is_explicitly_typed_container(target)) {
        diagnostics_.error("GDS4157",
                           context + ": Godot runtime storage " + target.display_name() +
                               " rejects runtime value " + runtime_source.display_name(),
                           span);
    }
}

void SemanticAnalyzer::validate_script_call(const ScriptMemberSymbol& member,
                                            const std::vector<Type>& arguments,
                                            const ast::Expression& call, SourceSpan span) {
    if (member.kind != ScriptMemberKind::function) {
        diagnostics_.error("GDS4053", "script member '" + member.name + "' is not callable", span);
        return;
    }
    model_.call_contracts_.insert_or_assign(
        &call,
        ResolvedCallContract{member.parameters, member.required_arguments, member.is_vararg});
    if (arguments.size() < member.required_arguments ||
        (!member.is_vararg && arguments.size() > member.parameters.size())) {
        diagnostics_.error(
            "GDS4054",
            "script method '" + member.name + "' expects " +
                std::to_string(member.required_arguments) +
                (member.required_arguments == member.parameters.size() && !member.is_vararg
                     ? " argument(s)"
                 : member.is_vararg
                     ? " or more argument(s)"
                     : " to " + std::to_string(member.parameters.size()) + " argument(s)") +
                ", got " + std::to_string(arguments.size()),
            span);
    }
    const auto checked = std::min(arguments.size(), member.parameters.size());
    for (std::size_t index = 0; index < checked; ++index) {
        require_expression_assignable(
            member.parameters[index], *call.operand(index + 1), arguments[index], span,
            "argument " + std::to_string(index + 1) + " of '" + member.name + "'");
    }
}

void SemanticAnalyzer::validate_local_call(const ast::FunctionDeclaration& function,
                                           const std::vector<Type>& arguments,
                                           const ast::Expression& call, SourceSpan span) {
    const auto required = static_cast<std::size_t>(
        std::count_if(function.parameters.begin(), function.parameters.end(),
                      [](const auto& parameter) { return !parameter.default_value; }));
    ResolvedCallContract contract;
    contract.required_arguments = required;
    contract.is_vararg = function.rest_parameter.has_value();
    contract.parameters.reserve(function.parameters.size());
    for (const auto& parameter : function.parameters) {
        contract.parameters.push_back(
            parameter.type ? type_from_name(*parameter.type, parameter.span) : variant_type);
    }
    model_.call_contracts_.insert_or_assign(&call, contract);
    if (arguments.size() < required ||
        (!function.rest_parameter && arguments.size() > function.parameters.size())) {
        diagnostics_.error(
            "GDS4054",
            "script method '" + function.name + "' expects " + std::to_string(required) +
                (function.rest_parameter ? " or more argument(s)"
                 : required == function.parameters.size()
                     ? " argument(s)"
                     : " to " + std::to_string(function.parameters.size()) + " argument(s)") +
                ", got " + std::to_string(arguments.size()),
            span);
    }
    const auto checked = std::min(arguments.size(), function.parameters.size());
    for (std::size_t index = 0; index < checked; ++index) {
        const auto target = contract.parameters[index];
        require_expression_assignable(
            target, *call.operand(index + 1), arguments[index], call.operand(index + 1)->span,
            "argument " + std::to_string(index + 1) + " of '" + function.name + "'");
    }
}

void SemanticAnalyzer::analyze_rest_parameter(const ast::Parameter& parameter) {
    auto type = Type{TypeKind::array, "Array"};
    if (parameter.type) {
        const auto specified = type_from_name(*parameter.type, parameter.span);
        if (specified.kind != TypeKind::array) {
            diagnostics_.error(
                "GDS4162", "the rest parameter type must be Array, got " + specified.display_name(),
                parameter.span);
        } else if (const auto descriptor = describe_container_type(specified);
                   descriptor && !descriptor->arguments.empty() &&
                   descriptor->arguments.front() != "Variant") {
            diagnostics_.error("GDS4163", "typed arrays are not supported for a rest parameter",
                               parameter.span);
        } else {
            type = specified;
        }
    }
    model_.parameter_types_[&parameter] = type;
    declare({SymbolKind::parameter, parameter.name, type, parameter.span, false});
}

void SemanticAnalyzer::validate_container_method_call(const Type& container,
                                                      const std::string_view method,
                                                      const std::vector<Type>& arguments,
                                                      const ast::Expression& call) {
    const auto descriptor = describe_container_type(container);
    if (!descriptor)
        return;
    const auto require_argument = [&](const std::size_t index, const Type& target) {
        if (index >= arguments.size() || index + 1 >= call.operand_count())
            return;
        const auto& expression = *call.operand(index + 1);
        require_expression_assignable(target, expression, arguments[index], expression.span,
                                      "argument " + std::to_string(index + 1) + " of '" +
                                          std::string{method} + "'");
    };
    if (descriptor->kind == ContainerTypeKind::array) {
        const auto element = type_from_name(descriptor->arguments.front(), call.span);
        if (method == "append" || method == "push_back" || method == "push_front" ||
            method == "fill") {
            require_argument(0, element);
        } else if (method == "insert") {
            require_argument(1, element);
        } else if (method == "append_array" || method == "assign") {
            require_argument(0, container);
        }
        return;
    }
    const auto key = type_from_name(descriptor->arguments.at(0), call.span);
    const auto value = type_from_name(descriptor->arguments.at(1), call.span);
    if (method == "set") {
        require_argument(0, key);
        require_argument(1, value);
    } else if (method == "get" || method == "has" || method == "erase" || method == "get_or_add") {
        require_argument(0, key);
        if (method == "get_or_add")
            require_argument(1, value);
    }
}

const ScriptInnerClassSymbol*
SemanticAnalyzer::find_inner_class(const std::string& name) const noexcept {
    if (const auto found = local_inner_classes_.find(name); found != local_inner_classes_.end()) {
        return &found->second;
    }
    if (current_inner_class_) {
        if (const auto* nested = find_nested_inner_class(*current_inner_class_, name))
            return nested;
        const auto separator = current_inner_class_->name.rfind('.');
        if (separator != std::string::npos) {
            const auto lexical = current_inner_class_->name.substr(0, separator + 1) + name;
            if (const auto found = local_inner_classes_.find(lexical);
                found != local_inner_classes_.end()) {
                return &found->second;
            }
        }
    }
    const ScriptInnerClassSymbol* unique = nullptr;
    for (const auto& [qualified, inner] : local_inner_classes_) {
        const auto separator = qualified.rfind('.');
        const auto leaf =
            separator == std::string::npos ? qualified : qualified.substr(separator + 1);
        if (leaf != name)
            continue;
        if (unique)
            return nullptr;
        unique = &inner;
    }
    if (unique)
        return unique;
    if (script_symbols_ && current_script_) {
        if (const auto* current = script_symbols_->find_inner(*current_script_, name))
            return current;
    }
    return script_symbols_ ? script_symbols_->find_inner_native(name) : nullptr;
}

const ScriptClassSymbol*
SemanticAnalyzer::find_script_class(const std::string& name) const noexcept {
    if (!script_symbols_)
        return nullptr;
    if (const auto* script = script_symbols_->find_class(name))
        return script;
    return script_symbols_->find_native_class(name);
}

const ScriptInnerClassSymbol*
SemanticAnalyzer::inner_base_of(const ScriptInnerClassSymbol& owner) const noexcept {
    if (owner.base_class_name.empty())
        return nullptr;
    if (const auto found = local_inner_classes_.find(owner.base_class_name);
        found != local_inner_classes_.end()) {
        return &found->second;
    }
    return script_symbols_ && current_script_
               ? script_symbols_->find_inner(*current_script_, owner.base_class_name)
               : nullptr;
}

const ScriptClassSymbol*
SemanticAnalyzer::inner_script_base_of(const ScriptInnerClassSymbol& owner) const noexcept {
    return script_symbols_ ? script_symbols_->base_of(owner) : nullptr;
}

const ScriptInnerClassSymbol*
SemanticAnalyzer::find_nested_inner_class(const ScriptInnerClassSymbol& owner,
                                          const std::string& name) const noexcept {
    const ScriptInnerClassSymbol* current = &owner;
    std::unordered_set<const ScriptInnerClassSymbol*> visited;
    while (current && visited.insert(current).second) {
        const auto qualified = current->name + "." + name;
        if (const auto found = local_inner_classes_.find(qualified);
            found != local_inner_classes_.end()) {
            return &found->second;
        }
        if (script_symbols_) {
            const auto* script_owner = script_symbols_->owner_of(*current);
            if (!script_owner)
                script_owner = current_script_;
            if (script_owner) {
                if (const auto* found = script_symbols_->find_inner(*script_owner, qualified))
                    return found;
            }
        }
        current = inner_base_of(*current);
    }
    return nullptr;
}

SemanticAnalyzer::InnerEnumLookup
SemanticAnalyzer::find_inner_enum(const std::string& name) const noexcept {
    const auto separator = name.rfind('.');
    if (separator == std::string::npos)
        return {};
    const auto owner_name = name.substr(0, separator);
    const auto enum_name = name.substr(separator + 1);
    const auto search = [&](const ScriptInnerClassSymbol* owner) -> InnerEnumLookup {
        std::unordered_set<const ScriptInnerClassSymbol*> visited;
        for (auto* current = owner; current && visited.insert(current).second;
             current = inner_base_of(*current)) {
            const auto found = std::find_if(
                current->enums.begin(), current->enums.end(),
                [&](const auto& enumeration) { return enumeration.name == enum_name; });
            if (found != current->enums.end())
                return {current, &*found};
        }
        return {};
    };
    if (const auto local = search(find_inner_class(owner_name)); local.enumeration)
        return local;
    if (script_symbols_ && current_script_) {
        return search(script_symbols_->find_inner(*current_script_, owner_name));
    }
    return {};
}

const ScriptMemberSymbol*
SemanticAnalyzer::find_inner_member(const ScriptInnerClassSymbol& owner,
                                    const std::string& name) const noexcept {
    const ScriptInnerClassSymbol* current = &owner;
    std::unordered_set<const ScriptInnerClassSymbol*> visited;
    while (current && visited.insert(current).second) {
        const auto found = std::find_if(current->members.begin(), current->members.end(),
                                        [&](const auto& member) { return member.name == name; });
        if (found != current->members.end())
            return &*found;
        current = inner_base_of(*current);
    }
    if (script_symbols_) {
        const auto* terminal = &owner;
        visited.clear();
        while (terminal && visited.insert(terminal).second) {
            if (const auto* base = inner_script_base_of(*terminal))
                return script_symbols_->find_member(*base, name);
            terminal = inner_base_of(*terminal);
        }
        if (const auto* external = script_symbols_->external_base_of(owner))
            return script_symbols_->find_external_member(*external, name);
    }
    return nullptr;
}

bool SemanticAnalyzer::script_function_is_static(const std::string& name) const noexcept {
    if (const auto found = functions_.find(name); found != functions_.end())
        return found->second->is_static;
    if (current_inner_class_) {
        const auto* member = find_inner_member(*current_inner_class_, name);
        return member && member->kind == ScriptMemberKind::function && member->is_static;
    }
    if (script_symbols_ && current_script_) {
        const auto* member = script_symbols_->find_member(*current_script_, name);
        return member && member->kind == ScriptMemberKind::function && member->is_static;
    }
    return false;
}

void SemanticAnalyzer::diagnose_static_instance_access(const std::string_view kind,
                                                       const std::string_view name,
                                                       const SourceSpan span) {
    if (instance_context_available_)
        return;
    diagnostics_.error("GDS4146",
                       "cannot use instance " + std::string{kind} + " '" + std::string{name} +
                           "' from a static context",
                       span);
}

void SemanticAnalyzer::record_script_dependency(const ScriptClassSymbol* dependency) {
    if (dependency && (!current_script_ || dependency->path != current_script_->path))
        model_.referenced_script_paths_.insert(dependency->path);
}

Type SemanticAnalyzer::declared_or_inferred(const std::optional<std::string>& annotation,
                                            const ast::ExpressionPtr& initializer) {
    if (annotation.has_value())
        return type_from_name(*annotation);
    return initializer ? analyze_expression(*initializer) : variant_type;
}

Type SemanticAnalyzer::type_from_name(const std::string& name, SourceSpan span) {
    const auto parsed_type = type_from_annotation(name);
    if (const auto container = describe_container_type(parsed_type)) {
        std::vector<Type> arguments;
        arguments.reserve(container->arguments.size());
        for (const auto& argument : container->arguments) {
            auto argument_type = type_from_name(argument, span);
            if (argument_type.kind == TypeKind::void_type) {
                diagnostics_.error("GDS4139", "typed container arguments cannot use the void type",
                                   span);
            }
            if (is_explicitly_typed_container(argument_type)) {
                diagnostics_.error(
                    "GDS4138",
                    "nested typed containers are not supported by Godot; use an untyped "
                    "Array or Dictionary for the nested value",
                    span);
            }
            arguments.push_back(std::move(argument_type));
        }
        std::string canonical =
            container->kind == ContainerTypeKind::array ? "Array[" : "Dictionary[";
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (index != 0)
                canonical += ", ";
            canonical += arguments[index].name;
        }
        canonical += ']';
        return {parsed_type.kind, std::move(canonical)};
    }
    if (const auto alias = script_resource_aliases_.find(name);
        alias != script_resource_aliases_.end()) {
        record_script_dependency(alias->second);
        return {TypeKind::object, alias->second->native_class_name};
    }
    if (enum_types_.find(name) != enum_types_.end()) {
        if (current_inner_class_) {
            const auto owner = current_inner_class_->native_class_name.empty()
                                   ? current_inner_class_->name
                                   : current_inner_class_->native_class_name;
            return {TypeKind::enumeration,
                    owner + (current_inner_class_->native_class_name.empty() ? "." : "::") + name};
        }
        if (current_script_) {
            return {TypeKind::enumeration, current_script_->native_class_name + "::" + name};
        }
        return {TypeKind::enumeration, name};
    }
    if (api_.has_global_enum(name))
        return {TypeKind::enumeration, name};
    if (api_.has_class_enum(base_type_, name))
        return {TypeKind::enumeration, base_type_ + "." + name};
    if (const auto separator = name.rfind('.');
        separator != std::string::npos &&
        api_.has_class_enum(name.substr(0, separator), name.substr(separator + 1))) {
        return {TypeKind::enumeration, name};
    }
    if (const auto project_enum = find_project_enum(script_symbols_, name);
        project_enum.enumeration) {
        record_script_dependency(project_enum.owner);
        return {TypeKind::enumeration,
                project_enum.native_owner + "::" + project_enum.enumeration->name};
    }
    if (const auto separator = name.find('.'); separator != std::string::npos) {
        const auto alias = script_resource_aliases_.find(name.substr(0, separator));
        if (alias != script_resource_aliases_.end()) {
            const auto* owner = alias->second;
            const auto member_name = name.substr(separator + 1);
            record_script_dependency(owner);
            if (const auto* inner = script_symbols_->find_inner(*owner, member_name))
                return {TypeKind::object, inner->native_class_name};
            if (const auto* enumeration = script_symbols_->find_enum(*owner, member_name)) {
                return {TypeKind::enumeration, owner->native_class_name + "::" + enumeration->name};
            }
            if (const auto inner_separator = member_name.rfind('.');
                inner_separator != std::string::npos) {
                const auto* inner =
                    script_symbols_->find_inner(*owner, member_name.substr(0, inner_separator));
                if (inner) {
                    const auto enum_name = member_name.substr(inner_separator + 1);
                    const auto enumeration = std::find_if(inner->enums.begin(), inner->enums.end(),
                                                          [&](const ScriptEnumSymbol& candidate) {
                                                              return candidate.name == enum_name;
                                                          });
                    if (enumeration != inner->enums.end()) {
                        return {TypeKind::enumeration,
                                inner->native_class_name + "::" + enumeration->name};
                    }
                }
            }
            diagnostics_.error("GDS4059",
                               "script resource alias '" + name.substr(0, separator) +
                                   "' has no type '" + member_name + "'",
                               span);
            return unknown_type;
        }
    }
    if (current_script_ && name.find('.') == std::string::npos) {
        if (const auto* enumeration = script_symbols_->find_enum(*current_script_, name)) {
            return {TypeKind::enumeration,
                    current_script_->native_class_name + "::" + enumeration->name};
        }
    }
    if (current_inner_class_ && name.find('.') == std::string::npos) {
        const auto enumeration =
            std::find_if(current_inner_class_->enums.begin(), current_inner_class_->enums.end(),
                         [&](const ScriptEnumSymbol& candidate) { return candidate.name == name; });
        if (enumeration != current_inner_class_->enums.end()) {
            const auto owner = current_inner_class_->native_class_name.empty()
                                   ? current_inner_class_->name
                                   : current_inner_class_->native_class_name;
            return {TypeKind::enumeration, owner + "::" + enumeration->name};
        }
    }
    if (const auto inner_enum = find_inner_enum(name); inner_enum.enumeration) {
        const auto owner = inner_enum.owner->native_class_name.empty()
                               ? inner_enum.owner->name
                               : inner_enum.owner->native_class_name;
        return {TypeKind::enumeration,
                owner + (inner_enum.owner->native_class_name.empty() ? "." : "::") +
                    inner_enum.enumeration->name};
    }
    if (const auto external_enum = find_external_enum(script_symbols_, name);
        external_enum.enumeration) {
        model_.referenced_extension_abis_.insert(external_enum.owner->provider_abi);
        return {TypeKind::enumeration, name};
    }
    const auto type = parsed_type;
    const auto* project_type = find_script_class(name);
    const auto* external_type = script_symbols_ ? script_symbols_->find_external(name) : nullptr;
    if (external_type)
        model_.referenced_extension_abis_.insert(external_type->provider_abi);
    record_script_dependency(project_type);
    if (type.kind == TypeKind::object && !api_.find_class(name) && !find_inner_class(name) &&
        !project_type && !external_type) {
        diagnostics_.error("GDS4059", "unknown Godot or project script type '" + name + "'", span);
    }
    return type;
}

bool SemanticAnalyzer::object_type_inherits(const Type& derived, const Type& base) const noexcept {
    if (derived.kind != TypeKind::object || base.kind != TypeKind::object)
        return false;
    if (derived.name == base.name || base.name == "Object")
        return true;
    if (api_.find_class(derived.name) && api_.find_class(base.name))
        return api_.inherits(derived.name, base.name);

    const ScriptClassSymbol* base_script = nullptr;
    if (script_symbols_) {
        const auto* derived_script = script_symbols_->find_class(derived.name);
        base_script = script_symbols_->find_class(base.name);
        if (!derived_script)
            derived_script = script_symbols_->find_native_class(derived.name);
        if (!base_script)
            base_script = script_symbols_->find_native_class(base.name);
        if (!derived_script && current_script_ && derived.name == current_script_->script_name)
            derived_script = current_script_;
        if (derived_script && base_script)
            return derived_script == base_script ||
                   script_symbols_->inherits(*derived_script, base_script->script_name);
        if (derived_script && api_.find_class(base.name))
            return api_.inherits(derived_script->godot_base_type, base.name);

        const auto* derived_external = script_symbols_->find_external(derived.name);
        const auto* base_external = script_symbols_->find_external(base.name);
        if (derived_external && api_.find_class(base.name))
            return api_.inherits(derived_external->godot_base_type, base.name);
        if (derived_script && base_external)
            return derived_script->godot_base_type == base_external->name;
    }

    const auto* derived_inner = find_inner_class(derived.name);
    const auto* base_inner = find_inner_class(base.name);
    if (derived_inner && base_inner) {
        for (auto* current = derived_inner; current; current = inner_base_of(*current)) {
            if (current == base_inner)
                return true;
        }
    }
    if (derived_inner && base_script && script_symbols_) {
        std::unordered_set<const ScriptInnerClassSymbol*> visited;
        for (auto* current = derived_inner; current && visited.insert(current).second;
             current = inner_base_of(*current)) {
            if (const auto* script_base = inner_script_base_of(*current))
                return script_base == base_script ||
                       script_symbols_->inherits(*script_base, base_script->script_name);
        }
    }
    if (derived_inner && api_.find_class(base.name))
        return api_.inherits(derived_inner->godot_base_type, base.name);
    return false;
}

bool SemanticAnalyzer::override_type_accepts(const Type& target,
                                             const Type& source) const noexcept {
    if (target == source || target.is_dynamic() || source.is_dynamic())
        return true;
    if (target.kind != TypeKind::object || source.kind != TypeKind::object)
        return is_implicitly_convertible(target, source);
    return object_type_inherits(source, target);
}

Type SemanticAnalyzer::container_element_type(const Type& container, SourceSpan span) {
    if (container.kind == TypeKind::integer)
        return {TypeKind::integer, "int"};
    if (container.kind == TypeKind::floating)
        return {TypeKind::floating, "float"};
    if (container.kind == TypeKind::string)
        return {TypeKind::string, "String"};
    if (container.kind == TypeKind::builtin &&
        (container.name == "Vector2" || container.name == "Vector3")) {
        return {TypeKind::floating, "float"};
    }
    if (container.kind == TypeKind::builtin &&
        (container.name == "Vector2i" || container.name == "Vector3i")) {
        return {TypeKind::integer, "int"};
    }
    if (container.is_packed_array())
        return packed_array_element_type(container);
    if (container.kind == TypeKind::array) {
        const auto arguments = generic_type_arguments(container.name, "Array", 1);
        return arguments ? type_from_name(arguments->front(), span) : variant_type;
    }
    if (container.kind == TypeKind::dictionary) {
        const auto arguments = generic_type_arguments(container.name, "Dictionary", 2);
        return arguments ? type_from_name(arguments->at(1), span) : variant_type;
    }
    return variant_type;
}

Type SemanticAnalyzer::iteration_element_type(const Type& container, const SourceSpan span) {
    if (container.kind == TypeKind::dictionary) {
        const auto arguments = generic_type_arguments(container.name, "Dictionary", 2);
        return arguments ? type_from_name(arguments->front(), span) : variant_type;
    }
    return container_element_type(container, span);
}

std::optional<Type> SemanticAnalyzer::object_iteration_element_type(const Type& object,
                                                                    const SourceSpan span) {
    const auto validate_member = [&](const ScriptMemberSymbol* member,
                                     const std::string_view name) -> bool {
        if (!member || member->kind != ScriptMemberKind::function || member->is_static ||
            member->parameters.size() != 1U || member->required_arguments > 1U) {
            diagnostics_.error("GDS4141",
                               "iterator object '" + object.display_name() +
                                   "' requires instance " + std::string{name} +
                                   "(state) with exactly one parameter",
                               span);
            return false;
        }
        if (name != "_iter_get" && !member->type.is_dynamic() &&
            member->type.kind != TypeKind::boolean) {
            diagnostics_.error(
                "GDS4141",
                "iterator method '" + std::string{name} + "' must return bool or Variant", span);
            return false;
        }
        if (name == "_iter_get" && member->type.kind == TypeKind::void_type) {
            diagnostics_.error("GDS4141", "iterator method '_iter_get' must return a value", span);
            return false;
        }
        return true;
    };
    const auto validate_protocol = [&](const auto& find_member) -> std::optional<Type> {
        const auto* initialize = find_member("_iter_init");
        const auto* advance = find_member("_iter_next");
        const auto* get = find_member("_iter_get");
        const bool valid_initialize = validate_member(initialize, "_iter_init");
        const bool valid_advance = validate_member(advance, "_iter_next");
        const bool valid_get = validate_member(get, "_iter_get");
        if (!valid_initialize || !valid_advance || !valid_get)
            return std::nullopt;
        return get->type;
    };

    if (const auto* inner = find_inner_class(object.name)) {
        return validate_protocol(
            [&](const std::string& name) { return find_inner_member(*inner, name); });
    }
    if (const auto* script = find_script_class(object.name)) {
        record_script_dependency(script);
        return validate_protocol(
            [&](const std::string& name) { return script_symbols_->find_member(*script, name); });
    }
    if (const auto* external =
            script_symbols_ ? script_symbols_->find_external(object.name) : nullptr) {
        model_.referenced_extension_abis_.insert(external->provider_abi);
        const auto* get = script_symbols_->find_external_member(*external, "_iter_get");
        if (!get && !external->members_complete &&
            !warning_is_ignored(active_warning_ignores_, "unsafe_method_access")) {
            diagnostics_.warning(
                "GDS4142",
                "runtime-only GDExtension iterator '" + object.display_name() +
                    "' has no complete ClassDB method contract; iteration remains Variant-checked",
                span);
            return variant_type;
        }
        return validate_protocol([&](const std::string& name) {
            return script_symbols_->find_external_member(*external, name);
        });
    }

    if (object.name == "PackedDataContainer")
        return Type{TypeKind::object, "PackedDataContainerRef"};
    if (object.name == "PackedDataContainerRef")
        return variant_type;

    diagnostics_.error("GDS4140",
                       "object type '" + object.display_name() +
                           "' does not implement the _iter_init/_iter_next/_iter_get protocol",
                       span);
    return std::nullopt;
}

Type SemanticAnalyzer::resolve_binary_expression(const ast::Expression& expression,
                                                 const Type& left, const Type& right) {
    const auto& operation = expression.value();
    if (operation == "is" || operation == "is not") {
        auto target_type = right;
        if (expression.operand(1)->kind() == ast::ExpressionKind::identifier) {
            const auto alias = script_resource_aliases_.find(expression.operand(1)->value());
            if (alias != script_resource_aliases_.end()) {
                target_type = {TypeKind::object, alias->second->native_class_name};
                model_.expression_types_.insert_or_assign(expression.operand(1).get(), target_type);
                model_.api_resolutions_.insert_or_assign(
                    expression.operand(1).get(),
                    ApiResolution{ApiResolutionKind::script_type_reference,
                                  alias->second->native_class_name, "", "", target_type, 0, 0,
                                  false, true});
                record_script_dependency(alias->second);
            }
        }
        const auto* target = model_.api_resolution_of(*expression.operand(1));
        const bool valid_target = target &&
                                  (target->kind == ApiResolutionKind::type_reference ||
                                   target->kind == ApiResolutionKind::external_type_reference ||
                                   target->kind == ApiResolutionKind::script_type_reference ||
                                   target->kind == ApiResolutionKind::inner_type_reference ||
                                   target->kind == ApiResolutionKind::script_enum_type ||
                                   target->kind == ApiResolutionKind::global_enum_type) &&
                                  target_type.kind != TypeKind::void_type;
        if (!valid_target) {
            diagnostics_.error("GDS4067", "the right operand of 'is' must be a type",
                               expression.operand(1)->span);
        }
        const auto* value_resolution = model_.api_resolution_of(*expression.operand(0));
        if (left.kind == TypeKind::void_type ||
            (value_resolution &&
             (value_resolution->kind == ApiResolutionKind::type_reference ||
              value_resolution->kind == ApiResolutionKind::external_type_reference ||
              value_resolution->kind == ApiResolutionKind::script_type_reference))) {
            diagnostics_.error("GDS4068", "the left operand of 'is' must be a value",
                               expression.operand(0)->span);
        }
        return {TypeKind::boolean, "bool"};
    }
    if (operation == "as") {
        auto target_type = right;
        if (expression.operand(1)->kind() == ast::ExpressionKind::identifier) {
            const auto alias = script_resource_aliases_.find(expression.operand(1)->value());
            if (alias != script_resource_aliases_.end()) {
                target_type = {TypeKind::object, alias->second->native_class_name};
                model_.expression_types_.insert_or_assign(expression.operand(1).get(), target_type);
                model_.api_resolutions_.insert_or_assign(
                    expression.operand(1).get(),
                    ApiResolution{ApiResolutionKind::script_type_reference,
                                  alias->second->native_class_name, "", "", target_type, 0, 0,
                                  false, true});
                record_script_dependency(alias->second);
            }
        }
        const auto* target = model_.api_resolution_of(*expression.operand(1));
        const bool valid_target = target &&
                                  (target->kind == ApiResolutionKind::type_reference ||
                                   target->kind == ApiResolutionKind::external_type_reference ||
                                   target->kind == ApiResolutionKind::script_type_reference ||
                                   target->kind == ApiResolutionKind::inner_type_reference ||
                                   target->kind == ApiResolutionKind::script_enum_type ||
                                   target->kind == ApiResolutionKind::global_enum_type) &&
                                  target_type.kind != TypeKind::void_type;
        if (!valid_target) {
            diagnostics_.error("GDS4074", "the right operand of 'as' must be a type",
                               expression.operand(1)->span);
            return unknown_type;
        }
        bool valid_conversion = false;
        if (left.is_dynamic() || target_type.is_dynamic()) {
            valid_conversion = true;
        } else if (target_type.kind == TypeKind::object) {
            // GDScript object casts are runtime-checked and return null for unrelated object
            // classes. Requiring a statically provable inheritance relationship would reject
            // legal downcasts from broad engine properties such as Material.
            valid_conversion = left.kind == TypeKind::nil || left.kind == TypeKind::object;
        } else {
            valid_conversion = is_explicitly_convertible(target_type, left);
        }
        if (valid_conversion && is_constant_expression(*expression.operand(0))) {
            const auto constant_source = constant_value_type_of(*expression.operand(0), left);
            if (!constant_source.is_dynamic()) {
                const auto conversion = classify_conversion(target_type, constant_source);
                valid_conversion = conversion == ConversionKind::identity ||
                                   conversion == ConversionKind::implicit;
            }
        }
        const bool deterministic_runtime_failure =
            valid_conversion && !is_explicit_runtime_constructible(target_type, left);
        if (deterministic_runtime_failure) {
            diagnostics_.error(
                "GDS4156",
                "cast is accepted by Godot's analyzer but has no runtime constructor from " +
                    left.display_name() + " to " + target_type.display_name(),
                expression.span);
            valid_conversion = false;
        }
        if (!valid_conversion) {
            if (!deterministic_runtime_failure) {
                diagnostics_.error("GDS4075",
                                   "invalid cast: cannot convert " + left.display_name() + " to " +
                                       target_type.display_name(),
                                   expression.span);
            }
        }
        return target_type;
    }
    if (operation == "and" || operation == "or") {
        require_truthy_value(left, expression.operand(0)->span,
                             "left operand of '" + operation + "'");
        require_truthy_value(right, expression.operand(1)->span,
                             "right operand of '" + operation + "'");
        return {TypeKind::boolean, "bool"};
    }
    if (left.is_dynamic() || right.is_dynamic()) {
        return operation == "==" || operation == "!=" || operation == "<" || operation == "<=" ||
                       operation == ">" || operation == ">=" || operation == "in" ||
                       operation == "not in"
                   ? Type{TypeKind::boolean, "bool"}
                   : variant_type;
    }
    if ((operation == "==" || operation == "!=") &&
        ((left.kind == TypeKind::object && right.kind == TypeKind::object) ||
         (left.kind == TypeKind::object && right.kind == TypeKind::nil) ||
         (left.kind == TypeKind::nil && right.kind == TypeKind::object))) {
        return {TypeKind::boolean, "bool"};
    }
    if ((operation == "in" || operation == "not in") &&
        (right.kind == TypeKind::array || right.kind == TypeKind::dictionary)) {
        // Membership is a Variant operation in Godot even for typed containers. The runtime
        // constraint governs stored values, not the type of a lookup probe.
        return {TypeKind::boolean, "bool"};
    }

    const auto left_name = builtin_operator_type(left);
    const auto right_name = builtin_operator_type(right);
    const auto lookup_operation = operation == "not in" ? "in" : operation;
    auto* record = api_.find_builtin_operator(left_name, lookup_operation, right_name);
    if (!record)
        record = api_.find_builtin_operator(left_name, lookup_operation, "Variant");
    if (!record) {
        diagnostics_.error("GDS4005",
                           "operator '" + operation + "' is not defined for " +
                               left.display_name() + " and " + right.display_name(),
                           expression.span);
        return unknown_type;
    }
    return operation == "not in" ? Type{TypeKind::boolean, "bool"}
                                 : type_from_godot_api(record->return_type);
}

std::optional<Type> SemanticAnalyzer::narrowed_flow_type(const Type& current,
                                                         const Type& target) const {
    if (target.kind == TypeKind::unknown || target.kind == TypeKind::void_type ||
        target.kind == TypeKind::script_resource) {
        return std::nullopt;
    }
    if (current.is_dynamic())
        return target;
    if (current == target)
        return current;
    if (current.kind != TypeKind::object || target.kind != TypeKind::object)
        return std::nullopt;

    const auto inherits = [&](const Type& derived, const Type& base) {
        if (api_.inherits(derived.name, base.name))
            return true;
        if (script_symbols_ && script_symbols_->inherits(derived.name, base.name))
            return true;
        const auto* derived_script = find_script_class(derived.name);
        return derived_script && api_.inherits(derived_script->godot_base_type, base.name);
    };
    if (inherits(target, current))
        return target;
    if (inherits(current, target))
        return current;
    return std::nullopt;
}

ConditionalRefinements
SemanticAnalyzer::conditional_refinements(const ast::Expression& expression) const {
    if (expression.kind() == ast::ExpressionKind::unary && expression.value() == "not" &&
        expression.operand_count() == 1) {
        auto refinements = conditional_refinements(*expression.operand(0));
        std::swap(refinements.when_true, refinements.when_false);
        return refinements;
    }
    if (expression.kind() == ast::ExpressionKind::identifier) {
        const auto* symbol = model_.symbol_of(expression);
        if (symbol &&
            (symbol->kind == SymbolKind::local || symbol->kind == SymbolKind::parameter) &&
            symbol->identity != 0) {
            ConditionalRefinements result;
            result.when_true.non_null.insert(symbol->identity);
            return result;
        }
        return {};
    }
    if (expression.kind() != ast::ExpressionKind::binary || expression.operand_count() != 2)
        return {};

    if (expression.value() == "and" || expression.value() == "or") {
        const auto left = conditional_refinements(*expression.operand(0));
        const auto right = conditional_refinements(*expression.operand(1));
        if (expression.value() == "and") {
            const auto right_false_path = sequence_refinements(left.when_true, right.when_false);
            return {sequence_refinements(left.when_true, right.when_true),
                    common_refinements(left.when_false, right_false_path)};
        }
        const auto right_true_path = sequence_refinements(left.when_false, right.when_true);
        return {common_refinements(left.when_true, right_true_path),
                sequence_refinements(left.when_false, right.when_false)};
    }
    if (expression.value() == "==" || expression.value() == "!=") {
        const auto& left = *expression.operand(0);
        const auto& right = *expression.operand(1);
        const auto is_null = [](const ast::Expression& operand) {
            return operand.kind() == ast::ExpressionKind::literal &&
                   operand.literal_kind() == ast::LiteralKind::nil;
        };
        const auto* value = is_null(left) ? &right : is_null(right) ? &left : nullptr;
        const auto* symbol = value && value->kind() == ast::ExpressionKind::identifier
                                 ? model_.symbol_of(*value)
                                 : nullptr;
        if (symbol &&
            (symbol->kind == SymbolKind::local || symbol->kind == SymbolKind::parameter) &&
            symbol->identity != 0) {
            ConditionalRefinements result;
            auto& non_null =
                expression.value() == "!=" ? result.when_true.non_null : result.when_false.non_null;
            non_null.insert(symbol->identity);
            return result;
        }
    }
    if (expression.value() != "is" && expression.value() != "is not")
        return {};

    const auto& value = *expression.operand(0);
    if (value.kind() != ast::ExpressionKind::identifier)
        return {};
    const auto* symbol = model_.symbol_of(value);
    if (!symbol || (symbol->kind != SymbolKind::local && symbol->kind != SymbolKind::parameter) ||
        symbol->identity == 0) {
        return {};
    }
    const auto* target_resolution = model_.api_resolution_of(*expression.operand(1));
    if (!target_resolution ||
        (target_resolution->kind != ApiResolutionKind::type_reference &&
         target_resolution->kind != ApiResolutionKind::external_type_reference &&
         target_resolution->kind != ApiResolutionKind::script_type_reference &&
         target_resolution->kind != ApiResolutionKind::inner_type_reference)) {
        return {};
    }
    const auto target = model_.type_of(*expression.operand(1));
    const auto refined = narrowed_flow_type(model_.type_of(value), target);
    if (!refined)
        return {};
    ConditionalRefinements result;
    auto& outcome = expression.value() == "is" ? result.when_true : result.when_false;
    outcome.types.emplace(symbol->identity, *refined);
    outcome.non_null.insert(symbol->identity);
    return result;
}

Type SemanticAnalyzer::analyze_binary_tree(const ast::Expression& expression) {
    enum class Stage { enter, after_left, after_right };
    struct Frame {
        const ast::Expression* expression{nullptr};
        Stage stage{Stage::enter};
        std::optional<FlowTypeState> saved_flow;
    };
    std::vector<Frame> work{{&expression, Stage::enter, std::nullopt}};
    while (!work.empty()) {
        auto frame = std::move(work.back());
        work.pop_back();
        if (frame.expression->kind() != ast::ExpressionKind::binary) {
            (void)analyze_expression(*frame.expression);
            continue;
        }
        if (frame.stage == Stage::after_right) {
            const auto& left_expression = *frame.expression->operand(0);
            const auto& right_expression = *frame.expression->operand(1);
            const auto left = model_.type_of(left_expression);
            const auto right = model_.type_of(right_expression);
            model_.expression_types_[frame.expression] =
                resolve_binary_expression(*frame.expression, left, right);
            if (frame.saved_flow)
                flow_types_ = std::move(*frame.saved_flow);
            continue;
        }
        if (frame.stage == Stage::after_left) {
            const auto logical =
                frame.expression->value() == "and" || frame.expression->value() == "or";
            Frame completion{frame.expression, Stage::after_right, std::nullopt};
            if (logical) {
                completion.saved_flow = flow_types_;
                const auto left_refinements =
                    conditional_refinements(*frame.expression->operand(0));
                flow_types_.apply(frame.expression->value() == "and" ? left_refinements.when_true
                                                                     : left_refinements.when_false);
            }
            work.push_back(std::move(completion));
            work.push_back({frame.expression->operand(1).get(), Stage::enter, std::nullopt});
            continue;
        }

        work.push_back({frame.expression, Stage::after_left, std::nullopt});
        work.push_back({frame.expression->operand(0).get(), Stage::enter, std::nullopt});
    }
    return model_.type_of(expression);
}

Type SemanticAnalyzer::analyze_expression(const ast::Expression& expression) {
    Type result = unknown_type;
    const auto property_resolution = [this](const ApiResolutionKind kind,
                                            const GodotPropertyRecord& property) -> ApiResolution {
        ApiResolution resolution{kind,
                                 property.owner,
                                 property.getter,
                                 property.setter,
                                 api_.property_getter_type(property),
                                 0,
                                 0,
                                 false,
                                 property.direct,
                                 property.index};
        resolution.assignment_type = api_.property_setter_type(property);
        return resolution;
    };
    switch (expression.kind()) {
    case ast::ExpressionKind::literal:
        switch (expression.literal_kind()) {
        case ast::LiteralKind::nil:
            result = {TypeKind::nil, "null"};
            break;
        case ast::LiteralKind::boolean:
            result = {TypeKind::boolean, "bool"};
            break;
        case ast::LiteralKind::integer:
            result = {TypeKind::integer, "int"};
            break;
        case ast::LiteralKind::floating:
            result = {TypeKind::floating, "float"};
            break;
        case ast::LiteralKind::string:
            result = {TypeKind::string, "String"};
            break;
        case ast::LiteralKind::string_name:
            result = {TypeKind::string_name, "StringName"};
            break;
        case ast::LiteralKind::node_path:
            result = {TypeKind::builtin, "NodePath"};
            break;
        case ast::LiteralKind::none:
            result = unknown_type;
            break;
        }
        break;
    case ast::ExpressionKind::node_reference:
        diagnose_static_instance_access("node lookup", expression.value(), expression.span);
        result = variant_type;
        break;
    case ast::ExpressionKind::identifier: {
        if (expression.value().find('[') != std::string::npos) {
            result = type_from_name(expression.value(), expression.span);
            model_.api_resolutions_.emplace(
                &expression, ApiResolution{ApiResolutionKind::type_reference, expression.value(),
                                           "", "", result, 0, 0, false, true});
            break;
        }
        if (expression.value().find('.') != std::string::npos) {
            result = type_from_name(expression.value(), expression.span);
            if (const auto project_enum = find_project_enum(script_symbols_, expression.value());
                project_enum.enumeration) {
                model_.api_resolutions_.emplace(
                    &expression,
                    ApiResolution{ApiResolutionKind::script_enum_type,
                                  project_enum.native_owner + "::" + project_enum.enumeration->name,
                                  "", "", result, 0, 0, false, true});
            } else if (const auto external_enum =
                           find_external_enum(script_symbols_, expression.value());
                       external_enum.enumeration) {
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::global_enum_type, "0", "", "",
                                               result, 0, 0, false, true});
            } else if (const auto separator = expression.value().rfind('.');
                       separator != std::string::npos &&
                       api_.has_class_enum(expression.value().substr(0, separator),
                                           expression.value().substr(separator + 1))) {
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::global_enum_type,
                                               "godot::" + expression.value().substr(0, separator) +
                                                   "::" + expression.value().substr(separator + 1),
                                               "", "", result, 0, 0, false, true});
            } else if (const auto* inner = find_inner_class(expression.value())) {
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::inner_type_reference, inner->name,
                                               "", "", result, 0, 0, false, true});
            } else {
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::type_reference,
                                                              expression.value(), "", "", result, 0,
                                                              0, false, true});
            }
            break;
        }
        if (expression.value() == "self") {
            diagnose_static_instance_access("receiver", expression.value(), expression.span);
            result = current_inner_class_ ? Type{TypeKind::object, current_inner_class_->name}
                     : current_script_    ? Type{TypeKind::object, current_script_->script_name}
                                          : Type{TypeKind::object, base_type_};
            model_.non_null_expressions_.insert(&expression);
            break;
        }
        if (expression.value() == "super") {
            const auto* base_inner = current_inner_base_;
            const auto* base_script = !base_inner && current_inner_class_
                                          ? inner_script_base_of(*current_inner_class_)
                                      : !base_inner && script_symbols_ && current_script_
                                          ? script_symbols_->base_of(*current_script_)
                                          : nullptr;
            result = base_inner    ? Type{TypeKind::object, base_inner->name}
                     : base_script ? Type{TypeKind::object, base_script->script_name}
                                   : Type{TypeKind::object, base_type_};
            record_script_dependency(base_script);
            model_.api_resolutions_.emplace(
                &expression, ApiResolution{ApiResolutionKind::script_super,
                                           base_inner    ? base_inner->name
                                           : base_script ? base_script->native_class_name
                                                         : "godot::" + base_type_,
                                           "", "", result, 0, 0, false, true});
            break;
        }
        if (const auto* symbol = resolve(expression.value())) {
            if (symbol->kind == SymbolKind::field &&
                static_fields_.find(expression.value()) == static_fields_.end()) {
                diagnose_static_instance_access("field", expression.value(), expression.span);
            }
            result = symbol->kind == SymbolKind::function ? Type{TypeKind::builtin, "Callable"}
                                                          : symbol->type;
            if (!suppress_flow_refinements_ &&
                (symbol->kind == SymbolKind::local || symbol->kind == SymbolKind::parameter) &&
                symbol->identity != 0) {
                if (const auto* refined = flow_types_.find(symbol->identity))
                    result = *refined;
                if (flow_types_.is_non_null(symbol->identity))
                    model_.non_null_expressions_.insert(&expression);
            }
            model_.referenced_symbols_.emplace(&expression, *symbol);
            const bool external_member =
                script_symbols_ &&
                ((current_inner_class_ &&
                  script_symbols_->member_is_external(*current_inner_class_, expression.value())) ||
                 (!current_inner_class_ && current_script_ &&
                  script_symbols_->member_is_external(*current_script_, expression.value())));
            const auto* external_base =
                !external_member       ? nullptr
                : current_inner_class_ ? script_symbols_->external_base_of(*current_inner_class_)
                                       : script_symbols_->external_base_of(*current_script_);
            if (external_base)
                model_.referenced_extension_abis_.insert(external_base->provider_abi);
            if (external_base && symbol->kind == SymbolKind::function) {
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::external_callable,
                                                              external_base->name, "", "", result,
                                                              0, 0, false, false});
            } else if (external_base && symbol->kind == SymbolKind::signal) {
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::external_signal,
                                                              external_base->name, "", "", result,
                                                              0, 0, false, false});
            } else if (external_base && symbol->kind == SymbolKind::field) {
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::dynamic_property,
                                                              external_base->name, "", "", result,
                                                              0, 0, false, false});
            } else if (external_base && (symbol->kind == SymbolKind::constant ||
                                         symbol->kind == SymbolKind::enum_value)) {
                const auto* member =
                    script_symbols_->find_external_member(*external_base, expression.value());
                model_.api_resolutions_.emplace(
                    &expression,
                    ApiResolution{ApiResolutionKind::global_constant,
                                  member ? std::to_string(member->constant_value) : "0", "", "",
                                  result, 0, 0, false, true});
            } else if (symbol->kind == SymbolKind::function) {
                const bool is_static = script_function_is_static(expression.value());
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{is_static ? ApiResolutionKind::script_static_callable
                                                         : ApiResolutionKind::script_callable,
                                               "", "", "", result, 0, 0, false, false});
            }
            if (symbol->kind == SymbolKind::constant) {
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{symbol->storage == SymbolStorage::function_local
                                                   ? ApiResolutionKind::local_constant
                                                   : ApiResolutionKind::script_constant,
                                               "", "", "", result, 0, 0, false, true});
            }
            if (symbol->kind == SymbolKind::enum_value) {
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::enum_member, "",
                                                              "", "", result, 0, 0, false, true});
            }
            const bool static_field =
                symbol->kind == SymbolKind::field &&
                static_fields_.find(expression.value()) != static_fields_.end();
            const bool accessor_field =
                symbol->kind == SymbolKind::field &&
                accessor_fields_.find(expression.value()) != accessor_fields_.end() &&
                current_accessor_fields_.find(expression.value()) == current_accessor_fields_.end();
            if (static_field && accessor_field) {
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::script_property,
                                                              "", "_gdpp_get_" + expression.value(),
                                                              "_gdpp_set_" + expression.value(),
                                                              result, 0, 0, false, true});
            } else if (static_field) {
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::script_static_field, "", "", "",
                                               result, 0, 0, false, true});
            } else if (accessor_field) {
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::script_property,
                                                              "", "_gdpp_get_" + expression.value(),
                                                              "_gdpp_set_" + expression.value(),
                                                              result, 0, 0, false, false});
            }
            if (symbol->kind == SymbolKind::signal) {
                result = {TypeKind::builtin, "Signal"};
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::script_signal, "",
                                                              "", "", result, 0, 0, false, false});
            }
            if (symbol->kind == SymbolKind::enum_type) {
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::script_enum_type, result.name, "",
                                               "", result, 0, 0, false, true});
            }
        } else if (const auto alias = script_resource_aliases_.find(expression.value());
                   alias != script_resource_aliases_.end()) {
            result = {TypeKind::script_resource, alias->second->path};
            record_script_dependency(alias->second);
            model_.api_resolutions_.emplace(&expression,
                                            ApiResolution{ApiResolutionKind::script_resource,
                                                          alias->second->native_class_name, "", "",
                                                          result, 0, 0, false, true});
        } else if (expression.value() == "PI" || expression.value() == "TAU" ||
                   expression.value() == "INF" || expression.value() == "NAN") {
            result = {TypeKind::floating, "float"};
            const auto cpp_name = expression.value() == "PI"    ? "Math_PI"
                                  : expression.value() == "TAU" ? "Math_TAU"
                                  : expression.value() == "INF" ? "Math_INF"
                                                                : "Math_NAN";
            model_.api_resolutions_.emplace(
                &expression, ApiResolution{ApiResolutionKind::global_constant, cpp_name, "", "",
                                           result, 0, 0, false, true});
        } else if (const auto* class_constant =
                       api_.find_class_constant(base_type_, expression.value())) {
            result = {TypeKind::integer, "int"};
            model_.api_resolutions_.emplace(&expression,
                                            ApiResolution{ApiResolutionKind::global_constant,
                                                          std::to_string(class_constant->value), "",
                                                          "", result, 0, 0, false, true});
        } else if (const auto* constant = api_.find_global_constant(expression.value())) {
            result = {TypeKind::integer, "int"};
            model_.api_resolutions_.emplace(&expression,
                                            ApiResolution{ApiResolutionKind::global_constant,
                                                          std::to_string(constant->value), "", "",
                                                          result, 0, 0, false, true});
        } else if (api_.has_global_enum(expression.value())) {
            result = {TypeKind::enumeration, expression.value()};
            model_.api_resolutions_.emplace(
                &expression, ApiResolution{ApiResolutionKind::global_enum_type, expression.value(),
                                           "", "", result, 0, 0, false, true});
        } else if (const auto* enum_value = api_.find_global_enum_value(expression.value())) {
            result = {TypeKind::integer, "int"};
            model_.api_resolutions_.emplace(&expression,
                                            ApiResolution{ApiResolutionKind::global_enum_value,
                                                          std::to_string(enum_value->value), "", "",
                                                          result, 0, 0, false, true});
        } else if (const auto* inner = find_inner_class(expression.value())) {
            result = {TypeKind::object, inner->name};
            model_.api_resolutions_.emplace(
                &expression, ApiResolution{ApiResolutionKind::inner_type_reference, inner->name, "",
                                           "", result, 0, 0, false, true});
        } else if (const auto* autoload = script_symbols_
                                              ? script_symbols_->find_autoload(expression.value())
                                              : nullptr) {
            // Project globals live in the GDScript global namespace and may intentionally use a
            // name that also exists in extension_api.json. Match Godot's project lookup by
            // resolving autoloads before engine singletons and native classes.
            result = {TypeKind::object, autoload->script_name};
            record_script_dependency(autoload);
            model_.api_resolutions_.emplace(
                &expression,
                ApiResolution{ApiResolutionKind::script_autoload, autoload->native_class_name,
                              expression.value(), "", result, 0, 0, false, false});
        } else if (const auto* project_type = script_symbols_
                                                  ? script_symbols_->find_global(expression.value())
                                                  : nullptr) {
            result = {TypeKind::object, project_type->script_name};
            record_script_dependency(project_type);
            model_.api_resolutions_.emplace(&expression,
                                            ApiResolution{ApiResolutionKind::script_type_reference,
                                                          project_type->native_class_name, "", "",
                                                          result, 0, 0, false, true});
        } else if (const auto* external_type =
                       script_symbols_ ? script_symbols_->find_external(expression.value())
                                       : nullptr) {
            result = {TypeKind::object, external_type->name};
            model_.referenced_extension_abis_.insert(external_type->provider_abi);
            model_.api_resolutions_.emplace(
                &expression, ApiResolution{ApiResolutionKind::external_type_reference,
                                           external_type->name, "", "", result, 0, 0, false, true});
        } else if (const auto* singleton = api_.find_singleton(expression.value())) {
            result = {TypeKind::object, singleton->type};
            model_.api_resolutions_.emplace(&expression, ApiResolution{ApiResolutionKind::singleton,
                                                                       singleton->type, "", "",
                                                                       result, 0, 0, false, true});
        } else if (script_symbols_) {
            if (api_.find_signal(base_type_, expression.value())) {
                result = {TypeKind::builtin, "Signal"};
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::script_signal, base_type_, "", "",
                                               result, 0, 0, false, false});
            } else if (api_.find_method(base_type_, expression.value())) {
                result = {TypeKind::builtin, "Callable"};
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::script_callable, base_type_, "",
                                               "", result, 0, 0, false, false});
            } else if (const auto* engine_type = api_.find_class(expression.value())) {
                result = type_from_annotation(engine_type->name);
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::type_reference, engine_type->name,
                                               "", "", result, 0, 0, false, true});
            } else if (const auto* property = api_.find_property(base_type_, expression.value())) {
                auto resolution = property_resolution(ApiResolutionKind::property, *property);
                result = resolution.type;
                model_.api_resolutions_.emplace(&expression, std::move(resolution));
            }
        } else if (api_.find_signal(base_type_, expression.value())) {
            result = {TypeKind::builtin, "Signal"};
            model_.api_resolutions_.emplace(
                &expression, ApiResolution{ApiResolutionKind::script_signal, base_type_, "", "",
                                           result, 0, 0, false, false});
        } else if (api_.find_method(base_type_, expression.value())) {
            result = {TypeKind::builtin, "Callable"};
            model_.api_resolutions_.emplace(
                &expression, ApiResolution{ApiResolutionKind::script_callable, base_type_, "", "",
                                           result, 0, 0, false, false});
        } else if (const auto* type = api_.find_class(expression.value())) {
            result = type_from_annotation(type->name);
            model_.api_resolutions_.emplace(
                &expression, ApiResolution{ApiResolutionKind::type_reference, type->name, "", "",
                                           result, 0, 0, false, true});
        } else if (const auto* property = api_.find_property(base_type_, expression.value())) {
            auto resolution = property_resolution(ApiResolutionKind::property, *property);
            result = resolution.type;
            model_.api_resolutions_.emplace(&expression, std::move(resolution));
        }
        // GDExtensions may register named engine singletons that are intentionally absent from
        // Godot's official extension_api.json (for example GodotSteam's `Steam`). Offline AOT
        // compilation cannot load arbitrary project extensions, so preserve GDScript's global
        // singleton convention and resolve unknown PascalCase globals through Engine at runtime.
        if (model_.api_resolutions_.find(&expression) == model_.api_resolutions_.end() &&
            model_.referenced_symbols_.find(&expression) == model_.referenced_symbols_.end() &&
            !expression.value().empty() &&
            std::isupper(static_cast<unsigned char>(expression.value().front())) != 0) {
            result = variant_type;
            model_.api_resolutions_.emplace(
                &expression, ApiResolution{ApiResolutionKind::external_singleton,
                                           expression.value(), "", "", result, 0, 0, false, false});
        }
        if (model_.api_resolutions_.find(&expression) == model_.api_resolutions_.end() &&
            model_.referenced_symbols_.find(&expression) == model_.referenced_symbols_.end()) {
            diagnostics_.error("GDS4122", "unknown identifier '" + expression.value() + "'",
                               expression.span);
        }
        if (const auto* resolution = model_.api_resolution_of(expression);
            resolution && !instance_context_available_) {
            if (resolution->kind == ApiResolutionKind::property ||
                resolution->kind == ApiResolutionKind::script_property) {
                if (static_fields_.find(expression.value()) == static_fields_.end())
                    diagnose_static_instance_access("property", expression.value(),
                                                    expression.span);
            } else if (resolution->kind == ApiResolutionKind::script_signal) {
                diagnose_static_instance_access("signal", expression.value(), expression.span);
            } else if (resolution->kind == ApiResolutionKind::script_callable) {
                diagnose_static_instance_access("method", expression.value(), expression.span);
            }
        }
        break;
    }
    case ast::ExpressionKind::lambda:
        if (!expression.lambda()) {
            diagnostics_.error("GDS4098", "lambda expression is missing its function body",
                               expression.span);
            result = unknown_type;
        } else {
            analyze_lambda(*expression.lambda());
            result = {TypeKind::builtin, "Callable"};
        }
        break;
    case ast::ExpressionKind::unary: {
        const auto operand = analyze_expression(*expression.operand(0));
        if (expression.value() == "not") {
            require_truthy_value(operand, expression.operand(0)->span, "operand of 'not'");
            result = {TypeKind::boolean, "bool"};
        } else if (operand.is_dynamic()) {
            result = variant_type;
        } else {
            const auto operation = expression.value() == "+"   ? "unary+"
                                   : expression.value() == "-" ? "unary-"
                                                               : expression.value();
            const auto owner = builtin_operator_type(operand);
            const auto* record = api_.find_builtin_operator(owner, operation);
            if (!record) {
                diagnostics_.error("GDS4003",
                                   "operator '" + expression.value() + "' is not defined for " +
                                       operand.display_name(),
                                   expression.span);
                result = unknown_type;
            } else {
                result = type_from_godot_api(record->return_type);
            }
        }
        break;
    }
    case ast::ExpressionKind::await_expression: {
        ++await_operand_depth_;
        const auto awaited = analyze_expression(*expression.operand(0));
        --await_operand_depth_;
        const bool coroutine_call = model_.is_coroutine_call(*expression.operand(0));
        const bool can_suspend = coroutine_call || awaited.is_dynamic() ||
                                 (awaited.kind == TypeKind::builtin && awaited.name == "Signal");
        current_callable_suspends_ = current_callable_suspends_ || can_suspend;
        if (!in_function_) {
            diagnostics_.error("GDS4090", "await expressions are only valid inside functions",
                               expression.span);
        } else if (!await_expression_allowed_) {
            diagnostics_.error(
                "GDS4090",
                "await in this expression context requires a dedicated coroutine control-flow "
                "lowering that is not implemented yet",
                expression.span);
        }
        if (can_suspend && current_function_name_ == "_init") {
            diagnostics_.error("GDS4097", "_init cannot suspend on a signal", expression.span);
        }
        if (can_suspend && current_function_static_) {
            diagnostics_.error("GDS4091", "static functions cannot suspend on a signal yet",
                               expression.span);
        }
        if (can_suspend && expected_return_.kind != TypeKind::void_type &&
            !allow_dynamic_await_return_) {
            diagnostics_.error("GDS4092",
                               "an AOT function containing await must currently return void",
                               expression.span);
        }
        if (!can_suspend && !warning_is_ignored(active_warning_ignores_, "redundant_await"))
            diagnostics_.warning("GDS4093", "await operand does not suspend", expression.span);
        result = can_suspend ? variant_type : awaited;
        break;
    }
    case ast::ExpressionKind::binary:
        result = analyze_binary_tree(expression);
        break;
    case ast::ExpressionKind::conditional: {
        const auto condition_type = analyze_expression(*expression.operand(1));
        require_truthy_value(condition_type, expression.operand(1)->span,
                             "conditional expression condition");
        const auto refinements = conditional_refinements(*expression.operand(1));
        const auto entry_state = flow_types_;

        flow_types_.apply(refinements.when_true);
        const auto when_true = analyze_expression(*expression.operand(0));

        flow_types_ = entry_state;
        flow_types_.apply(refinements.when_false);
        const auto when_false = analyze_expression(*expression.operand(2));
        flow_types_ = entry_state;
        if (when_true == when_false) {
            result = when_true;
        } else if (when_true.is_numeric() && when_false.is_numeric()) {
            result = when_true.kind == TypeKind::floating || when_false.kind == TypeKind::floating
                         ? Type{TypeKind::floating, "float"}
                         : Type{TypeKind::integer, "int"};
        } else if (when_true.kind == TypeKind::nil && when_false.kind == TypeKind::object) {
            result = when_false;
        } else if (when_false.kind == TypeKind::nil && when_true.kind == TypeKind::object) {
            result = when_true;
        } else {
            result = variant_type;
        }
        break;
    }
    case ast::ExpressionKind::call: {
        const auto& callee = *expression.operand(0);
        const auto mark_coroutine_call = [&](const bool coroutine) {
            if (!coroutine)
                return;
            model_.coroutine_calls_.insert(&expression);
            if (await_operand_depth_ == 0 && discarded_expression_ != &expression) {
                diagnostics_.error("GDS4132",
                                   "coroutine results must be awaited or explicitly discarded",
                                   expression.span);
            }
        };
        std::vector<Type> argument_types;
        argument_types.reserve(expression.operand_count() - 1);
        for (std::size_t index = 1; index < expression.operand_count(); ++index)
            argument_types.push_back(analyze_expression(*expression.operand(index)));
        const auto argument_count = expression.operand_count() - 1;
        const auto* language_intrinsic = callee.kind() == ast::ExpressionKind::identifier
                                             ? IntrinsicRegistry::latest().find(callee.value())
                                             : nullptr;
        const auto resolve_method = [&](const GodotMethodRecord* method) {
            if (!method)
                return false;
            if (argument_count < method->required_arguments ||
                (!method->is_vararg && argument_count > method->maximum_arguments)) {
                diagnostics_.error("GDS4011",
                                   "method '" + std::string{method->name} + "' expects " +
                                       std::to_string(method->required_arguments) +
                                       (method->is_vararg ? " or more argument(s)"
                                        : method->required_arguments == method->maximum_arguments
                                            ? " argument(s)"
                                            : " to " + std::to_string(method->maximum_arguments) +
                                                  " argument(s)") +
                                       ", got " + std::to_string(argument_count),
                                   expression.span);
            }
            const auto checked_arguments =
                std::min(argument_count, static_cast<std::size_t>(method->maximum_arguments));
            for (std::size_t index = 0; index < checked_arguments; ++index) {
                if (const auto* argument = api_.argument(*method, index)) {
                    require_assignable(type_from_godot_api(argument->type), argument_types[index],
                                       expression.operand(index + 1)->span,
                                       "argument " + std::to_string(index + 1) + " of '" +
                                           method->name + "'");
                }
            }
            result = type_from_godot_api(method->return_type);
            model_.api_resolutions_.emplace(
                &callee, ApiResolution{ApiResolutionKind::method, method->owner, "", "", result,
                                       method->required_arguments, method->maximum_arguments,
                                       method->is_vararg, false});
            return true;
        };
        const auto resolve_constructor = [&](const std::string& name) {
            const auto* type_record = api_.find_class(name);
            if (!type_record || !type_record->builtin)
                return false;
            bool has_arity = false;
            for (std::size_t occurrence = 0;; ++occurrence) {
                const auto* constructor = api_.find_constructor(name, argument_count, occurrence);
                if (!constructor)
                    break;
                has_arity = true;
                bool compatible = true;
                for (std::size_t index = 0; index < argument_count; ++index) {
                    const auto* argument = api_.argument(*constructor, index);
                    if (!argument) {
                        compatible = false;
                        break;
                    }
                    const auto target = type_from_godot_api(argument->type);
                    const auto& source = argument_types[index];
                    const bool inherited = target.kind == TypeKind::object &&
                                           source.kind == TypeKind::object &&
                                           api_.inherits(source.name, target.name);
                    const bool packed_color = target.kind == TypeKind::builtin &&
                                              target.name == "Color" &&
                                              source.kind == TypeKind::integer;
                    const bool object_rid =
                        target.kind == TypeKind::builtin && target.name == "RID" &&
                        source.kind == TypeKind::object && api_.find_method(source.name, "get_rid");
                    if (!is_assignable(target, source) && !inherited && !packed_color &&
                        !object_rid) {
                        compatible = false;
                        break;
                    }
                }
                if (!compatible)
                    continue;
                result = type_from_annotation(name);
                model_.expression_types_[&callee] = result;
                model_.api_resolutions_.emplace(
                    &callee, ApiResolution{ApiResolutionKind::constructor, name, "", "", result,
                                           static_cast<std::uint16_t>(argument_count),
                                           static_cast<std::uint16_t>(argument_count), false, true,
                                           static_cast<std::int64_t>(occurrence)});
                return true;
            }
            diagnostics_.error(has_arity ? "GDS4013" : "GDS4014",
                               has_arity ? "no matching constructor for '" + name + "'"
                                         : "constructor for '" + name + "' does not accept " +
                                               std::to_string(argument_count) + " argument(s)",
                               expression.span);
            return true;
        };
        const auto resolve_utility = [&](const GodotUtilityFunctionRecord* function) {
            if (!function)
                return false;
            if (argument_count < function->required_arguments ||
                (!function->is_vararg && argument_count > function->maximum_arguments)) {
                diagnostics_.error(
                    "GDS4073",
                    "utility function '" + std::string{function->name} + "' expects " +
                        std::to_string(function->required_arguments) +
                        (function->is_vararg ? " or more argument(s)"
                         : function->required_arguments == function->maximum_arguments
                             ? " argument(s)"
                             : " to " + std::to_string(function->maximum_arguments) +
                                   " argument(s)") +
                        ", got " + std::to_string(argument_count),
                    expression.span);
            }
            const auto checked =
                std::min(argument_count, static_cast<std::size_t>(function->maximum_arguments));
            for (std::size_t index = 0; index < checked; ++index) {
                if (const auto* argument = api_.argument(*function, index)) {
                    require_assignable(type_from_godot_api(argument->type), argument_types[index],
                                       expression.operand(index + 1)->span,
                                       "argument " + std::to_string(index + 1) + " of '" +
                                           function->name + "'");
                }
            }
            result = type_from_godot_api(function->return_type);
            model_.api_resolutions_.emplace(
                &callee, ApiResolution{ApiResolutionKind::utility_function, function->name, "", "",
                                       result, function->required_arguments,
                                       function->maximum_arguments, function->is_vararg, true});
            return true;
        };
        const auto resolve_intrinsic = [&](const IntrinsicFeature& feature) {
            if (argument_count < feature.minimum_arguments ||
                argument_count > feature.maximum_arguments) {
                diagnostics_.error(feature.kind == IntrinsicKind::length ? "GDS4076" : "GDS4075",
                                   std::string{feature.name} + " expects " +
                                       (feature.minimum_arguments == feature.maximum_arguments
                                            ? "exactly " + std::to_string(feature.minimum_arguments)
                                            : std::to_string(feature.minimum_arguments) + " to " +
                                                  std::to_string(feature.maximum_arguments)) +
                                       " argument(s), got " + std::to_string(argument_count),
                                   expression.span);
            }
            const auto checked =
                std::min(argument_count, static_cast<std::size_t>(feature.maximum_arguments));
            for (std::size_t index = 0; index < checked; ++index) {
                const auto rule = feature.argument_rules[index];
                const auto context = "argument " + std::to_string(index + 1) + " of '" +
                                     std::string{feature.name} + "'";
                switch (rule) {
                case IntrinsicArgumentRule::any:
                case IntrinsicArgumentRule::resource_path:
                    break;
                case IntrinsicArgumentRule::integer:
                    require_assignable({TypeKind::integer, "int"}, argument_types[index],
                                       expression.operand(index + 1)->span, context);
                    break;
                case IntrinsicArgumentRule::string:
                    require_assignable({TypeKind::string, "String"}, argument_types[index],
                                       expression.operand(index + 1)->span, context);
                    break;
                case IntrinsicArgumentRule::string_name:
                    require_assignable({TypeKind::string_name, "StringName"}, argument_types[index],
                                       expression.operand(index + 1)->span, context);
                    break;
                case IntrinsicArgumentRule::type_descriptor: {
                    const auto& argument = *expression.operand(index + 1);
                    const auto* resolution = model_.api_resolution_of(argument);
                    const bool type_reference =
                        resolution &&
                        (resolution->kind == ApiResolutionKind::type_reference ||
                         resolution->kind == ApiResolutionKind::external_type_reference ||
                         resolution->kind == ApiResolutionKind::script_type_reference ||
                         resolution->kind == ApiResolutionKind::inner_type_reference);
                    if (!argument_types[index].is_dynamic() &&
                        argument_types[index].kind != TypeKind::integer && !type_reference &&
                        argument_types[index].kind != TypeKind::script_resource) {
                        diagnostics_.error(
                            "GDS4144",
                            context + " must be a TYPE_* constant, engine class, or script type",
                            argument.span);
                    }
                    break;
                }
                }
            }
            switch (feature.result_rule) {
            case IntrinsicResultRule::dynamic:
                result = variant_type;
                break;
            case IntrinsicResultRule::boolean:
                result = {TypeKind::boolean, "bool"};
                break;
            case IntrinsicResultRule::integer:
                result = {TypeKind::integer, "int"};
                break;
            case IntrinsicResultRule::string:
                result = {TypeKind::string, "String"};
                break;
            case IntrinsicResultRule::color:
                result = {TypeKind::builtin, "Color"};
                break;
            case IntrinsicResultRule::array:
                result = {TypeKind::array, "Array"};
                break;
            case IntrinsicResultRule::integer_array:
                result = {TypeKind::array, "Array[int]"};
                break;
            case IntrinsicResultRule::resource:
                result = variant_type;
                break;
            }
            ApiResolution resolution{
                ApiResolutionKind::intrinsic, std::string{feature.name}, "",    "",  result,
                feature.minimum_arguments,    feature.maximum_arguments, false, true};
            resolution.intrinsic = feature.kind;
            model_.api_resolutions_.insert_or_assign(&callee, std::move(resolution));
        };
        if (language_intrinsic && (language_intrinsic->kind == IntrinsicKind::preload ||
                                   language_intrinsic->kind == IntrinsicKind::load)) {
            const bool is_preload = language_intrinsic->kind == IntrinsicKind::preload;
            const auto constant_path = argument_count == 1
                                           ? constant_string_expression(*expression.operand(1))
                                           : std::optional<std::string>{};
            if (argument_count != 1 || (is_preload && !constant_path)) {
                diagnostics_.error("GDS4060",
                                   callee.value() +
                                       (is_preload
                                            ? " requires exactly one constant project resource path"
                                            : " requires exactly one resource path"),
                                   expression.span);
                break;
            }
            if (!constant_path) {
                require_assignable({TypeKind::string, "String"}, argument_types.front(),
                                   expression.operand(1)->span, "load resource path");
                result = variant_type;
                ApiResolution resolution{
                    ApiResolutionKind::intrinsic, "load", "", "", result, 1, 1, false, true};
                resolution.intrinsic = IntrinsicKind::load;
                model_.api_resolutions_.emplace(&callee, std::move(resolution));
                break;
            }
            ApiResolution intrinsic_resolution{ApiResolutionKind::intrinsic,
                                               std::string{language_intrinsic->name},
                                               "",
                                               "",
                                               variant_type,
                                               1,
                                               1,
                                               false,
                                               true};
            intrinsic_resolution.intrinsic = language_intrinsic->kind;
            model_.api_resolutions_.insert_or_assign(&callee, std::move(intrinsic_resolution));
            const auto& resource_path = *constant_path;
            const auto resolved_resource_path =
                script_symbols_
                    ? script_symbols_->resolve_resource_path(current_script_path_, resource_path)
                    : std::optional<std::string>{};
            const auto& effective_resource_path =
                resolved_resource_path ? *resolved_resource_path : resource_path;
            const auto* target =
                script_symbols_ ? script_symbols_->resolve_path(current_script_path_, resource_path)
                                : nullptr;
            if (target) {
                record_script_dependency(target);
                result = {TypeKind::script_resource, target->path};
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::script_resource,
                                                              target->native_class_name, "", "",
                                                              result, 1, 1, false, true});
            } else if ((effective_resource_path.size() >= 5 &&
                        effective_resource_path.compare(effective_resource_path.size() - 5, 5,
                                                        ".tscn") == 0) ||
                       (effective_resource_path.size() >= 4 &&
                        effective_resource_path.compare(effective_resource_path.size() - 4, 4,
                                                        ".scn") == 0)) {
                result = {TypeKind::object, "PackedScene"};
                auto& resolution = model_.api_resolutions_.at(&callee);
                resolution.type = result;
            } else if (effective_resource_path.size() >= 3 &&
                       effective_resource_path.compare(effective_resource_path.size() - 3, 3,
                                                       ".gd") == 0) {
                diagnostics_.error("GDS4061",
                                   "project script '" + resource_path + "' was not found for " +
                                       callee.value(),
                                   expression.span);
            } else if (resolved_resource_path ||
                       (resource_path.rfind("res://", 0) == 0 && resource_path.size() > 6)) {
                result = variant_type;
                model_.api_resolutions_.at(&callee).type = result;
            } else {
                diagnostics_.error("GDS4061",
                                   "invalid project resource path '" + resource_path + "' for " +
                                       callee.value(),
                                   expression.span);
            }
            break;
        }
        if (callee.kind() == ast::ExpressionKind::identifier) {
            if (callee.value() == "super") {
                const auto* base_inner = current_inner_base_;
                const auto* base_script = !base_inner && current_inner_class_
                                              ? inner_script_base_of(*current_inner_class_)
                                          : !base_inner && script_symbols_ && current_script_
                                              ? script_symbols_->base_of(*current_script_)
                                              : nullptr;
                const auto* external_base =
                    !base_inner && !base_script && script_symbols_
                        ? current_inner_class_
                              ? script_symbols_->external_base_of(*current_inner_class_)
                          : current_script_ && current_script_->attached
                              ? script_symbols_->external_base_of(*current_script_)
                              : nullptr
                        : nullptr;
                const auto* external_member =
                    external_base ? script_symbols_->find_external_member(*external_base,
                                                                          current_function_name_)
                                  : nullptr;
                if (external_member && external_member->kind == ScriptMemberKind::function) {
                    if (!external_member->has_method_hash) {
                        diagnostics_.error("GDS4160",
                                           "external super method '" + external_base->name + "." +
                                               current_function_name_ +
                                               "' has no reflected MethodBind compatibility hash",
                                           expression.span);
                        result = unknown_type;
                        break;
                    }
                    validate_script_call(*external_member, argument_types, expression,
                                         expression.span);
                    result = external_member->type;
                    model_.referenced_extension_abis_.insert(external_base->provider_abi);
                    model_.api_resolutions_[&callee] = ApiResolution{
                        ApiResolutionKind::external_super_method,
                        external_base->name,
                        "",
                        current_function_name_,
                        result,
                        static_cast<std::uint16_t>(external_member->required_arguments),
                        static_cast<std::uint16_t>(external_member->parameters.size()),
                        external_member->is_vararg,
                        true,
                        static_cast<std::int64_t>(external_member->method_hash)};
                    break;
                }
                const auto* member =
                    base_inner ? find_inner_member(*base_inner, current_function_name_)
                    : base_script
                        ? script_symbols_->find_member(*base_script, current_function_name_)
                        : nullptr;
                const auto* method =
                    member ? nullptr : api_.find_method(base_type_, current_function_name_);
                if (member && member->kind == ScriptMemberKind::function) {
                    if (member->is_abstract) {
                        diagnostics_.error("GDS4150",
                                           "cannot call abstract parent method '" +
                                               current_function_name_ +
                                               "' because it has no implementation",
                                           expression.span);
                    }
                    if (!instance_context_available_ && !member->is_static)
                        diagnose_static_instance_access("method", current_function_name_,
                                                        expression.span);
                    validate_script_call(*member, argument_types, expression, expression.span);
                    result = member->type;
                    mark_coroutine_call(member->is_coroutine);
                } else if (method) {
                    if (!instance_context_available_ && !method->is_static)
                        diagnose_static_instance_access("method", current_function_name_,
                                                        expression.span);
                    (void)resolve_method(method);
                } else {
                    diagnostics_.error("GDS4122",
                                       "super call has no base implementation for '" +
                                           current_function_name_ + "'",
                                       expression.span);
                    result = unknown_type;
                }
                model_.api_resolutions_[&callee] =
                    ApiResolution{ApiResolutionKind::script_super,
                                  base_inner    ? base_inner->name
                                  : base_script ? base_script->native_class_name
                                                : "godot::" + base_type_,
                                  method ? method->owner : "",
                                  current_function_name_,
                                  result,
                                  member   ? static_cast<std::uint16_t>(member->required_arguments)
                                  : method ? method->required_arguments
                                           : std::uint16_t{0},
                                  member   ? static_cast<std::uint16_t>(member->parameters.size())
                                  : method ? method->maximum_arguments
                                           : std::uint16_t{0},
                                  member ? member->is_vararg : method && method->is_vararg,
                                  true};
            } else if (language_intrinsic) {
                resolve_intrinsic(*language_intrinsic);
            } else if (resolve_utility(api_.find_utility_function(callee.value()))) {
            } else if (const auto* symbol = resolve(callee.value())) {
                result = symbol->type;
                model_.referenced_symbols_.emplace(&callee, *symbol);
                if (symbol->kind == SymbolKind::function) {
                    if (!script_function_is_static(callee.value()))
                        diagnose_static_instance_access("method", callee.value(), expression.span);
                    const auto local = functions_.find(callee.value());
                    const auto* member =
                        current_inner_class_
                            ? find_inner_member(*current_inner_class_, callee.value())
                        : script_symbols_ && current_script_
                            ? script_symbols_->find_member(*current_script_, callee.value())
                            : nullptr;
                    mark_coroutine_call(member ? member->is_coroutine
                                               : local != functions_.end() &&
                                                     contains_await_syntax(local->second->body));
                }
                if (symbol->kind == SymbolKind::function && script_symbols_ && current_script_) {
                    const auto* member =
                        current_inner_class_
                            ? find_inner_member(*current_inner_class_, callee.value())
                            : script_symbols_->find_member(*current_script_, callee.value());
                    if (member) {
                        validate_script_call(*member, argument_types, expression, expression.span);
                        result = member->type;
                        if (current_inner_class_) {
                            if (script_symbols_->member_is_external(*current_inner_class_,
                                                                    callee.value())) {
                                if (const auto* external =
                                        script_symbols_->external_base_of(*current_inner_class_)) {
                                    model_.referenced_extension_abis_.insert(
                                        external->provider_abi);
                                    model_.api_resolutions_.insert_or_assign(
                                        &callee,
                                        ApiResolution{
                                            ApiResolutionKind::dynamic_method, external->name, "",
                                            "", result,
                                            static_cast<std::uint16_t>(member->required_arguments),
                                            static_cast<std::uint16_t>(member->parameters.size()),
                                            member->is_vararg, false});
                                }
                                break;
                            }
                            const bool dynamic_dispatch =
                                !member->is_static && script_symbols_->requires_dynamic_dispatch(
                                                          *current_inner_class_, callee.value());
                            if (dynamic_dispatch) {
                                mark_coroutine_call(script_symbols_->may_dispatch_coroutine(
                                    *current_inner_class_, callee.value()));
                                model_.api_resolutions_.insert_or_assign(
                                    &callee, ApiResolution{ApiResolutionKind::dynamic_method, "",
                                                           "", "", result, 0, 0, true, false});
                            }
                            break;
                        }
                        const bool external_member =
                            script_symbols_->member_is_external(*current_script_, callee.value());
                        if (external_member) {
                            if (const auto* external =
                                    script_symbols_->external_base_of(*current_script_)) {
                                model_.referenced_extension_abis_.insert(external->provider_abi);
                                model_.api_resolutions_.insert_or_assign(
                                    &callee,
                                    ApiResolution{
                                        ApiResolutionKind::dynamic_method, external->name, "", "",
                                        result,
                                        static_cast<std::uint16_t>(member->required_arguments),
                                        static_cast<std::uint16_t>(member->parameters.size()),
                                        member->is_vararg, false});
                            }
                        } else if (!member->is_static && script_symbols_->requires_dynamic_dispatch(
                                                             *current_script_, callee.value())) {
                            mark_coroutine_call(script_symbols_->may_dispatch_coroutine(
                                *current_script_, callee.value()));
                            model_.api_resolutions_.emplace(
                                &callee, ApiResolution{ApiResolutionKind::dynamic_method, "", "",
                                                       "", result, 0, 0, true, false});
                        }
                    }
                } else if (symbol->kind == SymbolKind::function) {
                    if (const auto local = functions_.find(callee.value());
                        local != functions_.end()) {
                        validate_local_call(*local->second, argument_types, expression,
                                            expression.span);
                    }
                } else if (symbol->kind != SymbolKind::function) {
                    diagnostics_.error(
                        "GDS4070",
                        "value '" + callee.value() +
                            "' is not directly callable; Callable values must use .call(...)",
                        expression.span);
                    result = unknown_type;
                }
            } else if (const auto* method = api_.find_method(base_type_, callee.value())) {
                if (!method->is_static)
                    diagnose_static_instance_access("method", callee.value(), expression.span);
                (void)resolve_method(method);
            } else {
                if (!resolve_constructor(callee.value())) {
                    result = analyze_expression(callee);
                    if (result.kind == TypeKind::unknown) {
                        diagnostics_.error("GDS4071",
                                           "unknown function or callable '" + callee.value() + "'",
                                           expression.span);
                    }
                }
            }
        } else if (callee.kind() == ast::ExpressionKind::member) {
            const auto object_type = analyze_expression(*callee.operand(0));
            const auto* object_resolution = model_.api_resolution_of(*callee.operand(0));
            const bool called_on_super =
                object_resolution && object_resolution->kind == ApiResolutionKind::script_super;
            const auto* project_super = called_on_super && script_symbols_ && current_script_
                                            ? script_symbols_->base_of(*current_script_)
                                            : nullptr;
            const auto* external_super =
                called_on_super && !project_super && script_symbols_
                    ? current_inner_class_
                          ? script_symbols_->external_base_of(*current_inner_class_)
                      : current_script_ && current_script_->attached
                          ? script_symbols_->external_base_of(*current_script_)
                          : nullptr
                    : nullptr;
            if (external_super) {
                const auto* member =
                    script_symbols_->find_external_member(*external_super, callee.value());
                if (member && member->kind == ScriptMemberKind::function) {
                    if (!member->has_method_hash) {
                        diagnostics_.error("GDS4160",
                                           "external super method '" + external_super->name + "." +
                                               callee.value() +
                                               "' has no reflected MethodBind compatibility hash",
                                           expression.span);
                        result = unknown_type;
                        break;
                    }
                    validate_script_call(*member, argument_types, expression, expression.span);
                    result = member->type;
                    model_.referenced_extension_abis_.insert(external_super->provider_abi);
                    model_.api_resolutions_.emplace(
                        &callee,
                        ApiResolution{ApiResolutionKind::external_super_method,
                                      external_super->name, "", callee.value(), result,
                                      static_cast<std::uint16_t>(member->required_arguments),
                                      static_cast<std::uint16_t>(member->parameters.size()),
                                      member->is_vararg, true,
                                      static_cast<std::int64_t>(member->method_hash)});
                    break;
                }
            }
            if (object_resolution &&
                object_resolution->kind == ApiResolutionKind::external_type_reference) {
                if (callee.value() == "new") {
                    if (argument_count != 0) {
                        diagnostics_.error(
                            "GDS4063",
                            "runtime GDExtension new() does not accept constructor arguments",
                            expression.span);
                    }
                    result = object_type;
                    model_.api_resolutions_.emplace(
                        &callee,
                        ApiResolution{ApiResolutionKind::external_constructor,
                                      object_resolution->owner, "", "", result, 0, 0, false, true});
                    break;
                }
                const auto* external_owner =
                    script_symbols_ ? script_symbols_->find_external(object_resolution->owner)
                                    : nullptr;
                const auto* member =
                    external_owner
                        ? script_symbols_->find_external_member(*external_owner, callee.value())
                        : nullptr;
                if (!member || member->kind != ScriptMemberKind::function || !member->is_static) {
                    diagnostics_.error("GDS4056",
                                       "external type '" + object_resolution->owner +
                                           "' has no declared static method '" + callee.value() +
                                           "'",
                                       expression.span);
                    result = unknown_type;
                    break;
                }
                validate_script_call(*member, argument_types, expression, expression.span);
                result = member->type;
                model_.api_resolutions_.emplace(
                    &callee, ApiResolution{ApiResolutionKind::external_static_method,
                                           object_resolution->owner, "", "", result,
                                           static_cast<std::uint16_t>(member->required_arguments),
                                           static_cast<std::uint16_t>(member->parameters.size()),
                                           member->is_vararg, true});
                break;
            }
            if (object_type.kind == TypeKind::script_resource) {
                const auto* target =
                    script_symbols_ ? script_symbols_->find_path(object_type.name) : nullptr;
                if (!target) {
                    diagnostics_.error("GDS4062", "script resource metadata is unavailable",
                                       expression.span);
                    break;
                }
                record_script_dependency(target);
                if (callee.value() != "new") {
                    const auto* member = script_symbols_->find_member(*target, callee.value());
                    if (!member || member->kind != ScriptMemberKind::function) {
                        diagnostics_.error("GDS4055",
                                           "script resource '" + target->script_name +
                                               "' has no method '" + callee.value() + "'",
                                           expression.span);
                        result = unknown_type;
                        break;
                    }
                    if (!member->is_static) {
                        diagnostics_.error("GDS4056",
                                           "instance method '" + callee.value() +
                                               "' cannot be called on a script resource",
                                           expression.span);
                        result = unknown_type;
                        break;
                    }
                    validate_script_call(*member, argument_types, expression, expression.span);
                    result = member->type;
                    mark_coroutine_call(member->is_coroutine);
                    model_.api_resolutions_.emplace(
                        &callee,
                        ApiResolution{ApiResolutionKind::script_static_callable,
                                      target->native_class_name, "", "", result,
                                      static_cast<std::uint16_t>(member->required_arguments),
                                      static_cast<std::uint16_t>(member->parameters.size()),
                                      member->is_vararg, true});
                    break;
                }
                if (target->is_abstract) {
                    diagnostics_.error("GDS4111",
                                       "cannot instantiate abstract script class '" +
                                           target->script_name + "'",
                                       expression.span);
                }
                if (const auto* initializer = script_symbols_->find_member(*target, "_init")) {
                    validate_script_call(*initializer, argument_types, expression, expression.span);
                } else if (argument_count != 0) {
                    diagnostics_.error("GDS4063",
                                       "script new() received arguments but target has no _init",
                                       expression.span);
                }
                result = {TypeKind::object, target->script_name};
                model_.api_resolutions_.emplace(&callee,
                                                ApiResolution{ApiResolutionKind::script_constructor,
                                                              target->native_class_name, "", "",
                                                              result, 0, 0, false, true});
                break;
            }
            if (const auto* inner = object_type.kind == TypeKind::object
                                        ? find_inner_class(object_type.name)
                                        : nullptr) {
                const bool called_on_type =
                    object_resolution &&
                    object_resolution->kind == ApiResolutionKind::inner_type_reference;
                if (called_on_type && callee.value() == "new") {
                    if (inner->is_abstract) {
                        diagnostics_.error("GDS4111",
                                           "cannot instantiate abstract internal class '" +
                                               inner->name + "'",
                                           expression.span);
                    }
                    const auto initializer = std::find_if(
                        inner->members.begin(), inner->members.end(), [](const auto& member) {
                            return member.kind == ScriptMemberKind::function &&
                                   member.name == "_init";
                        });
                    if (initializer != inner->members.end()) {
                        validate_script_call(*initializer, argument_types, expression,
                                             expression.span);
                    } else if (argument_count != 0) {
                        diagnostics_.error(
                            "GDS4063",
                            "internal class new() received arguments but target has no _init",
                            expression.span);
                    }
                    const auto& inner_identity =
                        inner->native_class_name.empty() ? inner->name : inner->native_class_name;
                    result = {TypeKind::object, inner_identity};
                    model_.api_resolutions_.emplace(
                        &callee, ApiResolution{ApiResolutionKind::inner_constructor, inner_identity,
                                               "", "", result, 0, 0, false, true});
                    break;
                }
                const auto* member = find_inner_member(*inner, callee.value());
                if (!member && !called_on_type) {
                    result = variant_type;
                    model_.api_resolutions_.emplace(
                        &callee, ApiResolution{ApiResolutionKind::dynamic_method, "", "", "",
                                               result, 0, 0, true, false});
                    break;
                }
                if (!member || member->kind != ScriptMemberKind::function) {
                    diagnostics_.error("GDS4055",
                                       "internal class '" + inner->name + "' has no method '" +
                                           callee.value() + "'",
                                       expression.span);
                    result = unknown_type;
                    break;
                }
                if (called_on_type && !member->is_static) {
                    diagnostics_.error("GDS4056",
                                       "instance method '" + callee.value() +
                                           "' cannot be called on an internal class type",
                                       expression.span);
                } else if (!called_on_type && !called_on_super && member->is_static &&
                           !warning_is_ignored(active_warning_ignores_,
                                               "static_called_on_instance")) {
                    diagnostics_.warning("GDS4130",
                                         "static internal class method '" + callee.value() +
                                             "' is called on an instance",
                                         expression.span);
                }
                if (called_on_super && !member->is_static)
                    diagnose_static_instance_access("method", callee.value(), expression.span);
                if (called_on_super && member->is_abstract) {
                    diagnostics_.error("GDS4150",
                                       "cannot call abstract parent method '" + callee.value() +
                                           "' because it has no implementation",
                                       expression.span);
                }
                validate_script_call(*member, argument_types, expression, expression.span);
                result = member->type;
                const bool dynamic_dispatch =
                    !called_on_type && !called_on_super && !member->is_static && script_symbols_ &&
                    script_symbols_->requires_dynamic_dispatch(*inner, callee.value());
                mark_coroutine_call(member->is_coroutine ||
                                    (dynamic_dispatch && script_symbols_->may_dispatch_coroutine(
                                                             *inner, callee.value())));
                if (called_on_super) {
                    model_.api_resolutions_.insert_or_assign(
                        &callee,
                        ApiResolution{ApiResolutionKind::script_super, object_resolution->owner, "",
                                      callee.value(), result,
                                      static_cast<std::uint16_t>(member->required_arguments),
                                      static_cast<std::uint16_t>(member->parameters.size()),
                                      member->is_vararg, true});
                } else if (dynamic_dispatch) {
                    model_.api_resolutions_.insert_or_assign(
                        &callee, ApiResolution{ApiResolutionKind::dynamic_method, "", "", "",
                                               result, 0, 0, true, false});
                }
                break;
            }
            const ScriptClassSymbol* script_owner = nullptr;
            if (script_symbols_ && object_type.kind == TypeKind::object) {
                script_owner =
                    called_on_super && current_script_ ? script_symbols_->base_of(*current_script_)
                    : object_resolution &&
                            (object_resolution->kind == ApiResolutionKind::script_autoload ||
                             object_resolution->kind == ApiResolutionKind::script_type_reference)
                        ? script_symbols_->find_native_class(object_resolution->owner)
                        : find_script_class(object_type.name);
                if (!script_owner && callee.operand(0)->kind() == ast::ExpressionKind::identifier &&
                    callee.operand(0)->value() == "self") {
                    script_owner = current_script_;
                }
            }
            if (script_owner) {
                record_script_dependency(script_owner);
                const bool called_on_type =
                    object_resolution &&
                    object_resolution->kind == ApiResolutionKind::script_type_reference;
                if (called_on_type && callee.value() == "new") {
                    if (script_owner->is_abstract) {
                        diagnostics_.error("GDS4111",
                                           "cannot instantiate abstract script class '" +
                                               script_owner->script_name + "'",
                                           expression.span);
                    }
                    if (const auto* initializer =
                            script_symbols_->find_member(*script_owner, "_init")) {
                        validate_script_call(*initializer, argument_types, expression,
                                             expression.span);
                    } else if (argument_count != 0) {
                        diagnostics_.error(
                            "GDS4063", "script new() received arguments but target has no _init",
                            expression.span);
                    }
                    result = {TypeKind::object, script_owner->script_name};
                    model_.api_resolutions_.emplace(
                        &callee, ApiResolution{ApiResolutionKind::script_constructor,
                                               script_owner->native_class_name, "", "", result, 0,
                                               0, false, true});
                    break;
                }
                if (!called_on_type && callee.value() == "free") {
                    if (argument_count != 0)
                        diagnostics_.error("GDS4064", "free() does not accept arguments",
                                           expression.span);
                    result = void_type;
                    model_.api_resolutions_.emplace(
                        &callee, ApiResolution{ApiResolutionKind::script_free, "", "", "", result,
                                               0, 0, false, true});
                    break;
                }
                const auto* member = script_symbols_->find_member(*script_owner, callee.value());
                if (!member) {
                    if (!called_on_type) {
                        const auto* method =
                            api_.find_method(script_owner->godot_base_type, callee.value());
                        if (resolve_method(method)) {
                            if (script_owner->attached && !called_on_super) {
                                model_.api_resolutions_.insert_or_assign(
                                    &callee,
                                    ApiResolution{
                                        ApiResolutionKind::dynamic_method,
                                        script_owner->godot_base_type, "", "", result,
                                        method ? method->required_arguments : std::uint16_t{0},
                                        method ? method->maximum_arguments : std::uint16_t{0},
                                        method && method->is_vararg, false});
                            }
                            if (called_on_super && method && !method->is_static)
                                diagnose_static_instance_access("method", callee.value(),
                                                                expression.span);
                            if (called_on_super) {
                                model_.api_resolutions_[&callee] = ApiResolution{
                                    ApiResolutionKind::script_super,
                                    object_resolution->owner,
                                    method ? method->owner : "",
                                    "",
                                    result,
                                    method ? method->required_arguments : std::uint16_t{0},
                                    method ? method->maximum_arguments : std::uint16_t{0},
                                    method && method->is_vararg,
                                    true};
                            }
                            break;
                        }
                        if (GodotApi::for_version(latest_godot_version)
                                .find_method(script_owner->godot_base_type, callee.value())) {
                            diagnostics_.error("GDS4016",
                                               "method '" + callee.value() +
                                                   "' is not available on Godot type '" +
                                                   script_owner->godot_base_type +
                                                   "' for the selected target version",
                                               expression.span);
                            break;
                        }
                    }
                    if (!called_on_type && !called_on_super) {
                        result = variant_type;
                        model_.api_resolutions_.emplace(
                            &callee, ApiResolution{ApiResolutionKind::dynamic_method, "", "", "",
                                                   result, 0, 0, true, false});
                        break;
                    }
                    diagnostics_.error("GDS4055",
                                       "script type '" + script_owner->script_name +
                                           "' has no member '" + callee.value() + "'",
                                       expression.span);
                    break;
                }
                if (called_on_type && !member->is_static) {
                    diagnostics_.error("GDS4056",
                                       "instance member '" + callee.value() +
                                           "' cannot be called on a script type",
                                       expression.span);
                } else if (!called_on_type && !called_on_super && member->is_static &&
                           !warning_is_ignored(active_warning_ignores_,
                                               "static_called_on_instance")) {
                    diagnostics_.warning("GDS4130",
                                         "static method '" + callee.value() +
                                             "' is called on a script instance",
                                         expression.span);
                }
                if (called_on_super && !member->is_static)
                    diagnose_static_instance_access("method", callee.value(), expression.span);
                if (called_on_super && member->is_abstract) {
                    diagnostics_.error("GDS4150",
                                       "cannot call abstract parent method '" + callee.value() +
                                           "' because it has no implementation",
                                       expression.span);
                }
                validate_script_call(*member, argument_types, expression, expression.span);
                result = member->type;
                const bool explicit_self_receiver =
                    callee.operand(0)->kind() == ast::ExpressionKind::identifier &&
                    callee.operand(0)->value() == "self";
                const bool dynamic_dispatch =
                    !called_on_type && !called_on_super &&
                    ((script_owner->attached && !explicit_self_receiver) ||
                     script_symbols_->requires_dynamic_dispatch(*script_owner, callee.value()));
                mark_coroutine_call(member->is_coroutine ||
                                    (dynamic_dispatch && script_symbols_->may_dispatch_coroutine(
                                                             *script_owner, callee.value())));
                if (called_on_super) {
                    model_.api_resolutions_.emplace(
                        &callee,
                        ApiResolution{ApiResolutionKind::script_super, object_resolution->owner, "",
                                      "", result,
                                      static_cast<std::uint16_t>(member->required_arguments),
                                      static_cast<std::uint16_t>(member->parameters.size()),
                                      member->is_vararg, true});
                } else if (dynamic_dispatch) {
                    model_.api_resolutions_.emplace(
                        &callee, ApiResolution{ApiResolutionKind::dynamic_method, "", "", "",
                                               result, 0, 0, true, false});
                }
                break;
            }
            const auto* external_owner = object_type.kind == TypeKind::object && script_symbols_
                                             ? script_symbols_->find_external(object_type.name)
                                             : nullptr;
            if (external_owner) {
                if (const auto* member =
                        script_symbols_->find_external_member(*external_owner, callee.value())) {
                    if (member->kind != ScriptMemberKind::function) {
                        diagnostics_.error(
                            "GDS4070", "external member '" + callee.value() + "' is not callable",
                            expression.span);
                        result = unknown_type;
                    } else if (member->is_static) {
                        diagnostics_.error("GDS4057",
                                           "static external method '" + callee.value() +
                                               "' must be called on its type",
                                           expression.span);
                        result = unknown_type;
                    } else {
                        validate_script_call(*member, argument_types, expression, expression.span);
                        result = member->type;
                        model_.api_resolutions_.emplace(
                            &callee,
                            ApiResolution{ApiResolutionKind::dynamic_method, external_owner->name,
                                          "", "", result,
                                          static_cast<std::uint16_t>(member->required_arguments),
                                          static_cast<std::uint16_t>(member->parameters.size()),
                                          member->is_vararg, false});
                    }
                    break;
                }
                if (external_owner->members_complete) {
                    diagnostics_.error("GDS4112",
                                       "external type '" + external_owner->name +
                                           "' has no declared method '" + callee.value() + "'",
                                       expression.span);
                    result = unknown_type;
                    break;
                }
            }
            if (object_type.is_dynamic()) {
                result = variant_type;
                model_.api_resolutions_.emplace(&callee,
                                                ApiResolution{ApiResolutionKind::dynamic_method, "",
                                                              "", "", result, 0, 0, true, false});
                break;
            }
            std::string owner;
            if (object_type.kind == TypeKind::array)
                owner = "Array";
            else if (object_type.kind == TypeKind::dictionary)
                owner = "Dictionary";
            else if (object_type.kind == TypeKind::string)
                owner = "String";
            else if (object_type.kind == TypeKind::string_name)
                owner = "StringName";
            else if (object_type.kind == TypeKind::builtin || object_type.kind == TypeKind::object)
                owner = object_type.name;
            const auto* method = api_.find_method(owner, callee.value());
            const auto* latest_method =
                GodotApi::for_version(latest_godot_version).find_method(owner, callee.value());
            const bool called_on_type =
                object_resolution && object_resolution->kind == ApiResolutionKind::type_reference;
            if (called_on_type && object_type.kind == TypeKind::object && callee.value() == "new") {
                if (argument_count != 0) {
                    diagnostics_.error("GDS4077", "Godot object new() does not accept arguments",
                                       expression.span);
                }
                result = object_type;
                model_.api_resolutions_.emplace(&callee,
                                                ApiResolution{ApiResolutionKind::constructor, owner,
                                                              "", "", result, 0, 0, false, true});
                break;
            }
            const bool dynamically_typed_object =
                object_type.kind == TypeKind::object &&
                (api_.find_class(object_type.name) ||
                 (script_symbols_ && script_symbols_->find_external(object_type.name)));
            if (!method && !latest_method && dynamically_typed_object && !called_on_type) {
                result = variant_type;
                model_.api_resolutions_.emplace(&callee,
                                                ApiResolution{ApiResolutionKind::dynamic_method, "",
                                                              "", "", result, 0, 0, true, false});
                break;
            }
            if (!method && !owner.empty() && !object_type.is_dynamic()) {
                diagnostics_.error("GDS4016",
                                   "method '" + callee.value() +
                                       "' is not available on Godot type '" + owner +
                                       "' for the selected target version",
                                   expression.span);
            }
            if (method && called_on_type && !method->is_static) {
                diagnostics_.error("GDS4015",
                                   "instance method '" + callee.value() +
                                       "' cannot be called on a Godot type",
                                   expression.span);
            }
            if (method && !called_on_type && !called_on_super && method->is_static &&
                !warning_is_ignored(active_warning_ignores_, "static_called_on_instance")) {
                diagnostics_.warning("GDS4130",
                                     "static method '" + callee.value() +
                                         "' is called on a Godot value instance",
                                     expression.span);
            }
            if (method && called_on_super && !method->is_static)
                diagnose_static_instance_access("method", callee.value(), expression.span);
            if (method && !called_on_type)
                validate_container_method_call(object_type, callee.value(), argument_types,
                                               expression);
            if (resolve_method(method)) {
                if (called_on_super) {
                    model_.api_resolutions_[&callee] =
                        ApiResolution{ApiResolutionKind::script_super,
                                      object_resolution->owner,
                                      method ? method->owner : "",
                                      "",
                                      result,
                                      method ? method->required_arguments : std::uint16_t{0},
                                      method ? method->maximum_arguments : std::uint16_t{0},
                                      method && method->is_vararg,
                                      true};
                }
            } else {
                result = analyze_expression(callee);
            }
        } else {
            result = analyze_expression(callee);
            diagnostics_.error(
                "GDS4072",
                "expression is not directly callable; invoke a Callable through .call(...)",
                expression.span);
            result = unknown_type;
        }
        break;
    }
    case ast::ExpressionKind::member: {
        const auto object_type = analyze_expression(*expression.operand(0));
        if (object_type.kind == TypeKind::enumeration) {
            if (const auto* global =
                    api_.find_global_enum_value(object_type.name, expression.value())) {
                result = object_type;
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::global_enum_value,
                                                              std::to_string(global->value), "", "",
                                                              result, 0, 0, false, true});
                break;
            }
            if (const auto separator = object_type.name.rfind('.');
                separator != std::string::npos) {
                const auto owner = object_type.name.substr(0, separator);
                const auto enum_name = object_type.name.substr(separator + 1);
                if (const auto* class_value =
                        api_.find_class_enum_value(owner, enum_name, expression.value())) {
                    result = object_type;
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::global_enum_value,
                                                   std::to_string(class_value->value), "", "",
                                                   result, 0, 0, false, true});
                    break;
                }
            }
            const auto enumeration = enum_members_.find(object_type.name);
            const auto project_enum = find_project_enum(script_symbols_, object_type.name);
            const auto inner_enum = find_inner_enum(object_type.name);
            const auto external_enum = find_external_enum(script_symbols_, object_type.name);
            if (external_enum.enumeration) {
                const auto found =
                    std::find_if(external_enum.enumeration->entries.begin(),
                                 external_enum.enumeration->entries.end(), [&](const auto& entry) {
                                     return entry.name == expression.value();
                                 });
                if (found == external_enum.enumeration->entries.end()) {
                    diagnostics_.error("GDS4041",
                                       "enum '" + object_type.name + "' has no member '" +
                                           expression.value() + "'",
                                       expression.span);
                    result = unknown_type;
                } else {
                    result = object_type;
                    model_.referenced_extension_abis_.insert(external_enum.owner->provider_abi);
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::global_enum_value,
                                                   std::to_string(found->value), "", "", result, 0,
                                                   0, false, true});
                }
            } else if (inner_enum.enumeration) {
                const auto found = std::find_if(
                    inner_enum.enumeration->entries.begin(), inner_enum.enumeration->entries.end(),
                    [&](const auto& entry) { return entry.name == expression.value(); });
                if (found == inner_enum.enumeration->entries.end()) {
                    diagnostics_.error("GDS4041",
                                       "enum '" + object_type.name + "' has no member '" +
                                           expression.value() + "'",
                                       expression.span);
                    result = unknown_type;
                } else {
                    auto native_owner = inner_enum.owner->native_class_name;
                    if (native_owner.empty() && script_symbols_ && current_script_) {
                        if (const auto* published = script_symbols_->find_inner(
                                *current_script_, inner_enum.owner->name)) {
                            native_owner = published->native_class_name;
                        }
                    }
                    result = object_type;
                    model_.api_resolutions_.emplace(
                        &expression,
                        ApiResolution{ApiResolutionKind::enum_member,
                                      native_owner.empty()
                                          ? inner_enum.owner->name +
                                                "::" + inner_enum.enumeration->name
                                          : native_owner + "::" + inner_enum.enumeration->name,
                                      "", "", result, 0, 0, false, true});
                }
            } else if (enumeration == enum_members_.end() && !project_enum.enumeration) {
                diagnostics_.error("GDS4041",
                                   "enum '" + object_type.name + "' has no member '" +
                                       expression.value() + "'",
                                   expression.span);
                result = unknown_type;
            } else if (enumeration != enum_members_.end()) {
                const auto member = enumeration->second.find(expression.value());
                if (member == enumeration->second.end()) {
                    diagnostics_.error("GDS4041",
                                       "enum '" + object_type.name + "' has no member '" +
                                           expression.value() + "'",
                                       expression.span);
                    result = unknown_type;
                } else {
                    result = object_type;
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::enum_member, object_type.name,
                                                   "", "", result, 0, 0, false, true});
                }
            } else {
                const auto found =
                    std::find_if(project_enum.enumeration->entries.begin(),
                                 project_enum.enumeration->entries.end(), [&](const auto& entry) {
                                     return entry.name == expression.value();
                                 });
                if (found == project_enum.enumeration->entries.end()) {
                    diagnostics_.error("GDS4041",
                                       "enum '" + object_type.name + "' has no member '" +
                                           expression.value() + "'",
                                       expression.span);
                    result = unknown_type;
                } else {
                    result = object_type;
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::enum_member,
                                                   project_enum.native_owner +
                                                       "::" + project_enum.enumeration->name,
                                                   "", "", result, 0, 0, false, true});
                }
            }
            record_script_dependency(project_enum.owner);
            break;
        }
        const auto* object_resolution = model_.api_resolution_of(*expression.operand(0));
        if (object_resolution && object_resolution->kind == ApiResolutionKind::script_super) {
            diagnostics_.error("GDS4089", "super members can only be used as method calls",
                               expression.span);
            result = unknown_type;
            break;
        }
        if (object_type.is_dynamic()) {
            result = variant_type;
            model_.api_resolutions_.emplace(&expression,
                                            ApiResolution{ApiResolutionKind::dynamic_property, "",
                                                          "", "", result, 0, 0, false, false});
            break;
        }
        if (object_type.kind == TypeKind::script_resource) {
            const auto* target =
                script_symbols_ ? script_symbols_->find_path(object_type.name) : nullptr;
            if (!target) {
                diagnostics_.error("GDS4062", "script resource metadata is unavailable",
                                   expression.span);
                result = unknown_type;
                break;
            }
            record_script_dependency(target);
            if (const auto* inner = script_symbols_->find_inner(*target, expression.value())) {
                result = {TypeKind::object, inner->native_class_name};
                model_.api_resolutions_.emplace(
                    &expression,
                    ApiResolution{ApiResolutionKind::inner_type_reference, inner->native_class_name,
                                  "", "", result, 0, 0, false, true});
                break;
            }
            if (const auto* enumeration = script_symbols_->find_enum(*target, expression.value())) {
                result = {TypeKind::enumeration,
                          target->native_class_name + "::" + enumeration->name};
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::script_enum_type, result.name, "",
                                               "", result, 0, 0, false, true});
                break;
            }
            const auto* member = script_symbols_->find_member(*target, expression.value());
            if (!member) {
                diagnostics_.error("GDS4055",
                                   "script resource '" + target->script_name + "' has no member '" +
                                       expression.value() + "'",
                                   expression.span);
                result = unknown_type;
                break;
            }
            if (member->kind == ScriptMemberKind::constant) {
                result = member->type;
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::script_constant,
                                                              target->native_class_name, "", "",
                                                              result, 0, 0, false, true});
                break;
            }
            if (member->kind == ScriptMemberKind::enum_value) {
                result = member->type;
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::enum_member,
                                                              target->native_class_name, "", "",
                                                              result, 0, 0, false, true});
                break;
            }
            if (member->kind == ScriptMemberKind::field && member->is_static) {
                const bool runtime_static_field =
                    current_script_ && current_script_->is_tool && !target->is_tool;
                result = runtime_static_field ? variant_type : member->type;
                model_.api_resolutions_.emplace(
                    &expression,
                    ApiResolution{runtime_static_field
                                      ? ApiResolutionKind::script_runtime_static_field
                                      : ApiResolutionKind::script_property,
                                  target->native_class_name, "_gdpp_get_" + expression.value(),
                                  "_gdpp_set_" + expression.value(), result, 0, 0, false, true});
                break;
            }
            if (member->kind == ScriptMemberKind::function && member->is_static) {
                diagnostics_.error("GDS4096",
                                   "static script methods cannot be used as Callable values",
                                   expression.span);
            } else {
                diagnostics_.error("GDS4058",
                                   "instance member '" + expression.value() +
                                       "' cannot be accessed on a script resource",
                                   expression.span);
            }
            result = unknown_type;
            break;
        }
        if (const auto* inner = object_type.kind == TypeKind::object
                                    ? find_inner_class(object_type.name)
                                    : nullptr) {
            const bool accessed_on_type =
                object_resolution &&
                object_resolution->kind == ApiResolutionKind::inner_type_reference;
            const bool accessed_on_self =
                expression.operand(0)->kind() == ast::ExpressionKind::identifier &&
                expression.operand(0)->value() == "self";
            if (accessed_on_type || accessed_on_self) {
                if (const auto* nested = find_nested_inner_class(*inner, expression.value())) {
                    const auto& nested_identity = nested->native_class_name.empty()
                                                      ? nested->name
                                                      : nested->native_class_name;
                    result = {TypeKind::object, nested_identity};
                    model_.api_resolutions_.emplace(
                        &expression,
                        ApiResolution{ApiResolutionKind::inner_type_reference, nested_identity, "",
                                      "", result, 0, 0, false, true});
                    break;
                }
                if (const auto enumeration = find_inner_enum((inner->native_class_name.empty()
                                                                  ? inner->name
                                                                  : inner->native_class_name) +
                                                             "." + expression.value());
                    enumeration.enumeration) {
                    auto native_owner = enumeration.owner->native_class_name;
                    if (native_owner.empty() && script_symbols_ && current_script_) {
                        if (const auto* published = script_symbols_->find_inner(
                                *current_script_, enumeration.owner->name)) {
                            native_owner = published->native_class_name;
                        }
                    }
                    result = {TypeKind::enumeration,
                              native_owner.empty()
                                  ? enumeration.owner->name + "." + enumeration.enumeration->name
                                  : native_owner + "::" + enumeration.enumeration->name};
                    model_.api_resolutions_.emplace(
                        &expression,
                        ApiResolution{ApiResolutionKind::script_enum_type,
                                      native_owner.empty()
                                          ? enumeration.owner->name +
                                                "::" + enumeration.enumeration->name
                                          : native_owner + "::" + enumeration.enumeration->name,
                                      "", "", result, 0, 0, false, true});
                    break;
                }
            }
            const auto* found = find_inner_member(*inner, expression.value());
            if (!found) {
                if (accessed_on_type) {
                    diagnostics_.error("GDS4055",
                                       "internal class '" + inner->name + "' has no member '" +
                                           expression.value() + "'",
                                       expression.span);
                    result = unknown_type;
                } else {
                    result = variant_type;
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::dynamic_property, "", "", "",
                                                   result, 0, 0, false, false});
                }
                break;
            }
            if (found->kind == ScriptMemberKind::function) {
                if (accessed_on_type || found->is_static) {
                    diagnostics_.error("GDS4096",
                                       "only instance internal methods can be Callable values",
                                       expression.span);
                    result = unknown_type;
                } else {
                    result = {TypeKind::builtin, "Callable"};
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::script_callable, inner->name,
                                                   "", "", result, 0, 0, false, false});
                }
                break;
            }
            if (found->kind == ScriptMemberKind::enum_value) {
                result = found->type;
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::enum_member,
                                                              inner->native_class_name, "", "",
                                                              result, 0, 0, false, true});
                break;
            }
            if (found->kind == ScriptMemberKind::constant) {
                result = found->type;
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::script_constant,
                                                              inner->native_class_name.empty()
                                                                  ? inner->name
                                                                  : inner->native_class_name,
                                                              "", "", result, 0, 0, false, true});
                break;
            }
            if (found->kind == ScriptMemberKind::signal && !accessed_on_type) {
                result = found->type;
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::script_signal, inner->name, "",
                                               "", result, 0, 0, false, false});
                break;
            }
            if (found->kind != ScriptMemberKind::field) {
                diagnostics_.error("GDS4058", "invalid internal class member access",
                                   expression.span);
                result = unknown_type;
                break;
            }
            if (accessed_on_type && !found->is_static) {
                diagnostics_.error("GDS4058",
                                   "instance field '" + expression.value() +
                                       "' cannot be accessed on an internal class type",
                                   expression.span);
                result = unknown_type;
                break;
            }
            result = found->type;
            model_.api_resolutions_.emplace(
                &expression,
                ApiResolution{ApiResolutionKind::script_property, inner->name,
                              "_gdpp_get_" + expression.value(), "_gdpp_set_" + expression.value(),
                              result, 0, 0, false, false});
            break;
        }
        const ScriptClassSymbol* script_owner = nullptr;
        if (script_symbols_ && object_type.kind == TypeKind::object) {
            script_owner =
                object_resolution &&
                        (object_resolution->kind == ApiResolutionKind::script_autoload ||
                         object_resolution->kind == ApiResolutionKind::script_type_reference)
                    ? script_symbols_->find_native_class(object_resolution->owner)
                    : find_script_class(object_type.name);
            if (!script_owner && expression.operand(0)->kind() == ast::ExpressionKind::identifier &&
                expression.operand(0)->value() == "self") {
                script_owner = current_script_;
            }
        }
        if (script_owner) {
            record_script_dependency(script_owner);
            const bool accessed_on_type =
                object_resolution &&
                object_resolution->kind == ApiResolutionKind::script_type_reference;
            if (const auto* enumeration =
                    script_symbols_->find_enum(*script_owner, expression.value())) {
                result = {TypeKind::enumeration,
                          script_owner->native_class_name + "::" + enumeration->name};
                model_.api_resolutions_.emplace(
                    &expression,
                    ApiResolution{ApiResolutionKind::script_enum_type,
                                  script_owner->native_class_name + "::" + enumeration->name, "",
                                  "", result, 0, 0, false, true});
                break;
            }
            const auto* member = script_symbols_->find_member(*script_owner, expression.value());
            if (!member) {
                if (accessed_on_type &&
                    api_.has_class_enum(script_owner->godot_base_type, expression.value())) {
                    result = {TypeKind::enumeration,
                              script_owner->godot_base_type + "." + expression.value()};
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::global_enum_type,
                                                   "godot::" + script_owner->godot_base_type +
                                                       "::" + expression.value(),
                                                   "", "", result, 0, 0, false, true});
                    break;
                }
                if (const auto* constant = api_.find_class_constant(script_owner->godot_base_type,
                                                                    expression.value())) {
                    result = {TypeKind::integer, "int"};
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::global_constant,
                                                   std::to_string(constant->value), "", "", result,
                                                   0, 0, false, true});
                    break;
                }
                if (!accessed_on_type) {
                    if (api_.find_signal(script_owner->godot_base_type, expression.value())) {
                        result = {TypeKind::builtin, "Signal"};
                        model_.api_resolutions_.emplace(
                            &expression, ApiResolution{ApiResolutionKind::script_signal,
                                                       script_owner->godot_base_type, "", "",
                                                       result, 0, 0, false, false});
                        break;
                    }
                    if (const auto* property =
                            api_.find_property(script_owner->godot_base_type, expression.value())) {
                        auto resolution = property_resolution(
                            script_owner->attached ? ApiResolutionKind::dynamic_property
                                                   : ApiResolutionKind::property,
                            *property);
                        result = resolution.type;
                        model_.api_resolutions_.emplace(&expression, std::move(resolution));
                        break;
                    }
                }
                if (!accessed_on_type) {
                    result = variant_type;
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::dynamic_property, "", "", "",
                                                   result, 0, 0, false, false});
                    break;
                }
                diagnostics_.error("GDS4055",
                                   "script type '" + script_owner->script_name +
                                       "' has no member '" + expression.value() + "'",
                                   expression.span);
                result = unknown_type;
                break;
            }
            if (accessed_on_type && member->kind == ScriptMemberKind::constant) {
                result = member->type;
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::script_constant,
                                                              script_owner->native_class_name, "",
                                                              "", result, 0, 0, false, true});
                break;
            }
            if (member->kind == ScriptMemberKind::enum_value) {
                result = member->type;
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::enum_member,
                                                              script_owner->native_class_name, "",
                                                              "", result, 0, 0, false, true});
                break;
            }
            if (member->kind == ScriptMemberKind::constant) {
                // Godot permits class constants through script instances and Autoload names.
                // Lower them to the native class scope so no singleton lookup is required.
                result = member->type;
                model_.api_resolutions_.emplace(&expression,
                                                ApiResolution{ApiResolutionKind::script_constant,
                                                              script_owner->native_class_name, "",
                                                              "", result, 0, 0, false, true});
                break;
            }
            if (accessed_on_type && member->kind == ScriptMemberKind::field && !member->is_static) {
                diagnostics_.error("GDS4058",
                                   "instance field '" + expression.value() +
                                       "' cannot be accessed on a script type",
                                   expression.span);
                result = unknown_type;
                break;
            }
            if (member->kind == ScriptMemberKind::signal) {
                if (accessed_on_type) {
                    diagnostics_.error("GDS4094", "signals cannot be accessed on a script type",
                                       expression.span);
                    result = unknown_type;
                } else {
                    result = {TypeKind::builtin, "Signal"};
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::script_signal,
                                                   script_owner->native_class_name, "", "", result,
                                                   0, 0, false, false});
                }
                break;
            }
            if (member->kind == ScriptMemberKind::function) {
                if (accessed_on_type || member->is_static) {
                    diagnostics_.error(
                        "GDS4096", "only instance script methods can be used as Callable values",
                        expression.span);
                    result = unknown_type;
                } else {
                    result = {TypeKind::builtin, "Callable"};
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::script_callable,
                                                   script_owner->native_class_name, "", "", result,
                                                   0, 0, false, false});
                }
                break;
            }
            if (member->kind != ScriptMemberKind::field) {
                diagnostics_.error("GDS4058",
                                   "script member '" + expression.value() +
                                       "' is not an instance field",
                                   expression.span);
                result = unknown_type;
                break;
            }
            const bool runtime_static_field = current_script_ && current_script_->is_tool &&
                                              !script_owner->is_tool && accessed_on_type &&
                                              member->is_static;
            const bool attached_instance_field =
                script_owner->attached && !accessed_on_type && !member->is_static;
            result = runtime_static_field ? variant_type : member->type;
            model_.api_resolutions_.emplace(
                &expression,
                ApiResolution{runtime_static_field ? ApiResolutionKind::script_runtime_static_field
                              : attached_instance_field ? ApiResolutionKind::dynamic_property
                                                        : ApiResolutionKind::script_property,
                              script_owner->native_class_name, "_gdpp_get_" + expression.value(),
                              "_gdpp_set_" + expression.value(), result, 0, 0, false, false});
            break;
        }
        if (const auto* external_owner = object_type.kind == TypeKind::object && script_symbols_
                                             ? script_symbols_->find_external(object_type.name)
                                             : nullptr) {
            const bool accessed_on_type =
                object_resolution &&
                object_resolution->kind == ApiResolutionKind::external_type_reference;
            if (accessed_on_type) {
                if (const auto* enumeration =
                        script_symbols_->find_external_enum(*external_owner, expression.value())) {
                    result = {TypeKind::enumeration,
                              external_owner->name + "." + enumeration->name};
                    model_.referenced_extension_abis_.insert(external_owner->provider_abi);
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::global_enum_type, "0", "", "",
                                                   result, 0, 0, false, true});
                    break;
                }
                for (const auto& enumeration : external_owner->enums) {
                    const auto found = std::find_if(
                        enumeration.entries.begin(), enumeration.entries.end(),
                        [&](const auto& entry) { return entry.name == expression.value(); });
                    if (found == enumeration.entries.end())
                        continue;
                    result = {TypeKind::enumeration, external_owner->name + "." + enumeration.name};
                    model_.referenced_extension_abis_.insert(external_owner->provider_abi);
                    model_.api_resolutions_.emplace(
                        &expression, ApiResolution{ApiResolutionKind::global_enum_value,
                                                   std::to_string(found->value), "", "", result, 0,
                                                   0, false, true});
                    break;
                }
                if (result.kind == TypeKind::enumeration)
                    break;
            }
            if (const auto* member =
                    script_symbols_->find_external_member(*external_owner, expression.value())) {
                if (member->kind == ScriptMemberKind::constant) {
                    if (!accessed_on_type) {
                        diagnostics_.error("GDS4058",
                                           "external constant '" + expression.value() +
                                               "' must be accessed on its type",
                                           expression.span);
                        result = unknown_type;
                    } else {
                        result = member->type;
                        model_.api_resolutions_.emplace(
                            &expression, ApiResolution{ApiResolutionKind::global_constant,
                                                       std::to_string(member->constant_value), "",
                                                       "", result, 0, 0, false, true});
                    }
                } else if (accessed_on_type) {
                    diagnostics_.error("GDS4058",
                                       "external instance member '" + expression.value() +
                                           "' cannot be accessed on its type",
                                       expression.span);
                    result = unknown_type;
                } else if (member->kind == ScriptMemberKind::function) {
                    result = {TypeKind::builtin, "Callable"};
                    model_.api_resolutions_.emplace(
                        &expression,
                        ApiResolution{ApiResolutionKind::external_callable, external_owner->name,
                                      "", "", result, 0, 0, false, false});
                } else if (member->kind == ScriptMemberKind::signal) {
                    result = {TypeKind::builtin, "Signal"};
                    model_.api_resolutions_.emplace(
                        &expression,
                        ApiResolution{ApiResolutionKind::external_signal, external_owner->name, "",
                                      "", result, 0, 0, false, false});
                } else {
                    result = member->type;
                    ApiResolution resolution{ApiResolutionKind::dynamic_property,
                                             external_owner->name,
                                             "",
                                             "",
                                             result,
                                             0,
                                             0,
                                             false,
                                             false};
                    resolution.read_only = member->read_only;
                    model_.api_resolutions_.emplace(&expression, std::move(resolution));
                }
                break;
            }
            if (external_owner->members_complete) {
                diagnostics_.error("GDS4112",
                                   "external type '" + external_owner->name +
                                       "' has no declared member '" + expression.value() + "'",
                                   expression.span);
                result = unknown_type;
                break;
            }
        }
        std::string owner;
        if (object_type.kind == TypeKind::array)
            owner = "Array";
        else if (object_type.kind == TypeKind::dictionary)
            owner = "Dictionary";
        else if (object_type.kind == TypeKind::string)
            owner = "String";
        else if (object_type.kind == TypeKind::string_name)
            owner = "StringName";
        else if (object_type.kind == TypeKind::builtin || object_type.kind == TypeKind::object)
            owner = object_type.name;
        const bool accessed_on_type =
            object_resolution && object_resolution->kind == ApiResolutionKind::type_reference;
        if (accessed_on_type) {
            if (api_.has_class_enum(owner, expression.value())) {
                result = {TypeKind::enumeration, owner + "." + expression.value()};
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::global_enum_type,
                                               "godot::" + owner + "::" + expression.value(), "",
                                               "", result, 0, 0, false, true});
                break;
            }
            if (const auto* constant = api_.find_builtin_constant(owner, expression.value())) {
                result = type_from_godot_api(constant->type);
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::builtin_constant, constant->value,
                                               "", "", result, 0, 0, false, true});
                break;
            }
        }
        if (const auto* constant = api_.find_class_constant(owner, expression.value())) {
            result = {TypeKind::integer, "int"};
            model_.api_resolutions_.emplace(&expression,
                                            ApiResolution{ApiResolutionKind::global_constant,
                                                          std::to_string(constant->value), "", "",
                                                          result, 0, 0, false, true});
            break;
        }
        if (!accessed_on_type && api_.find_signal(owner, expression.value())) {
            result = {TypeKind::builtin, "Signal"};
            model_.api_resolutions_.emplace(&expression,
                                            ApiResolution{ApiResolutionKind::script_signal, owner,
                                                          "", "", result, 0, 0, false, false});
        } else if (const auto* property = api_.find_property(owner, expression.value())) {
            auto resolution = property_resolution(ApiResolutionKind::property, *property);
            result = resolution.type;
            model_.api_resolutions_.emplace(&expression, std::move(resolution));
        } else if (object_type.kind == TypeKind::dictionary) {
            // GDScript supports dictionary.key as syntax sugar for dictionary["key"].
            result = variant_type;
            model_.api_resolutions_.emplace(&expression,
                                            ApiResolution{ApiResolutionKind::dynamic_property, "",
                                                          "", "", result, 0, 0, false, false});
        } else {
            const bool dynamically_typed_object =
                object_type.kind == TypeKind::object &&
                (api_.find_class(object_type.name) ||
                 (script_symbols_ && script_symbols_->find_external(object_type.name)));
            if (dynamically_typed_object && !accessed_on_type) {
                result = variant_type;
                model_.api_resolutions_.emplace(
                    &expression, ApiResolution{ApiResolutionKind::dynamic_property, "", "", "",
                                               result, 0, 0, false, false});
            } else {
                result = unknown_type;
            }
        }
        break;
    }
    case ast::ExpressionKind::subscript: {
        const auto container = analyze_expression(*expression.operand(0));
        const auto index = analyze_expression(*expression.operand(1));
        if (container.kind == TypeKind::array || container.kind == TypeKind::string ||
            container.is_packed_array()) {
            require_assignable({TypeKind::integer, "int"}, index, expression.operand(1)->span,
                               "container index");
        } else if (container.kind == TypeKind::dictionary) {
            if (const auto descriptor = describe_container_type(container)) {
                const auto key_type = type_from_name(descriptor->arguments.at(0), expression.span);
                require_expression_assignable(key_type, *expression.operand(1), index,
                                              expression.operand(1)->span, "dictionary key");
            }
        }
        result = container_element_type(container, expression.span);
        break;
    }
    case ast::ExpressionKind::array_literal:
        for (std::size_t index = 0; index < expression.operand_count(); ++index)
            (void)analyze_expression(*expression.operand(index));
        result = {TypeKind::array, "Array"};
        break;
    case ast::ExpressionKind::dictionary_literal:
        for (std::size_t index = 0; index < expression.operand_count(); ++index)
            (void)analyze_expression(*expression.operand(index));
        result = {TypeKind::dictionary, "Dictionary"};
        break;
    }
    model_.expression_types_[&expression] = result;
    return result;
}

bool SemanticAnalyzer::is_constant_expression(const ast::Expression& expression) const {
    switch (expression.kind()) {
    case ast::ExpressionKind::literal:
        return true;
    case ast::ExpressionKind::identifier: {
        const auto* symbol = model_.symbol_of(expression);
        if (symbol &&
            (symbol->kind == SymbolKind::constant || symbol->kind == SymbolKind::enum_value)) {
            return true;
        }
        const auto* resolution = model_.api_resolution_of(expression);
        return resolution && (resolution->kind == ApiResolutionKind::global_constant ||
                              resolution->kind == ApiResolutionKind::global_enum_value ||
                              resolution->kind == ApiResolutionKind::type_reference ||
                              resolution->kind == ApiResolutionKind::external_type_reference ||
                              resolution->kind == ApiResolutionKind::script_type_reference ||
                              resolution->kind == ApiResolutionKind::inner_type_reference ||
                              resolution->kind == ApiResolutionKind::global_enum_type ||
                              resolution->kind == ApiResolutionKind::script_enum_type);
    }
    case ast::ExpressionKind::member: {
        const auto* resolution = model_.api_resolution_of(expression);
        if (resolution && (resolution->kind == ApiResolutionKind::enum_member ||
                           resolution->kind == ApiResolutionKind::script_constant ||
                           resolution->kind == ApiResolutionKind::builtin_constant ||
                           resolution->kind == ApiResolutionKind::global_constant ||
                           resolution->kind == ApiResolutionKind::global_enum_value)) {
            return true;
        }
        // Godot reduces named access on a constant value (for example Vector2(1, 2).x).
        return expression.operand_count() == 1 && is_constant_expression(*expression.operand(0));
    }
    case ast::ExpressionKind::unary:
        return expression.operand_count() == 1 && is_constant_expression(*expression.operand(0));
    case ast::ExpressionKind::await_expression:
        return false;
    case ast::ExpressionKind::binary:
        if (expression.operand_count() != 2)
            return false;
        // Godot deliberately does not fold binary operators over shared Variant values because
        // doing so would expose one mutable reference as a compile-time default.
        if (is_shared_constant_type(model_.type_of(*expression.operand(0))) ||
            is_shared_constant_type(model_.type_of(*expression.operand(1)))) {
            return false;
        }
        return is_constant_expression(*expression.operand(0)) &&
               is_constant_expression(*expression.operand(1));
    case ast::ExpressionKind::conditional:
        for (std::size_t index = 0; index < expression.operand_count(); ++index) {
            if (!is_constant_expression(*expression.operand(index)))
                return false;
        }
        return true;
    case ast::ExpressionKind::call: {
        if (expression.operand_count() == 0)
            return false;
        for (std::size_t index = 1; index < expression.operand_count(); ++index) {
            if (!is_constant_expression(*expression.operand(index)))
                return false;
        }
        const auto& callee = *expression.operand(0);
        const auto* resolution = model_.api_resolution_of(callee);
        if (!resolution)
            return false;
        if (resolution->kind == ApiResolutionKind::constructor)
            return !is_shared_constant_type(resolution->type);
        if (resolution->kind == ApiResolutionKind::utility_function) {
            const auto* function = api_.find_utility_function(resolution->owner);
            return function && function->is_constant;
        }
        if (resolution->kind == ApiResolutionKind::intrinsic) {
            const auto* feature = IntrinsicRegistry::latest().find(resolution->intrinsic);
            return feature && feature->is_constant;
        }
        return false;
    }
    case ast::ExpressionKind::subscript:
        if (expression.operand_count() == 0)
            return false;
        for (std::size_t index = 0; index < expression.operand_count(); ++index) {
            if (!is_constant_expression(*expression.operand(index)))
                return false;
        }
        return true;
    // Array and Dictionary literals are intentionally call-time defaults. Godot constructs a
    // fresh mutable container for every omitted argument instead of sharing a folded instance.
    case ast::ExpressionKind::array_literal:
    case ast::ExpressionKind::dictionary_literal:
    case ast::ExpressionKind::node_reference:
    case ast::ExpressionKind::lambda:
        return false;
    }
    return false;
}

Type SemanticAnalyzer::constant_value_type_of(const ast::Expression& expression,
                                              const Type& fallback) const {
    if (expression.kind() == ast::ExpressionKind::identifier) {
        const auto* symbol = model_.symbol_of(expression);
        if (symbol && symbol->kind == SymbolKind::constant && symbol->constant_value_type)
            return *symbol->constant_value_type;
    }
    return fallback;
}

Type SemanticAnalyzer::runtime_storage_type_of(const ast::Expression& expression) const {
    const auto semantic_type = model_.type_of(expression);
    if (expression.kind() == ast::ExpressionKind::binary && expression.value() == "as" &&
        expression.operand_count() == 2 && is_explicitly_typed_container(semantic_type)) {
        const auto operand_type = model_.storage_type_of(*expression.operand(0));
        if (operand_type != semantic_type)
            return operand_type;
    }
    return model_.storage_type_of(expression);
}

std::optional<std::string>
SemanticAnalyzer::constant_string_expression(const ast::Expression& expression) const {
    if (expression.kind() == ast::ExpressionKind::literal &&
        expression.literal_kind() == ast::LiteralKind::string) {
        return expression.value();
    }
    if (expression.kind() == ast::ExpressionKind::identifier) {
        const auto* symbol = model_.symbol_of(expression);
        return symbol && symbol->kind == SymbolKind::constant ? symbol->constant_string_value
                                                              : std::nullopt;
    }
    if (expression.kind() == ast::ExpressionKind::binary && expression.value() == "+" &&
        expression.operand_count() == 2) {
        const auto left = constant_string_expression(*expression.operand(0));
        const auto right = constant_string_expression(*expression.operand(1));
        if (left && right)
            return *left + *right;
    }
    return std::nullopt;
}

std::optional<std::int64_t>
SemanticAnalyzer::constant_integer_expression(const ast::Expression& expression) const {
    std::unordered_map<std::string, std::int64_t> constants;
    for (const auto& scope : scopes_) {
        for (const auto& [name, symbol] : scope) {
            if (symbol.constant_integer_value)
                constants.insert_or_assign(name, *symbol.constant_integer_value);
        }
    }
    const auto collect_api_constants = [&](const auto& self,
                                           const ast::Expression& candidate) -> void {
        if (candidate.kind() == ast::ExpressionKind::identifier) {
            if (const auto* constant = api_.find_global_constant(candidate.value()))
                constants.insert_or_assign(candidate.value(), constant->value);
            if (const auto* enum_value = api_.find_global_enum_value(candidate.value()))
                constants.insert_or_assign(candidate.value(), enum_value->value);
        }
        for (std::size_t index = 0; index < candidate.operand_count(); ++index)
            self(self, *candidate.operand(index));
    };
    collect_api_constants(collect_api_constants, expression);
    return evaluate_integer_constant(expression, constants);
}

bool SemanticAnalyzer::is_match_value_pattern(const ast::Expression& expression) const {
    // Godot also permits a live identifier or a pure attribute chain (A.B.C) as the complete
    // pattern. It deliberately does not extend that exemption into arithmetic or subscripts.
    if (expression.kind() == ast::ExpressionKind::identifier)
        return model_.symbol_of(expression) || model_.api_resolution_of(expression);
    if (expression.kind() == ast::ExpressionKind::member) {
        const ast::Expression* base = &expression;
        while (base->kind() == ast::ExpressionKind::member && base->operand_count() == 1)
            base = base->operand(0).get();
        if (base->kind() == ast::ExpressionKind::identifier)
            return true;
    }
    return is_constant_expression(expression);
}

void SemanticAnalyzer::analyze_match_pattern(const ast::MatchPattern& pattern,
                                             const Type& matched_type) {
    model_.match_pattern_types_[&pattern] = matched_type;
    switch (pattern.kind()) {
    case ast::MatchPatternKind::value: {
        const auto pattern_type = analyze_expression(*pattern.expression());
        if (!is_match_value_pattern(*pattern.expression())) {
            diagnostics_.error(
                "GDS4045",
                "match expressions must be constant, an identifier, or an attribute access",
                pattern.span);
        }
        const bool same_project_enum =
            current_script_ && matched_type.kind == TypeKind::enumeration &&
            pattern_type.kind == TypeKind::enumeration &&
            (matched_type.name == current_script_->script_name + "." + pattern_type.name ||
             pattern_type.name == current_script_->script_name + "." + matched_type.name);
        if (!is_assignable(matched_type, pattern_type) &&
            !is_assignable(pattern_type, matched_type) && !same_project_enum) {
            diagnostics_.error("GDS4046",
                               "match pattern type " + pattern_type.display_name() +
                                   " cannot match " + matched_type.display_name(),
                               pattern.span);
        }
        return;
    }
    case ast::MatchPatternKind::binding:
        if (const auto* existing = resolve(pattern.name());
            existing &&
            (existing->kind == SymbolKind::local || existing->kind == SymbolKind::constant ||
             existing->kind == SymbolKind::parameter)) {
            diagnostics_.error("GDS4049",
                               "match binding '" + pattern.name() +
                                   "' conflicts with a variable in the enclosing scope",
                               pattern.span);
        }
        declare({SymbolKind::local, pattern.name(), matched_type, pattern.span, false});
        return;
    case ast::MatchPatternKind::wildcard:
        return;
    case ast::MatchPatternKind::rest:
        diagnostics_.error("GDS4048",
                           "rest pattern can only be used inside an array or dictionary pattern",
                           pattern.span);
        return;
    case ast::MatchPatternKind::array: {
        if (!matched_type.is_dynamic() && matched_type.kind != TypeKind::array) {
            diagnostics_.error("GDS4046", "array pattern requires an Array or Variant value",
                               pattern.span);
        }
        const auto element_type = matched_type.kind == TypeKind::array
                                      ? container_element_type(matched_type, pattern.span)
                                      : variant_type;
        std::size_t rest_count = 0;
        for (std::size_t index = 0; index < pattern.elements.size(); ++index) {
            const auto& child = *pattern.elements[index];
            if (child.kind() == ast::MatchPatternKind::rest) {
                ++rest_count;
                if (index + 1 != pattern.elements.size()) {
                    diagnostics_.error("GDS4048", "array rest pattern must be the last element",
                                       child.span);
                }
            }
            if (child.kind() != ast::MatchPatternKind::rest)
                analyze_match_pattern(child, element_type);
        }
        if (rest_count > 1U)
            diagnostics_.error("GDS4048", "array pattern accepts at most one rest marker",
                               pattern.span);
        return;
    }
    case ast::MatchPatternKind::dictionary: {
        if (!matched_type.is_dynamic() && matched_type.kind != TypeKind::dictionary) {
            diagnostics_.error("GDS4046",
                               "dictionary pattern requires a Dictionary or Variant value",
                               pattern.span);
        }
        if (pattern.keys.size() != pattern.elements.size()) {
            diagnostics_.error("GDS4048", "dictionary pattern key/value structure is invalid",
                               pattern.span);
            return;
        }
        std::size_t rest_count = 0;
        for (std::size_t index = 0; index < pattern.elements.size(); ++index) {
            const auto& child = *pattern.elements[index];
            if (!pattern.keys[index]) {
                ++rest_count;
                if (child.kind() != ast::MatchPatternKind::rest) {
                    diagnostics_.error("GDS4048", "dictionary rest entry is malformed", child.span);
                }
                if (index + 1 != pattern.elements.size()) {
                    diagnostics_.error("GDS4048", "dictionary rest pattern must be the last entry",
                                       child.span);
                }
                continue;
            }
            (void)analyze_expression(*pattern.keys[index]);
            if (!is_constant_expression(*pattern.keys[index])) {
                diagnostics_.error("GDS4045",
                                   "dictionary match keys must be compile-time constants",
                                   pattern.keys[index]->span);
            }
            analyze_match_pattern(child, variant_type);
        }
        if (rest_count > 1U)
            diagnostics_.error("GDS4048", "dictionary pattern accepts at most one rest marker",
                               pattern.span);
        return;
    }
    }
}

bool SemanticAnalyzer::is_assignment_target(const ast::Expression& expression) const noexcept {
    if (expression.kind() == ast::ExpressionKind::subscript)
        return expression.operand_count() == 2;
    if (expression.kind() == ast::ExpressionKind::identifier) {
        if (const auto* symbol = model_.symbol_of(expression)) {
            return symbol->kind == SymbolKind::field || symbol->kind == SymbolKind::parameter ||
                   symbol->kind == SymbolKind::local || symbol->kind == SymbolKind::constant;
        }
    }
    if (expression.kind() != ast::ExpressionKind::identifier &&
        expression.kind() != ast::ExpressionKind::member) {
        return false;
    }
    const auto* resolution = model_.api_resolution_of(expression);
    if (!resolution)
        return false;
    return resolution->kind == ApiResolutionKind::property ||
           resolution->kind == ApiResolutionKind::script_property ||
           resolution->kind == ApiResolutionKind::script_runtime_static_field ||
           resolution->kind == ApiResolutionKind::dynamic_property ||
           resolution->kind == ApiResolutionKind::script_static_field;
}

SemanticAnalyzer::FlowResult SemanticAnalyzer::analyze_statement(const ast::Statement& statement) {
    const WarningIgnoreScope warning_scope{active_warning_ignores_, statement.annotations};
    switch (statement.kind()) {
    case ast::StatementKind::expression: {
        const auto* previous_discarded_expression = discarded_expression_;
        discarded_expression_ = statement.expression().get();
        (void)analyze_expression(*statement.expression());
        discarded_expression_ = previous_discarded_expression;
        return FlowResult{true, false, false, false};
    }
    case ast::StatementKind::return_statement: {
        const auto actual = statement.expression() ? analyze_expression(*statement.expression())
                                                   : (expected_return_.kind == TypeKind::void_type
                                                          ? Type{TypeKind::void_type, "void"}
                                                          : Type{TypeKind::nil, "null"});
        if (statement.expression()) {
            require_expression_assignable(expected_return_, *statement.expression(), actual,
                                          statement.span, "invalid return value");
        } else if (expected_return_.kind != TypeKind::void_type) {
            require_assignable(expected_return_, actual, statement.span, "invalid return value");
        }
        return FlowResult{false, true, false, false};
    }
    case ast::StatementKind::assert_statement: {
        const auto condition = analyze_expression(*statement.condition());
        require_truthy_value(condition, statement.condition()->span, "assert condition");
        if (statement.expression()) {
            const auto message = analyze_expression(*statement.expression());
            require_assignable({TypeKind::string, "String"}, message, statement.expression()->span,
                               "assert message must be String");
        }
        return FlowResult{true, false, false, false};
    }
    case ast::StatementKind::variable: {
        if (statement.is_constant() && statement.expression() &&
            expression_contains_await(*statement.expression())) {
            diagnostics_.error("GDS2025", "a local constant initializer cannot await a signal",
                               statement.expression()->span);
        }
        const auto initializer =
            statement.expression() ? analyze_expression(*statement.expression()) : variant_type;
        Type type = statement.type().has_value() ? type_from_name(*statement.type(), statement.span)
                                                 : variant_type;
        if (statement.infer_type()) {
            require_inferable_type(initializer, statement.span, "local variable");
            type = initializer;
        } else if (statement.is_constant() && !statement.type()) {
            type = initializer;
        }
        if ((statement.type().has_value() || statement.infer_type()) && statement.expression()) {
            require_expression_assignable(type, *statement.expression(), initializer,
                                          statement.span, "invalid initializer");
        }
        model_.local_types_[&statement] = type;
        const auto constant_string = statement.is_constant() && statement.expression()
                                         ? constant_string_expression(*statement.expression())
                                         : std::optional<std::string>{};
        const auto constant_integer = statement.is_constant() && statement.expression()
                                          ? constant_integer_expression(*statement.expression())
                                          : std::optional<std::int64_t>{};
        const auto constant_value_type = statement.is_constant() && statement.expression()
                                             ? std::optional<Type>{initializer}
                                             : std::nullopt;
        declare({statement.is_constant() ? SymbolKind::constant : SymbolKind::local,
                 statement.name(), type, statement.span, statement.is_constant(), constant_string,
                 SymbolStorage::function_local, constant_integer, 0, constant_value_type});
        return FlowResult{true, false, false, false};
    }
    case ast::StatementKind::assignment: {
        const auto previous_await_context = await_expression_allowed_;
        const auto previous_suppression = suppress_flow_refinements_;
        await_expression_allowed_ = false;
        suppress_flow_refinements_ =
            statement.condition()->kind() == ast::ExpressionKind::identifier;
        const auto target = analyze_expression(*statement.condition());
        suppress_flow_refinements_ = previous_suppression;
        await_expression_allowed_ = previous_await_context;
        const auto value = analyze_expression(*statement.expression());
        auto assignment_target = target;
        if (const auto* resolution = model_.api_resolution_of(*statement.condition());
            resolution && resolution->assignment_type.kind != TypeKind::unknown) {
            assignment_target = resolution->assignment_type;
        }
        if (!is_assignment_target(*statement.condition())) {
            diagnostics_.error("GDS4110", "assignment target is not writable",
                               statement.condition()->span);
        }
        if (const auto* symbol = model_.symbol_of(*statement.condition());
            symbol && symbol->read_only) {
            diagnostics_.error("GDS4006", "cannot assign to constant '" + symbol->name + "'",
                               statement.span);
        }
        if (const auto* resolution = model_.api_resolution_of(*statement.condition());
            resolution && resolution->kind == ApiResolutionKind::property && !resolution->direct &&
            resolution->setter.empty()) {
            diagnostics_.error("GDS4012", "cannot assign to a read-only Godot property",
                               statement.span);
        }
        if (const auto* resolution = model_.api_resolution_of(*statement.condition());
            resolution && resolution->read_only) {
            diagnostics_.error("GDS4113", "cannot assign to a read-only extension property",
                               statement.span);
        }
        auto assigned = value;
        if (statement.operation() != "=") {
            const auto operation =
                statement.operation().substr(0, statement.operation().size() - 1);
            if (target.is_dynamic() || value.is_dynamic()) {
                assigned = variant_type;
            } else if (const auto* record =
                           api_.find_builtin_operator(builtin_operator_type(target), operation,
                                                      builtin_operator_type(value))) {
                assigned = type_from_godot_api(record->return_type);
            } else {
                diagnostics_.error("GDS4005",
                                   "operator '" + operation + "' is not defined for " +
                                       target.display_name() + " and " + value.display_name(),
                                   statement.span);
                assigned = unknown_type;
            }
        }
        if (statement.operation() == "=") {
            require_expression_assignable(assignment_target, *statement.expression(), assigned,
                                          statement.span, "invalid assignment");
        } else {
            require_assignable(assignment_target, assigned, statement.span, "invalid assignment");
        }
        if (const auto* symbol = model_.symbol_of(*statement.condition());
            symbol &&
            (symbol->kind == SymbolKind::local || symbol->kind == SymbolKind::parameter)) {
            flow_types_.invalidate(symbol->identity);
        }
        return FlowResult{true, false, false, false};
    }
    case ast::StatementKind::if_statement: {
        const auto condition_type = analyze_expression(*statement.condition());
        require_truthy_value(condition_type, statement.condition()->span, "if condition");
        const auto refinements = conditional_refinements(*statement.condition());
        const auto entry_state = flow_types_;

        flow_types_ = entry_state;
        flow_types_.apply(refinements.when_true);
        scopes_.emplace_back();
        const auto body_flow = analyze_statements(statement.body());
        scopes_.pop_back();
        const auto body_state = flow_types_;

        auto else_flow = FlowResult{true, false, false, false};
        flow_types_ = entry_state;
        flow_types_.apply(refinements.when_false);
        if (!statement.else_body().empty()) {
            scopes_.emplace_back();
            else_flow = analyze_statements(statement.else_body());
            scopes_.pop_back();
        }
        const auto else_state = flow_types_;
        flow_types_ =
            FlowTypeState::join_fallthrough({body_flow.falls_through ? &body_state : nullptr,
                                             else_flow.falls_through ? &else_state : nullptr});
        return FlowResult{body_flow.falls_through || else_flow.falls_through,
                          body_flow.returns || else_flow.returns,
                          body_flow.breaks || else_flow.breaks,
                          body_flow.continues || else_flow.continues};
    }
    case ast::StatementKind::match_statement: {
        const auto matched_type = analyze_expression(*statement.condition());
        if (statement.match_branches().empty())
            return FlowResult{true, false, false, false};
        const auto entry_state = flow_types_;
        const auto* matched_symbol = model_.symbol_of(*statement.condition());
        std::vector<FlowTypeState> fallthrough_states;
        fallthrough_states.reserve(statement.match_branches().size() + 1U);
        bool unconditional_seen = false;
        FlowResult flow;
        for (const auto& branch : statement.match_branches()) {
            const bool branch_reachable = !unconditional_seen;
            if (branch.patterns.empty()) {
                diagnostics_.error("GDS4043", "match branch requires at least one pattern",
                                   branch.span);
                flow.falls_through = true;
                continue;
            }
            if (unconditional_seen &&
                !warning_is_ignored(active_warning_ignores_, "unreachable_pattern"))
                diagnostics_.warning("GDS4044", "match branch is unreachable after a catch-all",
                                     branch.span);
            flow_types_ = entry_state;
            std::optional<Type> structural_type;
            bool uniform_structural_type = !branch.patterns.empty();
            for (const auto& pattern : branch.patterns) {
                const auto candidate =
                    pattern.kind() == ast::MatchPatternKind::array
                        ? std::optional<Type>{{TypeKind::array, "Array"}}
                    : pattern.kind() == ast::MatchPatternKind::dictionary
                        ? std::optional<Type>{{TypeKind::dictionary, "Dictionary"}}
                        : std::nullopt;
                if (!candidate || (structural_type && *structural_type != *candidate)) {
                    uniform_structural_type = false;
                    break;
                }
                structural_type = candidate;
            }
            if (uniform_structural_type && structural_type && matched_symbol &&
                (matched_symbol->kind == SymbolKind::local ||
                 matched_symbol->kind == SymbolKind::parameter)) {
                if (const auto refined = narrowed_flow_type(matched_type, *structural_type))
                    flow_types_.refine(matched_symbol->identity, *refined);
                flow_types_.mark_non_null(matched_symbol->identity);
            }
            scopes_.emplace_back();
            bool catch_all = false;
            for (const auto& pattern : branch.patterns) {
                analyze_match_pattern(pattern, matched_type);
                catch_all = catch_all || pattern.kind() == ast::MatchPatternKind::wildcard ||
                            pattern.kind() == ast::MatchPatternKind::binding;
                if (branch.patterns.size() != 1U && match_pattern_contains_binding(pattern)) {
                    diagnostics_.error("GDS4047",
                                       "variable bindings cannot be combined with alternative "
                                       "match patterns",
                                       pattern.span);
                }
            }
            if (branch.guard) {
                const auto guard_type = analyze_expression(*branch.guard);
                require_truthy_value(guard_type, branch.guard->span, "match guard");
                flow_types_.apply(conditional_refinements(*branch.guard).when_true);
            }
            const auto branch_flow = analyze_statements(branch.body);
            scopes_.pop_back();
            if (branch_reachable) {
                flow.falls_through = flow.falls_through || branch_flow.falls_through;
                flow.returns = flow.returns || branch_flow.returns;
                flow.breaks = flow.breaks || branch_flow.breaks;
                flow.continues = flow.continues || branch_flow.continues;
                if (branch_flow.falls_through)
                    fallthrough_states.push_back(flow_types_);
            }
            if (catch_all && !branch.guard)
                unconditional_seen = true;
        }
        if (!unconditional_seen) {
            flow.falls_through = true;
            fallthrough_states.push_back(entry_state);
        }
        std::vector<const FlowTypeState*> predecessors;
        predecessors.reserve(fallthrough_states.size());
        for (const auto& state : fallthrough_states)
            predecessors.push_back(&state);
        flow_types_ = FlowTypeState::join_fallthrough(predecessors);
        return flow;
    }
    case ast::StatementKind::while_statement: {
        const auto condition_type = analyze_expression(*statement.condition());
        require_truthy_value(condition_type, statement.condition()->span, "while condition");
        const auto refinements = conditional_refinements(*statement.condition());
        const auto entry_state = flow_types_;
        flow_types_.apply(refinements.when_true);
        ++loop_depth_;
        scopes_.emplace_back();
        const auto body_flow = analyze_statements(statement.body());
        scopes_.pop_back();
        --loop_depth_;
        const bool constant_true =
            statement.condition()->kind() == ast::ExpressionKind::literal &&
            statement.condition()->literal_kind() == ast::LiteralKind::boolean &&
            statement.condition()->value() == "true";
        auto normal_exit_state = entry_state;
        normal_exit_state.apply(refinements.when_false);
        flow_types_ = body_flow.breaks
                          ? FlowTypeState::join_fallthrough({&entry_state, &normal_exit_state})
                          : std::move(normal_exit_state);
        return FlowResult{!constant_true || body_flow.breaks, body_flow.returns, false, false};
    }
    case ast::StatementKind::for_statement: {
        const auto* loop = statement.get_if<ast::ForStatement>();
        const auto iterator_span = loop ? loop->iterator_span : statement.span;
        const auto type_span = loop && loop->type_span ? *loop->type_span : statement.span;
        auto iterable = analyze_expression(*statement.condition());
        const auto specified_element_type =
            statement.type() ? type_from_name(*statement.type(), type_span) : variant_type;
        if (statement.type() && !specified_element_type.is_dynamic() &&
            statement.condition()->kind() == ast::ExpressionKind::array_literal) {
            const Type constrained{TypeKind::array,
                                   "Array[" + specified_element_type.display_name() + "]"};
            require_expression_assignable(constrained, *statement.condition(), iterable,
                                          statement.condition()->span, "typed for-loop iterable");
            iterable = model_.type_of(*statement.condition());
        } else if (statement.type() && !specified_element_type.is_dynamic() &&
                   statement.condition()->kind() == ast::ExpressionKind::dictionary_literal) {
            const Type constrained{TypeKind::dictionary, "Dictionary[" +
                                                             specified_element_type.display_name() +
                                                             ", Variant]"};
            require_expression_assignable(constrained, *statement.condition(), iterable,
                                          statement.condition()->span, "typed for-loop iterable");
            iterable = model_.type_of(*statement.condition());
        }
        const auto object_element_type =
            iterable.kind == TypeKind::object
                ? object_iteration_element_type(iterable, statement.condition()->span)
                : std::optional<Type>{};
        const bool mathematical_range =
            iterable.kind == TypeKind::floating ||
            (iterable.kind == TypeKind::builtin &&
             (iterable.name == "Vector2" || iterable.name == "Vector2i" ||
              iterable.name == "Vector3" || iterable.name == "Vector3i"));
        if (!iterable.is_dynamic() && iterable.kind != TypeKind::array &&
            iterable.kind != TypeKind::dictionary && iterable.kind != TypeKind::string &&
            iterable.kind != TypeKind::integer && !mathematical_range &&
            !iterable.is_packed_array() && iterable.kind != TypeKind::object) {
            diagnostics_.error("GDS4007", "for loop expression is not iterable",
                               statement.condition()->span);
        }
        if (const auto existing = scopes_.back().find(statement.name());
            existing != scopes_.back().end() && (existing->second.kind == SymbolKind::local ||
                                                 existing->second.kind == SymbolKind::constant ||
                                                 existing->second.kind == SymbolKind::parameter)) {
            diagnostics_.error("GDS4125",
                               "iterator variable '" + statement.name() +
                                   "' conflicts with a variable in the same scope",
                               iterator_span);
        }
        ++loop_depth_;
        scopes_.emplace_back();
        const auto inferred_element_type =
            object_element_type.value_or(iteration_element_type(iterable, statement.span));
        bool intrinsic_range = false;
        if (statement.condition()->kind() == ast::ExpressionKind::call &&
            statement.condition()->operand_count() >= 1) {
            const auto* resolution = model_.api_resolution_of(*statement.condition()->operand(0));
            intrinsic_range = resolution && resolution->kind == ApiResolutionKind::intrinsic &&
                              resolution->intrinsic == IntrinsicKind::range;
        }
        model_.iteration_plans_[&statement] =
            make_iteration_plan(iterable, inferred_element_type, intrinsic_range);
        const auto element_type = statement.type() ? specified_element_type : inferred_element_type;
        if (statement.type())
            require_assignable(element_type, inferred_element_type, type_span,
                               "invalid iterator variable type");
        model_.local_types_[&statement] = element_type;
        declare({SymbolKind::local, statement.name(), element_type, iterator_span, false});
        const auto body_flow = analyze_statements(statement.body());
        scopes_.pop_back();
        --loop_depth_;
        return FlowResult{true, body_flow.returns, false, false};
    }
    case ast::StatementKind::break_statement:
    case ast::StatementKind::continue_statement: {
        if (loop_depth_ == 0) {
            diagnostics_.error("GDS4008", "loop control statement used outside a loop",
                               statement.span);
        }
        return statement.kind() == ast::StatementKind::break_statement
                   ? FlowResult{false, false, true, false}
                   : FlowResult{false, false, false, true};
    }
    case ast::StatementKind::pass_statement:
        return FlowResult{true, false, false, false};
    case ast::StatementKind::breakpoint_statement:
        model_.debug_variables_.insert_or_assign(&statement, visible_debug_variables());
        return FlowResult{true, false, false, false};
    }
    return FlowResult{true, false, false, false};
}

SemanticAnalyzer::FlowResult
SemanticAnalyzer::analyze_statements(const std::vector<ast::Statement>& statements) {
    FlowResult flow{true, false, false, false};
    for (const auto& statement : statements) {
        const bool reachable = flow.falls_through;
        const bool ignores_unreachable =
            annotations_ignore_warning(statement.annotations, "unreachable_code") ||
            warning_is_ignored(active_warning_ignores_, "unreachable_code");
        if (!reachable && !ignores_unreachable)
            diagnostics_.warning("GDS4069", "unreachable statement", statement.span);
        const auto entry_state = flow_types_;
        const auto statement_flow = analyze_statement(statement);
        if (!reachable) {
            flow_types_ = entry_state;
            continue;
        }
        flow.falls_through = statement_flow.falls_through;
        flow.returns = flow.returns || statement_flow.returns;
        flow.breaks = flow.breaks || statement_flow.breaks;
        flow.continues = flow.continues || statement_flow.continues;
    }
    return flow;
}

void SemanticAnalyzer::analyze_rpc_annotations(const ast::FunctionDeclaration& function) {
    const ast::Annotation* rpc = nullptr;
    for (const auto& annotation : function.annotations) {
        if (annotation.name != "rpc")
            continue;
        if (rpc) {
            diagnostics_.error("GDS4133", "RPC annotations can only be used once per function",
                               annotation.span);
            continue;
        }
        rpc = &annotation;
    }
    if (!rpc)
        return;

    RpcConfiguration configuration;
    bool valid = rpc->arguments.size() <= 4;
    std::uint8_t permission_arguments = 0;
    std::uint8_t locality_arguments = 0;
    std::uint8_t transfer_arguments = 0;
    const auto string_argument_count = std::min<std::size_t>(rpc->arguments.size(), 3);
    for (std::size_t index = 0; index < string_argument_count; ++index) {
        const auto previous_await_context = await_expression_allowed_;
        await_expression_allowed_ = false;
        (void)analyze_expression(*rpc->arguments[index]);
        await_expression_allowed_ = previous_await_context;
        const auto value = constant_string_expression(*rpc->arguments[index]);
        if (!value) {
            diagnostics_.error("GDS4134", "RPC arguments 1 to 3 must be constant String values",
                               rpc->arguments[index]->span);
            valid = false;
            continue;
        }
        if (*value == "authority") {
            ++permission_arguments;
            configuration.permission = RpcPermission::authority;
        } else if (*value == "any_peer") {
            ++permission_arguments;
            configuration.permission = RpcPermission::any_peer;
        } else if (*value == "call_remote") {
            ++locality_arguments;
            configuration.call_local = false;
        } else if (*value == "call_local") {
            ++locality_arguments;
            configuration.call_local = true;
        } else if (*value == "unreliable") {
            ++transfer_arguments;
            configuration.transfer_mode = RpcTransferMode::unreliable;
        } else if (*value == "unreliable_ordered") {
            ++transfer_arguments;
            configuration.transfer_mode = RpcTransferMode::unreliable_ordered;
        } else if (*value == "reliable") {
            ++transfer_arguments;
            configuration.transfer_mode = RpcTransferMode::reliable;
        } else {
            diagnostics_.error("GDS4135",
                               "invalid RPC argument '" + *value +
                                   "'; expected a permission, locality, or transfer mode",
                               rpc->arguments[index]->span);
            valid = false;
        }
    }
    if (permission_arguments > 1) {
        diagnostics_.error("GDS4136", "RPC permission must be specified no more than once",
                           rpc->span);
        valid = false;
    }
    if (locality_arguments > 1) {
        diagnostics_.error("GDS4136", "RPC locality must be specified no more than once",
                           rpc->span);
        valid = false;
    }
    if (transfer_arguments > 1) {
        diagnostics_.error("GDS4136", "RPC transfer mode must be specified no more than once",
                           rpc->span);
        valid = false;
    }
    if (rpc->arguments.size() == 4) {
        const auto previous_await_context = await_expression_allowed_;
        await_expression_allowed_ = false;
        (void)analyze_expression(*rpc->arguments[3]);
        await_expression_allowed_ = previous_await_context;
        const auto channel = constant_integer_expression(*rpc->arguments[3]);
        if (!channel) {
            diagnostics_.error("GDS4137", "RPC transfer channel must be a constant int value",
                               rpc->arguments[3]->span);
            valid = false;
        } else {
            configuration.channel = *channel;
        }
    }
    if (valid)
        model_.rpc_configurations_.insert_or_assign(&function, configuration);
}

void SemanticAnalyzer::analyze_function(const ast::FunctionDeclaration& function) {
    const WarningIgnoreScope warning_scope{active_warning_ignores_, function.annotations};
    const auto previous_in_function = in_function_;
    const auto previous_function_name = current_function_name_;
    const auto previous_callable_suspends = current_callable_suspends_;
    const auto previous_instance_context = instance_context_available_;
    auto previous_flow_types = std::move(flow_types_);
    flow_types_.clear();
    in_function_ = true;
    current_function_name_ = function.name;
    current_callable_suspends_ = false;
    const auto abstract_annotations = static_cast<std::size_t>(std::count_if(
        function.annotations.begin(), function.annotations.end(),
        [](const ast::Annotation& annotation) { return annotation.name == "abstract"; }));
    for (const auto& annotation : function.annotations) {
        if (annotation.name == "warning_ignore" || annotation.name == "rpc" ||
            annotation.name == "abstract") {
            continue;
        }
        diagnostics_.error("GDS4112",
                           "function annotation '@" + annotation.name +
                               "' is recognized but its runtime lowering is not implemented",
                           annotation.span);
    }
    if (abstract_annotations > 1) {
        diagnostics_.error("GDS4147", "@abstract can only be used once per function",
                           function.span);
    }
    if (function.is_abstract && function.is_static) {
        diagnostics_.error("GDS4147", "@abstract cannot be applied to static functions",
                           function.span);
    }
    const bool valid_abstract_contract = abstract_annotations == 1 && !function.is_static;
    if (valid_abstract_contract && function.has_body) {
        diagnostics_.error("GDS4148", "an abstract function cannot have a body", function.span);
    } else if (!valid_abstract_contract && !function.has_body) {
        diagnostics_.error(
            "GDS4148",
            "a function must have a body or be marked with one valid @abstract annotation",
            function.span);
    }
    if (function.name == "_init" && function.is_static)
        diagnostics_.error("GDS4065", "_init cannot be static", function.span);
    if (function.name == "_static_init" && !function.is_static)
        diagnostics_.error("GDS4123", "_static_init must be declared static", function.span);
    if (function.name == "_static_init" &&
        (!function.parameters.empty() || function.rest_parameter))
        diagnostics_.error("GDS4124", "_static_init cannot declare parameters", function.span);
    if (function.name == "_init" && function.return_type && *function.return_type != "void") {
        diagnostics_.error("GDS4066", "_init cannot declare a non-void return type", function.span);
    }
    if (function.name == "_static_init" && function.return_type &&
        *function.return_type != "void") {
        diagnostics_.error("GDS4066", "_static_init cannot declare a non-void return type",
                           function.span);
    }
    const auto* virtual_method = api_.find_method(base_type_, function.name);
    if (virtual_method && !virtual_method->is_virtual)
        virtual_method = nullptr;
    const ScriptMemberSymbol* inherited_script_method = nullptr;
    if (function.name != "_init" && current_inner_class_) {
        if (current_inner_base_) {
            const auto* inherited = find_inner_member(*current_inner_base_, function.name);
            if (inherited && inherited->kind == ScriptMemberKind::function)
                inherited_script_method = inherited;
        } else if (const auto* base = inner_script_base_of(*current_inner_class_)) {
            const auto* inherited = script_symbols_->find_member(*base, function.name);
            if (inherited && inherited->kind == ScriptMemberKind::function)
                inherited_script_method = inherited;
        } else if (script_symbols_) {
            const auto* external = script_symbols_->external_base_of(*current_inner_class_);
            const auto* inherited =
                external ? script_symbols_->find_external_member(*external, function.name)
                         : nullptr;
            if (inherited && inherited->kind == ScriptMemberKind::function)
                inherited_script_method = inherited;
        }
    } else if (function.name != "_init" && script_symbols_ && current_script_) {
        if (const auto* base = script_symbols_->base_of(*current_script_)) {
            const auto* inherited = script_symbols_->find_member(*base, function.name);
            if (inherited && inherited->kind == ScriptMemberKind::function) {
                inherited_script_method = inherited;
            }
        }
    }
    expected_return_ =
        function.name == "_init" || function.name == "_static_init" ? void_type
        : function.return_type.has_value() ? type_from_name(*function.return_type, function.span)
        : inherited_script_method          ? inherited_script_method->type
        : virtual_method && std::string_view{virtual_method->return_type}.empty() ? void_type
        : virtual_method ? type_from_godot_api(virtual_method->return_type)
                         : variant_type;
    model_.function_return_types_[&function] = expected_return_;
    // Native coroutine methods expose a Variant that is either the synchronously completed
    // source return value or a per-call completion Signal. This preserves typed and untyped
    // GDScript return values without changing their source-level type.
    allow_dynamic_await_return_ = true;
    current_function_static_ = function.is_static;
    instance_context_available_ = !function.is_static;
    analyze_rpc_annotations(function);
    current_accessor_fields_.clear();
    if (const auto found = bound_accessor_fields_.find(function.name);
        found != bound_accessor_fields_.end()) {
        current_accessor_fields_ = found->second;
    }
    scopes_.emplace_back();
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
        const auto& parameter = function.parameters[index];
        const auto* virtual_argument = virtual_method && index < virtual_method->maximum_arguments
                                           ? api_.argument(*virtual_method, index)
                                           : nullptr;
        const Type* inherited_argument =
            inherited_script_method && index < inherited_script_method->parameters.size()
                ? &inherited_script_method->parameters[index]
                : nullptr;
        std::optional<Type> analyzed_default;
        const auto previous_await_context = await_expression_allowed_;
        await_expression_allowed_ = false;
        if (parameter.default_value)
            analyzed_default = analyze_expression(*parameter.default_value);
        await_expression_allowed_ = previous_await_context;
        if (parameter.infer_type && analyzed_default)
            require_inferable_type(*analyzed_default, parameter.span, "parameter");
        const auto type = parameter.type.has_value()
                              ? type_from_name(*parameter.type, parameter.span)
                          : parameter.infer_type && analyzed_default ? *analyzed_default
                          : inherited_argument                       ? *inherited_argument
                          : virtual_argument ? type_from_godot_api(virtual_argument->type)
                                             : variant_type;
        model_.parameter_types_[&parameter] = type;
        if (parameter.default_value) {
            model_.default_argument_evaluations_.insert_or_assign(
                &parameter, is_constant_expression(*parameter.default_value)
                                ? DefaultArgumentEvaluation::compile_time_constant
                                : DefaultArgumentEvaluation::call_time);
        }
        declare({SymbolKind::parameter, parameter.name, type, parameter.span, false});
        if (analyzed_default)
            require_expression_assignable(type, *parameter.default_value, *analyzed_default,
                                          parameter.span, "invalid default value");
    }
    if (function.rest_parameter)
        analyze_rest_parameter(*function.rest_parameter);
    const auto current_required = static_cast<std::size_t>(
        std::count_if(function.parameters.begin(), function.parameters.end(),
                      [](const auto& parameter) { return parameter.default_value == nullptr; }));
    const auto validate_override = [&](const std::string& owner, const Type& parent_return,
                                       const std::vector<Type>& parent_parameters,
                                       const std::size_t parent_required, const bool parent_static,
                                       const bool parent_vararg) {
        if (function.is_static != parent_static) {
            diagnostics_.error("GDS4143",
                               owner + " override '" + function.name +
                                   "' must preserve the parent's static/instance qualifier",
                               function.span);
        }
        if (current_required > parent_required ||
            (!function.rest_parameter &&
             (parent_vararg || function.parameters.size() < parent_parameters.size()))) {
            diagnostics_.error("GDS4102",
                               owner + " override '" + function.name + "' accepts " +
                                   std::to_string(current_required) + " to " +
                                   (function.rest_parameter
                                        ? std::string{"unbounded"}
                                        : std::to_string(function.parameters.size())) +
                                   " argument(s), which does not include the parent range " +
                                   std::to_string(parent_required) + " to " +
                                   (parent_vararg ? std::string{"unbounded"}
                                                  : std::to_string(parent_parameters.size())),
                               function.span);
        }
        if (function.return_type &&
            ((expected_return_.is_dynamic() && !parent_return.is_dynamic()) ||
             !override_type_accepts(parent_return, expected_return_))) {
            diagnostics_.error("GDS4120",
                               owner + " override '" + function.name +
                                   "' must return a subtype of " + parent_return.display_name() +
                                   ", got " + expected_return_.display_name(),
                               function.span);
        }
        const auto checked = std::min(function.parameters.size(), parent_parameters.size());
        for (std::size_t index = 0; index < checked; ++index) {
            const auto& parameter = function.parameters[index];
            const auto current_type = model_.type_of(parameter);
            const bool explicit_type = parameter.type.has_value() || parameter.infer_type;
            const bool hard_variant_parent =
                parent_parameters[index].is_dynamic() && owner == "script" &&
                inherited_script_method &&
                index < inherited_script_method->explicit_parameter_types.size() &&
                inherited_script_method->explicit_parameter_types[index];
            if (explicit_type && ((hard_variant_parent && !current_type.is_dynamic()) ||
                                  !override_type_accepts(current_type, parent_parameters[index]))) {
                diagnostics_.error("GDS4121",
                                   "parameter " + std::to_string(index + 1) + " of " + owner +
                                       " override '" + function.name + "' must accept " +
                                       parent_parameters[index].display_name() + ", got " +
                                       current_type.display_name(),
                                   parameter.span);
            }
        }
    };
    if (inherited_script_method) {
        validate_override("script", inherited_script_method->type,
                          inherited_script_method->parameters,
                          inherited_script_method->required_arguments,
                          inherited_script_method->is_static, inherited_script_method->is_vararg);
    } else if (virtual_method) {
        std::vector<Type> api_parameters;
        api_parameters.reserve(virtual_method->maximum_arguments);
        bool exposes_native_pointer =
            std::string_view{virtual_method->return_type}.find('*') != std::string_view::npos;
        for (std::size_t index = 0; index < virtual_method->maximum_arguments; ++index) {
            if (const auto* argument = api_.argument(*virtual_method, index)) {
                api_parameters.push_back(type_from_godot_api(argument->type));
                exposes_native_pointer =
                    exposes_native_pointer ||
                    std::string_view{argument->type}.find('*') != std::string_view::npos;
            }
        }
        if (exposes_native_pointer) {
            diagnostics_.error(
                "GDS4118",
                "Godot virtual override '" + function.name +
                    "' exposes an internal native pointer ABI that GDScript cannot represent",
                function.span);
        }
        const auto api_return = std::string_view{virtual_method->return_type}.empty()
                                    ? void_type
                                    : type_from_godot_api(virtual_method->return_type);
        validate_override("Godot virtual", api_return, api_parameters,
                          virtual_method->required_arguments, virtual_method->is_static,
                          virtual_method->is_vararg);
    }
    const auto flow = valid_abstract_contract ? FlowResult{} : analyze_statements(function.body);
    if (current_callable_suspends_)
        model_.coroutine_functions_.insert(&function);
    scopes_.pop_back();
    if (expected_return_.kind != TypeKind::void_type && !expected_return_.is_dynamic() &&
        flow.falls_through) {
        diagnostics_.error("GDS4009",
                           "function returning " + expected_return_.display_name() +
                               " does not return a value on every reachable path",
                           function.span);
    }
    current_accessor_fields_.clear();
    allow_dynamic_await_return_ = false;
    current_function_static_ = false;
    instance_context_available_ = previous_instance_context;
    current_callable_suspends_ = previous_callable_suspends;
    in_function_ = previous_in_function;
    current_function_name_ = previous_function_name;
    flow_types_ = std::move(previous_flow_types);
}

void SemanticAnalyzer::analyze_lambda(const ast::LambdaExpression& expression) {
    const auto previous_return = expected_return_;
    const auto previous_static = current_function_static_;
    const auto previous_in_function = in_function_;
    const auto previous_loop_depth = loop_depth_;
    const auto previous_function_name = current_function_name_;
    const auto previous_dynamic_await = allow_dynamic_await_return_;
    const auto previous_callable_suspends = current_callable_suspends_;
    auto previous_flow_types = std::move(flow_types_);
    flow_types_.clear();

    expected_return_ = expression.return_type
                           ? type_from_name(*expression.return_type, expression.span)
                           : variant_type;
    model_.lambda_return_types_[&expression] = expected_return_;
    if (in_function_ && instance_context_available_)
        model_.owner_bound_lambdas_.insert(&expression);
    current_function_static_ = false;
    in_function_ = true;
    current_function_name_ = expression.name.empty() ? "<lambda>" : expression.name;
    loop_depth_ = 0;
    allow_dynamic_await_return_ = !expression.return_type;
    current_callable_suspends_ = false;
    scopes_.emplace_back();
    for (const auto& parameter : expression.parameters) {
        std::optional<Type> analyzed_default;
        const auto previous_await_context = await_expression_allowed_;
        await_expression_allowed_ = false;
        if (parameter.default_value)
            analyzed_default = analyze_expression(*parameter.default_value);
        await_expression_allowed_ = previous_await_context;
        if (parameter.infer_type && analyzed_default)
            require_inferable_type(*analyzed_default, parameter.span, "lambda parameter");
        const auto type = parameter.type ? type_from_name(*parameter.type, parameter.span)
                          : parameter.infer_type && analyzed_default ? *analyzed_default
                                                                     : variant_type;
        model_.parameter_types_[&parameter] = type;
        if (parameter.default_value) {
            model_.default_argument_evaluations_.insert_or_assign(
                &parameter, is_constant_expression(*parameter.default_value)
                                ? DefaultArgumentEvaluation::compile_time_constant
                                : DefaultArgumentEvaluation::call_time);
        }
        declare({SymbolKind::parameter, parameter.name, type, parameter.span, false});
        if (analyzed_default)
            require_expression_assignable(type, *parameter.default_value, *analyzed_default,
                                          parameter.span, "invalid lambda default value");
    }
    if (expression.rest_parameter)
        analyze_rest_parameter(*expression.rest_parameter);
    const auto flow = analyze_statements(expression.body);
    if (current_callable_suspends_)
        model_.coroutine_lambdas_.insert(&expression);
    scopes_.pop_back();
    if (expected_return_.kind != TypeKind::void_type && !expected_return_.is_dynamic() &&
        flow.falls_through) {
        diagnostics_.error("GDS4009",
                           "lambda returning " + expected_return_.display_name() +
                               " does not return a value on every reachable path",
                           expression.span);
    }

    allow_dynamic_await_return_ = previous_dynamic_await;
    current_callable_suspends_ = previous_callable_suspends;
    loop_depth_ = previous_loop_depth;
    in_function_ = previous_in_function;
    current_function_static_ = previous_static;
    current_function_name_ = previous_function_name;
    expected_return_ = previous_return;
    flow_types_ = std::move(previous_flow_types);
}

void SemanticAnalyzer::analyze_class(const ast::ClassDeclaration& declaration) {
    const WarningIgnoreScope warning_scope{active_warning_ignores_, declaration.annotations};
    const auto abstract_annotations = static_cast<std::size_t>(std::count_if(
        declaration.annotations.begin(), declaration.annotations.end(),
        [](const ast::Annotation& annotation) { return annotation.name == "abstract"; }));
    for (const auto& annotation : declaration.annotations) {
        if (annotation.name == "warning_ignore" || annotation.name == "abstract")
            continue;
        diagnostics_.error("GDS4113",
                           "internal class annotation '@" + annotation.name +
                               "' is recognized but its lowering is not implemented",
                           annotation.span);
    }
    if (abstract_annotations > 1) {
        diagnostics_.error("GDS4147", "@abstract can only be used once per class",
                           declaration.span);
    }
    auto saved_scopes = std::move(scopes_);
    auto saved_enum_types = std::move(enum_types_);
    auto saved_enum_members = std::move(enum_members_);
    auto saved_accessor_fields = std::move(accessor_fields_);
    auto saved_static_fields = std::move(static_fields_);
    auto saved_current_accessor_fields = std::move(current_accessor_fields_);
    auto saved_bound_accessor_fields = std::move(bound_accessor_fields_);
    auto saved_functions = std::move(functions_);
    const auto saved_dynamic_await = allow_dynamic_await_return_;
    const auto saved_static = current_function_static_;
    const auto saved_instance_context = instance_context_available_;
    const auto saved_in_function = in_function_;
    const auto saved_return = expected_return_;
    const auto saved_base = base_type_;
    const auto saved_loop_depth = loop_depth_;
    const auto* saved_inner = current_inner_class_;
    const auto* saved_inner_base = current_inner_base_;

    scopes_.clear();
    scopes_.emplace_back();
    scopes_.emplace_back();
    enum_types_.clear();
    enum_members_.clear();
    accessor_fields_.clear();
    static_fields_.clear();
    current_accessor_fields_.clear();
    active_warning_ignores_.clear();
    bound_accessor_fields_.clear();
    functions_.clear();
    allow_dynamic_await_return_ = false;
    current_function_static_ = false;
    instance_context_available_ = true;
    in_function_ = false;
    expected_return_ = void_type;
    loop_depth_ = 0;
    current_inner_class_ = find_inner_class(declaration.name);
    current_inner_base_ = current_inner_class_ ? inner_base_of(*current_inner_class_) : nullptr;
    const ScriptClassSymbol* current_inner_script_base = nullptr;
    if (current_inner_class_) {
        std::unordered_set<const ScriptInnerClassSymbol*> visited;
        for (auto* current = current_inner_class_; current && visited.insert(current).second;
             current = inner_base_of(*current)) {
            if (const auto* script_base = inner_script_base_of(*current)) {
                current_inner_script_base = script_base;
                break;
            }
        }
    }
    base_type_ = current_inner_class_ ? current_inner_class_->godot_base_type
                                      : declaration.base_type.value_or("RefCounted");
    record_script_dependency(current_inner_script_base);
    validate_inner_abstract_contract(declaration);

    if (!current_inner_base_ && !current_inner_script_base && !api_.find_class(base_type_)) {
        diagnostics_.error("GDS4099",
                           "internal class base '" + base_type_ + "' is not a Godot engine type",
                           declaration.span);
        base_type_ = "RefCounted";
    }
    const auto import_inherited_member = [&](const ScriptMemberSymbol& member) {
        auto kind = SymbolKind::field;
        if (member.kind == ScriptMemberKind::constant)
            kind = SymbolKind::constant;
        else if (member.kind == ScriptMemberKind::enum_value)
            kind = SymbolKind::enum_value;
        else if (member.kind == ScriptMemberKind::function)
            kind = SymbolKind::function;
        else if (member.kind == ScriptMemberKind::signal)
            kind = SymbolKind::signal;
        scopes_.front().insert_or_assign(member.name,
                                         Symbol{kind,
                                                member.name,
                                                member.type,
                                                {},
                                                member.kind == ScriptMemberKind::constant ||
                                                    member.kind == ScriptMemberKind::enum_value});
        if (member.kind == ScriptMemberKind::field && member.has_accessor)
            accessor_fields_.insert(member.name);
        if (member.kind == ScriptMemberKind::field && member.is_static)
            static_fields_.insert(member.name);
    };
    if (current_inner_script_base && script_symbols_) {
        std::vector<const ScriptClassSymbol*> hierarchy;
        for (auto* base = current_inner_script_base; base; base = script_symbols_->base_of(*base))
            hierarchy.push_back(base);
        for (auto base = hierarchy.rbegin(); base != hierarchy.rend(); ++base) {
            for (const auto& member : (*base)->members)
                import_inherited_member(member);
        }
    }
    if (current_inner_class_ && script_symbols_) {
        if (const auto* external = script_symbols_->external_base_of(*current_inner_class_)) {
            for (const auto& member : external->members)
                import_inherited_member(member);
        }
    }
    if (current_inner_base_) {
        std::vector<const ScriptInnerClassSymbol*> hierarchy;
        std::unordered_set<const ScriptInnerClassSymbol*> visited;
        for (auto* base = current_inner_base_; base && visited.insert(base).second;
             base = inner_base_of(*base)) {
            hierarchy.push_back(base);
        }
        for (auto base = hierarchy.rbegin(); base != hierarchy.rend(); ++base) {
            for (const auto& member : (*base)->members)
                import_inherited_member(member);
        }
    }
    // Internal classes retain lexical access to enums declared by their owning script. Rebuild
    // that outer enum scope after isolating the class body so unqualified annotations and values
    // resolve to the same canonical enum identity as code at script scope.
    if (current_script_) {
        for (const auto& enumeration : current_script_->enums) {
            const auto qualified = current_script_->native_class_name + "::" + enumeration.name;
            scopes_.front().insert_or_assign(enumeration.name,
                                             Symbol{SymbolKind::enum_type,
                                                    enumeration.name,
                                                    {TypeKind::enumeration, qualified},
                                                    {},
                                                    true});
            auto& values = enum_members_[qualified];
            for (const auto& entry : enumeration.entries)
                values.insert_or_assign(entry.name, entry.value);
        }
    }
    analyze_enums(declaration.enums);
    for (const auto& function : declaration.functions)
        functions_.emplace(function.name, &function);
    for (const auto& variable : declaration.variables) {
        if (variable.is_static)
            static_fields_.insert(variable.name);
        if (variable.getter || variable.setter) {
            accessor_fields_.insert(variable.name);
            if (variable.getter && !variable.getter->method.empty())
                bound_accessor_fields_[variable.getter->method].insert(variable.name);
            if (variable.setter && !variable.setter->method.empty())
                bound_accessor_fields_[variable.setter->method].insert(variable.name);
        }
        Type type = variable.type ? type_from_name(*variable.type, variable.span) : variant_type;
        if (variable.infer_type || (variable.is_constant && !variable.type))
            type = unknown_type;
        declare({variable.is_constant ? SymbolKind::constant : SymbolKind::field, variable.name,
                 type, variable.span, variable.is_constant});
    }
    for (const auto& signal : declaration.signals) {
        declare(
            {SymbolKind::signal, signal.name, {TypeKind::builtin, "Signal"}, signal.span, true});
    }
    for (const auto& function : declaration.functions) {
        const auto type = function.name == "_init" ? void_type
                          : function.return_type
                              ? type_from_name(*function.return_type, function.span)
                              : variant_type;
        declare({SymbolKind::function, function.name, type, function.span, true});
    }
    const auto analyze_internal_variable = [&](const ast::VariableDeclaration& variable) {
        const WarningIgnoreScope variable_warning_scope{active_warning_ignores_,
                                                        variable.annotations};
        const auto saved_variable_instance_context = instance_context_available_;
        instance_context_available_ = !variable.is_static;
        const auto initializer =
            variable.initializer ? analyze_expression(*variable.initializer) : variant_type;
        instance_context_available_ = saved_variable_instance_context;
        Type type = variable.type ? type_from_name(*variable.type, variable.span) : variant_type;
        if (variable.infer_type) {
            require_inferable_type(initializer, variable.span, "internal field");
            type = initializer;
            if (variable.initializer) {
                require_expression_assignable(type, *variable.initializer, initializer,
                                              variable.span,
                                              "invalid inferred internal field initializer");
            }
        } else if (variable.is_constant && !variable.type) {
            type = initializer;
        } else if (variable.type && variable.initializer) {
            require_expression_assignable(type, *variable.initializer, initializer, variable.span,
                                          "invalid internal field initializer");
        }
        model_.variable_types_[&variable] = type;
        const auto property_type = has_property_annotation(variable)
                                       ? export_property_type(variable, type, initializer)
                                       : type;
        model_.property_types_[&variable] = property_type;
        validate_annotations(variable, property_type);
        if (current_inner_class_) {
            if (auto owner = local_inner_classes_.find(current_inner_class_->name);
                owner != local_inner_classes_.end()) {
                const auto member =
                    std::find_if(owner->second.members.begin(), owner->second.members.end(),
                                 [&](const auto& value) { return value.name == variable.name; });
                if (member != owner->second.members.end())
                    member->type = type;
            }
        }
        if (const auto found = scopes_.back().find(variable.name); found != scopes_.back().end()) {
            found->second.type = type;
            if (variable.is_constant && variable.initializer) {
                found->second.constant_string_value =
                    constant_string_expression(*variable.initializer);
                found->second.constant_integer_value =
                    constant_integer_expression(*variable.initializer);
                found->second.constant_value_type = initializer;
            }
        }
    };
    // Constants are visible throughout a GDScript class regardless of textual order. Resolve
    // them before ordinary field initializers so forward constant references keep their native
    // scalar type instead of degrading to Variant.
    for (const auto& variable : declaration.variables) {
        if (variable.is_constant)
            analyze_internal_variable(variable);
    }
    for (const auto& variable : declaration.variables) {
        if (!variable.is_constant)
            analyze_internal_variable(variable);
    }
    for (const auto& signal : declaration.signals) {
        std::unordered_set<std::string> names;
        for (const auto& parameter : signal.parameters) {
            const auto type =
                parameter.type ? type_from_name(*parameter.type, parameter.span) : variant_type;
            model_.parameter_types_[&parameter] = type;
            if (!names.insert(parameter.name).second) {
                diagnostics_.error("GDS4010", "duplicate signal parameter '" + parameter.name + "'",
                                   parameter.span);
            }
        }
    }
    for (const auto& variable : declaration.variables)
        analyze_property_accessors(variable, model_.type_of(variable));
    for (const auto& function : declaration.functions)
        analyze_function(function);
    for (const auto& nested : declaration.classes)
        analyze_class(nested);

    scopes_ = std::move(saved_scopes);
    enum_types_ = std::move(saved_enum_types);
    enum_members_ = std::move(saved_enum_members);
    accessor_fields_ = std::move(saved_accessor_fields);
    static_fields_ = std::move(saved_static_fields);
    current_accessor_fields_ = std::move(saved_current_accessor_fields);
    bound_accessor_fields_ = std::move(saved_bound_accessor_fields);
    functions_ = std::move(saved_functions);
    allow_dynamic_await_return_ = saved_dynamic_await;
    current_function_static_ = saved_static;
    instance_context_available_ = saved_instance_context;
    in_function_ = saved_in_function;
    expected_return_ = saved_return;
    base_type_ = saved_base;
    loop_depth_ = saved_loop_depth;
    current_inner_class_ = saved_inner;
    current_inner_base_ = saved_inner_base;
}

void SemanticAnalyzer::validate_script_abstract_contract(const ast::Script& script) {
    const bool class_is_abstract = std::any_of(
        script.annotations.begin(), script.annotations.end(),
        [](const ast::Annotation& annotation) { return annotation.name == "abstract"; });
    if (class_is_abstract)
        return;

    std::unordered_set<std::string> implemented;
    for (const auto& function : script.functions) {
        if (function.is_abstract && !function.is_static) {
            diagnostics_.error("GDS4149",
                               "concrete script class declares abstract method '" + function.name +
                                   "'; mark the class @abstract or implement the method",
                               function.span);
        } else {
            implemented.insert(function.name);
        }
    }

    if (script.base_type) {
        const auto* local_base = find_inner_class(*script.base_type);
        for (auto* base = local_base; base && base->is_abstract; base = inner_base_of(*base)) {
            for (const auto& member : base->members) {
                if (member.kind != ScriptMemberKind::function)
                    continue;
                if (member.is_abstract && implemented.find(member.name) == implemented.end()) {
                    diagnostics_.error(
                        "GDS4149",
                        "concrete script class must implement inherited abstract method '" +
                            base->name + "." + member.name + "' or be marked @abstract",
                        script.span);
                } else if (!member.is_abstract) {
                    implemented.insert(member.name);
                }
            }
        }
    }

    if (!script_symbols_ || !current_script_)
        return;
    for (auto* base = script_symbols_->base_of(*current_script_); base && base->is_abstract;
         base = script_symbols_->base_of(*base)) {
        for (const auto& member : base->members) {
            if (member.kind != ScriptMemberKind::function)
                continue;
            if (member.is_abstract && implemented.find(member.name) == implemented.end()) {
                diagnostics_.error(
                    "GDS4149",
                    "concrete script class must implement inherited abstract method '" +
                        base->script_name + "." + member.name + "' or be marked @abstract",
                    script.span);
            } else if (!member.is_abstract) {
                implemented.insert(member.name);
            }
        }
    }
}

void SemanticAnalyzer::validate_inner_abstract_contract(const ast::ClassDeclaration& declaration) {
    const bool class_is_abstract =
        std::count_if(
            declaration.annotations.begin(), declaration.annotations.end(),
            [](const ast::Annotation& annotation) { return annotation.name == "abstract"; }) == 1;
    if (class_is_abstract)
        return;

    std::unordered_set<std::string> implemented;
    for (const auto& function : declaration.functions) {
        if (function.is_abstract && !function.is_static) {
            diagnostics_.error("GDS4149",
                               "concrete internal class '" + declaration.name +
                                   "' declares abstract method '" + function.name +
                                   "'; mark the class @abstract or implement the method",
                               function.span);
        } else {
            implemented.insert(function.name);
        }
    }

    for (auto* base = current_inner_base_; base && base->is_abstract; base = inner_base_of(*base)) {
        for (const auto& member : base->members) {
            if (member.kind != ScriptMemberKind::function)
                continue;
            if (member.is_abstract && implemented.find(member.name) == implemented.end()) {
                diagnostics_.error("GDS4149",
                                   "concrete internal class '" + declaration.name +
                                       "' must implement inherited abstract method '" + base->name +
                                       "." + member.name + "' or be marked @abstract",
                                   declaration.span);
            } else if (!member.is_abstract) {
                implemented.insert(member.name);
            }
        }
    }
    if (!script_symbols_ || !current_inner_class_)
        return;
    const ScriptInnerClassSymbol* terminal = current_inner_class_;
    std::unordered_set<const ScriptInnerClassSymbol*> visited;
    while (terminal && visited.insert(terminal).second) {
        if (const auto* base = inner_script_base_of(*terminal)) {
            for (auto* script_base = base; script_base && script_base->is_abstract;
                 script_base = script_symbols_->base_of(*script_base)) {
                for (const auto& member : script_base->members) {
                    if (member.kind != ScriptMemberKind::function)
                        continue;
                    if (member.is_abstract && implemented.find(member.name) == implemented.end()) {
                        diagnostics_.error("GDS4149",
                                           "concrete internal class '" + declaration.name +
                                               "' must implement inherited abstract method '" +
                                               script_base->script_name + "." + member.name +
                                               "' or be marked @abstract",
                                           declaration.span);
                    } else if (!member.is_abstract) {
                        implemented.insert(member.name);
                    }
                }
            }
            break;
        }
        terminal = inner_base_of(*terminal);
    }
}

void SemanticAnalyzer::analyze_property_accessors(const ast::VariableDeclaration& variable,
                                                  const Type& type) {
    const WarningIgnoreScope warning_scope{active_warning_ignores_, variable.annotations};
    if (!variable.getter && !variable.setter)
        return;
    if (variable.is_constant) {
        diagnostics_.error("GDS4049", "constants cannot have property accessors", variable.span);
        return;
    }
    current_accessor_fields_.clear();
    current_accessor_fields_.insert(variable.name);
    const auto saved_return = expected_return_;
    const auto saved_dynamic_await = allow_dynamic_await_return_;
    const auto saved_static = current_function_static_;
    const auto saved_instance_context = instance_context_available_;
    const auto saved_in_function = in_function_;
    const auto prepare_accessor_body = [&](const std::vector<ast::Statement>&,
                                           const Type& return_type) {
        expected_return_ = return_type;
        allow_dynamic_await_return_ = false;
        current_function_static_ = variable.is_static;
        instance_context_available_ = !variable.is_static;
        in_function_ = true;
    };
    if (variable.getter) {
        if (!variable.getter->method.empty()) {
            const auto found = functions_.find(variable.getter->method);
            if (found == functions_.end()) {
                diagnostics_.error("GDS4082",
                                   "property getter method '" + variable.getter->method +
                                       "' was not found",
                                   variable.getter->span);
            } else {
                const auto& function = *found->second;
                if (!function.parameters.empty()) {
                    diagnostics_.error("GDS4083", "property getter method must accept no arguments",
                                       variable.getter->span);
                }
                if (function.is_static != variable.is_static) {
                    diagnostics_.error("GDS4084",
                                       "property getter method staticness must match its field",
                                       variable.getter->span);
                }
                const auto return_type = function.return_type
                                             ? type_from_name(*function.return_type, function.span)
                                             : variant_type;
                require_assignable(type, return_type, variable.getter->span,
                                   "property getter result");
            }
        } else {
            prepare_accessor_body(variable.getter->body, type);
            scopes_.emplace_back();
            const auto flow = analyze_statements(variable.getter->body);
            scopes_.pop_back();
            if (flow.falls_through) {
                diagnostics_.error("GDS4050", "property getter must return a value on every path",
                                   variable.getter->span);
            }
        }
    }
    if (variable.setter) {
        if (!variable.setter->method.empty()) {
            const auto found = functions_.find(variable.setter->method);
            if (found == functions_.end()) {
                diagnostics_.error("GDS4085",
                                   "property setter method '" + variable.setter->method +
                                       "' was not found",
                                   variable.setter->span);
            } else {
                const auto& function = *found->second;
                if (function.parameters.size() != 1) {
                    diagnostics_.error("GDS4086",
                                       "property setter method must accept exactly one argument",
                                       variable.setter->span);
                } else {
                    const auto parameter_type =
                        function.parameters.front().type
                            ? type_from_name(*function.parameters.front().type,
                                             function.parameters.front().span)
                            : variant_type;
                    require_assignable(parameter_type, type, variable.setter->span,
                                       "property setter argument");
                }
                if (function.is_static != variable.is_static) {
                    diagnostics_.error("GDS4087",
                                       "property setter method staticness must match its field",
                                       variable.setter->span);
                }
                if (function.return_type && *function.return_type != "void") {
                    diagnostics_.error("GDS4088", "property setter method must return void",
                                       variable.setter->span);
                }
            }
        } else {
            prepare_accessor_body(variable.setter->body, void_type);
            if (variable.setter->parameter.empty()) {
                diagnostics_.error("GDS4051", "property setter requires a parameter",
                                   variable.setter->span);
            }
            expected_return_ = void_type;
            scopes_.emplace_back();
            declare({SymbolKind::parameter, variable.setter->parameter, type, variable.setter->span,
                     false});
            (void)analyze_statements(variable.setter->body);
            scopes_.pop_back();
        }
    }
    expected_return_ = saved_return;
    allow_dynamic_await_return_ = saved_dynamic_await;
    current_function_static_ = saved_static;
    instance_context_available_ = saved_instance_context;
    in_function_ = saved_in_function;
    current_accessor_fields_.clear();
}

void SemanticAnalyzer::analyze_enums(const std::vector<ast::EnumDeclaration>& declarations) {
    for (const auto& declaration : declarations) {
        if (declaration.name) {
            if (!enum_types_.insert(*declaration.name).second) {
                diagnostics_.error("GDS4036", "duplicate enum type '" + *declaration.name + "'",
                                   declaration.span);
            } else {
                const auto qualified_name =
                    current_inner_class_
                        ? (current_inner_class_->native_class_name.empty()
                               ? current_inner_class_->name + "." + *declaration.name
                               : current_inner_class_->native_class_name + "::" + *declaration.name)
                    : current_script_
                        ? current_script_->native_class_name + "::" + *declaration.name
                        : *declaration.name;
                declare({SymbolKind::enum_type,
                         *declaration.name,
                         {TypeKind::enumeration, qualified_name},
                         declaration.span,
                         true});
            }
        }
    }

    for (const auto& declaration : declarations) {
        if (declaration.entries.empty()) {
            diagnostics_.error("GDS4037", "an enum must declare at least one member",
                               declaration.span);
            continue;
        }
        std::unordered_map<std::string, std::int64_t> values;
        std::int64_t next_value = 0;
        for (const auto& entry : declaration.entries) {
            if (values.find(entry.name) != values.end()) {
                diagnostics_.error("GDS4038", "duplicate enum member '" + entry.name + "'",
                                   entry.span);
                continue;
            }
            auto value = entry.value ? evaluate_integer_constant(*entry.value, values)
                                     : std::optional<std::int64_t>{next_value};
            if (!value) {
                diagnostics_.error("GDS4039",
                                   "enum member '" + entry.name +
                                       "' requires a valid integer constant expression",
                                   entry.span);
                value = 0;
            }
            values.emplace(entry.name, *value);
            model_.enum_values_[&entry] = *value;
            if (!declaration.name) {
                declare({SymbolKind::enum_value,
                         entry.name,
                         {TypeKind::integer, "int"},
                         entry.span,
                         true});
            }
            if (*value == std::numeric_limits<std::int64_t>::max()) {
                if (&entry != &declaration.entries.back()) {
                    diagnostics_.error("GDS4040", "automatic enum value overflows int64",
                                       entry.span);
                }
                next_value = 0;
            } else {
                next_value = *value + 1;
            }
        }
        if (declaration.name) {
            const auto qualified_name =
                current_inner_class_
                    ? (current_inner_class_->native_class_name.empty()
                           ? current_inner_class_->name + "." + *declaration.name
                           : current_inner_class_->native_class_name + "::" + *declaration.name)
                : current_script_ ? current_script_->native_class_name + "::" + *declaration.name
                                  : *declaration.name;
            enum_members_[qualified_name] = std::move(values);
        }
    }
}

void SemanticAnalyzer::validate_annotations(const ast::VariableDeclaration& variable,
                                            const Type& type) {
    if (variable.annotations.empty())
        return;

    const ast::Annotation* property_annotation = nullptr;
    for (const auto& candidate : variable.annotations) {
        if (candidate.name == "onready") {
            if (!candidate.arguments.empty()) {
                diagnostics_.error("GDS4021", "@onready does not accept arguments", candidate.span);
            }
            if (variable.is_constant) {
                diagnostics_.error("GDS4024", "@onready cannot be applied to constants",
                                   candidate.span);
            }
            if (variable.is_static) {
                diagnostics_.error("GDS4024", "@onready cannot be applied to static variables",
                                   candidate.span);
            }
            continue;
        }
        if (candidate.name == "warning_ignore")
            continue;
        if (candidate.name == "export_group" || candidate.name == "export_subgroup" ||
            candidate.name == "export_category") {
            const auto maximum =
                candidate.name == "export_category" ? std::size_t{1} : std::size_t{2};
            if (candidate.arguments.empty() || candidate.arguments.size() > maximum) {
                diagnostics_.error("GDS4022",
                                   "@" + candidate.name +
                                       (maximum == 1 ? " expects one string argument"
                                                     : " expects one or two string arguments"),
                                   candidate.span);
            }
            for (const auto& argument : candidate.arguments) {
                if (!is_string_literal(*argument)) {
                    diagnostics_.error("GDS4023",
                                       "@" + candidate.name + " arguments must be string literals",
                                       argument->span);
                }
            }
            continue;
        }
        if (property_annotation) {
            diagnostics_.error("GDS4020", "a field accepts at most one export annotation",
                               candidate.span);
        } else {
            property_annotation = &candidate;
        }
    }
    if (!property_annotation)
        return;
    const auto& annotation = *property_annotation;
    const auto& name = annotation.name;
    const auto argument_count = annotation.arguments.size();
    const auto require_no_arguments = [&] {
        if (argument_count != 0) {
            diagnostics_.error("GDS4021", "@" + name + " does not accept arguments",
                               annotation.span);
            return false;
        }
        return true;
    };
    const auto require_string_arguments = [&](std::size_t minimum, std::size_t maximum) {
        if (argument_count < minimum || argument_count > maximum) {
            diagnostics_.error("GDS4022",
                               "@" + name + " expects " + std::to_string(minimum) +
                                   (minimum == maximum ? "" : " to " + std::to_string(maximum)) +
                                   " string argument(s)",
                               annotation.span);
            return false;
        }
        bool valid = true;
        for (const auto& argument : annotation.arguments) {
            if (!is_string_literal(*argument)) {
                diagnostics_.error("GDS4023", "@" + name + " arguments must be string literals",
                                   argument->span);
                valid = false;
            }
        }
        return valid;
    };
    if (variable.is_constant) {
        diagnostics_.error("GDS4024", "export annotations cannot be applied to constants",
                           annotation.span);
        return;
    }
    if (variable.is_static) {
        diagnostics_.error("GDS4024", "export annotations cannot be applied to static variables",
                           annotation.span);
        return;
    }
    const bool unchecked_export =
        name == "export_storage" || name == "export_custom" || name == "export_enum";
    if (!unchecked_export &&
        (type.is_dynamic() || type.kind == TypeKind::nil || type.kind == TypeKind::void_type)) {
        diagnostics_.error("GDS4025", "exported fields require a concrete serializable type",
                           variable.span);
    }
    const auto* script_type = script_symbols_ && type.kind == TypeKind::object
                                  ? script_symbols_->find_global(type.name)
                                  : nullptr;
    const bool script_serializable =
        script_type && (api_.inherits(script_type->godot_base_type, "Node") ||
                        api_.inherits(script_type->godot_base_type, "Resource"));
    if (!unchecked_export && type.kind == TypeKind::object && !api_.inherits(type.name, "Node") &&
        !api_.inherits(type.name, "Resource") && !script_serializable) {
        diagnostics_.error("GDS4035", "exported object fields must derive from Node or Resource",
                           variable.span);
    }

    const auto value_type = exported_value_type(type);
    const bool string_collection = value_type.kind == TypeKind::string;
    const bool integer_collection = value_type.kind == TypeKind::integer;
    const bool numeric_collection = integer_collection || value_type.kind == TypeKind::floating;
    const bool color_collection =
        value_type.kind == TypeKind::builtin && value_type.name == "Color";
    const bool node_path_collection =
        value_type.kind == TypeKind::builtin && value_type.name == "NodePath";
    if (name == "export") {
        require_no_arguments();
    } else if (name == "export_range") {
        if (!numeric_collection) {
            diagnostics_.error("GDS4026", "@export_range requires an int or float field",
                               variable.span);
        }
        if (argument_count < 2) {
            diagnostics_.error("GDS4027", "@export_range requires minimum and maximum values",
                               annotation.span);
        }
        for (std::size_t index = 0; index < argument_count; ++index) {
            const bool valid = index < 3 ? is_number_literal(*annotation.arguments[index])
                                         : is_string_literal(*annotation.arguments[index]);
            if (!valid) {
                diagnostics_.error("GDS4028",
                                   "@export_range expects numeric bounds/step followed by "
                                   "optional string flags",
                                   annotation.arguments[index]->span);
            }
        }
    } else if (name == "export_enum") {
        if (!type.is_dynamic() && value_type.kind != TypeKind::integer &&
            value_type.kind != TypeKind::string) {
            diagnostics_.error("GDS4029", "@export_enum requires an int or String field",
                               variable.span);
        }
        require_string_arguments(1, 256);
    } else if (name == "export_flags") {
        if (!integer_collection) {
            diagnostics_.error("GDS4030", "@export_flags requires an int field", variable.span);
        }
        require_string_arguments(1, 32);
    } else if (name == "export_flags_2d_render" || name == "export_flags_2d_physics" ||
               name == "export_flags_2d_navigation" || name == "export_flags_3d_render" ||
               name == "export_flags_3d_physics" || name == "export_flags_3d_navigation" ||
               name == "export_flags_avoidance") {
        if (!integer_collection) {
            diagnostics_.error("GDS4030", "@" + name + " requires an int collection field",
                               variable.span);
        }
        require_no_arguments();
    } else if (name == "export_file" || name == "export_file_path" ||
               name == "export_global_file") {
        if (!string_collection) {
            diagnostics_.error("GDS4031", "@" + name + " requires a String collection field",
                               variable.span);
        }
        require_string_arguments(0, 256);
    } else if (name == "export_dir" || name == "export_global_dir") {
        if (!string_collection) {
            diagnostics_.error("GDS4031", "@" + name + " requires a String collection field",
                               variable.span);
        }
        require_no_arguments();
    } else if (name == "export_multiline") {
        const bool multiline_type = string_collection || type.kind == TypeKind::dictionary ||
                                    value_type.kind == TypeKind::dictionary;
        if (!multiline_type) {
            diagnostics_.error("GDS4031", "@export_multiline requires text or Dictionary values",
                               variable.span);
        }
        require_string_arguments(0, 256);
    } else if (name == "export_color_no_alpha") {
        if (!color_collection) {
            diagnostics_.error("GDS4032", "@export_color_no_alpha requires a Color field",
                               variable.span);
        }
        require_no_arguments();
    } else if (name == "export_node_path") {
        if (!node_path_collection) {
            diagnostics_.error("GDS4033", "@export_node_path requires a NodePath field",
                               variable.span);
        }
        require_string_arguments(0, 64);
    } else if (name == "export_placeholder") {
        if (!string_collection) {
            diagnostics_.error("GDS4031", "@export_placeholder requires a String collection field",
                               variable.span);
        }
        require_string_arguments(1, 1);
    } else if (name == "export_exp_easing") {
        if (value_type.kind != TypeKind::floating) {
            diagnostics_.error("GDS4026", "@export_exp_easing requires a float collection field",
                               variable.span);
        }
        require_string_arguments(0, 2);
    } else if (name == "export_storage") {
        require_no_arguments();
    } else if (name == "export_tool_button") {
        if (!script_tool_) {
            diagnostics_.error("GDS4036", "@export_tool_button requires a @tool script",
                               annotation.span);
        }
        if (type.kind != TypeKind::builtin || type.name != "Callable") {
            diagnostics_.error("GDS4037", "@export_tool_button requires a Callable field",
                               variable.span);
        }
        require_string_arguments(1, 2);
    } else if (name == "export_custom") {
        if (argument_count < 2 || argument_count > 3) {
            diagnostics_.error("GDS4022", "@export_custom expects 2 or 3 arguments",
                               annotation.span);
        }
        const auto require_constant_integer = [&](const std::size_t index,
                                                  const std::string& role) {
            if (index >= argument_count)
                return;
            const auto& argument = *annotation.arguments[index];
            const auto value = constant_integer_expression(argument);
            if (value) {
                model_.constant_integer_values_.insert_or_assign(&argument, *value);
                return;
            }
            const auto diagnose_unknown_identifiers =
                [&](const auto& self, const ast::Expression& candidate) -> void {
                if (candidate.kind() == ast::ExpressionKind::identifier &&
                    !resolve(candidate.value()) && !api_.find_global_constant(candidate.value()) &&
                    !api_.find_global_enum_value(candidate.value())) {
                    diagnostics_.error("GDS4122", "unknown identifier '" + candidate.value() + "'",
                                       candidate.span);
                }
                for (std::size_t operand = 0; operand < candidate.operand_count(); ++operand)
                    self(self, *candidate.operand(operand));
            };
            diagnose_unknown_identifiers(diagnose_unknown_identifiers, argument);
            diagnostics_.error("GDS4145",
                               "@export_custom " + role + " must be an integer constant expression",
                               argument.span);
        };
        require_constant_integer(0, "property hint");
        if (argument_count >= 2 && !is_string_literal(*annotation.arguments[1])) {
            diagnostics_.error("GDS4023", "@export_custom hint string must be a string literal",
                               annotation.arguments[1]->span);
        }
        require_constant_integer(2, "property usage flags");
    } else {
        diagnostics_.error("GDS4034", "unsupported annotation '@" + name + "'", annotation.span);
    }
}

SemanticModel SemanticAnalyzer::analyze(const ast::Script& script) {
    model_ = {};
    scopes_.clear();
    enum_types_.clear();
    enum_members_.clear();
    accessor_fields_.clear();
    static_fields_.clear();
    current_accessor_fields_.clear();
    bound_accessor_fields_.clear();
    functions_.clear();
    local_inner_classes_.clear();
    script_resource_aliases_.clear();
    allow_dynamic_await_return_ = false;
    current_function_static_ = false;
    instance_context_available_ = true;
    current_inner_class_ = nullptr;
    current_inner_base_ = nullptr;
    script_tool_ = script.tool;
    const auto abstract_annotations = static_cast<std::size_t>(std::count_if(
        script.annotations.begin(), script.annotations.end(),
        [](const ast::Annotation& annotation) { return annotation.name == "abstract"; }));
    if (abstract_annotations > 1) {
        diagnostics_.error("GDS4147", "@abstract can only be used once per script class",
                           script.span);
    }
    for (const auto& annotation : script.annotations) {
        if (annotation.name != "icon")
            continue;
        if (annotation.arguments.size() != 1 || !is_string_literal(*annotation.arguments.front())) {
            diagnostics_.error("GDS4114", "@icon expects one string literal path", annotation.span);
        } else if (annotation.arguments.front()->value().empty()) {
            diagnostics_.error("GDS4115", "@icon path cannot be empty", annotation.span);
        }
    }
    current_script_ = script_symbols_ ? script_symbols_->find_path(current_script_path_) : nullptr;
    if (script_symbols_ && current_script_)
        record_script_dependency(script_symbols_->base_of(*current_script_));
    if (script_symbols_) {
        for (const auto& variable : script.variables) {
            if (!variable.is_constant || !variable.initializer)
                continue;
            const auto* call = variable.initializer->get_if<ast::CallExpression>();
            if (!call || !call->callee || call->callee->kind() != ast::ExpressionKind::identifier ||
                call->callee->value() != "preload" || call->arguments.size() != 1 ||
                !call->arguments.front() ||
                call->arguments.front()->literal_kind() != ast::LiteralKind::string) {
                continue;
            }
            if (const auto* target = script_symbols_->resolve_path(
                    current_script_path_, call->arguments.front()->value())) {
                script_resource_aliases_.insert_or_assign(variable.name, target);
                record_script_dependency(target);
            }
        }
    }
    Scope inherited_scope;
    if (script_symbols_ && current_script_) {
        for (const auto* member : script_symbols_->inherited_members(*current_script_)) {
            auto kind = SymbolKind::field;
            if (member->kind == ScriptMemberKind::constant)
                kind = SymbolKind::constant;
            else if (member->kind == ScriptMemberKind::enum_value)
                kind = SymbolKind::enum_value;
            else if (member->kind == ScriptMemberKind::function)
                kind = SymbolKind::function;
            else if (member->kind == ScriptMemberKind::signal)
                kind = SymbolKind::signal;
            inherited_scope.emplace(member->name,
                                    Symbol{kind,
                                           member->name,
                                           member->type,
                                           {},
                                           member->kind == ScriptMemberKind::constant ||
                                               member->kind == ScriptMemberKind::enum_value});
            if (member->kind == ScriptMemberKind::field && member->has_accessor)
                accessor_fields_.insert(member->name);
            if (member->kind == ScriptMemberKind::field && member->is_static)
                static_fields_.insert(member->name);
        }
    }
    scopes_.push_back(std::move(inherited_scope));
    scopes_.emplace_back();
    const auto declared_base = script.base_type.value_or("Node");
    base_type_ = semantic_base_type_.empty() ? declared_base : semantic_base_type_;
    if (semantic_base_type_.empty() && !api_.find_class(base_type_)) {
        diagnostics_.error("GDS4052",
                           "base script '" + declared_base +
                               "' requires project-level inheritance resolution",
                           script.span);
    }
    const bool requires_tool_base = base_type_ == "EditorPlugin" || base_type_ == "EditorScript" ||
                                    api_.inherits(base_type_, "EditorPlugin") ||
                                    api_.inherits(base_type_, "EditorScript");
    if (requires_tool_base && !script_tool_) {
        diagnostics_.error(
            "GDS4152", "scripts derived from '" + base_type_ + "' must declare @tool", script.span);
    }
    const auto register_inner_classes = [&](const auto& self, const auto& declarations,
                                            const std::string& parent) -> void {
        for (const auto& declaration : declarations) {
            const auto qualified =
                parent.empty() ? declaration.name : parent + "." + declaration.name;
            ScriptInnerClassSymbol symbol;
            symbol.name = qualified;
            if (current_script_) {
                if (const auto* published =
                        script_symbols_->find_inner(*current_script_, qualified)) {
                    symbol.native_class_name = published->native_class_name;
                    symbol.godot_base_type = published->godot_base_type;
                    symbol.attached_native_base = published->attached_native_base;
                    symbol.external_base_name = published->external_base_name;
                    symbol.base_class_name = published->base_class_name;
                    symbol.base_script_path = published->base_script_path;
                }
            }
            if (symbol.native_class_name.empty())
                symbol.godot_base_type = declaration.base_type.value_or("RefCounted");
            symbol.is_abstract = std::any_of(
                declaration.annotations.begin(), declaration.annotations.end(),
                [](const ast::Annotation& annotation) { return annotation.name == "abstract"; });
            if (!local_inner_classes_.emplace(qualified, std::move(symbol)).second) {
                diagnostics_.error("GDS4101", "duplicate internal class '" + qualified + "'",
                                   declaration.span);
            }
            self(self, declaration.classes, qualified);
        }
    };
    register_inner_classes(register_inner_classes, script.classes, "");
    const auto resolve_inner_base_name = [&](const std::string& owner,
                                             const std::string& base) -> std::string {
        if (local_inner_classes_.find(base) != local_inner_classes_.end())
            return base;
        const auto separator = owner.rfind('.');
        if (separator != std::string::npos) {
            const auto lexical = owner.substr(0, separator + 1) + base;
            if (local_inner_classes_.find(lexical) != local_inner_classes_.end())
                return lexical;
        }
        const ScriptInnerClassSymbol* unique = nullptr;
        for (const auto& [qualified, candidate] : local_inner_classes_) {
            const auto leaf_separator = qualified.rfind('.');
            const auto leaf = leaf_separator == std::string::npos
                                  ? qualified
                                  : qualified.substr(leaf_separator + 1);
            if (leaf != base)
                continue;
            if (unique)
                return {};
            unique = &candidate;
        }
        return unique ? unique->name : std::string{};
    };
    const auto resolve_inner_bases = [&](const auto& self, const auto& declarations,
                                         const std::string& parent) -> void {
        for (const auto& declaration : declarations) {
            const auto qualified =
                parent.empty() ? declaration.name : parent + "." + declaration.name;
            if (auto found = local_inner_classes_.find(qualified);
                found != local_inner_classes_.end() && declaration.base_type &&
                !api_.find_class(*declaration.base_type) &&
                found->second.base_script_path.empty()) {
                found->second.base_class_name =
                    resolve_inner_base_name(qualified, *declaration.base_type);
            }
            self(self, declaration.classes, qualified);
        }
    };
    resolve_inner_bases(resolve_inner_bases, script.classes, "");
    for (auto& [name, inner] : local_inner_classes_) {
        std::unordered_set<std::string> visited{name};
        auto* current = &inner;
        while (!current->base_class_name.empty()) {
            if (!visited.insert(current->base_class_name).second) {
                diagnostics_.error("GDS4102",
                                   "cyclic internal class inheritance involving '" + name + "'",
                                   script.span);
                current->base_class_name.clear();
                break;
            }
            const auto base = local_inner_classes_.find(current->base_class_name);
            if (base == local_inner_classes_.end())
                break;
            current = &base->second;
        }
        inner.godot_base_type = current->godot_base_type;
        inner.attached_native_base = current->attached_native_base;
        inner.external_base_name = current->external_base_name;
    }
    const auto populate_inner_enums = [&](const auto& self, const auto& declarations,
                                          const std::string& parent) -> void {
        for (const auto& declaration : declarations) {
            const auto qualified =
                parent.empty() ? declaration.name : parent + "." + declaration.name;
            auto found = local_inner_classes_.find(qualified);
            if (found == local_inner_classes_.end())
                continue;
            for (const auto& enumeration : declaration.enums) {
                ScriptEnumSymbol enum_symbol;
                enum_symbol.name = enumeration.name.value_or("");
                std::unordered_map<std::string, std::int64_t> previous;
                std::int64_t next_value = 0;
                for (const auto& entry : enumeration.entries) {
                    const auto value = entry.value
                                           ? evaluate_integer_constant(*entry.value, previous)
                                           : std::optional<std::int64_t>{next_value};
                    const auto resolved = value.value_or(0);
                    previous.emplace(entry.name, resolved);
                    enum_symbol.entries.push_back({entry.name, resolved});
                    if (!enumeration.name) {
                        ScriptMemberSymbol member;
                        member.kind = ScriptMemberKind::enum_value;
                        member.name = entry.name;
                        member.type = {TypeKind::integer, "int"};
                        member.is_static = true;
                        found->second.members.push_back(std::move(member));
                    }
                    if (resolved != std::numeric_limits<std::int64_t>::max())
                        next_value = resolved + 1;
                }
                if (enumeration.name)
                    found->second.enums.push_back(std::move(enum_symbol));
            }
            self(self, declaration.classes, qualified);
        }
    };
    populate_inner_enums(populate_inner_enums, script.classes, "");
    const auto populate_inner_classes = [&](const auto& self, const auto& declarations,
                                            const std::string& parent) -> void {
        for (const auto& declaration : declarations) {
            const auto qualified =
                parent.empty() ? declaration.name : parent + "." + declaration.name;
            auto found = local_inner_classes_.find(qualified);
            if (found == local_inner_classes_.end())
                continue;
            const auto* saved_inner = current_inner_class_;
            current_inner_class_ = &found->second;
            for (const auto& variable : declaration.variables) {
                ScriptMemberSymbol member;
                member.kind =
                    variable.is_constant ? ScriptMemberKind::constant : ScriptMemberKind::field;
                member.name = variable.name;
                member.type =
                    variable.type ? type_from_name(*variable.type, variable.span) : variant_type;
                member.is_static = variable.is_constant || variable.is_static;
                member.has_accessor = variable.getter.has_value() || variable.setter.has_value();
                found->second.members.push_back(std::move(member));
            }
            for (const auto& function : declaration.functions) {
                ScriptMemberSymbol member;
                member.kind = ScriptMemberKind::function;
                member.name = function.name;
                member.type = function.name == "_init" ? void_type
                              : function.return_type
                                  ? type_from_name(*function.return_type, function.span)
                                  : variant_type;
                member.is_static = function.is_static;
                member.is_vararg = function.rest_parameter.has_value();
                member.is_abstract = function.is_abstract;
                member.has_explicit_type =
                    function.name == "_init" || function.return_type.has_value();
                for (const auto& parameter : function.parameters) {
                    member.parameters.push_back(
                        parameter.type ? type_from_name(*parameter.type, parameter.span)
                                       : variant_type);
                    member.explicit_parameter_types.push_back(parameter.type.has_value() ||
                                                              parameter.infer_type);
                    member.default_parameters.push_back(parameter.default_value != nullptr);
                    if (!parameter.default_value)
                        ++member.required_arguments;
                }
                found->second.members.push_back(std::move(member));
            }
            for (const auto& signal : declaration.signals) {
                ScriptMemberSymbol member;
                member.kind = ScriptMemberKind::signal;
                member.name = signal.name;
                member.type = {TypeKind::builtin, "Signal"};
                for (const auto& parameter : signal.parameters) {
                    member.parameters.push_back(
                        parameter.type ? type_from_name(*parameter.type, parameter.span)
                                       : variant_type);
                    ++member.required_arguments;
                }
                found->second.members.push_back(std::move(member));
            }
            current_inner_class_ = saved_inner;
            self(self, declaration.classes, qualified);
        }
    };
    populate_inner_classes(populate_inner_classes, script.classes, "");
    validate_script_abstract_contract(script);
    if (const auto local_base = local_inner_classes_.find(declared_base);
        local_base != local_inner_classes_.end()) {
        for (const auto& member : local_base->second.members) {
            auto kind = SymbolKind::field;
            if (member.kind == ScriptMemberKind::constant)
                kind = SymbolKind::constant;
            else if (member.kind == ScriptMemberKind::enum_value)
                kind = SymbolKind::enum_value;
            else if (member.kind == ScriptMemberKind::function)
                kind = SymbolKind::function;
            else if (member.kind == ScriptMemberKind::signal)
                kind = SymbolKind::signal;
            scopes_.front().insert_or_assign(
                member.name, Symbol{kind,
                                    member.name,
                                    member.type,
                                    {},
                                    member.kind == ScriptMemberKind::constant ||
                                        member.kind == ScriptMemberKind::enum_value});
            if (member.kind == ScriptMemberKind::field && member.has_accessor)
                accessor_fields_.insert(member.name);
            if (member.kind == ScriptMemberKind::field && member.is_static)
                static_fields_.insert(member.name);
        }
    }
    analyze_enums(script.enums);

    for (const auto& function : script.functions)
        functions_.emplace(function.name, &function);

    for (const auto& variable : script.variables) {
        if (variable.is_static)
            static_fields_.insert(variable.name);
        if (variable.getter || variable.setter) {
            accessor_fields_.insert(variable.name);
            if (variable.getter && !variable.getter->method.empty())
                bound_accessor_fields_[variable.getter->method].insert(variable.name);
            if (variable.setter && !variable.setter->method.empty())
                bound_accessor_fields_[variable.setter->method].insert(variable.name);
        }
    }

    for (const auto& variable : script.variables) {
        Type type = variable.type.has_value() ? type_from_name(*variable.type, variable.span)
                                              : variant_type;
        if (variable.infer_type || (variable.is_constant && !variable.type.has_value())) {
            type = unknown_type;
        }
        declare({variable.is_constant ? SymbolKind::constant : SymbolKind::field, variable.name,
                 type, variable.span, variable.is_constant});
    }
    for (const auto& signal : script.signals) {
        declare(
            {SymbolKind::signal, signal.name, {TypeKind::builtin, "Signal"}, signal.span, true});
    }
    for (const auto& function : script.functions) {
        const auto type = function.name == "_init" ? void_type
                          : function.return_type.has_value()
                              ? type_from_name(*function.return_type, function.span)
                              : variant_type;
        declare({SymbolKind::function, function.name, type, function.span, true});
    }

    for (const auto& declaration : script.classes)
        analyze_class(declaration);

    const auto analyze_script_variable = [&](const ast::VariableDeclaration& variable) {
        const WarningIgnoreScope warning_scope{active_warning_ignores_, variable.annotations};
        const auto saved_instance_context = instance_context_available_;
        instance_context_available_ = !variable.is_static;
        const auto initializer =
            variable.initializer ? analyze_expression(*variable.initializer) : variant_type;
        instance_context_available_ = saved_instance_context;
        Type type = variable.type.has_value() ? type_from_name(*variable.type, variable.span)
                                              : variant_type;
        if (variable.infer_type) {
            require_inferable_type(initializer, variable.span, "field");
            type = initializer;
            if (variable.initializer) {
                require_expression_assignable(type, *variable.initializer, initializer,
                                              variable.span, "invalid inferred field initializer");
            }
        } else if (variable.is_constant && !variable.type.has_value()) {
            type = initializer;
        } else if (variable.type.has_value() && variable.initializer) {
            require_expression_assignable(type, *variable.initializer, initializer, variable.span,
                                          "invalid field initializer");
        }
        model_.variable_types_[&variable] = type;
        const auto property_type = has_property_annotation(variable)
                                       ? export_property_type(variable, type, initializer)
                                       : type;
        model_.property_types_[&variable] = property_type;
        validate_annotations(variable, property_type);
        const auto found = scopes_.back().find(variable.name);
        if (found != scopes_.back().end()) {
            found->second.type = type;
            if (variable.is_constant && variable.initializer) {
                found->second.constant_string_value =
                    constant_string_expression(*variable.initializer);
                found->second.constant_integer_value =
                    constant_integer_expression(*variable.initializer);
                found->second.constant_value_type = initializer;
            }
        }
    };
    for (const auto& variable : script.variables) {
        if (variable.is_constant)
            analyze_script_variable(variable);
    }
    for (const auto& variable : script.variables) {
        if (!variable.is_constant)
            analyze_script_variable(variable);
    }

    for (const auto& signal : script.signals) {
        Scope parameters;
        for (const auto& parameter : signal.parameters) {
            const auto type = parameter.type.has_value()
                                  ? type_from_name(*parameter.type, parameter.span)
                                  : variant_type;
            model_.parameter_types_[&parameter] = type;
            if (!parameters
                     .emplace(parameter.name, Symbol{SymbolKind::parameter, parameter.name, type,
                                                     parameter.span, true})
                     .second) {
                diagnostics_.error("GDS4010", "duplicate signal parameter '" + parameter.name + "'",
                                   parameter.span);
            }
        }
    }
    for (const auto& variable : script.variables)
        analyze_property_accessors(variable, model_.type_of(variable));
    for (const auto& function : script.functions)
        analyze_function(function);
    return std::move(model_);
}

} // namespace gdpp
