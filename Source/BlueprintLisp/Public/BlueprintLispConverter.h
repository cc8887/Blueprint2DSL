// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// BlueprintLispConverter.h - Blueprint <-> BlueprintLisp DSL Conversion API
//
// This is the main public API for converting between UBlueprint EventGraphs
// and the BlueprintLisp S-expression DSL.
//
// Usage example:
//
//   // Export
//   FBlueprintLispConverter::FExportOptions Opts;
//   FBlueprintLispResult Result = FBlueprintLispConverter::Export(Blueprint, TEXT("EventGraph"), Opts);
//   if (Result.bSuccess) UE_LOG(LogTemp, Log, TEXT("%s"), *Result.LispCode);
//
//   // Import
//   FBlueprintLispConverter::FImportOptions ImpOpts;
//   FBlueprintLispResult ImpResult = FBlueprintLispConverter::Import(Blueprint, TEXT("EventGraph"), LispCode, ImpOpts);

#pragma once

#include "CoreMinimal.h"
#include "BlueprintLispAST.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;

// ----------------------------------------------------------------------------------
// Result type
// ----------------------------------------------------------------------------------

struct BLUEPRINTLISP_API FBlueprintLispResult
{
	bool    bSuccess  = false;
	FString Error;
	FString LispCode;                          // Populated by Export
	TArray<FString> Warnings;

	static FBlueprintLispResult Ok(const FString& Code)
	{
		FBlueprintLispResult R;
		R.bSuccess = true;
		R.LispCode = Code;
		return R;
	}
	static FBlueprintLispResult Fail(const FString& Msg)
	{
		FBlueprintLispResult R;
		R.bSuccess = false;
		R.Error    = Msg;
		return R;
	}
};

// ----------------------------------------------------------------------------------
// Main converter
// ----------------------------------------------------------------------------------

/**
 * FBlueprintLispConverter
 *
 * Editor-only utility class for bidirectional Blueprint EventGraph <-> BlueprintLisp DSL conversion.
 * All methods are static; no instance required.
 */
class BLUEPRINTLISP_API FBlueprintLispConverter
{
public:

	// ---------------------------------------------------------------
	// Export  (Blueprint -> DSL)
	// ---------------------------------------------------------------

	struct FExportOptions
	{
		bool bPrettyPrint       = true;
		bool bIncludeComments   = true;
		bool bIncludePositions  = false; // Embed :pos metadata
		bool bStableIds         = true;  // Append :id tags for incremental update
	};

	/**
	 * Export all events in the named graph to BlueprintLisp DSL.
	 * @param Blueprint     The Blueprint asset to read
	 * @param GraphName     Graph to export (e.g. "EventGraph")
	 * @param Options       Export options
	 * @return              FBlueprintLispResult with LispCode on success
	 */
	static FBlueprintLispResult Export(
		UBlueprint*           Blueprint,
		const FString&        GraphName = TEXT("EventGraph"),
		const FExportOptions& Options   = FExportOptions());

	/**
	 * Export a specific UEdGraph directly (e.g. AnimationTransitionGraph).
	 * Use this when the graph is not reachable via Blueprint->FunctionGraphs/UbergraphPages.
	 * @param Graph         The graph to export
	 * @param Options       Export options
	 */
	static FBlueprintLispResult ExportGraph(
		UEdGraph*             Graph,
		const FExportOptions& Options = FExportOptions());

	/** Export a Blueprint loaded from the given asset path */
	static FBlueprintLispResult ExportByPath(
		const FString&        BlueprintPath,
		const FString&        GraphName = TEXT("EventGraph"),
		const FExportOptions& Options   = FExportOptions());

	// ---------------------------------------------------------------
	// Import  (DSL -> Blueprint)
	// ---------------------------------------------------------------

	struct FImportOptions
	{
		bool bClearExisting = false;  // Wipe existing nodes before importing
		bool bAutoLayout    = true;   // Run auto-layout after import
		bool bCompile       = true;   // Compile Blueprint after import
	};

	/**
	 * Import BlueprintLisp DSL into the named graph, creating/wiring nodes.
	 * @param Blueprint     The Blueprint asset to modify
	 * @param GraphName     Target graph
	 * @param LispCode      DSL source code
	 * @param Options       Import options
	 */
	static FBlueprintLispResult Import(
		UBlueprint*           Blueprint,
		const FString&        GraphName,
		const FString&        LispCode,
		const FImportOptions& Options = FImportOptions());

	/**
	 * Import BlueprintLisp DSL into a specific UEdGraph directly.
	 * Use this for graphs not reachable via Blueprint->FunctionGraphs/UbergraphPages
	 * (e.g. AnimationTransitionGraph).
	 * @param Graph         The target graph to write nodes into
	 * @param LispCode      DSL source code
	 * @param Options       Import options
	 */
	static FBlueprintLispResult ImportGraph(
		UEdGraph*             Graph,
		const FString&        LispCode,
		const FImportOptions& Options = FImportOptions());

	/** Import into a Blueprint loaded from an asset path */
	static FBlueprintLispResult ImportByPath(
		const FString&        BlueprintPath,
		const FString&        GraphName,
		const FString&        LispCode,
		const FImportOptions& Options = FImportOptions());

	// ---------------------------------------------------------------
	// Incremental Update  (compute diff and apply only changes)
	// ---------------------------------------------------------------

	struct FUpdateOptions
	{
		bool bAutoLayout    = false;
		bool bCompile       = true;
	};

	/**
	 * Incrementally update an existing EventGraph:
	 *   1. Export current graph to DSL (old AST)
	 *   2. Parse new DSL code (new AST)
	 *   3. Semantic diff via :event-id / :id matching
	 *   4. Apply only additions/removals/modifications
	 */
	static FBlueprintLispResult Update(
		UBlueprint*           Blueprint,
		const FString&        GraphName,
		const FString&        NewLispCode,
		const FUpdateOptions& Options = FUpdateOptions());

	// ---------------------------------------------------------------
	// Validation
	// ---------------------------------------------------------------

	/** Parse and validate without creating any nodes */
	static FBlueprintLispResult Validate(const FString& LispCode);

private:
	// Internal helpers — implemented in BlueprintLispConverter.cpp
	static UEdGraph* FindOrCreateGraph(UBlueprint* BP, const FString& GraphName);
};
