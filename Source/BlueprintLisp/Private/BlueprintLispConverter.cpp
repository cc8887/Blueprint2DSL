// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// BlueprintLispConverter.cpp - Blueprint EventGraph <-> BlueprintLisp DSL
//
// Export logic derived from ECABridge/ECABlueprintLispCommands.cpp (Epic Games, Experimental)
// Original author: Jon Olick
//
// This file implements the public FBlueprintLispConverter API.
// Import (DSL->BP) is currently stubbed; Export (BP->DSL) is fully implemented.

#include "BlueprintLispConverter.h"
#include "BlueprintLispModule.h"

#if WITH_EDITOR

#include "BlueprintLispAST.h"

#include "Engine/Blueprint.h"
#include "Animation/AnimInstance.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_Message.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_IfThenElse.h"

#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Self.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Composite.h"

#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_GenericCreateObject.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputKey.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_ActorBoundEvent.h"
#include "K2Node_MakeArray.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_Select.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_FunctionTerminator.h"
#include "K2Node_EnumEquality.h"
#include "K2Node_EnumInequality.h"
#include "K2Node_GetEnumeratorName.h"
#include "K2Node_GetEnumeratorNameAsString.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectIterator.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "Engine/LevelScriptBlueprint.h"
#include "UObject/UnrealType.h"


#include "AnimGraphNode_TransitionResult.h"
#include "AnimationTransitionGraph.h"
#include "K2Node_AnimNodeReference.h"
#include "K2Node_AnimGetter.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintLisp, Log, All);

// ============================================================================
// Internal helpers: short-GUID generation for stable :id tags
// ============================================================================
namespace
{

/** Compute shortest unique GUID prefix for a set of GUIDs */
static TMap<FGuid, FString> ComputeShortIds(const TArray<FGuid>& Guids)
{
	static const int32 Lengths[] = { 8, 12, 16, 20, 32 };
	TMap<FGuid, FString> Result;
	if (Guids.IsEmpty()) return Result;

	for (int32 LenIdx = 0; LenIdx < UE_ARRAY_COUNT(Lengths); LenIdx++)
	{
		int32 Len = Lengths[LenIdx];
		TMap<FString, int32> PrefixCount;
		for (const FGuid& G : Guids)
		{
			FString S = G.ToString(EGuidFormats::Digits).Left(Len).ToLower();
			PrefixCount.FindOrAdd(S)++;
		}
		for (const FGuid& G : Guids)
		{
			if (Result.Contains(G)) continue;
			FString S = G.ToString(EGuidFormats::Digits).Left(Len).ToLower();
			if (PrefixCount[S] == 1)
				Result.Add(G, S);
		}
		bool bAllDone = true;
		for (const FGuid& G : Guids)
			if (!Result.Contains(G)) { bAllDone = false; break; }
		if (bAllDone) break;
	}
	// Fallback: full 32-char
	for (const FGuid& G : Guids)
		if (!Result.Contains(G))
			Result.Add(G, G.ToString(EGuidFormats::Digits).ToLower());
	return Result;
}

// ============================================================================
// Export: BP -> DSL
// ============================================================================

static FString EXP_GetFunctionOwnerToken(UFunction* Function, UEdGraph* Graph)
{
	UClass* OwnerClass = Function ? Function->GetOuterUClass() : nullptr;
	UBlueprint* Blueprint = Graph ? FBlueprintEditorUtils::FindBlueprintForGraph(Graph) : nullptr;
	if (OwnerClass && Blueprint
		&& (OwnerClass == Blueprint->GeneratedClass
			|| OwnerClass == Blueprint->SkeletonGeneratedClass
			|| OwnerClass->ClassGeneratedBy == Blueprint))
	{
		return TEXT("self");
	}
	return OwnerClass ? OwnerClass->GetPathName() : FString();
}

static thread_local TArray<FString>* GBlueprintLispExportErrors = nullptr;

struct FScopedBlueprintLispExportErrors
{
	explicit FScopedBlueprintLispExportErrors(TArray<FString>& InErrors)
		: Previous(GBlueprintLispExportErrors)
	{
		GBlueprintLispExportErrors = &InErrors;
	}

	~FScopedBlueprintLispExportErrors()
	{
		GBlueprintLispExportErrors = Previous;
	}

	TArray<FString>* Previous = nullptr;
};

static void EXP_AddExportError(const FString& Error)
{
	if (GBlueprintLispExportErrors)
	{
		GBlueprintLispExportErrors->AddUnique(Error);
	}
}

// Forward declarations
static FLispNodePtr ConvertPureExpressionToLisp(UEdGraphPin* ValuePin, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited, const TMap<FGuid, FString>* ShortIds = nullptr);
static FLispNodePtr ConvertNodeToLisp(UEdGraphNode* Node, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited, bool bPositions, const TMap<FGuid, FString>& ShortIds);
static FLispNodePtr ConvertExecChainToLisp(UEdGraphPin* ExecPin, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited, bool bPositions, const TMap<FGuid, FString>& ShortIds);

// ImportGraph helper (defined below after ExportGraph helpers)
static UEdGraphPin* BuildPureExprNode(const FLispNodePtr& Expr, UEdGraph* Graph, UBlueprint* BP, TArray<UEdGraphNode*>& CreatedNodes, FString& OutLiteralValue, TArray<FString>* OutErrors = nullptr);


/** Append :id keyword to a form if the node has a stable GUID in ShortIds */
static FLispNodePtr AppendNodeId(FLispNodePtr Form, UEdGraphNode* Node, const TMap<FGuid, FString>& ShortIds)
{
	if (!Form.IsValid() || Form->IsNil() || !Form->IsList() || !Node) return Form;
	if (const FString* Id = ShortIds.Find(Node->NodeGuid))
	{
		Form->Children.Add(FLispNode::MakeKeyword(TEXT(":id")));
		Form->Children.Add(FLispNode::MakeString(*Id));
	}
	return Form;
}

static bool BP_IsStructuralSeqWrapper(const FLispNodePtr& Body)
{
	return Body.IsValid()
		&& Body->IsList()
		&& Body->IsForm(TEXT("seq"))
		&& !Body->HasKeyword(TEXT(":id"));
}


/** Get clean function name from a K2Node_CallFunction */
static FString GetCleanNodeName(UEdGraphNode* Node)
{
	if (UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(Node))
	{
		if (UFunction* Func = CF->GetTargetFunction())
			return Func->GetName();
	}
	return Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
}

/** Find the "then" exec output pin of a node */
static UEdGraphPin* GetThenPin(UEdGraphNode* Node)
{
	for (UEdGraphPin* Pin : Node->Pins)
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName == UEdGraphSchema_K2::PN_Then)
			return Pin;
	// Fallback: first exec output
	for (UEdGraphPin* Pin : Node->Pins)
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			return Pin;
	return nullptr;
}

/** Map EdGraphPinType to a Lisp type symbol */
static FString PinTypeToLispType(const FEdGraphPinType& PT)
{
	auto BaseTypeToLisp = [](const FName& PinCategory, const FName& PinSubCategory, UObject* PinSubCategoryObject) -> FString
	{
		const FString Cat = PinCategory.ToString();
		if (Cat == TEXT("bool"))   return TEXT("bool");
		if (Cat == TEXT("int"))    return TEXT("int");
		if (Cat == TEXT("int64"))  return TEXT("int64");
		if (Cat == TEXT("float"))  return TEXT("float");
		if (Cat == TEXT("real") || Cat == TEXT("double")) return PinSubCategory == UEdGraphSchema_K2::PC_Double ? TEXT("double") : TEXT("float");
		if (Cat == TEXT("string")) return TEXT("string");
		if (Cat == TEXT("name"))   return TEXT("name");
		if (Cat == TEXT("text"))   return TEXT("text");
		if (Cat == TEXT("byte"))
		{
			if (PinSubCategoryObject)
			{
				return PinSubCategoryObject->GetName();
			}
			return TEXT("byte");
		}
		if (Cat == TEXT("struct"))
		{
			if (PinSubCategoryObject)
			{
				return PinSubCategoryObject->GetName().ToLower();
			}
			return TEXT("struct");
		}
		if (Cat == TEXT("object") || Cat == TEXT("class"))
		{
			if (PinSubCategoryObject)
			{
				return PinSubCategoryObject->GetName();
			}
			return TEXT("object");
		}
		if (Cat == TEXT("softclass"))
		{
			if (PinSubCategoryObject)
			{
				return FString::Printf(TEXT("softclass<%s>"), *PinSubCategoryObject->GetName());
			}
			return TEXT("softclass");
		}
		if (Cat == TEXT("interface"))
		{
			if (PinSubCategoryObject)
			{
				return FString::Printf(TEXT("interface<%s>"), *PinSubCategoryObject->GetName());
			}
			return TEXT("interface");
		}
		return Cat.ToLower();
	};

	if (PT.IsArray())
	{
		return FString::Printf(TEXT("array<%s>"), *BaseTypeToLisp(PT.PinCategory, PT.PinSubCategory, PT.PinSubCategoryObject.Get()));
	}
	if (PT.IsSet())
	{
		return FString::Printf(TEXT("set<%s>"), *BaseTypeToLisp(PT.PinCategory, PT.PinSubCategory, PT.PinSubCategoryObject.Get()));
	}
	if (PT.IsMap())
	{
		return FString::Printf(
			TEXT("map<%s,%s>"),
			*BaseTypeToLisp(PT.PinCategory, PT.PinSubCategory, PT.PinSubCategoryObject.Get()),
			*BaseTypeToLisp(PT.PinValueType.TerminalCategory, PT.PinValueType.TerminalSubCategory, PT.PinValueType.TerminalSubCategoryObject.Get()));
	}

	return BaseTypeToLisp(PT.PinCategory, PT.PinSubCategory, PT.PinSubCategoryObject.Get());
}

static bool EXP_IsEntryValueSource(UEdGraphNode* SourceNode)
{
	if (!SourceNode) return false;
	if (Cast<UK2Node_FunctionEntry>(SourceNode)) return true;
	if (Cast<UK2Node_CustomEvent>(SourceNode) || Cast<UK2Node_Event>(SourceNode)) return true;
	if (Cast<UK2Node_InputAction>(SourceNode) || Cast<UK2Node_InputKey>(SourceNode)) return true;
	if (Cast<UK2Node_ComponentBoundEvent>(SourceNode) || Cast<UK2Node_ActorBoundEvent>(SourceNode)) return true;
	if (UK2Node_Tunnel* TunnelNode = Cast<UK2Node_Tunnel>(SourceNode))
	{
		return TunnelNode->DrawNodeAsEntry();
	}
	return false;
}

static FString EXP_GetReusableValueSymbol(UEdGraphNode* SourceNode, UEdGraphPin* SourcePin)
{
	if (!SourceNode || !SourcePin) return TEXT("");
	if (SourcePin->Direction != EGPD_Output) return TEXT("");
	if (SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) return TEXT("");

	if (EXP_IsEntryValueSource(SourceNode))
	{
		return SourcePin->PinName.ToString();
	}

	if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(SourceNode))
	{
		const FString VarName = VarSet->VariableReference.GetMemberName().ToString();
		return VarName.IsEmpty() ? SourcePin->PinName.ToString() : VarName;
	}

	if (Cast<UK2Node_CallFunction>(SourceNode))
	{
		return SourcePin->PinName.ToString().ToLower();
	}

	if (Cast<UK2Node_MacroInstance>(SourceNode))
	{
		return SourcePin->PinName.ToString().Replace(TEXT(" "), TEXT(""));
	}

	return TEXT("");
}

static FLispNodePtr EXP_MakeNameAtom(const FString& Name)
{
	const bool bNeedsQuoting = Name.Contains(TEXT(" ")) || Name.Contains(TEXT("\t"));
	return bNeedsQuoting ? FLispNode::MakeString(Name) : FLispNode::MakeSymbol(Name);
}

static FString EXP_GetStableStructIdentifier(const UScriptStruct* StructType)
{
	if (!StructType) return FString();
	const FString Path = StructType->GetPathName();
	return Path.StartsWith(TEXT("/Script/")) ? StructType->GetName() : Path;
}

static FString EXP_StripGeneratedPinSuffixes(const FString& Name)
{
	FString Result = Name;
	while (true)
	{
		int32 LastUnderscore = INDEX_NONE;
		if (!Result.FindLastChar(TEXT('_'), LastUnderscore) || LastUnderscore <= 0)
		{
			break;
		}

		const FString Tail = Result.Mid(LastUnderscore + 1);
		bool bAllDigits = !Tail.IsEmpty();
		bool bAllHexDigits = Tail.Len() == 32;
		for (const TCHAR Ch : Tail)
		{
			if (!FChar::IsDigit(Ch))
			{
				bAllDigits = false;
			}
			if (!FChar::IsHexDigit(Ch))
			{
				bAllHexDigits = false;
			}
		}

		if (!bAllDigits && !bAllHexDigits)
		{
			break;
		}

		Result = Result.Left(LastUnderscore);
	}

	return Result;
}

static FString EXP_GetStablePinKeywordName(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return TEXT("");
	}

	return EXP_StripGeneratedPinSuffixes(Pin->PinName.ToString()).Replace(TEXT(" "), TEXT("")).ToLower();
}

static void EXP_AppendMacroOutputDeclaration(TArray<FLispNodePtr>& Args, UEdGraphPin* Pin)
{
	if (!Pin || Pin->Direction != EGPD_Output) return;
	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) return;
	if (Pin->bHidden) return;

	Args.Add(FLispNode::MakeKeyword(TEXT(":out")));
	TArray<FLispNodePtr> OutPair;
	OutPair.Add(FLispNode::MakeSymbol(Pin->PinName.ToString().Replace(TEXT(" "), TEXT(""))));
	OutPair.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
	Args.Add(FLispNode::MakeList(OutPair));
}

static FLispNodePtr EXP_BuildMacroCallForm(UK2Node_MacroInstance* MacroInst, UEdGraphPin* SelectedOutputPin, bool bIncludeAllOutputs, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited, const TMap<FGuid, FString>* ShortIds = nullptr)
{
	if (!MacroInst) return FLispNode::MakeNil();


	FString MacroName;
	if (UEdGraph* MacroGraph = MacroInst->GetMacroGraph())
	{
		MacroName = MacroGraph->GetName();
	}
	if (MacroName.IsEmpty())
	{
		MacroName = MacroInst->GetNodeTitle(ENodeTitleType::ListView).ToString();
	}

	TArray<FLispNodePtr> Args;
	Args.Add(FLispNode::MakeSymbol(TEXT("call-macro")));
	Args.Add(EXP_MakeNameAtom(MacroName));



	for (UEdGraphPin* Pin : MacroInst->Pins)
	{
		if (Pin->Direction != EGPD_Input) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->bHidden) continue;
		FLispNodePtr Val = ConvertPureExpressionToLisp(Pin, Graph, Visited, ShortIds);
		if (!Val->IsNil())
		{
			Args.Add(FLispNode::MakeKeyword(FString::Printf(TEXT(":%s"), *Pin->PinName.ToString().ToLower())));
			Args.Add(Val);
		}
	}


	if (bIncludeAllOutputs)
	{
		for (UEdGraphPin* Pin : MacroInst->Pins)
		{
			EXP_AppendMacroOutputDeclaration(Args, Pin);
		}
	}
	else
	{
		EXP_AppendMacroOutputDeclaration(Args, SelectedOutputPin);
	}

	return ShortIds ? AppendNodeId(FLispNode::MakeList(Args), MacroInst, *ShortIds) : FLispNode::MakeList(Args);
}



// ----- Convert pure (data-flow) expression to Lisp -----

static FLispNodePtr EXP_ConvertLiteralBoolCallToLisp(UK2Node_CallFunction* CallNode, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited)
{
	if (!CallNode)
	{
		return FLispNode::MakeNil();
	}

	UFunction* TargetFunction = CallNode->GetTargetFunction();
	if (!TargetFunction || TargetFunction->GetName() != TEXT("MakeLiteralBool"))
	{
		return FLispNode::MakeNil();
	}

	for (UEdGraphPin* Pin : CallNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Boolean) continue;
		return ConvertPureExpressionToLisp(Pin, Graph, Visited);
	}

	return FLispNode::MakeNil();
}

static FLispNodePtr ConvertPureExpressionToLisp(UEdGraphPin* ValuePin, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited, const TMap<FGuid, FString>* ShortIds)
{


	if (!ValuePin || ValuePin->LinkedTo.Num() == 0)
	{
		// Return default value as literal
		if (!ValuePin) return FLispNode::MakeNil();
		if (ValuePin->DefaultObject)
		{
			TArray<FLispNodePtr> AssetArgs;
			AssetArgs.Add(FLispNode::MakeSymbol(TEXT("asset")));
			AssetArgs.Add(FLispNode::MakeString(ValuePin->DefaultObject->GetPathName()));
			return FLispNode::MakeList(AssetArgs);
		}
		FString DefaultValue = ValuePin->DefaultValue;
		if (ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
			&& GetDefault<UEdGraphSchema_K2>()->DoesDefaultValueMatchAutogenerated(*ValuePin))
		{
			return FLispNode::MakeNil();
		}
		if (DefaultValue.IsEmpty())
		{
			DefaultValue = ValuePin->AutogeneratedDefaultValue;
		}
		if (DefaultValue.IsEmpty())
		{
			if (ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			{
				DefaultValue = TEXT("false");
			}
			else if (ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int
				|| ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Float
				|| ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Double
				|| ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
			{
				DefaultValue = TEXT("0");
			}
		}
		if (DefaultValue.IsEmpty()) return FLispNode::MakeNil();
		double Num = 0;
		if (ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			return FLispNode::MakeSymbol(DefaultValue.ToLower() == TEXT("true") ? TEXT("true") : TEXT("false"));
		if (ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int
			|| ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Float
			|| ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Double
			|| ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			if (LexTryParseString(Num, *DefaultValue))
				return FLispNode::MakeNumber(Num);
		}
		if (!DefaultValue.IsEmpty())
			return FLispNode::MakeString(DefaultValue);
		return FLispNode::MakeNil();
	}

	UEdGraphPin* SourcePin = ValuePin->LinkedTo[0];
	if (!SourcePin) return FLispNode::MakeNil();
	UEdGraphNode* SourceNode = SourcePin->GetOwningNode();
	if (!SourceNode) return FLispNode::MakeNil();

	if (EXP_IsEntryValueSource(SourceNode))
	{
		const FString ParamName = EXP_GetReusableValueSymbol(SourceNode, SourcePin);
		if (!ParamName.IsEmpty())
		{
			return FLispNode::MakeSymbol(ParamName);
		}
	}

	if (UK2Node_VariableSet* SourceVarSet = Cast<UK2Node_VariableSet>(SourceNode))
	{
		if (!SourceVarSet->VariableReference.IsLocalScope() && SourceVarSet->VariableReference.IsSelfContext())
		{
			TArray<FLispNodePtr> MemberItems;
			MemberItems.Add(FLispNode::MakeSymbol(FString::Printf(TEXT("self.%s"),
				*SourceVarSet->VariableReference.GetMemberName().ToString())));
			return FLispNode::MakeList(MemberItems);
		}
		const FString ReusableValue = EXP_GetReusableValueSymbol(SourceVarSet, SourcePin);
		if (!ReusableValue.IsEmpty())
		{
			return FLispNode::MakeSymbol(ReusableValue);
		}
	}

	if (UK2Node_Knot* KnotNode = Cast<UK2Node_Knot>(SourceNode))
	{
		if (UEdGraphPin* KnotInputPin = KnotNode->GetInputPin())
		{
			return ConvertPureExpressionToLisp(KnotInputPin, Graph, Visited, ShortIds);
		}
	}

	if (SourceNode->GetClass()->GetName() == TEXT("K2Node_EvaluateChooser2"))
	{
		return FLispNode::MakeSymbol(SourcePin->PinName.ToString());
	}
	if (UK2Node_DynamicCast* DynamicCastNode = Cast<UK2Node_DynamicCast>(SourceNode))
	{
		if (SourcePin == DynamicCastNode->GetBoolSuccessPin() && DynamicCastNode->IsNodePure())
		{
			TArray<FLispNodePtr> Args;
			Args.Add(FLispNode::MakeSymbol(TEXT("pure-cast-succeeds")));
			Args.Add(FLispNode::MakeKeyword(TEXT(":class")));
			Args.Add(FLispNode::MakeString(DynamicCastNode->TargetType ? DynamicCastNode->TargetType->GetPathName() : TEXT("")));
			Args.Add(FLispNode::MakeKeyword(TEXT(":object")));
			Args.Add(ConvertPureExpressionToLisp(DynamicCastNode->GetCastSourcePin(), Graph, Visited, ShortIds));
			return ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);
		}
		if (SourcePin == DynamicCastNode->GetCastResultPin())
		{
			if (DynamicCastNode->IsNodePure())
			{
				TArray<FLispNodePtr> Args;
				Args.Add(FLispNode::MakeSymbol(TEXT("pure-cast")));
				Args.Add(FLispNode::MakeKeyword(TEXT(":class")));
				Args.Add(FLispNode::MakeString(DynamicCastNode->TargetType ? DynamicCastNode->TargetType->GetPathName() : TEXT("")));
				Args.Add(FLispNode::MakeKeyword(TEXT(":object")));
				Args.Add(ConvertPureExpressionToLisp(DynamicCastNode->GetCastSourcePin(), Graph, Visited, ShortIds));
				return ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);
			}
			return FLispNode::MakeSymbol(SourcePin->PinName.ToString());
		}
	}
	if (UK2Node_GenericCreateObject* CreateObjectNode = Cast<UK2Node_GenericCreateObject>(SourceNode))
	{
		if (SourcePin == CreateObjectNode->GetResultPin())
		{
			return FLispNode::MakeSymbol(SourcePin->PinName.ToString());
		}
	}

	// A collapsed graph is a real nested graph, not an opaque pure-node call. Preserve
	// every boundary pin plus the expression feeding each exit tunnel input.
	if (UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(SourceNode))
	{
		if (!CompositeNode->BoundGraph || !CompositeNode->GetEntryNode() || !CompositeNode->GetExitNode())
		{
			EXP_AddExportError(FString::Printf(TEXT("Collapsed graph '%s' has no valid bound graph or tunnels"), *SourceNode->GetName()));
			return FLispNode::MakeSymbol(TEXT("invalid-collapsed-graph"));
		}
		if (Visited.Contains(SourceNode))
		{
			EXP_AddExportError(FString::Printf(TEXT("Collapsed graph '%s' contains a circular boundary reference"), *SourceNode->GetName()));
			return FLispNode::MakeSymbol(TEXT("...circular..."));
		}

		Visited.Add(SourceNode);
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("collapsed-graph")));
		Args.Add(FLispNode::MakeKeyword(TEXT(":name")));
		Args.Add(FLispNode::MakeString(CompositeNode->BoundGraph->GetName()));

		for (UEdGraphPin* Pin : CompositeNode->Pins)
		{
			if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->ParentPin != nullptr)
			{
				continue;
			}

			if (Pin->Direction == EGPD_Input)
			{
				TArray<FLispNodePtr> InputSpec;
				InputSpec.Add(EXP_MakeNameAtom(Pin->PinName.ToString()));
				InputSpec.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
				InputSpec.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, ShortIds));
				Args.Add(FLispNode::MakeKeyword(TEXT(":input")));
				Args.Add(FLispNode::MakeList(InputSpec));
			}
			else if (Pin->Direction == EGPD_Output)
			{
				UEdGraphPin* ExitPin = CompositeNode->GetExitNode()->FindPin(Pin->PinName, EGPD_Input);
				if (!ExitPin)
				{
					EXP_AddExportError(FString::Printf(
						TEXT("Collapsed graph '%s' output '%s' has no matching exit tunnel pin"),
						*SourceNode->GetName(), *Pin->PinName.ToString()));
					continue;
				}

				TArray<FLispNodePtr> OutputSpec;
				OutputSpec.Add(EXP_MakeNameAtom(Pin->PinName.ToString()));
				OutputSpec.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
				OutputSpec.Add(ConvertPureExpressionToLisp(ExitPin, CompositeNode->BoundGraph, Visited, ShortIds));
				Args.Add(FLispNode::MakeKeyword(TEXT(":output")));
				Args.Add(FLispNode::MakeList(OutputSpec));
			}
		}

		Args.Add(FLispNode::MakeKeyword(TEXT(":selected")));
		Args.Add(EXP_MakeNameAtom(SourcePin->PinName.ToString()));
		Visited.Remove(SourceNode);
		return ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);
	}


	// Variable get

	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(SourceNode))

	{
		FString VarName = VarGet->VariableReference.GetMemberName().ToString();
		if (SourcePin->ParentPin && SourcePin->ParentPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			UScriptStruct* StructType = Cast<UScriptStruct>(SourcePin->ParentPin->PinType.PinSubCategoryObject.Get());
			FString FieldName = SourcePin->PinName.ToString();
			const FString GeneratedPrefix = VarName + TEXT("_");
			if (FieldName.StartsWith(GeneratedPrefix)) FieldName.RightChopInline(GeneratedPrefix.Len());

			TArray<FLispNodePtr> ValueItems;
			ValueItems.Add(FLispNode::MakeSymbol(FString::Printf(TEXT("self.%s"), *VarName)));
			TArray<FLispNodePtr> Args;
			Args.Add(FLispNode::MakeSymbol(TEXT("break-struct")));
			Args.Add(FLispNode::MakeKeyword(TEXT(":struct")));
			Args.Add(EXP_MakeNameAtom(EXP_GetStableStructIdentifier(StructType)));
			Args.Add(FLispNode::MakeKeyword(TEXT(":value")));
			Args.Add(FLispNode::MakeList(ValueItems));
			Args.Add(FLispNode::MakeKeyword(TEXT(":field")));
			Args.Add(EXP_MakeNameAtom(FieldName));
			return ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);
		}
		if (VarGet->VariableReference.IsLocalScope())
			return FLispNode::MakeSymbol(VarName);
		// Member variable: (self.VarName)
		TArray<FLispNodePtr> Items;
		Items.Add(FLispNode::MakeSymbol(FString::Printf(TEXT("self.%s"), *VarName)));
		return FLispNode::MakeList(Items);
	}

	// Self node
	if (Cast<UK2Node_Self>(SourceNode))
		return FLispNode::MakeSymbol(TEXT("self"));

	// Preserve explicit user-defined struct construction in pure data flow.
	// Native make functions are canonicalized separately in the call-function
	// branch below; UK2Node_MakeStruct needs the same DSL representation.
	if (UK2Node_MakeStruct* MakeStructNode = Cast<UK2Node_MakeStruct>(SourceNode))
	{
		if (!MakeStructNode->StructType)
		{
			return FLispNode::MakeNil();
		}
		TArray<FLispNodePtr> MakeArgs;
		MakeArgs.Add(FLispNode::MakeSymbol(TEXT("make-struct")));
		MakeArgs.Add(FLispNode::MakeKeyword(TEXT(":struct")));
		MakeArgs.Add(FLispNode::MakeString(MakeStructNode->StructType->GetPathName()));
		for (UEdGraphPin* Pin : MakeStructNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				|| Pin->bHidden || Pin->ParentPin) continue;
			TArray<FLispNodePtr> FieldSpec;
			FieldSpec.Add(EXP_MakeNameAtom(Pin->PinName.ToString()));
			FieldSpec.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
			FieldSpec.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, ShortIds));
			MakeArgs.Add(FLispNode::MakeKeyword(TEXT(":field")));
			MakeArgs.Add(FLispNode::MakeList(FieldSpec));
		}
		return FLispNode::MakeList(MakeArgs);
	}

	// Literal function call (pure node or any call node providing a value)
	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(SourceNode))
	{
		if (Visited.Contains(SourceNode))
		{
			const FString ReusableValue = EXP_GetReusableValueSymbol(SourceNode, SourcePin);
			return ReusableValue.IsEmpty() ? FLispNode::MakeSymbol(TEXT("...circular...")) : FLispNode::MakeSymbol(ReusableValue);
		}
		Visited.Add(SourceNode);

		if (FLispNodePtr LiteralBool = EXP_ConvertLiteralBoolCallToLisp(CallNode, Graph, Visited);
			LiteralBool.IsValid() && !LiteralBool->IsNil())
		{
			Visited.Remove(SourceNode);
			return LiteralBool;
		}

		if (UFunction* TargetFunction = CallNode->GetTargetFunction())
		{
			const UEdGraphPin* StructOutputPin = SourcePin->ParentPin ? SourcePin->ParentPin : SourcePin;
			UScriptStruct* OutputStructType = StructOutputPin
				? Cast<UScriptStruct>(StructOutputPin->PinType.PinSubCategoryObject.Get())
				: nullptr;
			if (!SourcePin->ParentPin
				&& OutputStructType
				&& OutputStructType->HasMetaData(FBlueprintMetadata::MD_NativeMakeFunction))
			{
				const FString NativeMakePath = OutputStructType->GetMetaData(FBlueprintMetadata::MD_NativeMakeFunction);
				if (NativeMakePath.Contains(TargetFunction->GetName(), ESearchCase::IgnoreCase))
				{
					TArray<FLispNodePtr> MakeArgs;
					MakeArgs.Add(FLispNode::MakeSymbol(TEXT("make-struct")));
					MakeArgs.Add(FLispNode::MakeKeyword(TEXT(":struct")));
					MakeArgs.Add(FLispNode::MakeString(OutputStructType->GetPathName()));
					for (UEdGraphPin* Pin : CallNode->Pins)
					{
						if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
							|| Pin->PinName == UEdGraphSchema_K2::PN_Self || Pin->bHidden) continue;
						TArray<FLispNodePtr> FieldSpec;
						FieldSpec.Add(EXP_MakeNameAtom(Pin->PinName.ToString()));
						FieldSpec.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
						FieldSpec.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, ShortIds));
						MakeArgs.Add(FLispNode::MakeKeyword(TEXT(":field")));
						MakeArgs.Add(FLispNode::MakeList(FieldSpec));
					}
					Visited.Remove(SourceNode);
					return FLispNode::MakeList(MakeArgs);
				}
			}

			UEdGraphPin* StructInputPin = nullptr;
			UScriptStruct* InputStructType = nullptr;
			for (UEdGraphPin* Pin : CallNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
				{
					InputStructType = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
					if (InputStructType && InputStructType->HasMetaData(FBlueprintMetadata::MD_NativeBreakFunction))
					{
						StructInputPin = Pin;
						break;
					}
				}
			}
			if (StructInputPin && InputStructType)
			{
				const FString NativeBreakPath = InputStructType->GetMetaData(FBlueprintMetadata::MD_NativeBreakFunction);
				const bool bIsDeclaredNativeBreak = NativeBreakPath.Contains(TargetFunction->GetName(), ESearchCase::IgnoreCase);
				if (bIsDeclaredNativeBreak)
				{
					const UEdGraphPin* SelectedFieldPin = SourcePin->ParentPin ? SourcePin->ParentPin : SourcePin;
					TArray<FLispNodePtr> BreakArgs;
					BreakArgs.Add(FLispNode::MakeSymbol(TEXT("break-struct")));
					BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":struct")));
					BreakArgs.Add(EXP_MakeNameAtom(EXP_GetStableStructIdentifier(InputStructType)));
					BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":value")));
					BreakArgs.Add(ConvertPureExpressionToLisp(StructInputPin, Graph, Visited, ShortIds));
					BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":field")));
					BreakArgs.Add(EXP_MakeNameAtom(SelectedFieldPin->PinName.ToString()));
					Visited.Remove(SourceNode);
					return ShortIds ? AppendNodeId(FLispNode::MakeList(BreakArgs), SourceNode, *ShortIds) : FLispNode::MakeList(BreakArgs);
				}
			}
		}


		FString FuncName = GetCleanNodeName(SourceNode);
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(FuncName));
		if (Cast<UK2Node_Message>(CallNode))
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":call-kind")));
			Args.Add(FLispNode::MakeString(TEXT("message")));
		}
		else if (Cast<UK2Node_PromotableOperator>(CallNode))
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":call-kind")));
			Args.Add(FLispNode::MakeString(TEXT("promotable-operator")));
		}
		else if (Cast<UK2Node_AnimGetter>(CallNode))
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":call-kind")));
			Args.Add(FLispNode::MakeString(TEXT("anim-getter")));
		}
		if (UFunction* TargetFunction = CallNode->GetTargetFunction())
		{
			const FString OwnerToken = EXP_GetFunctionOwnerToken(TargetFunction, Graph);
			if (!OwnerToken.IsEmpty())
			{
				Args.Add(FLispNode::MakeKeyword(TEXT(":owner")));
				Args.Add(FLispNode::MakeString(OwnerToken));
			}
		}
		const UEdGraphPin* SelectedCallOutputPin = SourcePin->ParentPin ? SourcePin->ParentPin : SourcePin;
		if (SelectedCallOutputPin->PinName != UEdGraphSchema_K2::PN_ReturnValue)
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":out-pin")));
			Args.Add(FLispNode::MakeString(SelectedCallOutputPin->PinName.ToString()));
		}
		if (SelectedCallOutputPin->PinType.PinSubCategoryObject.IsValid())
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":result-type-object")));
			Args.Add(FLispNode::MakeString(SelectedCallOutputPin->PinType.PinSubCategoryObject->GetPathName()));
		}


		// Target object
		UEdGraphPin* SelfPin = SourceNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
		if (SelfPin && SelfPin->LinkedTo.Num() > 0)
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":self")));
			Args.Add(ConvertPureExpressionToLisp(SelfPin, Graph, Visited, ShortIds));
		}

		// Input data pins
		for (UEdGraphPin* Pin : SourceNode->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
			if (Pin->bHidden) continue;
			const FString StablePinKeyword = EXP_GetStablePinKeywordName(Pin);
			if (StablePinKeyword.IsEmpty()) continue;
			Args.Add(FLispNode::MakeKeyword(FString::Printf(TEXT(":%s"), *StablePinKeyword)));
			Args.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, ShortIds));
		}
		Visited.Remove(SourceNode);
		FLispNodePtr FunctionCallForm = ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);
		if (SourcePin->ParentPin && SourcePin->ParentPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			UScriptStruct* StructType = Cast<UScriptStruct>(SourcePin->ParentPin->PinType.PinSubCategoryObject.Get());
			FString FieldName = SourcePin->PinName.ToString();
			const FString GeneratedPrefix = SourcePin->ParentPin->PinName.ToString() + TEXT("_");
			if (FieldName.StartsWith(GeneratedPrefix)) FieldName.RightChopInline(GeneratedPrefix.Len());
			TArray<FLispNodePtr> BreakArgs;
			BreakArgs.Add(FLispNode::MakeSymbol(TEXT("break-struct")));
			BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":struct")));
			BreakArgs.Add(EXP_MakeNameAtom(EXP_GetStableStructIdentifier(StructType)));
			BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":value")));
			BreakArgs.Add(FunctionCallForm);
			BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":field")));
			BreakArgs.Add(EXP_MakeNameAtom(FieldName));
			return FLispNode::MakeList(BreakArgs);
		}
		return FunctionCallForm;

	}

	// MacroInstance output: export as (call-macro <name> [:param value]... :out (Pin Type))
	// MacroInstance is NOT a pure node, but its data output pins are accessed as pure expressions.
	if (UK2Node_MacroInstance* MacroInst = Cast<UK2Node_MacroInstance>(SourceNode))
	{
		if (Visited.Contains(SourceNode))
		{
			const FString ReusableValue = EXP_GetReusableValueSymbol(SourceNode, SourcePin);
			return ReusableValue.IsEmpty() ? FLispNode::MakeSymbol(TEXT("...circular...")) : FLispNode::MakeSymbol(ReusableValue);
		}
		Visited.Add(SourceNode);
		FLispNodePtr MacroForm = EXP_BuildMacroCallForm(MacroInst, SourcePin, false, Graph, Visited, ShortIds);

		Visited.Remove(SourceNode);
		return MacroForm;
	}


	if (UK2Node_MakeArray* MakeArrayNode = Cast<UK2Node_MakeArray>(SourceNode))
	{
		if (Visited.Contains(SourceNode)) return FLispNode::MakeSymbol(TEXT("...circular..."));
		Visited.Add(SourceNode);

		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("make-array")));
	for (UEdGraphPin* Pin : MakeArrayNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->ParentPin != nullptr) continue;
		Args.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, ShortIds));
	}
	Visited.Remove(SourceNode);
	return ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);

	}

	if (UK2Node_GetArrayItem* GetArrayItemNode = Cast<UK2Node_GetArrayItem>(SourceNode))
	{
		if (Visited.Contains(SourceNode)) return FLispNode::MakeSymbol(TEXT("...circular..."));
		Visited.Add(SourceNode);

		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("get-array-item")));
	if (UEdGraphPin* ArrayPin = GetArrayItemNode->GetTargetArrayPin())
	{
		Args.Add(ConvertPureExpressionToLisp(ArrayPin, Graph, Visited, ShortIds));
	}
	if (UEdGraphPin* IndexPin = GetArrayItemNode->GetIndexPin())
	{
		Args.Add(ConvertPureExpressionToLisp(IndexPin, Graph, Visited, ShortIds));
	}

	Visited.Remove(SourceNode);
	return ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);

	}

	if (UK2Node_Select* SelectNode = Cast<UK2Node_Select>(SourceNode))
	{
		if (Visited.Contains(SourceNode)) return FLispNode::MakeSymbol(TEXT("...circular..."));
		Visited.Add(SourceNode);

		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("select")));
		Args.Add(FLispNode::MakeKeyword(TEXT(":index")));
		Args.Add(ConvertPureExpressionToLisp(SelectNode->GetIndexPin(), Graph, Visited, ShortIds));
		if (const UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin())
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":result-type")));
			Args.Add(FLispNode::MakeSymbol(PinTypeToLispType(ReturnPin->PinType)));
			if (ReturnPin->PinType.PinSubCategoryObject.IsValid())
			{
				Args.Add(FLispNode::MakeKeyword(TEXT(":result-type-object")));
				Args.Add(FLispNode::MakeString(ReturnPin->PinType.PinSubCategoryObject->GetPathName()));
			}
		}

		TArray<UEdGraphPin*> OptionPins;
		SelectNode->GetOptionPins(OptionPins);
		for (UEdGraphPin* OptionPin : OptionPins)
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":option")));
			Args.Add(ConvertPureExpressionToLisp(OptionPin, Graph, Visited, ShortIds));
		}

		Visited.Remove(SourceNode);
		return ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);
	}

	if (UK2Node_AnimNodeReference* AnimNodeReference = Cast<UK2Node_AnimNodeReference>(SourceNode))
	{
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("anim-node-reference")));
		Args.Add(FLispNode::MakeKeyword(TEXT(":tag")));
		FName Tag = NAME_None;
		if (const FNameProperty* TagProperty = FindFProperty<FNameProperty>(AnimNodeReference->GetClass(), TEXT("Tag")))
		{
			Tag = TagProperty->GetPropertyValue_InContainer(AnimNodeReference);
		}
		Args.Add(FLispNode::MakeString(Tag.ToString()));
		return ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);
	}

	if (UK2Node_GetEnumeratorName* EnumNameNode = Cast<UK2Node_GetEnumeratorName>(SourceNode))
	{
		if (Visited.Contains(SourceNode)) return FLispNode::MakeSymbol(TEXT("...circular..."));
		Visited.Add(SourceNode);
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(SourceNode->IsA<UK2Node_GetEnumeratorNameAsString>()
			? TEXT("enum-to-string") : TEXT("enum-to-name")));
		for (UEdGraphPin* Pin : EnumNameNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				Args.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, ShortIds));
				break;
			}
		}
		Visited.Remove(SourceNode);
		return ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);
	}

	if (SourceNode->GetClass()->GetName() == TEXT("K2Node_PropertyAccess"))
	{
		if (Visited.Contains(SourceNode)) return FLispNode::MakeSymbol(TEXT("...circular..."));
		Visited.Add(SourceNode);
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("property-access")));
		if (FArrayProperty* PathProperty = FindFProperty<FArrayProperty>(SourceNode->GetClass(), TEXT("Path")))
		{
			FScriptArrayHelper PathHelper(PathProperty, PathProperty->ContainerPtrToValuePtr<void>(SourceNode));
			TArray<FLispNodePtr> PathItems;
			for (int32 PathIndex = 0; PathIndex < PathHelper.Num(); ++PathIndex)
			{
				if (FStrProperty* PathElementProperty = CastField<FStrProperty>(PathProperty->Inner))
				{
					PathItems.Add(FLispNode::MakeString(PathElementProperty->GetPropertyValue(PathHelper.GetRawPtr(PathIndex))));
				}
			}
			Args.Add(FLispNode::MakeKeyword(TEXT(":path")));
			Args.Add(FLispNode::MakeList(PathItems));
		}
		if (FNameProperty* ContextProperty = FindFProperty<FNameProperty>(SourceNode->GetClass(), TEXT("ContextId")))
		{
			const FName ContextId = ContextProperty->GetPropertyValue_InContainer(SourceNode);
			if (!ContextId.IsNone())
			{
				Args.Add(FLispNode::MakeKeyword(TEXT(":context")));
				Args.Add(FLispNode::MakeString(ContextId.ToString()));
			}
		}
		Visited.Remove(SourceNode);
		FLispNodePtr PropertyExpression = FLispNode::MakeList(Args);
		if (ShortIds)
		{
			PropertyExpression = AppendNodeId(PropertyExpression, SourceNode, *ShortIds);
		}
		if (SourcePin->ParentPin && SourcePin->ParentPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			UScriptStruct* StructType = Cast<UScriptStruct>(SourcePin->ParentPin->PinType.PinSubCategoryObject.Get());
			FString FieldName = SourcePin->PinName.ToString();
			const FString Prefix = SourcePin->ParentPin->PinName.ToString() + TEXT("_");
			if (FieldName.StartsWith(Prefix)) FieldName.RightChopInline(Prefix.Len());

			TArray<FLispNodePtr> BreakArgs;
			BreakArgs.Add(FLispNode::MakeSymbol(TEXT("break-struct")));
			BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":struct")));
			BreakArgs.Add(EXP_MakeNameAtom(StructType ? EXP_GetStableStructIdentifier(StructType) : TEXT("struct")));
			BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":value")));
			BreakArgs.Add(PropertyExpression);
			BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":field")));
			BreakArgs.Add(EXP_MakeNameAtom(FieldName));
			return FLispNode::MakeList(BreakArgs);
		}
		return PropertyExpression;
	}

	if (UK2Node_BreakStruct* BreakStructNode = Cast<UK2Node_BreakStruct>(SourceNode))
	{
		if (Visited.Contains(SourceNode)) return FLispNode::MakeSymbol(TEXT("...circular..."));
		Visited.Add(SourceNode);

		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("break-struct")));
		Args.Add(FLispNode::MakeKeyword(TEXT(":struct")));
		Args.Add(EXP_MakeNameAtom(BreakStructNode->StructType
			? EXP_GetStableStructIdentifier(BreakStructNode->StructType)
			: BreakStructNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));

		for (UEdGraphPin* Pin : BreakStructNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			Args.Add(FLispNode::MakeKeyword(TEXT(":value")));
			Args.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, ShortIds));
			break;
		}

		Args.Add(FLispNode::MakeKeyword(TEXT(":field")));
		Args.Add(EXP_MakeNameAtom(SourcePin->PinName.ToString()));
		Visited.Remove(SourceNode);
		return ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);

	}

	// Generic K2Node pure node (e.g. UK2Node_EnumEquality, UK2Node_EnumInequality, etc.)
	// These derive from UK2Node but not UK2Node_CallFunction, yet they are pure and output values.
	if (UK2Node* K2Node = Cast<UK2Node>(SourceNode))
	{
		if (K2Node->IsNodePure())
		{
			if (Visited.Contains(SourceNode)) return FLispNode::MakeSymbol(TEXT("...circular..."));
			Visited.Add(SourceNode);

			// Use compact node title (e.g. "!=" for EnumInequality) if available, else class name
			FString NodeName = K2Node->GetCompactNodeTitle().ToString();
			if (NodeName.IsEmpty())
				NodeName = SourceNode->GetClass()->GetName();

			TArray<FLispNodePtr> Args;
			Args.Add(FLispNode::MakeSymbol(NodeName));

			for (UEdGraphPin* Pin : SourceNode->Pins)
			{
				if (Pin->Direction != EGPD_Input) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
				Args.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, ShortIds));
		}
		Visited.Remove(SourceNode);
		FLispNodePtr CallForm = ShortIds ? AppendNodeId(FLispNode::MakeList(Args), SourceNode, *ShortIds) : FLispNode::MakeList(Args);
		if (SourcePin->ParentPin && SourcePin->ParentPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			UScriptStruct* StructType = Cast<UScriptStruct>(SourcePin->ParentPin->PinType.PinSubCategoryObject.Get());
			FString FieldName = SourcePin->PinName.ToString();
			const FString GeneratedPrefix = SourcePin->ParentPin->PinName.ToString() + TEXT("_");
			if (FieldName.StartsWith(GeneratedPrefix)) FieldName.RightChopInline(GeneratedPrefix.Len());
			TArray<FLispNodePtr> BreakArgs;
			BreakArgs.Add(FLispNode::MakeSymbol(TEXT("break-struct")));
			BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":struct")));
			BreakArgs.Add(EXP_MakeNameAtom(EXP_GetStableStructIdentifier(StructType)));
			BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":value")));
			BreakArgs.Add(CallForm);
			BreakArgs.Add(FLispNode::MakeKeyword(TEXT(":field")));
			BreakArgs.Add(EXP_MakeNameAtom(FieldName));
			return FLispNode::MakeList(BreakArgs);
		}
		return CallForm;

	}
	}


	// Fallback for non-pure nodes: return node class name as opaque symbol
	{
		FString ClassName = SourceNode->GetClass()->GetName();
		EXP_AddExportError(FString::Printf(
			TEXT("Pure expression source '%s' (%s) feeding pin '%s' cannot be represented by BlueprintLisp"),
			*SourceNode->GetName(), *ClassName, *ValuePin->PinName.ToString()));
		return FLispNode::MakeSymbol(ClassName);
	}
}

// ----- Convert a single exec node to Lisp -----
static FLispNodePtr ConvertNodeToLisp(UEdGraphNode* Node, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited, bool bPositions, const TMap<FGuid, FString>& ShortIds)
{
	if (!Node) return FLispNode::MakeNil();
	if (Visited.Contains(Node)) return FLispNode::MakeNil();
	Visited.Add(Node);

	// ---- function result ----
	if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
	{
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("return")));
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input
				|| Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				|| Pin->ParentPin || (Pin->bHidden && Pin->SubPins.IsEmpty())
				|| Pin->PinName == UEdGraphSchema_K2::PN_Execute)
			{
				continue;
			}

			TArray<FLispNodePtr> ValuePair;
			ValuePair.Add(EXP_MakeNameAtom(Pin->PinName.ToString()));
			FLispNodePtr ValueExpression;
			if (Pin->SubPins.Num() > 0 && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				TArray<FLispNodePtr> MakeStructArgs;
				MakeStructArgs.Add(FLispNode::MakeSymbol(TEXT("make-struct")));
				MakeStructArgs.Add(FLispNode::MakeKeyword(TEXT(":struct")));
				MakeStructArgs.Add(FLispNode::MakeString(Pin->PinType.PinSubCategoryObject.IsValid()
					? Pin->PinType.PinSubCategoryObject->GetPathName() : TEXT("")));
				for (UEdGraphPin* SubPin : Pin->SubPins)
				{
					if (!SubPin) continue;
					FString FieldName = SubPin->PinName.ToString();
					const FString Prefix = Pin->PinName.ToString() + TEXT("_");
					if (FieldName.StartsWith(Prefix)) FieldName.RightChopInline(Prefix.Len());
					TArray<FLispNodePtr> FieldSpec;
					FieldSpec.Add(EXP_MakeNameAtom(FieldName));
					FieldSpec.Add(FLispNode::MakeSymbol(PinTypeToLispType(SubPin->PinType)));
					FieldSpec.Add(ConvertPureExpressionToLisp(SubPin, Graph, Visited, &ShortIds));
					MakeStructArgs.Add(FLispNode::MakeKeyword(TEXT(":field")));
					MakeStructArgs.Add(FLispNode::MakeList(FieldSpec));
				}
				ValueExpression = FLispNode::MakeList(MakeStructArgs);
			}
			else
			{
				ValueExpression = ConvertPureExpressionToLisp(Pin, Graph, Visited, &ShortIds);
			}
			if (Pin->LinkedTo.Num() > 0)
			{
				UEdGraphNode* SourceNode = Pin->LinkedTo[0] ? Pin->LinkedTo[0]->GetOwningNode() : nullptr;
				const bool bOpaqueSource = SourceNode && ValueExpression.IsValid() && ValueExpression->IsSymbol()
					&& (ValueExpression->StringValue == SourceNode->GetClass()->GetName()
						|| ValueExpression->StringValue == TEXT("...circular..."));
				if (!ValueExpression.IsValid() || ValueExpression->IsNil() || bOpaqueSource)
				{
					EXP_AddExportError(FString::Printf(
						TEXT("FunctionResult '%s' pin '%s' has a connected value source that BlueprintLisp cannot represent"),
						*ResultNode->GetName(), *Pin->PinName.ToString()));
				}
			}
			ValuePair.Add(ValueExpression.IsValid() ? ValueExpression : FLispNode::MakeNil());
			Args.Add(FLispNode::MakeKeyword(TEXT(":value")));
			Args.Add(FLispNode::MakeList(ValuePair));
		}
		return AppendNodeId(FLispNode::MakeList(Args), ResultNode, ShortIds);
	}

	// ---- branch ----
	if (UK2Node_IfThenElse* BranchNode = Cast<UK2Node_IfThenElse>(Node))
	{
		UEdGraphPin* CondPin  = BranchNode->GetConditionPin();
		UEdGraphPin* TruePin  = BranchNode->GetThenPin();
		UEdGraphPin* FalsePin = BranchNode->GetElsePin();

		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("branch")));
		Args.Add(ConvertPureExpressionToLisp(CondPin, Graph, Visited, &ShortIds));

		Args.Add(FLispNode::MakeKeyword(TEXT(":true")));
		Args.Add(ConvertExecChainToLisp(TruePin, Graph, Visited, bPositions, ShortIds));
		Args.Add(FLispNode::MakeKeyword(TEXT(":false")));
		Args.Add(ConvertExecChainToLisp(FalsePin, Graph, Visited, bPositions, ShortIds));
		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}

	// GenericCreateObject is impure while its result is consumed as data. Preserve
	// the node and output binding in one form so both flows resolve to one instance.
	if (UK2Node_GenericCreateObject* CreateObjectNode = Cast<UK2Node_GenericCreateObject>(Node))
	{
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("create-object")));
		Args.Add(FLispNode::MakeKeyword(TEXT(":class")));
		Args.Add(FLispNode::MakeString(CreateObjectNode->GetClassToSpawn()
			? CreateObjectNode->GetClassToSpawn()->GetPathName() : TEXT("")));

		UEdGraphPin* ClassPin = CreateObjectNode->GetClassPin();
		UEdGraphPin* OuterPin = CreateObjectNode->GetOuterPin();
		if (OuterPin)
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":outer")));
			Args.Add(ConvertPureExpressionToLisp(OuterPin, Graph, Visited, &ShortIds));
		}
		for (UEdGraphPin* Pin : CreateObjectNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				|| Pin == ClassPin || Pin == OuterPin || Pin->bHidden || Pin->ParentPin)
			{
				continue;
			}
			TArray<FLispNodePtr> InputSpec;
			InputSpec.Add(EXP_MakeNameAtom(Pin->PinName.ToString()));
			InputSpec.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
			InputSpec.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, &ShortIds));
			Args.Add(FLispNode::MakeKeyword(TEXT(":input")));
			Args.Add(FLispNode::MakeList(InputSpec));
		}
		if (UEdGraphPin* ResultPin = CreateObjectNode->GetResultPin())
		{
			TArray<FLispNodePtr> OutputSpec;
			OutputSpec.Add(EXP_MakeNameAtom(ResultPin->PinName.ToString()));
			OutputSpec.Add(FLispNode::MakeSymbol(PinTypeToLispType(ResultPin->PinType)));
			Args.Add(FLispNode::MakeKeyword(TEXT(":out")));
			Args.Add(FLispNode::MakeList(OutputSpec));
		}
		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}

	// Chooser is an impure multi-output node. Its execution and data outputs must be
	// represented by one form so downstream symbols resolve to the same node.
	if (Node->GetClass()->GetName() == TEXT("K2Node_EvaluateChooser2"))
	{
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("evaluate-chooser")));

		if (FObjectPropertyBase* ChooserProperty = FindFProperty<FObjectPropertyBase>(Node->GetClass(), TEXT("Chooser")))
		{
			if (UObject* Chooser = ChooserProperty->GetObjectPropertyValue_InContainer(Node))
			{
				TArray<FLispNodePtr> AssetForm;
				AssetForm.Add(FLispNode::MakeSymbol(TEXT("asset")));
				AssetForm.Add(FLispNode::MakeString(Chooser->GetPathName()));
				Args.Add(FLispNode::MakeKeyword(TEXT(":chooser")));
				Args.Add(FLispNode::MakeList(AssetForm));
			}
		}

		auto AddReflectedProperty = [&Args, Node](const TCHAR* PropertyName, const TCHAR* Keyword)
		{
			if (FProperty* Property = Node->GetClass()->FindPropertyByName(PropertyName))
			{
				FString Value;
				const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);
				Property->ExportText_Direct(Value, ValuePtr, nullptr, Node, PPF_None);
				if (!Value.IsEmpty())
				{
					Args.Add(FLispNode::MakeKeyword(Keyword));
					if (Property->IsA<FBoolProperty>()) Args.Add(FLispNode::MakeSymbol(Value.ToLower()));
					else Args.Add(FLispNode::MakeString(Value));
				}
			}
		};
		AddReflectedProperty(TEXT("Mode"), TEXT(":mode"));
		AddReflectedProperty(TEXT("StructOutputMode"), TEXT(":struct-output-mode"));
		AddReflectedProperty(TEXT("bReturnSoftObjectReference"), TEXT(":return-soft-object"));

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->bHidden || Pin->ParentPin)
			{
				continue;
			}
			TArray<FLispNodePtr> PinSpec;
			PinSpec.Add(EXP_MakeNameAtom(Pin->PinName.ToString()));
			PinSpec.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
			if (Pin->Direction == EGPD_Input)
			{
				PinSpec.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, &ShortIds));
				Args.Add(FLispNode::MakeKeyword(TEXT(":input")));
			}
			else
			{
				Args.Add(FLispNode::MakeKeyword(TEXT(":out")));
			}
			Args.Add(FLispNode::MakeList(PinSpec));
		}
		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}

	if (UK2Node_SetFieldsInStruct* SetFieldsNode = Cast<UK2Node_SetFieldsInStruct>(Node))
	{
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("set-struct-fields")));
		Args.Add(FLispNode::MakeKeyword(TEXT(":struct")));
		Args.Add(FLispNode::MakeString(SetFieldsNode->StructType ? SetFieldsNode->StructType->GetPathName() : TEXT("")));
		if (UEdGraphPin* StructRefPin = SetFieldsNode->FindPin(TEXT("StructRef"), EGPD_Input))
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":target")));
			Args.Add(ConvertPureExpressionToLisp(StructRefPin, Graph, Visited, &ShortIds));
		}
		for (UEdGraphPin* Pin : SetFieldsNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				|| Pin->PinName == TEXT("StructRef") || Pin->bHidden || Pin->ParentPin)
			{
				continue;
			}
			TArray<FLispNodePtr> FieldSpec;
			FieldSpec.Add(EXP_MakeNameAtom(Pin->PinName.ToString()));
			FieldSpec.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
			FieldSpec.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, &ShortIds));
			Args.Add(FLispNode::MakeKeyword(TEXT(":field")));
			Args.Add(FLispNode::MakeList(FieldSpec));
		}
		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}

	// ---- set variable ----
	if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(Node))
	{
		FString VarName = VarSet->VariableReference.GetMemberName().ToString();
		UEdGraphPin* ValuePin = VarSet->FindPin(VarName, EGPD_Input);
		if (!ValuePin)
			for (UEdGraphPin* P : VarSet->Pins)
				if (P->Direction == EGPD_Input && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					{ ValuePin = P; break; }

		TArray<FLispNodePtr> Args;
		const bool bLocalVariable = VarSet->VariableReference.IsLocalScope();
		Args.Add(FLispNode::MakeSymbol(bLocalVariable ? TEXT("set-local") : TEXT("set")));
		Args.Add(EXP_MakeNameAtom(VarName));
		if (bLocalVariable)
		{
			Args.Add(FLispNode::MakeSymbol(ValuePin ? PinTypeToLispType(ValuePin->PinType) : TEXT("wildcard")));
		}
		if (ValuePin && ValuePin->SubPins.Num() > 0 && ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			TArray<FLispNodePtr> MakeStructArgs;
			MakeStructArgs.Add(FLispNode::MakeSymbol(TEXT("make-struct")));
			MakeStructArgs.Add(FLispNode::MakeKeyword(TEXT(":struct")));
			MakeStructArgs.Add(FLispNode::MakeString(ValuePin->PinType.PinSubCategoryObject.IsValid()
				? ValuePin->PinType.PinSubCategoryObject->GetPathName() : TEXT("")));
			for (UEdGraphPin* SubPin : ValuePin->SubPins)
			{
				if (!SubPin) continue;
				FString FieldName = SubPin->PinName.ToString();
				const FString Prefix = ValuePin->PinName.ToString() + TEXT("_");
				if (FieldName.StartsWith(Prefix)) FieldName.RightChopInline(Prefix.Len());
				TArray<FLispNodePtr> FieldSpec;
				FieldSpec.Add(EXP_MakeNameAtom(FieldName));
				FieldSpec.Add(FLispNode::MakeSymbol(PinTypeToLispType(SubPin->PinType)));
				FieldSpec.Add(ConvertPureExpressionToLisp(SubPin, Graph, Visited, &ShortIds));
				MakeStructArgs.Add(FLispNode::MakeKeyword(TEXT(":field")));
				MakeStructArgs.Add(FLispNode::MakeList(FieldSpec));
			}
			Args.Add(FLispNode::MakeList(MakeStructArgs));
		}
		else
		{
			Args.Add(ConvertPureExpressionToLisp(ValuePin, Graph, Visited, &ShortIds));
		}

		if (!bLocalVariable && !VarSet->VariableReference.IsSelfContext())
		{
			if (const UClass* OwnerClass = VarSet->VariableReference.GetMemberParentClass())
			{
				Args.Add(FLispNode::MakeKeyword(TEXT(":owner")));
				Args.Add(FLispNode::MakeString(OwnerClass->GetPathName()));
			}
			if (UEdGraphPin* SelfPin = VarSet->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input))
			{
				Args.Add(FLispNode::MakeKeyword(TEXT(":self")));
				Args.Add(ConvertPureExpressionToLisp(SelfPin, Graph, Visited, &ShortIds));
			}
		}


		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}

	// ---- function call ----
	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		if (UFunction* TargetFunction = CallNode->GetTargetFunction())
		{
			if (!TargetFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable) && !TargetFunction->HasAnyFunctionFlags(FUNC_BlueprintPure))
			{
				FString FuncName = TargetFunction->GetName();
				TArray<FLispNodePtr> Args;
				Args.Add(FLispNode::MakeSymbol(TEXT("call-parent")));
				Args.Add(EXP_MakeNameAtom(FuncName));

				for (UEdGraphPin* Pin : CallNode->Pins)
				{
					if (Pin->Direction != EGPD_Input) continue;
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
					if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
					if (Pin->bHidden) continue;
					FLispNodePtr Val = ConvertPureExpressionToLisp(Pin, Graph, Visited, &ShortIds);

					if (!Val->IsNil())
					{
						const FString StablePinKeyword = EXP_GetStablePinKeywordName(Pin);
						if (StablePinKeyword.IsEmpty()) continue;
						Args.Add(FLispNode::MakeKeyword(FString::Printf(TEXT(":%s"), *StablePinKeyword)));
						Args.Add(Val);
					}
				}

				TArray<FLispNodePtr> OutPins;
				for (UEdGraphPin* Pin : CallNode->Pins)
					if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
						OutPins.Add(FLispNode::MakeSymbol(Pin->PinName.ToString().ToLower()));

				if (OutPins.Num() == 1)
				{
					TArray<FLispNodePtr> Let;
					Let.Add(FLispNode::MakeSymbol(TEXT("let")));
					Let.Add(OutPins[0]);
					Let.Add(AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds));
					return FLispNode::MakeList(Let);
				}

				return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
			}
		}

		FString FuncName = GetCleanNodeName(Node);
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(FuncName));
		if (Cast<UK2Node_Message>(CallNode))
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":call-kind")));
			Args.Add(FLispNode::MakeString(TEXT("message")));
		}
		else if (Cast<UK2Node_AnimGetter>(CallNode))
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":call-kind")));
			Args.Add(FLispNode::MakeString(TEXT("anim-getter")));
		}
		if (UFunction* TargetFunction = CallNode->GetTargetFunction())
		{
			const FString OwnerToken = EXP_GetFunctionOwnerToken(TargetFunction, Graph);
			if (!OwnerToken.IsEmpty())
			{
				Args.Add(FLispNode::MakeKeyword(TEXT(":owner")));
				Args.Add(FLispNode::MakeString(OwnerToken));
			}
		}


		// Target object (self pin)
		UEdGraphPin* SelfPin = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
		if (SelfPin && SelfPin->LinkedTo.Num() > 0)
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":self")));
			Args.Add(ConvertPureExpressionToLisp(SelfPin, Graph, Visited, &ShortIds));
		}


		// Input data pins
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
			if (Pin->bHidden) continue;
			FLispNodePtr Val = ConvertPureExpressionToLisp(Pin, Graph, Visited, &ShortIds);

			if (!Val->IsNil())
			{
				const FString StablePinKeyword = EXP_GetStablePinKeywordName(Pin);
				if (StablePinKeyword.IsEmpty()) continue;
				Args.Add(FLispNode::MakeKeyword(FString::Printf(TEXT(":%s"), *StablePinKeyword)));
				Args.Add(Val);
			}
		}

		// Output: wrap in (let result ...)
		TArray<FLispNodePtr> OutPins;
		for (UEdGraphPin* Pin : Node->Pins)
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				OutPins.Add(FLispNode::MakeSymbol(Pin->PinName.ToString().ToLower()));

		if (OutPins.Num() == 1)
		{
			TArray<FLispNodePtr> Let;
			Let.Add(FLispNode::MakeSymbol(TEXT("let")));
			Let.Add(OutPins[0]);
			Let.Add(AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds));
			return FLispNode::MakeList(Let);
		}

		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}

	// ---- sequence ----
	if (UK2Node_ExecutionSequence* SeqNode = Cast<UK2Node_ExecutionSequence>(Node))
	{
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("seq")));
		for (UEdGraphPin* Pin : SeqNode->Pins)
		{
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				FLispNodePtr Body = ConvertExecChainToLisp(Pin, Graph, Visited, bPositions, ShortIds);
				if (!Body.IsValid() || Body->IsNil())
				{
					continue;
				}
				if (BP_IsStructuralSeqWrapper(Body))
				{
					for (int32 BodyIdx = 1; BodyIdx < Body->Num(); ++BodyIdx)
					{
						Args.Add(Body->Get(BodyIdx));
					}
				}
				else
				{
					Args.Add(Body);
				}

			}
		}
		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}


	// ---- dynamic cast ----
	if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		FString TypeName = CastNode->TargetType ? CastNode->TargetType->GetName() : TEXT("?");
		UEdGraphPin* ObjPin = CastNode->GetCastSourcePin();
		UEdGraphPin* SuccessPin = CastNode->GetValidCastPin();

		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("cast")));
		Args.Add(FLispNode::MakeSymbol(TypeName));
		Args.Add(ConvertPureExpressionToLisp(ObjPin, Graph, Visited, &ShortIds));

		FLispNodePtr SuccBody = ConvertExecChainToLisp(SuccessPin, Graph, Visited, bPositions, ShortIds);
		if (SuccBody.IsValid() && !SuccBody->IsNil()) Args.Add(SuccBody);
		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}

	// ---- switch integer ----
	if (UK2Node_SwitchEnum* SwitchEnum = Cast<UK2Node_SwitchEnum>(Node))
	{
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("switch-enum")));
		Args.Add(FLispNode::MakeString(SwitchEnum->GetEnum() ? SwitchEnum->GetEnum()->GetPathName() : TEXT("")));
		Args.Add(ConvertPureExpressionToLisp(SwitchEnum->GetSelectionPin(), Graph, Visited, &ShortIds));
		for (UEdGraphPin* Pin : SwitchEnum->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
				|| Pin == SwitchEnum->GetDefaultPin())
			{
				continue;
			}
			TArray<FLispNodePtr> CasePair;
			CasePair.Add(FLispNode::MakeString(Pin->PinName.ToString()));
			CasePair.Add(ConvertExecChainToLisp(Pin, Graph, Visited, bPositions, ShortIds));
			Args.Add(FLispNode::MakeKeyword(TEXT(":case")));
			Args.Add(FLispNode::MakeList(CasePair));
		}
		if (UEdGraphPin* DefaultPin = SwitchEnum->GetDefaultPin())
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":default")));
			Args.Add(ConvertExecChainToLisp(DefaultPin, Graph, Visited, bPositions, ShortIds));
		}
		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}

	// ---- switch integer ----
	if (UK2Node_SwitchInteger* SwitchInt = Cast<UK2Node_SwitchInteger>(Node))
	{
		UEdGraphPin* SelPin = SwitchInt->GetSelectionPin();
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("switch-int")));
		Args.Add(ConvertPureExpressionToLisp(SelPin, Graph, Visited, &ShortIds));

		for (UEdGraphPin* Pin : SwitchInt->Pins)
		{
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				&& Pin->PinName != TEXT("default"))
			{
				Args.Add(FLispNode::MakeKeyword(FString::Printf(TEXT(":%s"), *Pin->PinName.ToString())));
				Args.Add(ConvertExecChainToLisp(Pin, Graph, Visited, bPositions, ShortIds));
			}
		}
		UEdGraphPin* DefaultPin = SwitchInt->GetDefaultPin();
		if (DefaultPin)
		{
			Args.Add(FLispNode::MakeKeyword(TEXT(":default")));
			Args.Add(ConvertExecChainToLisp(DefaultPin, Graph, Visited, bPositions, ShortIds));
		}
		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}

	// ---- macro instance (call-macro) ----
	if (UK2Node_MacroInstance* MacroInst = Cast<UK2Node_MacroInstance>(Node))
	{
		// MacroInstance inherits from UK2Node_Tunnel but is NOT an exit tunnel.
		// Export as (call-macro <name> [:param value]... [:out (Pin Type)]...) with exec-chain continuation.
		return AppendNodeId(EXP_BuildMacroCallForm(MacroInst, nullptr, true, Graph, Visited, &ShortIds), Node, ShortIds);

	}


	// ---- macro exit tunnel ----
	// IMPORTANT: Must check MacroInstance BEFORE Tunnel, because MacroInstance inherits from Tunnel.
	// At this point we know it's a Tunnel but NOT a MacroInstance.
	if (UK2Node_Tunnel* TunnelNode = Cast<UK2Node_Tunnel>(Node))
	{
		if (!TunnelNode->DrawNodeAsEntry())
		{
			// Exit tunnel: output (exit <name> [:output (name value)]...)
			FString ExitName = TunnelNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
			TArray<FLispNodePtr> Args;
			Args.Add(FLispNode::MakeSymbol(TEXT("exit")));
			Args.Add(EXP_MakeNameAtom(ExitName.IsEmpty() ? TEXT("") : ExitName));


			// Output pins on exit tunnel = macro output values
			for (UEdGraphPin* Pin : TunnelNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
					&& !Pin->bHidden && Pin->PinName != UEdGraphSchema_K2::PN_Execute)
				{
					Args.Add(FLispNode::MakeKeyword(TEXT(":output")));
					TArray<FLispNodePtr> OutPair;
					OutPair.Add(EXP_MakeNameAtom(Pin->PinName.ToString()));
					OutPair.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited, &ShortIds));


					Args.Add(FLispNode::MakeList(OutPair));
				}
			}

			return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
		}
	}

	// ---- fallback: generic call representation ----
	FString NodeLabel = Node->GetNodeTitle(ENodeTitleType::ListView).ToString()
		.Replace(TEXT(" "), TEXT("-")).ToLower();
	TArray<FLispNodePtr> FallbackArgs;
	FallbackArgs.Add(FLispNode::MakeSymbol(NodeLabel.IsEmpty() ? TEXT("node") : NodeLabel));
	return AppendNodeId(FLispNode::MakeList(FallbackArgs), Node, ShortIds);
}

// ----- Follow an exec chain and emit a Lisp form list -----
static FLispNodePtr ConvertExecChainToLisp(UEdGraphPin* ExecPin, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited, bool bPositions, const TMap<FGuid, FString>& ShortIds)
{
	if (!ExecPin || ExecPin->LinkedTo.Num() == 0) return FLispNode::MakeNil();

	TArray<FLispNodePtr> Statements;
	UEdGraphPin* CurrentPin = ExecPin;

	while (CurrentPin && CurrentPin->LinkedTo.Num() > 0)
	{
		UEdGraphNode* NextNode = CurrentPin->LinkedTo[0]->GetOwningNode();
		if (!NextNode || Visited.Contains(NextNode)) break;

		if (UK2Node_Knot* KnotNode = Cast<UK2Node_Knot>(NextNode))
		{
			UEdGraphPin* KnotInput = KnotNode->GetInputPin();
			UEdGraphPin* KnotOutput = KnotNode->GetOutputPin();
			if (KnotInput && KnotOutput
				&& KnotInput->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				&& KnotOutput->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				Visited.Add(KnotNode);
				CurrentPin = KnotOutput;
				continue;
			}
		}

		// Skip entry tunnel nodes (they are handled as macro entry points, not as chain nodes)
		// But MacroInstance (which inherits Tunnel) should NOT be skipped — it's a call node.
		if (UK2Node_Tunnel* TE = Cast<UK2Node_Tunnel>(NextNode))
		{
			if (TE->DrawNodeAsEntry() && !Cast<UK2Node_MacroInstance>(NextNode)) break;
		}

		FLispNodePtr NodeLisp = ConvertNodeToLisp(NextNode, Graph, Visited, bPositions, ShortIds);
		if (NodeLisp.IsValid() && !NodeLisp->IsNil())
			Statements.Add(NodeLisp);

		// branch / sequence terminate the chain (their downstream exec paths are handled inside ConvertNodeToLisp)
		if (Cast<UK2Node_IfThenElse>(NextNode) || Cast<UK2Node_ExecutionSequence>(NextNode)) break;

		// Exit tunnel terminates the chain (macro exit point)

		// But MacroInstance is NOT an exit tunnel — it continues the exec chain.
		if (UK2Node_Tunnel* TE = Cast<UK2Node_Tunnel>(NextNode))
		{
			if (!TE->DrawNodeAsEntry() && !Cast<UK2Node_MacroInstance>(NextNode)) break;
		}

		CurrentPin = GetThenPin(NextNode);
	}

	if (Statements.Num() == 0) return FLispNode::MakeNil();
	if (Statements.Num() == 1) return Statements[0];

	// Multiple statements: wrap in seq
	TArray<FLispNodePtr> Seq;
	Seq.Add(FLispNode::MakeSymbol(TEXT("seq")));
	Seq.Append(Statements);
	return FLispNode::MakeList(Seq);
}

static void AppendEventMetadata(TArray<FLispNodePtr>& EventArgs, UEdGraphNode* EventNode, bool bPositions,
	const TMap<FGuid, FString>& ShortEventIds)
{
	if (!EventNode) return;

	if (const FString* EId = ShortEventIds.Find(EventNode->NodeGuid))
	{
		EventArgs.Add(FLispNode::MakeKeyword(TEXT(":event-id")));
		EventArgs.Add(FLispNode::MakeString(*EId));
	}

	if (bPositions)
	{
		EventArgs.Add(FLispNode::MakeKeyword(TEXT(":pos")));
		EventArgs.Add(FLispNode::MakeString(FString::Printf(TEXT("%d,%d"), EventNode->NodePosX, EventNode->NodePosY)));
	}
}

static void AppendTruthyKeyword(TArray<FLispNodePtr>& EventArgs, const TCHAR* Keyword, bool bValue)
{
	if (!bValue) return;
	EventArgs.Add(FLispNode::MakeKeyword(Keyword));
	EventArgs.Add(FLispNode::MakeSymbol(TEXT("true")));
}

static void AppendExecBodyToArgs(TArray<FLispNodePtr>& EventArgs, UEdGraphPin* ExecOutPin, UEdGraph* Graph, bool bPositions,
	const TMap<FGuid, FString>& ShortNodeIds)
{
	TSet<UEdGraphNode*> Visited;
	FLispNodePtr Body = ConvertExecChainToLisp(ExecOutPin, Graph, Visited, bPositions, ShortNodeIds);
	if (!Body.IsValid() || Body->IsNil()) return;

	if (BP_IsStructuralSeqWrapper(Body))
	{
		for (int32 i = 1; i < Body->Num(); i++)
		{
			EventArgs.Add(Body->Get(i));
		}
	}
	else
	{
		EventArgs.Add(Body);
	}

}

static bool EXP_ShouldSkipCustomEventParamPin(UK2Node_CustomEvent* Event, UEdGraphPin* Pin)
{
	if (!Event || !Pin)
	{
		return true;
	}
	if (Pin->Direction != EGPD_Output || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->bHidden)
	{
		return true;
	}

	const bool bIsImplicitDelegatePin = (Pin->PinName == UK2Node_Event::DelegateOutputName
		&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate);
	if (!bIsImplicitDelegatePin)
	{
		return false;
	}

	for (UEdGraphPin* OtherPin : Event->Pins)
	{
		if (!OtherPin || OtherPin == Pin)
		{
			continue;
		}
		if (OtherPin->Direction != EGPD_Output || OtherPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || OtherPin->bHidden)
		{
			continue;
		}
		return false;
	}

	return true;
}

// ----- Convert a standard K2Node_Event -----

static FLispNodePtr ConvertEventToLisp(UK2Node_Event* Event, UEdGraph* Graph, bool bPositions,
	const TMap<FGuid, FString>& ShortEventIds, const TMap<FGuid, FString>& ShortNodeIds)
{
	FString EventName = Event->EventReference.GetMemberName().ToString();
	if (EventName.IsEmpty()) EventName = Event->CustomFunctionName.ToString();
	if (EventName.IsEmpty()) EventName = Event->GetNodeTitle(ENodeTitleType::ListView).ToString();

	TArray<FLispNodePtr> EventArgs;
	EventArgs.Add(FLispNode::MakeSymbol(TEXT("event")));
	EventArgs.Add(EXP_MakeNameAtom(EventName));
	AppendEventMetadata(EventArgs, Event, bPositions, ShortEventIds);

	AppendExecBodyToArgs(EventArgs, GetThenPin(Event), Graph, bPositions, ShortNodeIds);
	return FLispNode::MakeList(EventArgs);
}

static FLispNodePtr ConvertInputActionToLisp(UK2Node_InputAction* Event, UEdGraph* Graph, bool bPositions,
	const TMap<FGuid, FString>& ShortEventIds, const TMap<FGuid, FString>& ShortNodeIds)
{
	TArray<FLispNodePtr> EventArgs;
	EventArgs.Add(FLispNode::MakeSymbol(TEXT("input-action")));
	EventArgs.Add(FLispNode::MakeKeyword(TEXT(":action")));
	EventArgs.Add(FLispNode::MakeString(Event->InputActionName.ToString()));
	AppendTruthyKeyword(EventArgs, TEXT(":consume-input"), Event->bConsumeInput);
	AppendTruthyKeyword(EventArgs, TEXT(":execute-when-paused"), Event->bExecuteWhenPaused);
	AppendTruthyKeyword(EventArgs, TEXT(":override-parent-binding"), Event->bOverrideParentBinding);
	AppendEventMetadata(EventArgs, Event, bPositions, ShortEventIds);

	TSet<UEdGraphNode*> PressedVisited;
	FLispNodePtr PressedBody = ConvertExecChainToLisp(Event->GetPressedPin(), Graph, PressedVisited, bPositions, ShortNodeIds);
	if (PressedBody.IsValid() && !PressedBody->IsNil())
	{
		EventArgs.Add(FLispNode::MakeKeyword(TEXT(":pressed")));
		EventArgs.Add(PressedBody);
	}

	TSet<UEdGraphNode*> ReleasedVisited;
	FLispNodePtr ReleasedBody = ConvertExecChainToLisp(Event->GetReleasedPin(), Graph, ReleasedVisited, bPositions, ShortNodeIds);
	if (ReleasedBody.IsValid() && !ReleasedBody->IsNil())
	{
		EventArgs.Add(FLispNode::MakeKeyword(TEXT(":released")));
		EventArgs.Add(ReleasedBody);
	}

	return FLispNode::MakeList(EventArgs);
}

static FLispNodePtr ConvertInputKeyToLisp(UK2Node_InputKey* Event, UEdGraph* Graph, bool bPositions,
	const TMap<FGuid, FString>& ShortEventIds, const TMap<FGuid, FString>& ShortNodeIds)
{
	FString KeyName = Event->InputKey.GetFName().ToString();
	if (KeyName.IsEmpty())
	{
		KeyName = Event->InputKey.ToString();
	}

	TArray<FLispNodePtr> EventArgs;
	EventArgs.Add(FLispNode::MakeSymbol(TEXT("input-key")));
	EventArgs.Add(FLispNode::MakeKeyword(TEXT(":key")));
	EventArgs.Add(FLispNode::MakeString(KeyName));
	AppendTruthyKeyword(EventArgs, TEXT(":consume-input"), Event->bConsumeInput);
	AppendTruthyKeyword(EventArgs, TEXT(":execute-when-paused"), Event->bExecuteWhenPaused);
	AppendTruthyKeyword(EventArgs, TEXT(":override-parent-binding"), Event->bOverrideParentBinding);
	AppendTruthyKeyword(EventArgs, TEXT(":control"), Event->bControl);
	AppendTruthyKeyword(EventArgs, TEXT(":alt"), Event->bAlt);
	AppendTruthyKeyword(EventArgs, TEXT(":shift"), Event->bShift);
	AppendTruthyKeyword(EventArgs, TEXT(":command"), Event->bCommand);
	AppendEventMetadata(EventArgs, Event, bPositions, ShortEventIds);

	TSet<UEdGraphNode*> PressedVisited;
	FLispNodePtr PressedBody = ConvertExecChainToLisp(Event->GetPressedPin(), Graph, PressedVisited, bPositions, ShortNodeIds);
	if (PressedBody.IsValid() && !PressedBody->IsNil())
	{
		EventArgs.Add(FLispNode::MakeKeyword(TEXT(":pressed")));
		EventArgs.Add(PressedBody);
	}

	TSet<UEdGraphNode*> ReleasedVisited;
	FLispNodePtr ReleasedBody = ConvertExecChainToLisp(Event->GetReleasedPin(), Graph, ReleasedVisited, bPositions, ShortNodeIds);
	if (ReleasedBody.IsValid() && !ReleasedBody->IsNil())
	{
		EventArgs.Add(FLispNode::MakeKeyword(TEXT(":released")));
		EventArgs.Add(ReleasedBody);
	}

	return FLispNode::MakeList(EventArgs);
}

static FName GetComponentBoundEventPropertyName(const UK2Node_ComponentBoundEvent* Event)
{
	if (Event)
	{
		if (const FNameProperty* Property = FindFProperty<FNameProperty>(Event->GetClass(), TEXT("ComponentPropertyName")))
		{
			return Property->GetPropertyValue_InContainer(Event);
		}
	}
	return NAME_None;
}

static FLispNodePtr ConvertComponentBoundEventToLisp(UK2Node_ComponentBoundEvent* Event, UEdGraph* Graph, bool bPositions,
	const TMap<FGuid, FString>& ShortEventIds, const TMap<FGuid, FString>& ShortNodeIds)
{
	FString ComponentName = GetComponentBoundEventPropertyName(Event).ToString();
	FString DelegateName = Event->GetDocumentationExcerptName();
	if (DelegateName.IsEmpty())
	{
		if (FMulticastDelegateProperty* DelegateProperty = Event->GetTargetDelegateProperty())
		{
			DelegateName = DelegateProperty->GetFName().ToString();
		}
	}

	TArray<FLispNodePtr> EventArgs;
	EventArgs.Add(FLispNode::MakeSymbol(TEXT("component-bound-event")));
	EventArgs.Add(FLispNode::MakeKeyword(TEXT(":component")));
	EventArgs.Add(FLispNode::MakeString(ComponentName));
	EventArgs.Add(FLispNode::MakeKeyword(TEXT(":delegate")));
	EventArgs.Add(FLispNode::MakeString(DelegateName));
	AppendEventMetadata(EventArgs, Event, bPositions, ShortEventIds);
	AppendExecBodyToArgs(EventArgs, GetThenPin(Event), Graph, bPositions, ShortNodeIds);
	return FLispNode::MakeList(EventArgs);
}

static FLispNodePtr ConvertActorBoundEventToLisp(UK2Node_ActorBoundEvent* Event, UEdGraph* Graph, bool bPositions,
	const TMap<FGuid, FString>& ShortEventIds, const TMap<FGuid, FString>& ShortNodeIds)
{
	FString ActorName;
	if (AActor* EventOwner = Event->GetReferencedLevelActor())
	{
		ActorName = EventOwner->GetActorLabel();
		if (ActorName.IsEmpty())
		{
			ActorName = EventOwner->GetName();
		}
	}

	FString DelegateName = Event->GetDocumentationExcerptName();
	if (DelegateName.IsEmpty())
	{
		if (FMulticastDelegateProperty* DelegateProperty = Event->GetTargetDelegateProperty())
		{
			DelegateName = DelegateProperty->GetFName().ToString();
		}
	}

	TArray<FLispNodePtr> EventArgs;
	EventArgs.Add(FLispNode::MakeSymbol(TEXT("actor-bound-event")));
	EventArgs.Add(FLispNode::MakeKeyword(TEXT(":actor")));
	EventArgs.Add(FLispNode::MakeString(ActorName));
	EventArgs.Add(FLispNode::MakeKeyword(TEXT(":delegate")));
	EventArgs.Add(FLispNode::MakeString(DelegateName));
	AppendEventMetadata(EventArgs, Event, bPositions, ShortEventIds);
	AppendExecBodyToArgs(EventArgs, GetThenPin(Event), Graph, bPositions, ShortNodeIds);
	return FLispNode::MakeList(EventArgs);
}


// ----- Convert a CustomEvent node -----
static FLispNodePtr ConvertCustomEventToLisp(UK2Node_CustomEvent* Event, UEdGraph* Graph, bool bPositions,
	const TMap<FGuid, FString>& ShortEventIds, const TMap<FGuid, FString>& ShortNodeIds)
{
	TSet<UEdGraphNode*> Visited;
	FString EventName = Event->CustomFunctionName.ToString();

	TArray<FLispNodePtr> EventArgs;
	EventArgs.Add(FLispNode::MakeSymbol(TEXT("event")));
	EventArgs.Add(EXP_MakeNameAtom(EventName));

	if (const FString* EId = ShortEventIds.Find(Event->NodeGuid))

	{
		EventArgs.Add(FLispNode::MakeKeyword(TEXT(":event-id")));
		EventArgs.Add(FLispNode::MakeString(*EId));
	}

	// Parameters
	for (UEdGraphPin* Pin : Event->Pins)
	{
		if (EXP_ShouldSkipCustomEventParamPin(Event, Pin))
		{
			continue;
		}
		EventArgs.Add(FLispNode::MakeKeyword(TEXT(":param")));
		TArray<FLispNodePtr> ParamPair;
		ParamPair.Add(FLispNode::MakeSymbol(Pin->PinName.ToString()));
		ParamPair.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
		EventArgs.Add(FLispNode::MakeList(ParamPair));
	}


	UEdGraphPin* ThenPin = GetThenPin(Event);
	FLispNodePtr Body = ConvertExecChainToLisp(ThenPin, Graph, Visited, bPositions, ShortNodeIds);
	if (Body.IsValid() && !Body->IsNil())
	{
		if (BP_IsStructuralSeqWrapper(Body))
			for (int32 i = 1; i < Body->Num(); i++) EventArgs.Add(Body->Get(i));
		else EventArgs.Add(Body);
	}


	return FLispNode::MakeList(EventArgs);
}

// ----- Convert a FunctionEntry node -----
// Function graphs use UK2Node_FunctionEntry as their entry point (not UK2Node_Event).
// We export these as (function <name> [:param (name type)]... body...) to distinguish
// them from event-driven graphs.
static FLispNodePtr ConvertFunctionEntryToLisp(UK2Node_FunctionEntry* FuncEntry, UEdGraph* Graph, bool bPositions,
	const TMap<FGuid, FString>& ShortEventIds, const TMap<FGuid, FString>& ShortNodeIds)
{
	TSet<UEdGraphNode*> Visited;

	// Function name: prefer CustomGeneratedFunctionName, fallback to graph name
	FString FuncName = FuncEntry->CustomGeneratedFunctionName.ToString();
	if (FuncName.IsEmpty() || FuncName.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		FuncName = Graph ? Graph->GetName() : TEXT("");
	}
	if (FuncName.IsEmpty())
	{
		FuncName = FuncEntry->GetNodeTitle(ENodeTitleType::ListView).ToString();
	}


	TArray<FLispNodePtr> FuncArgs;
	FuncArgs.Add(FLispNode::MakeSymbol(TEXT("function")));
	FuncArgs.Add(EXP_MakeNameAtom(FuncName));
	FuncArgs.Add(FLispNode::MakeKeyword(TEXT(":thread-safe")));
	FuncArgs.Add(FLispNode::MakeSymbol(FuncEntry->MetaData.bThreadSafe ? TEXT("true") : TEXT("false")));
	FuncArgs.Add(FLispNode::MakeKeyword(TEXT(":pure")));
	FuncArgs.Add(FLispNode::MakeSymbol((FuncEntry->GetFunctionFlags() & FUNC_BlueprintPure) != 0
		? TEXT("true") : TEXT("false")));


	// :event-id for stable identification
	if (const FString* EId = ShortEventIds.Find(FuncEntry->NodeGuid))
	{
		FuncArgs.Add(FLispNode::MakeKeyword(TEXT(":event-id")));
		FuncArgs.Add(FLispNode::MakeString(*EId));
	}

	// Position metadata
	if (bPositions)
	{
		FuncArgs.Add(FLispNode::MakeKeyword(TEXT(":pos")));
		FuncArgs.Add(FLispNode::MakeString(FString::Printf(TEXT("%d,%d"), FuncEntry->NodePosX, FuncEntry->NodePosY)));
	}

	// Parameters (output pins on FunctionEntry = function input params)
	for (UEdGraphPin* Pin : FuncEntry->Pins)
	{
		if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
			&& !Pin->bHidden && Pin->PinName != UEdGraphSchema_K2::PN_Then)
		{
			FuncArgs.Add(FLispNode::MakeKeyword(TEXT(":param")));
			TArray<FLispNodePtr> ParamPair;
			ParamPair.Add(FLispNode::MakeSymbol(Pin->PinName.ToString()));
			ParamPair.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
			if (Pin->PinType.bIsReference)
			{
				ParamPair.Add(FLispNode::MakeKeyword(TEXT(":ref")));
				ParamPair.Add(FLispNode::MakeSymbol(TEXT("true")));
			}
			if (Pin->PinType.bIsConst)
			{
				ParamPair.Add(FLispNode::MakeKeyword(TEXT(":const")));
				ParamPair.Add(FLispNode::MakeSymbol(TEXT("true")));
			}
			FuncArgs.Add(FLispNode::MakeList(ParamPair));
		}
	}

	// Return type (if the function has a return value pin on the FunctionResult node)
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
		{
			for (UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
					&& !Pin->ParentPin && (!Pin->bHidden || Pin->SubPins.Num() > 0)
					&& Pin->PinName != UEdGraphSchema_K2::PN_Execute)
				{
					FuncArgs.Add(FLispNode::MakeKeyword(TEXT(":return")));
					TArray<FLispNodePtr> RetPair;
					RetPair.Add(FLispNode::MakeSymbol(Pin->PinName.ToString()));
					RetPair.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
					FuncArgs.Add(FLispNode::MakeList(RetPair));
				}
			}
			break; // Only one result node per function graph
		}
	}

	for (const FBPVariableDescription& LocalVariable : FuncEntry->LocalVariables)
	{
		FuncArgs.Add(FLispNode::MakeKeyword(TEXT(":local")));
		TArray<FLispNodePtr> LocalPair;
		LocalPair.Add(EXP_MakeNameAtom(LocalVariable.VarName.ToString()));
		LocalPair.Add(FLispNode::MakeSymbol(PinTypeToLispType(LocalVariable.VarType)));
		FuncArgs.Add(FLispNode::MakeList(LocalPair));
	}

	// Exec output -> body
	UEdGraphPin* ThenPin = GetThenPin(FuncEntry);
	FLispNodePtr Body = ConvertExecChainToLisp(ThenPin, Graph, Visited, bPositions, ShortNodeIds);
	if (Body.IsValid() && !Body->IsNil())
	{
		if (BP_IsStructuralSeqWrapper(Body))
			for (int32 i = 1; i < Body->Num(); i++) FuncArgs.Add(Body->Get(i));
		else FuncArgs.Add(Body);
	}


	return FLispNode::MakeList(FuncArgs);
}

// ----- Convert a Tunnel entry node (Macro graph input) -----
// Macro graphs use UK2Node_Tunnel as their entry/exit points.
// The entry tunnel has bCanHaveOutputs=true and DrawNodeAsEntry()=true.
// We export these as (macro <name> [:param (name type)]... body...) to distinguish
// them from event-driven and function graphs.
static FLispNodePtr ConvertTunnelEntryToLisp(UK2Node_Tunnel* TunnelEntry, UEdGraph* Graph, bool bPositions,
	const TMap<FGuid, FString>& ShortEventIds, const TMap<FGuid, FString>& ShortNodeIds)
{
	TSet<UEdGraphNode*> Visited;

	FString MacroName = Graph->GetName();

	TArray<FLispNodePtr> MacroArgs;
	MacroArgs.Add(FLispNode::MakeSymbol(TEXT("macro")));
	MacroArgs.Add(EXP_MakeNameAtom(MacroName));


	// :event-id for stable identification
	if (const FString* EId = ShortEventIds.Find(TunnelEntry->NodeGuid))
	{
		MacroArgs.Add(FLispNode::MakeKeyword(TEXT(":event-id")));
		MacroArgs.Add(FLispNode::MakeString(*EId));
	}

	// Position metadata
	if (bPositions)
	{
		MacroArgs.Add(FLispNode::MakeKeyword(TEXT(":pos")));
		MacroArgs.Add(FLispNode::MakeString(FString::Printf(TEXT("%d,%d"), TunnelEntry->NodePosX, TunnelEntry->NodePosY)));
	}

	// Parameters (output pins on entry tunnel = macro input params)
	for (UEdGraphPin* Pin : TunnelEntry->Pins)
	{
		if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
			&& !Pin->bHidden && Pin->PinName != UEdGraphSchema_K2::PN_Then)
		{
			MacroArgs.Add(FLispNode::MakeKeyword(TEXT(":param")));
			TArray<FLispNodePtr> ParamPair;
			ParamPair.Add(FLispNode::MakeSymbol(Pin->PinName.ToString()));
			ParamPair.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
			MacroArgs.Add(FLispNode::MakeList(ParamPair));
		}
	}

	// Exits: enumerate exit tunnel nodes in the same graph (for documentation)
	// IMPORTANT: Skip MacroInstance nodes which also inherit from UK2Node_Tunnel.
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (UK2Node_Tunnel* ExitTunnel = Cast<UK2Node_Tunnel>(N))
		{
			// Skip MacroInstance nodes — they are NOT exit tunnels
			if (Cast<UK2Node_MacroInstance>(N)) continue;
			if (!ExitTunnel->DrawNodeAsEntry())
			{
				FString ExitName = ExitTunnel->GetNodeTitle(ENodeTitleType::ListView).ToString();
				MacroArgs.Add(FLispNode::MakeKeyword(TEXT(":exit")));
				TArray<FLispNodePtr> ExitInfo;
				ExitInfo.Add(FLispNode::MakeSymbol(ExitName.IsEmpty() ? TEXT("") : ExitName));
				// Output pin types on exit tunnel
				for (UEdGraphPin* EPin : ExitTunnel->Pins)
				{
					if (EPin->Direction == EGPD_Input && EPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
						&& !EPin->bHidden && EPin->PinName != UEdGraphSchema_K2::PN_Execute)
					{
						TArray<FLispNodePtr> OutPair;
						OutPair.Add(FLispNode::MakeSymbol(EPin->PinName.ToString()));
						OutPair.Add(FLispNode::MakeSymbol(PinTypeToLispType(EPin->PinType)));
						ExitInfo.Add(FLispNode::MakeList(OutPair));
					}
				}
				MacroArgs.Add(FLispNode::MakeList(ExitInfo));
			}
		}
	}

	// Exec output -> body
	// Note: Tunnel entry nodes don't use PN_Then for their exec output pin.
	// The exec output pin name is usually empty or custom-named.
	// We find the first exec output pin directly.
	UEdGraphPin* TunnelExecOut = nullptr;
	for (UEdGraphPin* Pin : TunnelEntry->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& !Pin->bHidden)
		{
			TunnelExecOut = Pin;
			break;
		}
	}
	FLispNodePtr Body = ConvertExecChainToLisp(TunnelExecOut, Graph, Visited, bPositions, ShortNodeIds);
	if (Body.IsValid() && !Body->IsNil())
	{
		if (BP_IsStructuralSeqWrapper(Body))
			for (int32 i = 1; i < Body->Num(); i++) MacroArgs.Add(Body->Get(i));
		else MacroArgs.Add(Body);
	}

	else if (!TunnelExecOut)
	{
		// Pure-data macro: no exec flow. Trace data dependencies from exit tunnel inputs.
		// Output as (exit <name> [:output (pin-name expr)]...) for each exit that has wired inputs.
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (UK2Node_Tunnel* ExitTunnel = Cast<UK2Node_Tunnel>(N))
			{
				if (Cast<UK2Node_MacroInstance>(N)) continue;
				if (ExitTunnel->DrawNodeAsEntry()) continue;

				bool bHasWiredInput = false;
				for (UEdGraphPin* EPin : ExitTunnel->Pins)
				{
					if (EPin->Direction == EGPD_Input && EPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
						&& !EPin->bHidden && EPin->PinName != UEdGraphSchema_K2::PN_Execute
						&& EPin->LinkedTo.Num() > 0)
					{
						bHasWiredInput = true;
						break;
					}
				}
				if (!bHasWiredInput) continue;

				FString ExitName = ExitTunnel->GetNodeTitle(ENodeTitleType::ListView).ToString();
				TArray<FLispNodePtr> ExitArgs;
				ExitArgs.Add(FLispNode::MakeSymbol(TEXT("exit")));
				ExitArgs.Add(FLispNode::MakeSymbol(ExitName.IsEmpty() ? TEXT("") : ExitName));

				for (UEdGraphPin* EPin : ExitTunnel->Pins)
				{
					if (EPin->Direction == EGPD_Input && EPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
						&& !EPin->bHidden && EPin->PinName != UEdGraphSchema_K2::PN_Execute)
					{
						FLispNodePtr Val = ConvertPureExpressionToLisp(EPin, Graph, Visited);
						if (!Val->IsNil())
						{
				ExitArgs.Add(FLispNode::MakeKeyword(TEXT(":output")));
				TArray<FLispNodePtr> OutPair;
				OutPair.Add(EXP_MakeNameAtom(EPin->PinName.ToString()));
				OutPair.Add(Val);

							ExitArgs.Add(FLispNode::MakeList(OutPair));
						}
					}
				}
				MacroArgs.Add(FLispNode::MakeList(ExitArgs));
			}
		}
	}

	return FLispNode::MakeList(MacroArgs);
}

// ============================================================================
// Import helpers: DSL -> K2Node network
// Adapted from ECABridge/ECABlueprintLispCommands.cpp (Epic Games, Experimental)
// ============================================================================

/** Context for Lisp → Blueprint conversion (mirrors ECABridge's FLispToBPContext) */
struct FBPImportContext
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph*   Graph     = nullptr;
	FBlueprintLispConverter::EImportMode ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;

	TMap<FString, UEdGraphNode*> TempIdToNode;     // tempId / NodeGuid → node
	TSet<FGuid> ConsumedRootEventGuids;
	TArray<UEdGraphNode*> ReusableBodyNodes;
	TMap<FString, UEdGraphNode*> ReusableBodyStableIdToNode;
	TSet<FGuid> ConsumedReusableBodyGuids;
	TSet<FGuid> ConsumedFunctionResultGuids;

	TMap<FString, FString>       VariableToNodeId; // var name → node GUID or _literal_ key

	TMap<FString, FString>       VariableToPin;    // var name → pin name
	TMap<FString, UFunction*>    FunctionCache;    // deterministic function lookup cache

	TArray<FString> Errors;
	TArray<FString> Warnings;

	int32   NextTempId = 0;
	int32   CurrentX   = 0;
	int32   CurrentY   = 0;
	FString LastAssetPath;

	FString GenerateTempId()  { return FString::Printf(TEXT("_t%d"), NextTempId++); }
	void    AdvancePosition() { CurrentX += 350; }
	void    NewRow()          { CurrentX = 0; CurrentY += 200; }
};

static FBlueprintLispResult IMP_FailFromContext(const FBPImportContext& Ctx, const FString& FallbackMessage)
{
	const FString ErrorText = Ctx.Errors.Num() > 0 ? FString::Join(Ctx.Errors, TEXT("\n")) : FallbackMessage;
	FBlueprintLispResult Result = FBlueprintLispResult::Fail(ErrorText);
	Result.Warnings = Ctx.Warnings;
	return Result;
}

static FBlueprintLispResult IMP_OkFromContext(const FString& Summary, const FBPImportContext& Ctx)
{
	FBlueprintLispResult Result = FBlueprintLispResult::Ok(Summary);
	Result.Warnings = Ctx.Warnings;
	return Result;
}

static void IMP_RecordCompileStatus(UBlueprint* Blueprint, FBPImportContext& Ctx, const FString& ContextLabel)
{
	if (!Blueprint)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("%s: Blueprint is null after compile"), *ContextLabel));
		return;
	}

	if (Blueprint->Status == BS_Error)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("%s: Blueprint compile finished with errors for %s"), *ContextLabel, *Blueprint->GetPathName()));
		return;
	}

	if (Blueprint->Status == BS_UpToDateWithWarnings)
	{
		Ctx.Warnings.Add(FString::Printf(TEXT("%s: Blueprint compile finished with warnings for %s"), *ContextLabel, *Blueprint->GetPathName()));
	}
}

namespace
{
	const FName IMP_AutoLayoutBehaviorName(TEXT("AutoLayout"));

	static BlueprintLispImportLifecycle::FImportLifecycleContext IMP_MakeLifecycleContext(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FBlueprintLispConverter::FImportOptions& Options)
	{
		BlueprintLispImportLifecycle::FImportLifecycleContext Context;
		Context.ImportSessionId = FGuid::NewGuid();
		Context.TargetAsset = Blueprint;
		Context.TargetGraph = Graph;
		Context.ScopeName = Graph ? FName(*Graph->GetName()) : NAME_None;
		Context.bIsFullRebuild = (Options.ImportMode == FBlueprintLispConverter::EImportMode::ReplaceGraph);
		Context.bIsIncremental = !Context.bIsFullRebuild;
		Context.bIsHeadless = IsRunningCommandlet() || !FSlateApplication::IsInitialized();
		Context.bWillCompile = Options.bCompile;
		if (Options.bAutoLayout)
		{
			Context.RequestedBehaviors.Add(IMP_AutoLayoutBehaviorName);
		}
		return Context;
	}

	static BlueprintLispImportLifecycle::FImportNodeChange IMP_MakeNodeChange(
		UEdGraphNode* Node,
		BlueprintLispImportLifecycle::EImportNodeChangeType ChangeType)
	{
		BlueprintLispImportLifecycle::FImportNodeChange Change;
		Change.Node = Node;
		Change.ChangeType = ChangeType;
		return Change;
	}

	static void IMP_CollectNodeChanges(
		const TSet<UEdGraphNode*>& PreExistingNodes,
		UEdGraph* Graph,
		TArray<BlueprintLispImportLifecycle::FImportNodeChange>& OutChanges)
	{
		if (!Graph)
		{
			return;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && !PreExistingNodes.Contains(Node))
			{
				OutChanges.Add(IMP_MakeNodeChange(Node, BlueprintLispImportLifecycle::EImportNodeChangeType::Added));
			}
		}
	}

	static void IMP_BroadcastNodePhase(
		BlueprintLispImportLifecycle::EImportLifecyclePhase Phase,
		const BlueprintLispImportLifecycle::FImportLifecycleContext& Context,
		const TArray<BlueprintLispImportLifecycle::FImportNodeChange>& Changes)
	{
		if (!FBlueprintLispModule::IsAvailable())
		{
			return;
		}

		BlueprintLispImportLifecycle::FImportNodePhaseEvent Event;
		Event.Phase = Phase;
		Event.Context = Context;
		Event.Changes = Changes;
		FBlueprintLispModule::Get().BroadcastNodePhase(Event);
	}

	static void IMP_BroadcastPropertyPhase(
		BlueprintLispImportLifecycle::EImportLifecyclePhase Phase,
		const BlueprintLispImportLifecycle::FImportLifecycleContext& Context,
		const TArray<BlueprintLispImportLifecycle::FImportPropertyChange>& Changes)
	{
		if (!FBlueprintLispModule::IsAvailable())
		{
			return;
		}

		BlueprintLispImportLifecycle::FImportPropertyPhaseEvent Event;
		Event.Phase = Phase;
		Event.Context = Context;
		Event.Changes = Changes;
		FBlueprintLispModule::Get().BroadcastPropertyPhase(Event);
	}

	static void IMP_BroadcastFinalizePhase(
		BlueprintLispImportLifecycle::EImportLifecyclePhase Phase,
		const BlueprintLispImportLifecycle::FImportLifecycleContext& Context)
	{
		if (!FBlueprintLispModule::IsAvailable())
		{
			return;
		}

		BlueprintLispImportLifecycle::FImportFinalizePhaseEvent Event;
		Event.Phase = Phase;
		Event.Context = Context;
		FBlueprintLispModule::Get().BroadcastFinalizePhase(Event);
	}
}

// Forward decls (Import helpers)

static UEdGraphPin* IMP_ResolveLispExpr(const FLispNodePtr& Expr, FBPImportContext& Ctx);
static UEdGraphNode* IMP_ConvertFormToNode(const FLispNodePtr& Form, FBPImportContext& Ctx, UEdGraphPin*& OutExecPin);
static void IMP_ConvertExecBody(const FLispNodePtr& Body, FBPImportContext& Ctx, UEdGraphPin*& CurrentExecPin);
static bool IMP_SetPinFromExpr(UEdGraphPin* Pin, const FLispNodePtr& Expr, FBPImportContext& Ctx);
static bool IMP_Connect(UEdGraphPin* Src, UEdGraphPin* Dst, FBPImportContext& Ctx);
static bool IMP_BuildPinTypeFromLispType(const FString& TypeName, FEdGraphPinType& OutPinType, FBPImportContext& Ctx);
static FString IMP_ExtractCallMacroName(const FLispNodePtr& Form, int32& OutArgStartIndex);
static UK2Node_MacroInstance* IMP_CreateMacroInstanceNode(const FLispNodePtr& Form, FBPImportContext& Ctx, UEdGraphPin*& OutPreferredOutputPin);
static bool IMP_ExtractBindingNameAndValueIndex(const FLispNodePtr& Form, int32 StartIndex, FString& OutName, int32& OutValueIndex);
static UScriptStruct* IMP_FindStructByName(const FString& StructName, FBPImportContext& Ctx);
static FString IMP_GetAtomName(const FLispNodePtr& Node);
static bool IMP_ArePinTypesEquivalent(const FEdGraphPinType& A, const FEdGraphPinType& B);
static bool IMP_TryGetExistingBlueprintVariableType(UBlueprint* Blueprint, const FName& VarName, FEdGraphPinType& OutPinType);

struct FIMPBlueprintVariableImportSpec
{
	FString VarName;
	FString TypeName;
	FEdGraphPinType RequestedPinType;
	bool bHasDefaultValue = false;
	FString DefaultValue;
	bool bHasExposeOnSpawn = false;
	bool bExposeOnSpawn = false;
	bool bHasInstanceEditable = false;
	bool bInstanceEditable = false;
};

static bool IMP_TryParseBoolLiteral(const FLispNodePtr& Node, bool& OutValue);
static bool IMP_TryBuildBlueprintVariableDefaultValueString(const FString& VarName, const FLispNodePtr& Expr, const FEdGraphPinType& PinType, FString& OutDefaultValue, FBPImportContext& Ctx);
static bool IMP_TryParseBlueprintVariableImportSpec(const FLispNodePtr& Form, FIMPBlueprintVariableImportSpec& OutSpec, FBPImportContext& Ctx);
static bool IMP_TryFindOwnedBlueprintVariable(UBlueprint* Blueprint, const FName& VarName, UBlueprint*& OutOwnerBlueprint, int32& OutVarIndex);
static bool IMP_ApplyBlueprintVariableImportSpec(const FIMPBlueprintVariableImportSpec& Spec, FBPImportContext& Ctx);
static void IMP_EnsureBlueprintVariablesFromTopLevelForms(const TArray<FLispNodePtr>& Nodes, FBPImportContext& Ctx);
static void IMP_EnsureGuid(UEdGraphNode* N);
static UEdGraphPin* IMP_GetExecOutput(UEdGraphNode* N);
static void IMP_CollectDownstreamExecNodes(UEdGraphPin* ExecOutPin, TSet<UEdGraphNode*>& OutNodes);
static void IMP_CollectPureDependencyNodes(UEdGraphPin* ValuePin, TSet<UEdGraphNode*>& OutNodes);




enum class EIMPGraphKind : uint8
{
	EventGraph,
	FunctionGraph,
	MacroGraph,
	TransitionGraph,
	Unknown,
};

static EIMPGraphKind IMP_DetectGraphKind(UEdGraph* Graph)
{
	if (!Graph) return EIMPGraphKind::Unknown;
	if (Cast<UAnimationTransitionGraph>(Graph)) return EIMPGraphKind::TransitionGraph;

	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (Cast<UAnimGraphNode_TransitionResult>(N)) return EIMPGraphKind::TransitionGraph;
		if (Cast<UK2Node_FunctionEntry>(N)) return EIMPGraphKind::FunctionGraph;
		if (UK2Node_Tunnel* TE = Cast<UK2Node_Tunnel>(N))
		{
			if (TE->DrawNodeAsEntry()) return EIMPGraphKind::MacroGraph;
		}
	}

	return EIMPGraphKind::EventGraph;
}

static void IMP_ClearGraphForReplace(UEdGraph* Graph, EIMPGraphKind Kind)
{
	if (!Graph) return;

	TArray<UEdGraphNode*> NodesToRemove;
	for (UEdGraphNode* N : Graph->Nodes)
	{
		bool bKeep = false;
		switch (Kind)
		{
		case EIMPGraphKind::FunctionGraph:
			bKeep = Cast<UK2Node_FunctionEntry>(N) || Cast<UK2Node_FunctionResult>(N);
			break;
		case EIMPGraphKind::MacroGraph:
			bKeep = Cast<UK2Node_Tunnel>(N) != nullptr;
			break;
		case EIMPGraphKind::TransitionGraph:
			bKeep = Cast<UAnimGraphNode_TransitionResult>(N) != nullptr;
			break;
		case EIMPGraphKind::EventGraph:
		case EIMPGraphKind::Unknown:
		default:
			bKeep = false;
			break;
		}
		if (!bKeep)
		{
			NodesToRemove.Add(N);
		}
	}

	for (UEdGraphNode* N : NodesToRemove)
	{
		Graph->RemoveNode(N);
	}
}

static FString IMP_GetKeywordAtomValue(const FLispNodePtr& Form, const TCHAR* Keyword)
{
	if (!Form.IsValid() || !Form->IsList() || !Form->HasKeyword(Keyword))
	{
		return FString();
	}
	return IMP_GetAtomName(Form->GetKeywordArg(Keyword)).TrimStartAndEnd();
}

static bool IMP_IsEventStableIdNode(UEdGraphNode* Node)
{
	if (!Node)
	{
		return false;
	}

	if (Cast<UK2Node_InputAction>(Node) || Cast<UK2Node_InputKey>(Node)
		|| Cast<UK2Node_ComponentBoundEvent>(Node) || Cast<UK2Node_ActorBoundEvent>(Node)
		|| Cast<UK2Node_CustomEvent>(Node) || Cast<UK2Node_Event>(Node) || Cast<UK2Node_FunctionEntry>(Node))
	{
		return true;
	}

	if (UK2Node_Tunnel* TunnelNode = Cast<UK2Node_Tunnel>(Node))
	{
		return TunnelNode->DrawNodeAsEntry();
	}

	return false;
}

static void IMP_BuildStableIdIndex(UEdGraph* Graph, bool bEventIds, TMap<FString, UEdGraphNode*>& OutStableIdToNode, const TSet<FGuid>* AllowedGuids = nullptr, const TSet<FGuid>* ExcludedGuids = nullptr)
{
	OutStableIdToNode.Reset();
	if (!Graph)
	{
		return;
	}

	TArray<FGuid> Guids;
	TMap<FGuid, UEdGraphNode*> GuidToNode;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		IMP_EnsureGuid(Node);
		if (!Node->NodeGuid.IsValid())
		{
			continue;
		}
		if (ExcludedGuids && ExcludedGuids->Contains(Node->NodeGuid))
		{
			continue;
		}

		const bool bMatchesGroup = bEventIds ? IMP_IsEventStableIdNode(Node) : !IMP_IsEventStableIdNode(Node);
		if (!bMatchesGroup)
		{
			continue;
		}

		Guids.Add(Node->NodeGuid);
		if (!AllowedGuids || AllowedGuids->Contains(Node->NodeGuid))
		{
			GuidToNode.Add(Node->NodeGuid, Node);
		}
	}

	const TMap<FGuid, FString> ShortIds = ComputeShortIds(Guids);
	for (const TPair<FGuid, FString>& Pair : ShortIds)
	{
		if (UEdGraphNode* const* FoundNode = GuidToNode.Find(Pair.Key))
		{
			OutStableIdToNode.Add(Pair.Value.ToLower(), *FoundNode);
		}
	}
}

static void IMP_ClearAllNodeLinks(UEdGraphNode* Node)
{
	if (!Node)
	{
		return;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			Pin->BreakAllPinLinks();
		}
	}
}

static bool IMP_IsPendingReusableBodyNode(UEdGraphNode* Node, const FBPImportContext& Ctx)
{
	return Node
		&& Ctx.ReusableBodyNodes.Contains(Node)
		&& !Ctx.ConsumedReusableBodyGuids.Contains(Node->NodeGuid);
}

static FString IMP_GetRequestedNodeStableId(const FLispNodePtr& Form)
{
	return IMP_GetKeywordAtomValue(Form, TEXT(":id")).ToLower();
}

static void IMP_ApplyRequestedStableId(UEdGraphNode* Node, const FLispNodePtr& Form, const bool bEventId)
{
	if (!Node || !Form.IsValid()) return;
	FString StableId = IMP_GetKeywordAtomValue(Form, bEventId ? TEXT(":event-id") : TEXT(":id")).ToLower();
	if (StableId.IsEmpty() || StableId.Len() > 32) return;
	for (const TCHAR Character : StableId)
	{
		if (!FChar::IsHexDigit(Character)) return;
	}

	const int32 PrefixLength = StableId.Len();
	StableId += FString::ChrN(32 - PrefixLength, TEXT('0'));
	if (PrefixLength < 32)
	{
		StableId[31] = bEventId ? TEXT('1') : TEXT('2');
	}
	FGuid StableGuid;
	if (FGuid::ParseExact(StableId, EGuidFormats::Digits, StableGuid) && StableGuid.IsValid())
	{
		Node->NodeGuid = StableGuid;
	}
}

static void IMP_ResetReusableBodyNodePool(FBPImportContext& Ctx)
{
	Ctx.ReusableBodyNodes.Reset();
	Ctx.ReusableBodyStableIdToNode.Reset();
	Ctx.ConsumedReusableBodyGuids.Reset();
}

static void IMP_PrepareExistingEventBodyForIncrementalReuse(UEdGraphNode* EventNode, FBPImportContext& Ctx)
{
	IMP_ResetReusableBodyNodePool(Ctx);
	if (!EventNode || !Ctx.Graph)
	{
		return;
	}

	TSet<UEdGraphNode*> NodesToReuse;
	for (UEdGraphPin* Pin : EventNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}

		IMP_CollectDownstreamExecNodes(Pin, NodesToReuse);
		Pin->BreakAllPinLinks();
	}

	TSet<FGuid> AllowedGuids;
	for (UEdGraphNode* Node : NodesToReuse)
	{
		if (!Node)
		{
			continue;
		}
		IMP_EnsureGuid(Node);
		Ctx.ReusableBodyNodes.Add(Node);
		AllowedGuids.Add(Node->NodeGuid);
		IMP_ClearAllNodeLinks(Node);
	}

	IMP_BuildStableIdIndex(Ctx.Graph, false, Ctx.ReusableBodyStableIdToNode, &AllowedGuids, nullptr);
}

static void IMP_FinalizeExistingEventBodyIncrementalReuse(FBPImportContext& Ctx)
{
	for (UEdGraphNode* Node : Ctx.ReusableBodyNodes)
	{
		if (!Node || Ctx.ConsumedReusableBodyGuids.Contains(Node->NodeGuid))
		{
			continue;
		}
		Ctx.Graph->RemoveNode(Node);
	}

	IMP_ResetReusableBodyNodePool(Ctx);
}

static UEdGraphNode* IMP_FindReusableBodyNodeByStableId(const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (Ctx.ImportMode == FBlueprintLispConverter::EImportMode::ReplaceGraph)
	{
		return nullptr;
	}

	const FString RequestedId = IMP_GetRequestedNodeStableId(Form);
	if (RequestedId.IsEmpty())
	{
		return nullptr;
	}

	if (UEdGraphNode* const* FoundNode = Ctx.ReusableBodyStableIdToNode.Find(RequestedId))
	{
		if (!Ctx.ConsumedReusableBodyGuids.Contains((*FoundNode)->NodeGuid))
		{
			return *FoundNode;
		}
	}

	return nullptr;
}

static void IMP_MarkReusableBodyNodeConsumed(UEdGraphNode* Node, FBPImportContext& Ctx)
{
	if (!Node)
	{
		return;
	}

	IMP_ClearAllNodeLinks(Node);
	Node->ReconstructNode();
	Ctx.ConsumedReusableBodyGuids.Add(Node->NodeGuid);
	Ctx.TempIdToNode.FindOrAdd(Node->NodeGuid.ToString()) = Node;
}

static UK2Node_IfThenElse* IMP_CreateOrReuseBranchNode(const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_IfThenElse* ReusableBranchNode = Cast<UK2Node_IfThenElse>(ReusableNode))
		{
			IMP_MarkReusableBodyNodeConsumed(ReusableBranchNode, Ctx);
			Ctx.AdvancePosition();
			return ReusableBranchNode;
		}
	}

	UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Ctx.Graph);
	BranchNode->NodePosX = Ctx.CurrentX;
	BranchNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(BranchNode, false, false);
	BranchNode->AllocateDefaultPins();
	IMP_EnsureGuid(BranchNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), BranchNode);
	Ctx.TempIdToNode.Add(BranchNode->NodeGuid.ToString(), BranchNode);
	return BranchNode;
}

static bool IMP_IsCompatibleExistingVariableSetNode(UK2Node_VariableSet* VariableSetNode,
	const FString& RequestedVariableName, const UClass* RequestedOwnerClass)
{
	if (!VariableSetNode)
	{
		return false;
	}

	return VariableSetNode->VariableReference.GetMemberName().ToString().Equals(RequestedVariableName, ESearchCase::IgnoreCase)
		&& VariableSetNode->VariableReference.GetMemberParentClass() == RequestedOwnerClass;
}

static UK2Node_VariableSet* IMP_CreateOrReuseVariableSetNode(const FLispNodePtr& Form, const FString& VariableName, FBPImportContext& Ctx)
{
	UClass* OwnerClass = nullptr;
	const FString OwnerPath = IMP_GetKeywordAtomValue(Form, TEXT(":owner"));
	if (!OwnerPath.IsEmpty())
	{
		OwnerClass = LoadObject<UClass>(nullptr, *OwnerPath);
		if (!OwnerClass)
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: set owner class not found: %s"), *OwnerPath));
			return nullptr;
		}
	}

	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_VariableSet* ReusableSetNode = Cast<UK2Node_VariableSet>(ReusableNode))
		{
			if (IMP_IsCompatibleExistingVariableSetNode(ReusableSetNode, VariableName, OwnerClass))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableSetNode, Ctx);
				Ctx.AdvancePosition();
				return ReusableSetNode;
			}
		}
	}

	UK2Node_VariableSet* VariableSetNode = NewObject<UK2Node_VariableSet>(Ctx.Graph);
	if (OwnerClass)
	{
		VariableSetNode->VariableReference.SetExternalMember(FName(*VariableName), OwnerClass);
	}
	else
	{
		VariableSetNode->VariableReference.SetSelfMember(FName(*VariableName));
	}
	VariableSetNode->NodePosX = Ctx.CurrentX;
	VariableSetNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(VariableSetNode, false, false);
	VariableSetNode->AllocateDefaultPins();
	IMP_EnsureGuid(VariableSetNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), VariableSetNode);
	Ctx.TempIdToNode.Add(VariableSetNode->NodeGuid.ToString(), VariableSetNode);
	return VariableSetNode;
}

static bool IMP_IsCompatibleExistingDynamicCastNode(UK2Node_DynamicCast* DynamicCastNode, UClass* RequestedTargetClass)
{
	if (!DynamicCastNode || !RequestedTargetClass)
	{
		return false;
	}

	return DynamicCastNode->TargetType == RequestedTargetClass;
}

static UK2Node_DynamicCast* IMP_CreateOrReuseDynamicCastNode(const FLispNodePtr& Form, UClass* TargetClass, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_DynamicCast* ReusableCastNode = Cast<UK2Node_DynamicCast>(ReusableNode))
		{
			if (IMP_IsCompatibleExistingDynamicCastNode(ReusableCastNode, TargetClass))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableCastNode, Ctx);
				ReusableCastNode->SetPurity(false);
				Ctx.AdvancePosition();
				return ReusableCastNode;
			}
		}
	}

	UK2Node_DynamicCast* DynamicCastNode = NewObject<UK2Node_DynamicCast>(Ctx.Graph);
	DynamicCastNode->TargetType = TargetClass;
	DynamicCastNode->NodePosX = Ctx.CurrentX;
	DynamicCastNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(DynamicCastNode, false, false);
	DynamicCastNode->SetPurity(false);
	DynamicCastNode->AllocateDefaultPins();
	IMP_EnsureGuid(DynamicCastNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), DynamicCastNode);
	Ctx.TempIdToNode.Add(DynamicCastNode->NodeGuid.ToString(), DynamicCastNode);
	return DynamicCastNode;
}

static bool IMP_IsCompatibleExistingCreateObjectNode(UK2Node_GenericCreateObject* CreateObjectNode, UClass* RequestedClass)
{
	return CreateObjectNode && RequestedClass && CreateObjectNode->GetClassToSpawn() == RequestedClass;
}

static UK2Node_GenericCreateObject* IMP_CreateOrReuseGenericCreateObjectNode(
	const FLispNodePtr& Form, UClass* TargetClass, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_GenericCreateObject* ReusableCreateNode = Cast<UK2Node_GenericCreateObject>(ReusableNode))
		{
			if (IMP_IsCompatibleExistingCreateObjectNode(ReusableCreateNode, TargetClass))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableCreateNode, Ctx);
				Ctx.AdvancePosition();
				return ReusableCreateNode;
			}
		}
	}

	UK2Node_GenericCreateObject* CreateObjectNode = NewObject<UK2Node_GenericCreateObject>(Ctx.Graph);
	CreateObjectNode->NodePosX = Ctx.CurrentX;
	CreateObjectNode->NodePosY = Ctx.CurrentY;
	CreateObjectNode->CreateNewGuid();
	Ctx.Graph->AddNode(CreateObjectNode, false, false);
	CreateObjectNode->PostPlacedNewNode();
	CreateObjectNode->AllocateDefaultPins();
	if (UEdGraphPin* ClassPin = CreateObjectNode->GetClassPin())
	{
		ClassPin->DefaultObject = TargetClass;
		CreateObjectNode->PinDefaultValueChanged(ClassPin);
	}
	IMP_EnsureGuid(CreateObjectNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), CreateObjectNode);
	Ctx.TempIdToNode.Add(CreateObjectNode->NodeGuid.ToString(), CreateObjectNode);
	return CreateObjectNode;
}

static bool IMP_IsCompatibleExistingSwitchIntegerNode(UK2Node_SwitchInteger* SwitchNode, const TArray<TPair<int32, FLispNodePtr>>& RequestedCaseBodies, bool bRequestedHasDefaultPin)
{
	if (!SwitchNode)
	{
		return false;
	}

	if ((SwitchNode->GetDefaultPin() != nullptr) != bRequestedHasDefaultPin)
	{
		return false;
	}

	TArray<int32> ExistingCaseLabels;
	for (UEdGraphPin* Pin : SwitchNode->Pins)
	{
		if (Pin
			&& Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& !Pin->PinName.ToString().Equals(TEXT("default"), ESearchCase::IgnoreCase))
		{
			ExistingCaseLabels.Add(FCString::Atoi(*Pin->PinName.ToString()));
		}
	}

	ExistingCaseLabels.Sort();

	if (ExistingCaseLabels.Num() != RequestedCaseBodies.Num())
	{
		return false;
	}

	for (int32 CaseIdx = 0; CaseIdx < RequestedCaseBodies.Num(); ++CaseIdx)
	{
		if (ExistingCaseLabels[CaseIdx] != RequestedCaseBodies[CaseIdx].Key)
		{
			return false;
		}
	}

	return true;
}

static UK2Node_SwitchInteger* IMP_CreateOrReuseSwitchIntegerNode(const FLispNodePtr& Form, const TArray<TPair<int32, FLispNodePtr>>& CaseBodies, bool bHasDefaultPin, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_SwitchInteger* ReusableSwitchNode = Cast<UK2Node_SwitchInteger>(ReusableNode))
		{
			if (IMP_IsCompatibleExistingSwitchIntegerNode(ReusableSwitchNode, CaseBodies, bHasDefaultPin))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableSwitchNode, Ctx);
				Ctx.AdvancePosition();
				return ReusableSwitchNode;
			}
		}
	}

	UK2Node_SwitchInteger* SwitchNode = NewObject<UK2Node_SwitchInteger>(Ctx.Graph);
	SwitchNode->StartIndex = CaseBodies.Num() > 0 ? CaseBodies[0].Key : 0;
	SwitchNode->bHasDefaultPin = bHasDefaultPin;
	SwitchNode->NodePosX = Ctx.CurrentX;
	SwitchNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(SwitchNode, false, false);
	SwitchNode->AllocateDefaultPins();
	for (int32 CaseIdx = 0; CaseIdx < CaseBodies.Num(); ++CaseIdx)
	{
		SwitchNode->AddPinToSwitchNode();
	}
	IMP_EnsureGuid(SwitchNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), SwitchNode);
	Ctx.TempIdToNode.Add(SwitchNode->NodeGuid.ToString(), SwitchNode);
	return SwitchNode;
}

static int32 IMP_CountExecOutputPins(UEdGraphNode* Node)
{
	int32 ExecOutputCount = 0;
	if (!Node)
	{
		return ExecOutputCount;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			++ExecOutputCount;
		}
	}

	return ExecOutputCount;
}

static bool IMP_IsCompatibleExistingSequenceNode(UK2Node_ExecutionSequence* SequenceNode, int32 RequestedBranchCount)
{
	if (!SequenceNode)
	{
		return false;
	}

	const int32 RequestedExecOutputs = FMath::Max(RequestedBranchCount, 2);
	return IMP_CountExecOutputPins(SequenceNode) == RequestedExecOutputs;
}

static UK2Node_ExecutionSequence* IMP_CreateOrReuseSequenceNode(const FLispNodePtr& Form, int32 RequestedBranchCount, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_ExecutionSequence* ReusableSequenceNode = Cast<UK2Node_ExecutionSequence>(ReusableNode))
		{
			if (IMP_IsCompatibleExistingSequenceNode(ReusableSequenceNode, RequestedBranchCount))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableSequenceNode, Ctx);
				Ctx.AdvancePosition();
				return ReusableSequenceNode;
			}
		}
	}

	UK2Node_ExecutionSequence* SequenceNode = NewObject<UK2Node_ExecutionSequence>(Ctx.Graph);
	SequenceNode->NodePosX = Ctx.CurrentX;
	SequenceNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(SequenceNode, false, false);
	SequenceNode->AllocateDefaultPins();
	const int32 RequestedExecOutputs = FMath::Max(RequestedBranchCount, 2);
	while (IMP_CountExecOutputPins(SequenceNode) < RequestedExecOutputs)
	{
		SequenceNode->AddInputPin();
	}
	IMP_EnsureGuid(SequenceNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), SequenceNode);
	Ctx.TempIdToNode.Add(SequenceNode->NodeGuid.ToString(), SequenceNode);
	return SequenceNode;
}

static bool IMP_IsOpaqueGenericFallbackForm(const FLispNodePtr& Form)
{
	if (!Form.IsValid() || !Form->IsList() || Form->Num() < 3)
	{
		return false;
	}

	bool bHasStableId = false;
	for (int32 i = 1; i < Form->Num(); ++i)
	{
		const FLispNodePtr Part = Form->Get(i);
		if (!Part.IsValid())
		{
			continue;
		}
		if (!Part->IsKeyword())
		{
			return false;
		}

		const FString KeywordName = Part->StringValue.StartsWith(TEXT(":")) ? Part->StringValue.Mid(1) : Part->StringValue;
		if (KeywordName.Equals(TEXT("id"), ESearchCase::IgnoreCase))
		{
			bHasStableId = true;
		}

		if (i + 1 >= Form->Num())
		{
			return false;
		}

		const FLispNodePtr Value = Form->Get(i + 1);
		if (Value.IsValid() && Value->IsList())
		{
			return false;
		}

		i += 1;
	}

	return bHasStableId;
}

static UEdGraphNode* IMP_TryReuseOpaqueGenericBodyNode(const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (!IMP_IsOpaqueGenericFallbackForm(Form))
	{
		return nullptr;
	}

	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		Ctx.ConsumedReusableBodyGuids.Add(ReusableNode->NodeGuid);
		Ctx.TempIdToNode.FindOrAdd(ReusableNode->NodeGuid.ToString()) = ReusableNode;
		Ctx.AdvancePosition();
		return ReusableNode;
	}

	return nullptr;
}

static FString IMP_GetExistingCallNodeName(UK2Node_CallFunction* CallNode)

{



	if (!CallNode)
	{
		return FString();
	}

	if (UFunction* TargetFunction = CallNode->GetTargetFunction())
	{
		return TargetFunction->GetName();
	}

	const FName MemberName = CallNode->FunctionReference.GetMemberName();
	if (!MemberName.IsNone())
	{
		return MemberName.ToString();
	}

	return CallNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
}

static bool IMP_IsCompatibleExistingCallNode(UK2Node_CallFunction* CallNode, const FString& RequestedFunctionName, bool bUseSelfMemberReference, bool bRequestedPromotableOperator, bool bRequestedAnimGetter)
{
	if (!CallNode || Cast<UK2Node_CallParentFunction>(CallNode))
	{
		return false;
	}

	const FString ExistingFunctionName = IMP_GetExistingCallNodeName(CallNode);
	if (!ExistingFunctionName.Equals(RequestedFunctionName, ESearchCase::IgnoreCase))
	{
		return false;
	}

	const bool bExistingUsesSelfMemberReference = !CallNode->FunctionReference.GetMemberName().IsNone() && CallNode->GetTargetFunction() == nullptr;
	return bExistingUsesSelfMemberReference == bUseSelfMemberReference
		&& (Cast<UK2Node_PromotableOperator>(CallNode) != nullptr) == bRequestedPromotableOperator
		&& (Cast<UK2Node_AnimGetter>(CallNode) != nullptr) == bRequestedAnimGetter;
}

static UK2Node_CallFunction* IMP_CreateOrReuseCallFunctionNode(const FLispNodePtr& Form, UFunction* Function, const FString& RequestedFunctionName, bool bUseSelfMemberReference, FBPImportContext& Ctx)
{
	const FString CallKind = IMP_GetKeywordAtomValue(Form, TEXT(":call-kind"));
	const bool bRequestedPromotableOperator = CallKind.Equals(TEXT("promotable-operator"), ESearchCase::IgnoreCase);
	const bool bRequestedAnimGetter = CallKind.Equals(TEXT("anim-getter"), ESearchCase::IgnoreCase);
	auto ApplyDeclaredResultType = [&Form, &RequestedFunctionName](UK2Node_CallFunction* CallNode)
	{
		if (!CallNode) return;
		const FString TypeObjectPath = IMP_GetKeywordAtomValue(Form, TEXT(":result-type-object"));
		if (TypeObjectPath.IsEmpty()) return;
		UObject* TypeObject = StaticLoadObject(UObject::StaticClass(), nullptr, *TypeObjectPath);
		if (!TypeObject) return;
		UEdGraphPin* DeclaredOutputPin = nullptr;
		const FString DeclaredOutputName = IMP_GetKeywordAtomValue(Form, TEXT(":out-pin"));
		if (!DeclaredOutputName.IsEmpty())
		{
			for (UEdGraphPin* Pin : CallNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output && Pin->PinName.ToString().Equals(DeclaredOutputName, ESearchCase::IgnoreCase))
				{
					DeclaredOutputPin = Pin;
					break;
				}
			}
		}
		if (!DeclaredOutputPin)
		{
			DeclaredOutputPin = CallNode->GetReturnValuePin();
		}
		if (DeclaredOutputPin)
		{
			DeclaredOutputPin->PinType.PinSubCategoryObject = TypeObject;
		}
		if (RequestedFunctionName.Equals(TEXT("GetComponentByClass"), ESearchCase::IgnoreCase))
		{
			if (UEdGraphPin* ClassPin = CallNode->FindPin(TEXT("ComponentClass"), EGPD_Input))
			{
				ClassPin->DefaultObject = Cast<UClass>(TypeObject);
				ClassPin->DefaultValue.Empty();
			}
		}
	};
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_CallFunction* ReusableCallNode = Cast<UK2Node_CallFunction>(ReusableNode))
		{
			if (IMP_IsCompatibleExistingCallNode(ReusableCallNode, RequestedFunctionName, bUseSelfMemberReference, bRequestedPromotableOperator, bRequestedAnimGetter))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableCallNode, Ctx);
				ApplyDeclaredResultType(ReusableCallNode);
				Ctx.AdvancePosition();
				Ctx.TempIdToNode.FindOrAdd(ReusableCallNode->NodeGuid.ToString()) = ReusableCallNode;
				return ReusableCallNode;
			}

		}
	}

	UK2Node_CallFunction* CallNode = CallKind.Equals(TEXT("message"), ESearchCase::IgnoreCase)
		? NewObject<UK2Node_Message>(Ctx.Graph)
		: bRequestedAnimGetter
			? NewObject<UK2Node_AnimGetter>(Ctx.Graph)
		: bRequestedPromotableOperator
			? NewObject<UK2Node_PromotableOperator>(Ctx.Graph)
		: Function && Function->HasMetaData(FBlueprintMetadata::MD_CommutativeAssociativeBinaryOperator)
			? NewObject<UK2Node_CommutativeAssociativeBinaryOperator>(Ctx.Graph)
			: Function && Function->HasMetaData(FBlueprintMetadata::MD_ArrayParam)
				? NewObject<UK2Node_CallArrayFunction>(Ctx.Graph)
			: NewObject<UK2Node_CallFunction>(Ctx.Graph);
	if (bUseSelfMemberReference)
	{
		CallNode->FunctionReference.SetSelfMember(FName(*RequestedFunctionName));
	}
	else if (Function)
	{
		CallNode->SetFromFunction(Function);
	}
	CallNode->NodePosX = Ctx.CurrentX;
	CallNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(CallNode, false, false);
	CallNode->PostPlacedNewNode();
	CallNode->AllocateDefaultPins();
	ApplyDeclaredResultType(CallNode);
	IMP_EnsureGuid(CallNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), CallNode);
	Ctx.TempIdToNode.Add(CallNode->NodeGuid.ToString(), CallNode);
	return CallNode;
}

static bool IMP_IsCompatibleExistingCallParentNode(UK2Node_CallParentFunction* CallParentNode, const FString& RequestedFunctionName)
{
	if (!CallParentNode)
	{
		return false;
	}

	return IMP_GetExistingCallNodeName(CallParentNode).Equals(RequestedFunctionName, ESearchCase::IgnoreCase);
}

static UK2Node_CallParentFunction* IMP_CreateOrReuseCallParentNode(const FLispNodePtr& Form, UFunction* Function, const FString& RequestedFunctionName, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_CallParentFunction* ReusableCallParentNode = Cast<UK2Node_CallParentFunction>(ReusableNode))
		{
			if (IMP_IsCompatibleExistingCallParentNode(ReusableCallParentNode, RequestedFunctionName))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableCallParentNode, Ctx);
				Ctx.AdvancePosition();
				return ReusableCallParentNode;
			}
		}
	}

	UK2Node_CallParentFunction* CallParentNode = NewObject<UK2Node_CallParentFunction>(Ctx.Graph);
	if (Function)
	{
		CallParentNode->SetFromFunction(Function);
	}
	CallParentNode->NodePosX = Ctx.CurrentX;
	CallParentNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(CallParentNode, false, false);
	CallParentNode->AllocateDefaultPins();
	IMP_EnsureGuid(CallParentNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), CallParentNode);
	Ctx.TempIdToNode.Add(CallParentNode->NodeGuid.ToString(), CallParentNode);
	return CallParentNode;
}

static FString IMP_GetMacroInstanceName(UK2Node_MacroInstance* MacroNode)

{
	if (!MacroNode)
	{
		return FString();
	}

	if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
	{
		return MacroGraph->GetName();
	}

	return MacroNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
}

static UK2Node_MacroInstance* IMP_FindReusableMacroInstanceNode(const FLispNodePtr& Form, const FString& MacroName, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_MacroInstance* ReusableMacroNode = Cast<UK2Node_MacroInstance>(ReusableNode))
		{
			if (IMP_GetMacroInstanceName(ReusableMacroNode).Equals(MacroName, ESearchCase::IgnoreCase))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableMacroNode, Ctx);
				Ctx.AdvancePosition();
				return ReusableMacroNode;
			}
		}
	}

	return nullptr;
}

static TArray<UEdGraphPin*> IMP_GetMakeArrayValueInputPins(UK2Node_MakeArray* MakeArrayNode)
{
	TArray<UEdGraphPin*> Pins;
	if (!MakeArrayNode)
	{
		return Pins;
	}

	for (UEdGraphPin* Pin : MakeArrayNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->ParentPin != nullptr) continue;
		Pins.Add(Pin);
	}
	return Pins;
}

static bool IMP_IsCompatibleExistingMakeArrayNode(UK2Node_MakeArray* MakeArrayNode, int32 RequestedItemCount)
{
	if (!MakeArrayNode)
	{
		return false;
	}

	return IMP_GetMakeArrayValueInputPins(MakeArrayNode).Num() == RequestedItemCount;
}

static UK2Node_MakeArray* IMP_CreateOrReuseMakeArrayNode(const FLispNodePtr& Form, int32 RequestedItemCount, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_MakeArray* ReusableMakeArrayNode = Cast<UK2Node_MakeArray>(ReusableNode))
		{
			if (IMP_IsCompatibleExistingMakeArrayNode(ReusableMakeArrayNode, RequestedItemCount))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableMakeArrayNode, Ctx);
				Ctx.AdvancePosition();
				Ctx.TempIdToNode.FindOrAdd(ReusableMakeArrayNode->NodeGuid.ToString()) = ReusableMakeArrayNode;
				return ReusableMakeArrayNode;
			}

		}
	}

	UK2Node_MakeArray* MakeArrayNode = NewObject<UK2Node_MakeArray>(Ctx.Graph);
	MakeArrayNode->NodePosX = Ctx.CurrentX;
	MakeArrayNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(MakeArrayNode, false, false);
	MakeArrayNode->AllocateDefaultPins();
	IMP_EnsureGuid(MakeArrayNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), MakeArrayNode);
	Ctx.TempIdToNode.Add(MakeArrayNode->NodeGuid.ToString(), MakeArrayNode);
	return MakeArrayNode;
}

static UK2Node_GetArrayItem* IMP_CreateOrReuseGetArrayItemNode(const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_GetArrayItem* ReusableGetArrayItemNode = Cast<UK2Node_GetArrayItem>(ReusableNode))
		{
			IMP_MarkReusableBodyNodeConsumed(ReusableGetArrayItemNode, Ctx);
			Ctx.AdvancePosition();
			Ctx.TempIdToNode.FindOrAdd(ReusableGetArrayItemNode->NodeGuid.ToString()) = ReusableGetArrayItemNode;
			return ReusableGetArrayItemNode;
		}

	}

	UK2Node_GetArrayItem* GetArrayItemNode = NewObject<UK2Node_GetArrayItem>(Ctx.Graph);
	GetArrayItemNode->NodePosX = Ctx.CurrentX;
	GetArrayItemNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(GetArrayItemNode, false, false);
	GetArrayItemNode->AllocateDefaultPins();
	IMP_EnsureGuid(GetArrayItemNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), GetArrayItemNode);
	Ctx.TempIdToNode.Add(GetArrayItemNode->NodeGuid.ToString(), GetArrayItemNode);
	return GetArrayItemNode;
}

static bool IMP_IsCompatibleExistingBreakStructNode(UK2Node_BreakStruct* BreakNode, UScriptStruct* StructType)
{
	return BreakNode && BreakNode->StructType == StructType;
}

static UK2Node_BreakStruct* IMP_CreateOrReuseBreakStructNode(const FLispNodePtr& Form, UScriptStruct* StructType, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_BreakStruct* ReusableBreakNode = Cast<UK2Node_BreakStruct>(ReusableNode))
		{
			if (IMP_IsCompatibleExistingBreakStructNode(ReusableBreakNode, StructType))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableBreakNode, Ctx);
				Ctx.AdvancePosition();
				Ctx.TempIdToNode.FindOrAdd(ReusableBreakNode->NodeGuid.ToString()) = ReusableBreakNode;
				return ReusableBreakNode;
			}

		}
	}

	UK2Node_BreakStruct* BreakNode = NewObject<UK2Node_BreakStruct>(Ctx.Graph);
	BreakNode->StructType = StructType;
	BreakNode->NodePosX = Ctx.CurrentX;
	BreakNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(BreakNode, false, false);
	BreakNode->AllocateDefaultPins();
	IMP_EnsureGuid(BreakNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), BreakNode);
	Ctx.TempIdToNode.Add(BreakNode->NodeGuid.ToString(), BreakNode);
	return BreakNode;
}

static bool IMP_IsCompatibleExistingEnumCompareNode(UEdGraphNode* Node, bool bRequestedEquality)
{
	if (!Node)
	{
		return false;
	}

	return bRequestedEquality
		? Cast<UK2Node_EnumEquality>(Node) != nullptr
		: Cast<UK2Node_EnumInequality>(Node) != nullptr;
}

static UK2Node* IMP_CreateOrReuseEnumCompareNode(const FLispNodePtr& Form, bool bRequestedEquality, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (IMP_IsCompatibleExistingEnumCompareNode(ReusableNode, bRequestedEquality))
		{
			IMP_MarkReusableBodyNodeConsumed(ReusableNode, Ctx);
			Ctx.AdvancePosition();
			Ctx.TempIdToNode.FindOrAdd(ReusableNode->NodeGuid.ToString()) = ReusableNode;
			return Cast<UK2Node>(ReusableNode);
		}
	}

	UK2Node* EnumCompareNode = bRequestedEquality
		? Cast<UK2Node>(NewObject<UK2Node_EnumEquality>(Ctx.Graph))
		: Cast<UK2Node>(NewObject<UK2Node_EnumInequality>(Ctx.Graph));
	if (!EnumCompareNode)
	{
		return nullptr;
	}

	EnumCompareNode->NodePosX = Ctx.CurrentX;
	EnumCompareNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(EnumCompareNode, false, false);
	EnumCompareNode->AllocateDefaultPins();
	IMP_EnsureGuid(EnumCompareNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), EnumCompareNode);
	Ctx.TempIdToNode.Add(EnumCompareNode->NodeGuid.ToString(), EnumCompareNode);
	return EnumCompareNode;
}

static FString IMP_GetReusableEventName(UK2Node_Event* EventNode)



{
	if (!EventNode)
	{
		return FString();
	}
	if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(EventNode))
	{
		const FString CustomName = CustomEventNode->CustomFunctionName.ToString();
		if (!CustomName.IsEmpty())
		{
			return CustomName;
		}
	}

	FString EventName = EventNode->EventReference.GetMemberName().ToString();
	if (EventName.IsEmpty())
	{
		EventName = EventNode->CustomFunctionName.ToString();
	}
	if (EventName.IsEmpty())
	{
		EventName = EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
	}
	return EventName;
}

static bool IMP_IsCompatibleExistingEventNode(UK2Node_Event* Candidate, const FString& EventName, UFunction* EventSignatureFunc, bool bHasExplicitParams)
{
	if (!Candidate)
	{
		return false;
	}

	if (bHasExplicitParams)
	{
		if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Candidate))
		{
			return CustomEventNode->CustomFunctionName.ToString().Equals(EventName, ESearchCase::IgnoreCase);
		}
		return false;
	}

	if (EventSignatureFunc)
	{
		if (Cast<UK2Node_CustomEvent>(Candidate))
		{
			return false;
		}
		const FString CandidateName = Candidate->EventReference.GetMemberName().ToString();
		return CandidateName.Equals(EventSignatureFunc->GetName(), ESearchCase::IgnoreCase)
			|| CandidateName.Equals(EventName, ESearchCase::IgnoreCase);
	}

	if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Candidate))
	{
		return CustomEventNode->CustomFunctionName.ToString().Equals(EventName, ESearchCase::IgnoreCase);
	}

	return IMP_GetReusableEventName(Candidate).Equals(EventName, ESearchCase::IgnoreCase);
}

static void IMP_CollectPureDependencyNodes(UEdGraphPin* ValuePin, TSet<UEdGraphNode*>& OutNodes)
{
	if (!ValuePin)
	{
		return;
	}

	for (UEdGraphPin* LinkedPin : ValuePin->LinkedTo)
	{
		UEdGraphNode* SourceNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
		if (!SourceNode || OutNodes.Contains(SourceNode) || IMP_IsEventStableIdNode(SourceNode))
		{
			continue;
		}

		OutNodes.Add(SourceNode);
		for (UEdGraphPin* Pin : SourceNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			IMP_CollectPureDependencyNodes(Pin, OutNodes);
		}
	}
}

static void IMP_CollectDownstreamExecNodes(UEdGraphPin* ExecOutPin, TSet<UEdGraphNode*>& OutNodes)
{
	if (!ExecOutPin)
	{
		return;
	}

	for (UEdGraphPin* LinkedPin : ExecOutPin->LinkedTo)
	{
		UEdGraphNode* NextNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
		if (!NextNode || OutNodes.Contains(NextNode) || Cast<UK2Node_Event>(NextNode))
		{
			continue;
		}

		OutNodes.Add(NextNode);
		for (UEdGraphPin* Pin : NextNode->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				IMP_CollectPureDependencyNodes(Pin, OutNodes);
				continue;
			}
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				IMP_CollectDownstreamExecNodes(Pin, OutNodes);
			}
		}
	}
}


static void IMP_ClearExistingEventExecChain(UK2Node_Event* EventNode, FBPImportContext& Ctx)
{
	if (!EventNode || !Ctx.Graph)
	{
		return;
	}

	TSet<UEdGraphNode*> NodesToRemove;
	if (UEdGraphPin* ExecOutPin = IMP_GetExecOutput(EventNode))
	{
		IMP_CollectDownstreamExecNodes(ExecOutPin, NodesToRemove);
		ExecOutPin->BreakAllPinLinks();
	}

	for (UEdGraphNode* Node : NodesToRemove)
	{
		Ctx.Graph->RemoveNode(Node);
	}
}

static UK2Node_Event* IMP_FindReusableEventNode(const FLispNodePtr& EventForm, const FString& EventName, UFunction* EventSignatureFunc, bool bHasExplicitParams, FBPImportContext& Ctx)
{
	if (!Ctx.Graph || Ctx.ImportMode == FBlueprintLispConverter::EImportMode::ReplaceGraph)
	{
		return nullptr;
	}

	TArray<UEdGraphNode*> ExistingEventNodes;
	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			if (!Ctx.ConsumedRootEventGuids.Contains(EventNode->NodeGuid))
			{
				ExistingEventNodes.Add(EventNode);
			}
		}
	}

	const FString RequestedEventId = IMP_GetKeywordAtomValue(EventForm, TEXT(":event-id")).ToLower();
	if (!RequestedEventId.IsEmpty())
	{
		TMap<FString, UEdGraphNode*> StableIdToNode;
		IMP_BuildStableIdIndex(Ctx.Graph, true, StableIdToNode, nullptr, &Ctx.ConsumedRootEventGuids);
		if (UEdGraphNode* const* FoundNode = StableIdToNode.Find(RequestedEventId))

		{
			if (UK2Node_Event* MatchedById = Cast<UK2Node_Event>(*FoundNode))
			{
				if (IMP_IsCompatibleExistingEventNode(MatchedById, EventName, EventSignatureFunc, bHasExplicitParams))
				{
					return MatchedById;
				}
			}
		}
	}


	for (UEdGraphNode* Node : ExistingEventNodes)
	{
		if (UK2Node_Event* Candidate = Cast<UK2Node_Event>(Node))
		{
			if (IMP_IsCompatibleExistingEventNode(Candidate, EventName, EventSignatureFunc, bHasExplicitParams))
			{
				return Candidate;
			}
		}
	}

	return nullptr;
}

static bool IMP_IsCompatibleExistingInputActionNode(UK2Node_InputAction* InputNode, const FString& RequestedActionName)
{
	return InputNode
		&& InputNode->InputActionName.ToString().Equals(RequestedActionName, ESearchCase::IgnoreCase);
}

static UK2Node_InputAction* IMP_FindReusableInputActionNode(const FLispNodePtr& Form, const FString& ActionName, FBPImportContext& Ctx)
{
	if (!Ctx.Graph || Ctx.ImportMode == FBlueprintLispConverter::EImportMode::ReplaceGraph)
	{
		return nullptr;
	}

	TArray<UEdGraphNode*> ExistingInputNodes;
	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (UK2Node_InputAction* InputNode = Cast<UK2Node_InputAction>(Node))
		{
			if (!Ctx.ConsumedRootEventGuids.Contains(InputNode->NodeGuid))
			{
				ExistingInputNodes.Add(InputNode);
			}
		}
	}

	const FString RequestedEventId = IMP_GetKeywordAtomValue(Form, TEXT(":event-id")).ToLower();
	if (!RequestedEventId.IsEmpty())
	{
		TMap<FString, UEdGraphNode*> StableIdToNode;
		IMP_BuildStableIdIndex(Ctx.Graph, true, StableIdToNode, nullptr, &Ctx.ConsumedRootEventGuids);
		if (UEdGraphNode* const* FoundNode = StableIdToNode.Find(RequestedEventId))
		{
			if (UK2Node_InputAction* MatchedById = Cast<UK2Node_InputAction>(*FoundNode))
			{
				if (IMP_IsCompatibleExistingInputActionNode(MatchedById, ActionName))
				{
					return MatchedById;
				}
			}
		}
	}

	for (UEdGraphNode* Node : ExistingInputNodes)
	{
		if (UK2Node_InputAction* Candidate = Cast<UK2Node_InputAction>(Node))
		{
			if (IMP_IsCompatibleExistingInputActionNode(Candidate, ActionName))
			{
				return Candidate;
			}
		}
	}

	return nullptr;
}

static bool IMP_IsCompatibleExistingInputKeyNode(UK2Node_InputKey* InputNode, const FString& RequestedKeyName)
{
	if (!InputNode)
	{
		return false;
	}

	FString ExistingKeyName = InputNode->InputKey.GetFName().ToString();
	if (ExistingKeyName.IsEmpty())
	{
		ExistingKeyName = InputNode->InputKey.ToString();
	}
	return ExistingKeyName.Equals(RequestedKeyName, ESearchCase::IgnoreCase);
}

static UK2Node_InputKey* IMP_FindReusableInputKeyNode(const FLispNodePtr& Form, const FString& KeyName, FBPImportContext& Ctx)
{
	if (!Ctx.Graph || Ctx.ImportMode == FBlueprintLispConverter::EImportMode::ReplaceGraph)
	{
		return nullptr;
	}

	TArray<UEdGraphNode*> ExistingInputNodes;
	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (UK2Node_InputKey* InputNode = Cast<UK2Node_InputKey>(Node))
		{
			if (!Ctx.ConsumedRootEventGuids.Contains(InputNode->NodeGuid))
			{
				ExistingInputNodes.Add(InputNode);
			}
		}
	}

	const FString RequestedEventId = IMP_GetKeywordAtomValue(Form, TEXT(":event-id")).ToLower();
	if (!RequestedEventId.IsEmpty())
	{
		TMap<FString, UEdGraphNode*> StableIdToNode;
		IMP_BuildStableIdIndex(Ctx.Graph, true, StableIdToNode, nullptr, &Ctx.ConsumedRootEventGuids);
		if (UEdGraphNode* const* FoundNode = StableIdToNode.Find(RequestedEventId))
		{
			if (UK2Node_InputKey* MatchedById = Cast<UK2Node_InputKey>(*FoundNode))
			{
				if (IMP_IsCompatibleExistingInputKeyNode(MatchedById, KeyName))
				{
					return MatchedById;
				}
			}
		}
	}

	for (UEdGraphNode* Node : ExistingInputNodes)
	{
		if (UK2Node_InputKey* Candidate = Cast<UK2Node_InputKey>(Node))
		{
			if (IMP_IsCompatibleExistingInputKeyNode(Candidate, KeyName))
			{
				return Candidate;
			}
		}
	}

	return nullptr;
}

static FString IMP_GetComponentBoundEventDelegateName(UK2Node_ComponentBoundEvent* EventNode)
{
	if (!EventNode)
	{
		return FString();
	}

	FString DelegateName = EventNode->GetDocumentationExcerptName();
	if (DelegateName.IsEmpty())
	{
		if (FMulticastDelegateProperty* DelegateProperty = EventNode->GetTargetDelegateProperty())
		{
			DelegateName = DelegateProperty->GetFName().ToString();
		}
	}
	return DelegateName;
}

static bool IMP_IsCompatibleExistingComponentBoundEventNode(UK2Node_ComponentBoundEvent* EventNode, const FString& RequestedComponentName, const FString& RequestedDelegateName)
{
	return EventNode
		&& GetComponentBoundEventPropertyName(EventNode).ToString().Equals(RequestedComponentName, ESearchCase::IgnoreCase)
		&& IMP_GetComponentBoundEventDelegateName(EventNode).Equals(RequestedDelegateName, ESearchCase::IgnoreCase);
}

static UK2Node_ComponentBoundEvent* IMP_FindReusableComponentBoundEventNode(const FLispNodePtr& Form, const FString& ComponentName, const FString& DelegateName, FBPImportContext& Ctx)
{
	if (!Ctx.Graph || Ctx.ImportMode == FBlueprintLispConverter::EImportMode::ReplaceGraph)
	{
		return nullptr;
	}

	TArray<UEdGraphNode*> ExistingEventNodes;
	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (UK2Node_ComponentBoundEvent* EventNode = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			if (!Ctx.ConsumedRootEventGuids.Contains(EventNode->NodeGuid))
			{
				ExistingEventNodes.Add(EventNode);
			}
		}
	}

	const FString RequestedEventId = IMP_GetKeywordAtomValue(Form, TEXT(":event-id")).ToLower();
	if (!RequestedEventId.IsEmpty())
	{
		TMap<FString, UEdGraphNode*> StableIdToNode;
		IMP_BuildStableIdIndex(Ctx.Graph, true, StableIdToNode, nullptr, &Ctx.ConsumedRootEventGuids);
		if (UEdGraphNode* const* FoundNode = StableIdToNode.Find(RequestedEventId))
		{
			if (UK2Node_ComponentBoundEvent* MatchedById = Cast<UK2Node_ComponentBoundEvent>(*FoundNode))
			{
				if (IMP_IsCompatibleExistingComponentBoundEventNode(MatchedById, ComponentName, DelegateName))
				{
					return MatchedById;
				}
			}
		}
	}

	for (UEdGraphNode* Node : ExistingEventNodes)
	{
		if (UK2Node_ComponentBoundEvent* Candidate = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			if (IMP_IsCompatibleExistingComponentBoundEventNode(Candidate, ComponentName, DelegateName))
			{
				return Candidate;
			}
		}
	}

	return nullptr;
}

static FString IMP_GetActorBoundEventActorName(UK2Node_ActorBoundEvent* EventNode)
{
	if (!EventNode)
	{
		return FString();
	}

	if (AActor* EventOwner = EventNode->GetReferencedLevelActor())
	{
		const FString ActorLabel = EventOwner->GetActorLabel();
		return ActorLabel.IsEmpty() ? EventOwner->GetName() : ActorLabel;
	}

	return FString();
}

static FString IMP_GetActorBoundEventDelegateName(UK2Node_ActorBoundEvent* EventNode)
{
	if (!EventNode)
	{
		return FString();
	}

	FString DelegateName = EventNode->GetDocumentationExcerptName();
	if (DelegateName.IsEmpty())
	{
		if (FMulticastDelegateProperty* DelegateProperty = EventNode->GetTargetDelegateProperty())
		{
			DelegateName = DelegateProperty->GetFName().ToString();
		}
	}
	return DelegateName;
}

static bool IMP_IsCompatibleExistingActorBoundEventNode(UK2Node_ActorBoundEvent* EventNode, const FString& RequestedActorName, const FString& RequestedDelegateName)
{
	return EventNode
		&& IMP_GetActorBoundEventActorName(EventNode).Equals(RequestedActorName, ESearchCase::IgnoreCase)
		&& IMP_GetActorBoundEventDelegateName(EventNode).Equals(RequestedDelegateName, ESearchCase::IgnoreCase);
}

static UK2Node_ActorBoundEvent* IMP_FindReusableActorBoundEventNode(const FLispNodePtr& Form, const FString& ActorName, const FString& DelegateName, FBPImportContext& Ctx)
{
	if (!Ctx.Graph || Ctx.ImportMode == FBlueprintLispConverter::EImportMode::ReplaceGraph)
	{
		return nullptr;
	}

	TArray<UEdGraphNode*> ExistingEventNodes;
	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (UK2Node_ActorBoundEvent* EventNode = Cast<UK2Node_ActorBoundEvent>(Node))
		{
			if (!Ctx.ConsumedRootEventGuids.Contains(EventNode->NodeGuid))
			{
				ExistingEventNodes.Add(EventNode);
			}
		}
	}

	const FString RequestedEventId = IMP_GetKeywordAtomValue(Form, TEXT(":event-id")).ToLower();
	if (!RequestedEventId.IsEmpty())
	{
		TMap<FString, UEdGraphNode*> StableIdToNode;
		IMP_BuildStableIdIndex(Ctx.Graph, true, StableIdToNode, nullptr, &Ctx.ConsumedRootEventGuids);
		if (UEdGraphNode* const* FoundNode = StableIdToNode.Find(RequestedEventId))
		{
			if (UK2Node_ActorBoundEvent* MatchedById = Cast<UK2Node_ActorBoundEvent>(*FoundNode))
			{
				if (IMP_IsCompatibleExistingActorBoundEventNode(MatchedById, ActorName, DelegateName))
				{
					return MatchedById;
				}
			}
		}
	}

	for (UEdGraphNode* Node : ExistingEventNodes)
	{
		if (UK2Node_ActorBoundEvent* Candidate = Cast<UK2Node_ActorBoundEvent>(Node))
		{
			if (IMP_IsCompatibleExistingActorBoundEventNode(Candidate, ActorName, DelegateName))
			{
				return Candidate;
			}
		}
	}

	return nullptr;
}

static void IMP_CollectUnsupportedForms(const FLispNodePtr& Node, TSet<FString>& OutForms)

{
	if (!Node.IsValid() || !Node->IsList() || Node->Num() == 0) return;

	for (int32 i = 0; i < Node->Num(); ++i)
	{
		IMP_CollectUnsupportedForms(Node->Get(i), OutForms);
	}
}


static bool IMP_ValidateImportCoverage(const TArray<FLispNodePtr>& Nodes, FBPImportContext& Ctx)
{
	TSet<FString> UnsupportedForms;
	for (const FLispNodePtr& Node : Nodes)
	{
		IMP_CollectUnsupportedForms(Node, UnsupportedForms);
	}
	if (UnsupportedForms.Num() == 0)
	{
		return true;
	}

	TArray<FString> SortedForms = UnsupportedForms.Array();
	SortedForms.Sort();
	Ctx.Errors.Add(FString::Printf(
		TEXT("Import aborted: unsupported DSL forms detected: %s"),
		*FString::Join(SortedForms, TEXT(", "))));
	return false;
}

static bool IMP_TryCreateConnection(UEdGraph* Graph, UEdGraphPin* Src, UEdGraphPin* Dst, FString* OutError = nullptr)
{
	if (!Src || !Dst)
	{
		if (OutError) *OutError = TEXT("null pin");
		return false;
	}
	if (!Graph || !Graph->GetSchema())
	{
		if (OutError) *OutError = TEXT("graph schema is null");
		return false;
	}
	if (Graph->GetSchema()->TryCreateConnection(Src, Dst))
	{
		return true;
	}

	if (OutError)
	{
		const FString SrcTypeObject = Src->PinType.PinSubCategoryObject.IsValid()
			? Src->PinType.PinSubCategoryObject->GetPathName() : TEXT("<none>");
		const FString DstTypeObject = Dst->PinType.PinSubCategoryObject.IsValid()
			? Dst->PinType.PinSubCategoryObject->GetPathName() : TEXT("<none>");
		*OutError = FString::Printf(
			TEXT("schema rejected connection %s.%s (%s:%s) -> %s.%s (%s:%s)"),
			*Src->GetOwningNode()->GetName(),
			*Src->PinName.ToString(),
			*Src->PinType.PinCategory.ToString(),
			*SrcTypeObject,
			*Dst->GetOwningNode()->GetName(),
			*Dst->PinName.ToString(),
			*Dst->PinType.PinCategory.ToString(),
			*DstTypeObject);
	}
	return false;
}

// --- Pin helpers ---
static FString IMP_NormalizePinLookupName(const FString& Name);
static FString IMP_StripGeneratedPinSuffixes(const FString& Name);

static bool IMP_DoesPinNameMatchLookup(
	const FString& CandidateName,
	const FString& RequestedName,
	const FString& RequestedNoSpaces,
	const FString& RequestedNormalized,
	const FString& RequestedStrippedNoSpaces,
	const FString& RequestedStrippedNormalized)
{
	if (CandidateName.Equals(RequestedName, ESearchCase::IgnoreCase))
	{
		return true;
	}

	const FString CandidateStripped = IMP_StripGeneratedPinSuffixes(CandidateName);
	const FString CandidateNoSpaces = CandidateName.Replace(TEXT(" "), TEXT(""));
	const FString CandidateStrippedNoSpaces = CandidateStripped.Replace(TEXT(" "), TEXT(""));
	if (!RequestedNoSpaces.IsEmpty()
		&& (CandidateNoSpaces.Equals(RequestedNoSpaces, ESearchCase::IgnoreCase)
			|| CandidateStrippedNoSpaces.Equals(RequestedNoSpaces, ESearchCase::IgnoreCase)))
	{
		return true;
	}
	if (!RequestedStrippedNoSpaces.IsEmpty()
		&& (CandidateNoSpaces.Equals(RequestedStrippedNoSpaces, ESearchCase::IgnoreCase)
			|| CandidateStrippedNoSpaces.Equals(RequestedStrippedNoSpaces, ESearchCase::IgnoreCase)))
	{
		return true;
	}

	const FString CandidateNormalized = IMP_NormalizePinLookupName(CandidateName);
	const FString CandidateStrippedNormalized = IMP_NormalizePinLookupName(CandidateStripped);
	if (!RequestedNormalized.IsEmpty()
		&& (CandidateNormalized.Equals(RequestedNormalized, ESearchCase::IgnoreCase)
			|| CandidateStrippedNormalized.Equals(RequestedNormalized, ESearchCase::IgnoreCase)))
	{
		return true;
	}
	if (!RequestedStrippedNormalized.IsEmpty()
		&& (CandidateNormalized.Equals(RequestedStrippedNormalized, ESearchCase::IgnoreCase)
			|| CandidateStrippedNormalized.Equals(RequestedStrippedNormalized, ESearchCase::IgnoreCase)))
	{
		return true;
	}

	const FString CandidateWithoutLeadingIn = CandidateNormalized.StartsWith(TEXT("in")) ? CandidateNormalized.Mid(2) : CandidateNormalized;
	const FString CandidateStrippedWithoutLeadingIn = CandidateStrippedNormalized.StartsWith(TEXT("in")) ? CandidateStrippedNormalized.Mid(2) : CandidateStrippedNormalized;
	const FString RequestedWithoutLeadingIn = RequestedNormalized.StartsWith(TEXT("in")) ? RequestedNormalized.Mid(2) : RequestedNormalized;
	const FString RequestedStrippedWithoutLeadingIn = RequestedStrippedNormalized.StartsWith(TEXT("in")) ? RequestedStrippedNormalized.Mid(2) : RequestedStrippedNormalized;
	if (!RequestedWithoutLeadingIn.IsEmpty()
		&& (CandidateNormalized.Equals(RequestedWithoutLeadingIn, ESearchCase::IgnoreCase)
			|| CandidateStrippedNormalized.Equals(RequestedWithoutLeadingIn, ESearchCase::IgnoreCase)))
	{
		return true;
	}
	if (!RequestedStrippedWithoutLeadingIn.IsEmpty()
		&& (CandidateNormalized.Equals(RequestedStrippedWithoutLeadingIn, ESearchCase::IgnoreCase)
			|| CandidateStrippedNormalized.Equals(RequestedStrippedWithoutLeadingIn, ESearchCase::IgnoreCase)))
	{
		return true;
	}
	if ((!CandidateWithoutLeadingIn.IsEmpty() && CandidateWithoutLeadingIn.Equals(RequestedNormalized, ESearchCase::IgnoreCase))
		|| (!CandidateStrippedWithoutLeadingIn.IsEmpty() && CandidateStrippedWithoutLeadingIn.Equals(RequestedNormalized, ESearchCase::IgnoreCase)))
	{
		return true;
	}
	if ((!CandidateWithoutLeadingIn.IsEmpty() && CandidateWithoutLeadingIn.Equals(RequestedStrippedNormalized, ESearchCase::IgnoreCase))
		|| (!CandidateStrippedWithoutLeadingIn.IsEmpty() && CandidateStrippedWithoutLeadingIn.Equals(RequestedStrippedNormalized, ESearchCase::IgnoreCase)))
	{
		return true;
	}

	return false;
}


static UEdGraphPin* IMP_FindPinByNameRecursive(
	const TArray<UEdGraphPin*>& Pins,
	EEdGraphPinDirection Direction,
	const FString& RequestedName,
	const FString& RequestedNoSpaces,
	const FString& RequestedNormalized,
	const FString& RequestedStrippedNoSpaces,
	const FString& RequestedStrippedNormalized,
	bool bSkipExecPins)
{
	for (UEdGraphPin* P : Pins)
	{
		if (!P) continue;

		if (P->Direction == Direction)
		{
			if ((!bSkipExecPins || P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				&& IMP_DoesPinNameMatchLookup(P->PinName.ToString(), RequestedName, RequestedNoSpaces, RequestedNormalized, RequestedStrippedNoSpaces, RequestedStrippedNormalized))
			{
				return P;
			}
		}

		if (P->SubPins.Num() > 0)
		{
			if (UEdGraphPin* NestedMatch = IMP_FindPinByNameRecursive(P->SubPins, Direction, RequestedName, RequestedNoSpaces, RequestedNormalized, RequestedStrippedNoSpaces, RequestedStrippedNormalized, bSkipExecPins))
			{
				return NestedMatch;
			}
		}
	}

	return nullptr;
}

static UEdGraphPin* IMP_FindOutputPinByName(UEdGraphNode* N, const FString& Name)
{
	if (!N || Name.IsEmpty()) return nullptr;

	const FString RequestedNoSpaces = Name.Replace(TEXT(" "), TEXT(""));
	const FString RequestedNormalized = IMP_NormalizePinLookupName(Name);
	const FString RequestedStripped = IMP_StripGeneratedPinSuffixes(Name);
	const FString RequestedStrippedNoSpaces = RequestedStripped.Replace(TEXT(" "), TEXT(""));
	const FString RequestedStrippedNormalized = IMP_NormalizePinLookupName(RequestedStripped);

	return IMP_FindPinByNameRecursive(N->Pins, EGPD_Output, Name, RequestedNoSpaces, RequestedNormalized, RequestedStrippedNoSpaces, RequestedStrippedNormalized, true);
}
static UEdGraphPin* IMP_FindOutputPin(UEdGraphNode* N, const FString& Name)
{
	if (!N) return nullptr;
	if (UEdGraphPin* Exact = IMP_FindOutputPinByName(N, Name)) return Exact;
	for (UEdGraphPin* P : N->Pins)
		if (P && P->Direction == EGPD_Output && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && !P->bHidden)
			return P;
	return nullptr;
}

static UEdGraphPin* IMP_GetDeclaredCallOutputPin(UK2Node_CallFunction* CallNode, const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (!CallNode) return nullptr;

	const FString DeclaredOutputName = IMP_GetKeywordAtomValue(Form, TEXT(":out-pin"));
	if (DeclaredOutputName.IsEmpty())
	{
		return IMP_FindOutputPin(CallNode, TEXT("ReturnValue"));
	}

	if (UEdGraphPin* DeclaredOutputPin = IMP_FindOutputPinByName(CallNode, DeclaredOutputName))
	{
		return DeclaredOutputPin;
	}

	Ctx.Errors.Add(FString::Printf(TEXT("IMP: declared call output pin not found: %s.%s"),
		*CallNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *DeclaredOutputName));
	return nullptr;
}

static FString IMP_NormalizePinLookupName(const FString& Name)
{
	FString Normalized = Name.TrimStartAndEnd().ToLower();
	Normalized.ReplaceInline(TEXT(" "), TEXT(""));
	Normalized.ReplaceInline(TEXT("_"), TEXT(""));
	Normalized.ReplaceInline(TEXT("-"), TEXT(""));
	Normalized.ReplaceInline(TEXT("."), TEXT(""));
	return Normalized;
}

static FString IMP_StripGeneratedPinSuffixes(const FString& Name)
{
	FString Result = Name;
	while (true)
	{
		int32 LastUnderscore = INDEX_NONE;
		if (!Result.FindLastChar(TEXT('_'), LastUnderscore) || LastUnderscore <= 0)
		{
			break;
		}

		const FString Tail = Result.Mid(LastUnderscore + 1);
		bool bAllDigits = !Tail.IsEmpty();
		bool bAllHexDigits = Tail.Len() == 32;
		for (const TCHAR Ch : Tail)
		{
			if (!FChar::IsDigit(Ch))
			{
				bAllDigits = false;
			}
			if (!FChar::IsHexDigit(Ch))
			{
				bAllHexDigits = false;
			}
		}

		if (!bAllDigits && !bAllHexDigits)
		{
			break;
		}

		Result = Result.Left(LastUnderscore);
	}

	return Result;
}

static UEdGraphPin* IMP_FindInputPin(UEdGraphNode* N, const FString& Name)
{
	if (!N || Name.IsEmpty()) return nullptr;

	const FString RequestedNoSpaces = Name.Replace(TEXT(" "), TEXT(""));
	const FString RequestedNormalized = IMP_NormalizePinLookupName(Name);
	const FString RequestedStripped = IMP_StripGeneratedPinSuffixes(Name);
	const FString RequestedStrippedNoSpaces = RequestedStripped.Replace(TEXT(" "), TEXT(""));
	const FString RequestedStrippedNormalized = IMP_NormalizePinLookupName(RequestedStripped);

	auto SearchExistingPins = [&]() -> UEdGraphPin*
	{
		return IMP_FindPinByNameRecursive(N->Pins, EGPD_Input, Name, RequestedNoSpaces, RequestedNormalized, RequestedStrippedNoSpaces, RequestedStrippedNormalized, false);
	};

	if (UEdGraphPin* DirectMatch = SearchExistingPins())
	{
		return DirectMatch;
	}

	const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(N->GetSchema());
	bool bExpandedStructPin = false;
	if (K2Schema)
	{
		const TArray<UEdGraphPin*> RootPins = N->Pins;
		for (UEdGraphPin* P : RootPins)
		{
			if (!P || P->GetOwningNode() != N) continue;
			if (P->Direction != EGPD_Input) continue;
			if (P->ParentPin != nullptr) continue;
			if (P->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct) continue;
			if (P->SubPins.Num() > 0) continue;
			if (!P->PinType.PinSubCategoryObject.IsValid()) continue;

			const FString RootNormalized = IMP_NormalizePinLookupName(P->PinName.ToString());
			if (RootNormalized.IsEmpty()) continue;

			const bool bLooksLikeSplitChild = (RequestedNormalized.StartsWith(RootNormalized) && RequestedNormalized != RootNormalized)
				|| (RequestedStrippedNormalized.StartsWith(RootNormalized) && RequestedStrippedNormalized != RootNormalized);
			if (!bLooksLikeSplitChild)
			{
				continue;
			}

			K2Schema->SplitPin(P, false);
			bExpandedStructPin = true;
		}
	}

	if (bExpandedStructPin)
	{
		return SearchExistingPins();
	}

	return nullptr;
}


static UEdGraphPin* IMP_GetExecOutput(UEdGraphNode* N)
{
	if (!N) return nullptr;
	for (UEdGraphPin* P : N->Pins)
		if (P && P->Direction == EGPD_Output && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			return P;
	return nullptr;
}
static UEdGraphPin* IMP_GetExecInput(UEdGraphNode* N)
{
	if (!N) return nullptr;
	for (UEdGraphPin* P : N->Pins)
		if (P && P->Direction == EGPD_Input && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			return P;
	return nullptr;
}
static void IMP_EnsureGuid(UEdGraphNode* N) { if (N && !N->NodeGuid.IsValid()) N->CreateNewGuid(); }

static void IMP_RegisterBoundValue(const FString& VarName, UEdGraphPin* ValuePin, FBPImportContext& Ctx)
{
	if (VarName.IsEmpty() || !ValuePin) return;

	UEdGraphNode* SourceNode = ValuePin->GetOwningNode();
	if (!SourceNode) return;

	IMP_EnsureGuid(SourceNode);
	const FString NodeGuid = SourceNode->NodeGuid.ToString();
	const FString PinName = ValuePin->PinName.ToString();

	auto RegisterAlias = [&Ctx, &NodeGuid, &PinName](const FString& Alias)
	{
		if (Alias.IsEmpty()) return;
		Ctx.VariableToNodeId.Add(Alias, NodeGuid);
		Ctx.VariableToPin.Add(Alias, PinName);
		const FString NoSpaces = Alias.Replace(TEXT(" "), TEXT(""));
		if (NoSpaces != Alias)
		{
			Ctx.VariableToNodeId.Add(NoSpaces, NodeGuid);
			Ctx.VariableToPin.Add(NoSpaces, PinName);
		}
	};

	RegisterAlias(VarName);
	Ctx.TempIdToNode.FindOrAdd(NodeGuid) = SourceNode;
	Ctx.TempIdToNode.Add(TEXT("_var_") + VarName, SourceNode);
}

static bool IMP_ShouldIgnoreCallKeyword(const FString& KeywordName)
{
	return KeywordName.Equals(TEXT("id"), ESearchCase::IgnoreCase)
		|| KeywordName.Equals(TEXT("pos"), ESearchCase::IgnoreCase)
		|| KeywordName.Equals(TEXT("owner"), ESearchCase::IgnoreCase)
		|| KeywordName.Equals(TEXT("call-kind"), ESearchCase::IgnoreCase)
		|| KeywordName.Equals(TEXT("out-pin"), ESearchCase::IgnoreCase)
		|| KeywordName.Equals(TEXT("result-type-object"), ESearchCase::IgnoreCase);
}

static void IMP_ApplyCallInputs(UK2Node_CallFunction* CallNode, const FLispNodePtr& Form, int32 StartIndex, bool bTreatFirstPositionalAsSelf, FBPImportContext& Ctx)
{
	if (!CallNode || !Form.IsValid()) return;

	TArray<UEdGraphPin*> DataInputPins;
	for (UEdGraphPin* Pin : CallNode->Pins)
	{
		if (!Pin) continue;
		if (Pin->Direction != EGPD_Input) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
		if (Pin->bHidden) continue;
		DataInputPins.Add(Pin);
	}

	TSet<FString> AssignedPins;
	auto MarkAssigned = [&AssignedPins](UEdGraphPin* Pin)
	{
		if (Pin)
		{
			AssignedPins.Add(Pin->PinName.ToString());
		}
	};

	int32 ArgIndex = StartIndex;
	const bool bHasDeclaredOwner = Form->HasKeyword(TEXT(":owner"));
	while (ArgIndex + 1 < Form->Num() && Form->Get(ArgIndex)->IsKeyword())
	{
		const FString RawKeyword = Form->Get(ArgIndex)->StringValue;
		const FString KeywordName = RawKeyword.StartsWith(TEXT(":")) ? RawKeyword.Mid(1) : RawKeyword;
		if (!IMP_ShouldIgnoreCallKeyword(KeywordName)) break;
		ArgIndex += 2;
	}
	if (bTreatFirstPositionalAsSelf && !bHasDeclaredOwner && ArgIndex < Form->Num() && !Form->Get(ArgIndex)->IsKeyword())
	{
		if (UEdGraphPin* SelfPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Self))
		{
			if (!SelfPin->bHidden)
			{
				IMP_SetPinFromExpr(SelfPin, Form->Get(ArgIndex), Ctx);
				MarkAssigned(SelfPin);
				++ArgIndex;
			}
		}
	}


	int32 NextPositionalPin = 0;
	auto ConsumeNextDataPin = [&DataInputPins, &AssignedPins, &NextPositionalPin]() -> UEdGraphPin*
	{
		while (NextPositionalPin < DataInputPins.Num())
		{
			UEdGraphPin* Candidate = DataInputPins[NextPositionalPin++];
			if (Candidate && !AssignedPins.Contains(Candidate->PinName.ToString()))
			{
				return Candidate;
			}
		}
		return nullptr;
	};

	TMap<FString, UEdGraphNode*> StructInputBuilders;
	auto TryAssignStructMemberKeyword = [&Ctx, CallNode, &StructInputBuilders, &MarkAssigned](const FString& KeywordName, const FLispNodePtr& ValueNode) -> bool
	{
		const FString StableKeywordName = IMP_StripGeneratedPinSuffixes(KeywordName);
		int32 FirstUnderscore = INDEX_NONE;
		if (!StableKeywordName.FindChar(TEXT('_'), FirstUnderscore) || FirstUnderscore <= 0 || FirstUnderscore >= StableKeywordName.Len() - 1)
		{
			return false;
		}

		const FString RootPinName = StableKeywordName.Left(FirstUnderscore);
		const FString MemberPinName = StableKeywordName.Mid(FirstUnderscore + 1);
		UEdGraphPin* RootInputPin = IMP_FindInputPin(CallNode, RootPinName);
		if (!RootInputPin || RootInputPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
		{
			return false;
		}

		UScriptStruct* StructType = Cast<UScriptStruct>(RootInputPin->PinType.PinSubCategoryObject.Get());
		if (!StructType)
		{
			return false;
		}

		const FString MemberRequestedNoSpaces = MemberPinName.Replace(TEXT(" "), TEXT(""));
		const FString MemberRequestedNormalized = IMP_NormalizePinLookupName(MemberPinName);
		const FString MemberRequestedStripped = IMP_StripGeneratedPinSuffixes(MemberPinName);
		const FString MemberRequestedStrippedNoSpaces = MemberRequestedStripped.Replace(TEXT(" "), TEXT(""));
		const FString MemberRequestedStrippedNormalized = IMP_NormalizePinLookupName(MemberRequestedStripped);

		if (RootInputPin->SubPins.Num() == 0)
		{
			if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(CallNode->GetSchema()))
			{
				K2Schema->SplitPin(RootInputPin, false);
			}
		}

		if (UEdGraphPin* DirectMemberInputPin = IMP_FindPinByNameRecursive(RootInputPin->SubPins, EGPD_Input, MemberPinName, MemberRequestedNoSpaces, MemberRequestedNormalized, MemberRequestedStrippedNoSpaces, MemberRequestedStrippedNormalized, false))
		{
			IMP_SetPinFromExpr(DirectMemberInputPin, ValueNode, Ctx);
			MarkAssigned(RootInputPin);
			return true;
		}

		const FString BuilderKey = RootInputPin->PinName.ToString();
		UEdGraphNode* StructBuilderNode = StructInputBuilders.FindRef(BuilderKey);
		if (!StructBuilderNode)
		{
			if (StructType->HasMetaData(FBlueprintMetadata::MD_NativeMakeFunction))
			{
				const FString NativeMakeFunctionPath = StructType->GetMetaData(FBlueprintMetadata::MD_NativeMakeFunction);
				if (UFunction* NativeMakeFunction = FindObject<UFunction>(nullptr, *NativeMakeFunctionPath, true))
				{
					UK2Node_CallFunction* MakeCallNode = NewObject<UK2Node_CallFunction>(Ctx.Graph);
					MakeCallNode->SetFromFunction(NativeMakeFunction);
					StructBuilderNode = MakeCallNode;
				}
			}

			if (!StructBuilderNode)
			{
				UK2Node_MakeStruct* MakeStructNode = NewObject<UK2Node_MakeStruct>(Ctx.Graph);
				MakeStructNode->StructType = StructType;
				StructBuilderNode = MakeStructNode;
			}

			StructBuilderNode->NodePosX = Ctx.CurrentX;
			StructBuilderNode->NodePosY = Ctx.CurrentY;
			Ctx.Graph->AddNode(StructBuilderNode, false, false);
			StructBuilderNode->AllocateDefaultPins();
			IMP_EnsureGuid(StructBuilderNode);
			Ctx.AdvancePosition();
			Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), StructBuilderNode);
			StructInputBuilders.Add(BuilderKey, StructBuilderNode);

			if (UEdGraphPin* StructOutputPin = IMP_FindOutputPin(StructBuilderNode, TEXT("")))
			{
				IMP_Connect(StructOutputPin, RootInputPin, Ctx);
			}
		}

		UEdGraphPin* MemberInputPin = IMP_FindInputPin(StructBuilderNode, MemberPinName);
		if (!MemberInputPin)
		{
			const FString BuilderTitle = StructBuilderNode ? StructBuilderNode->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("<null>");
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: struct builder input pin not found: %s.%s via %s"), *StructType->GetName(), *MemberPinName, *BuilderTitle));
			return true;
		}

		IMP_SetPinFromExpr(MemberInputPin, ValueNode, Ctx);
		MarkAssigned(RootInputPin);
		return true;
	};

	for (; ArgIndex < Form->Num(); ++ArgIndex)
	{
		const FLispNodePtr ArgNode = Form->Get(ArgIndex);
		if (!ArgNode.IsValid()) continue;

		if (ArgNode->IsKeyword())
		{
			const FString Keyword = ArgNode->StringValue;
			const FString KeywordName = Keyword.StartsWith(TEXT(":")) ? Keyword.Mid(1) : Keyword;
			if (ArgIndex + 1 >= Form->Num())
			{
				break;
			}

			const FLispNodePtr ValueNode = Form->Get(++ArgIndex);
			if (IMP_ShouldIgnoreCallKeyword(KeywordName))
			{
				continue;
			}

			UEdGraphPin* InputPin = IMP_FindInputPin(CallNode, KeywordName);
			if (!InputPin)
			{
				if (UK2Node_CommutativeAssociativeBinaryOperator* VariadicNode = Cast<UK2Node_CommutativeAssociativeBinaryOperator>(CallNode))
				{
					for (int32 AddedPinCount = 0; AddedPinCount < 128 && VariadicNode->CanAddPin(); ++AddedPinCount)
					{
						VariadicNode->AddInputPin();
						InputPin = IMP_FindInputPin(CallNode, KeywordName);
						if (InputPin) break;
					}
				}
			}

			if (InputPin)
			{
				IMP_SetPinFromExpr(InputPin, ValueNode, Ctx);
				MarkAssigned(InputPin);
			}
			else if (!TryAssignStructMemberKeyword(KeywordName, ValueNode))
			{
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: call input pin not found: %s.%s"), *CallNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *KeywordName));
			}
			continue;
		}

		if (UEdGraphPin* InputPin = ConsumeNextDataPin())
		{
			IMP_SetPinFromExpr(InputPin, ArgNode, Ctx);
			MarkAssigned(InputPin);
		}
	}
}

static void IMP_UpdateCurrentExecPin(UEdGraphNode* Node, UEdGraphPin* OutExecPin, UEdGraphPin*& CurrentExecPin)
{
	if (OutExecPin)
	{
		CurrentExecPin = OutExecPin;
		return;
	}

	if (Node && !Cast<UK2Node_IfThenElse>(Node))
	{
		if (UEdGraphPin* ExecOut = IMP_GetExecOutput(Node))
		{
			CurrentExecPin = ExecOut;
		}
	}
}

// Connect two pins (source→target) with schema validation.

static bool IMP_Connect(UEdGraphPin* Src, UEdGraphPin* Dst, FBPImportContext& Ctx)
{
	FString Error;
	if (!IMP_TryCreateConnection(Ctx.Graph, Src, Dst, &Error))
	{
		Ctx.Errors.Add(FString::Printf(TEXT("IMP_Connect: %s"), *Error));
		return false;
	}
	return true;
}

static bool IMP_MapLispTypeToPinCategory(const FString& TypeName, FName& OutCategory)
{
	const FString Normalized = TypeName.TrimStartAndEnd().ToLower();
	if (Normalized == TEXT("bool"))   { OutCategory = UEdGraphSchema_K2::PC_Boolean; return true; }
	if (Normalized == TEXT("int"))    { OutCategory = UEdGraphSchema_K2::PC_Int; return true; }
	if (Normalized == TEXT("int64"))  { OutCategory = UEdGraphSchema_K2::PC_Int64; return true; }
	if (Normalized == TEXT("float") || Normalized == TEXT("double") || Normalized == TEXT("real"))
	{
		OutCategory = UEdGraphSchema_K2::PC_Float;
		return true;
	}
	if (Normalized == TEXT("string")) { OutCategory = UEdGraphSchema_K2::PC_String; return true; }
	if (Normalized == TEXT("name"))   { OutCategory = UEdGraphSchema_K2::PC_Name; return true; }
	if (Normalized == TEXT("text"))   { OutCategory = UEdGraphSchema_K2::PC_Text; return true; }
	if (Normalized == TEXT("byte"))   { OutCategory = UEdGraphSchema_K2::PC_Byte; return true; }
	if (Normalized == TEXT("vector") || Normalized == TEXT("vector2d") || Normalized == TEXT("rotator") || Normalized == TEXT("transform"))
	{
		OutCategory = UEdGraphSchema_K2::PC_Struct;
		return true;
	}
	if (Normalized == TEXT("wildcard")) { OutCategory = UEdGraphSchema_K2::PC_Wildcard; return true; }
	return false;
}


static bool IMP_TryInferLiteralPinCategory(const FLispNodePtr& Expr, FName& OutCategory)
{
	if (!Expr.IsValid() || Expr->IsNil()) return false;
	if (Expr->IsString())
	{
		OutCategory = UEdGraphSchema_K2::PC_String;
		return true;
	}
	if (Expr->IsNumber())
	{
		const double IntCandidate = static_cast<double>(static_cast<int64>(Expr->NumberValue));
		OutCategory = FMath::IsNearlyEqual(Expr->NumberValue, IntCandidate)
			? UEdGraphSchema_K2::PC_Int
			: UEdGraphSchema_K2::PC_Float;
		return true;
	}
	if (Expr->IsSymbol())
	{
		const FString S = Expr->StringValue;
		if (S.Equals(TEXT("true"), ESearchCase::IgnoreCase) || S.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			OutCategory = UEdGraphSchema_K2::PC_Boolean;
			return true;
		}
	}
	return false;
}

static void IMP_ApplyPinCategory(UEdGraphPin* Pin, const FName& PinCategory)
{
	if (!Pin || PinCategory.IsNone()) return;
	Pin->PinType.PinCategory = PinCategory;
	Pin->PinType.PinSubCategory = NAME_None;
	Pin->PinType.PinSubCategoryObject = nullptr;
}

static bool IMP_ApplyLispTypeToPin(UEdGraphPin* Pin, const FString& TypeName)
{
	if (!Pin) return false;

	const FString Normalized = TypeName.TrimStartAndEnd().ToLower();
	if (Normalized == TEXT("float"))
	{
		Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		Pin->PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		Pin->PinType.PinSubCategoryObject = nullptr;
		return true;
	}
	if (Normalized == TEXT("double") || Normalized == TEXT("real"))
	{
		Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		Pin->PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		Pin->PinType.PinSubCategoryObject = nullptr;
		return true;
	}
	if (Normalized == TEXT("vector"))
	{
		Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		Pin->PinType.PinSubCategory = NAME_None;
		Pin->PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		return true;
	}
	if (Normalized == TEXT("vector2d"))
	{
		Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		Pin->PinType.PinSubCategory = NAME_None;
		Pin->PinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
		return true;
	}
	if (Normalized == TEXT("rotator"))
	{
		Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		Pin->PinType.PinSubCategory = NAME_None;
		Pin->PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		return true;
	}
	if (Normalized == TEXT("transform"))
	{
		Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		Pin->PinType.PinSubCategory = NAME_None;
		Pin->PinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		return true;
	}

	FName PinCategory = NAME_None;
	if (!IMP_MapLispTypeToPinCategory(TypeName, PinCategory))
	{
		return false;
	}
	IMP_ApplyPinCategory(Pin, PinCategory);
	return true;
}

static void IMP_SeedMakeArrayLiteralType(UK2Node_MakeArray* MakeArrayNode, const TArray<UEdGraphPin*>& InputPins, const TArray<FLispNodePtr>& ItemExprs)

{
	if (!MakeArrayNode) return;

	FName InferredCategory = NAME_None;
	for (const FLispNodePtr& ItemExpr : ItemExprs)
	{
		FName ItemCategory = NAME_None;
		if (!IMP_TryInferLiteralPinCategory(ItemExpr, ItemCategory)
			|| ItemCategory == UEdGraphSchema_K2::PC_Wildcard)
		{
			continue;
		}
		if (InferredCategory.IsNone())
		{
			InferredCategory = ItemCategory;
		}
		else if ((InferredCategory == UEdGraphSchema_K2::PC_Int && ItemCategory == UEdGraphSchema_K2::PC_Float)
			|| (InferredCategory == UEdGraphSchema_K2::PC_Float && ItemCategory == UEdGraphSchema_K2::PC_Int))
		{
			InferredCategory = UEdGraphSchema_K2::PC_Float;
		}
	}

	if (InferredCategory.IsNone() || InferredCategory == UEdGraphSchema_K2::PC_Wildcard)
	{
		return;
	}

	if (UEdGraphPin* OutputPin = MakeArrayNode->GetOutputPin())
	{
		IMP_ApplyPinCategory(OutputPin, InferredCategory);
	}

	for (UEdGraphPin* InputPin : InputPins)
	{
		if (InputPin && InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
		{
			IMP_ApplyPinCategory(InputPin, InferredCategory);
		}
	}
}

static bool IMP_CanPinsConnectWithoutMutation(UEdGraph* Graph, UEdGraphPin* Src, UEdGraphPin* Dst)
{
	if (!Graph || !Graph->GetSchema() || !Src || !Dst)
	{
		return false;
	}

	const FPinConnectionResponse Response = Graph->GetSchema()->CanCreateConnection(Src, Dst);
	return Response.Response != CONNECT_RESPONSE_DISALLOW;
}

static int32 IMP_CountVisibleDataOutputPins(UEdGraphNode* Node)
{
	if (!Node) return 0;

	int32 Count = 0;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Pin->bHidden) continue;
		++Count;
	}
	return Count;
}

static UEdGraphPin* IMP_SelectBestMacroOutputPinForDestination(UK2Node_MacroInstance* MacroNode, UEdGraphPin* DestinationPin, UEdGraph* Graph)
{
	if (!MacroNode || !DestinationPin)
	{
		return nullptr;
	}

	TArray<UEdGraphPin*> CompatiblePins;
	for (UEdGraphPin* Candidate : MacroNode->Pins)
	{
		if (!Candidate || Candidate->Direction != EGPD_Output) continue;
		if (Candidate->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Candidate->bHidden) continue;
		if (IMP_CanPinsConnectWithoutMutation(Graph, Candidate, DestinationPin))
		{
			CompatiblePins.Add(Candidate);
		}
	}

	if (CompatiblePins.Num() == 1)
	{
		return CompatiblePins[0];
	}

	const FString DestinationName = DestinationPin->PinName.ToString();
	const FString DestinationNoSpaces = DestinationName.Replace(TEXT(" "), TEXT(""));
	for (UEdGraphPin* Candidate : CompatiblePins)
	{
		const FString CandidateName = Candidate->PinName.ToString();
		if (CandidateName.Equals(DestinationName, ESearchCase::IgnoreCase))
		{
			return Candidate;
		}
		if (!DestinationNoSpaces.IsEmpty() && CandidateName.Replace(TEXT(" "), TEXT("")).Equals(DestinationNoSpaces, ESearchCase::IgnoreCase))
		{
			return Candidate;
		}
	}

	return nullptr;
}

static UEdGraphPin* IMP_SelectBestDataOutputPinForDestination(UEdGraphNode* SourceNode, UEdGraphPin* DestinationPin, UEdGraph* Graph, const FString& PreferredOutputName = TEXT(""))
{
	if (!SourceNode || !DestinationPin)
	{
		return nullptr;
	}

	if (!PreferredOutputName.IsEmpty())
	{
		if (UEdGraphPin* PreferredPin = IMP_FindOutputPinByName(SourceNode, PreferredOutputName))
		{
			return PreferredPin;
		}
	}

	TArray<UEdGraphPin*> CompatiblePins;
	for (UEdGraphPin* Candidate : SourceNode->Pins)
	{
		if (!Candidate || Candidate->Direction != EGPD_Output) continue;
		if (Candidate->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Candidate->bHidden) continue;
		if (IMP_CanPinsConnectWithoutMutation(Graph, Candidate, DestinationPin))
		{
			CompatiblePins.Add(Candidate);
		}
	}

	if (CompatiblePins.Num() == 1)
	{
		return CompatiblePins[0];
	}

	const FString DestinationName = DestinationPin->PinName.ToString();
	const FString DestinationNoSpaces = DestinationName.Replace(TEXT(" "), TEXT(""));
	const FString DestinationNormalized = IMP_NormalizePinLookupName(DestinationName);
	const FString DestinationWithoutLeadingIn = DestinationNormalized.StartsWith(TEXT("in")) ? DestinationNormalized.Mid(2) : DestinationNormalized;
	for (UEdGraphPin* Candidate : CompatiblePins)
	{
		const FString CandidateName = Candidate->PinName.ToString();
		const FString CandidateNoSpaces = CandidateName.Replace(TEXT(" "), TEXT(""));
		const FString CandidateNormalized = IMP_NormalizePinLookupName(CandidateName);
		if (CandidateName.Equals(DestinationName, ESearchCase::IgnoreCase))
		{
			return Candidate;
		}
		if (!DestinationNoSpaces.IsEmpty() && CandidateNoSpaces.Equals(DestinationNoSpaces, ESearchCase::IgnoreCase))
		{
			return Candidate;
		}
		if (!DestinationNormalized.IsEmpty() && CandidateNormalized.Equals(DestinationNormalized, ESearchCase::IgnoreCase))
		{
			return Candidate;
		}
		if (!DestinationWithoutLeadingIn.IsEmpty() && CandidateNormalized.Equals(DestinationWithoutLeadingIn, ESearchCase::IgnoreCase))
		{
			return Candidate;
		}
		if (!DestinationWithoutLeadingIn.IsEmpty() && (DestinationWithoutLeadingIn.Contains(CandidateNormalized) || CandidateNormalized.Contains(DestinationWithoutLeadingIn)))
		{
			return Candidate;
		}
		if (!DestinationNormalized.IsEmpty() && (DestinationNormalized.Contains(CandidateNormalized) || CandidateNormalized.Contains(DestinationNormalized)))
		{
			return Candidate;
		}
	}

	return nullptr;
}

static void IMP_RegisterImportedStableNode(UEdGraphNode* Node, const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	const FString StableId = IMP_GetRequestedNodeStableId(Form);
	if (Node && !StableId.IsEmpty())
	{
		Ctx.TempIdToNode.FindOrAdd(TEXT("_stable_") + StableId) = Node;
	}
}

static UEdGraphPin* IMP_FindImportedStableOutputPin(const FLispNodePtr& Form, UEdGraphPin* DestinationPin, FBPImportContext& Ctx)
{
	const FString StableId = IMP_GetRequestedNodeStableId(Form);
	if (StableId.IsEmpty()) return nullptr;
	UEdGraphNode* ExistingNode = Ctx.TempIdToNode.FindRef(TEXT("_stable_") + StableId);
	if (!ExistingNode && Ctx.Graph)
	{
		for (UEdGraphNode* Candidate : Ctx.Graph->Nodes)
		{
			if (Candidate && Candidate->NodeGuid.ToString(EGuidFormats::Digits).StartsWith(StableId, ESearchCase::IgnoreCase))
			{
				ExistingNode = Candidate;
				Ctx.TempIdToNode.Add(TEXT("_stable_") + StableId, Candidate);
				break;
			}
		}
	}
	if (!ExistingNode) return nullptr;

	FString PreferredOutputName = IMP_GetKeywordAtomValue(Form, TEXT(":out-pin"));
	if (PreferredOutputName.IsEmpty()) PreferredOutputName = IMP_GetKeywordAtomValue(Form, TEXT(":field"));
	if (PreferredOutputName.IsEmpty()) PreferredOutputName = IMP_GetKeywordAtomValue(Form, TEXT(":out"));
	if (!PreferredOutputName.IsEmpty())
	{
		if (UEdGraphPin* PreferredPin = IMP_FindOutputPinByName(ExistingNode, PreferredOutputName)) return PreferredPin;
		const FString PreferredNormalized = IMP_NormalizePinLookupName(PreferredOutputName);
		if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Ctx.Graph->GetSchema()))
		{
			const TArray<UEdGraphPin*> RootPins = ExistingNode->Pins;
			for (UEdGraphPin* RootPin : RootPins)
			{
				if (!RootPin || RootPin->Direction != EGPD_Output || RootPin->ParentPin
					|| RootPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct) continue;
				const FString RootNormalized = IMP_NormalizePinLookupName(RootPin->PinName.ToString());
				if (!PreferredNormalized.StartsWith(RootNormalized) || PreferredNormalized == RootNormalized) continue;
				if (RootPin->SubPins.IsEmpty()) K2Schema->SplitPin(RootPin, false);
				const FString FieldSuffix = PreferredNormalized.Mid(RootNormalized.Len());
				for (UEdGraphPin* SubPin : RootPin->SubPins)
				{
					if (!SubPin) continue;
					const FString SubPinNormalized = IMP_NormalizePinLookupName(SubPin->PinName.ToString());
					if (SubPinNormalized == PreferredNormalized
						|| (!FieldSuffix.IsEmpty() && SubPinNormalized.EndsWith(FieldSuffix))) return SubPin;
				}
			}
		}
	}
	if (Form->IsForm(TEXT("property-access")))
	{
		if (UEdGraphPin* ValuePin = ExistingNode->FindPin(TEXT("Value"), EGPD_Output)) return ValuePin;
	}
	if (UK2Node_Select* SelectNode = Cast<UK2Node_Select>(ExistingNode)) return SelectNode->GetReturnValuePin();
	if (DestinationPin)
	{
		if (UEdGraphPin* CompatiblePin = IMP_SelectBestDataOutputPinForDestination(ExistingNode, DestinationPin, Ctx.Graph)) return CompatiblePin;
	}
	return IMP_FindOutputPin(ExistingNode, TEXT(""));
}

static UEdGraphPin* IMP_TryBuildSelectOutputPin(const FLispNodePtr& Expr, UEdGraphPin* DestinationPin, FBPImportContext& Ctx, bool& bHandled)
{
	bHandled = false;
	if (!Expr.IsValid() || !Expr->IsForm(TEXT("select")))
	{
		return nullptr;
	}
	bHandled = true;

	const FLispNodePtr IndexExpr = Expr->GetKeywordArg(TEXT(":index"));
	TArray<FLispNodePtr> OptionExprs;
	for (int32 Index = 1; Index + 1 < Expr->Num(); ++Index)
	{
		const FLispNodePtr Keyword = Expr->Get(Index);
		if (Keyword.IsValid() && Keyword->IsKeyword() && Keyword->StringValue.Equals(TEXT(":option"), ESearchCase::IgnoreCase))
		{
			OptionExprs.Add(Expr->Get(++Index));
		}
	}
	if (!IndexExpr.IsValid() || IndexExpr->IsNil() || OptionExprs.Num() < 2)
	{
		Ctx.Errors.Add(TEXT("IMP: select requires :index and at least two :option expressions"));
		return nullptr;
	}

	UEdGraphPin* IndexSourcePin = IMP_ResolveLispExpr(IndexExpr, Ctx);
	UK2Node_Select* SelectNode = NewObject<UK2Node_Select>(Ctx.Graph);
	SelectNode->NodePosX = Ctx.CurrentX;
	SelectNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(SelectNode, false, false);
	SelectNode->AllocateDefaultPins();
	IMP_EnsureGuid(SelectNode);

	if (IndexSourcePin)
	{
		UEdGraphPin* IndexPin = SelectNode->GetIndexPin();
		IndexPin->PinType = IndexSourcePin->PinType;
		SelectNode->ChangePinType(IndexPin);
	}

	TArray<UEdGraphPin*> OptionPins;
	SelectNode->GetOptionPins(OptionPins);
	if (SelectNode->GetEnum())
	{
		if (OptionPins.Num() != OptionExprs.Num())
		{
			UEnum* SelectEnum = SelectNode->GetEnum();
			Ctx.Errors.Add(FString::Printf(
				TEXT("IMP: enum select '%s' has %d enum entries, exposes %d options, but DSL contains %d"),
				SelectEnum ? *SelectEnum->GetPathName() : TEXT("<null>"),
				SelectEnum ? SelectEnum->NumEnums() : 0,
				OptionPins.Num(), OptionExprs.Num()));
			return nullptr;
		}
	}
	else
	{
		while (OptionPins.Num() < OptionExprs.Num())
		{
			const int32 PreviousCount = OptionPins.Num();
			SelectNode->AddInputPin();
			SelectNode->GetOptionPins(OptionPins);
			if (OptionPins.Num() <= PreviousCount)
			{
				Ctx.Errors.Add(TEXT("IMP: select option count did not increase during reconstruction"));
				return nullptr;
			}
		}
		while (OptionPins.Num() > OptionExprs.Num() && SelectNode->CanRemoveOptionPinToNode())
		{
			const int32 PreviousCount = OptionPins.Num();
			SelectNode->RemoveOptionPinToNode();
			SelectNode->GetOptionPins(OptionPins);
			if (OptionPins.Num() >= PreviousCount)
			{
				Ctx.Errors.Add(TEXT("IMP: select option count did not decrease during reconstruction"));
				return nullptr;
			}
		}
	}

	FEdGraphPinType ResultPinType = DestinationPin ? DestinationPin->PinType : FEdGraphPinType();
	const FString DeclaredResultType = IMP_GetKeywordAtomValue(Expr, TEXT(":result-type"));
	const FString ResultTypeObjectPath = IMP_GetKeywordAtomValue(Expr, TEXT(":result-type-object"));
	bool bAppliedResultTypeObject = false;
	if (!ResultTypeObjectPath.IsEmpty())
	{
		if (UObject* ResultTypeObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ResultTypeObjectPath))
		{
			ResultPinType.PinSubCategoryObject = ResultTypeObject;
			if (ResultTypeObject->IsA<UEnum>())
			{
				ResultPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			}
			else if (ResultTypeObject->IsA<UScriptStruct>())
			{
				ResultPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			}
			bAppliedResultTypeObject = true;
		}
	}
	if (!bAppliedResultTypeObject && !DeclaredResultType.IsEmpty())
	{
		FEdGraphPinType ParsedResultType;
		if (IMP_BuildPinTypeFromLispType(DeclaredResultType, ParsedResultType, Ctx))
		{
			ResultPinType = ParsedResultType;
		}
	}

	UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin();
	ReturnPin->PinType = ResultPinType;
	SelectNode->ChangePinType(ReturnPin);
	ReturnPin = SelectNode->GetReturnValuePin();
	SelectNode->GetOptionPins(OptionPins);

	UEdGraphPin* IndexPin = SelectNode->GetIndexPin();
	const bool bIndexAssigned = IndexSourcePin
		? IMP_Connect(IndexSourcePin, IndexPin, Ctx)
		: IMP_SetPinFromExpr(IndexPin, IndexExpr, Ctx);
	if (!bIndexAssigned)
	{
		Ctx.Errors.Add(TEXT("IMP: select index could not be reconstructed"));
		return nullptr;
	}

	for (int32 OptionIndex = 0; OptionIndex < OptionExprs.Num() && OptionIndex < OptionPins.Num(); ++OptionIndex)
	{
		if (!IMP_SetPinFromExpr(OptionPins[OptionIndex], OptionExprs[OptionIndex], Ctx))
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: select option %d could not be reconstructed"), OptionIndex));
			return nullptr;
		}
	}

	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), SelectNode);
	IMP_ApplyRequestedStableId(SelectNode, Expr, false);
	IMP_RegisterImportedStableNode(SelectNode, Expr, Ctx);
	Ctx.TempIdToNode.Add(SelectNode->NodeGuid.ToString(), SelectNode);
	Ctx.AdvancePosition();
	return ReturnPin;
}

static UEdGraphPin* IMP_TryBuildBreakStructOutputPin(const FLispNodePtr& Expr, UEdGraphPin* DestinationPin, FBPImportContext& Ctx, bool& bHandled)
{
	bHandled = false;
	if (!Expr.IsValid() || !Expr->IsList() || Expr->Num() == 0)
	{
		return nullptr;
	}

	FString StructTypeName;
	FString PreferredFieldName;
	FLispNodePtr ValueExpr = FLispNode::MakeNil();

	if (Expr->IsForm(TEXT("break-struct")))
	{
		bHandled = true;
		const FLispNodePtr StructNode = Expr->GetKeywordArg(TEXT(":struct"));
		ValueExpr = Expr->GetKeywordArg(TEXT(":value"));
		const FLispNodePtr FieldNode = Expr->GetKeywordArg(TEXT(":field"));
		if (StructNode.IsValid() && (StructNode->IsString() || StructNode->IsSymbol()))
		{
			StructTypeName = StructNode->StringValue;
		}
		if (FieldNode.IsValid() && (FieldNode->IsString() || FieldNode->IsSymbol()))
		{
			PreferredFieldName = FieldNode->StringValue;
		}
	}
	else
	{
		FString LegacyBreakName;
		int32 ValueIndex = INDEX_NONE;
		if (!IMP_ExtractBindingNameAndValueIndex(Expr, 0, LegacyBreakName, ValueIndex)
			|| !LegacyBreakName.StartsWith(TEXT("Break "), ESearchCase::IgnoreCase)
			|| ValueIndex <= 0
			|| ValueIndex >= Expr->Num())
		{
			return nullptr;
		}

		bHandled = true;
		StructTypeName = LegacyBreakName.Mid(6).TrimStartAndEnd();
		ValueExpr = Expr->Get(ValueIndex);
	}

	if (StructTypeName.IsEmpty() || !ValueExpr.IsValid() || ValueExpr->IsNil())
	{
		Ctx.Errors.Add(TEXT("IMP: break-struct expression is missing struct type or source value"));
		return nullptr;
	}

	UScriptStruct* StructType = IMP_FindStructByName(StructTypeName, Ctx);
	if (!StructType)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("IMP: break-struct struct type not found: %s"), *StructTypeName));
		return nullptr;
	}

	if (IMP_GetRequestedNodeStableId(Expr).IsEmpty() && !PreferredFieldName.IsEmpty())
	{
		if (UEdGraphPin* StructValuePin = IMP_ResolveLispExpr(ValueExpr, Ctx))
		{
			if (StructValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
				&& StructValuePin->PinType.PinSubCategoryObject == StructType)
			{
				if (StructValuePin->SubPins.IsEmpty())
				{
					if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Ctx.Graph->GetSchema()))
					{
						K2Schema->SplitPin(StructValuePin, false);
					}
				}
				const FString PreferredNormalized = IMP_NormalizePinLookupName(PreferredFieldName);
				for (UEdGraphPin* SubPin : StructValuePin->SubPins)
				{
					if (!SubPin) continue;
					const FString SubPinNormalized = IMP_NormalizePinLookupName(SubPin->PinName.ToString());
					if (SubPinNormalized == PreferredNormalized || SubPinNormalized.EndsWith(PreferredNormalized))
					{
						return SubPin;
					}
				}
			}
		}
	}
	UEdGraphNode* BreakNode = nullptr;
	if (StructType->HasMetaData(FBlueprintMetadata::MD_NativeBreakFunction))
	{
		const FString NativeBreakPath = StructType->GetMetaData(FBlueprintMetadata::MD_NativeBreakFunction);
		if (UFunction* NativeBreakFunction = FindObject<UFunction>(nullptr, *NativeBreakPath, true))
		{
			UK2Node_CallFunction* BreakCallNode = NewObject<UK2Node_CallFunction>(Ctx.Graph);
			BreakCallNode->SetFromFunction(NativeBreakFunction);
			BreakCallNode->NodePosX = Ctx.CurrentX;
			BreakCallNode->NodePosY = Ctx.CurrentY;
			Ctx.Graph->AddNode(BreakCallNode, false, false);
			BreakCallNode->AllocateDefaultPins();
			IMP_EnsureGuid(BreakCallNode);
			Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), BreakCallNode);
			Ctx.TempIdToNode.Add(BreakCallNode->NodeGuid.ToString(), BreakCallNode);
			Ctx.AdvancePosition();
			BreakNode = BreakCallNode;
		}
	}
	if (!BreakNode)
	{
		BreakNode = IMP_CreateOrReuseBreakStructNode(Expr, StructType, Ctx);
	}

	UEdGraphPin* StructInputPin = nullptr;

	UEdGraphPin* FirstDataInputPin = nullptr;
	for (UEdGraphPin* Candidate : BreakNode->Pins)
	{
		if (!Candidate || Candidate->Direction != EGPD_Input) continue;
		if (Candidate->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		if (Candidate->PinName == UEdGraphSchema_K2::PN_Self) continue;
		if (!FirstDataInputPin) FirstDataInputPin = Candidate;
		if (Candidate->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
			&& Candidate->PinType.PinSubCategoryObject == StructType)
		{
			StructInputPin = Candidate;
			break;
		}
	}
	if (!StructInputPin) StructInputPin = FirstDataInputPin;

	if (!StructInputPin)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("IMP: break-struct input pin missing for %s"), *StructTypeName));
		return nullptr;
	}

	if (!IMP_SetPinFromExpr(StructInputPin, ValueExpr, Ctx))
	{
		return nullptr;
	}

	UEdGraphPin* PreferredSplitOutputPin = nullptr;
	if (!PreferredFieldName.IsEmpty() && !IMP_FindOutputPinByName(BreakNode, PreferredFieldName))
	{
		const FString PreferredNormalized = IMP_NormalizePinLookupName(PreferredFieldName);
		if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Ctx.Graph->GetSchema()))
		{
			const TArray<UEdGraphPin*> RootPins = BreakNode->Pins;
			for (UEdGraphPin* RootPin : RootPins)
			{
				if (!RootPin || RootPin->Direction != EGPD_Output || RootPin->ParentPin
					|| RootPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct) continue;
				const FString RootNormalized = IMP_NormalizePinLookupName(RootPin->PinName.ToString());
				if (!PreferredNormalized.StartsWith(RootNormalized) || PreferredNormalized == RootNormalized) continue;
				if (RootPin->SubPins.IsEmpty()) K2Schema->SplitPin(RootPin, false);
				const FString FieldSuffix = PreferredNormalized.Mid(RootNormalized.Len());
				for (UEdGraphPin* SubPin : RootPin->SubPins)
				{
					if (!SubPin) continue;
					const FString SubPinNormalized = IMP_NormalizePinLookupName(SubPin->PinName.ToString());
					if (SubPinNormalized == PreferredNormalized
						|| (!FieldSuffix.IsEmpty() && SubPinNormalized.EndsWith(FieldSuffix)))
					{
						PreferredSplitOutputPin = SubPin;
						break;
					}
				}
				if (PreferredSplitOutputPin) break;
			}
		}
	}

	UEdGraphPin* SelectedOutputPin = PreferredSplitOutputPin ? PreferredSplitOutputPin : (!PreferredFieldName.IsEmpty()
		? IMP_FindOutputPinByName(BreakNode, PreferredFieldName)
		: nullptr);
	if (!SelectedOutputPin && DestinationPin)
	{
		SelectedOutputPin = IMP_SelectBestDataOutputPinForDestination(BreakNode, DestinationPin, Ctx.Graph, PreferredFieldName);
	}
	if (!SelectedOutputPin)
	{
		const FString OwnerNodeTitle = DestinationPin && DestinationPin->GetOwningNode()
			? DestinationPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString()
			: TEXT("<no destination>");
		const FString DestinationPinName = DestinationPin ? DestinationPin->PinName.ToString() : TEXT("<none>");
		Ctx.Errors.Add(FString::Printf(
			TEXT("IMP: break-struct '%s' has no output compatible with pin %s.%s"),
			*StructTypeName,
			*OwnerNodeTitle,
			*DestinationPinName));
		return nullptr;
	}

	IMP_RegisterBoundValue(SelectedOutputPin->PinName.ToString(), SelectedOutputPin, Ctx);
	IMP_ApplyRequestedStableId(BreakNode, Expr, false);
	IMP_RegisterImportedStableNode(BreakNode, Expr, Ctx);
	Ctx.TempIdToNode.FindOrAdd(BreakNode->NodeGuid.ToString()) = BreakNode;
	return SelectedOutputPin;
}

// Set a pin's default value from a Lisp expression (number, string, bool, or connected expr)
static bool IMP_SetPinFromExpr(UEdGraphPin* Pin, const FLispNodePtr& Expr, FBPImportContext& Ctx)


{
	if (!Pin || !Expr.IsValid()) return false;
	if (UEdGraphPin* StableOutputPin = IMP_FindImportedStableOutputPin(Expr, Pin, Ctx))
	{
		return IMP_Connect(StableOutputPin, Pin, Ctx);
	}
	if (Expr->IsNil())
	{
		if (const UEdGraphSchema* Schema = Ctx.Graph ? Ctx.Graph->GetSchema() : nullptr)
		{
			Schema->ResetPinToAutogeneratedDefaultValue(Pin, false);
		}
		else
		{
			Pin->DefaultValue.Empty();
			Pin->DefaultObject = nullptr;
		}
		return true;
	}

	if (Expr->IsForm(TEXT("call-macro")) && Expr->Num() >= 2)
	{
		UEdGraphPin* PreferredOutputPin = nullptr;
		if (UK2Node_MacroInstance* MacroNode = IMP_CreateMacroInstanceNode(Expr, Ctx, PreferredOutputPin))
		{
			UEdGraphPin* SelectedOutputPin = PreferredOutputPin ? PreferredOutputPin : IMP_FindOutputPin(MacroNode, TEXT(""));
			const bool bHasExplicitOut = Expr->HasKeyword(TEXT(":out"));
			if (!bHasExplicitOut)
			{
				if (UEdGraphPin* CompatibleOutputPin = IMP_SelectBestMacroOutputPinForDestination(MacroNode, Pin, Ctx.Graph))
				{
					SelectedOutputPin = CompatibleOutputPin;
				}
				else if (IMP_CountVisibleDataOutputPins(MacroNode) > 1
					&& (!SelectedOutputPin || !IMP_CanPinsConnectWithoutMutation(Ctx.Graph, SelectedOutputPin, Pin)))
				{
					int32 MacroArgStartIndex = 2;
					const FString MacroName = IMP_ExtractCallMacroName(Expr, MacroArgStartIndex);
					const FString OwnerNodeTitle = Pin->GetOwningNode() ? Pin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("<null node>");
					Ctx.Errors.Add(FString::Printf(
						TEXT("IMP_SetPinFromExpr: call-macro '%s' has no unique output compatible with pin %s.%s; add :out to disambiguate"),
						*MacroName,
						*OwnerNodeTitle,
						*Pin->PinName.ToString()));
					return false;
				}
			}

			if (SelectedOutputPin)
			{
				return IMP_Connect(SelectedOutputPin, Pin, Ctx);
			}
		}
	}

	bool bHandledSelectExpr = false;
	if (UEdGraphPin* SelectOutputPin = IMP_TryBuildSelectOutputPin(Expr, Pin, Ctx, bHandledSelectExpr))
	{
		return IMP_Connect(SelectOutputPin, Pin, Ctx);
	}
	if (bHandledSelectExpr)
	{
		return false;
	}

	bool bHandledBreakStructExpr = false;
	if (UEdGraphPin* BreakStructOutputPin = IMP_TryBuildBreakStructOutputPin(Expr, Pin, Ctx, bHandledBreakStructExpr))
	{
		return IMP_Connect(BreakStructOutputPin, Pin, Ctx);
	}
	if (bHandledBreakStructExpr)
	{
		return false;
	}

	UEdGraphPin* Src = IMP_ResolveLispExpr(Expr, Ctx);
	if (Src)
	{
		if (UK2Node_MakeArray* MakeArrayNode = Cast<UK2Node_MakeArray>(Src->GetOwningNode());
			MakeArrayNode && Pin->PinType.ContainerType == EPinContainerType::Array)
		{
			Src->PinType = Pin->PinType;
			FEdGraphPinType ElementPinType = Pin->PinType;
			ElementPinType.ContainerType = EPinContainerType::None;
			for (UEdGraphPin* ArrayPin : MakeArrayNode->Pins)
			{
				if (ArrayPin && ArrayPin->Direction == EGPD_Input)
				{
					ArrayPin->PinType = ElementPinType;
				}
			}
		}
		return IMP_Connect(Src, Pin, Ctx);
	}

	if (!Ctx.LastAssetPath.IsEmpty())
	{
		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Ctx.LastAssetPath);
		Ctx.LastAssetPath.Empty();
		if (Asset) { Pin->DefaultObject = Asset; return true; }
	}


	if (Expr->IsNumber())
	{
		const FName PinCategory = Pin->PinType.PinCategory;
		if (PinCategory == UEdGraphSchema_K2::PC_Int || PinCategory == UEdGraphSchema_K2::PC_Int64 || PinCategory == UEdGraphSchema_K2::PC_Byte)
		{
			Pin->DefaultValue = LexToString(static_cast<int64>(Expr->NumberValue));
		}
		else
		{
			Pin->DefaultValue = FString::SanitizeFloat(Expr->NumberValue);
		}
		return true;
	}

	if (Expr->IsString())       { Pin->DefaultValue = Expr->StringValue; return true; }
	if (Expr->IsSymbol())
	{
		FString S = Expr->StringValue;
		if (S.Equals(TEXT("true"),  ESearchCase::IgnoreCase)) { Pin->DefaultValue = TEXT("true");  return true; }
		if (S.Equals(TEXT("false"), ESearchCase::IgnoreCase)) { Pin->DefaultValue = TEXT("false"); return true; }
		if (S.Equals(TEXT("nil"),   ESearchCase::IgnoreCase)) { Pin->DefaultValue = TEXT("");      return true; }

		const FName PinCategory = Pin->PinType.PinCategory;
		if (PinCategory == UEdGraphSchema_K2::PC_String || PinCategory == UEdGraphSchema_K2::PC_Name || PinCategory == UEdGraphSchema_K2::PC_Text)
		{
			Pin->DefaultValue = S;
			return true;
		}

		const FString NodeTitle = Pin->GetOwningNode() ? Pin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("<null node>");
		Ctx.Errors.Add(FString::Printf(TEXT("IMP_SetPinFromExpr: unresolved symbol '%s' for pin %s.%s"), *S, *NodeTitle, *Pin->PinName.ToString()));
		return false;
	}

	const FString UnsupportedNodeTitle = Pin->GetOwningNode() ? Pin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("<null node>");
	Ctx.Errors.Add(FString::Printf(TEXT("IMP_SetPinFromExpr: unsupported expression for pin %s.%s"),
		*UnsupportedNodeTitle,
		*Pin->PinName.ToString()));

	return false;

}

// Find a UFunction by name using deterministic class search + cache.
static UFunction* IMP_FindFunction(const FString& FuncName, FBPImportContext& Ctx)
{
	if (UFunction** Cached = Ctx.FunctionCache.Find(FuncName))
	{
		return *Cached;
	}

	TArray<UClass*> ToSearch;
	auto AddUniqueClass = [&ToSearch](UClass* InClass)
	{
		if (InClass && !ToSearch.Contains(InClass))
		{
			ToSearch.Add(InClass);
		}
	};

	if (Ctx.Blueprint)
	{
		AddUniqueClass(Ctx.Blueprint->GeneratedClass);
		AddUniqueClass(Ctx.Blueprint->ParentClass);
	}
	AddUniqueClass(UKismetSystemLibrary::StaticClass());
	AddUniqueClass(UGameplayStatics::StaticClass());
	AddUniqueClass(UKismetMathLibrary::StaticClass());
	AddUniqueClass(UKismetStringLibrary::StaticClass());
	AddUniqueClass(AActor::StaticClass());
	AddUniqueClass(APawn::StaticClass());
	AddUniqueClass(ACharacter::StaticClass());

	TArray<FString> Names = { FuncName, TEXT("K2_") + FuncName };
	if (FuncName.StartsWith(TEXT("K2_")))
	{
		Names.Add(FuncName.Mid(3));
	}

	for (const FString& N : Names)
	{
		for (UClass* C : ToSearch)
		{
			if (!C) continue;
			if (UFunction* F = C->FindFunctionByName(*N))
			{
				if (F->HasAnyFunctionFlags(FUNC_BlueprintCallable) || F->HasAnyFunctionFlags(FUNC_BlueprintPure))
				{
					Ctx.FunctionCache.Add(FuncName, F);
					return F;
				}
			}
		}
	}


	for (const FString& N : Names)
	{
		for (TObjectIterator<UFunction> It; It; ++It)
		{
			if (It->GetName() == N && (It->HasAnyFunctionFlags(FUNC_BlueprintCallable) || It->HasAnyFunctionFlags(FUNC_BlueprintPure)))
			{
				Ctx.FunctionCache.Add(FuncName, *It);
				return *It;
			}
		}
	}

	Ctx.FunctionCache.Add(FuncName, nullptr);
	return nullptr;
}

static UFunction* IMP_FindFunctionForForm(const FString& FuncName, const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	const FString OwnerPath = IMP_GetKeywordAtomValue(Form, TEXT(":owner"));
	if (!OwnerPath.IsEmpty())
	{
		if (OwnerPath.Equals(TEXT("self"), ESearchCase::IgnoreCase))
		{
			TArray<FString> Names = { FuncName, TEXT("K2_") + FuncName };
			if (FuncName.StartsWith(TEXT("K2_"))) Names.Add(FuncName.Mid(3));
			if (Ctx.Blueprint)
			{
				for (UClass* LocalClass : { Ctx.Blueprint->SkeletonGeneratedClass, Ctx.Blueprint->GeneratedClass })
				{
					if (!LocalClass) continue;
					for (const FString& CandidateName : Names)
					{
						if (UFunction* LocalFunction = LocalClass->FindFunctionByName(*CandidateName))
						{
							return LocalFunction;
						}
					}
				}
			}
			return nullptr;
		}
		if (UClass* OwnerClass = LoadObject<UClass>(nullptr, *OwnerPath))
		{
			TArray<FString> Names = { FuncName, TEXT("K2_") + FuncName };
			if (FuncName.StartsWith(TEXT("K2_"))) Names.Add(FuncName.Mid(3));
			if (Ctx.Blueprint && Ctx.Blueprint->SkeletonGeneratedClass && Ctx.Blueprint->ParentClass
				&& Ctx.Blueprint->ParentClass->IsChildOf(UAnimInstance::StaticClass())
				&& OwnerClass->IsChildOf(Ctx.Blueprint->ParentClass))
			{
				for (const FString& CandidateName : Names)
				{
					if (UFunction* LocalFunction = Ctx.Blueprint->SkeletonGeneratedClass->FindFunctionByName(*CandidateName))
					{
						return LocalFunction;
					}
				}
			}
			for (const FString& CandidateName : Names)
			{
				if (UFunction* Function = OwnerClass->FindFunctionByName(*CandidateName))
				{
					if (Function->HasAnyFunctionFlags(FUNC_BlueprintCallable) || Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
					{
						return Function;
					}
				}
			}
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: function '%s' not found on declared owner '%s'"), *FuncName, *OwnerPath));
			return nullptr;
		}
		Ctx.Errors.Add(FString::Printf(TEXT("IMP: declared function owner could not be loaded: %s"), *OwnerPath));
		return nullptr;
	}
	return IMP_FindFunction(FuncName, Ctx);
}

static UFunction* IMP_FindParentFunction(const FString& FuncName, FBPImportContext& Ctx)
{
	if (!Ctx.Blueprint || !Ctx.Blueprint->ParentClass)
	{
		return nullptr;
	}

	TArray<FString> Names = { FuncName, TEXT("K2_") + FuncName };
	if (FuncName.StartsWith(TEXT("K2_")))
	{
		Names.Add(FuncName.Mid(3));
	}

	for (UClass* ClassCursor = Ctx.Blueprint->ParentClass; ClassCursor; ClassCursor = ClassCursor->GetSuperClass())
	{
		for (const FString& CandidateName : Names)
		{
			if (UFunction* Func = ClassCursor->FindFunctionByName(*CandidateName))
			{
				return Func;
			}
		}
	}

	return nullptr;
}

static UFunction* IMP_FindImplementedInterfaceFunction(const FString& FuncName, FBPImportContext& Ctx)
{
	if (!Ctx.Blueprint)
	{
		return nullptr;
	}

	TArray<FString> Names = { FuncName, TEXT("K2_") + FuncName };
	if (FuncName.StartsWith(TEXT("K2_")))
	{
		Names.Add(FuncName.Mid(3));
	}

	for (const FBPInterfaceDescription& Desc : Ctx.Blueprint->ImplementedInterfaces)
	{
		UClass* InterfaceClass = Desc.Interface;
		if (!InterfaceClass)
		{
			continue;
		}

		for (const FString& CandidateName : Names)
		{
			if (UFunction* Func = InterfaceClass->FindFunctionByName(*CandidateName))
			{
				return Func;
			}
		}

	}

	return nullptr;
}


static UEdGraph* IMP_FindMacroGraphByName(const FString& MacroName, FBPImportContext& Ctx)
{

	auto FindInBlueprint = [&MacroName](UBlueprint* Blueprint) -> UEdGraph*
	{
		if (!Blueprint) return nullptr;
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph && Graph->GetName().Equals(MacroName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
		return nullptr;
	};

	if (UEdGraph* Graph = FindInBlueprint(Ctx.Blueprint))
	{
		return Graph;
	}

	for (TObjectIterator<UBlueprint> It; It; ++It)
	{
		if (UEdGraph* Graph = FindInBlueprint(*It))
		{
			return Graph;
		}
	}

	static const TCHAR* KnownMacroBlueprintPaths[] = {
		TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"),
		TEXT("/Engine/EditorBlueprintResources/ActorMacros.ActorMacros"),
		TEXT("/Engine/EditorBlueprintResources/ActorComponentMacros.ActorComponentMacros")
	};

	for (const TCHAR* BlueprintPath : KnownMacroBlueprintPaths)
	{
		if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, BlueprintPath))
		{
			if (UEdGraph* Graph = FindInBlueprint(Blueprint))
			{
				return Graph;
			}
		}
	}

	return nullptr;
}

static UK2Node_Tunnel* IMP_FindMacroExitTunnel(UEdGraph* Graph, const FString& ExitName)
{
	if (!Graph) return nullptr;

	UK2Node_Tunnel* FirstExit = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
		if (!Tunnel || Tunnel->DrawNodeAsEntry() || Cast<UK2Node_MacroInstance>(Node))
		{
			continue;
		}

		if (!FirstExit)
		{
			FirstExit = Tunnel;
		}

		const FString TunnelName = Tunnel->GetNodeTitle(ENodeTitleType::ListView).ToString();
		if (ExitName.IsEmpty() || TunnelName.Equals(ExitName, ESearchCase::IgnoreCase))
		{
			return Tunnel;
		}
	}

	return ExitName.IsEmpty() ? FirstExit : nullptr;
}

static bool IMP_IsCompatibleExistingMacroExitTunnel(UK2Node_Tunnel* Tunnel, const FString& ExitName)
{
	if (!Tunnel || Tunnel->DrawNodeAsEntry() || Cast<UK2Node_MacroInstance>(Tunnel))
	{
		return false;
	}

	const FString TunnelName = Tunnel->GetNodeTitle(ENodeTitleType::ListView).ToString();
	return ExitName.IsEmpty() || TunnelName.Equals(ExitName, ESearchCase::IgnoreCase);
}

static UK2Node_Tunnel* IMP_FindReusableMacroExitTunnel(const FLispNodePtr& Form, UEdGraph* Graph, const FString& ExitName)
{
	if (!Graph)
	{
		return nullptr;
	}

	const FString RequestedId = IMP_GetRequestedNodeStableId(Form);
	if (!RequestedId.IsEmpty())
	{
		TMap<FString, UEdGraphNode*> StableIdToNode;
		IMP_BuildStableIdIndex(Graph, false, StableIdToNode, nullptr, nullptr);
		if (UEdGraphNode* const* FoundNode = StableIdToNode.Find(RequestedId))
		{
			if (UK2Node_Tunnel* ReusableTunnel = Cast<UK2Node_Tunnel>(*FoundNode))
			{
				if (IMP_IsCompatibleExistingMacroExitTunnel(ReusableTunnel, ExitName))
				{
					return ReusableTunnel;
				}
			}
		}
	}

	return IMP_FindMacroExitTunnel(Graph, ExitName);
}

static UK2Node_FunctionResult* IMP_FindFunctionResult(const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (!Ctx.Graph)
	{
		return nullptr;
	}

	const FString RequestedId = IMP_GetRequestedNodeStableId(Form);
	if (!RequestedId.IsEmpty())
	{
		TMap<FString, UEdGraphNode*> StableIdToNode;
		IMP_BuildStableIdIndex(Ctx.Graph, false, StableIdToNode, nullptr, nullptr);
		if (UEdGraphNode* const* FoundNode = StableIdToNode.Find(RequestedId))
		{
			if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(*FoundNode))
			{
				if (!Ctx.ConsumedFunctionResultGuids.Contains(ResultNode->NodeGuid))
				{
					return ResultNode;
				}
			}
		}
	}

	UK2Node_FunctionResult* TemplateResult = nullptr;
	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
		{
			if (!TemplateResult)
			{
				TemplateResult = ResultNode;
			}
			if (!Ctx.ConsumedFunctionResultGuids.Contains(ResultNode->NodeGuid))
			{
				return ResultNode;
			}
		}
	}

	if (!TemplateResult)
	{
		return nullptr;
	}

	UK2Node_FunctionResult* NewResult = NewObject<UK2Node_FunctionResult>(Ctx.Graph);
	NewResult->FunctionReference = TemplateResult->FunctionReference;
	NewResult->NodePosX = Ctx.CurrentX;
	NewResult->NodePosY = Ctx.CurrentY;
	NewResult->CreateNewGuid();
	NewResult->PostPlacedNewNode();
	NewResult->AllocateDefaultPins();
	Ctx.Graph->AddNode(NewResult, false, false);

	for (UEdGraphPin* TemplatePin : TemplateResult->Pins)
	{
		if (!TemplatePin || TemplatePin->Direction != EGPD_Input
			|| TemplatePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			|| TemplatePin->bHidden || TemplatePin->PinName == UEdGraphSchema_K2::PN_Execute)
		{
			continue;
		}
		UEdGraphPin* NewPin = IMP_FindInputPin(NewResult, TemplatePin->PinName.ToString());
		if (!NewPin)
		{
			NewPin = NewResult->CreateUserDefinedPin(TemplatePin->PinName, TemplatePin->PinType, EGPD_Input, false);
		}
		if (NewPin)
		{
			NewPin->DefaultValue = TemplatePin->DefaultValue;
			NewPin->AutogeneratedDefaultValue = TemplatePin->AutogeneratedDefaultValue;
		}
	}
	Ctx.AdvancePosition();
	return NewResult;
}


static FString IMP_ExtractCompoundName(const FLispNodePtr& Form, int32 StartIndex, int32& OutArgStartIndex)
{
	OutArgStartIndex = StartIndex + 1;
	if (!Form.IsValid() || Form->Num() <= StartIndex)
	{
		return TEXT("");
	}

	TArray<FString> NameParts;
	for (int32 i = StartIndex; i < Form->Num(); ++i)
	{
		const FLispNodePtr Part = Form->Get(i);
		if (!Part.IsValid())
		{
			continue;
		}
		if (Part->IsKeyword() || Part->IsList())
		{
			OutArgStartIndex = i;
			break;
		}

		NameParts.Add(Part->StringValue);
		OutArgStartIndex = i + 1;
		if (Part->IsString())
		{
			break;
		}
	}

	return FString::Join(NameParts, TEXT(" "));
}

static bool IMP_ExtractBindingNameAndValueIndex(const FLispNodePtr& Form, int32 StartIndex, FString& OutName, int32& OutValueIndex)
{
	OutName.Reset();
	OutValueIndex = INDEX_NONE;
	if (!Form.IsValid() || Form->Num() <= StartIndex + 1)
	{
		return false;
	}

	const FLispNodePtr FirstPart = Form->Get(StartIndex);
	if (!FirstPart.IsValid())
	{
		return false;
	}
	if (FirstPart->IsString())
	{
		OutName = FirstPart->StringValue;
		OutValueIndex = StartIndex + 1;
		return OutValueIndex < Form->Num();
	}

	TArray<FString> PositionalParts;
	TArray<int32> PositionalIndices;
	int32 BoundaryIndex = Form->Num();
	for (int32 i = StartIndex; i < Form->Num(); ++i)
	{
		const FLispNodePtr Part = Form->Get(i);
		if (!Part.IsValid())
		{
			continue;
		}
		if (Part->IsKeyword())
		{
			BoundaryIndex = i;
			break;
		}
		if (Part->IsList())
		{
			BoundaryIndex = i;
			break;
		}

		PositionalParts.Add(Part->StringValue);
		PositionalIndices.Add(i);
	}

	if (BoundaryIndex < Form->Num() && Form->Get(BoundaryIndex).IsValid() && Form->Get(BoundaryIndex)->IsList())
	{
		if (PositionalParts.Num() <= 0)
		{
			return false;
		}
		OutName = FString::Join(PositionalParts, TEXT(" "));
		OutValueIndex = BoundaryIndex;
		return true;
	}

	if (PositionalParts.Num() < 2)
	{
		return false;
	}

	TArray<FString> NameParts;
	for (int32 i = 0; i < PositionalParts.Num() - 1; ++i)
	{
		NameParts.Add(PositionalParts[i]);
	}
	OutName = FString::Join(NameParts, TEXT(" "));
	OutValueIndex = PositionalIndices.Last();
	return !OutName.IsEmpty() && OutValueIndex >= 0 && OutValueIndex < Form->Num();
}

static FString IMP_ExtractCallMacroName(const FLispNodePtr& Form, int32& OutArgStartIndex)
{
	return IMP_ExtractCompoundName(Form, 1, OutArgStartIndex);
}



static UEdGraphPin* IMP_ConfigureMacroInstanceNode(UK2Node_MacroInstance* MacroNode, const FLispNodePtr& Form, FBPImportContext& Ctx, int32 ArgStartIndex)
{
	if (!MacroNode || !Form.IsValid()) return nullptr;

	const FString NodeGuid = MacroNode->NodeGuid.ToString();
	UEdGraphPin* PreferredOutputPin = nullptr;
	TArray<FLispNodePtr> DeferredOutDeclarations;

	auto ProcessOutDeclaration = [&Ctx, MacroNode, &PreferredOutputPin, &NodeGuid](const FLispNodePtr& Value, bool bEmitWarning)
	{
		if (!Value.IsValid() || !Value->IsList() || Value->Num() < 1)
		{
			return;
		}

		FString OutName;
		FString OutType;
		if (Value->Num() >= 2)
		{
			TArray<FString> OutNameParts;
			for (int32 PartIndex = 0; PartIndex < Value->Num() - 1; ++PartIndex)
			{
				if (Value->Get(PartIndex).IsValid())
				{
					OutNameParts.Add(Value->Get(PartIndex)->StringValue);
				}
			}
			OutName = FString::Join(OutNameParts, TEXT(" "));
			if (Value->Get(Value->Num() - 1).IsValid())
			{
				OutType = Value->Get(Value->Num() - 1)->StringValue;
			}
		}
		else if (Value->Get(0).IsValid())
		{
			OutName = Value->Get(0)->StringValue;
		}

		if (UEdGraphPin* OutPin = IMP_FindOutputPinByName(MacroNode, OutName))
		{
			if (!OutType.IsEmpty() && (OutPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard || OutPin->PinType.PinCategory.IsNone()))
			{
				IMP_ApplyLispTypeToPin(OutPin, OutType);
			}


			if (!PreferredOutputPin)
			{
				PreferredOutputPin = OutPin;
			}

			Ctx.VariableToNodeId.Add(OutName, NodeGuid);
			Ctx.VariableToPin.Add(OutName, OutPin->PinName.ToString());
			const FString NoSpaces = OutName.Replace(TEXT(" "), TEXT(""));
			if (NoSpaces != OutName)
			{
				Ctx.VariableToNodeId.Add(NoSpaces, NodeGuid);
				Ctx.VariableToPin.Add(NoSpaces, OutPin->PinName.ToString());
			}
		}
		else if (bEmitWarning)
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: macro output pin not found: %s.%s"), *MacroNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *OutName));

		}
	};

	for (int32 i = ArgStartIndex; i < Form->Num(); ++i)
	{
		if (!Form->Get(i)->IsKeyword())
		{
			continue;
		}
		const FString Keyword = Form->Get(i)->StringValue;
		const FLispNodePtr Value = (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil();
		const FString KeywordName = Keyword.StartsWith(TEXT(":")) ? Keyword.Mid(1) : Keyword;
		if (KeywordName.Equals(TEXT("out"), ESearchCase::IgnoreCase))
		{
			DeferredOutDeclarations.Add(Value);
			ProcessOutDeclaration(Value, false);
			i += 1;
		}
	}

	for (int32 i = ArgStartIndex; i < Form->Num(); ++i)
	{
		if (!Form->Get(i)->IsKeyword())
		{
			continue;
		}

		const FString Keyword = Form->Get(i)->StringValue;
		const FLispNodePtr Value = (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil();
		const FString KeywordName = Keyword.StartsWith(TEXT(":")) ? Keyword.Mid(1) : Keyword;
		if (KeywordName.Equals(TEXT("out"), ESearchCase::IgnoreCase))
		{
			i += 1;
			continue;
		}
		if (IMP_ShouldIgnoreCallKeyword(KeywordName) || KeywordName.Equals(TEXT("event-id"), ESearchCase::IgnoreCase))
		{
			i += 1;
			continue;
		}

		if (UEdGraphPin* InputPin = IMP_FindInputPin(MacroNode, KeywordName))
		{
			IMP_SetPinFromExpr(InputPin, Value, Ctx);
		}
		else
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: macro input pin not found: %s.%s"), *MacroNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *KeywordName));

		}
		i += 1;
	}

	for (const FLispNodePtr& OutDecl : DeferredOutDeclarations)
	{
		ProcessOutDeclaration(OutDecl, true);
	}

	if (!PreferredOutputPin)
	{
		PreferredOutputPin = IMP_FindOutputPin(MacroNode, TEXT(""));
	}

	return PreferredOutputPin;
}



static UK2Node_MacroInstance* IMP_CreateMacroInstanceNode(const FLispNodePtr& Form, FBPImportContext& Ctx, UEdGraphPin*& OutPreferredOutputPin)
{
	OutPreferredOutputPin = nullptr;
	int32 ArgStartIndex = 2;
	const FString MacroName = IMP_ExtractCallMacroName(Form, ArgStartIndex);
	if (MacroName.IsEmpty())
	{
		Ctx.Errors.Add(TEXT("IMP: call-macro missing macro name"));
		return nullptr;
	}

	UEdGraph* MacroGraph = IMP_FindMacroGraphByName(MacroName, Ctx);
	if (!MacroGraph)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("IMP: macro graph not found: %s"), *MacroName));

		return nullptr;
	}

	UK2Node_MacroInstance* MacroNode = IMP_FindReusableMacroInstanceNode(Form, MacroName, Ctx);
	if (!MacroNode)
	{
		MacroNode = NewObject<UK2Node_MacroInstance>(Ctx.Graph);
		MacroNode->SetMacroGraph(MacroGraph);
		MacroNode->NodePosX = Ctx.CurrentX;
		MacroNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(MacroNode, false, false);
		MacroNode->AllocateDefaultPins();
		IMP_EnsureGuid(MacroNode);
		Ctx.AdvancePosition();
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), MacroNode);
	}
	else
	{
		MacroNode->SetMacroGraph(MacroGraph);
	}
	Ctx.TempIdToNode.Add(MacroNode->NodeGuid.ToString(), MacroNode);

	OutPreferredOutputPin = IMP_ConfigureMacroInstanceNode(MacroNode, Form, Ctx, ArgStartIndex);


	return MacroNode;
}


static bool IMP_IsTruthy(const FLispNodePtr& Value)
{
	if (!Value.IsValid() || Value->IsNil()) return false;
	if (Value->IsNumber()) return Value->NumberValue != 0.0;
	if (Value->IsString())
	{
		return Value->StringValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Value->StringValue.Equals(TEXT("1"), ESearchCase::IgnoreCase);
	}
	if (Value->IsSymbol())
	{
		return Value->StringValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Value->StringValue.Equals(TEXT("1"), ESearchCase::IgnoreCase)
			|| Value->StringValue.Equals(TEXT("yes"), ESearchCase::IgnoreCase);
	}
	return false;
}

static UClass* IMP_FindClassByName(const FString& TypeName, FBPImportContext& Ctx)
{
	if (TypeName.IsEmpty()) return nullptr;

	auto MatchesClass = [&TypeName](UClass* InClass) -> bool
	{
		if (!InClass) return false;
		if (InClass->GetName().Equals(TypeName, ESearchCase::IgnoreCase)) return true;
		if (UBlueprint* BP = UBlueprint::GetBlueprintFromClass(InClass))
		{
			if (BP->GetName().Equals(TypeName, ESearchCase::IgnoreCase)) return true;
		}
		return false;
	};

	if (TypeName.Contains(TEXT("/")) || TypeName.Contains(TEXT(".")))
	{
		if (UClass* LoadedClass = LoadClass<UObject>(nullptr, *TypeName))
		{
			return LoadedClass;
		}
	}

	if (MatchesClass(Ctx.Blueprint ? Ctx.Blueprint->GeneratedClass : nullptr))
	{
		return Ctx.Blueprint->GeneratedClass;
	}
	if (MatchesClass(Ctx.Blueprint ? Ctx.Blueprint->ParentClass : nullptr))
	{
		return Ctx.Blueprint->ParentClass;
	}

	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (MatchesClass(*It))
		{
			return *It;
		}
	}

	return nullptr;
}

static UScriptStruct* IMP_FindStructByName(const FString& StructName, FBPImportContext& Ctx)
{
	if (StructName.IsEmpty()) return nullptr;

	auto MatchesStruct = [&StructName](UScriptStruct* InStruct) -> bool
	{
		if (!InStruct) return false;
		return InStruct->GetName().Equals(StructName, ESearchCase::IgnoreCase)
			|| InStruct->GetStructCPPName().Equals(StructName, ESearchCase::IgnoreCase);
	};

	if (StructName.Contains(TEXT("/")) || StructName.Contains(TEXT(".")))
	{
		if (UScriptStruct* LoadedStruct = LoadObject<UScriptStruct>(nullptr, *StructName))
		{
			return LoadedStruct;
		}
	}




	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (MatchesStruct(*It))
		{
			return *It;
		}
	}

	return nullptr;
}

static UEnum* IMP_FindEnumByName(const FString& EnumName)

{
	if (EnumName.IsEmpty()) return nullptr;

	if (EnumName.Contains(TEXT("/")) || EnumName.Contains(TEXT(".")))
	{
		if (UEnum* LoadedEnum = LoadObject<UEnum>(nullptr, *EnumName))
		{
			return LoadedEnum;
		}
	}

	for (TObjectIterator<UEnum> It; It; ++It)
	{
		if (*It && It->GetName().Equals(EnumName, ESearchCase::IgnoreCase))
		{
			return *It;
		}
	}

	return nullptr;
}

static FString IMP_GetAtomName(const FLispNodePtr& Node)
{
	if (!Node.IsValid() || Node->IsNil()) return TEXT("");
	if (Node->IsString() || Node->IsSymbol() || Node->IsKeyword()) return Node->StringValue;
	if (Node->IsNumber()) return LexToString(Node->NumberValue);
	return TEXT("");
}

static bool IMP_TryExtractNamedTypedPair(const FLispNodePtr& PairNode, FString& OutName, FString& OutType)
{
	OutName.Reset();
	OutType.Reset();
	if (!PairNode.IsValid() || !PairNode->IsList() || PairNode->Num() < 2)
	{
		return false;
	}

	OutName = IMP_GetAtomName(PairNode->Get(0));
	OutType = IMP_GetAtomName(PairNode->Get(1));
	return !OutName.IsEmpty() && !OutType.IsEmpty();
}

static void IMP_ApplyNamedTypedPairQualifiers(const FLispNodePtr& PairNode, FEdGraphPinType& PinType)
{
	if (!PairNode.IsValid() || !PairNode->IsList()) return;
	PinType.bIsReference = PairNode->HasKeyword(TEXT(":ref"))
		&& IMP_IsTruthy(PairNode->GetKeywordArg(TEXT(":ref")));
	PinType.bIsConst = PairNode->HasKeyword(TEXT(":const"))
		&& IMP_IsTruthy(PairNode->GetKeywordArg(TEXT(":const")));
}

static bool IMP_BuildPinTypeFromLispType(const FString& TypeName, FEdGraphPinType& OutPinType, FBPImportContext& Ctx);

static bool IMP_LooksLikeContainerTypeGrammar(const FString& TypeName)
{
	const FString Normalized = TypeName.TrimStartAndEnd();
	int32 GenericStartIndex = INDEX_NONE;
	if (!Normalized.FindChar('<', GenericStartIndex) || GenericStartIndex <= 0)
	{
		return false;
	}

	const FString Prefix = Normalized.Left(GenericStartIndex).TrimStartAndEnd().ToLower();
	return Prefix == TEXT("array") || Prefix == TEXT("set") || Prefix == TEXT("map");
}

static bool IMP_TrySplitTopLevelTypeArguments(const FString& Source, TArray<FString>& OutArgs)
{
	OutArgs.Reset();

	int32 Depth = 0;
	FString Current;
	for (const TCHAR Char : Source)
	{
		if (Char == '<')
		{
			Depth += 1;
			Current.AppendChar(Char);
			continue;
		}
		if (Char == '>')
		{
			if (Depth <= 0)
			{
				return false;
			}
			Depth -= 1;
			Current.AppendChar(Char);
			continue;
		}
		if (Char == ',' && Depth == 0)
		{
			const FString TrimmedArg = Current.TrimStartAndEnd();
			if (TrimmedArg.IsEmpty())
			{
				return false;
			}
			OutArgs.Add(TrimmedArg);
			Current.Reset();
			continue;
		}
		Current.AppendChar(Char);
	}

	if (Depth != 0)
	{
		return false;
	}

	const FString TrimmedArg = Current.TrimStartAndEnd();
	if (TrimmedArg.IsEmpty())
	{
		return false;
	}
	OutArgs.Add(TrimmedArg);
	return true;
}

static bool IMP_TryParseContainerTypeGrammar(const FString& TypeName, FString& OutContainerKind, TArray<FString>& OutTypeArgs)
{
	OutContainerKind.Reset();
	OutTypeArgs.Reset();

	const FString Normalized = TypeName.TrimStartAndEnd();
	int32 GenericStartIndex = INDEX_NONE;
	if (!Normalized.FindChar('<', GenericStartIndex) || GenericStartIndex <= 0 || !Normalized.EndsWith(TEXT(">")))
	{
		return false;
	}

	OutContainerKind = Normalized.Left(GenericStartIndex).TrimStartAndEnd().ToLower();
	if (OutContainerKind != TEXT("array") && OutContainerKind != TEXT("set") && OutContainerKind != TEXT("map"))
	{
		OutContainerKind.Reset();
		return false;
	}

	const FString InnerArgs = Normalized.Mid(GenericStartIndex + 1, Normalized.Len() - GenericStartIndex - 2).TrimStartAndEnd();
	if (InnerArgs.IsEmpty() || !IMP_TrySplitTopLevelTypeArguments(InnerArgs, OutTypeArgs))
	{
		OutContainerKind.Reset();
		OutTypeArgs.Reset();
		return false;
	}

	const int32 ExpectedArgCount = OutContainerKind == TEXT("map") ? 2 : 1;
	if (OutTypeArgs.Num() != ExpectedArgCount)
	{
		OutContainerKind.Reset();
		OutTypeArgs.Reset();
		return false;
	}

	return true;
}

static bool IMP_TryParseWrappedReferenceTypeGrammar(const FString& TypeName, FString& OutWrapperKind, FString& OutInnerType)
{
	OutWrapperKind.Reset();
	OutInnerType.Reset();

	const FString Normalized = TypeName.TrimStartAndEnd();
	int32 GenericStartIndex = INDEX_NONE;
	if (!Normalized.FindChar('<', GenericStartIndex) || GenericStartIndex <= 0 || !Normalized.EndsWith(TEXT(">")))
	{
		return false;
	}

	OutWrapperKind = Normalized.Left(GenericStartIndex).TrimStartAndEnd().ToLower();
	if (OutWrapperKind != TEXT("softclass") && OutWrapperKind != TEXT("interface"))
	{
		OutWrapperKind.Reset();
		return false;
	}

	const FString InnerArgs = Normalized.Mid(GenericStartIndex + 1, Normalized.Len() - GenericStartIndex - 2).TrimStartAndEnd();
	TArray<FString> ParsedArgs;
	if (InnerArgs.IsEmpty() || !IMP_TrySplitTopLevelTypeArguments(InnerArgs, ParsedArgs) || ParsedArgs.Num() != 1)
	{
		OutWrapperKind.Reset();
		return false;
	}

	OutInnerType = ParsedArgs[0];
	return true;
}

static bool IMP_BuildTerminalTypeFromLispType(const FString& TypeName, FEdGraphTerminalType& OutTerminalType, FBPImportContext& Ctx)
{
	FEdGraphPinType ValuePinType;
	if (!IMP_BuildPinTypeFromLispType(TypeName, ValuePinType, Ctx))
	{
		return false;
	}
	if (ValuePinType.IsArray() || ValuePinType.IsSet() || ValuePinType.IsMap())
	{
		Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: nested container type '%s' is not supported"), *TypeName));
		return false;
	}

	OutTerminalType = FEdGraphTerminalType();
	OutTerminalType.TerminalCategory = ValuePinType.PinCategory;
	OutTerminalType.TerminalSubCategory = ValuePinType.PinSubCategory;
	OutTerminalType.TerminalSubCategoryObject = ValuePinType.PinSubCategoryObject.Get();
	return true;
}

static bool IMP_BuildNonContainerPinTypeFromLispType(const FString& TypeName, FEdGraphPinType& OutPinType, FBPImportContext& Ctx)
{
	OutPinType = FEdGraphPinType();

	const FString Normalized = TypeName.TrimStartAndEnd();
	const FString Lower = Normalized.ToLower();
	if (Lower.IsEmpty())
	{
		return false;
	}

	if (Lower == TEXT("float"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		return true;
	}
	if (Lower == TEXT("double") || Lower == TEXT("real"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return true;
	}
	if (Lower == TEXT("vector"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		return true;
	}
	if (Lower == TEXT("vector2d"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
		return true;
	}
	if (Lower == TEXT("rotator"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		return true;
	}
	if (Lower == TEXT("transform"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		return true;
	}

	FName PinCategory = NAME_None;
	if (IMP_MapLispTypeToPinCategory(Normalized, PinCategory))
	{
		OutPinType.PinCategory = PinCategory;
		return true;
	}

	if (UScriptStruct* ResolvedStruct = IMP_FindStructByName(Normalized, Ctx))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = ResolvedStruct;
		return true;
	}

	FString WrapperKind;
	FString WrappedInnerType;
	if (IMP_TryParseWrappedReferenceTypeGrammar(Normalized, WrapperKind, WrappedInnerType))
	{
		UClass* ResolvedWrappedClass = IMP_FindClassByName(WrappedInnerType, Ctx);
		if (!ResolvedWrappedClass)
		{
			Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: could not resolve %s type '%s'"), *WrapperKind, *WrappedInnerType));
			return false;
		}

		if (WrapperKind == TEXT("softclass"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
			OutPinType.PinSubCategoryObject = ResolvedWrappedClass;
			return true;
		}

		if (!ResolvedWrappedClass->HasAnyClassFlags(CLASS_Interface))
		{
			Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: type '%s' is not an interface class"), *WrappedInnerType));
			return false;
		}

		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Interface;
		OutPinType.PinSubCategoryObject = ResolvedWrappedClass;
		return true;
	}

	if (UEnum* ResolvedEnum = IMP_FindEnumByName(Normalized))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		OutPinType.PinSubCategoryObject = ResolvedEnum;
		return true;
	}

	if (UClass* ResolvedClass = IMP_FindClassByName(Normalized, Ctx))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		OutPinType.PinSubCategoryObject = ResolvedClass;
		return true;
	}

	return false;
}

static bool IMP_BuildPinTypeFromLispType(const FString& TypeName, FEdGraphPinType& OutPinType, FBPImportContext& Ctx)
{
	OutPinType = FEdGraphPinType();

	if (IMP_LooksLikeContainerTypeGrammar(TypeName))
	{
		FString ContainerKind;
		TArray<FString> TypeArgs;
		if (!IMP_TryParseContainerTypeGrammar(TypeName, ContainerKind, TypeArgs))
		{
			Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: malformed container type '%s'"), *TypeName));
			return false;
		}

		if (!IMP_BuildNonContainerPinTypeFromLispType(TypeArgs[0], OutPinType, Ctx))
		{
			return false;
		}
		if (OutPinType.IsArray() || OutPinType.IsSet() || OutPinType.IsMap())
		{
			Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: nested container type '%s' is not supported"), *TypeName));
			return false;
		}

		if (ContainerKind == TEXT("array"))
		{
			OutPinType.ContainerType = EPinContainerType::Array;
			return true;
		}
		if (ContainerKind == TEXT("set"))
		{
			OutPinType.ContainerType = EPinContainerType::Set;
			return true;
		}

		FEdGraphTerminalType ValueTerminalType;
		if (!IMP_BuildTerminalTypeFromLispType(TypeArgs[1], ValueTerminalType, Ctx))
		{
			return false;
		}
		OutPinType.ContainerType = EPinContainerType::Map;
		OutPinType.PinValueType = ValueTerminalType;
		return true;
	}

	return IMP_BuildNonContainerPinTypeFromLispType(TypeName, OutPinType, Ctx);
}

static bool IMP_ArePinTypesEquivalent(const FEdGraphPinType& A, const FEdGraphPinType& B)
{
	if (const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>())
	{
		return K2Schema->ArePinTypesEquivalent(A, B);
	}
	return false;
}

static bool IMP_TryGetExistingBlueprintVariableType(UBlueprint* Blueprint, const FName& VarName, FEdGraphPinType& OutPinType)
{
	if (!Blueprint || VarName.IsNone())
	{
		return false;
	}

	UBlueprint* OwnerBlueprint = Blueprint;
	const int32 NewVarIndex = FBlueprintEditorUtils::FindNewVariableIndexAndBlueprint(Blueprint, VarName, OwnerBlueprint);
	if (OwnerBlueprint && NewVarIndex != INDEX_NONE && OwnerBlueprint->NewVariables.IsValidIndex(NewVarIndex))
	{
		OutPinType = OwnerBlueprint->NewVariables[NewVarIndex].VarType;
		return true;
	}

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	if (!K2Schema)
	{
		return false;
	}

	auto TryResolveFromClass = [&OutPinType, &VarName, K2Schema](const UClass* InClass) -> bool
	{
		if (!InClass)
		{
			return false;
		}
		if (const FProperty* ExistingProperty = InClass->FindPropertyByName(VarName))
		{
			return K2Schema->ConvertPropertyToPinType(ExistingProperty, OutPinType);
		}
		return false;
	};

	return TryResolveFromClass(Blueprint->SkeletonGeneratedClass)
		|| TryResolveFromClass(Blueprint->GeneratedClass)
		|| TryResolveFromClass(Blueprint->ParentClass);
}

static bool IMP_TryParseBoolLiteral(const FLispNodePtr& Node, bool& OutValue)
{
	if (!Node.IsValid() || Node->IsNil())
	{
		return false;
	}

	if (Node->IsSymbol() || Node->IsString() || Node->IsKeyword())
	{
		const FString Value = Node->StringValue;
		if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			OutValue = true;
			return true;
		}
		if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			OutValue = false;
			return true;
		}
	}

	if (Node->IsNumber())
	{
		if (FMath::IsNearlyZero(Node->NumberValue))
		{
			OutValue = false;
			return true;
		}
		if (FMath::IsNearlyEqual(Node->NumberValue, 1.0))
		{
			OutValue = true;
			return true;
		}
	}

	return false;
}

static bool IMP_TryBuildBlueprintVariableDefaultValueString(const FString& VarName, const FLispNodePtr& Expr, const FEdGraphPinType& PinType, FString& OutDefaultValue, FBPImportContext& Ctx)
{
	OutDefaultValue.Reset();
	if (!Expr.IsValid() || Expr->IsNil())
	{
		return true;
	}

	const FName PinCategory = PinType.PinCategory;
	if (Expr->IsNumber())
	{
		if (PinCategory == UEdGraphSchema_K2::PC_Int || PinCategory == UEdGraphSchema_K2::PC_Int64 || PinCategory == UEdGraphSchema_K2::PC_Byte)
		{
			const int64 RoundedValue = FMath::RoundToInt64(Expr->NumberValue);
			if (!FMath::IsNearlyEqual(Expr->NumberValue, static_cast<double>(RoundedValue)))
			{
				Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: :default for '%s' must be an integer literal"), *VarName));
				return false;
			}
			OutDefaultValue = LexToString(RoundedValue);
			return true;
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Real || PinCategory == UEdGraphSchema_K2::PC_Float || PinCategory == UEdGraphSchema_K2::PC_Double)
		{
			OutDefaultValue = FString::SanitizeFloat(Expr->NumberValue);
			return true;
		}

		Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: numeric :default is unsupported for variable '%s' of type '%s'"), *VarName, *PinTypeToLispType(PinType)));
		return false;
	}

	if (Expr->IsString())
	{
		if (PinCategory == UEdGraphSchema_K2::PC_String || PinCategory == UEdGraphSchema_K2::PC_Name || PinCategory == UEdGraphSchema_K2::PC_Text)
		{
			OutDefaultValue = Expr->StringValue;
			return true;
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Byte && PinType.PinSubCategoryObject.IsValid() && Cast<UEnum>(PinType.PinSubCategoryObject.Get()))
		{
			OutDefaultValue = Expr->StringValue;
			return true;
		}

		Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: string :default is unsupported for variable '%s' of type '%s'"), *VarName, *PinTypeToLispType(PinType)));
		return false;
	}

	if (Expr->IsSymbol() || Expr->IsKeyword())
	{
		const FString SymbolValue = Expr->StringValue;
		if (SymbolValue.Equals(TEXT("nil"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		if (PinCategory == UEdGraphSchema_K2::PC_Boolean)
		{
			bool bDefaultBool = false;
			if (!IMP_TryParseBoolLiteral(Expr, bDefaultBool))
			{
				Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: :default for '%s' must be true/false"), *VarName));
				return false;
			}
			OutDefaultValue = bDefaultBool ? TEXT("true") : TEXT("false");
			return true;
		}

		if (PinCategory == UEdGraphSchema_K2::PC_String || PinCategory == UEdGraphSchema_K2::PC_Name || PinCategory == UEdGraphSchema_K2::PC_Text)
		{
			OutDefaultValue = SymbolValue;
			return true;
		}
		if (PinCategory == UEdGraphSchema_K2::PC_Byte && PinType.PinSubCategoryObject.IsValid() && Cast<UEnum>(PinType.PinSubCategoryObject.Get()))
		{
			OutDefaultValue = SymbolValue;
			return true;
		}
	}

	Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: unsupported :default literal for variable '%s' of type '%s'"), *VarName, *PinTypeToLispType(PinType)));
	return false;
}

static bool IMP_TryParseBlueprintVariableImportSpec(const FLispNodePtr& Form, FIMPBlueprintVariableImportSpec& OutSpec, FBPImportContext& Ctx)
{
	OutSpec = FIMPBlueprintVariableImportSpec();
	if (!Form.IsValid() || !Form->IsList() || Form->Num() < 3)
	{
		Ctx.Errors.Add(TEXT("Import var form failed: expected (var Name Type [:default Literal] [:expose-on-spawn Bool])"));
		return false;
	}

	OutSpec.VarName = IMP_GetAtomName(Form->Get(1));
	OutSpec.TypeName = IMP_GetAtomName(Form->Get(2));
	if (OutSpec.VarName.IsEmpty() || OutSpec.TypeName.IsEmpty())
	{
		Ctx.Errors.Add(TEXT("Import var form failed: variable name or type is empty"));
		return false;
	}

	if (!IMP_BuildPinTypeFromLispType(OutSpec.TypeName, OutSpec.RequestedPinType, Ctx))
	{
		Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: unsupported variable type '%s' for '%s'"), *OutSpec.TypeName, *OutSpec.VarName));
		return false;
	}

	for (int32 ArgIndex = 3; ArgIndex < Form->Num(); ArgIndex += 2)
	{
		if (ArgIndex + 1 >= Form->Num())
		{
			Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: option '%s' is missing a value"), *IMP_GetAtomName(Form->Get(ArgIndex))));
			return false;
		}

		const FLispNodePtr KeywordNode = Form->Get(ArgIndex);
		if (!KeywordNode.IsValid() || !KeywordNode->IsKeyword())
		{
			Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: '%s' expects keyword/value options after the type"), *OutSpec.VarName));
			return false;
		}

		const FString Keyword = KeywordNode->StringValue;
		const FLispNodePtr ValueNode = Form->Get(ArgIndex + 1);
		if (Keyword.Equals(TEXT(":default"), ESearchCase::IgnoreCase))
		{
			if (OutSpec.bHasDefaultValue)
			{
				Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: duplicate :default for '%s'"), *OutSpec.VarName));
				return false;
			}
			if (!IMP_TryBuildBlueprintVariableDefaultValueString(OutSpec.VarName, ValueNode, OutSpec.RequestedPinType, OutSpec.DefaultValue, Ctx))
			{
				return false;
			}
			OutSpec.bHasDefaultValue = true;
			continue;
		}
		if (Keyword.Equals(TEXT(":expose-on-spawn"), ESearchCase::IgnoreCase))
		{
			if (OutSpec.bHasExposeOnSpawn)
			{
				Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: duplicate :expose-on-spawn for '%s'"), *OutSpec.VarName));
				return false;
			}
			if (!IMP_TryParseBoolLiteral(ValueNode, OutSpec.bExposeOnSpawn))
			{
				Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: :expose-on-spawn for '%s' must be true/false"), *OutSpec.VarName));
				return false;
			}
			OutSpec.bHasExposeOnSpawn = true;
			continue;
		}
		if (Keyword.Equals(TEXT(":instance-editable"), ESearchCase::IgnoreCase))
		{
			if (OutSpec.bHasInstanceEditable)
			{
				Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: duplicate :instance-editable for '%s'"), *OutSpec.VarName));
				return false;
			}
			if (!IMP_TryParseBoolLiteral(ValueNode, OutSpec.bInstanceEditable))
			{
				Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: :instance-editable for '%s' must be true/false"), *OutSpec.VarName));
				return false;
			}
			OutSpec.bHasInstanceEditable = true;
			continue;
		}

		Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: unsupported option '%s' for '%s'"), *Keyword, *OutSpec.VarName));
		return false;
	}

	if (OutSpec.bHasExposeOnSpawn && OutSpec.bHasInstanceEditable && OutSpec.bExposeOnSpawn && !OutSpec.bInstanceEditable)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: '%s' cannot set :expose-on-spawn true together with :instance-editable false"), *OutSpec.VarName));
		return false;
	}

	return true;
}

static bool IMP_TryFindOwnedBlueprintVariable(UBlueprint* Blueprint, const FName& VarName, UBlueprint*& OutOwnerBlueprint, int32& OutVarIndex)
{
	OutOwnerBlueprint = Blueprint;
	OutVarIndex = FBlueprintEditorUtils::FindNewVariableIndexAndBlueprint(Blueprint, VarName, OutOwnerBlueprint);
	return OutOwnerBlueprint && OutVarIndex != INDEX_NONE && OutOwnerBlueprint->NewVariables.IsValidIndex(OutVarIndex);
}

static bool IMP_ApplyBlueprintVariableImportSpec(const FIMPBlueprintVariableImportSpec& Spec, FBPImportContext& Ctx)
{
	if (!Ctx.Blueprint || (!Spec.bHasDefaultValue && !Spec.bHasExposeOnSpawn && !Spec.bHasInstanceEditable))
	{
		return true;
	}

	UBlueprint* OwnerBlueprint = nullptr;
	int32 VarIndex = INDEX_NONE;
	if (!IMP_TryFindOwnedBlueprintVariable(Ctx.Blueprint, FName(*Spec.VarName), OwnerBlueprint, VarIndex) || OwnerBlueprint != Ctx.Blueprint)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: variable '%s' is not declared on the target Blueprint, so top-level var options cannot be applied"), *Spec.VarName));
		return false;
	}

	bool bModified = false;
	bool bStructurallyModified = false;
	const FName VariableFName(*Spec.VarName);
	FBPVariableDescription& VariableDesc = Ctx.Blueprint->NewVariables[VarIndex];
	if (Spec.bHasDefaultValue && VariableDesc.DefaultValue != Spec.DefaultValue)
	{
		Ctx.Blueprint->Modify();
		VariableDesc.DefaultValue = Spec.DefaultValue;
		bModified = true;
	}

	bool bCurrentInstanceEditable = (VariableDesc.PropertyFlags & CPF_DisableEditOnInstance) == 0;
	if (Spec.bHasInstanceEditable && Spec.bInstanceEditable != bCurrentInstanceEditable)
	{
		Ctx.Blueprint->Modify();
		if (Spec.bInstanceEditable)
		{
			VariableDesc.PropertyFlags &= ~CPF_DisableEditOnInstance;
			VariableDesc.PropertyFlags |= (CPF_Edit | CPF_BlueprintVisible);
			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Ctx.Blueprint, VariableFName, false);
		}
		else
		{
			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Ctx.Blueprint, VariableFName, true);
		}
		bModified = true;
		bStructurallyModified = true;
		bCurrentInstanceEditable = (VariableDesc.PropertyFlags & CPF_DisableEditOnInstance) == 0;
	}

	if (Spec.bHasExposeOnSpawn)
	{
		FString ExistingExposeOnSpawnValue;
		const bool bHadExposeOnSpawnMeta = FBlueprintEditorUtils::GetBlueprintVariableMetaData(
			Ctx.Blueprint,
			VariableFName,
			nullptr,
			FBlueprintMetadata::MD_ExposeOnSpawn,
			ExistingExposeOnSpawnValue);
		const bool bCurrentlyExposeOnSpawn = bHadExposeOnSpawnMeta && ExistingExposeOnSpawnValue.Equals(TEXT("true"), ESearchCase::IgnoreCase);
		const bool bTargetInstanceEditable = Spec.bHasInstanceEditable ? Spec.bInstanceEditable : (Spec.bExposeOnSpawn || bCurrentInstanceEditable);
		if (bTargetInstanceEditable != bCurrentInstanceEditable)
		{
			Ctx.Blueprint->Modify();
			if (bTargetInstanceEditable)
			{
				VariableDesc.PropertyFlags &= ~CPF_DisableEditOnInstance;
				VariableDesc.PropertyFlags |= (CPF_Edit | CPF_BlueprintVisible);
				FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Ctx.Blueprint, VariableFName, false);
			}
			else
			{
				FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Ctx.Blueprint, VariableFName, true);
			}
			bModified = true;
			bStructurallyModified = true;
			bCurrentInstanceEditable = (VariableDesc.PropertyFlags & CPF_DisableEditOnInstance) == 0;
		}

		if (Spec.bExposeOnSpawn != bCurrentlyExposeOnSpawn || (!Spec.bExposeOnSpawn && bHadExposeOnSpawnMeta))
		{
			if (Spec.bExposeOnSpawn)
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(Ctx.Blueprint, VariableFName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
			}
			else
			{
				FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Ctx.Blueprint, VariableFName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn);
			}
			bModified = true;
		}
	}

	if (bStructurallyModified)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);
	}
	else if (bModified)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Ctx.Blueprint);
	}
	return true;
}

static void IMP_EnsureBlueprintVariablesFromTopLevelForms(const TArray<FLispNodePtr>& Nodes, FBPImportContext& Ctx)
{
	if (!Ctx.Blueprint)
	{
		return;
	}

	for (const FLispNodePtr& Form : Nodes)
	{
		if (!Form.IsValid() || !Form->IsList() || Form->Num() == 0)
		{
			continue;
		}

		if (!Form->GetFormName().Equals(TEXT("var"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		FIMPBlueprintVariableImportSpec Spec;
		if (!IMP_TryParseBlueprintVariableImportSpec(Form, Spec, Ctx))
		{
			continue;
		}

		FEdGraphPinType ExistingPinType;
		if (IMP_TryGetExistingBlueprintVariableType(Ctx.Blueprint, FName(*Spec.VarName), ExistingPinType))
		{
			if (!IMP_ArePinTypesEquivalent(ExistingPinType, Spec.RequestedPinType))
			{
				Ctx.Errors.Add(FString::Printf(
					TEXT("Import var form failed: variable '%s' already exists with type '%s', requested '%s'"),
					*Spec.VarName,
					*PinTypeToLispType(ExistingPinType),
					*PinTypeToLispType(Spec.RequestedPinType)));
				continue;
			}

			IMP_ApplyBlueprintVariableImportSpec(Spec, Ctx);
			continue;
		}

		if (!FBlueprintEditorUtils::AddMemberVariable(Ctx.Blueprint, FName(*Spec.VarName), Spec.RequestedPinType, Spec.bHasDefaultValue ? Spec.DefaultValue : FString()))
		{
			Ctx.Errors.Add(FString::Printf(TEXT("Import var form failed: could not create variable '%s'"), *Spec.VarName));
			continue;
		}

		IMP_ApplyBlueprintVariableImportSpec(Spec, Ctx);
	}
}

static void IMP_EnsureFunctionEntryParamsFromFunctionForm(UK2Node_FunctionEntry* ExistingEntry, const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (!ExistingEntry || !Form.IsValid()) return;

	int32 ArgStartIndex = 2;
	const FString FunctionName = IMP_ExtractCompoundName(Form, 1, ArgStartIndex);
	if (Ctx.Blueprint && Ctx.Blueprint->ParentClass && !FunctionName.IsEmpty())
	{
		if (UFunction* ParentFunction = Ctx.Blueprint->ParentClass->FindFunctionByName(FName(*FunctionName)))
		{
			if (UEdGraphSchema_K2::CanKismetOverrideFunction(ParentFunction))
			{
				ExistingEntry->FunctionReference.SetExternalMember(
					ParentFunction->GetFName(), ParentFunction->GetOuterUClass());
			}
		}
	}

	ExistingEntry->MetaData.bThreadSafe = Form->HasKeyword(TEXT(":thread-safe"))
		&& IMP_IsTruthy(Form->GetKeywordArg(TEXT(":thread-safe")));

	const bool bPure = Form->HasKeyword(TEXT(":pure"))
		&& IMP_IsTruthy(Form->GetKeywordArg(TEXT(":pure")));
	const uint32 PreviousExtraFlags = ExistingEntry->GetExtraFlags();
	const uint32 DesiredExtraFlags = bPure
		? PreviousExtraFlags | FUNC_BlueprintPure
		: PreviousExtraFlags & ~FUNC_BlueprintPure;
	ExistingEntry->SetExtraFlags(DesiredExtraFlags);

	bool bChanged = PreviousExtraFlags != DesiredExtraFlags;
	for (int32 i = ArgStartIndex; i + 1 < Form->Num(); ++i)
	{
		const FLispNodePtr KeywordNode = Form->Get(i);
		if (!KeywordNode.IsValid() || !KeywordNode->IsKeyword())
		{
			break;
		}

		const FString Keyword = KeywordNode->StringValue;
		const FLispNodePtr ValueNode = Form->Get(i + 1);
		if (Keyword.Equals(TEXT(":param"), ESearchCase::IgnoreCase))
		{
			FString ParamName;
			FString ParamType;
			if (!IMP_TryExtractNamedTypedPair(ValueNode, ParamName, ParamType))
			{
				Ctx.Errors.Add(TEXT("Import function form failed: invalid :param declaration"));
				i += 1;
				continue;
			}

			UEdGraphPin* ExistingParameterPin = nullptr;
			for (UEdGraphPin* ExistingPin : ExistingEntry->Pins)
			{
				if (!ExistingPin || ExistingPin->Direction != EGPD_Output || ExistingPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || ExistingPin->bHidden)
				{
					continue;
				}
				if (ExistingPin->PinName.ToString().Equals(ParamName, ESearchCase::IgnoreCase))
				{
					ExistingParameterPin = ExistingPin;
					break;
				}
			}

			FEdGraphPinType ParamPinType;
			if (!IMP_BuildPinTypeFromLispType(ParamType, ParamPinType, Ctx))
			{
				Ctx.Errors.Add(FString::Printf(TEXT("Import function form failed: unsupported parameter type '%s'"), *ParamType));
				i += 1;
				continue;
			}
			IMP_ApplyNamedTypedPairQualifiers(ValueNode, ParamPinType);

			if (ExistingParameterPin)
			{
				if (!IMP_ArePinTypesEquivalent(ExistingParameterPin->PinType, ParamPinType)
					|| ExistingParameterPin->PinType.bIsReference != ParamPinType.bIsReference
					|| ExistingParameterPin->PinType.bIsConst != ParamPinType.bIsConst)
				{
					ExistingParameterPin->PinType = ParamPinType;
					bChanged = true;
				}
				i += 1;
				continue;
			}

			if (!ExistingEntry->CreateUserDefinedPin(FName(*ParamName), ParamPinType, EGPD_Output, false))
			{
				Ctx.Errors.Add(FString::Printf(TEXT("Import function form failed: could not create parameter '%s'"), *ParamName));
				i += 1;
				continue;
			}
			bChanged = true;
		}

		i += 1;
	}

	if (!bChanged)
	{
		return;
	}

	const bool bPrevDisableOrphanPinSaving = ExistingEntry->bDisableOrphanPinSaving;
	ExistingEntry->bDisableOrphanPinSaving = true;
	ExistingEntry->ReconstructNode();
	ExistingEntry->bDisableOrphanPinSaving = bPrevDisableOrphanPinSaving;

	if (const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>())
	{
		K2Schema->HandleParameterDefaultValueChanged(ExistingEntry);
	}
}

static void IMP_EnsureFunctionLocalsFromFunctionForm(UK2Node_FunctionEntry* ExistingEntry, const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (!ExistingEntry || !Form.IsValid()) return;
	int32 ArgStartIndex = 2;
	IMP_ExtractCompoundName(Form, 1, ArgStartIndex);
	for (int32 Index = ArgStartIndex; Index + 1 < Form->Num(); ++Index)
	{
		const FLispNodePtr KeywordNode = Form->Get(Index);
		if (!KeywordNode.IsValid() || !KeywordNode->IsKeyword()) break;
		const FLispNodePtr ValueNode = Form->Get(Index + 1);
		if (KeywordNode->StringValue.Equals(TEXT(":local"), ESearchCase::IgnoreCase))
		{
			FString LocalName;
			FString LocalType;
			if (!IMP_TryExtractNamedTypedPair(ValueNode, LocalName, LocalType))
			{
				Ctx.Errors.Add(TEXT("Import function form failed: invalid :local declaration"));
				Index += 1;
				continue;
			}
			const bool bExists = ExistingEntry->LocalVariables.ContainsByPredicate(
				[&LocalName](const FBPVariableDescription& Description) { return Description.VarName == FName(*LocalName); });
			if (!bExists)
			{
				FEdGraphPinType LocalPinType;
				if (!IMP_BuildPinTypeFromLispType(LocalType, LocalPinType, Ctx))
				{
					Ctx.Errors.Add(FString::Printf(TEXT("Import function form failed: unsupported local type '%s'"), *LocalType));
					Index += 1;
					continue;
				}
				if (!FBlueprintEditorUtils::AddLocalVariable(Ctx.Blueprint, Ctx.Graph, FName(*LocalName), LocalPinType))
				{
					Ctx.Errors.Add(FString::Printf(TEXT("Import function form failed: could not add local '%s'"), *LocalName));
				}
			}
		}
		Index += 1;
	}
}

static void IMP_EnsureFunctionResultFromFunctionForm(UK2Node_FunctionEntry* ExistingEntry, const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (!ExistingEntry || !Ctx.Graph || !Form.IsValid()) return;

	UK2Node_FunctionResult* ResultNode = nullptr;
	for (UEdGraphNode* Node : Ctx.Graph->Nodes)
	{
		if (UK2Node_FunctionResult* ExistingResult = Cast<UK2Node_FunctionResult>(Node))
		{
			ResultNode = ExistingResult;
			break;
		}
	}
	if (!ResultNode)
	{
		ResultNode = NewObject<UK2Node_FunctionResult>(Ctx.Graph);
		ResultNode->FunctionReference = ExistingEntry->FunctionReference;
		ResultNode->NodePosX = ExistingEntry->NodePosX + 600;
		ResultNode->NodePosY = ExistingEntry->NodePosY;
		ResultNode->CreateNewGuid();
		ResultNode->PostPlacedNewNode();
		ResultNode->AllocateDefaultPins();
		Ctx.Graph->AddNode(ResultNode, false, false);
		UE_LOG(LogBlueprintLisp, Log, TEXT("Function import created result node %s in graph %s (%d graph nodes)"),
			*ResultNode->GetName(), *Ctx.Graph->GetName(), Ctx.Graph->Nodes.Num());
	}

	int32 ArgStartIndex = 2;
	IMP_ExtractCompoundName(Form, 1, ArgStartIndex);
	for (int32 Index = ArgStartIndex; Index + 1 < Form->Num(); ++Index)
	{
		const FLispNodePtr KeywordNode = Form->Get(Index);
		if (!KeywordNode.IsValid() || !KeywordNode->IsKeyword()) break;
		const FLispNodePtr ValueNode = Form->Get(Index + 1);
		if (KeywordNode->StringValue.Equals(TEXT(":return"), ESearchCase::IgnoreCase))
		{
			FString ReturnName;
			FString ReturnType;
			if (!IMP_TryExtractNamedTypedPair(ValueNode, ReturnName, ReturnType))
			{
				Ctx.Errors.Add(TEXT("Import function form failed: invalid :return declaration"));
				Index += 1;
				continue;
			}
			if (!IMP_FindInputPin(ResultNode, ReturnName))
			{
				FEdGraphPinType ReturnPinType;
				if (!IMP_BuildPinTypeFromLispType(ReturnType, ReturnPinType, Ctx))
				{
					Ctx.Errors.Add(FString::Printf(TEXT("Import function form failed: unsupported return type '%s'"), *ReturnType));
					Index += 1;
					continue;
				}
				if (!ResultNode->CreateUserDefinedPin(FName(*ReturnName), ReturnPinType, EGPD_Input, false))
				{
					Ctx.Errors.Add(FString::Printf(TEXT("Import function form failed: could not create return pin '%s'"), *ReturnName));
				}
			}
		}
		Index += 1;
	}
}

static FLispNodePtr IMP_MakeSeqBody(const TArray<FLispNodePtr>& Statements)
{

	if (Statements.Num() == 0) return FLispNode::MakeNil();
	if (Statements.Num() == 1) return Statements[0];

	TArray<FLispNodePtr> Items;
	Items.Reserve(Statements.Num() + 1);
	Items.Add(FLispNode::MakeSymbol(TEXT("seq")));
	for (const FLispNodePtr& Statement : Statements)
	{
		Items.Add(Statement);
	}
	return FLispNode::MakeList(Items);
}

static void IMP_RegisterEventOutputPins(UEdGraphNode* EventNode, FBPImportContext& Ctx)
{
	if (!EventNode) return;

	const FString EventGuid = EventNode->NodeGuid.ToString();
	Ctx.TempIdToNode.Add(EventGuid, EventNode);

	auto RegisterAlias = [&Ctx, &EventGuid](const FString& Alias, const FString& PinName)
	{
		if (Alias.IsEmpty() || PinName.IsEmpty()) return;
		Ctx.VariableToNodeId.Add(Alias, EventGuid);
		Ctx.VariableToPin.Add(Alias, PinName);
		const FString NoSpaces = Alias.Replace(TEXT(" "), TEXT(""));
		if (NoSpaces != Alias)
		{
			Ctx.VariableToNodeId.Add(NoSpaces, EventGuid);
			Ctx.VariableToPin.Add(NoSpaces, PinName);
		}
	};

	for (UEdGraphPin* Pin : EventNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->bHidden)
		{
			continue;
		}
		const FString PinName = Pin->PinName.ToString();
		RegisterAlias(PinName, PinName);
	}
}

static FObjectProperty* IMP_FindComponentProperty(UBlueprint* Blueprint, const FString& ComponentName)
{
	if (!Blueprint || ComponentName.IsEmpty()) return nullptr;

	const FName PropertyName(*ComponentName);
	if (Blueprint->GeneratedClass)
	{
		if (FObjectProperty* Property = FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, PropertyName))
		{
			return Property;
		}
	}
	if (Blueprint->SkeletonGeneratedClass)
	{
		if (FObjectProperty* Property = FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, PropertyName))
		{
			return Property;
		}
	}
	return nullptr;
}

static FMulticastDelegateProperty* IMP_FindDelegateProperty(UClass* OwnerClass, const FString& DelegateName)
{
	if (!OwnerClass || DelegateName.IsEmpty()) return nullptr;
	return FindFProperty<FMulticastDelegateProperty>(OwnerClass, FName(*DelegateName));
}

static AActor* IMP_FindActorInLevel(ULevel* Level, const FString& ActorName)
{
	if (!Level || ActorName.IsEmpty()) return nullptr;

	for (AActor* Actor : Level->Actors)
	{
		if (!Actor) continue;
		if (Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase)) return Actor;
		if (Actor->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase)) return Actor;
	}
	return nullptr;
}

static void IMP_ConvertInputActionForm(const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (!Form.IsValid() || !Form->IsList()) return;

	FString ActionName;
	TArray<FLispNodePtr> TrailingStatements;
	FLispNodePtr PressedBody = FLispNode::MakeNil();
	FLispNodePtr ReleasedBody = FLispNode::MakeNil();
	for (int32 i = 1; i < Form->Num();)
	{
		if (Form->Get(i)->IsKeyword())
		{
			const FString Keyword = Form->Get(i)->StringValue;
			const FLispNodePtr Value = (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil();
			if (Keyword.Equals(TEXT(":action"), ESearchCase::IgnoreCase))
			{
				ActionName = IMP_GetAtomName(Value);
			}
			else if (Keyword.Equals(TEXT(":pressed"), ESearchCase::IgnoreCase))
			{
				PressedBody = Value;
			}
			else if (Keyword.Equals(TEXT(":released"), ESearchCase::IgnoreCase))
			{
				ReleasedBody = Value;
			}
			i += 2;
			continue;
		}

		if (ActionName.IsEmpty())
		{
			ActionName = IMP_GetAtomName(Form->Get(i));
		}
		else
		{
			TrailingStatements.Add(Form->Get(i));
		}
		++i;
	}

	if ((!PressedBody.IsValid() || PressedBody->IsNil()) && TrailingStatements.Num() > 0)
	{
		PressedBody = IMP_MakeSeqBody(TrailingStatements);
	}
	if (ActionName.IsEmpty())
	{
		Ctx.Errors.Add(TEXT("IMP: input-action missing action name"));

		return;
	}

	UK2Node_InputAction* InputNode = IMP_FindReusableInputActionNode(Form, ActionName, Ctx);
	const bool bReusedExistingInputNode = (InputNode != nullptr);
	if (bReusedExistingInputNode)
	{
		IMP_PrepareExistingEventBodyForIncrementalReuse(InputNode, Ctx);
	}
	else
	{
		InputNode = NewObject<UK2Node_InputAction>(Ctx.Graph);
		InputNode->NodePosX = Ctx.CurrentX;
		InputNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(InputNode, false, false);
		InputNode->AllocateDefaultPins();
		IMP_EnsureGuid(InputNode);
	}

	InputNode->InputActionName = FName(*ActionName);
	InputNode->bConsumeInput = Form->HasKeyword(TEXT(":consume-input")) && IMP_IsTruthy(Form->GetKeywordArg(TEXT(":consume-input")));
	InputNode->bExecuteWhenPaused = Form->HasKeyword(TEXT(":execute-when-paused")) && IMP_IsTruthy(Form->GetKeywordArg(TEXT(":execute-when-paused")));
	InputNode->bOverrideParentBinding = Form->HasKeyword(TEXT(":override-parent-binding")) && IMP_IsTruthy(Form->GetKeywordArg(TEXT(":override-parent-binding")));
	if (bReusedExistingInputNode)
	{
		InputNode->ReconstructNode();
	}

	Ctx.ConsumedRootEventGuids.Add(InputNode->NodeGuid);
	Ctx.AdvancePosition();
	IMP_RegisterEventOutputPins(InputNode, Ctx);

	UEdGraphPin* PressedPin = InputNode->GetPressedPin();
	IMP_ConvertExecBody(PressedBody, Ctx, PressedPin);
	UEdGraphPin* ReleasedPin = InputNode->GetReleasedPin();
	IMP_ConvertExecBody(ReleasedBody, Ctx, ReleasedPin);
	if (bReusedExistingInputNode)
	{
		IMP_FinalizeExistingEventBodyIncrementalReuse(Ctx);
	}
	Ctx.NewRow();
}

static void IMP_ConvertInputKeyForm(const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (!Form.IsValid() || !Form->IsList()) return;

	FString KeyName;
	TArray<FLispNodePtr> TrailingStatements;
	FLispNodePtr PressedBody = FLispNode::MakeNil();
	FLispNodePtr ReleasedBody = FLispNode::MakeNil();
	for (int32 i = 1; i < Form->Num();)
	{
		if (Form->Get(i)->IsKeyword())
		{
			const FString Keyword = Form->Get(i)->StringValue;
			const FLispNodePtr Value = (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil();
			if (Keyword.Equals(TEXT(":key"), ESearchCase::IgnoreCase))
			{
				KeyName = IMP_GetAtomName(Value);
			}
			else if (Keyword.Equals(TEXT(":pressed"), ESearchCase::IgnoreCase))
			{
				PressedBody = Value;
			}
			else if (Keyword.Equals(TEXT(":released"), ESearchCase::IgnoreCase))
			{
				ReleasedBody = Value;
			}
			i += 2;
			continue;
		}

		if (KeyName.IsEmpty())
		{
			KeyName = IMP_GetAtomName(Form->Get(i));
		}
		else
		{
			TrailingStatements.Add(Form->Get(i));
		}
		++i;
	}

	if ((!PressedBody.IsValid() || PressedBody->IsNil()) && TrailingStatements.Num() > 0)
	{
		PressedBody = IMP_MakeSeqBody(TrailingStatements);
	}
	if (KeyName.IsEmpty())
	{
		Ctx.Errors.Add(TEXT("IMP: input-key missing key name"));

		return;
	}

	UK2Node_InputKey* InputNode = IMP_FindReusableInputKeyNode(Form, KeyName, Ctx);
	const bool bReusedExistingInputNode = (InputNode != nullptr);
	if (bReusedExistingInputNode)
	{
		IMP_PrepareExistingEventBodyForIncrementalReuse(InputNode, Ctx);
	}
	else
	{
		InputNode = NewObject<UK2Node_InputKey>(Ctx.Graph);
		InputNode->NodePosX = Ctx.CurrentX;
		InputNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(InputNode, false, false);
		InputNode->AllocateDefaultPins();
		IMP_EnsureGuid(InputNode);
	}

	InputNode->InputKey = FKey(*KeyName);
	InputNode->bConsumeInput = Form->HasKeyword(TEXT(":consume-input")) && IMP_IsTruthy(Form->GetKeywordArg(TEXT(":consume-input")));
	InputNode->bExecuteWhenPaused = Form->HasKeyword(TEXT(":execute-when-paused")) && IMP_IsTruthy(Form->GetKeywordArg(TEXT(":execute-when-paused")));
	InputNode->bOverrideParentBinding = Form->HasKeyword(TEXT(":override-parent-binding")) && IMP_IsTruthy(Form->GetKeywordArg(TEXT(":override-parent-binding")));
	InputNode->bControl = (Form->HasKeyword(TEXT(":control")) && IMP_IsTruthy(Form->GetKeywordArg(TEXT(":control"))))
		|| (Form->HasKeyword(TEXT(":ctrl")) && IMP_IsTruthy(Form->GetKeywordArg(TEXT(":ctrl"))));
	InputNode->bAlt = Form->HasKeyword(TEXT(":alt")) && IMP_IsTruthy(Form->GetKeywordArg(TEXT(":alt")));
	InputNode->bShift = Form->HasKeyword(TEXT(":shift")) && IMP_IsTruthy(Form->GetKeywordArg(TEXT(":shift")));
	InputNode->bCommand = Form->HasKeyword(TEXT(":command")) && IMP_IsTruthy(Form->GetKeywordArg(TEXT(":command")));
	if (bReusedExistingInputNode)
	{
		InputNode->ReconstructNode();
	}

	Ctx.ConsumedRootEventGuids.Add(InputNode->NodeGuid);
	Ctx.AdvancePosition();
	IMP_RegisterEventOutputPins(InputNode, Ctx);

	UEdGraphPin* PressedPin = InputNode->GetPressedPin();
	IMP_ConvertExecBody(PressedBody, Ctx, PressedPin);
	UEdGraphPin* ReleasedPin = InputNode->GetReleasedPin();
	IMP_ConvertExecBody(ReleasedBody, Ctx, ReleasedPin);
	if (bReusedExistingInputNode)
	{
		IMP_FinalizeExistingEventBodyIncrementalReuse(Ctx);
	}
	Ctx.NewRow();
}

static void IMP_ConvertComponentBoundEventForm(const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (!Form.IsValid() || !Form->IsList()) return;

	FString ComponentName;
	FString DelegateName;
	TArray<FLispNodePtr> BodyStatements;
	for (int32 i = 1; i < Form->Num();)
	{
		if (Form->Get(i)->IsKeyword())
		{
			const FString Keyword = Form->Get(i)->StringValue;
			const FLispNodePtr Value = (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil();
			if (Keyword.Equals(TEXT(":component"), ESearchCase::IgnoreCase))
			{
				ComponentName = IMP_GetAtomName(Value);
			}
			else if (Keyword.Equals(TEXT(":delegate"), ESearchCase::IgnoreCase))
			{
				DelegateName = IMP_GetAtomName(Value);
			}
			else if (Keyword.Equals(TEXT(":body"), ESearchCase::IgnoreCase))
			{
				BodyStatements.Add(Value);
			}
			i += 2;
			continue;
		}

		if (ComponentName.IsEmpty())
		{
			ComponentName = IMP_GetAtomName(Form->Get(i));
		}
		else if (DelegateName.IsEmpty())
		{
			DelegateName = IMP_GetAtomName(Form->Get(i));
		}
		else
		{
			BodyStatements.Add(Form->Get(i));
		}
		++i;
	}

	if (ComponentName.IsEmpty() || DelegateName.IsEmpty())
	{
		Ctx.Errors.Add(TEXT("IMP: component-bound-event missing component or delegate name"));

		return;
	}

	FObjectProperty* ComponentProperty = IMP_FindComponentProperty(Ctx.Blueprint, ComponentName);
	if (!ComponentProperty)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("IMP: component property not found for bound event: %s"), *ComponentName));

		return;
	}
	FMulticastDelegateProperty* DelegateProperty = IMP_FindDelegateProperty(ComponentProperty->PropertyClass, DelegateName);
	if (!DelegateProperty)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("IMP: component delegate not found: %s.%s"), *ComponentName, *DelegateName));

		return;
	}

	UK2Node_ComponentBoundEvent* EventNode = IMP_FindReusableComponentBoundEventNode(Form, ComponentName, DelegateName, Ctx);
	const bool bReusedExistingEventNode = (EventNode != nullptr);
	if (bReusedExistingEventNode)
	{
		IMP_PrepareExistingEventBodyForIncrementalReuse(EventNode, Ctx);
	}
	else
	{
		EventNode = NewObject<UK2Node_ComponentBoundEvent>(Ctx.Graph);
		EventNode->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
		EventNode->NodePosX = Ctx.CurrentX;
		EventNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(EventNode, false, false);
		EventNode->AllocateDefaultPins();
		IMP_EnsureGuid(EventNode);
	}

	Ctx.ConsumedRootEventGuids.Add(EventNode->NodeGuid);
	Ctx.AdvancePosition();
	IMP_RegisterEventOutputPins(EventNode, Ctx);

	UEdGraphPin* CurrentExecPin = IMP_GetExecOutput(EventNode);
	FLispNodePtr Body = IMP_MakeSeqBody(BodyStatements);
	IMP_ConvertExecBody(Body, Ctx, CurrentExecPin);
	if (bReusedExistingEventNode)
	{
		IMP_FinalizeExistingEventBodyIncrementalReuse(Ctx);
	}
	Ctx.NewRow();
}

static void IMP_ConvertActorBoundEventForm(const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (!Form.IsValid() || !Form->IsList()) return;

	FString ActorName;
	FString DelegateName;
	TArray<FLispNodePtr> BodyStatements;
	for (int32 i = 1; i < Form->Num();)
	{
		if (Form->Get(i)->IsKeyword())
		{
			const FString Keyword = Form->Get(i)->StringValue;
			const FLispNodePtr Value = (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil();
			if (Keyword.Equals(TEXT(":actor"), ESearchCase::IgnoreCase))
			{
				ActorName = IMP_GetAtomName(Value);
			}
			else if (Keyword.Equals(TEXT(":delegate"), ESearchCase::IgnoreCase))
			{
				DelegateName = IMP_GetAtomName(Value);
			}
			else if (Keyword.Equals(TEXT(":body"), ESearchCase::IgnoreCase))
			{
				BodyStatements.Add(Value);
			}
			i += 2;
			continue;
		}

		if (ActorName.IsEmpty())
		{
			ActorName = IMP_GetAtomName(Form->Get(i));
		}
		else if (DelegateName.IsEmpty())
		{
			DelegateName = IMP_GetAtomName(Form->Get(i));
		}
		else
		{
			BodyStatements.Add(Form->Get(i));
		}
		++i;
	}

	if (ActorName.IsEmpty() || DelegateName.IsEmpty())
	{
		Ctx.Errors.Add(TEXT("IMP: actor-bound-event missing actor or delegate name"));

		return;
	}

	ULevelScriptBlueprint* LevelBlueprint = Cast<ULevelScriptBlueprint>(Ctx.Blueprint);
	if (!LevelBlueprint)
	{
		Ctx.Errors.Add(TEXT("IMP: actor-bound-event currently requires a LevelScriptBlueprint target"));

		return;
	}
	ULevel* Level = LevelBlueprint->GetLevel();
	AActor* TargetActor = IMP_FindActorInLevel(Level, ActorName);
	if (!TargetActor)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("IMP: level actor not found for bound event: %s"), *ActorName));

		return;
	}
	FMulticastDelegateProperty* DelegateProperty = IMP_FindDelegateProperty(TargetActor->GetClass(), DelegateName);
	if (!DelegateProperty)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("IMP: actor delegate not found: %s.%s"), *ActorName, *DelegateName));

		return;
	}

	UK2Node_ActorBoundEvent* EventNode = IMP_FindReusableActorBoundEventNode(Form, ActorName, DelegateName, Ctx);
	const bool bReusedExistingEventNode = (EventNode != nullptr);
	if (bReusedExistingEventNode)
	{
		IMP_PrepareExistingEventBodyForIncrementalReuse(EventNode, Ctx);
	}
	else
	{
		EventNode = NewObject<UK2Node_ActorBoundEvent>(Ctx.Graph);
		EventNode->InitializeActorBoundEventParams(TargetActor, DelegateProperty);
		EventNode->NodePosX = Ctx.CurrentX;
		EventNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(EventNode, false, false);
		EventNode->AllocateDefaultPins();
		IMP_EnsureGuid(EventNode);
	}

	Ctx.ConsumedRootEventGuids.Add(EventNode->NodeGuid);
	Ctx.AdvancePosition();
	IMP_RegisterEventOutputPins(EventNode, Ctx);

	UEdGraphPin* CurrentExecPin = IMP_GetExecOutput(EventNode);
	FLispNodePtr Body = IMP_MakeSeqBody(BodyStatements);
	IMP_ConvertExecBody(Body, Ctx, CurrentExecPin);
	if (bReusedExistingEventNode)
	{
		IMP_FinalizeExistingEventBodyIncrementalReuse(Ctx);
	}
	Ctx.NewRow();
}

// --- Resolve pure expression → output pin ---
static UEdGraphPin* IMP_ResolveLispExprInternal(const FLispNodePtr& Expr, FBPImportContext& Ctx)
{
	if (!Expr.IsValid() || Expr->IsNil()) return nullptr;

	// Symbol: variable lookup or self
	if (Expr->IsSymbol())
	{
		FString Sym = Expr->StringValue;

		if (Sym.Equals(TEXT("self"), ESearchCase::IgnoreCase))
		{
			UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Ctx.Graph);
			SelfNode->NodePosX = Ctx.CurrentX; SelfNode->NodePosY = Ctx.CurrentY;
			Ctx.Graph->AddNode(SelfNode, false, false);
			SelfNode->AllocateDefaultPins(); IMP_EnsureGuid(SelfNode);
			Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), SelfNode);
			return SelfNode->FindPin(UEdGraphSchema_K2::PN_Self);
		}
		if (Sym.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Sym.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			if (UFunction* MakeLiteralBool = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("MakeLiteralBool")))
			{
				UK2Node_CallFunction* LiteralNode = NewObject<UK2Node_CallFunction>(Ctx.Graph);
				LiteralNode->SetFromFunction(MakeLiteralBool);
				LiteralNode->NodePosX = Ctx.CurrentX;
				LiteralNode->NodePosY = Ctx.CurrentY;
				Ctx.Graph->AddNode(LiteralNode, false, false);
				LiteralNode->AllocateDefaultPins();
				IMP_EnsureGuid(LiteralNode);
				Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), LiteralNode);
				if (UEdGraphPin* ValuePin = LiteralNode->FindPin(TEXT("Value")))
				{
					ValuePin->DefaultValue = Sym.Equals(TEXT("true"), ESearchCase::IgnoreCase) ? TEXT("true") : TEXT("false");
				}
				return IMP_FindOutputPin(LiteralNode, TEXT("ReturnValue"));
			}
			return nullptr;
		}

		// Check variable table
		if (Ctx.VariableToNodeId.Contains(Sym))
		{
			FString NodeId = Ctx.VariableToNodeId[Sym];
			FString PinName = Ctx.VariableToPin.Contains(Sym) ? Ctx.VariableToPin[Sym] : TEXT("");

			// Literal numeric
			if (NodeId.StartsWith(TEXT("_literal_")))
			{
				FString LitVal = PinName;
				FString LitKey = TEXT("_literalnode_") + Sym;
				if (UEdGraphNode** N = Ctx.TempIdToNode.Find(LitKey)) return IMP_FindOutputPin(*N, TEXT("ReturnValue"));
				UFunction* Mul = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Multiply_DoubleDouble"));
				if (!Mul) Mul = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Multiply_FloatFloat"));
				if (Mul)
				{
					UK2Node_CallFunction* LN = NewObject<UK2Node_CallFunction>(Ctx.Graph);
					LN->SetFromFunction(Mul); LN->NodePosX = Ctx.CurrentX; LN->NodePosY = Ctx.CurrentY;
					Ctx.Graph->AddNode(LN, false, false); LN->AllocateDefaultPins(); IMP_EnsureGuid(LN);
					if (UEdGraphPin* A = LN->FindPin(TEXT("A"))) A->DefaultValue = LitVal;
					if (UEdGraphPin* B = LN->FindPin(TEXT("B"))) B->DefaultValue = TEXT("1.0");
					Ctx.TempIdToNode.Add(LitKey, LN);
					return IMP_FindOutputPin(LN, TEXT("ReturnValue"));
				}
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: failed to materialize numeric literal binding for '%s'"), *Sym));
				return nullptr;

			}

			// Literal string
			if (NodeId.StartsWith(TEXT("_literalstr_")))
			{
				FString LitVal = PinName;
				FString LitKey = TEXT("_literalstrnode_") + Sym;
				if (UEdGraphNode** N = Ctx.TempIdToNode.Find(LitKey)) return IMP_FindOutputPin(*N, TEXT("ReturnValue"));
				UFunction* Cat = UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Concat_StrStr"));
				if (Cat)
				{
					UK2Node_CallFunction* LN = NewObject<UK2Node_CallFunction>(Ctx.Graph);
					LN->SetFromFunction(Cat); LN->NodePosX = Ctx.CurrentX; LN->NodePosY = Ctx.CurrentY;
					Ctx.Graph->AddNode(LN, false, false); LN->AllocateDefaultPins(); IMP_EnsureGuid(LN);
					if (UEdGraphPin* A = LN->FindPin(TEXT("A"))) A->DefaultValue = LitVal;
					Ctx.TempIdToNode.Add(LitKey, LN);
					return IMP_FindOutputPin(LN, TEXT("ReturnValue"));
				}
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: failed to materialize string literal binding for '%s'"), *Sym));
				return nullptr;

			}

			// Direct variable key lookup
			FString VarKey = TEXT("_var_") + Sym;
			if (UEdGraphNode** N = Ctx.TempIdToNode.Find(VarKey))
				if (UEdGraphPin* P = IMP_FindOutputPinByName(*N, PinName)) return P;

			// NodeGuid lookup
			if (UEdGraphNode** N = Ctx.TempIdToNode.Find(NodeId))
				if (UEdGraphPin* P = IMP_FindOutputPinByName(*N, PinName)) return P;

			// Graph scan
			for (UEdGraphNode* N : Ctx.Graph->Nodes)
				if (N && N->NodeGuid.ToString() == NodeId)
					if (UEdGraphPin* P = IMP_FindOutputPinByName(N, PinName))
					{ Ctx.TempIdToNode.Add(NodeId, N); return P; }

		}

		auto FindMatchingOutputPin = [](UEdGraphNode* Node, const FString& RequestedName) -> UEdGraphPin*
		{
			if (!Node || RequestedName.IsEmpty()) return nullptr;

			const FString RequestedNoSpaces = RequestedName.Replace(TEXT(" "), TEXT(""));
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

				const FString PinName = Pin->PinName.ToString();
				if (PinName.Equals(RequestedName, ESearchCase::IgnoreCase))
				{
					return Pin;
				}

				const FString PinNoSpaces = PinName.Replace(TEXT(" "), TEXT(""));
				if (!RequestedNoSpaces.IsEmpty() && PinNoSpaces.Equals(RequestedNoSpaces, ESearchCase::IgnoreCase))
				{
					return Pin;
				}
			}

			return nullptr;
		};

		if (Ctx.Graph)
		{
			for (int32 NodeIndex = Ctx.Graph->Nodes.Num() - 1; NodeIndex >= 0; --NodeIndex)
			{
				UEdGraphNode* ExistingNode = Ctx.Graph->Nodes[NodeIndex];
				if (!ExistingNode || IMP_IsPendingReusableBodyNode(ExistingNode, Ctx))
				{
					continue;
				}
				if (UEdGraphPin* MatchedPin = FindMatchingOutputPin(ExistingNode, Sym))
				{

					IMP_EnsureGuid(ExistingNode);
					const FString ExistingNodeGuid = ExistingNode->NodeGuid.ToString();
					Ctx.TempIdToNode.FindOrAdd(ExistingNodeGuid) = ExistingNode;
					Ctx.VariableToNodeId.Add(Sym, ExistingNodeGuid);
					Ctx.VariableToPin.Add(Sym, MatchedPin->PinName.ToString());
					const FString SymNoSpaces = Sym.Replace(TEXT(" "), TEXT(""));
					if (SymNoSpaces != Sym)
					{
						Ctx.VariableToNodeId.Add(SymNoSpaces, ExistingNodeGuid);
						Ctx.VariableToPin.Add(SymNoSpaces, MatchedPin->PinName.ToString());
					}
					return MatchedPin;
				}
			}
		}

		for (UEdGraphNode* GraphNode : Ctx.Graph->Nodes)
		{
			UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(GraphNode);
			if (!FunctionEntry) continue;
			if (FBPVariableDescription* LocalDescription = FunctionEntry->LocalVariables.FindByPredicate(
				[&Sym](const FBPVariableDescription& Description) { return Description.VarName == FName(*Sym); }))
			{
				UK2Node_VariableGet* LocalGet = NewObject<UK2Node_VariableGet>(Ctx.Graph);
				LocalGet->VariableReference.SetLocalMember(FName(*Sym), Ctx.Graph->GetName(), LocalDescription->VarGuid);
				LocalGet->NodePosX = Ctx.CurrentX;
				LocalGet->NodePosY = Ctx.CurrentY;
				Ctx.Graph->AddNode(LocalGet, false, false);
				LocalGet->AllocateDefaultPins();
				IMP_EnsureGuid(LocalGet);
				UEdGraphPin* ValuePin = IMP_FindOutputPinByName(LocalGet, Sym);
				if (!ValuePin)
				{
					ValuePin = LocalGet->CreatePin(EGPD_Output, LocalDescription->VarType, FName(*Sym));
				}
				Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), LocalGet);
				IMP_RegisterBoundValue(Sym, ValuePin, Ctx);
				return ValuePin;
			}
		}

		// Fallback: create member variable get
		UK2Node_VariableGet* VarGet = NewObject<UK2Node_VariableGet>(Ctx.Graph);
		VarGet->VariableReference.SetSelfMember(FName(*Sym));
		VarGet->NodePosX = Ctx.CurrentX; VarGet->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(VarGet, false, false); VarGet->AllocateDefaultPins(); IMP_EnsureGuid(VarGet);
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), VarGet);
		return IMP_FindOutputPin(VarGet, TEXT(""));
	}


	if (!Expr->IsList() || Expr->Num() == 0) return nullptr;

	FString FormName = Expr->GetFormName();
	if (FormName.Equals(TEXT("select"), ESearchCase::IgnoreCase))
	{
		bool bHandled = false;
		return IMP_TryBuildSelectOutputPin(Expr, nullptr, Ctx, bHandled);
	}
	if (FormName.Equals(TEXT("break-struct"), ESearchCase::IgnoreCase))
	{
		bool bHandled = false;
		return IMP_TryBuildBreakStructOutputPin(Expr, nullptr, Ctx, bHandled);
	}

	// (collapsed-graph :name "Graph" :input (Name Type Expr)...
	//                  :output (Name Type Expr)... :selected Name :id "...")
	if (FormName.Equals(TEXT("collapsed-graph"), ESearchCase::IgnoreCase))
	{
		struct FBoundarySpec
		{
			FString Name;
			FString Type;
			FLispNodePtr Expression;
		};

		TArray<FBoundarySpec> Inputs;
		TArray<FBoundarySpec> Outputs;
		for (int32 Index = 1; Index + 1 < Expr->Num(); ++Index)
		{
			const FLispNodePtr Keyword = Expr->Get(Index);
			if (!Keyword.IsValid() || !Keyword->IsKeyword())
			{
				continue;
			}
			const bool bInput = Keyword->StringValue.Equals(TEXT(":input"), ESearchCase::IgnoreCase);
			const bool bOutput = Keyword->StringValue.Equals(TEXT(":output"), ESearchCase::IgnoreCase);
			if (!bInput && !bOutput)
			{
				Index++;
				continue;
			}

			const FLispNodePtr SpecForm = Expr->Get(++Index);
			if (!SpecForm.IsValid() || !SpecForm->IsList() || SpecForm->Num() < 3)
			{
				Ctx.Errors.Add(TEXT("IMP: collapsed-graph boundary spec must be (Name Type Expression)"));
				continue;
			}
			FBoundarySpec Spec;
			Spec.Name = IMP_GetAtomName(SpecForm->Get(0));
			Spec.Type = IMP_GetAtomName(SpecForm->Get(1));
			Spec.Expression = SpecForm->Get(2);
			(bInput ? Inputs : Outputs).Add(MoveTemp(Spec));
		}

		if (Outputs.IsEmpty())
		{
			Ctx.Errors.Add(TEXT("IMP: collapsed-graph must declare at least one :output"));
			return nullptr;
		}

		UK2Node_Composite* CompositeNode = NewObject<UK2Node_Composite>(Ctx.Graph);
		CompositeNode->NodePosX = Ctx.CurrentX;
		CompositeNode->NodePosY = Ctx.CurrentY;
		CompositeNode->CreateNewGuid();
		Ctx.Graph->AddNode(CompositeNode, false, false);
		CompositeNode->PostPlacedNewNode();
		CompositeNode->AllocateDefaultPins();
		if (const FLispNodePtr NameNode = Expr->GetKeywordArg(TEXT(":name")); NameNode.IsValid() && !NameNode->IsNil())
		{
			CompositeNode->OnRenameNode(IMP_GetAtomName(NameNode));
		}

		UK2Node_Tunnel* EntryTunnel = CompositeNode->GetEntryNode();
		UK2Node_Tunnel* ExitTunnel = CompositeNode->GetExitNode();
		for (const FBoundarySpec& Spec : Inputs)
		{
			FEdGraphPinType PinType;
			if (IMP_BuildPinTypeFromLispType(Spec.Type, PinType, Ctx))
			{
				EntryTunnel->CreateUserDefinedPin(FName(*Spec.Name), PinType, EGPD_Output, false);
			}
		}
		for (const FBoundarySpec& Spec : Outputs)
		{
			FEdGraphPinType PinType;
			if (IMP_BuildPinTypeFromLispType(Spec.Type, PinType, Ctx))
			{
				ExitTunnel->CreateUserDefinedPin(FName(*Spec.Name), PinType, EGPD_Input, false);
			}
		}
		CompositeNode->ReconstructNode();

		for (const FBoundarySpec& Spec : Inputs)
		{
			if (UEdGraphPin* OuterInput = CompositeNode->FindPin(FName(*Spec.Name), EGPD_Input))
			{
				IMP_SetPinFromExpr(OuterInput, Spec.Expression, Ctx);
			}
		}

		FBPImportContext InnerCtx;
		InnerCtx.Blueprint = Ctx.Blueprint;
		InnerCtx.Graph = CompositeNode->BoundGraph;
		InnerCtx.ImportMode = Ctx.ImportMode;
		InnerCtx.FunctionCache = Ctx.FunctionCache;
		IMP_EnsureGuid(EntryTunnel);
		const FString EntryGuid = EntryTunnel->NodeGuid.ToString();
		InnerCtx.TempIdToNode.Add(EntryGuid, EntryTunnel);
		for (const FBoundarySpec& Spec : Inputs)
		{
			InnerCtx.VariableToNodeId.Add(Spec.Name, EntryGuid);
			InnerCtx.VariableToPin.Add(Spec.Name, Spec.Name);
		}
		for (const FBoundarySpec& Spec : Outputs)
		{
			UEdGraphPin* ExitPin = ExitTunnel->FindPin(FName(*Spec.Name), EGPD_Input);
			if (!ExitPin || !IMP_SetPinFromExpr(ExitPin, Spec.Expression, InnerCtx))
			{
				InnerCtx.Errors.Add(FString::Printf(TEXT("IMP: collapsed-graph output '%s' could not be reconstructed"), *Spec.Name));
			}
		}
		Ctx.FunctionCache = InnerCtx.FunctionCache;
		Ctx.Errors.Append(InnerCtx.Errors);
		Ctx.Warnings.Append(InnerCtx.Warnings);

		IMP_EnsureGuid(CompositeNode);
		Ctx.TempIdToNode.FindOrAdd(CompositeNode->NodeGuid.ToString()) = CompositeNode;
		Ctx.AdvancePosition();
		const FString SelectedName = IMP_GetKeywordAtomValue(Expr, TEXT(":selected"));
		return CompositeNode->FindPin(FName(*(SelectedName.IsEmpty() ? Outputs[0].Name : SelectedName)), EGPD_Output);
	}

	// (asset "path")
	if (FormName.Equals(TEXT("asset"), ESearchCase::IgnoreCase) && Expr->Num() >= 2)
	{
		Ctx.LastAssetPath = Expr->Get(1)->IsString() ? Expr->Get(1)->StringValue : Expr->Get(1)->StringValue;
		return nullptr;
	}

	// (call-macro Name [:input value]... [:out (Pin Type)]...)
	if (FormName.Equals(TEXT("call-macro"), ESearchCase::IgnoreCase) && Expr->Num() >= 2)
	{
		UEdGraphPin* PreferredOutputPin = nullptr;
		if (UK2Node_MacroInstance* MacroNode = IMP_CreateMacroInstanceNode(Expr, Ctx, PreferredOutputPin))
		{
			return PreferredOutputPin ? PreferredOutputPin : IMP_FindOutputPin(MacroNode, TEXT(""));
		}
		return nullptr;
	}

	if ((FormName.Equals(TEXT("enum-to-name"), ESearchCase::IgnoreCase)
		|| FormName.Equals(TEXT("enum-to-string"), ESearchCase::IgnoreCase)) && Expr->Num() >= 2)
	{
		UK2Node_GetEnumeratorName* EnumNameNode = FormName.Equals(TEXT("enum-to-string"), ESearchCase::IgnoreCase)
			? static_cast<UK2Node_GetEnumeratorName*>(NewObject<UK2Node_GetEnumeratorNameAsString>(Ctx.Graph))
			: NewObject<UK2Node_GetEnumeratorName>(Ctx.Graph);
		EnumNameNode->NodePosX = Ctx.CurrentX;
		EnumNameNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(EnumNameNode, false, false);
		EnumNameNode->AllocateDefaultPins();
		IMP_EnsureGuid(EnumNameNode);
		UEdGraphPin* InputPin = nullptr;
		UEdGraphPin* OutputPin = nullptr;
		for (UEdGraphPin* Pin : EnumNameNode->Pins)
		{
			if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (!InputPin && Pin->Direction == EGPD_Input) InputPin = Pin;
			if (!OutputPin && Pin->Direction == EGPD_Output) OutputPin = Pin;
		}
		if (!InputPin || !OutputPin || !IMP_SetPinFromExpr(InputPin, Expr->Get(1), Ctx))
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: %s could not reconstruct its enum input"), *FormName));
			return nullptr;
		}
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), EnumNameNode);
		Ctx.TempIdToNode.Add(EnumNameNode->NodeGuid.ToString(), EnumNameNode);
		Ctx.AdvancePosition();
		return OutputPin;
	}

	if (FormName.Equals(TEXT("anim-node-reference"), ESearchCase::IgnoreCase))
	{
		UK2Node_AnimNodeReference* ReferenceNode = NewObject<UK2Node_AnimNodeReference>(Ctx.Graph);
		if (FNameProperty* TagProperty = FindFProperty<FNameProperty>(ReferenceNode->GetClass(), TEXT("Tag")))
		{
			TagProperty->SetPropertyValue_InContainer(ReferenceNode, FName(*IMP_GetKeywordAtomValue(Expr, TEXT(":tag"))));
		}
		ReferenceNode->NodePosX = Ctx.CurrentX;
		ReferenceNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(ReferenceNode, false, false);
		static_cast<UK2Node*>(ReferenceNode)->AllocateDefaultPins();
		IMP_EnsureGuid(ReferenceNode);
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), ReferenceNode);
		Ctx.TempIdToNode.Add(ReferenceNode->NodeGuid.ToString(), ReferenceNode);
		Ctx.AdvancePosition();
		return IMP_FindOutputPinByName(ReferenceNode, TEXT("Value"));
	}

	if (FormName.Equals(TEXT("pure-cast"), ESearchCase::IgnoreCase)
		|| FormName.Equals(TEXT("pure-cast-succeeds"), ESearchCase::IgnoreCase))
	{
		const FString TargetClassPath = IMP_GetKeywordAtomValue(Expr, TEXT(":class"));
		UClass* TargetClass = LoadObject<UClass>(nullptr, *TargetClassPath);
		const FLispNodePtr ObjectExpr = Expr->GetKeywordArg(TEXT(":object"));
		if (!TargetClass || !ObjectExpr.IsValid() || ObjectExpr->IsNil())
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: invalid pure-cast target or object: %s"), *TargetClassPath));
			return nullptr;
		}
		UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Ctx.Graph);
		CastNode->TargetType = TargetClass;
		CastNode->SetPurity(true);
		CastNode->NodePosX = Ctx.CurrentX;
		CastNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(CastNode, false, false);
		CastNode->AllocateDefaultPins();
		IMP_EnsureGuid(CastNode);
		if (!IMP_SetPinFromExpr(CastNode->GetCastSourcePin(), ObjectExpr, Ctx))
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: pure-cast object could not be reconstructed: %s"), *TargetClassPath));
			return nullptr;
		}
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), CastNode);
		Ctx.TempIdToNode.Add(CastNode->NodeGuid.ToString(), CastNode);
		Ctx.AdvancePosition();
		return FormName.Equals(TEXT("pure-cast-succeeds"), ESearchCase::IgnoreCase)
			? CastNode->GetBoolSuccessPin() : CastNode->GetCastResultPin();
	}

	if (FormName.Equals(TEXT("property-access"), ESearchCase::IgnoreCase))
	{
		UClass* PropertyAccessClass = LoadClass<UK2Node>(nullptr, TEXT("/Script/PropertyAccessNode.K2Node_PropertyAccess"));
		if (!PropertyAccessClass)
		{
			Ctx.Errors.Add(TEXT("IMP: K2Node_PropertyAccess class is unavailable"));
			return nullptr;
		}
		UK2Node* PropertyAccessNode = NewObject<UK2Node>(Ctx.Graph, PropertyAccessClass);
		PropertyAccessNode->NodePosX = Ctx.CurrentX;
		PropertyAccessNode->NodePosY = Ctx.CurrentY;
		if (FArrayProperty* PathProperty = FindFProperty<FArrayProperty>(PropertyAccessClass, TEXT("Path")))
		{
			const FLispNodePtr PathNode = Expr->GetKeywordArg(TEXT(":path"));
			FScriptArrayHelper PathHelper(PathProperty, PathProperty->ContainerPtrToValuePtr<void>(PropertyAccessNode));
			PathHelper.EmptyValues();
			if (PathNode.IsValid() && PathNode->IsList())
			{
				if (FStrProperty* PathElementProperty = CastField<FStrProperty>(PathProperty->Inner))
				{
					for (int32 PathIndex = 0; PathIndex < PathNode->Num(); ++PathIndex)
					{
						const FLispNodePtr SegmentNode = PathNode->Get(PathIndex);
						const int32 AddedIndex = PathHelper.AddValue();
						PathElementProperty->SetPropertyValue(PathHelper.GetRawPtr(AddedIndex), IMP_GetAtomName(SegmentNode));
					}
				}
			}
		}
		if (FNameProperty* ContextProperty = FindFProperty<FNameProperty>(PropertyAccessClass, TEXT("ContextId")))
		{
			const FString ContextId = IMP_GetKeywordAtomValue(Expr, TEXT(":context"));
			if (!ContextId.IsEmpty()) ContextProperty->SetPropertyValue_InContainer(PropertyAccessNode, FName(*ContextId));
		}
		Ctx.Graph->AddNode(PropertyAccessNode, false, false);
		PropertyAccessNode->AllocateDefaultPins();
		IMP_EnsureGuid(PropertyAccessNode);
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), PropertyAccessNode);
		Ctx.TempIdToNode.Add(PropertyAccessNode->NodeGuid.ToString(), PropertyAccessNode);
		Ctx.AdvancePosition();
		return IMP_FindOutputPinByName(PropertyAccessNode, TEXT("Value"));
	}


	if (FormName.Equals(TEXT("make-struct"), ESearchCase::IgnoreCase))
	{
		const FString StructName = IMP_GetKeywordAtomValue(Expr, TEXT(":struct"));
		UScriptStruct* StructType = IMP_FindStructByName(StructName, Ctx);
		if (!StructType)
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: make-struct type not found: %s"), *StructName));
			return nullptr;
		}

		UEdGraphNode* BuilderNode = nullptr;
		if (StructType->HasMetaData(FBlueprintMetadata::MD_NativeMakeFunction))
		{
			const FString NativeMakePath = StructType->GetMetaData(FBlueprintMetadata::MD_NativeMakeFunction);
			if (UFunction* NativeMakeFunction = FindObject<UFunction>(nullptr, *NativeMakePath, true))
			{
				UK2Node_CallFunction* MakeCallNode = NewObject<UK2Node_CallFunction>(Ctx.Graph);
				MakeCallNode->SetFromFunction(NativeMakeFunction);
				BuilderNode = MakeCallNode;
			}
		}
		if (!BuilderNode)
		{
			UK2Node_MakeStruct* MakeStructNode = NewObject<UK2Node_MakeStruct>(Ctx.Graph);
			MakeStructNode->StructType = StructType;
			BuilderNode = MakeStructNode;
		}

		BuilderNode->NodePosX = Ctx.CurrentX;
		BuilderNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(BuilderNode, false, false);
		BuilderNode->AllocateDefaultPins();
		IMP_EnsureGuid(BuilderNode);
		Ctx.AdvancePosition();
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), BuilderNode);
		Ctx.TempIdToNode.Add(BuilderNode->NodeGuid.ToString(), BuilderNode);

		for (int32 Index = 1; Index + 1 < Expr->Num(); ++Index)
		{
			const FLispNodePtr KeywordNode = Expr->Get(Index);
			if (!KeywordNode.IsValid() || !KeywordNode->IsKeyword()
				|| !KeywordNode->StringValue.Equals(TEXT(":field"), ESearchCase::IgnoreCase)) continue;
			const FLispNodePtr FieldSpec = Expr->Get(++Index);
			if (!FieldSpec.IsValid() || !FieldSpec->IsList() || FieldSpec->Num() < 3) continue;
			const FString FieldName = IMP_GetAtomName(FieldSpec->Get(0));
			if (UEdGraphPin* FieldPin = IMP_FindInputPin(BuilderNode, FieldName))
			{
				IMP_SetPinFromExpr(FieldPin, FieldSpec->Get(2), Ctx);
			}
			else
			{
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: make-struct input pin not found: %s.%s"), *StructName, *FieldName));
			}
		}
		return IMP_FindOutputPin(BuilderNode, TEXT("ReturnValue"));
	}

	if (FormName.Equals(TEXT("make-array"), ESearchCase::IgnoreCase))
	{
		TArray<FLispNodePtr> ItemExprs;

		for (int32 i = 1; i < Expr->Num(); ++i)
		{
			FLispNodePtr ArgExpr = Expr->Get(i);
			if (ArgExpr->IsKeyword())
			{
				i++;
				continue;
			}
			ItemExprs.Add(ArgExpr);
		}

		UK2Node_MakeArray* MakeArrayNode = IMP_CreateOrReuseMakeArrayNode(Expr, ItemExprs.Num(), Ctx);
		TArray<UEdGraphPin*> InputPins = IMP_GetMakeArrayValueInputPins(MakeArrayNode);

		if (ItemExprs.Num() == 0)
		{
			if (InputPins.Num() > 0)
			{
				MakeArrayNode->RemoveInputPin(InputPins[0]);
				InputPins = IMP_GetMakeArrayValueInputPins(MakeArrayNode);

			}
		}
		else
		{
			while (InputPins.Num() < ItemExprs.Num())
			{
				MakeArrayNode->AddInputPin();
				InputPins = IMP_GetMakeArrayValueInputPins(MakeArrayNode);

			}
		}

		IMP_SeedMakeArrayLiteralType(MakeArrayNode, InputPins, ItemExprs);

		for (int32 Index = 0; Index < ItemExprs.Num() && Index < InputPins.Num(); ++Index)
		{
			IMP_SetPinFromExpr(InputPins[Index], ItemExprs[Index], Ctx);
		}

		Ctx.TempIdToNode.FindOrAdd(MakeArrayNode->NodeGuid.ToString()) = MakeArrayNode;
		return MakeArrayNode->GetOutputPin();

	}

	if (FormName.Equals(TEXT("get-array-item"), ESearchCase::IgnoreCase))
	{
		UK2Node_GetArrayItem* GetArrayItemNode = IMP_CreateOrReuseGetArrayItemNode(Expr, Ctx);

		FLispNodePtr ArrayExpr = Expr->HasKeyword(TEXT(":array"))

			? Expr->GetKeywordArg(TEXT(":array"))
			: (Expr->Num() > 1 ? Expr->Get(1) : FLispNode::MakeNil());
		FLispNodePtr IndexExpr = Expr->HasKeyword(TEXT(":index"))
			? Expr->GetKeywordArg(TEXT(":index"))
			: (Expr->Num() > 2 ? Expr->Get(2) : FLispNode::MakeNil());

		if (UEdGraphPin* ArrayPin = GetArrayItemNode->GetTargetArrayPin())
		{
			IMP_SetPinFromExpr(ArrayPin, ArrayExpr, Ctx);
		}
		if (UEdGraphPin* IndexPin = GetArrayItemNode->GetIndexPin())
		{
			IMP_SetPinFromExpr(IndexPin, IndexExpr, Ctx);
		}

		Ctx.TempIdToNode.FindOrAdd(GetArrayItemNode->NodeGuid.ToString()) = GetArrayItemNode;
		return GetArrayItemNode->GetResultPin();

	}

	if (FormName.Equals(TEXT("=="), ESearchCase::CaseSensitive) || FormName.Equals(TEXT("!="), ESearchCase::CaseSensitive))
	{
		const bool bRequestedEquality = FormName.Equals(TEXT("=="), ESearchCase::CaseSensitive);
		if (UK2Node* EnumCompareNode = IMP_CreateOrReuseEnumCompareNode(Expr, bRequestedEquality, Ctx))
		{
			int32 ArgIndex = 1;
			int32 DataPinIndex = 0;
			for (UEdGraphPin* Pin : EnumCompareNode->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (ArgIndex >= Expr->Num()) break;

				FLispNodePtr ArgExpr = Expr->Get(ArgIndex);
				if (ArgExpr->IsKeyword())
				{
					ArgIndex++;
					if (ArgIndex >= Expr->Num()) break;
					ArgExpr = Expr->Get(ArgIndex);
				}

				IMP_SetPinFromExpr(Pin, ArgExpr, Ctx);
				ArgIndex++;
				DataPinIndex++;
				if (DataPinIndex >= 2) break;
			}

			Ctx.TempIdToNode.FindOrAdd(EnumCompareNode->NodeGuid.ToString()) = EnumCompareNode;
			return IMP_FindOutputPin(EnumCompareNode, TEXT(""));
		}
		return nullptr;
	}

	// Generic function call / pure expr: (FuncName [self] [:pin value]...)

	int32 CompoundArgStartIndex = 1;
	const FString CompoundFormName = IMP_ExtractCompoundName(Expr, 0, CompoundArgStartIndex);
	if (CompoundArgStartIndex >= Expr->Num() && CompoundFormName.StartsWith(TEXT("self."), ESearchCase::IgnoreCase))
	{
		const FString MemberName = CompoundFormName.Mid(5);
		UK2Node_VariableGet* VarGet = NewObject<UK2Node_VariableGet>(Ctx.Graph);
		VarGet->VariableReference.SetSelfMember(FName(*MemberName));
		VarGet->NodePosX = Ctx.CurrentX; VarGet->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(VarGet, false, false); VarGet->AllocateDefaultPins(); IMP_EnsureGuid(VarGet);
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), VarGet);
		return IMP_FindOutputPin(VarGet, TEXT(""));
	}

	if (!FormName.IsEmpty())
	{
		if (UFunction* DirectFunc = IMP_FindFunctionForForm(FormName, Expr, Ctx))
		{
			UK2Node_CallFunction* CN = IMP_CreateOrReuseCallFunctionNode(Expr, DirectFunc, FormName, false, Ctx);
			IMP_ApplyCallInputs(CN, Expr, 1, true, Ctx);
			return IMP_GetDeclaredCallOutputPin(CN, Expr, Ctx);
		}
	}

	FString CompoundBindingName;

	int32 CompoundBindingValueIndex = INDEX_NONE;
	if (IMP_ExtractBindingNameAndValueIndex(Expr, 0, CompoundBindingName, CompoundBindingValueIndex))
	{
		if (UFunction* CompoundFunc = IMP_FindFunctionForForm(CompoundBindingName, Expr, Ctx))
		{
			UK2Node_CallFunction* CN = IMP_CreateOrReuseCallFunctionNode(Expr, CompoundFunc, CompoundBindingName, false, Ctx);
			IMP_ApplyCallInputs(CN, Expr, CompoundBindingValueIndex, true, Ctx);
			return IMP_GetDeclaredCallOutputPin(CN, Expr, Ctx);
		}


		if (Expr->Num() == 2 && CompoundBindingValueIndex == 1)
		{
			return IMP_ResolveLispExpr(Expr->Get(1), Ctx);
		}
	}

	const FString PrimaryFormName = !CompoundFormName.IsEmpty() ? CompoundFormName : FormName;
	const int32 PrimaryArgStartIndex = !CompoundFormName.IsEmpty() ? CompoundArgStartIndex : 1;
	if (!PrimaryFormName.IsEmpty())
	{
		if (UFunction* F = IMP_FindFunctionForForm(PrimaryFormName, Expr, Ctx))
		{
			UK2Node_CallFunction* CN = IMP_CreateOrReuseCallFunctionNode(Expr, F, PrimaryFormName, false, Ctx);
			IMP_ApplyCallInputs(CN, Expr, PrimaryArgStartIndex, true, Ctx);
			return IMP_GetDeclaredCallOutputPin(CN, Expr, Ctx);
		}


		// Try variable get with same name as form
		UK2Node_VariableGet* VarGet = NewObject<UK2Node_VariableGet>(Ctx.Graph);
		VarGet->VariableReference.SetSelfMember(FName(*PrimaryFormName));
		VarGet->NodePosX = Ctx.CurrentX; VarGet->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(VarGet, false, false); VarGet->AllocateDefaultPins(); IMP_EnsureGuid(VarGet);
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), VarGet);
		return IMP_FindOutputPin(VarGet, TEXT(""));
	}



	return nullptr;
}

static UEdGraphPin* IMP_ResolveLispExpr(const FLispNodePtr& Expr, FBPImportContext& Ctx)
{
	if (UEdGraphPin* StableOutputPin = IMP_FindImportedStableOutputPin(Expr, nullptr, Ctx))
	{
		return StableOutputPin;
	}
	UEdGraphPin* ResolvedPin = IMP_ResolveLispExprInternal(Expr, Ctx);
	if (ResolvedPin)
	{
		IMP_ApplyRequestedStableId(ResolvedPin->GetOwningNode(), Expr, false);
		IMP_RegisterImportedStableNode(ResolvedPin->GetOwningNode(), Expr, Ctx);
		Ctx.TempIdToNode.FindOrAdd(ResolvedPin->GetOwningNode()->NodeGuid.ToString()) = ResolvedPin->GetOwningNode();
	}
	return ResolvedPin;
}

// --- Convert a single executable form → K2Node ---
static UEdGraphNode* IMP_ConvertFormToNodeStable(const FLispNodePtr& Form, FBPImportContext& Ctx, UEdGraphPin*& OutExecPin)
{
	UEdGraphNode* Node = IMP_ConvertFormToNode(Form, Ctx, OutExecPin);
	if (Node)
	{
		IMP_ApplyRequestedStableId(Node, Form, false);
		Ctx.TempIdToNode.FindOrAdd(Node->NodeGuid.ToString()) = Node;
	}
	return Node;
}

static UEdGraphNode* IMP_ConvertFormToNode(const FLispNodePtr& Form, FBPImportContext& Ctx, UEdGraphPin*& OutExecPin)
{
	OutExecPin = nullptr;
	if (!Form.IsValid() || !Form->IsList() || Form->Num() == 0) return nullptr;

	FString FormName = Form->GetFormName();

	// (seq s1 s2 ...) — execute in order; if it carries :id, treat it as an actual UK2Node_ExecutionSequence
	if (FormName.Equals(TEXT("seq"), ESearchCase::IgnoreCase))
	{
		TArray<FLispNodePtr> SequenceBodies;
		for (int32 i = 1; i < Form->Num(); ++i)
		{
			const FLispNodePtr Part = Form->Get(i);
			if (!Part.IsValid())
			{
				continue;
			}
			if (Part->IsKeyword())
			{
				i += 1;
				continue;
			}
			SequenceBodies.Add(Part);
		}

		if (!IMP_GetRequestedNodeStableId(Form).IsEmpty())
		{
			UK2Node_ExecutionSequence* SequenceNode = IMP_CreateOrReuseSequenceNode(Form, SequenceBodies.Num(), Ctx);
			for (int32 BranchIndex = 0; BranchIndex < SequenceBodies.Num(); ++BranchIndex)
			{
				if (UEdGraphPin* BranchExecPin = SequenceNode->GetThenPinGivenIndex(BranchIndex))
				{
					UEdGraphPin* MutableBranchExecPin = BranchExecPin;
					IMP_ConvertExecBody(SequenceBodies[BranchIndex], Ctx, MutableBranchExecPin);
				}
			}

			OutExecPin = nullptr;
			return SequenceNode;
		}

		UEdGraphNode* First = nullptr;
		UEdGraphPin* CurExec = nullptr;
		for (const FLispNodePtr& BodyPart : SequenceBodies)
		{
			UEdGraphPin* StmtOut = nullptr;
			UEdGraphNode* SN = IMP_ConvertFormToNodeStable(BodyPart, Ctx, StmtOut);
			if (SN)
			{
				if (!First) First = SN;
				if (CurExec) if (UEdGraphPin* In = IMP_GetExecInput(SN)) IMP_Connect(CurExec, In, Ctx);
				IMP_UpdateCurrentExecPin(SN, StmtOut, CurExec);
			}
		}

		OutExecPin = CurExec;
		return First;
	}

	if (FormName.Equals(TEXT("create-object"), ESearchCase::IgnoreCase))
	{
		const FString ClassPath = IMP_GetAtomName(Form->GetKeywordArg(TEXT(":class")));
		UClass* TargetClass = IMP_FindClassByName(ClassPath, Ctx);
		if (!TargetClass)
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: create-object class not found: %s"), *ClassPath));
			return nullptr;
		}

		UK2Node_GenericCreateObject* CreateObjectNode = IMP_CreateOrReuseGenericCreateObjectNode(Form, TargetClass, Ctx);
		if (!CreateObjectNode) return nullptr;

		const FLispNodePtr OuterExpr = Form->GetKeywordArg(TEXT(":outer"));
		if (OuterExpr.IsValid() && !OuterExpr->IsNil())
		{
			IMP_SetPinFromExpr(CreateObjectNode->GetOuterPin(), OuterExpr, Ctx);
		}

		for (int32 Index = 1; Index + 1 < Form->Num(); ++Index)
		{
			const FLispNodePtr Keyword = Form->Get(Index);
			if (!Keyword.IsValid() || !Keyword->IsKeyword()) continue;
			if (Keyword->StringValue.Equals(TEXT(":input"), ESearchCase::IgnoreCase))
			{
				const FLispNodePtr InputSpec = Form->Get(++Index);
				if (!InputSpec.IsValid() || !InputSpec->IsList() || InputSpec->Num() < 3) continue;
				const FString PinName = IMP_GetAtomName(InputSpec->Get(0));
				if (UEdGraphPin* InputPin = CreateObjectNode->FindPin(FName(*PinName), EGPD_Input))
				{
					IMP_SetPinFromExpr(InputPin, InputSpec->Get(2), Ctx);
				}
				else
				{
					Ctx.Errors.Add(FString::Printf(TEXT("IMP: create-object input pin not found: %s"), *PinName));
				}
				continue;
			}
			if (Keyword->StringValue.Equals(TEXT(":out"), ESearchCase::IgnoreCase))
			{
				const FLispNodePtr OutputSpec = Form->Get(++Index);
				if (OutputSpec.IsValid() && OutputSpec->IsList() && OutputSpec->Num() >= 1)
				{
					IMP_RegisterBoundValue(IMP_GetAtomName(OutputSpec->Get(0)), CreateObjectNode->GetResultPin(), Ctx);
				}
				continue;
			}
			Index++;
		}
		if (UEdGraphPin* ResultPin = CreateObjectNode->GetResultPin())
		{
			IMP_RegisterBoundValue(ResultPin->PinName.ToString(), ResultPin, Ctx);
		}
		OutExecPin = CreateObjectNode->GetThenPin();
		return CreateObjectNode;
	}

	if (FormName.Equals(TEXT("evaluate-chooser"), ESearchCase::IgnoreCase))
	{
		UClass* ChooserNodeClass = LoadObject<UClass>(nullptr, TEXT("/Script/ChooserUncooked.K2Node_EvaluateChooser2"));
		if (!ChooserNodeClass || !ChooserNodeClass->IsChildOf(UK2Node::StaticClass()))
		{
			Ctx.Errors.Add(TEXT("IMP: K2Node_EvaluateChooser2 class is unavailable; ensure ChooserUncooked is loaded"));
			return nullptr;
		}

		UK2Node* ChooserNode = NewObject<UK2Node>(Ctx.Graph, ChooserNodeClass);
		ChooserNode->NodePosX = Ctx.CurrentX;
		ChooserNode->NodePosY = Ctx.CurrentY;
		ChooserNode->CreateNewGuid();
		Ctx.Graph->AddNode(ChooserNode, false, false);
		ChooserNode->PostPlacedNewNode();

		const FLispNodePtr ChooserAssetForm = Form->GetKeywordArg(TEXT(":chooser"));
		UObject* ChooserAsset = nullptr;
		if (ChooserAssetForm.IsValid() && ChooserAssetForm->IsForm(TEXT("asset")) && ChooserAssetForm->Num() >= 2)
		{
			const FString AssetPath = IMP_GetAtomName(ChooserAssetForm->Get(1));
			ChooserAsset = LoadObject<UObject>(nullptr, *AssetPath);
			if (!ChooserAsset)
			{
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: evaluate-chooser asset could not be loaded: %s"), *AssetPath));
				Ctx.Graph->RemoveNode(ChooserNode);
				return nullptr;
			}
		}
		else
		{
			Ctx.Errors.Add(TEXT("IMP: evaluate-chooser is missing :chooser (asset ...)"));
			Ctx.Graph->RemoveNode(ChooserNode);
			return nullptr;
		}

		if (FObjectPropertyBase* ChooserProperty = FindFProperty<FObjectPropertyBase>(ChooserNodeClass, TEXT("Chooser")))
		{
			ChooserProperty->SetObjectPropertyValue_InContainer(ChooserNode, ChooserAsset);
		}
		else
		{
			Ctx.Errors.Add(TEXT("IMP: evaluate-chooser node has no Chooser property"));
			Ctx.Graph->RemoveNode(ChooserNode);
			return nullptr;
		}

		auto ImportReflectedProperty = [Form, ChooserNode, ChooserNodeClass, &Ctx](const TCHAR* Keyword, const TCHAR* PropertyName)
		{
			const FLispNodePtr ValueNode = Form->GetKeywordArg(Keyword);
			if (!ValueNode.IsValid() || ValueNode->IsNil()) return;
			FProperty* Property = ChooserNodeClass->FindPropertyByName(PropertyName);
			if (!Property) return;
			const FString Value = IMP_GetAtomName(ValueNode);
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(ChooserNode);
			if (!Property->ImportText_Direct(*Value, ValuePtr, ChooserNode, PPF_None))
			{
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: evaluate-chooser property %s rejected value '%s'"), PropertyName, *Value));
			}
		};
		ImportReflectedProperty(TEXT(":mode"), TEXT("Mode"));
		ImportReflectedProperty(TEXT(":struct-output-mode"), TEXT("StructOutputMode"));
		ImportReflectedProperty(TEXT(":return-soft-object"), TEXT("bReturnSoftObjectReference"));
		ChooserNode->AllocateDefaultPins();

		for (int32 Index = 1; Index + 1 < Form->Num(); ++Index)
		{
			const FLispNodePtr Keyword = Form->Get(Index);
			if (!Keyword.IsValid() || !Keyword->IsKeyword()) continue;
			if (!Keyword->StringValue.Equals(TEXT(":input"), ESearchCase::IgnoreCase))
			{
				Index++;
				continue;
			}
			const FLispNodePtr InputSpec = Form->Get(++Index);
			if (!InputSpec.IsValid() || !InputSpec->IsList() || InputSpec->Num() < 3) continue;
			const FString PinName = IMP_GetAtomName(InputSpec->Get(0));
			if (UEdGraphPin* InputPin = ChooserNode->FindPin(FName(*PinName), EGPD_Input))
			{
				const FLispNodePtr InputValue = InputSpec->Get(2);
				if (InputValue.IsValid() && InputValue->IsSymbol()
					&& InputValue->StringValue.Equals(TEXT("self"), ESearchCase::IgnoreCase)
					&& InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object
					&& Ctx.Blueprint && Ctx.Blueprint->GeneratedClass)
				{
					InputPin->PinType.PinSubCategoryObject = Ctx.Blueprint->GeneratedClass;
				}
				IMP_SetPinFromExpr(InputPin, InputValue, Ctx);
			}
			else
			{
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: evaluate-chooser input pin not found: %s"), *PinName));
			}
		}

		IMP_EnsureGuid(ChooserNode);
		Ctx.TempIdToNode.FindOrAdd(ChooserNode->NodeGuid.ToString()) = ChooserNode;
		for (UEdGraphPin* Pin : ChooserNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && !Pin->bHidden)
			{
				IMP_RegisterBoundValue(Pin->PinName.ToString(), Pin, Ctx);
			}
		}
		OutExecPin = IMP_GetExecOutput(ChooserNode);
		Ctx.AdvancePosition();
		return ChooserNode;
	}


	// (branch cond :true body :false body)
	if (FormName.Equals(TEXT("branch"), ESearchCase::IgnoreCase))
	{
		UK2Node_IfThenElse* BN = IMP_CreateOrReuseBranchNode(Form, Ctx);
		if (Form->Num() > 1)

		{
			UEdGraphPin* CondPin = BN->GetConditionPin();
			if (!CondPin || !IMP_SetPinFromExpr(CondPin, Form->Get(1), Ctx))
			{
				Ctx.Errors.Add(TEXT("IMP: branch condition could not be resolved to an output pin"));
			}
		}

		FLispNodePtr TrueBody  = Form->GetKeywordArg(TEXT(":true"));
		FLispNodePtr FalseBody = Form->GetKeywordArg(TEXT(":false"));
		if (TrueBody.IsValid() && !TrueBody->IsNil())
		{
			UEdGraphPin* ThenPin = BN->GetThenPin();
			IMP_ConvertExecBody(TrueBody, Ctx, ThenPin);
		}
		if (FalseBody.IsValid() && !FalseBody->IsNil())
		{
			UEdGraphPin* ElsePin = BN->GetElsePin();
			IMP_ConvertExecBody(FalseBody, Ctx, ElsePin);
		}
		OutExecPin = nullptr;
		return BN;
	}

	// (cast TypeName ObjExpr [SuccessBody] [:fail FailBody])
	if (FormName.Equals(TEXT("cast"), ESearchCase::IgnoreCase) && Form->Num() >= 3)
	{
		const FString TypeName = Form->Get(1)->StringValue;
		UClass* TargetClass = IMP_FindClassByName(TypeName, Ctx);
		if (!TargetClass)
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: cast target class not found: %s"), *TypeName));

			return nullptr;
		}

		UK2Node_DynamicCast* CastNode = IMP_CreateOrReuseDynamicCastNode(Form, TargetClass, Ctx);

		if (UEdGraphPin* CastResultPin = CastNode->GetCastResultPin())

		{
			IMP_RegisterBoundValue(TEXT("K2Node_DynamicCast"), CastResultPin, Ctx);
			IMP_RegisterBoundValue(CastResultPin->PinName.ToString(), CastResultPin, Ctx);
		}

		if (UEdGraphPin* SourcePin = CastNode->GetCastSourcePin())
		{
			IMP_SetPinFromExpr(SourcePin, Form->Get(2), Ctx);
		}


		FLispNodePtr SuccessBody = (Form->Num() >= 4 && !Form->Get(3)->IsKeyword()) ? Form->Get(3) : FLispNode::MakeNil();
		FLispNodePtr FailBody = Form->GetKeywordArg(TEXT(":fail"));
		if (SuccessBody.IsValid() && !SuccessBody->IsNil())
		{
			UEdGraphPin* SuccessPin = CastNode->GetValidCastPin();
			IMP_ConvertExecBody(SuccessBody, Ctx, SuccessPin);
		}
		if (FailBody.IsValid() && !FailBody->IsNil())
		{
			UEdGraphPin* FailPin = CastNode->GetInvalidCastPin();
			IMP_ConvertExecBody(FailBody, Ctx, FailPin);
		}

		OutExecPin = nullptr;
		return CastNode;
	}

	// (switch-int Selection :0 Body :1 Body ... [:default Body])
	if (FormName.Equals(TEXT("switch-int"), ESearchCase::IgnoreCase) && Form->Num() >= 2)
	{
		TArray<TPair<int32, FLispNodePtr>> CaseBodies;
		FLispNodePtr DefaultBody = FLispNode::MakeNil();
		for (int32 i = 2; i < Form->Num(); ++i)
		{
			if (!Form->Get(i)->IsKeyword()) continue;
			const FString Keyword = Form->Get(i)->StringValue;
			if (Keyword.Equals(TEXT(":default"), ESearchCase::IgnoreCase))
			{
				DefaultBody = (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil();
				i += 1;
				continue;
			}
			if (Keyword.Equals(TEXT(":id"), ESearchCase::IgnoreCase))
			{
				i += 1;
				continue;
			}

			const FString CaseName = Keyword.StartsWith(TEXT(":")) ? Keyword.Mid(1) : Keyword;
			CaseBodies.Emplace(FCString::Atoi(*CaseName), (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil());
			i += 1;
		}
		CaseBodies.Sort([](const TPair<int32, FLispNodePtr>& A, const TPair<int32, FLispNodePtr>& B) { return A.Key < B.Key; });

		const bool bHasDefaultBody = DefaultBody.IsValid() && !DefaultBody->IsNil();
		UK2Node_SwitchInteger* SwitchNode = IMP_CreateOrReuseSwitchIntegerNode(Form, CaseBodies, bHasDefaultBody, Ctx);

		for (int32 CaseIdx = 1; CaseIdx < CaseBodies.Num(); ++CaseIdx)

		{
			if (CaseBodies[CaseIdx].Key != CaseBodies[0].Key + CaseIdx)
			{
				Ctx.Errors.Add(TEXT("IMP: switch-int currently expects contiguous case labels; sparse switch is unsupported"));
				return SwitchNode;

			}
		}

		if (UEdGraphPin* SelectionPin = SwitchNode->GetSelectionPin())
		{
			IMP_SetPinFromExpr(SelectionPin, Form->Get(1), Ctx);
		}
		for (const TPair<int32, FLispNodePtr>& CaseBody : CaseBodies)
		{
			if (UEdGraphPin* CasePin = SwitchNode->FindPin(FName(*FString::FromInt(CaseBody.Key)), EGPD_Output))
			{
				UEdGraphPin* MutableCasePin = CasePin;
				IMP_ConvertExecBody(CaseBody.Value, Ctx, MutableCasePin);
			}
		}
		if (DefaultBody.IsValid() && !DefaultBody->IsNil())
		{
			if (UEdGraphPin* DefaultPin = SwitchNode->GetDefaultPin())
			{
				UEdGraphPin* MutableDefaultPin = DefaultPin;
				IMP_ConvertExecBody(DefaultBody, Ctx, MutableDefaultPin);
			}
		}

		OutExecPin = nullptr;
		return SwitchNode;
	}

	// (switch-string Selection [:case-sensitive true] [:case ("Value" Body)]... [:Literal Body] [:default Body])
	if (FormName.Equals(TEXT("switch-string"), ESearchCase::IgnoreCase) && Form->Num() >= 2)
	{
		TArray<TPair<FString, FLispNodePtr>> CaseBodies;
		FLispNodePtr DefaultBody = FLispNode::MakeNil();
		bool bCaseSensitive = false;
		for (int32 i = 2; i < Form->Num(); ++i)
		{
			if (!Form->Get(i)->IsKeyword()) continue;
			const FString Keyword = Form->Get(i)->StringValue;
			if (Keyword.Equals(TEXT(":case-sensitive"), ESearchCase::IgnoreCase))
			{
				bCaseSensitive = (i + 1 < Form->Num()) ? IMP_IsTruthy(Form->Get(i + 1)) : false;
				i += 1;
				continue;
			}
			if (Keyword.Equals(TEXT(":default"), ESearchCase::IgnoreCase))
			{
				DefaultBody = (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil();
				i += 1;
				continue;
			}
			if (Keyword.Equals(TEXT(":case"), ESearchCase::IgnoreCase))
			{
				const FLispNodePtr CasePair = (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil();
				if (CasePair.IsValid() && CasePair->IsList() && CasePair->Num() >= 2)
				{
					CaseBodies.Emplace(CasePair->Get(0)->StringValue, CasePair->Get(1));
				}
				i += 1;
				continue;
			}

			const FString CaseLabel = Keyword.StartsWith(TEXT(":")) ? Keyword.Mid(1) : Keyword;
			CaseBodies.Emplace(CaseLabel, (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil());
			i += 1;
		}

		UK2Node_SwitchString* SwitchNode = NewObject<UK2Node_SwitchString>(Ctx.Graph);
		SwitchNode->bIsCaseSensitive = bCaseSensitive;
		SwitchNode->FunctionName = bCaseSensitive ? TEXT("NotEqual_StrStr") : TEXT("NotEqual_StriStri");
		SwitchNode->FunctionClass = UKismetStringLibrary::StaticClass();
		SwitchNode->bHasDefaultPin = DefaultBody.IsValid() && !DefaultBody->IsNil();
		SwitchNode->PinNames.Reset();
		for (const TPair<FString, FLispNodePtr>& CaseBody : CaseBodies)
		{
			SwitchNode->PinNames.Add(FName(*CaseBody.Key));
		}
		SwitchNode->NodePosX = Ctx.CurrentX;
		SwitchNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(SwitchNode, false, false);
		SwitchNode->AllocateDefaultPins();
		IMP_EnsureGuid(SwitchNode);
		Ctx.AdvancePosition();
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), SwitchNode);

		if (UEdGraphPin* SelectionPin = SwitchNode->GetSelectionPin())
		{
			IMP_SetPinFromExpr(SelectionPin, Form->Get(1), Ctx);
		}
		for (const TPair<FString, FLispNodePtr>& CaseBody : CaseBodies)
		{
			if (UEdGraphPin* CasePin = SwitchNode->FindPin(FName(*CaseBody.Key), EGPD_Output))
			{
				UEdGraphPin* MutableCasePin = CasePin;
				IMP_ConvertExecBody(CaseBody.Value, Ctx, MutableCasePin);
			}
		}
		if (DefaultBody.IsValid() && !DefaultBody->IsNil())
		{
			if (UEdGraphPin* DefaultPin = SwitchNode->GetDefaultPin())
			{
				UEdGraphPin* MutableDefaultPin = DefaultPin;
				IMP_ConvertExecBody(DefaultBody, Ctx, MutableDefaultPin);
			}
		}

		OutExecPin = nullptr;
		return SwitchNode;
	}

	// (switch-enum EnumType Selection :Value Body ... [:default Body])
	if (FormName.Equals(TEXT("switch-enum"), ESearchCase::IgnoreCase) && Form->Num() >= 3)
	{
		const FString EnumName = Form->Get(1)->StringValue;
		UEnum* TargetEnum = IMP_FindEnumByName(EnumName);
		if (!TargetEnum)
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: enum not found for switch-enum: %s"), *EnumName));

			return nullptr;
		}

		TArray<TPair<FString, FLispNodePtr>> CaseBodies;
		FLispNodePtr DefaultBody = FLispNode::MakeNil();
		for (int32 i = 3; i < Form->Num(); ++i)
		{
			if (!Form->Get(i)->IsKeyword()) continue;
			const FString Keyword = Form->Get(i)->StringValue;
			if (Keyword.Equals(TEXT(":id"), ESearchCase::IgnoreCase) || Keyword.Equals(TEXT(":pos"), ESearchCase::IgnoreCase))
			{
				i += 1;
				continue;
			}
			if (Keyword.Equals(TEXT(":default"), ESearchCase::IgnoreCase))
			{
				DefaultBody = (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil();
				i += 1;
				continue;
			}
			if (Keyword.Equals(TEXT(":case"), ESearchCase::IgnoreCase))
			{
				const FLispNodePtr CasePair = (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil();
				if (CasePair.IsValid() && CasePair->IsList() && CasePair->Num() >= 2)
				{
					CaseBodies.Emplace(CasePair->Get(0)->StringValue, CasePair->Get(1));
				}
				i += 1;
				continue;
			}

			const FString CaseLabel = Keyword.StartsWith(TEXT(":")) ? Keyword.Mid(1) : Keyword;
			CaseBodies.Emplace(CaseLabel, (i + 1 < Form->Num()) ? Form->Get(i + 1) : FLispNode::MakeNil());
			i += 1;
		}

		UK2Node_SwitchEnum* SwitchNode = NewObject<UK2Node_SwitchEnum>(Ctx.Graph);
		if (FObjectProperty* EnumProperty = FindFProperty<FObjectProperty>(SwitchNode->GetClass(), TEXT("Enum")))
		{
			EnumProperty->SetObjectPropertyValue_InContainer(SwitchNode, TargetEnum);
		}
		SwitchNode->bHasDefaultPin = DefaultBody.IsValid() && !DefaultBody->IsNil();
		SwitchNode->NodePosX = Ctx.CurrentX;
		SwitchNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(SwitchNode, false, false);
		SwitchNode->AllocateDefaultPins();
		IMP_EnsureGuid(SwitchNode);
		Ctx.AdvancePosition();
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), SwitchNode);

		if (UEdGraphPin* SelectionPin = SwitchNode->GetSelectionPin())
		{
			IMP_SetPinFromExpr(SelectionPin, Form->Get(2), Ctx);
		}
		TArray<UEdGraphPin*> OrderedCasePins;
		for (UEdGraphPin* Pin : SwitchNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
				&& Pin != SwitchNode->GetDefaultPin())
			{
				OrderedCasePins.Add(Pin);
			}
		}
		for (int32 CaseIndex = 0; CaseIndex < CaseBodies.Num(); ++CaseIndex)
		{
			const TPair<FString, FLispNodePtr>& CaseBody = CaseBodies[CaseIndex];
			FString CaseLabel = CaseBody.Key;
			if (CaseLabel.Contains(TEXT("::")))
			{
				CaseLabel = CaseLabel.RightChop(CaseLabel.Find(TEXT("::")) + 2);
			}
			UEdGraphPin* CasePin = SwitchNode->FindPin(FName(*CaseLabel), EGPD_Output);
			if (!CasePin && OrderedCasePins.IsValidIndex(CaseIndex))
			{
				CasePin = OrderedCasePins[CaseIndex];
			}
			if (CasePin)
			{
				UEdGraphPin* MutableCasePin = CasePin;
				IMP_ConvertExecBody(CaseBody.Value, Ctx, MutableCasePin);
			}
			else
			{
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: switch-enum case pin not found: %s.%s"), *EnumName, *CaseLabel));

			}
		}
		if (DefaultBody.IsValid() && !DefaultBody->IsNil())
		{
			if (UEdGraphPin* DefaultPin = SwitchNode->GetDefaultPin())
			{
				UEdGraphPin* MutableDefaultPin = DefaultPin;
				IMP_ConvertExecBody(DefaultBody, Ctx, MutableDefaultPin);
			}
		}

		OutExecPin = nullptr;
		return SwitchNode;
	}

	if (FormName.Equals(TEXT("set-struct-fields"), ESearchCase::IgnoreCase))
	{
		const FString StructName = IMP_GetKeywordAtomValue(Form, TEXT(":struct"));
		UScriptStruct* StructType = IMP_FindStructByName(StructName, Ctx);
		const FLispNodePtr TargetExpr = Form->GetKeywordArg(TEXT(":target"));
		struct FFieldAssignment { FString Name; FLispNodePtr Value; };
		TArray<FFieldAssignment> FieldAssignments;
		for (int32 Index = 1; Index + 1 < Form->Num(); ++Index)
		{
			const FLispNodePtr KeywordNode = Form->Get(Index);
			if (!KeywordNode.IsValid() || !KeywordNode->IsKeyword()
				|| !KeywordNode->StringValue.Equals(TEXT(":field"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FLispNodePtr FieldSpec = Form->Get(++Index);
			if (FieldSpec.IsValid() && FieldSpec->IsList() && FieldSpec->Num() >= 3)
			{
				FieldAssignments.Add({ IMP_GetAtomName(FieldSpec->Get(0)), FieldSpec->Get(2) });
			}
		}
		if (!StructType || !TargetExpr.IsValid() || TargetExpr->IsNil())
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: invalid set-struct-fields declaration for '%s'"), *StructName));
			return nullptr;
		}

		UK2Node_SetFieldsInStruct* SetFieldsNode = NewObject<UK2Node_SetFieldsInStruct>(Ctx.Graph);
		SetFieldsNode->StructType = StructType;
		SetFieldsNode->NodePosX = Ctx.CurrentX;
		SetFieldsNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(SetFieldsNode, false, false);
		SetFieldsNode->AllocateDefaultPins();
		for (FOptionalPinFromProperty& OptionalPin : SetFieldsNode->ShowPinForProperties)
		{
			OptionalPin.bShowPin = FieldAssignments.ContainsByPredicate(
				[&OptionalPin](const FFieldAssignment& Assignment)
				{
					return IMP_NormalizePinLookupName(Assignment.Name)
						== IMP_NormalizePinLookupName(OptionalPin.PropertyName.ToString());
				});
		}
		SetFieldsNode->ReconstructNode();
		IMP_EnsureGuid(SetFieldsNode);
		Ctx.AdvancePosition();
		if (UEdGraphPin* StructRefPin = SetFieldsNode->FindPin(TEXT("StructRef"), EGPD_Input))
		{
			IMP_SetPinFromExpr(StructRefPin, TargetExpr, Ctx);
		}
		for (const FFieldAssignment& Assignment : FieldAssignments)
		{
			if (UEdGraphPin* FieldPin = IMP_FindInputPin(SetFieldsNode, Assignment.Name))
			{
				IMP_SetPinFromExpr(FieldPin, Assignment.Value, Ctx);
			}
			else
			{
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: set-struct-fields pin not found: %s.%s"), *StructName, *Assignment.Name));
			}
		}
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), SetFieldsNode);
		Ctx.TempIdToNode.Add(SetFieldsNode->NodeGuid.ToString(), SetFieldsNode);
		OutExecPin = IMP_GetExecOutput(SetFieldsNode);
		return SetFieldsNode;
	}

	// (set-local var type val)
	if (FormName.Equals(TEXT("set-local"), ESearchCase::IgnoreCase) && Form->Num() >= 4)
	{
		const FString VarName = IMP_GetAtomName(Form->Get(1));
		const FString TypeName = IMP_GetAtomName(Form->Get(2));
		FEdGraphPinType VarType;
		if (VarName.IsEmpty() || !IMP_BuildPinTypeFromLispType(TypeName, VarType, Ctx))
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: invalid set-local declaration: %s %s"), *VarName, *TypeName));
			return nullptr;
		}

		UK2Node_FunctionEntry* FunctionEntry = nullptr;
		for (UEdGraphNode* GraphNode : Ctx.Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(GraphNode))
			{
				FunctionEntry = Candidate;
				break;
			}
		}
		if (!FunctionEntry)
		{
			Ctx.Errors.Add(TEXT("IMP: set-local requires a function entry node"));
			return nullptr;
		}

		FBPVariableDescription* LocalDescription = FunctionEntry->LocalVariables.FindByPredicate(
			[&VarName](const FBPVariableDescription& Description) { return Description.VarName == FName(*VarName); });
		if (!LocalDescription)
		{
			if (!FBlueprintEditorUtils::AddLocalVariable(Ctx.Blueprint, Ctx.Graph, FName(*VarName), VarType))
			{
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: could not add local variable: %s"), *VarName));
				return nullptr;
			}
			LocalDescription = FunctionEntry->LocalVariables.FindByPredicate(
				[&VarName](const FBPVariableDescription& Description) { return Description.VarName == FName(*VarName); });
			if (!LocalDescription) return nullptr;
		}

		UK2Node_VariableSet* LocalSetNode = NewObject<UK2Node_VariableSet>(Ctx.Graph);
		LocalSetNode->VariableReference.SetLocalMember(FName(*VarName), Ctx.Graph->GetName(), LocalDescription->VarGuid);
		LocalSetNode->NodePosX = Ctx.CurrentX;
		LocalSetNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(LocalSetNode, false, false);
		LocalSetNode->AllocateDefaultPins();
		IMP_EnsureGuid(LocalSetNode);
		Ctx.AdvancePosition();
		UEdGraphPin* ValuePin = IMP_FindInputPin(LocalSetNode, VarName);
		if (!ValuePin)
		{
			if (!LocalSetNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
			{
				LocalSetNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
			}
			if (!LocalSetNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output))
			{
				LocalSetNode->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
			}
			ValuePin = LocalSetNode->CreatePin(EGPD_Input, VarType, FName(*VarName));
			LocalSetNode->CreatePin(EGPD_Output, VarType, FName(*VarName));
		}
		if (!ValuePin || !IMP_SetPinFromExpr(ValuePin, Form->Get(3), Ctx))
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: set-local value could not be reconstructed: %s"), *VarName));
			return nullptr;
		}
		if (UEdGraphPin* OutputPin = IMP_FindOutputPinByName(LocalSetNode, VarName))
		{
			IMP_RegisterBoundValue(VarName, OutputPin, Ctx);
		}
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), LocalSetNode);
		Ctx.TempIdToNode.Add(LocalSetNode->NodeGuid.ToString(), LocalSetNode);
		OutExecPin = IMP_GetExecOutput(LocalSetNode);
		return LocalSetNode;
	}

	// (set var val)
	if (FormName.Equals(TEXT("set"), ESearchCase::IgnoreCase) && Form->Num() >= 3)
	{
		int32 ValueIndex = INDEX_NONE;
		FString VarName;
		if (!IMP_ExtractBindingNameAndValueIndex(Form, 1, VarName, ValueIndex))
		{
			Ctx.Errors.Add(TEXT("IMP: set missing variable name or value"));
			return nullptr;
		}
		UK2Node_VariableSet* SN = IMP_CreateOrReuseVariableSetNode(Form, VarName, Ctx);
		if (!SN)
		{
			return nullptr;
		}
		if (const FLispNodePtr SelfExpression = Form->GetKeywordArg(TEXT(":self")); SelfExpression.IsValid())
		{
			UEdGraphPin* SelfPin = SN->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
			if (!SelfPin || !IMP_SetPinFromExpr(SelfPin, SelfExpression, Ctx))
			{
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: set target object could not be reconstructed: %s"), *VarName));
				return nullptr;
			}
		}
		for (UEdGraphPin* P : SN->Pins)

			if (P->Direction == EGPD_Input && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
				&& P->PinName != UEdGraphSchema_K2::PN_Self)
			{ IMP_SetPinFromExpr(P, Form->Get(ValueIndex), Ctx); break; }
		for (UEdGraphPin* P : SN->Pins)
			if (P->Direction == EGPD_Output && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && !P->bHidden)
			{ IMP_RegisterBoundValue(VarName, P, Ctx); break; }
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), SN);
		OutExecPin = IMP_GetExecOutput(SN);
		return SN;

	}

	// (let var expr) — bind variable; keep exec-producing expr in the chain
	if (FormName.Equals(TEXT("let"), ESearchCase::IgnoreCase) && Form->Num() >= 3)
	{
		int32 ExprIndex = INDEX_NONE;
		FString VarName;
		if (!IMP_ExtractBindingNameAndValueIndex(Form, 1, VarName, ExprIndex))
		{
			Ctx.Errors.Add(TEXT("IMP: let missing variable name or value"));
			return nullptr;
		}
		FLispNodePtr ExprNode = Form->Get(ExprIndex);


		if (ExprNode->IsNumber())
		{
			Ctx.VariableToNodeId.Add(VarName, TEXT("_literal_") + VarName);
			Ctx.VariableToPin.Add(VarName, FString::SanitizeFloat(ExprNode->NumberValue));
			return nullptr;
		}
		if (ExprNode->IsString())
		{
			Ctx.VariableToNodeId.Add(VarName, TEXT("_literalstr_") + VarName);
			Ctx.VariableToPin.Add(VarName, ExprNode->StringValue);
			return nullptr;
		}

		UEdGraphNode* BoundNode = nullptr;
		UEdGraphPin* BoundPin = nullptr;
		UEdGraphPin* BoundExecOut = nullptr;
		const bool bCallLikeExpr = ExprNode.IsValid() && ExprNode->IsList() && ExprNode->Num() > 0
			&& (ExprNode->IsForm(TEXT("call"))
				|| ExprNode->IsForm(TEXT("call-macro"))
				|| IMP_FindFunction(ExprNode->GetFormName(), Ctx) != nullptr);

		if (bCallLikeExpr)
		{
			BoundNode = IMP_ConvertFormToNodeStable(ExprNode, Ctx, BoundExecOut);
			BoundPin = IMP_FindOutputPinByName(BoundNode, VarName);
			if (!BoundPin)
			{
				BoundPin = IMP_FindOutputPin(BoundNode, TEXT(""));
			}
		}


		if (!BoundPin)
		{
			BoundPin = IMP_ResolveLispExpr(ExprNode, Ctx);
			if (BoundPin)
			{
				BoundNode = BoundPin->GetOwningNode();
				if (!BoundExecOut && BoundNode)
				{
					BoundExecOut = IMP_GetExecOutput(BoundNode);
				}
			}
		}

		if (BoundPin)
		{
			IMP_RegisterBoundValue(VarName, BoundPin, Ctx);
			if (BoundNode)
			{
				OutExecPin = BoundExecOut;
				return BoundNode;
			}
			return nullptr;
		}

		Ctx.Errors.Add(FString::Printf(TEXT("IMP: let binding could not resolve value for '%s'"), *VarName));
		return nullptr;

	}


	// (call-macro Name [:input value]... [:out (Pin Type)]...)
	if (FormName.Equals(TEXT("call-macro"), ESearchCase::IgnoreCase) && Form->Num() >= 2)
	{
		UEdGraphPin* PreferredOutputPin = nullptr;
		if (UK2Node_MacroInstance* MacroNode = IMP_CreateMacroInstanceNode(Form, Ctx, PreferredOutputPin))
		{
			OutExecPin = IMP_GetExecOutput(MacroNode);
			return MacroNode;
		}
		return nullptr;
	}


	// (exit Name [:output (Pin Expr)]...)
	if (FormName.Equals(TEXT("exit"), ESearchCase::IgnoreCase) && Form->Num() >= 2)
	{
		int32 OutputStartIndex = 2;
		const FString ExitName = IMP_ExtractCompoundName(Form, 1, OutputStartIndex);
		UK2Node_Tunnel* ExitTunnel = IMP_FindReusableMacroExitTunnel(Form, Ctx.Graph, ExitName);

		if (!ExitTunnel)
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: macro exit tunnel not found: %s"), *ExitName));

			return nullptr;
		}

		if (IMP_IsPendingReusableBodyNode(ExitTunnel, Ctx))
		{
			IMP_MarkReusableBodyNodeConsumed(ExitTunnel, Ctx);
			Ctx.AdvancePosition();
		}

		for (int32 i = OutputStartIndex; i < Form->Num(); ++i)

		{
			if (!Form->Get(i)->IsKeyword())
			{
				continue;
			}
			const FString Keyword = Form->Get(i)->StringValue;
			if (!Keyword.Equals(TEXT(":output"), ESearchCase::IgnoreCase))
			{
				i += 1;
				continue;
			}
			if (i + 1 >= Form->Num())
			{
				break;
			}

			const FLispNodePtr OutputPair = Form->Get(i + 1);
			if (OutputPair.IsValid() && OutputPair->IsList() && OutputPair->Num() >= 2)
			{
				int32 OutputValueIndex = 1;
				FString OutputName;
				if (OutputPair->Get(0)->IsString())
				{
					OutputName = OutputPair->Get(0)->StringValue;
					OutputValueIndex = 1;
				}
				else if (OutputPair->Num() > 2)
				{
					TArray<FString> OutputNameParts;
					for (int32 PartIndex = 0; PartIndex < OutputPair->Num() - 1; ++PartIndex)
					{
						if (OutputPair->Get(PartIndex).IsValid())
						{
							OutputNameParts.Add(OutputPair->Get(PartIndex)->StringValue);
						}
					}
					OutputName = FString::Join(OutputNameParts, TEXT(" "));
					OutputValueIndex = OutputPair->Num() - 1;
				}
				else
				{
					OutputName = OutputPair->Get(0)->StringValue;
					OutputValueIndex = 1;
				}

				if (UEdGraphPin* OutputPin = IMP_FindInputPin(ExitTunnel, OutputName))
				{
					IMP_SetPinFromExpr(OutputPin, OutputPair->Get(OutputValueIndex), Ctx);
				}
				else
				{
					Ctx.Errors.Add(FString::Printf(TEXT("IMP: macro exit pin not found: %s.%s"), *ExitName, *OutputName));

				}
			}
			i += 1;
		}

		OutExecPin = nullptr;
		return ExitTunnel;
	}

	// (return [:value (Pin Expr)]...)
	if (FormName.Equals(TEXT("return"), ESearchCase::IgnoreCase))
	{
		UK2Node_FunctionResult* ResultNode = IMP_FindFunctionResult(Form, Ctx);
		if (!ResultNode)
		{
			int32 ResultNodeCount = 0;
			for (UEdGraphNode* GraphNode : Ctx.Graph->Nodes)
			{
				if (GraphNode && GraphNode->IsA<UK2Node_FunctionResult>()) ++ResultNodeCount;
			}
			UE_LOG(LogBlueprintLisp, Warning, TEXT("Function return lookup failed in graph %s: nodes=%d results=%d consumed=%d"),
				*Ctx.Graph->GetName(), Ctx.Graph->Nodes.Num(), ResultNodeCount, Ctx.ConsumedFunctionResultGuids.Num());
			Ctx.Errors.Add(TEXT("IMP: function result node not found or already consumed"));
			return nullptr;
		}

		IMP_ApplyRequestedStableId(ResultNode, Form, false);
		IMP_ClearAllNodeLinks(ResultNode);
		Ctx.ConsumedFunctionResultGuids.Add(ResultNode->NodeGuid);
		for (int32 i = 1; i < Form->Num(); ++i)
		{
			if (!Form->Get(i)->IsKeyword())
			{
				continue;
			}
			const FString Keyword = Form->Get(i)->StringValue;
			if (!Keyword.Equals(TEXT(":value"), ESearchCase::IgnoreCase))
			{
				i += 1;
				continue;
			}
			if (i + 1 >= Form->Num())
			{
				Ctx.Errors.Add(TEXT("IMP: return is missing its :value pair"));
				break;
			}

			const FLispNodePtr ValuePair = Form->Get(i + 1);
			if (!ValuePair.IsValid() || !ValuePair->IsList() || ValuePair->Num() < 2)
			{
				Ctx.Errors.Add(TEXT("IMP: return has an invalid :value pair"));
				i += 1;
				continue;
			}

			const FString PinName = IMP_GetAtomName(ValuePair->Get(0));
			UEdGraphPin* ReturnPin = IMP_FindInputPin(ResultNode, PinName);
			if (!ReturnPin)
			{
				Ctx.Errors.Add(FString::Printf(TEXT("IMP: function return pin not found: %s"), *PinName));
				i += 1;
				continue;
			}
			IMP_SetPinFromExpr(ReturnPin, ValuePair->Get(ValuePair->Num() - 1), Ctx);
			i += 1;
		}

		OutExecPin = nullptr;
		return ResultNode;
	}


	// (call-parent Name [:pin value]...)
	if (FormName.Equals(TEXT("call-parent"), ESearchCase::IgnoreCase) && Form->Num() >= 2)
	{
		int32 ArgStartIndex = 2;
		const FString FuncName = IMP_ExtractCompoundName(Form, 1, ArgStartIndex);
		if (UFunction* F = IMP_FindParentFunction(FuncName, Ctx))
		{
			UK2Node_CallParentFunction* CN = IMP_CreateOrReuseCallParentNode(Form, F, FuncName, Ctx);
			IMP_ApplyCallInputs(CN, Form, ArgStartIndex, false, Ctx);
			OutExecPin = IMP_GetExecOutput(CN);
			return CN;
		}


		Ctx.Errors.Add(FString::Printf(TEXT("IMP: parent function not found: %s"), *FuncName));
		return nullptr;
	}

	// (call target func args...)
	if (FormName.Equals(TEXT("call"), ESearchCase::IgnoreCase) && Form->Num() >= 3)
	{
		FString FuncName = Form->Get(2)->IsSymbol() ? Form->Get(2)->StringValue : TEXT("");

		UFunction* F = IMP_FindFunctionForForm(FuncName, Form, Ctx);
		if (!F && Ctx.Blueprint)
		{
			// Blueprint's own functions
			for (UEdGraph* G : Ctx.Blueprint->FunctionGraphs)
				if (G && G->GetFName() == FName(*FuncName))
				{
					UK2Node_CallFunction* CN = IMP_CreateOrReuseCallFunctionNode(Form, nullptr, FuncName, true, Ctx);
					if (UEdGraphPin* TargetPin = CN->FindPin(UEdGraphSchema_K2::PN_Self))
					{
						UEdGraphPin* TargetSrc = IMP_ResolveLispExpr(Form->Get(1), Ctx);
						if (TargetSrc) IMP_Connect(TargetSrc, TargetPin, Ctx);
					}
					IMP_ApplyCallInputs(CN, Form, 3, false, Ctx);
					OutExecPin = IMP_GetExecOutput(CN);
					return CN;
				}
		}


		if (F)
		{
			UK2Node_CallFunction* CN = IMP_CreateOrReuseCallFunctionNode(Form, F, FuncName, false, Ctx);
			// Target object
			if (UEdGraphPin* TargetPin = CN->FindPin(UEdGraphSchema_K2::PN_Self))
			{
				UEdGraphPin* TargetSrc = IMP_ResolveLispExpr(Form->Get(1), Ctx);
				if (TargetSrc) IMP_Connect(TargetSrc, TargetPin, Ctx);
			}
			IMP_ApplyCallInputs(CN, Form, 3, false, Ctx);
			OutExecPin = IMP_GetExecOutput(CN);
			return CN;
		}


		Ctx.Errors.Add(FString::Printf(TEXT("IMP: function not found: %s"), *FuncName));

		return nullptr;
	}

	// (FuncName [self] [:pin value]...) — shorthand call
	if (!FormName.IsEmpty())
	{
		if (UFunction* F = IMP_FindFunctionForForm(FormName, Form, Ctx))
		{
			UK2Node_CallFunction* CN = IMP_CreateOrReuseCallFunctionNode(Form, F, FormName, false, Ctx);
			IMP_ApplyCallInputs(CN, Form, 1, true, Ctx);
			OutExecPin = IMP_GetExecOutput(CN);
			return CN;
		}


		if (UFunction* ParentFunc = IMP_FindParentFunction(FormName, Ctx))
		{
			if (!ParentFunc->HasAnyFunctionFlags(FUNC_BlueprintCallable) && !ParentFunc->HasAnyFunctionFlags(FUNC_BlueprintPure))
			{
				UK2Node_CallParentFunction* CN = NewObject<UK2Node_CallParentFunction>(Ctx.Graph);
				CN->SetFromFunction(ParentFunc); CN->NodePosX = Ctx.CurrentX; CN->NodePosY = Ctx.CurrentY;
				Ctx.Graph->AddNode(CN, false, false); CN->AllocateDefaultPins(); IMP_EnsureGuid(CN); Ctx.AdvancePosition();
				IMP_ApplyCallInputs(CN, Form, 1, false, Ctx);
				Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), CN);
				OutExecPin = IMP_GetExecOutput(CN);
				return CN;
			}
		}
	}

	if (UEdGraphNode* OpaqueReusableNode = IMP_TryReuseOpaqueGenericBodyNode(Form, Ctx))
	{
		OutExecPin = IMP_GetExecOutput(OpaqueReusableNode);
		return OpaqueReusableNode;
	}

	Ctx.Errors.Add(FString::Printf(TEXT("IMP: unhandled form: %s"), *FormName));

	return nullptr;
}


// --- Convert exec body: single statement or (seq ...) ---
static void IMP_ConvertExecBody(const FLispNodePtr& Body, FBPImportContext& Ctx, UEdGraphPin*& CurrentExecPin)
{
	if (!Body.IsValid() || Body->IsNil()) return;

	if (BP_IsStructuralSeqWrapper(Body))
	{
		for (int32 i = 1; i < Body->Num(); i++)
		{
			UEdGraphPin* StmtOut = nullptr;
			UEdGraphNode* SN = IMP_ConvertFormToNodeStable(Body->Get(i), Ctx, StmtOut);
			if (SN && CurrentExecPin)
				if (UEdGraphPin* In = IMP_GetExecInput(SN)) IMP_Connect(CurrentExecPin, In, Ctx);
			IMP_UpdateCurrentExecPin(SN, StmtOut, CurrentExecPin);
		}
	}
	else
	{
		UEdGraphPin* StmtOut = nullptr;
		UEdGraphNode* SN = IMP_ConvertFormToNodeStable(Body, Ctx, StmtOut);
		if (SN && CurrentExecPin)
			if (UEdGraphPin* In = IMP_GetExecInput(SN)) IMP_Connect(CurrentExecPin, In, Ctx);
		IMP_UpdateCurrentExecPin(SN, StmtOut, CurrentExecPin);
	}


}

// --- Convert a top-level (event ...) form ---
static void IMP_ConvertEventForm(const FLispNodePtr& EventForm, FBPImportContext& Ctx)
{
	if (!EventForm->IsList() || EventForm->Num() < 2) return;

	int32 ArgStartIndex = 2;
	FString EventName = IMP_ExtractCompoundName(EventForm, 1, ArgStartIndex);


	// Common name mapping
	if (EventName.Equals(TEXT("BeginPlay"), ESearchCase::IgnoreCase))  EventName = TEXT("ReceiveBeginPlay");
	if (EventName.Equals(TEXT("Tick"),      ESearchCase::IgnoreCase))  EventName = TEXT("ReceiveTick");
	if (EventName.Equals(TEXT("EndPlay"),   ESearchCase::IgnoreCase))  EventName = TEXT("ReceiveEndPlay");

	UK2Node_Event* EventNode = nullptr;

	// 当前 DSL 里只有 custom event 会显式导出 :param；若带 :param 仍去匹配 override/interface，
	// 容易把 custom event 误建成 UK2Node_Event 并丢掉自定义参数。
	const bool bHasExplicitParams = EventForm->HasKeyword(TEXT(":param"));

	// Override / interface event / custom event?
	UFunction* OverrideFunc = bHasExplicitParams ? nullptr : IMP_FindParentFunction(EventName, Ctx);
	UFunction* InterfaceFunc = (bHasExplicitParams || OverrideFunc) ? nullptr : IMP_FindImplementedInterfaceFunction(EventName, Ctx);
	UFunction* EventSignatureFunc = OverrideFunc ? OverrideFunc : InterfaceFunc;

	EventNode = IMP_FindReusableEventNode(EventForm, EventName, EventSignatureFunc, bHasExplicitParams, Ctx);
	const bool bReusedExistingEventNode = (EventNode != nullptr);
	if (bReusedExistingEventNode)
	{
		IMP_PrepareExistingEventBodyForIncrementalReuse(EventNode, Ctx);
	}

	else if (EventSignatureFunc)
	{
		EventNode = NewObject<UK2Node_Event>(Ctx.Graph);
		EventNode->EventReference.SetFromField<UFunction>(EventSignatureFunc, false);
		EventNode->bOverrideFunction = (OverrideFunc != nullptr);
		EventNode->NodePosX = Ctx.CurrentX;
		EventNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(EventNode, false, false);
		EventNode->AllocateDefaultPins();
		IMP_EnsureGuid(EventNode);
	}
	else
	{
		UK2Node_CustomEvent* CE = NewObject<UK2Node_CustomEvent>(Ctx.Graph);
		CE->CustomFunctionName = FName(*EventName);
		EventNode = CE;
		EventNode->NodePosX = Ctx.CurrentX;
		EventNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(EventNode, false, false);
		EventNode->AllocateDefaultPins();
		IMP_EnsureGuid(EventNode);
	}
	IMP_ApplyRequestedStableId(EventNode, EventForm, true);

	if (EventNode)
	{
		Ctx.ConsumedRootEventGuids.Add(EventNode->NodeGuid);
	}
	Ctx.AdvancePosition();


	if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(EventNode))
	{
		bool bChangedParams = false;
		for (int32 i = ArgStartIndex; i + 1 < EventForm->Num(); ++i)
		{
			const FLispNodePtr KeywordNode = EventForm->Get(i);
			if (!KeywordNode.IsValid() || !KeywordNode->IsKeyword())
			{
				break;
			}

			const FString Keyword = KeywordNode->StringValue;
			const FLispNodePtr ValueNode = EventForm->Get(i + 1);
			if (Keyword.Equals(TEXT(":param"), ESearchCase::IgnoreCase))
			{
				FString ParamName;
				FString ParamType;
				if (!IMP_TryExtractNamedTypedPair(ValueNode, ParamName, ParamType))
				{
					Ctx.Errors.Add(TEXT("Import event form failed: invalid :param declaration"));
					i += 1;
					continue;
				}

				bool bAlreadyExists = false;
				for (UEdGraphPin* ExistingPin : CustomEventNode->Pins)
				{
					if (!ExistingPin || ExistingPin->Direction != EGPD_Output || ExistingPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || ExistingPin->bHidden)
					{
						continue;
					}
					if (ExistingPin->PinName.ToString().Equals(ParamName, ESearchCase::IgnoreCase))
					{
						bAlreadyExists = true;
						break;
					}
				}
				if (bAlreadyExists)
				{
					i += 1;
					continue;
				}

				FEdGraphPinType ParamPinType;
				if (!IMP_BuildPinTypeFromLispType(ParamType, ParamPinType, Ctx))
				{
					Ctx.Errors.Add(FString::Printf(TEXT("Import event form failed: unsupported parameter type '%s'"), *ParamType));
					i += 1;
					continue;
				}

				if (!CustomEventNode->CreateUserDefinedPin(FName(*ParamName), ParamPinType, EGPD_Output, false))
				{
					Ctx.Errors.Add(FString::Printf(TEXT("Import event form failed: could not create parameter '%s'"), *ParamName));
					i += 1;
					continue;
				}
				if (UEdGraphPin* CreatedParamPin = CustomEventNode->FindPin(FName(*ParamName)))
				{
					IMP_ApplyLispTypeToPin(CreatedParamPin, ParamType);
				}
				bChangedParams = true;
			}

			i += 1;
		}

		if (bChangedParams)
		{
			const bool bPrevDisableOrphanPinSaving = CustomEventNode->bDisableOrphanPinSaving;
			CustomEventNode->bDisableOrphanPinSaving = true;
			CustomEventNode->ReconstructNode();
			CustomEventNode->bDisableOrphanPinSaving = bPrevDisableOrphanPinSaving;
			if (const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>())
			{
				K2Schema->HandleParameterDefaultValueChanged(CustomEventNode);
			}
		}
	}


	const FString EventGuid = EventNode->NodeGuid.ToString();
	Ctx.TempIdToNode.Add(EventGuid, EventNode);
	IMP_RegisterEventOutputPins(EventNode, Ctx);

	UEdGraphPin* CurrentExecPin = IMP_GetExecOutput(EventNode);

	// Skip past event name and keyword args (:event-id, :params) to find body
	int32 BodyStart = ArgStartIndex;
	for (int32 i = ArgStartIndex; i < EventForm->Num(); i++)

	{
		if (EventForm->Get(i)->IsKeyword()) { i++; BodyStart = i + 1; continue; }
		BodyStart = i; break;
	}

	for (int32 i = BodyStart; i < EventForm->Num(); i++)
	{
		if (EventForm->Get(i)->IsKeyword()) continue;
		UEdGraphPin* StmtOut = nullptr;
		UEdGraphNode* SN = IMP_ConvertFormToNodeStable(EventForm->Get(i), Ctx, StmtOut);
		if (SN && CurrentExecPin)
			if (UEdGraphPin* In = IMP_GetExecInput(SN)) IMP_Connect(CurrentExecPin, In, Ctx);
		if (StmtOut) CurrentExecPin = StmtOut;
		else if (SN && !Cast<UK2Node_IfThenElse>(SN)) CurrentExecPin = IMP_GetExecOutput(SN);
	}

	if (bReusedExistingEventNode)
	{
		IMP_FinalizeExistingEventBodyIncrementalReuse(Ctx);
	}

	Ctx.NewRow();
}


// ============================================================================
// End of Import helpers
// ============================================================================



/**
 * Recursively build K2Nodes in Graph from a pure S-expression.
 * Returns the output UEdGraphPin that represents the value of this expression,
 * or nullptr on failure (in which case OutLiteralValue may be set for literals).
 */
static bool IMP_RecordPureExprConnectionFailure(UEdGraph* Graph, UEdGraphPin* Src, UEdGraphPin* Dst, const FString& ContextLabel, TArray<FString>* OutErrors)
{
	FString Error;
	if (IMP_TryCreateConnection(Graph, Src, Dst, &Error))
	{
		return true;
	}

	if (OutErrors)
	{
		OutErrors->Add(FString::Printf(TEXT("%s: %s"), *ContextLabel, *Error));
	}
	return false;
}

static UEdGraphPin* BuildPureExprNode(
	const FLispNodePtr& Expr,
	UEdGraph* Graph,
	UBlueprint* BP,
	TArray<UEdGraphNode*>& CreatedNodes,
	FString& OutLiteralValue,
	TArray<FString>* OutErrors)
{

	OutLiteralValue.Reset();
	if (!Expr.IsValid() || Expr->IsNil()) return nullptr;

	// --- Literals ---
	if (Expr->IsNumber())
	{
		OutLiteralValue = FString::SanitizeFloat(Expr->NumberValue);
		return nullptr;
	}
	if (Expr->IsString())
	{
		OutLiteralValue = Expr->StringValue;
		return nullptr;
	}
	if (Expr->IsSymbol())
	{
		FString Sym = Expr->StringValue;
		if (Sym == TEXT("true") || Sym == TEXT("false"))
		{
			OutLiteralValue = Sym;
			return nullptr;
		}
		// Bare symbol -> member variable get
		UK2Node_VariableGet* VarNode = NewObject<UK2Node_VariableGet>(Graph);
		VarNode->VariableReference.SetSelfMember(FName(*Sym));
		VarNode->CreateNewGuid();
		VarNode->PostPlacedNewNode();
		VarNode->AllocateDefaultPins();
		Graph->AddNode(VarNode, false, false);
		CreatedNodes.Add(VarNode);
		for (UEdGraphPin* Pin : VarNode->Pins)
			if (Pin && Pin->Direction == EGPD_Output
				&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				return Pin;
		return nullptr;
	}

	if (!Expr->IsList() || Expr->Num() == 0) return nullptr;

	FLispNodePtr Head = Expr->Get(0);
	if (!Head.IsValid()) return nullptr;

	// --- (self.VarName) or single-element list ---
	if (Head->IsSymbol())
	{
		FString Sym = Head->StringValue;

		// (self.VarName) — single element list acting as member variable reference
		if (Sym.StartsWith(TEXT("self.")))
		{
			FString VarName = Sym.Mid(5);
			UK2Node_VariableGet* VarNode = NewObject<UK2Node_VariableGet>(Graph);
			VarNode->VariableReference.SetSelfMember(FName(*VarName));
			VarNode->CreateNewGuid();
			VarNode->PostPlacedNewNode();
			VarNode->AllocateDefaultPins();
			Graph->AddNode(VarNode, false, false);
			CreatedNodes.Add(VarNode);
			for (UEdGraphPin* Pin : VarNode->Pins)
				if (Pin && Pin->Direction == EGPD_Output
					&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					return Pin;
			return nullptr;
		}

		if (Sym.Equals(TEXT("make-array"), ESearchCase::IgnoreCase))
		{
			UK2Node_MakeArray* MakeArrayNode = NewObject<UK2Node_MakeArray>(Graph);
			MakeArrayNode->CreateNewGuid();
			MakeArrayNode->PostPlacedNewNode();
			MakeArrayNode->AllocateDefaultPins();
			Graph->AddNode(MakeArrayNode, false, false);
			CreatedNodes.Add(MakeArrayNode);

			TArray<FLispNodePtr> ItemExprs;
			for (int32 i = 1; i < Expr->Num(); ++i)
			{
				FLispNodePtr ArgExpr = Expr->Get(i);
				if (ArgExpr->IsKeyword())
				{
					i++;
					continue;
				}
				ItemExprs.Add(ArgExpr);
			}

			auto GatherArrayInputs = [MakeArrayNode]()
			{
				TArray<UEdGraphPin*> Pins;
				for (UEdGraphPin* Pin : MakeArrayNode->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Input) continue;
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
					if (Pin->ParentPin != nullptr) continue;
					Pins.Add(Pin);
				}
				return Pins;
			};

			TArray<UEdGraphPin*> InputPins = GatherArrayInputs();
			if (ItemExprs.Num() == 0)
			{
				if (InputPins.Num() > 0)
				{
					MakeArrayNode->RemoveInputPin(InputPins[0]);
					InputPins = GatherArrayInputs();
				}
			}
			else
			{
				while (InputPins.Num() < ItemExprs.Num())
				{
					MakeArrayNode->AddInputPin();
					InputPins = GatherArrayInputs();
				}
			}

			IMP_SeedMakeArrayLiteralType(MakeArrayNode, InputPins, ItemExprs);

			for (int32 Index = 0; Index < ItemExprs.Num() && Index < InputPins.Num(); ++Index)
			{
				FString LiteralVal;
				UEdGraphPin* ArgOutputPin = BuildPureExprNode(ItemExprs[Index], Graph, BP, CreatedNodes, LiteralVal, OutErrors);
				if (ArgOutputPin)
				{
					IMP_RecordPureExprConnectionFailure(Graph, ArgOutputPin, InputPins[Index], TEXT("BuildPureExprNode(make-array item connect)"), OutErrors);
				}

				else if (!LiteralVal.IsEmpty())
				{
					InputPins[Index]->DefaultValue = LiteralVal;
				}
			}


			return MakeArrayNode->GetOutputPin();
		}

		if (Sym.Equals(TEXT("get-array-item"), ESearchCase::IgnoreCase))
		{
			UK2Node_GetArrayItem* GetArrayItemNode = NewObject<UK2Node_GetArrayItem>(Graph);
			GetArrayItemNode->CreateNewGuid();

			GetArrayItemNode->PostPlacedNewNode();
			GetArrayItemNode->AllocateDefaultPins();
			Graph->AddNode(GetArrayItemNode, false, false);
			CreatedNodes.Add(GetArrayItemNode);

			FLispNodePtr ArrayExpr = Expr->HasKeyword(TEXT(":array"))
				? Expr->GetKeywordArg(TEXT(":array"))
				: (Expr->Num() > 1 ? Expr->Get(1) : FLispNode::MakeNil());
			FLispNodePtr IndexExpr = Expr->HasKeyword(TEXT(":index"))
				? Expr->GetKeywordArg(TEXT(":index"))
				: (Expr->Num() > 2 ? Expr->Get(2) : FLispNode::MakeNil());

			if (UEdGraphPin* ArrayPin = GetArrayItemNode->GetTargetArrayPin())
			{
				FString LiteralVal;
				UEdGraphPin* ArrayOutputPin = BuildPureExprNode(ArrayExpr, Graph, BP, CreatedNodes, LiteralVal, OutErrors);
				if (ArrayOutputPin)
				{
					IMP_RecordPureExprConnectionFailure(Graph, ArrayOutputPin, ArrayPin, TEXT("BuildPureExprNode(get-array-item array connect)"), OutErrors);
				}

			}
			if (UEdGraphPin* IndexPin = GetArrayItemNode->GetIndexPin())
			{
				FString LiteralVal;
				UEdGraphPin* IndexOutputPin = BuildPureExprNode(IndexExpr, Graph, BP, CreatedNodes, LiteralVal, OutErrors);
				if (IndexOutputPin)
				{
					IMP_RecordPureExprConnectionFailure(Graph, IndexOutputPin, IndexPin, TEXT("BuildPureExprNode(get-array-item index connect)"), OutErrors);
				}

				else if (!LiteralVal.IsEmpty())
				{
					IndexPin->DefaultValue = LiteralVal;
				}
			}

			return GetArrayItemNode->GetResultPin();
		}

		// --- Enum comparison special nodes: == and != ---
		// These correspond to UK2Node_EnumEquality / UK2Node_EnumInequality
		if (Sym == TEXT("==") || Sym == TEXT("!="))
		{

			UK2Node* CompNode = nullptr;
			if (Sym == TEXT("=="))
				CompNode = NewObject<UK2Node_EnumEquality>(Graph);
			else
				CompNode = NewObject<UK2Node_EnumInequality>(Graph);

			CompNode->CreateNewGuid();
			CompNode->PostPlacedNewNode();
			CompNode->AllocateDefaultPins();
			Graph->AddNode(CompNode, false, false);
			CreatedNodes.Add(CompNode);

			// Connect arguments: first two non-exec input data pins
			int32 ArgIdx = 1;
			int32 DataPinIdx = 0;
			for (UEdGraphPin* Pin : CompNode->Pins)
			{
				if (Pin->Direction != EGPD_Input) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (ArgIdx >= Expr->Num()) break;

				FLispNodePtr ArgExpr = Expr->Get(ArgIdx);
				if (ArgExpr->IsKeyword()) { ArgIdx++; if (ArgIdx >= Expr->Num()) break; ArgExpr = Expr->Get(ArgIdx); }

				FString LiteralVal;
				UEdGraphPin* ArgOutputPin = BuildPureExprNode(ArgExpr, Graph, BP, CreatedNodes, LiteralVal, OutErrors);
				if (ArgOutputPin)
				{
					IMP_RecordPureExprConnectionFailure(Graph, ArgOutputPin, Pin, TEXT("BuildPureExprNode(enum compare arg connect)"), OutErrors);
				}

				else if (!LiteralVal.IsEmpty())
					Pin->DefaultValue = LiteralVal;

				ArgIdx++;
				DataPinIdx++;
				if (DataPinIdx >= 2) break; // EnumEquality has exactly 2 data inputs
			}

			// Type propagation will happen when ImportGraph calls ReconstructNode on all created nodes.
			// Do NOT call PostReconstructNode here — connections may not be finalized yet.

			// Return the bool output pin
			if (UK2Node_EnumEquality* EqNode = Cast<UK2Node_EnumEquality>(CompNode))
				return EqNode->GetReturnValuePin();
			return nullptr;
		}

		// --- (FuncName arg0 arg1 ...) ---
		FBPImportContext PureCtx;
		PureCtx.Blueprint = BP;
		PureCtx.Graph = Graph;
		const FString DeclaredOwner = IMP_GetKeywordAtomValue(Expr, TEXT(":owner"));
		const bool bSelfOwner = DeclaredOwner.Equals(TEXT("self"), ESearchCase::IgnoreCase);
		UFunction* TargetFunc = IMP_FindFunctionForForm(Sym, Expr, PureCtx);
		const bool bHasSelfFunctionGraph = BP && BP->FunctionGraphs.ContainsByPredicate(
			[&Sym](const UEdGraph* FunctionGraph) { return FunctionGraph && FunctionGraph->GetName() == Sym; });
		if (!TargetFunc && !(bSelfOwner && bHasSelfFunctionGraph))
		{
			if (OutErrors)
			{
				OutErrors->Append(PureCtx.Errors);
				OutErrors->Add(FString::Printf(TEXT("BuildPureExprNode: could not resolve pure UFunction '%s' on owner '%s'"),
					*Sym, *DeclaredOwner));
			}
			return nullptr;
		}

		TSet<UEdGraphNode*> NodesBefore;
		for (UEdGraphNode* ExistingNode : Graph->Nodes) NodesBefore.Add(ExistingNode);
		UK2Node_CallFunction* CallNode = IMP_CreateOrReuseCallFunctionNode(
			Expr, TargetFunc, Sym, bSelfOwner, PureCtx);
		IMP_ApplyCallInputs(CallNode, Expr, 1, true, PureCtx);
		for (UEdGraphNode* CreatedNode : Graph->Nodes)
		{
			if (CreatedNode && !NodesBefore.Contains(CreatedNode)) CreatedNodes.AddUnique(CreatedNode);
		}
		if (OutErrors) OutErrors->Append(PureCtx.Errors);
		return IMP_GetDeclaredCallOutputPin(CallNode, Expr, PureCtx);
	}

	return nullptr;
}

} // anonymous namespace

// ============================================================================
// FBlueprintLispConverter  (public API)
// ============================================================================

FBlueprintLispResult FBlueprintLispConverter::Export(
	UBlueprint* Blueprint,
	const FString& GraphName,
	const FExportOptions& Options)
{
	if (!Blueprint)
		return FBlueprintLispResult::Fail(TEXT("Blueprint is null"));

	// Find target graph
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : Blueprint->UbergraphPages)
		if (G && G->GetName() == GraphName) { Graph = G; break; }
	if (!Graph)
		for (UEdGraph* G : Blueprint->FunctionGraphs)
			if (G && G->GetName() == GraphName) { Graph = G; break; }
	if (!Graph)
		for (UEdGraph* G : Blueprint->MacroGraphs)
			if (G && G->GetName() == GraphName) { Graph = G; break; }
	if (!Graph)
		return FBlueprintLispResult::Fail(FString::Printf(TEXT("Graph '%s' not found in '%s'"), *GraphName, *Blueprint->GetName()));

	return ExportGraph(Graph, Options);
}

FBlueprintLispResult FBlueprintLispConverter::ExportByPath(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FExportOptions& Options)
{
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!BP)
		return FBlueprintLispResult::Fail(FString::Printf(TEXT("Failed to load Blueprint: %s"), *BlueprintPath));
	return Export(BP, GraphName, Options);
}

FBlueprintLispResult FBlueprintLispConverter::ExportGraph(
	UEdGraph* Graph,
	const FExportOptions& Options)
{
	if (!Graph)
		return FBlueprintLispResult::Fail(TEXT("ExportGraph: Graph is null"));

	TArray<FString> ExportErrors;
	FScopedBlueprintLispExportErrors ExportErrorScope(ExportErrors);

	TArray<FGuid> EventGuids, NodeGuids;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Cast<UK2Node_InputAction>(Node) || Cast<UK2Node_InputKey>(Node)
			|| Cast<UK2Node_CustomEvent>(Node) || Cast<UK2Node_Event>(Node) || Cast<UK2Node_FunctionEntry>(Node))
			EventGuids.Add(Node->NodeGuid);
		else if (UK2Node_Tunnel* TE = Cast<UK2Node_Tunnel>(Node); TE && TE->DrawNodeAsEntry())
			EventGuids.Add(Node->NodeGuid);
		else if (Node->NodeGuid.IsValid())
			NodeGuids.Add(Node->NodeGuid);
	}
	TMap<FGuid, FString> ShortEventIds = Options.bStableIds ? ComputeShortIds(EventGuids) : TMap<FGuid,FString>();
	TMap<FGuid, FString> ShortNodeIds  = Options.bStableIds ? ComputeShortIds(NodeGuids)  : TMap<FGuid,FString>();

	TArray<FString> Forms;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_InputAction* IA = Cast<UK2Node_InputAction>(Node))
		{
			FLispNodePtr Form = ConvertInputActionToLisp(IA, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
			if (Form.IsValid() && !Form->IsNil()) Forms.Add(Form->ToString(Options.bPrettyPrint, 0));
		}
		else if (UK2Node_InputKey* IK = Cast<UK2Node_InputKey>(Node))
		{
			FLispNodePtr Form = ConvertInputKeyToLisp(IK, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
			if (Form.IsValid() && !Form->IsNil()) Forms.Add(Form->ToString(Options.bPrettyPrint, 0));
		}
		else if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
		{
			FLispNodePtr Form = ConvertCustomEventToLisp(CE, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
			if (Form.IsValid() && !Form->IsNil()) Forms.Add(Form->ToString(Options.bPrettyPrint, 0));
		}
		else if (UK2Node_ComponentBoundEvent* CBE = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			FLispNodePtr Form = ConvertComponentBoundEventToLisp(CBE, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
			if (Form.IsValid() && !Form->IsNil()) Forms.Add(Form->ToString(Options.bPrettyPrint, 0));
		}
		else if (UK2Node_ActorBoundEvent* ABE = Cast<UK2Node_ActorBoundEvent>(Node))
		{
			FLispNodePtr Form = ConvertActorBoundEventToLisp(ABE, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
			if (Form.IsValid() && !Form->IsNil()) Forms.Add(Form->ToString(Options.bPrettyPrint, 0));
		}
		else if (UK2Node_Event* E = Cast<UK2Node_Event>(Node))
		{
			FLispNodePtr Form = ConvertEventToLisp(E, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
			if (Form.IsValid() && !Form->IsNil()) Forms.Add(Form->ToString(Options.bPrettyPrint, 0));
		}
		else if (UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Node))
		{
			FLispNodePtr Form = ConvertFunctionEntryToLisp(FE, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
			if (Form.IsValid() && !Form->IsNil()) Forms.Add(Form->ToString(Options.bPrettyPrint, 0));
		}
		else if (UK2Node_Tunnel* TE = Cast<UK2Node_Tunnel>(Node); TE && TE->DrawNodeAsEntry())
		{
			FLispNodePtr Form = ConvertTunnelEntryToLisp(TE, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
			if (Form.IsValid() && !Form->IsNil()) Forms.Add(Form->ToString(Options.bPrettyPrint, 0));
		}
	}
	if (ExportErrors.Num() > 0)
	{
		return FBlueprintLispResult::Fail(FString::Join(ExportErrors, TEXT("\n")));
	}

	// Function-graph mode: handles AnimationTransitionGraph and other pure-expression graphs
	// that have a Result/Sink node but no Event entry node.
	// We locate the sink node, find its bool input pin, and export the pure DAG as (transition-cond <expr>).
	if (Forms.IsEmpty())
	{
		UAnimationTransitionGraph* TransGraph = Cast<UAnimationTransitionGraph>(Graph);
		UAnimGraphNode_TransitionResult* ResultNode = TransGraph ? TransGraph->GetResultNode() : nullptr;
		if (!ResultNode)
		{
			// Fallback: look for any node that is a "sink" (IsNodeRootSet)
			for (UEdGraphNode* N : Graph->Nodes)
			{
				if (UK2Node* K2N = Cast<UK2Node>(N))
				{
					// Check class name as fallback
					if (N->GetClass()->GetName().Contains(TEXT("TransitionResult")))
					{
						ResultNode = Cast<UAnimGraphNode_TransitionResult>(N);
						break;
					}
				}
			}
		}

		if (ResultNode)
		{
			// Find the bool input pin (bCanEnterTransition)
			UEdGraphPin* BoolPin = nullptr;
			for (UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input
					&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
				{
					BoolPin = Pin;
					break;
				}
			}

			if (BoolPin)
			{
				TSet<UEdGraphNode*> Visited;
				FLispNodePtr CondExpr = ConvertPureExpressionToLisp(BoolPin, Graph, Visited);

				// Wrap in (transition-cond <expr>)
				TArray<FLispNodePtr> Form;
				Form.Add(FLispNode::MakeSymbol(TEXT("transition-cond")));
				Form.Add(CondExpr.IsValid() ? CondExpr : FLispNode::MakeSymbol(TEXT("true")));
				FLispNodePtr TransForm = FLispNode::MakeList(Form);
				Forms.Add(TransForm->ToString(Options.bPrettyPrint, 0));
			}
			else
			{
				return FBlueprintLispResult::Fail(TEXT("ExportGraph: TransitionResult has no bool input pin"));
			}
		}
		else
		{
			// No event/transition nodes: return Ok with a skip comment instead of Fail
			FBlueprintLispResult R;
			R.bSuccess = true;
			R.LispCode = FString::Printf(TEXT("; skip: No event nodes found in graph '%s' (not an EventGraph, and no TransitionResult found)"), *Graph->GetName());
			R.Warnings.Add(FString::Printf(TEXT("No event nodes found in graph '%s' (skipped)"), *Graph->GetName()));
			return R;
		}
	}

	FString Code;
	for (int32 i = 0; i < Forms.Num(); i++)
	{
		if (i > 0) Code += TEXT("\n\n");
		Code += Forms[i];
	}
	return FBlueprintLispResult::Ok(Code);
}

FBlueprintLispResult FBlueprintLispConverter::ExportPureExpression(
	UEdGraphPin* InputPin,
	const FExportOptions& Options)
{
	if (!InputPin || !InputPin->GetOwningNode() || !InputPin->GetOwningNode()->GetGraph())
	{
		return FBlueprintLispResult::Fail(TEXT("ExportPureExpression: input pin or graph is null"));
	}
	if (InputPin->Direction != EGPD_Input || InputPin->LinkedTo.IsEmpty())
	{
		return FBlueprintLispResult::Fail(TEXT("ExportPureExpression: pin is not a connected input"));
	}

	TArray<FString> ExportErrors;
	FScopedBlueprintLispExportErrors ExportErrorScope(ExportErrors);
	TMap<FGuid, FString> ShortIds;
	if (Options.bStableIds)
	{
		TArray<FGuid> NodeGuids;
		for (UEdGraphNode* Node : InputPin->GetOwningNode()->GetGraph()->Nodes)
		{
			if (Node && Node->NodeGuid.IsValid()) NodeGuids.Add(Node->NodeGuid);
		}
		ShortIds = ComputeShortIds(NodeGuids);
	}

	TSet<UEdGraphNode*> Visited;
	const FLispNodePtr Expression = ConvertPureExpressionToLisp(
		InputPin, InputPin->GetOwningNode()->GetGraph(), Visited, Options.bStableIds ? &ShortIds : nullptr);
	if (!ExportErrors.IsEmpty())
	{
		return FBlueprintLispResult::Fail(FString::Join(ExportErrors, TEXT("\n")));
	}
	if (!Expression.IsValid() || Expression->IsNil())
	{
		return FBlueprintLispResult::Fail(TEXT("ExportPureExpression: expression is empty"));
	}
	return FBlueprintLispResult::Ok(Expression->ToString(Options.bPrettyPrint, 0));
}

FBlueprintLispResult FBlueprintLispConverter::Validate(const FString& LispCode)
{
	FLispParseResult PR = FLispParser::Parse(LispCode);
	if (!PR.bSuccess)
		return FBlueprintLispResult::Fail(FString::Printf(TEXT("Parse error at %d:%d: %s"),
			PR.ErrorLine, PR.ErrorColumn, *PR.Error));

	for (const auto& Node : PR.Nodes)
	{
		if (!Node.IsValid() || !Node->IsList())
			return FBlueprintLispResult::Fail(TEXT("Top-level expressions must be lists"));
		FString Form = Node->GetFormName().ToLower();
		static const TSet<FString> ValidForms = {
			TEXT("event"), TEXT("input-action"), TEXT("input-key"), TEXT("component-bound-event"), TEXT("actor-bound-event"),
			TEXT("func"), TEXT("function"), TEXT("macro"), TEXT("exit"),
			TEXT("call-macro"),
			TEXT("var"), TEXT("comment"),
			TEXT("transition-cond")  // function-graph mode: pure bool expression for AnimationTransitionGraph
		};
		if (!ValidForms.Contains(Form))
			return FBlueprintLispResult::Fail(FString::Printf(TEXT("Unknown top-level form: %s"), *Form));
	}

	return FBlueprintLispResult::Ok(LispCode);
}

FBlueprintLispResult FBlueprintLispConverter::Import(
	UBlueprint*           Blueprint,
	const FString&        GraphName,
	const FString&        LispCode,
	const FImportOptions& Options)
{
	if (!Blueprint)
		return FBlueprintLispResult::Fail(TEXT("Import: Blueprint is null"));

	// Locate the target graph
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : Blueprint->UbergraphPages)
		if (G && G->GetName() == GraphName)
			{ Graph = G; break; }
	if (!Graph)
		for (UEdGraph* G : Blueprint->FunctionGraphs)
			if (G && G->GetName() == GraphName) { Graph = G; break; }
	if (!Graph)
		for (UEdGraph* G : Blueprint->MacroGraphs)
			if (G && G->GetName() == GraphName) { Graph = G; break; }
	if (!Graph)
		return FBlueprintLispResult::Fail(FString::Printf(TEXT("Import: graph '%s' not found in '%s'"),
			*GraphName, *Blueprint->GetName()));

	if (Options.ImportMode == FBlueprintLispConverter::EImportMode::UpdateSemantic)
	{
		return FBlueprintLispResult::Fail(TEXT("Import: UpdateSemantic mode is not implemented yet. Use Update() when semantic diff is available."));
	}

	// Parse DSL
	FLispParseResult PR = FLispParser::Parse(LispCode);
	if (!PR.bSuccess)
		return FBlueprintLispResult::Fail(FString::Printf(TEXT("Import: parse error at %d:%d: %s"),
			PR.ErrorLine, PR.ErrorColumn, *PR.Error));

	// Set up context
	FBPImportContext Ctx;
	Ctx.Blueprint = Blueprint;
	Ctx.Graph     = Graph;
	Ctx.ImportMode = Options.ImportMode;

	const BlueprintLispImportLifecycle::FImportLifecycleContext LifecycleContext =
		IMP_MakeLifecycleContext(Blueprint, Graph, Options);
	TSet<UEdGraphNode*> PreExistingNodes;
	for (UEdGraphNode* ExistingNode : Graph->Nodes)
	{
		if (ExistingNode)
		{
			PreExistingNodes.Add(ExistingNode);
		}
	}
	TArray<BlueprintLispImportLifecycle::FImportPropertyChange> PropertyChanges;

	if (Options.bFailOnUnsupportedForm && !IMP_ValidateImportCoverage(PR.Nodes, Ctx))
	{
		return FBlueprintLispResult::Fail(Ctx.Errors.Num() > 0 ? Ctx.Errors[0] : TEXT("Import aborted due to unsupported DSL forms"));
	}

	IMP_EnsureBlueprintVariablesFromTopLevelForms(PR.Nodes, Ctx);
	if (Ctx.Errors.Num() > 0)
	{
		return IMP_FailFromContext(Ctx, TEXT("Import var declaration failed"));
	}

	IMP_BroadcastNodePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PreNodeChanges, LifecycleContext, {});
	IMP_BroadcastPropertyPhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PrePropertyChanges, LifecycleContext, PropertyChanges);

	if (Options.ImportMode == FBlueprintLispConverter::EImportMode::ReplaceGraph)
	{
		IMP_ClearGraphForReplace(Graph, IMP_DetectGraphKind(Graph));
	}

	// Process top-level forms
	int32 EventsCreated = 0;
	for (const FLispNodePtr& Form : PR.Nodes)
	{
		if (!Form->IsList() || Form->Num() == 0) continue;
		FString FormName = Form->GetFormName();

		if (FormName.Equals(TEXT("event"), ESearchCase::IgnoreCase))
		{
			IMP_ConvertEventForm(Form, Ctx);
			EventsCreated++;
		}
		else if (FormName.Equals(TEXT("input-action"), ESearchCase::IgnoreCase))
		{
			IMP_ConvertInputActionForm(Form, Ctx);
			EventsCreated++;
		}
		else if (FormName.Equals(TEXT("input-key"), ESearchCase::IgnoreCase))
		{
			IMP_ConvertInputKeyForm(Form, Ctx);
			EventsCreated++;
		}
		else if (FormName.Equals(TEXT("component-bound-event"), ESearchCase::IgnoreCase))
		{
			IMP_ConvertComponentBoundEventForm(Form, Ctx);
			EventsCreated++;
		}
		else if (FormName.Equals(TEXT("actor-bound-event"), ESearchCase::IgnoreCase))
		{
			IMP_ConvertActorBoundEventForm(Form, Ctx);
			EventsCreated++;
		}
		else if (FormName.Equals(TEXT("func"), ESearchCase::IgnoreCase))
		{
			// func: creates a new function graph
			if (Form->Num() < 2)
			{
				Ctx.Errors.Add(TEXT("Import: (func ...) is missing function name"));
				continue;
			}

			int32 NameArgStartIndex = 2;
			FString FuncName = IMP_ExtractCompoundName(Form, 1, NameArgStartIndex);
			if (FuncName.IsEmpty()) FuncName = TEXT("NewFunction");
			bool bExists = false;

			for (UEdGraph* G : Blueprint->FunctionGraphs)
				if (G && G->GetFName() == FName(*FuncName)) { bExists = true; break; }
			if (!bExists)
			{
				UEdGraph* FG = FBlueprintEditorUtils::CreateNewGraph(
					Blueprint, FName(*FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				if (FG)
				{
					FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, FG, true, nullptr);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
					EventsCreated++;
				}
				else
				{
					Ctx.Errors.Add(FString::Printf(TEXT("Import: failed to create FunctionGraph '%s'"), *FuncName));
				}
			}
		}

		else if (FormName.Equals(TEXT("function"), ESearchCase::IgnoreCase))
		{
			// function: import into an existing function graph (the FunctionEntry node already exists)
			// Find the existing FunctionEntry node in the current graph
			UK2Node_FunctionEntry* ExistingEntry = nullptr;
			for (UEdGraphNode* N : Graph->Nodes)
			{
				ExistingEntry = Cast<UK2Node_FunctionEntry>(N);
				if (ExistingEntry) break;
			}

			if (!ExistingEntry)
			{
				Ctx.Errors.Add(FString::Printf(TEXT("Import: no FunctionEntry node found in graph '%s' for top-level function form"), *Graph->GetName()));
				continue;
			}

			{

				IMP_ApplyRequestedStableId(ExistingEntry, Form, true);
				IMP_EnsureFunctionEntryParamsFromFunctionForm(ExistingEntry, Form, Ctx);
				IMP_EnsureFunctionLocalsFromFunctionForm(ExistingEntry, Form, Ctx);
				IMP_EnsureFunctionResultFromFunctionForm(ExistingEntry, Form, Ctx);
				if (Ctx.Errors.Num() > 0)
				{
					return IMP_FailFromContext(Ctx, TEXT("Import function form failed"));
				}

				// Register the FunctionEntry's output pins as variables for downstream node resolution
				FString EntryGuid = ExistingEntry->NodeGuid.ToString();
				Ctx.TempIdToNode.Add(EntryGuid, ExistingEntry);


				for (UEdGraphPin* P : ExistingEntry->Pins)
				{
					if (P->Direction != EGPD_Output || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || P->bHidden) continue;
					FString PinName = P->PinName.ToString();
					Ctx.VariableToNodeId.Add(PinName, EntryGuid);
					Ctx.VariableToPin.Add(PinName, PinName);
					FString NoSpaces = PinName.Replace(TEXT(" "), TEXT(""));
					if (NoSpaces != PinName) { Ctx.VariableToNodeId.Add(NoSpaces, EntryGuid); Ctx.VariableToPin.Add(NoSpaces, PinName); }
				}

				UEdGraphPin* CurrentExecPin = IMP_GetExecOutput(ExistingEntry);
				const bool bReuseExistingFunctionBody = (Options.ImportMode != FBlueprintLispConverter::EImportMode::ReplaceGraph);
				if (bReuseExistingFunctionBody)
				{
					IMP_PrepareExistingEventBodyForIncrementalReuse(ExistingEntry, Ctx);
				}

				int32 NameArgStartIndex = 2;
				IMP_ExtractCompoundName(Form, 1, NameArgStartIndex);

				// Skip past function name and keyword args (:event-id, :param, :return, :pos) to find body
				int32 BodyStart = NameArgStartIndex;
				for (int32 i = NameArgStartIndex; i < Form->Num(); i++)

				{
					if (Form->Get(i)->IsKeyword()) { i++; BodyStart = i + 1; continue; }
					BodyStart = i; break;
				}

				for (int32 i = BodyStart; i < Form->Num(); i++)
				{
					UEdGraphPin* OutPin = nullptr;
					UEdGraphNode* NewNode = IMP_ConvertFormToNodeStable(Form->Get(i), Ctx, OutPin);
					if (CurrentExecPin && NewNode)
					{
						UEdGraphPin* InExec = IMP_GetExecInput(NewNode);
						if (InExec) IMP_Connect(CurrentExecPin, InExec, Ctx);
					}
					IMP_UpdateCurrentExecPin(NewNode, OutPin, CurrentExecPin);
				}

				if (bReuseExistingFunctionBody)
				{
					IMP_FinalizeExistingEventBodyIncrementalReuse(Ctx);
				}

				if (Ctx.Errors.Num() > 0)
				{
					return IMP_FailFromContext(Ctx, TEXT("Import function form failed"));
				}


				EventsCreated++;
			}
		}
		else if (FormName.Equals(TEXT("macro"), ESearchCase::IgnoreCase))


		{
			// macro: import into an existing macro graph (the Tunnel entry node already exists)
			UK2Node_Tunnel* ExistingTunnel = nullptr;
			for (UEdGraphNode* N : Graph->Nodes)
			{
				UK2Node_Tunnel* TE = Cast<UK2Node_Tunnel>(N);
				if (TE && TE->DrawNodeAsEntry()) { ExistingTunnel = TE; break; }
			}

			if (!ExistingTunnel)
			{
				Ctx.Errors.Add(FString::Printf(TEXT("Import: no macro entry tunnel found in graph '%s' for top-level macro form"), *Graph->GetName()));
				continue;
			}

			{

				IMP_ApplyRequestedStableId(ExistingTunnel, Form, true);
				FString EntryGuid = ExistingTunnel->NodeGuid.ToString();
				Ctx.TempIdToNode.Add(EntryGuid, ExistingTunnel);

				for (UEdGraphPin* P : ExistingTunnel->Pins)
				{
					if (P->Direction != EGPD_Output || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || P->bHidden) continue;
					FString PinName = P->PinName.ToString();
					Ctx.VariableToNodeId.Add(PinName, EntryGuid);
					Ctx.VariableToPin.Add(PinName, PinName);
					FString NoSpaces = PinName.Replace(TEXT(" "), TEXT(""));
					if (NoSpaces != PinName) { Ctx.VariableToNodeId.Add(NoSpaces, EntryGuid); Ctx.VariableToPin.Add(NoSpaces, PinName); }
				}

				UEdGraphPin* CurrentExecPin = IMP_GetExecOutput(ExistingTunnel);
				const bool bReuseExistingMacroBody = (Options.ImportMode != FBlueprintLispConverter::EImportMode::ReplaceGraph);
				if (bReuseExistingMacroBody)
				{
					IMP_PrepareExistingEventBodyForIncrementalReuse(ExistingTunnel, Ctx);
				}

				int32 NameArgStartIndex = 2;
				IMP_ExtractCompoundName(Form, 1, NameArgStartIndex);

				int32 BodyStart = NameArgStartIndex;
				for (int32 i = NameArgStartIndex; i < Form->Num(); i++)

				{
					if (Form->Get(i)->IsKeyword()) { i++; BodyStart = i + 1; continue; }
					BodyStart = i; break;
				}

				for (int32 i = BodyStart; i < Form->Num(); i++)
				{
					UEdGraphPin* OutPin = nullptr;
					UEdGraphNode* NewNode = IMP_ConvertFormToNodeStable(Form->Get(i), Ctx, OutPin);
					if (CurrentExecPin && NewNode)
					{
						UEdGraphPin* InExec = IMP_GetExecInput(NewNode);
						if (InExec) IMP_Connect(CurrentExecPin, InExec, Ctx);
					}
					IMP_UpdateCurrentExecPin(NewNode, OutPin, CurrentExecPin);
				}
				if (bReuseExistingMacroBody)
				{
					IMP_FinalizeExistingEventBodyIncrementalReuse(Ctx);
				}
				EventsCreated++;

			}
		}
		else if (FormName.Equals(TEXT("var"), ESearchCase::IgnoreCase)
			|| FormName.Equals(TEXT("comment"), ESearchCase::IgnoreCase))
		{
			// asset-level declarations / comments are handled separately and should not emit graph nodes here
			continue;
		}
		else

		{
			// other top-level forms: treat as anonymous exec body
			UEdGraphPin* ExecOut = nullptr;
			IMP_ConvertFormToNodeStable(Form, Ctx, ExecOut);
		}
	}

	// Reconstruct all nodes to resolve wildcards
	for (UEdGraphNode* N : Graph->Nodes)
		if (N) N->ReconstructNode();

	// Mark blueprint modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TArray<BlueprintLispImportLifecycle::FImportNodeChange> NodeChanges;
	IMP_CollectNodeChanges(PreExistingNodes, Graph, NodeChanges);
	IMP_BroadcastNodePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostNodeChanges, LifecycleContext, NodeChanges);
	IMP_BroadcastPropertyPhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostPropertyChanges, LifecycleContext, PropertyChanges);

	// Optionally compile
	if (Options.bCompile)
	{
		IMP_BroadcastFinalizePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PreFinalize, LifecycleContext);
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		IMP_RecordCompileStatus(Blueprint, Ctx, TEXT("Import"));
		IMP_BroadcastFinalizePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostFinalize, LifecycleContext);
	}

	if (Ctx.Errors.Num() > 0)
	{
		return IMP_FailFromContext(Ctx, TEXT("Import failed"));
	}

	// Return summary
	FString Summary = FString::Printf(TEXT("Import OK: %d events, %d nodes"),
		EventsCreated, Graph->Nodes.Num());
	return IMP_OkFromContext(Summary, Ctx);

}

FBlueprintLispResult FBlueprintLispConverter::ImportGraph(
	UEdGraph* Graph,
	const FString& LispCode,
	const FImportOptions& Options)
{
	if (!Graph)
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: Graph is null"));

	if (Options.ImportMode == FBlueprintLispConverter::EImportMode::UpdateSemantic)
	{
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: UpdateSemantic mode is not implemented yet. Use Update() when semantic diff is available."));
	}

	// Parse the DSL
	FLispParseResult PR = FLispParser::Parse(LispCode);
	if (!PR.bSuccess)
		return FBlueprintLispResult::Fail(FString::Printf(TEXT("ImportGraph: parse error at %d:%d: %s"),
			PR.ErrorLine, PR.ErrorColumn, *PR.Error));

	if (PR.Nodes.IsEmpty())
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: no top-level expressions"));

	FBPImportContext ValidationCtx;
	ValidationCtx.Graph = Graph;
	ValidationCtx.Blueprint = Graph->GetTypedOuter<UBlueprint>();

	const BlueprintLispImportLifecycle::FImportLifecycleContext LifecycleContext =
		IMP_MakeLifecycleContext(ValidationCtx.Blueprint, Graph, Options);
	TSet<UEdGraphNode*> PreExistingNodes;
	for (UEdGraphNode* ExistingNode : Graph->Nodes)
	{
		if (ExistingNode)
		{
			PreExistingNodes.Add(ExistingNode);
		}
	}
	TArray<BlueprintLispImportLifecycle::FImportPropertyChange> PropertyChanges;

	if (Options.bFailOnUnsupportedForm && !IMP_ValidateImportCoverage(PR.Nodes, ValidationCtx))
	{
		return FBlueprintLispResult::Fail(ValidationCtx.Errors.Num() > 0 ? ValidationCtx.Errors[0] : TEXT("ImportGraph aborted due to unsupported DSL forms"));
	}

	IMP_EnsureBlueprintVariablesFromTopLevelForms(PR.Nodes, ValidationCtx);
	if (ValidationCtx.Errors.Num() > 0)
	{
		return IMP_FailFromContext(ValidationCtx, TEXT("ImportGraph var declaration failed"));
	}

	IMP_BroadcastNodePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PreNodeChanges, LifecycleContext, {});
	IMP_BroadcastPropertyPhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PrePropertyChanges, LifecycleContext, PropertyChanges);

	if (Options.ImportMode == FBlueprintLispConverter::EImportMode::ReplaceGraph)
	{
		IMP_ClearGraphForReplace(Graph, IMP_DetectGraphKind(Graph));
	}

	FLispNodePtr TopExpr;
	for (const FLispNodePtr& Candidate : PR.Nodes)
	{
		if (!Candidate.IsValid() || !Candidate->IsList() || Candidate->Num() == 0)
		{
			continue;
		}
		if (Candidate->GetFormName().Equals(TEXT("var"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		TopExpr = Candidate;
		break;
	}
	if (!TopExpr.IsValid() || !TopExpr->IsList())
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: no non-var top-level expression found"));

	FString FormName = TopExpr->GetFormName().ToLower();
	if (FormName == TEXT("function"))
	{
		// Import a function graph: find the existing FunctionEntry node and wire body
		UK2Node_FunctionEntry* ExistingEntry = nullptr;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			ExistingEntry = Cast<UK2Node_FunctionEntry>(N);
			if (ExistingEntry) break;
		}
		if (!ExistingEntry)
			return FBlueprintLispResult::Fail(TEXT("ImportGraph: no FunctionEntry node found in graph"));

		// Set up context
		UBlueprint* BP = nullptr;
		for (UEdGraph* G : ExistingEntry->GetTypedOuter<UBlueprint>()->FunctionGraphs)
		{
			if (G == Graph) { BP = ExistingEntry->GetTypedOuter<UBlueprint>(); break; }
		}
		if (!BP)
			BP = ExistingEntry->GetTypedOuter<UBlueprint>();

		FBPImportContext Ctx;
		Ctx.Blueprint = BP;
		Ctx.Graph     = Graph;
		Ctx.ImportMode = Options.ImportMode;

		IMP_ApplyRequestedStableId(ExistingEntry, TopExpr, true);
		IMP_EnsureFunctionEntryParamsFromFunctionForm(ExistingEntry, TopExpr, Ctx);
		IMP_EnsureFunctionLocalsFromFunctionForm(ExistingEntry, TopExpr, Ctx);
		IMP_EnsureFunctionResultFromFunctionForm(ExistingEntry, TopExpr, Ctx);

		if (Ctx.Errors.Num() > 0)
		{
			return IMP_FailFromContext(Ctx, TEXT("ImportGraph function import failed"));
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP,
			EBlueprintCompileOptions::RegenerateSkeletonOnly
			| EBlueprintCompileOptions::SkipGarbageCollection
			| EBlueprintCompileOptions::SkipSave);
		ExistingEntry->ReconstructNode();
		if (Options.bSignatureOnly)
		{
			return IMP_OkFromContext(FString::Printf(TEXT("ImportGraph OK: function signature imported for %s"), *Graph->GetName()), Ctx);
		}

		FString EntryGuid = ExistingEntry->NodeGuid.ToString();
		Ctx.TempIdToNode.Add(EntryGuid, ExistingEntry);


		for (UEdGraphPin* P : ExistingEntry->Pins)
		{
			if (P->Direction != EGPD_Output || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || P->bHidden) continue;
			FString PinName = P->PinName.ToString();
			Ctx.VariableToNodeId.Add(PinName, EntryGuid);
			Ctx.VariableToPin.Add(PinName, PinName);
		}

		UEdGraphPin* CurrentExecPin = IMP_GetExecOutput(ExistingEntry);
		const bool bReuseExistingFunctionBody = (Options.ImportMode != FBlueprintLispConverter::EImportMode::ReplaceGraph);
		if (bReuseExistingFunctionBody)
		{
			IMP_PrepareExistingEventBodyForIncrementalReuse(ExistingEntry, Ctx);
		}

		int32 BodyStart = 2;
		for (int32 i = 2; i < TopExpr->Num(); i++)
		{
			if (TopExpr->Get(i)->IsKeyword()) { i++; BodyStart = i + 1; continue; }
			BodyStart = i; break;
		}

		for (int32 i = BodyStart; i < TopExpr->Num(); i++)
		{
			UEdGraphPin* OutPin = nullptr;
			UEdGraphNode* NewNode = IMP_ConvertFormToNodeStable(TopExpr->Get(i), Ctx, OutPin);
			if (CurrentExecPin && NewNode)
			{
				UEdGraphPin* InExec = IMP_GetExecInput(NewNode);
				if (InExec) IMP_Connect(CurrentExecPin, InExec, Ctx);
			}
			IMP_UpdateCurrentExecPin(NewNode, OutPin, CurrentExecPin);
		}

		if (bReuseExistingFunctionBody)
		{
			IMP_FinalizeExistingEventBodyIncrementalReuse(Ctx);
		}


		for (UEdGraphNode* N : Graph->Nodes)
			if (N) N->ReconstructNode();

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TArray<BlueprintLispImportLifecycle::FImportNodeChange> NodeChanges;
		IMP_CollectNodeChanges(PreExistingNodes, Graph, NodeChanges);
		IMP_BroadcastNodePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostNodeChanges, LifecycleContext, NodeChanges);
		IMP_BroadcastPropertyPhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostPropertyChanges, LifecycleContext, PropertyChanges);

		if (Options.bCompile)
		{
			IMP_BroadcastFinalizePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PreFinalize, LifecycleContext);
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
			IMP_RecordCompileStatus(BP, Ctx, TEXT("ImportGraph(function)"));
			IMP_BroadcastFinalizePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostFinalize, LifecycleContext);
		}

		if (Ctx.Errors.Num() > 0)
		{
			return IMP_FailFromContext(Ctx, TEXT("ImportGraph function import failed"));
		}

		return IMP_OkFromContext(FString::Printf(TEXT("ImportGraph OK: function body imported, %d nodes"), Graph->Nodes.Num()), Ctx);

	}


	if (FormName == TEXT("macro"))

	{
		// Import a macro graph: find the existing Tunnel entry node and wire body
		UK2Node_Tunnel* ExistingTunnel = nullptr;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			UK2Node_Tunnel* TE = Cast<UK2Node_Tunnel>(N);
			if (TE && TE->DrawNodeAsEntry()) { ExistingTunnel = TE; break; }
		}
		if (!ExistingTunnel)
			return FBlueprintLispResult::Fail(TEXT("ImportGraph: no Tunnel entry node found in graph"));

		UBlueprint* BP = ExistingTunnel->GetTypedOuter<UBlueprint>();
		if (!BP)
			return FBlueprintLispResult::Fail(TEXT("ImportGraph: cannot find owning Blueprint"));

		FBPImportContext Ctx;
		Ctx.Blueprint = BP;
		Ctx.Graph     = Graph;
		Ctx.ImportMode = Options.ImportMode;

		IMP_ApplyRequestedStableId(ExistingTunnel, TopExpr, true);
		FString EntryGuid = ExistingTunnel->NodeGuid.ToString();

		Ctx.TempIdToNode.Add(EntryGuid, ExistingTunnel);

		for (UEdGraphPin* P : ExistingTunnel->Pins)
		{
			if (P->Direction != EGPD_Output || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || P->bHidden) continue;
			FString PinName = P->PinName.ToString();
			Ctx.VariableToNodeId.Add(PinName, EntryGuid);
			Ctx.VariableToPin.Add(PinName, PinName);
		}

		UEdGraphPin* CurrentExecPin = IMP_GetExecOutput(ExistingTunnel);
		const bool bReuseExistingMacroBody = (Options.ImportMode != FBlueprintLispConverter::EImportMode::ReplaceGraph);
		if (bReuseExistingMacroBody)
		{
			IMP_PrepareExistingEventBodyForIncrementalReuse(ExistingTunnel, Ctx);
		}

		int32 BodyStart = 2;
		for (int32 i = 2; i < TopExpr->Num(); i++)
		{
			if (TopExpr->Get(i)->IsKeyword()) { i++; BodyStart = i + 1; continue; }
			BodyStart = i; break;
		}

		for (int32 i = BodyStart; i < TopExpr->Num(); i++)
		{
			UEdGraphPin* OutPin = nullptr;
			UEdGraphNode* NewNode = IMP_ConvertFormToNodeStable(TopExpr->Get(i), Ctx, OutPin);
			if (CurrentExecPin && NewNode)
			{
				UEdGraphPin* InExec = IMP_GetExecInput(NewNode);
				if (InExec) IMP_Connect(CurrentExecPin, InExec, Ctx);
			}
			IMP_UpdateCurrentExecPin(NewNode, OutPin, CurrentExecPin);
		}

		if (bReuseExistingMacroBody)
		{
			IMP_FinalizeExistingEventBodyIncrementalReuse(Ctx);
		}


		for (UEdGraphNode* N : Graph->Nodes)
			if (N) N->ReconstructNode();

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TArray<BlueprintLispImportLifecycle::FImportNodeChange> NodeChanges;
		IMP_CollectNodeChanges(PreExistingNodes, Graph, NodeChanges);
		IMP_BroadcastNodePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostNodeChanges, LifecycleContext, NodeChanges);
		IMP_BroadcastPropertyPhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostPropertyChanges, LifecycleContext, PropertyChanges);

		if (Options.bCompile)
		{
			IMP_BroadcastFinalizePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PreFinalize, LifecycleContext);
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
			IMP_RecordCompileStatus(BP, Ctx, TEXT("ImportGraph(macro)"));
			IMP_BroadcastFinalizePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostFinalize, LifecycleContext);
		}

		if (Ctx.Errors.Num() > 0)
		{
			return IMP_FailFromContext(Ctx, TEXT("ImportGraph macro import failed"));
		}

		return IMP_OkFromContext(FString::Printf(TEXT("ImportGraph OK: macro body imported, %d nodes"), Graph->Nodes.Num()), Ctx);

	}

	if (FormName != TEXT("transition-cond"))

		return FBlueprintLispResult::Fail(FString::Printf(TEXT("ImportGraph: expected (transition-cond ...), (function ...), or (macro ...), got (%s ...)"), *FormName));

	if (TopExpr->Num() < 2)
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: (transition-cond) missing condition expression"));

	// Find or create the TransitionResult node
	UAnimGraphNode_TransitionResult* ResultNode = nullptr;
	UAnimationTransitionGraph* TransGraph = Cast<UAnimationTransitionGraph>(Graph);
	if (TransGraph)
		ResultNode = TransGraph->GetResultNode();
	if (!ResultNode)
	{
		for (UEdGraphNode* N : Graph->Nodes)
			if (UAnimGraphNode_TransitionResult* TR = Cast<UAnimGraphNode_TransitionResult>(N))
				{ ResultNode = TR; break; }
	}
	if (!ResultNode)
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: no TransitionResult node found in graph"));

	// Find the bool input pin
	UEdGraphPin* BoolInputPin = nullptr;
	for (UEdGraphPin* Pin : ResultNode->Pins)
		if (Pin && Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			{ BoolInputPin = Pin; break; }

	if (!BoolInputPin)
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: TransitionResult has no bool input pin"));

	// Build owner Blueprint reference for function lookup
	UBlueprint* OwnerBP = Graph->GetTypedOuter<UBlueprint>();

	// Build the pure expression tree
	FLispNodePtr CondExpr = TopExpr->Get(1);
	TArray<UEdGraphNode*> CreatedNodes;
	TArray<FString> PureExprErrors;
	FString LiteralVal;
	UEdGraphPin* OutputPin = BuildPureExprNode(CondExpr, Graph, OwnerBP, CreatedNodes, LiteralVal, &PureExprErrors);


	if (OutputPin)
	{
		// Break any existing links on the bool pin
		BoolInputPin->BreakAllPinLinks();
		FString ConnectionError;
		if (!IMP_TryCreateConnection(Graph, OutputPin, BoolInputPin, &ConnectionError))
		{
			return FBlueprintLispResult::Fail(FString::Printf(TEXT("ImportGraph: %s"), *ConnectionError));
		}
	}
	else if (!LiteralVal.IsEmpty())
	{
		BoolInputPin->DefaultValue = LiteralVal;
	}
	else
	{
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: condition expression produced no output pin"));
	}

	if (PureExprErrors.Num() > 0)
	{
		return FBlueprintLispResult::Fail(FString::Join(PureExprErrors, TEXT("\n")));
	}

	// Simple auto-layout: place created nodes to the left of ResultNode

	float X = ResultNode->NodePosX - 300.0f;
	for (int32 i = 0; i < CreatedNodes.Num(); i++)
	{
		CreatedNodes[i]->NodePosX = X;
		CreatedNodes[i]->NodePosY = ResultNode->NodePosY + (i - CreatedNodes.Num() / 2) * 80.0f;
		X -= 220.0f;
	}

	// Reconstruct all created nodes to resolve wildcard types (e.g. EnumEquality pin types)
	// This must happen AFTER all connections are made so type propagation can flow correctly.
	for (UEdGraphNode* N : CreatedNodes)
		if (N) N->ReconstructNode();

	FBPImportContext TransitionCtx;
	TransitionCtx.Blueprint = OwnerBP;
	TransitionCtx.Graph = Graph;
	TransitionCtx.Errors.Append(PureExprErrors);

	if (OwnerBP)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(OwnerBP);

		TArray<BlueprintLispImportLifecycle::FImportNodeChange> NodeChanges;
		IMP_CollectNodeChanges(PreExistingNodes, Graph, NodeChanges);
		IMP_BroadcastNodePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostNodeChanges, LifecycleContext, NodeChanges);
		IMP_BroadcastPropertyPhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostPropertyChanges, LifecycleContext, PropertyChanges);

		if (Options.bCompile)
		{
			IMP_BroadcastFinalizePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PreFinalize, LifecycleContext);
			FKismetEditorUtilities::CompileBlueprint(OwnerBP, EBlueprintCompileOptions::SkipGarbageCollection);
			IMP_RecordCompileStatus(OwnerBP, TransitionCtx, TEXT("ImportGraph(transition-cond)"));
			IMP_BroadcastFinalizePhase(BlueprintLispImportLifecycle::EImportLifecyclePhase::PostFinalize, LifecycleContext);
		}
	}

	if (TransitionCtx.Errors.Num() > 0)
	{
		return IMP_FailFromContext(TransitionCtx, TEXT("ImportGraph transition condition import failed"));
	}

	UE_LOG(LogBlueprintLisp, Log, TEXT("ImportGraph: restored transition condition (%d nodes created)"), CreatedNodes.Num());
	return IMP_OkFromContext(LispCode, TransitionCtx);

}

FBlueprintLispResult FBlueprintLispConverter::ImportPureExpression(
	UEdGraph* Graph,
	UEdGraphPin* TargetPin,
	const FString& LispCode)
{
	if (!Graph || !TargetPin || TargetPin->GetOwningNode()->GetGraph() != Graph)
	{
		return FBlueprintLispResult::Fail(TEXT("ImportPureExpression: graph and target pin are required"));
	}
	if (TargetPin->Direction != EGPD_Input)
	{
		return FBlueprintLispResult::Fail(TEXT("ImportPureExpression: target pin must be an input"));
	}

	const FLispParseResult ParseResult = FLispParser::Parse(LispCode);
	if (!ParseResult.bSuccess || ParseResult.Nodes.Num() != 1 || !ParseResult.Nodes[0].IsValid())
	{
		return FBlueprintLispResult::Fail(ParseResult.bSuccess
			? TEXT("ImportPureExpression: expected exactly one expression")
			: ParseResult.Error);
	}

	FBPImportContext Ctx;
	Ctx.Blueprint = Graph->GetTypedOuter<UBlueprint>();
	Ctx.Graph = Graph;
	TargetPin->BreakAllPinLinks();
	if (!IMP_SetPinFromExpr(TargetPin, ParseResult.Nodes[0], Ctx) || !Ctx.Errors.IsEmpty())
	{
		return IMP_FailFromContext(Ctx, TEXT("ImportPureExpression failed"));
	}
	if (Ctx.Blueprint) FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);
	return IMP_OkFromContext(LispCode, Ctx);
}

FBlueprintLispResult FBlueprintLispConverter::ImportByPath(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& LispCode,
	const FImportOptions& Options)
{
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!BP)
		return FBlueprintLispResult::Fail(FString::Printf(TEXT("Failed to load Blueprint: %s"), *BlueprintPath));
	return Import(BP, GraphName, LispCode, Options);
}

FBlueprintLispResult FBlueprintLispConverter::Update(
	UBlueprint* /*Blueprint*/,
	const FString& /*GraphName*/,
	const FString& /*NewLispCode*/,
	const FUpdateOptions& /*Options*/)
{
	// TODO: Incremental update via semantic diff is not yet implemented.
	return FBlueprintLispResult::Fail(TEXT("BlueprintLisp incremental Update is not yet implemented."));
}

UEdGraph* FBlueprintLispConverter::FindOrCreateGraph(UBlueprint* BP, const FString& GraphName)
{
	if (!BP) return nullptr;
	for (UEdGraph* G : BP->UbergraphPages)
		if (G && G->GetName() == GraphName) return G;
	for (UEdGraph* G : BP->FunctionGraphs)
		if (G && G->GetName() == GraphName) return G;
	return nullptr;
}

#endif // WITH_EDITOR
