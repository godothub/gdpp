#pragma once

#include "gdpp/core/diagnostic.hpp"
#include "gdpp/ir/mir.hpp"
#include "gdpp/semantic/godot_api.hpp"
#include "gdpp/semantic/script_symbols.hpp"

#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gdpp {

struct GeneratedUnit {
    std::string script_class_name;
    std::string class_name;
    std::string header_file_name;
    std::string source_file_name;
    std::string header;
    std::string source;
    std::vector<std::string> inner_class_names;
    std::vector<std::string> abstract_inner_class_names;
    std::optional<std::string> icon_path;
    bool is_abstract{false};
    bool is_tool{false};
    bool static_unload{false};
    bool is_attached{false};
    std::string attached_native_base;
};

class CodeGenerator final {
  public:
    explicit CodeGenerator(DiagnosticBag& diagnostics, const GodotApi& api = GodotApi::instance(),
                           const ScriptSymbolTable* script_symbols = nullptr)
        : diagnostics_(diagnostics), api_(api), script_symbols_(script_symbols) {}
    [[nodiscard]] GeneratedUnit generate(const mir::Module& module, const std::string& source_path,
                                         const std::string& native_class_suffix = {},
                                         const std::string& native_base_class = {},
                                         const std::string& native_base_header = {},
                                         bool attached_script = false,
                                         const std::string& attached_native_base = {},
                                         const std::string& attached_base_script_path = {},
                                         const std::string& script_contract_hash = {}) const;

  private:
    struct StatementSlice {
        const std::vector<ir::Statement>* statements{nullptr};
        std::size_t begin{0};
    };

    struct AsyncLoopControl {
        std::vector<StatementSlice> break_tails;
        std::string break_terminal;
        std::string continue_terminal;
        std::shared_ptr<const AsyncLoopControl> parent;
    };

    struct MatchBinding {
        std::string name;
        std::string slot;
        Type type;
    };

    [[nodiscard]] std::string emit_expression(const ir::Expression& expression) const;
    [[nodiscard]] std::string emit_integer_binary(const ir::Expression& expression) const;
    [[nodiscard]] std::string emit_integer_operation(std::string_view operation,
                                                     std::string left_value,
                                                     std::string right_value,
                                                     const Type& result_type) const;
    [[nodiscard]] std::string emit_truthy(const ir::Expression& expression) const;
    [[nodiscard]] std::string emit_conversion(const Type& target, const Type& source,
                                              std::string value) const;
    [[nodiscard]] std::string emit_explicit_conversion(const Type& target, const Type& source,
                                                       std::string value) const;
    [[nodiscard]] std::string emit_parameter_default(const ir::Parameter& parameter) const;
    [[nodiscard]] std::string parameter_native_type(const ir::Parameter& parameter) const;
    [[nodiscard]] std::string parameter_native_name(const ir::Parameter& parameter) const;
    [[nodiscard]] std::string
    emit_parameter_default_initializers(const std::vector<ir::Parameter>& parameters,
                                        std::size_t indent) const;
    [[nodiscard]] std::string
    emit_bound_parameter_defaults(const std::vector<ir::Parameter>& parameters) const;
    [[nodiscard]] static std::string method_callback_name(const ir::Function& function);
    [[nodiscard]] std::string emit_method_callback_declaration(const ir::Function& function) const;
    [[nodiscard]] std::string
    emit_method_callback_definition(const ir::Function& function, std::string_view native_class,
                                    std::string_view native_method,
                                    std::string_view native_return_type) const;
    [[nodiscard]] std::string emit_method_registration(const ir::Function& function,
                                                       std::string_view native_class,
                                                       std::string_view native_return_type) const;
    [[nodiscard]] std::string emit_api_argument(std::string_view api_type,
                                                std::string_view native_meta, const Type& source,
                                                std::string value) const;
    [[nodiscard]] std::string emit_api_return(const Type& target, std::string value) const;
    [[nodiscard]] std::string emit_subscript_read(const Type& container, const Type& result,
                                                  std::string target, std::string index,
                                                  SourceSpan span) const;
    [[nodiscard]] std::string emit_subscript_store(const Type& container, std::string value) const;
    [[nodiscard]] std::string emit_storage_assignment(const Type& target_type, std::string target,
                                                      std::string value) const;
    [[nodiscard]] std::string emit_direct_builtin_member(std::string_view owner, std::string object,
                                                         std::string_view member) const;
    [[nodiscard]] std::string emit_direct_builtin_assignment(std::string_view owner,
                                                             std::string object,
                                                             std::string_view member,
                                                             std::string value) const;
    [[nodiscard]] std::string emit_dynamic_assignment(const ir::Statement& statement,
                                                      std::size_t indent) const;
    [[nodiscard]] std::string emit_dictionary_member_assignment(const ir::Statement& statement,
                                                                std::size_t indent) const;
    void collect_match_bindings(const ir::MatchPattern& pattern,
                                std::vector<MatchBinding>& bindings) const;
    [[nodiscard]] std::string emit_match_pattern(const ir::MatchPattern& pattern,
                                                 const std::string& candidate,
                                                 const std::vector<MatchBinding>& bindings) const;
    [[nodiscard]] std::string emit_statement(const ir::Statement& statement,
                                             std::size_t indent) const;
    [[nodiscard]] std::string emit_statement_body(const ir::Statement& statement,
                                                  std::size_t indent) const;
    [[nodiscard]] std::string emit_debug_frame(std::size_t line, std::size_t indent) const;
    [[nodiscard]] std::string emit_debug_line(std::size_t line, std::size_t indent) const;
    [[nodiscard]] std::string emit_debug_breakpoint(const ir::Statement& statement,
                                                    std::size_t indent) const;
    [[nodiscard]] std::string
    emit_statements(const std::vector<ir::Statement>& statements, std::size_t indent,
                    std::size_t begin = 0,
                    const std::vector<std::pair<std::string, Type>>& entry_locals = {}) const;
    [[nodiscard]] static std::vector<std::pair<std::string, Type>>
    parameter_locals(const std::vector<ir::Parameter>& parameters,
                     const ir::Parameter* rest_parameter = nullptr);
    [[nodiscard]] std::string
    emit_async_statements(const std::vector<ir::Statement>& statements, std::size_t indent,
                          std::size_t begin, std::vector<StatementSlice> tails,
                          const std::string& terminal, bool continuation_context,
                          std::shared_ptr<const AsyncLoopControl> loop_control = {}) const;
    [[nodiscard]] std::string
    emit_async_match_branch(const ir::Statement& branch, std::size_t next_branch,
                            std::size_t after_branch, const std::string& value_name,
                            const std::string& keep_alive, std::size_t indent,
                            std::shared_ptr<const AsyncLoopControl> loop_control) const;
    [[nodiscard]] std::string emit_assert_failure(const ir::Statement& statement,
                                                  std::size_t indent,
                                                  bool continuation_context) const;
    [[nodiscard]] bool statement_contains_await(const ir::Statement& statement) const noexcept;
    [[nodiscard]] static bool await_can_suspend(const ir::Statement& statement) noexcept;
    [[nodiscard]] std::string async_return(std::size_t indent, bool continuation_context) const;
    [[nodiscard]] std::string coroutine_return(std::size_t indent, std::string value,
                                               bool continuation_context) const;
    [[nodiscard]] bool can_emit_flat_async(const ir::Function& function,
                                           const mir::Function& mir_function) const noexcept;
    [[nodiscard]] std::string emit_flat_async(const mir::Function& function,
                                              std::size_t indent) const;
    [[nodiscard]] std::string lift_async_loop_locals(const ir::Statement& statement,
                                                     std::size_t indent) const;
    [[nodiscard]] std::string cpp_type(const Type& type) const;
    [[nodiscard]] std::string self_object_expression() const;
    [[nodiscard]] std::string await_owner_expression() const;
    [[nodiscard]] std::string godot_owner_expression() const;
    [[nodiscard]] std::string api_native_type(std::string_view api_type,
                                              std::string_view native_meta) const;
    [[nodiscard]] std::string virtual_parameter_type(const GodotMethodRecord& method,
                                                     std::size_t index) const;
    [[nodiscard]] std::string virtual_return_type(const GodotMethodRecord& method) const;
    [[nodiscard]] std::string native_property_info(const Type& type, std::string_view name) const;
    [[nodiscard]] Type container_argument_type(std::string_view type_name) const;
    [[nodiscard]] std::string container_cpp_argument(std::string_view type_name) const;
    [[nodiscard]] std::string container_object_tag_identity(std::string_view type_name) const;
    [[nodiscard]] std::string container_object_runtime_name(std::string_view type_name) const;
    [[nodiscard]] std::string inner_cpp_type(std::string_view name) const;
    [[nodiscard]] std::string inner_godot_base_type(std::string_view name) const;
    [[nodiscard]] std::string inner_attached_native_base_type(std::string_view name) const;
    [[nodiscard]] std::string
    attached_script_source_path(const Type& type, std::string_view resolved_owner = {}) const;
    [[nodiscard]] std::string emit_attached_script_cast(const Type& target,
                                                        std::string value) const;
    [[nodiscard]] bool is_ref_counted_object(const Type& type) const noexcept;
    [[nodiscard]] std::string native_super_owner(std::string_view owner) const;
    struct InnerMethodDeclaration {
        const ScriptMemberSymbol* method{nullptr};
        const ScriptInnerClassSymbol* inner_owner{nullptr};
        const ScriptClassSymbol* script_owner{nullptr};
    };
    [[nodiscard]] bool same_native_method_abi(const ScriptMemberSymbol& derived,
                                              std::string_view derived_godot_base,
                                              const ScriptMemberSymbol& base,
                                              std::string_view base_godot_base) const;
    [[nodiscard]] bool same_native_function_abi(const ir::Function& derived,
                                                std::string_view derived_godot_base,
                                                const ir::Function& base,
                                                std::string_view base_godot_base) const;
    [[nodiscard]] const ir::Function*
    find_inherited_inner_function(std::string_view base, std::string_view method,
                                  std::string* declaration_owner = nullptr) const noexcept;
    [[nodiscard]] std::string script_method_native_name(const ScriptClassSymbol& owner,
                                                        const ScriptMemberSymbol& method) const;
    [[nodiscard]] std::string
    script_method_implementation_name(const ScriptClassSymbol& owner,
                                      const ScriptMemberSymbol& method) const;
    [[nodiscard]] const ScriptInnerClassSymbol*
    inner_base_of(const ScriptInnerClassSymbol& owner) const noexcept;
    [[nodiscard]] InnerMethodDeclaration
    find_inner_method_declaration(const ScriptInnerClassSymbol& owner, std::string_view method,
                                  bool include_owner) const noexcept;
    [[nodiscard]] std::string inner_method_native_name(const ScriptInnerClassSymbol& owner,
                                                       const ScriptMemberSymbol& method) const;
    [[nodiscard]] std::string
    inner_method_implementation_name(const ScriptInnerClassSymbol& owner,
                                     const ScriptMemberSymbol& method) const;
    [[nodiscard]] bool inner_overrides_method(const ScriptInnerClassSymbol& owner,
                                              const ScriptMemberSymbol& method) const;
    [[nodiscard]] bool managed_constant_field(const ir::Field& field) const;
    [[nodiscard]] bool managed_constant_reference(const ir::Expression& expression) const;
    void emit_inner_class_declaration(const ir::Class& declaration, std::ostringstream& header,
                                      const std::string& native_name,
                                      const std::string& source_name, bool tool_mode) const;
    void emit_inner_class_definition(const ir::Class& declaration, std::ostringstream& source,
                                     const std::string& native_name, const std::string& source_name,
                                     bool tool_mode) const;
    void emit_attached_descriptor_definition(
        std::ostringstream& source, const std::string& native_name, const std::string& source_path,
        const std::string& global_name, const std::string& native_base_type,
        const std::string& base_script_path, const std::string& contract_hash, bool tool_mode,
        bool is_abstract, const std::vector<ir::Field>& fields,
        const std::vector<ir::Function>& functions, const std::vector<ir::Signal>& signals,
        const std::vector<ir::Enum>& enums) const;
    [[nodiscard]] static std::string sanitize_identifier(const std::string& value);
    [[nodiscard]] static std::string sanitize_qualified_identifier(std::string_view value);
    [[nodiscard]] static std::string enum_identifier(const std::string& value);

    DiagnosticBag& diagnostics_;
    const GodotApi& api_;
    const ScriptSymbolTable* script_symbols_{nullptr};
    mutable const ScriptClassSymbol* current_script_{nullptr};
    mutable const ScriptInnerClassSymbol* current_inner_script_{nullptr};
    mutable std::string detail_namespace_;
    mutable std::string current_source_path_;
    mutable Type current_return_type_;
    mutable bool current_coroutine_abi_{false};
    mutable std::string current_coroutine_state_;
    mutable bool in_function_body_{false};
    mutable bool in_callable_lambda_{false};
    mutable bool in_async_continuation_{false};
    mutable bool attached_script_{false};
    mutable std::string attached_godot_base_type_;
    mutable std::string current_script_contract_hash_;
    mutable std::unordered_map<std::string, std::string> inner_native_names_;
    mutable std::unordered_map<std::string, std::string> inner_godot_base_types_;
    mutable std::unordered_map<std::string, std::string> inner_attached_native_base_types_;
    mutable std::unordered_map<std::string, std::string> inner_base_names_;
    mutable std::unordered_map<std::string, const ir::Class*> inner_declarations_;
    mutable std::unordered_set<std::string> inner_ref_types_;
    mutable std::unordered_set<std::string> container_enum_types_;
    mutable std::unordered_map<std::string, std::vector<Type>> local_function_parameters_;
    mutable std::unordered_map<std::string, const ir::Function*> local_functions_;
    mutable std::unordered_map<std::string, const ir::Function*> constructor_functions_;
    mutable std::unordered_map<std::string, Type> current_local_types_;
    mutable std::unordered_set<std::string> ambiguous_local_names_;
    mutable std::unordered_map<std::string, std::string> local_expression_overrides_;
    mutable std::string current_debug_source_;
    mutable std::string current_debug_function_;
    mutable std::string current_debug_instance_;
    mutable std::vector<std::pair<std::string, std::string>> current_debug_members_;
    mutable std::string current_native_class_name_;
    mutable std::string current_godot_base_type_;
    mutable std::size_t match_counter_{0};
    mutable std::size_t temporary_counter_{0};
};

} // namespace gdpp
