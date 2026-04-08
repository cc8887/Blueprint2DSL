// FBlueprintLispMappingRegistry.h - In-memory Blueprint <-> BlueprintLisp DSL Lookup Table
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Singleton registry maintaining an in-memory lookup table for
 * Blueprint <-> BlueprintLisp DSL bidirectional path mapping.
 *
 * Unlike AnimBP2FP (per-asset), BlueprintLisp exports per-graph,
 * so each Blueprint can map to multiple .bplisp files (one per graph).
 *
 * Path convention:
 *   /Game/Path/AssetName -> {Project}/Saved/BP2DSL/BlueprintLisp/Path/AssetName/{GraphName}.bplisp
 *   /MyPlugin/Path/X    -> {Project}/Saved/BP2DSL/BlueprintLisp/Path/X/{GraphName}.bplisp
 *
 * Thread safety: NOT thread-safe. All access must be on the game thread.
 */
class BLUEPRINTLISP_API FBlueprintLispMappingRegistry
{
public:
	/** Get the singleton instance */
	static FBlueprintLispMappingRegistry& Get();

	// ========== Lifecycle ==========

	/** Initialize the registry: scan AssetRegistry for all Blueprints, scan DSL directory, reconcile. */
	void Initialize();

	/** Clear all entries */
	void Reset();

	// ========== Queries ==========

	/**
	 * Find all DSL file paths for a given Blueprint package path.
	 * Returns empty array if not found.
	 */
	TArray<FString> FindDSLFilesByBlueprint(const FString& BlueprintPath) const;

	/**
	 * Resolve a DSL file path back to the Blueprint package path.
	 * Returns empty string if not a valid BlueprintLisp DSL path.
	 */
	FString FindBlueprintByDSLFile(const FString& DSLFilePath) const;

	/** Get all known DSL file paths */
	TArray<FString> GetAllDSLFilePaths() const;

	/** Number of tracked DSL files */
	int32 Num() const { return MappingEntries.Num(); }

	// ========== Path Conversion (Static Helpers) ==========

	/**
	 * Check if a package path belongs to an exportable content root.
	 * Returns true for /Game/, plugin mount points, etc.
	 * Returns false for /Engine/, /Script/, /Temp/, /Transient/.
	 */
	static bool IsExportablePackage(const FString& PackagePath);

	/**
	 * Convert a Blueprint package path + graph name to the default DSL file path.
	 * Convention: /Game/Path/AssetName + "EventGraph"
	 *           -> {Project}/Saved/BP2DSL/BlueprintLisp/Path/AssetName/EventGraph.bplisp
	 *
	 * @param BlueprintPath  Package path, e.g. /Game/Foo/BP_Character
	 * @param GraphName      Graph name, e.g. "EventGraph"
	 * @return Absolute DSL file path, or empty string if invalid
	 */
	static FString BlueprintToDSLPath(
		const FString& BlueprintPath,
		const FString& GraphName);

	/**
	 * Convert a DSL file path back to the Blueprint package path.
	 * Resolution strategy:
	 *   1. Extract directory-based asset path + graph name from the DSL file path.
	 *   2. Query AssetRegistry for a Blueprint matching the asset name.
	 *   3. Unique match -> return real PackagePath (preserving mount point).
	 *   4. No/ambiguous match -> fallback to /Game/ prefix + Warning.
	 *
	 * @param DSLFilePath  Absolute DSL file path
	 * @return Package path, or empty string if not a valid BlueprintLisp DSL path
	 */
	static FString DSLToBlueprintPath(const FString& DSLFilePath);

private:
	// Singleton
	FBlueprintLispMappingRegistry() = default;
	FBlueprintLispMappingRegistry(const FBlueprintLispMappingRegistry&) = delete;
	FBlueprintLispMappingRegistry& operator=(const FBlueprintLispMappingRegistry&) = delete;

	/** Single mapping entry: BlueprintPath <-> DSLFilePath (1:many) */
	struct FMappingEntry
	{
		FString BlueprintPath;
		FString DSLFilePath;
		FString GraphName;
	};

	// Scan all Blueprint assets via AssetRegistry
	void ScanBlueprints();

	// Scan existing .bplisp files from the DSL output directory
	void ScanDSLFiles();

	// ========== Data ==========
	TArray<FMappingEntry> MappingEntries;

	/** Quick lookup: DSLFilePath -> BlueprintPath */
	TMap<FString, FString> DSLFileToBlueprint;

	/** Quick lookup: BlueprintPath -> array of DSLFilePaths */
	TMap<FString, TArray<FString>> BlueprintToDSLFiles;
};
