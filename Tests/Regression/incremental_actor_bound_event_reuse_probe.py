import json
import os
import re
import unreal

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR
RESULT_PATH = os.path.join(OUTPUT_DIR, "incremental_actor_bound_event_reuse_probe_result.json")
EXPORT_BEFORE_PATH = os.path.join(OUTPUT_DIR, "incremental_actor_bound_event_reuse_probe_before_update.bplisp")
EXPORT_AFTER_PATH = os.path.join(OUTPUT_DIR, "incremental_actor_bound_event_reuse_probe_after_update.bplisp")

TEST_DIR = "/Game/AnimBP2FP/Phase5"
SOURCE_MAP_ASSET = "/Game/AdvancedLocomotionV4/Levels/ALS_DemoLevel"
TEST_MAP_ASSET = f"{TEST_DIR}/ALS_DemoLevel_ActorBoundEventReuseProbe"
GRAPH_NAME = "EventGraph"
ACTOR_LABEL = "WB_ActorBoundEventReuseProbeTarget"
DELEGATE_NAME = "OnActorBeginOverlap"
TEXT_V1 = "Incremental ActorBoundEvent Reuse V1"
TEXT_V2 = "Incremental ActorBoundEvent Reuse V2"

DSL_V1 = rf'''(actor-bound-event
  :actor "{ACTOR_LABEL}"
  :delegate "{DELEGATE_NAME}"
  (PrintString
    :instring "{TEXT_V1}"
    :bprinttoscreen true
    :bprinttolog true
    :textcolor "(R=0.0,G=1.0,B=0.0,A=1.0)"
    :duration 2
    :key "None"))'''

bridge = unreal.BlueprintLispPythonBridge
asset_lib = unreal.EditorAssetLibrary
report = {
    "success": False,
    "source_map_asset": SOURCE_MAP_ASSET,
    "test_map_asset": TEST_MAP_ASSET,
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


def duplicate_test_map():
    ensure_directory(TEST_DIR)
    if asset_lib.does_asset_exist(TEST_MAP_ASSET):
        asset_lib.delete_asset(TEST_MAP_ASSET)
    if not asset_lib.duplicate_asset(SOURCE_MAP_ASSET, TEST_MAP_ASSET):
        raise RuntimeError(f"复制测试地图失败: {TEST_MAP_ASSET}")


def get_prop(obj, *names):
    for name in names:
        try:
            return obj.get_editor_property(name)
        except Exception:
            pass
    return None


def resolve_level_script_blueprint_path(world_asset, map_asset_path):
    level = get_prop(world_asset, "persistent_level", "PersistentLevel")
    if not level:
        editor_world = unreal.EditorLevelLibrary.get_editor_world()
        if editor_world:
            level = get_prop(editor_world, "persistent_level", "PersistentLevel", "current_level", "CurrentLevel")
    if level and hasattr(level, "get_level_script_blueprint"):
        level_bp = level.get_level_script_blueprint()
        if level_bp:
            return level_bp, level_bp.get_path_name()

    map_name = map_asset_path.split("/")[-1]
    candidate_paths = [
        f"{map_asset_path}.{map_name}:PersistentLevel.{map_name}",
        f"{map_asset_path}.{map_name}:PersistentLevel.LevelScriptBlueprint",
        f"{map_asset_path}.{map_name}:PersistentLevel.{map_name}_LevelScriptBlueprint",
    ]
    for candidate_path in candidate_paths:
        try:
            candidate_obj = unreal.load_object(None, candidate_path)
            if candidate_obj:
                return candidate_obj, candidate_obj.get_path_name()
        except Exception:
            pass

    raise RuntimeError("无法解析 LevelScriptBlueprint 路径")


def ensure_actor_target():
    actors = unreal.EditorLevelLibrary.get_all_level_actors()
    for actor in actors:
        try:
            if actor.get_actor_label() == ACTOR_LABEL:
                return actor
        except Exception:
            pass
    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.Actor, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
    actor.set_actor_label(ACTOR_LABEL)
    return actor


def export_graph(asset_path):
    return bridge.export_graph_to_text(asset_path, GRAPH_NAME, False, True)


def extract_event_id(dsl_text):
    match = re.search(r':event-id\s+"([^"]+)"', dsl_text or "")
    return match.group(1) if match else ""


try:
    duplicate_test_map()
    unreal.EditorLoadingAndSavingUtils.load_map(TEST_MAP_ASSET)
    world_asset = unreal.load_asset(TEST_MAP_ASSET)
    if not world_asset:
        raise RuntimeError(f"加载测试地图失败: {TEST_MAP_ASSET}")

    actor = ensure_actor_target()
    report["actor_label"] = actor.get_actor_label()
    level_bp, level_bp_path = resolve_level_script_blueprint_path(world_asset, TEST_MAP_ASSET)
    report["level_bp_path"] = level_bp_path

    import_result = bridge.import_graph_from_text(level_bp_path, GRAPH_NAME, DSL_V1, REPLACE_GRAPH, True, True)
    report["import_success"] = get_bool(import_result, "success", "b_success")
    report["import_message"] = get_text(import_result, "message")
    report["import_warnings"] = get_list(import_result, "warnings")
    if not report["import_success"]:
        raise RuntimeError(f"首次导入失败: {report['import_message']}")

    export_before = export_graph(level_bp_path)
    export_before_dsl = get_text(export_before, "dsl_text")
    save_text(EXPORT_BEFORE_PATH, export_before_dsl)
    event_id_before = extract_event_id(export_before_dsl)
    report["export_before_success"] = get_bool(export_before, "success", "b_success")
    report["event_id_before"] = event_id_before
    if not report["export_before_success"] or not event_id_before:
        raise RuntimeError("首次导出失败或未找到 actor-bound-event 的 :event-id")

    updated_dsl = export_before_dsl.replace(TEXT_V1, TEXT_V2)
    update_result = bridge.import_graph_from_text(level_bp_path, GRAPH_NAME, updated_dsl, MERGE_APPEND, True, True)
    report["update_success"] = get_bool(update_result, "success", "b_success")
    report["update_message"] = get_text(update_result, "message")
    report["update_warnings"] = get_list(update_result, "warnings")
    report["uses_replacegraph_fallback"] = "ReplaceGraph fallback" in report["update_message"]
    if not report["update_success"]:
        raise RuntimeError(f"update 失败: {report['update_message']}")
    if report["uses_replacegraph_fallback"]:
        raise RuntimeError("update 回退到了 ReplaceGraph")

    export_after = export_graph(level_bp_path)
    export_after_dsl = get_text(export_after, "dsl_text")
    save_text(EXPORT_AFTER_PATH, export_after_dsl)
    event_id_after = extract_event_id(export_after_dsl)
    report["export_after_success"] = get_bool(export_after, "success", "b_success")
    report["event_id_after"] = event_id_after
    report["contains_v2"] = TEXT_V2 in export_after_dsl
    report["contains_v1"] = TEXT_V1 in export_after_dsl
    report["actor_bound_event_reused"] = bool(event_id_before) and event_id_before == event_id_after

    if not report["export_after_success"]:
        raise RuntimeError("update 后导出失败")
    if not report["actor_bound_event_reused"]:
        raise RuntimeError("actor-bound-event 的 :event-id 在 update 前后未保持一致")
    if not report["contains_v2"] or report["contains_v1"]:
        raise RuntimeError("update 后 actor-bound-event probe 内容未按预期更新")

    try:
        unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
    except Exception:
        pass

    report["success"] = True
except Exception as exc:
    report["errors"].append(str(exc))
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False))
