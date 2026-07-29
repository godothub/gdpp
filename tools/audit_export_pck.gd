extends SceneTree

const HARNESS_FILES := {
    "res://audit_export_pck.gd": true,
    "res://audit_export_pck.gd.uid": true,
    "res://inspect_export_pck.gd": true,
    "res://inspect_export_pck.gd.uid": true,
}
const EXTENSION_REGISTRY := "res://.godot/extension_list.cfg"
const RUNTIME_DESCRIPTOR := "res://addons/gdpp/gdpp.gdextension"
const PROJECT_LIBRARY_ENTRY_SYMBOL := "gdpp_library_init"


func _init() -> void:
    var arguments := OS.get_cmdline_user_args()
    if arguments.size() < 1 or arguments.size() > 3:
        push_error(
            "用法：godot --headless --path <空项目> --script audit_export_pck.gd -- "
            + "<导出包.pck> [完整导出目录] [项目原生动态库]"
        )
        quit(2)
        return

    var package_path := ProjectSettings.globalize_path(arguments[0])
    if not ProjectSettings.load_resource_pack(package_path, true):
        push_error("无法加载 PCK：%s" % package_path)
        quit(3)
        return

    var files: PackedStringArray = []
    if not _collect_files("res://", files):
        quit(4)
        return

    var violations: PackedStringArray = []
    var native_library_path := arguments[2] if arguments.size() == 3 else ""
    var web_audit := native_library_path.get_extension().to_lower() == "wasm"
    _validate_runtime_extension(violations, native_library_path, web_audit)
    var package_file_count := 0
    for path in files:
        # The audit program belongs to the empty host project, not the mounted
        # package. Resource packs overlay res://, so exclude this known harness.
        if HARNESS_FILES.has(path):
            continue
        package_file_count += 1
        var lower := path.to_lower()
        if lower.ends_with(".gd") or lower.ends_with(".gdc"):
            violations.append("包含 GDScript 源码或字节码：%s" % path)
        if lower.get_extension() in ["c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx"]:
            violations.append("包含原生源码或头文件：%s" % path)
        if lower.get_extension() in ["a", "exp", "ilk", "lib", "o", "obj", "pdb"]:
            violations.append("包含原生构建中间产物：%s" % path)
        if path.begins_with("res://addons/gdpp/") and not _allowed_runtime_path(path):
            violations.append("包含非运行时 GDPP 文件：%s" % path)
    var native_library_count := -1
    if arguments.size() >= 2:
        var export_directory := ProjectSettings.globalize_path(arguments[1])
        var native_files: PackedStringArray = []
        if not _collect_native_files(export_directory, native_files):
            violations.append("无法审计完整导出目录：%s" % export_directory)
        else:
            native_library_count = 0
            for native_path in native_files:
                var filename := native_path.get_file().to_lower()
                var extension := filename.get_extension()
                if filename.contains("gdpp_compiler") or filename.contains("gdpp_fallback"):
                    violations.append("完整导出目录包含非项目运行时库：%s" % native_path)
                if extension in [
                    "a", "c", "cc", "cpp", "cxx", "exp", "h", "hh", "hpp", "hxx",
                    "ilk", "lib", "o", "obj", "pdb"
                ]:
                    violations.append("完整导出目录包含原生源码或中间产物：%s" % native_path)
                if _is_legacy_project_library(filename):
                    violations.append("完整导出目录包含旧命名的项目原生库：%s" % native_path)
                if _is_project_library(filename):
                    native_library_count += 1
            if native_library_count != 1:
                violations.append(
                    "完整导出目录应且只应包含一个 GDPP 项目原生库，实际为 %d"
                    % native_library_count
                )

    print("PCK_AUDIT_FILES=%d" % package_file_count)
    print("PCK_AUDIT_PROJECT_LIBRARIES=%d" % native_library_count)
    print("PCK_AUDIT_VIOLATIONS=%d" % violations.size())
    for violation in violations:
        push_error(violation)
    quit(0 if violations.is_empty() else 5)


func _collect_files(directory_path: String, output: PackedStringArray) -> bool:
    var directory := DirAccess.open(directory_path)
    if directory == null:
        push_error("无法读取 PCK 目录：%s" % directory_path)
        return false
    directory.list_dir_begin()
    while true:
        var name := directory.get_next()
        if name.is_empty():
            break
        if name == "." or name == "..":
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


func _allowed_runtime_path(path: String) -> bool:
    if path == RUNTIME_DESCRIPTOR:
        return true
    if path.begins_with("res://addons/gdpp/runtime/autoload/"):
        return path.ends_with(".tscn")
    if path.begins_with("res://addons/gdpp/runtime/scenes/"):
        return path.ends_with(".scn")
    if path.begins_with("res://addons/gdpp/runtime/resources/"):
        return path.ends_with(".res")
    if path.begins_with("res://addons/gdpp/runtime/scripts/"):
        return path.ends_with(".gdppscript")
    if path.begins_with("res://addons/gdpp/binary/"):
        var filename := path.get_file().to_lower()
        return _is_project_library(filename, true)
    return false


func _validate_runtime_extension(
    violations: PackedStringArray,
    native_library_path: String,
    web_audit: bool
) -> void:
    if not FileAccess.file_exists(EXTENSION_REGISTRY):
        violations.append("缺少运行时 GDExtension 注册表")
    else:
        var extension_registry := FileAccess.get_file_as_string(EXTENSION_REGISTRY)
        if not extension_registry.contains(RUNTIME_DESCRIPTOR):
            violations.append("运行时注册表缺少 GDPP 项目原生库")

    if not FileAccess.file_exists(RUNTIME_DESCRIPTOR):
        violations.append("缺少 GDPP 项目运行时描述符")
        return
    var runtime_descriptor := FileAccess.get_file_as_string(RUNTIME_DESCRIPTOR)
    if not runtime_descriptor.contains(
        'entry_symbol = "%s"' % PROJECT_LIBRARY_ENTRY_SYMBOL
    ):
        violations.append("GDPP 项目描述符未指向项目原生库入口")
    if runtime_descriptor.contains("gdpp_export_fallback_library_init"):
        violations.append("GDPP 项目描述符仍指向 fallback")
    var runtime_library_path := _runtime_library_path()
    if runtime_library_path.is_empty():
        violations.append("项目运行时描述符没有可审计的原生库条目")
        return
    if not _is_project_library(runtime_library_path.get_file()):
        violations.append("项目运行时描述符引用了非标准命名的原生库")
    if native_library_path.is_empty():
        violations.append("未提供项目原生动态库，无法核对运行时载荷")
        return
    var native_library_absolute := native_library_path
    if not native_library_absolute.is_absolute_path():
        native_library_absolute = ProjectSettings.globalize_path(native_library_absolute)
    if not FileAccess.file_exists(native_library_absolute):
        violations.append("项目原生动态库不存在：%s" % native_library_absolute)
        return
    if web_audit:
        if runtime_library_path.get_extension().to_lower() != "wasm":
            violations.append("Web 运行时描述符未引用 Wasm side module")
        if not runtime_descriptor.contains("web.") or not runtime_descriptor.contains("wasm32"):
            violations.append("Web 运行时描述符缺少 web/wasm32 特征约束")


func _runtime_library_path() -> String:
    if not FileAccess.file_exists(RUNTIME_DESCRIPTOR):
        return ""
    var in_libraries := false
    for raw_line in FileAccess.get_file_as_string(RUNTIME_DESCRIPTOR).split("\n"):
        var line := raw_line.strip_edges()
        if line == "[libraries]":
            in_libraries = true
            continue
        if line.begins_with("["):
            in_libraries = false
        if not in_libraries or not line.contains("="):
            continue
        var value := line.get_slice("=", 1).strip_edges()
        return value.trim_prefix("\"").trim_suffix("\"")
    return ""


func _is_project_library(filename: String, release_only := false) -> bool:
    var lower := filename.to_lower()
    if lower.get_extension() not in ["dll", "dylib", "so", "wasm"]:
        return false
    if release_only:
        return lower.begins_with("gdpp.release.") or lower.begins_with("libgdpp.release.")
    return (
        lower.begins_with("gdpp.debug.")
        or lower.begins_with("gdpp.release.")
        or lower.begins_with("libgdpp.debug.")
        or lower.begins_with("libgdpp.release.")
    )


func _is_legacy_project_library(filename: String) -> bool:
    var lower := filename.to_lower()
    return (
        lower.get_extension() in ["dll", "dylib", "so", "wasm"]
        and (
            lower.begins_with("gdpp_project.")
            or lower.begins_with("libgdpp_project.")
        )
    )


func _collect_native_files(directory_path: String, output: PackedStringArray) -> bool:
    var directory := DirAccess.open(directory_path)
    if directory == null:
        return false
    directory.list_dir_begin()
    while true:
        var name := directory.get_next()
        if name.is_empty():
            break
        if name == "." or name == "..":
            continue
        var path := directory_path.path_join(name)
        if directory.current_is_dir():
            if not _collect_native_files(path, output):
                directory.list_dir_end()
                return false
        else:
            output.append(path)
    directory.list_dir_end()
    return true
