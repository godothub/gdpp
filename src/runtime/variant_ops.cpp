#include "gdpp/runtime/variant_ops.hpp"

#include "gdpp/numeric/integer_semantics.hpp"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/window.hpp>
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

namespace gdpp::runtime {

class CoroutineState final {
  public:
    godot::ObjectID owner;
    godot::StringName signal;
    godot::Variant result;
    std::mutex mutex;
    bool completed{false};
    bool exposed{false};
};

namespace {

std::atomic<std::uint64_t>& coroutine_counter() {
    static std::atomic<std::uint64_t> value{0};
    return value;
}

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

void report_integer_error(const integer::ArithmeticError error) {
    if (error == integer::ArithmeticError::division_by_zero)
        godot::UtilityFunctions::push_error("GDPP: division by zero error in operator '/'");
    else if (error == integer::ArithmeticError::modulo_by_zero)
        godot::UtilityFunctions::push_error("GDPP: modulo by zero error in operator '%'");
}

std::int64_t integer_result_or_zero(const integer::Result result) {
    if (result)
        return result.value;
    report_integer_error(result.error);
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

class AwaitCallable final : public godot::CallableCustom {
  public:
    AwaitCallable(godot::Object* owner, AwaitContinuation continuation)
        : owner_(owner ? owner->get_instance_id() : godot::ObjectID{}),
          continuation_(std::move(continuation)) {}

    [[nodiscard]] std::uint32_t hash() const override {
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(owner_));
    }

    [[nodiscard]] godot::String get_as_text() const override { return "GDPP await continuation"; }

    static bool compare_equal(const godot::CallableCustom* left,
                              const godot::CallableCustom* right) {
        return left == right;
    }

    [[nodiscard]] CompareEqualFunc get_compare_equal_func() const override {
        return &AwaitCallable::compare_equal;
    }

    static bool compare_less(const godot::CallableCustom* left,
                             const godot::CallableCustom* right) {
        return left < right;
    }

    [[nodiscard]] CompareLessFunc get_compare_less_func() const override {
        return &AwaitCallable::compare_less;
    }

    [[nodiscard]] godot::ObjectID get_object() const override { return owner_; }

    void call(const godot::Variant** arguments, int argument_count, godot::Variant& return_value,
              GDExtensionCallError& error) const override {
        if (!godot::ObjectDB::get_instance(static_cast<std::uint64_t>(owner_))) {
            return_value = {};
            error.error = GDEXTENSION_CALL_OK;
            return;
        }
        godot::Array values;
        for (int index = 0; index < argument_count; ++index)
            values.push_back(*arguments[index]);
        continuation_(values);
        return_value = {};
        error.error = GDEXTENSION_CALL_OK;
    }

  private:
    godot::ObjectID owner_;
    AwaitContinuation continuation_;
};

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

void report_invalid_member(const char* operation, const godot::StringName& name) {
    godot::UtilityFunctions::push_error(godot::String("GDPP: dynamic ") + operation + " '" +
                                        godot::String(name) + "' failed");
}

void report_invalid_key(const char* operation) {
    godot::UtilityFunctions::push_error(godot::String("GDPP: dynamic keyed ") + operation +
                                        " failed");
}

} // namespace

godot::Variant default_argument() { return default_argument_marker(); }

bool is_default_argument(const godot::Variant& value) {
    return value.get_type() == godot::Variant::STRING_NAME &&
           static_cast<godot::StringName>(value) == default_argument_marker();
}

godot::Variant binary(godot::Variant::Operator operation, const godot::Variant& left,
                      const godot::Variant& right) {
    if (left.get_type() == godot::Variant::INT && right.get_type() == godot::Variant::INT) {
        const auto left_value = static_cast<std::int64_t>(left);
        const auto right_value = static_cast<std::int64_t>(right);
        if (const auto result = evaluate_integer_operator(operation, left_value, right_value)) {
            if (!*result) {
                report_integer_error(result->error);
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
        godot::UtilityFunctions::push_error("GDPP: invalid dynamic binary operation");
        return {};
    }
    return result;
}

godot::Variant binary_integer(const godot::Variant::Operator operation, const godot::Variant& left,
                              const std::int64_t right) {
    if (left.get_type() == godot::Variant::INT) {
        const auto left_value = static_cast<std::int64_t>(left);
        if (const auto result = evaluate_integer_operator(operation, left_value, right)) {
            if (!*result) {
                report_integer_error(result->error);
                return {};
            }
            return result->value;
        }
    }
    return binary(operation, left, godot::Variant(right));
}

godot::Variant binary_integer(const godot::Variant::Operator operation, const std::int64_t left,
                              const godot::Variant& right) {
    if (right.get_type() == godot::Variant::INT) {
        const auto right_value = static_cast<std::int64_t>(right);
        if (const auto result = evaluate_integer_operator(operation, left, right_value)) {
            if (!*result) {
                report_integer_error(result->error);
                return {};
            }
            return result->value;
        }
    }
    return binary(operation, godot::Variant(left), right);
}

void compound_assign(godot::Variant& target, const godot::Variant::Operator operation,
                     const godot::Variant& value) {
    target = binary(operation, target, value);
}

void compound_assign_integer(godot::Variant& target, const godot::Variant::Operator operation,
                             const std::int64_t value) {
    target = binary_integer(operation, target, value);
}

void assign_dictionary(godot::Dictionary& target, const godot::Dictionary& value) {
    if (&target == &value)
        return;
    target = value;
}

godot::Variant unary(godot::Variant::Operator operation, const godot::Variant& operand) {
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
        godot::UtilityFunctions::push_error("GDPP: invalid dynamic unary operation");
        return {};
    }
    return result;
}

std::int64_t integer_divide(const std::int64_t left, const std::int64_t right) {
    return integer_result_or_zero(integer::divide(left, right));
}

std::int64_t integer_modulo(const std::int64_t left, const std::int64_t right) {
    return integer_result_or_zero(integer::modulo(left, right));
}

bool is_instance_valid(const godot::Variant& value) noexcept {
    return value.get_type() == godot::Variant::OBJECT && value.get_validated_object() != nullptr;
}

godot::Array make_range(std::int64_t stop) { return make_range(0, stop, 1); }

godot::Array make_range(std::int64_t start, std::int64_t stop) {
    return make_range(start, stop, 1);
}

godot::Array make_range(std::int64_t start, std::int64_t stop, std::int64_t step) {
    godot::Array result;
    if (step == 0) {
        godot::UtilityFunctions::push_error("GDPP: range step cannot be zero");
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

godot::Array get_stack() {
    // Godot exposes GDScript stack frames only while its language debugger is active. Native AOT
    // frames do not have GDScript VM stack records, so the compatible non-debug result is empty.
    // Callers already use this contract to fall back to an unknown source location.
    return {};
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

godot::Object* find_engine_singleton(const godot::StringName& name) {
    const auto* engine = godot::Engine::get_singleton();
    auto* singleton = engine ? engine->get_singleton(name) : nullptr;
    if (!singleton) {
        godot::UtilityFunctions::push_error(godot::String("GDPP: engine singleton '") +
                                            godot::String(name) + "' is unavailable");
    }
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

godot::Variant instantiate_external_class(const godot::StringName& name) {
    auto* class_db = godot::ClassDBSingleton::get_singleton();
    if (!class_db || !class_db->class_exists(name) || !class_db->can_instantiate(name)) {
        godot::UtilityFunctions::push_error(godot::String("GDPP: external class '") +
                                            godot::String(name) +
                                            "' is unavailable or cannot be instantiated");
        return {};
    }
    return class_db->instantiate(name);
}

bool is_external_instance(const godot::Variant& value, const godot::StringName& class_name) {
    if (value.get_type() != godot::Variant::OBJECT)
        return false;
    auto* object = value.get_validated_object();
    return object && object->is_class(class_name);
}

godot::Callable external_callable(const godot::Variant& value, const godot::StringName& method) {
    auto* object = value.get_validated_object();
    if (!object) {
        godot::UtilityFunctions::push_error(
            "GDPP: cannot create a Callable for a null external object");
        return {};
    }
    return {object, method};
}

godot::Signal external_signal(const godot::Variant& value, const godot::StringName& signal) {
    auto* object = value.get_validated_object();
    if (!object) {
        godot::UtilityFunctions::push_error(
            "GDPP: cannot access a Signal on a null external object");
        return {};
    }
    return {object, signal};
}

bool await_signal(const godot::Variant& signal_value, godot::Object* owner,
                  AwaitContinuation continuation) {
    if (!owner || !continuation || signal_value.get_type() != godot::Variant::SIGNAL) {
        godot::UtilityFunctions::push_error("GDPP: await requires a live owner and Signal");
        return false;
    }
    auto signal = static_cast<godot::Signal>(signal_value);
    if (signal.is_null()) {
        godot::UtilityFunctions::push_error("GDPP: cannot await a null Signal");
        return false;
    }
    const godot::Callable callable{memnew(AwaitCallable(owner, std::move(continuation)))};
    const auto error = signal.connect(callable, godot::Object::CONNECT_ONE_SHOT);
    if (error != 0) {
        godot::UtilityFunctions::push_error("GDPP: failed to connect await continuation");
        return false;
    }
    return true;
}

godot::Variant await_result(const godot::Array& arguments) {
    if (arguments.is_empty())
        return {};
    if (arguments.size() == 1)
        return arguments[0];
    return arguments;
}

CoroutineStatePtr begin_coroutine(godot::Object* owner) {
    if (!owner) {
        godot::UtilityFunctions::push_error("GDPP: cannot start a coroutine without an owner");
        return {};
    }
    auto state = std::make_shared<CoroutineState>();
    state->owner = owner->get_instance_id();
    do {
        const auto index = coroutine_counter().fetch_add(1, std::memory_order_relaxed);
        const auto name = std::string{"__gdpp_coroutine_completed_"} + std::to_string(index);
        state->signal = godot::StringName(name.c_str());
    } while (owner->has_user_signal(state->signal));

    godot::Dictionary argument;
    argument["name"] = "result";
    argument["type"] = static_cast<std::int64_t>(godot::Variant::NIL);
    godot::Array arguments;
    arguments.push_back(argument);
    owner->add_user_signal(godot::String(state->signal), arguments);
    return state;
}

godot::Variant coroutine_result(const CoroutineStatePtr& state) {
    if (!state)
        return {};
    godot::Variant result;
    bool completed = false;
    {
        const std::lock_guard<std::mutex> lock{state->mutex};
        completed = state->completed;
        if (completed)
            result = state->result;
        else
            state->exposed = true;
    }
    auto* owner = godot::ObjectDB::get_instance(static_cast<std::uint64_t>(state->owner));
    if (!owner)
        return completed ? result : godot::Variant{};
    if (completed) {
        if (owner->has_user_signal(state->signal))
            owner->remove_user_signal(state->signal);
        return result;
    }
    return godot::Signal(owner, state->signal);
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
    auto* owner = godot::ObjectDB::get_instance(static_cast<std::uint64_t>(state->owner));
    if (!owner)
        return;
    owner->emit_signal(state->signal, result);
    if (owner->has_user_signal(state->signal))
        owner->remove_user_signal(state->signal);
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
                                 const godot::Variant** arguments, std::size_t argument_count) {
    static const godot::StringName get_script_method{"get_script"};
    static const godot::StringName set_script_method{"set_script"};
    if (argument_count == 0 && method == get_script_method &&
        target.get_type() == godot::Variant::OBJECT) {
        return script_identity(static_cast<godot::Object*>(target));
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
            godot::UtilityFunctions::push_error(
                godot::String("GDPP: cannot attach native script class '") +
                godot::String(requested_class) +
                "' to an object that was not converted to that class during export");
            return {};
        }
    }
    godot::Variant result;
    GDExtensionCallError error{GDEXTENSION_CALL_OK, 0, 0};
    target.callp(method, arguments, static_cast<int>(argument_count), result, error);
    if (error.error != GDEXTENSION_CALL_OK) {
        report_invalid_member("call", method);
        return {};
    }
    return result;
}

godot::Variant get_named(const godot::Variant& target, const godot::StringName& name) {
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
        report_invalid_member("property read", name);
        return {};
    }
    return result;
}

void set_named(godot::Variant& target, const godot::StringName& name, const godot::Variant& value) {
    if (target.get_type() == godot::Variant::DICTIONARY) {
        auto dictionary = static_cast<godot::Dictionary>(target);
        dictionary[godot::Variant(name)] = value;
        return;
    }
    bool valid = false;
    target.set_named(name, value, valid);
    if (!valid)
        report_invalid_member("property write", name);
}

godot::Variant get_key(const godot::Variant& target, const godot::Variant& key) {
    bool valid = false;
    auto result = target.get(key, &valid);
    if (!valid) {
        report_invalid_key("read");
        return {};
    }
    return result;
}

void set_key(godot::Variant& target, const godot::Variant& key, const godot::Variant& value) {
    bool valid = false;
    target.set(key, value, &valid);
    if (!valid)
        report_invalid_key("write");
}

void report_index_out_of_bounds(const char* container, const std::int64_t index,
                                const std::int64_t size) {
    godot::UtilityFunctions::push_error(
        godot::String{"GDPP: "} + container + " index " + godot::String::num_int64(index) +
        " is out of bounds for size " + godot::String::num_int64(size));
}

bool iter_init(const godot::Variant& iterable, godot::Variant& iterator) {
    bool valid = false;
    const bool available = iterable.iter_init(iterator, valid);
    if (!valid)
        godot::UtilityFunctions::push_error("GDPP: value is not dynamically iterable");
    return valid && available;
}

bool iter_next(const godot::Variant& iterable, godot::Variant& iterator) {
    bool valid = false;
    const bool available = iterable.iter_next(iterator, valid);
    if (!valid)
        godot::UtilityFunctions::push_error("GDPP: dynamic iterator advance failed");
    return valid && available;
}

godot::Variant iter_get(const godot::Variant& iterable, const godot::Variant& iterator) {
    bool valid = false;
    auto value = iterable.iter_get(iterator, valid);
    if (!valid) {
        godot::UtilityFunctions::push_error("GDPP: dynamic iterator value read failed");
        return {};
    }
    return value;
}

} // namespace gdpp::runtime
