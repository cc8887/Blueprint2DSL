import json
import traceback
import unreal
import os


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR

RESULT_PATH = os.path.join(OUTPUT_DIR, "repair_villager_select_symbol_test_result.json")
SOURCE_PATH = os.path.join(OUTPUT_DIR, "villager_select_before_print.bplisp")
FIXED_INPUT_PATH = os.path.join(OUTPUT_DIR, "villager_select_fixed_symbols_input.bplisp")
EXPORT_PATH = os.path.join(OUTPUT_DIR, "villager_select_fixed_symbols_export.bplisp")
BP_PATH = "/Game/Blueprint/Core/Player/BP_Player"
GRAPH_NAME = "Villager Select"

report = {
    "success": False,
    "bp_path": BP_PATH,
    "graph_name": GRAPH_NAME,
    "steps": [],
    "errors": [],
}


def get_import_mode():
    for attr_name in ("BlueprintLispPythonImportMode", "BlueprintLispImportMode"):
        enum_type = getattr(unreal, attr_name, None)
        if enum_type is None:
            continue
        for value_name in ("REPLACE_GRAPH", "ReplaceGraph"):
            value = getattr(enum_type, value_name, None)
            if value is not None:
                return value, f"{attr_name}.{value_name}"
    raise RuntimeError("未找到 ReplaceGraph 导入枚举")


try:
    with open(SOURCE_PATH, "r", encoding="utf-8") as f:
        source_text = f.read()

    fixed_text = source_text.replace('"K2Node_FunctionEntry"', 'Selected').replace('"...circular..."', 'returnvalue')
    if fixed_text == source_text:
        raise RuntimeError("修复前后 DSL 文本完全相同，说明目标占位未命中")

    with open(FIXED_INPUT_PATH, "w", encoding="utf-8") as f:
        f.write(fixed_text)
    report["steps"].append(f"已生成修正 DSL: {FIXED_INPUT_PATH}")

    import_mode, import_mode_name = get_import_mode()
    report["steps"].append(f"解析导入枚举成功: {import_mode_name}")

    import_result = unreal.BlueprintLispPythonBridge.import_graph_from_text(BP_PATH, GRAPH_NAME, fixed_text, import_mode, True, True)

    import_success = bool(getattr(import_result, "success", getattr(import_result, "b_success", False)))
    report["import_success"] = import_success
    report["import_message"] = str(getattr(import_result, "message", ""))
    if not import_success:
        raise RuntimeError(report["import_message"] or "修正 DSL 导入失败")
    report["steps"].append(f"修正 DSL 导入成功: {report['import_message']}")

    export_result = unreal.BlueprintLispPythonBridge.export_graph_to_text(BP_PATH, GRAPH_NAME, False, True)
    export_success = bool(getattr(export_result, "success", getattr(export_result, "b_success", False)))
    if not export_success:
        raise RuntimeError(str(getattr(export_result, "message", "回导出失败")))

    export_text = str(getattr(export_result, "dsl_text", ""))
    with open(EXPORT_PATH, "w", encoding="utf-8") as f:
        f.write(export_text)
    report["steps"].append(f"已导出修正后 DSL: {EXPORT_PATH}")

    report["contains_selected_symbol"] = "(set\n    Selected\n    Selected" in export_text
    report["contains_ns_path_returnvalue"] = "(set\n    NS_Path\n    returnvalue" in export_text
    report["contains_bad_function_entry"] = "K2Node_FunctionEntry" in export_text
    report["contains_bad_circular"] = "...circular..." in export_text
    report["contains_spawn"] = "SpawnSystemAttached" in export_text
    report["contains_timer"] = "K2_SetTimer" in export_text

    report["success"] = all([
        report["contains_selected_symbol"],
        report["contains_ns_path_returnvalue"],
        not report["contains_bad_function_entry"],
        not report["contains_bad_circular"],
        report["contains_spawn"],
        report["contains_timer"],
    ])
    if not report["success"]:
        raise RuntimeError("修正 DSL 导入后回导出仍未得到期望符号形式")
except Exception as exc:
    report["errors"].append(str(exc))
    report["errors"].append(traceback.format_exc())
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
