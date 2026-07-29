extends Node

@export var payload := 0:
    set(value):
        payload = value
        if not is_inside_tree():
            await ready
        var resumed_behaviors: Array = get_meta(&"resumed_behaviors", [])
        resumed_behaviors.append(behavior_identity())
        set_meta(&"resumed_behaviors", resumed_behaviors)


func behavior_identity() -> String:
    return "intermediate"
