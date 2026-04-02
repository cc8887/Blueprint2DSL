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
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Self.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
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
#include "K2Node_FunctionTerminator.h"
#include "K2Node_EnumEquality.h"
#include "K2Node_EnumInequality.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectIterator.h"

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
static UEdGraphPin* BuildPureExprNode(const FLispNodePtr& Expr, UEdGraph* Graph, UBlueprint* BP, TArray<UEdGraphNode*>& CreatedNodes, FString& OutLiteralValue);

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

// ----- Convert pure (data-flow) expression to Lisp -----
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
		if (Visited.Contains(SourceNode)) return FLispNode::MakeSymbol(TEXT("...circular..."));
		Visited.Add(SourceNode);

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
		Args.Add(FLispNode::MakeSymbol(VarName));
		Args.Add(ConvertPureExpressionToLisp(ValuePin, Graph, Visited));
		return AppendNodeId(FLispNode::MakeList(Args), Node, ShortIds);
	}

	// ---- function call ----
	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
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
			FLispNodePtr Val = ConvertPureExpressionToLisp(Pin, Graph, Visited);
			if (!Val->IsNil())
			{
				Args.Add(FLispNode::MakeKeyword(FString::Printf(TEXT(":%s"), *Pin->PinName.ToString().ToLower())));
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
				if (Body.IsValid() && !Body->IsNil()) Args.Add(Body);
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

		FLispNodePtr NodeLisp = ConvertNodeToLisp(NextNode, Graph, Visited, bPositions, ShortIds);
		if (NodeLisp.IsValid() && !NodeLisp->IsNil())
			Statements.Add(NodeLisp);

		// branch terminates the chain (branches handled inside ConvertNodeToLisp)
		if (Cast<UK2Node_IfThenElse>(NextNode)) break;

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

// ----- Convert a standard K2Node_Event -----
static FLispNodePtr ConvertEventToLisp(UK2Node_Event* Event, UEdGraph* Graph, bool bPositions,
	const TMap<FGuid, FString>& ShortEventIds, const TMap<FGuid, FString>& ShortNodeIds)
{
	TSet<UEdGraphNode*> Visited;
	FString EventName = Event->EventReference.GetMemberName().ToString();
	if (EventName.IsEmpty()) EventName = Event->CustomFunctionName.ToString();
	if (EventName.IsEmpty()) EventName = Event->GetNodeTitle(ENodeTitleType::ListView).ToString();

	TArray<FLispNodePtr> EventArgs;
	EventArgs.Add(FLispNode::MakeSymbol(TEXT("event")));
	EventArgs.Add(FLispNode::MakeSymbol(EventName));

	// :event-id for stable identification
	if (const FString* EId = ShortEventIds.Find(Event->NodeGuid))
	{
		EventArgs.Add(FLispNode::MakeKeyword(TEXT(":event-id")));
		EventArgs.Add(FLispNode::MakeString(*EId));
	}

	// Position metadata
	if (bPositions)
	{
		EventArgs.Add(FLispNode::MakeKeyword(TEXT(":pos")));
		EventArgs.Add(FLispNode::MakeString(FString::Printf(TEXT("%d,%d"), Event->NodePosX, Event->NodePosY)));
	}

	// Exec output -> body
	UEdGraphPin* ThenPin = GetThenPin(Event);
	FLispNodePtr Body = ConvertExecChainToLisp(ThenPin, Graph, Visited, bPositions, ShortNodeIds);
	if (Body.IsValid() && !Body->IsNil())
	{
		if (Body->IsForm(TEXT("seq")))
		{
			for (int32 i = 1; i < Body->Num(); i++)
				EventArgs.Add(Body->Get(i));
		}
		else EventArgs.Add(Body);
	}

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
	EventArgs.Add(FLispNode::MakeSymbol(EventName));

	if (const FString* EId = ShortEventIds.Find(Event->NodeGuid))
	{
		EventArgs.Add(FLispNode::MakeKeyword(TEXT(":event-id")));
		EventArgs.Add(FLispNode::MakeString(*EId));
	}

	// Parameters
	for (UEdGraphPin* Pin : Event->Pins)
	{
		if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			EventArgs.Add(FLispNode::MakeKeyword(TEXT(":param")));
			TArray<FLispNodePtr> ParamPair;
			ParamPair.Add(FLispNode::MakeSymbol(Pin->PinName.ToString()));
			ParamPair.Add(FLispNode::MakeSymbol(PinTypeToLispType(Pin->PinType)));
			EventArgs.Add(FLispNode::MakeList(ParamPair));
		}
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

// ============================================================================
// ImportGraph helpers: Pure DAG reconstruction
// ============================================================================

/**
 * Recursively build K2Nodes in Graph from a pure S-expression.
 * Returns the output UEdGraphPin that represents the value of this expression,
 * or nullptr on failure (in which case OutLiteralValue may be set for literals).
 */
static UEdGraphPin* BuildPureExprNode(
	const FLispNodePtr& Expr,
	UEdGraph* Graph,
	UBlueprint* BP,
	TArray<UEdGraphNode*>& CreatedNodes,
	FString& OutLiteralValue)
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
				UEdGraphPin* ArgOutputPin = BuildPureExprNode(ArgExpr, Graph, BP, CreatedNodes, LiteralVal);
				if (ArgOutputPin)
					Pin->MakeLinkTo(ArgOutputPin);
				else if (!LiteralVal.IsEmpty())
					Pin->DefaultValue = LiteralVal;

				ArgIdx++;
				DataPinIdx++;
				if (DataPinIdx >= 2) break; // EnumEquality has exactly 2 data inputs
			}

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

		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
		if (TargetFunc)
		{
			CallNode->SetFromFunction(TargetFunc);
		}
		else
		{
			CallNode->FunctionReference.SetExternalMember(FName(*Sym), nullptr);
			UE_LOG(LogBlueprintLisp, Warning,
				TEXT("ImportGraph: could not find UFunction '%s' — node may be incomplete"), *Sym);
		}
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
			UEdGraphPin* ArgOutputPin = BuildPureExprNode(ArgExpr, Graph, BP, CreatedNodes, LiteralVal);
			if (ArgOutputPin)
				Pin->MakeLinkTo(ArgOutputPin);
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
		return FBlueprintLispResult::Fail(FString::Printf(TEXT("Graph '%s' not found in '%s'"), *GraphName, *Blueprint->GetName()));

	// Collect event nodes and build short IDs
	TArray<UK2Node_Event*>       Events;
	TArray<UK2Node_CustomEvent*> CustomEvents;
	TArray<FGuid> EventGuids, NodeGuids;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node)) { CustomEvents.Add(CE); EventGuids.Add(Node->NodeGuid); }
		else if (UK2Node_Event* E = Cast<UK2Node_Event>(Node))         { Events.Add(E);        EventGuids.Add(Node->NodeGuid); }
		else if (Node->NodeGuid.IsValid())                              { NodeGuids.Add(Node->NodeGuid); }
	}

	TMap<FGuid, FString> ShortEventIds = Options.bStableIds ? ComputeShortIds(EventGuids) : TMap<FGuid,FString>();
	TMap<FGuid, FString> ShortNodeIds  = Options.bStableIds ? ComputeShortIds(NodeGuids)  : TMap<FGuid,FString>();

	// Sort events for deterministic output
	Events.Sort([](const UK2Node_Event& A, const UK2Node_Event& B){
		return A.EventReference.GetMemberName().ToString() < B.EventReference.GetMemberName().ToString();
	});
	CustomEvents.Sort([](const UK2Node_CustomEvent& A, const UK2Node_CustomEvent& B){
		return A.CustomFunctionName.ToString() < B.CustomFunctionName.ToString();
	});

	// Generate Lisp forms
	TArray<FString> Forms;
	for (UK2Node_Event* E : Events)
	{
		FLispNodePtr Form = ConvertEventToLisp(E, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
		if (Form.IsValid() && !Form->IsNil())
			Forms.Add(Form->ToString(Options.bPrettyPrint, 0));
	}
	for (UK2Node_CustomEvent* CE : CustomEvents)
	{
		FLispNodePtr Form = ConvertCustomEventToLisp(CE, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
		if (Form.IsValid() && !Form->IsNil())
			Forms.Add(Form->ToString(Options.bPrettyPrint, 0));
	}

	if (Forms.IsEmpty())
		return FBlueprintLispResult::Fail(FString::Printf(TEXT("No events found in graph '%s'"), *GraphName));

	FString Code;
	for (int32 i = 0; i < Forms.Num(); i++)
	{
		if (i > 0) Code += TEXT("\n\n");
		Code += Forms[i];
	}

	return FBlueprintLispResult::Ok(Code);
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
		if (Cast<UK2Node_CustomEvent>(Node) || Cast<UK2Node_Event>(Node))
			EventGuids.Add(Node->NodeGuid);
		else if (Node->NodeGuid.IsValid())
			NodeGuids.Add(Node->NodeGuid);
	}
	TMap<FGuid, FString> ShortEventIds = Options.bStableIds ? ComputeShortIds(EventGuids) : TMap<FGuid,FString>();
	TMap<FGuid, FString> ShortNodeIds  = Options.bStableIds ? ComputeShortIds(NodeGuids)  : TMap<FGuid,FString>();

	TArray<FString> Forms;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
		{
			FLispNodePtr Form = ConvertCustomEventToLisp(CE, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
			if (Form.IsValid() && !Form->IsNil()) Forms.Add(Form->ToString(Options.bPrettyPrint, 0));
		}
		else if (UK2Node_Event* E = Cast<UK2Node_Event>(Node))
		{
			FLispNodePtr Form = ConvertEventToLisp(E, Graph, Options.bIncludePositions, ShortEventIds, ShortNodeIds);
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
			return FBlueprintLispResult::Fail(TEXT("No event nodes found in graph (not an EventGraph, and no TransitionResult found)"));
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
			TEXT("event"), TEXT("func"), TEXT("macro"), TEXT("var"), TEXT("comment"),
			TEXT("transition-cond")  // function-graph mode: pure bool expression for AnimationTransitionGraph
		};
		if (!ValidForms.Contains(Form))
			return FBlueprintLispResult::Fail(FString::Printf(TEXT("Unknown top-level form: %s"), *Form));
	}

	return FBlueprintLispResult::Ok(LispCode);
}

FBlueprintLispResult FBlueprintLispConverter::Import(
	UBlueprint* /*Blueprint*/,
	const FString& /*GraphName*/,
	const FString& /*LispCode*/,
	const FImportOptions& /*Options*/)
{
	// TODO: Import (DSL -> Blueprint) is not yet implemented.
	// It will be implemented in a follow-up.
	return FBlueprintLispResult::Fail(TEXT("BlueprintLisp Import is not yet implemented."));
}

FBlueprintLispResult FBlueprintLispConverter::ImportGraph(
	UEdGraph* Graph,
	const FString& LispCode,
	const FImportOptions& Options)
{
	if (!Graph)
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: Graph is null"));

	// Parse the DSL
	FLispParseResult PR = FLispParser::Parse(LispCode);
	if (!PR.bSuccess)
		return FBlueprintLispResult::Fail(FString::Printf(TEXT("ImportGraph: parse error at %d:%d: %s"),
			PR.ErrorLine, PR.ErrorColumn, *PR.Error));

	if (PR.Nodes.IsEmpty())
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: no top-level expressions"));

	FLispNodePtr TopExpr = PR.Nodes[0];
	if (!TopExpr.IsValid() || !TopExpr->IsList())
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: top-level expression must be a list"));

	FString FormName = TopExpr->GetFormName().ToLower();
	if (FormName != TEXT("transition-cond"))
		return FBlueprintLispResult::Fail(FString::Printf(TEXT("ImportGraph: expected (transition-cond ...), got (%s ...)"), *FormName));

	if (TopExpr->Num() < 2)
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: (transition-cond) missing condition expression"));

	// Clear existing nodes if requested (keep only the Result node)
	if (Options.bClearExisting)
	{
		TArray<UEdGraphNode*> NodesToRemove;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			// Keep TransitionResult
			if (!Cast<UAnimGraphNode_TransitionResult>(N))
				NodesToRemove.Add(N);
		}
		for (UEdGraphNode* N : NodesToRemove)
			Graph->RemoveNode(N);
	}

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
	FString LiteralVal;
	UEdGraphPin* OutputPin = BuildPureExprNode(CondExpr, Graph, OwnerBP, CreatedNodes, LiteralVal);

	if (OutputPin)
	{
		// Break any existing links on the bool pin
		BoolInputPin->BreakAllPinLinks();
		BoolInputPin->MakeLinkTo(OutputPin);
	}
	else if (!LiteralVal.IsEmpty())
	{
		BoolInputPin->DefaultValue = LiteralVal;
	}
	else
	{
		return FBlueprintLispResult::Fail(TEXT("ImportGraph: condition expression produced no output pin"));
	}

	// Simple auto-layout: place created nodes to the left of ResultNode
	float X = ResultNode->NodePosX - 300.0f;
	for (int32 i = 0; i < CreatedNodes.Num(); i++)
	{
		CreatedNodes[i]->NodePosX = X;
		CreatedNodes[i]->NodePosY = ResultNode->NodePosY + (i - CreatedNodes.Num() / 2) * 80.0f;
		X -= 220.0f;
	}

	UE_LOG(LogBlueprintLisp, Log, TEXT("ImportGraph: restored transition condition (%d nodes created)"), CreatedNodes.Num());
	return FBlueprintLispResult::Ok(LispCode);
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
