import json
import os
import unreal

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RESULT_PATH = os.path.join(SCRIPT_DIR, "import_var_declaration_probe_result.json")
EXPORT_AFTER_CREATE_PATH = os.path.join(SCRIPT_DIR, "import_var_declaration_probe_after_create.bplisp")
EXPORT_AFTER_MISMATCH_PATH = os.path.join(SCRIPT_DIR, "import_var_declaration_probe_after_mismatch.bplisp")

TEST_DIR = "/Game/Blueprint/BPLispTests"
TEST_ASSET = f"{TEST_DIR}/BP_VarDeclarationProbe"
GRAPH_NAME = "EventGraph"
VARIABLE_NAME = "AutoCreatedHealth"
DEFAULT_VALUE = 42.0
SET_VALUE = 99

bridge = unreal.BlueprintLispPythonBridge
asset_lib = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
report = {
    "success": False,
    "test_asset": TEST_ASSET,
    "graph_name": GRAPH_NAME,
    "variable_name": VARIABLE_NAME,
    "errors": [],
}


def build_create_dsl():
    return rf'''(var {VARIABLE_NAME} float :default {DEFAULT_VALUE:g} :expose-on-spawn true)
(event
  ReceiveBeginPlay
  (set {VARIABLE_NAME} {SET_VALUE}))'''


def build_mismatch_dsl():
    return rf'''(var {VARIABLE_NAME} bool :default true :expose-on-spawn false)
(event
  ReceiveBeginPlay
  (set {VARIABLE_NAME} true))'''


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


def import_graph(dsl_text):
    return bridge.import_graph_from_text(TEST_ASSET, GRAPH_NAME, dsl_text, REPLACE_GRAPH, True, True)


def export_graph():
    return bridge.export_graph_to_text(TEST_ASSET, GRAPH_NAME, False, True)


def contains_set_literal(dsl_text, variable_name, value_text):
    normalized = " ".join((dsl_text or "").split())
    target = f"(set {variable_name} {value_text}"
    return target in normalized


def load_blueprint_asset():
    blueprint = asset_lib.load_asset(TEST_ASSET)
    if not blueprint:
        raise RuntimeError(f"无法加载测试蓝图: {TEST_ASSET}")
    return blueprint


def safe_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def try_get_generated_default_value(blueprint, variable_name):
    try:
        generated_class = blueprint.generated_class()
        if not generated_class:
            return None
        default_object = unreal.get_default_object(generated_class)
        if not default_object:
            return None
        return default_object.get_editor_property(variable_name)
    except Exception:
        return None


def inspect_variable_state(stage_name):
    inspect_result = bridge.inspect_member_variable(TEST_ASSET, VARIABLE_NAME)
    report[f"inspect_success_{stage_name}"] = get_bool(inspect_result, "success", "b_success")
    report[f"inspect_message_{stage_name}"] = get_text(inspect_result, "message")
    report[f"inspect_warnings_{stage_name}"] = get_list(inspect_result, "warnings")
    payload_text = get_text(inspect_result, "dsl_text")
    report[f"inspect_payload_text_{stage_name}"] = payload_text
    if not report[f"inspect_success_{stage_name}"]:
        return None
    try:
        payload = json.loads(payload_text or "{}")
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"变量检查结果不是合法 JSON ({stage_name}): {exc}")
    report[f"inspect_payload_{stage_name}"] = payload
    return payload


def list_variable_state(stage_name):
    list_result = bridge.list_member_variables(TEST_ASSET)
    report[f"list_success_{stage_name}"] = get_bool(list_result, "success", "b_success")
    report[f"list_message_{stage_name}"] = get_text(list_result, "message")
    report[f"list_warnings_{stage_name}"] = get_list(list_result, "warnings")
    payload_text = get_text(list_result, "dsl_text")
    report[f"list_payload_text_{stage_name}"] = payload_text
    if not report[f"list_success_{stage_name}"]:
        return None
    try:
        payload = json.loads(payload_text or "{}")
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"变量列表结果不是合法 JSON ({stage_name}): {exc}")
    report[f"list_payload_{stage_name}"] = payload
    for variable_payload in payload.get("variables", []) or []:
        if str(variable_payload.get("variable_name", "")) == VARIABLE_NAME:
            report[f"list_variable_payload_{stage_name}"] = variable_payload
            return variable_payload
    report[f"list_variable_payload_{stage_name}"] = None
    return None


def capture_variable_state(blueprint, stage_name):
    payload = inspect_variable_state(stage_name)
    list_payload = list_variable_state(stage_name)
    report[f"variable_found_{stage_name}"] = bool(payload)
    report[f"variable_list_found_{stage_name}"] = bool(list_payload)
    if not payload:
        return None

    default_value_text = str(payload.get("default_value", "") or "")
    generated_default_value = try_get_generated_default_value(blueprint, VARIABLE_NAME)

    report[f"variable_default_value_text_{stage_name}"] = default_value_text
    report[f"variable_owner_blueprint_path_{stage_name}"] = str(payload.get("owner_blueprint_path", "") or "")
    report[f"declared_on_target_blueprint_{stage_name}"] = bool(payload.get("declared_on_target_blueprint", False))
    report[f"has_expose_on_spawn_{stage_name}"] = bool(payload.get("has_expose_on_spawn", False))
    report[f"is_instance_editable_{stage_name}"] = bool((list_payload or {}).get("is_instance_editable", False))
    report[f"list_has_expose_on_spawn_{stage_name}"] = bool((list_payload or {}).get("has_expose_on_spawn", False))
    report[f"generated_default_value_{stage_name}"] = generated_default_value

    default_value_number = safe_float(default_value_text)
    generated_default_number = safe_float(generated_default_value)

    if generated_default_value is None:
        report[f"generated_default_matches_{stage_name}"] = None
    else:
        report[f"generated_default_matches_{stage_name}"] = generated_default_number is not None and abs(generated_default_number - DEFAULT_VALUE) < 1e-6

    report[f"default_value_matches_{stage_name}"] = (
        report[f"generated_default_matches_{stage_name}"] is True
        or (
            generated_default_value is None
            and default_value_number is not None
            and abs(default_value_number - DEFAULT_VALUE) < 1e-6
        )
    )

    return {
        "default_value_text": default_value_text,
        "generated_default_value": generated_default_value,
        "payload": payload,
        "list_payload": list_payload,
    }


try:
    recreate_character_bp(TEST_ASSET)

    create_result = import_graph(build_create_dsl())
    report["create_success"] = get_bool(create_result, "success", "b_success")
    report["create_message"] = get_text(create_result, "message")
    report["create_warnings"] = get_list(create_result, "warnings")
    if not report["create_success"]:
        raise RuntimeError(f"变量创建导入失败: {report['create_message']}")

    export_after_create = export_graph()
    export_after_create_dsl = get_text(export_after_create, "dsl_text")
    save_text(EXPORT_AFTER_CREATE_PATH, export_after_create_dsl)
    report["export_after_create_success"] = get_bool(export_after_create, "success", "b_success")
    report["contains_variable_name_after_create"] = VARIABLE_NAME in export_after_create_dsl
    report["contains_numeric_assignment_after_create"] = contains_set_literal(export_after_create_dsl, VARIABLE_NAME, str(SET_VALUE))
    if not report["export_after_create_success"]:
        raise RuntimeError("变量创建后导出失败")
    if not report["contains_numeric_assignment_after_create"]:
        raise RuntimeError("变量创建后导出结果未保留数值赋值")

    blueprint_after_create = load_blueprint_asset()
    variable_state_after_create = capture_variable_state(blueprint_after_create, "after_create")
    if not variable_state_after_create:
        raise RuntimeError("变量创建后未找到可检查的成员变量状态")
    if not report["declared_on_target_blueprint_after_create"]:
        raise RuntimeError(f"变量创建后 owner blueprint 不符合预期: {report['inspect_payload_after_create']}")
    if not report["variable_list_found_after_create"]:
        raise RuntimeError("变量创建后未在成员变量列表中找到目标变量")
    if not report["default_value_matches_after_create"]:
        raise RuntimeError(f"变量创建后默认值不符合预期: {report['variable_default_value_text_after_create']}")
    if not report["has_expose_on_spawn_after_create"] or not report["list_has_expose_on_spawn_after_create"]:
        raise RuntimeError(f"变量创建后缺少 ExposeOnSpawn 元数据: {report['inspect_payload_after_create']}")
    if not report["is_instance_editable_after_create"]:
        raise RuntimeError(f"变量创建后未标记为 Instance Editable: {report['list_variable_payload_after_create']}")
    if report["generated_default_matches_after_create"] is False:
        raise RuntimeError(f"变量创建后生成类默认值不符合预期: {report['generated_default_value_after_create']}")

    mismatch_result = import_graph(build_mismatch_dsl())
    report["mismatch_success"] = get_bool(mismatch_result, "success", "b_success")
    report["mismatch_message"] = get_text(mismatch_result, "message")
    report["mismatch_warnings"] = get_list(mismatch_result, "warnings")
    report["mismatch_rejected"] = not report["mismatch_success"] and "already exists with type" in report["mismatch_message"]
    if report["mismatch_success"]:
        raise RuntimeError("变量类型冲突导入本应失败，但实际成功")
    if not report["mismatch_rejected"]:
        raise RuntimeError(f"变量类型冲突失败信息不符合预期: {report['mismatch_message']}")

    export_after_mismatch = export_graph()
    export_after_mismatch_dsl = get_text(export_after_mismatch, "dsl_text")
    save_text(EXPORT_AFTER_MISMATCH_PATH, export_after_mismatch_dsl)
    report["export_after_mismatch_success"] = get_bool(export_after_mismatch, "success", "b_success")
    report["still_contains_numeric_assignment"] = contains_set_literal(export_after_mismatch_dsl, VARIABLE_NAME, str(SET_VALUE))
    report["contains_bool_assignment"] = contains_set_literal(export_after_mismatch_dsl, VARIABLE_NAME, "true")
    if not report["export_after_mismatch_success"]:
        raise RuntimeError("类型冲突失败后重新导出失败")
    if not report["still_contains_numeric_assignment"] or report["contains_bool_assignment"]:
        raise RuntimeError("类型冲突失败后图内容未保持创建成功后的状态")

    blueprint_after_mismatch = load_blueprint_asset()
    variable_state_after_mismatch = capture_variable_state(blueprint_after_mismatch, "after_mismatch")
    if not variable_state_after_mismatch:
        raise RuntimeError("类型冲突失败后变量定义丢失")
    if not report["declared_on_target_blueprint_after_mismatch"]:
        raise RuntimeError(f"类型冲突失败后 owner blueprint 被破坏: {report['inspect_payload_after_mismatch']}")
    if not report["variable_list_found_after_mismatch"]:
        raise RuntimeError("类型冲突失败后未在成员变量列表中找到目标变量")
    if not report["default_value_matches_after_mismatch"]:
        raise RuntimeError(f"类型冲突失败后默认值被破坏: {report['variable_default_value_text_after_mismatch']}")
    if not report["has_expose_on_spawn_after_mismatch"] or not report["list_has_expose_on_spawn_after_mismatch"]:
        raise RuntimeError(f"类型冲突失败后 ExposeOnSpawn 元数据被破坏: {report['inspect_payload_after_mismatch']}")
    if not report["is_instance_editable_after_mismatch"]:
        raise RuntimeError(f"类型冲突失败后 Instance Editable 被破坏: {report['list_variable_payload_after_mismatch']}")
    if report["generated_default_matches_after_mismatch"] is False:
        raise RuntimeError(f"类型冲突失败后生成类默认值被破坏: {report['generated_default_value_after_mismatch']}")

    report["success"] = True
except Exception as exc:
    report["errors"].append(str(exc))
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False))
