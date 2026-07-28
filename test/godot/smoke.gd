extends SceneTree

const EXPORT_PLUGIN := preload("res://addons/gdpp/export_plugin.gd")


func _init() -> void:
    call_deferred("_run")


func _run() -> void:
    var compiler := GDPPCompiler.new()
    if not compiler.is_target_supported("windows", "x86_64"):
        push_error("GDPP rejected the shipped Windows x86_64 target")
        quit(1)
        return
    if compiler.is_target_supported("windows", "arm64"):
        push_error("GDPP advertised the unavailable Windows arm64 target")
        quit(1)
        return
    var result: Dictionary = compiler.compile_source(
        "extends Node\nclass_name NativeSmoke\nfunc answer() -> int:\n    return 42\n",
        "native_smoke.gd"
    )
    if not result.get("success", false):
        push_error("GDPP smoke compilation failed: %s" % result.get("diagnostics", []))
        quit(1)
        return
    if "GDCLASS(GDPPNative_NativeSmoke, godot::Node)" not in result.get("header", ""):
        push_error("GDPP generated an unexpected class")
        quit(1)
        return
    var optimization: Dictionary = result.get("optimization", {})
    if optimization.get("constants_folded", -1) < 0:
        push_error("GDPP did not expose optimization statistics")
        quit(1)
        return
    if not _verify_script_language_boundaries():
        quit(1)
        return
    if not _verify_resource_transform_boundaries():
        quit(1)
        return
    print("GDPP_SMOKE_OK")
    quit(0)


func _verify_script_language_boundaries() -> bool:
    var gdscript := GDScript.new()
    if not EXPORT_PLUGIN._is_gdscript_provider_for(
        gdscript,
        "res://fixture.custom",
        {}
    ):
        push_error("GDPP did not identify a GDScript provider independently of its suffix")
        return false
    if not EXPORT_PLUGIN._is_gdscript_provider_for(null, "res://fixture.gdc", {}):
        push_error("GDPP did not identify a compiled GDScript suffix")
        return false
    var foreign := ClassDB.instantiate(&"AttachedCompiledScript") as Script
    if foreign == null:
        push_error("GDPP could not instantiate the foreign ScriptExtension oracle")
        return false
    if EXPORT_PLUGIN._is_gdscript_provider_for(
        foreign,
        "res://fixture.foreign",
        {}
    ):
        push_error("GDPP classified another ScriptLanguage provider as GDScript")
        return false
    return true


func _verify_resource_transform_boundaries() -> bool:
    var fixture_root := "res://addons/gdpp/build/resource-transform-boundary"
    var absolute_root := ProjectSettings.globalize_path(fixture_root)
    var directory_error := DirAccess.make_dir_recursive_absolute(absolute_root)
    if directory_error != OK and directory_error != ERR_ALREADY_EXISTS:
        push_error("GDPP could not create its resource transformation boundary fixture")
        return false
    var files := {
        "child.gd": "extends Node\n",
        "foreign.cs": "using Godot;\npublic partial class Foreign : Node {}\n",
        "child.tscn": (
            "[gd_scene load_steps=2 format=3]\n\n"
            + "[ext_resource type=\"Script\" path=\"%s/child.gd\" id=\"1\"]\n\n"
            + "[node name=\"Child\" type=\"Node\"]\n"
            + "script = ExtResource(\"1\")\n"
        ) % fixture_root,
        "foreign_parent.tscn": (
            "[gd_scene load_steps=3 format=3]\n\n"
            + "[ext_resource type=\"Script\" path=\"%s/foreign.cs\" id=\"1\"]\n"
            + "[ext_resource type=\"PackedScene\" path=\"%s/child.tscn\" id=\"2\"]\n\n"
            + "[node name=\"Foreign\" type=\"Node\"]\n"
            + "script = ExtResource(\"1\")\n\n"
            + "[node name=\"Child\" parent=\".\" instance=ExtResource(\"2\")]\n"
        ) % [fixture_root, fixture_root],
    }
    for name: String in files:
        var file := FileAccess.open(fixture_root.path_join(name), FileAccess.WRITE)
        if file == null:
            push_error("GDPP could not create resource boundary fixture '%s'" % name)
            return false
        file.store_string(str(files[name]))
        file = null
    if not EXPORT_PLUGIN._resource_requires_aot_for(
        fixture_root.path_join("child.tscn"),
        {},
        {}
    ):
        push_error("GDPP did not select a scene with a direct GDScript owner")
        return false
    if EXPORT_PLUGIN._resource_requires_aot_for(
        fixture_root.path_join("foreign_parent.tscn"),
        {},
        {}
    ):
        push_error("GDPP pulled a foreign-language resource into a nested AOT transform")
        return false
    return true
