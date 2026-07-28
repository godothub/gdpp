#include "gdpp/runtime/variant_ops.hpp"

#include "gdpp/numeric/integer_semantics.hpp"
#include "gdpp/runtime/attached_script.hpp"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_custom.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/signal.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gdpp::runtime {

class CoroutineState final {
  public:
    godot::ObjectID owner;
    godot::ObjectID function_state;
    godot::Ref<CoroutineFunctionState> initial_function_state;
    godot::Variant result;
    std::mutex mutex;
    ScriptFaultState script_fault;
    bool completed{false};
    bool exposed{false};
};

namespace detail {
thread_local ScriptFaultState* active_script_fault = nullptr;
} // namespace detail

namespace {

godot::ObjectID variant_object_id(const godot::Variant& value) {
    // Calling the conversion operator explicitly is intentional. MSVC otherwise considers the
    // ObjectID signed and unsigned integer constructors alongside Variant's integer conversions
    // and rejects an otherwise direct static_cast as ambiguous.
    return value.operator godot::ObjectID();
}

const godot::StringName& coroutine_completed_signal() {
    static const godot::StringName name{"completed"};
    return name;
}

struct ActiveInitialization final {
    const ScriptInitializationState* state{nullptr};
    ActiveInitialization* previous{nullptr};
};

thread_local ActiveInitialization* active_initialization = nullptr;

bool initialization_is_active(const ScriptInitializationState* state) noexcept {
    for (auto* active = active_initialization; active; active = active->previous) {
        if (active->state == state)
            return true;
    }
    return false;
}

class ActiveInitializationScope final {
  public:
    explicit ActiveInitializationScope(const ScriptInitializationState* state) noexcept
        : node_{state, active_initialization} {
        active_initialization = &node_;
    }

    ~ActiveInitializationScope() { active_initialization = node_.previous; }

    ActiveInitializationScope(const ActiveInitializationScope&) = delete;
    ActiveInitializationScope& operator=(const ActiveInitializationScope&) = delete;

  private:
    ActiveInitialization node_;
};

ScriptFaultState* coroutine_script_fault(const CoroutineStatePtr& coroutine) noexcept {
    return coroutine ? &coroutine->script_fault : nullptr;
}

} // namespace

void CoroutineFunctionState::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("resume", "arg"), &CoroutineFunctionState::resume,
                                DEFVAL(godot::Variant{}));
    godot::ClassDB::bind_method(godot::D_METHOD("is_valid", "extended_check"),
                                &CoroutineFunctionState::is_valid, DEFVAL(false));

    godot::MethodInfo callback{"_signal_callback"};
    godot::ClassDB::bind_vararg_method(godot::METHOD_FLAGS_DEFAULT, callback.name,
                                       &CoroutineFunctionState::signal_callback, callback);

    godot::MethodInfo completed{coroutine_completed_signal()};
    completed.arguments.push_back(godot::PropertyInfo{godot::Variant::NIL,
                                                      "result",
                                                      godot::PROPERTY_HINT_NONE,
                                                      {},
                                                      godot::PROPERTY_USAGE_NIL_IS_VARIANT});
    godot::ClassDB::add_signal(get_class_static(), completed);
}

bool CoroutineFunctionState::is_valid(const bool extended_check) const {
    std::lock_guard lock{mutex_};
    if (!continuation_)
        return false;
    return !extended_check || owner_.is_null() ||
           godot::ObjectDB::get_instance(static_cast<std::uint64_t>(owner_));
}

void CoroutineFunctionState::clear_incoming_connections() {
    const godot::TypedArray<godot::Dictionary> connections = get_incoming_connections();
    for (std::int64_t index = 0; index < connections.size(); ++index) {
        const godot::Dictionary connection = connections[index];
        godot::Signal signal = connection.get("signal", godot::Signal{});
        const godot::Callable callable = connection.get("callable", godot::Callable{});
        if (!signal.is_null() && signal.is_connected(callable))
            signal.disconnect(callable);
    }
}

godot::Variant CoroutineFunctionState::resume(const godot::Variant& argument) {
    AwaitContinuation continuation;
    CoroutineStatePtr coroutine;
    {
        std::lock_guard lock{mutex_};
        if (!continuation_) {
            godot::UtilityFunctions::push_error(
                "GDPP: cannot resume a completed or invalid coroutine function state");
            return {};
        }
        if (!owner_.is_null() &&
            !godot::ObjectDB::get_instance(static_cast<std::uint64_t>(owner_))) {
            continuation_ = {};
            godot::UtilityFunctions::push_error(
                "GDPP: cannot resume a coroutine after its instance was freed");
            return {};
        }
        coroutine = coroutine_.lock();
        continuation = std::move(continuation_);
    }
    clear_incoming_connections();
    godot::Array values;
    values.push_back(argument);
    continuation(values);
    return coroutine ? coroutine_result(coroutine) : godot::Variant{};
}

godot::Variant CoroutineFunctionState::signal_callback(const godot::Variant** arguments,
                                                       const GDExtensionInt argument_count,
                                                       GDExtensionCallError& error) {
    error.error = GDEXTENSION_CALL_OK;
    if (argument_count < 1) {
        error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
        error.expected = 1;
        return {};
    }

    godot::Variant result;
    const auto signal_argument_count = argument_count - 1;
    if (signal_argument_count == 1) {
        result = *arguments[0];
    } else if (signal_argument_count > 1) {
        godot::Array values;
        values.resize(signal_argument_count);
        for (GDExtensionInt index = 0; index < signal_argument_count; ++index)
            values[index] = *arguments[index];
        result = values;
    }
    return resume(result);
}

void CoroutineFunctionState::install(godot::Object* owner, const CoroutineStatePtr& coroutine,
                                     AwaitContinuation continuation) {
    std::lock_guard lock{mutex_};
    owner_ = owner ? owner->get_instance_id() : godot::ObjectID{};
    coroutine_ = coroutine;
    continuation_ = std::move(continuation);
}

void CoroutineFunctionState::finish() {
    {
        std::lock_guard lock{mutex_};
        continuation_ = {};
    }
    clear_incoming_connections();
}

godot::String CoroutineFunctionState::_to_string() const {
    return godot::String{"<GDScriptFunctionState#"} +
           godot::String::num_int64(static_cast<std::int64_t>(get_instance_id())) + ">";
}

ScriptFunctionScope::ScriptFunctionScope(const CoroutineStatePtr& coroutine) noexcept
    : state_{coroutine_script_fault(coroutine)}, previous_{detail::active_script_fault} {
    if (!state_)
        state_ = &local_;
    detail::active_script_fault = state_;
}

bool ScriptInitializationState::run(const char* failure_message,
                                    const std::function<void()>& initialize,
                                    const std::function<void()>& rollback) {
    ScriptFunctionScope fault_scope{ScriptFaultPolicy::inherit_existing};
    if (script_function_failed())
        return false;

    auto phase = phase_.load(std::memory_order_acquire);
    if (phase == Phase::ready)
        return true;
    if (phase == Phase::failed) {
        report_script_failure(failure_message ? failure_message
                                              : "Script initialization previously failed.");
        return false;
    }
    if (initialization_is_active(this))
        return true;

    std::lock_guard<std::mutex> lock{mutex_};
    phase = phase_.load(std::memory_order_relaxed);
    if (phase == Phase::ready)
        return true;
    if (phase == Phase::failed) {
        report_script_failure(failure_message ? failure_message
                                              : "Script initialization previously failed.");
        return false;
    }

    ActiveInitializationScope initialization_scope{this};
    if (initialize)
        initialize();
    if (script_function_failed()) {
        if (rollback)
            rollback();
        phase_.store(Phase::failed, std::memory_order_release);
        return false;
    }
    phase_.store(Phase::ready, std::memory_order_release);
    return true;
}

void ScriptInitializationState::reset(const std::function<void()>& cleanup) {
    if (initialization_is_active(this))
        return;
    std::lock_guard<std::mutex> lock{mutex_};
    ActiveInitializationScope initialization_scope{this};
    if (cleanup)
        cleanup();
    phase_.store(Phase::uninitialized, std::memory_order_release);
}

bool ScriptInitializationState::ready() const noexcept {
    return phase_.load(std::memory_order_acquire) == Phase::ready;
}

bool ScriptInitializationState::failed() const noexcept {
    return phase_.load(std::memory_order_acquire) == Phase::failed;
}

godot::String describe_variant_type(const godot::Variant& value) {
    if (value.get_type() == godot::Variant::ARRAY) {
        const auto array = static_cast<godot::Array>(value);
        if (!array.is_typed())
            return "Array";
        const auto builtin = static_cast<godot::Variant::Type>(array.get_typed_builtin());
        const auto class_name = godot::String{array.get_typed_class_name()};
        const auto element =
            !class_name.is_empty() ? class_name : godot::Variant::get_type_name(builtin);
        return godot::String{"Array["} + element + "]";
    }
    if (value.get_type() == godot::Variant::DICTIONARY) {
        const auto dictionary = static_cast<godot::Dictionary>(value);
        if (!dictionary.is_typed())
            return "Dictionary";
        const auto key_builtin =
            static_cast<godot::Variant::Type>(dictionary.get_typed_key_builtin());
        const auto value_builtin =
            static_cast<godot::Variant::Type>(dictionary.get_typed_value_builtin());
        const auto key_class = godot::String{dictionary.get_typed_key_class_name()};
        const auto value_class = godot::String{dictionary.get_typed_value_class_name()};
        const auto key =
            !key_class.is_empty() ? key_class : godot::Variant::get_type_name(key_builtin);
        const auto mapped =
            !value_class.is_empty() ? value_class : godot::Variant::get_type_name(value_builtin);
        return godot::String{"Dictionary["} + key + ", " + mapped + "]";
    }
    if (value.get_type() == godot::Variant::OBJECT) {
        if (auto* object = value.get_validated_object())
            return object->get_class();
        const auto id = variant_object_id(value);
        return id.is_valid() ? godot::String{"freed Object"} : godot::String{"null Object"};
    }
    return godot::Variant::get_type_name(value.get_type());
}

void report_script_failure(const godot::String& message, const ScriptSourceLocation location) {
    mark_script_failure();
    godot::String located = message;
    if (location.path && *location.path != '\0') {
        located +=
            " at " + godot::String{location.path} + ":" + godot::String::num_int64(location.line);
        if (location.column > 0)
            located += ":" + godot::String::num_int64(location.column);
    }
    godot::UtilityFunctions::push_error(located);
}

void report_script_failure(const godot::String& message, const char* source_path,
                           const std::int64_t line, const std::int64_t column) {
    report_script_failure(message, ScriptSourceLocation{source_path, line, column});
}

godot::Object* strict_native_object_storage(const godot::Variant& value,
                                            const godot::StringName& expected_class,
                                            const ScriptSourceLocation location) {
    if (script_function_failed())
        return nullptr;
    if (value.get_type() == godot::Variant::NIL)
        return nullptr;
    if (value.get_type() != godot::Variant::OBJECT) {
        report_script_failure(godot::String{"Cannot assign "} + describe_variant_type(value) +
                                  " to " + godot::String{expected_class} + ".",
                              location);
        return nullptr;
    }
    auto* object = value.get_validated_object();
    if (!object) {
        const auto id = variant_object_id(value);
        if (id.is_valid())
            report_script_failure("Cannot assign a previously freed object instance.", location);
        return nullptr;
    }
    if (!object->is_class(expected_class)) {
        report_script_failure(godot::String{"Cannot assign object of type "} +
                                  godot::String{object->get_class()} + " to " +
                                  godot::String{expected_class} + ".",
                              location);
        return nullptr;
    }
    return object;
}

godot::Variant strict_external_object_storage(const godot::Variant& value,
                                              const godot::StringName& expected_class,
                                              const ScriptSourceLocation location) {
    if (value.get_type() == godot::Variant::NIL)
        return {};
    return strict_native_object_storage(value, expected_class, location) ? value : godot::Variant{};
}

void emit_local_signal_variants(godot::Object* owner, const godot::Variant** arguments,
                                const std::int64_t argument_count,
                                const ScriptSourceLocation location) {
    static const godot::StringName method_name{"emit_signal"};
    static GDExtensionMethodBindPtr method_bind =
        godot::gdextension_interface::classdb_get_method_bind(
            godot::Object::get_class_static()._native_ptr(), method_name._native_ptr(), 4047867050);
    if (!owner) {
        report_script_failure("Cannot emit a local signal on a null instance.", location);
        return;
    }
    if (!method_bind) {
        report_script_failure("Cannot resolve the Object.emit_signal runtime method.", location);
        return;
    }

    GDExtensionCallError error{};
    godot::Variant result;
    godot::gdextension_interface::object_method_bind_call(
        method_bind, owner->_owner, reinterpret_cast<GDExtensionConstVariantPtr*>(arguments),
        argument_count, result._native_ptr(), &error);
    if (error.error != GDEXTENSION_CALL_OK)
        report_script_failure("Local signal emission failed.", location);
}

namespace {

const godot::StringName& default_argument_marker() {
    static const godot::StringName marker{
        "__gdpp_internal_omitted_argument_7f7b20d940d64b33aebdbdc51ca21ab3__"};
    return marker;
}

std::optional<integer::Result> evaluate_integer_operator(const godot::Variant::Operator operation,
                                                         const std::int64_t left,
                                                         const std::int64_t right) {
    switch (operation) {
    case godot::Variant::OP_ADD:
        return integer::Result{integer::add(left, right)};
    case godot::Variant::OP_SUBTRACT:
        return integer::Result{integer::subtract(left, right)};
    case godot::Variant::OP_MULTIPLY:
        return integer::Result{integer::multiply(left, right)};
    case godot::Variant::OP_DIVIDE:
        return integer::divide(left, right);
    case godot::Variant::OP_MODULE:
        return integer::modulo(left, right);
    case godot::Variant::OP_SHIFT_LEFT:
        return integer::Result{integer::shift_left(left, right)};
    case godot::Variant::OP_SHIFT_RIGHT:
        return integer::Result{integer::shift_right(left, right)};
    case godot::Variant::OP_BIT_AND:
        return integer::Result{integer::bit_and(left, right)};
    case godot::Variant::OP_BIT_OR:
        return integer::Result{integer::bit_or(left, right)};
    case godot::Variant::OP_BIT_XOR:
        return integer::Result{integer::bit_xor(left, right)};
    default:
        return std::nullopt;
    }
}

void report_integer_error(const integer::ArithmeticError error,
                          const ScriptSourceLocation location = {}) {
    if (error == integer::ArithmeticError::division_by_zero)
        report_script_failure("Division by zero error in operator '/'.", location);
    else if (error == integer::ArithmeticError::modulo_by_zero)
        report_script_failure("Modulo by zero error in operator '%'.", location);
}

std::int64_t integer_result_or_zero(const integer::Result result,
                                    const ScriptSourceLocation location = {}) {
    if (result)
        return result.value;
    report_integer_error(result.error, location);
    return 0;
}

std::mutex& autoload_registry_mutex() {
    static std::mutex value;
    return value;
}

std::unordered_map<std::string, std::uint64_t>& autoload_registry() {
    // Store ordinary UTF-8 keys and instance IDs so process-static state never
    // retains Godot-owned StringName/Object wrappers during extension teardown.
    static auto* value = new std::unordered_map<std::string, std::uint64_t>();
    return *value;
}

std::string autoload_key(const godot::StringName& name) {
    const auto utf8 = godot::String(name).utf8();
    return {utf8.get_data(), static_cast<std::size_t>(utf8.length())};
}

const char* operator_name(const godot::Variant::Operator operation) noexcept {
    switch (operation) {
    case godot::Variant::OP_EQUAL:
        return "==";
    case godot::Variant::OP_NOT_EQUAL:
        return "!=";
    case godot::Variant::OP_LESS:
        return "<";
    case godot::Variant::OP_LESS_EQUAL:
        return "<=";
    case godot::Variant::OP_GREATER:
        return ">";
    case godot::Variant::OP_GREATER_EQUAL:
        return ">=";
    case godot::Variant::OP_ADD:
        return "+";
    case godot::Variant::OP_SUBTRACT:
        return "-";
    case godot::Variant::OP_MULTIPLY:
        return "*";
    case godot::Variant::OP_DIVIDE:
        return "/";
    case godot::Variant::OP_NEGATE:
        return "unary -";
    case godot::Variant::OP_POSITIVE:
        return "unary +";
    case godot::Variant::OP_MODULE:
        return "%";
    case godot::Variant::OP_POWER:
        return "**";
    case godot::Variant::OP_SHIFT_LEFT:
        return "<<";
    case godot::Variant::OP_SHIFT_RIGHT:
        return ">>";
    case godot::Variant::OP_BIT_AND:
        return "&";
    case godot::Variant::OP_BIT_OR:
        return "|";
    case godot::Variant::OP_BIT_XOR:
        return "^";
    case godot::Variant::OP_BIT_NEGATE:
        return "~";
    case godot::Variant::OP_AND:
        return "and";
    case godot::Variant::OP_OR:
        return "or";
    case godot::Variant::OP_XOR:
        return "xor";
    case godot::Variant::OP_NOT:
        return "not";
    case godot::Variant::OP_IN:
        return "in";
    case godot::Variant::OP_MAX:
        break;
    }
    return "<unknown>";
}

godot::String call_error_message(const godot::String& subject, const GDExtensionCallError& error) {
    switch (error.error) {
    case GDEXTENSION_CALL_ERROR_INVALID_METHOD:
        return subject + godot::String{" has no callable method."};
    case GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT:
        return subject + godot::String{" rejected argument "} +
               godot::String::num_int64(error.argument + 1) + godot::String{"; expected "} +
               godot::Variant::get_type_name(static_cast<godot::Variant::Type>(error.expected)) +
               godot::String{"."};
    case GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS:
        return subject + godot::String{" received too many arguments; expected "} +
               godot::String::num_int64(error.expected) + godot::String{"."};
    case GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS:
        return subject + godot::String{" received too few arguments; expected "} +
               godot::String::num_int64(error.expected) + godot::String{"."};
    case GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL:
        return subject + godot::String{" targets a null or freed instance."};
    case GDEXTENSION_CALL_ERROR_METHOD_NOT_CONST:
        return subject + godot::String{" cannot call a non-const method in this context."};
    case GDEXTENSION_CALL_OK:
        break;
    }
    return subject + godot::String{" failed with call error "} +
           godot::String::num_int64(static_cast<std::int64_t>(error.error)) + godot::String{"."};
}

class LambdaCallable final : public godot::CallableCustom {
  public:
    LambdaCallable(godot::Object* owner, std::size_t required_arguments,
                   std::size_t positional_arguments, bool is_vararg,
                   CallableContinuation continuation)
        : owner_(owner ? owner->get_instance_id() : godot::ObjectID{}),
          required_arguments_(required_arguments), positional_arguments_(positional_arguments),
          is_vararg_(is_vararg), continuation_(std::move(continuation)) {}

    [[nodiscard]] std::uint32_t hash() const override {
        return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(this));
    }

    [[nodiscard]] godot::String get_as_text() const override { return "GDPP lambda"; }

    [[nodiscard]] bool is_valid() const override {
        // Static GDScript function values intentionally have no Object owner. The
        // godot-cpp default treats an empty ObjectID as invalid, which prevents an
        // otherwise live CallableCustom from being connected to signals. Owner-free
        // callables remain valid for the lifetime of their Callable value; captured
        // instance lambdas continue to expire with their owner.
        return !owner_.is_valid() ||
               godot::ObjectDB::get_instance(static_cast<std::uint64_t>(owner_)) != nullptr;
    }

    static bool compare_equal(const godot::CallableCustom* left,
                              const godot::CallableCustom* right) {
        return left == right;
    }

    [[nodiscard]] CompareEqualFunc get_compare_equal_func() const override {
        return &LambdaCallable::compare_equal;
    }

    static bool compare_less(const godot::CallableCustom* left,
                             const godot::CallableCustom* right) {
        return left < right;
    }

    [[nodiscard]] CompareLessFunc get_compare_less_func() const override {
        return &LambdaCallable::compare_less;
    }

    [[nodiscard]] godot::ObjectID get_object() const override { return owner_; }

    [[nodiscard]] int get_argument_count(bool& valid) const override {
        valid = true;
        return static_cast<int>(positional_arguments_);
    }

    void call(const godot::Variant** arguments, int argument_count, godot::Variant& return_value,
              GDExtensionCallError& error) const override {
        if (owner_.is_valid() &&
            !godot::ObjectDB::get_instance(static_cast<std::uint64_t>(owner_))) {
            return_value = {};
            error.error = GDEXTENSION_CALL_OK;
            return;
        }
        if (argument_count < static_cast<int>(required_arguments_)) {
            error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
            error.expected = static_cast<int32_t>(required_arguments_);
            return;
        }
        if (!is_vararg_ && argument_count > static_cast<int>(positional_arguments_)) {
            error.error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
            error.expected = static_cast<int32_t>(positional_arguments_);
            return;
        }
        godot::Array values;
        for (int index = 0; index < argument_count; ++index)
            values.push_back(*arguments[index]);
        return_value = continuation_(values);
        error.error = GDEXTENSION_CALL_OK;
    }

  private:
    godot::ObjectID owner_;
    std::size_t required_arguments_{0};
    std::size_t positional_arguments_{0};
    bool is_vararg_{false};
    CallableContinuation continuation_;
};

void report_invalid_member(const char* operation, const godot::StringName& name,
                           const ScriptSourceLocation location) {
    report_script_failure(
        godot::String("Dynamic ") + operation + " '" + godot::String(name) + "' failed.", location);
}

void report_invalid_key(const char* operation, const godot::Variant& target,
                        const godot::Variant& key, const ScriptSourceLocation location) {
    report_script_failure(godot::String("Invalid keyed ") + operation + " on " +
                              describe_variant_type(target) + " with key type " +
                              describe_variant_type(key) + ".",
                          location);
}

bool reject_invalid_object_target(const godot::Variant& target, const char* operation,
                                  const godot::StringName* member,
                                  const ScriptSourceLocation location) {
    if (target.get_type() != godot::Variant::OBJECT || target.get_validated_object())
        return false;
    auto message = godot::String{"Cannot "} + operation;
    if (member)
        message += godot::String{" '"} + godot::String{*member} + "'";
    const auto id = variant_object_id(target);
    report_script_failure(
        message + (id.is_valid() ? " on a previously freed instance." : " on a null instance."),
        location);
    return true;
}

} // namespace

godot::Variant default_argument() { return default_argument_marker(); }

bool is_default_argument(const godot::Variant& value) {
    return value.get_type() == godot::Variant::STRING_NAME &&
           static_cast<godot::StringName>(value) == default_argument_marker();
}

godot::Variant binary(godot::Variant::Operator operation, const godot::Variant& left,
                      const godot::Variant& right, const ScriptSourceLocation location) {
    if (left.get_type() == godot::Variant::INT && right.get_type() == godot::Variant::INT) {
        const auto left_value = static_cast<std::int64_t>(left);
        const auto right_value = static_cast<std::int64_t>(right);
        if (const auto result = evaluate_integer_operator(operation, left_value, right_value)) {
            if (!*result) {
                report_integer_error(result->error, location);
                return {};
            }
            return result->value;
        }
    }
    godot::Variant result;
    bool valid = false;
    godot::Variant::evaluate(operation, left, right, result, valid);
    if (!valid) {
        // GDScript permits equality checks between unrelated Variant types: they compare unequal
        // instead of reporting an invalid operator. Variant::evaluate exposes that case through
        // r_valid=false, so preserve the language behavior before diagnosing real operator errors.
        if (operation == godot::Variant::OP_EQUAL)
            return false;
        if (operation == godot::Variant::OP_NOT_EQUAL)
            return true;
        report_script_failure(godot::String{"Invalid operands "} + describe_variant_type(left) +
                                  " and " + describe_variant_type(right) + " for operator '" +
                                  operator_name(operation) + "'.",
                              location);
        return {};
    }
    return result;
}

godot::Variant binary_integer(const godot::Variant::Operator operation, const godot::Variant& left,
                              const std::int64_t right, const ScriptSourceLocation location) {
    if (left.get_type() == godot::Variant::INT) {
        const auto left_value = static_cast<std::int64_t>(left);
        if (const auto result = evaluate_integer_operator(operation, left_value, right)) {
            if (!*result) {
                report_integer_error(result->error, location);
                return {};
            }
            return result->value;
        }
    }
    return binary(operation, left, godot::Variant(right), location);
}

godot::Variant binary_integer(const godot::Variant::Operator operation, const std::int64_t left,
                              const godot::Variant& right, const ScriptSourceLocation location) {
    if (right.get_type() == godot::Variant::INT) {
        const auto right_value = static_cast<std::int64_t>(right);
        if (const auto result = evaluate_integer_operator(operation, left, right_value)) {
            if (!*result) {
                report_integer_error(result->error, location);
                return {};
            }
            return result->value;
        }
    }
    return binary(operation, godot::Variant(left), right, location);
}

void compound_assign(godot::Variant& target, const godot::Variant::Operator operation,
                     const godot::Variant& value, const ScriptSourceLocation location) {
    if (target.get_type() == godot::Variant::INT && value.get_type() == godot::Variant::INT) {
        const auto left = *godot::VariantInternal::get_int(&target);
        const auto right = *godot::VariantInternal::get_int(&value);
        if (const auto result = evaluate_integer_operator(operation, left, right)) {
            if (!*result) {
                report_integer_error(result->error, location);
                return;
            }
            *godot::VariantInternal::get_int(&target) = result->value;
            return;
        }
    }
    const auto result = binary(operation, target, value, location);
    if (!script_function_failed())
        target = result;
}

void compound_assign_integer(godot::Variant& target, const godot::Variant::Operator operation,
                             const std::int64_t value, const ScriptSourceLocation location) {
    if (target.get_type() == godot::Variant::INT) {
        const auto left = *godot::VariantInternal::get_int(&target);
        if (const auto result = evaluate_integer_operator(operation, left, value)) {
            if (!*result) {
                report_integer_error(result->error, location);
                return;
            }
            *godot::VariantInternal::get_int(&target) = result->value;
            return;
        }
    }
    const auto result = binary_integer(operation, target, value, location);
    if (!script_function_failed())
        target = result;
}

void assign_dictionary(godot::Dictionary& target, const godot::Dictionary& value) {
    if (&target == &value)
        return;
    target = value;
}

godot::Variant unary(godot::Variant::Operator operation, const godot::Variant& operand,
                     const ScriptSourceLocation location) {
    if (operand.get_type() == godot::Variant::INT) {
        const auto value = static_cast<std::int64_t>(operand);
        if (operation == godot::Variant::OP_POSITIVE)
            return value;
        if (operation == godot::Variant::OP_NEGATE)
            return integer::negate(value);
        if (operation == godot::Variant::OP_BIT_NEGATE)
            return integer::bit_not(value);
    }
    godot::Variant result;
    bool valid = false;
    godot::Variant::evaluate(operation, operand, godot::Variant{}, result, valid);
    if (!valid) {
        report_script_failure(godot::String{"Invalid operand "} + describe_variant_type(operand) +
                                  " for operator '" + operator_name(operation) + "'.",
                              location);
        return {};
    }
    return result;
}

std::int64_t integer_divide(const std::int64_t left, const std::int64_t right,
                            const ScriptSourceLocation location) {
    return integer_result_or_zero(integer::divide(left, right), location);
}

std::int64_t integer_modulo(const std::int64_t left, const std::int64_t right,
                            const ScriptSourceLocation location) {
    return integer_result_or_zero(integer::modulo(left, right), location);
}

bool is_instance_valid(const godot::Variant& value) noexcept {
    return value.get_type() == godot::Variant::OBJECT && value.get_validated_object() != nullptr;
}

void free_object_at(const godot::Variant& value, const ScriptSourceLocation location) {
    if (script_function_failed())
        return;
    if (value.get_type() != godot::Variant::OBJECT) {
        report_script_failure("Attempted to free a non-Object value.", location);
        return;
    }
    auto* object = value.get_validated_object();
    if (!object) {
        const auto identity = variant_object_id(value);
        report_script_failure(identity.is_valid() ? "Attempted to free a previously freed Object."
                                                  : "Attempted to free a null Object.",
                              location);
        return;
    }
    godot::Variant target = value;
    godot::Variant ignored;
    GDExtensionCallError error{};
    target.callp(godot::StringName{"free"}, nullptr, 0, ignored, error);
    if (error.error == GDEXTENSION_CALL_OK)
        return;
    if (error.error == GDEXTENSION_CALL_ERROR_INVALID_METHOD) {
        report_script_failure(godot::Object::cast_to<godot::RefCounted>(object)
                                  ? "Attempted to free a RefCounted object."
                                  : "Attempted to free a locked object (calling or emitting).",
                              location);
        return;
    }
    report_script_failure(call_error_message("Object.free()", error), location);
}

godot::Array make_range(std::int64_t stop) { return make_range(0, stop, 1); }

godot::Array make_range(std::int64_t start, std::int64_t stop) {
    return make_range(start, stop, 1);
}

godot::Array make_range(std::int64_t start, std::int64_t stop, std::int64_t step) {
    godot::Array result;
    if (step == 0) {
        report_script_failure("Error calling GDScript utility function \"range()\": Step argument "
                              "is zero!",
                              nullptr, 0, 0);
        return result;
    }
    for (auto value = start; step > 0 ? value < stop : value > stop;) {
        result.push_back(value);
        value = integer::range_advance(value, step, stop);
    }
    return result;
}

std::int64_t length(const godot::Variant& value) {
    godot::Variant target = value;
    const auto result = call_dynamic_impl(target, godot::StringName("size"), nullptr, 0);
    return static_cast<std::int64_t>(result);
}

godot::Array get_stack() { return attached_debug_stack(); }

void print_debug_values(const godot::Array& values) {
    godot::String message;
    for (std::int64_t index = 0; index < values.size(); ++index)
        message += static_cast<godot::String>(values[index]);
    const auto stack = attached_debug_stack();
    if (!stack.is_empty()) {
        const godot::Dictionary frame = stack[0];
        const godot::Variant line = frame.get("line", -1);
        message += "\n   At: " + static_cast<godot::String>(frame.get("source", "")) + ":" +
                   godot::String::num_int64(line.operator std::int64_t()) + ":" +
                   static_cast<godot::String>(frame.get("function", "")) + "()";
    }
    godot::UtilityFunctions::print(message);
}

void print_stack() {
    const auto stack = attached_debug_stack();
    for (std::int64_t index = 0; index < stack.size(); ++index) {
        const godot::Dictionary frame = stack[index];
        const godot::Variant line = frame.get("line", -1);
        godot::UtilityFunctions::print("Frame " + godot::String::num_int64(index) + " - " +
                                       static_cast<godot::String>(frame.get("source", "")) + ":" +
                                       godot::String::num_int64(line.operator std::int64_t()) +
                                       " in function '" +
                                       static_cast<godot::String>(frame.get("function", "")) + "'");
    }
}

godot::Dictionary instance_to_dictionary(const godot::Variant& instance,
                                         const ScriptSourceLocation location) {
    if (instance.get_type() == godot::Variant::NIL)
        return {};
    if (instance.get_type() != godot::Variant::OBJECT || !instance.get_validated_object()) {
        report_script_failure(
            "Error calling GDScript utility function \"inst_to_dict()\": Not a script with an "
            "instance.",
            location);
        return {};
    }
    godot::String error;
    auto result = attached_instance_to_dictionary(instance.get_validated_object(), &error);
    if (!error.is_empty())
        report_script_failure(
            "Error calling GDScript utility function \"inst_to_dict()\": " + error, location);
    return result;
}

godot::Variant dictionary_to_instance(const godot::Variant& dictionary,
                                      const ScriptSourceLocation location) {
    if (dictionary.get_type() != godot::Variant::DICTIONARY) {
        report_script_failure(
            "Error calling GDScript utility function \"dict_to_inst()\": Expected a Dictionary.",
            location);
        return {};
    }
    godot::String error;
    auto result = dictionary_to_attached_instance(godot::Dictionary{dictionary}, &error);
    if (!error.is_empty())
        report_script_failure(
            "Error calling GDScript utility function \"dict_to_inst()\": " + error, location);
    return result;
}

godot::Variant convert_value(const godot::Variant& value, const std::int64_t type) {
    if (type < 0 || type >= static_cast<std::int64_t>(godot::Variant::VARIANT_MAX)) {
        godot::UtilityFunctions::push_error(
            "GDPP: invalid type argument to convert(), use a TYPE_* constant");
        return {};
    }
    return godot::UtilityFunctions::type_convert(value, type);
}

bool type_exists(const godot::Variant& name) {
    if (name.get_type() != godot::Variant::STRING &&
        name.get_type() != godot::Variant::STRING_NAME) {
        godot::UtilityFunctions::push_error(
            "GDPP: type_exists() expects a String or StringName argument");
        return false;
    }
    const auto* class_db = godot::ClassDBSingleton::get_singleton();
    return class_db && class_db->class_exists(static_cast<godot::StringName>(name));
}

godot::String character(const std::int64_t code) {
    const bool surrogate = code >= 0xD800 && code <= 0xDFFF;
    if (code <= 0 || code > 0x10FFFF || surrogate) {
        godot::UtilityFunctions::push_error(
            godot::String("GDPP: invalid Unicode scalar passed to char(): ") +
            godot::String::num_int64(code));
        return {};
    }
    return godot::String::chr(code);
}

std::int64_t ordinal(const godot::Variant& character_value) {
    if (character_value.get_type() != godot::Variant::STRING &&
        character_value.get_type() != godot::Variant::STRING_NAME) {
        godot::UtilityFunctions::push_error("GDPP: ord() expects a String argument");
        return 0;
    }
    const auto value = static_cast<godot::String>(character_value);
    if (value.length() != 1) {
        godot::UtilityFunctions::push_error(
            "GDPP: ord() expects a string containing exactly one Unicode character");
        return 0;
    }
    return value.unicode_at(0);
}

godot::Color color8(const std::int64_t red, const std::int64_t green, const std::int64_t blue,
                    const std::int64_t alpha) {
    return godot::Color::from_rgba8(red, green, blue, alpha);
}

bool is_instance_of(const godot::Variant& value, const godot::Variant& type_descriptor) {
    if (type_descriptor.get_type() == godot::Variant::INT) {
        const auto type = static_cast<std::int64_t>(type_descriptor);
        if (type < 0 || type >= static_cast<std::int64_t>(godot::Variant::VARIANT_MAX)) {
            godot::UtilityFunctions::push_error(
                "GDPP: invalid TYPE_* constant passed to is_instance_of()");
            return false;
        }
        return static_cast<std::int64_t>(value.get_type()) == type;
    }

    if (type_descriptor.get_type() == godot::Variant::STRING_NAME ||
        type_descriptor.get_type() == godot::Variant::STRING) {
        if (value.get_type() != godot::Variant::OBJECT)
            return false;
        auto* object = value.get_validated_object();
        return object && object->is_class(static_cast<godot::StringName>(type_descriptor));
    }

    if (type_descriptor.get_type() == godot::Variant::OBJECT) {
        auto* descriptor_object = type_descriptor.get_validated_object();
        auto* script = godot::Object::cast_to<godot::Script>(descriptor_object);
        auto* object =
            value.get_type() == godot::Variant::OBJECT ? value.get_validated_object() : nullptr;
        if (script)
            return object && script->instance_has(object);
    }

    godot::UtilityFunctions::push_error(
        "GDPP: is_instance_of() expects a TYPE_* constant, class, or script type");
    return false;
}

godot::Variant load_resource(const godot::String& path) {
    const auto script_path = resolve_attached_script_resource_path(path);
    if (!script_path.is_empty()) {
        if (const auto descriptor = find_attached_script(script_path); descriptor) {
            godot::String error;
            const auto script = attached_script_resource(script_path, &error);
            if (script.is_valid())
                return godot::Variant(static_cast<const godot::Object*>(script.ptr()));
            godot::UtilityFunctions::push_error(
                godot::String{"GDPP: cannot materialize compiled script '"} + path + "': " + error);
            return {};
        }
    }
    const auto resource = godot::ResourceLoader::get_singleton()->load(path);
    if (resource.is_null())
        godot::UtilityFunctions::push_error(godot::String("GDPP: cannot load resource '") + path +
                                            "'");
    // Do not rely on Ref<T>::operator Variant() here. On MSVC the Resource* argument can select
    // Variant(GDExtensionConstVariantPtr) instead of Variant(const Object*), which treats the
    // wrapper object as an already constructed native Variant and corrupts the returned value.
    // Selecting the Object overload explicitly also keeps the conversion ABI-stable across the
    // Godot 4.4+ SDK packages.
    return godot::Variant(static_cast<const godot::Object*>(resource.ptr()));
}

bool is_editor_hint() noexcept {
    const auto* engine = godot::Engine::get_singleton();
    return engine != nullptr && engine->is_editor_hint();
}

void register_autoload(const godot::StringName& name, godot::Object* instance) {
    if (!instance)
        return;
    std::lock_guard lock{autoload_registry_mutex()};
    const auto key = autoload_key(name);
    const auto found = autoload_registry().find(key);
    if (found != autoload_registry().end() &&
        godot::ObjectDB::get_instance(found->second) != nullptr) {
        return;
    }
    autoload_registry().insert_or_assign(key,
                                         static_cast<std::uint64_t>(instance->get_instance_id()));
}

godot::Object* find_autoload(const godot::StringName& name) {
    const auto* engine = godot::Engine::get_singleton();
    auto* tree =
        engine ? godot::Object::cast_to<godot::SceneTree>(engine->get_main_loop()) : nullptr;
    auto* root = tree ? tree->get_root() : nullptr;
    if (root) {
        // The root Window can exist before SceneTree marks it as inside the active
        // tree (for example when an exported game starts from a scene passed on the
        // command line). Absolute NodePaths reject that valid startup window. An
        // autoload is always a direct root child, so relative lookup is equivalent
        // after startup and remains safe during this early initialization phase.
        if (auto* instance = root->get_node_or_null(godot::NodePath(godot::String(name))))
            return instance;
    }
    // Native constructors run before PackedScene assigns the node name or adds
    // the instance to SceneTree. Generated autoload classes register their
    // ObjectID before executing field initializers/_init(), which preserves
    // GDScript's ordered autoload visibility during that startup window.
    std::lock_guard lock{autoload_registry_mutex()};
    const auto found = autoload_registry().find(autoload_key(name));
    return found == autoload_registry().end() ? nullptr
                                              : godot::ObjectDB::get_instance(found->second);
}

godot::Object* find_engine_singleton_at(const godot::StringName& name,
                                        const ScriptSourceLocation location) {
    const auto* engine = godot::Engine::get_singleton();
    auto* singleton = engine ? engine->get_singleton(name) : nullptr;
    if (!singleton)
        report_script_failure(godot::String{"Engine singleton '"} + godot::String{name} +
                                  godot::String{"' is unavailable."},
                              location);
    return singleton;
}

godot::Variant script_identity(godot::Object* object) {
    if (!object)
        return {};
    const auto native_class = object->get_class();
    if (native_class.begins_with("GDPPNative_"))
        return godot::StringName(native_class);
    return object->get_script();
}

godot::Variant call_callable_impl(const godot::Callable& callable, const godot::Variant** arguments,
                                  const std::size_t argument_count,
                                  const ScriptSourceLocation location) {
    if (!callable.is_valid()) {
        report_script_failure("Attempt to call a null or freed Callable.", location);
        return {};
    }

    // Calling through Variant::callp is the public GDExtension path that preserves the
    // Callable's CallError. godot-cpp's variadic Callable::call() deliberately discards that
    // error, which would let generated execution continue after invalid arity, invalid argument,
    // unbind, and freed-target failures even though GDScript stops the current function.
    static const godot::StringName call_method{"call"};
    godot::Variant callable_value{callable};
    godot::Variant result;
    GDExtensionCallError error{GDEXTENSION_CALL_OK, 0, 0};
    callable_value.callp(call_method, arguments, static_cast<int>(argument_count), result, error);
    if (error.error != GDEXTENSION_CALL_OK) {
        report_script_failure(call_error_message("Callable", error), location);
        return {};
    }
    return result;
}

godot::Variant instantiate_external_class_at(const godot::StringName& name,
                                             const ScriptSourceLocation location) {
    auto* class_db = godot::ClassDBSingleton::get_singleton();
    if (!class_db || !class_db->class_exists(name) || !class_db->can_instantiate(name)) {
        report_script_failure(godot::String{"External class '"} + godot::String{name} +
                                  godot::String{"' is unavailable or cannot be instantiated."},
                              location);
        return {};
    }
    godot::Variant result{class_db->instantiate(name)};
    if (result.get_type() != godot::Variant::OBJECT || !result.get_validated_object()) {
        report_script_failure(godot::String{"External class '"} + godot::String{name} +
                                  godot::String{"' returned no live instance."},
                              location);
        return {};
    }
    return result;
}

godot::Variant call_external_static_impl(const godot::StringName& class_name,
                                         const godot::StringName& method,
                                         const godot::Variant** arguments,
                                         const std::size_t argument_count,
                                         const ScriptSourceLocation location) {
    auto* class_db = godot::ClassDBSingleton::get_singleton();
    if (!class_db || !class_db->class_exists(class_name) ||
        !class_db->class_has_method(class_name, method)) {
        report_script_failure(godot::String{"External static method '"} +
                                  godot::String{class_name} + godot::String{"."} +
                                  godot::String{method} + godot::String{"' is unavailable."},
                              location);
        return {};
    }

    static const godot::StringName class_call_static_method{"class_call_static"};
    godot::Variant class_db_value{class_db};
    godot::Variant result;
    GDExtensionCallError error{GDEXTENSION_CALL_OK, 0, 0};
    class_db_value.callp(class_call_static_method, arguments, static_cast<int>(argument_count),
                         result, error);
    if (error.error != GDEXTENSION_CALL_OK) {
        report_script_failure(call_error_message(godot::String{"External static method '"} +
                                                     godot::String{class_name} +
                                                     godot::String{"."} + godot::String{method} +
                                                     godot::String{"'"},
                                                 error),
                              location);
        return {};
    }
    return result;
}

bool is_external_instance(const godot::Variant& value, const godot::StringName& class_name) {
    if (value.get_type() != godot::Variant::OBJECT)
        return false;
    auto* object = value.get_validated_object();
    return object && object->is_class(class_name);
}

godot::Callable bound_method_at(const godot::Variant& value, const godot::StringName& method,
                                const std::size_t required_arguments,
                                const std::size_t positional_arguments, const bool is_vararg,
                                const ScriptSourceLocation location) {
    if (value.get_type() == godot::Variant::OBJECT) {
        auto* object = value.get_validated_object();
        if (!object) {
            report_script_failure("Cannot create a Callable for a null or freed Godot object.",
                                  location);
            return {};
        }
        if (!object->has_method(method)) {
            report_script_failure(godot::String{"Godot object has no method '"} +
                                      godot::String{method} + godot::String{"'."},
                                  location);
            return {};
        }
        return {object, method};
    }
    if (!value.has_method(method)) {
        report_script_failure(godot::String{"Godot value of type '"} +
                                  godot::Variant::get_type_name(value.get_type()) +
                                  godot::String{"' has no method '"} + godot::String{method} +
                                  godot::String{"'."},
                              location);
        return {};
    }
    return make_callable(nullptr, required_arguments, positional_arguments, is_vararg,
                         [target = value, method, location](const godot::Array& arguments) mutable {
                             std::vector<godot::Variant> values;
                             values.reserve(static_cast<std::size_t>(arguments.size()));
                             for (std::int64_t index = 0; index < arguments.size(); ++index)
                                 values.push_back(arguments[index]);
                             std::vector<const godot::Variant*> pointers;
                             pointers.reserve(values.size());
                             for (const auto& argument : values)
                                 pointers.push_back(&argument);
                             return call_dynamic_impl(target, method, pointers.data(),
                                                      pointers.size(), location);
                         });
}

godot::Callable bound_builtin_static_method_at(const godot::Variant::Type type,
                                               const godot::StringName& method,
                                               const std::size_t required_arguments,
                                               const std::size_t positional_arguments,
                                               const bool is_vararg,
                                               const ScriptSourceLocation location) {
    return make_callable(
        nullptr, required_arguments, positional_arguments, is_vararg,
        [type, method, location](const godot::Array& arguments) {
            std::vector<godot::Variant> values;
            values.reserve(static_cast<std::size_t>(arguments.size()));
            for (std::int64_t index = 0; index < arguments.size(); ++index)
                values.push_back(arguments[index]);
            std::vector<const godot::Variant*> pointers;
            pointers.reserve(values.size());
            for (const auto& argument : values)
                pointers.push_back(&argument);
            godot::Variant result;
            GDExtensionCallError error{GDEXTENSION_CALL_OK, 0, 0};
            godot::Variant::callp_static(type, method, pointers.data(),
                                         static_cast<int>(pointers.size()), result, error);
            if (error.error != GDEXTENSION_CALL_OK) {
                report_script_failure(call_error_message(godot::String{"Static builtin method '"} +
                                                             godot::Variant::get_type_name(type) +
                                                             godot::String{"."} +
                                                             godot::String{method} +
                                                             godot::String{"'"},
                                                         error),
                                      location);
                return godot::Variant{};
            }
            return result;
        });
}

godot::Callable bound_class_static_method_at(const godot::StringName& class_name,
                                             const godot::StringName& method,
                                             const std::size_t required_arguments,
                                             const std::size_t positional_arguments,
                                             const bool is_vararg,
                                             const ScriptSourceLocation location) {
    return make_callable(nullptr, required_arguments, positional_arguments, is_vararg,
                         [class_name, method, location](const godot::Array& arguments) {
                             std::vector<godot::Variant> values;
                             values.reserve(2 + static_cast<std::size_t>(arguments.size()));
                             values.emplace_back(class_name);
                             values.emplace_back(method);
                             for (std::int64_t index = 0; index < arguments.size(); ++index)
                                 values.push_back(arguments[index]);
                             std::vector<const godot::Variant*> pointers;
                             pointers.reserve(values.size());
                             for (const auto& argument : values)
                                 pointers.push_back(&argument);
                             return call_external_static_impl(class_name, method, pointers.data(),
                                                              pointers.size(), location);
                         });
}

godot::Callable external_callable_at(const godot::Variant& value, const godot::StringName& method,
                                     const ScriptSourceLocation location) {
    auto* object = value.get_validated_object();
    if (!object) {
        report_script_failure("Cannot create a Callable for a null or freed external object.",
                              location);
        return {};
    }
    if (!object->has_method(method)) {
        report_script_failure(godot::String{"External object has no method '"} +
                                  godot::String{method} + godot::String{"'."},
                              location);
        return {};
    }
    return {object, method};
}

godot::Signal external_signal_at(const godot::Variant& value, const godot::StringName& signal,
                                 const ScriptSourceLocation location) {
    auto* object = value.get_validated_object();
    if (!object) {
        report_script_failure("Cannot access a Signal on a null or freed external object.",
                              location);
        return {};
    }
    if (!object->has_signal(signal)) {
        report_script_failure(godot::String{"External object has no signal '"} +
                                  godot::String{signal} + godot::String{"'."},
                              location);
        return {};
    }
    return {object, signal};
}

bool await_signal(const godot::Variant& signal_value, godot::Object* owner,
                  const CoroutineStatePtr& coroutine, AwaitContinuation continuation) {
    if (!continuation) {
        godot::UtilityFunctions::push_error("GDPP: await requires a live continuation");
        return false;
    }
    const auto active_coroutine = coroutine ? coroutine : begin_coroutine(owner);
    if (!active_coroutine)
        return false;
    godot::Signal signal;
    if (signal_value.get_type() == godot::Variant::SIGNAL) {
        signal = static_cast<godot::Signal>(signal_value);
    } else if (signal_value.get_type() == godot::Variant::OBJECT) {
        auto* state = signal_value.get_validated_object();
        if (state && state->has_signal(coroutine_completed_signal()) &&
            (state->is_class(CoroutineFunctionState::get_class_static()) ||
             state->is_class(godot::StringName{"GDScriptFunctionState"}))) {
            signal = godot::Signal(state, coroutine_completed_signal());
        }
    }
    if (signal.is_null()) {
        godot::UtilityFunctions::push_error(
            "GDPP: await requires a Signal or coroutine function state");
        return false;
    }
    auto* function_state_object =
        godot::ObjectDB::get_instance(static_cast<std::uint64_t>(active_coroutine->function_state));
    godot::Ref<CoroutineFunctionState> function_state{
        godot::Object::cast_to<CoroutineFunctionState>(function_state_object)};
    if (function_state.is_null()) {
        godot::UtilityFunctions::push_error(
            "GDPP: await cannot continue because its coroutine function state was freed");
        return false;
    }

    function_state->install(owner, active_coroutine, std::move(continuation));
    godot::Callable callable{function_state.ptr(), "_signal_callback"};
    callable = callable.bind(godot::Variant{function_state});
    const auto error = signal.connect(callable, godot::Object::CONNECT_ONE_SHOT);
    if (error != 0) {
        function_state->finish();
        godot::UtilityFunctions::push_error("GDPP: failed to connect await continuation");
        return false;
    }
    if (!coroutine) {
        const std::lock_guard<std::mutex> lock{active_coroutine->mutex};
        active_coroutine->initial_function_state.unref();
    }
    return true;
}

bool is_awaitable(const godot::Variant& value) {
    if (value.get_type() == godot::Variant::SIGNAL)
        return true;
    if (value.get_type() != godot::Variant::OBJECT)
        return false;
    auto* state = value.get_validated_object();
    return state && state->has_signal(coroutine_completed_signal()) &&
           (state->is_class(CoroutineFunctionState::get_class_static()) ||
            state->is_class(godot::StringName{"GDScriptFunctionState"}));
}

godot::Variant await_result(const godot::Array& arguments) {
    if (arguments.is_empty())
        return {};
    if (arguments.size() == 1)
        return arguments[0];
    return arguments;
}

CoroutineStatePtr begin_coroutine(godot::Object* owner) {
    auto state = std::make_shared<CoroutineState>();
    state->initial_function_state.instantiate();
    if (state->initial_function_state.is_null()) {
        godot::UtilityFunctions::push_error("GDPP: cannot allocate a coroutine function state");
        return {};
    }
    state->owner = owner ? owner->get_instance_id() : godot::ObjectID{};
    state->function_state = state->initial_function_state->get_instance_id();
    return state;
}

godot::Object* coroutine_owner(const CoroutineStatePtr& state) {
    if (!state)
        return nullptr;
    if (state->owner.is_null())
        return godot::ObjectDB::get_instance(static_cast<std::uint64_t>(state->function_state));
    return godot::ObjectDB::get_instance(static_cast<std::uint64_t>(state->owner));
}

godot::Variant coroutine_result(const CoroutineStatePtr& state) {
    if (!state)
        return {};
    godot::Variant result;
    godot::Ref<CoroutineFunctionState> function_state;
    bool completed = false;
    {
        const std::lock_guard<std::mutex> lock{state->mutex};
        completed = state->completed;
        if (completed)
            result = state->result;
        else {
            state->exposed = true;
            function_state = state->initial_function_state;
            if (function_state.is_null()) {
                auto* object = godot::ObjectDB::get_instance(
                    static_cast<std::uint64_t>(state->function_state));
                function_state.reference_ptr(
                    godot::Object::cast_to<CoroutineFunctionState>(object));
            }
        }
        state->initial_function_state.unref();
    }
    if (completed)
        return result;
    return godot::Variant{function_state};
}

void complete_coroutine(const CoroutineStatePtr& state, const godot::Variant& result) {
    if (!state)
        return;
    bool exposed = false;
    {
        const std::lock_guard<std::mutex> lock{state->mutex};
        if (state->completed)
            return;
        state->result = result;
        state->completed = true;
        exposed = state->exposed;
    }
    if (!exposed)
        return;
    auto* function_state_object =
        godot::ObjectDB::get_instance(static_cast<std::uint64_t>(state->function_state));
    godot::Ref<CoroutineFunctionState> function_state{
        godot::Object::cast_to<CoroutineFunctionState>(function_state_object)};
    if (function_state.is_null())
        return;
    function_state->finish();
    function_state->emit_signal(coroutine_completed_signal(), result);
}

bool validate_virtual_return(const godot::Variant& value, const godot::Variant::Type expected_type,
                             const godot::StringName& method,
                             const godot::StringName& expected_name,
                             const godot::StringName& expected_class, const char* source_path,
                             const std::int64_t line, const std::int64_t column) {
    // Godot invokes script virtuals through Variant and casts the returned value only after the
    // language call. A pending GDScript coroutine is therefore allowed to cross every virtual
    // return ABI as its FunctionState object, even when the declared result is String or another
    // concrete type. Preserve that observable behavior; validating the eventual typed result is
    // only meaningful when the coroutine completed synchronously.
    if (is_awaitable(value))
        return true;
    bool compatible = false;
    if (expected_type == godot::Variant::OBJECT) {
        compatible =
            value.get_type() == godot::Variant::NIL || value.get_type() == godot::Variant::OBJECT;
        if (compatible && value.get_type() == godot::Variant::OBJECT &&
            !expected_class.is_empty()) {
            auto* object = value.get_validated_object();
            compatible = object && object->is_class(expected_class);
        }
    } else {
        compatible = godot::Variant::can_convert_strict(value.get_type(), expected_type);
    }
    if (compatible)
        return true;

    const auto path = source_path ? godot::String::utf8(source_path) : godot::String{"<unknown>"};
    godot::UtilityFunctions::push_error(
        godot::String{"GDPP: virtual method '"} + godot::String(method) + "' returned " +
        godot::Variant::get_type_name(value.get_type()) + ", expected " +
        godot::String(expected_name) + " at " + path + ":" + godot::String::num_int64(line) + ":" +
        godot::String::num_int64(column));
    return false;
}

godot::Callable make_callable(godot::Object* owner, std::size_t required_arguments,
                              std::size_t maximum_arguments, CallableContinuation continuation) {
    return make_callable(owner, required_arguments, maximum_arguments, false,
                         std::move(continuation));
}

godot::Callable make_callable(godot::Object* owner, std::size_t required_arguments,
                              std::size_t positional_arguments, const bool is_vararg,
                              CallableContinuation continuation) {
    if (!continuation || required_arguments > positional_arguments) {
        godot::UtilityFunctions::push_error("GDPP: invalid lambda callable configuration");
        return {};
    }
    return godot::Callable{memnew(LambdaCallable(owner, required_arguments, positional_arguments,
                                                 is_vararg, std::move(continuation)))};
}

namespace {

void bind_variant_method_impl(const godot::StringName& class_name, const godot::MethodInfo& method,
                              const GDExtensionClassMethodCall call, const bool has_return_value,
                              const bool is_vararg) {
    if (class_name.is_empty() || method.name.is_empty() || !call) {
        godot::UtilityFunctions::push_error("GDPP: invalid Variant method registration");
        return;
    }

    godot::LocalVector<godot::PropertyInfo> properties;
    properties.reserve(method.arguments.size() + 1);
    properties.push_back(method.return_val);
    for (const auto& argument : method.arguments)
        properties.push_back(argument);

    godot::LocalVector<GDExtensionPropertyInfo> extension_properties;
    extension_properties.reserve(properties.size());
    for (const auto& property : properties) {
        extension_properties.push_back({
            static_cast<GDExtensionVariantType>(property.type),
            property.name._native_ptr(),
            property.class_name._native_ptr(),
            static_cast<std::uint32_t>(property.hint),
            property.hint_string._native_ptr(),
            property.usage,
        });
    }

    godot::LocalVector<GDExtensionClassMethodArgumentMetadata> metadata;
    metadata.resize(properties.size());
    metadata[0] = method.return_val_metadata;
    for (std::uint32_t index = 0; index < method.arguments.size(); ++index) {
        metadata[index + 1] = index < method.arguments_metadata.size()
                                  ? method.arguments_metadata[index]
                                  : GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;
    }

    godot::LocalVector<GDExtensionVariantPtr> defaults;
    defaults.reserve(method.default_arguments.size());
    for (const auto& value : method.default_arguments)
        defaults.push_back(const_cast<GDExtensionVariantPtr>(value._native_ptr()));

    auto flags = method.flags;
    if (is_vararg)
        flags |= GDEXTENSION_METHOD_FLAG_VARARG;
    const GDExtensionClassMethodInfo extension_method{
        method.name._native_ptr(),
        nullptr,
        call,
        nullptr,
        flags,
        static_cast<GDExtensionBool>(has_return_value),
        extension_properties.ptr(),
        metadata[0],
        static_cast<std::uint32_t>(method.arguments.size()),
        method.arguments.is_empty() ? nullptr : extension_properties.ptr() + 1,
        method.arguments.is_empty() ? nullptr : metadata.ptr() + 1,
        static_cast<std::uint32_t>(method.default_arguments.size()),
        defaults.is_empty() ? nullptr : defaults.ptr(),
    };
    godot::gdextension_interface::classdb_register_extension_class_method(
        godot::gdextension_interface::library, class_name._native_ptr(), &extension_method);
}

} // namespace

void bind_vararg_method(const godot::StringName& class_name, const godot::MethodInfo& method,
                        const GDExtensionClassMethodCall call, const bool has_return_value) {
    bind_variant_method_impl(class_name, method, call, has_return_value, true);
}

void bind_variant_method(const godot::StringName& class_name, const godot::MethodInfo& method,
                         const GDExtensionClassMethodCall call, const bool has_return_value) {
    bind_variant_method_impl(class_name, method, call, has_return_value, false);
}

godot::Variant call_dynamic_impl(godot::Variant& target, const godot::StringName& method,
                                 const godot::Variant** arguments, std::size_t argument_count,
                                 const ScriptSourceLocation location) {
    if (reject_invalid_object_target(target, "call method", &method, location))
        return {};
    static const godot::StringName get_script_method{"get_script"};
    static const godot::StringName new_method{"new"};
    static const godot::StringName set_script_method{"set_script"};
    if (argument_count == 0 && method == get_script_method &&
        target.get_type() == godot::Variant::OBJECT) {
        return script_identity(static_cast<godot::Object*>(target));
    }
    // `Script.new()` is a language-level constructor operation, not a ClassDB method. Calling
    // Variant::callp("new") works for GDScript only because that language intercepts its own
    // Script resource; a ScriptExtension otherwise reports an invalid dynamic call before
    // `_instance_create()` can run. Route every runtime-discovered AOT Script through the same
    // constructor used by statically resolved script resources, including varargs `_init`.
    if (method == new_method && target.get_type() == godot::Variant::OBJECT) {
        auto* script =
            godot::Object::cast_to<AttachedCompiledScript>(target.get_validated_object());
        if (script) {
            godot::Array constructor_arguments;
            constructor_arguments.resize(static_cast<std::int64_t>(argument_count));
            for (std::size_t index = 0; index < argument_count; ++index)
                constructor_arguments[static_cast<std::int64_t>(index)] = *arguments[index];
            godot::String error;
            auto result = instantiate_attached_script(script->get_source_path(),
                                                      constructor_arguments, &error);
            if (!error.is_empty())
                report_script_failure("GDPP: dynamic compiled Script construction failed: " + error,
                                      location);
            return result;
        }
    }
    // Export conversion has already replaced every scene-owned GDScript instance with its
    // generated native class. Some projects defensively call set_script(preload("type.gd")) on
    // duplicated scene prototypes. The original call is redundant after that conversion, and a
    // native GDExtension class cannot be attached through Object::set_script(). Preserve the
    // mutation semantics only when the target is already the requested native class; otherwise
    // report the unsupported dynamic attachment instead of silently accepting a broken object.
    if (argument_count == 1 && method == set_script_method &&
        target.get_type() == godot::Variant::OBJECT && arguments && arguments[0] &&
        arguments[0]->get_type() == godot::Variant::STRING_NAME) {
        auto* object = target.get_validated_object();
        const auto requested_class = static_cast<godot::StringName>(*arguments[0]);
        if (object && requested_class.begins_with("GDPPNative_") &&
            object->is_class(requested_class)) {
            return {};
        }
        if (requested_class.begins_with("GDPPNative_")) {
            report_script_failure(
                godot::String("GDPP: cannot attach native script class '") +
                    godot::String(requested_class) +
                    "' to an object that was not converted to that class during export",
                location);
            return {};
        }
    }
    godot::Variant result;
    GDExtensionCallError error{GDEXTENSION_CALL_OK, 0, 0};
    target.callp(method, arguments, static_cast<int>(argument_count), result, error);
    if (error.error != GDEXTENSION_CALL_OK) {
        report_invalid_member("call", method, location);
        return {};
    }
    return result;
}

godot::Variant get_named(const godot::Variant& target, const godot::StringName& name,
                         const ScriptSourceLocation location) {
    if (reject_invalid_object_target(target, "read property", &name, location))
        return {};
    // GDScript defines `dictionary.identifier` as keyed Dictionary access, including
    // dictionaries returned through Variant boundaries such as JSON.parse() and HTTP APIs.
    // Variant::get_named() only covers built-in/object properties and rejects Dictionary,
    // so route this case through Dictionary::get() explicitly.
    if (target.get_type() == godot::Variant::DICTIONARY) {
        const auto dictionary = static_cast<godot::Dictionary>(target);
        return dictionary.get(godot::Variant(name), godot::Variant{});
    }
    bool valid = false;
    auto result = target.get_named(name, valid);
    if (!valid) {
        report_invalid_member("property read", name, location);
        return {};
    }
    return result;
}

void set_named(godot::Variant& target, const godot::StringName& name, const godot::Variant& value,
               const ScriptSourceLocation location) {
    if (reject_invalid_object_target(target, "write property", &name, location))
        return;
    if (target.get_type() == godot::Variant::DICTIONARY) {
        auto dictionary = static_cast<godot::Dictionary>(target);
        dictionary[godot::Variant(name)] = value;
        return;
    }
    bool valid = false;
    target.set_named(name, value, valid);
    if (!valid)
        report_invalid_member("property write", name, location);
}

godot::Variant get_key(const godot::Variant& target, const godot::Variant& key,
                       const ScriptSourceLocation location) {
    if (reject_invalid_object_target(target, "read key", nullptr, location))
        return {};
    bool valid = false;
    auto result = target.get(key, &valid);
    if (!valid) {
        report_invalid_key("read", target, key, location);
        return {};
    }
    return result;
}

void set_key(godot::Variant& target, const godot::Variant& key, const godot::Variant& value,
             const ScriptSourceLocation location) {
    if (reject_invalid_object_target(target, "write key", nullptr, location))
        return;
    bool valid = false;
    target.set(key, value, &valid);
    if (!valid)
        report_invalid_key("write", target, key, location);
}

void report_index_out_of_bounds(const char* container, const char* operation,
                                const std::int64_t index, const std::int64_t size,
                                const char* source_path, const std::int64_t line,
                                const std::int64_t column) {
    const auto message = godot::String{"Out of bounds "} + operation + " index '" +
                         godot::String::num_int64(index) + "' (on base: '" + container +
                         "', size " + godot::String::num_int64(size) + ")";
    report_script_failure(message, source_path, line, column);
}

bool iter_init(const godot::Variant& iterable, godot::Variant& iterator,
               const ScriptSourceLocation location) {
    if (reject_invalid_object_target(iterable, "start iteration", nullptr, location))
        return false;
    bool valid = false;
    const bool available = iterable.iter_init(iterator, valid);
    if (!valid)
        report_script_failure(godot::String{"Value of type "} + describe_variant_type(iterable) +
                                  " is not iterable.",
                              location);
    return valid && available;
}

bool iter_next(const godot::Variant& iterable, godot::Variant& iterator,
               const ScriptSourceLocation location) {
    if (reject_invalid_object_target(iterable, "advance iteration", nullptr, location))
        return false;
    bool valid = false;
    const bool available = iterable.iter_next(iterator, valid);
    if (!valid)
        report_script_failure(godot::String{"Iterator advance failed for "} +
                                  describe_variant_type(iterable) + ".",
                              location);
    return valid && available;
}

godot::Variant iter_get(const godot::Variant& iterable, const godot::Variant& iterator,
                        const ScriptSourceLocation location) {
    if (reject_invalid_object_target(iterable, "read iterator value", nullptr, location))
        return {};
    bool valid = false;
    auto value = iterable.iter_get(iterator, valid);
    if (!valid) {
        report_script_failure(godot::String{"Iterator value read failed for "} +
                                  describe_variant_type(iterable) + ".",
                              location);
        return {};
    }
    return value;
}

} // namespace gdpp::runtime
