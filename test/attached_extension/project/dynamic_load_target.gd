extends RefCounted

var payload := 40


func evaluate(delta: int) -> int:
    return payload + delta
