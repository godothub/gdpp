@tool
extends EditorExportPlugin

const NATIVE_BUILD_JOB := preload("res://addons/gdpp/native_build_job.gd")
const OUTPUT_DIRECTORY := "res://addons/gdpp/build/project"
const EXPORT_WORKER_ROOT := OUTPUT_DIRECTORY + "/export-worker"
const BINARY_DIRECTORY := "res://addons/gdpp/binary"
const COMPILER_DESCRIPTOR := "res://addons/gdpp/gdpp.gdextension"
const ADDON_PREFIX := "res://addons/gdpp/"
const RUNTIME_RESOURCE_PREFIX := "res://addons/gdpp/runtime/"
const PROJECT_LIBRARY_ENTRY_SYMBOL := "gdpp_library_init"
const COMPILER_SETTING := "gdpp/build/cpp_compiler"
const SDK_SETTING := "gdpp/build/sdk_root"
const ANDROID_NDK_SETTING := "gdpp/build/android_ndk_root"
const EMSCRIPTEN_CXX_SETTING := "gdpp/build/emscripten_cxx"
const TARGET_VERSION_SETTING := "gdpp/target_godot_version"
const STRIP_OPTION := "gdpp/strip_gdscript_sources"
const ALLOW_SOURCE_FALLBACK_OPTION := "gdpp/allow_source_fallback"
const EXTENSION_REGISTRY := "res://.godot/extension_list.cfg"
const EXTENSION_REGISTRY_BACKUP := "res://.godot/gdpp_extension_list.export-backup"
const COMPILER_DESCRIPTOR_BACKUP := "res://.godot/gdpp_compiler_descriptor.export-backup"
const PROVIDER_DESCRIPTORS_BACKUP := (
    "res://.godot/gdpp_provider_descriptors.export-backup.json"
)
const ARCHITECTURE_FEATURES := [
    "x86_32",
    "x86_64",
    "arm32",
    "arm64",
    "rv64",
    "ppc64",
    "wasm32",
    "loongarch64",
    "universal",
]
const SCRIPT_CLASS_CACHE := "res://.godot/global_script_class_cache.cfg"
const GODOT_EXPORT_CACHE_DIRECTORY := "res://.godot/exported"
const EXPORT_TRANSFORM_REVISION := 24

var _compiler: Object
var _build_progress: CanvasLayer
var _active_build_label := ""
var _ready := false
var _script_classes: Dictionary = {}
var _attached_script_bases: Dictionary = {}
var _script_contract_hashes: Dictionary = {}
var _editor_script_descriptors: Array = []
var _compiled_scripts: Dictionary = {}
var _abstract_scripts: Dictionary = {}
var _editor_only_scripts: Dictionary = {}
var _resource_aot_requirements: Dictionary = {}
var _runtime_descriptor := ""
var _runtime_library_path := ""
var _output_library := ""
var _build_id := ""
var _target_platform := ""
var _target_architecture := ""
var _target_variant := ""
var _target_precision := ""
var _build_profile := ""
var _export_extension_registry := ""
var _export_script_class_cache := ""
var _include_project_extension := false
var _export_features: Dictionary = {}
var _extension_registry_original := ""
var _extension_registry_modified := false
var _provider_descriptor_overrides: Dictionary = {}
var _registered_shared_objects: Dictionary = {}
var _registered_apple_entries: Dictionary = {}
var _has_resource_scripts := false
var _export_output_path := ""
var _autoload_files: Dictionary = {}
var _autoload_replacements: Dictionary = {}
var _autoload_originals: Dictionary = {}
var _metrics_mutex := Mutex.new()
var _customized_scene_count := 0
var _replaced_node_count := 0
var _customized_resource_count := 0
var _copied_property_count := 0
var _strict_failure_injected := false
var _prepared_export_transforms: Dictionary = {}
var _transform_worker_directory := ""
var _worker_mode := false
var _worker_error := ""


func configure(compiler: Object, build_progress: CanvasLayer) -> void:
    _compiler = compiler
    _build_progress = build_progress


func _get_name() -> String:
    # Godot 4.5 mixes a scene customizer's name into the on-disk export-cache
    # directory but accidentally leaves its scene configuration hash out of
    # that directory key. Include the transformation revision and native build
    # ID in the name as a compatibility workaround so a repaired compiler or
    # exporter can never reuse scenes cached by an older transformation.
    # This plugin must run before Godot's built-in "GDExtension" exporter. It
    # virtualizes GDPP's editor-only descriptor and selected Universal 2
    # provider descriptors before the built-in scanner can inspect them.
    return "AOT GDPP scene transformer r%d %s" % [
        EXPORT_TRANSFORM_REVISION,
        _build_id,
    ]


func _supports_platform(platform: EditorExportPlatform) -> bool:
    var os_name := platform.get_os_name().to_lower()
    return (
        os_name == "macos"
        or os_name == "windows"
        or os_name == "linux"
        or os_name == "android"
        or os_name == "ios"
        or os_name == "web"
    )


func _get_export_options(_platform: EditorExportPlatform) -> Array[Dictionary]:
    return [
        {
            "option": {
                "name": STRIP_OPTION,
                "type": TYPE_BOOL,
                "usage": PROPERTY_USAGE_DEFAULT,
            },
            "default_value": true,
        },
        {
            "option": {
                "name": ALLOW_SOURCE_FALLBACK_OPTION,
                "type": TYPE_BOOL,
                "usage": PROPERTY_USAGE_DEFAULT,
            },
            "default_value": false,
        },
    ]


func _export_begin(
    features: PackedStringArray,
    is_debug: bool,
    path: String,
    _flags: int
) -> void:
    # Recover a previous export transaction if Godot aborted before _export_end().
    _restore_export_transaction()
    _reset_export_state()
    _export_output_path = path
    _target_platform = _platform_from_features(features)
    _target_architecture = _architecture_from_features(features)
    _target_variant = _web_variant_from_features(features)
    _target_precision = _precision_from_features(features)
    _build_profile = "debug" if is_debug else "release"
    for feature: String in features:
        _export_features[feature] = true
    if get_option(STRIP_OPTION) != true:
        print("GDPP: AOT source stripping is disabled for this export preset")
        # Godot 4.5 can reuse a remapped scene produced by an earlier AOT preset even when the
        # current preset disables transformation. Such a scene names GDPPNative classes that are
        # deliberately absent from the pure-GDScript package. Invalidate the generated export
        # cache before file enumeration so fallback exports always start from project sources.
        if not _clear_godot_export_cache():
            _fail_export("cannot clear stale native scene export cache")
            return
        # This is an intentional pure-GDScript export, not a failed AOT build. Do not load the
        # empty fallback extension or retain any project-native library in the comparison/package.
        _activate_fallback_descriptors(is_debug)
        if not _prepare_extension_registry(false):
            return
        return
    if not _prepare_export(features, is_debug):
        _activate_fallback_descriptors(is_debug)
        if not _prepare_extension_registry(
            _target_platform in ["macos", "windows", "linux"]
        ):
            return
        if (
            not _strict_failure_injected
            and _include_project_extension
            and not _register_runtime_library()
        ):
            return
        return

    if not _prepare_script_class_cache():
        _activate_fallback_descriptors(is_debug)
        # Android and Web have no host-loadable fallback library. Keeping a
        # project descriptor after a failed AOT transaction would make Godot
        # package a missing or wrong-ABI artifact instead of failing closed.
        if not _prepare_extension_registry(
            _target_platform in ["macos", "windows", "linux"]
        ):
            return
        if (
            not _strict_failure_injected
            and _include_project_extension
            and not _register_runtime_library()
        ):
            return
        return
    if not _prepare_extension_registry():
        return
    if not _register_runtime_library():
        return
    _activate_autoloads()

    # GDPP owns registration of the project artifact. Its physical descriptor remains editor-only,
    # while _export_file() supplies runtime bytes and prevents the built-in GDExtension scanner from
    # observing that editor descriptor. This leaves exactly one registration path on every target.
    _ready = true
    print(
        "GDPP: %s %s/%s project library is ready for binary-only export" % [
            _build_profile,
            _target_platform,
            _target_architecture,
        ]
    )


func _export_end() -> void:
    _restore_autoloads()
    _remove_successful_export_fallback()
    _restore_export_transaction()
    _print_export_summary()
    _reset_export_state()


func _begin_customize_scenes(
    _platform: EditorExportPlatform,
    _features: PackedStringArray
) -> bool:
    # Supported Godot releases feed raw exported paths through ResourceLoader when either global
    # customization pass is enabled. That includes icons, certificates and arbitrary customer
    # data for which no ResourceFormatLoader exists. _export_file() therefore owns selective scene
    # transformation and loads each qualifying scene exactly once.
    return false


func _begin_customize_resources(
    _platform: EditorExportPlatform,
    _features: PackedStringArray
) -> bool:
    # Godot 4.4/4.5 applies resource customization to every exported file,
    # including raw files that ResourceLoader cannot load (for example .ico,
    # .md and .txt). Script-backed .tres/.res files are transformed selectively
    # in _export_file(), and embedded Resources are transformed while their
    # owning scene is copied, so the global resource pass is neither necessary
    # nor safe for a production export.
    return false


func _get_customization_configuration_hash() -> int:
    return hash([
        EXPORT_TRANSFORM_REVISION,
        _build_id,
        _target_platform,
        _target_architecture,
        _target_variant,
        _target_precision,
        _build_profile,
    ])


func _customize_scene(scene: Node, path: String) -> Node:
    if not _ready or not _resource_requires_aot(path):
        return null
    # Snapshot the already instantiated source tree to obtain its serialized property and
    # connection state. The isolated worker loads each root once, so @tool resources, custom
    # loaders, shaders and VisualShader graphs are not evaluated again by this transformation.
    var packed_scene := PackedScene.new()
    var snapshot_error := packed_scene.pack(scene)
    if snapshot_error != OK:
        _fail_export(
            "scene '%s' cannot snapshot its serialized state during AOT customization (error %d)"
            % [path, snapshot_error]
        )
        return null
    var resource_replacements: Dictionary = {}
    var resource_changes := {"count": 0}
    return _transform_scene_with_state(
        scene,
        packed_scene.get_state(),
        path,
        resource_replacements,
        resource_changes
    )


func _transform_scene_with_state(
    scene: Node,
    scene_state: SceneState,
    path: String,
    resource_replacements: Dictionary,
    resource_changes: Dictionary
) -> Node:
    var resource_change_start := int(resource_changes.get("count", 0))
    var replacement_plan: Array[Dictionary] = []
    if not _collect_scene_replacement_plan(scene, scene_state, path, replacement_plan):
        _fail_export("scene '%s' contains a script that cannot be replaced safely" % path)
        return null

    var replacement_nodes: Dictionary = {}
    var connection_snapshots := _snapshot_connections(scene, scene_state)
    var result := _replace_planned_nodes(
        scene,
        replacement_plan,
        replacement_nodes,
        resource_replacements,
        resource_changes,
        path
    )
    if result == null:
        _restore_connections(connection_snapshots, {})
        _fail_export("scene '%s' could not be replaced atomically" % path)
        return null
    _restore_connections(connection_snapshots, replacement_nodes)
    _transform_serialized_scene_resources(
        result,
        scene_state,
        path,
        resource_replacements,
        resource_changes
    )
    if _strict_failure_injected:
        result.free()
        return null
    if (
        replacement_plan.is_empty()
        and int(resource_changes.count) == resource_change_start
    ):
        return null
    _record_scene_metrics(
        replacement_plan.size(),
        int(replacement_nodes.get("__gdpp_copied_properties", 0))
    )
    return result


func _customize_resource(resource: Resource, path: String) -> Resource:
    # Sanitize the complete storage graph even when the root belongs to another ScriptLanguage.
    # A C# or custom-language Resource can legally own nested GDScript-backed resources; preserving
    # its provider must not let those nested project scripts escape the binary-only transaction.
    var replacements: Dictionary = {}
    var changes := {"count": 0}
    var transformed := _transform_resource_graph(
        resource,
        path,
        replacements,
        changes,
        true
    )
    if transformed == null:
        return null
    return transformed if int(changes.get("count", 0)) > 0 else null


func _export_file(path: String, _type: String, _features: PackedStringArray) -> void:
    if _strict_failure_injected:
        # Fail closed even on Godot versions whose command-line exporter does
        # not convert EXPORT_MESSAGE_ERROR into a non-zero process exit. No
        # customer resource or script is allowed into the failed package.
        skip()
        return

    # Keep one physical descriptor path in both editor and export without ever changing its
    # on-disk editor contents. This plugin sorts before Godot's built-in GDExtension exporter,
    # supplies the runtime bytes at the same virtual package path, then skips the physical file.
    if path == COMPILER_DESCRIPTOR:
        if _include_project_extension and not _runtime_descriptor.is_empty():
            add_file(COMPILER_DESCRIPTOR, _runtime_descriptor.to_utf8_buffer(), false)
        skip()
        return

    if path == EXTENSION_REGISTRY and not _export_extension_registry.is_empty():
        add_file(path, _export_extension_registry.to_utf8_buffer(), false)
        skip()
        return

    # Selected Universal 2 providers are registered by GDPP without changing customer files.
    # Debug packages may receive an in-package debug alias to the verified Release Mach-O when
    # the provider does not ship a separate debug binary.
    if _provider_descriptor_overrides.has(path):
        add_file(path, str(_provider_descriptor_overrides[path]).to_utf8_buffer(), false)
        skip()
        return

    if path == SCRIPT_CLASS_CACHE and _ready and not _export_script_class_cache.is_empty():
        add_file(path, _export_script_class_cache.to_utf8_buffer(), false)
        skip()
        return

    if path.begins_with(ADDON_PREFIX):
        skip()
        return

    if (
        _ready
        and path.get_extension().to_lower() in ["tscn", "scn"]
        and _resource_requires_aot(path)
    ):
        if _export_prepared_transform(path, "scenes", "scn"):
            return
        if _strict_failure_injected:
            skip()
            return

    if (
        _ready
        and _has_resource_scripts
        and path.get_extension().to_lower() in ["tres", "res"]
        and _resource_requires_aot(path)
    ):
        if _export_prepared_transform(path, "resources", "res"):
            return
        if _strict_failure_injected:
            skip()
            return

    var source_path := path.trim_suffix("c") if path.ends_with(".gdc") else path
    if _ready and _compiled_scripts.has(source_path):
        skip()


func _resource_requires_aot(path: String) -> bool:
    if _resource_aot_requirements.has(path):
        return bool(_resource_aot_requirements[path])

    # Binary resources cannot be inspected safely without loading them. Preserve the existing
    # fail-closed transformation path for .scn/.res so embedded GDScript can never bypass source
    # stripping. Text resources can be classified without instantiating foreign ScriptLanguage
    # resources, which lets pure C#, GDExtension-language and data-only scenes follow Godot's
    # native exporter even when the current editor cannot execute that language.
    if path.get_extension().to_lower() in ["scn", "res"]:
        _resource_aot_requirements[path] = true
        return true

    var pending := PackedStringArray([path])
    var visited: Dictionary = {}
    while not pending.is_empty():
        var current := pending[pending.size() - 1]
        pending.resize(pending.size() - 1)
        if visited.has(current):
            continue
        visited[current] = true

        var extension := current.get_extension().to_lower()
        var source_path := current.trim_suffix("c") if current.ends_with(".gdc") else current
        if extension in ["gd", "gdc"] or _compiled_scripts.has(source_path):
            _resource_aot_requirements[path] = true
            return true
        if extension in ["scn", "res"]:
            _resource_aot_requirements[path] = true
            return true
        if extension not in ["tscn", "tres"]:
            continue
        if _text_resource_mentions_gdscript(current):
            _resource_aot_requirements[path] = true
            return true

        for encoded_dependency: String in ResourceLoader.get_dependencies(current):
            var dependency := _dependency_resource_path(encoded_dependency)
            if dependency.is_empty():
                continue
            var dependency_extension := dependency.get_extension().to_lower()
            if dependency_extension in ["gd", "gdc"]:
                _resource_aot_requirements[path] = true
                return true
            if dependency_extension in ["tscn", "tres", "scn", "res"]:
                pending.push_back(dependency)

    _resource_aot_requirements[path] = false
    return false


func _dependency_resource_path(encoded_dependency: String) -> String:
    for field: String in encoded_dependency.split("::", false):
        if field.begins_with("res://"):
            return field
        if field.begins_with("uid://"):
            var uid := ResourceUID.text_to_id(field)
            if uid != ResourceUID.INVALID_ID and ResourceUID.has_id(uid):
                var resolved := ResourceUID.get_id_path(uid)
                if resolved.begins_with("res://"):
                    return resolved
    return ""


func _text_resource_mentions_gdscript(path: String) -> bool:
    var file := FileAccess.open(path, FileAccess.READ)
    if file == null:
        # Classification must never turn an unreadable resource into an unchecked pass-through.
        return true
    var content := file.get_as_text()
    return (
        content.contains('type="GDScript"')
        or content.contains('type = "GDScript"')
        or content.contains("script/source")
        or content.contains('.gd"')
        or content.contains('.gdc"')
    )


func _export_prepared_transform(
    source_path: String,
    runtime_directory: String,
    extension: String
) -> bool:
    if not _prepared_export_transforms.has(source_path):
        _fail_export(
            "isolated resource transformation did not prepare exported path '%s'" % source_path
        )
        return false
    var entry: Dictionary = _prepared_export_transforms[source_path]
    var status := str(entry.get("status", "failed"))
    if status == "unchanged":
        return false
    if status != "transformed":
        _fail_export(
            "isolated resource transformation failed for '%s': %s"
            % [source_path, str(entry.get("error", "unknown worker failure"))]
        )
        return false
    var output_path := str(entry.get("output_path", ""))
    var bytes := FileAccess.get_file_as_bytes(output_path)
    var read_error := FileAccess.get_open_error()
    if read_error != OK or bytes.is_empty():
        _fail_export(
            "cannot read transformed resource '%s' (error %d)" % [source_path, read_error]
        )
        return false

    var runtime_path := RUNTIME_RESOURCE_PREFIX.path_join(
        "%s/%s-%s.%s" % [
            runtime_directory,
            _build_id.left(12),
            source_path.md5_text(),
            extension,
        ]
    )
    # `remap = true` replaces only the current scene/resource file. All customer ResourceLoader,
    # @tool and shader work happened in the isolated worker, so the editor process neither
    # re-evaluates unrelated assets nor inherits their diagnostic call stacks.
    add_file(runtime_path, bytes, true)
    _metrics_mutex.lock()
    _customized_scene_count += int(entry.get("scenes", 0))
    _replaced_node_count += int(entry.get("nodes", 0))
    _customized_resource_count += int(entry.get("resources", 0))
    _copied_property_count += int(entry.get("properties", 0))
    _metrics_mutex.unlock()
    return true


func _prepare_export_transforms() -> bool:
    var paths := _collect_export_transform_paths()
    if paths.is_empty():
        return true
    if (
        not _compiler.has_method(&"run_export_transform_worker")
        or not _compiler.has_method(&"install_editor_script_descriptors")
    ):
        _fail_export("compiler service does not provide isolated resource transformation")
        return false

    _transform_worker_directory = EXPORT_WORKER_ROOT.path_join(_build_id.left(12))
    var worker_absolute := ProjectSettings.globalize_path(_transform_worker_directory)
    if DirAccess.dir_exists_absolute(worker_absolute):
        if not _remove_directory_contents(worker_absolute):
            _fail_export("cannot clear the previous isolated resource transformation")
            return false
    var directory_error := DirAccess.make_dir_recursive_absolute(worker_absolute)
    if directory_error != OK and directory_error != ERR_ALREADY_EXISTS:
        _fail_export(
            "cannot create the isolated resource transformation directory (error %d)"
            % directory_error
        )
        return false

    var state_path := _transform_worker_directory.path_join("state.bin")
    var result_path := _transform_worker_directory.path_join("result.bin")
    var state_file := FileAccess.open(state_path, FileAccess.WRITE)
    if state_file == null:
        _fail_export("cannot create isolated resource transformation state")
        return false
    state_file.store_var({
        "schema": 1,
        "paths": paths,
        "output_root": _transform_worker_directory.path_join("resources"),
        "script_classes": _script_classes,
        "attached_script_bases": _attached_script_bases,
        "script_contract_hashes": _script_contract_hashes,
        "editor_script_descriptors": _editor_script_descriptors,
        "abstract_scripts": _abstract_scripts,
        "editor_only_scripts": _editor_only_scripts,
    }, true)
    state_file.flush()
    var state_error := state_file.get_error()
    state_file = null
    if state_error != OK:
        _fail_export(
            "cannot write isolated resource transformation state (error %d)" % state_error
        )
        return false

    var process: Dictionary = _compiler.run_export_transform_worker(state_path, result_path)
    if not bool(process.get("success", false)):
        var diagnostics: PackedStringArray = process.get("diagnostics", PackedStringArray())
        _fail_export(
            (
                "isolated resource transformer could not complete"
                if diagnostics.is_empty()
                else "\n".join(diagnostics)
            )
        )
        return false

    var result_file := FileAccess.open(result_path, FileAccess.READ)
    if result_file == null:
        _fail_export("isolated resource transformer produced no readable result")
        return false
    var result: Variant = result_file.get_var(true)
    var result_error := result_file.get_error()
    result_file = null
    if result_error != OK or not (result is Dictionary):
        _fail_export(
            "isolated resource transformer produced malformed state (error %d)" % result_error
        )
        return false
    var report: Dictionary = result
    if int(report.get("schema", 0)) != 1 or not bool(report.get("success", false)):
        _fail_export("isolated resource transformer rejected the project resource graph")
        return false
    var entries: Variant = report.get("entries")
    if not (entries is Dictionary):
        _fail_export("isolated resource transformer returned no path manifest")
        return false
    _prepared_export_transforms = entries
    for path: String in paths:
        if not _prepared_export_transforms.has(path):
            _fail_export(
                "isolated resource transformer omitted project path '%s'" % path
            )
            return false
    return true


func _collect_export_transform_paths() -> PackedStringArray:
    var result := PackedStringArray()
    var pending := PackedStringArray(["res://"])
    while not pending.is_empty():
        var directory := pending[pending.size() - 1]
        pending.resize(pending.size() - 1)
        for entry: String in ResourceLoader.list_directory(directory):
            var path := directory.path_join(entry)
            if entry.ends_with("/"):
                var directory_name := entry.trim_suffix("/")
                if (
                    directory_name.begins_with(".")
                    or path.begins_with(ADDON_PREFIX)
                ):
                    continue
                pending.push_back(path)
                continue
            if (
                path.begins_with(ADDON_PREFIX)
                or path.get_extension().to_lower() not in ["tscn", "scn", "tres", "res"]
                or not _resource_requires_aot(path)
            ):
                continue
            result.append(path)
    result.sort()
    return result


func prepare_isolated_transform_worker(compiler: Object, state: Dictionary) -> Dictionary:
    _worker_mode = true
    _worker_error = ""
    _compiler = compiler
    _script_classes = state.get("script_classes", {})
    _attached_script_bases = state.get("attached_script_bases", {})
    _script_contract_hashes = state.get("script_contract_hashes", {})
    _editor_script_descriptors = state.get("editor_script_descriptors", [])
    _abstract_scripts = state.get("abstract_scripts", {})
    _editor_only_scripts = state.get("editor_only_scripts", {})
    _compiled_scripts.clear()
    for path: String in _script_classes:
        _compiled_scripts[path] = true
    _ready = true
    _strict_failure_injected = false
    var install: Dictionary = _compiler.install_editor_script_descriptors(
        _editor_script_descriptors
    )
    if not bool(install.get("success", false)):
        return {
            "success": false,
            "error": "cannot install compiled script metadata in isolated transformer",
        }
    if not _validate_native_classes():
        return {
            "success": false,
            "error": (
                _worker_error
                if not _worker_error.is_empty()
                else "compiled script metadata is incomplete in isolated transformer"
            ),
        }
    return {"success": true}


func run_isolated_transform_worker(state: Dictionary) -> Dictionary:
    var output_root := str(state.get("output_root", ""))
    var output_absolute := ProjectSettings.globalize_path(output_root)
    var directory_error := DirAccess.make_dir_recursive_absolute(output_absolute)
    if (
        output_root.is_empty()
        or directory_error != OK and directory_error != ERR_ALREADY_EXISTS
    ):
        return {
            "schema": 1,
            "success": false,
            "error": "cannot create isolated transformed-resource output",
        }
    var entries: Dictionary = {}
    for path: String in state.get("paths", PackedStringArray()):
        entries[path] = _run_isolated_transform(path, output_root)
    return {
        "schema": 1,
        "success": true,
        "entries": entries,
    }


func finish_isolated_transform_worker() -> void:
    if _compiler != null and _compiler.has_method(&"clear_editor_script_descriptors"):
        _compiler.clear_editor_script_descriptors()
    _ready = false
    _compiled_scripts.clear()
    _script_classes.clear()
    _attached_script_bases.clear()
    _script_contract_hashes.clear()
    _editor_script_descriptors.clear()
    _abstract_scripts.clear()
    _editor_only_scripts.clear()
    _prepared_export_transforms.clear()
    _compiler = null


func _run_isolated_transform(path: String, output_root: String) -> Dictionary:
    _ready = true
    _strict_failure_injected = false
    _worker_error = ""
    var before := [
        _customized_scene_count,
        _replaced_node_count,
        _customized_resource_count,
        _copied_property_count,
    ]
    var extension := path.get_extension().to_lower()
    var output_extension := "scn" if extension in ["tscn", "scn"] else "res"
    var output_path := output_root.path_join("%s.%s" % [path.md5_text(), output_extension])
    var status := "unchanged"

    if extension in ["tscn", "scn"]:
        var packed := ResourceLoader.load(
            path,
            "PackedScene",
            ResourceLoader.CACHE_MODE_IGNORE
        ) as PackedScene
        if packed == null:
            _worker_error = "scene cannot be loaded"
            status = "failed"
        else:
            var source_root := packed.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
            if source_root == null:
                _worker_error = "scene cannot be instantiated"
                status = "failed"
            else:
                var transformed_root := _customize_scene(source_root, path)
                if transformed_root == null:
                    if is_instance_valid(source_root):
                        source_root.free()
                    status = "failed" if _strict_failure_injected else "unchanged"
                else:
                    var transformed_scene := PackedScene.new()
                    var pack_error := transformed_scene.pack(transformed_root)
                    transformed_root.free()
                    if pack_error != OK:
                        _worker_error = "scene cannot be packed (error %d)" % pack_error
                        status = "failed"
                    else:
                        var save_error := ResourceSaver.save(transformed_scene, output_path)
                        if save_error != OK:
                            _worker_error = "scene cannot be serialized (error %d)" % save_error
                            status = "failed"
                        else:
                            status = "transformed"
    else:
        var source := ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
        if source == null:
            _worker_error = "resource cannot be loaded"
            status = "failed"
        else:
            var replacements: Dictionary = {}
            var changes := {"count": 0}
            var transformed := _transform_resource_graph(
                source,
                path,
                replacements,
                changes,
                true
            )
            if transformed == null:
                status = "failed" if _strict_failure_injected else "unchanged"
            elif int(changes.get("count", 0)) > 0:
                var save_error := ResourceSaver.save(transformed, output_path)
                if save_error != OK:
                    _worker_error = "resource cannot be serialized (error %d)" % save_error
                    status = "failed"
                else:
                    status = "transformed"

    if status == "failed" and _worker_error.is_empty():
        _worker_error = "resource graph cannot be transformed safely"
    return {
        "status": status,
        "output_path": output_path if status == "transformed" else "",
        "error": _worker_error,
        "scenes": _customized_scene_count - int(before[0]),
        "nodes": _replaced_node_count - int(before[1]),
        "resources": _customized_resource_count - int(before[2]),
        "properties": _copied_property_count - int(before[3]),
    }


func _prepare_export(features: PackedStringArray, is_debug: bool) -> bool:
    if _build_progress != null:
        _build_progress.begin(PackedStringArray([
            "debug" if is_debug else "release",
        ]))
    var success := _prepare_export_impl(features, is_debug)
    if _build_progress != null:
        _build_progress.finish()
    return success


func _prepare_export_impl(features: PackedStringArray, is_debug: bool) -> bool:
    if _compiler == null:
        _fail_export("compiler service is not available; keeping GDScript sources")
        return false
    if not _remove_legacy_project_artifacts():
        _fail_export("cannot remove project libraries generated with the retired filename")
        return false

    _target_platform = _platform_from_features(features)
    _target_architecture = _architecture_from_features(features)
    _target_variant = _web_variant_from_features(features)
    _target_precision = _precision_from_features(features)
    if _target_platform.is_empty() or _target_architecture.is_empty():
        _fail_export(
            "unsupported or ambiguous export target features: %s; keeping GDScript sources"
            % [", ".join(features)]
        )
        return false
    if _target_precision.is_empty():
        _fail_export(
            "export target must expose exactly one Godot real_t precision feature "
            + "('single' or 'double'); keeping GDScript sources"
        )
        return false
    if not _compiler.is_target_supported(_target_platform, _target_architecture):
        _fail_export(
            "unsupported GDPP native export target '%s/%s'; keeping GDScript sources"
            % [_target_platform, _target_architecture]
        )
        return false
    if _target_platform == "web":
        if _target_variant.is_empty():
            _fail_export(
                "Web export must select exactly one thread mode (threads or nothreads)"
            )
            return false
        var preset := get_export_preset()
        if preset == null or preset.get("variant/extensions_support") != true:
            _fail_export(
                "Web AOT requires the export preset option 'Variant > Extensions Support'"
            )
            return false
    _build_profile = "debug" if is_debug else "release"

    var target_version := str(ProjectSettings.get_setting(TARGET_VERSION_SETTING, "4.4"))
    var sdk_root := str(ProjectSettings.get_setting(SDK_SETTING, ""))
    var cpp_compiler := str(ProjectSettings.get_setting(COMPILER_SETTING, ""))

    var distribution_compiler := cpp_compiler
    if _target_platform == "android":
        distribution_compiler = _android_compiler()
        if distribution_compiler.is_empty():
            return false
    elif _target_platform == "ios":
        distribution_compiler = _ios_compiler()
        if distribution_compiler.is_empty():
            return false
    elif _target_platform == "web":
        distribution_compiler = _web_compiler()
        if distribution_compiler.is_empty():
            return false

    if _build_progress != null:
        _build_progress.set_active_stage(_build_profile)
    _active_build_label = _build_profile
    var distribution_progress := Callable()
    if _build_progress != null and _build_progress.is_available():
        distribution_progress = Callable(self, "_on_native_build_progress")
    var distribution_outcome := _run_native_build({
        "project_root": "res://",
        "output_directory": OUTPUT_DIRECTORY,
        "sdk_root": sdk_root,
        "compiler_executable": distribution_compiler,
        "target_version": target_version,
        "build_profile": _build_profile,
        "target_platform": _target_platform,
        "target_architecture": _target_architecture,
        "target_variant": _target_variant,
        "target_precision": _target_precision,
    }, distribution_progress)
    if not _accept_build_outcome(distribution_outcome, _build_profile):
        return false
    var distribution_result: Dictionary = distribution_outcome.get("plan", {})

    _script_classes = distribution_result.get("script_classes", {})
    _attached_script_bases = distribution_result.get("attached_script_bases", {})
    _script_contract_hashes = distribution_result.get("script_contract_hashes", {})
    _editor_script_descriptors = distribution_result.get("editor_script_descriptors", [])
    _compiled_scripts.clear()
    for script_path: String in _script_classes:
        _compiled_scripts[script_path] = true
    _abstract_scripts.clear()
    for script_path: String in distribution_result.get("abstract_scripts", PackedStringArray()):
        _abstract_scripts[script_path] = true
    _editor_only_scripts.clear()
    for script_path: String in distribution_result.get(
        "editor_only_scripts", PackedStringArray()
    ):
        _editor_only_scripts[script_path] = true
    _build_id = str(distribution_result.get("build_id", ""))
    _output_library = str(distribution_result.get("output_library", ""))
    if not _native_artifact_exists(_output_library):
        _fail_export("native distribution library was not produced: %s" % _output_library)
        return false
    if not _install_editor_script_descriptors():
        return false
    if not _validate_native_classes():
        return false
    if not _prepare_autoloads():
        return false
    if not _prepare_export_transforms():
        return false

    var library_path := "res://addons/gdpp/binary/%s" % _output_library.get_file()
    _runtime_library_path = library_path
    _runtime_descriptor = (
        "[configuration]\n\n"
        + 'entry_symbol = "%s"\n' % PROJECT_LIBRARY_ENTRY_SYMBOL
        + "compatibility_minimum = \"%s\"\n" % target_version
        + "reloadable = false\n\n"
        + "[libraries]\n\n"
        + _runtime_library_entries(
            library_path,
            "threads" if _target_platform == "web" and _target_variant == "threads" else ""
        )
    )
    return true


func _run_native_build(request: Dictionary, progress_callback: Callable) -> Dictionary:
    var job := NATIVE_BUILD_JOB.new()
    var frame_callback := Callable()
    if _build_progress != null and _build_progress.is_available():
        frame_callback = Callable(_build_progress, "refresh")
    return job.run(_compiler, request, progress_callback, frame_callback)


func _accept_build_outcome(outcome: Dictionary, label: String) -> bool:
    _active_build_label = label
    var plan: Dictionary = outcome.get("plan", {})
    if not plan.get("success", false):
        _publish_nonfatal_build_diagnostics(plan, label)
        var plan_errors := _build_diagnostic_channel(plan, "errors", true)
        if plan_errors.is_empty():
            plan_errors.push_back("native build plan failed without an error diagnostic")
        for diagnostic: String in plan_errors:
            _fail_export("%s build: %s" % [label, diagnostic])
        return false
    var execution: Dictionary = outcome.get("execution", {})
    _publish_nonfatal_build_diagnostics(execution, label)
    if not execution.get("success", false):
        var execution_errors := _build_diagnostic_channel(execution, "errors", true)
        if execution_errors.is_empty():
            execution_errors.push_back("native build failed without an error diagnostic")
        for diagnostic: String in execution_errors:
            _fail_export("%s build: %s" % [label, diagnostic])
        return false
    return true


func _publish_nonfatal_build_diagnostics(result: Dictionary, label: String) -> void:
    var warnings := _build_diagnostic_channel(result, "warnings", not result.has("warnings"))
    for diagnostic: String in warnings:
        push_warning("GDPP: %s build: %s" % [label, diagnostic])
    var notes := _build_diagnostic_channel(result, "notes", false)
    for diagnostic: String in notes:
        print("GDPP: %s build: %s" % [label, diagnostic])


func _build_diagnostic_channel(
    result: Dictionary,
    channel: String,
    fallback_to_legacy_diagnostics: bool
) -> PackedStringArray:
    if result.has(channel):
        return result.get(channel, PackedStringArray())
    if fallback_to_legacy_diagnostics:
        return result.get("diagnostics", PackedStringArray())
    return PackedStringArray()


func _on_native_build_progress(phase: String, completed: int, total: int) -> void:
    if _build_progress != null:
        _build_progress.update(_active_build_label, phase, completed, total)


func _install_editor_script_descriptors() -> bool:
    if _editor_script_descriptors.size() != _script_classes.size():
        _fail_export("compiler returned an incomplete editor AOT metadata model")
        return false
    for entry: Variant in _editor_script_descriptors:
        if not (entry is Dictionary):
            _fail_export("compiler returned malformed editor AOT metadata")
            return false
        var script_path := str(entry.get("source_path", ""))
        if (
            script_path.is_empty()
            or not _script_classes.has(script_path)
            or str(entry.get("contract_hash", ""))
            != str(_script_contract_hashes.get(script_path, ""))
            or StringName(entry.get("native_base_type", &""))
            != StringName(_attached_script_bases.get(script_path, &""))
        ):
            _fail_export("compiler returned inconsistent editor metadata for '%s'" % script_path)
            return false

    # The project compiler owns this reflection model. Loading customer `.gd` resources here
    # would execute static initialization and can create GDScript resource cycles during export.
    var outcome: Dictionary = _compiler.install_editor_script_descriptors(
        _editor_script_descriptors
    )
    if not bool(outcome.get("success", false)):
        for diagnostic in outcome.get("diagnostics", []):
            push_error("GDPP: %s" % diagnostic)
        _fail_export("cannot install the editor AOT script metadata bridge")
        return false
    if int(outcome.get("registered_count", -1)) != _editor_script_descriptors.size():
        _fail_export("editor AOT script metadata bridge registered an incomplete project")
        return false
    return true


func _validate_native_classes() -> bool:
    _has_resource_scripts = false
    if not ClassDB.class_exists(&"AttachedCompiledScript"):
        _fail_export("attached script runtime class is unavailable")
        return false
    if _attached_script_bases.size() != _script_classes.size():
        _fail_export("compiled project does not provide an attached owner for every script")
        return false
    if _script_contract_hashes.size() != _script_classes.size():
        _fail_export("compiled project does not provide an ABI digest for every script")
        return false
    for script_path: String in _script_classes:
        if _editor_only_scripts.has(script_path):
            continue
        var attached_base := StringName(_attached_script_bases[script_path])
        if not ClassDB.class_exists(attached_base):
            _fail_export(
                "native owner '%s' for '%s' is unavailable" % [
                    attached_base,
                    script_path,
                ]
            )
            return false
        if not _abstract_scripts.has(script_path) and not ClassDB.can_instantiate(attached_base):
            _fail_export(
                "native owner '%s' for '%s' cannot be instantiated" % [
                    attached_base,
                    script_path,
                ]
            )
            return false
        if ClassDB.is_parent_class(attached_base, &"Resource"):
            _has_resource_scripts = true
    return true


func _prepare_autoloads() -> bool:
    for property: Dictionary in ProjectSettings.get_property_list():
        var setting := str(property.get("name", ""))
        if not setting.begins_with("autoload/"):
            continue
        var original := str(ProjectSettings.get_setting(setting, ""))
        var configured_path := original.trim_prefix("*")
        var script_path := _resolve_resource_uid(configured_path)
        if configured_path.begins_with("uid://") and script_path.is_empty():
            _fail_export("autoload '%s' has an unresolved resource UID" % setting)
            return false
        if not _compiled_scripts.has(script_path):
            continue
        if _editor_only_scripts.has(script_path):
            _fail_export("autoload '%s' uses an editor-only script" % setting)
            return false
        var attached_base := StringName(_attached_script_bases[script_path])
        # Export preparation must not construct customer autoloads: their field
        # initializers may access services that are only valid after SceneTree startup.
        if not ClassDB.is_parent_class(attached_base, &"Node"):
            _fail_export(
                "script autoload '%s' must compile to a Node-derived native class"
                % setting.trim_prefix("autoload/")
            )
            return false
        var autoload_name := setting.trim_prefix("autoload/")
        var generated_path := RUNTIME_RESOURCE_PREFIX + "autoload/%s.tscn" % setting.md5_text()
        var scene := (
            "[gd_scene load_steps=2 format=3]\n\n"
            + "[sub_resource type=\"AttachedCompiledScript\" id=\"AttachedScript\"]\n"
            + "source_path = \"%s\"\n\n" % script_path.c_escape()
            + "contract_hash = \"%s\"\n\n"
            % str(_script_contract_hashes[script_path]).c_escape()
            + "[node name=\"%s\" type=\"%s\"]\n" % [
                autoload_name.c_escape(),
                str(attached_base).c_escape(),
            ]
            + "script = SubResource(\"AttachedScript\")\n"
        )
        _autoload_files[generated_path] = scene
        _autoload_replacements[setting] = generated_path
        _autoload_originals[setting] = original
    return true


func _resolve_resource_uid(path: String) -> String:
    if not path.begins_with("uid://"):
        return path
    var resource_id := ResourceUID.text_to_id(path)
    if resource_id == ResourceUID.INVALID_ID or not ResourceUID.has_id(resource_id):
        return ""
    return ResourceUID.get_id_path(resource_id)


func _activate_autoloads() -> void:
    for setting: String in _autoload_replacements:
        var original := str(_autoload_originals[setting])
        var prefix := "*" if original.begins_with("*") else ""
        ProjectSettings.set_setting(setting, prefix + str(_autoload_replacements[setting]))
    for path: String in _autoload_files:
        add_file(path, str(_autoload_files[path]).to_utf8_buffer(), false)


func _restore_autoloads() -> void:
    for setting: String in _autoload_originals:
        ProjectSettings.set_setting(setting, _autoload_originals[setting])
    _autoload_originals.clear()


func _platform_from_features(features: PackedStringArray) -> String:
    var matches: Array[String] = []
    for value in ["macos", "windows", "linux", "android", "ios", "web"]:
        if features.has(value):
            matches.append(value)
    return matches[0] if matches.size() == 1 else ""


func _architecture_from_features(features: PackedStringArray) -> String:
    if _target_platform == "web":
        return "wasm32" if features.has("wasm32") else ""
    if _target_platform == "ios":
        return "arm64" if features.has("arm64") else ""
    if features.has("universal"):
        return "universal"
    var matches: Array[String] = []
    for value in ["arm64", "x86_64"]:
        if features.has(value):
            matches.append(value)
    if matches.size() == 2 and _target_platform == "macos":
        return "universal"
    if matches.size() == 1:
        return matches[0]
    if matches.is_empty() and _compiler != null:
        return _compiler.get_host_architecture()
    return ""


func _web_variant_from_features(features: PackedStringArray) -> String:
    if _target_platform != "web":
        return ""
    var matches: Array[String] = []
    for value in ["threads", "nothreads"]:
        if features.has(value):
            matches.append(value)
    return matches[0] if matches.size() == 1 else ""


func _precision_from_features(features: PackedStringArray) -> String:
    var matches: Array[String] = []
    for value in ["single", "double"]:
        if features.has(value):
            matches.append(value)
    return matches[0] if matches.size() == 1 else ""


func _android_compiler() -> String:
    var ndk_root := str(ProjectSettings.get_setting(ANDROID_NDK_SETTING, ""))
    if ndk_root.is_empty():
        _fail_export(
            "Android NDK is not configured; set '%s' to an installed NDK directory"
            % ANDROID_NDK_SETTING
        )
        return ""
    var host_tag := {
        "macOS": "darwin-x86_64",
        "Windows": "windows-x86_64",
        "Linux": "linux-x86_64",
    }.get(OS.get_name(), "")
    if host_tag.is_empty():
        _fail_export("Android cross-compilation is not supported on host '%s'" % OS.get_name())
        return ""
    var executable := "clang++.exe" if OS.get_name() == "Windows" else "clang++"
    var compiler := ndk_root.path_join(
        "toolchains/llvm/prebuilt/%s/bin/%s" % [host_tag, executable]
    )
    if not FileAccess.file_exists(compiler):
        _fail_export("Android NDK C++ compiler was not found: %s" % compiler)
        return ""
    return compiler


func _web_compiler() -> String:
    var compiler := str(ProjectSettings.get_setting(EMSCRIPTEN_CXX_SETTING, "em++"))
    if compiler.strip_edges().is_empty():
        _fail_export(
            "Emscripten C++ compiler is not configured; set '%s' to em++"
            % EMSCRIPTEN_CXX_SETTING
        )
        return ""
    return compiler


func _ios_compiler() -> String:
    if OS.get_name() != "macOS":
        _fail_export("iOS AOT export requires macOS with a complete Xcode installation")
        return ""
    var output: Array = []
    var status := OS.execute("xcrun", ["--find", "clang++"], output, true)
    if status != 0 or output.is_empty():
        _fail_export(
            "Xcode C++ tools were not found; install Xcode and run xcode-select first"
        )
        return ""
    return "xcrun"


func _native_artifact_exists(path: String) -> bool:
    if FileAccess.file_exists(path):
        return true
    var absolute := ProjectSettings.globalize_path(path)
    return (
        path.get_extension().to_lower() == "xcframework"
        and DirAccess.dir_exists_absolute(absolute)
        and FileAccess.file_exists(path.path_join("Info.plist"))
    )


func _runtime_library_entries(library_path: String, feature := "") -> String:
    var prefix := _target_platform
    if not feature.is_empty():
        prefix += "." + feature
    if not _target_precision.is_empty():
        prefix += "." + _target_precision
    # Universal 2 is a Mach-O payload property, not a Godot runtime feature.
    # Each process reports its active CPU architecture, and both entries load
    # the same fat dylib.
    if _target_platform == "macos" and _target_architecture == "universal":
        return (
            "%s.arm64 = \"%s\"\n%s.x86_64 = \"%s\"\n"
            % [prefix, library_path, prefix, library_path]
        )
    return "%s.%s = \"%s\"\n" % [prefix, _target_architecture, library_path]


func _shared_object_tags() -> PackedStringArray:
    var tags := PackedStringArray()
    if not _target_platform.is_empty():
        tags.append(_target_platform)
    if (
        not _target_architecture.is_empty()
        and not (_target_platform == "macos" and _target_architecture == "universal")
    ):
        tags.append(_target_architecture)
    if _target_platform == "web" and _target_variant == "threads":
        tags.append("threads")
    if not _target_precision.is_empty():
        tags.append(_target_precision)
    return tags


func _register_runtime_library() -> bool:
    if (
        not _include_project_extension
        or _runtime_descriptor.is_empty()
        or _runtime_library_path.is_empty()
    ):
        return true
    if not _register_shared_object_once(_runtime_library_path, _shared_object_tags()):
        return false
    if (
        _target_platform == "ios"
        and (
            _runtime_library_path.ends_with(".a")
            or _runtime_library_path.ends_with(".xcframework")
        )
    ):
        return _register_apple_embedded_entry(PROJECT_LIBRARY_ENTRY_SYMBOL)
    return true


func _register_shared_object_once(
    resource_path: String,
    tags: PackedStringArray,
    target := ""
) -> bool:
    if resource_path.is_empty():
        _fail_export("cannot register an empty native-library path")
        return false
    var absolute_path := ProjectSettings.globalize_path(resource_path)
    if (
        not FileAccess.file_exists(resource_path)
        and not FileAccess.file_exists(absolute_path)
        and not DirAccess.dir_exists_absolute(absolute_path)
    ):
        _fail_export("native export dependency does not exist: %s" % resource_path)
        return false
    var registration_key := "%s\n%s" % [resource_path, target]
    if _registered_shared_objects.has(registration_key):
        return true
    add_shared_object(resource_path, tags, target)
    _registered_shared_objects[registration_key] = true
    return true


func _register_apple_embedded_entry(entry_symbol: String) -> bool:
    if _registered_apple_entries.has(entry_symbol):
        return true
    var modern_api := has_method(&"add_apple_embedded_platform_cpp_code")
    var legacy_api := has_method(&"add_ios_cpp_code")
    if not modern_api and not legacy_api:
        _fail_export("Godot does not expose Apple embedded export registration APIs")
        return false

    var callback_symbol := (
        "add_apple_embedded_platform_init_callback"
        if modern_api
        else "add_ios_init_callback"
    )
    var additional_code := (
        "extern void register_dynamic_symbol(char *name, void *address);\n"
        + "extern void $CALLBACK(void (*cb)());\n"
        + "\n"
        + "extern \"C\" void $ENTRY();\n"
        + "void $ENTRY_init() {\n"
        + (
            "  if (&$ENTRY) "
            + "register_dynamic_symbol((char *)\"$ENTRY\", (void *)$ENTRY);\n"
        )
        + "}\n"
        + "struct $ENTRY_struct {\n"
        + "  $ENTRY_struct() {\n"
        + "    $CALLBACK($ENTRY_init);\n"
        + "  }\n"
        + "};\n"
        + "$ENTRY_struct $ENTRY_struct_instance;\n\n"
    )
    additional_code = additional_code.replace("$CALLBACK", callback_symbol)
    additional_code = additional_code.replace("$ENTRY", entry_symbol)
    var linker_flags := "-Wl,-U,_%s" % entry_symbol
    if modern_api:
        call(&"add_apple_embedded_platform_cpp_code", additional_code)
        call(&"add_apple_embedded_platform_linker_flags", linker_flags)
    else:
        call(&"add_ios_cpp_code", additional_code)
        call(&"add_ios_linker_flags", linker_flags)
    _registered_apple_entries[entry_symbol] = true
    return true


func _collect_scene_replacement_plan(
    scene_root: Node,
    scene_state: SceneState,
    scene_path: String,
    replacement_plan: Array[Dictionary]
) -> bool:
    # Inherited scenes serialize only their differences. Once their root script
    # is replaced by a native class, the inherited root is materialized, so the
    # script properties owned by every base SceneState must be transformed too.
    # Process derived-to-base and let the first occurrence of a node path win;
    # this preserves both script overrides and explicit `script = null` values.
    var handled_script_paths: Dictionary = {}
    for state: SceneState in _scene_state_chain(scene_state):
        for node_index in state.get_node_count():
            var has_script_property := false
            var script_value: Variant = null
            for property_index in state.get_node_property_count(node_index):
                if state.get_node_property_name(node_index, property_index) == &"script":
                    has_script_property = true
                    script_value = state.get_node_property_value(node_index, property_index)
                    break
            if not has_script_property:
                continue

            var node_path := state.get_node_path(node_index)
            var path_key := str(node_path)
            if handled_script_paths.has(path_key):
                continue
            handled_script_paths[path_key] = true
            var node := _state_node(scene_root, node_path)
            if node == null:
                push_error(
                    "GDPP: scene '%s' cannot resolve serialized node '%s'" % [
                        scene_path,
                        node_path,
                    ]
                )
                return false

            if script_value == null:
                # A parent scene may explicitly clear a script inherited from an
                # instanced PackedScene. The child scene is AOT-transformed to a
                # native type, so keeping only `script = null` would no longer
                # remove its behavior. Materialize just this overridden node as
                # its original engine class.
                if state != scene_state or _is_nested_scene_override(node, scene_root):
                    replacement_plan.append({
                        "node": node,
                        "native_class": StringName(node.get_class()),
                        "script_path": "",
                        "stored_properties": _serialized_node_properties(
                            scene_state,
                            node_path
                        ),
                    })
                continue

            if not script_value is Script:
                push_error(
                    "GDPP: scene '%s' has a non-Script value in '%s:script'" % [
                        scene_path,
                        node_path,
                    ]
                )
                return false
            var script_path := _script_path(node)
            if script_path.is_empty():
                script_path = (script_value as Script).resource_path
            if _editor_only_scripts.has(script_path):
                push_error(
                    "GDPP: runtime scene '%s' uses editor-only script '%s'"
                    % [scene_path, script_path]
                )
                return false
            if not _script_classes.has(script_path):
                if _is_gdscript_provider(script_value as Script, script_path):
                    push_error(
                        "GDPP: scene '%s' uses uncompiled script '%s'"
                        % [scene_path, script_path]
                    )
                    return false
                # Scripts owned by another installed ScriptLanguage remain attached to their
                # original node. GDPP compiles only GDScript and must not require C#, custom
                # language, or editor extension projects to rewrite valid customer scenes.
                continue
            var attached_base := StringName(_attached_script_bases[script_path])
            if not node.is_class(attached_base):
                push_error(
                    (
                        "GDPP: object '%s' for '%s' is not an instance of attached owner '%s'"
                    )
                    % [node.get_class(), script_path, attached_base]
                )
                return false
            replacement_plan.append({
                "node": node,
                "native_class": attached_base,
                "script_path": script_path,
                "attached": true,
                "stored_properties": _serialized_node_properties(
                    scene_state,
                    node_path
                ),
            })

    # SceneState stores parents before their children. Replacing from leaves to
    # root keeps every NodePath and owner valid while parent nodes are swapped.
    replacement_plan.reverse()
    return true


func _scene_state_chain(scene_state: SceneState) -> Array[SceneState]:
    var result: Array[SceneState] = []
    var current := scene_state
    while current != null:
        result.append(current)
        # Godot 4.5 exposed SceneState's existing base-state traversal to scripts. On the
        # supported 4.4 baseline every scene resource is still transformed independently, but
        # its local SceneState is the only state the editor API can inspect safely.
        if not current.has_method(&"get_base_scene_state"):
            break
        current = current.call(&"get_base_scene_state") as SceneState
    return result


func _serialized_node_properties(scene_state: SceneState, node_path: NodePath) -> Dictionary:
    var result: Dictionary = {}
    # Derived states override base states. Keep the first serialized value for
    # each name while still materializing inherited properties on the new node.
    for state: SceneState in _scene_state_chain(scene_state):
        for node_index in state.get_node_count():
            if state.get_node_path(node_index) != node_path:
                continue
            for property_index in state.get_node_property_count(node_index):
                var name := str(state.get_node_property_name(node_index, property_index))
                if name == "script" or result.has(name):
                    continue
                result[name] = state.get_node_property_value(node_index, property_index)
            break
    return result


func _is_nested_scene_override(node: Node, scene_root: Node) -> bool:
    var current := node
    while current != null and current != scene_root:
        if not current.scene_file_path.is_empty():
            return true
        current = current.get_parent()
    return false


func _state_node(scene_root: Node, path: NodePath) -> Node:
    if path == NodePath(".") or path.is_empty():
        return scene_root
    return scene_root.get_node_or_null(path)


func _replace_planned_nodes(
    scene_root: Node,
    replacement_plan: Array[Dictionary],
    replacement_nodes: Dictionary,
    resource_replacements: Dictionary,
    resource_changes: Dictionary,
    scene_path: String
) -> Node:
    # Prepare and populate every replacement before touching the source tree.
    # A missing class/property therefore fails the complete scene atomically
    # instead of leaving a partially replaced graph whose scripts are stripped.
    var prepared: Dictionary = {}
    var attached_scripts: Dictionary = {}
    var copied_properties := 0
    for plan: Dictionary in replacement_plan:
        var node: Node = plan.node
        if bool(plan.get("attached", false)):
            var compiled_script := _make_attached_script(str(plan.script_path))
            if compiled_script == null:
                _free_prepared_replacements(prepared)
                return null
            attached_scripts[node.get_instance_id()] = compiled_script
            continue
        var native_class := StringName(plan.native_class)
        var replacement := ClassDB.instantiate(native_class) as Node
        if replacement == null:
            push_error("GDPP: cannot instantiate native scene class '%s'" % native_class)
            _free_prepared_replacements(prepared)
            return null
        var copied := _copy_storage_properties(
            node,
            replacement,
            plan.get("stored_properties", {}),
            resource_replacements,
            resource_changes,
            scene_path
        )
        if copied < 0:
            replacement.free()
            _free_prepared_replacements(prepared)
            return null
        copied_properties += copied
        replacement.name = node.name
        replacement.unique_name_in_owner = node.unique_name_in_owner
        for group: StringName in node.get_groups():
            replacement.add_to_group(group, true)
        prepared[node.get_instance_id()] = replacement

    var result := scene_root
    for plan: Dictionary in replacement_plan:
        var node: Node = plan.node
        if bool(plan.get("attached", false)):
            var attached_copied := _install_attached_script(
                node,
                attached_scripts[node.get_instance_id()],
                plan.get("stored_properties", {}),
                resource_replacements,
                resource_changes,
                scene_path
            )
            if attached_copied < 0:
                _free_prepared_replacements(prepared)
                return null
            copied_properties += attached_copied
            continue
        var replaces_root := node == result
        var replacement: Node = prepared[node.get_instance_id()]
        _replace_node(node, replacement, replacement_nodes)
        if replaces_root:
            result = replacement
    replacement_nodes["__gdpp_copied_properties"] = copied_properties
    return result


func _transform_serialized_scene_resources(
    scene_root: Node,
    scene_state: SceneState,
    scene_path: String,
    replacements: Dictionary,
    changes: Dictionary
) -> void:
    # Script-backed built-in Resources may be stored on engine nodes that do
    # not themselves have a script. Walk exactly the properties serialized by
    # the SceneState chain, so optional/non-storage getters are never observed.
    var handled_properties: Dictionary = {}
    for state: SceneState in _scene_state_chain(scene_state):
        for node_index in state.get_node_count():
            var node_path := state.get_node_path(node_index)
            var node := _state_node(scene_root, node_path)
            if node == null:
                continue
            for property_index in state.get_node_property_count(node_index):
                var name := str(state.get_node_property_name(node_index, property_index))
                if name == "script":
                    continue
                var property_key := "%s\n%s" % [node_path, name]
                if handled_properties.has(property_key):
                    continue
                handled_properties[property_key] = true
                var original: Variant = node.get(name)
                if not (original is Resource or original is Array or original is Dictionary):
                    continue
                var change_count_before := int(changes.get("count", 0))
                var transformed := _sanitize_storage_value(
                    original,
                    replacements,
                    changes,
                    scene_path
                )
                if int(changes.get("count", 0)) <= change_count_before:
                    continue
                if name.begins_with("metadata/"):
                    node.set_meta(name.trim_prefix("metadata/"), transformed)
                else:
                    node.set(name, transformed)


func _replace_node(
    node: Node,
    replacement: Node,
    replacement_nodes: Dictionary
) -> void:
    # ClassDB instances start with an engine-generated name. Node.replace_by() does not promise to
    # transfer the old name, but NodePath references are serialized against it.
    var old_id := node.get_instance_id()
    var original_owner := node.owner
    var parent := node.get_parent()
    if parent != null:
        node.replace_by(replacement, true)
        replacement.owner = original_owner
        _replace_owner(replacement, node, replacement)
    else:
        for child in node.get_children():
            var owned_by_root := child.owner == node
            if owned_by_root:
                child.owner = null
            node.remove_child(child)
            replacement.add_child(child)
            if owned_by_root:
                child.owner = replacement
        _replace_owner(replacement, node, replacement)
    replacement_nodes[old_id] = replacement
    node.free()


func _free_prepared_replacements(prepared: Dictionary) -> void:
    for replacement: Node in prepared.values():
        replacement.free()


func _replace_owner(node: Node, old_owner: Node, new_owner: Node) -> void:
    for child in node.get_children():
        if child.owner == old_owner:
            child.owner = new_owner
        _replace_owner(child, old_owner, new_owner)


func _make_attached_script(script_path: String) -> Script:
    var instance := ClassDB.instantiate(&"AttachedCompiledScript")
    if not instance is Script:
        if instance != null:
            instance.free()
        push_error("GDPP: cannot instantiate the attached script runtime")
        return null
    var script := instance as Script
    script.set("source_path", script_path)
    script.set("contract_hash", str(_script_contract_hashes.get(script_path, "")))
    if (
        script.get_instance_base_type().is_empty()
        or str(script.get("source_path")) != script_path
        or str(script.get("contract_hash")) != str(_script_contract_hashes.get(script_path, ""))
    ):
        push_error("GDPP: compiled script descriptor for '%s' is unavailable" % script_path)
        return null
    return script


func _storage_property_values(
    source: Object,
    serialized_properties: Variant = null
) -> Dictionary:
    if serialized_properties is Dictionary:
        var values := (serialized_properties as Dictionary).duplicate()
        # SceneState intentionally exposes serialized Object references as NodePath values and
        # may retain null sentinels written by older scene schemas for non-null scalar fields.
        # GDScript resolves those encodings before populating the instance. Read only those
        # indirect values back from the live source object; all ordinary serialized values remain
        # untouched, so export does not introduce additional custom-accessor observations.
        var source_script := source.get_script() as Script
        if source_script == null:
            return values
        for property: Dictionary in source_script.get_script_property_list():
            var property_name := str(property.get("name", ""))
            var usage := int(property.get("usage", 0))
            var property_type := int(property.get("type", TYPE_NIL))
            if (
                property_name.is_empty()
                or (usage & PROPERTY_USAGE_STORAGE) == 0
                or not values.has(property_name)
            ):
                continue
            var serialized_value: Variant = values[property_name]
            if (
                (serialized_value is NodePath and property_type == TYPE_OBJECT)
                or (serialized_value == null and property_type != TYPE_NIL)
            ):
                values[property_name] = source.get(property_name)
        return values
    var values: Dictionary = {}
    for property: Dictionary in source.get_property_list():
        var property_name := str(property.get("name", ""))
        var usage := int(property.get("usage", 0))
        if property_name == "script" or (usage & PROPERTY_USAGE_STORAGE) == 0:
            continue
        values[property_name] = source.get(property_name)
    return values


func _script_storage_property_names(source: Object) -> Dictionary:
    var result: Dictionary = {}
    var source_script := source.get_script() as Script
    if source_script == null:
        return result
    for property: Dictionary in source_script.get_script_property_list():
        var usage := int(property.get("usage", 0))
        if (usage & PROPERTY_USAGE_STORAGE) != 0:
            result[str(property.get("name", ""))] = true
    return result


func _restore_storage_properties(
    source_class: StringName,
    destination: Object,
    source_properties: Dictionary,
    replacements: Dictionary,
    changes: Dictionary,
    context_path: String,
    source_script_properties: Dictionary = {},
    preserve_unlisted_native_state: bool = false
) -> int:
    var destination_properties: Dictionary = {}
    for property: Dictionary in destination.get_property_list():
        destination_properties[str(property.get("name", ""))] = true
    var copied := 0
    for name: String in source_properties:
        # Godot exposes Object metadata as dynamic `metadata/<key>` storage
        # properties. A fresh native instance does not list those properties
        # until the metadata is created, so treating them as a missing native
        # field would reject otherwise valid scenes. Preserve both editor and
        # runtime metadata through Object's metadata API.
        if name.begins_with("metadata/"):
            var metadata_name := name.trim_prefix("metadata/")
            destination.set_meta(
                metadata_name,
                _sanitize_storage_value(
                    source_properties[name],
                    replacements,
                    changes,
                    context_path
                )
            )
            copied += 1
            continue
        if not destination_properties.has(name):
            # Some engine classes accept compatibility storage keys through their native
            # serialization hooks without publishing them in get_property_list(). AnimationPlayer
            # "libraries" is one example. Attaching a compiled script mutates this same object, so
            # those native values are already present and are sanitized by the serialized-resource
            # pass that follows. Original script fields do not receive this exemption: if one is
            # absent from the compiled descriptor, fail closed as a compiler contract error.
            if preserve_unlisted_native_state and not source_script_properties.has(name):
                copied += 1
                continue
            push_error(
                "GDPP: native class '%s' is missing stored property '%s' from '%s'" % [
                    destination.get_class(),
                    name,
                    source_class,
                ]
            )
            return -1
        destination.set(
            name,
            _sanitize_storage_value(
                source_properties[name],
                replacements,
                changes,
                context_path
            )
        )
        copied += 1
    return copied


func _install_attached_script(
    object: Object,
    script: Script,
    serialized_properties: Variant = null,
    resource_replacements: Variant = null,
    resource_changes: Variant = null,
    context_path: String = ""
) -> int:
    var replacements: Dictionary = (
        resource_replacements if resource_replacements is Dictionary else {}
    )
    var changes: Dictionary = resource_changes if resource_changes is Dictionary else {"count": 0}
    var source_class := StringName(object.get_class())
    var source_properties := _storage_property_values(object, serialized_properties)
    var source_script_properties := _script_storage_property_names(object)
    var source_script := object.get_script() as Script
    _disconnect_replaced_script_connections(object, source_script)
    object.set_script(script)
    if object.get_script() != script:
        push_error(
            "GDPP: third-party object '%s' rejected compiled script '%s'" % [
                source_class,
                str(script.get("source_path")),
            ]
        )
        return -1
    var stored_properties := PackedStringArray()
    for property_name: String in source_properties:
        stored_properties.append(property_name)
    if (
        _compiler == null
        or not _compiler.has_method(&"set_editor_script_storage_state")
        or not _compiler.set_editor_script_storage_state(object, stored_properties)
    ):
        push_error(
            "GDPP: cannot commit export storage state for compiled script '%s'"
            % str(script.get("source_path"))
        )
        return -1
    return _restore_storage_properties(
        source_class,
        object,
        source_properties,
        replacements,
        changes,
        context_path,
        source_script_properties,
        true
    )


func _disconnect_replaced_script_connections(object: Object, source_script: Script) -> void:
    # @tool Resource constructors and setters commonly connect child Resource.changed signals back
    # to methods on the script instance. Replacing that instance with an export-only metadata
    # descriptor invalidates those transient callbacks. They are recreated by the compiled
    # behavior constructor in the exported process and are not serialized scene connections.
    # SceneState-owned Node connections were already snapshotted by _snapshot_connections().
    var script_methods: Dictionary = {}
    var current_script := source_script
    while current_script != null:
        for method: Dictionary in current_script.get_script_method_list():
            script_methods[StringName(method.get("name", ""))] = true
        current_script = current_script.get_base_script()
    for connection: Dictionary in object.get_incoming_connections():
        var signal_value: Variant = connection.get("signal")
        var callable_value: Variant = connection.get("callable")
        if not (signal_value is Signal) or not (callable_value is Callable):
            continue
        var source_signal: Signal = signal_value
        var target_callable: Callable = callable_value
        if (
            target_callable.get_object() != object
            or not script_methods.has(target_callable.get_method())
        ):
            continue
        if source_signal.is_connected(target_callable):
            source_signal.disconnect(target_callable)


func _attach_compiled_script(
    object: Object,
    script_path: String,
    serialized_properties: Variant = null,
    resource_replacements: Variant = null,
    resource_changes: Variant = null,
    context_path: String = ""
) -> int:
    var script := _make_attached_script(script_path)
    if script == null:
        return -1
    return _install_attached_script(
        object,
        script,
        serialized_properties,
        resource_replacements,
        resource_changes,
        context_path
    )


func _copy_storage_properties(
    source: Object,
    destination: Object,
    serialized_properties: Variant = null,
    resource_replacements: Variant = null,
    resource_changes: Variant = null,
    context_path: String = ""
) -> int:
    var replacements: Dictionary = (
        resource_replacements if resource_replacements is Dictionary else {}
    )
    var changes: Dictionary = resource_changes if resource_changes is Dictionary else {"count": 0}
    return _restore_storage_properties(
        StringName(source.get_class()),
        destination,
        _storage_property_values(source, serialized_properties),
        replacements,
        changes,
        context_path
    )


func _sanitize_storage_value(
    value: Variant,
    resource_replacements: Dictionary = {},
    resource_changes: Dictionary = {"count": 0},
    context_path: String = ""
) -> Variant:
    # Godot's typed Array/Dictionary metadata can name the original GDScript
    # resource. The exporter then rejects the native replacement even though it
    # represents the same project class. Rebuild containers without script
    # constraints while recursively preserving their values and insertion order.
    if value is Array:
        var result: Array = []
        result.resize(value.size())
        for index in value.size():
            result[index] = _sanitize_storage_value(
                value[index],
                resource_replacements,
                resource_changes,
                context_path
            )
        return result
    if value is Dictionary:
        var result: Dictionary = {}
        for key: Variant in value:
            result[
                _sanitize_storage_value(
                    key,
                    resource_replacements,
                    resource_changes,
                    context_path
                )
            ] = _sanitize_storage_value(
                value[key],
                resource_replacements,
                resource_changes,
                context_path
            )
        return result
    if value is Resource:
        return _transform_resource_graph(
            value,
            context_path,
            resource_replacements,
            resource_changes,
            false
        )
    return value


func _transform_resource_graph(
    resource: Resource,
    context_path: String,
    replacements: Dictionary,
    changes: Dictionary,
    is_root: bool
) -> Resource:
    if resource == null:
        return null
    var instance_id := resource.get_instance_id()
    if replacements.has(instance_id):
        return replacements[instance_id]

    # External Resources are exported through their own path and receive an
    # explicit remap in _export_file(). Only the current root and built-in
    # subresources may be mutated here; this preserves sharing and relative
    # dependency semantics across scenes.
    var resource_path := resource.resource_path
    if not is_root and not resource_path.is_empty() and not resource_path.contains("::"):
        return resource

    if resource is PackedScene:
        # PackedScene-valued properties can contain their own serialized node
        # graph (for example a projectile template embedded in a tower Resource).
        # Traversing `_bundled` as an ordinary Dictionary would retain Script
        # variants; instantiate and repack it so node types are native too.
        replacements[instance_id] = resource
        var nested_root := (resource as PackedScene).instantiate(
            PackedScene.GEN_EDIT_STATE_INSTANCE
        )
        if nested_root == null:
            replacements.erase(instance_id)
            _fail_export("embedded PackedScene in '%s' cannot be instantiated" % context_path)
            return null
        var transformed_root := _transform_scene_with_state(
            nested_root,
            (resource as PackedScene).get_state(),
            "%s::PackedScene" % context_path,
            replacements,
            changes
        )
        if transformed_root == null:
            if is_instance_valid(nested_root):
                nested_root.free()
            if _strict_failure_injected:
                replacements.erase(instance_id)
                return null
            return resource
        var transformed_scene := PackedScene.new()
        var pack_error := transformed_scene.pack(transformed_root)
        transformed_root.free()
        if pack_error != OK:
            replacements.erase(instance_id)
            _fail_export(
                "embedded PackedScene in '%s' cannot be packed (error %d)"
                % [context_path, pack_error]
            )
            return null
        transformed_scene.resource_name = resource.resource_name
        transformed_scene.resource_local_to_scene = resource.resource_local_to_scene
        for metadata_name: StringName in resource.get_meta_list():
            transformed_scene.set_meta(metadata_name, resource.get_meta(metadata_name))
        replacements[instance_id] = transformed_scene
        changes.count = int(changes.get("count", 0)) + 1
        return transformed_scene

    var script_path := _script_path(resource)
    if not script_path.is_empty():
        if _editor_only_scripts.has(script_path):
            _fail_export(
                "runtime resource graph '%s' uses editor-only script '%s'"
                % [context_path, script_path]
            )
            return null
        if not _script_classes.has(script_path):
            var source_script := resource.get_script() as Script
            if _is_gdscript_provider(source_script, script_path):
                _fail_export(
                    "resource graph '%s' uses an uncompiled script '%s'"
                    % [context_path, script_path]
                )
                return null
        else:
            # Install the identity mapping before copying so self-references and
            # cyclic built-in Resource graphs retain their exact topology.
            replacements[instance_id] = resource
            var copied := _attach_compiled_script(
                resource,
                script_path,
                null,
                replacements,
                changes,
                context_path
            )
            if copied < 0:
                replacements.erase(instance_id)
                _fail_export(
                    "resource graph '%s' cannot attach compiled script '%s'" % [
                        context_path,
                        script_path,
                    ]
                )
                return null
            changes.count = int(changes.get("count", 0)) + 1
            _metrics_mutex.lock()
            _customized_resource_count += 1
            _copied_property_count += copied
            _metrics_mutex.unlock()
            return resource

    replacements[instance_id] = resource
    # Traverse storage properties only. Godot's 4.4/4.5 global resource pass
    # reads every Object property, including optional getters such as
    # GPUParticles3D.draw_pass_2..4; restricting traversal to serialized state
    # avoids observable reads that are invalid for the current object shape.
    for property: Dictionary in resource.get_property_list():
        var name := str(property.get("name", ""))
        var usage := int(property.get("usage", 0))
        if name == "script" or (usage & PROPERTY_USAGE_STORAGE) == 0:
            continue
        var original: Variant = resource.get(name)
        if not (original is Resource or original is Array or original is Dictionary):
            continue
        var change_count_before := int(changes.get("count", 0))
        var transformed := _sanitize_storage_value(
            original,
            replacements,
            changes,
            context_path
        )
        if int(changes.get("count", 0)) > change_count_before:
            resource.set(name, transformed)
    return resource


func _script_path(object: Object) -> String:
    var script: Script = object.get_script()
    return "" if script == null else script.resource_path


func _is_gdscript_provider(script: Script, path: String) -> bool:
    var source_path := path.trim_suffix("c") if path.ends_with(".gdc") else path
    if _compiled_scripts.has(source_path):
        return true
    var extension := path.get_extension().to_lower()
    return extension in ["gd", "gdc"] or (script != null and script.is_class(&"GDScript"))


func _snapshot_connections(root: Node, scene_state: SceneState) -> Array[Dictionary]:
    var result: Array[Dictionary] = []
    # SceneState lists only the connections serialized by this scene. Reading
    # it avoids copying persistent connections owned by nested PackedScenes
    # into their parent, which would make signals fire more than once.
    for state: SceneState in _scene_state_chain(scene_state):
        for connection_index in state.get_connection_count():
            var source := _state_node(
                root,
                state.get_connection_source(connection_index)
            )
            var target := _state_node(
                root,
                state.get_connection_target(connection_index)
            )
            if source == null or target == null:
                continue
            var signal_name := state.get_connection_signal(connection_index)
            var callable := Callable(
                target,
                state.get_connection_method(connection_index)
            ).bindv(state.get_connection_binds(connection_index))
            var unbound := state.get_connection_unbinds(connection_index)
            if unbound > 0:
                callable = callable.unbind(unbound)
            var signal_value := Signal(source, signal_name)
            # The same connection can appear in more than one state in an
            # inheritance chain. Disconnecting it once naturally deduplicates
            # the snapshot without inventing a separate key format.
            if not signal_value.is_connected(callable):
                continue
            result.append({
                "source_id": source.get_instance_id(),
                "source": source,
                "signal": signal_name,
                "target_id": target.get_instance_id(),
                "target": target,
                "method": state.get_connection_method(connection_index),
                "bound": state.get_connection_binds(connection_index),
                "unbound": unbound,
                "flags": state.get_connection_flags(connection_index),
            })
            signal_value.disconnect(callable)
    return result


func _restore_connections(
    connection_snapshots: Array[Dictionary],
    replacement_nodes: Dictionary
) -> void:
    for connection: Dictionary in connection_snapshots:
        var source: Object = replacement_nodes.get(
            connection.source_id,
            connection.source
        )
        var target: Object = replacement_nodes.get(
            connection.target_id,
            connection.target
        )
        if source == null or target == null:
            continue
        var callable := Callable(target, connection.method).bindv(connection.bound)
        if int(connection.unbound) > 0:
            callable = callable.unbind(int(connection.unbound))
        var signal_value := Signal(source, connection.signal)
        if not signal_value.is_connected(callable):
            signal_value.connect(callable, int(connection.flags))


func _fail_export(message: String) -> void:
    if _worker_mode:
        _worker_error = message
        _strict_failure_injected = true
        push_error("GDPP export worker: %s" % message)
        return
    _ready = false
    push_error("GDPP export: %s" % message)
    var platform := get_export_platform()
    if platform != null:
        var allow_fallback: bool = get_option(ALLOW_SOURCE_FALLBACK_OPTION) == true
        var fail_closed: bool = get_option(STRIP_OPTION) == true and not allow_fallback
        platform.add_message(
            (
                EditorExportPlatform.EXPORT_MESSAGE_ERROR
                if fail_closed
                else EditorExportPlatform.EXPORT_MESSAGE_WARNING
            ),
            "GDPP AOT",
            (
                "%s; binary-only export is blocked to prevent source disclosure"
                if fail_closed
                else "%s; this export keeps the original GDScript sources"
            ) % message
        )
        if fail_closed:
            _inject_strict_export_failure()


func _inject_strict_export_failure() -> void:
    if _strict_failure_injected:
        return
    _strict_failure_injected = true
    var extension := {
        "macos": "dylib",
        "linux": "so",
        "windows": "dll",
        "android": "so",
        "ios": "xcframework",
        "web": "wasm",
    }.get(_target_platform, "invalid")
    var sentinel := "res://addons/gdpp/build/__gdpp_aot_export_failed__.%s" % extension
    var tags := _shared_object_tags()
    # EditorExportPlugin has no public abort callback in Godot 4.4–4.7.
    # A deliberately absent shared object turns a fail-closed AOT error into
    # the platform export's own ERR_FILE_NOT_FOUND instead of a successful
    # package that silently contains customer source code.
    add_shared_object(sentinel, tags, "")


func _prepare_extension_registry(include_project_extension := true) -> bool:
    _include_project_extension = include_project_extension
    var retained: PackedStringArray = []
    var compiler_registered := false
    var original := ""
    if FileAccess.file_exists(EXTENSION_REGISTRY):
        original = FileAccess.get_file_as_string(EXTENSION_REGISTRY)
        for line in original.split("\n", false):
            var value := line.strip_edges()
            if value.is_empty():
                continue
            if value == COMPILER_DESCRIPTOR:
                compiler_registered = true
                continue
            if value.begins_with(OUTPUT_DIRECTORY + "/"):
                continue
            if not retained.has(value):
                retained.append(value)
    if not _prepare_provider_export_overrides(retained):
        return false
    if include_project_extension:
        if not compiler_registered:
            _fail_export(
                (
                    "Godot's extension registry does not contain '%s'; "
                    + "rescan the project after installing GDPP"
                )
                % COMPILER_DESCRIPTOR
            )
            return false
        retained.append(COMPILER_DESCRIPTOR)
    _export_extension_registry = "" if retained.is_empty() else "\n".join(retained) + "\n"

    # Successful AOT and desktop fallback exports keep Godot's registry byte-stable. The physical
    # editor descriptor already occupies the runtime descriptor's virtual path, so Godot's forced
    # registry filter preserves that path while _export_file() replaces only the packaged bytes.
    if include_project_extension:
        return true

    # A deliberate source-only export (or a non-desktop source fallback) has no runtime extension.
    # Godot 4.4-4.7 regenerates this forced metadata after plugin callbacks, so this one bounded
    # transaction is still required to remove GDPP's descriptor from that package. It only touches
    # generated `.godot` metadata; editor and customer extension descriptors remain immutable.
    if not _export_extension_registry.is_empty():
        add_file(EXTENSION_REGISTRY, _export_extension_registry.to_utf8_buffer(), false)
    if original != _export_extension_registry:
        if not _write_text_file(EXTENSION_REGISTRY_BACKUP, original):
            _fail_export("cannot create the extension registry transaction backup")
            return false
        if not _write_text_file(EXTENSION_REGISTRY, _export_extension_registry):
            DirAccess.remove_absolute(ProjectSettings.globalize_path(EXTENSION_REGISTRY_BACKUP))
            _fail_export("cannot prepare the runtime extension registry")
            return false
        _extension_registry_original = original
        _extension_registry_modified = true
    return true


func _clear_godot_export_cache() -> bool:
    var absolute := ProjectSettings.globalize_path(GODOT_EXPORT_CACHE_DIRECTORY)
    if not DirAccess.dir_exists_absolute(absolute):
        return true
    return _remove_directory_contents(absolute)


func _remove_legacy_project_artifacts() -> bool:
    var binary_absolute := ProjectSettings.globalize_path(BINARY_DIRECTORY)
    if not DirAccess.dir_exists_absolute(binary_absolute):
        return true
    var directory := DirAccess.open(binary_absolute)
    if directory == null:
        return false
    var legacy_paths: PackedStringArray = []
    directory.list_dir_begin()
    while true:
        var name := directory.get_next()
        if name.is_empty():
            break
        var lower := name.to_lower()
        if (
            lower.begins_with("gdpp_project.")
            or lower.begins_with("libgdpp_project.")
        ):
            legacy_paths.append(binary_absolute.path_join(name))
    directory.list_dir_end()
    for path in legacy_paths:
        if DirAccess.dir_exists_absolute(path):
            if not _remove_directory_contents(path):
                return false
        if DirAccess.remove_absolute(path) != OK:
            return false
    return true


func _remove_directory_contents(absolute: String) -> bool:
    var directory := DirAccess.open(absolute)
    if directory == null:
        return false
    var success := true
    directory.list_dir_begin()
    while true:
        var name := directory.get_next()
        if name.is_empty():
            break
        if name in [".", ".."]:
            continue
        var child := absolute.path_join(name)
        if directory.current_is_dir():
            if not _remove_directory_contents(child):
                success = false
            elif DirAccess.remove_absolute(child) != OK:
                success = false
        elif DirAccess.remove_absolute(child) != OK:
            success = false
    directory.list_dir_end()
    return success


func _prepare_provider_export_overrides(descriptors: PackedStringArray) -> bool:
    if _target_platform != "macos" or _target_architecture != "universal":
        return true

    var normalized_features: Dictionary = _export_features.duplicate()
    for architecture: String in ARCHITECTURE_FEATURES:
        normalized_features.erase(architecture)
    normalized_features["universal"] = true

    for path: String in descriptors:
        if path.get_extension().to_lower() != "gdextension":
            continue
        if not FileAccess.file_exists(path):
            _fail_export("provider extension descriptor does not exist: %s" % path)
            return false
        var config := ConfigFile.new()
        if config.load(path) != OK:
            _fail_export("cannot parse provider extension descriptor: %s" % path)
            return false
        if not config.has_section("libraries"):
            continue

        var pairs: Dictionary = {}
        var library_keys := config.get_section_keys("libraries")
        for key: String in library_keys:
            var parts := key.split(".", false)
            if not parts.has("macos") or not parts.has("arm64"):
                continue
            var architecture_index := parts.find("arm64")
            var sibling_parts := parts.duplicate()
            sibling_parts[architecture_index] = "x86_64"
            var sibling_key := ".".join(sibling_parts)
            if not config.has_section_key("libraries", sibling_key):
                continue
            var arm_library := str(config.get_value("libraries", key, ""))
            var x86_library := str(config.get_value("libraries", sibling_key, ""))
            if arm_library.is_empty() or arm_library != x86_library:
                continue
            var universal_parts := parts.duplicate()
            universal_parts[architecture_index] = "universal"
            var universal_key := ".".join(universal_parts)
            if (
                config.has_section_key("libraries", universal_key)
                and str(config.get_value("libraries", universal_key, "")) != arm_library
            ):
                _fail_export(
                    (
                        "provider extension '%s' has conflicting '%s' and dual-architecture "
                        + "library entries"
                    )
                    % [path, universal_key]
                )
                return false
            if not _is_universal_macos_library(arm_library):
                _fail_export(
                    (
                        "provider extension '%s' maps arm64 and x86_64 to '%s', but the "
                        + "library is not a verifiable Universal 2 Mach-O"
                    )
                    % [path, arm_library]
                )
                return false
            pairs[universal_key] = {
                "arm": key,
                "x86": sibling_key,
                "library": arm_library,
            }

        if pairs.is_empty():
            continue
        var original := FileAccess.get_file_as_string(path)
        if original.is_empty():
            _fail_export("cannot read provider extension descriptor: %s" % path)
            return false
        for universal_key: String in pairs:
            var pair: Dictionary = pairs[universal_key]
            config.erase_section_key("libraries", str(pair.arm))
            config.erase_section_key("libraries", str(pair.x86))
            config.set_value("libraries", universal_key, str(pair.library))

        var selection_features := normalized_features
        var selection := _select_extension_library(config, path, selection_features)
        var release_fallback := false
        if selection.is_empty() and _build_profile == "debug":
            selection_features = normalized_features.duplicate()
            selection_features.erase("debug")
            selection_features["release"] = true
            selection = _select_extension_library(config, path, selection_features)
            release_fallback = not selection.is_empty()
        if selection.is_empty():
            _fail_export(
                "provider extension '%s' has no Universal 2 library for this export" % path
            )
            return false
        var selected_path := str(selection.get("path", ""))
        if not _is_universal_macos_library(selected_path):
            _fail_export(
                "provider extension '%s' selected a non-Universal library '%s'"
                % [path, selected_path]
            )
            return false
        if not _register_shared_object_once(selected_path, _shared_object_tags()):
            return false
        if not _register_extension_dependencies(
            config,
            path,
            selection_features
        ):
            return false
        if release_fallback:
            var runtime_config := ConfigFile.new()
            if runtime_config.load(path) != OK:
                _fail_export("cannot rebuild provider extension descriptor: %s" % path)
                return false
            runtime_config.set_value(
                "libraries",
                "macos.debug.arm64",
                selected_path
            )
            runtime_config.set_value(
                "libraries",
                "macos.debug.x86_64",
                selected_path
            )
            _provider_descriptor_overrides[path] = runtime_config.encode_to_text()
        else:
            _provider_descriptor_overrides[path] = original
    return true


func _select_extension_library(
    config: ConfigFile,
    descriptor_path: String,
    features: Dictionary
) -> Dictionary:
    var best_path := ""
    var best_tags := PackedStringArray()
    for key: String in config.get_section_keys("libraries"):
        var tags := key.split(".", false)
        var matches := true
        for tag: String in tags:
            if not features.has(tag.strip_edges()):
                matches = false
                break
        if not matches or tags.size() <= best_tags.size():
            continue
        best_path = str(config.get_value("libraries", key, ""))
        best_tags = tags
    if best_path.is_empty():
        return {}
    if best_path.is_relative_path():
        best_path = descriptor_path.get_base_dir().path_join(best_path)
    return {
        "path": best_path,
        "tags": best_tags,
    }


func _register_extension_dependencies(
    config: ConfigFile,
    descriptor_path: String,
    features: Dictionary
) -> bool:
    if not config.has_section("dependencies"):
        return true
    for key: String in config.get_section_keys("dependencies"):
        var tags := key.split(".", false)
        var matches := true
        for tag: String in tags:
            if not features.has(tag.strip_edges()):
                matches = false
                break
        if not matches:
            continue
        var raw_dependencies: Variant = config.get_value("dependencies", key, {})
        if not raw_dependencies is Dictionary:
            _fail_export(
                "provider extension '%s' has malformed dependencies for '%s'"
                % [descriptor_path, key]
            )
            return false
        var dependencies: Dictionary = raw_dependencies
        for dependency_key: Variant in dependencies:
            var dependency_path := str(dependency_key)
            if dependency_path.is_relative_path():
                dependency_path = descriptor_path.get_base_dir().path_join(dependency_path)
            var target := str(dependencies[dependency_key])
            if not _register_shared_object_once(dependency_path, tags, target):
                return false
        break
    return true


func _is_universal_macos_library(resource_path: String) -> bool:
    var binary_path := ProjectSettings.globalize_path(resource_path)
    if DirAccess.dir_exists_absolute(binary_path) and resource_path.ends_with(".framework"):
        binary_path = binary_path.path_join(resource_path.get_file().get_basename())
    if not FileAccess.file_exists(binary_path):
        return false
    var output: Array = []
    if OS.execute("/usr/bin/lipo", ["-archs", binary_path], output, true) != 0:
        return false
    var architectures := PackedStringArray()
    for line: Variant in output:
        architectures.append_array(str(line).strip_edges().split(" ", false))
    return architectures.has("arm64") and architectures.has("x86_64")


func _restore_export_transaction() -> void:
    _restore_provider_descriptors()
    _restore_compiler_descriptor()
    _restore_extension_registry()


func _restore_provider_descriptors() -> void:
    # Migration recovery for an interrupted export performed by GDPP 1.7.8 or
    # earlier. New exports never write provider descriptors or this backup.
    if not FileAccess.file_exists(PROVIDER_DESCRIPTORS_BACKUP):
        return
    var recovered: Variant = JSON.parse_string(
        FileAccess.get_file_as_string(PROVIDER_DESCRIPTORS_BACKUP)
    )
    if not recovered is Dictionary:
        push_error("GDPP export: cannot parse the provider descriptor transaction backup")
        return
    var originals: Dictionary = recovered
    for path: String in originals:
        if not _write_text_file(path, str(originals[path])):
            push_error("GDPP export: cannot restore provider descriptor '%s'" % path)
            return
    DirAccess.remove_absolute(ProjectSettings.globalize_path(PROVIDER_DESCRIPTORS_BACKUP))


func _restore_compiler_descriptor() -> void:
    # Migration recovery for the old descriptor-rewrite transaction. The
    # compiler descriptor remains immutable in all new exports.
    if not FileAccess.file_exists(COMPILER_DESCRIPTOR_BACKUP):
        return
    var original := FileAccess.get_file_as_string(COMPILER_DESCRIPTOR_BACKUP)
    if not _write_text_file(COMPILER_DESCRIPTOR, original):
        push_error("GDPP export: cannot restore the editor compiler descriptor")
        return
    DirAccess.remove_absolute(ProjectSettings.globalize_path(COMPILER_DESCRIPTOR_BACKUP))


func _restore_extension_registry() -> void:
    if not _extension_registry_modified and FileAccess.file_exists(EXTENSION_REGISTRY_BACKUP):
        _extension_registry_original = FileAccess.get_file_as_string(EXTENSION_REGISTRY_BACKUP)
        _extension_registry_modified = true
    if not _extension_registry_modified:
        return
    if not _write_text_file(EXTENSION_REGISTRY, _extension_registry_original):
        push_error("GDPP export: cannot restore the editor extension registry")
        return
    DirAccess.remove_absolute(ProjectSettings.globalize_path(EXTENSION_REGISTRY_BACKUP))
    _extension_registry_original = ""
    _extension_registry_modified = false


func _write_text_file(path: String, contents: String) -> bool:
    var file := FileAccess.open(path, FileAccess.WRITE)
    if file == null:
        return false
    file.store_string(contents)
    return file.get_error() == OK


func _remove_successful_export_fallback() -> void:
    if _export_output_path.is_empty() or (not _ready and _include_project_extension):
        return
    var extension := {
        "macos": "dylib",
        "linux": "so",
        "windows": "dll",
        "android": "so",
    }.get(_target_platform, "")
    if extension.is_empty():
        return
    var prefix := "" if _target_platform == "windows" else "lib"
    var filename := "%sgdpp_fallback.%s.%s.%s" % [
        prefix,
        _target_platform,
        _target_architecture,
        extension,
    ]
    var exported_path := _export_output_path.get_base_dir().path_join(filename)
    if FileAccess.file_exists(exported_path):
        DirAccess.remove_absolute(exported_path)


func _activate_fallback_descriptors(is_debug: bool) -> void:
    if _target_platform.is_empty() or _target_architecture.is_empty():
        return
    var extension := {
        "macos": "dylib",
        "linux": "so",
        "windows": "dll",
    }.get(_target_platform, "")
    if extension.is_empty():
        _runtime_descriptor = ""
        _runtime_library_path = ""
        return
    var prefix := "" if _target_platform == "windows" else "lib"
    var filename := "%sgdpp_fallback.%s.%s.%s" % [
        prefix,
        _target_platform,
        _target_architecture,
        extension,
    ]
    var library_path := "res://addons/gdpp/binary/%s" % filename
    _runtime_library_path = library_path
    var feature := "debug" if is_debug else "release"
    var descriptor := (
        "[configuration]\n\n"
        + "entry_symbol = \"gdpp_export_fallback_library_init\"\n"
        + "compatibility_minimum = \"4.4\"\n"
        + "reloadable = false\n\n"
        + "[libraries]\n\n"
        + _runtime_library_entries(library_path, feature)
    )
    _runtime_descriptor = descriptor


func _prepare_script_class_cache() -> bool:
    if not FileAccess.file_exists(SCRIPT_CLASS_CACHE):
        _export_script_class_cache = ""
        return true
    var config := ConfigFile.new()
    if config.load(SCRIPT_CLASS_CACHE) != OK:
        _fail_export("cannot read Godot's global script class cache")
        return false
    var entries: Array = config.get_value("", "list", [])
    var retained: Array = []
    for entry: Dictionary in entries:
        if not _compiled_scripts.has(str(entry.get("path", ""))):
            retained.append(entry)
    config.set_value("", "list", retained)
    _export_script_class_cache = config.encode_to_text()
    return true


func _record_scene_metrics(replaced_nodes: int, copied_properties: int) -> void:
    _metrics_mutex.lock()
    _customized_scene_count += 1
    _replaced_node_count += replaced_nodes
    _copied_property_count += copied_properties
    _metrics_mutex.unlock()


func _print_export_summary() -> void:
    if not _ready:
        return
    _metrics_mutex.lock()
    var scene_count := _customized_scene_count
    var node_count := _replaced_node_count
    var resource_count := _customized_resource_count
    var property_count := _copied_property_count
    _metrics_mutex.unlock()
    var cache_state := "reused_or_empty" if scene_count == 0 and resource_count == 0 else "rebuilt"
    print(
        "GDPP_AOT_SUMMARY scenes=%d nodes=%d resources=%d properties=%d cache=%s" % [
            scene_count,
            node_count,
            resource_count,
            property_count,
            cache_state,
        ]
    )


func _reset_export_state() -> void:
    if _compiler != null and _compiler.has_method(&"clear_editor_script_descriptors"):
        _compiler.clear_editor_script_descriptors()
    var worker_root_absolute := ProjectSettings.globalize_path(EXPORT_WORKER_ROOT)
    if DirAccess.dir_exists_absolute(worker_root_absolute):
        if _remove_directory_contents(worker_root_absolute):
            DirAccess.remove_absolute(worker_root_absolute)
    _ready = false
    _script_classes.clear()
    _attached_script_bases.clear()
    _script_contract_hashes.clear()
    _editor_script_descriptors.clear()
    _compiled_scripts.clear()
    _abstract_scripts.clear()
    _editor_only_scripts.clear()
    _resource_aot_requirements.clear()
    _runtime_descriptor = ""
    _runtime_library_path = ""
    _output_library = ""
    _build_id = ""
    _target_platform = ""
    _target_architecture = ""
    _target_variant = ""
    _target_precision = ""
    _build_profile = ""
    _export_extension_registry = ""
    _export_script_class_cache = ""
    _export_features.clear()
    _provider_descriptor_overrides.clear()
    _registered_shared_objects.clear()
    _registered_apple_entries.clear()
    _include_project_extension = false
    _has_resource_scripts = false
    _export_output_path = ""
    _autoload_files.clear()
    _autoload_replacements.clear()
    _autoload_originals.clear()
    _prepared_export_transforms.clear()
    _transform_worker_directory = ""
    _worker_mode = false
    _worker_error = ""
    _metrics_mutex.lock()
    _customized_scene_count = 0
    _replaced_node_count = 0
    _customized_resource_count = 0
    _copied_property_count = 0
    _strict_failure_injected = false
    _metrics_mutex.unlock()
