import json
import traceback
import unreal
import os


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR

RESULT_PATH = os.path.join(OUTPUT_DIR, "roundtrip_array_pure_expr_cropout_result.json")
EXPORT_PATH = os.path.join(OUTPUT_DIR, "roundtrip_array_pure_expr_cropout_export.bplisp")

BP_PATH = "/Game/Blueprint/BPLispTests/BP_BPLispEventImportTest"
GRAPH_NAME = "EventGraph"
DSL_TEXT = r'''(event BeginPlay
  (PrintString
    :instring
      (get-array-item
        (make-array "Array_A" "Array_B")
        1)
    :bprinttoscreen true
    :bprinttolog true
    :textcolor "(R=1,G=1,B=0,A=1)"
    :duration 2
    :key "None"
    :id "arrpure001"))'''

report = {
    "success": False,
    "bp_path": BP_PATH,
    "graph_name": GRAPH_NAME,
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



def save_text(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)



def main():
    replace_graph = enum_value(["REPLACE_GRAPH", "ReplaceGraph"])

    import_result = unreal.BlueprintLispPythonBridge.import_graph_from_text(
        BP_PATH,
        GRAPH_NAME,
        DSL_TEXT,
        replace_graph,
        True,
        True,
    )
    report["import_success"] = get_result_bool(import_result, "success", "b_success")
    report["import_message"] = get_result_text(import_result, "message")
    if not report["import_success"]:
        raise RuntimeError(f"数组纯表达式导入失败: {report['import_message']}")
    log(f"数组纯表达式导入成功: {report['import_message']}")

    export_result = unreal.BlueprintLispPythonBridge.export_graph_to_text(BP_PATH, GRAPH_NAME, False, True)
    report["export_success"] = get_result_bool(export_result, "success", "b_success")
    report["export_message"] = get_result_text(export_result, "message")
    if not report["export_success"]:
        raise RuntimeError(f"数组纯表达式导出失败: {report['export_message']}")

    export_text = get_result_text(export_result, "dsl_text")
    save_text(EXPORT_PATH, export_text)
    log(f"已导出验证 DSL: {EXPORT_PATH}")

    report["contains_make_array"] = "make-array" in export_text
    report["contains_get_array_item"] = "get-array-item" in export_text
    report["contains_array_a"] = '"Array_A"' in export_text
    report["contains_array_b"] = '"Array_B"' in export_text
    report["contains_print_string"] = "PrintString" in export_text

    report["success"] = all([
        report["contains_make_array"],
        report["contains_get_array_item"],
        report["contains_array_a"],
        report["contains_array_b"],
        report["contains_print_string"],
    ])
    if not report["success"]:
        raise RuntimeError("回导出缺少 make-array / get-array-item 或关键字面量")

    log("已确认 make-array / get-array-item 在导入和回导出中闭环通过")


try:
    main()
except Exception as exc:
    fail(str(exc))
    fail(traceback.format_exc())
finally:
    save_report()
