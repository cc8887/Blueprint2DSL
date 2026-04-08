// FBlueprintLispMappingRegistry.cpp - In-memory Blueprint <-> BlueprintLisp DSL Lookup Table
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "FBlueprintLispMappingRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/Blueprint.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"

// ========== Singleton ==========

FBlueprintLispMappingRegistry& FBlueprintLispMappingRegistry::Get()
{
	static FBlueprintLispMappingRegistry Instance;
	return Instance;
}

// ========== Lifecycle ==========

void FBlueprintLispMappingRegistry::Initialize()
{
	Reset();

	UE_LOG(LogTemp, Log, TEXT("BlueprintLispMappingRegistry: Initializing..."));

	// Phase 1: Scan blueprints
	ScanBlueprints();

	// Phase 2: Scan existing DSL files
	ScanDSLFiles();

	UE_LOG(LogTemp, Log, TEXT("BlueprintLispMappingRegistry: Initialized with %d DSL file mappings across %d Blueprints"),
		MappingEntries.Num(), BlueprintToDSLFiles.Num());
}

void FBlueprintLispMappingRegistry::Reset()
{
	MappingEntries.Empty();
	DSLFileToBlueprint.Empty();
	BlueprintToDSLFiles.Empty();
}

// ========== Queries ==========

TArray<FString> FBlueprintLispMappingRegistry::FindDSLFilesByBlueprint(const FString& BlueprintPath) const
{
	const TArray<FString>* FilesPtr = BlueprintToDSLFiles.Find(BlueprintPath);
	return FilesPtr ? *FilesPtr : TArray<FString>();
}

FString FBlueprintLispMappingRegistry::FindBlueprintByDSLFile(const FString& DSLFilePath) const
{
	const FString* BPPathPtr = DSLFileToBlueprint.Find(DSLFilePath);
	return BPPathPtr ? *BPPathPtr : FString();
}

TArray<FString> FBlueprintLispMappingRegistry::GetAllDSLFilePaths() const
{
	TArray<FString> Paths;
	Paths.Reserve(DSLFileToBlueprint.Num());
	for (const auto& Pair : DSLFileToBlueprint)
	{
		Paths.Add(Pair.Key);
	}
	return Paths;
}

// ========== Path Conversion (Internal Helpers) ==========

namespace
{
	/** Category tag string for BlueprintLisp DSL output directory */
	static const FString BlueprintLispCategory = TEXT("BlueprintLisp");

	// Strip the content root mount point prefix from a UE package path.
	// e.g., /Game/Characters/ALS/ALS_Npc -> Characters/ALS/ALS_Npc
	//        /MyPlugin/Characters/ALS/ALS_Npc -> Characters/ALS/ALS_Npc
	// Returns empty string for system mount points (/Engine/, /Script/, etc.).
	FString StripContentRootPrefix(const FString& PackagePath)
	{
		if (PackagePath.IsEmpty() || !PackagePath.StartsWith(TEXT("/")))
		{
			return FString();
		}

		int32 SecondSlash = PackagePath.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromStart, 1);
		if (SecondSlash <= 0)
		{
			return FString();
		}

		FString MountPoint = PackagePath.Mid(1, SecondSlash - 1);

		if (MountPoint == TEXT("Engine") ||
			MountPoint == TEXT("Script") ||
			MountPoint == TEXT("Temp") ||
			MountPoint == TEXT("Transient"))
		{
			return FString();
		}

		return PackagePath.RightChop(SecondSlash + 1);
	}
}

// ========== Path Conversion (Public Static) ==========

bool FBlueprintLispMappingRegistry::IsExportablePackage(const FString& PackagePath)
{
	return !StripContentRootPrefix(PackagePath).IsEmpty();
}

FString FBlueprintLispMappingRegistry::BlueprintToDSLPath(
	const FString& BlueprintPath,
	const FString& GraphName)
{
	if (BlueprintPath.IsEmpty() || GraphName.IsEmpty())
	{
		return FString();
	}

	FString RelativePath = StripContentRootPrefix(BlueprintPath);
	if (RelativePath.IsEmpty())
	{
		return FString();
	}

	// {ProjectDir}/Saved/BP2DSL/BlueprintLisp/Path/AssetName/{GraphName}.bplisp
	FString DSLDir = FPaths::ProjectDir() /
		TEXT("Saved") / TEXT("BP2DSL") / BlueprintLispCategory / RelativePath / GraphName;

	return DSLDir + TEXT(".bplisp");
}

FString FBlueprintLispMappingRegistry::DSLToBlueprintPath(const FString& DSLFilePath)
{
	// Expected: {ProjectDir}/Saved/BP2DSL/BlueprintLisp/Path/AssetName/{GraphName}.bplisp
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString ExpectedPrefix = ProjectDir / TEXT("Saved") / TEXT("BP2DSL") / BlueprintLispCategory / TEXT("");

	if (!DSLFilePath.StartsWith(ExpectedPrefix))
	{
		return FString();
	}

	// Strip prefix: Path/AssetName/{GraphName}.bplisp
	FString RelativePath = DSLFilePath.RightChop(ExpectedPrefix.Len());

	// The last path component is {GraphName}.bplisp, everything before is the asset relative path
	FString DirPart = FPaths::GetPath(RelativePath);
	FString AssetName = FPaths::GetBaseFilename(DirPart); // Last dir component = asset name

	if (AssetName.IsEmpty())
	{
		return FString();
	}

	// Phase 1: Try to resolve via AssetRegistry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> AllBPs;
	AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBPs);

	TArray<FString> Candidates;
	for (const FAssetData& Asset : AllBPs)
	{
		if (Asset.AssetName.ToString() == AssetName && IsExportablePackage(Asset.PackageName.ToString()))
		{
			Candidates.Add(Asset.PackageName.ToString());
		}
	}

	if (Candidates.Num() == 1)
	{
		return Candidates[0];
	}
	else if (Candidates.Num() > 1)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintLispMappingRegistry: DSLToBlueprintPath: ambiguous asset name '%s' (%d candidates), falling back to /Game/"),
			*AssetName, Candidates.Num());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintLispMappingRegistry: DSLToBlueprintPath: no matching Blueprint for '%s', falling back to /Game/"),
			*AssetName);
	}

	// Phase 2: Fallback to /Game/
	return TEXT("/Game/") + DirPart;
}

// ========== Internal Scanning ==========

void FBlueprintLispMappingRegistry::ScanBlueprints()
{
	// Note: We don't pre-populate per-graph entries because we don't know which graphs
	// each Blueprint has until the user exports. The DSL scan below handles this.
	// This is intentionally lighter than AnimBP2FP's ScanBlueprints().
	UE_LOG(LogTemp, Log, TEXT("BlueprintLispMappingRegistry: Blueprint scan skipped (lazy population on DSL scan)"));
}

void FBlueprintLispMappingRegistry::ScanDSLFiles()
{
	FString ScanDir = FPaths::ProjectDir() / TEXT("Saved") / TEXT("BP2DSL") / BlueprintLispCategory;

	if (!IFileManager::Get().DirectoryExists(*ScanDir))
	{
		return;
	}

	TArray<FString> FoundFiles;
	IFileManager::Get().FindFilesRecursive(FoundFiles, *ScanDir, TEXT("*.bplisp"), true, false);

	// Build asset name -> package path lookup
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TMap<FString, TArray<FString>> BPNameToPaths;
	TArray<FAssetData> AllBPs;
	AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBPs);
	for (const FAssetData& Asset : AllBPs)
	{
		FString PkgPath = Asset.PackageName.ToString();
		if (IsExportablePackage(PkgPath))
		{
			BPNameToPaths.FindOrAdd(Asset.AssetName.ToString()).Add(PkgPath);
		}
	}

	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString ExpectedPrefix = ProjectDir / TEXT("Saved") / TEXT("BP2DSL") / BlueprintLispCategory / TEXT("");

	for (const FString& FilePath : FoundFiles)
	{
		FString AbsPath = FPaths::ConvertRelativePathToFull(FilePath);

		if (!AbsPath.StartsWith(ExpectedPrefix))
		{
			continue;
		}

		// Strip prefix: Path/AssetName/{GraphName}.bplisp
		FString RelativePath = AbsPath.RightChop(ExpectedPrefix.Len());
		FString DirPart = FPaths::GetPath(RelativePath);
		FString AssetName = FPaths::GetBaseFilename(DirPart);
		FString GraphName = FPaths::GetBaseFilename(RelativePath);

		if (AssetName.IsEmpty() || GraphName.IsEmpty())
		{
			continue;
		}

		// Resolve Blueprint path
		FString BPPath;
		if (const TArray<FString>* Paths = BPNameToPaths.Find(AssetName))
		{
			if (Paths->Num() == 1)
			{
				BPPath = (*Paths)[0];
			}
			else
			{
				// Ambiguous: try exact path match
				FString ExpectedBPPath = TEXT("/Game/") + DirPart;
				for (const FString& P : *Paths)
				{
					if (P == ExpectedBPPath)
					{
						BPPath = P;
						break;
					}
				}
				if (BPPath.IsEmpty())
				{
					UE_LOG(LogTemp, Warning, TEXT("BlueprintLispMappingRegistry: Ambiguous blueprint name '%s' (%d matches) for DSL: %s"),
						*AssetName, Paths->Num(), *FPaths::GetCleanFilename(AbsPath));
				}
			}
		}

		// Fallback to /Game/ convention if not resolved
		if (BPPath.IsEmpty())
		{
			BPPath = TEXT("/Game/") + DirPart;
		}

		// Add mapping
		FMappingEntry Entry;
		Entry.BlueprintPath = BPPath;
		Entry.DSLFilePath = AbsPath;
		Entry.GraphName = GraphName;

		int32 Idx = MappingEntries.Add(MoveTemp(Entry));
		DSLFileToBlueprint.Add(AbsPath, BPPath);
		BlueprintToDSLFiles.FindOrAdd(BPPath).Add(AbsPath);
	}

	UE_LOG(LogTemp, Log, TEXT("BlueprintLispMappingRegistry: Scanned %d .bplisp files"), FoundFiles.Num());
}
