import json
import os
import re
import unreal

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR
RESULT_PATH = os.path.join(OUTPUT_DIR, "incremental_break_struct_reuse_probe_result.json")
EXPORT_BEFORE_PATH = os.path.join(OUTPUT_DIR, "incremental_break_struct_reuse_probe_before_update.bplisp")
EXPORT_AFTER_PATH = os.path.join(OUTPUT_DIR, "incremental_break_struct_reuse_probe_after_update.bplisp")

TEST_DIR = "/Game/Blueprint/BPLispTests"
TEST_ASSET = f"{TEST_DIR}/BP_BreakStructReuseProbe"
GRAPH_NAME = "EventGraph"
STRUCT_NAME = "Vector"
FIELD_V1 = "X"
FIELD_V2 = "Y"
PRINT_TEXT = "Incremental Break Struct Reuse"

bridge = unreal.BlueprintLispPythonBridge
asset_lib = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
report = {
    "success": False,
    "test_asset": TEST_ASSET,
    "graph_name": GRAPH_NAME,
    "struct_name": STRUCT_NAME,
    "errors": [],
}


def build_dsl(field_name):
    return rf'''(event
  ReceiveBeginPlay
  (PrintString
    :instring "{PRINT_TEXT}"
    :bprinttoscreen true
    :bprinttolog true
    :textcolor "(R=0.0,G=1.0,B=0.0,A=1.0)"
    :duration
      (break-struct
        :struct {STRUCT_NAME}
        :value (K2_GetActorLocation)
        :field {field_name})
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


def extract_form_blocks(dsl_text, form_name):
    token = f"({form_name}"
    blocks = []
    search_from = 0
    while True:
        start = dsl_text.find(token, search_from)
        if start < 0:
            return blocks
        depth = 0
        for index in range(start, len(dsl_text)):
            ch = dsl_text[index]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    blocks.append(dsl_text[start:index + 1])
                    search_from = index + 1
                    break
        else:
            return blocks


def extract_last_form_id(dsl_text, form_name):
    ids = []
    for block in extract_form_blocks(dsl_text or "", form_name):
        ids.extend(re.findall(r':id\s+"([^"]+)"', block))
    return ids[-1] if ids else ""


def extract_break_struct_field(block_text):
    if not block_text:
        return ""
    match = re.search(r':field\s+(?:"([^"]+)"|([^\s\)]+))', block_text)
    if not match:
        return ""
    return match.group(1) or match.group(2) or ""


def replace_field_value(dsl_text, old_field, new_field):
    replaced = dsl_text.replace(f":field {old_field}", f":field {new_field}", 1)
    if replaced != dsl_text:
        return replaced
    replaced = dsl_text.replace(f':field "{old_field}"', f':field "{new_field}"', 1)
    return replaced


try:
    recreate_character_bp(TEST_ASSET)

    import_result = import_graph(TEST_ASSET, GRAPH_NAME, build_dsl(FIELD_V1), REPLACE_GRAPH)
    report["import_success"] = get_bool(import_result, "success", "b_success")
    report["import_message"] = get_text(import_result, "message")
    report["import_warnings"] = get_list(import_result, "warnings")
    if not report["import_success"]:
        raise RuntimeError(f"首次导入失败: {report['import_message']}")

    export_before = export_graph(TEST_ASSET, GRAPH_NAME)
    export_before_dsl = get_text(export_before, "dsl_text")
    save_text(EXPORT_BEFORE_PATH, export_before_dsl)
    break_struct_blocks_before = extract_form_blocks(export_before_dsl, "break-struct")
    break_struct_id_before = extract_last_form_id(export_before_dsl, "break-struct")
    print_id_before = extract_last_form_id(export_before_dsl, "PrintString")
    field_before = extract_break_struct_field(break_struct_blocks_before[-1] if break_struct_blocks_before else "")
    report["export_before_success"] = get_bool(export_before, "success", "b_success")
    report["break_struct_id_before"] = break_struct_id_before
    report["print_id_before"] = print_id_before
    report["field_before"] = field_before
    report["contains_break_struct"] = "(break-struct" in export_before_dsl
    report["contains_source_call"] = "(K2_GetActorLocation" in export_before_dsl
    if not report["export_before_success"]:
        raise RuntimeError("首次导出失败")
    if not break_struct_id_before:
        raise RuntimeError("首次导出未找到 break-struct 的 :id")
    if field_before != FIELD_V1:
        raise RuntimeError(f"首次导出的 break-struct :field 异常: {field_before}")

    updated_dsl = replace_field_value(export_before_dsl, FIELD_V1, FIELD_V2)
    if updated_dsl == export_before_dsl:
        raise RuntimeError("未能在导出 DSL 中定位 break-struct 的 :field 更新点")

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
    break_struct_blocks_after = extract_form_blocks(export_after_dsl, "break-struct")
    break_struct_id_after = extract_last_form_id(export_after_dsl, "break-struct")
    print_id_after = extract_last_form_id(export_after_dsl, "PrintString")
    field_after = extract_break_struct_field(break_struct_blocks_after[-1] if break_struct_blocks_after else "")
    report["export_after_success"] = get_bool(export_after, "success", "b_success")
    report["break_struct_id_after"] = break_struct_id_after
    report["print_id_after"] = print_id_after
    report["field_after"] = field_after
    report["contains_field_v2"] = field_after == FIELD_V2
    report["contains_field_v1"] = field_after == FIELD_V1
    report["break_struct_reused"] = bool(break_struct_id_before) and break_struct_id_before == break_struct_id_after
    report["print_reused"] = bool(print_id_before) and print_id_before == print_id_after

    if not report["export_after_success"]:
        raise RuntimeError("update 后导出失败")
    if not report["break_struct_reused"]:
        raise RuntimeError("break-struct 的 :id 在 update 前后未保持一致")
    if field_after != FIELD_V2:
        raise RuntimeError(f"update 后 break-struct 的 :field 未按预期更新: {field_after}")

    report["success"] = True
except Exception as exc:
    report["errors"].append(str(exc))
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False))
