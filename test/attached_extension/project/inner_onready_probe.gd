extends Node


class InnerProbe extends Node:
    @onready var marker: Node = $Marker


    func initialized() -> bool:
        return marker == get_node("Marker")


func verify_inner_onready() -> bool:
    var probe := InnerProbe.new()
    probe.name = "RuntimeInnerProbe"
    var marker := Node.new()
    marker.name = "Marker"
    probe.add_child(marker)
    add_child(probe)
    var initialized := probe.initialized()
    probe.queue_free()
    return initialized
