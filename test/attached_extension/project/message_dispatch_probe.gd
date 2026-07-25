extends Node

signal item_changed(index: int)

@onready var items: Array[Node] = [$Item0, $Item1]

var handlers: Dictionary = {}
var processed_messages := 0


class MessageRecord extends RefCounted:
    var item_index := -1


    func parse(payload: PackedByteArray) -> bool:
        if not _accept_typed_self(self) or payload.is_empty():
            return false
        item_index = int(payload[0])
        return true


    func _accept_typed_self(value: MessageRecord) -> bool:
        return value == self


func _ready() -> void:
    handlers[1] = {2: _apply_message}
    item_changed.connect(_on_item_changed)


func dispatch_burst(count: int) -> bool:
    var packet := PackedByteArray([0, 0, 0, 1, 0, 0, 0, 2, 0])
    for index in count:
        packet[8] = index % items.size()
        if not _process_packet(packet):
            return false
    return processed_messages == count


func _process_packet(packet: PackedByteArray) -> bool:
    if packet.size() < 9:
        return false
    var main_id := (
        (int(packet[0]) << 24)
        | (int(packet[1]) << 16)
        | (int(packet[2]) << 8)
        | int(packet[3])
    )
    var sub_id := (
        (int(packet[4]) << 24)
        | (int(packet[5]) << 16)
        | (int(packet[6]) << 8)
        | int(packet[7])
    )
    var group: Dictionary = handlers.get(main_id, {})
    var handler: Callable = group.get(sub_id, Callable())
    if not handler.is_valid():
        return false
    handler.call(packet.slice(8))
    return true


func _apply_message(payload: PackedByteArray) -> void:
    var message := MessageRecord.new()
    if not message.parse(payload):
        return
    var index := message.item_index
    items[index].set_meta(&"message_count", int(items[index].get_meta(&"message_count", 0)) + 1)
    item_changed.emit(index)


func _on_item_changed(_index: int) -> void:
    processed_messages += 1
