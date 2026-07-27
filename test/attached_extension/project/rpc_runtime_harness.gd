extends RefCounted

const RPC_PROBE := preload("res://rpc_runtime_probe.gd")

var _tree: SceneTree
var _server_peer: ENetMultiplayerPeer
var _client_peer: ENetMultiplayerPeer
var _replacement_peer: ENetMultiplayerPeer
var _server_api: SceneMultiplayer
var _client_api: SceneMultiplayer
var _server_root: Node
var _client_root: Node
var _server_node: Variant
var _client_node: Variant


func _wait_until(predicate: Callable, frame_limit: int = 600) -> bool:
    for _frame in range(frame_limit):
        if predicate.call():
            return true
        await _tree.process_frame
    return false


func _events(probe: Variant, kind: String) -> Array[Dictionary]:
    var result: Array[Dictionary] = []
    for event: Dictionary in probe.events:
        if event.get("kind", "") == kind:
            result.push_back(event)
    return result


func _sequences(probe: Variant, kind: String) -> Array[int]:
    var result: Array[int] = []
    for event: Dictionary in _events(probe, kind):
        result.push_back(int(event.get("sequence", -1)))
    return result


func _is_complete_sequence(values: Array[int], count: int) -> bool:
    if values.size() != count:
        return false
    for index in count:
        if values[index] != index:
            return false
    return true


func _configuration_matches(probe: Variant) -> bool:
    var script: Script = probe.get_script()
    if script == null:
        return false
    var config: Dictionary = script.get_rpc_config()
    if config.size() != 11:
        return false
    var inherited: Dictionary = config.get(&"inherited_remote", {})
    var inherited_override: Dictionary = config.get(&"inherited_override", {})
    var reconfigured: Dictionary = config.get(&"reconfigured_remote", {})
    if (
        inherited.get("rpc_mode") != MultiplayerAPI.RPC_MODE_ANY_PEER
        or inherited.get("transfer_mode") != MultiplayerPeer.TRANSFER_MODE_RELIABLE
        or inherited.get("call_local") != false
        or inherited.get("channel") != 7
        or inherited_override != inherited
        or reconfigured.get("rpc_mode") != MultiplayerAPI.RPC_MODE_ANY_PEER
        or reconfigured.get("transfer_mode") != MultiplayerPeer.TRANSFER_MODE_RELIABLE
        or reconfigured.get("call_local") != false
        or reconfigured.get("channel") != 7
    ):
        return false
    var defaults: Dictionary = config.get(&"defaults", {})
    if defaults.size() != 1 or defaults.get("rpc_mode") != MultiplayerAPI.RPC_MODE_AUTHORITY:
        return false
    var remote: Dictionary = config.get(&"remote_only", {})
    if (
        remote.size() != 4
        or remote.get("rpc_mode") != MultiplayerAPI.RPC_MODE_ANY_PEER
        or remote.get("transfer_mode") != MultiplayerPeer.TRANSFER_MODE_RELIABLE
        or remote.get("call_local") != false
        or remote.get("channel") != 0
    ):
        return false
    var local: Dictionary = config.get(&"local_and_remote", {})
    if local.get("call_local") != true or local.get("channel") != 1:
        return false
    var authority: Dictionary = config.get(&"authority_only", {})
    if (
        authority.get("rpc_mode") != MultiplayerAPI.RPC_MODE_AUTHORITY
        or authority.get("channel") != 2
    ):
        return false
    var ordered: Dictionary = config.get(&"ordered_sequence", {})
    if (
        ordered.get("transfer_mode") != MultiplayerPeer.TRANSFER_MODE_UNRELIABLE_ORDERED
        or ordered.get("channel") != 4
    ):
        return false
    var static_remote: Dictionary = config.get(&"static_remote", {})
    return (
        static_remote.get("rpc_mode") == MultiplayerAPI.RPC_MODE_ANY_PEER
        and static_remote.get("transfer_mode") == MultiplayerPeer.TRANSFER_MODE_RELIABLE
        and static_remote.get("call_local") == false
        and static_remote.get("channel") == 6
    )


func _cleanup() -> void:
    # SceneMultiplayer caches RPC nodes and owns one-shot tree_exited connections for them. Drain
    # those caches through the node lifecycle before clearing a peer or its root configuration;
    # otherwise supported Godot versions can attempt to disconnect an already-consumed cache
    # callback. Keep the transaction ordered and idempotent for every success and abort path.
    if _client_node != null and is_instance_valid(_client_node):
        if _client_node.is_inside_tree():
            _client_node.get_parent().remove_child(_client_node)
        _client_node.free()
        _client_node = null
    if _server_node != null and is_instance_valid(_server_node):
        if _server_node.is_inside_tree():
            _server_node.get_parent().remove_child(_server_node)
        _server_node.free()
        _server_node = null
    if _replacement_peer != null:
        _replacement_peer.close()
    if _client_peer != null:
        _client_peer.close()
    if _server_peer != null:
        _server_peer.close()
    if _client_api != null:
        _client_api.multiplayer_peer = null
    if _server_api != null:
        _server_api.multiplayer_peer = null
    if _tree != null:
        if (
            _client_root != null
            and is_instance_valid(_client_root)
            and _client_root.is_inside_tree()
        ):
            _tree.set_multiplayer(null, _client_root.get_path())
        if (
            _server_root != null
            and is_instance_valid(_server_root)
            and _server_root.is_inside_tree()
        ):
            _tree.set_multiplayer(null, _server_root.get_path())
    if _server_root != null and is_instance_valid(_server_root):
        _server_root.free()
    if _client_root != null and is_instance_valid(_client_root):
        _client_root.free()
    _server_root = null
    _client_root = null
    _server_api = null
    _client_api = null
    _replacement_peer = null
    _client_peer = null
    _server_peer = null


func _abort(message: String) -> String:
    _cleanup()
    return message


func verify(tree: SceneTree) -> String:
    _tree = tree
    var port := -1
    var port_seed := int(Time.get_ticks_usec() % 20000)
    for attempt in range(32):
        var candidate := ENetMultiplayerPeer.new()
        var candidate_port := 30000 + ((port_seed + attempt * 7919) % 20000)
        if candidate.create_server(candidate_port, 1, 8) == OK:
            _server_peer = candidate
            port = candidate_port
            break
    if _server_peer == null:
        return _abort("multi-peer RPC fixture could not reserve a loopback ENet port")

    _client_peer = ENetMultiplayerPeer.new()
    if _client_peer.create_client("127.0.0.1", port, 8) != OK:
        return _abort("multi-peer RPC client could not connect to its loopback server")

    _server_api = SceneMultiplayer.new()
    _server_api.multiplayer_peer = _server_peer
    _client_api = SceneMultiplayer.new()
    _client_api.multiplayer_peer = _client_peer
    _server_root = Node.new()
    _server_root.name = "GdppRpcServer"
    _tree.root.add_child(_server_root)
    _client_root = Node.new()
    _client_root.name = "GdppRpcClient"
    _tree.root.add_child(_client_root)
    _tree.set_multiplayer(_server_api, _server_root.get_path())
    _tree.set_multiplayer(_client_api, _client_root.get_path())

    RPC_PROBE.static_events.clear()
    _server_node = RPC_PROBE.new()
    _server_node.name = "Actor"
    _server_root.add_child(_server_node)
    _client_node = RPC_PROBE.new()
    _client_node.name = "Actor"
    _client_root.add_child(_client_node)

    if not _configuration_matches(_server_node):
        return _abort(
            "AOT Script RPC reflection differs from official GDScript: %s"
            % [_server_node.get_script().get_rpc_config()],
        )
    if not await _wait_until(
        func() -> bool:
            return (
                _client_peer.get_connection_status()
                == MultiplayerPeer.CONNECTION_CONNECTED
            )
    ):
        return _abort("multi-peer RPC connection timed out")
    var client_id := _client_peer.get_unique_id()

    if _client_node.rpc_id(1, &"remote_only", "initial", 0) != OK:
        return _abort("any-peer reliable RPC send failed")
    if not await _wait_until(
        func() -> bool: return _events(_server_node, "remote").size() == 1
    ):
        return _abort("any-peer reliable RPC was not delivered")
    var remote_event: Dictionary = _events(_server_node, "remote")[0]
    if remote_event.get("value") != "initial" or remote_event.get("sender") != client_id:
        return _abort("RPC payload or remote sender identity changed")

    if _client_node.rpc_id(1, &"inherited_remote", "base") != OK:
        return _abort("inherited RPC send failed")
    if _client_node.rpc_id(1, &"inherited_override", "override") != OK:
        return _abort("RPC send through an unannotated override failed")
    if _client_node.rpc_id(1, &"reconfigured_remote", "reconfigured") != OK:
        return _abort("reconfigured RPC send failed")
    if not await _wait_until(
        func() -> bool:
            return (
                _events(_server_node, "inherited").size() == 1
                and _events(_server_node, "inherited_override").size() == 1
                and _events(_server_node, "reconfigured").size() == 1
            )
    ):
        return _abort("inherited or reconfigured RPC was not delivered")
    for kind in ["inherited", "inherited_override", "reconfigured"]:
        var inherited_event: Dictionary = _events(_server_node, kind)[0]
        if inherited_event.get("sender") != client_id:
            return _abort("inherited RPC lost the remote sender identity")

    if _client_node.rpc(&"local_and_remote", "both") != OK:
        return _abort("call-local RPC send failed")
    if not await _wait_until(
        func() -> bool:
            return (
                _events(_server_node, "local").size() == 1
                and _events(_client_node, "local").size() == 1
            )
    ):
        return _abort("call-local RPC did not execute exactly once on both peers")
    var server_local: Dictionary = _events(_server_node, "local")[0]
    var client_local: Dictionary = _events(_client_node, "local")[0]
    if (
        server_local.get("value") != "both"
        or client_local.get("value") != "both"
        or server_local.get("sender") != client_id
        or client_local.get("sender") != client_id
    ):
        return _abort("call-local RPC changed its payload or sender identity")

    if _server_node.rpc_id(client_id, &"authority_only", "server") != OK:
        return _abort("authority RPC send failed")
    if not await _wait_until(
        func() -> bool: return _events(_client_node, "authority").size() == 1
    ):
        return _abort("authority RPC was not delivered to the client")
    var authority_event: Dictionary = _events(_client_node, "authority")[0]
    if authority_event.get("sender") != 1 or authority_event.get("value") != "server":
        return _abort("authority RPC sender or payload changed")

    for sequence in range(32):
        if _client_node.rpc_id(1, &"reliable_sequence", sequence) != OK:
            return _abort("reliable channel rejected an RPC packet")
    if not await _wait_until(
        func() -> bool: return _sequences(_server_node, "reliable").size() == 32
    ):
        return _abort("reliable channel dropped an RPC packet")
    if not _is_complete_sequence(_sequences(_server_node, "reliable"), 32):
        return _abort("reliable channel reordered RPC packets")

    for sequence in range(48):
        if _client_node.rpc_id(1, &"ordered_sequence", sequence) != OK:
            return _abort("unreliable-ordered channel rejected an RPC packet")
    if not await _wait_until(
        func() -> bool: return not _sequences(_server_node, "ordered").is_empty()
    ):
        return _abort("unreliable-ordered channel delivered no RPC packets")
    for _frame in range(20):
        await _tree.process_frame
    var previous_sequence := -1
    for sequence in _sequences(_server_node, "ordered"):
        if sequence <= previous_sequence:
            return _abort("unreliable-ordered channel reordered RPC packets")
        previous_sequence = sequence

    var variant_value := {
        "gift": {"name": "rocket", "count": 3},
        "scores": [7, 11, 13],
        "active": true,
    }
    var byte_value := PackedByteArray([0, 1, 127, 128, 255])
    var path_value := NodePath("Arena/Player:health")
    if (
        _client_node.rpc_id(
            1,
            &"variant_payload",
            variant_value,
            byte_value,
            path_value,
        )
        != OK
    ):
        return _abort("complex Variant RPC send failed")
    if not await _wait_until(
        func() -> bool: return _events(_server_node, "payload").size() == 1
    ):
        return _abort("complex Variant RPC payload was not delivered")
    var payload_event: Dictionary = _events(_server_node, "payload")[0]
    if (
        payload_event.get("payload") != variant_value
        or payload_event.get("bytes") != byte_value
        or payload_event.get("path") != path_value
        or payload_event.get("sender") != client_id
    ):
        return _abort("complex Variant RPC payload changed during serialization")

    if _client_node.rpc_id(1, &"static_remote", 99) != OK:
        return _abort("static RPC send failed")
    if not await _wait_until(func() -> bool: return RPC_PROBE.static_events == [99]):
        return _abort("static RPC method was not dispatched")

    var disconnected_ids: Array[int] = []
    _server_api.peer_disconnected.connect(
        func(peer_id: int) -> void:
            disconnected_ids.push_back(peer_id),
    )
    # Let the old SceneMultiplayer cache consume its tree-exit callback before the peer reset,
    # then reattach the same script instance to prove reconnect behavior without stale node IDs.
    _client_root.remove_child(_client_node)
    _client_peer.close()
    if not await _wait_until(func() -> bool: return disconnected_ids.has(client_id)):
        return _abort("RPC server did not observe the client disconnect")

    _replacement_peer = ENetMultiplayerPeer.new()
    if _replacement_peer.create_client("127.0.0.1", port, 8) != OK:
        return _abort("RPC client could not reconnect")
    _client_api.multiplayer_peer = _replacement_peer
    _client_root.add_child(_client_node)
    if not await _wait_until(
        func() -> bool:
            return (
                _replacement_peer.get_connection_status()
                == MultiplayerPeer.CONNECTION_CONNECTED
            )
    ):
        return _abort("RPC client reconnection timed out")
    var replacement_id := _replacement_peer.get_unique_id()
    if _client_node.rpc_id(1, &"remote_only", "reconnected", 1) != OK:
        return _abort("RPC send after reconnection failed")
    if not await _wait_until(
        func() -> bool: return _events(_server_node, "remote").size() == 2
    ):
        return _abort("RPC was not delivered after reconnection")
    var reconnect_event: Dictionary = _events(_server_node, "remote")[1]
    if (
        reconnect_event.get("value") != "reconnected"
        or reconnect_event.get("sequence") != 1
        or reconnect_event.get("sender") != replacement_id
    ):
        return _abort("RPC reconnection retained stale peer or payload state")

    _cleanup()
    return ""
