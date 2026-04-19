import json
import traceback
import unreal
import os


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR

RESULT_PATH = os.path.join(OUTPUT_DIR, "create_edge_scroll_like_blueprint_result.json")
FUNCTION_EXPORT_PATH = os.path.join(OUTPUT_DIR, "bp_player_edge_scroll_like_test_function_export.bplisp")

SOURCE_BP_PATH = "/Game/Blueprint/Core/Player/BP_Player"
TARGET_BP_DIR = "/Game/Blueprint/BPLispTests"
TARGET_BP_NAME = "BP_Player_EdgeScrollLike_Test"
TARGET_BP_PATH = f"{TARGET_BP_DIR}/{TARGET_BP_NAME}"
TARGET_FUNCTION_NAME = "WB_ApplyEdgeMoveLike"

report = {
    "success": False,
    "source_bp": SOURCE_BP_PATH,
    "target_bp": TARGET_BP_PATH,
    "target_function": TARGET_FUNCTION_NAME,
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



def contains_edge_move_macro_call(text):
    if not text:
        return False
    return "call-macro" in text and "Edge Move" in text



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


REPLACE_GRAPH = enum_value(["REPLACE_GRAPH", "ReplaceGraph"])
MERGE_APPEND = enum_value(["MERGE_APPEND", "MergeAppend"])



def ensure_directory(asset_dir):
    if not unreal.EditorAssetLibrary.does_directory_exist(asset_dir):
        unreal.EditorAssetLibrary.make_directory(asset_dir)
        log(f"已创建目录: {asset_dir}")



def duplicate_or_load_target_blueprint():
    ensure_directory(TARGET_BP_DIR)

    existing = unreal.load_asset(TARGET_BP_PATH)
    if existing:
        log(f"复用已存在测试蓝图: {TARGET_BP_PATH}")
        report["created_new_blueprint"] = False
        return existing

    source_bp = unreal.load_asset(SOURCE_BP_PATH)
    if not source_bp:
        raise RuntimeError(f"无法加载参考蓝图: {SOURCE_BP_PATH}")

    duplicated = None
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    try:
        duplicated = asset_tools.duplicate_asset(TARGET_BP_NAME, TARGET_BP_DIR, source_bp)
        if duplicated:
            log(f"通过 AssetTools 复制参考蓝图成功: {TARGET_BP_PATH}")
    except Exception as exc:
        log(f"AssetTools 复制失败，准备尝试 EditorAssetLibrary: {exc}")

    if not duplicated:
        duplicate_ok = unreal.EditorAssetLibrary.duplicate_asset(SOURCE_BP_PATH, TARGET_BP_PATH)
        if not duplicate_ok:
            raise RuntimeError(f"复制参考蓝图失败: {SOURCE_BP_PATH} -> {TARGET_BP_PATH}")
        duplicated = unreal.load_asset(TARGET_BP_PATH)
        if duplicated:
            log(f"通过 EditorAssetLibrary 复制参考蓝图成功: {TARGET_BP_PATH}")

    if not duplicated:
        raise RuntimeError(f"创建目标蓝图失败: {TARGET_BP_PATH}")

    unreal.EditorAssetLibrary.save_loaded_asset(duplicated)
    report["created_new_blueprint"] = True
    return duplicated



def list_graphs(bp_path):
    result = unreal.BlueprintLispPythonBridge.list_graphs(bp_path)
    success = get_result_bool(result, "success", "b_success")
    message = get_result_text(result, "message")
    dsl_text = get_result_text(result, "dsl_text")
    report["list_graphs_success"] = success
    report["list_graphs_message"] = message
    report["list_graphs_text"] = dsl_text
    if not success:
        raise RuntimeError(f"列图失败: {message}")
    return dsl_text



def import_graph_text(bp_path, graph_name, dsl_text, import_mode, tag):
    result = unreal.BlueprintLispPythonBridge.import_graph_from_text(
        bp_path,
        graph_name,
        dsl_text,
        import_mode,
        True,
        True,
    )
    success = get_result_bool(result, "success", "b_success")
    message = get_result_text(result, "message")
    warnings = [str(x) for x in getattr(result, "warnings", [])]
    report[f"import_{tag}_success"] = success
    report[f"import_{tag}_message"] = message
    report[f"import_{tag}_warnings"] = warnings
    if not success:
        raise RuntimeError(f"{tag} 导入失败: {message} | warnings={warnings}")
    log(f"{tag} 导入成功: {message}")



def export_graph_text(bp_path, graph_name, tag):
    result = unreal.BlueprintLispPythonBridge.export_graph_to_text(bp_path, graph_name, False, True)
    success = get_result_bool(result, "success", "b_success")
    message = get_result_text(result, "message")
    dsl_text = get_result_text(result, "dsl_text")
    report[f"export_{tag}_success"] = success
    report[f"export_{tag}_message"] = message
    if not success:
        raise RuntimeError(f"{tag} 导出失败: {message}")
    return dsl_text



def create_function_graph():
    create_dsl = f"(func {TARGET_FUNCTION_NAME})"
    validate_result = unreal.BlueprintLispPythonBridge.validate_dsl(create_dsl)
    report["validate_create_success"] = get_result_bool(validate_result, "success", "b_success")
    report["validate_create_message"] = get_result_text(validate_result, "message")
    if not report["validate_create_success"]:
        raise RuntimeError(f"创建函数 DSL 校验失败: {report['validate_create_message']}")

    import_graph_text(TARGET_BP_PATH, "EventGraph", create_dsl, MERGE_APPEND, "create_function")



def build_function_dsl():
    return f'''(function
  {TARGET_FUNCTION_NAME}
  (call-macro
    Edge Move
    :out (Direction vector)
    :out (Strength float))
  (AddMovementInput
    :worlddirection Direction
    :scalevalue Strength
    :bforce false))'''




def import_function_body():
    dsl_text = build_function_dsl()
    validate_result = unreal.BlueprintLispPythonBridge.validate_dsl(dsl_text)
    report["validate_function_success"] = get_result_bool(validate_result, "success", "b_success")
    report["validate_function_message"] = get_result_text(validate_result, "message")
    if not report["validate_function_success"]:
        raise RuntimeError(f"函数 DSL 校验失败: {report['validate_function_message']}")

    import_graph_text(TARGET_BP_PATH, TARGET_FUNCTION_NAME, dsl_text, REPLACE_GRAPH, "function_body")



def verify_function_export():
    export_text = export_graph_text(TARGET_BP_PATH, TARGET_FUNCTION_NAME, "function")
    save_text(FUNCTION_EXPORT_PATH, export_text)
    report["function_export_path"] = FUNCTION_EXPORT_PATH
    report["function_export_contains_add_movement_input"] = "AddMovementInput" in export_text
    report["function_export_contains_edge_move_macro"] = contains_edge_move_macro_call(export_text)
    report["function_export_text"] = export_text


    if not report["function_export_contains_add_movement_input"]:
        raise RuntimeError("导出结果未发现 AddMovementInput")
    if not report["function_export_contains_edge_move_macro"]:
        raise RuntimeError("导出结果未发现 call-macro Edge Move")

    log(f"已导出并确认新函数 DSL: {FUNCTION_EXPORT_PATH}")



def main():
    bp = duplicate_or_load_target_blueprint()
    unreal.EditorAssetLibrary.save_loaded_asset(bp)

    graph_list_before = list_graphs(TARGET_BP_PATH)
    report["function_exists_before"] = TARGET_FUNCTION_NAME in graph_list_before

    if not report["function_exists_before"]:
        create_function_graph()
    else:
        log(f"目标函数图已存在，直接覆盖函数体: {TARGET_FUNCTION_NAME}")

    graph_list_after_create = list_graphs(TARGET_BP_PATH)
    report["function_exists_after_create"] = TARGET_FUNCTION_NAME in graph_list_after_create
    if not report["function_exists_after_create"]:
        raise RuntimeError(f"创建后仍未发现函数图: {TARGET_FUNCTION_NAME}")

    import_function_body()
    verify_function_export()

    report["success"] = True


try:
    main()
except Exception as exc:
    fail(str(exc))
    fail(traceback.format_exc())
finally:
    save_report()
