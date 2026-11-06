# Legacy graph fidelity and explicit migrations

`BlueprintLisp.Maintenance` adds minimal native reproductions for failures found
in already-imported assets. These run in transient Blueprints and do not require
the original game, its components, or its data tables.

```powershell
python Tests/Regression/run_semantic_regression.py `
  --editor '<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' `
  --project '<host>/Host.uproject' `
  --output '<evidence outside the host project>/maintenance' `
  --filter BlueprintLisp.Maintenance
```

Use a new output directory for each run. Keep evidence outside directories that
other jobs might delete, including a host's Saved directory during cleanup.

The tests check observable values and importer failure boundaries:

- Legacy quoted arrays such as `"[0, 0x11, -3]"` become typed MakeArray nodes.
  Runtime values remain `[0, 17, -3]`.
- Byte enum arrays use actual enumerator names; ObjectTypeQuery values 0 and 17
  become ObjectTypeQuery1 and ObjectTypeQuery18, preserving their values.
- Malformed arrays are rejected before disconnecting the previous value.
- Missing writable reference inputs fail even with compilation disabled.
  AutoCreateRefTerm and const-reference inputs retain their engine behavior.
- An untyped generic GetDataTableRowFromName call fails with guidance to use
  `get-data-table-row`; a missing table/row type is not silently substituted.
- Export rejects reads from impure calls without an execution input.
- Explicitly scheduling a missing call preserves its return value, bool and
  array outputs, supplied by exactly one execution per consumer.

## Maintenance API

`unreal.BlueprintLispGraphMaintenance` requires UE 5.8 or later and operates in memory.
Older engines return an unsupported result without changing assets. Methods do not save
packages or compile Blueprints. Callers must back up existing files, inspect the
returned result, compile, and explicitly save only reviewed changes. Most
methods can partially change a graph on failure; discard that in-memory asset
and reload its backup rather than saving a failed operation.

- `inspect_blueprint(bp)` returns JSON containing native member/function
  references, actual pin types, defaults, links and animation bindings. This
  includes nodes whose missing fields no longer have usable pins.
- `repair_graph(bp, graph_name, remove_isolated_missing_variables=False)` expands
  valid primitive-array defaults, removes safe obsolete float conversions,
  exposes hidden bound animation inputs, and removes redundant orphan pins only
  when the corresponding live pin retains the same default. It reports every
  change. Enabling the last argument additionally removes missing-variable
  artifacts only when every execution/data pin is already disconnected. This
  cleanup does **not** reconstruct their original lost behavior.
- `set_pin_expression(...)` replaces one input through the checked expression
  importer. It can build explicit external getters, arrays and typed structures.
- `schedule_call_before_consumer(...)` is an explicit execution-order migration.
  It copies a call and any affected pure function expressions for one specified
  consumer. This avoids sharing an unexecuted frame output between independent
  event paths. It refuses unsupported data slices or ambiguous predecessors;
  it does not choose where arbitrary side effects should run.
- `bind_delegate_input(...)` binds an explicitly named self callback after
  checking its signature.
- `set_member_real_precision(...)`, `set_function_object_output_class(...)` and
  `set_function_class_input(...)` migrate explicit declarations. Use original
  metadata as evidence; a warning alone is not enough to choose a type.
- `create_data_table(struct_path, table_path, fields_json, rows_json)` creates a
  missing class-field row struct and table from explicit data. The current schema
  accepts fields of the form `{"name":"Widget","class":"/Script/UMG.UserWidget"}`.
  It refuses existing packages and does not create substitute classes for missing
  references. Save the returned assets separately after validating their data.

Float migration preserves precision boundaries. A double-to-float node is only
bypassed when each destination still requires float. Float-to-double widening is
only bypassed when its actual input is float; an incorrectly widened variable
must first be restored from authoritative metadata. A double wire feeding a
float input and then widening again can have observable rounding behavior.

Typed DataTable support and maintenance tests are separate from production
asset migration. Passing fixture tests does not rewrite saved legacy nodes or
recover missing UI/component implementations. Cold-load saved migrations in a
new editor process, and retain remaining asset/behavior gaps in the delivery
record.
