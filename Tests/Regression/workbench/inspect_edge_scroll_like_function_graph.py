import json
import traceback
import unreal
import os


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = SCRIPT_DIR

RESULT_PATH = os.path.join(OUTPUT_DIR, "inspect_edge_scroll_like_function_graph_result.json")
BP_PATH = "/Game/Blueprint/BPLispTests/BP_Player_EdgeScrollLike_Test"
GRAPH_NAME = "WB_ApplyEdgeMoveLike"

report = {
    "success": False,
    "bp_path": BP_PATH,
    "graph_name": GRAPH_NAME,
    "nodes": [],
    "errors": [],
}


def pin_info(pin):
    linked = []
    try:
        for linked_pin in list(pin.get_editor_property("linked_to")):
            owner = linked_pin.get_outer()
            linked.append({
                "pin_name": str(linked_pin.get_name()),
                "node_title": str(owner.get_node_title(unreal.NodeTitleType.LIST_VIEW)),
                "node_class": owner.get_class().get_name(),
            })
    except Exception as exc:
        linked.append({"error": str(exc)})

    pin_type = None
    try:
        pin_type = pin.get_editor_property("pin_type")
    except Exception:
        pin_type = None

    return {
        "pin_name": str(pin.get_name()),
        "direction": str(getattr(pin, "direction", "<unknown>")),
        "category": str(getattr(pin_type, "pin_category", "<unknown>")),
        "subcategory": str(getattr(pin_type, "pin_sub_category", "<unknown>")),
        "subcategory_object": str(getattr(pin_type, "pin_sub_category_object", None)),
        "default_value": str(getattr(pin, "default_value", "")),
        "linked_to": linked,
    }


try:
    bp = unreal.load_asset(BP_PATH)
    if not bp:
        raise RuntimeError(f"加载蓝图失败: {BP_PATH}")

    target_graph = unreal.BlueprintEditorLibrary.find_graph(bp, GRAPH_NAME)
    if not target_graph:
        raise RuntimeError(f"未找到函数图: {GRAPH_NAME}")

    graph_nodes = list(target_graph.get_editor_property("nodes"))
    for node in graph_nodes:
        pins = []
        try:
            pins = list(node.get_editor_property("pins"))
        except Exception:
            pins = []
        report["nodes"].append({
            "node_title": str(node.get_node_title(unreal.NodeTitleType.LIST_VIEW)),
            "node_class": node.get_class().get_name(),
            "pins": [pin_info(pin) for pin in pins],
        })

    report["success"] = True
except Exception as exc:
    report["errors"].append(str(exc))
    report["errors"].append(traceback.format_exc())
finally:
    with open(RESULT_PATH, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
