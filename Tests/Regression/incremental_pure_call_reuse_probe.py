import json
import os
import re
import unreal

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR
RESULT_PATH = os.path.join(OUTPUT_DIR, "incremental_pure_call_reuse_probe_result.json")
EXPORT_BEFORE_PATH = os.path.join(OUTPUT_DIR, "incremental_pure_call_reuse_probe_before_update.bplisp")
EXPORT_AFTER_PATH = os.path.join(OUTPUT_DIR, "incremental_pure_call_reuse_probe_after_update.bplisp")

TEST_DIR = "/Game/Blueprint/BPLispTests"
TEST_ASSET = f"{TEST_DIR}/BP_PureCallReuseProbe"
GRAPH_NAME = "EventGraph"
TEXT_BODY = "Incremental Pure Call Reuse"
PURE_CALL_SPECS = [
    {
        "name": "Multiply_DoubleDouble",
        "value_v1": "2.25",
        "value_v2": "3.75",
    },
    {
        "name": "Multiply_FloatFloat",
        "value_v1": "2.25",
        "value_v2": "3.75",
    },
]

bridge = unreal.BlueprintLispPythonBridge
asset_lib = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
report = {
    "success": False,
    "test_asset": TEST_ASSET,
    "graph_name": GRAPH_NAME,
    "errors": [],
    "attempts": [],
}


def build_dsl(spec, value_literal):
    pure_expr = f'({spec["name"]} {value_literal} 1)'
    return rf'''(event
  ReceiveBeginPlay
  (PrintString
    :instring "{TEXT_BODY}"
    :bprinttoscreen true
    :bprinttolog true
    :textcolor "(R=0.0,G=1.0,B=0.0,A=1.0)"
    :duration {pure_expr}
    :key "None"))'''


def enum_value(candidates):
    for attr_name in dir(unreal):
        if "BlueprintLisp" not in attr_name or "ImportMode" not in attr_name:
            continue
        enum_cls = getattr(unreal, attr_name, None)
        if enum_cls is None:
            continue
        for candidate in candidates:
            if hasattr(enum_cls, candidate):
                return getattr(enum_cls, candidate)
    raise RuntimeError("未找到 BlueprintLisp Python ImportMode 枚举")


REPLACE_GRAPH = enum_value(["REPLACE_GRAPH", "ReplaceGraph"])
MERGE_APPEND = enum_value(["MERGE_APPEND", "MergeAppend"])


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


def ensure_directory(asset_dir):
    if not asset_lib.does_directory_exist(asset_dir):
        asset_lib.make_directory(asset_dir)


def recreate_character_bp(asset_path):
    ensure_directory(TEST_DIR)
    if asset_lib.does_asset_exist(asset_path):
        asset_lib.delete_asset(asset_path)
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("ParentClass", unreal.Character)
    asset_name = asset_path.rsplit("/", 1)[-1]
    bp = asset_tools.create_asset(asset_name, TEST_DIR, None, factory)
    if not bp:
        raise RuntimeError(f"创建测试蓝图失败: {asset_path}")
    asset_lib.save_loaded_asset(bp)
    return bp


def import_graph(asset_path, graph_name, dsl_text, import_mode):
    return bridge.import_graph_from_text(asset_path, graph_name, dsl_text, import_mode, True, True)


def export_graph(asset_path, graph_name):
    return bridge.export_graph_to_text(asset_path, graph_name, False, True)


def extract_form_id(dsl_text, form_name):
    pattern = r'\(' + re.escape(form_name) + r'[\s\S]*?:id\s+"([^"]+)"\)'
    match = re.search(pattern, dsl_text or "", re.S)
    return match.group(1) if match else ""


try:
    selected_spec = None
    import_result = None
    for spec in PURE_CALL_SPECS:
        recreate_character_bp(TEST_ASSET)
        attempt_dsl = build_dsl(spec, spec["value_v1"])
        attempt_result = import_graph(TEST_ASSET, GRAPH_NAME, attempt_dsl, REPLACE_GRAPH)
        attempt_info = {
            "pure_call_name": spec["name"],
            "success": get_bool(attempt_result, "success", "b_success"),
            "message": get_text(attempt_result, "message"),
            "warnings": get_list(attempt_result, "warnings"),
        }
        report["attempts"].append(attempt_info)
        if attempt_info["success"]:
            selected_spec = spec
            import_result = attempt_result
            break

    if not selected_spec or import_result is None:
        raise RuntimeError("所有 pure call 候选表达式都导入失败")

    report["pure_call_name"] = selected_spec["name"]
    report["import_success"] = get_bool(import_result, "success", "b_success")
    report["import_message"] = get_text(import_result, "message")
    report["import_warnings"] = get_list(import_result, "warnings")

    export_before = export_graph(TEST_ASSET, GRAPH_NAME)
    export_before_dsl = get_text(export_before, "dsl_text")
    save_text(EXPORT_BEFORE_PATH, export_before_dsl)
    pure_call_id_before = extract_form_id(export_before_dsl, selected_spec["name"])
    print_id_before = extract_form_id(export_before_dsl, "PrintString")
    report["export_before_success"] = get_bool(export_before, "success", "b_success")
    report["pure_call_id_before"] = pure_call_id_before
    report["print_id_before"] = print_id_before
    report["contains_pure_call"] = f'({selected_spec["name"]}' in export_before_dsl
    if not report["export_before_success"]:
        raise RuntimeError("首次导出失败")
    if not pure_call_id_before:
        raise RuntimeError("首次导出未找到 pure call 的 :id")
    if selected_spec["value_v2"] in export_before_dsl:
        raise RuntimeError("测试蓝图基线已包含 update 值，无法继续")

    updated_dsl = export_before_dsl.replace(selected_spec["value_v1"], selected_spec["value_v2"], 1)
    if updated_dsl == export_before_dsl:
        raise RuntimeError("未能在导出 DSL 中定位 pure call 的更新值")

    update_result = import_graph(TEST_ASSET, GRAPH_NAME, updated_dsl, MERGE_APPEND)
    report["update_success"] = get_bool(update_result, "success", "b_success")
    report["update_message"] = get_text(update_result, "message")
    report["update_warnings"] = get_list(update_result, "warnings")
    report["uses_replacegraph_fallback"] = "ReplaceGraph fallback" in report["update_message"]
    if not report["update_success"]:
        raise RuntimeError(f"update 失败: {report['update_message']}")
    if report["uses_replacegraph_fallback"]:
        raise RuntimeError("update 回退到了 ReplaceGraph")

    export_after = export_graph(TEST_ASSET, GRAPH_NAME)
    export_after_dsl = get_text(export_after, "dsl_text")
    save_text(EXPORT_AFTER_PATH, export_after_dsl)
    pure_call_id_after = extract_form_id(export_after_dsl, selected_spec["name"])
    print_id_after = extract_form_id(export_after_dsl, "PrintString")
    report["export_after_success"] = get_bool(export_after, "success", "b_success")
    report["pure_call_id_after"] = pure_call_id_after
    report["print_id_after"] = print_id_after
    report["contains_v2"] = selected_spec["value_v2"] in export_after_dsl
    report["contains_v1"] = selected_spec["value_v1"] in export_after_dsl
    report["pure_call_reused"] = bool(pure_call_id_before) and pure_call_id_before == pure_call_id_after
    report["print_reused"] = bool(print_id_before) and print_id_before == print_id_after

    if not report["export_after_success"]:
        raise RuntimeError("update 后导出失败")
    if not report["pure_call_reused"]:
        raise RuntimeError("pure call 的 :id 在 update 前后未保持一致")
    if not report["contains_v2"] or report["contains_v1"]:
        raise RuntimeError("update 后 pure call 数值未按预期更新")

    report["success"] = True
except Exception as exc:
    report["errors"].append(str(exc))
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False))
