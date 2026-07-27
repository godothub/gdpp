extends Node
class_name ScriptResourceConsumer

const Service = preload("service.gd")

func validate() -> bool:
    var script := load("service.gd")
    if script == null:
        return false
    return script != null and script.can_instantiate() and script.has_script_signal(&"executed")

func global_name() -> StringName:
    return Service.get_global_name()

func rename_resource() -> String:
    var script := load("service.gd")
    script.resource_name = "compiled"
    return script.resource_name

func changed_signal() -> Signal:
    return Service.changed

func accepts_script(script: Script) -> bool:
    return script != null

func passes_as_script() -> bool:
    return accepts_script(Service)

func has_script_type() -> bool:
    return Service is Script

func clear_resource() -> bool:
    var script := load("service.gd")
    script = null
    return not script

func conditional_resource(enabled: bool) -> bool:
    var script := load("service.gd") if enabled else null
    return script != null

func create() -> ScriptResourceService:
    return Service.new()
