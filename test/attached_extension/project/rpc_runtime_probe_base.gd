extends Node

var events: Array[Dictionary] = []


@rpc("any_peer", "call_remote", "reliable", 7)
func inherited_remote(value: String) -> void:
    events.push_back(
        {
            "kind": "inherited",
            "value": value,
            "sender": multiplayer.get_remote_sender_id(),
        },
    )


@rpc("any_peer", "call_remote", "reliable", 7)
func inherited_override(value: String) -> void:
    events.push_back({"kind": "base_override", "value": value})


@rpc("authority", "call_remote", "unreliable", 0)
func reconfigured_remote(value: String) -> void:
    events.push_back({"kind": "base_reconfigured", "value": value})
