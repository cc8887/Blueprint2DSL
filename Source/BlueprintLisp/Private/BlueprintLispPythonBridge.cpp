// BlueprintLispPythonBridge.cpp - Python-facing editor bridge for BlueprintLisp
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "BlueprintLispPythonBridge.h"
#include "BlueprintLispConverter.h"
#include "FBlueprintLispMappingRegistry.h"

#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace BPLispBridge
{
	static FBlueprintLispPythonResult MakeFailure(const FString& Message)
	{
		FBlueprintLispPythonResult Result;
		Result.bSuccess = false;
		Result.Message = Message;
		return Result;
	}

	/** Normalise /Game/Foo/BP_Bar -> /Game/Foo/BP_Bar.BP_Bar */
	static FString NormalizeBlueprintObjectPath(const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty() || Path.Contains(TEXT("'")) || Path.Contains(TEXT(".")))
		{
			return Path;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
		return AssetName.IsEmpty() ? Path : Path + TEXT(".") + AssetName;
	}

	static UBlueprint* LoadBlueprintByPath(const FString& BlueprintPath, FString& OutResolvedPath, FString& OutError)
	{
		if (BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("BlueprintPath is empty");
			return nullptr;
		}
		OutResolvedPath = NormalizeBlueprintObjectPath(BlueprintPath);
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *OutResolvedPath);
		if (!BP)
		{
			OutError = FString::Printf(TEXT("Failed to load Blueprint: %s"), *BlueprintPath);
		}
		return BP;
	}

	static bool ReadTextFile(const FString& FilePath, FString& OutText, FString& OutError)
	{
		if (FilePath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("FilePath is empty");
			return false;
		}
		if (!FFileHelper::LoadFileToString(OutText, *FilePath))
		{
			OutError = FString::Printf(TEXT("Failed to read file: %s"), *FilePath);
			return false;
		}
		return true;
	}

	static bool WriteTextFile(const FString& FilePath, const FString& Text, FString& OutError)
	{
		if (FilePath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("Output file path is empty");
			return false;
		}
		const FString Directory = FPaths::GetPath(FilePath);
		if (!Directory.IsEmpty() && !IFileManager::Get().DirectoryExists(*Directory))
		{
			IFileManager::Get().MakeDirectory(*Directory, true);
		}
		if (!FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write file: %s"), *FilePath);
			return false;
		}
		return true;
	}

	static bool SaveBlueprintPackage(UBlueprint* Blueprint, FString& OutError)
	{
		if (!Blueprint)
		{
			OutError = TEXT("Blueprint is null");
			return false;
		}
		UPackage* Package = Blueprint->GetPackage();
		if (!Package)
		{
			OutError = TEXT("Blueprint package is null");
			return false;
		}
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Package);
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true))
		{
			OutError = FString::Printf(TEXT("Failed to save package for %s"), *Blueprint->GetPathName());
			return false;
		}
		return true;
	}

	/** Convert FBlueprintLispResult to FBlueprintLispPythonResult (fills warnings) */
	static FBlueprintLispPythonResult FromLispResult(
		const FBlueprintLispResult& In,
		const FString& AssetPath,
		const FString& SuccessMsg)
	{
		FBlueprintLispPythonResult Out;
		Out.bSuccess = In.bSuccess;
		Out.AssetPath = AssetPath;
		Out.DSLText = In.LispCode;
		Out.Warnings = In.Warnings;
		if (!In.Error.IsEmpty())
		{
			Out.Warnings.Insert(In.Error, 0);
		}
		Out.Message = In.bSuccess ? SuccessMsg : (In.Error.IsEmpty() ? TEXT("Operation failed") : In.Error);
		return Out;
	}
}

// ========== Export ==========

FBlueprintLispPythonResult UBlueprintLispPythonBridge::ExportGraphToText(
	const FString& BlueprintPath,
	const FString& GraphName,
	bool bIncludePositions,
	bool bStableIds)
{
	FString ResolvedPath;
	FString Error;
	UBlueprint* BP = BPLispBridge::LoadBlueprintByPath(BlueprintPath, ResolvedPath, Error);
	if (!BP)
	{
		return BPLispBridge::MakeFailure(Error);
	}

	FBlueprintLispConverter::FExportOptions Opts;
	Opts.bIncludePositions = bIncludePositions;
	Opts.bStableIds = bStableIds;

	FBlueprintLispResult LispResult = FBlueprintLispConverter::Export(BP, GraphName, Opts);
	return BPLispBridge::FromLispResult(LispResult, ResolvedPath,
		FString::Printf(TEXT("Exported graph '%s' from %s"), *GraphName, *ResolvedPath));
}

FBlueprintLispPythonResult UBlueprintLispPythonBridge::ExportGraphToFile(
	const FString& BlueprintPath,
	const FString& OutputFilePath,
	const FString& GraphName,
	bool bIncludePositions,
	bool bStableIds)
{
	FBlueprintLispPythonResult Result = ExportGraphToText(BlueprintPath, GraphName, bIncludePositions, bStableIds);
	if (!Result.bSuccess)
	{
		return Result;
	}
	FString WriteError;
	if (!BPLispBridge::WriteTextFile(OutputFilePath, Result.DSLText, WriteError))
	{
		return BPLispBridge::MakeFailure(WriteError);
	}
	Result.FilePath = OutputFilePath;
	Result.Message = FString::Printf(TEXT("Exported graph '%s' to file: %s"), *GraphName, *OutputFilePath);
	return Result;
}

FBlueprintLispPythonResult UBlueprintLispPythonBridge::ExportGraphToDefaultPath(
	const FString& BlueprintPath,
	const FString& GraphName,
	bool bIncludePositions,
	bool bStableIds)
{
	// Resolve default DSL file path via MappingRegistry
	FString DefaultPath = FBlueprintLispMappingRegistry::BlueprintToDSLPath(BlueprintPath, GraphName);
	if (DefaultPath.IsEmpty())
	{
		return BPLispBridge::MakeFailure(FString::Printf(
			TEXT("Cannot determine default DSL path for '%s' graph '%s' (invalid or non-exportable package)"),
			*BlueprintPath, *GraphName));
	}

	// Delegate to ExportGraphToFile
	FBlueprintLispPythonResult Result = ExportGraphToFile(
		BlueprintPath, DefaultPath, GraphName, bIncludePositions, bStableIds);
	return Result;
}

// ========== Import ==========

FBlueprintLispPythonResult UBlueprintLispPythonBridge::ImportGraphFromText(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& DSLText,
	bool bClearExisting,
	bool bCompile,
	bool bSavePackage)
{
	if (DSLText.TrimStartAndEnd().IsEmpty())
	{
		return BPLispBridge::MakeFailure(TEXT("DSLText is empty"));
	}
	FString ResolvedPath;
	FString Error;
	UBlueprint* BP = BPLispBridge::LoadBlueprintByPath(BlueprintPath, ResolvedPath, Error);
	if (!BP)
	{
		return BPLispBridge::MakeFailure(Error);
	}

	FBlueprintLispConverter::FImportOptions Opts;
	Opts.bClearExisting = bClearExisting;
	Opts.bCompile = bCompile;
	Opts.bAutoLayout = true;

	FBlueprintLispResult LispResult = FBlueprintLispConverter::Import(BP, GraphName, DSLText, Opts);
	FBlueprintLispPythonResult Result = BPLispBridge::FromLispResult(LispResult, ResolvedPath,
		FString::Printf(TEXT("Imported graph '%s' into %s"), *GraphName, *ResolvedPath));

	if (Result.bSuccess && bSavePackage)
	{
		FString SaveError;
		Result.bSavedPackage = BPLispBridge::SaveBlueprintPackage(BP, SaveError);
		if (!Result.bSavedPackage)
		{
			Result.Warnings.Add(SaveError);
			Result.Message += TEXT(" (package save failed)");
		}
	}
	return Result;
}

FBlueprintLispPythonResult UBlueprintLispPythonBridge::ImportGraphFromFile(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& InputFilePath,
	bool bClearExisting,
	bool bCompile,
	bool bSavePackage)
{
	FString DSLText;
	FString Error;
	if (!BPLispBridge::ReadTextFile(InputFilePath, DSLText, Error))
	{
		return BPLispBridge::MakeFailure(Error);
	}
	FBlueprintLispPythonResult Result = ImportGraphFromText(
		BlueprintPath, GraphName, DSLText, bClearExisting, bCompile, bSavePackage);
	Result.FilePath = InputFilePath;
	return Result;
}

// ========== Incremental Update ==========

FBlueprintLispPythonResult UBlueprintLispPythonBridge::UpdateGraphFromText(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& NewDSLText,
	bool bCompile,
	bool bSavePackage)
{
	if (NewDSLText.TrimStartAndEnd().IsEmpty())
	{
		return BPLispBridge::MakeFailure(TEXT("NewDSLText is empty"));
	}
	FString ResolvedPath;
	FString Error;
	UBlueprint* BP = BPLispBridge::LoadBlueprintByPath(BlueprintPath, ResolvedPath, Error);
	if (!BP)
	{
		return BPLispBridge::MakeFailure(Error);
	}

	FBlueprintLispConverter::FUpdateOptions Opts;
	Opts.bCompile = bCompile;
	Opts.bAutoLayout = false;

	FBlueprintLispResult LispResult = FBlueprintLispConverter::Update(BP, GraphName, NewDSLText, Opts);
	FBlueprintLispPythonResult Result = BPLispBridge::FromLispResult(LispResult, ResolvedPath,
		FString::Printf(TEXT("Updated graph '%s' in %s"), *GraphName, *ResolvedPath));

	if (Result.bSuccess && bSavePackage)
	{
		FString SaveError;
		Result.bSavedPackage = BPLispBridge::SaveBlueprintPackage(BP, SaveError);
		if (!Result.bSavedPackage)
		{
			Result.Warnings.Add(SaveError);
			Result.Message += TEXT(" (package save failed)");
		}
	}
	return Result;
}

FBlueprintLispPythonResult UBlueprintLispPythonBridge::UpdateGraphFromFile(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& InputFilePath,
	bool bCompile,
	bool bSavePackage)
{
	FString DSLText;
	FString Error;
	if (!BPLispBridge::ReadTextFile(InputFilePath, DSLText, Error))
	{
		return BPLispBridge::MakeFailure(Error);
	}
	FBlueprintLispPythonResult Result = UpdateGraphFromText(BlueprintPath, GraphName, DSLText, bCompile, bSavePackage);
	Result.FilePath = InputFilePath;
	return Result;
}

// ========== Query ==========

FBlueprintLispPythonResult UBlueprintLispPythonBridge::ListGraphs(const FString& BlueprintPath)
{
	FString ResolvedPath;
	FString Error;
	UBlueprint* BP = BPLispBridge::LoadBlueprintByPath(BlueprintPath, ResolvedPath, Error);
	if (!BP)
	{
		return BPLispBridge::MakeFailure(Error);
	}

	FString GraphList;
	GraphList.Reserve(256);

	auto AppendGraph = [&GraphList](const TCHAR* Type, UEdGraph* G)
	{
		if (!G) return;
		if (!GraphList.IsEmpty()) GraphList += TEXT("\n");
		GraphList += FString::Printf(TEXT("[%s] %s"), Type, *G->GetName());
	};

	for (UEdGraph* G : BP->UbergraphPages)
		AppendGraph(TEXT("Ubergraph"), G);
	for (UEdGraph* G : BP->FunctionGraphs)
		AppendGraph(TEXT("Function"), G);
	for (UEdGraph* G : BP->MacroGraphs)
		AppendGraph(TEXT("Macro"), G);
	// Note: EventDrivenTaskGraphs was removed in UE5.5 - skip this graph type

	FBlueprintLispPythonResult Result;
	Result.bSuccess = true;
	Result.AssetPath = ResolvedPath;
	Result.DSLText = GraphList;
	Result.Message = FString::Printf(TEXT("Found %d graphs in %s"),
		BP->UbergraphPages.Num() + BP->FunctionGraphs.Num() + BP->MacroGraphs.Num(),
		*ResolvedPath);
	return Result;
}

// ========== Validation ==========

FBlueprintLispPythonResult UBlueprintLispPythonBridge::ValidateDSL(const FString& DSLText)
{
	FBlueprintLispResult LispResult = FBlueprintLispConverter::Validate(DSLText);

	FBlueprintLispPythonResult Result;
	Result.bSuccess = LispResult.bSuccess;
	Result.DSLText = DSLText;
	Result.Warnings = LispResult.Warnings;
	if (!LispResult.Error.IsEmpty())
	{
		Result.Warnings.Insert(LispResult.Error, 0);
	}
	Result.Message = LispResult.bSuccess
		? TEXT("DSL syntax is valid")
		: FString::Printf(TEXT("DSL validation failed: %s"), *LispResult.Error);
	return Result;
}
