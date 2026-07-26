extends Node

func classify(value: Variant) -> String:
    match value:
        [var first, ..]:
            return "array:%s" % first
        {"kind": "gift", "count": var count, ..} when count > 0:
            return "gift"
        0, null:
            return "empty"
        _:
            return "other"

func delayed(signal_value: Signal) -> int:
    var value := await signal_value
    assert(value != null, "missing")
    while value:
        value = await signal_value
    return 1
