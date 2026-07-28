#pragma once

#include "gdpp/core/diagnostic.hpp"
#include "gdpp/frontend/ast.hpp"
#include "gdpp/semantic/default_argument.hpp"
#include "gdpp/semantic/flow.hpp"
#include "gdpp/semantic/godot_api.hpp"
#include "gdpp/semantic/intrinsics.hpp"
#include "gdpp/semantic/iteration.hpp"
#include "gdpp/semantic/rpc.hpp"
#include "gdpp/semantic/script_symbols.hpp"
#include "gdpp/semantic/type.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gdpp {

enum class SymbolKind {
    field,
    constant,
    function,
    signal,
    parameter,
    local,
    enum_type,
    enum_value
};

enum class SymbolStorage { class_member, function_local };

struct Symbol {
    SymbolKind kind{SymbolKind::local};
    std::string name;
    Type type;
    SourceSpan declaration{};
    bool read_only{false};
    std::optional<std::string> constant_string_value;
    SymbolStorage storage{SymbolStorage::class_member};
    std::optional<std::int64_t> constant_integer_value;
    FlowSymbolId identity{0};
    std::optional<Type> constant_value_type;

    Symbol() = default;
    Symbol(SymbolKind symbol_kind, std::string symbol_name, Type symbol_type,
           SourceSpan declaration_span, bool symbol_read_only,
           std::optional<std::string> string_value = std::nullopt,
           SymbolStorage symbol_storage = SymbolStorage::class_member,
           std::optional<std::int64_t> integer_value = std::nullopt,
           FlowSymbolId symbol_identity = 0, std::optional<Type> reduced_type = std::nullopt)
        : kind(symbol_kind), name(std::move(symbol_name)), type(std::move(symbol_type)),
          declaration(declaration_span), read_only(symbol_read_only),
          constant_string_value(std::move(string_value)),
          storage(symbol_kind == SymbolKind::local || symbol_kind == SymbolKind::parameter
                      ? SymbolStorage::function_local
                      : symbol_storage),
          constant_integer_value(integer_value), identity(symbol_identity),
          constant_value_type(std::move(reduced_type)) {}
};

enum class ApiResolutionKind {
    none,
    method,
    godot_callable,
    property,
    constructor,
    singleton,
    external_singleton,
    type_reference,
    external_type_reference,
    script_type_reference,
    script_autoload,
    script_constant,
    local_constant,
    script_enum_type,
    script_resource,
    script_constructor,
    external_constructor,
    external_static_method,
    external_super_method,
    external_callable,
    external_signal,
    script_super,
    script_signal,
    script_callable,
    script_static_callable,
    script_static_field,
    script_runtime_static_field,
    script_free,
    enum_member,
    script_property,
    dynamic_method,
    dynamic_property,
    utility_function,
    global_constant,
    global_enum_type,
    global_enum_value,
    builtin_constant,
    intrinsic,
    inner_type_reference,
    inner_constructor
};

struct ApiResolution {
    ApiResolution() = default;
    ApiResolution(ApiResolutionKind resolution_kind, std::string resolution_owner,
                  std::string resolution_getter, std::string resolution_setter,
                  Type resolution_type, std::uint16_t resolution_required_arguments = 0,
                  std::uint16_t resolution_maximum_arguments = 0, bool resolution_is_vararg = false,
                  bool resolution_direct = false, std::int64_t resolution_indexed_argument = -1,
                  IntrinsicKind resolution_intrinsic = IntrinsicKind::none,
                  bool resolution_read_only = false, Type resolution_assignment_type = {})
        : kind(resolution_kind), owner(std::move(resolution_owner)),
          getter(std::move(resolution_getter)), setter(std::move(resolution_setter)),
          type(std::move(resolution_type)), required_arguments(resolution_required_arguments),
          maximum_arguments(resolution_maximum_arguments), is_vararg(resolution_is_vararg),
          direct(resolution_direct), indexed_argument(resolution_indexed_argument),
          intrinsic(resolution_intrinsic), read_only(resolution_read_only),
          assignment_type(std::move(resolution_assignment_type)) {}

    ApiResolutionKind kind{ApiResolutionKind::none};
    std::string owner;
    std::string getter;
    std::string setter;
    Type type;
    std::uint16_t required_arguments{0};
    std::uint16_t maximum_arguments{0};
    bool is_vararg{false};
    bool direct{false};
    std::int64_t indexed_argument{-1};
    IntrinsicKind intrinsic{IntrinsicKind::none};
    bool read_only{false};
    // Godot property metadata can describe inspector alternatives while the native setter
    // accepts a common base class. Keep the write contract separate from the getter result.
    Type assignment_type;
};

struct ResolvedCallContract {
    std::vector<Type> parameters;
    std::size_t required_arguments{0};
    bool is_vararg{false};
};

struct DebugVariable {
    std::string name;
    Type type;
    SourceSpan declaration{};
    FlowSymbolId identity{0};
};

class SemanticModel final {
  public:
    [[nodiscard]] Type type_of(const ast::Expression& expression) const;
    [[nodiscard]] Type storage_type_of(const ast::Expression& expression) const;
    [[nodiscard]] bool is_non_null(const ast::Expression& expression) const noexcept;
    [[nodiscard]] Type type_of(const ast::VariableDeclaration& declaration) const;
    [[nodiscard]] Type property_type_of(const ast::VariableDeclaration& declaration) const;
    [[nodiscard]] Type type_of(const ast::Statement& statement) const;
    [[nodiscard]] IterationPlan iteration_plan_of(const ast::Statement& statement) const;
    [[nodiscard]] const std::vector<DebugVariable>&
    debug_variables_at(const ast::Statement& statement) const noexcept;
    [[nodiscard]] const std::vector<DebugVariable>&
    suspension_variables_at(const ast::Expression& expression) const noexcept;
    [[nodiscard]] Type type_of(const ast::MatchPattern& pattern) const;
    [[nodiscard]] Type type_of(const ast::Parameter& parameter) const;
    [[nodiscard]] DefaultArgumentEvaluation
    default_argument_evaluation_of(const ast::Parameter& parameter) const noexcept;
    [[nodiscard]] Type return_type_of(const ast::FunctionDeclaration& function) const;
    [[nodiscard]] Type return_type_of(const ast::LambdaExpression& function) const;
    [[nodiscard]] bool is_coroutine(const ast::FunctionDeclaration& function) const noexcept;
    [[nodiscard]] bool is_coroutine(const ast::LambdaExpression& function) const noexcept;
    [[nodiscard]] bool is_coroutine(const ast::PropertyAccessor& accessor) const noexcept;
    [[nodiscard]] bool is_coroutine_call(const ast::Expression& expression) const noexcept;
    [[nodiscard]] bool owner_bound(const ast::LambdaExpression& function) const noexcept;
    [[nodiscard]] const RpcConfiguration*
    rpc_configuration_of(const ast::FunctionDeclaration& function) const noexcept;
    [[nodiscard]] std::int64_t value_of(const ast::EnumEntry& entry) const;
    [[nodiscard]] std::optional<std::int64_t>
    constant_integer_value_of(const ast::Expression& expression) const noexcept;
    [[nodiscard]] const Symbol* symbol_of(const ast::Expression& expression) const noexcept;
    [[nodiscard]] const Symbol* symbol_of(const ast::Statement& statement) const noexcept;
    [[nodiscard]] const Symbol* symbol_of(const ast::MatchPattern& pattern) const noexcept;
    [[nodiscard]] const Symbol* symbol_of(const ast::Parameter& parameter) const noexcept;
    [[nodiscard]] const ApiResolution*
    api_resolution_of(const ast::Expression& expression) const noexcept;
    [[nodiscard]] const ResolvedCallContract*
    call_contract_of(const ast::Expression& expression) const noexcept;
    [[nodiscard]] const std::unordered_set<std::string>& referenced_script_paths() const noexcept {
        return referenced_script_paths_;
    }
    [[nodiscard]] const std::unordered_set<std::string>&
    referenced_extension_abis() const noexcept {
        return referenced_extension_abis_;
    }

  private:
    friend class SemanticAnalyzer;
    std::unordered_map<const ast::Expression*, Type> expression_types_;
    // A dynamic call can retain a narrow static type while carrying a wider runtime container.
    // Keeping both types prevents native code generation from validating the value before
    // GDScript reaches a real typed-storage boundary.
    std::unordered_map<const ast::Expression*, Type> expression_storage_types_;
    std::unordered_set<const ast::Expression*> non_null_expressions_;
    std::unordered_map<const ast::VariableDeclaration*, Type> variable_types_;
    std::unordered_map<const ast::VariableDeclaration*, Type> property_types_;
    std::unordered_map<const ast::Statement*, Type> local_types_;
    std::unordered_map<const ast::Statement*, IterationPlan> iteration_plans_;
    std::unordered_map<const ast::Statement*, std::vector<DebugVariable>> debug_variables_;
    std::unordered_map<const ast::Expression*, std::vector<DebugVariable>> suspension_variables_;
    std::unordered_map<const ast::MatchPattern*, Type> match_pattern_types_;
    std::unordered_map<const ast::Parameter*, Type> parameter_types_;
    std::unordered_map<const ast::Parameter*, DefaultArgumentEvaluation>
        default_argument_evaluations_;
    std::unordered_map<const ast::FunctionDeclaration*, Type> function_return_types_;
    std::unordered_map<const ast::LambdaExpression*, Type> lambda_return_types_;
    std::unordered_set<const ast::LambdaExpression*> owner_bound_lambdas_;
    std::unordered_set<const ast::FunctionDeclaration*> coroutine_functions_;
    std::unordered_set<const ast::LambdaExpression*> coroutine_lambdas_;
    std::unordered_set<const ast::PropertyAccessor*> coroutine_accessors_;
    std::unordered_set<const ast::Expression*> coroutine_calls_;
    std::unordered_map<const ast::FunctionDeclaration*, RpcConfiguration> rpc_configurations_;
    std::unordered_map<const ast::EnumEntry*, std::int64_t> enum_values_;
    std::unordered_map<const ast::Expression*, std::int64_t> constant_integer_values_;
    std::unordered_map<const ast::Expression*, Symbol> referenced_symbols_;
    std::unordered_map<const ast::Statement*, Symbol> local_symbols_;
    std::unordered_map<const ast::MatchPattern*, Symbol> match_pattern_symbols_;
    std::unordered_map<const ast::Parameter*, Symbol> parameter_symbols_;
    std::unordered_map<const ast::Expression*, ApiResolution> api_resolutions_;
    std::unordered_map<const ast::Expression*, ResolvedCallContract> call_contracts_;
    std::unordered_set<std::string> referenced_script_paths_;
    std::unordered_set<std::string> referenced_extension_abis_;
};

class SemanticAnalyzer final {
  public:
    explicit SemanticAnalyzer(DiagnosticBag& diagnostics,
                              const GodotApi& api = GodotApi::instance(),
                              std::string semantic_base_type = {},
                              const ScriptSymbolTable* script_symbols = nullptr,
                              std::string current_script_path = {})
        : diagnostics_(diagnostics), api_(api), semantic_base_type_(std::move(semantic_base_type)),
          script_symbols_(script_symbols), current_script_path_(std::move(current_script_path)) {}
    [[nodiscard]] SemanticModel analyze(const ast::Script& script);

  private:
    using Scope = std::unordered_map<std::string, Symbol>;

    struct FlowResult {
        bool falls_through{false};
        bool returns{false};
        bool breaks{false};
        bool continues{false};
    };

    void declare(Symbol symbol);
    [[nodiscard]] const Symbol* resolve(const std::string& name) const noexcept;
    void analyze_function(const ast::FunctionDeclaration& function);
    void analyze_rest_parameter(const ast::Parameter& parameter);
    void analyze_rpc_annotations(const ast::FunctionDeclaration& function);
    void analyze_class(const ast::ClassDeclaration& declaration);
    void validate_script_abstract_contract(const ast::Script& script);
    void validate_inner_abstract_contract(const ast::ClassDeclaration& declaration);
    void analyze_lambda(const ast::LambdaExpression& expression);
    void analyze_enums(const std::vector<ast::EnumDeclaration>& declarations);
    void analyze_property_accessors(const ast::VariableDeclaration& variable, const Type& type);
    void validate_annotations(const ast::VariableDeclaration& variable, const Type& type);
    [[nodiscard]] FlowResult analyze_statements(const std::vector<ast::Statement>& statements);
    [[nodiscard]] FlowResult analyze_statement(const ast::Statement& statement);
    [[nodiscard]] std::vector<DebugVariable> visible_debug_variables() const;
    [[nodiscard]] Type analyze_expression(const ast::Expression& expression);
    [[nodiscard]] Type analyze_binary_tree(const ast::Expression& expression);
    [[nodiscard]] ConditionalRefinements
    conditional_refinements(const ast::Expression& expression) const;
    [[nodiscard]] std::optional<Type> narrowed_flow_type(const Type& current,
                                                         const Type& target) const;
    [[nodiscard]] Type resolve_binary_expression(const ast::Expression& expression,
                                                 const Type& left, const Type& right);
    [[nodiscard]] bool is_constant_expression(const ast::Expression& expression) const;
    [[nodiscard]] Type constant_value_type_of(const ast::Expression& expression,
                                              const Type& fallback) const;
    [[nodiscard]] Type runtime_storage_type_of(const ast::Expression& expression) const;
    [[nodiscard]] std::optional<std::string>
    constant_string_expression(const ast::Expression& expression) const;
    [[nodiscard]] std::optional<std::int64_t>
    constant_integer_expression(const ast::Expression& expression) const;
    [[nodiscard]] bool is_match_value_pattern(const ast::Expression& expression) const;
    void analyze_match_pattern(const ast::MatchPattern& pattern, const Type& matched_type);
    [[nodiscard]] bool is_assignment_target(const ast::Expression& expression) const noexcept;
    [[nodiscard]] Type type_from_name(const std::string& name, SourceSpan span = {});
    [[nodiscard]] bool object_type_inherits(const Type& derived, const Type& base) const noexcept;
    [[nodiscard]] bool override_type_accepts(const Type& target, const Type& source) const noexcept;
    [[nodiscard]] Type container_element_type(const Type& container, SourceSpan span = {});
    [[nodiscard]] Type iteration_element_type(const Type& container, SourceSpan span = {});
    [[nodiscard]] std::optional<Type> object_iteration_element_type(const Type& object,
                                                                    SourceSpan span);
    [[nodiscard]] Type declared_or_inferred(const std::optional<std::string>& annotation,
                                            const ast::ExpressionPtr& initializer);
    void require_assignable(const Type& target, const Type& source, SourceSpan span,
                            const std::string& context);
    void require_truthy_value(const Type& type, SourceSpan span, const std::string& context);
    void require_inferable_type(const Type& type, SourceSpan span, const std::string& context);
    void require_expression_assignable(const Type& target, const ast::Expression& expression,
                                       const Type& source, SourceSpan span,
                                       const std::string& context);
    void validate_script_call(const ScriptMemberSymbol& member, const std::vector<Type>& arguments,
                              const ast::Expression& call, SourceSpan span);
    void validate_local_call(const ast::FunctionDeclaration& function,
                             const std::vector<Type>& arguments, const ast::Expression& call,
                             SourceSpan span);
    void validate_container_method_call(const Type& container, std::string_view method,
                                        const std::vector<Type>& arguments,
                                        const ast::Expression& call);
    [[nodiscard]] const ScriptInnerClassSymbol*
    find_inner_class(const std::string& name) const noexcept;
    [[nodiscard]] const ScriptClassSymbol*
    find_script_class(const std::string& name) const noexcept;
    [[nodiscard]] const ScriptInnerClassSymbol*
    inner_base_of(const ScriptInnerClassSymbol& owner) const noexcept;
    [[nodiscard]] const ScriptClassSymbol*
    inner_script_base_of(const ScriptInnerClassSymbol& owner) const noexcept;
    [[nodiscard]] const ScriptInnerClassSymbol*
    find_nested_inner_class(const ScriptInnerClassSymbol& owner,
                            const std::string& name) const noexcept;
    struct InnerEnumLookup {
        const ScriptInnerClassSymbol* owner{nullptr};
        const ScriptEnumSymbol* enumeration{nullptr};
    };
    [[nodiscard]] InnerEnumLookup find_inner_enum(const std::string& name) const noexcept;
    [[nodiscard]] const ScriptMemberSymbol*
    find_inner_member(const ScriptInnerClassSymbol& owner, const std::string& name) const noexcept;
    [[nodiscard]] bool script_function_is_static(const std::string& name) const noexcept;
    [[nodiscard]] bool accessor_is_coroutine(const ast::PropertyAccessor& accessor) const noexcept;
    void diagnose_static_instance_access(std::string_view kind, std::string_view name,
                                         SourceSpan span);
    void record_script_dependency(const ScriptClassSymbol* dependency);

    DiagnosticBag& diagnostics_;
    const GodotApi& api_;
    std::string semantic_base_type_;
    const ScriptSymbolTable* script_symbols_{nullptr};
    std::string current_script_path_;
    const ScriptClassSymbol* current_script_{nullptr};
    SemanticModel model_;
    std::vector<Scope> scopes_;
    std::unordered_set<std::string> enum_types_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::int64_t>> enum_members_;
    std::unordered_set<std::string> accessor_fields_;
    std::unordered_set<std::string> coroutine_getter_fields_;
    std::unordered_set<std::string> static_fields_;
    std::unordered_set<std::string> current_accessor_fields_;
    std::unordered_set<std::string> active_warning_ignores_;
    std::unordered_map<std::string, std::unordered_set<std::string>> bound_accessor_fields_;
    std::unordered_map<std::string, const ast::FunctionDeclaration*> functions_;
    std::unordered_map<std::string, ScriptInnerClassSymbol> local_inner_classes_;
    std::unordered_map<std::string, const ScriptClassSymbol*> script_resource_aliases_;
    const ScriptInnerClassSymbol* current_inner_class_{nullptr};
    const ScriptInnerClassSymbol* current_inner_base_{nullptr};
    bool allow_dynamic_await_return_{false};
    bool await_expression_allowed_{true};
    bool current_function_static_{false};
    bool instance_context_available_{true};
    bool current_callable_suspends_{false};
    bool in_function_{false};
    bool script_tool_{false};
    Type expected_return_{TypeKind::void_type, "void"};
    std::string current_function_name_;
    std::string base_type_{"Node"};
    std::size_t loop_depth_{0};
    std::size_t await_operand_depth_{0};
    const ast::Expression* discarded_expression_{nullptr};
    FlowSymbolId next_symbol_identity_{1};
    FlowTypeState flow_types_;
    bool suppress_flow_refinements_{false};
};

} // namespace gdpp
