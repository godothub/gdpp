#include "gdpp/runtime/attached_script.hpp"
#include "gdpp/runtime/variant_ops.hpp"

#include <godot_cpp/core/error_macros.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace gdpp::runtime {

AttachedScriptDescriptor&
AttachedScriptDescriptor::operator=(const AttachedScriptDescriptor& other) {
    if (this == &other)
        return *this;
    source_path = other.source_path;
    global_name = other.global_name;
    native_base_type = other.native_base_type;
    base_script_path = other.base_script_path;
    contract_hash = other.contract_hash;
    behavior_class = other.behavior_class;
    factory = other.factory;
    properties = other.properties;
    static_properties = other.static_properties;
    methods = other.methods;
    method_dispatches = other.method_dispatches;
    signals = other.signals;
    assign_dictionary(constants, other.constants);
    deferred_constants = other.deferred_constants;
    rpc_config = other.rpc_config;
    tool = other.tool;
    abstract = other.abstract;
    editor_metadata_only = other.editor_metadata_only;
    return *this;
}

AttachedScriptDescriptor& AttachedScriptDescriptor::operator=(AttachedScriptDescriptor&& other) {
    if (this == &other)
        return *this;
    source_path = std::move(other.source_path);
    global_name = std::move(other.global_name);
    native_base_type = std::move(other.native_base_type);
    base_script_path = std::move(other.base_script_path);
    contract_hash = std::move(other.contract_hash);
    behavior_class = std::move(other.behavior_class);
    factory = other.factory;
    properties = std::move(other.properties);
    static_properties = std::move(other.static_properties);
    methods = std::move(other.methods);
    method_dispatches = std::move(other.method_dispatches);
    signals = std::move(other.signals);
    assign_dictionary(constants, other.constants);
    deferred_constants = std::move(other.deferred_constants);
    rpc_config = std::move(other.rpc_config);
    tool = other.tool;
    abstract = other.abstract;
    editor_metadata_only = other.editor_metadata_only;
    return *this;
}

namespace {

std::mutex& registry_mutex() {
    static std::mutex value;
    return value;
}

std::map<std::string, AttachedScriptDescriptor>& registry() {
    static std::map<std::string, AttachedScriptDescriptor> value;
    return value;
}

std::map<std::string, std::string>& behavior_registry() {
    static std::map<std::string, std::string> value;
    return value;
}

std::map<std::string, godot::Ref<AttachedCompiledScript>>& script_resources() {
    static std::map<std::string, godot::Ref<AttachedCompiledScript>> value;
    return value;
}

std::string registry_key(const godot::String& path) {
    const godot::CharString utf8 = path.utf8();
    return {utf8.get_data(), static_cast<std::size_t>(utf8.length())};
}

bool valid_contract_hash(const godot::String& value) {
    const auto hash = registry_key(value);
    return hash.size() == 64U &&
           std::all_of(hash.begin(), hash.end(),
                       [](const unsigned char character) { return std::isxdigit(character) != 0; });
}

void set_error(godot::String* destination, const godot::String& message) {
    if (destination)
        *destination = message;
}

bool same_property_info(const godot::PropertyInfo& left, const godot::PropertyInfo& right) {
    return left.type == right.type && left.name == right.name &&
           left.class_name == right.class_name && left.hint == right.hint &&
           left.hint_string == right.hint_string && left.usage == right.usage;
}

bool same_variant(const godot::Variant& left, const godot::Variant& right) {
    // Variant equality intentionally coerces some scalar families and treats NaN as unequal to
    // itself. Descriptor identity instead needs exact runtime type plus Godot's hash comparison,
    // which is the same key-equivalence contract used by Dictionary.
    return left.get_type() == right.get_type() && left.hash_compare(right);
}

bool same_method_info(const godot::MethodInfo& left, const godot::MethodInfo& right) {
    if (left.name != right.name || !same_property_info(left.return_val, right.return_val) ||
        left.flags != right.flags || left.id != right.id ||
        left.return_val_metadata != right.return_val_metadata ||
        left.arguments.size() != right.arguments.size() ||
        left.default_arguments.size() != right.default_arguments.size() ||
        left.arguments_metadata.size() != right.arguments_metadata.size()) {
        return false;
    }
    for (decltype(left.arguments.size()) index = 0; index < left.arguments.size(); ++index) {
        if (!same_property_info(left.arguments[index], right.arguments[index]))
            return false;
    }
    for (decltype(left.default_arguments.size()) index = 0; index < left.default_arguments.size();
         ++index) {
        if (!same_variant(left.default_arguments[index], right.default_arguments[index]))
            return false;
    }
    for (decltype(left.arguments_metadata.size()) index = 0; index < left.arguments_metadata.size();
         ++index) {
        if (left.arguments_metadata[index] != right.arguments_metadata[index])
            return false;
    }
    return true;
}

bool same_property(const AttachedScriptProperty& left, const AttachedScriptProperty& right) {
    return same_property_info(left.info, right.info) && left.has_default == right.has_default &&
           left.getter == right.getter && left.setter == right.setter &&
           left.storage_getter == right.storage_getter &&
           left.storage_setter == right.storage_setter &&
           (!left.has_default || same_variant(left.default_value, right.default_value));
}

bool same_deferred_constant(const AttachedScriptDeferredConstant& left,
                            const AttachedScriptDeferredConstant& right) {
    return left.name == right.name && left.resolver == right.resolver;
}

bool same_method_dispatch(const AttachedScriptMethodDispatch& left,
                          const AttachedScriptMethodDispatch& right) {
    return left.name == right.name && left.call == right.call;
}

bool same_static_property(const AttachedScriptStaticProperty& left,
                          const AttachedScriptStaticProperty& right) {
    return left.name == right.name && left.getter == right.getter && left.setter == right.setter;
}

template <typename Items, typename Equal>
bool same_ordered_items(const Items& left, const Items& right, Equal&& equal) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!equal(left[index], right[index]))
            return false;
    }
    return true;
}

bool same_identity(const AttachedScriptDescriptor& left, const AttachedScriptDescriptor& right) {
    return left.source_path == right.source_path && left.global_name == right.global_name &&
           left.native_base_type == right.native_base_type &&
           left.base_script_path == right.base_script_path &&
           left.contract_hash == right.contract_hash &&
           left.behavior_class == right.behavior_class && left.factory == right.factory &&
           left.tool == right.tool && left.abstract == right.abstract &&
           left.editor_metadata_only == right.editor_metadata_only &&
           same_ordered_items(left.properties, right.properties, same_property) &&
           same_ordered_items(left.static_properties, right.static_properties,
                              same_static_property) &&
           same_ordered_items(left.methods, right.methods, same_method_info) &&
           same_ordered_items(left.method_dispatches, right.method_dispatches,
                              same_method_dispatch) &&
           same_ordered_items(left.signals, right.signals, same_method_info) &&
           same_variant(godot::Variant{left.constants}, godot::Variant{right.constants}) &&
           same_ordered_items(left.deferred_constants, right.deferred_constants,
                              same_deferred_constant) &&
           same_variant(left.rpc_config, right.rpc_config);
}

template <typename Items, typename Name>
bool names_are_unique(const Items& items, Name&& name, godot::String* duplicate) {
    std::unordered_set<std::string> names;
    for (const auto& item : items) {
        const auto key = registry_key(godot::String{name(item)});
        if (!names.emplace(key).second) {
            if (duplicate)
                *duplicate = godot::String{name(item)};
            return false;
        }
    }
    return true;
}

} // namespace

void AttachedScriptBehavior::attach_owner(godot::Object* owner) {
    ERR_FAIL_NULL(owner);
    ERR_FAIL_COND_MSG(owner_ != nullptr && owner_ != owner,
                      "A GDPP attached behavior cannot be moved between owners");
    owner_ = owner;
}

void AttachedScriptBehavior::detach_owner() { owner_ = nullptr; }

godot::Object* AttachedScriptBehavior::owner() const { return owner_; }

void AttachedScriptBehavior::_gdpp_initialize_instance() {}

void AttachedScriptBehavior::_gdpp_initialize_onready() {}

void AttachedScriptBehavior::_gdpp_dispatch_notification(std::int32_t, bool) {}

void AttachedScriptBehavior::_bind_methods() {}

bool register_attached_script(AttachedScriptDescriptor descriptor, godot::String* error) {
    descriptor.source_path = descriptor.source_path.simplify_path();
    if (descriptor.source_path.is_empty() || !descriptor.source_path.begins_with("res://")) {
        set_error(error, "attached script source_path must be an absolute res:// path");
        return false;
    }
    if (descriptor.native_base_type.is_empty()) {
        set_error(error, "attached script native_base_type must not be empty");
        return false;
    }
    if (!valid_contract_hash(descriptor.contract_hash)) {
        set_error(error, "attached script contract_hash must be a 64-character hexadecimal digest");
        return false;
    }
    if (descriptor.behavior_class.is_empty() && !descriptor.editor_metadata_only) {
        set_error(error, "attached script behavior_class must not be empty");
        return false;
    }
    if (!descriptor.abstract && descriptor.factory == nullptr && !descriptor.editor_metadata_only) {
        set_error(error, "concrete attached script must provide a behavior factory");
        return false;
    }
    if (!descriptor.base_script_path.is_empty() &&
        !descriptor.base_script_path.begins_with("res://")) {
        set_error(error, "attached base_script_path must be empty or an absolute res:// path");
        return false;
    }

    godot::String duplicate;
    if (!names_are_unique(
            descriptor.properties,
            [](const AttachedScriptProperty& item) { return item.info.name; }, &duplicate)) {
        set_error(error, "duplicate attached script property: " + duplicate);
        return false;
    }
    if (!descriptor.editor_metadata_only) {
        for (const auto& property : descriptor.properties) {
            if (!property.getter || !property.setter || !property.storage_getter ||
                !property.storage_setter) {
                set_error(error, "attached script property has no runtime accessor: " +
                                     godot::String{property.info.name});
                return false;
            }
        }
    }
    if (!names_are_unique(
            descriptor.static_properties,
            [](const AttachedScriptStaticProperty& item) { return item.name; }, &duplicate)) {
        set_error(error, "duplicate attached script static property: " + duplicate);
        return false;
    }
    if (!descriptor.editor_metadata_only) {
        for (const auto& property : descriptor.static_properties) {
            if (!property.getter || !property.setter) {
                set_error(error, "attached script static property has no runtime accessor: " +
                                     godot::String{property.name});
                return false;
            }
        }
    } else if (!descriptor.static_properties.empty()) {
        set_error(error, "editor-only attached metadata must not contain static property accessors");
        return false;
    }
    if (!names_are_unique(
            descriptor.methods, [](const godot::MethodInfo& item) { return item.name; },
            &duplicate)) {
        set_error(error, "duplicate attached script method: " + duplicate);
        return false;
    }
    if (!names_are_unique(
            descriptor.method_dispatches,
            [](const AttachedScriptMethodDispatch& item) { return item.name; }, &duplicate)) {
        set_error(error, "duplicate attached script method dispatch: " + duplicate);
        return false;
    }
    if (!descriptor.editor_metadata_only) {
        if (descriptor.method_dispatches.size() != descriptor.methods.size()) {
            set_error(error, "attached script method dispatch count does not match reflection");
            return false;
        }
        for (std::size_t index = 0; index < descriptor.methods.size(); ++index) {
            if (descriptor.method_dispatches[index].name != descriptor.methods[index].name ||
                !descriptor.method_dispatches[index].call) {
                set_error(error, "attached script method dispatch does not match reflection: " +
                                     godot::String{descriptor.methods[index].name});
                return false;
            }
        }
    } else if (!descriptor.method_dispatches.empty()) {
        set_error(error, "editor-only attached metadata must not contain runtime method dispatch");
        return false;
    }
    if (!names_are_unique(
            descriptor.signals, [](const godot::MethodInfo& item) { return item.name; },
            &duplicate)) {
        set_error(error, "duplicate attached script signal: " + duplicate);
        return false;
    }
    if (!names_are_unique(
            descriptor.deferred_constants,
            [](const AttachedScriptDeferredConstant& item) { return item.name; }, &duplicate)) {
        set_error(error, "duplicate deferred attached script constant: " + duplicate);
        return false;
    }
    for (const auto& constant : descriptor.deferred_constants) {
        if (!constant.resolver) {
            set_error(error, "deferred attached script constant has no resolver: " +
                                 godot::String{constant.name});
            return false;
        }
        if (descriptor.constants.has(constant.name)) {
            set_error(error, "attached script constant is both eager and deferred: " +
                                 godot::String{constant.name});
            return false;
        }
    }

    std::lock_guard<std::mutex> lock{registry_mutex()};
    const auto key = registry_key(descriptor.source_path);
    const auto existing = registry().find(key);
    if (existing != registry().end()) {
        if (same_identity(existing->second, descriptor))
            return true;
        set_error(error, "conflicting attached script descriptor: " + descriptor.source_path);
        return false;
    }
    const auto behavior_key = registry_key(godot::String{descriptor.behavior_class});
    if (!behavior_key.empty()) {
        const auto behavior = behavior_registry().find(behavior_key);
        if (behavior != behavior_registry().end() && behavior->second != key) {
            set_error(error, "attached behavior class is already registered for another script: " +
                                 godot::String{descriptor.behavior_class});
            return false;
        }
    }
    registry().emplace(key, std::move(descriptor));
    if (!behavior_key.empty())
        behavior_registry().emplace(behavior_key, key);
    return true;
}

void unregister_all_attached_scripts() {
    // Descriptor and canonical Script resources are valid only while every live ScriptInstance
    // can still dispatch into this library. This also makes temporary editor metadata resets safe
    // during repeated exports and extension reloads.
    detach_all_attached_script_instances();
    std::lock_guard<std::mutex> lock{registry_mutex()};
    script_resources().clear();
    behavior_registry().clear();
    registry().clear();
}

std::optional<AttachedScriptDescriptor> find_attached_script(const godot::String& source_path) {
    std::lock_guard<std::mutex> lock{registry_mutex()};
    const auto found = registry().find(registry_key(source_path.simplify_path()));
    if (found == registry().end())
        return std::nullopt;
    return found->second;
}

std::optional<AttachedScriptDescriptor>
find_attached_script_by_behavior_class(const godot::StringName& behavior_class) {
    std::lock_guard<std::mutex> lock{registry_mutex()};
    const auto behavior =
        behavior_registry().find(registry_key(godot::String{behavior_class}));
    if (behavior == behavior_registry().end())
        return std::nullopt;
    const auto descriptor = registry().find(behavior->second);
    if (descriptor == registry().end())
        return std::nullopt;
    return descriptor->second;
}

godot::Ref<AttachedCompiledScript> attached_script_resource(const godot::String& source_path,
                                                            godot::String* error) {
    const auto normalized = source_path.simplify_path();
    const auto key = registry_key(normalized);
    godot::Ref<AttachedCompiledScript> script;
    godot::String contract_hash;
    {
        std::lock_guard<std::mutex> lock{registry_mutex()};
        const auto descriptor = registry().find(key);
        if (descriptor == registry().end()) {
            set_error(error, "attached script is not registered: " + normalized);
            return {};
        }
        contract_hash = descriptor->second.contract_hash;
        const auto cached = script_resources().find(key);
        if (cached != script_resources().end())
            script = cached->second;
    }
    if (script.is_null()) {
        script.instantiate();
        if (script.is_null()) {
            set_error(error, "failed to instantiate attached script resource: " + normalized);
            return {};
        }
        script->set_source_path(normalized);
        std::lock_guard<std::mutex> lock{registry_mutex()};
        const auto [stored, inserted] = script_resources().emplace(key, script);
        if (!inserted)
            script = stored->second;
    }
    if (script->get_contract_hash() != contract_hash)
        script->set_contract_hash(contract_hash);
    return script;
}

godot::Ref<AttachedCompiledScript>
attached_container_script_resource(const godot::String& source_path) {
    const auto normalized = source_path.simplify_path();
    const auto key = registry_key(normalized);
    godot::Ref<AttachedCompiledScript> script;
    godot::String contract_hash;
    {
        std::lock_guard<std::mutex> lock{registry_mutex()};
        if (const auto descriptor = registry().find(key); descriptor != registry().end())
            contract_hash = descriptor->second.contract_hash;
        const auto cached = script_resources().find(key);
        if (cached != script_resources().end())
            script = cached->second;
    }
    if (script.is_valid()) {
        if (!contract_hash.is_empty() && script->get_contract_hash() != contract_hash)
            script->set_contract_hash(contract_hash);
        return script;
    }

    script.instantiate();
    ERR_FAIL_COND_V_MSG(script.is_null(), {}, "Failed to instantiate attached container Script");
    script->set_source_path(normalized);
    if (!contract_hash.is_empty())
        script->set_contract_hash(contract_hash);
    std::lock_guard<std::mutex> lock{registry_mutex()};
    const auto [stored, inserted] = script_resources().emplace(key, script);
    return inserted ? script : stored->second;
}

std::optional<AttachedScriptDescriptor> resolve_attached_script(const godot::String& source_path,
                                                                godot::String* error) {
    std::lock_guard<std::mutex> lock{registry_mutex()};
    const auto root = registry().find(registry_key(source_path.simplify_path()));
    if (root == registry().end()) {
        set_error(error, "attached script descriptor is not registered: " + source_path);
        return std::nullopt;
    }

    AttachedScriptDescriptor resolved = root->second;
    std::unordered_set<std::string> visited;
    visited.emplace(root->first);
    auto base_path = resolved.base_script_path;
    const auto append_unique = [](auto& destination, const auto& inherited, auto&& name) {
        for (const auto& item : inherited) {
            const auto duplicate =
                std::any_of(destination.begin(), destination.end(),
                            [&](const auto& existing) { return name(existing) == name(item); });
            if (!duplicate)
                destination.push_back(item);
        }
    };

    while (!base_path.is_empty()) {
        const auto key = registry_key(base_path.simplify_path());
        if (!visited.emplace(key).second) {
            set_error(error, "cyclic attached script inheritance at: " + base_path);
            return std::nullopt;
        }
        const auto base = registry().find(key);
        if (base == registry().end()) {
            set_error(error, "attached base script descriptor is not registered: " + base_path);
            return std::nullopt;
        }
        if (base->second.native_base_type != resolved.native_base_type) {
            set_error(error,
                      "attached script inheritance changes native base type at: " + base_path);
            return std::nullopt;
        }
        if (base->second.rpc_config.get_type() == godot::Variant::DICTIONARY) {
            godot::Dictionary rpc = resolved.rpc_config.get_type() == godot::Variant::DICTIONARY
                                        ? godot::Dictionary{resolved.rpc_config}
                                        : godot::Dictionary{};
            const godot::Dictionary inherited_rpc = base->second.rpc_config;
            const godot::Array method_names = inherited_rpc.keys();
            for (std::int64_t index = 0; index < method_names.size(); ++index) {
                const godot::StringName method_name{method_names[index]};
                if (!rpc.has(method_name))
                    rpc[method_name] = inherited_rpc[method_name];
            }
            if (!rpc.is_empty())
                resolved.rpc_config = rpc;
        }
        append_unique(resolved.properties, base->second.properties,
                      [](const AttachedScriptProperty& item) { return item.info.name; });
        append_unique(resolved.static_properties, base->second.static_properties,
                      [](const AttachedScriptStaticProperty& item) { return item.name; });
        append_unique(resolved.methods, base->second.methods,
                      [](const godot::MethodInfo& item) { return item.name; });
        append_unique(resolved.method_dispatches, base->second.method_dispatches,
                      [](const AttachedScriptMethodDispatch& item) { return item.name; });
        append_unique(resolved.signals, base->second.signals,
                      [](const godot::MethodInfo& item) { return item.name; });
        const godot::Array constant_names = base->second.constants.keys();
        for (std::int64_t index = 0; index < constant_names.size(); ++index) {
            const godot::Variant& name = constant_names[index];
            const godot::StringName constant_name{name};
            const bool deferred =
                std::any_of(resolved.deferred_constants.begin(), resolved.deferred_constants.end(),
                            [&](const auto& item) { return item.name == constant_name; });
            if (!resolved.constants.has(name) && !deferred)
                resolved.constants[name] = base->second.constants[name];
        }
        for (const auto& inherited : base->second.deferred_constants) {
            const bool already_present =
                resolved.constants.has(inherited.name) ||
                std::any_of(resolved.deferred_constants.begin(), resolved.deferred_constants.end(),
                            [&](const auto& item) { return item.name == inherited.name; });
            if (!already_present)
                resolved.deferred_constants.push_back(inherited);
        }
        base_path = base->second.base_script_path;
    }
    return resolved;
}

std::vector<godot::String> attached_script_paths() {
    std::lock_guard<std::mutex> lock{registry_mutex()};
    std::vector<godot::String> result;
    result.reserve(registry().size());
    for (const auto& [path, descriptor] : registry())
        result.push_back(descriptor.source_path);
    return result;
}

godot::Dictionary
materialize_attached_script_constants(const AttachedScriptDescriptor& descriptor) {
    godot::Dictionary result;
    assign_dictionary(result, descriptor.constants);
    for (const auto& constant : descriptor.deferred_constants) {
        if (constant.resolver)
            result[constant.name] = constant.resolver();
    }
    return result;
}

bool get_attached_script_constant(const AttachedScriptDescriptor& descriptor,
                                  const godot::StringName& name, godot::Variant& value) {
    if (descriptor.constants.has(name)) {
        value = descriptor.constants[name];
        return true;
    }
    const auto found =
        std::find_if(descriptor.deferred_constants.begin(), descriptor.deferred_constants.end(),
                     [&](const AttachedScriptDeferredConstant& constant) {
                         return constant.name == name;
                     });
    if (found == descriptor.deferred_constants.end() || !found->resolver)
        return false;
    value = found->resolver();
    return true;
}

} // namespace gdpp::runtime
