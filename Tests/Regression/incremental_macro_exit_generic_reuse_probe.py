import json
import os
import re
import unreal

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR
RESULT_PATH = os.path.join(OUTPUT_DIR, "incremental_macro_exit_generic_reuse_probe_result.json")
EXPORT_BEFORE_PATH = os.path.join(OUTPUT_DIR, "incremental_macro_exit_generic_reuse_probe_before_update.bplisp")
EXPORT_AFTER_PATH = os.path.join(OUTPUT_DIR, "incremental_macro_exit_generic_reuse_probe_after_update.bplisp")

SOURCE_BP_PATH = "/Game/Blueprint/Core/Player/BP_Player"
TEST_DIR = "/Game/Blueprint/BPLispTests"
TEST_BP_PATH = f"{TEST_DIR}/BP_Player_MacroExitGenericReuseProbe"
GRAPH_NAME = "Villager Overlap Check"
GENERIC_FORM_NAME = "变更路线节点"
TEXT_V2 = "Incremental Macro Exit/Generic Reuse V2"
PRINT_INSERTION = rf'''        (PrintString
          :instring "{TEXT_V2}"
          :bprinttoscreen true
          :bprinttolog true
          :textcolor "(R=0.0,G=1.0,B=0.0,A=1.0)"
          :duration 2
          :key "None")
        (exit'''

bridge = unreal.BlueprintLispPythonBridge
asset_lib = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
report = {
    "success": False,
    "source_bp_path": SOURCE_BP_PATH,
    "test_bp_path": TEST_BP_PATH,
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


def duplicate_test_blueprint():
    ensure_directory(TEST_DIR)
    if asset_lib.does_asset_exist(TEST_BP_PATH):
        asset_lib.delete_asset(TEST_BP_PATH)
    source_bp = unreal.load_asset(SOURCE_BP_PATH)
    if not source_bp:
        raise RuntimeError(f"加载源蓝图失败: {SOURCE_BP_PATH}")
    asset_name = TEST_BP_PATH.rsplit("/", 1)[-1]
    duplicated = asset_tools.duplicate_asset(asset_name, TEST_DIR, source_bp)
    if not duplicated:
        if not asset_lib.duplicate_asset(SOURCE_BP_PATH, TEST_BP_PATH):
            raise RuntimeError(f"复制测试蓝图失败: {TEST_BP_PATH}")
        duplicated = unreal.load_asset(TEST_BP_PATH)
    if not duplicated:
        raise RuntimeError(f"加载复制后的测试蓝图失败: {TEST_BP_PATH}")
    asset_lib.save_loaded_asset(duplicated)
    return duplicated


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


def extract_generic_id(dsl_text):
    match = re.search(r'\(' + re.escape(GENERIC_FORM_NAME) + r'\s+:id\s+"([^"]+)"\)', dsl_text)
    return match.group(1) if match else ""


def extract_event_id(dsl_text):
    match = re.search(r':event-id\s+"([^"]+)"', dsl_text)
    return match.group(1) if match else ""


try:
    duplicate_test_blueprint()

    export_before = export_graph(TEST_BP_PATH, GRAPH_NAME)
    export_before_dsl = get_text(export_before, "dsl_text")
    save_text(EXPORT_BEFORE_PATH, export_before_dsl)
    event_id_before = extract_event_id(export_before_dsl)
    generic_id_before = extract_generic_id(export_before_dsl)
    branch_id_before = extract_form_id(export_before_dsl, "branch")
    exit_id_before = extract_form_id(export_before_dsl, "exit")
    report["export_before_success"] = get_bool(export_before, "success", "b_success")
    report["event_id_before"] = event_id_before
    report["generic_id_before"] = generic_id_before
    report["branch_id_before"] = branch_id_before
    report["exit_id_before"] = exit_id_before
    report["contains_generic_form"] = GENERIC_FORM_NAME in export_before_dsl
    if not report["export_before_success"]:
        raise RuntimeError("首次导出失败")
    if not event_id_before:
        raise RuntimeError("首次导出未找到 macro 的 :event-id")
    if not generic_id_before:
        raise RuntimeError("首次导出未找到 generic fallback 节点的 :id")
    if not branch_id_before:
        raise RuntimeError("首次导出未找到 branch 节点的 :id")
    if not exit_id_before:
        raise RuntimeError("首次导出未找到 exit 节点的 :id")
    if TEXT_V2 in export_before_dsl:
        raise RuntimeError("测试蓝图基线已包含 probe 文本，无法继续")

    updated_dsl = rf'''(macro
  "{GRAPH_NAME}"
  :event-id "{event_id_before}"
  :exit (输出 (Output Actor))
  ({GENERIC_FORM_NAME} :id "{generic_id_before}")
  (branch
    true
    :true
      (seq
        (PrintString
          :instring "{TEXT_V2}"
          :bprinttoscreen true
          :bprinttolog true
          :textcolor "(R=0.0,G=1.0,B=0.0,A=1.0)"
          :duration 2
          :key "None")
        (exit
          输出
          :output (Output self)
          :id "{exit_id_before}"))
    :false nil
    :id "{branch_id_before}"))'''

    update_result = import_graph(TEST_BP_PATH, GRAPH_NAME, updated_dsl, MERGE_APPEND)
    report["update_success"] = get_bool(update_result, "success", "b_success")
    report["update_message"] = get_text(update_result, "message")
    report["update_warnings"] = get_list(update_result, "warnings")
    report["uses_replacegraph_fallback"] = "ReplaceGraph fallback" in report["update_message"]
    if not report["update_success"]:
        raise RuntimeError(f"update 失败: {report['update_message']}")
    if report["uses_replacegraph_fallback"]:
        raise RuntimeError("update 回退到了 ReplaceGraph")

    export_after = export_graph(TEST_BP_PATH, GRAPH_NAME)
    export_after_dsl = get_text(export_after, "dsl_text")
    save_text(EXPORT_AFTER_PATH, export_after_dsl)
    generic_id_after = extract_generic_id(export_after_dsl)
    exit_id_after = extract_form_id(export_after_dsl, "exit")
    report["export_after_success"] = get_bool(export_after, "success", "b_success")
    report["generic_id_after"] = generic_id_after
    report["exit_id_after"] = exit_id_after
    report["contains_v2"] = TEXT_V2 in export_after_dsl
    report["generic_reused"] = bool(generic_id_before) and generic_id_before == generic_id_after
    report["exit_reused"] = bool(exit_id_before) and exit_id_before == exit_id_after

    if not report["export_after_success"]:
        raise RuntimeError("update 后导出失败")
    if not report["generic_reused"]:
        raise RuntimeError("generic fallback 节点的 :id 在 update 前后未保持一致")
    if not report["exit_reused"]:
        raise RuntimeError("exit 节点的 :id 在 update 前后未保持一致")
    if not report["contains_v2"]:
        raise RuntimeError("update 后未发现新增的 probe PrintString")

    report["success"] = True
except Exception as exc:
    report["errors"].append(str(exc))
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False))
