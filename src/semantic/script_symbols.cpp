#include "gdpp/semantic/script_symbols.hpp"

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace gdpp {

void ScriptSymbolTable::add(ScriptClassSymbol symbol) {
    const auto index = classes_.size();
    paths_.emplace(symbol.path, index);
    native_classes_.emplace(symbol.native_class_name, index);
    if (ambiguous_class_names_.find(symbol.script_name) == ambiguous_class_names_.end()) {
        const auto [existing, unique] = class_names_.emplace(symbol.script_name, index);
        if (!unique) {
            class_names_.erase(existing);
            ambiguous_class_names_.insert(symbol.script_name);
        }
    }
    if (symbol.globally_named)
        globals_.emplace(symbol.script_name, index);
    if (!symbol.autoload_name.empty()) {
        autoloads_.emplace(symbol.autoload_name, index);
        globals_.emplace(symbol.script_name, index);
    }
    classes_.push_back(std::move(symbol));
}

void ScriptSymbolTable::add_external(ExternalClassSymbol symbol) {
    if (external_names_.find(symbol.name) != external_names_.end())
        return;
    const auto index = external_classes_.size();
    external_names_.emplace(symbol.name, index);
    external_classes_.push_back(std::move(symbol));
}

void ScriptSymbolTable::add_resource_alias(std::string reference, std::string project_path) {
    resource_aliases_.insert_or_assign(std::move(reference), std::move(project_path));
}

bool ScriptSymbolTable::set_coroutine(const std::string& path, const std::string& inner_class,
                                      const std::string& method, const bool coroutine) {
    const auto found = paths_.find(path);
    if (found == paths_.end())
        return false;
    auto& owner = classes_[found->second];
    auto* members = &owner.members;
    if (!inner_class.empty()) {
        const auto inner = std::find_if(owner.inner_classes.begin(), owner.inner_classes.end(),
                                        [&](const auto& item) { return item.name == inner_class; });
        if (inner == owner.inner_classes.end())
            return false;
        members = &inner->members;
    }
    const auto member = std::find_if(members->begin(), members->end(), [&](const auto& item) {
        return item.kind == ScriptMemberKind::function && item.name == method;
    });
    if (member == members->end() || member->is_coroutine == coroutine)
        return false;
    member->is_coroutine = coroutine;
    return true;
}

bool ScriptSymbolTable::set_parameter_type(const std::string& path, const std::string& inner_class,
                                           const std::string& method, const std::size_t parameter,
                                           Type type) {
    const auto found = paths_.find(path);
    if (found == paths_.end())
        return false;
    auto& owner = classes_[found->second];
    auto* members = &owner.members;
    if (!inner_class.empty()) {
        const auto inner = std::find_if(owner.inner_classes.begin(), owner.inner_classes.end(),
                                        [&](const auto& item) { return item.name == inner_class; });
        if (inner == owner.inner_classes.end())
            return false;
        members = &inner->members;
    }
    const auto member = std::find_if(members->begin(), members->end(), [&](const auto& item) {
        return item.kind == ScriptMemberKind::function && item.name == method;
    });
    if (member == members->end() || parameter >= member->parameters.size() ||
        member->parameters[parameter] == type) {
        return false;
    }
    member->parameters[parameter] = std::move(type);
    return true;
}

bool ScriptSymbolTable::set_accessor_coroutines(const std::string& path,
                                                const std::string& inner_class,
                                                const std::string& field,
                                                const bool getter_coroutine,
                                                const bool setter_coroutine) {
    const auto found = paths_.find(path);
    if (found == paths_.end())
        return false;
    auto& owner = classes_[found->second];
    auto* members = &owner.members;
    if (!inner_class.empty()) {
        const auto inner = std::find_if(owner.inner_classes.begin(), owner.inner_classes.end(),
                                        [&](const auto& item) { return item.name == inner_class; });
        if (inner == owner.inner_classes.end())
            return false;
        members = &inner->members;
    }
    const auto member = std::find_if(members->begin(), members->end(), [&](const auto& item) {
        return item.kind == ScriptMemberKind::field && item.name == field;
    });
    if (member == members->end())
        return false;
    const bool changed = member->getter_is_coroutine != getter_coroutine ||
                         member->setter_is_coroutine != setter_coroutine;
    member->getter_is_coroutine = getter_coroutine;
    member->setter_is_coroutine = setter_coroutine;
    return changed;
}

void ScriptSymbolTable::update_class_identity(const std::string& path,
                                              std::string native_class_name,
                                              std::string header_file_name) {
    const auto found = paths_.find(path);
    if (found == paths_.end())
        return;
    auto& owner = classes_[found->second];
    const auto previous_native_name = owner.native_class_name;
    native_classes_.erase(owner.native_class_name);
    owner.native_class_name = std::move(native_class_name);
    owner.header_file_name = std::move(header_file_name);
    native_classes_.insert_or_assign(owner.native_class_name, found->second);
    const auto remap_type = [&](Type& type) {
        std::size_t offset = 0;
        while ((offset = type.name.find(previous_native_name, offset)) != std::string::npos) {
            type.name.replace(offset, previous_native_name.size(), owner.native_class_name);
            offset += owner.native_class_name.size();
        }
    };
    for (auto& script : classes_) {
        for (auto& member : script.members) {
            remap_type(member.type);
            for (auto& parameter : member.parameters)
                remap_type(parameter);
        }
        for (auto& inner : script.inner_classes) {
            for (auto& member : inner.members) {
                remap_type(member.type);
                for (auto& parameter : member.parameters)
                    remap_type(parameter);
            }
        }
    }
    for (auto& inner : owner.inner_classes) {
        if (inner.native_class_name.rfind(previous_native_name, 0) == 0) {
            inner.native_class_name = owner.native_class_name +
                                      inner.native_class_name.substr(previous_native_name.size());
        }
    }
}

const ScriptClassSymbol* ScriptSymbolTable::find_path(const std::string& path) const noexcept {
    const auto found = paths_.find(path);
    return found == paths_.end() ? nullptr : &classes_[found->second];
}

const ScriptClassSymbol*
ScriptSymbolTable::resolve_path(const std::string& owner_path,
                                const std::string& reference) const noexcept {
    const auto resource_path = resolve_resource_path(owner_path, reference);
    return resource_path ? find_path(*resource_path) : nullptr;
}

std::optional<std::string>
ScriptSymbolTable::resolve_resource_path(const std::string& owner_path,
                                         const std::string& reference) const noexcept {
    if (reference.rfind("uid://", 0) == 0) {
        const auto found = resource_aliases_.find(reference);
        return found == resource_aliases_.end() ? std::nullopt
                                                : std::optional<std::string>{found->second};
    }
    constexpr std::string_view resource_prefix{"res://"};
    std::filesystem::path path;
    if (reference.rfind(resource_prefix, 0) == 0)
        path = reference.substr(resource_prefix.size());
    else
        path = std::filesystem::path{owner_path}.parent_path() / reference;
    const auto normalized = path.lexically_normal();
    if (normalized.empty() || *normalized.begin() == "..")
        return std::nullopt;
    return normalized.generic_string();
}

const ScriptClassSymbol* ScriptSymbolTable::find_global(const std::string& name) const noexcept {
    const auto found = globals_.find(name);
    return found == globals_.end() ? nullptr : &classes_[found->second];
}

const ScriptClassSymbol* ScriptSymbolTable::find_class(const std::string& name) const noexcept {
    if (const auto global = globals_.find(name); global != globals_.end())
        return &classes_[global->second];
    const auto found = class_names_.find(name);
    return found == class_names_.end() ? nullptr : &classes_[found->second];
}

const ScriptClassSymbol*
ScriptSymbolTable::find_native_class(const std::string& name) const noexcept {
    const auto found = native_classes_.find(name);
    return found == native_classes_.end() ? nullptr : &classes_[found->second];
}

const ScriptClassSymbol* ScriptSymbolTable::find_autoload(const std::string& name) const noexcept {
    const auto found = autoloads_.find(name);
    return found == autoloads_.end() ? nullptr : &classes_[found->second];
}

const ExternalClassSymbol*
ScriptSymbolTable::find_external(const std::string& name) const noexcept {
    const auto found = external_names_.find(name);
    return found == external_names_.end() ? nullptr : &external_classes_[found->second];
}

const ExternalClassSymbol*
ScriptSymbolTable::external_base_of(const ScriptClassSymbol& owner) const noexcept {
    const ScriptClassSymbol* current = &owner;
    for (std::size_t depth = 0; current && depth <= classes_.size(); ++depth) {
        if (!current->external_base_name.empty())
            return find_external(current->external_base_name);
        current = base_of(*current);
    }
    return nullptr;
}

const ExternalClassSymbol*
ScriptSymbolTable::external_base_of(const ScriptInnerClassSymbol& owner) const noexcept {
    const ScriptInnerClassSymbol* current = &owner;
    std::unordered_set<const ScriptInnerClassSymbol*> visited;
    while (current && visited.insert(current).second) {
        if (!current->external_base_name.empty())
            return find_external(current->external_base_name);
        if (const auto* script_base = base_of(*current))
            return external_base_of(*script_base);
        current = inner_base_of(*current);
    }
    return nullptr;
}

const ScriptMemberSymbol*
ScriptSymbolTable::find_external_member(const ExternalClassSymbol& owner,
                                        const std::string& name) const noexcept {
    const auto found =
        std::find_if(owner.members.begin(), owner.members.end(),
                     [&](const ScriptMemberSymbol& member) { return member.name == name; });
    return found == owner.members.end() ? nullptr : &*found;
}

const ScriptEnumSymbol*
ScriptSymbolTable::find_external_enum(const ExternalClassSymbol& owner,
                                      const std::string& name) const noexcept {
    const auto found =
        std::find_if(owner.enums.begin(), owner.enums.end(),
                     [&](const auto& enumeration) { return enumeration.name == name; });
    return found == owner.enums.end() ? nullptr : &*found;
}

const ScriptClassSymbol* ScriptSymbolTable::base_of(const ScriptClassSymbol& owner) const noexcept {
    return owner.base_script_path.empty() ? nullptr : find_path(owner.base_script_path);
}

const ScriptClassSymbol*
ScriptSymbolTable::base_of(const ScriptInnerClassSymbol& owner) const noexcept {
    return owner.base_script_path.empty() ? nullptr : find_path(owner.base_script_path);
}

const ScriptInnerClassSymbol*
ScriptSymbolTable::inner_base_of(const ScriptInnerClassSymbol& owner) const noexcept {
    if (owner.base_class_name.empty())
        return nullptr;
    const ScriptInnerClassSymbol* canonical = &owner;
    const auto* script_owner = owner_of(owner);
    if (!script_owner && !owner.native_class_name.empty()) {
        canonical = find_inner_native(owner.native_class_name);
        script_owner = canonical ? owner_of(*canonical) : nullptr;
    }
    return script_owner && canonical ? find_inner(*script_owner, canonical->base_class_name)
                                     : nullptr;
}

const ScriptMemberSymbol* ScriptSymbolTable::find_member(const ScriptClassSymbol& owner,
                                                         const std::string& name) const noexcept {
    const ScriptClassSymbol* current = &owner;
    for (std::size_t depth = 0; current && depth <= classes_.size(); ++depth) {
        for (const auto& member : current->members) {
            if (member.name == name)
                return &member;
        }
        current =
            current->base_script_path.empty() ? nullptr : find_path(current->base_script_path);
    }
    if (const auto* external = external_base_of(owner))
        return find_external_member(*external, name);
    return nullptr;
}

bool ScriptSymbolTable::member_is_external(const ScriptClassSymbol& owner,
                                           const std::string& name) const noexcept {
    const auto* external = external_base_of(owner);
    const auto* external_member = external ? find_external_member(*external, name) : nullptr;
    return external_member && find_member(owner, name) == external_member;
}

bool ScriptSymbolTable::member_is_external(const ScriptInnerClassSymbol& owner,
                                           const std::string& name) const noexcept {
    const auto* external = external_base_of(owner);
    const auto* external_member = external ? find_external_member(*external, name) : nullptr;
    return external_member && find_inner_member(owner, name) == external_member;
}

const ScriptEnumSymbol* ScriptSymbolTable::find_enum(const ScriptClassSymbol& owner,
                                                     const std::string& name) const noexcept {
    const ScriptClassSymbol* current = &owner;
    for (std::size_t depth = 0; current && depth <= classes_.size(); ++depth) {
        for (const auto& enumeration : current->enums) {
            if (enumeration.name == name)
                return &enumeration;
        }
        current =
            current->base_script_path.empty() ? nullptr : find_path(current->base_script_path);
    }
    if (const auto* external = external_base_of(owner))
        return find_external_enum(*external, name);
    return nullptr;
}

const ScriptInnerClassSymbol*
ScriptSymbolTable::find_inner(const ScriptClassSymbol& owner,
                              const std::string& name) const noexcept {
    const auto exact = std::find_if(owner.inner_classes.begin(), owner.inner_classes.end(),
                                    [&](const auto& inner) { return inner.name == name; });
    if (exact != owner.inner_classes.end())
        return &*exact;

    const auto separator = name.rfind('.');
    const auto leaf = separator == std::string::npos ? name : name.substr(separator + 1);
    const ScriptInnerClassSymbol* unique = nullptr;
    for (const auto& inner : owner.inner_classes) {
        const auto inner_separator = inner.name.rfind('.');
        const auto inner_leaf = inner_separator == std::string::npos
                                    ? inner.name
                                    : inner.name.substr(inner_separator + 1);
        if (inner_leaf != leaf)
            continue;
        if (unique)
            return nullptr;
        unique = &inner;
    }
    return unique;
}

const ScriptInnerClassSymbol*
ScriptSymbolTable::find_inner_native(const std::string& name) const noexcept {
    for (const auto& owner : classes_) {
        const auto found = std::find_if(
            owner.inner_classes.begin(), owner.inner_classes.end(),
            [&](const ScriptInnerClassSymbol& inner) { return inner.native_class_name == name; });
        if (found != owner.inner_classes.end())
            return &*found;
    }
    return nullptr;
}

const ScriptClassSymbol*
ScriptSymbolTable::owner_of(const ScriptInnerClassSymbol& inner) const noexcept {
    for (const auto& owner : classes_) {
        const auto found = std::find_if(
            owner.inner_classes.begin(), owner.inner_classes.end(),
            [&](const ScriptInnerClassSymbol& candidate) { return &candidate == &inner; });
        if (found != owner.inner_classes.end())
            return &owner;
    }
    return nullptr;
}

const ScriptMemberSymbol*
ScriptSymbolTable::find_inner_member(const ScriptInnerClassSymbol& owner,
                                     const std::string& name) const noexcept {
    const ScriptInnerClassSymbol* current = &owner;
    std::unordered_set<const ScriptInnerClassSymbol*> visited;
    while (current && visited.insert(current).second) {
        const auto found = std::find_if(current->members.begin(), current->members.end(),
                                        [&](const auto& member) { return member.name == name; });
        if (found != current->members.end())
            return &*found;
        if (const auto* local_base = inner_base_of(*current)) {
            current = local_base;
            continue;
        }
        const auto* script_base = base_of(*current);
        if (script_base)
            return find_member(*script_base, name);
        const auto* external = external_base_of(*current);
        return external ? find_external_member(*external, name) : nullptr;
    }
    return nullptr;
}

std::vector<const ScriptMemberSymbol*>
ScriptSymbolTable::inherited_members(const ScriptClassSymbol& owner) const {
    std::vector<const ScriptMemberSymbol*> result;
    std::unordered_set<std::string> names;
    const auto* current =
        owner.base_script_path.empty() ? nullptr : find_path(owner.base_script_path);
    for (std::size_t depth = 0; current && depth <= classes_.size(); ++depth) {
        for (const auto& member : current->members) {
            if (names.insert(member.name).second)
                result.push_back(&member);
        }
        current =
            current->base_script_path.empty() ? nullptr : find_path(current->base_script_path);
    }
    if (const auto* external = external_base_of(owner)) {
        for (const auto& member : external->members) {
            if (names.insert(member.name).second)
                result.push_back(&member);
        }
    }
    return result;
}

bool ScriptSymbolTable::inherits(const std::string& derived_global_name,
                                 const std::string& base_global_name) const noexcept {
    if (derived_global_name == base_global_name)
        return true;
    const auto* current = find_global(derived_global_name);
    for (std::size_t depth = 0; current && depth <= classes_.size(); ++depth) {
        current =
            current->base_script_path.empty() ? nullptr : find_path(current->base_script_path);
        if (current && current->globally_named && current->script_name == base_global_name)
            return true;
    }
    return false;
}

bool ScriptSymbolTable::inherits(const ScriptClassSymbol& derived,
                                 const std::string& base_global_name) const noexcept {
    const ScriptClassSymbol* current = &derived;
    for (std::size_t depth = 0; current && depth <= classes_.size(); ++depth) {
        if (current->globally_named && current->script_name == base_global_name)
            return true;
        current = base_of(*current);
    }
    return false;
}

bool ScriptSymbolTable::requires_dynamic_dispatch(const ScriptClassSymbol& owner,
                                                  const std::string& method) const noexcept {
    const auto* contract = find_member(owner, method);
    if (!contract || contract->kind != ScriptMemberKind::function || contract->is_static)
        return false;
    const auto same_native_abi = [](const ScriptMemberSymbol& left,
                                    const ScriptMemberSymbol& right) {
        return left.type == right.type && left.parameters == right.parameters &&
               left.default_parameters == right.default_parameters &&
               left.is_vararg == right.is_vararg && left.is_coroutine == right.is_coroutine;
    };
    for (const auto& candidate : classes_) {
        if (candidate.path == owner.path)
            continue;
        bool descendant = false;
        const ScriptClassSymbol* base = base_of(candidate);
        for (std::size_t depth = 0; base && depth <= classes_.size(); ++depth) {
            if (base->path == owner.path) {
                descendant = true;
                break;
            }
            base = base_of(*base);
        }
        if (!descendant)
            continue;
        const auto override = std::find_if(candidate.members.begin(), candidate.members.end(),
                                           [&](const auto& member) {
                                               return member.kind == ScriptMemberKind::function &&
                                                      !member.is_static && member.name == method;
                                           });
        if (override != candidate.members.end() && !same_native_abi(*contract, *override))
            return true;
    }
    return false;
}

bool ScriptSymbolTable::requires_dynamic_dispatch(const ScriptInnerClassSymbol& owner,
                                                  const std::string& method) const noexcept {
    const auto* canonical =
        !owner.native_class_name.empty() ? find_inner_native(owner.native_class_name) : &owner;
    if (!canonical)
        canonical = &owner;
    const auto* contract = find_inner_member(*canonical, method);
    if (!contract || contract->kind != ScriptMemberKind::function || contract->is_static)
        return false;
    const auto same_native_abi = [](const ScriptMemberSymbol& left,
                                    const ScriptMemberSymbol& right) {
        return left.type == right.type && left.parameters == right.parameters &&
               left.default_parameters == right.default_parameters &&
               left.is_vararg == right.is_vararg && left.is_coroutine == right.is_coroutine;
    };
    const auto same_inner = [](const ScriptInnerClassSymbol& left,
                               const ScriptInnerClassSymbol& right) {
        return &left == &right || (!left.native_class_name.empty() &&
                                   left.native_class_name == right.native_class_name);
    };
    for (const auto& script : classes_) {
        for (const auto& candidate : script.inner_classes) {
            if (same_inner(candidate, *canonical))
                continue;
            bool descendant = false;
            const auto* base = inner_base_of(candidate);
            for (std::size_t depth = 0; base && depth <= script.inner_classes.size(); ++depth) {
                if (same_inner(*base, *canonical)) {
                    descendant = true;
                    break;
                }
                base = inner_base_of(*base);
            }
            if (!descendant)
                continue;
            const auto override = std::find_if(
                candidate.members.begin(), candidate.members.end(), [&](const auto& member) {
                    return member.kind == ScriptMemberKind::function && !member.is_static &&
                           member.name == method;
                });
            if (override != candidate.members.end() && !same_native_abi(*contract, *override))
                return true;
        }
    }
    return false;
}

bool ScriptSymbolTable::may_dispatch_coroutine(const ScriptClassSymbol& owner,
                                               const std::string& method) const noexcept {
    if (const auto* contract = find_member(owner, method);
        contract && contract->kind == ScriptMemberKind::function && contract->is_coroutine) {
        return true;
    }
    for (const auto& candidate : classes_) {
        if (candidate.path == owner.path)
            continue;
        bool descendant = false;
        const ScriptClassSymbol* base = base_of(candidate);
        for (std::size_t depth = 0; base && depth <= classes_.size(); ++depth) {
            if (base->path == owner.path) {
                descendant = true;
                break;
            }
            base = base_of(*base);
        }
        if (!descendant)
            continue;
        const auto override = std::find_if(candidate.members.begin(), candidate.members.end(),
                                           [&](const auto& member) {
                                               return member.kind == ScriptMemberKind::function &&
                                                      !member.is_static && member.name == method;
                                           });
        if (override != candidate.members.end() && override->is_coroutine)
            return true;
    }
    return false;
}

bool ScriptSymbolTable::may_dispatch_coroutine(const ScriptInnerClassSymbol& owner,
                                               const std::string& method) const noexcept {
    const auto* canonical =
        !owner.native_class_name.empty() ? find_inner_native(owner.native_class_name) : &owner;
    if (!canonical)
        canonical = &owner;
    if (const auto* contract = find_inner_member(*canonical, method);
        contract && contract->kind == ScriptMemberKind::function && contract->is_coroutine) {
        return true;
    }
    const auto same_inner = [](const ScriptInnerClassSymbol& left,
                               const ScriptInnerClassSymbol& right) {
        return &left == &right || (!left.native_class_name.empty() &&
                                   left.native_class_name == right.native_class_name);
    };
    for (const auto& script : classes_) {
        for (const auto& candidate : script.inner_classes) {
            bool descendant = false;
            const auto* base = inner_base_of(candidate);
            for (std::size_t depth = 0; base && depth <= script.inner_classes.size(); ++depth) {
                if (same_inner(*base, *canonical)) {
                    descendant = true;
                    break;
                }
                base = inner_base_of(*base);
            }
            if (!descendant)
                continue;
            const auto override = std::find_if(
                candidate.members.begin(), candidate.members.end(), [&](const auto& member) {
                    return member.kind == ScriptMemberKind::function && !member.is_static &&
                           member.name == method;
                });
            if (override != candidate.members.end() && override->is_coroutine)
                return true;
        }
    }
    return false;
}

} // namespace gdpp
