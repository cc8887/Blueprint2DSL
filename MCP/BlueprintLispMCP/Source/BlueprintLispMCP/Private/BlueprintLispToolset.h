// BlueprintLispToolset.h - UE 5.8 native MCP Toolset surface

#pragma once

#include "CoreMinimal.h"
#include "BlueprintLispPythonBridge.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "BlueprintLispToolset.generated.h"

/**
 * UE 5.8 Toolset Registry adapter for BlueprintLisp graph operations.
 *
 * The Python bridge remains available for compatibility. This native surface
 * intentionally accepts only asset paths and in-memory DSL text; file-backed
 * bridge methods stay outside the MCP toolset.
 */
UCLASS(BlueprintType, Hidden)
class BLUEPRINTLISPMCP_API UBlueprintLispToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/** Export one named Blueprint graph to BlueprintLisp text. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BlueprintLisp|Read")
	static FBlueprintLispPythonResult ExportBlueprintGraph(
		const FString& AssetPath,
		const FString& GraphName = TEXT("EventGraph"),
		bool bIncludePositions = false,
		bool bStableIds = true);

	/** List the graphs available on one Blueprint asset. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BlueprintLisp|Read")
	static FBlueprintLispPythonResult ListBlueprintGraphs(const FString& AssetPath);

	/** List Blueprint-declared member variables as a structured JSON payload. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BlueprintLisp|Read")
	static FBlueprintLispPythonResult ListBlueprintMemberVariables(const FString& AssetPath);

	/** Inspect one Blueprint member variable as a structured JSON payload. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BlueprintLisp|Read")
	static FBlueprintLispPythonResult InspectBlueprintMemberVariable(
		const FString& AssetPath,
		const FString& VariableName);

	/** Validate BlueprintLisp syntax without loading or mutating an asset. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BlueprintLisp|Read")
	static FBlueprintLispPythonResult ValidateBlueprintLisp(const FString& DSLText);

	/** Replace a named Blueprint graph from in-memory DSL and optionally compile/save. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BlueprintLisp|Write")
	static FBlueprintLispPythonResult ReplaceBlueprintGraph(
		const FString& AssetPath,
		const FString& GraphName,
		const FString& DSLText,
		bool bCompile = true,
		bool bSavePackage = true);

	/** Merge-append a named Blueprint graph using stable DSL IDs where available. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BlueprintLisp|Write")
	static FBlueprintLispPythonResult MergeBlueprintGraph(
		const FString& AssetPath,
		const FString& GraphName,
		const FString& DSLText,
		bool bCompile = true,
		bool bSavePackage = true);

};
