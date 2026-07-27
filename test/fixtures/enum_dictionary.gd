extends RefCounted

enum TokenType {
    IDENTIFIER = 2,
    NUMBER = 7,
}

func metadata() -> Dictionary:
    return TokenType

func names() -> Array:
    return TokenType.keys()

func name_for(value: int):
    return TokenType.find_key(value)

func has_name(value: String) -> bool:
    return TokenType.has(value)
