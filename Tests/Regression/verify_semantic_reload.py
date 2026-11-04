"""Run with UE's PythonScript commandlet after --save-fixtures, in a NEW process.

Optional BLUEPRINTLISP_RELOAD_RESULT selects the JSON report path. Reads only
the dedicated test assets; never writes or compiles production Blueprints.
"""
import json
import os
import pathlib
import unreal

report = {"success": False, "checks": [], "errors": []}
native = unreal.BlueprintLispSemanticTestObject
bridge = unreal.BlueprintLispPythonBridge
manifest_path = os.environ.get("BLUEPRINTLISP_FIXTURE_MANIFEST")
if not manifest_path:
    raise RuntimeError("Set BLUEPRINTLISP_FIXTURE_MANIFEST to the successful runner's fixtures.json")
fixtures = json.loads(pathlib.Path(manifest_path).read_text(encoding="utf-8"))


def check(label, condition):
    report["checks"].append({"name": label, "pass": bool(condition)})
    if not condition:
        report["errors"].append(label)


def load_fixture(name, token):
    path = fixtures[name]
    blueprint = unreal.load_asset(path)
    check(name + " loads from disk", blueprint is not None)
    cls = unreal.load_class(None, path + "." + name + "_C")
    check(name + " generated class loads", cls is not None)
    exported = bridge.export_graph_to_text(path, "Run", False, True)
    check(name + " re-exports after reload", exported.success)
    check(name + " retains semantic DSL", token in exported.dsl_text)
    return unreal.new_object(cls)


for name, token, kind in [
    ("BP_BL_External", "get-member", "external"),
    ("BP_BL_Class", "class</Script/BlueprintLisp.BlueprintLispSemanticTestObject>", "class"),
    ("BP_BL_Outputs", "node-output", "outputs"),
]:
    try:
        obj = load_fixture(name, token)
        if kind == "external":
            other = unreal.new_object(native)
            other.set_editor_property("value", 91.25)
            obj.set_editor_property("other", other)
            obj.call_method("Run")
            check("reloaded member getter reads external target", obj.get_editor_property("observed_scalar") == 91.25)
        elif kind == "class":
            expected = native.static_class()
            actual = obj.call_method("Run", args=(expected,))
            check("reloaded class parameter and return execute", actual == expected)
        else:
            obj.call_method("Run")
            check("reloaded calls execute exactly twice", obj.get_editor_property("invocation_count") == 2)
            check("reloaded return comes from first call", obj.get_editor_property("observed_number") == 6)
            check("reloaded bool comes from second call", not obj.get_editor_property("observed_positive"))
            check("reloaded array comes from first call", list(obj.get_editor_property("observed_values")) == [3, 4])
    except Exception as exc:
        report["errors"].append(name + ": " + str(exc))

report["success"] = not report["errors"]
result_path = pathlib.Path(os.environ.get("BLUEPRINTLISP_RELOAD_RESULT") or
                           str(pathlib.Path(unreal.Paths.project_saved_dir()) / "BlueprintLispSemanticReload.json"))
result_path.parent.mkdir(parents=True, exist_ok=True)
result_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
unreal.log("BlueprintLisp semantic reload: " + json.dumps(report))
if not report["success"]:
    raise RuntimeError("Semantic reload failed: " + "; ".join(report["errors"]))
