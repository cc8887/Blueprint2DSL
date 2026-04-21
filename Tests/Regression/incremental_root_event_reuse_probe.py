import json
import os
import re
import unreal

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR
RESULT_PATH = os.path.join(OUTPUT_DIR, "incremental_root_event_reuse_probe_result.json")
EXPORT_BEFORE_PATH = os.path.join(OUTPUT_DIR, "incremental_root_event_reuse_probe_before_update.bplisp")
EXPORT_AFTER_PATH = os.path.join(OUTPUT_DIR, "incremental_root_event_reuse_probe_after_update.bplisp")

SOURCE_ASSET = "/Game/AdvancedLocomotionV4/CharacterAssets/MannequinSkeleton/ALS_AnimBP"
TEST_ASSET = "/Game/AnimBP2FP/Phase5/ALS_AnimBP_RootEventReuseProbe"
GRAPH_NAME = "EventGraph"
TEXT_V1 = "Incremental Root Event Reuse V1"
TEXT_V2 = "Incremental Root Event Reuse V2"

DSL_V1 = rf'''(event
  BlueprintInitializeAnimation
  (PrintString
    :instring "{TEXT_V1}"
    :bprinttoscreen true
    :bprinttolog true
    :textcolor "(R=0.0,G=1.0,B=0.0,A=1.0)"
    :duration 2
    :key "None"))'''

bridge = unreal.AnimBP2FPPythonBridge
asset_lib = unreal.EditorAssetLibrary
report = {
    "success": False,
    "source_asset": SOURCE_ASSET,
    "test_asset": TEST_ASSET,
    "graph_name": GRAPH_NAME,
    "errors": [],
}


def get_bool(obj, *names):
    for name in names:
        if hasattr(obj, name):
            return bool(getattr(obj, name))
    return False


def get_text(obj, name):
    return str(getattr(obj, name, "")) if obj else ""


def get_list(obj, name):
    value = getattr(obj, name, []) if obj else []
    try:
        return [str(item) for item in value]
    except TypeError:
        return []


def save_text(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text or "")


def export_graph(asset_path):
    return bridge.export_event_graph_to_text(asset_path, GRAPH_NAME, False, True)


def extract_event_id(dsl_text):
    match = re.search(r':event-id\s+"([^"]+)"', dsl_text or "")
    return match.group(1) if match else ""


try:
    if asset_lib.does_asset_exist(TEST_ASSET):
        asset_lib.delete_asset(TEST_ASSET)
    report["duplicate_success"] = bool(asset_lib.duplicate_asset(SOURCE_ASSET, TEST_ASSET))
    if not report["duplicate_success"]:
        raise RuntimeError("复制测试资产失败")

    import_result = bridge.import_event_graph_from_text(TEST_ASSET, GRAPH_NAME, DSL_V1, False, False)
    report["import_success"] = get_bool(import_result, "success", "b_success")
    report["import_message"] = get_text(import_result, "message")
    report["import_warnings"] = get_list(import_result, "warnings")
    if not report["import_success"]:
        raise RuntimeError(f"首次导入失败: {report['import_message']}")

    export_before = export_graph(TEST_ASSET)
    export_before_dsl = get_text(export_before, "dsl_text")
    save_text(EXPORT_BEFORE_PATH, export_before_dsl)
    event_id_before = extract_event_id(export_before_dsl)
    report["export_before_success"] = get_bool(export_before, "success", "b_success")
    report["event_id_before"] = event_id_before
    if not report["export_before_success"] or not event_id_before:
        raise RuntimeError("首次导出失败或未找到 :event-id")

    updated_dsl = export_before_dsl.replace(TEXT_V1, TEXT_V2)
    update_result = bridge.update_event_graph_from_text(TEST_ASSET, GRAPH_NAME, updated_dsl, False, False)
    report["update_success"] = get_bool(update_result, "success", "b_success")
    report["update_message"] = get_text(update_result, "message")
    report["update_warnings"] = get_list(update_result, "warnings")
    report["uses_replacegraph_fallback"] = "ReplaceGraph fallback" in report["update_message"]
    if not report["update_success"]:
        raise RuntimeError(f"update 失败: {report['update_message']}")
    if report["uses_replacegraph_fallback"]:
        raise RuntimeError("update 回退到了 ReplaceGraph")

    export_after = export_graph(TEST_ASSET)
    export_after_dsl = get_text(export_after, "dsl_text")
    save_text(EXPORT_AFTER_PATH, export_after_dsl)
    event_id_after = extract_event_id(export_after_dsl)
    report["export_after_success"] = get_bool(export_after, "success", "b_success")
    report["event_id_after"] = event_id_after
    report["contains_v2"] = TEXT_V2 in export_after_dsl
    report["contains_v1"] = TEXT_V1 in export_after_dsl
    report["root_event_reused"] = bool(event_id_before) and event_id_before == event_id_after

    if not report["export_after_success"]:
        raise RuntimeError("update 后导出失败")
    if not report["root_event_reused"]:
        raise RuntimeError("root event 的 :event-id 在 update 前后未保持一致")
    if not report["contains_v2"] or report["contains_v1"]:
        raise RuntimeError("update 后事件内容未按预期更新")

    report["success"] = True
except Exception as exc:
    report["errors"].append(str(exc))
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False))
