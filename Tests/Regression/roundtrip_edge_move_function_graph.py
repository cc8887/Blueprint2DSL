import json
import re
import traceback
import unreal
import os


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR

RESULT_PATH = os.path.join(OUTPUT_DIR, "roundtrip_edge_move_function_graph_result.json")
EXPORT_DIR = OUTPUT_DIR
BP_PATH = "/Game/Blueprint/BPLispTests/BP_Player_EdgeScrollLike_Test"
FUNCTION_NAME = "WB_ApplyEdgeMoveLike"

EXPECTED_NORMALIZED_EXPORT = '''(function
  WB_ApplyEdgeMoveLike
  (AddMovementInput
    :worlddirection
      (call-macro
        "Edge Move"
        :out (Direction vector))
    :scalevalue
      (call-macro
        "Edge Move"
        :out (Strength float))
    :bforce false))'''.strip()

report = {
    "success": False,
    "bp_path": BP_PATH,
    "function_name": FUNCTION_NAME,
    "variants": [],
    "expected_normalized_export": EXPECTED_NORMALIZED_EXPORT,
    "errors": [],
}


def get_bool(result, *names):
    for name in names:
        if hasattr(result, name):
            return bool(getattr(result, name))
    return False



def get_text(result, *names):
    for name in names:
        if hasattr(result, name):
            value = getattr(result, name)
            if value is not None:
                return str(value)
    return ""



def save_text(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)



def contains_edge_move_macro_call(text):
    return bool(text) and "call-macro" in text and "Edge Move" in text



def normalize_export(text):
    if not text:
        return ""
    text = re.sub(r'\n\s*:event-id\s+"[^"]+"', "", text)
    text = re.sub(r'\n\s*:id\s+"[^"]+"', "", text)
    return text.strip()



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

VARIANTS = [
    {
        "id": "inline_double_call_macro",
        "dsl": f'''(function
  {FUNCTION_NAME}
  (AddMovementInput
    :worlddirection (call-macro Edge Move)
    :scalevalue (call-macro Edge Move)
    :bforce false))''',
    },
    {
        "id": "standalone_alias_then_use",
        "dsl": f'''(function
  {FUNCTION_NAME}
  (call-macro
    Edge Move
    :out (Direction vector)
    :out (Strength float))
  (AddMovementInput
    :worlddirection Direction
    :scalevalue Strength
    :bforce false))''',
    },
]

try:
    normalized_exports = {}

    for variant in VARIANTS:
        entry = {
            "id": variant["id"],
            "dsl": variant["dsl"],
            "validate_success": False,
            "import_success": False,
            "export_success": False,
            "warnings": [],
        }

        validate_result = unreal.BlueprintLispPythonBridge.validate_dsl(variant["dsl"])
        entry["validate_success"] = get_bool(validate_result, "success", "b_success")
        entry["validate_message"] = get_text(validate_result, "message")
        if not entry["validate_success"]:
            raise RuntimeError(f"{variant['id']} DSL 校验失败: {entry['validate_message']}")

        import_result = unreal.BlueprintLispPythonBridge.import_graph_from_text(
            BP_PATH,
            FUNCTION_NAME,
            variant["dsl"],
            REPLACE_GRAPH,
            True,
            True,
        )
        entry["import_success"] = get_bool(import_result, "success", "b_success")
        entry["import_message"] = get_text(import_result, "message")
        entry["warnings"] = [str(x) for x in getattr(import_result, "warnings", [])]
        if not entry["import_success"]:
            raise RuntimeError(f"{variant['id']} 导入失败: {entry['import_message']} | warnings={entry['warnings']}")

        export_result = unreal.BlueprintLispPythonBridge.export_graph_to_text(BP_PATH, FUNCTION_NAME, False, True)
        entry["export_success"] = get_bool(export_result, "success", "b_success")
        entry["export_message"] = get_text(export_result, "message")
        entry["export_text"] = get_text(export_result, "dsl_text")
        entry["normalized_export"] = normalize_export(entry["export_text"])
        entry["contains_call_macro"] = contains_edge_move_macro_call(entry["export_text"])
        entry["contains_add_movement_input"] = "AddMovementInput" in entry["export_text"]
        entry["contains_default_vector"] = '"0, 0, 0"' in entry["export_text"] or '"0.000000,0.000000,0.000000"' in entry["export_text"]
        entry["contains_default_one"] = ':scalevalue 1' in entry["export_text"]
        entry["export_path"] = f"{EXPORT_DIR}\\roundtrip_{variant['id']}_edge_move_export.bplisp"
        save_text(entry["export_path"], entry["export_text"])

        if not entry["export_success"]:
            raise RuntimeError(f"{variant['id']} 导出失败: {entry['export_message']}")
        if not entry["contains_call_macro"]:
            raise RuntimeError(f"{variant['id']} 回导出未发现 Edge Move call-macro")
        if not entry["contains_add_movement_input"]:
            raise RuntimeError(f"{variant['id']} 回导出未发现 AddMovementInput")
        if entry["contains_default_vector"]:
            raise RuntimeError(f"{variant['id']} 回导出把 worlddirection 退化成默认向量")
        if entry["contains_default_one"]:
            raise RuntimeError(f"{variant['id']} 回导出把 scalevalue 退化成默认值 1")
        if entry["normalized_export"] != EXPECTED_NORMALIZED_EXPORT:
            raise RuntimeError(
                f"{variant['id']} 回导出结构不符合预期\n=== actual ===\n{entry['normalized_export']}\n=== expected ===\n{EXPECTED_NORMALIZED_EXPORT}"
            )

        normalized_exports[variant["id"]] = entry["normalized_export"]
        report["variants"].append(entry)

    inline_export = normalized_exports["inline_double_call_macro"]
    alias_export = normalized_exports["standalone_alias_then_use"]
    report["inline_matches_alias_variant"] = inline_export == alias_export
    if not report["inline_matches_alias_variant"]:
        raise RuntimeError("inline_double_call_macro 与 standalone_alias_then_use 的回导出结构不一致")

    report["success"] = True
except Exception as exc:
    report["errors"].append(str(exc))
    report["errors"].append(traceback.format_exc())
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
