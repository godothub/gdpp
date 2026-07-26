extends "res://rpc_runtime_probe_base.gd"

static var static_events: Array[int] = []

@rpc
func defaults() -> void:
    pass


func inherited_override(value: String) -> void:
    events.push_back(
        {
            "kind": "inherited_override",
            "value": value,
            "sender": multiplayer.get_remote_sender_id(),
        },
    )


@rpc("any_peer", "call_remote", "reliable", 7)
func reconfigured_remote(value: String) -> void:
    events.push_back(
        {
            "kind": "reconfigured",
            "value": value,
            "sender": multiplayer.get_remote_sender_id(),
        },
    )


@rpc("any_peer", "call_remote", "reliable", 0)
func remote_only(value: String, sequence: int) -> void:
    events.push_back(
        {
            "kind": "remote",
            "value": value,
            "sequence": sequence,
            "sender": multiplayer.get_remote_sender_id(),
        },
    )


@rpc("any_peer", "call_local", "reliable", 1)
func local_and_remote(value: String) -> void:
    events.push_back(
        {
            "kind": "local",
            "value": value,
            "sender": multiplayer.get_remote_sender_id(),
        },
    )


@rpc("authority", "call_remote", "reliable", 2)
func authority_only(value: String) -> void:
    events.push_back(
        {
            "kind": "authority",
            "value": value,
            "sender": multiplayer.get_remote_sender_id(),
        },
    )


@rpc("any_peer", "call_remote", "reliable", 3)
func reliable_sequence(sequence: int) -> void:
    events.push_back({"kind": "reliable", "sequence": sequence})


@rpc("any_peer", "call_remote", "unreliable_ordered", 4)
func ordered_sequence(sequence: int) -> void:
    events.push_back({"kind": "ordered", "sequence": sequence})


@rpc("any_peer", "call_remote", "reliable", 5)
func variant_payload(payload: Dictionary, bytes: PackedByteArray, path: NodePath) -> void:
    events.push_back(
        {
            "kind": "payload",
            "payload": payload,
            "bytes": bytes,
            "path": path,
            "sender": multiplayer.get_remote_sender_id(),
        },
    )


@rpc("any_peer", "call_remote", "reliable", 6)
static func static_remote(value: int) -> void:
    static_events.push_back(value)
