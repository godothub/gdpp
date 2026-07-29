extends SceneTree

const EXTENSION_REGISTRY := "res://.godot/extension_list.cfg"
const VENDOR_DESCRIPTOR := "res://addons/vendor/vendor.gdextension"
const PROJECT_DESCRIPTOR := "res://addons/gdpp/gdpp.gdextension"
const PROJECT_LIBRARY_ENTRY := 'entry_symbol = "gdpp_library_init"'
const DISCOVERED_SCRIPT_REMAP := "res://runtime_discovery/OnlyScripts/index.gd.remap"
const HARNESS_FILES := {
    "res://audit_attached_export_pck.gd": true,
    "res://audit_attached_export_pck.gd.uid": true,
}


func _init() -> void:
    var arguments := OS.get_cmdline_user_args()
    if arguments.size() != 1:
        push_error("usage: audit_attached_export_pck.gd -- <export.pck>")
        quit(2)
        return
    var package_path := ProjectSettings.globalize_path(arguments[0])
    if not ProjectSettings.load_resource_pack(package_path, true):
        push_error("cannot mount attached export PCK: %s" % package_path)
        quit(3)
        return

    var files: PackedStringArray = []
    var violations: PackedStringArray = []
    if not _collect_files("res://", files):
        quit(4)
        return
    var transformed_scene_count := 0
    var transformed_resource_count := 0
    var transformed_script_count := 0
    for path in files:
        if HARNESS_FILES.has(path):
            continue
        var lower := path.to_lower()
        var extension := lower.get_extension()
        if extension in ["gd", "gdc"]:
            violations.append("export contains GDScript source or bytecode: %s" % path)
        if extension in ["c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx"]:
            violations.append("export contains native source: %s" % path)
        if lower.contains("gdpp_compiler") or lower.contains("gdpp_fallback"):
            violations.append("export contains a compiler-only library: %s" % path)
        if path.begins_with("res://addons/gdpp/runtime/scenes/"):
            transformed_scene_count += 1
        elif path.begins_with("res://addons/gdpp/runtime/resources/"):
            transformed_resource_count += 1
        elif path.begins_with("res://addons/gdpp/runtime/scripts/"):
            transformed_script_count += 1

    var registry_order := _audit_extension_registry(violations)
    if not FileAccess.file_exists(VENDOR_DESCRIPTOR):
        violations.append("export omitted the independent vendor descriptor")
    if not FileAccess.file_exists(PROJECT_DESCRIPTOR):
        violations.append("export omitted the GDPP project descriptor")
    if transformed_scene_count == 0:
        violations.append("export contains no transformed attached scene")
    if transformed_resource_count == 0:
        violations.append("export contains no transformed attached resource")
    if transformed_script_count == 0:
        violations.append("export contains no transformed compiled Script resource")
    if not FileAccess.file_exists(DISCOVERED_SCRIPT_REMAP):
        violations.append("export omitted the source-only module Script remap")

    print("GDPP_ATTACHED_PCK_FILES=%d" % files.size())
    print("GDPP_ATTACHED_PCK_SCENES=%d" % transformed_scene_count)
    print("GDPP_ATTACHED_PCK_RESOURCES=%d" % transformed_resource_count)
    print("GDPP_ATTACHED_PCK_SCRIPTS=%d" % transformed_script_count)
    print("GDPP_ATTACHED_PCK_REGISTRY_ORDER=%s" % registry_order)
    print("GDPP_ATTACHED_PCK_VIOLATIONS=%d" % violations.size())
    for violation in violations:
        push_error(violation)
    quit(0 if violations.is_empty() else 5)


func _audit_extension_registry(violations: PackedStringArray) -> String:
    if not FileAccess.file_exists(EXTENSION_REGISTRY):
        violations.append("export omitted the runtime extension registry")
        return "missing"
    var entries := FileAccess.get_file_as_string(EXTENSION_REGISTRY).split("\n", false)
    var vendor_index := entries.find(VENDOR_DESCRIPTOR)
    var project_index := entries.find(PROJECT_DESCRIPTOR)
    if vendor_index < 0:
        violations.append("runtime registry omitted the vendor extension")
    elif entries.count(VENDOR_DESCRIPTOR) != 1:
        violations.append("runtime registry contains the vendor extension more than once")
    if project_index < 0:
        violations.append("runtime registry omitted the GDPP project extension")
    elif entries.count(PROJECT_DESCRIPTOR) != 1:
        violations.append("runtime registry contains the GDPP project extension more than once")
    if FileAccess.file_exists(PROJECT_DESCRIPTOR):
        var descriptor := FileAccess.get_file_as_string(PROJECT_DESCRIPTOR)
        if not descriptor.contains(PROJECT_LIBRARY_ENTRY):
            violations.append("runtime descriptor does not use the GDPP project entry ABI")
        if descriptor.contains("gdpp_compiler"):
            violations.append("runtime descriptor still references a compiler library")
    if vendor_index < 0 or project_index < 0:
        return "invalid"
    return "provider-first" if vendor_index < project_index else "project-first"


func _collect_files(directory_path: String, output: PackedStringArray) -> bool:
    var directory := DirAccess.open(directory_path)
    if directory == null:
        push_error("cannot enumerate mounted PCK directory: %s" % directory_path)
        return false
    directory.list_dir_begin()
    while true:
        var name := directory.get_next()
        if name.is_empty():
            break
        if name in [".", ".."]:
            continue
        var path := directory_path.path_join(name)
        if directory.current_is_dir():
            if not _collect_files(path, output):
                directory.list_dir_end()
                return false
        else:
            output.append(path)
    directory.list_dir_end()
    return true
