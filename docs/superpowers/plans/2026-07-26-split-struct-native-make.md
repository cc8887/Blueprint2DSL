# Split Struct Native Make Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve split native-make output types while omitting redundant `:out-pin "ReturnValue"` metadata.

**Architecture:** Keep native-make canonicalization for root outputs, but route split child outputs through the existing generic call serializer so its parent type and `break-struct` wrapper are retained. Emit `:out-pin` only for non-default output names; the existing importer fallback continues to resolve omitted output metadata to `ReturnValue`.

**Tech Stack:** Unreal Engine 5.9, C++20, Unreal Automation Tests, UnrealBuildTool, PowerShell

---

### Task 1: Strengthen the regression contract

**Files:**
- Modify: `Source/BlueprintLisp/Private/Tests/BlueprintLispTests.cpp:625`

- [ ] **Step 1: Update the failing assertions**

Replace the combined output/type assertion with independent requirements:

```cpp
TestTrue(TEXT("call result type describes the selected parent output"),
	Exported.LispCode.Contains(TEXT(":result-type-object \"/Script/CoreUObject.Transform\"")));
TestFalse(TEXT("default call output omits redundant out-pin metadata"),
	Exported.LispCode.Contains(TEXT(":out-pin \"ReturnValue\"")));
TestTrue(TEXT("non-default call output keeps out-pin metadata"),
	Exported.LispCode.Contains(TEXT(":out-pin \"Origin\"")));
```

Feed `GetComponentBounds.Origin` into `MakeTransform.Location` so the same graph
also verifies that a non-default output remains explicitly named and survives the
round trip.

- [ ] **Step 2: Build the RED test binary**

Run the complete project Editor-target wrapper with the explicit temporary shared-output exception:

```powershell
& "$env:CODEX_HOME\skills\ue-diagnosing-plugin-build-load\scripts\build-ue-editor-with-plugins.ps1" `
  -Project F:\GASP\GASP.uproject `
  -EngineRoot D:\UnrealEngine `
  -AllowSharedPluginOutputs
```

Expected: build and plugin audit succeed.

- [ ] **Step 3: Run the focused automation test and verify RED**

```powershell
& D:\UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe `
  F:\GASP\GASP.uproject -unattended -nop4 -nosplash -NullRHI `
  '-ExecCmds=Automation RunTests BlueprintLisp.FunctionCall_SplitStructOutputPreservesParentType' `
  '-TestExit=Automation Test Queue Empty' `
  '-AbsLog=F:\GASP\Saved\Logs\BlueprintLisp-SplitStruct-RED-20260726.log'
```

Expected: FAIL because the split native-make path omits `break-struct` and the parent result type.

### Task 2: Implement the minimal exporter correction

**Files:**
- Modify: `Source/BlueprintLisp/Private/BlueprintLispConverter.cpp:787`
- Modify: `Source/BlueprintLisp/Private/BlueprintLispConverter.cpp:875`

- [ ] **Step 1: Restrict native-make canonicalization to root outputs**

```cpp
if (!SourcePin->ParentPin
	&& OutputStructType
	&& OutputStructType->HasMetaData(FBlueprintMetadata::MD_NativeMakeFunction))
```

- [ ] **Step 2: Omit metadata for the conventional default output**

```cpp
if (SelectedCallOutputPin->PinName != UEdGraphSchema_K2::PN_ReturnValue)
{
	Args.Add(FLispNode::MakeKeyword(TEXT(":out-pin")));
	Args.Add(FLispNode::MakeString(SelectedCallOutputPin->PinName.ToString()));
}
```

Keep `:result-type-object` emission independent of `:out-pin` so the parent `Transform` type remains explicit.

- [ ] **Step 3: Inspect the diff**

Run `git diff --check` and confirm only the two exporter decisions and regression assertions changed.

### Task 3: Build and verify GREEN

**Files:**
- Verify: `Saved/PluginBuildState/GASPEditor.json`
- Verify: `Saved/Logs/BlueprintLisp-SplitStruct-GREEN-20260726.log`

- [ ] **Step 1: Build and audit the complete Editor target**

Run the same complete-target wrapper from Task 1. Expected: exit code 0, no plugin dependency warnings, matching receipts/manifests/DLLs, and a current `GASPEditor.json` build-state file.

- [ ] **Step 2: Cold-start the focused regression test**

Run the Task 1 automation command with the GREEN log filename. Expected: one test performed, result `Success`, process exit code 0.

- [ ] **Step 3: Run the BlueprintLisp automation suite**

Use the same command with `Automation RunTests BlueprintLisp` and a unique full-suite log. Expected: all discovered BlueprintLisp tests pass with no test failures.

- [ ] **Step 4: Review final repository state**

Run `git status --short`, `git diff --check`, and `git diff`. Confirm no generated binaries or logs are staged and no unrelated source changes are present.
