#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/core/type_info.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/typed_dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <gdextension_interface.h>

#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace gdpp::runtime {

struct ScriptSourceLocation;

// The generated behavior object owns script fields and methods. The Godot object remains an
// instance of its original ClassDB type, including when that type belongs to another
// GDExtension. Keeping the owner as a non-owning pointer avoids a reference cycle: Godot owns the
// ScriptInstance, and the ScriptInstance owns the behavior.
class AttachedScriptBehavior : public godot::RefCounted {
    GDCLASS(AttachedScriptBehavior, godot::RefCounted)

  public:
    void attach_owner(godot::Object* owner);
    void detach_owner();
    [[nodiscard]] godot::Object* owner() const;

    // Generated classes override these hooks so initialization runs only after the external
    // owner is attached. This is required for field initializers that access self or native
    // properties. Onready initialization is a separate lifecycle, matching GDScript's
    // base-to-derived @implicit_ready() chain rather than relying on a user-declared _ready().
    virtual void _gdpp_initialize_instance();
    virtual void _gdpp_initialize_onready();
    virtual void _gdpp_dispatch_notification(std::int32_t what, bool reversed);

  protected:
    static void _bind_methods();

  private:
    godot::Object* owner_{nullptr};
};

// RefCounted construction changed in godot-cpp 4.7: memnew(T) now returns Ref<T> instead of a
// raw pointer. Keep the factory ownership explicit so generated code has one contract on every
// supported Godot release and never depends on the include order of ref.hpp.
using AttachedBehaviorFactory = godot::Ref<AttachedScriptBehavior> (*)();
using AttachedConstantResolver = godot::Variant (*)();
using AttachedPropertyGetter = godot::Variant (*)(AttachedScriptBehavior*);
using AttachedPropertySetter = bool (*)(AttachedScriptBehavior*, const godot::Variant&);

struct AttachedScriptProperty {
    godot::PropertyInfo info;
    godot::Variant default_value;
    // ScriptInstance property dispatch must not depend on ClassDBSingleton reflecting a
    // GDExtension-owned behavior object. Generated accessors retain the original typed
    // getter/setter semantics and also work for inherited descriptors.
    AttachedPropertyGetter getter{nullptr};
    AttachedPropertySetter setter{nullptr};
    // Deprecated GDScript instance-dictionary utilities serialize member slots directly: custom
    // accessors and _set/_get must not run. Generated raw accessors preserve that distinction
    // while still enforcing the native type contract on restoration.
    AttachedPropertyGetter storage_getter{nullptr};
    AttachedPropertySetter storage_setter{nullptr};
    bool has_default{false};
};

// Script constants that may touch ResourceLoader, ClassDB singletons, third-party services, or
// other script state must never be evaluated while a GDExtension is registering its classes.
// Generated descriptors therefore retain only a captureless resolver. Godot asks for the
// materialized constant Dictionary after startup, at which point the generated constant getter
// performs its normal thread-safe one-time initialization.
struct AttachedScriptDeferredConstant {
    godot::StringName name;
    AttachedConstantResolver resolver{nullptr};
};

// ScriptInstance has already resolved a customer method before entering the attached runtime.
// Retain the generated, validating method entry point beside its reflected MethodInfo so callbacks
// do not wrap the behavior in a Variant and repeat ClassDB method lookup on every signal,
// lifecycle, Callable, animation, or timer dispatch.
struct AttachedScriptMethodDispatch {
    godot::StringName name;
    GDExtensionClassMethodCall call{nullptr};
};

struct AttachedScriptDescriptor {
    AttachedScriptDescriptor() = default;
    AttachedScriptDescriptor(const AttachedScriptDescriptor&) = default;
    AttachedScriptDescriptor(AttachedScriptDescriptor&&) = default;
    AttachedScriptDescriptor& operator=(const AttachedScriptDescriptor& other);
    AttachedScriptDescriptor& operator=(AttachedScriptDescriptor&& other);

    godot::String source_path;
    godot::StringName global_name;
    godot::StringName native_base_type;
    godot::String base_script_path;
    godot::String contract_hash;
    godot::StringName behavior_class;
    AttachedBehaviorFactory factory{nullptr};
    std::vector<AttachedScriptProperty> properties;
    std::vector<godot::MethodInfo> methods;
    std::vector<AttachedScriptMethodDispatch> method_dispatches;
    std::vector<godot::MethodInfo> signals;
    godot::Dictionary constants;
    std::vector<AttachedScriptDeferredConstant> deferred_constants;
    godot::Variant rpc_config;
    bool tool{false};
    bool abstract{false};
    // The compiler extension installs reflection-only descriptors while export resources are
    // rewritten. They deliberately have no behavior factory: the editor needs property storage
    // and Script reflection, while executable behavior exists only in the target library.
    bool editor_metadata_only{false};
};

// Registration is deterministic and fails closed: duplicate paths are accepted only when their
// externally visible identity is identical. Generated libraries register every descriptor after
// all behavior classes have entered ClassDB.
[[nodiscard]] bool register_attached_script(AttachedScriptDescriptor descriptor,
                                            godot::String* error = nullptr);
void unregister_all_attached_scripts();
[[nodiscard]] std::optional<AttachedScriptDescriptor>
find_attached_script(const godot::String& source_path);
// Canonicalizes `res://`, `uid://`, and project-root-relative resource identities using Godot's
// active UID table. An unresolved UID or a non-project path returns an empty String.
[[nodiscard]] godot::String
resolve_attached_script_resource_path(const godot::String& resource_path);
// Resolves a complete script view, with derived declarations shadowing inherited ones. A missing
// or cyclic base chain fails closed and returns no descriptor.
[[nodiscard]] std::optional<AttachedScriptDescriptor>
resolve_attached_script(const godot::String& source_path, godot::String* error = nullptr);
[[nodiscard]] std::vector<godot::String> attached_script_paths();
// Resolves deferred script constants on demand. Descriptor registration and inheritance
// resolution remain metadata-only and never invoke a resolver.
[[nodiscard]] godot::Dictionary
materialize_attached_script_constants(const AttachedScriptDescriptor& descriptor);

class AttachedCompiledScript;

// Binary-only exports retain the source `res://*.gd` identity even though the source bytes are
// deliberately absent. This loader makes every ResourceLoader entry point resolve that identity
// to the process-local canonical AttachedCompiledScript. It is registered only by generated
// project libraries, never by the editor compiler bridge, so ordinary editor GDScript loading is
// unaffected. Unknown `.gd` paths fail closed instead of falling through to a source loader.
class AttachedScriptResourceLoader : public godot::ResourceFormatLoader {
    GDCLASS(AttachedScriptResourceLoader, godot::ResourceFormatLoader)

  public:
    godot::PackedStringArray _get_recognized_extensions() const override;
    bool _recognize_path(const godot::String& path, const godot::StringName& type) const override;
    bool _handles_type(const godot::StringName& type) const override;
    godot::String _get_resource_type(const godot::String& path) const override;
    godot::String _get_resource_script_class(const godot::String& path) const override;
    bool _exists(const godot::String& path) const override;
    godot::Variant _load(const godot::String& path, const godot::String& original_path,
                         bool use_sub_threads, std::int32_t cache_mode) const override;

  protected:
    static void _bind_methods();
};

// Registration owns one strong Ref for the complete scene-level lifetime and is idempotent.
// Callers must register AttachedScriptResourceLoader with ClassDB first. Removal must happen
// before descriptors are cleared so ResourceLoader can never observe a half-torn-down registry.
[[nodiscard]] bool register_attached_script_resource_loader(godot::String* error = nullptr);
void unregister_attached_script_resource_loader();

// Returns the process-local canonical Script resource for a registered source identity. Godot
// uses resource identity as part of typed Array/Dictionary compatibility, so all generated
// containers and runtime-created instances must share this resource instead of materializing
// equivalent-but-distinct ScriptExtension objects.
[[nodiscard]] godot::Ref<AttachedCompiledScript>
attached_script_resource(const godot::String& source_path, godot::String* error = nullptr);
// Descriptor construction can itself contain typed-container defaults whose target descriptor is
// registered later in the same generated library. This returns the same canonical resource
// without requiring that registration order; the exact contract is bound when the descriptor
// becomes available.
[[nodiscard]] godot::Ref<AttachedCompiledScript>
attached_container_script_resource(const godot::String& source_path);

// Script types are identities attached to a Godot owner, not ClassDB subclasses of that owner.
// These helpers provide the runtime equivalent of GDScript's `is` and `as` operations without
// ever casting an owner pointer to a generated behavior implementation.
[[nodiscard]] bool is_attached_script_instance(godot::Object* object,
                                               const godot::String& source_path);
// Restricts export-time ScriptInstance property-state serialization to fields that were actually
// stored by the source scene/resource. Target runtime instances reject this metadata-only API.
[[nodiscard]] bool
set_attached_editor_storage_state(godot::Object* object,
                                  const godot::PackedStringArray& stored_properties);
[[nodiscard]] godot::Object* cast_attached_script(const godot::Variant& value,
                                                  const godot::String& source_path);
[[nodiscard]] godot::Object* strict_attached_script_storage(const godot::Variant& value,
                                                            const godot::String& source_path,
                                                            const ScriptSourceLocation& location);
// Returns the opaque ScriptInstance handle Godot expects from debugger callbacks. The handle is
// valid only while the owner retains the attached compiled script.
[[nodiscard]] void* attached_script_instance_handle(godot::Object* object);

// Constructs the provider-owned native object, attaches the compiled script and invokes _init
// with the supplied arguments. The Variant preserves RefCounted ownership when applicable.
[[nodiscard]] godot::Variant instantiate_attached_script(const godot::String& source_path,
                                                         const godot::Array& arguments = {},
                                                         godot::String* error = nullptr);

// Godot represents Array[ScriptType] and Dictionary[..., ScriptType] with both the provider-owned
// native base class and the exact Script resource. A generated attached behavior is not a native
// subclass of its owner, so using the behavior ClassDB name alone rejects every valid element.
// Resolve the same native-base-plus-Script contract that GDScript installs on typed containers.
struct AttachedContainerType {
    std::uint32_t builtin_type{godot::Variant::NIL};
    godot::StringName class_name;
    godot::Variant script;
};

[[nodiscard]] AttachedContainerType
attached_container_type(const godot::String& source_path,
                        const godot::StringName& native_base_type);

template <typename Type, typename = void> struct ContainerTypeResolver {
    [[nodiscard]] static AttachedContainerType resolve() {
        return {static_cast<std::uint32_t>(godot::GetTypeInfo<Type>::VARIANT_TYPE), {}, {}};
    }

    [[nodiscard]] static godot::StringName reflection_name() {
        return godot::Variant::get_type_name(
            static_cast<godot::Variant::Type>(godot::GetTypeInfo<Type>::VARIANT_TYPE));
    }
};

// Generated object tags expose their ClassDB constraint and, for attached scripts, the exact
// source identity. Empty source paths retain ordinary native object-container behavior.
template <typename Type>
struct ContainerTypeResolver<Type, std::void_t<decltype(Type::get_class_static()),
                                               decltype(Type::_gdpp_attached_script_path)>> {
    [[nodiscard]] static AttachedContainerType resolve() {
        if (Type::_gdpp_attached_script_path[0] != '\0')
            return attached_container_type(godot::String(Type::_gdpp_attached_script_path),
                                           Type::get_class_static());
        return {godot::Variant::OBJECT, Type::get_class_static(), {}};
    }

    [[nodiscard]] static godot::StringName reflection_name() { return Type::get_class_static(); }
};

template <typename Element> class ScriptTypedArray : public godot::Array {
  public:
    ScriptTypedArray() { configure(); }

    explicit ScriptTypedArray(const godot::Variant& value)
        : ScriptTypedArray(godot::Array(value)) {}

    explicit ScriptTypedArray(const godot::Array& value) {
        configure();
        if (is_same_typed(value))
            godot::Array::operator=(value);
        else
            assign(value);
    }

    ScriptTypedArray(std::initializer_list<godot::Variant> values)
        : ScriptTypedArray(godot::Array(values)) {}

    void operator=(const godot::Array& value) {
        ERR_FAIL_COND_MSG(!is_same_typed(value),
                          "Cannot assign an Array with a different script element type.");
        godot::Array::operator=(value);
    }

  private:
    void configure() {
        const auto type = ContainerTypeResolver<Element>::resolve();
        set_typed(type.builtin_type, type.class_name, type.script);
    }
};

template <typename Key, typename Value> class ScriptTypedDictionary : public godot::Dictionary {
  public:
    ScriptTypedDictionary() { configure(); }

    explicit ScriptTypedDictionary(const godot::Variant& value)
        : ScriptTypedDictionary(godot::Dictionary(value)) {}

    explicit ScriptTypedDictionary(const godot::Dictionary& value) {
        configure();
        if (is_same_typed(value))
            godot::Dictionary::operator=(value);
        else
            assign(value);
    }

    ScriptTypedDictionary(
        std::initializer_list<godot::KeyValue<godot::Variant, godot::Variant>> values)
        : ScriptTypedDictionary() {
        for (const auto& entry : values)
            set(entry.key, entry.value);
    }

    void operator=(const godot::Dictionary& value) {
        ERR_FAIL_COND_MSG(!is_same_typed(value),
                          "Cannot assign a Dictionary with different script key/value types.");
        godot::Dictionary::operator=(value);
    }

  private:
    void configure() {
        const auto key = ContainerTypeResolver<Key>::resolve();
        const auto value = ContainerTypeResolver<Value>::resolve();
        set_typed(key.builtin_type, key.class_name, key.script, value.builtin_type,
                  value.class_name, value.script);
    }
};

[[nodiscard]] godot::Variant
call_attached_native_base_raw(godot::Object* owner, const godot::StringName& native_class,
                              const godot::StringName& method, std::uint32_t compatibility_hash,
                              const godot::Variant** arguments, std::int64_t argument_count);

template <typename... Arguments>
[[nodiscard]] godot::Variant
call_attached_native_base(godot::Object* owner, const godot::StringName& native_class,
                          const godot::StringName& method, std::uint32_t compatibility_hash,
                          Arguments&&... arguments) {
    std::array<godot::Variant, sizeof...(Arguments)> values{
        godot::Variant(std::forward<Arguments>(arguments))...};
    std::array<const godot::Variant*, sizeof...(Arguments)> pointers{};
    for (std::size_t index = 0; index < values.size(); ++index)
        pointers[index] = &values[index];
    return call_attached_native_base_raw(owner, native_class, method, compatibility_hash,
                                         pointers.data(),
                                         static_cast<std::int64_t>(pointers.size()));
}

// Debug frames are emitted only for debug export profiles. They give ScriptLanguageExtension the
// same top-first stack shape as interpreted GDScript while keeping all debugger state
// thread-local. A resumed coroutine can enter at a breakpoint after its original native frame has
// returned; debug_breakpoint() creates a temporary frame in that case.
class ScriptDebugFrame final {
  public:
    ScriptDebugFrame(const godot::String& source, const godot::StringName& function,
                     std::int32_t line, godot::Object* instance);
    ~ScriptDebugFrame();

    ScriptDebugFrame(const ScriptDebugFrame&) = delete;
    ScriptDebugFrame& operator=(const ScriptDebugFrame&) = delete;
    ScriptDebugFrame(ScriptDebugFrame&&) = delete;
    ScriptDebugFrame& operator=(ScriptDebugFrame&&) = delete;

    void set_line(std::int32_t line);

  private:
    std::uint64_t token_{0};
};

// Returns the current native AOT call stack in GDScript's top-first public Dictionary shape.
// Release exports naturally return an empty Array because they do not emit ScriptDebugFrame.
[[nodiscard]] godot::Array attached_debug_stack();

void debug_breakpoint(const godot::String& source, const godot::StringName& function,
                      std::int32_t line, godot::Object* instance,
                      const godot::PackedStringArray& local_names, const godot::Array& local_values,
                      const godot::PackedStringArray& member_names = {},
                      const godot::Array& member_values = {});

class AttachedCompiledLanguage : public godot::ScriptLanguageExtension {
    GDCLASS(AttachedCompiledLanguage, godot::ScriptLanguageExtension)

  public:
    static AttachedCompiledLanguage* get_singleton();
    static bool register_singleton(godot::String* error = nullptr);
    static void unregister_singleton();

    godot::String _get_name() const override;
    void _init() override;
    godot::String _get_type() const override;
    godot::String _get_extension() const override;
    void _finish() override;
    godot::PackedStringArray _get_reserved_words() const override;
    bool _is_control_flow_keyword(const godot::String& keyword) const override;
    godot::PackedStringArray _get_comment_delimiters() const override;
    godot::PackedStringArray _get_doc_comment_delimiters() const override;
    godot::PackedStringArray _get_string_delimiters() const override;
    godot::Ref<godot::Script> _make_template(const godot::String& source,
                                             const godot::String& class_name,
                                             const godot::String& base_class_name) const override;
    godot::TypedArray<godot::Dictionary>
    _get_built_in_templates(const godot::StringName& object) const override;
    bool _is_using_templates() override;
    godot::Dictionary _validate(const godot::String& script, const godot::String& path,
                                bool validate_functions, bool validate_errors,
                                bool validate_warnings, bool validate_safe_lines) const override;
    godot::String _validate_path(const godot::String& path) const override;
    godot::Object* _create_script() const override;
    bool _has_named_classes() const override;
    bool _supports_builtin_mode() const override;
    bool _supports_documentation() const override;
    bool _can_inherit_from_file() const override;
    std::int32_t _find_function(const godot::String& function,
                                const godot::String& code) const override;
    godot::String _make_function(const godot::String& class_name,
                                 const godot::String& function_name,
                                 const godot::PackedStringArray& function_args) const override;
    bool _can_make_function() const override;
    godot::Error _open_in_external_editor(const godot::Ref<godot::Script>& script,
                                          std::int32_t line, std::int32_t column) override;
    bool _overrides_external_editor() override;
    godot::ScriptLanguage::ScriptNameCasing _preferred_file_name_casing() const override;
    godot::Dictionary _complete_code(const godot::String& code, const godot::String& path,
                                     godot::Object* owner) const override;
    godot::Dictionary _lookup_code(const godot::String& code, const godot::String& symbol,
                                   const godot::String& path, godot::Object* owner) const override;
    godot::String _auto_indent_code(const godot::String& code, std::int32_t from_line,
                                    std::int32_t to_line) const override;
    void _add_global_constant(const godot::StringName& name, const godot::Variant& value) override;
    void _add_named_global_constant(const godot::StringName& name,
                                    const godot::Variant& value) override;
    void _remove_named_global_constant(const godot::StringName& name) override;
    void _thread_enter() override;
    void _thread_exit() override;
    godot::String _debug_get_error() const override;
    std::int32_t _debug_get_stack_level_count() const override;
    std::int32_t _debug_get_stack_level_line(std::int32_t level) const override;
    godot::String _debug_get_stack_level_function(std::int32_t level) const override;
    godot::String _debug_get_stack_level_source(std::int32_t level) const override;
    godot::Dictionary _debug_get_stack_level_locals(std::int32_t level, std::int32_t max_subitems,
                                                    std::int32_t max_depth) override;
    godot::Dictionary _debug_get_stack_level_members(std::int32_t level, std::int32_t max_subitems,
                                                     std::int32_t max_depth) override;
    void* _debug_get_stack_level_instance(std::int32_t level) override;
    godot::Dictionary _debug_get_globals(std::int32_t max_subitems,
                                         std::int32_t max_depth) override;
    godot::String _debug_parse_stack_level_expression(std::int32_t level,
                                                      const godot::String& expression,
                                                      std::int32_t max_subitems,
                                                      std::int32_t max_depth) override;
    godot::TypedArray<godot::Dictionary> _debug_get_current_stack_info() override;
    void _reload_all_scripts() override;
    void _reload_scripts(const godot::Array& scripts, bool soft_reload) override;
    void _reload_tool_script(const godot::Ref<godot::Script>& script, bool soft_reload) override;
    godot::PackedStringArray _get_recognized_extensions() const override;
    godot::TypedArray<godot::Dictionary> _get_public_functions() const override;
    godot::Dictionary _get_public_constants() const override;
    godot::TypedArray<godot::Dictionary> _get_public_annotations() const override;
    void _profiling_start() override;
    void _profiling_stop() override;
    void _profiling_set_save_native_calls(bool enable) override;
    std::int32_t
    _profiling_get_accumulated_data(godot::ScriptLanguageExtensionProfilingInfo* info_array,
                                    std::int32_t info_max) override;
    std::int32_t _profiling_get_frame_data(godot::ScriptLanguageExtensionProfilingInfo* info_array,
                                           std::int32_t info_max) override;
    void _frame() override;
    bool _handles_global_class_type(const godot::String& type) const override;
    godot::Dictionary _get_global_class_name(const godot::String& path) const override;

  protected:
    static void _bind_methods();
};

class AttachedCompiledScript : public godot::ScriptExtension {
    GDCLASS(AttachedCompiledScript, godot::ScriptExtension)

  public:
    void set_source_path(const godot::String& source_path);
    [[nodiscard]] godot::String get_source_path() const;
    void set_contract_hash(const godot::String& contract_hash);
    [[nodiscard]] godot::String get_contract_hash() const;

    bool _editor_can_reload_from_file() override;
    void _placeholder_erased(void* placeholder) override;
    bool _can_instantiate() const override;
    godot::Ref<godot::Script> _get_base_script() const override;
    godot::StringName _get_global_name() const override;
    bool _inherits_script(const godot::Ref<godot::Script>& script) const override;
    godot::StringName _get_instance_base_type() const override;
    void* _instance_create(godot::Object* object) const override;
    void* _placeholder_instance_create(godot::Object* object) const override;
    bool _instance_has(godot::Object* object) const override;
    bool _has_source_code() const override;
    godot::String _get_source_code() const override;
    void _set_source_code(const godot::String& code) override;
    godot::Error _reload(bool keep_state) override;
    godot::StringName _get_doc_class_name() const override;
    godot::TypedArray<godot::Dictionary> _get_documentation() const override;
    godot::String _get_class_icon_path() const override;
    bool _has_method(const godot::StringName& method) const override;
    bool _has_static_method(const godot::StringName& method) const override;
    godot::Variant
    _get_script_method_argument_count(const godot::StringName& method) const override;
    godot::Dictionary _get_method_info(const godot::StringName& method) const override;
    bool _is_tool() const override;
    bool _is_valid() const override;
    bool _is_abstract() const override;
    godot::ScriptLanguage* _get_language() const override;
    bool _has_script_signal(const godot::StringName& signal) const override;
    godot::TypedArray<godot::Dictionary> _get_script_signal_list() const override;
    bool _has_property_default_value(const godot::StringName& property) const override;
    godot::Variant _get_property_default_value(const godot::StringName& property) const override;
    void _update_exports() override;
    godot::TypedArray<godot::Dictionary> _get_script_method_list() const override;
    godot::TypedArray<godot::Dictionary> _get_script_property_list() const override;
    std::int32_t _get_member_line(const godot::StringName& member) const override;
    godot::Dictionary _get_constants() const override;
    godot::TypedArray<godot::StringName> _get_members() const override;
    bool _is_placeholder_fallback_enabled() const override;
    godot::Variant _get_rpc_config() const override;

  protected:
    static void _bind_methods();

  private:
    [[nodiscard]] std::optional<AttachedScriptDescriptor> descriptor() const;

    godot::String source_path_;
    godot::String contract_hash_;
};

} // namespace gdpp::runtime

namespace godot {

template <typename Element> struct GetTypeInfo<gdpp::runtime::ScriptTypedArray<Element>> {
    static constexpr GDExtensionVariantType VARIANT_TYPE = GDEXTENSION_VARIANT_TYPE_ARRAY;
    static constexpr GDExtensionClassMethodArgumentMetadata METADATA =
        GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;

    static PropertyInfo get_class_info() {
        return make_property_info(Variant::ARRAY, "", PROPERTY_HINT_ARRAY_TYPE,
                                  gdpp::runtime::ContainerTypeResolver<Element>::reflection_name());
    }
};

template <typename Element>
struct GetTypeInfo<const gdpp::runtime::ScriptTypedArray<Element>&>
    : GetTypeInfo<gdpp::runtime::ScriptTypedArray<Element>> {};

template <typename Element> struct VariantCaster<gdpp::runtime::ScriptTypedArray<Element>> {
    static gdpp::runtime::ScriptTypedArray<Element> cast(const Variant& value) {
        return gdpp::runtime::ScriptTypedArray<Element>(value);
    }
};

template <typename Element>
struct VariantCaster<const gdpp::runtime::ScriptTypedArray<Element>&>
    : VariantCaster<gdpp::runtime::ScriptTypedArray<Element>> {};

template <typename Element> struct PtrToArg<gdpp::runtime::ScriptTypedArray<Element>> {
    static gdpp::runtime::ScriptTypedArray<Element> convert(const void* value) {
        return gdpp::runtime::ScriptTypedArray<Element>(*reinterpret_cast<const Array*>(value));
    }

    using EncodeT = Array;

    static void encode(const gdpp::runtime::ScriptTypedArray<Element>& value, void* output) {
        *reinterpret_cast<Array*>(output) = value;
    }
};

template <typename Element>
struct PtrToArg<const gdpp::runtime::ScriptTypedArray<Element>&>
    : PtrToArg<gdpp::runtime::ScriptTypedArray<Element>> {};

template <typename Key, typename Value>
struct GetTypeInfo<gdpp::runtime::ScriptTypedDictionary<Key, Value>> {
    static constexpr GDExtensionVariantType VARIANT_TYPE = GDEXTENSION_VARIANT_TYPE_DICTIONARY;
    static constexpr GDExtensionClassMethodArgumentMetadata METADATA =
        GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;

    static PropertyInfo get_class_info() {
        return PropertyInfo(
            Variant::DICTIONARY, "", PROPERTY_HINT_DICTIONARY_TYPE,
            vformat("%s;%s", gdpp::runtime::ContainerTypeResolver<Key>::reflection_name(),
                    gdpp::runtime::ContainerTypeResolver<Value>::reflection_name()));
    }
};

template <typename Key, typename Value>
struct GetTypeInfo<const gdpp::runtime::ScriptTypedDictionary<Key, Value>&>
    : GetTypeInfo<gdpp::runtime::ScriptTypedDictionary<Key, Value>> {};

template <typename Key, typename Value>
struct VariantCaster<gdpp::runtime::ScriptTypedDictionary<Key, Value>> {
    static gdpp::runtime::ScriptTypedDictionary<Key, Value> cast(const Variant& value) {
        return gdpp::runtime::ScriptTypedDictionary<Key, Value>(value);
    }
};

template <typename Key, typename Value>
struct VariantCaster<const gdpp::runtime::ScriptTypedDictionary<Key, Value>&>
    : VariantCaster<gdpp::runtime::ScriptTypedDictionary<Key, Value>> {};

template <typename Key, typename Value>
struct PtrToArg<gdpp::runtime::ScriptTypedDictionary<Key, Value>> {
    static gdpp::runtime::ScriptTypedDictionary<Key, Value> convert(const void* value) {
        return gdpp::runtime::ScriptTypedDictionary<Key, Value>(
            *reinterpret_cast<const Dictionary*>(value));
    }

    using EncodeT = Dictionary;

    static void encode(const gdpp::runtime::ScriptTypedDictionary<Key, Value>& value,
                       void* output) {
        *reinterpret_cast<Dictionary*>(output) = value;
    }
};

template <typename Key, typename Value>
struct PtrToArg<const gdpp::runtime::ScriptTypedDictionary<Key, Value>&>
    : PtrToArg<gdpp::runtime::ScriptTypedDictionary<Key, Value>> {};

} // namespace godot
