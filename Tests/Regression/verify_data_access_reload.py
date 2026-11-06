"""Fresh Editor-process verification after DataAccess tests with --save-fixtures.

BLUEPRINTLISP_FIXTURE_MANIFEST selects fixtures.json from a successful run.
BLUEPRINTLISP_RELOAD_RESULT optionally selects this verification's output JSON.
Only the dedicated test assets are loaded; no Blueprint is modified or saved.
"""
import json
import os
import pathlib
import unreal

manifest_path = os.environ.get("BLUEPRINTLISP_FIXTURE_MANIFEST")
if not manifest_path:
    raise RuntimeError("Set BLUEPRINTLISP_FIXTURE_MANIFEST to a successful runner's fixtures.json")
fixtures = json.loads(pathlib.Path(manifest_path).read_text(encoding="utf-8"))
report = {"success": False, "checks": [], "errors": []}


def check(label, condition):
    report["checks"].append({"name": label, "pass": bool(condition)})
    if not condition:
        report["errors"].append(label)


def load_class(name, token):
    path = fixtures[name]
    blueprint = unreal.load_asset(path)
    check(name + " loads", blueprint is not None)
    cls = unreal.load_class(None, path + "." + name + "_C")
    check(name + " generated class loads", cls is not None)
    exported = unreal.BlueprintLispPythonBridge.export_graph_to_text(path, "Run", False, True)
    check(name + " re-exports", exported.success)
    check(name + " preserves its data access form", token in exported.dsl_text)
    return cls


for name, token in [("BP_BL_Map", "Map_Find"), ("BP_BL_MapStruct", "Map_Add")]:
    try:
        obj = unreal.new_object(load_class(name, token))
        obj.call_method("Run")
        if name == "BP_BL_Map":
            check("zero is found, absent key is not, overwrite is found", list(obj.get_editor_property("lookup_found")) == [True, False, True])
            check("zero, missing default and overwrite values survive", list(obj.get_editor_property("lookup_values")) == [0, 0, 37])
            check("Map_Add changes the original Map", obj.get_editor_property("int_map").get("Zero") == 37)
        else:
            check("struct Map retains the authored key", "ExactKey" in obj.get_editor_property("row_map"))
            check("struct Map retains both fields", obj.get_editor_property("row_count") == 23 and obj.get_editor_property("row_weight") == 2.5)
    except Exception as exc:
        report["errors"].append(name + ": " + str(exc))

try:
    table = unreal.load_asset(fixtures["DT_BL_Rows"])
    check("DataTable asset reloads", table is not None)
    check("DataTable row struct reloads", table.get_editor_property("row_struct").get_name() == "BlueprintLispDataAccessRow")
    for name in ["BP_BL_DataTable", "BP_BL_DataTableDynamic", "BP_BL_DataTableGate"]:
        cls = load_class(name, "get-data-table-row")
        for row in ["Existing", "Other", "Missing"]:
            obj = unreal.new_object(cls)
            obj.set_editor_property("table", table)
            obj.set_editor_property("requested_row", row)
            obj.call_method("Run")
            if name == "BP_BL_DataTableGate":
                check(name + " branch " + row, (obj.get_editor_property("missing_paths") == 1) if row == "Missing"
                      else list(obj.get_editor_property("lookup_values")) == [5])
            else:
                present = row != "Missing"
                check(name + " found branch " + row, obj.get_editor_property("found_paths") == int(present))
                check(name + " missing branch " + row, obj.get_editor_property("missing_paths") == int(not present))
                if present:
                    check(name + " Count " + row, obj.get_editor_property("row_count") == (0 if row == "Existing" else 42))
                    check(name + " Weight " + row, obj.get_editor_property("row_weight") == (4.75 if row == "Existing" else 9.25))
except Exception as exc:
    report["errors"].append("DataTable reload: " + str(exc))

report["success"] = not report["errors"]
path = pathlib.Path(os.environ.get("BLUEPRINTLISP_RELOAD_RESULT") or
                    str(pathlib.Path(unreal.Paths.project_saved_dir()) / "BlueprintLispDataAccessReload.json"))
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text(json.dumps(report, indent=2), encoding="utf-8")
unreal.log("BlueprintLisp data access reload: " + json.dumps(report))
if not report["success"]:
    raise RuntimeError("Data access reload failed: " + "; ".join(report["errors"]))
