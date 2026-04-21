// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// BlueprintLispConverter.cpp - Blueprint EventGraph <-> BlueprintLisp DSL
//
// Export logic derived from ECABridge/ECABlueprintLispCommands.cpp (Epic Games, Experimental)
// Original author: Jon Olick
//
// This file implements the public FBlueprintLispConverter API.
// Import (DSL->BP) is currently stubbed; Export (BP->DSL) is fully implemented.

#include "BlueprintLispConverter.h"

#if WITH_EDITOR

#include "BlueprintLispAST.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
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

#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
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
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_FunctionTerminator.h"
#include "K2Node_EnumEquality.h"
#include "K2Node_EnumInequality.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectIterator.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "Engine/LevelScriptBlueprint.h"
#include "UObject/UnrealType.h"


#include "AnimGraphNode_TransitionResult.h"
#include "AnimationTransitionGraph.h"

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

// Forward declarations
static FLispNodePtr ConvertPureExpressionToLisp(UEdGraphPin* ValuePin, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited);
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
	FString Cat = PT.PinCategory.ToString();
	if (Cat == TEXT("bool"))   return TEXT("bool");
	if (Cat == TEXT("int"))    return TEXT("int");
	if (Cat == TEXT("int64"))  return TEXT("int64");
	if (Cat == TEXT("float") || Cat == TEXT("real") || Cat == TEXT("double")) return TEXT("float");
	if (Cat == TEXT("string")) return TEXT("string");
	if (Cat == TEXT("name"))   return TEXT("name");
	if (Cat == TEXT("text"))   return TEXT("text");
	if (Cat == TEXT("struct"))
	{
		if (PT.PinSubCategoryObject.IsValid())
			return PT.PinSubCategoryObject->GetName().ToLower();
		return TEXT("struct");
	}
	if (Cat == TEXT("object") || Cat == TEXT("class"))
	{
		if (PT.PinSubCategoryObject.IsValid())
			return PT.PinSubCategoryObject->GetName();
		return TEXT("object");
	}
	return Cat.ToLower();
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
		return SourcePin->PinName.ToString();
	}

	return TEXT("");
}

static FLispNodePtr EXP_MakeNameAtom(const FString& Name)
{
	const bool bNeedsQuoting = Name.Contains(TEXT(" ")) || Name.Contains(TEXT("\t"));
	return bNeedsQuoting ? FLispNode::MakeString(Name) : FLispNode::MakeSymbol(Name);
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

	return EXP_StripGeneratedPinSuffixes(Pin->PinName.ToString()).ToLower();
}

static void EXP_AppendMacroOutputDeclaration(TArray<FLispNodePtr>& Args, UEdGraphPin* Pin)
{
	if (!Pin || Pin->Direction != EGPD_Output) return;
	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) return;
	if (Pin->bHidden) return;

	Args.Add(FLispNode::MakeKeyword(TEXT(":out")));
	TArray<FLispNodePtr> OutPair;
	OutPair.Add(FLispNode::MakeSymbol(Pin->PinName.ToString()));
	OutPair.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
	Args.Add(FLispNode::MakeList(OutPair));
}

static FLispNodePtr EXP_BuildMacroCallForm(UK2Node_MacroInstance* MacroInst, UEdGraphPin* SelectedOutputPin, bool bIncludeAllOutputs, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited)
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
		FLispNodePtr Val = ConvertPureExpressionToLisp(Pin, Graph, Visited);
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

	return FLispNode::MakeList(Args);
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

static FLispNodePtr ConvertPureExpressionToLisp(UEdGraphPin* ValuePin, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited)
{

	if (!ValuePin || ValuePin->LinkedTo.Num() == 0)
	{
		// Return default value as literal
		if (!ValuePin || ValuePin->DefaultValue.IsEmpty()) return FLispNode::MakeNil();
		double Num = 0;
		if (ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			return FLispNode::MakeSymbol(ValuePin->DefaultValue.ToLower() == TEXT("true") ? TEXT("true") : TEXT("false"));
		if (ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int
			|| ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Float
			|| ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Double
			|| ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			if (LexTryParseString(Num, *ValuePin->DefaultValue))
				return FLispNode::MakeNumber(Num);
		}
		if (!ValuePin->DefaultValue.IsEmpty())
			return FLispNode::MakeString(ValuePin->DefaultValue);
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
			return ConvertPureExpressionToLisp(KnotInputPin, Graph, Visited);
		}
	}

	// Variable get

	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(SourceNode))

	{
		FString VarName = VarGet->VariableReference.GetMemberName().ToString();
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


		FString FuncName = GetCleanNodeName(SourceNode);
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(FuncName));


		// Target object
		UEdGraphPin* SelfPin = SourceNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
		if (SelfPin && SelfPin->LinkedTo.Num() > 0)
			Args.Add(ConvertPureExpressionToLisp(SelfPin, Graph, Visited));

		// Input data pins
		for (UEdGraphPin* Pin : SourceNode->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
			Args.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited));
		}
		Visited.Remove(SourceNode);
		return FLispNode::MakeList(Args);
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
		FLispNodePtr MacroForm = EXP_BuildMacroCallForm(MacroInst, SourcePin, false, Graph, Visited);
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
			Args.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited));
		}
		Visited.Remove(SourceNode);
		return FLispNode::MakeList(Args);
	}

	if (UK2Node_GetArrayItem* GetArrayItemNode = Cast<UK2Node_GetArrayItem>(SourceNode))
	{
		if (Visited.Contains(SourceNode)) return FLispNode::MakeSymbol(TEXT("...circular..."));
		Visited.Add(SourceNode);

		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("get-array-item")));
		if (UEdGraphPin* ArrayPin = GetArrayItemNode->GetTargetArrayPin())
		{
			Args.Add(ConvertPureExpressionToLisp(ArrayPin, Graph, Visited));
		}
		if (UEdGraphPin* IndexPin = GetArrayItemNode->GetIndexPin())
		{
			Args.Add(ConvertPureExpressionToLisp(IndexPin, Graph, Visited));
		}

		Visited.Remove(SourceNode);
		return FLispNode::MakeList(Args);
	}

	if (UK2Node_BreakStruct* BreakStructNode = Cast<UK2Node_BreakStruct>(SourceNode))
	{
		if (Visited.Contains(SourceNode)) return FLispNode::MakeSymbol(TEXT("...circular..."));
		Visited.Add(SourceNode);

		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("break-struct")));
		Args.Add(FLispNode::MakeKeyword(TEXT(":struct")));
		Args.Add(EXP_MakeNameAtom(BreakStructNode->StructType ? BreakStructNode->StructType->GetName() : BreakStructNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));

		for (UEdGraphPin* Pin : BreakStructNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			Args.Add(FLispNode::MakeKeyword(TEXT(":value")));
			Args.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited));
			break;
		}

		Args.Add(FLispNode::MakeKeyword(TEXT(":field")));
		Args.Add(EXP_MakeNameAtom(SourcePin->PinName.ToString()));
		Visited.Remove(SourceNode);
		return FLispNode::MakeList(Args);
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
				Args.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited));
			}
			Visited.Remove(SourceNode);
			return FLispNode::MakeList(Args);
		}
	}


	// Fallback for non-pure nodes: return node class name as opaque symbol
	{
		FString ClassName = SourceNode->GetClass()->GetName();
		return FLispNode::MakeSymbol(ClassName);
	}
}

// ----- Convert a single exec node to Lisp -----
static FLispNodePtr ConvertNodeToLisp(UEdGraphNode* Node, UEdGraph* Graph, TSet<UEdGraphNode*>& Visited, bool bPositions, const TMap<FGuid, FString>& ShortIds)
{
	if (!Node) return FLispNode::MakeNil();
	if (Visited.Contains(Node)) return FLispNode::MakeNil();
	Visited.Add(Node);

	// ---- branch ----
	if (UK2Node_IfThenElse* BranchNode = Cast<UK2Node_IfThenElse>(Node))
	{
		UEdGraphPin* CondPin  = BranchNode->GetConditionPin();
		UEdGraphPin* TruePin  = BranchNode->GetThenPin();
		UEdGraphPin* FalsePin = BranchNode->GetElsePin();

		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("branch")));
		Args.Add(ConvertPureExpressionToLisp(CondPin, Graph, Visited));
		Args.Add(FLispNode::MakeKeyword(TEXT(":true")));
		Args.Add(ConvertExecChainToLisp(TruePin, Graph, Visited, bPositions, ShortIds));
		Args.Add(FLispNode::MakeKeyword(TEXT(":false")));
		Args.Add(ConvertExecChainToLisp(FalsePin, Graph, Visited, bPositions, ShortIds));
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
		Args.Add(FLispNode::MakeSymbol(TEXT("set")));
		Args.Add(EXP_MakeNameAtom(VarName));
		Args.Add(ConvertPureExpressionToLisp(ValuePin, Graph, Visited));

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
					FLispNodePtr Val = ConvertPureExpressionToLisp(Pin, Graph, Visited);
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


		// Target object (self pin)
		UEdGraphPin* SelfPin = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
		if (SelfPin && SelfPin->LinkedTo.Num() > 0)
			Args.Add(ConvertPureExpressionToLisp(SelfPin, Graph, Visited));

		// Input data pins
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
			if (Pin->bHidden) continue;
			FLispNodePtr Val = ConvertPureExpressionToLisp(Pin, Graph, Visited);
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
				if (Body->IsForm(TEXT("seq")))
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
		Args.Add(ConvertPureExpressionToLisp(ObjPin, Graph, Visited));
		FLispNodePtr SuccBody = ConvertExecChainToLisp(SuccessPin, Graph, Visited, bPositions, ShortIds);
		if (SuccBody.IsValid() && !SuccBody->IsNil()) Args.Add(SuccBody);
		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}

	// ---- switch integer ----
	if (UK2Node_SwitchInteger* SwitchInt = Cast<UK2Node_SwitchInteger>(Node))
	{
		UEdGraphPin* SelPin = SwitchInt->GetSelectionPin();
		TArray<FLispNodePtr> Args;
		Args.Add(FLispNode::MakeSymbol(TEXT("switch-int")));
		Args.Add(ConvertPureExpressionToLisp(SelPin, Graph, Visited));
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
		return AppendNodeId(EXP_BuildMacroCallForm(MacroInst, nullptr, true, Graph, Visited), Node, ShortIds);
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
					OutPair.Add(ConvertPureExpressionToLisp(Pin, Graph, Visited));

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

		// Skip entry tunnel nodes (they are handled as macro entry points, not as chain nodes)
		// But MacroInstance (which inherits Tunnel) should NOT be skipped — it's a call node.
		if (UK2Node_Tunnel* TE = Cast<UK2Node_Tunnel>(NextNode))
		{
			if (TE->DrawNodeAsEntry() && !Cast<UK2Node_MacroInstance>(NextNode)) break;
		}

		FLispNodePtr NodeLisp = ConvertNodeToLisp(NextNode, Graph, Visited, bPositions, ShortIds);
		if (NodeLisp.IsValid() && !NodeLisp->IsNil())
			Statements.Add(NodeLisp);

		// branch terminates the chain (branches handled inside ConvertNodeToLisp)
		if (Cast<UK2Node_IfThenElse>(NextNode)) break;

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

	if (Body->IsForm(TEXT("seq")))
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

static FLispNodePtr ConvertComponentBoundEventToLisp(UK2Node_ComponentBoundEvent* Event, UEdGraph* Graph, bool bPositions,
	const TMap<FGuid, FString>& ShortEventIds, const TMap<FGuid, FString>& ShortNodeIds)
{
	FString ComponentName = Event->GetComponentPropertyName().ToString();
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
		if (Body->IsForm(TEXT("seq")))
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
					&& !Pin->bHidden && Pin->PinName != UEdGraphSchema_K2::PN_Execute)
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

	// Exec output -> body
	UEdGraphPin* ThenPin = GetThenPin(FuncEntry);
	FLispNodePtr Body = ConvertExecChainToLisp(ThenPin, Graph, Visited, bPositions, ShortNodeIds);
	if (Body.IsValid() && !Body->IsNil())
	{
		if (Body->IsForm(TEXT("seq")))
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
		if (Body->IsForm(TEXT("seq")))
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

// Forward decls (Import helpers)

static UEdGraphPin* IMP_ResolveLispExpr(const FLispNodePtr& Expr, FBPImportContext& Ctx);
static UEdGraphNode* IMP_ConvertFormToNode(const FLispNodePtr& Form, FBPImportContext& Ctx, UEdGraphPin*& OutExecPin);
static void IMP_ConvertExecBody(const FLispNodePtr& Body, FBPImportContext& Ctx, UEdGraphPin*& CurrentExecPin);
static bool IMP_SetPinFromExpr(UEdGraphPin* Pin, const FLispNodePtr& Expr, FBPImportContext& Ctx);
static bool IMP_Connect(UEdGraphPin* Src, UEdGraphPin* Dst, FBPImportContext& Ctx);
static FString IMP_ExtractCallMacroName(const FLispNodePtr& Form, int32& OutArgStartIndex);
static UK2Node_MacroInstance* IMP_CreateMacroInstanceNode(const FLispNodePtr& Form, FBPImportContext& Ctx, UEdGraphPin*& OutPreferredOutputPin);
static bool IMP_ExtractBindingNameAndValueIndex(const FLispNodePtr& Form, int32 StartIndex, FString& OutName, int32& OutValueIndex);
static UScriptStruct* IMP_FindStructByName(const FString& StructName, FBPImportContext& Ctx);
static FString IMP_GetAtomName(const FLispNodePtr& Node);
static void IMP_EnsureGuid(UEdGraphNode* N);
static UEdGraphPin* IMP_GetExecOutput(UEdGraphNode* N);
static void IMP_CollectDownstreamExecNodes(UEdGraphPin* ExecOutPin, TSet<UEdGraphNode*>& OutNodes);



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

static void IMP_ResetReusableBodyNodePool(FBPImportContext& Ctx)
{
	Ctx.ReusableBodyNodes.Reset();
	Ctx.ReusableBodyStableIdToNode.Reset();
	Ctx.ConsumedReusableBodyGuids.Reset();
}

static void IMP_PrepareExistingEventBodyForIncrementalReuse(UK2Node_Event* EventNode, FBPImportContext& Ctx)
{
	IMP_ResetReusableBodyNodePool(Ctx);
	if (!EventNode || !Ctx.Graph)
	{
		return;
	}

	TSet<UEdGraphNode*> NodesToReuse;
	if (UEdGraphPin* ExecOutPin = IMP_GetExecOutput(EventNode))
	{
		IMP_CollectDownstreamExecNodes(ExecOutPin, NodesToReuse);
		ExecOutPin->BreakAllPinLinks();
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

static bool IMP_IsCompatibleExistingVariableSetNode(UK2Node_VariableSet* VariableSetNode, const FString& RequestedVariableName)
{
	if (!VariableSetNode)
	{
		return false;
	}

	return VariableSetNode->VariableReference.GetMemberName().ToString().Equals(RequestedVariableName, ESearchCase::IgnoreCase);
}

static UK2Node_VariableSet* IMP_CreateOrReuseVariableSetNode(const FLispNodePtr& Form, const FString& VariableName, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_VariableSet* ReusableSetNode = Cast<UK2Node_VariableSet>(ReusableNode))
		{
			if (IMP_IsCompatibleExistingVariableSetNode(ReusableSetNode, VariableName))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableSetNode, Ctx);
				Ctx.AdvancePosition();
				return ReusableSetNode;
			}
		}
	}

	UK2Node_VariableSet* VariableSetNode = NewObject<UK2Node_VariableSet>(Ctx.Graph);
	VariableSetNode->VariableReference.SetSelfMember(FName(*VariableName));
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

static bool IMP_IsCompatibleExistingCallNode(UK2Node_CallFunction* CallNode, const FString& RequestedFunctionName, bool bUseSelfMemberReference)
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
	return bExistingUsesSelfMemberReference == bUseSelfMemberReference;
}

static UK2Node_CallFunction* IMP_CreateOrReuseCallFunctionNode(const FLispNodePtr& Form, UFunction* Function, const FString& RequestedFunctionName, bool bUseSelfMemberReference, FBPImportContext& Ctx)
{
	if (UEdGraphNode* ReusableNode = IMP_FindReusableBodyNodeByStableId(Form, Ctx))
	{
		if (UK2Node_CallFunction* ReusableCallNode = Cast<UK2Node_CallFunction>(ReusableNode))
		{
			if (IMP_IsCompatibleExistingCallNode(ReusableCallNode, RequestedFunctionName, bUseSelfMemberReference))
			{
				IMP_MarkReusableBodyNodeConsumed(ReusableCallNode, Ctx);
				Ctx.AdvancePosition();
				return ReusableCallNode;
			}
		}
	}

	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Ctx.Graph);
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
	CallNode->AllocateDefaultPins();
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
			if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			IMP_CollectDownstreamExecNodes(Pin, OutNodes);
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
		*OutError = FString::Printf(
			TEXT("schema rejected connection %s.%s (%s) -> %s.%s (%s)"),
			*Src->GetOwningNode()->GetName(),
			*Src->PinName.ToString(),
			*Src->PinType.PinCategory.ToString(),
			*Dst->GetOwningNode()->GetName(),
			*Dst->PinName.ToString(),
			*Dst->PinType.PinCategory.ToString());
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
		|| KeywordName.Equals(TEXT("pos"), ESearchCase::IgnoreCase);
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
	if (bTreatFirstPositionalAsSelf && ArgIndex < Form->Num() && !Form->Get(ArgIndex)->IsKeyword())
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

			if (UEdGraphPin* InputPin = IMP_FindInputPin(CallNode, KeywordName))
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
		if (IMP_TryInferLiteralPinCategory(ItemExpr, InferredCategory)
			&& InferredCategory != UEdGraphSchema_K2::PC_Wildcard)
		{
			break;
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

static UEdGraphPin* IMP_TryBuildBreakStructOutputPin(const FLispNodePtr& Expr, UEdGraphPin* DestinationPin, FBPImportContext& Ctx, bool& bHandled)
{
	bHandled = false;
	if (!Expr.IsValid() || !Expr->IsList() || Expr->Num() == 0 || !DestinationPin)
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

	UK2Node_BreakStruct* BreakNode = NewObject<UK2Node_BreakStruct>(Ctx.Graph);
	BreakNode->StructType = StructType;
	BreakNode->NodePosX = Ctx.CurrentX;
	BreakNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(BreakNode, false, false);
	BreakNode->AllocateDefaultPins();
	IMP_EnsureGuid(BreakNode);
	Ctx.AdvancePosition();
	Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), BreakNode);

	UEdGraphPin* StructInputPin = nullptr;
	for (UEdGraphPin* Candidate : BreakNode->Pins)
	{
		if (!Candidate || Candidate->Direction != EGPD_Input) continue;
		if (Candidate->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
		StructInputPin = Candidate;
		break;
	}

	if (!StructInputPin)
	{
		Ctx.Errors.Add(FString::Printf(TEXT("IMP: break-struct input pin missing for %s"), *StructTypeName));
		return nullptr;
	}

	if (!IMP_SetPinFromExpr(StructInputPin, ValueExpr, Ctx))
	{
		return nullptr;
	}

	UEdGraphPin* SelectedOutputPin = IMP_SelectBestDataOutputPinForDestination(BreakNode, DestinationPin, Ctx.Graph, PreferredFieldName);
	if (!SelectedOutputPin)
	{
		const FString OwnerNodeTitle = DestinationPin->GetOwningNode() ? DestinationPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("<null node>");
		Ctx.Errors.Add(FString::Printf(
			TEXT("IMP: break-struct '%s' has no output compatible with pin %s.%s"),
			*StructTypeName,
			*OwnerNodeTitle,
			*DestinationPin->PinName.ToString()));
		return nullptr;
	}

	IMP_RegisterBoundValue(SelectedOutputPin->PinName.ToString(), SelectedOutputPin, Ctx);
	return SelectedOutputPin;
}

// Set a pin's default value from a Lisp expression (number, string, bool, or connected expr)
static bool IMP_SetPinFromExpr(UEdGraphPin* Pin, const FLispNodePtr& Expr, FBPImportContext& Ctx)


{
	if (!Pin || !Expr.IsValid()) return false;

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
	if (Src) { return IMP_Connect(Src, Pin, Ctx); }

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

static bool IMP_BuildPinTypeFromLispType(const FString& TypeName, FEdGraphPinType& OutPinType, FBPImportContext& Ctx)
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

	if (UClass* ResolvedClass = IMP_FindClassByName(Normalized, Ctx))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		OutPinType.PinSubCategoryObject = ResolvedClass;
		return true;
	}

	return false;
}


static void IMP_EnsureFunctionEntryParamsFromFunctionForm(UK2Node_FunctionEntry* ExistingEntry, const FLispNodePtr& Form, FBPImportContext& Ctx)
{
	if (!ExistingEntry || !Form.IsValid()) return;

	int32 ArgStartIndex = 2;
	IMP_ExtractCompoundName(Form, 1, ArgStartIndex);

	bool bChanged = false;
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

			bool bAlreadyExists = false;
			for (UEdGraphPin* ExistingPin : ExistingEntry->Pins)
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
				Ctx.Errors.Add(FString::Printf(TEXT("Import function form failed: unsupported parameter type '%s'"), *ParamType));
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

	UK2Node_InputAction* InputNode = NewObject<UK2Node_InputAction>(Ctx.Graph);
	InputNode->InputActionName = FName(*ActionName);
	if (Form->HasKeyword(TEXT(":consume-input"))) InputNode->bConsumeInput = IMP_IsTruthy(Form->GetKeywordArg(TEXT(":consume-input")));
	if (Form->HasKeyword(TEXT(":execute-when-paused"))) InputNode->bExecuteWhenPaused = IMP_IsTruthy(Form->GetKeywordArg(TEXT(":execute-when-paused")));
	if (Form->HasKeyword(TEXT(":override-parent-binding"))) InputNode->bOverrideParentBinding = IMP_IsTruthy(Form->GetKeywordArg(TEXT(":override-parent-binding")));
	InputNode->NodePosX = Ctx.CurrentX;
	InputNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(InputNode, false, false);
	InputNode->AllocateDefaultPins();
	IMP_EnsureGuid(InputNode);
	Ctx.AdvancePosition();
	IMP_RegisterEventOutputPins(InputNode, Ctx);

	UEdGraphPin* PressedPin = InputNode->GetPressedPin();
	IMP_ConvertExecBody(PressedBody, Ctx, PressedPin);
	UEdGraphPin* ReleasedPin = InputNode->GetReleasedPin();
	IMP_ConvertExecBody(ReleasedBody, Ctx, ReleasedPin);
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

	UK2Node_InputKey* InputNode = NewObject<UK2Node_InputKey>(Ctx.Graph);
	InputNode->InputKey = FKey(*KeyName);
	if (Form->HasKeyword(TEXT(":consume-input"))) InputNode->bConsumeInput = IMP_IsTruthy(Form->GetKeywordArg(TEXT(":consume-input")));
	if (Form->HasKeyword(TEXT(":execute-when-paused"))) InputNode->bExecuteWhenPaused = IMP_IsTruthy(Form->GetKeywordArg(TEXT(":execute-when-paused")));
	if (Form->HasKeyword(TEXT(":override-parent-binding"))) InputNode->bOverrideParentBinding = IMP_IsTruthy(Form->GetKeywordArg(TEXT(":override-parent-binding")));
	if (Form->HasKeyword(TEXT(":control"))) InputNode->bControl = IMP_IsTruthy(Form->GetKeywordArg(TEXT(":control")));
	if (Form->HasKeyword(TEXT(":ctrl"))) InputNode->bControl = IMP_IsTruthy(Form->GetKeywordArg(TEXT(":ctrl")));
	if (Form->HasKeyword(TEXT(":alt"))) InputNode->bAlt = IMP_IsTruthy(Form->GetKeywordArg(TEXT(":alt")));
	if (Form->HasKeyword(TEXT(":shift"))) InputNode->bShift = IMP_IsTruthy(Form->GetKeywordArg(TEXT(":shift")));
	if (Form->HasKeyword(TEXT(":command"))) InputNode->bCommand = IMP_IsTruthy(Form->GetKeywordArg(TEXT(":command")));
	InputNode->NodePosX = Ctx.CurrentX;
	InputNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(InputNode, false, false);
	InputNode->AllocateDefaultPins();
	IMP_EnsureGuid(InputNode);
	Ctx.AdvancePosition();
	IMP_RegisterEventOutputPins(InputNode, Ctx);

	UEdGraphPin* PressedPin = InputNode->GetPressedPin();
	IMP_ConvertExecBody(PressedBody, Ctx, PressedPin);
	UEdGraphPin* ReleasedPin = InputNode->GetReleasedPin();
	IMP_ConvertExecBody(ReleasedBody, Ctx, ReleasedPin);
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

	UK2Node_ComponentBoundEvent* EventNode = NewObject<UK2Node_ComponentBoundEvent>(Ctx.Graph);
	EventNode->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
	EventNode->NodePosX = Ctx.CurrentX;
	EventNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(EventNode, false, false);
	EventNode->AllocateDefaultPins();
	IMP_EnsureGuid(EventNode);
	Ctx.AdvancePosition();
	IMP_RegisterEventOutputPins(EventNode, Ctx);

	UEdGraphPin* CurrentExecPin = IMP_GetExecOutput(EventNode);
	FLispNodePtr Body = IMP_MakeSeqBody(BodyStatements);
	IMP_ConvertExecBody(Body, Ctx, CurrentExecPin);
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

	UK2Node_ActorBoundEvent* EventNode = NewObject<UK2Node_ActorBoundEvent>(Ctx.Graph);
	EventNode->InitializeActorBoundEventParams(TargetActor, DelegateProperty);
	EventNode->NodePosX = Ctx.CurrentX;
	EventNode->NodePosY = Ctx.CurrentY;
	Ctx.Graph->AddNode(EventNode, false, false);
	EventNode->AllocateDefaultPins();
	IMP_EnsureGuid(EventNode);
	Ctx.AdvancePosition();
	IMP_RegisterEventOutputPins(EventNode, Ctx);

	UEdGraphPin* CurrentExecPin = IMP_GetExecOutput(EventNode);
	FLispNodePtr Body = IMP_MakeSeqBody(BodyStatements);
	IMP_ConvertExecBody(Body, Ctx, CurrentExecPin);
	Ctx.NewRow();
}

// --- Resolve pure expression → output pin ---
static UEdGraphPin* IMP_ResolveLispExpr(const FLispNodePtr& Expr, FBPImportContext& Ctx)
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

		// Fallback: create variable get
		UK2Node_VariableGet* VarGet = NewObject<UK2Node_VariableGet>(Ctx.Graph);
		VarGet->VariableReference.SetSelfMember(FName(*Sym));
		VarGet->NodePosX = Ctx.CurrentX; VarGet->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(VarGet, false, false); VarGet->AllocateDefaultPins(); IMP_EnsureGuid(VarGet);
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), VarGet);
		return IMP_FindOutputPin(VarGet, TEXT(""));
	}


	if (!Expr->IsList() || Expr->Num() == 0) return nullptr;

	FString FormName = Expr->GetFormName();

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


	if (FormName.Equals(TEXT("make-array"), ESearchCase::IgnoreCase))
	{
		UK2Node_MakeArray* MakeArrayNode = NewObject<UK2Node_MakeArray>(Ctx.Graph);
		MakeArrayNode->NodePosX = Ctx.CurrentX; MakeArrayNode->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(MakeArrayNode, false, false); MakeArrayNode->AllocateDefaultPins(); IMP_EnsureGuid(MakeArrayNode);

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
			IMP_SetPinFromExpr(InputPins[Index], ItemExprs[Index], Ctx);
		}


		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), MakeArrayNode);
		return MakeArrayNode->GetOutputPin();
	}

	if (FormName.Equals(TEXT("get-array-item"), ESearchCase::IgnoreCase))
	{
		UK2Node_GetArrayItem* GetArrayItemNode = NewObject<UK2Node_GetArrayItem>(Ctx.Graph);
		GetArrayItemNode->NodePosX = Ctx.CurrentX; GetArrayItemNode->NodePosY = Ctx.CurrentY;

		Ctx.Graph->AddNode(GetArrayItemNode, false, false); GetArrayItemNode->AllocateDefaultPins(); IMP_EnsureGuid(GetArrayItemNode);

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

		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), GetArrayItemNode);
		return GetArrayItemNode->GetResultPin();
	}

	// Generic function call / pure expr: (FuncName [self] [:pin value]...)
	int32 CompoundArgStartIndex = 1;
	const FString CompoundFormName = IMP_ExtractCompoundName(Expr, 0, CompoundArgStartIndex);
	if (Expr->Num() == 1 && CompoundFormName.StartsWith(TEXT("self."), ESearchCase::IgnoreCase))
	{
		const FString MemberName = CompoundFormName.Mid(5);
		UK2Node_VariableGet* VarGet = NewObject<UK2Node_VariableGet>(Ctx.Graph);
		VarGet->VariableReference.SetSelfMember(FName(*MemberName));
		VarGet->NodePosX = Ctx.CurrentX; VarGet->NodePosY = Ctx.CurrentY;
		Ctx.Graph->AddNode(VarGet, false, false); VarGet->AllocateDefaultPins(); IMP_EnsureGuid(VarGet);
		Ctx.TempIdToNode.Add(Ctx.GenerateTempId(), VarGet);
		return IMP_FindOutputPin(VarGet, TEXT(""));
	}

	FString CompoundBindingName;
	int32 CompoundBindingValueIndex = INDEX_NONE;
	if (IMP_ExtractBindingNameAndValueIndex(Expr, 0, CompoundBindingName, CompoundBindingValueIndex))
	{
		if (UFunction* CompoundFunc = IMP_FindFunction(CompoundBindingName, Ctx))
		{
			UK2Node_CallFunction* CN = IMP_CreateOrReuseCallFunctionNode(Expr, CompoundFunc, CompoundBindingName, false, Ctx);
			IMP_ApplyCallInputs(CN, Expr, CompoundBindingValueIndex, true, Ctx);
			return IMP_FindOutputPin(CN, TEXT("ReturnValue"));
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
		if (UFunction* F = IMP_FindFunction(PrimaryFormName, Ctx))
		{
			UK2Node_CallFunction* CN = IMP_CreateOrReuseCallFunctionNode(Expr, F, PrimaryFormName, false, Ctx);
			IMP_ApplyCallInputs(CN, Expr, PrimaryArgStartIndex, true, Ctx);
			return IMP_FindOutputPin(CN, TEXT("ReturnValue"));
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

// --- Convert a single executable form → K2Node ---
static UEdGraphNode* IMP_ConvertFormToNode(const FLispNodePtr& Form, FBPImportContext& Ctx, UEdGraphPin*& OutExecPin)
{
	OutExecPin = nullptr;
	if (!Form.IsValid() || !Form->IsList() || Form->Num() == 0) return nullptr;

	FString FormName = Form->GetFormName();

	// (seq s1 s2 ...) — execute in order
	if (FormName.Equals(TEXT("seq"), ESearchCase::IgnoreCase))
	{
		UEdGraphNode* First = nullptr;
		UEdGraphPin* CurExec = nullptr;
		for (int32 i = 1; i < Form->Num(); i++)
		{
			UEdGraphPin* StmtOut = nullptr;
			UEdGraphNode* SN = IMP_ConvertFormToNode(Form->Get(i), Ctx, StmtOut);
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

	// (branch cond :true body :false body)
	if (FormName.Equals(TEXT("branch"), ESearchCase::IgnoreCase))
	{
		UK2Node_IfThenElse* BN = IMP_CreateOrReuseBranchNode(Form, Ctx);
		if (Form->Num() > 1)

		{
			UEdGraphPin* CondPin = BN->GetConditionPin();
			UEdGraphPin* CondSrc = IMP_ResolveLispExpr(Form->Get(1), Ctx);
			if (CondSrc && CondPin)
			{
				IMP_Connect(CondSrc, CondPin, Ctx);
			}
			else
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
			if (UEdGraphPin* CasePin = IMP_FindOutputPin(SwitchNode, FString::FromInt(CaseBody.Key)))
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
			if (UEdGraphPin* CasePin = IMP_FindOutputPin(SwitchNode, CaseBody.Key))
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
		SwitchNode->SetEnum(TargetEnum);
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
		for (const TPair<FString, FLispNodePtr>& CaseBody : CaseBodies)
		{
			FString CaseLabel = CaseBody.Key;
			if (CaseLabel.Contains(TEXT("::")))
			{
				CaseLabel = CaseLabel.RightChop(CaseLabel.Find(TEXT("::")) + 2);
			}
			if (UEdGraphPin* CasePin = IMP_FindOutputPin(SwitchNode, CaseLabel))
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
			BoundNode = IMP_ConvertFormToNode(ExprNode, Ctx, BoundExecOut);
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
		UK2Node_Tunnel* ExitTunnel = IMP_FindMacroExitTunnel(Ctx.Graph, ExitName);
		if (!ExitTunnel)
		{
			Ctx.Errors.Add(FString::Printf(TEXT("IMP: macro exit tunnel not found: %s"), *ExitName));

			return nullptr;
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

		UFunction* F = IMP_FindFunction(FuncName, Ctx);
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
		if (UFunction* F = IMP_FindFunction(FormName, Ctx))
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



	Ctx.Errors.Add(FString::Printf(TEXT("IMP: unhandled form: %s"), *FormName));

	return nullptr;
}

// --- Convert exec body: single statement or (seq ...) ---
static void IMP_ConvertExecBody(const FLispNodePtr& Body, FBPImportContext& Ctx, UEdGraphPin*& CurrentExecPin)
{
	if (!Body.IsValid() || Body->IsNil()) return;

	if (Body->IsList() && Body->GetFormName().Equals(TEXT("seq"), ESearchCase::IgnoreCase))
	{
		for (int32 i = 1; i < Body->Num(); i++)
		{
			UEdGraphPin* StmtOut = nullptr;
			UEdGraphNode* SN = IMP_ConvertFormToNode(Body->Get(i), Ctx, StmtOut);
			if (SN && CurrentExecPin)
				if (UEdGraphPin* In = IMP_GetExecInput(SN)) IMP_Connect(CurrentExecPin, In, Ctx);
			IMP_UpdateCurrentExecPin(SN, StmtOut, CurrentExecPin);
		}
	}
	else
	{
		UEdGraphPin* StmtOut = nullptr;
		UEdGraphNode* SN = IMP_ConvertFormToNode(Body, Ctx, StmtOut);
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
		UEdGraphNode* SN = IMP_ConvertFormToNode(EventForm->Get(i), Ctx, StmtOut);
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
		// Find a matching pure UFunction by name
		UFunction* TargetFunc = nullptr;
		if (BP && BP->GeneratedClass)
			TargetFunc = BP->GeneratedClass->FindFunctionByName(FName(*Sym));
		if (!TargetFunc)
		{
			for (TObjectIterator<UFunction> It; It; ++It)
			{
				if (It->GetName() == Sym && It->HasAnyFunctionFlags(FUNC_BlueprintPure))
				{
					TargetFunc = *It;
					break;
				}
			}
		}

		if (!TargetFunc)
		{
			if (OutErrors)
			{
				OutErrors->Add(FString::Printf(TEXT("BuildPureExprNode: could not resolve pure UFunction '%s'"), *Sym));
			}
			return nullptr;
		}

		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
		CallNode->SetFromFunction(TargetFunc);

		CallNode->CreateNewGuid();
		CallNode->PostPlacedNewNode();
		CallNode->AllocateDefaultPins();
		Graph->AddNode(CallNode, false, false);
		CreatedNodes.Add(CallNode);

		// Connect arguments to input data pins (positional, skip keywords as delimiters)
		int32 ArgIdx = 1;
		for (UEdGraphPin* Pin : CallNode->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
			if (ArgIdx >= Expr->Num()) break;

			FLispNodePtr ArgExpr = Expr->Get(ArgIdx);
			// Skip keyword separators (:key) used for named args
			if (ArgExpr->IsKeyword())
			{
				ArgIdx++;
				if (ArgIdx >= Expr->Num()) break;
				ArgExpr = Expr->Get(ArgIdx);
			}

			FString LiteralVal;
			UEdGraphPin* ArgOutputPin = BuildPureExprNode(ArgExpr, Graph, BP, CreatedNodes, LiteralVal, OutErrors);
			if (ArgOutputPin)
			{
				IMP_RecordPureExprConnectionFailure(Graph, ArgOutputPin, Pin, FString::Printf(TEXT("BuildPureExprNode(%s arg connect)"), *Sym), OutErrors);
			}

			else if (!LiteralVal.IsEmpty())
				Pin->DefaultValue = LiteralVal;

			ArgIdx++;
		}

		// Return the first non-exec output pin
		for (UEdGraphPin* Pin : CallNode->Pins)
			if (Pin && Pin->Direction == EGPD_Output
				&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				return Pin;
		return nullptr;
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
		if (G && (G->GetName() == GraphName || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase)))
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


	if (Options.bFailOnUnsupportedForm && !IMP_ValidateImportCoverage(PR.Nodes, Ctx))
	{
		return FBlueprintLispResult::Fail(Ctx.Errors.Num() > 0 ? Ctx.Errors[0] : TEXT("Import aborted due to unsupported DSL forms"));
	}

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

				IMP_EnsureFunctionEntryParamsFromFunctionForm(ExistingEntry, Form, Ctx);
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
					UEdGraphNode* NewNode = IMP_ConvertFormToNode(Form->Get(i), Ctx, OutPin);
					if (CurrentExecPin && NewNode)
					{
						UEdGraphPin* InExec = IMP_GetExecInput(NewNode);
						if (InExec) IMP_Connect(CurrentExecPin, InExec, Ctx);
					}
					IMP_UpdateCurrentExecPin(NewNode, OutPin, CurrentExecPin);
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
					UEdGraphNode* NewNode = IMP_ConvertFormToNode(Form->Get(i), Ctx, OutPin);
					if (CurrentExecPin && NewNode)
					{
						UEdGraphPin* InExec = IMP_GetExecInput(NewNode);
						if (InExec) IMP_Connect(CurrentExecPin, InExec, Ctx);
					}
					IMP_UpdateCurrentExecPin(NewNode, OutPin, CurrentExecPin);
				}
				EventsCreated++;
			}
		}
		else

		{
			// other top-level forms: treat as anonymous exec body
			UEdGraphPin* ExecOut = nullptr;
			IMP_ConvertFormToNode(Form, Ctx, ExecOut);
		}
	}

	// Reconstruct all nodes to resolve wildcards
	for (UEdGraphNode* N : Graph->Nodes)
		if (N) N->ReconstructNode();

	// Mark blueprint modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// Optionally compile
	if (Options.bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		IMP_RecordCompileStatus(Blueprint, Ctx, TEXT("Import"));
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
	if (Options.bFailOnUnsupportedForm && !IMP_ValidateImportCoverage(PR.Nodes, ValidationCtx))
	{
		return FBlueprintLispResult::Fail(ValidationCtx.Errors.Num() > 0 ? ValidationCtx.Errors[0] : TEXT("ImportGraph aborted due to unsupported DSL forms"));
	}

	if (Options.ImportMode == FBlueprintLispConverter::EImportMode::ReplaceGraph)
	{
		IMP_ClearGraphForReplace(Graph, IMP_DetectGraphKind(Graph));
	}

	FLispNodePtr TopExpr = PR.Nodes[0];
	if (!TopExpr.IsValid() || !TopExpr->IsList())
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: top-level expression must be a list"));

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

		IMP_EnsureFunctionEntryParamsFromFunctionForm(ExistingEntry, TopExpr, Ctx);

		if (Ctx.Errors.Num() > 0)
		{
			return IMP_FailFromContext(Ctx, TEXT("ImportGraph function import failed"));
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

		int32 BodyStart = 2;
		for (int32 i = 2; i < TopExpr->Num(); i++)
		{
			if (TopExpr->Get(i)->IsKeyword()) { i++; BodyStart = i + 1; continue; }
			BodyStart = i; break;
		}

		for (int32 i = BodyStart; i < TopExpr->Num(); i++)
		{
			UEdGraphPin* OutPin = nullptr;
			UEdGraphNode* NewNode = IMP_ConvertFormToNode(TopExpr->Get(i), Ctx, OutPin);
			if (CurrentExecPin && NewNode)
			{
				UEdGraphPin* InExec = IMP_GetExecInput(NewNode);
				if (InExec) IMP_Connect(CurrentExecPin, InExec, Ctx);
			}
			IMP_UpdateCurrentExecPin(NewNode, OutPin, CurrentExecPin);
		}

		for (UEdGraphNode* N : Graph->Nodes)
			if (N) N->ReconstructNode();

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		if (Options.bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
			IMP_RecordCompileStatus(BP, Ctx, TEXT("ImportGraph(function)"));
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

		int32 BodyStart = 2;
		for (int32 i = 2; i < TopExpr->Num(); i++)
		{
			if (TopExpr->Get(i)->IsKeyword()) { i++; BodyStart = i + 1; continue; }
			BodyStart = i; break;
		}

		for (int32 i = BodyStart; i < TopExpr->Num(); i++)
		{
			UEdGraphPin* OutPin = nullptr;
			UEdGraphNode* NewNode = IMP_ConvertFormToNode(TopExpr->Get(i), Ctx, OutPin);
			if (CurrentExecPin && NewNode)
			{
				UEdGraphPin* InExec = IMP_GetExecInput(NewNode);
				if (InExec) IMP_Connect(CurrentExecPin, InExec, Ctx);
			}
			IMP_UpdateCurrentExecPin(NewNode, OutPin, CurrentExecPin);
		}

		for (UEdGraphNode* N : Graph->Nodes)
			if (N) N->ReconstructNode();

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		if (Options.bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
			IMP_RecordCompileStatus(BP, Ctx, TEXT("ImportGraph(macro)"));
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
		if (Options.bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(OwnerBP, EBlueprintCompileOptions::SkipGarbageCollection);
			IMP_RecordCompileStatus(OwnerBP, TransitionCtx, TEXT("ImportGraph(transition-cond)"));
		}
	}

	if (TransitionCtx.Errors.Num() > 0)
	{
		return IMP_FailFromContext(TransitionCtx, TEXT("ImportGraph transition condition import failed"));
	}

	UE_LOG(LogBlueprintLisp, Log, TEXT("ImportGraph: restored transition condition (%d nodes created)"), CreatedNodes.Num());
	return IMP_OkFromContext(LispCode, TransitionCtx);

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
