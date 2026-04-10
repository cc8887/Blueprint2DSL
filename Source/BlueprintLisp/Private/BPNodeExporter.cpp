// BPNodeExporter.cpp - Export UE Blueprint Node definitions to BlueprintLisp stub
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "BPNodeExporter.h"

#if WITH_EDITOR
#include "BlueprintLispModule.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "UObject/UObjectIterator.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

// ========== 主导出函数 ==========

bool FBPNodeExporter::ExportAllNodes(const FString& OutputPath)
{
	TArray<FNodeInfo> AllNodes = ScanAllBlueprintNodes();

	// Sort by category then name
	AllNodes.Sort([](const FNodeInfo& A, const FNodeInfo& B)
	{
		if (A.Category != B.Category) return A.Category < B.Category;
		return A.NodeName < B.NodeName;
	});

	FString Output;
	Output += TEXT(";; Auto-generated BlueprintLisp Node Definitions (Stub)\n");
	Output += TEXT(";; Generated from Unreal Engine\n");
	Output += TEXT(";; Do not edit manually\n\n");
	Output += TEXT(";; Total: ") + FString::FromInt(AllNodes.Num()) + TEXT(" node types\n\n");

	// Group by category
	FString CurrentCategory;
	for (const FNodeInfo& Node : AllNodes)
	{
		if (Node.Category != CurrentCategory)
		{
			CurrentCategory = Node.Category;
			if (CurrentCategory.IsEmpty())
			{
				CurrentCategory = TEXT("Uncategorized");
			}
			Output += TEXT("\n;; ========== ") + CurrentCategory + TEXT(" ==========\n\n");
		}

		// Header comment
		if (!Node.Description.IsEmpty())
		{
			Output += TEXT(";; ") + Node.Description + TEXT("\n");
		}
		Output += TEXT(";; UE Class: ") + Node.ClassName + TEXT("\n");

		// Node definition
		FString Attrs;
		if (Node.bIsPure) Attrs += TEXT(" :pure");
		if (Node.bIsCommutative) Attrs += TEXT(" :commutative");

		Output += TEXT("(define-node ") + Node.NodeName + Attrs + TEXT("\n");

		// Pins
		if (Node.Pins.Num() > 0)
		{
			Output += TEXT("  (pins");
			for (const auto& Pin : Node.Pins)
			{
				Output += TEXT("\n    (") + Pin.Name + TEXT(" :dir ") + Pin.Direction;
				Output += TEXT(" :type ") + Pin.PinType;
				if (Pin.bOptional) Output += TEXT(" :optional");
				Output += TEXT(")");
			}
			Output += TEXT(")\n");
		}

		Output += TEXT(")\n\n");
	}

	// Ensure directory exists
	FString Dir = FPaths::GetPath(OutputPath);
	IFileManager::Get().MakeDirectory(*Dir, true);

	return FFileHelper::SaveStringToFile(Output, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

// ========== 扫描所有蓝图节点 ==========

TArray<FBPNodeExporter::FNodeInfo> FBPNodeExporter::ScanAllBlueprintNodes()
{
	TArray<FNodeInfo> Nodes;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;

		if (!Class->IsChildOf(UK2Node::StaticClass()))
			continue;

		// Skip abstract classes
		if (Class->HasAnyClassFlags(CLASS_Abstract))
			continue;

		FNodeInfo Info;
		Info.ClassName = Class->GetName();

		// Convert to kebab-case: K2Node_IfThenElse -> if-then-else, UK2Node_CallFunction -> call-function
		FString ShortName = Info.ClassName;
		static const int32 UPrefixLen = FCString::Strlen(TEXT("UK2Node_"));
		static const int32 PrefixLen = FCString::Strlen(TEXT("K2Node_"));
		if (ShortName.Len() > UPrefixLen && ShortName.StartsWith(TEXT("UK2Node_")))
		{
			ShortName = ShortName.Mid(UPrefixLen);
		}
		else if (ShortName.Len() > PrefixLen && ShortName.StartsWith(TEXT("K2Node_")))
		{
			ShortName = ShortName.Mid(PrefixLen);
		}
		Info.NodeName = ToKebabCase(ShortName);

		// Get metadata from CDO
		if (UK2Node* CDO = Cast<UK2Node>(Class->GetDefaultObject()))
		{
			Info.Description = CDO->GetTooltipText().ToString();
			Info.bIsPure = CDO->IsNodePure();

			// Extract pin info from Pins array
			for (UEdGraphPin* Pin : CDO->Pins)
			{
				if (!Pin) continue;
				if (Pin->bHidden) continue;

				FNodeInfo::FPinInfo PinInfo;
				PinInfo.Name = Pin->PinName.ToString();
				PinInfo.Direction = (Pin->Direction == EGPD_Input) ? TEXT("input") : TEXT("output");
				PinInfo.PinType = GetPinTypeString(Pin->PinType);
				PinInfo.bOptional = !Pin->bNotConnectable && !Pin->bDefaultValueIsIgnored;
				Info.Pins.Add(PinInfo);
			}
		}

		Nodes.Add(Info);
	}

	return Nodes;
}

// ========== 命名转换 ==========

FString FBPNodeExporter::ToKebabCase(const FString& PascalCase)
{
	FString Result;
	for (int32 i = 0; i < PascalCase.Len(); ++i)
	{
		TCHAR Ch = PascalCase[i];
		if (FChar::IsUpper(Ch) && i > 0)
		{
			bool bPrevUpper = FChar::IsUpper(PascalCase[i - 1]);
			bool bNextLower = (i + 1 < PascalCase.Len()) && FChar::IsLower(PascalCase[i + 1]);
			if (!bPrevUpper || bNextLower)
			{
				Result += TEXT('-');
			}
		}
		Result += FChar::ToLower(Ch);
	}
	return Result;
}

// ========== 引脚类型映射 ==========

FString FBPNodeExporter::GetPinTypeString(const FEdGraphPinType& PinType)
{
	const FName& Category = PinType.PinCategory;

	if (Category == UEdGraphSchema_K2::PC_Exec)
		return TEXT("exec");
	if (Category == UEdGraphSchema_K2::PC_Boolean)
		return TEXT("bool");
	if (Category == UEdGraphSchema_K2::PC_Float || Category == UEdGraphSchema_K2::PC_Real)
		return TEXT("float");
	if (Category == UEdGraphSchema_K2::PC_Int)
		return TEXT("int");
	if (Category == UEdGraphSchema_K2::PC_String)
		return TEXT("string");
	if (Category == UEdGraphSchema_K2::PC_Name)
		return TEXT("name");
	if (Category == UEdGraphSchema_K2::PC_Object)
		return TEXT("object");
	if (Category == UEdGraphSchema_K2::PC_Struct)
		return TEXT("struct");
	if (Category == UEdGraphSchema_K2::PC_Enum)
		return TEXT("enum");
	if (Category == UEdGraphSchema_K2::PC_Byte)
		return TEXT("byte");
	if (Category == UEdGraphSchema_K2::PC_Wildcard)
		return TEXT("wildcard");
	if (Category == UEdGraphSchema_K2::PC_Delegate)
		return TEXT("delegate");
	if (Category == UEdGraphSchema_K2::PC_Interface)
		return TEXT("interface");

	// Container types (may not exist in all UE versions)
	if (Category == TEXT("Array"))
		return TEXT("array");
	if (Category == TEXT("Set"))
		return TEXT("set");
	if (Category == TEXT("Map"))
		return TEXT("map");

	return TEXT("any");
}

#endif // WITH_EDITOR
