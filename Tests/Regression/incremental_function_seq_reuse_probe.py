import json
import os
import re
import unreal

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR
RESULT_PATH = os.path.join(OUTPUT_DIR, "incremental_function_seq_reuse_probe_result.json")
EXPORT_BEFORE_PATH = os.path.join(OUTPUT_DIR, "incremental_function_seq_reuse_probe_before_update.bplisp")
EXPORT_AFTER_PATH = os.path.join(OUTPUT_DIR, "incremental_function_seq_reuse_probe_after_update.bplisp")

TEST_DIR = "/Game/AnimBP2FP/Phase5"
TEST_ASSET = f"{TEST_DIR}/BP_FunctionSeqReuseProbe"
CREATE_GRAPH_HOST = "EventGraph"
GRAPH_NAME = "WB_SeqReuseProbe"
TEXT_V1 = "Incremental Sequence Reuse V1"
TEXT_V2 = "Incremental Sequence Reuse V2"

CREATE_DSL = f"(func {GRAPH_NAME})"
FUNCTION_DSL_V1 = rf'''(function
  {GRAPH_NAME}
  (seq
    (PrintString
      :instring "{TEXT_V1}"
      :bprinttoscreen true
      :bprinttolog true
      :textcolor "(R=0.0,G=1.0,B=0.0,A=1.0)"
      :duration 2
      :key "None")
    (PrintString
      :instring "{TEXT_V1}"
      :bprinttoscreen true
      :bprinttolog true
      :textcolor "(R=0.0,G=1.0,B=0.0,A=1.0)"
      :duration 2
      :key "None")
    :id "seq-root-probe"))'''

bridge = unreal.BlueprintLispPythonBridge
asset_lib = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
report = {
    "success": False,
    "test_asset": TEST_ASSET,
    "graph_name": GRAPH_NAME,
    "errors": [],
}


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


def extract_form_block(dsl_text, form_name):
    token = f"({form_name}"
    start = dsl_text.find(token)
    if start < 0:
        return ""
    depth = 0
    for index in range(start, len(dsl_text)):
        ch = dsl_text[index]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return dsl_text[start:index + 1]
    return ""


def extract_form_id(dsl_text, form_name):
    block = extract_form_block(dsl_text, form_name)
    ids = re.findall(r':id\s+"([^"]+)"', block)
    return ids[-1] if ids else ""


try:
    recreate_character_bp(TEST_ASSET)

    create_result = import_graph(TEST_ASSET, CREATE_GRAPH_HOST, CREATE_DSL, MERGE_APPEND)
    report["create_success"] = get_bool(create_result, "success", "b_success")
    report["create_message"] = get_text(create_result, "message")
    report["create_warnings"] = get_list(create_result, "warnings")
    if not report["create_success"]:
        raise RuntimeError(f"创建函数图失败: {report['create_message']}")

    import_result = import_graph(TEST_ASSET, GRAPH_NAME, FUNCTION_DSL_V1, REPLACE_GRAPH)
    report["import_success"] = get_bool(import_result, "success", "b_success")
    report["import_message"] = get_text(import_result, "message")
    report["import_warnings"] = get_list(import_result, "warnings")
    if not report["import_success"]:
        raise RuntimeError(f"首次导入失败: {report['import_message']}")

    export_before = export_graph(TEST_ASSET, GRAPH_NAME)
    export_before_dsl = get_text(export_before, "dsl_text")
    save_text(EXPORT_BEFORE_PATH, export_before_dsl)
    seq_id_before = extract_form_id(export_before_dsl, "seq")
    report["export_before_success"] = get_bool(export_before, "success", "b_success")
    report["seq_id_before"] = seq_id_before
    report["export_before_contains_seq"] = "(seq" in export_before_dsl
    if not report["export_before_success"] or not seq_id_before:
        raise RuntimeError("首次导出失败或未找到 sequence 节点的 :id")

    updated_dsl = export_before_dsl.replace(TEXT_V1, TEXT_V2, 1)
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
    seq_id_after = extract_form_id(export_after_dsl, "seq")
    report["export_after_success"] = get_bool(export_after, "success", "b_success")
    report["seq_id_after"] = seq_id_after
    report["contains_v2"] = TEXT_V2 in export_after_dsl
    report["contains_v1"] = TEXT_V1 in export_after_dsl
    report["seq_reused"] = bool(seq_id_before) and seq_id_before == seq_id_after

    if not report["export_after_success"]:
        raise RuntimeError("update 后导出失败")
    if not report["seq_reused"]:
        raise RuntimeError("sequence 节点的 :id 在 update 前后未保持一致")
    if not report["contains_v2"] or not report["contains_v1"]:
        raise RuntimeError("update 后 sequence probe 文本未按预期更新")

    report["success"] = True
except Exception as exc:
    report["errors"].append(str(exc))
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False))
