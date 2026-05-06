// BlueprintLispPythonBridge.cpp - Python-facing editor bridge for BlueprintLisp
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "BlueprintLispPythonBridge.h"
#include "BlueprintLispConverter.h"
#include "FBlueprintLispMappingRegistry.h"
#include "BPNodeExporter.h"

#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"


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

	static FString EscapeDSLStringLiteral(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return Escaped;
	}

	static FString TopLevelVarTypeFromCategory(const FName& PinCategory, const FName& PinSubCategory, UObject* PinSubCategoryObject, FString& OutUnsupportedReason)
	{
		OutUnsupportedReason.Reset();
		const FString PinCategoryString = PinCategory.ToString();
		if (PinCategoryString == TEXT("bool"))
		{
			return TEXT("bool");
		}
		if (PinCategoryString == TEXT("int"))
		{
			return TEXT("int");
		}
		if (PinCategoryString == TEXT("int64"))
		{
			return TEXT("int64");
		}
		if (PinCategoryString == TEXT("byte"))
		{
			if (PinSubCategoryObject)
			{
				return PinSubCategoryObject->GetName();
			}
			return TEXT("byte");
		}
		if (PinCategoryString == TEXT("float"))
		{
			return TEXT("float");
		}
		if (PinCategoryString == TEXT("real") || PinCategoryString == TEXT("double"))
		{
			return PinSubCategory == UEdGraphSchema_K2::PC_Double ? TEXT("double") : TEXT("float");
		}
		if (PinCategoryString == TEXT("string"))
		{
			return TEXT("string");
		}
		if (PinCategoryString == TEXT("name"))
		{
			return TEXT("name");
		}
		if (PinCategoryString == TEXT("text"))
		{
			return TEXT("text");
		}
		if (PinCategoryString == TEXT("struct"))
		{
			if (PinSubCategoryObject)
			{
				return PinSubCategoryObject->GetName().ToLower();
			}
			OutUnsupportedReason = TEXT("struct variable type is missing a concrete struct reference");
			return FString();
		}
		if (PinCategoryString == TEXT("object") || PinCategoryString == TEXT("class"))
		{
			if (PinSubCategoryObject)
			{
				return PinSubCategoryObject->GetName();
			}
			OutUnsupportedReason = TEXT("object/class variable type is missing a concrete class reference");
			return FString();
		}
		if (PinCategoryString == TEXT("softclass"))
		{
			if (PinSubCategoryObject)
			{
				return FString::Printf(TEXT("softclass<%s>"), *PinSubCategoryObject->GetName());
			}
			OutUnsupportedReason = TEXT("softclass variable type is missing a concrete class reference");
			return FString();
		}
		if (PinCategoryString == TEXT("interface"))
		{
			if (PinSubCategoryObject)
			{
				return FString::Printf(TEXT("interface<%s>"), *PinSubCategoryObject->GetName());
			}
			OutUnsupportedReason = TEXT("interface variable type is missing a concrete interface reference");
			return FString();
		}

		OutUnsupportedReason = FString::Printf(TEXT("pin category '%s' is not yet supported by top-level var import"), *PinCategoryString);
		return FString();
	}

	static FString TerminalTypeToTopLevelVarType(const FEdGraphTerminalType& TerminalType, FString& OutUnsupportedReason)
	{
		return TopLevelVarTypeFromCategory(
			TerminalType.TerminalCategory,
			TerminalType.TerminalSubCategory,
			TerminalType.TerminalSubCategoryObject.Get(),
			OutUnsupportedReason);
	}

	static FString PinTypeToTopLevelVarType(const FEdGraphPinType& PinType, FString& OutUnsupportedReason)
	{
		OutUnsupportedReason.Reset();

		if (PinType.IsArray())
		{
			FString ElementReason;
			const FString ElementType = TopLevelVarTypeFromCategory(
				PinType.PinCategory,
				PinType.PinSubCategory,
				PinType.PinSubCategoryObject.Get(),
				ElementReason);
			if (ElementType.IsEmpty())
			{
				OutUnsupportedReason = FString::Printf(TEXT("array element type is unsupported: %s"), *ElementReason);
				return FString();
			}
			return FString::Printf(TEXT("array<%s>"), *ElementType);
		}

		if (PinType.IsSet())
		{
			FString ElementReason;
			const FString ElementType = TopLevelVarTypeFromCategory(
				PinType.PinCategory,
				PinType.PinSubCategory,
				PinType.PinSubCategoryObject.Get(),
				ElementReason);
			if (ElementType.IsEmpty())
			{
				OutUnsupportedReason = FString::Printf(TEXT("set element type is unsupported: %s"), *ElementReason);
				return FString();
			}
			return FString::Printf(TEXT("set<%s>"), *ElementType);
		}

		if (PinType.IsMap())
		{
			FString KeyReason;
			const FString KeyType = TopLevelVarTypeFromCategory(
				PinType.PinCategory,
				PinType.PinSubCategory,
				PinType.PinSubCategoryObject.Get(),
				KeyReason);
			if (KeyType.IsEmpty())
			{
				OutUnsupportedReason = FString::Printf(TEXT("map key type is unsupported: %s"), *KeyReason);
				return FString();
			}

			FString ValueReason;
			const FString ValueType = TerminalTypeToTopLevelVarType(PinType.PinValueType, ValueReason);
			if (ValueType.IsEmpty())
			{
				OutUnsupportedReason = FString::Printf(TEXT("map value type is unsupported: %s"), *ValueReason);
				return FString();
			}

			return FString::Printf(TEXT("map<%s,%s>"), *KeyType, *ValueType);
		}

		return TopLevelVarTypeFromCategory(
			PinType.PinCategory,
			PinType.PinSubCategory,
			PinType.PinSubCategoryObject.Get(),
			OutUnsupportedReason);
	}

	static bool TryBuildTopLevelVarDefaultLiteral(const FEdGraphPinType& PinType, const FString& DefaultValue, FString& OutLiteral, FString& OutUnsupportedReason)
	{
		OutLiteral.Reset();
		OutUnsupportedReason.Reset();
		const FString TrimmedDefaultValue = DefaultValue.TrimStartAndEnd();
		if (TrimmedDefaultValue.IsEmpty())
		{
			return true;
		}

		if (PinType.IsArray() || PinType.IsSet() || PinType.IsMap())
		{
			OutUnsupportedReason = TEXT("container default literal export is not yet supported by top-level var import");
			return false;
		}

		const FString PinCategory = PinType.PinCategory.ToString();
		if (PinCategory == TEXT("bool"))
		{
			if (TrimmedDefaultValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)
				|| TrimmedDefaultValue.Equals(TEXT("false"), ESearchCase::IgnoreCase))
			{
				OutLiteral = TrimmedDefaultValue.ToLower();
				return true;
			}
			OutUnsupportedReason = TEXT("bool default is not a true/false literal");
			return false;
		}

		if (PinCategory == TEXT("int")
			|| PinCategory == TEXT("int64")
			|| (PinCategory == TEXT("byte") && !PinType.PinSubCategoryObject.IsValid())
			|| PinCategory == TEXT("float")
			|| PinCategory == TEXT("real")
			|| PinCategory == TEXT("double"))
		{
			OutLiteral = TrimmedDefaultValue;
			return true;
		}

		if (PinCategory == TEXT("byte") && PinType.PinSubCategoryObject.IsValid())
		{
			OutLiteral = FString::Printf(TEXT("\"%s\""), *EscapeDSLStringLiteral(DefaultValue));
			return true;
		}

		if (PinCategory == TEXT("string") || PinCategory == TEXT("name") || PinCategory == TEXT("text"))
		{
			OutLiteral = FString::Printf(TEXT("\"%s\""), *EscapeDSLStringLiteral(DefaultValue));
			return true;
		}

		OutUnsupportedReason = FString::Printf(TEXT("default literal export is not yet supported for pin category '%s'"), *PinCategory);
		return false;
	}

	static TSharedRef<FJsonObject> MakeMemberVariablePayload(UBlueprint* Blueprint, const FBPVariableDescription& VariableDesc)
	{
		const FName VariableName = VariableDesc.VarName;
		const FEdGraphPinType& PinType = VariableDesc.VarType;

		FString TypeUnsupportedReason;
		const FString DSLType = PinTypeToTopLevelVarType(PinType, TypeUnsupportedReason);
		const bool bTopLevelVarSupported = !DSLType.IsEmpty();

		FString DefaultLiteral;
		FString DefaultLiteralUnsupportedReason;
		const bool bDefaultLiteralSupported = TryBuildTopLevelVarDefaultLiteral(PinType, VariableDesc.DefaultValue, DefaultLiteral, DefaultLiteralUnsupportedReason);

		FString ExposeOnSpawnValue;
		const bool bHasExposeOnSpawn = FBlueprintEditorUtils::GetBlueprintVariableMetaData(
			Blueprint,
			VariableName,
			nullptr,
			FBlueprintMetadata::MD_ExposeOnSpawn,
			ExposeOnSpawnValue) && ExposeOnSpawnValue.Equals(TEXT("true"), ESearchCase::IgnoreCase);

		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("variable_name"), VariableName.ToString());
		Payload->SetStringField(TEXT("pin_category"), PinType.PinCategory.ToString());
		Payload->SetStringField(TEXT("pin_sub_category"), PinType.PinSubCategory.ToString());
		Payload->SetStringField(TEXT("default_value"), VariableDesc.DefaultValue);
		Payload->SetBoolField(TEXT("has_expose_on_spawn"), bHasExposeOnSpawn);
		Payload->SetBoolField(TEXT("is_instance_editable"), (VariableDesc.PropertyFlags & CPF_DisableEditOnInstance) == 0);
		Payload->SetBoolField(TEXT("is_array"), PinType.IsArray());
		Payload->SetBoolField(TEXT("is_set"), PinType.IsSet());
		Payload->SetBoolField(TEXT("is_map"), PinType.IsMap());
		Payload->SetBoolField(TEXT("top_level_var_supported"), bTopLevelVarSupported);
		Payload->SetBoolField(TEXT("default_literal_supported"), bDefaultLiteralSupported);
		Payload->SetBoolField(TEXT("exact_recreation_supported"), bTopLevelVarSupported && bDefaultLiteralSupported);
		if (!DSLType.IsEmpty())
		{
			Payload->SetStringField(TEXT("dsl_type"), DSLType);
		}
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Payload->SetStringField(TEXT("sub_category_object"), PinType.PinSubCategoryObject->GetPathName());
		}
		if (!TypeUnsupportedReason.IsEmpty())
		{
			Payload->SetStringField(TEXT("type_unsupported_reason"), TypeUnsupportedReason);
		}
		if (!DefaultLiteral.IsEmpty())
		{
			Payload->SetStringField(TEXT("dsl_default_literal"), DefaultLiteral);
		}
		if (!DefaultLiteralUnsupportedReason.IsEmpty())
		{
			Payload->SetStringField(TEXT("default_literal_unsupported_reason"), DefaultLiteralUnsupportedReason);
		}
		return Payload;
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

namespace
{
	static FBlueprintLispConverter::EImportMode ToConverterImportMode(EBlueprintLispPythonImportMode ImportMode)
	{
		switch (ImportMode)
		{
		case EBlueprintLispPythonImportMode::MergeAppend:
			return FBlueprintLispConverter::EImportMode::MergeAppend;
		case EBlueprintLispPythonImportMode::UpdateSemantic:
			return FBlueprintLispConverter::EImportMode::UpdateSemantic;
		case EBlueprintLispPythonImportMode::ReplaceGraph:
		default:
			return FBlueprintLispConverter::EImportMode::ReplaceGraph;
		}
	}
}

FBlueprintLispPythonResult UBlueprintLispPythonBridge::ImportGraphFromText(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& DSLText,
	EBlueprintLispPythonImportMode ImportMode,
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
	Opts.ImportMode = ToConverterImportMode(ImportMode);
	Opts.bCompile = bCompile;
	Opts.bAutoLayout = true;
	Opts.bFailOnUnsupportedForm = true;

	FBlueprintLispResult LispResult = FBlueprintLispConverter::Import(BP, GraphName, DSLText, Opts);
	FBlueprintLispPythonResult Result = BPLispBridge::FromLispResult(LispResult, ResolvedPath,
		FString::Printf(TEXT("Imported graph '%s' into %s"), *GraphName, *ResolvedPath));

	if (Result.bSuccess && bSavePackage)
	{
		FString SaveError;
		Result.bSavedPackage = BPLispBridge::SaveBlueprintPackage(BP, SaveError);
		if (!Result.bSavedPackage)
		{
			Result.bSuccess = false;
			Result.Warnings.Add(SaveError);
			Result.Message = SaveError;
		}
	}
	return Result;

}

FBlueprintLispPythonResult UBlueprintLispPythonBridge::ImportGraphFromFile(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& InputFilePath,
	EBlueprintLispPythonImportMode ImportMode,
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
		BlueprintPath, GraphName, DSLText, ImportMode, bCompile, bSavePackage);
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
			Result.bSuccess = false;
			Result.Warnings.Add(SaveError);
			Result.Message = SaveError;
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

FBlueprintLispPythonResult UBlueprintLispPythonBridge::InspectMemberVariable(const FString& BlueprintPath, const FString& VariableName)
{
	const FString TrimmedVariableName = VariableName.TrimStartAndEnd();
	if (TrimmedVariableName.IsEmpty())
	{
		return BPLispBridge::MakeFailure(TEXT("VariableName is empty"));
	}

	FString ResolvedPath;
	FString Error;
	UBlueprint* BP = BPLispBridge::LoadBlueprintByPath(BlueprintPath, ResolvedPath, Error);
	if (!BP)
	{
		return BPLispBridge::MakeFailure(Error);
	}

	const FName VariableFName(*TrimmedVariableName);
	UBlueprint* OwnerBlueprint = BP;
	const int32 NewVarIndex = FBlueprintEditorUtils::FindNewVariableIndexAndBlueprint(BP, VariableFName, OwnerBlueprint);
	const bool bHasVariableDescription = OwnerBlueprint && NewVarIndex != INDEX_NONE && OwnerBlueprint->NewVariables.IsValidIndex(NewVarIndex);

	const FProperty* VariableProperty = nullptr;
	auto TryResolveProperty = [&VariableProperty, &VariableFName](const UClass* InClass) -> bool
	{
		if (!InClass)
		{
			return false;
		}
		VariableProperty = InClass->FindPropertyByName(VariableFName);
		return VariableProperty != nullptr;
	};

	const bool bFoundProperty = TryResolveProperty(BP->SkeletonGeneratedClass)
		|| TryResolveProperty(BP->GeneratedClass)
		|| TryResolveProperty(BP->ParentClass);
	if (!bHasVariableDescription && !bFoundProperty)
	{
		return BPLispBridge::MakeFailure(FString::Printf(TEXT("Failed to find variable '%s' in %s"), *TrimmedVariableName, *ResolvedPath));
	}

	const bool bDeclaredOnTargetBlueprint = bHasVariableDescription && OwnerBlueprint == BP;
	const FString DefaultValue = bHasVariableDescription ? OwnerBlueprint->NewVariables[NewVarIndex].DefaultValue : FString();

	FString ExposeOnSpawnValue;
	const bool bHasExposeOnSpawn = FBlueprintEditorUtils::GetBlueprintVariableMetaData(
		BP,
		VariableFName,
		nullptr,
		FBlueprintMetadata::MD_ExposeOnSpawn,
		ExposeOnSpawnValue) && ExposeOnSpawnValue.Equals(TEXT("true"), ESearchCase::IgnoreCase);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("variable_name"), TrimmedVariableName);
	Payload->SetBoolField(TEXT("declared_on_target_blueprint"), bDeclaredOnTargetBlueprint);
	Payload->SetStringField(TEXT("owner_blueprint_path"), OwnerBlueprint ? OwnerBlueprint->GetPathName() : FString());
	Payload->SetStringField(TEXT("default_value"), DefaultValue);
	Payload->SetBoolField(TEXT("has_expose_on_spawn"), bHasExposeOnSpawn);
	if (VariableProperty)
	{
		Payload->SetStringField(TEXT("property_class"), VariableProperty->GetClass()->GetName());
	}

	FString PayloadText;
	TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&PayloadText);
	FJsonSerializer::Serialize(Payload, JsonWriter);

	FBlueprintLispPythonResult Result;
	Result.bSuccess = true;
	Result.AssetPath = ResolvedPath;
	Result.DSLText = PayloadText;
	Result.Message = FString::Printf(TEXT("Inspected variable '%s' in %s"), *TrimmedVariableName, *ResolvedPath);
	return Result;
}

FBlueprintLispPythonResult UBlueprintLispPythonBridge::ListMemberVariables(const FString& BlueprintPath)
{
	FString ResolvedPath;
	FString Error;
	UBlueprint* BP = BPLispBridge::LoadBlueprintByPath(BlueprintPath, ResolvedPath, Error);
	if (!BP)
	{
		return BPLispBridge::MakeFailure(Error);
	}

	TArray<TSharedPtr<FJsonValue>> VariablePayloads;
	VariablePayloads.Reserve(BP->NewVariables.Num());
	for (const FBPVariableDescription& VariableDesc : BP->NewVariables)
	{
		VariablePayloads.Add(MakeShared<FJsonValueObject>(BPLispBridge::MakeMemberVariablePayload(BP, VariableDesc)));
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("blueprint_path"), ResolvedPath);
	Payload->SetNumberField(TEXT("variable_count"), BP->NewVariables.Num());
	Payload->SetArrayField(TEXT("variables"), VariablePayloads);

	FString PayloadText;
	TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&PayloadText);
	FJsonSerializer::Serialize(Payload, JsonWriter);

	FBlueprintLispPythonResult Result;
	Result.bSuccess = true;
	Result.AssetPath = ResolvedPath;
	Result.DSLText = PayloadText;
	Result.Message = FString::Printf(TEXT("Listed %d member variables in %s"), BP->NewVariables.Num(), *ResolvedPath);
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

// ========== Stub Export ==========

FBlueprintLispPythonResult UBlueprintLispPythonBridge::ExportStub(const FString& OutputFilePath)
{
#if WITH_EDITOR
	FString StubPath = OutputFilePath;
	if (StubPath.TrimStartAndEnd().IsEmpty())
	{
		StubPath = FPaths::ProjectDir() / TEXT("Saved") / TEXT("BP2DSL") / TEXT("BlueprintLisp") / TEXT("bplisp-stub.scm");
	}
	FPaths::NormalizeFilename(StubPath);

	bool bOk = FBPNodeExporter::ExportAllNodes(StubPath);

	FBlueprintLispPythonResult Result;
	Result.bSuccess = bOk;
	Result.FilePath = StubPath;
	Result.Message = bOk
		? FString::Printf(TEXT("Blueprint node stub exported to: %s"), *StubPath)
		: TEXT("Failed to export blueprint node stub");
	return Result;
#else
	FBlueprintLispPythonResult Result;
	Result.bSuccess = false;
	Result.Message = TEXT("Stub export is only available in editor builds");
	return Result;
#endif
}


