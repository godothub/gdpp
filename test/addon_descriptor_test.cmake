set(runtime_descriptor "${GDPP_TEST_SOURCE_DIR}/example/addons/gdpp/gdpp_project.gdextension")
if(EXISTS "${runtime_descriptor}")
    message(FATAL_ERROR
        "The runtime descriptor must be generated into the export, not scanned beside the compiler")
endif()

file(READ "${GDPP_TEST_SOURCE_DIR}/example/addons/gdpp/plugin.cfg" plugin_metadata)
foreach(required_plugin_metadata IN ITEMS
        "name=\"GDPP\""
        "description=\"GDScript AOT & Extension\""
        "author=\"GodotHub\""
        "website=\"https://godothub.com\""
        "version=\"1.8.2\""
        "script=\"plugin.gd\"")
    string(FIND "${plugin_metadata}" "${required_plugin_metadata}" metadata_offset)
    if(metadata_offset EQUAL -1)
        message(FATAL_ERROR
            "Plugin metadata is missing: ${required_plugin_metadata}")
    endif()
endforeach()

set(compiler_descriptor
    "${GDPP_TEST_SOURCE_DIR}/example/addons/gdpp/gdpp.gdextension")
file(READ "${compiler_descriptor}" compiler_content)
string(FIND "${compiler_content}" "reloadable = false" compiler_reloadable_offset)
if(compiler_reloadable_offset EQUAL -1)
    message(FATAL_ERROR
        "Compiler descriptor must not advertise unsupported GDExtension hot reload")
endif()
set(compiler_entries "macos.editor.single.arm64" "macos.editor.single.x86_64")
set(compiler_libraries
    "libgdpp_compiler.macos.universal.dylib"
    "libgdpp_compiler.macos.universal.dylib")
foreach(entry library IN ZIP_LISTS compiler_entries compiler_libraries)
    string(FIND "${compiler_content}"
        "${entry} = \"res://addons/gdpp/binary/${library}\"" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR
            "Compiler descriptor does not map ${entry} to ${library}")
    endif()
endforeach()
string(REGEX MATCH "macos\\.[^=\n]*universal[^=\n]* =" invalid_compiler_feature
    "${compiler_content}")
if(invalid_compiler_feature)
    message(FATAL_ERROR
        "Compiler descriptor uses 'universal' as a Godot feature tag: "
        "${invalid_compiler_feature}")
endif()

file(READ "${GDPP_TEST_SOURCE_DIR}/example/addons/gdpp/export_plugin.gd" export_plugin)
file(READ "${GDPP_TEST_SOURCE_DIR}/example/addons/gdpp/build_progress.gd" build_progress)
file(READ "${GDPP_TEST_SOURCE_DIR}/example/addons/gdpp/native_build_job.gd" native_build_job)
file(READ "${GDPP_TEST_SOURCE_DIR}/example/addons/gdpp/plugin.gd" editor_plugin)
file(READ "${GDPP_TEST_SOURCE_DIR}/src/integration/godot/compiler_service.cpp" compiler_service)
file(READ "${GDPP_TEST_SOURCE_DIR}/cmake/GodotPlugin.cmake" godot_plugin_build)
string(FIND "${compiler_service}"
    "output[\"attached_script_bases\"] = attached_script_bases;"
    attached_metadata_offset)
if(attached_metadata_offset EQUAL -1)
    message(FATAL_ERROR
        "Compiler service does not expose third-party attachment metadata to the exporter")
endif()
string(FIND "${compiler_service}"
    "output[\"editor_only_scripts\"] = editor_only_scripts;"
    editor_only_metadata_offset)
if(editor_only_metadata_offset EQUAL -1)
    message(FATAL_ERROR
        "Compiler service does not expose editor-only script metadata to the exporter")
endif()
foreach(required_runtime_export IN ITEMS
        "return \"AOT GDPP scene transformer r%d %s\""
        "add_file(COMPILER_DESCRIPTOR, _runtime_descriptor.to_utf8_buffer(), false)"
        "func _register_runtime_library() -> bool:"
        "func _register_shared_object_once("
        "add_shared_object(resource_path, tags, target)"
        "func _register_apple_embedded_entry(entry_symbol: String) -> bool:"
        "add_apple_embedded_platform_cpp_code"
        "add_ios_cpp_code"
        "add_apple_embedded_platform_init_callback"
        "add_ios_init_callback"
        "var linker_flags := \"-Wl,-U,_%s\" % entry_symbol"
        "func _prepare_provider_export_overrides(descriptors: PackedStringArray) -> bool:"
        "_provider_descriptor_overrides[path] = original"
        "selection_features.erase(\"debug\")"
        "\"macos.debug.arm64\""
        "runtime_config.encode_to_text()"
        "func _clear_godot_export_cache() -> bool:"
        "func _remove_legacy_project_artifacts() -> bool:"
        "if not _remove_legacy_project_artifacts():"
        "func _is_gdscript_provider(script: Script, path: String) -> bool:"
        "script.is_class(&\"GDScript\")"
        "if _is_gdscript_provider(script_value as Script, script_path):"
        "func _restore_compiler_descriptor() -> void:")
    string(FIND "${export_plugin}" "${required_runtime_export}" runtime_export_offset)
    if(runtime_export_offset EQUAL -1)
        message(FATAL_ERROR
            "Single-descriptor export transaction is missing: ${required_runtime_export}")
    endif()
endforeach()
foreach(forbidden_descriptor_mutation IN ITEMS
        "func _prepare_compiler_descriptor() -> bool:"
        "_write_text_file(COMPILER_DESCRIPTOR, _export_scan_descriptor)"
        "_write_text_file(PROVIDER_DESCRIPTORS_BACKUP"
        "_write_text_file(path, str(rewritten[path]))")
    string(FIND "${export_plugin}" "${forbidden_descriptor_mutation}"
        descriptor_mutation_offset)
    if(NOT descriptor_mutation_offset EQUAL -1)
        message(FATAL_ERROR
            "Export must not mutate editor or provider descriptors: "
            "${forbidden_descriptor_mutation}")
    endif()
endforeach()
foreach(required_target_matrix IN ITEMS
        "if not _compiler.is_target_supported(_target_platform, _target_architecture):")
    string(FIND "${export_plugin}" "${required_target_matrix}" target_matrix_offset)
    if(target_matrix_offset EQUAL -1)
        message(FATAL_ERROR
            "Export target capability preflight is missing: ${required_target_matrix}")
    endif()
endforeach()
foreach(required_target_service IN ITEMS
        "godot::D_METHOD(\"is_target_supported\", \"platform\", \"architecture\")"
        "native_architecture_supported(*parsed_platform, native_string(architecture))")
    string(FIND "${compiler_service}" "${required_target_service}" target_service_offset)
    if(target_service_offset EQUAL -1)
        message(FATAL_ERROR
            "Compiler target capability service is missing: ${required_target_service}")
    endif()
endforeach()
foreach(required_windows_process_contract IN ITEMS
        "CreateProcessW("
        "CreatePipe(&output_read, &output_write"
        "CreateFileW(L\"NUL\""
        "STARTF_USESHOWWINDOW"
        "STARTF_USESTDHANDLES"
        "SW_HIDE"
        "CREATE_NO_WINDOW"
        "CreateDesktopW("
        "CREATE_UNICODE_ENVIRONMENT"
        "VSCMD_SKIP_SENDTELEMETRY"
        "VSCMD_SKIP_VCPKG_ACTIVATION"
        "vswhere.exe"
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
        "GDPP_VCVARS_PATH"
        "cached_msvc_environment("
        "resolve_msvc_executable("
        "resolve_msvc_compiler_for_plan("
        "build_options.compiler_executable = resolved_compiler_executable;"
        "const bool requires_resolution = !requested.has_parent_path();"
        "options.environment = &environment->block;"
        "return execute_hidden_windows_command_line(std::move(command_line), options);"
        "return execute_hidden_windows_process(wide_arguments);")
    string(FIND "${compiler_service}" "${required_windows_process_contract}"
        windows_process_offset)
    if(windows_process_offset EQUAL -1)
        message(FATAL_ERROR
            "Background Windows export process contract is missing: "
            "${required_windows_process_contract}")
    endif()
endforeach()
string(FIND "${compiler_service}"
    "if (!windows_environment(L\"INCLUDE\"))"
    inherited_msvc_path_trust_offset)
if(NOT inherited_msvc_path_trust_offset EQUAL -1)
    message(FATAL_ERROR
        "MSVC executable resolution must not trust an inherited INCLUDE variable")
endif()
foreach(forbidden_machine_specific_toolchain_path IN ITEMS
        "windows_environment(L\"USERPROFILE\")"
        "\"software/VS\"")
    string(FIND "${compiler_service}" "${forbidden_machine_specific_toolchain_path}"
        machine_specific_toolchain_offset)
    if(NOT machine_specific_toolchain_offset EQUAL -1)
        message(FATAL_ERROR
            "Production toolchain discovery contains a machine-specific path: "
            "${forbidden_machine_specific_toolchain_path}")
    endif()
endforeach()
string(FIND "${compiler_service}" "execute_with_vcvars(" repeated_vcvars_offset)
if(NOT repeated_vcvars_offset EQUAL -1)
    message(FATAL_ERROR
        "MSVC environment initialization must not wrap every compiler command")
endif()
string(FIND "${godot_plugin_build}"
    "target_link_libraries(gdpp_godot_plugin PRIVATE user32)"
    hidden_desktop_link_offset)
if(hidden_desktop_link_offset EQUAL -1)
    message(FATAL_ERROR
        "Windows compiler plugin does not link the isolated desktop API")
endif()
string(FIND "${compiler_service}" "_wspawnvp(" visible_windows_spawn_offset)
if(NOT visible_windows_spawn_offset EQUAL -1)
    message(FATAL_ERROR
        "Windows export tools must not be launched through a visible console spawn")
endif()
foreach(required_serial_build_contract IN ITEMS
        "for (std::size_t index = begin; index < end; ++index)"
        "auto process_result ="
        "toolchain output for '")
    string(FIND "${compiler_service}" "${required_serial_build_contract}"
        serial_build_offset)
    if(serial_build_offset EQUAL -1)
        message(FATAL_ERROR
            "Serial native build diagnostics contract is missing: "
            "${required_serial_build_contract}")
    endif()
endforeach()
foreach(forbidden_parallel_build_contract IN ITEMS
        "hardware_concurrency()"
        "worker_count"
        "std::thread worker"
        "refresh_editor_display()")
    string(FIND "${compiler_service}" "${forbidden_parallel_build_contract}"
        parallel_build_offset)
    if(NOT parallel_build_offset EQUAL -1)
        message(FATAL_ERROR
            "Native export commands must remain strictly serial: "
            "${forbidden_parallel_build_contract}")
    endif()
endforeach()
foreach(required_build_progress_contract IN ITEMS
        "extends CanvasLayer"
        "layer = 127"
        "func begin(stages: PackedStringArray) -> void:"
        "func set_active_stage(stage: String) -> void:"
        "func update(stage: String, phase: String, completed: int, total: int) -> void:"
        "func refresh(snap_to_target := false) -> void:"
        "static func calculate_hierarchical_progress("
        "float(stage_index) + stage_progress"
        "float(phase_index) + item_progress"
        "class ImmediateProgressFill:"
        "func set_progress_immediate(fraction: float) -> void:"
        "RenderingServer.canvas_item_clear(canvas_item)"
        "RenderingServer.canvas_item_add_rect(canvas_item, rect, _color)"
        "_fill.set_progress_immediate(_displayed_progress)"
        "class ImmediateTaskLabel:"
        "func set_text_immediate(text: String) -> void:"
        "RenderingServer.canvas_item_clear(get_canvas_item())"
        "_font.draw_string("
        "static func format_task_text(phase: String, completed: int, total: int) -> String:"
        "text += \" (%d/%d)\""
        "_task_label.set_text_immediate(format_task_text("
        "get_last_exclusive_window()"
        "DisplayServer.window_get_active_popup()"
        "reparent(host_window, false)"
        "\"GDPP AOT Build\""
        "\"Precompiling project scripts\""
        "\"Compiling project sources\""
        "\"AOT build complete\""
        "RenderingServer.force_sync()"
        "RenderingServer.force_draw(true)")
    string(FIND "${build_progress}" "${required_build_progress_contract}"
        build_progress_offset)
    if(build_progress_offset EQUAL -1)
        message(FATAL_ERROR
            "Native build progress overlay is missing: ${required_build_progress_contract}")
    endif()
endforeach()
foreach(forbidden_build_progress_contract IN ITEMS
        "extends Window"
        "FILL_COLUMN_COUNT"
        "_fill_columns"
        "set_translation_profile"
        "STAGE_TEXT"
        "_stage_labels"
        "stage_label"
        "\"GDPP Native Build\""
        "\"Preparing AOT export\""
        "\"Validating the editor-native bridge\""
        "\"Building the debug export\""
        "\"Building the release export\""
        "_fill.scale = Vector2("
        "_fill.modulate ="
        "_fill.size = Vector2("
        "set_deferred(\"size\"")
    string(FIND "${build_progress}" "${forbidden_build_progress_contract}"
        forbidden_build_progress_offset)
    if(NOT forbidden_build_progress_offset EQUAL -1)
        message(FATAL_ERROR
            "Native build progress must use one top-level continuous bar: "
            "${forbidden_build_progress_contract}")
    endif()
endforeach()
foreach(required_background_build_contract IN ITEMS
        "extends RefCounted"
        "compiler.prepare_project_build()"
        "_thread = Thread.new()"
        "_thread.start("
        "while _thread.is_alive():"
        "_thread.wait_to_finish()"
        "_progress_mutex.lock()"
        "_dispatch_progress(progress_callback)"
        "_advance_frame(frame_callback, false)"
        "_advance_frame(frame_callback, true)"
        "DisplayServer.process_events()"
        "RenderingServer.force_draw()"
        "compiler.compile_project("
        "compiler.execute_project_build(")
    string(FIND "${native_build_job}" "${required_background_build_contract}"
        background_build_offset)
    if(background_build_offset EQUAL -1)
        message(FATAL_ERROR
            "Responsive background native-build contract is missing: "
            "${required_background_build_contract}")
    endif()
endforeach()
foreach(required_progress_integration IN ITEMS
        "_export_plugin.configure(_compiler, _build_progress)"
        "_build_progress.begin(PackedStringArray(["
        "_build_progress.set_active_stage("
        "_build_progress.finish()"
        "Callable(self, \"_on_native_build_progress\")"
        "NATIVE_BUILD_JOB.new()"
        "godot::D_METHOD(\"compile_project\", \"project_root\""
        "godot::D_METHOD(\"prepare_project_build\")"
        "options.progress_callback ="
        "ProjectCompilePhase::translate"
        "godot::D_METHOD(\"execute_project_build\", \"build_plan\", \"progress_callback\")"
        "report_build_progress(progress_callback, phase")
    string(FIND
        "${editor_plugin}\n${export_plugin}\n${native_build_job}\n${compiler_service}"
        "${required_progress_integration}"
        progress_integration_offset)
    if(progress_integration_offset EQUAL -1)
        message(FATAL_ERROR
            "Native build progress integration is missing: ${required_progress_integration}")
    endif()
endforeach()
foreach(required_scene_compatibility IN ITEMS
        "current.has_method(&\"get_base_scene_state\")"
        "current.call(&\"get_base_scene_state\") as SceneState")
    string(FIND "${export_plugin}" "${required_scene_compatibility}" compatibility_offset)
    if(compatibility_offset EQUAL -1)
        message(FATAL_ERROR
            "Godot 4.4 scene-state compatibility guard is missing: "
            "${required_scene_compatibility}")
    endif()
endforeach()
foreach(required_abstract_contract IN ITEMS
        "distribution_result.get(\"abstract_scripts\", PackedStringArray())"
        "not _abstract_scripts.has(script_path)"
        "and not ClassDB.can_instantiate(attached_base)")
    string(FIND "${export_plugin}" "${required_abstract_contract}" abstract_contract_offset)
    if(abstract_contract_offset EQUAL -1)
        message(FATAL_ERROR
            "Abstract native export validation is missing: ${required_abstract_contract}")
    endif()
endforeach()
foreach(required_editor_only_contract IN ITEMS
        "\"editor_only_scripts\", PackedStringArray()"
        "if _editor_only_scripts.has(script_path):"
        "autoload '%s' uses an editor-only script"
        "runtime scene '%s' uses editor-only script '%s'"
        "runtime resource graph '%s' uses editor-only script '%s'")
    string(FIND "${export_plugin}" "${required_editor_only_contract}" editor_only_contract_offset)
    if(editor_only_contract_offset EQUAL -1)
        message(FATAL_ERROR
            "Editor-only runtime export guard is missing: ${required_editor_only_contract}")
    endif()
endforeach()
foreach(required_autoload_rewrite IN ITEMS
        "func _resolve_resource_uid(path: String) -> String:"
        "ResourceUID.get_id_path(resource_id)"
        "ProjectSettings.set_setting(setting, prefix + str(_autoload_replacements[setting]))"
        "ProjectSettings.set_setting(setting, _autoload_originals[setting])")
    string(FIND "${export_plugin}" "${required_autoload_rewrite}" autoload_rewrite_offset)
    if(autoload_rewrite_offset EQUAL -1)
        message(FATAL_ERROR
            "Transactional binary autoload rewrite is missing: ${required_autoload_rewrite}")
    endif()
endforeach()
string(FIND "${export_plugin}" "add_file(script_path + \".remap\"" autoload_sidecar_offset)
if(NOT autoload_sidecar_offset EQUAL -1)
    message(FATAL_ERROR
        "Script-to-scene remap sidecars do not satisfy Godot's autoload type check")
endif()
foreach(required_attached_contract IN ITEMS
        "distribution_result.get(\"attached_script_bases\", {})"
        "ClassDB.class_exists(&\"AttachedCompiledScript\")"
        "func _install_attached_script("
        "object.set_script(script)"
        "[sub_resource type=\\\"AttachedCompiledScript\\\" id=\\\"AttachedScript\\\"]")
    string(FIND "${export_plugin}" "${required_attached_contract}" attached_contract_offset)
    if(attached_contract_offset EQUAL -1)
        message(FATAL_ERROR
            "Third-party GDExtension export attachment is missing: ${required_attached_contract}")
    endif()
endforeach()
string(FIND "${editor_plugin}"
    "if not DirAccess.dir_exists_absolute(ndk_parent):" safe_ndk_probe)
if(safe_ndk_probe EQUAL -1)
    message(FATAL_ERROR
        "Optional Android NDK discovery must not query a missing directory")
endif()
foreach(required_legacy_transaction_recovery IN ITEMS
        "gdpp_provider_descriptors.export-backup.json"
        "recovered = _restore_provider_descriptors() or recovered"
        "func _restore_provider_descriptors() -> bool:")
    string(FIND "${editor_plugin}" "${required_legacy_transaction_recovery}"
        legacy_transaction_recovery_offset)
    if(legacy_transaction_recovery_offset EQUAL -1)
        message(FATAL_ERROR
            "Interrupted pre-1.7.9 descriptor recovery is missing: "
            "${required_legacy_transaction_recovery}")
    endif()
endforeach()
foreach(required_complete_package_sdk_probe IN ITEMS
        "var single_host_manifest := version_root.path_join(\"sdk.manifest\")"
        "var complete_host_manifest := version_root.path_join("
        "_compiler.get_host_platform()"
        "_compiler.get_host_architecture()"
        "FileAccess.file_exists(complete_host_manifest)")
    string(FIND "${editor_plugin}" "${required_complete_package_sdk_probe}"
        complete_package_sdk_probe_offset)
    if(complete_package_sdk_probe_offset EQUAL -1)
        message(FATAL_ERROR
            "Complete package host SDK discovery is missing: "
            "${required_complete_package_sdk_probe}")
    endif()
endforeach()
file(READ "${GDPP_TEST_SOURCE_DIR}/example/export_presets.cfg" export_presets)
file(READ "${GDPP_TEST_SOURCE_DIR}/example/project.godot" example_project)
string(FIND "${example_project}"
    "gdscript/warnings/inference_on_variant=2"
    strict_variant_warning_offset)
if(strict_variant_warning_offset EQUAL -1)
    message(FATAL_ERROR
        "Godot compatibility project must treat Variant inference warnings as errors")
endif()
foreach(forbidden_export_minimum IN ITEMS
        "gradle_build/min_sdk="
        "application/min_ios_version="
        "min_macos_version")
    string(FIND "${export_presets}" "${forbidden_export_minimum}" minimum_offset)
    if(NOT minimum_offset EQUAL -1)
        message(FATAL_ERROR
            "Export fixtures must inherit the official template minimum instead of "
            "overriding it: ${forbidden_export_minimum}")
    endif()
endforeach()

foreach(invalid_android_option IN ITEMS
        "gradle_build/compress_native_libraries=true"
        "gradle_build/target_sdk=\"35\""
        "package/signed=true")
    string(FIND "${export_presets}" "${invalid_android_option}" invalid_offset)
    if(NOT invalid_offset EQUAL -1)
        message(FATAL_ERROR
            "Non-Gradle unsigned Android fixture contains invalid option: ${invalid_android_option}")
    endif()
endforeach()
foreach(required_windows_option IN ITEMS
        "name=\"Windows x86_64\""
        "name=\"Windows GDScript Fallback\""
        "platform=\"Windows Desktop\""
        "export_path=\"addons/gdpp/build/export/GDPPExample.exe\""
        "export_path=\"addons/gdpp/build/export/GDPPFallback.exe\"")
    string(FIND "${export_presets}" "${required_windows_option}" required_offset)
    if(required_offset EQUAL -1)
        message(FATAL_ERROR "Windows export fixture is missing: ${required_windows_option}")
    endif()
endforeach()

foreach(required_macos_option IN ITEMS
        "name=\"macOS Universal\""
        "name=\"macOS GDScript Fallback\""
        "export_path=\"addons/gdpp/build/export/GDPPFallback.app\"")
    string(FIND "${export_presets}" "${required_macos_option}" required_offset)
    if(required_offset EQUAL -1)
        message(FATAL_ERROR "macOS export fixture is missing: ${required_macos_option}")
    endif()
endforeach()

foreach(required_web_option IN ITEMS
        "name=\"Web AOT\""
        "name=\"Web AOT Threads\""
        "name=\"Web GDScript Fallback\""
        "platform=\"Web\""
        "variant/extensions_support=true"
        "variant/thread_support=true"
        "export_path=\"addons/gdpp/build/export/web/GDPPExample.html\"")
    string(FIND "${export_presets}" "${required_web_option}" required_offset)
    if(required_offset EQUAL -1)
        message(FATAL_ERROR "Web export fixture is missing: ${required_web_option}")
    endif()
endforeach()

foreach(required_web_implementation IN ITEMS
        "func _web_variant_from_features"
        "variant/extensions_support"
        "EMSCRIPTEN_CXX_SETTING"
        "\"threads\" if _target_platform == \"web\"")
    string(FIND "${export_plugin}" "${required_web_implementation}" required_offset)
    if(required_offset EQUAL -1)
        message(FATAL_ERROR
            "Web export implementation is missing: ${required_web_implementation}")
    endif()
endforeach()

foreach(required_project_entry_implementation IN ITEMS
        "const PROJECT_LIBRARY_ENTRY_SYMBOL := \"gdpp_library_init\""
        "'entry_symbol = \"%s\"\\n' % PROJECT_LIBRARY_ENTRY_SYMBOL"
        "_register_apple_embedded_entry(PROJECT_LIBRARY_ENTRY_SYMBOL)")
    string(FIND "${export_plugin}" "${required_project_entry_implementation}" required_offset)
    if(required_offset EQUAL -1)
        message(FATAL_ERROR
            "Project entry ABI implementation is missing: ${required_project_entry_implementation}")
    endif()
endforeach()
string(FIND "${export_plugin}" "gdpp_project_library_init" legacy_project_entry_offset)
if(NOT legacy_project_entry_offset EQUAL -1)
    message(FATAL_ERROR "Export plugin still references the retired project entry symbol")
endif()

foreach(required_ios_option IN ITEMS
        "name=\"iOS AOT\""
        "name=\"iOS GDScript Fallback\""
        "platform=\"iOS\""
        "application/export_project_only=true"
        "architectures/arm64=true"
        "export_path=\"addons/gdpp/build/export/ios/GDPPExample\"")
    string(FIND "${export_presets}" "${required_ios_option}" required_offset)
    if(required_offset EQUAL -1)
        message(FATAL_ERROR "iOS export fixture is missing: ${required_ios_option}")
    endif()
endforeach()
foreach(required_ios_implementation IN ITEMS
        "func _ios_compiler() -> String:"
        "func _native_artifact_exists(path: String) -> bool:"
        "\"macos\", \"windows\", \"linux\", \"android\", \"ios\", \"web\""
        "\"ios\": \"xcframework\"")
    string(FIND "${export_plugin}" "${required_ios_implementation}" required_offset)
    if(required_offset EQUAL -1)
        message(FATAL_ERROR
            "iOS export implementation is missing: ${required_ios_implementation}")
    endif()
endforeach()
