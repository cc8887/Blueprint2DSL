// BlueprintLispPythonBridge.h - Python-facing editor bridge for BlueprintLisp
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
//
// Exposes FBlueprintLispConverter to Unreal Python so AI agents can:
//   - Export any Blueprint EventGraph (or other graph) to BlueprintLisp DSL text
//   - Import BlueprintLisp DSL text back into a Blueprint graph
//   - Incrementally update an existing graph (semantic diff + apply)
//   - Validate DSL syntax without touching any assets
//
// This works for ANY Blueprint (not just AnimBlueprint).
// AnimBlueprint users can also use unreal.AnimBP2FPPythonBridge.export_event_graph_to_text,
// but this bridge is the primary entry point for general Blueprint graph work.
//
// Python usage:
//   import unreal
//   result = unreal.BlueprintLispPythonBridge.export_graph_to_text(
//       "/Game/Foo/BP_Character.BP_Character", "EventGraph")
//   print(result.dsl_text)

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BlueprintLispPythonBridge.generated.h"

/**
 * Structured result returned to Unreal Python / Blueprint callers.
 */
USTRUCT(BlueprintType)
struct BLUEPRINTLISP_API FBlueprintLispPythonResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="BlueprintLisp")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category="BlueprintLisp")
	FString Message;

	/** Resolved object path of the Blueprint that was operated on. */
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLisp")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category="BlueprintLisp")
	FString FilePath;

	/** The BlueprintLisp DSL text (populated by Export/Update operations). */
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLisp")
	FString DSLText;

	UPROPERTY(BlueprintReadOnly, Category="BlueprintLisp")
	bool bSavedPackage = false;

	UPROPERTY(BlueprintReadOnly, Category="BlueprintLisp")
	TArray<FString> Warnings;
};

/**
 * Editor-only bridge exposed to Unreal Python so AI agents can read/write
 * any Blueprint's EventGraph (or other named graph) via BlueprintLisp DSL.
 */
UCLASS()
class BLUEPRINTLISP_API UBlueprintLispPythonBridge : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ---------------------------------------------------------------
	// Export  (Blueprint graph -> DSL text)
	// ---------------------------------------------------------------

	/**
	 * Export the named graph of any Blueprint asset to BlueprintLisp DSL text.
	 * @param BlueprintPath  Package or object path, e.g. /Game/Foo/BP_Character or /Game/Foo/BP_Character.BP_Character
	 * @param GraphName      Graph to export, e.g. "EventGraph" (default)
	 * @param bIncludePositions  Embed :pos metadata for editor node positions
	 * @param bStableIds         Append :id tags for incremental diff/update
	 */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Python")
	static FBlueprintLispPythonResult ExportGraphToText(
		const FString& BlueprintPath,
		const FString& GraphName = TEXT("EventGraph"),
		bool bIncludePositions = false,
		bool bStableIds = true);

	/**
	 * Export the named graph to a .bplisp file.
	 */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Python")
	static FBlueprintLispPythonResult ExportGraphToFile(
		const FString& BlueprintPath,
		const FString& OutputFilePath,
		const FString& GraphName = TEXT("EventGraph"),
		bool bIncludePositions = false,
		bool bStableIds = true);

	/**
	 * Export the named graph to the default .bplisp file path.
	 * Path convention: {Project}/Saved/BP2DSL/BlueprintLisp/{RelPath}/{GraphName}.bplisp
	 * e.g. /Game/Props/BP_Door + "EventGraph"
	 *   -> Saved/BP2DSL/BlueprintLisp/Props/BP_Door/EventGraph.bplisp
	 * Creates intermediate directories as needed.
	 */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Python")
	static FBlueprintLispPythonResult ExportGraphToDefaultPath(
		const FString& BlueprintPath,
		const FString& GraphName = TEXT("EventGraph"),
		bool bIncludePositions = false,
		bool bStableIds = true);

	// ---------------------------------------------------------------
	// Import  (DSL text -> Blueprint graph)
	// ---------------------------------------------------------------

	/**
	 * Import BlueprintLisp DSL text into the named graph of a Blueprint.
	 * By default clears existing nodes first and compiles after import.
	 * @param bClearExisting  Wipe existing graph nodes before importing
	 * @param bCompile        Compile Blueprint after import
	 * @param bSavePackage    Save package to disk after import
	 */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Python")
	static FBlueprintLispPythonResult ImportGraphFromText(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& DSLText,
		bool bClearExisting = false,
		bool bCompile = true,
		bool bSavePackage = true);

	/**
	 * Import a .bplisp file into the named graph of a Blueprint.
	 */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Python")
	static FBlueprintLispPythonResult ImportGraphFromFile(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& InputFilePath,
		bool bClearExisting = false,
		bool bCompile = true,
		bool bSavePackage = true);

	// ---------------------------------------------------------------
	// Incremental Update  (semantic diff + apply)
	// ---------------------------------------------------------------

	/**
	 * Incrementally update an existing Blueprint graph:
	 *   1. Export current graph (old AST)
	 *   2. Parse new DSL (new AST)
	 *   3. Semantic diff via :event-id / :id tags
	 *   4. Apply only additions/removals/modifications
	 * @param bCompile    Compile Blueprint after update
	 * @param bSavePackage Save package to disk after update
	 */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Python")
	static FBlueprintLispPythonResult UpdateGraphFromText(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& NewDSLText,
		bool bCompile = true,
		bool bSavePackage = true);

	/**
	 * Incrementally update a Blueprint graph from a .bplisp file.
	 */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Python")
	static FBlueprintLispPythonResult UpdateGraphFromFile(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& InputFilePath,
		bool bCompile = true,
		bool bSavePackage = true);

	// ---------------------------------------------------------------
	// Query
	// ---------------------------------------------------------------

	/**
	 * List all graph names in a Blueprint.
	 * Returns graph names in DSLText, one per line, prefixed with type:
	 *   [Ubergraph] EventGraph
	 *   [Function] FuncName
	 *   [Macro] MacroName
	 */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Python")
	static FBlueprintLispPythonResult ListGraphs(const FString& BlueprintPath);

	// ---------------------------------------------------------------
	// Validation
	// ---------------------------------------------------------------

	/**
	 * Parse and validate DSL syntax without touching any Blueprint asset.
	 * Returns bSuccess=true if the DSL is syntactically valid.
	 */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Python")
	static FBlueprintLispPythonResult ValidateDSL(const FString& DSLText);
};
