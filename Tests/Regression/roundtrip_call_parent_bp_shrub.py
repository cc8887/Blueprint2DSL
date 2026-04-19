import json
import traceback
import unreal
import os


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR

RESULT_PATH = os.path.join(OUTPUT_DIR, "roundtrip_call_parent_bp_shrub_result.json")
ORIGINAL_EXPORT_PATH = os.path.join(OUTPUT_DIR, "bp_shrub_call_parent_original_export.bplisp")
MODIFIED_INPUT_PATH = os.path.join(OUTPUT_DIR, "bp_shrub_call_parent_input.bplisp")
MODIFIED_EXPORT_PATH = os.path.join(OUTPUT_DIR, "bp_shrub_call_parent_export.bplisp")
RESTORED_EXPORT_PATH = os.path.join(OUTPUT_DIR, "bp_shrub_call_parent_restored_export.bplisp")

BP_PATH = "/Game/Blueprint/Interactable/Resources/BP_Shrub"
GRAPH_NAME = "EventGraph"
MARKER_TEXT = "DSL_CallParentProbe"

report = {
    "success": False,
    "bp_path": BP_PATH,
    "graph_name": GRAPH_NAME,
    "marker_text": MARKER_TEXT,
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



def build_probe_event():
    return f'''(event CallParentProbeEvent
  (PrintString
    :instring "{MARKER_TEXT}"
    :bprinttoscreen true
    :bprinttolog true
    :textcolor "(R=1,G=0.3,B=0.3,A=1)"
    :duration 2
    :key "None"))'''



def append_probe_event(original_text):
    return original_text.rstrip() + "\n\n" + build_probe_event() + "\n"



def collect_call_parent_flags(prefix, export_text):
    report[f"{prefix}_has_call_parent_beginplay"] = "(call-parent\n    ReceiveBeginPlay" in export_text
    report[f"{prefix}_has_call_parent_overlap"] = "(call-parent\n    ReceiveActorBeginOverlap" in export_text
    report[f"{prefix}_has_call_parent_tick"] = "(call-parent\n    ReceiveTick" in export_text
    report[f"{prefix}_has_overlap_param"] = ":otheractor OtherActor" in export_text
    report[f"{prefix}_has_tick_param"] = ":deltaseconds DeltaSeconds" in export_text
    report[f"{prefix}_call_parent_count"] = export_text.count("(call-parent")



def assert_call_parent_intact(prefix):
    if not report[f"{prefix}_has_call_parent_beginplay"]:
        raise RuntimeError(f"{prefix} 导出未保留 ReceiveBeginPlay 的 call-parent")
    if not report[f"{prefix}_has_call_parent_overlap"]:
        raise RuntimeError(f"{prefix} 导出未保留 ReceiveActorBeginOverlap 的 call-parent")
    if not report[f"{prefix}_has_call_parent_tick"]:
        raise RuntimeError(f"{prefix} 导出未保留 ReceiveTick 的 call-parent")
    if not report[f"{prefix}_has_overlap_param"]:
        raise RuntimeError(f"{prefix} 导出未保留 ReceiveActorBeginOverlap 的 OtherActor 参数")
    if not report[f"{prefix}_has_tick_param"]:
        raise RuntimeError(f"{prefix} 导出未保留 ReceiveTick 的 DeltaSeconds 参数")
    if report[f"{prefix}_call_parent_count"] < 3:
        raise RuntimeError(f"{prefix} 导出的 call-parent 数量少于预期")



def validate_original_export(export_text):
    report["original_has_marker"] = MARKER_TEXT in export_text
    collect_call_parent_flags("original", export_text)
    if report["original_has_marker"]:
        raise RuntimeError("原始导出已包含 DSL_CallParentProbe，测试基线被污染")
    assert_call_parent_intact("original")



def validate_modified_export(export_text):
    report["modified_has_marker"] = MARKER_TEXT in export_text
    report["modified_has_probe_event_name"] = "CallParentProbeEvent" in export_text
    collect_call_parent_flags("modified", export_text)
    if not report["modified_has_marker"]:
        raise RuntimeError("回导出未发现 DSL_CallParentProbe，说明 probe event 未保留")
    if not report["modified_has_probe_event_name"]:
        raise RuntimeError("回导出未发现 CallParentProbeEvent")
    assert_call_parent_intact("modified")



def validate_restored_export(export_text):
    report["restored_has_marker"] = MARKER_TEXT in export_text
    report["restored_has_probe_event_name"] = "CallParentProbeEvent" in export_text
    collect_call_parent_flags("restored", export_text)
    if report["restored_has_marker"]:
        raise RuntimeError("恢复后仍残留 DSL_CallParentProbe")
    if report["restored_has_probe_event_name"]:
        raise RuntimeError("恢复后仍残留 CallParentProbeEvent")
    assert_call_parent_intact("restored")



def main():
    replace_graph = enum_value(["REPLACE_GRAPH", "ReplaceGraph"])
    original_text = ""
    modified_applied = False

    validate_result = unreal.BlueprintLispPythonBridge.validate_dsl(build_probe_event())
    report["validate_fragment_success"] = get_result_bool(validate_result, "success", "b_success")
    report["validate_fragment_message"] = get_result_text(validate_result, "message")
    if not report["validate_fragment_success"]:
        raise RuntimeError(f"probe event 片段校验失败: {report['validate_fragment_message']}")
    log("BP_Shrub call-parent probe event 片段语法校验通过")

    try:
        original_text = export_graph_text("original")
        save_text(ORIGINAL_EXPORT_PATH, original_text)
        log(f"已保存 BP_Shrub call-parent 原始 DSL: {ORIGINAL_EXPORT_PATH}")
        validate_original_export(original_text)
        log("已确认 BP_Shrub 原始导出包含 3 条 call-parent 基线")

        modified_text = append_probe_event(original_text)
        save_text(MODIFIED_INPUT_PATH, modified_text)
        log(f"已生成 BP_Shrub call-parent 测试 DSL: {MODIFIED_INPUT_PATH}")

        validate_full_result = unreal.BlueprintLispPythonBridge.validate_dsl(modified_text)
        report["validate_full_success"] = get_result_bool(validate_full_result, "success", "b_success")
        report["validate_full_message"] = get_result_text(validate_full_result, "message")
        if not report["validate_full_success"]:
            raise RuntimeError(f"完整 DSL 校验失败: {report['validate_full_message']}")
        log("BP_Shrub call-parent 完整测试 DSL 校验通过")

        import_graph_text(modified_text, replace_graph, "modified")
        modified_applied = True

        modified_export = export_graph_text("modified")
        save_text(MODIFIED_EXPORT_PATH, modified_export)
        log(f"已保存 BP_Shrub call-parent 回导出 DSL: {MODIFIED_EXPORT_PATH}")
        validate_modified_export(modified_export)
        log("已确认 BP_Shrub 回导出继续保留 3 条 call-parent 与 probe event")
    finally:
        if original_text and modified_applied:
            import_graph_text(original_text, replace_graph, "restore")
            restored_export = export_graph_text("restored")
            save_text(RESTORED_EXPORT_PATH, restored_export)
            log(f"已保存 BP_Shrub call-parent 恢复后 DSL: {RESTORED_EXPORT_PATH}")
            validate_restored_export(restored_export)
            log("已确认 BP_Shrub 恢复后仍保留 call-parent 基线且无 probe 残留")

    report["success"] = True


try:
    main()
except Exception as exc:
    fail(str(exc))
    fail(traceback.format_exc())
finally:
    save_report()
