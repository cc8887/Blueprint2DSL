# Split Struct Native Make Export Fix

## Problem

When a child pin of a split native-make function output is exported, the native
make canonicalization path returns a `make-struct` form before the generic call
path can preserve the selected child field. For `MakeTransform.ReturnValue.Rotation`,
the exported expression therefore describes the whole `Transform` instead of the
selected `Rotator`.

## Design

Native make canonicalization will apply only when the selected output is a root
pin. When the selected output has a parent pin, export will continue through the
existing generic function-call path. That path records the parent output name and
type, then wraps the call in `break-struct` for the selected child field.

This preserves the existing DSL contract for split outputs:

```lisp
(break-struct
  :struct Transform
  :value (MakeTransform
    :out-pin "ReturnValue"
    :result-type-object "/Script/CoreUObject.Transform"
    ...)
  :field Rotation)
```

Unsplit native-make outputs remain canonicalized as `make-struct`, so the change
does not alter their current representation.

## Scope

The implementation is limited to the native-make selection condition in
`ConvertPureExpressionToLisp`. It does not change the importer, DSL syntax, test
contract, or handling of explicit `UK2Node_MakeStruct` nodes.

## Verification

1. Preserve the existing failing regression test
   `BlueprintLisp.FunctionCall_SplitStructOutputPreservesParentType` as the RED case.
2. Build the complete `GASPEditor` target against the associated UE 5.9 source tree.
3. Audit plugin receipts, module mappings, engine identity, and DLL freshness.
4. Cold-start `UnrealEditor-Cmd` and rerun the focused regression test.
5. Run the relevant BlueprintLisp automation test set to detect regressions.
