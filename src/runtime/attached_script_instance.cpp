#include "gdpp/runtime/attached_script.hpp"
#include "gdpp/runtime/variant_ops.hpp"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/gd_script.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/signal.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <gdextension_interface.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gdpp::runtime {
namespace {

class AttachedScriptInstance final {
  public:
    AttachedScriptInstance(godot::Object* attached_owner,
                           godot::Ref<AttachedCompiledScript> attached_script,
                           AttachedScriptDescriptor attached_descriptor,
                           godot::Ref<AttachedScriptBehavior> attached_behavior)
        : owner{attached_owner}, script{std::move(attached_script)},
          descriptor{std::move(attached_descriptor)}, behavior{std::move(attached_behavior)} {
        if (behavior.is_null()) {
            for (const auto& property : descriptor.properties) {
                if (property.has_default)
                    metadata_values[property.info.name] = property.default_value;
            }
        }
    }

    ~AttachedScriptInstance() {
        if (behavior.is_valid())
            behavior->detach_owner();
        std::lock_guard<std::mutex> lock{instances_mutex()};
        const auto found = instances().find(owner);
        if (found != instances().end() && found->second == this)
            instances().erase(found);
    }

    static std::mutex& instances_mutex() {
        static std::mutex value;
        return value;
    }

    static std::unordered_map<godot::Object*, AttachedScriptInstance*>& instances() {
        static std::unordered_map<godot::Object*, AttachedScriptInstance*> value;
        return value;
    }

    godot::Object* owner;
    godot::Ref<AttachedCompiledScript> script;
    AttachedScriptDescriptor descriptor;
    godot::Ref<AttachedScriptBehavior> behavior;
    godot::Dictionary metadata_values;
    godot::Dictionary editor_stored_properties;
    bool has_editor_storage_state{false};
    void* godot_instance_handle{nullptr};
};

struct PendingConstruction {
    godot::String source_path;
    const godot::Array* arguments{nullptr};
    bool skip_initializer{false};
    bool consumed{false};
};

thread_local PendingConstruction* pending_construction{nullptr};

std::mutex& native_bind_mutex() {
    static std::mutex value;
    return value;
}

std::unordered_map<std::string, GDExtensionMethodBindPtr>& native_bind_cache() {
    static std::unordered_map<std::string, GDExtensionMethodBindPtr> value;
    return value;
}

GDExtensionInterfaceObjectSetScriptInstance object_set_script_instance_interface() {
    // The project library is compiled against the 4.4 compatibility surface, whose generated
    // godot-cpp loader intentionally omits interfaces introduced later. Resolve this optional 4.5
    // hook through Godot's canonical procedure table so one binary remains ABI-compatible across
    // every supported 4.x minor.
    static const auto value = reinterpret_cast<GDExtensionInterfaceObjectSetScriptInstance>(
        godot::gdextension_interface::get_proc_address
            ? godot::gdextension_interface::get_proc_address("object_set_script_instance")
            : nullptr);
    return value;
}

std::string native_bind_key(const godot::StringName& native_class, const godot::StringName& method,
                            const std::uint32_t compatibility_hash) {
    const godot::CharString class_utf8 = godot::String{native_class}.utf8();
    const godot::CharString method_utf8 = godot::String{method}.utf8();
    return std::string{class_utf8.get_data(), static_cast<std::size_t>(class_utf8.length())} +
           "\n" +
           std::string{method_utf8.get_data(), static_cast<std::size_t>(method_utf8.length())} +
           "\n" + std::to_string(compatibility_hash);
}

const AttachedScriptProperty* find_property(const AttachedScriptInstance* instance,
                                            const godot::StringName& name) {
    const auto found =
        std::find_if(instance->descriptor.properties.begin(), instance->descriptor.properties.end(),
                     [&](const auto& item) { return item.info.name == name; });
    return found == instance->descriptor.properties.end() ? nullptr : &*found;
}

const godot::MethodInfo* find_method(const AttachedScriptInstance* instance,
                                     const godot::StringName& name) {
    const auto found =
        std::find_if(instance->descriptor.methods.begin(), instance->descriptor.methods.end(),
                     [&](const auto& item) { return item.name == name; });
    return found == instance->descriptor.methods.end() ? nullptr : &*found;
}

const AttachedScriptMethodDispatch* find_method_dispatch(const AttachedScriptInstance* instance,
                                                         const godot::StringName& name) {
    const auto found = std::find_if(instance->descriptor.method_dispatches.begin(),
                                    instance->descriptor.method_dispatches.end(),
                                    [&](const auto& item) { return item.name == name; });
    return found == instance->descriptor.method_dispatches.end() ? nullptr : &*found;
}

bool has_signal(const AttachedScriptInstance* instance, const godot::StringName& name) {
    return std::any_of(instance->descriptor.signals.begin(), instance->descriptor.signals.end(),
                       [&](const auto& item) { return item.name == name; });
}

godot::Variant call_behavior(AttachedScriptInstance* instance,
                             const AttachedScriptMethodDispatch* dispatch,
                             const godot::Variant** arguments, std::int64_t argument_count,
                             GDExtensionCallError& error) {
    if (instance->behavior.is_null() || !dispatch || !dispatch->call) {
        error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return {};
    }
    godot::Variant result;
    dispatch->call(nullptr, instance->behavior.ptr(),
                   reinterpret_cast<const GDExtensionConstVariantPtr*>(arguments), argument_count,
                   result._native_ptr(), &error);
    return result;
}

godot::Variant call_behavior(AttachedScriptInstance* instance, const godot::StringName& method,
                             const godot::Variant** arguments, std::int64_t argument_count,
                             GDExtensionCallError& error) {
    return call_behavior(instance, find_method_dispatch(instance, method), arguments,
                         argument_count, error);
}

GDExtensionVariantPtr copy_variant(const godot::Variant& value) {
    return memnew(godot::Variant(value));
}

void destroy_variant(GDExtensionVariantPtr value) {
    memdelete(reinterpret_cast<godot::Variant*>(value));
}

GDExtensionPropertyInfo copy_property_info(const godot::PropertyInfo& info) {
    return {static_cast<GDExtensionVariantType>(info.type), memnew(godot::StringName(info.name)),
            memnew(godot::StringName(info.class_name)),     info.hint,
            memnew(godot::String(info.hint_string)),        info.usage};
}

void destroy_property_info(const GDExtensionPropertyInfo& info) {
    memdelete(reinterpret_cast<godot::StringName*>(info.name));
    memdelete(reinterpret_cast<godot::StringName*>(info.class_name));
    memdelete(reinterpret_cast<godot::String*>(info.hint_string));
}

void destroy_property_infos(const GDExtensionPropertyInfo* values, std::uint32_t count) {
    if (!values)
        return;
    for (std::uint32_t index = 0; index < count; ++index)
        destroy_property_info(values[index]);
    godot::memdelete_arr(values);
}

GDExtensionMethodInfo copy_method_info(const godot::MethodInfo& info) {
    auto* arguments = info.arguments.is_empty()
                          ? nullptr
                          : memnew_arr(GDExtensionPropertyInfo, info.arguments.size());
    for (std::uint32_t index = 0; index < info.arguments.size(); ++index)
        arguments[index] = copy_property_info(info.arguments[index]);

    auto** defaults = info.default_arguments.is_empty()
                          ? nullptr
                          : memnew_arr(GDExtensionVariantPtr, info.default_arguments.size());
    for (std::uint32_t index = 0; index < info.default_arguments.size(); ++index)
        defaults[index] = copy_variant(info.default_arguments[index]);

    return {memnew(godot::StringName(info.name)),
            copy_property_info(info.return_val),
            info.flags,
            info.id,
            info.arguments.size(),
            arguments,
            info.default_arguments.size(),
            defaults};
}

void destroy_method_info(const GDExtensionMethodInfo& info) {
    memdelete(reinterpret_cast<godot::StringName*>(info.name));
    destroy_property_info(info.return_value);
    destroy_property_infos(info.arguments, info.argument_count);
    if (info.default_arguments) {
        for (std::uint32_t index = 0; index < info.default_argument_count; ++index)
            destroy_variant(info.default_arguments[index]);
        godot::memdelete_arr(info.default_arguments);
    }
}

void destroy_method_infos(const GDExtensionMethodInfo* values, std::uint32_t count) {
    if (!values)
        return;
    for (std::uint32_t index = 0; index < count; ++index)
        destroy_method_info(values[index]);
    godot::memdelete_arr(values);
}

void publish_legacy_construction_signals(godot::Object* owner,
                                         const AttachedScriptDescriptor& descriptor) {
    // Godot 4.4 does not expose object_set_script_instance(), so ScriptExtension implementations
    // cannot publish their ScriptInstance before _instance_create() returns. Keep that legacy
    // engine functional by publishing owner-local construction signals. Godot 4.5 and newer use
    // the exact pre-attached ScriptInstance path below and never enter here.
    for (const auto& signal : descriptor.signals) {
        if (owner->has_signal(signal.name))
            continue;
        godot::Array arguments;
        arguments.resize(signal.arguments.size());
        for (std::uint32_t index = 0; index < signal.arguments.size(); ++index)
            arguments[index] = static_cast<godot::Dictionary>(signal.arguments[index]);
        owner->add_user_signal(godot::String{signal.name}, arguments);
    }
}

GDExtensionBool instance_set(AttachedScriptInstance* instance, const godot::StringName* name,
                             const godot::Variant* value) {
    if (instance->behavior.is_null()) {
        if (!find_property(instance, *name))
            return false;
        instance->metadata_values[*name] = *value;
        return true;
    }
    if (find_method(instance, "_set")) {
        const godot::Variant property_name{*name};
        const godot::Variant* arguments[]{&property_name, value};
        GDExtensionCallError error{};
        const auto handled = call_behavior(instance, "_set", arguments, 2, error);
        if (error.error == GDEXTENSION_CALL_OK && static_cast<bool>(handled))
            return true;
    }
    const auto* property = find_property(instance, *name);
    if (!property)
        return false;
    if (property->setter)
        return property->setter(instance->behavior.ptr(), *value);
    return godot::ClassDB::class_set_property(instance->behavior.ptr(), *name, *value) == godot::OK;
}

GDExtensionBool instance_get(AttachedScriptInstance* instance, const godot::StringName* name,
                             godot::Variant* result) {
    if (instance->behavior.is_null()) {
        const auto* property = find_property(instance, *name);
        if (property) {
            *result = instance->metadata_values.get(
                *name, property->has_default ? property->default_value : godot::Variant{});
            return true;
        }
        if (find_method(instance, *name)) {
            *result = godot::Callable(instance->owner, *name);
            return true;
        }
        if (has_signal(instance, *name)) {
            *result = godot::Signal(instance->owner, *name);
            return true;
        }
        return false;
    }
    if (find_method(instance, "_get")) {
        const godot::Variant property_name{*name};
        const godot::Variant* arguments[]{&property_name};
        GDExtensionCallError error{};
        const auto value = call_behavior(instance, "_get", arguments, 1, error);
        if (error.error == GDEXTENSION_CALL_OK && value.get_type() != godot::Variant::NIL) {
            *result = value;
            return true;
        }
    }
    if (const auto* property = find_property(instance, *name)) {
        *result = property->getter
                      ? property->getter(instance->behavior.ptr())
                      : godot::ClassDB::class_get_property(instance->behavior.ptr(), *name);
        return true;
    }
    if (find_method(instance, *name)) {
        *result = godot::Callable(instance->owner, *name);
        return true;
    }
    if (has_signal(instance, *name)) {
        *result = godot::Signal(instance->owner, *name);
        return true;
    }
    return false;
}

const GDExtensionPropertyInfo* instance_property_list(AttachedScriptInstance* instance,
                                                      std::uint32_t* count) {
    *count = static_cast<std::uint32_t>(instance->descriptor.properties.size());
    if (*count == 0)
        return nullptr;
    auto* result = memnew_arr(GDExtensionPropertyInfo, *count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        auto info = instance->descriptor.properties[index].info;
        if (instance->behavior.is_null() && instance->has_editor_storage_state &&
            !instance->editor_stored_properties.has(info.name)) {
            // PackedScene queries the property list as well as get_property_state(). Keep the
            // field dynamically visible for copy/validation, but remove STORAGE for fields that
            // were not present in the source SceneState.
            info.usage &= ~static_cast<std::uint32_t>(godot::PROPERTY_USAGE_STORAGE);
        }
        result[index] = copy_property_info(info);
    }
    return result;
}

void instance_free_property_list(AttachedScriptInstance*, const GDExtensionPropertyInfo* values,
                                 std::uint32_t count) {
    destroy_property_infos(values, count);
}

GDExtensionBool instance_property_can_revert(AttachedScriptInstance* instance,
                                             const godot::StringName* name) {
    const auto* property = find_property(instance, *name);
    return property && property->has_default;
}

GDExtensionBool instance_property_get_revert(AttachedScriptInstance* instance,
                                             const godot::StringName* name,
                                             godot::Variant* result) {
    const auto* property = find_property(instance, *name);
    if (!property || !property->has_default)
        return false;
    *result = property->default_value;
    return true;
}

GDExtensionObjectPtr instance_owner(AttachedScriptInstance* instance) {
    return instance->owner->_owner;
}

void instance_property_state(AttachedScriptInstance* instance,
                             GDExtensionScriptInstancePropertyStateAdd add, void* userdata) {
    for (const auto& property : instance->descriptor.properties) {
        if ((property.info.usage & godot::PROPERTY_USAGE_STORAGE) == 0)
            continue;
        if (instance->behavior.is_null() && instance->has_editor_storage_state &&
            !instance->editor_stored_properties.has(property.info.name)) {
            // Export-time metadata descriptors intentionally do not evaluate customer field
            // initializers. Only source properties recorded in the serialized scene/resource
            // are emitted; all others are initialized by the target behavior constructor.
            continue;
        }
        godot::Variant value;
        if (instance->behavior.is_valid()) {
            value = property.getter ? property.getter(instance->behavior.ptr())
                                    : godot::ClassDB::class_get_property(instance->behavior.ptr(),
                                                                         property.info.name);
        } else {
            value = instance->metadata_values.get(property.info.name, property.has_default
                                                                          ? property.default_value
                                                                          : godot::Variant{});
        }
        add(&property.info.name, &value, userdata);
    }
}

const GDExtensionMethodInfo* instance_method_list(AttachedScriptInstance* instance,
                                                  std::uint32_t* count) {
    *count = static_cast<std::uint32_t>(instance->descriptor.methods.size());
    if (*count == 0)
        return nullptr;
    auto* result = memnew_arr(GDExtensionMethodInfo, *count);
    for (std::uint32_t index = 0; index < *count; ++index)
        result[index] = copy_method_info(instance->descriptor.methods[index]);
    return result;
}

void instance_free_method_list(AttachedScriptInstance*, const GDExtensionMethodInfo* values,
                               std::uint32_t count) {
    destroy_method_infos(values, count);
}

GDExtensionVariantType instance_property_type(AttachedScriptInstance* instance,
                                              const godot::StringName* name,
                                              GDExtensionBool* valid) {
    const auto* property = find_property(instance, *name);
    *valid = property != nullptr;
    return property ? static_cast<GDExtensionVariantType>(property->info.type)
                    : GDEXTENSION_VARIANT_TYPE_NIL;
}

GDExtensionBool instance_has_method(AttachedScriptInstance* instance,
                                    const godot::StringName* name) {
    return find_method(instance, *name) != nullptr;
}

GDExtensionInt instance_method_argument_count(AttachedScriptInstance* instance,
                                              const godot::StringName* name,
                                              GDExtensionBool* valid) {
    const auto* method = find_method(instance, *name);
    *valid = method != nullptr;
    return method ? static_cast<GDExtensionInt>(method->arguments.size()) : 0;
}

void instance_call(AttachedScriptInstance* instance, const godot::StringName* method,
                   const godot::Variant** arguments, GDExtensionInt argument_count,
                   godot::Variant* result, GDExtensionCallError* error) {
    // GDScript runs every @implicit_ready() initializer from the base script to the most-derived
    // script before dispatching _ready(). Keep that lifecycle independent of whether the customer
    // declared _ready() and repeat it when request_ready() causes another ready notification.
    if (*method == godot::StringName{"_ready"} && instance->behavior.is_valid()) {
        // @implicit_ready is one script function in GDScript. A failed initializer stops the
        // remaining base-to-derived initializer chain, but it must neither suppress _ready nor
        // contaminate whichever generated function caused the node to enter the tree.
        ScriptFunctionScope onready_initialization_scope;
        instance->behavior->_gdpp_initialize_onready();
    }
    const auto* dispatch = find_method_dispatch(instance, *method);
    if (!dispatch) {
        error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return;
    }
    if (instance->behavior.is_null()) {
        error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
        return;
    }
    *result = call_behavior(instance, dispatch, arguments, argument_count, *error);
}

void instance_notification(AttachedScriptInstance* instance, std::int32_t what,
                           GDExtensionBool reversed) {
    if (instance->behavior.is_valid())
        instance->behavior->_gdpp_dispatch_notification(what, reversed);
}

void instance_to_string(AttachedScriptInstance* instance, GDExtensionBool* valid,
                        godot::String* result) {
    if (instance->behavior.is_null() || !find_method(instance, "_to_string")) {
        *valid = false;
        return;
    }
    GDExtensionCallError error{};
    const auto value = call_behavior(instance, "_to_string", nullptr, 0, error);
    *valid = error.error == GDEXTENSION_CALL_OK && value.get_type() == godot::Variant::STRING;
    if (*valid)
        *result = value;
}

void instance_refcount_incremented(AttachedScriptInstance*) {}

GDExtensionBool instance_refcount_decremented(AttachedScriptInstance*) { return true; }

GDExtensionObjectPtr instance_script(AttachedScriptInstance* instance) {
    return instance->script.ptr()->_owner;
}

GDExtensionBool instance_is_placeholder(AttachedScriptInstance*) { return false; }

GDExtensionScriptLanguagePtr instance_language(AttachedScriptInstance*) {
    const auto* language = AttachedCompiledLanguage::get_singleton();
    return language ? language->_owner : nullptr;
}

void instance_free(AttachedScriptInstance* instance) { godot::memdelete(instance); }

const GDExtensionScriptInstanceInfo3& script_instance_info() {
    static const GDExtensionScriptInstanceInfo3 value{
        reinterpret_cast<GDExtensionScriptInstanceSet>(instance_set),
        reinterpret_cast<GDExtensionScriptInstanceGet>(instance_get),
        reinterpret_cast<GDExtensionScriptInstanceGetPropertyList>(instance_property_list),
        reinterpret_cast<GDExtensionScriptInstanceFreePropertyList2>(instance_free_property_list),
        nullptr,
        reinterpret_cast<GDExtensionScriptInstancePropertyCanRevert>(instance_property_can_revert),
        reinterpret_cast<GDExtensionScriptInstancePropertyGetRevert>(instance_property_get_revert),
        reinterpret_cast<GDExtensionScriptInstanceGetOwner>(instance_owner),
        reinterpret_cast<GDExtensionScriptInstanceGetPropertyState>(instance_property_state),
        reinterpret_cast<GDExtensionScriptInstanceGetMethodList>(instance_method_list),
        reinterpret_cast<GDExtensionScriptInstanceFreeMethodList2>(instance_free_method_list),
        reinterpret_cast<GDExtensionScriptInstanceGetPropertyType>(instance_property_type),
        nullptr,
        reinterpret_cast<GDExtensionScriptInstanceHasMethod>(instance_has_method),
        reinterpret_cast<GDExtensionScriptInstanceGetMethodArgumentCount>(
            instance_method_argument_count),
        reinterpret_cast<GDExtensionScriptInstanceCall>(instance_call),
        reinterpret_cast<GDExtensionScriptInstanceNotification2>(instance_notification),
        reinterpret_cast<GDExtensionScriptInstanceToString>(instance_to_string),
        reinterpret_cast<GDExtensionScriptInstanceRefCountIncremented>(
            instance_refcount_incremented),
        reinterpret_cast<GDExtensionScriptInstanceRefCountDecremented>(
            instance_refcount_decremented),
        reinterpret_cast<GDExtensionScriptInstanceGetScript>(instance_script),
        reinterpret_cast<GDExtensionScriptInstanceIsPlaceholder>(instance_is_placeholder),
        nullptr,
        nullptr,
        reinterpret_cast<GDExtensionScriptInstanceGetLanguage>(instance_language),
        reinterpret_cast<GDExtensionScriptInstanceFree>(instance_free),
    };
    return value;
}

} // namespace

void* AttachedCompiledScript::_instance_create(godot::Object* object) const {
    const auto metadata = descriptor();
    if (!metadata || metadata->abstract || !object ||
        !object->is_class(metadata->native_base_type)) {
        return nullptr;
    }

    godot::Ref<AttachedScriptBehavior> behavior;
    if (metadata->factory) {
        behavior = metadata->factory();
        if (behavior.is_null())
            return nullptr;
        behavior->attach_owner(object);
    } else {
        const auto* engine = godot::Engine::get_singleton();
        if (!metadata->editor_metadata_only || !engine || !engine->is_editor_hint())
            return nullptr;
    }

    godot::Ref<AttachedCompiledScript> script{const_cast<AttachedCompiledScript*>(this)};
    auto* instance = memnew(AttachedScriptInstance(object, std::move(script), *metadata, behavior));
    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock{AttachedScriptInstance::instances_mutex()};
        inserted = AttachedScriptInstance::instances().emplace(object, instance).second;
    }
    if (!inserted) {
        // Destruction detaches the speculative behavior and must run without the registry mutex.
        // It also verifies pointer identity before erasing so the incumbent instance remains
        // authoritative when a duplicate attachment races with teardown or editor reload.
        godot::memdelete(instance);
        return nullptr;
    }
    if (behavior.is_null()) {
        instance->godot_instance_handle = godot::gdextension_interface::script_instance_create3(
            &script_instance_info(), instance);
        if (!instance->godot_instance_handle) {
            godot::memdelete(instance);
            return nullptr;
        }
        return instance->godot_instance_handle;
    }

    // GDScript attaches its ScriptInstance before evaluating fields and _init(). Reproduce that
    // ordering so custom signals, properties, methods and reflection are all available throughout
    // construction. Godot added the public GDExtension attachment hook in 4.5; the returned handle
    // is the same one Object::set_script() stores when this callback returns, so no second instance
    // is created and Object::set_script_instance() treats the outer assignment as a no-op.
    const auto object_set_script_instance = object_set_script_instance_interface();
    const bool can_pre_attach = object_set_script_instance != nullptr;
    if (can_pre_attach) {
        instance->godot_instance_handle =
            godot::gdextension_interface::script_instance_create3(&script_instance_info(), instance);
        if (!instance->godot_instance_handle) {
            godot::memdelete(instance);
            return nullptr;
        }
        object_set_script_instance(object->_owner, instance->godot_instance_handle);
    } else {
        publish_legacy_construction_signals(object, *metadata);
    }

    const bool matches_construction =
        pending_construction && pending_construction->source_path == metadata->source_path;
    const bool skip_initializer = matches_construction && pending_construction->skip_initializer;
    if (matches_construction)
        pending_construction->consumed = true;

    if (!skip_initializer) {
        // The complete base-to-derived @implicit_new chain shares one fault state. Its failure
        // preserves fields assigned before the failing expression, stops later field
        // initializers, and remains isolated from both the constructor caller and _init.
        ScriptFunctionScope instance_initialization_scope;
        behavior->_gdpp_initialize_instance();
    }

    if (!skip_initializer && find_method(instance, "_init")) {
        const godot::Array empty_arguments;
        const auto* arguments = &empty_arguments;
        if (matches_construction) {
            arguments = pending_construction->arguments;
        }
        std::vector<const godot::Variant*> argument_pointers;
        argument_pointers.reserve(static_cast<std::size_t>(arguments->size()));
        for (std::int64_t index = 0; index < arguments->size(); ++index)
            argument_pointers.push_back(&(*arguments)[index]);
        GDExtensionCallError call_error{};
        call_behavior(instance, "_init", argument_pointers.data(), arguments->size(), call_error);
        if (call_error.error != GDEXTENSION_CALL_OK) {
            if (can_pre_attach) {
                // Clearing the owner destroys the ScriptInstance wrapper, whose free callback owns
                // and destroys `instance`. Deleting the payload directly would leave Object with a
                // dangling wrapper until the outer set_script() call resumed.
                object_set_script_instance(object->_owner, nullptr);
            } else {
                godot::memdelete(instance);
            }
            return nullptr;
        }
    }
    if (!can_pre_attach) {
        instance->godot_instance_handle =
            godot::gdextension_interface::script_instance_create3(&script_instance_info(), instance);
        if (!instance->godot_instance_handle) {
            godot::memdelete(instance);
            return nullptr;
        }
    }
    return instance->godot_instance_handle;
}

void* AttachedCompiledScript::_placeholder_instance_create(godot::Object*) const { return nullptr; }

bool AttachedCompiledScript::_instance_has(godot::Object* object) const {
    std::lock_guard<std::mutex> lock{AttachedScriptInstance::instances_mutex()};
    const auto found = AttachedScriptInstance::instances().find(object);
    return found != AttachedScriptInstance::instances().end() &&
           found->second->script.ptr() == this;
}

bool is_attached_script_instance(godot::Object* object, const godot::String& source_path) {
    if (!object || source_path.is_empty())
        return false;
    godot::String current_path;
    {
        std::lock_guard<std::mutex> lock{AttachedScriptInstance::instances_mutex()};
        const auto found = AttachedScriptInstance::instances().find(object);
        if (found == AttachedScriptInstance::instances().end())
            return false;
        current_path = found->second->descriptor.source_path.simplify_path();
    }

    const auto expected = source_path.simplify_path();
    std::vector<godot::String> visited;
    while (!current_path.is_empty()) {
        if (current_path == expected)
            return true;
        if (std::find(visited.begin(), visited.end(), current_path) != visited.end())
            return false;
        visited.push_back(current_path);
        const auto descriptor = find_attached_script(current_path);
        if (!descriptor)
            return false;
        current_path = descriptor->base_script_path.simplify_path();
    }
    return false;
}

void* attached_script_instance_handle(godot::Object* object) {
    if (!object)
        return nullptr;
    std::lock_guard<std::mutex> lock{AttachedScriptInstance::instances_mutex()};
    const auto found = AttachedScriptInstance::instances().find(object);
    return found == AttachedScriptInstance::instances().end()
               ? nullptr
               : found->second->godot_instance_handle;
}

godot::String attached_script_instance_source_path(godot::Object* object) {
    if (!object)
        return {};
    std::lock_guard<std::mutex> lock{AttachedScriptInstance::instances_mutex()};
    const auto found = AttachedScriptInstance::instances().find(object);
    return found == AttachedScriptInstance::instances().end()
               ? godot::String{}
               : found->second->descriptor.source_path;
}

bool set_attached_editor_storage_state(godot::Object* object,
                                       const godot::PackedStringArray& stored_properties) {
    if (!object)
        return false;
    std::lock_guard<std::mutex> lock{AttachedScriptInstance::instances_mutex()};
    const auto found = AttachedScriptInstance::instances().find(object);
    if (found == AttachedScriptInstance::instances().end() || found->second->behavior.is_valid() ||
        !found->second->descriptor.editor_metadata_only) {
        return false;
    }
    auto* instance = found->second;
    instance->editor_stored_properties.clear();
    for (std::int64_t index = 0; index < stored_properties.size(); ++index)
        instance->editor_stored_properties[godot::StringName{stored_properties[index]}] = true;
    instance->has_editor_storage_state = true;
    return true;
}

godot::Object* cast_attached_script(const godot::Variant& value, const godot::String& source_path) {
    auto* object = value.get_validated_object();
    return is_attached_script_instance(object, source_path) ? object : nullptr;
}

void detach_all_attached_script_instances() {
    // ScriptInstance::free erases its payload from `instances()`. Never hold the registry mutex
    // while asking Godot to clear an instance, because the callback must acquire the same mutex.
    // Taking one owner at a time also tolerates arbitrary customer destructor order and keeps raw
    // pointers inside the shortest possible main-thread shutdown window.
    for (;;) {
        godot::Object* owner = nullptr;
        AttachedScriptInstance* expected = nullptr;
        {
            std::lock_guard<std::mutex> lock{AttachedScriptInstance::instances_mutex()};
            if (AttachedScriptInstance::instances().empty())
                break;
            const auto entry = AttachedScriptInstance::instances().begin();
            owner = entry->first;
            expected = entry->second;
        }

        if (const auto object_set_script_instance = object_set_script_instance_interface()) {
            object_set_script_instance(owner->_owner, nullptr);
        } else {
            // Godot 4.4 predates object_set_script_instance(). Object::set_script() is its
            // supported teardown path and synchronously invokes the ScriptInstance free callback.
            owner->set_script(godot::Variant{});
        }

        std::lock_guard<std::mutex> lock{AttachedScriptInstance::instances_mutex()};
        const auto remaining = AttachedScriptInstance::instances().find(owner);
        if (remaining != AttachedScriptInstance::instances().end() &&
            remaining->second == expected) {
            godot::UtilityFunctions::push_error(
                "GDPP: Godot did not release a compiled ScriptInstance during extension "
                "shutdown");
            break;
        }
    }

    pending_construction = nullptr;
    std::lock_guard<std::mutex> lock{native_bind_mutex()};
    native_bind_cache().clear();
}

godot::Ref<godot::Script> strict_gdscript_storage(const godot::Variant& value,
                                                  const ScriptSourceLocation& location) {
    if (script_function_failed())
        return {};
    if (value.get_type() == godot::Variant::NIL)
        return {};
    if (value.get_type() != godot::Variant::OBJECT) {
        report_script_failure(godot::String{"Cannot assign "} + describe_variant_type(value) +
                                  " to GDScript.",
                              location);
        return {};
    }
    auto* object = value.get_validated_object();
    if (!object)
        return {};
    auto* script = godot::Object::cast_to<godot::Script>(object);
    if (script &&
        (godot::Object::cast_to<godot::GDScript>(script) ||
         godot::Object::cast_to<AttachedCompiledScript>(script))) {
        return godot::Ref<godot::Script>(script);
    }
    report_script_failure(godot::String{"Cannot assign object of type "} +
                              godot::String{object->get_class()} + " to GDScript.",
                          location);
    return {};
}

godot::Object* strict_attached_script_storage(const godot::Variant& value,
                                              const godot::String& source_path,
                                              const ScriptSourceLocation& location) {
    if (value.get_type() == godot::Variant::NIL)
        return nullptr;
    auto* object = strict_native_object_storage(value, godot::Object::get_class_static(), location);
    if (!object || script_function_failed())
        return nullptr;
    if (!is_attached_script_instance(object, source_path)) {
        report_script_failure(godot::String{"Cannot assign object of type "} +
                                  godot::String{object->get_class()} + " to script type " +
                                  source_path + ".",
                              location);
        return nullptr;
    }
    return object;
}

godot::Variant instantiate_attached_script(const godot::String& source_path,
                                           const godot::Array& arguments, godot::String* error,
                                           const bool skip_initializer) {
    const auto fail = [error](const godot::String& message) {
        if (error)
            *error = message;
        else
            godot::UtilityFunctions::push_error("GDPP: " + message);
    };
    const auto descriptor = resolve_attached_script(source_path, error);
    if (!descriptor) {
        if (!error)
            godot::UtilityFunctions::push_error("GDPP: attached script is not registered: " +
                                                source_path);
        return {};
    }
    auto* class_db = godot::ClassDBSingleton::get_singleton();
    if (!class_db || !class_db->can_instantiate(descriptor->native_base_type)) {
        fail("attached native base is not instantiable: " +
             godot::String{descriptor->native_base_type});
        return {};
    }

    godot::Variant instance = class_db->instantiate(descriptor->native_base_type);
    godot::Object* object = instance;
    if (!object) {
        fail("ClassDB failed to instantiate attached native base: " +
             godot::String{descriptor->native_base_type});
        return {};
    }

    const auto script = attached_script_resource(descriptor->source_path, error);
    if (script.is_null()) {
        if (!error)
            godot::UtilityFunctions::push_error(
                "GDPP: failed to materialize attached script resource: " + descriptor->source_path);
        return {};
    }
    PendingConstruction construction{descriptor->source_path, &arguments, skip_initializer, false};
    auto* previous = pending_construction;
    pending_construction = &construction;
    object->set_script(script);
    pending_construction = previous;
    if (!script->_instance_has(object) || !construction.consumed) {
        fail("failed to attach or initialize compiled script: " + descriptor->source_path);
        // ClassDB returns a strong Variant for RefCounted instances, but ordinary Objects remain
        // caller-owned. Release either ownership model before reporting construction failure.
        if (godot::Object::cast_to<godot::RefCounted>(object))
            instance = godot::Variant{};
        else
            godot::memdelete(object);
        return {};
    }
    return instance;
}

godot::Dictionary attached_instance_to_dictionary(godot::Object* object, godot::String* error) {
    if (!object) {
        if (error)
            *error = "Not a script with an instance.";
        return {};
    }

    AttachedScriptDescriptor descriptor;
    godot::Ref<AttachedScriptBehavior> behavior;
    godot::Dictionary metadata_values;
    {
        std::lock_guard<std::mutex> lock{AttachedScriptInstance::instances_mutex()};
        const auto found = AttachedScriptInstance::instances().find(object);
        if (found == AttachedScriptInstance::instances().end()) {
            if (error)
                *error = "Not a script with an instance.";
            return {};
        }
        descriptor = found->second->descriptor;
        behavior = found->second->behavior;
        metadata_values = found->second->metadata_values;
    }

    const auto separator = descriptor.source_path.find("::");
    const auto root_path =
        separator < 0 ? descriptor.source_path : descriptor.source_path.substr(0, separator);
    if (root_path.is_empty() || !root_path.begins_with("res://")) {
        if (error)
            *error = "Not based on a resource file.";
        return {};
    }

    godot::Dictionary result;
    result["@path"] = root_path;
    result["@subpath"] =
        separator < 0
            ? godot::NodePath{}
            : godot::NodePath{descriptor.source_path.substr(separator + 2).replace(".", "/")};
    for (const auto& property : descriptor.properties) {
        if (result.has(property.info.name))
            continue;
        if (behavior.is_valid()) {
            if (!property.storage_getter) {
                if (error)
                    *error = "Script member has no raw storage getter: " +
                             godot::String{property.info.name};
                return {};
            }
            result[property.info.name] = property.storage_getter(behavior.ptr());
        } else {
            result[property.info.name] = metadata_values.get(
                property.info.name,
                property.has_default ? property.default_value : godot::Variant{});
        }
    }
    return result;
}

godot::Variant dictionary_to_attached_instance(const godot::Dictionary& dictionary,
                                               godot::String* error) {
    const auto fail = [error](const godot::String& message) {
        if (error)
            *error = message;
    };
    if (!dictionary.has("@path")) {
        fail("Invalid instance dictionary format (missing @path).");
        return {};
    }
    const godot::Variant path_value = dictionary["@path"];
    if (path_value.get_type() != godot::Variant::STRING &&
        path_value.get_type() != godot::Variant::STRING_NAME) {
        fail("Invalid instance dictionary format (@path is not a resource path).");
        return {};
    }
    auto source_path = static_cast<godot::String>(path_value).simplify_path();
    if (source_path.is_empty() || !source_path.begins_with("res://")) {
        fail("Invalid instance dictionary format (@path is not a project resource).");
        return {};
    }

    if (dictionary.has("@subpath")) {
        const godot::Variant subpath_value = dictionary["@subpath"];
        godot::NodePath subpath;
        if (subpath_value.get_type() == godot::Variant::NODE_PATH) {
            subpath = subpath_value.operator godot::NodePath();
        } else if (subpath_value.get_type() == godot::Variant::STRING ||
                   subpath_value.get_type() == godot::Variant::STRING_NAME) {
            subpath = godot::NodePath{static_cast<godot::String>(subpath_value)};
        } else if (subpath_value.get_type() != godot::Variant::NIL) {
            fail("Invalid instance dictionary format (@subpath is not a NodePath).");
            return {};
        }
        for (std::int64_t index = 0; index < subpath.get_name_count(); ++index) {
            source_path += index == 0 ? "::" : ".";
            source_path += godot::String{subpath.get_name(index)};
        }
    }

    godot::String construction_error;
    const godot::Array no_arguments;
    auto result = instantiate_attached_script(source_path, no_arguments, &construction_error, true);
    if (!construction_error.is_empty() || result.get_type() != godot::Variant::OBJECT ||
        !result.get_validated_object()) {
        fail(construction_error.is_empty()
                 ? "Invalid instance dictionary format (can't load script at @path)."
                 : construction_error);
        return {};
    }

    const auto discard_result = [&result]() {
        auto* object = result.get_validated_object();
        if (!object)
            return;
        if (godot::Object::cast_to<godot::RefCounted>(object))
            result = godot::Variant{};
        else {
            godot::memdelete(object);
            result = godot::Variant{};
        }
    };
    AttachedScriptDescriptor descriptor;
    godot::Ref<AttachedScriptBehavior> behavior;
    bool restorable = false;
    {
        std::lock_guard<std::mutex> lock{AttachedScriptInstance::instances_mutex()};
        const auto found = AttachedScriptInstance::instances().find(result.get_validated_object());
        if (found != AttachedScriptInstance::instances().end() &&
            found->second->behavior.is_valid()) {
            descriptor = found->second->descriptor;
            behavior = found->second->behavior;
            restorable = true;
        }
    }
    if (!restorable) {
        // Releasing either RefCounted or caller-owned objects can destroy the ScriptInstance and
        // re-enter the registry. Never perform that teardown while holding instances_mutex().
        fail("Cannot restore a compiled script instance.");
        discard_result();
        return {};
    }
    for (const auto& property : descriptor.properties) {
        if (!dictionary.has(property.info.name))
            continue;
        if (!property.storage_setter ||
            !property.storage_setter(behavior.ptr(), dictionary[property.info.name])) {
            fail("Cannot restore script member: " + godot::String{property.info.name});
            discard_result();
            return {};
        }
    }
    return result;
}

godot::Variant call_attached_native_base_raw(godot::Object* owner,
                                             const godot::StringName& native_class,
                                             const godot::StringName& method,
                                             const std::uint32_t compatibility_hash,
                                             const godot::Variant** arguments,
                                             const std::int64_t argument_count) {
    if (!owner || !owner->is_class(native_class) || argument_count < 0) {
        godot::UtilityFunctions::push_error(
            "GDPP: invalid owner or argument count for an attached native super call");
        return {};
    }
    GDExtensionMethodBindPtr method_bind{nullptr};
    {
        std::lock_guard<std::mutex> lock{native_bind_mutex()};
        const auto key = native_bind_key(native_class, method, compatibility_hash);
        const auto found = native_bind_cache().find(key);
        if (found != native_bind_cache().end()) {
            method_bind = found->second;
        } else {
            method_bind = godot::gdextension_interface::classdb_get_method_bind(
                native_class._native_ptr(), method._native_ptr(), compatibility_hash);
            if (method_bind)
                native_bind_cache().emplace(key, method_bind);
        }
    }
    if (!method_bind) {
        godot::UtilityFunctions::push_error(
            godot::String{"GDPP: cannot resolve attached native super method "} +
            godot::String{native_class} + "." + godot::String{method});
        return {};
    }

    godot::Variant result;
    GDExtensionCallError error{};
    godot::gdextension_interface::object_method_bind_call(
        method_bind, owner->_owner, reinterpret_cast<GDExtensionConstVariantPtr*>(arguments),
        argument_count, result._native_ptr(), &error);
    if (error.error != GDEXTENSION_CALL_OK) {
        godot::UtilityFunctions::push_error(
            godot::String{"GDPP: attached native super call failed for "} +
            godot::String{native_class} + "." + godot::String{method});
        return {};
    }
    return result;
}

} // namespace gdpp::runtime
