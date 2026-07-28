extends Node
class_name ScriptResourceService

const Consumer = preload("consumer.gd")

signal executed

var consumer_script := preload("consumer.gd")

func execute() -> void:
    executed.emit()

func consumer_resource() -> Script:
    return consumer_script
