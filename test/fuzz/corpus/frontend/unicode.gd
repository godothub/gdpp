extends Node

var 玩家名称: String = "你好 🌍"
var quoted := &"ключ"
var path := ^"世界/节点"

func 计算(值: int) -> int:
    var 合计 := 值
    合计 += 0x7fff_ffff
    return 合计
