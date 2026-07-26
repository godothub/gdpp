#include "compiler_service.hpp"
#include "gdpp/runtime/attached_script.hpp"
#include "gdpp/runtime/variant_ops.hpp"

#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>

namespace {

void initialize_gdpp(godot::ModuleInitializationLevel level) {
    if (level != godot::MODULE_INITIALIZATION_LEVEL_SCENE)
        return;
    GDREGISTER_CLASS(gdpp::runtime::CoroutineFunctionState);
    GDREGISTER_CLASS(gdpp::runtime::AttachedScriptBehavior);
    GDREGISTER_CLASS(gdpp::runtime::AttachedCompiledLanguage);
    GDREGISTER_CLASS(gdpp::runtime::AttachedCompiledScript);
    GDREGISTER_CLASS(gdpp::runtime::AttachedScriptResourceLoader);
    GDREGISTER_CLASS(gdpp::extension::GDPPCompiler);
    godot::String error;
    ERR_FAIL_COND_MSG(!gdpp::runtime::AttachedCompiledLanguage::register_singleton(&error), error);
}

void uninitialize_gdpp(godot::ModuleInitializationLevel level) {
    if (level != godot::MODULE_INITIALIZATION_LEVEL_SCENE)
        return;
    gdpp::runtime::AttachedCompiledLanguage::unregister_singleton();
    gdpp::runtime::unregister_all_attached_scripts();
}

} // namespace

extern "C" GDExtensionBool GDE_EXPORT
gdpp_library_init(GDExtensionInterfaceGetProcAddress get_proc_address,
                  GDExtensionClassLibraryPtr library, GDExtensionInitialization* initialization) {
    godot::GDExtensionBinding::InitObject init{get_proc_address, library, initialization};
    init.register_initializer(initialize_gdpp);
    init.register_terminator(uninitialize_gdpp);
    init.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
