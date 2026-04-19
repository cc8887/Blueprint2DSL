import json
import traceback
import unreal
import os


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR

RESULT_PATH = os.path.join(OUTPUT_DIR, "roundtrip_endplay_loop_item_bp_shrub_result.json")
ORIGINAL_EXPORT_PATH = os.path.join(OUTPUT_DIR, "bp_shrub_endplay_loop_item_original_export.bplisp")
MODIFIED_INPUT_PATH = os.path.join(OUTPUT_DIR, "bp_shrub_endplay_loop_item_input.bplisp")
MODIFIED_EXPORT_PATH = os.path.join(OUTPUT_DIR, "bp_shrub_endplay_loop_item_export.bplisp")
RESTORED_EXPORT_PATH = os.path.join(OUTPUT_DIR, "bp_shrub_endplay_loop_item_restored_export.bplisp")

BP_PATH = "/Game/Blueprint/Interactable/Resources/BP_Shrub"
GRAPH_NAME = "EventGraph"
LOOP_VALUES = [f"ShrubLoop{i}" for i in range(4)]

report = {
    "success": False,
    "bp_path": BP_PATH,
    "graph_name": GRAPH_NAME,
    "steps": [],
    "errors": [],
    "loop_values": LOOP_VALUES,
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


def build_endplay_event():
    loop_array = " ".join(f'"{value}"' for value in LOOP_VALUES)
    return f'''(event EndPlay
  (call-macro
    ForEachLoop
    :out (Array Element string)
    :out (Array Index int)
    :array (make-array {loop_array}))
  (PrintString
    :instring ArrayElement
    :bprinttoscreen true
    :bprinttolog true
    :textcolor "(R=0,G=1,B=1,A=1)"
    :duration 2
    :key "None"))'''


def append_endplay_event(original_text):
    return original_text.rstrip() + "\n\n" + build_endplay_event() + "\n"


def validate_modified_export(export_text):
    report["modified_contains_endplay_name"] = ("ReceiveEndPlay" in export_text) or ("\n  EndPlay\n" in export_text)
    report["modified_contains_foreach"] = ("ForEachLoop" in export_text) or ("for-each-loop" in export_text)
    report["modified_contains_make_array"] = "make-array" in export_text
    report["modified_loop_value_hits"] = {value: (f'"{value}"' in export_text) for value in LOOP_VALUES}
    report["modified_loop_value_count"] = sum(1 for hit in report["modified_loop_value_hits"].values() if hit)
    report["modified_print_uses_array_element"] = (
        ":instring ArrayElement" in export_text
        or ":instring Array Element" in export_text
    )
    report["modified_out_decl_string"] = ":out (Array Element string)" in export_text

    make_array_index = export_text.find("(make-array")
    array_segment = ""
    if make_array_index >= 0:
        array_tail = export_text[make_array_index:]
        out_index = array_tail.find("\n    :out")
        array_segment = array_tail if out_index < 0 else array_tail[:out_index]
    report["modified_make_array_nil_count"] = array_segment.count("nil")

    if not report["modified_contains_endplay_name"]:
        raise RuntimeError("回导出未发现 EndPlay / ReceiveEndPlay")
    if not report["modified_contains_foreach"]:
        raise RuntimeError("回导出未发现 ForEachLoop")
    if not report["modified_contains_make_array"]:
        raise RuntimeError("回导出未发现 make-array")
    if report["modified_loop_value_count"] != len(LOOP_VALUES):
        raise RuntimeError("回导出未完整保留 BP_Shrub 测试数组字符串值")
    if report["modified_make_array_nil_count"] != 0:
        raise RuntimeError("回导出的 BP_Shrub make-array 仍出现 nil")
    if not report["modified_out_decl_string"]:
        raise RuntimeError("回导出未保留 Array Element 的 string 类型声明")
    if not report["modified_print_uses_array_element"]:
        raise RuntimeError("回导出未确认 PrintString 仍引用 ArrayElement")


def validate_restored_export(export_text):
    report["restored_has_endplay"] = ("ReceiveEndPlay" in export_text) or ("\n  EndPlay\n" in export_text)
    report["restored_has_foreach"] = ("ForEachLoop" in export_text) or ("for-each-loop" in export_text)
    report["restored_has_loop_values"] = any(f'"{value}"' in export_text for value in LOOP_VALUES)
    report["restored_has_array_element_ref"] = (":instring ArrayElement" in export_text) or (":instring Array Element" in export_text)

    if report["restored_has_endplay"]:
        raise RuntimeError("恢复后仍残留 EndPlay")
    if report["restored_has_foreach"]:
        raise RuntimeError("恢复后仍残留 ForEachLoop")
    if report["restored_has_loop_values"]:
        raise RuntimeError("恢复后仍残留 BP_Shrub 测试数组值")
    if report["restored_has_array_element_ref"]:
        raise RuntimeError("恢复后仍残留 ArrayElement 引用")


def main():
    replace_graph = enum_value(["REPLACE_GRAPH", "ReplaceGraph"])
    original_text = ""
    modified_applied = False

    validate_result = unreal.BlueprintLispPythonBridge.validate_dsl(build_endplay_event())
    report["validate_fragment_success"] = get_result_bool(validate_result, "success", "b_success")
    report["validate_fragment_message"] = get_result_text(validate_result, "message")
    if not report["validate_fragment_success"]:
        raise RuntimeError(f"EndPlay 片段校验失败: {report['validate_fragment_message']}")
    log("BP_Shrub EndPlay + ForEachLoop + ArrayElement 片段语法校验通过")

    try:
        original_text = export_graph_text("original")
        save_text(ORIGINAL_EXPORT_PATH, original_text)
        log(f"已保存 BP_Shrub 原始 DSL: {ORIGINAL_EXPORT_PATH}")

        modified_text = append_endplay_event(original_text)
        save_text(MODIFIED_INPUT_PATH, modified_text)
        log(f"已生成 BP_Shrub 测试 DSL: {MODIFIED_INPUT_PATH}")

        validate_full_result = unreal.BlueprintLispPythonBridge.validate_dsl(modified_text)
        report["validate_full_success"] = get_result_bool(validate_full_result, "success", "b_success")
        report["validate_full_message"] = get_result_text(validate_full_result, "message")
        if not report["validate_full_success"]:
            raise RuntimeError(f"完整 DSL 校验失败: {report['validate_full_message']}")
        log("BP_Shrub 完整测试 DSL 校验通过")

        import_graph_text(modified_text, replace_graph, "modified")
        modified_applied = True

        modified_export = export_graph_text("modified")
        save_text(MODIFIED_EXPORT_PATH, modified_export)
        log(f"已保存 BP_Shrub 测试回导出 DSL: {MODIFIED_EXPORT_PATH}")
        validate_modified_export(modified_export)
        log("已确认 BP_Shrub 回导出保留 EndPlay、ForEachLoop、ArrayElement 引用与字符串数组值")
    finally:
        if original_text and modified_applied:
            import_graph_text(original_text, replace_graph, "restore")
            restored_export = export_graph_text("restored")
            save_text(RESTORED_EXPORT_PATH, restored_export)
            log(f"已保存 BP_Shrub 恢复后回导出 DSL: {RESTORED_EXPORT_PATH}")
            validate_restored_export(restored_export)
            log("已确认 BP_Shrub 已恢复到测试前状态")

    report["success"] = True


try:
    main()
except Exception as exc:
    fail(str(exc))
    fail(traceback.format_exc())
finally:
    save_report()
