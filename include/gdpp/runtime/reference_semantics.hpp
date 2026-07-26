#pragma once

#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/variant_internal.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace gdpp::runtime {

// godot-cpp's generated PackedArray C++ copy constructors implement the explicit
// `PackedArray(other)` copy operation. GDScript assignment and argument passing use a different
// contract: variables receive another reference to the same packed storage until duplicate() or a
// rebinding operation creates a new value. Store the value as a Variant because Variant copying is
// the engine ABI operation that retains that shared identity.
template <typename PackedArray> class SharedPackedArray final {
  public:
    SharedPackedArray() : value_(PackedArray{}) {}
    SharedPackedArray(const PackedArray& value) : value_(value) {}
    SharedPackedArray(PackedArray&& value) : value_(value) {}
    explicit SharedPackedArray(const godot::Array& value) : value_(PackedArray(value)) {}

    explicit SharedPackedArray(const godot::Variant& value) {
        if (value.get_type() ==
            static_cast<godot::Variant::Type>(
                godot::internal::VariantInternalType<PackedArray>::type)) {
            value_ = value;
            return;
        }
        ERR_PRINT("GDPP: shared packed-array storage received an incompatible Variant.");
        value_ = PackedArray{};
    }

    SharedPackedArray(const SharedPackedArray&) = default;
    SharedPackedArray(SharedPackedArray&&) noexcept = default;
    SharedPackedArray& operator=(const SharedPackedArray&) = default;
    SharedPackedArray& operator=(SharedPackedArray&&) noexcept = default;

    SharedPackedArray& operator=(const PackedArray& value) {
        value_ = godot::Variant(value);
        return *this;
    }

    SharedPackedArray& operator=(PackedArray&& value) {
        value_ = godot::Variant(value);
        return *this;
    }

    [[nodiscard]] PackedArray& native() noexcept {
        return *godot::VariantInternal::get_internal_value<PackedArray>(&value_);
    }

    [[nodiscard]] const PackedArray& native() const noexcept {
        return *godot::VariantInternal::get_internal_value<PackedArray>(&value_);
    }

    [[nodiscard]] godot::Variant& variant() noexcept { return value_; }
    [[nodiscard]] const godot::Variant& variant() const noexcept { return value_; }

    // Native PackedArray views are deliberately explicit. Keeping both this view and the Variant
    // view implicit makes `Variant(shared)` ambiguous on MSVC because both converting
    // constructors are equally viable. Generated Godot API calls use packed_native_argument()
    // below whenever the engine ABI requires the native PackedArray type.
    explicit operator PackedArray&() noexcept { return native(); }
    explicit operator const PackedArray&() const noexcept { return native(); }
    // godot-cpp's MethodBind return path assigns native results into Variant and therefore
    // requires this implicit view. Generated explicit Variant construction never relies on
    // overload resolution: it routes through to_variant() below.
    operator godot::Variant() const { return value_; }

    [[nodiscard]] bool operator!() const { return native().is_empty(); }

    friend bool operator==(const SharedPackedArray& left, const SharedPackedArray& right) {
        return left.value_ == right.value_;
    }

    friend bool operator!=(const SharedPackedArray& left, const SharedPackedArray& right) {
        return !(left == right);
    }

  private:
    godot::Variant value_;
};

template <typename> struct IsSharedPackedArray : std::false_type {};

template <typename PackedArray>
struct IsSharedPackedArray<SharedPackedArray<PackedArray>> : std::true_type {};

// Every generated native-to-Variant boundary routes through one overload-independent adapter.
// This avoids compiler-specific constructor selection while retaining the exact Variant storage
// that gives PackedArrays their GDScript shared-reference identity.
[[nodiscard]] inline godot::Variant to_variant(std::nullptr_t) { return {}; }

template <typename Value>
[[nodiscard]] godot::Variant to_variant(Value&& value) {
    using Stored = std::remove_cv_t<std::remove_reference_t<Value>>;
    if constexpr (IsSharedPackedArray<Stored>::value)
        return value.variant();
    else
        return godot::Variant(std::forward<Value>(value));
}

// Keep SharedPackedArray's native conversion explicit without imposing copies on Godot API calls.
// Raw godot-cpp PackedArrays pass through unchanged, so this adapter is also safe for temporary
// results returned directly by another engine method.
template <typename Value> [[nodiscard]] decltype(auto) packed_native_argument(Value&& value) {
    using Stored = std::remove_cv_t<std::remove_reference_t<Value>>;
    if constexpr (IsSharedPackedArray<Stored>::value)
        return std::forward<Value>(value).native();
    else
        return std::forward<Value>(value);
}

// godot-cpp's vararg wrappers construct one Variant for each supplied argument. Pass ordinary
// native values through so that construction happens exactly once, while exposing the retained
// Variant storage for SharedPackedArray values whose native conversion is intentionally explicit.
// This adapter is only for APIs that perform their own Variant construction; raw Variant
// boundaries must continue to use to_variant().
[[nodiscard]] inline godot::Variant variant_constructor_argument(std::nullptr_t) { return {}; }

template <typename Value> [[nodiscard]] decltype(auto) variant_constructor_argument(Value&& value) {
    using Stored = std::remove_cv_t<std::remove_reference_t<Value>>;
    if constexpr (IsSharedPackedArray<Stored>::value)
        return std::forward<Value>(value).variant();
    else
        return std::forward<Value>(value);
}

} // namespace gdpp::runtime

// Generated methods use SharedPackedArray internally, but their reflected ABI remains the exact
// Godot PackedArray Variant type. These adapters are also required for generated property
// accessors; public script functions use GDPP's Variant-call bridge so their parameter identity is
// not lost through godot-cpp's value-oriented PackedArray caster.
namespace godot {

template <typename PackedArray>
struct GetTypeInfo<gdpp::runtime::SharedPackedArray<PackedArray>> {
    static constexpr GDExtensionVariantType VARIANT_TYPE = GetTypeInfo<PackedArray>::VARIANT_TYPE;
    static constexpr GDExtensionClassMethodArgumentMetadata METADATA =
        GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;

    static inline PropertyInfo get_class_info() {
        return GetTypeInfo<PackedArray>::get_class_info();
    }
};

template <typename PackedArray>
struct GetTypeInfo<const gdpp::runtime::SharedPackedArray<PackedArray>&>
    : GetTypeInfo<gdpp::runtime::SharedPackedArray<PackedArray>> {};

template <typename PackedArray>
struct VariantCaster<gdpp::runtime::SharedPackedArray<PackedArray>> {
    static _FORCE_INLINE_ gdpp::runtime::SharedPackedArray<PackedArray>
    cast(const Variant& value) {
        return gdpp::runtime::SharedPackedArray<PackedArray>(value);
    }
};

template <typename PackedArray>
struct VariantCaster<const gdpp::runtime::SharedPackedArray<PackedArray>&>
    : VariantCaster<gdpp::runtime::SharedPackedArray<PackedArray>> {};

template <typename PackedArray>
struct PtrToArg<gdpp::runtime::SharedPackedArray<PackedArray>> {
    static _FORCE_INLINE_ gdpp::runtime::SharedPackedArray<PackedArray>
    convert(const void* value) {
        return gdpp::runtime::SharedPackedArray<PackedArray>(
            *reinterpret_cast<const PackedArray*>(value));
    }

    using EncodeT = PackedArray;

    static _FORCE_INLINE_ void
    encode(const gdpp::runtime::SharedPackedArray<PackedArray>& value, void* output) {
        *reinterpret_cast<PackedArray*>(output) = value.native();
    }
};

template <typename PackedArray>
struct PtrToArg<const gdpp::runtime::SharedPackedArray<PackedArray>&>
    : PtrToArg<gdpp::runtime::SharedPackedArray<PackedArray>> {};

} // namespace godot
