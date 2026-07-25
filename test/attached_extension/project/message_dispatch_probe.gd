extends Node

signal item_changed(index: int)

@onready var items: Array[Node] = [$Item0, $Item1]
@onready var gift_count: Label = $GiftCount
@onready var kuaishou_gift: Sprite2D = $KuaishouGift
@onready var douyin_gift: Sprite2D = $DouyinGift

var handlers: Dictionary = {}
var processed_messages := 0


class GiftRecord extends RefCounted:
    var item_index := -1
    var count := 0
    var source := 0


    func parse(payload: PackedByteArray) -> bool:
        if not _accept_typed_self(self) or payload.size() < 3:
            return false
        item_index = int(payload[0])
        count = int(payload[1])
        source = int(payload[2])
        return true


    func _accept_typed_self(value: GiftRecord) -> bool:
        return value == self


class LocalMessageServer extends RefCounted:


    func gift_packet(index: int, count: int, source: int) -> PackedByteArray:
        return PackedByteArray([0, 1, 0, 2, index, count, source])


func _ready() -> void:
    handlers[1] = {2: _apply_message}
    item_changed.connect(_on_item_changed)


func dispatch_burst(count: int) -> bool:
    var server := LocalMessageServer.new()
    for index in count:
        var packet := server.gift_packet(
            index % items.size(),
            (index % 250) + 1,
            (index % 2) + 1,
        )
        if not _process_packet(packet):
            return false
    var expected_source := ((count - 1) % 2) + 1
    return (
        processed_messages == count
        and gift_count.text == "X %d" % (((count - 1) % 250) + 1)
        and kuaishou_gift.visible == (expected_source == 1)
        and douyin_gift.visible == (expected_source == 2)
        and not _process_packet(PackedByteArray([0, 1, 0]))
        and not _process_packet(PackedByteArray([0, 9, 0, 9, 0, 1, 1]))
    )


func _process_packet(packet: PackedByteArray) -> bool:
    if packet.size() < 5:
        return false
    var main_id := (int(packet[0]) << 8) | int(packet[1])
    var sub_id := (int(packet[2]) << 8) | int(packet[3])
    var group: Dictionary = handlers.get(main_id, {})
    var handler: Callable = group.get(sub_id, Callable())
    if not handler.is_valid():
        return false
    handler.call(packet.slice(4))
    return true


func _apply_message(payload: PackedByteArray) -> void:
    var message := GiftRecord.new()
    if not message.parse(payload):
        return
    var index := message.item_index
    items[index].set_meta(&"message_count", int(items[index].get_meta(&"message_count", 0)) + 1)
    gift_count.text = "X %d" % message.count
    kuaishou_gift.visible = message.source == 1
    douyin_gift.visible = message.source == 2
    item_changed.emit(index)


func _on_item_changed(_index: int) -> void:
    processed_messages += 1
