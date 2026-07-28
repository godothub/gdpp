#pragma once

#include "gdpp/runtime/reference_semantics.hpp"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/signal.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/typed_dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>

namespace godot {
class Object;
}

namespace gdpp::runtime {

class CoroutineState;
using CoroutineStatePtr = std::shared_ptr<CoroutineState>;
using AwaitContinuation = std::function<void(const godot::Array&)>;

// GDScript exposes a pending coroutine as a RefCounted function-state object whose `completed`
// signal is consumed by `await`. Returning a raw Signal is sufficient for a direct known await,
// but changes dynamic calls and engine virtual returns. Keep the public Variant shape equivalent
// while retaining the generated coroutine implementation behind an opaque shared state.
class CoroutineFunctionState final : public godot::RefCounted {
    GDCLASS(CoroutineFunctionState, godot::RefCounted)

  public:
    [[nodiscard]] bool is_valid(bool extended_check = false) const;
    godot::Variant resume(const godot::Variant& argument = {});
    [[nodiscard]] godot::String _to_string() const;

  protected:
    static void _bind_methods();

  private:
    friend bool await_signal(const godot::Variant&, godot::Object*, const CoroutineStatePtr&,
                             AwaitContinuation);
    friend void complete_coroutine(const CoroutineStatePtr&, const godot::Variant&);

    godot::Variant signal_callback(const godot::Variant** arguments, GDExtensionInt argument_count,
                                   GDExtensionCallError& error);
    void install(godot::Object* owner, const CoroutineStatePtr& coroutine,
                 AwaitContinuation continuation);
    void finish();
    void clear_incoming_connections();

    mutable std::mutex mutex_;
    godot::ObjectID owner_;
    std::weak_ptr<CoroutineState> coroutine_;
    AwaitContinuation continuation_;
};

struct ScriptFaultState final {
    // A fault state is only installed in the thread-local active invocation. Coroutine states
    // can migrate between threads, but CoroutineFunctionState serializes continuation ownership
    // before the state is installed on the resuming thread. Keeping the flag invocation-local
    // lets generated hot paths fold redundant success checks instead of issuing an atomic load
    // after every expression.
    bool failed{false};
};

namespace detail {
extern thread_local ScriptFaultState* active_script_fault;
} // namespace detail

struct ScriptSourceLocation final {
    const char* path{nullptr};
    std::int64_t line{0};
    std::int64_t column{0};
};

enum class ScriptFaultPolicy : std::uint8_t {
    isolated,
    inherit_existing,
};

// GDScript runtime failures terminate only the currently executing script function. A nested
// generated call therefore installs an independent frame, while every continuation of one
// coroutine re-enters the state owned by that coroutine. This explicit model avoids C++ exceptions
// (disabled by Godot) and never allows a failure marker to escape across a GDExtension ABI call.
class ScriptFunctionScope final {
  public:
    ScriptFunctionScope() noexcept : state_{&local_}, previous_{detail::active_script_fault} {
        detail::active_script_fault = state_;
    }
    explicit ScriptFunctionScope(const ScriptFaultPolicy policy) noexcept
        : state_{policy == ScriptFaultPolicy::inherit_existing && detail::active_script_fault
                     ? detail::active_script_fault
                     : &local_},
          previous_{detail::active_script_fault} {
        detail::active_script_fault = state_;
    }
    explicit ScriptFunctionScope(const CoroutineStatePtr& coroutine) noexcept;
    ~ScriptFunctionScope() { detail::active_script_fault = previous_; }

    ScriptFunctionScope(const ScriptFunctionScope&) = delete;
    ScriptFunctionScope& operator=(const ScriptFunctionScope&) = delete;

    [[nodiscard]] bool failed() const noexcept { return state_ && state_->failed; }

  private:
    ScriptFaultState local_;
    ScriptFaultState* state_{nullptr};
    ScriptFaultState* previous_{nullptr};
};

// Static fields, managed constants, and cached preloads form one-time script initialization
// transactions. The state below provides a thread-safe fast path after success, permits
// same-thread recursive reads during an active transaction, preserves a terminal failure instead
// of publishing partially initialized storage, and makes static unload explicitly resettable.
class ScriptInitializationState final {
  public:
    ScriptInitializationState() = default;

    ScriptInitializationState(const ScriptInitializationState&) = delete;
    ScriptInitializationState& operator=(const ScriptInitializationState&) = delete;

    [[nodiscard]] bool run(const char* failure_message, const std::function<void()>& initialize,
                           const std::function<void()>& rollback = {});
    void reset(const std::function<void()>& cleanup = {});

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool failed() const noexcept;

  private:
    enum class Phase : std::uint8_t {
        uninitialized,
        ready,
        failed,
    };

    std::atomic<Phase> phase_{Phase::uninitialized};
    std::mutex mutex_;
};

inline void mark_script_failure() noexcept {
    if (detail::active_script_fault)
        detail::active_script_fault->failed = true;
}

[[nodiscard]] inline bool script_function_failed() noexcept {
    return detail::active_script_fault && detail::active_script_fault->failed;
}
[[nodiscard]] godot::String describe_variant_type(const godot::Variant& value);
void report_script_failure(const godot::String& message, ScriptSourceLocation location = {});
void report_script_failure(const godot::String& message, const char* source_path, std::int64_t line,
                           std::int64_t column);

// GDScript's typed builtin assignment bytecode accepts an exact Variant type or one of Godot's
// strict implicit conversions. A raw godot-cpp Variant cast is intentionally more permissive and
// can silently turn invalid customer data into a default value, so every dynamic storage boundary
// must validate before materializing native C++ storage.
template <typename Target>
[[nodiscard]] Target strict_builtin_storage(const godot::Variant& value,
                                            const godot::Variant::Type target_type,
                                            const ScriptSourceLocation location = {}) {
    if (script_function_failed())
        return {};
    if (value.get_type() != target_type &&
        !godot::Variant::can_convert_strict(value.get_type(), target_type)) {
        report_script_failure(godot::String{"Cannot assign "} + describe_variant_type(value) +
                                  " to " + godot::Variant::get_type_name(target_type) + ".",
                              location);
        return {};
    }
    return static_cast<Target>(value);
}

[[nodiscard]] godot::Object* strict_native_object_storage(const godot::Variant& value,
                                                          const godot::StringName& expected_class,
                                                          ScriptSourceLocation location = {});

template <typename ObjectType>
[[nodiscard]] ObjectType* strict_native_pointer_storage(const godot::Variant& value,
                                                        const godot::StringName& expected_class,
                                                        const ScriptSourceLocation location = {}) {
    return godot::Object::cast_to<ObjectType>(
        strict_native_object_storage(value, expected_class, location));
}

template <typename ObjectType>
[[nodiscard]] ObjectStorage<ObjectType>
strict_native_object_value_storage(const godot::Variant& value,
                                   const godot::StringName& expected_class,
                                   const ScriptSourceLocation location = {}) {
    if (!strict_native_object_storage(value, expected_class, location))
        return {};
    return ObjectStorage<ObjectType>(value);
}

template <typename ObjectType>
[[nodiscard]] godot::Ref<ObjectType>
strict_native_ref_storage(const godot::Variant& value, const godot::StringName& expected_class,
                          const ScriptSourceLocation location = {}) {
    return godot::Ref<ObjectType>(godot::Object::cast_to<ObjectType>(
        strict_native_object_storage(value, expected_class, location)));
}

[[nodiscard]] godot::Variant strict_external_object_storage(const godot::Variant& value,
                                                            const godot::StringName& expected_class,
                                                            ScriptSourceLocation location = {});

template <typename PackedArray>
[[nodiscard]] SharedPackedArray<PackedArray>
strict_packed_array_storage(const godot::Variant& value, const ScriptSourceLocation location = {}) {
    if (script_function_failed())
        return {};
    const auto expected =
        static_cast<godot::Variant::Type>(godot::internal::VariantInternalType<PackedArray>::type);
    if (value.get_type() != expected &&
        !godot::Variant::can_convert_strict(value.get_type(), expected)) {
        report_script_failure(godot::String{"Cannot assign "} + describe_variant_type(value) +
                                  " to " + godot::Variant::get_type_name(expected) + ".",
                              location);
        return {};
    }
    if (value.get_type() == expected)
        return SharedPackedArray<PackedArray>(value);
    return SharedPackedArray<PackedArray>(static_cast<PackedArray>(value));
}

template <typename Target>
[[nodiscard]] Target explicit_variant_cast(const godot::Variant& value,
                                           const godot::Variant::Type target_type,
                                           const ScriptSourceLocation location = {}) {
    if (script_function_failed())
        return {};
    const auto source_type = value.get_type();
    if (source_type == target_type)
        return static_cast<Target>(value);
    bool compatible = godot::Variant::can_convert(source_type, target_type);
    if (target_type == godot::Variant::STRING) {
        compatible =
            source_type == godot::Variant::STRING_NAME || source_type == godot::Variant::NODE_PATH;
    }
    if (!compatible) {
        report_script_failure(godot::String{"Cannot convert "} + describe_variant_type(value) +
                                  " to " + godot::Variant::get_type_name(target_type) + ".",
                              location);
        return {};
    }
    return static_cast<Target>(value);
}

// godot-cpp's TypedArray/TypedDictionary converting constructors call assign(), which coerces
// elements. GDScript typed storage instead requires identical runtime container metadata. Route
// dynamic boundaries through the strict base-container assignment operators to preserve that
// contract.
template <typename TypedContainer>
[[nodiscard]] TypedContainer strict_typed_storage(const godot::Variant& value,
                                                  const ScriptSourceLocation location = {}) {
    TypedContainer result;
    if (script_function_failed())
        return result;
    if constexpr (std::is_base_of_v<godot::Array, TypedContainer>) {
        if (value.get_type() != godot::Variant::ARRAY) {
            report_script_failure(godot::String{"Cannot assign "} + describe_variant_type(value) +
                                      " to " + describe_variant_type(godot::Variant(result)) + ".",
                                  location);
            return result;
        }
        const auto source = godot::Array(value);
        if (!result.is_same_typed(source)) {
            report_script_failure(godot::String{"Cannot assign "} + describe_variant_type(value) +
                                      " to " + describe_variant_type(godot::Variant(result)) + ".",
                                  location);
            return result;
        }
        result = source;
    } else {
        static_assert(std::is_base_of_v<godot::Dictionary, TypedContainer>,
                      "strict_typed_storage requires a typed Godot container");
        if (value.get_type() != godot::Variant::DICTIONARY) {
            report_script_failure(godot::String{"Cannot assign "} + describe_variant_type(value) +
                                      " to " + describe_variant_type(godot::Variant(result)) + ".",
                                  location);
            return result;
        }
        const auto source = godot::Dictionary(value);
        if (!result.is_same_typed(source)) {
            report_script_failure(godot::String{"Cannot assign "} + describe_variant_type(value) +
                                      " to " + describe_variant_type(godot::Variant(result)) + ".",
                                  location);
            return result;
        }
        result = source;
    }
    return result;
}

[[nodiscard]] godot::Variant binary(godot::Variant::Operator operation, const godot::Variant& left,
                                    const godot::Variant& right,
                                    ScriptSourceLocation location = {});
// Keep statically typed integer operands out of temporary Variant wrappers. These entry points are
// particularly important for mixed expressions such as `dictionary.value & 3` and
// `typed_total += callable.call()`, where only one side is genuinely dynamic.
[[nodiscard]] godot::Variant binary_integer(godot::Variant::Operator operation,
                                            const godot::Variant& left, std::int64_t right,
                                            ScriptSourceLocation location = {});
[[nodiscard]] godot::Variant binary_integer(godot::Variant::Operator operation, std::int64_t left,
                                            const godot::Variant& right,
                                            ScriptSourceLocation location = {});
void compound_assign(godot::Variant& target, godot::Variant::Operator operation,
                     const godot::Variant& value, ScriptSourceLocation location = {});
void compound_assign_integer(godot::Variant& target, godot::Variant::Operator operation,
                             std::int64_t value, ScriptSourceLocation location = {});

// Local script signals already have a statically known owner and name. The generated
// godot-cpp Object::emit_signal wrapper reconstructs both the immutable signal-name Variant and
// every argument Variant on each emission. Keep the public MethodBind behavior, but allow
// generated code to cache the name per call site and construct each changing argument exactly
// once. Arbitrary external Signal values intentionally remain on godot::Signal::emit().
void emit_local_signal_variants(godot::Object* owner, const godot::Variant** arguments,
                                std::int64_t argument_count, ScriptSourceLocation location = {});

template <typename... Arguments>
void emit_local_signal(godot::Object* owner, const godot::Variant& signal_name,
                       Arguments&&... arguments) {
    std::array<godot::Variant, sizeof...(Arguments)> values{
        {to_variant(std::forward<Arguments>(arguments))...}};
    std::array<const godot::Variant*, 1 + sizeof...(Arguments)> pointers{};
    pointers[0] = &signal_name;
    for (std::size_t index = 0; index < values.size(); ++index)
        pointers[index + 1] = &values[index];
    emit_local_signal_variants(owner, pointers.data(), static_cast<std::int64_t>(pointers.size()));
}

template <typename... Arguments>
void emit_local_signal_at(const ScriptSourceLocation location, godot::Object* owner,
                          const godot::Variant& signal_name, Arguments&&... arguments) {
    std::array<godot::Variant, sizeof...(Arguments)> values{
        {to_variant(std::forward<Arguments>(arguments))...}};
    std::array<const godot::Variant*, 1 + sizeof...(Arguments)> pointers{};
    pointers[0] = &signal_name;
    for (std::size_t index = 0; index < values.size(); ++index)
        pointers[index + 1] = &values[index];
    emit_local_signal_variants(owner, pointers.data(), static_cast<std::int64_t>(pointers.size()),
                               location);
}
// godot-cpp 4.x generated built-in copy operators do not guard exact self-assignment. Dictionary
// additionally cannot be move-assigned safely because its move operator constructs over the live
// opaque handle without destroying it. Generated reference-backed storage writes use these
// helpers so self-assignment is a no-op and every Dictionary-derived target uses the copy ABI.
void assign_dictionary(godot::Dictionary& target, const godot::Dictionary& value);

template <typename Target, typename Value,
          std::enable_if_t<std::is_base_of_v<godot::Dictionary, Target> &&
                               std::is_base_of_v<godot::Dictionary,
                                                 std::remove_cv_t<std::remove_reference_t<Value>>>,
                           int> = 0>
void assign_native_storage(Target& target, Value&& value) {
    if constexpr (std::is_same_v<Target, std::remove_cv_t<std::remove_reference_t<Value>>>) {
        if (std::addressof(target) == std::addressof(value))
            return;
    }
    if constexpr (std::is_same_v<Target, godot::Dictionary>) {
        assign_dictionary(target, static_cast<const godot::Dictionary&>(value));
    } else {
        target.operator=(static_cast<const godot::Dictionary&>(value));
    }
}

template <typename Target, typename Value,
          std::enable_if_t<!(std::is_base_of_v<godot::Dictionary, Target> &&
                             std::is_base_of_v<godot::Dictionary,
                                               std::remove_cv_t<std::remove_reference_t<Value>>>),
                           int> = 0>
void assign_native_storage(Target& target, Value&& value) {
    if constexpr (std::is_same_v<Target, std::remove_cv_t<std::remove_reference_t<Value>>>) {
        if (std::addressof(target) == std::addressof(value))
            return;
    }
    target = std::forward<Value>(value);
}
[[nodiscard]] godot::Variant unary(godot::Variant::Operator operation,
                                   const godot::Variant& operand,
                                   ScriptSourceLocation location = {});
[[nodiscard]] std::int64_t integer_divide(std::int64_t left, std::int64_t right,
                                          ScriptSourceLocation location = {});
[[nodiscard]] std::int64_t integer_modulo(std::int64_t left, std::int64_t right,
                                          ScriptSourceLocation location = {});
[[nodiscard]] bool is_instance_valid(const godot::Variant& value) noexcept;
void free_object_at(const godot::Variant& value, ScriptSourceLocation location = {});
[[nodiscard]] godot::Array make_range(std::int64_t stop);
[[nodiscard]] godot::Array make_range(std::int64_t start, std::int64_t stop);
[[nodiscard]] godot::Array make_range(std::int64_t start, std::int64_t stop, std::int64_t step);
template <typename... Arguments>
[[nodiscard]] godot::Array make_range_checked(Arguments&&... arguments) {
    constexpr auto argument_count = sizeof...(Arguments);
    if constexpr (argument_count == 1) {
        auto values = std::forward_as_tuple(std::forward<Arguments>(arguments)...);
        return make_range(static_cast<std::int64_t>(std::get<0>(values)));
    } else if constexpr (argument_count == 2) {
        auto values = std::forward_as_tuple(std::forward<Arguments>(arguments)...);
        return make_range(static_cast<std::int64_t>(std::get<0>(values)),
                          static_cast<std::int64_t>(std::get<1>(values)));
    } else if constexpr (argument_count == 3) {
        auto values = std::forward_as_tuple(std::forward<Arguments>(arguments)...);
        return make_range(static_cast<std::int64_t>(std::get<0>(values)),
                          static_cast<std::int64_t>(std::get<1>(values)),
                          static_cast<std::int64_t>(std::get<2>(values)));
    } else {
        report_script_failure(
            godot::String{"Error calling GDScript utility function \"range()\": Expected 1 to 3 "
                          "arguments, got "} +
            godot::String::num_int64(static_cast<std::int64_t>(argument_count)) + ".");
        return {};
    }
}
[[nodiscard]] std::int64_t length(const godot::Variant& value);
[[nodiscard]] godot::Array get_stack();
[[nodiscard]] godot::Variant convert_value(const godot::Variant& value, std::int64_t type);
[[nodiscard]] bool type_exists(const godot::Variant& name);
[[nodiscard]] godot::String character(std::int64_t code);
[[nodiscard]] std::int64_t ordinal(const godot::Variant& character);
[[nodiscard]] godot::Color color8(std::int64_t red, std::int64_t green, std::int64_t blue,
                                  std::int64_t alpha = 255);
[[nodiscard]] bool is_instance_of(const godot::Variant& value,
                                  const godot::Variant& type_descriptor);
[[nodiscard]] godot::Variant load_resource(const godot::String& path);
[[nodiscard]] bool is_editor_hint() noexcept;
void register_autoload(const godot::StringName& name, godot::Object* instance);
[[nodiscard]] godot::Object* find_autoload(const godot::StringName& name);
[[nodiscard]] godot::Object* find_engine_singleton_at(const godot::StringName& name,
                                                      ScriptSourceLocation location = {});
[[nodiscard]] godot::Variant script_identity(godot::Object* object);
[[nodiscard]] godot::Variant instantiate_external_class_at(const godot::StringName& name,
                                                           ScriptSourceLocation location = {});

[[nodiscard]] godot::Variant call_external_static_impl(const godot::StringName& class_name,
                                                       const godot::StringName& method,
                                                       const godot::Variant** arguments,
                                                       std::size_t argument_count,
                                                       ScriptSourceLocation location = {});

template <typename... Arguments>
[[nodiscard]] godot::Variant
call_external_static_at(const ScriptSourceLocation location, const godot::StringName& class_name,
                        const godot::StringName& method, Arguments&&... arguments) {
    std::array<godot::Variant, 2 + sizeof...(Arguments)> values{
        godot::Variant{class_name}, godot::Variant{method},
        to_variant(std::forward<Arguments>(arguments))...};
    std::array<const godot::Variant*, 2 + sizeof...(Arguments)> pointers{};
    for (std::size_t index = 0; index < values.size(); ++index)
        pointers[index] = &values[index];
    return call_external_static_impl(class_name, method, pointers.data(), pointers.size(),
                                     location);
}

[[nodiscard]] bool is_external_instance(const godot::Variant& value,
                                        const godot::StringName& class_name);
[[nodiscard]] godot::Callable external_callable_at(const godot::Variant& value,
                                                   const godot::StringName& method,
                                                   ScriptSourceLocation location = {});
[[nodiscard]] godot::Signal external_signal_at(const godot::Variant& value,
                                               const godot::StringName& signal,
                                               ScriptSourceLocation location = {});

[[nodiscard]] bool await_signal(const godot::Variant& signal, godot::Object* owner,
                                const CoroutineStatePtr& coroutine, AwaitContinuation continuation);
[[nodiscard]] bool is_awaitable(const godot::Variant& value);
[[nodiscard]] godot::Variant await_result(const godot::Array& arguments);

[[nodiscard]] CoroutineStatePtr begin_coroutine(godot::Object* owner);
[[nodiscard]] godot::Object* coroutine_owner(const CoroutineStatePtr& state);
[[nodiscard]] godot::Variant coroutine_result(const CoroutineStatePtr& state);
void complete_coroutine(const CoroutineStatePtr& state, const godot::Variant& result);
[[nodiscard]] bool
validate_virtual_return(const godot::Variant& value, godot::Variant::Type expected_type,
                        const godot::StringName& method, const godot::StringName& expected_name,
                        const godot::StringName& expected_class, const char* source_path,
                        std::int64_t line, std::int64_t column);

// Native method bindings cannot represent a GDScript default expression that depends on
// the receiving instance.  A private marker preserves the distinction between an omitted
// argument and an explicitly supplied null value until the generated method body evaluates
// the original expression.
[[nodiscard]] godot::Variant default_argument();
[[nodiscard]] bool is_default_argument(const godot::Variant& value);

using CallableContinuation = std::function<godot::Variant(const godot::Array&)>;
[[nodiscard]] godot::Callable make_callable(godot::Object* owner, std::size_t required_arguments,
                                            std::size_t maximum_arguments,
                                            CallableContinuation continuation);

[[nodiscard]] godot::Variant call_callable_impl(const godot::Callable& callable,
                                                const godot::Variant** arguments,
                                                std::size_t argument_count,
                                                ScriptSourceLocation location = {});

template <typename... Arguments>
[[nodiscard]] godot::Variant call_callable_at(const ScriptSourceLocation location,
                                              const godot::Callable& callable,
                                              Arguments&&... arguments) {
    std::array<godot::Variant, sizeof...(Arguments)> values{
        {to_variant(std::forward<Arguments>(arguments))...}};
    std::array<const godot::Variant*, sizeof...(Arguments)> pointers{};
    for (std::size_t index = 0; index < values.size(); ++index)
        pointers[index] = &values[index];
    return call_callable_impl(callable, pointers.data(), pointers.size(), location);
}
[[nodiscard]] godot::Callable make_callable(godot::Object* owner, std::size_t required_arguments,
                                            std::size_t positional_arguments, bool is_vararg,
                                            CallableContinuation continuation);

// godot-cpp exposes instance-only vararg method binding. Generated scripts also require static
// variadic methods and one ABI for attached ScriptExtension behavior classes, so register the raw
// GDExtension call thunk while retaining full MethodInfo reflection and default arguments.
void bind_vararg_method(const godot::StringName& class_name, const godot::MethodInfo& method,
                        GDExtensionClassMethodCall call, bool has_return_value);
void bind_variant_method(const godot::StringName& class_name, const godot::MethodInfo& method,
                         GDExtensionClassMethodCall call, bool has_return_value);

// Local lambdas remain ordinary Godot Callables when they escape, but a call made while the
// generated C++ retains the concrete adapter type can invoke the closure directly. This removes
// CallableCustom dispatch and heap-backed argument packing from typed hot paths without changing
// first-class Callable behavior.
template <typename... Values> class LocalCallableArguments final {
  public:
    explicit LocalCallableArguments(Values... values) : values_(std::move(values)...) {}

    [[nodiscard]] constexpr std::int64_t size() const noexcept {
        return static_cast<std::int64_t>(sizeof...(Values));
    }

    template <std::size_t Index> [[nodiscard]] decltype(auto) get() const noexcept {
        return std::get<Index>(values_);
    }

    [[nodiscard]] godot::Variant operator[](const std::size_t index) const {
        return variant_at(index, std::index_sequence_for<Values...>{});
    }

  private:
    template <std::size_t... Indices>
    [[nodiscard]] godot::Variant variant_at(const std::size_t index,
                                            std::index_sequence<Indices...>) const {
        godot::Variant result;
        const bool found =
            (false || ... ||
             (index == Indices ? (result = to_variant(std::get<Indices>(values_)), true) : false));
        static_cast<void>(found);
        return result;
    }

    std::tuple<Values...> values_;
};

template <std::size_t Index, typename... Values>
[[nodiscard]] godot::Variant
local_callable_argument(const LocalCallableArguments<Values...>& arguments) {
    if constexpr (Index < sizeof...(Values))
        return to_variant(arguments.template get<Index>());
    return {};
}

template <std::size_t Index>
[[nodiscard]] godot::Variant local_callable_argument(const godot::Array& arguments) {
    return Index < static_cast<std::size_t>(arguments.size()) ? arguments[Index] : godot::Variant{};
}

// A direct local invocation retains the native argument types at the call site. Exact typed
// parameters can therefore cross the lambda ABI without a Variant round trip; erased/escaped
// Callables and every non-exact conversion still use the same strict Godot conversion boundary.
template <typename Target, std::size_t Index, typename... Values>
[[nodiscard]] Target
local_callable_typed_argument(const LocalCallableArguments<Values...>& arguments,
                              const godot::Variant::Type target_type,
                              const ScriptSourceLocation location = {}) {
    if constexpr (Index >= sizeof...(Values)) {
        return {};
    } else {
        using Source =
            std::remove_cv_t<std::remove_reference_t<decltype(arguments.template get<Index>())>>;
        if constexpr (std::is_same_v<Target, Source>) {
            return arguments.template get<Index>();
        } else {
            return strict_builtin_storage<Target>(to_variant(arguments.template get<Index>()),
                                                  target_type, location);
        }
    }
}

template <typename Target, std::size_t Index>
[[nodiscard]] Target local_callable_typed_argument(const godot::Array& arguments,
                                                   const godot::Variant::Type target_type,
                                                   const ScriptSourceLocation location = {}) {
    const auto value =
        Index < static_cast<std::size_t>(arguments.size()) ? arguments[Index] : godot::Variant{};
    return strict_builtin_storage<Target>(value, target_type, location);
}

template <typename Callback, std::size_t RequiredArguments, std::size_t PositionalArguments,
          bool IsVararg>
class LocalCallable final : public godot::Callable {
  public:
    LocalCallable(godot::Object* owner, Callback callback)
        : godot::Callable(make_callable(owner, RequiredArguments, PositionalArguments, IsVararg,
                                        [bridge = callback](const godot::Array& arguments) mutable {
                                            return bridge(arguments);
                                        })),
          callback_(std::move(callback)) {}

    LocalCallable(const LocalCallable& other)
        : godot::Callable(other), callback_(other.callback_), direct_(other.direct_) {}
    LocalCallable(LocalCallable&& other) noexcept
        : godot::Callable(std::move(other)), callback_(std::move(other.callback_)),
          direct_(other.direct_) {}

    LocalCallable& operator=(const LocalCallable& other) {
        if (this == &other)
            return *this;
        godot::Callable::operator=(other);
        // C++17 closure types are not generally assignable. The copied Godot Callable already
        // owns the correct continuation, so assigned adapters conservatively use that ABI path.
        direct_ = false;
        return *this;
    }
    LocalCallable& operator=(LocalCallable&& other) noexcept {
        if (this == &other)
            return *this;
        godot::Callable::operator=(std::move(other));
        direct_ = false;
        return *this;
    }
    LocalCallable& operator=(const godot::Callable& other) {
        if (static_cast<const godot::Callable*>(this) == &other)
            return *this;
        godot::Callable::operator=(other);
        direct_ = false;
        return *this;
    }
    LocalCallable& operator=(godot::Callable&& other) noexcept {
        if (static_cast<godot::Callable*>(this) == &other)
            return *this;
        godot::Callable::operator=(std::move(other));
        direct_ = false;
        return *this;
    }

    template <typename... Arguments>
    [[nodiscard]] godot::Variant call(const Arguments&... arguments) const {
        if (!direct_)
            return godot::Callable::call(arguments...);
        LocalCallableArguments<std::decay_t<Arguments>...> values(arguments...);
        return callback_(values);
    }

    template <typename... Arguments>
    [[nodiscard]] godot::Variant call_checked(const ScriptSourceLocation location,
                                              const Arguments&... arguments) const {
        if (!direct_)
            return call_callable_at(location, static_cast<const godot::Callable&>(*this),
                                    arguments...);
        // The concrete adapter can only remain direct inside the generated invocation that
        // created it. Its owner is therefore either null (a static lambda) or the currently
        // executing, engine-locked script object. Escaped and assigned values deliberately lose
        // the direct flag and take call_callable_at(), which performs the full ObjectDB validity
        // check before dispatch. Repeating that global lookup for every local lambda iteration is
        // both redundant and materially slower than GDScript's local callable path.
        constexpr auto argument_count = sizeof...(Arguments);
        if constexpr (argument_count < RequiredArguments ||
                      (!IsVararg && argument_count > PositionalArguments)) {
            report_script_failure(
                godot::String{"Invalid Callable argument count: received "} +
                    godot::String::num_int64(static_cast<std::int64_t>(argument_count)) +
                    ", expected " +
                    godot::String::num_int64(static_cast<std::int64_t>(RequiredArguments)) +
                    (RequiredArguments == PositionalArguments
                         ? godot::String{}
                         : godot::String{" to "} +
                               godot::String::num_int64(
                                   static_cast<std::int64_t>(PositionalArguments))) +
                    ".",
                location);
            return {};
        }
        LocalCallableArguments<std::decay_t<Arguments>...> values(arguments...);
        return callback_(values);
    }

  private:
    mutable Callback callback_;
    bool direct_{true};
};

template <typename Callback, std::size_t RequiredArguments, std::size_t PositionalArguments,
          bool IsVararg, typename... Arguments>
[[nodiscard]] godot::Variant call_callable_at(
    const ScriptSourceLocation location,
    const LocalCallable<Callback, RequiredArguments, PositionalArguments, IsVararg>& callable,
    Arguments&&... arguments) {
    return callable.call_checked(location, std::forward<Arguments>(arguments)...);
}

template <std::size_t RequiredArguments, std::size_t PositionalArguments, bool IsVararg = false,
          typename Callback>
[[nodiscard]] auto make_local_callable(godot::Object* owner, Callback&& callback) {
    using StoredCallback = std::decay_t<Callback>;
    return LocalCallable<StoredCallback, RequiredArguments, PositionalArguments, IsVararg>(
        owner, std::forward<Callback>(callback));
}

[[nodiscard]] godot::Variant call_dynamic_impl(godot::Variant& target,
                                               const godot::StringName& method,
                                               const godot::Variant** arguments,
                                               std::size_t argument_count,
                                               ScriptSourceLocation location = {});

template <typename... Arguments>
[[nodiscard]] godot::Variant call_dynamic(godot::Variant& target, const godot::StringName& method,
                                          Arguments&&... arguments) {
    std::array<godot::Variant, sizeof...(Arguments)> values{
        godot::Variant(std::forward<Arguments>(arguments))...};
    std::array<const godot::Variant*, sizeof...(Arguments)> pointers{};
    for (std::size_t index = 0; index < values.size(); ++index)
        pointers[index] = &values[index];
    return call_dynamic_impl(target, method, pointers.data(), pointers.size());
}

template <typename... Arguments>
[[nodiscard]] godot::Variant
call_dynamic_at(const ScriptSourceLocation location, godot::Variant& target,
                const godot::StringName& method, Arguments&&... arguments) {
    std::array<godot::Variant, sizeof...(Arguments)> values{
        godot::Variant(std::forward<Arguments>(arguments))...};
    std::array<const godot::Variant*, sizeof...(Arguments)> pointers{};
    for (std::size_t index = 0; index < values.size(); ++index)
        pointers[index] = &values[index];
    return call_dynamic_impl(target, method, pointers.data(), pointers.size(), location);
}

[[nodiscard]] godot::Variant get_named(const godot::Variant& target, const godot::StringName& name,
                                       ScriptSourceLocation location = {});
void set_named(godot::Variant& target, const godot::StringName& name, const godot::Variant& value,
               ScriptSourceLocation location = {});

[[nodiscard]] godot::Variant get_key(const godot::Variant& target, const godot::Variant& key,
                                     ScriptSourceLocation location = {});
void set_key(godot::Variant& target, const godot::Variant& key, const godot::Variant& value,
             ScriptSourceLocation location = {});
void report_index_out_of_bounds(const char* container, const char* operation, std::int64_t index,
                                std::int64_t size, const char* source_path, std::int64_t line,
                                std::int64_t column);

[[nodiscard]] inline godot::Variant checked_array_get(const godot::Array& target,
                                                      std::int64_t index, const char* source_path,
                                                      std::int64_t line, std::int64_t column) {
    const auto size = target.size();
    const auto normalized = index < 0 ? index + size : index;
    if (normalized < 0 || normalized >= size) {
        report_index_out_of_bounds("Array", "read", index, size, source_path, line, column);
        return {};
    }
    return target[normalized];
}

template <typename Value>
[[nodiscard]] Value checked_typed_array_get(const godot::Array& target, std::int64_t index,
                                            const char* source_path, std::int64_t line,
                                            std::int64_t column) {
    const auto size = target.size();
    const auto normalized = index < 0 ? index + size : index;
    if (normalized < 0 || normalized >= size) {
        report_index_out_of_bounds("Array", "read", index, size, source_path, line, column);
        return {};
    }
    return godot::VariantCaster<Value>::cast(target[normalized]);
}

inline void checked_array_set(godot::Array& target, std::int64_t index, const godot::Variant& value,
                              const char* source_path, std::int64_t line, std::int64_t column) {
    const auto size = target.size();
    const auto normalized = index < 0 ? index + size : index;
    if (normalized < 0 || normalized >= size) {
        report_index_out_of_bounds("Array", "write", index, size, source_path, line, column);
        return;
    }
    target[normalized] = value;
}

[[nodiscard]] inline godot::Variant
checked_dictionary_get(const godot::Dictionary& target, const godot::Variant& key,
                       const ScriptSourceLocation location = {}) {
    // Variant's keyed ABI exposes the authoritative Dictionary lookup validity bit. It therefore
    // distinguishes a stored Nil from a missing or typed-invalid key in one lookup, without the
    // built-in-method dispatch overhead of Dictionary::get() or a second has() probe.
    bool valid = false;
    const auto value = godot::Variant(target).get_keyed(key, valid);
    if (!valid) {
        report_script_failure(godot::String{"Invalid access to property or key "} +
                                  key.stringify() + " on " +
                                  describe_variant_type(godot::Variant(target)) + ".",
                              location);
        return {};
    }
    return value;
}

[[nodiscard]] inline godot::Variant
checked_dictionary_get_named(const godot::Dictionary& target, const godot::StringName& key,
                             const ScriptSourceLocation location = {}) {
    // Named Dictionary access has a dedicated ABI path which accepts StringName directly and
    // reports lookup validity. Keep this separate from the general keyed path so `record.name`
    // neither allocates a temporary key Variant nor passes through Dictionary's method binder.
    bool valid = false;
    const auto value = godot::Variant(target).get_named(key, valid);
    if (!valid) {
        report_script_failure(godot::String{"Invalid access to property or key "} +
                                  godot::String(key) + " on " +
                                  describe_variant_type(godot::Variant(target)) + ".",
                              location);
        return {};
    }
    return value;
}

inline void checked_dictionary_set(godot::Dictionary& target, const godot::Variant& key,
                                   const godot::Variant& value,
                                   const ScriptSourceLocation location = {}) {
    if (script_function_failed())
        return;
    if (!target.set(key, value)) {
        report_script_failure(godot::String{"Invalid assignment of property or key "} +
                                  key.stringify() + " with " + describe_variant_type(value) +
                                  " on " + describe_variant_type(godot::Variant(target)) + ".",
                              location);
    }
}

inline void checked_dictionary_set_named(godot::Dictionary& target, const godot::StringName& key,
                                         const godot::Variant& value,
                                         const ScriptSourceLocation location = {}) {
    bool valid = false;
    auto target_variant = godot::Variant(target);
    target_variant.set_named(key, value, valid);
    if (!valid) {
        report_script_failure(godot::String{"Invalid assignment of property or key "} +
                                  godot::String(key) + " with " + describe_variant_type(value) +
                                  " on " + describe_variant_type(target_variant) + ".",
                              location);
    }
}

inline void unchecked_dictionary_set(godot::Dictionary& target, const godot::Variant& key,
                                     const godot::Variant& value,
                                     const ScriptSourceLocation location = {}) {
    if (script_function_failed())
        return;
    if (!target.set(key, value) && target.is_read_only()) {
        report_script_failure("Invalid assignment on read-only Dictionary.", location);
    }
}

inline void unchecked_dictionary_set_named(godot::Dictionary& target, const godot::StringName& key,
                                           const godot::Variant& value,
                                           const ScriptSourceLocation location = {}) {
    if (script_function_failed())
        return;
    bool valid = false;
    auto target_variant = godot::Variant(target);
    target_variant.set_named(key, value, valid);
    // GDScript deliberately continues after a typed compound value fails validation, but a
    // read-only Dictionary is a fatal assignment boundary. Query read-only state only on the
    // failed cold path so valid compound writes retain one getter and one setter lookup.
    if (!valid && target.is_read_only()) {
        report_script_failure("Invalid assignment on read-only Dictionary.", location);
    }
}

template <typename PackedArray>
using PackedArrayElement =
    std::remove_cv_t<std::remove_reference_t<decltype(std::declval<const PackedArray&>()[0])>>;

// A statically typed PackedArray subscript already has an authoritative native element type.
// Returning that element directly keeps the bounds check while avoiding a Variant allocation and
// conversion on every read in generated hot loops.
template <typename PackedArray>
[[nodiscard]] PackedArrayElement<PackedArray>
checked_packed_array_get(const SharedPackedArray<PackedArray>& target, std::int64_t index,
                         const char* source_path, std::int64_t line, std::int64_t column) {
    const auto size = target.native().size();
    const auto normalized = index < 0 ? index + size : index;
    if (normalized < 0 || normalized >= size) {
        report_index_out_of_bounds("PackedArray", "read", index, size, source_path, line, column);
        return {};
    }
    return target.native()[normalized];
}

template <typename PackedArray, typename Value>
void checked_packed_array_set(SharedPackedArray<PackedArray>& target, std::int64_t index,
                              Value&& value, const char* source_path, std::int64_t line,
                              std::int64_t column) {
    const auto size = target.native().size();
    const auto normalized = index < 0 ? index + size : index;
    if (normalized < 0 || normalized >= size) {
        report_index_out_of_bounds("PackedArray", "write", index, size, source_path, line, column);
        return;
    }
    target.native()[normalized] = std::forward<Value>(value);
}

[[nodiscard]] bool iter_init(const godot::Variant& iterable, godot::Variant& iterator,
                             ScriptSourceLocation location = {});
[[nodiscard]] bool iter_next(const godot::Variant& iterable, godot::Variant& iterator,
                             ScriptSourceLocation location = {});
[[nodiscard]] godot::Variant iter_get(const godot::Variant& iterable,
                                      const godot::Variant& iterator,
                                      ScriptSourceLocation location = {});

} // namespace gdpp::runtime
