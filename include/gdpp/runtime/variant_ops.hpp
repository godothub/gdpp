#pragma once

#include "gdpp/runtime/reference_semantics.hpp"

#include <godot_cpp/classes/class_db_singleton.hpp>
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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace godot {
class Object;
}

namespace gdpp::runtime {

template <typename Target>
[[nodiscard]] Target explicit_variant_cast(const godot::Variant& value,
                                           const godot::Variant::Type target_type) {
    bool compatible = godot::Variant::can_convert(value.get_type(), target_type);
    if (target_type == godot::Variant::STRING) {
        compatible = value.get_type() == godot::Variant::STRING ||
                     value.get_type() == godot::Variant::STRING_NAME ||
                     value.get_type() == godot::Variant::NODE_PATH;
    }
    ERR_FAIL_COND_V_MSG(!compatible, Target{},
                        "GDScript explicit cast has no compatible runtime constructor.");
    return static_cast<Target>(value);
}

// godot-cpp's TypedArray/TypedDictionary converting constructors call assign(), which coerces
// elements. GDScript typed storage instead requires identical runtime container metadata. Route
// dynamic boundaries through the strict base-container assignment operators to preserve that
// contract.
template <typename TypedContainer>
[[nodiscard]] TypedContainer strict_typed_storage(const godot::Variant& value) {
    TypedContainer result;
    if constexpr (std::is_base_of_v<godot::Array, TypedContainer>) {
        ERR_FAIL_COND_V_MSG(value.get_type() != godot::Variant::ARRAY, result,
                            "Cannot assign a non-Array value to typed Array storage.");
        result = godot::Array(value);
    } else {
        static_assert(std::is_base_of_v<godot::Dictionary, TypedContainer>,
                      "strict_typed_storage requires a typed Godot container");
        ERR_FAIL_COND_V_MSG(value.get_type() != godot::Variant::DICTIONARY, result,
                            "Cannot assign a non-Dictionary value to typed Dictionary storage.");
        result = godot::Dictionary(value);
    }
    return result;
}

[[nodiscard]] godot::Variant binary(godot::Variant::Operator operation, const godot::Variant& left,
                                    const godot::Variant& right);
// Keep statically typed integer operands out of temporary Variant wrappers. These entry points are
// particularly important for mixed expressions such as `dictionary.value & 3` and
// `typed_total += callable.call()`, where only one side is genuinely dynamic.
[[nodiscard]] godot::Variant binary_integer(godot::Variant::Operator operation,
                                            const godot::Variant& left, std::int64_t right);
[[nodiscard]] godot::Variant binary_integer(godot::Variant::Operator operation, std::int64_t left,
                                            const godot::Variant& right);
void compound_assign(godot::Variant& target, godot::Variant::Operator operation,
                     const godot::Variant& value);
void compound_assign_integer(godot::Variant& target, godot::Variant::Operator operation,
                             std::int64_t value);
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
                                   const godot::Variant& operand);
[[nodiscard]] std::int64_t integer_divide(std::int64_t left, std::int64_t right);
[[nodiscard]] std::int64_t integer_modulo(std::int64_t left, std::int64_t right);
[[nodiscard]] bool is_instance_valid(const godot::Variant& value) noexcept;
[[nodiscard]] godot::Array make_range(std::int64_t stop);
[[nodiscard]] godot::Array make_range(std::int64_t start, std::int64_t stop);
[[nodiscard]] godot::Array make_range(std::int64_t start, std::int64_t stop, std::int64_t step);
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
[[nodiscard]] godot::Object* find_engine_singleton(const godot::StringName& name);
[[nodiscard]] godot::Variant script_identity(godot::Object* object);
[[nodiscard]] godot::Variant instantiate_external_class(const godot::StringName& name);

template <typename... Arguments>
[[nodiscard]] godot::Variant call_external_static(const godot::StringName& class_name,
                                                  const godot::StringName& method,
                                                  Arguments&&... arguments) {
    auto* class_db = godot::ClassDBSingleton::get_singleton();
    if (!class_db || !class_db->class_exists(class_name) ||
        !class_db->class_has_method(class_name, method)) {
        return {};
    }
    return class_db->class_call_static(class_name, method, std::forward<Arguments>(arguments)...);
}

[[nodiscard]] bool is_external_instance(const godot::Variant& value,
                                        const godot::StringName& class_name);
[[nodiscard]] godot::Callable external_callable(const godot::Variant& value,
                                                const godot::StringName& method);
[[nodiscard]] godot::Signal external_signal(const godot::Variant& value,
                                            const godot::StringName& signal);

using AwaitContinuation = std::function<void(const godot::Array&)>;
[[nodiscard]] bool await_signal(const godot::Variant& signal, godot::Object* owner,
                                AwaitContinuation continuation);
[[nodiscard]] godot::Variant await_result(const godot::Array& arguments);

class CoroutineState;
using CoroutineStatePtr = std::shared_ptr<CoroutineState>;
[[nodiscard]] CoroutineStatePtr begin_coroutine(godot::Object* owner);
[[nodiscard]] godot::Variant coroutine_result(const CoroutineStatePtr& state);
void complete_coroutine(const CoroutineStatePtr& state, const godot::Variant& result);

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
template <std::size_t Size> class LocalCallableArguments final {
  public:
    explicit LocalCallableArguments(std::array<godot::Variant, Size> values)
        : values_(std::move(values)) {}

    [[nodiscard]] std::int64_t size() const noexcept { return static_cast<std::int64_t>(Size); }
    [[nodiscard]] const godot::Variant& operator[](std::size_t index) const noexcept {
        return values_[index];
    }

  private:
    std::array<godot::Variant, Size> values_;
};

template <typename Callback> class LocalCallable final : public godot::Callable {
  public:
    LocalCallable(godot::Object* owner, std::size_t required_arguments,
                  std::size_t maximum_arguments, Callback callback)
        : godot::Callable(make_callable(owner, required_arguments, maximum_arguments,
                                        [bridge = callback](const godot::Array& arguments) mutable {
                                            return bridge(arguments);
                                        })),
          callback_(std::move(callback)) {}

    LocalCallable(godot::Object* owner, std::size_t required_arguments,
                  std::size_t positional_arguments, bool is_vararg, Callback callback)
        : godot::Callable(make_callable(owner, required_arguments, positional_arguments, is_vararg,
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
        LocalCallableArguments<sizeof...(Arguments)> values(
            std::array<godot::Variant, sizeof...(Arguments)>{arguments...});
        return callback_(values);
    }

  private:
    mutable Callback callback_;
    bool direct_{true};
};

template <typename Callback>
[[nodiscard]] auto make_local_callable(godot::Object* owner, std::size_t required_arguments,
                                       std::size_t maximum_arguments, Callback&& callback) {
    using StoredCallback = std::decay_t<Callback>;
    return LocalCallable<StoredCallback>(owner, required_arguments, maximum_arguments,
                                         std::forward<Callback>(callback));
}

template <typename Callback>
[[nodiscard]] auto make_local_callable(godot::Object* owner, std::size_t required_arguments,
                                       std::size_t positional_arguments, bool is_vararg,
                                       Callback&& callback) {
    using StoredCallback = std::decay_t<Callback>;
    return LocalCallable<StoredCallback>(owner, required_arguments, positional_arguments, is_vararg,
                                         std::forward<Callback>(callback));
}

[[nodiscard]] godot::Variant call_dynamic_impl(godot::Variant& target,
                                               const godot::StringName& method,
                                               const godot::Variant** arguments,
                                               std::size_t argument_count);

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

[[nodiscard]] godot::Variant get_named(const godot::Variant& target, const godot::StringName& name);
void set_named(godot::Variant& target, const godot::StringName& name, const godot::Variant& value);

[[nodiscard]] godot::Variant get_key(const godot::Variant& target, const godot::Variant& key);
void set_key(godot::Variant& target, const godot::Variant& key, const godot::Variant& value);
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

inline void checked_array_set(godot::Array& target, std::int64_t index,
                              const godot::Variant& value, const char* source_path,
                              std::int64_t line, std::int64_t column) {
    const auto size = target.size();
    const auto normalized = index < 0 ? index + size : index;
    if (normalized < 0 || normalized >= size) {
        report_index_out_of_bounds("Array", "write", index, size, source_path, line, column);
        return;
    }
    target[normalized] = value;
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

[[nodiscard]] bool iter_init(const godot::Variant& iterable, godot::Variant& iterator);
[[nodiscard]] bool iter_next(const godot::Variant& iterable, godot::Variant& iterator);
[[nodiscard]] godot::Variant iter_get(const godot::Variant& iterable,
                                      const godot::Variant& iterator);

} // namespace gdpp::runtime
