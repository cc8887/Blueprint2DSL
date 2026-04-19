import json
import traceback
import unreal
import os


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR

RESULT_PATH = os.path.join(OUTPUT_DIR, "probe_edge_move_function_variants_result.json")
EXPORT_DIR = OUTPUT_DIR
BP_PATH = "/Game/Blueprint/BPLispTests/BP_Player_EdgeScrollLike_Test"
FUNCTION_NAME = "WB_ApplyEdgeMoveLike"

report = {
    "success": False,
    "bp_path": BP_PATH,
    "function_name": FUNCTION_NAME,
    "variants": [],
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
    {
        "id": "let_direction_single_macro",
        "dsl": f'''(function
  {FUNCTION_NAME}
  (let Direction
    (call-macro
      Edge Move
      :out (Direction vector)
      :out (Strength float)))
  (AddMovementInput
    :worlddirection Direction
    :scalevalue Strength
    :bforce false))''',
    },
    {
        "id": "double_let_double_macro",
        "dsl": f'''(function
  {FUNCTION_NAME}
  (let Direction
    (call-macro
      Edge Move
      :out (Direction vector)
      :out (Strength float)))
  (let Strength
    (call-macro
      Edge Move
      :out (Direction vector)
      :out (Strength float)))
  (AddMovementInput
    :worlddirection Direction
    :scalevalue Strength
    :bforce false))''',
    },
]


def save_text(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def contains_edge_move_macro_call(text):
    if not text:
        return False
    return "call-macro" in text and "Edge Move" in text


best_variant = None


try:
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
            report["variants"].append(entry)
            continue

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
            report["variants"].append(entry)
            continue

        export_result = unreal.BlueprintLispPythonBridge.export_graph_to_text(BP_PATH, FUNCTION_NAME, False, True)
        entry["export_success"] = get_bool(export_result, "success", "b_success")
        entry["export_message"] = get_text(export_result, "message")
        entry["export_text"] = get_text(export_result, "dsl_text")
        entry["contains_call_macro"] = contains_edge_move_macro_call(entry["export_text"])
        entry["contains_direction_symbol"] = "Direction" in entry["export_text"]

        entry["contains_strength_symbol"] = "Strength" in entry["export_text"]
        entry["contains_add_movement_input"] = "AddMovementInput" in entry["export_text"]
        entry["contains_default_vector"] = '"0, 0, 0"' in entry["export_text"] or '"0.000000,0.000000,0.000000"' in entry["export_text"]
        entry["contains_default_one"] = ':scalevalue 1' in entry["export_text"]
        export_path = f"{EXPORT_DIR}\\probe_{variant['id']}_export.bplisp"
        entry["export_path"] = export_path
        save_text(export_path, entry["export_text"])

        if entry["contains_add_movement_input"] and not entry["contains_default_vector"]:
            if best_variant is None:
                best_variant = variant
                entry["selected_as_best"] = True

        report["variants"].append(entry)

    if best_variant is not None:
        final_import = unreal.BlueprintLispPythonBridge.import_graph_from_text(
            BP_PATH,
            FUNCTION_NAME,
            best_variant["dsl"],
            REPLACE_GRAPH,
            True,
            True,
        )
        report["final_variant_id"] = best_variant["id"]
        report["final_import_success"] = get_bool(final_import, "success", "b_success")
        report["final_import_message"] = get_text(final_import, "message")
        final_export = unreal.BlueprintLispPythonBridge.export_graph_to_text(BP_PATH, FUNCTION_NAME, False, True)
        report["final_export_success"] = get_bool(final_export, "success", "b_success")
        report["final_export_message"] = get_text(final_export, "message")
        report["final_export_text"] = get_text(final_export, "dsl_text")
        save_text(os.path.join(OUTPUT_DIR, "bp_player_edge_scroll_like_test_function_export.bplisp"), report["final_export_text"])

    report["success"] = True
except Exception as exc:
    report["errors"].append(str(exc))
    report["errors"].append(traceback.format_exc())
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
