import json
import traceback
import unreal
import os


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR

RESULT_PATH = os.path.join(OUTPUT_DIR, "roundtrip_villager_select_exec_chain_cropout_result.json")
ORIGINAL_EXPORT_PATH = os.path.join(OUTPUT_DIR, "villager_select_exec_chain_original_export.bplisp")
MODIFIED_INPUT_PATH = os.path.join(OUTPUT_DIR, "villager_select_exec_chain_input.bplisp")
MODIFIED_EXPORT_PATH = os.path.join(OUTPUT_DIR, "villager_select_exec_chain_export.bplisp")
RESTORED_EXPORT_PATH = os.path.join(OUTPUT_DIR, "villager_select_exec_chain_restored_export.bplisp")

BP_PATH = "/Game/Blueprint/Core/Player/BP_Player"
GRAPH_NAME = "Villager Select"
PRINT_TEXT = "DSL_Debug_VillagerSelect"

report = {
    "success": False,
    "bp_path": BP_PATH,
    "graph_name": GRAPH_NAME,
    "print_text": PRINT_TEXT,
    "steps": [],
    "errors": [],
}


def log(msg):
    unreal.log(msg)
    report["steps"].append(msg)



def fail(msg):
    unreal.log_error(msg)
    report["errors"].append(msg)



def save_report():
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)



def save_text(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)



def get_result_bool(result, *names):
    for name in names:
        if hasattr(result, name):
            return bool(getattr(result, name))
    return False



def get_result_text(result, *names):
    for name in names:
        if hasattr(result, name):
            value = getattr(result, name)
            if value is not None:
                return str(value)
    return ""



def enum_value(candidates):
    for attr_name in dir(unreal):
        if "BlueprintLisp" not in attr_name or "ImportMode" not in attr_name:
            continue
        enum_cls = getattr(unreal, attr_name, None)
        if enum_cls is None:
            continue
        for candidate in candidates:
            if hasattr(enum_cls, candidate):
                value = getattr(enum_cls, candidate)
                log(f"解析导入枚举成功: {attr_name}.{candidate}")
                return value
    raise RuntimeError("未找到 BlueprintLisp Python ImportMode 枚举")



def export_graph_text(tag):
    result = unreal.BlueprintLispPythonBridge.export_graph_to_text(BP_PATH, GRAPH_NAME, False, True)
    success = get_result_bool(result, "success", "b_success")
    message = get_result_text(result, "message")
    report[f"export_{tag}_success"] = success
    report[f"export_{tag}_message"] = message
    if not success:
        raise RuntimeError(f"{tag} 导出失败: {message}")
    return get_result_text(result, "dsl_text")



def import_graph_text(dsl_text, replace_graph, tag):
    result = unreal.BlueprintLispPythonBridge.import_graph_from_text(
        BP_PATH,
        GRAPH_NAME,
        dsl_text,
        replace_graph,
        True,
        True,
    )
    success = get_result_bool(result, "success", "b_success")
    message = get_result_text(result, "message")
    report[f"import_{tag}_success"] = success
    report[f"import_{tag}_message"] = message
    if not success:
        raise RuntimeError(f"{tag} 导入失败: {message}")
    log(f"{tag} 导入成功: {message}")



def insert_print_into_function_dsl(dsl_text):
    marker = f'  (PrintString "{PRINT_TEXT}")'
    if marker in dsl_text:
        return dsl_text, False

    lines = dsl_text.splitlines()
    insert_index = None
    for i in range(1, len(lines)):
        if lines[i].startswith("  ("):
            insert_index = i
            break

    if insert_index is None:
        if lines and lines[-1].strip() == ")":
            insert_index = len(lines) - 1
        else:
            insert_index = len(lines)

    lines.insert(insert_index, marker)
    return "\n".join(lines), True



def validate_original_export(export_text):
    report["original_contains_spawn"] = "SpawnSystemAttached" in export_text
    report["original_contains_timer"] = "K2_SetTimer" in export_text
    report["original_contains_original_print"] = "Villager Select called!" in export_text
    report["original_contains_selected_symbol"] = "(set\n    Selected\n    Selected" in export_text
    report["original_contains_ns_path_returnvalue"] = "(set\n    NS_Path\n    returnvalue" in export_text
    report["original_contains_debug_print"] = PRINT_TEXT in export_text
    report["original_contains_bad_function_entry"] = "K2Node_FunctionEntry" in export_text
    report["original_contains_bad_circular"] = "...circular..." in export_text

    if not report["original_contains_spawn"]:
        raise RuntimeError("原始导出缺少 SpawnSystemAttached，当前基线不符合预期")
    if not report["original_contains_timer"]:
        raise RuntimeError("原始导出缺少 K2_SetTimer，当前基线不符合预期")
    if not report["original_contains_original_print"]:
        raise RuntimeError("原始导出缺少 Villager Select called!，当前基线不符合预期")
    if not report["original_contains_selected_symbol"]:
        raise RuntimeError("原始导出缺少 Selected -> Selected 符号绑定")
    if not report["original_contains_ns_path_returnvalue"]:
        raise RuntimeError("原始导出缺少 NS_Path -> returnvalue 符号绑定")
    if report["original_contains_debug_print"]:
        raise RuntimeError("原始导出已包含 DSL_Debug_VillagerSelect，测试基线被污染")
    if report["original_contains_bad_function_entry"]:
        raise RuntimeError("原始导出仍包含 K2Node_FunctionEntry 占位")
    if report["original_contains_bad_circular"]:
        raise RuntimeError("原始导出仍包含 ...circular... 占位")



def validate_modified_export(export_text):
    report["modified_contains_debug_print"] = PRINT_TEXT in export_text
    report["modified_contains_spawn"] = "SpawnSystemAttached" in export_text
    report["modified_contains_timer"] = "K2_SetTimer" in export_text
    report["modified_contains_selected_symbol"] = "(set\n    Selected\n    Selected" in export_text
    report["modified_contains_ns_path_returnvalue"] = "(set\n    NS_Path\n    returnvalue" in export_text
    report["modified_contains_bad_function_entry"] = "K2Node_FunctionEntry" in export_text
    report["modified_contains_bad_circular"] = "...circular..." in export_text

    if not report["modified_contains_debug_print"]:
        raise RuntimeError("重新导出后未发现 DSL_Debug_VillagerSelect，DSL 注入未保留")
    if not report["modified_contains_spawn"]:
        raise RuntimeError("重新导出后丢失 SpawnSystemAttached，说明函数 exec 链被破坏")
    if not report["modified_contains_timer"]:
        raise RuntimeError("重新导出后丢失 K2_SetTimer，说明函数 exec 链被破坏")
    if not report["modified_contains_selected_symbol"]:
        raise RuntimeError("重新导出后丢失 Selected -> Selected 符号绑定")
    if not report["modified_contains_ns_path_returnvalue"]:
        raise RuntimeError("重新导出后丢失 NS_Path -> returnvalue 符号绑定")
    if report["modified_contains_bad_function_entry"]:
        raise RuntimeError("重新导出后重新出现 K2Node_FunctionEntry 占位")
    if report["modified_contains_bad_circular"]:
        raise RuntimeError("重新导出后重新出现 ...circular... 占位")



def validate_restored_export(export_text):
    report["restored_contains_debug_print"] = PRINT_TEXT in export_text
    report["restored_contains_spawn"] = "SpawnSystemAttached" in export_text
    report["restored_contains_timer"] = "K2_SetTimer" in export_text
    report["restored_contains_original_print"] = "Villager Select called!" in export_text
    report["restored_contains_selected_symbol"] = "(set\n    Selected\n    Selected" in export_text
    report["restored_contains_ns_path_returnvalue"] = "(set\n    NS_Path\n    returnvalue" in export_text
    report["restored_contains_bad_function_entry"] = "K2Node_FunctionEntry" in export_text
    report["restored_contains_bad_circular"] = "...circular..." in export_text

    if report["restored_contains_debug_print"]:
        raise RuntimeError("恢复后仍残留 DSL_Debug_VillagerSelect")
    if not report["restored_contains_spawn"]:
        raise RuntimeError("恢复后缺少 SpawnSystemAttached")
    if not report["restored_contains_timer"]:
        raise RuntimeError("恢复后缺少 K2_SetTimer")
    if not report["restored_contains_original_print"]:
        raise RuntimeError("恢复后缺少 Villager Select called!")
    if not report["restored_contains_selected_symbol"]:
        raise RuntimeError("恢复后缺少 Selected -> Selected 符号绑定")
    if not report["restored_contains_ns_path_returnvalue"]:
        raise RuntimeError("恢复后缺少 NS_Path -> returnvalue 符号绑定")
    if report["restored_contains_bad_function_entry"]:
        raise RuntimeError("恢复后仍包含 K2Node_FunctionEntry 占位")
    if report["restored_contains_bad_circular"]:
        raise RuntimeError("恢复后仍包含 ...circular... 占位")



def main():
    replace_graph = enum_value(["REPLACE_GRAPH", "ReplaceGraph"])
    original_text = ""
    modified_applied = False

    try:
        original_text = export_graph_text("original")
        save_text(ORIGINAL_EXPORT_PATH, original_text)
        log(f"已保存原始 Villager Select DSL: {ORIGINAL_EXPORT_PATH}")
        validate_original_export(original_text)
        log("已确认原始 Villager Select 基线正常")

        modified_text, inserted = insert_print_into_function_dsl(original_text)
        report["dsl_inserted"] = inserted
        save_text(MODIFIED_INPUT_PATH, modified_text)
        log(f"已生成 ExecChain 测试 DSL: {MODIFIED_INPUT_PATH}")

        validate_result = unreal.BlueprintLispPythonBridge.validate_dsl(modified_text)
        report["validate_full_success"] = get_result_bool(validate_result, "success", "b_success")
        report["validate_full_message"] = get_result_text(validate_result, "message")
        if not report["validate_full_success"]:
            raise RuntimeError(f"Villager Select 完整 DSL 校验失败: {report['validate_full_message']}")
        log("Villager Select ExecChain 测试 DSL 校验通过")

        import_graph_text(modified_text, replace_graph, "modified")
        modified_applied = True

        modified_export = export_graph_text("modified")
        save_text(MODIFIED_EXPORT_PATH, modified_export)
        log(f"已保存 ExecChain 回导出 DSL: {MODIFIED_EXPORT_PATH}")
        validate_modified_export(modified_export)
        log("已确认注入调试 PrintString 后关键调用与符号仍保留")
    finally:
        if original_text and modified_applied:
            import_graph_text(original_text, replace_graph, "restore")
            restored_export = export_graph_text("restored")
            save_text(RESTORED_EXPORT_PATH, restored_export)
            log(f"已保存恢复后 DSL: {RESTORED_EXPORT_PATH}")
            validate_restored_export(restored_export)
            log("已确认 Villager Select 已恢复到测试前状态")

    report["success"] = True


try:
    main()
except Exception as exc:
    fail(str(exc))
    fail(traceback.format_exc())
finally:
    save_report()
