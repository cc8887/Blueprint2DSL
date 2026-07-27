// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// BlueprintLispTests.cpp - UE Automation Tests for BlueprintLisp AST/Parser
//
// Run via:
//   UnrealEditor.exe <project> -run=AutomationTests -filter="BlueprintLisp"
// Or in Editor:
//   Window -> Developer Tools -> Session Frontend -> Automation

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "BlueprintLispAST.h"

#if WITH_DEV_AUTOMATION_TESTS

// ============================================================================
// Helper macros
// ============================================================================

// Standard test flags: runs in Editor + Commandlet context, ProductFilter
#define BL_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

#define BL_TEST(Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(F##Name, "BlueprintLisp." #Name, BL_FLAGS)

// ============================================================================
// FLispNode factory tests
// ============================================================================

BL_TEST(NodeFactory_Nil)
bool FNodeFactory_Nil::RunTest(const FString& Parameters)
{
	auto N = FLispNode::MakeNil();
	TestTrue(TEXT("IsNil"), N->IsNil());
	TestFalse(TEXT("not IsList"), N->IsList());
	TestEqual(TEXT("ToString"), N->ToString(), FString(TEXT("nil")));
	return true;
}

BL_TEST(NodeFactory_Symbol)
bool FNodeFactory_Symbol::RunTest(const FString& Parameters)
{
	auto N = FLispNode::MakeSymbol(TEXT("BeginPlay"));
	TestTrue(TEXT("IsSymbol"), N->IsSymbol());
	TestEqual(TEXT("StringValue"), N->StringValue, FString(TEXT("BeginPlay")));
	TestEqual(TEXT("ToString"), N->ToString(), FString(TEXT("BeginPlay")));
	return true;
}

BL_TEST(NodeFactory_Keyword)
bool FNodeFactory_Keyword::RunTest(const FString& Parameters)
{
	auto N = FLispNode::MakeKeyword(TEXT(":true"));
	TestTrue(TEXT("IsKeyword"), N->IsKeyword());
	TestEqual(TEXT("ToString"), N->ToString(), FString(TEXT(":true")));
	return true;
}

BL_TEST(NodeFactory_Number_Int)
bool FNodeFactory_Number_Int::RunTest(const FString& Parameters)
{
	auto N = FLispNode::MakeNumber(42.0);
	TestTrue(TEXT("IsNumber"), N->IsNumber());
	TestEqual(TEXT("NumberValue"), N->NumberValue, 42.0);
	TestEqual(TEXT("ToString"), N->ToString(), FString(TEXT("42")));
	return true;
}

BL_TEST(NodeFactory_Number_Float)
bool FNodeFactory_Number_Float::RunTest(const FString& Parameters)
{
	auto N = FLispNode::MakeNumber(3.14);
	TestTrue(TEXT("IsNumber"), N->IsNumber());
	FString S = N->ToString();
	TestTrue(TEXT("Contains dot"), S.Contains(TEXT(".")));
	return true;
}

BL_TEST(NodeFactory_String)
bool FNodeFactory_String::RunTest(const FString& Parameters)
{
	auto N = FLispNode::MakeString(TEXT("hello world"));
	TestTrue(TEXT("IsString"), N->IsString());
	TestEqual(TEXT("StringValue"), N->StringValue, FString(TEXT("hello world")));
	TestEqual(TEXT("ToString"), N->ToString(), FString(TEXT("\"hello world\"")));
	return true;
}

BL_TEST(NodeFactory_String_Escape)
bool FNodeFactory_String_Escape::RunTest(const FString& Parameters)
{
	// String with quotes and newlines should be escaped
	auto N = FLispNode::MakeString(TEXT("line1\nline2"));
	FString S = N->ToString();
	TestTrue(TEXT("Escaped newline"), S.Contains(TEXT("\\n")));
	return true;
}

BL_TEST(NodeFactory_List_Empty)
bool FNodeFactory_List_Empty::RunTest(const FString& Parameters)
{
	auto N = FLispNode::MakeList({});
	TestTrue(TEXT("IsList"), N->IsList());
	TestEqual(TEXT("Num"), N->Num(), 0);
	TestEqual(TEXT("ToString"), N->ToString(), FString(TEXT("()")));
	return true;
}

BL_TEST(NodeFactory_List_Children)
bool FNodeFactory_List_Children::RunTest(const FString& Parameters)
{
	TArray<FLispNodePtr> Items = {
		FLispNode::MakeSymbol(TEXT("event")),
		FLispNode::MakeSymbol(TEXT("BeginPlay"))
	};
	auto N = FLispNode::MakeList(Items);
	TestTrue(TEXT("IsList"), N->IsList());
	TestEqual(TEXT("Num"), N->Num(), 2);
	TestEqual(TEXT("Get(0)"), N->Get(0)->StringValue, FString(TEXT("event")));
	TestEqual(TEXT("Get(1)"), N->Get(1)->StringValue, FString(TEXT("BeginPlay")));
	TestTrue(TEXT("IsForm event"), N->IsForm(TEXT("event")));
	TestFalse(TEXT("not IsForm func"), N->IsForm(TEXT("func")));
	TestEqual(TEXT("GetFormName"), N->GetFormName(), FString(TEXT("event")));
	return true;
}

BL_TEST(NodeFactory_List_OutOfBounds)
bool FNodeFactory_List_OutOfBounds::RunTest(const FString& Parameters)
{
	auto N = FLispNode::MakeList({ FLispNode::MakeSymbol(TEXT("x")) });
	auto OOB = N->Get(99);
	TestTrue(TEXT("OOB returns Nil"), OOB->IsNil());
	return true;
}

BL_TEST(NodeFactory_GetKeywordArg)
bool FNodeFactory_GetKeywordArg::RunTest(const FString& Parameters)
{
	// (branch cond :true A :false B)
	TArray<FLispNodePtr> Items = {
		FLispNode::MakeSymbol(TEXT("branch")),
		FLispNode::MakeSymbol(TEXT("cond")),
		FLispNode::MakeKeyword(TEXT(":true")),
		FLispNode::MakeSymbol(TEXT("A")),
		FLispNode::MakeKeyword(TEXT(":false")),
		FLispNode::MakeSymbol(TEXT("B")),
	};
	auto N = FLispNode::MakeList(Items);

	auto True = N->GetKeywordArg(TEXT(":true"));
	TestFalse(TEXT(":true not nil"), True->IsNil());
	TestEqual(TEXT(":true value"), True->StringValue, FString(TEXT("A")));

	auto False = N->GetKeywordArg(TEXT(":false"));
	TestFalse(TEXT(":false not nil"), False->IsNil());
	TestEqual(TEXT(":false value"), False->StringValue, FString(TEXT("B")));

	auto Missing = N->GetKeywordArg(TEXT(":missing"));
	TestTrue(TEXT(":missing is nil"), Missing->IsNil());

	TestTrue(TEXT("HasKeyword :true"), N->HasKeyword(TEXT(":true")));
	TestFalse(TEXT("no :missing"), N->HasKeyword(TEXT(":missing")));
	return true;
}

// ============================================================================
// FLispParser tests
// ============================================================================

BL_TEST(Parser_EmptyString)
bool FParser_EmptyString::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT(""));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestEqual(TEXT("0 nodes"), R.Nodes.Num(), 0);
	return true;
}

BL_TEST(Parser_WhitespaceOnly)
bool FParser_WhitespaceOnly::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("   \n\t  "));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestEqual(TEXT("0 nodes"), R.Nodes.Num(), 0);
	return true;
}

BL_TEST(Parser_Comment)
bool FParser_Comment::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("; this is a comment\n; another"));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestEqual(TEXT("0 nodes"), R.Nodes.Num(), 0);
	return true;
}

BL_TEST(Parser_Symbol)
bool FParser_Symbol::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("BeginPlay"));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestEqual(TEXT("1 node"), R.Nodes.Num(), 1);
	TestTrue(TEXT("IsSymbol"), R.Nodes[0]->IsSymbol());
	TestEqual(TEXT("value"), R.Nodes[0]->StringValue, FString(TEXT("BeginPlay")));
	return true;
}

BL_TEST(Parser_Keyword)
bool FParser_Keyword::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT(":event-id"));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestEqual(TEXT("1 node"), R.Nodes.Num(), 1);
	TestTrue(TEXT("IsKeyword"), R.Nodes[0]->IsKeyword());
	TestEqual(TEXT("value"), R.Nodes[0]->StringValue, FString(TEXT(":event-id")));
	return true;
}

BL_TEST(Parser_Integer)
bool FParser_Integer::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("42"));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestTrue(TEXT("IsNumber"), R.Nodes[0]->IsNumber());
	TestEqual(TEXT("value"), R.Nodes[0]->NumberValue, 42.0);
	return true;
}

BL_TEST(Parser_NegativeNumber)
bool FParser_NegativeNumber::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("-3.14"));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestTrue(TEXT("IsNumber"), R.Nodes[0]->IsNumber());
	TestTrue(TEXT("negative"), R.Nodes[0]->NumberValue < 0.0);
	return true;
}

BL_TEST(Parser_String)
bool FParser_String::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("\"hello world\""));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestTrue(TEXT("IsString"), R.Nodes[0]->IsString());
	TestEqual(TEXT("value"), R.Nodes[0]->StringValue, FString(TEXT("hello world")));
	return true;
}

BL_TEST(Parser_StringEscape)
bool FParser_StringEscape::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("\"line1\\nline2\""));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestTrue(TEXT("IsString"), R.Nodes[0]->IsString());
	TestTrue(TEXT("contains newline"), R.Nodes[0]->StringValue.Contains(TEXT("\n")));
	return true;
}

BL_TEST(Parser_Nil)
bool FParser_Nil::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("nil"));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestTrue(TEXT("IsNil"), R.Nodes[0]->IsNil());
	return true;
}

BL_TEST(Parser_Bool_True)
bool FParser_Bool_True::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("true"));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestTrue(TEXT("IsSymbol"), R.Nodes[0]->IsSymbol());
	TestEqual(TEXT("value"), R.Nodes[0]->StringValue, FString(TEXT("true")));
	return true;
}

BL_TEST(Parser_SimpleList)
bool FParser_SimpleList::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("(event BeginPlay)"));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestEqual(TEXT("1 node"), R.Nodes.Num(), 1);
	TestTrue(TEXT("IsList"), R.Nodes[0]->IsList());
	TestEqual(TEXT("Num"), R.Nodes[0]->Num(), 2);
	TestTrue(TEXT("IsForm event"), R.Nodes[0]->IsForm(TEXT("event")));
	return true;
}

BL_TEST(Parser_NestedList)
bool FParser_NestedList::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("(branch (IsValid player) :true (PrintString \"ok\") :false nil)"));
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestEqual(TEXT("1 node"), R.Nodes.Num(), 1);
	auto Root = R.Nodes[0];
	TestTrue(TEXT("IsForm branch"), Root->IsForm(TEXT("branch")));
	// cond is (IsValid player)
	auto Cond = Root->Get(1);
	TestTrue(TEXT("cond IsList"), Cond->IsList());
	TestTrue(TEXT("cond IsForm IsValid"), Cond->IsForm(TEXT("IsValid")));
	// :true value
	auto TrueVal = Root->GetKeywordArg(TEXT(":true"));
	TestFalse(TEXT(":true not nil"), TrueVal->IsNil());
	TestTrue(TEXT(":true IsForm PrintString"), TrueVal->IsForm(TEXT("PrintString")));
	return true;
}

BL_TEST(Parser_MultipleTopLevel)
bool FParser_MultipleTopLevel::RunTest(const FString& Parameters)
{
	FString Code = TEXT("(event BeginPlay (PrintString \"start\"))\n\n(event EndPlay (PrintString \"end\"))");
	auto R = FLispParser::Parse(Code);
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestEqual(TEXT("2 nodes"), R.Nodes.Num(), 2);
	TestTrue(TEXT("first IsForm event"), R.Nodes[0]->IsForm(TEXT("event")));
	TestTrue(TEXT("second IsForm event"), R.Nodes[1]->IsForm(TEXT("event")));
	return true;
}

BL_TEST(Parser_WithComments)
bool FParser_WithComments::RunTest(const FString& Parameters)
{
	FString Code = TEXT(
		"; This is BeginPlay\n"
		"(event BeginPlay\n"
		"  ; Print something\n"
		"  (PrintString \"hello\"))"
	);
	auto R = FLispParser::Parse(Code);
	TestTrue(TEXT("bSuccess"), R.bSuccess);
	TestEqual(TEXT("1 node"), R.Nodes.Num(), 1);
	TestTrue(TEXT("IsForm event"), R.Nodes[0]->IsForm(TEXT("event")));
	return true;
}

BL_TEST(Parser_ErrorUnmatchedParen)
bool FParser_ErrorUnmatchedParen::RunTest(const FString& Parameters)
{
	auto R = FLispParser::Parse(TEXT("(event BeginPlay"));
	// Should fail - unmatched open paren
	TestFalse(TEXT("should fail"), R.bSuccess);
	return true;
}

BL_TEST(Parser_IdempotentRoundTrip)
bool FParser_IdempotentRoundTrip::RunTest(const FString& Parameters)
{
	// Parse → ToString → Parse → ToString should produce identical output
	FString Original = TEXT("(event BeginPlay :event-id \"abc123\" (let player (GetPlayerCharacter 0)) (branch (IsValid player) :true (PrintString \"ok\") :false nil))");

	auto R1 = FLispParser::Parse(Original);
	TestTrue(TEXT("first parse succeeds"), R1.bSuccess);
	if (!R1.bSuccess) return false;

	FString S1 = R1.Nodes[0]->ToString(false, 0);

	auto R2 = FLispParser::Parse(S1);
	TestTrue(TEXT("second parse succeeds"), R2.bSuccess);
	if (!R2.bSuccess) return false;

	FString S2 = R2.Nodes[0]->ToString(false, 0);
	TestEqual(TEXT("idempotent"), S1, S2);
	return true;
}

// ============================================================================
// BlueprintLisp utility namespace tests
// ============================================================================

BL_TEST(Utility_PrettyPrint)
bool FUtility_PrettyPrint::RunTest(const FString& Parameters)
{
	FString Code = TEXT("(event BeginPlay (PrintString \"hello\"))");
	FString Pretty = BlueprintLisp::PrettyPrint(Code);
	TestFalse(TEXT("not empty"), Pretty.IsEmpty());
	TestTrue(TEXT("contains event"), Pretty.Contains(TEXT("event")));
	return true;
}

BL_TEST(Utility_Minify)
bool FUtility_Minify::RunTest(const FString& Parameters)
{
	FString Code = TEXT("(event  BeginPlay\n  (PrintString  \"hello\"))");
	FString Mini = BlueprintLisp::Minify(Code);
	TestFalse(TEXT("not empty"), Mini.IsEmpty());
	// Should not have double spaces or newlines
	TestFalse(TEXT("no double space"), Mini.Contains(TEXT("  ")));
	TestFalse(TEXT("no newline"), Mini.Contains(TEXT("\n")));
	return true;
}

BL_TEST(Utility_ExtractSymbols)
bool FUtility_ExtractSymbols::RunTest(const FString& Parameters)
{
	FString Code = TEXT("(event BeginPlay (let x (GetPlayerCharacter 0)) (PrintString x))");
	TArray<FString> Syms = BlueprintLisp::ExtractSymbols(Code);
	TestTrue(TEXT("contains event"), Syms.Contains(TEXT("event")));
	TestTrue(TEXT("contains BeginPlay"), Syms.Contains(TEXT("BeginPlay")));
	TestTrue(TEXT("contains let"), Syms.Contains(TEXT("let")));
	TestTrue(TEXT("contains x"), Syms.Contains(TEXT("x")));
	return true;
}

BL_TEST(Utility_IsValidSymbol)
bool FUtility_IsValidSymbol::RunTest(const FString& Parameters)
{
	TestTrue (TEXT("BeginPlay"),         BlueprintLisp::IsValidSymbol(TEXT("BeginPlay")));
	TestTrue (TEXT("my-var"),            BlueprintLisp::IsValidSymbol(TEXT("my-var")));
	TestTrue (TEXT("_private"),          BlueprintLisp::IsValidSymbol(TEXT("_private")));
	TestFalse(TEXT("empty"),             BlueprintLisp::IsValidSymbol(TEXT("")));
	TestFalse(TEXT("starts with digit"), BlueprintLisp::IsValidSymbol(TEXT("3var")));
	TestFalse(TEXT("starts with colon"), BlueprintLisp::IsValidSymbol(TEXT(":keyword")));
	return true;
}

// ============================================================================
// FBlueprintLispConverter::Validate tests
// ============================================================================

#include "BlueprintLispConverter.h"

BL_TEST(Converter_Validate_Valid)
bool FConverter_Validate_Valid::RunTest(const FString& Parameters)
{
	FString Code = TEXT("(event BeginPlay (PrintString \"hello\"))");
	auto R = FBlueprintLispConverter::Validate(Code);
	TestTrue(TEXT("valid event"), R.bSuccess);
	return true;
}

BL_TEST(Converter_Validate_InvalidForm)
bool FConverter_Validate_InvalidForm::RunTest(const FString& Parameters)
{
	FString Code = TEXT("(unknown-form x y z)");
	auto R = FBlueprintLispConverter::Validate(Code);
	TestFalse(TEXT("invalid top-level form"), R.bSuccess);
	TestFalse(TEXT("has error message"), R.Error.IsEmpty());
	return true;
}

BL_TEST(Converter_Validate_ParseError)
bool FConverter_Validate_ParseError::RunTest(const FString& Parameters)
{
	FString Code = TEXT("(event BeginPlay (PrintString \"hello\")");  // Missing )
	auto R = FBlueprintLispConverter::Validate(Code);
	TestFalse(TEXT("parse error detected"), R.bSuccess);
	return true;
}

BL_TEST(Converter_Validate_MultipleEvents)
bool FConverter_Validate_MultipleEvents::RunTest(const FString& Parameters)
{
	FString Code = TEXT("(event BeginPlay)\n(event EndPlay)\n(func MyFunc)");
	auto R = FBlueprintLispConverter::Validate(Code);
	TestTrue(TEXT("multiple valid forms"), R.bSuccess);
	return true;
}

// ============================================================================
// Import-Lifecycle Hook Integration Tests
//
// Guard the producer-side contract that BlueprintAutoLayout relies on: after
// an EventGraph import that touches nodes, BlueprintLisp broadcasts a
// PostNodeChanges event carrying the changed UEdGraphNodes and the
// "AutoLayout" behavior token. BlueprintAutoLayout's FBlueprintLispAutoLayoutHook
// consumes this to run LayoutSelection over the changed nodes.
// ============================================================================

#include "BlueprintLispModule.h"
#include "BlueprintLispConverter.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Select.h"
#include "K2Node_Composite.h"
#include "K2Node_Tunnel.h"
#include "K2Node_CallFunction.h"
#include "K2Node_GenericCreateObject.h"
#include "K2Node_VariableSet.h"
#include "Curves/CurveFloat.h"
#include "Animation/AnimInstance.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

BL_TEST(FunctionImport_PreservesNativeParentOverrideIdentity)
bool FFunctionImport_PreservesNativeParentOverrideIdentity::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), GetTransientPackage(), TEXT("ABP_BL_NativeOverride"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
		TEXT("BlueprintLispNativeOverrideTest"));
	TestNotNull(TEXT("transient AnimInstance Blueprint is created"), Blueprint);
	if (!Blueprint) return false;

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("BlueprintThreadSafeUpdateAnimation"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true, nullptr);

	UK2Node_FunctionEntry* Entry = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node))
		{
			Entry = Candidate;
			break;
		}
	}
	TestNotNull(TEXT("function entry exists"), Entry);
	if (!Entry) return false;

	FBlueprintLispConverter::FImportOptions Options;
	Options.bAutoLayout = false;
	Options.bCompile = false;
	Options.bSignatureOnly = true;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(
		Graph,
		TEXT("(function BlueprintThreadSafeUpdateAnimation :param (DeltaTime float))"),
		Options);
	TestTrue(TEXT("native override signature imports"), Imported.bSuccess);

	UFunction* ParentFunction = UAnimInstance::StaticClass()->FindFunctionByName(TEXT("BlueprintThreadSafeUpdateAnimation"));
	TestNotNull(TEXT("native parent function exists"), ParentFunction);
	TestTrue(TEXT("function entry resolves to the native parent override"),
		ParentFunction && Entry->FunctionReference.ResolveMember<UFunction>(Blueprint->ParentClass) == ParentFunction);
	return true;
}

BL_TEST(FunctionCall_SplitStructOutputPreservesParentType)
bool FFunctionCall_SplitStructOutputPreservesParentType::RunTest(const FString& Parameters)
{
	auto MakeRotatorFunction = [](const FName BlueprintName, UEdGraph*& OutGraph, UK2Node_FunctionResult*& OutResult)
	{
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			UObject::StaticClass(), GetTransientPackage(), BlueprintName, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
			TEXT("BlueprintLispSplitStructOutputTest"));
		OutGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, TEXT("SplitTransformRotation"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, OutGraph, true, nullptr);
		for (UEdGraphNode* Node : OutGraph->Nodes)
		{
			OutResult = Cast<UK2Node_FunctionResult>(Node);
			if (OutResult) break;
		}
		if (!OutResult)
		{
			OutResult = NewObject<UK2Node_FunctionResult>(OutGraph);
			OutResult->CreateNewGuid();
			OutGraph->AddNode(OutResult, false, false);
			OutResult->PostPlacedNewNode();
			OutResult->AllocateDefaultPins();
		}
		FEdGraphPinType RotatorType;
		RotatorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		RotatorType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		OutResult->CreateUserDefinedPin(TEXT("ReturnValue"), RotatorType, EGPD_Input, false);
		return Blueprint;
	};

	UEdGraph* SourceGraph = nullptr;
	UK2Node_FunctionResult* SourceResult = nullptr;
	UBlueprint* SourceBlueprint = MakeRotatorFunction(TEXT("BP_BL_SplitStructSource"), SourceGraph, SourceResult);
	TestNotNull(TEXT("source graph exists"), SourceGraph);
	TestNotNull(TEXT("source result exists"), SourceResult);
	if (!SourceBlueprint || !SourceGraph || !SourceResult) return false;
	UK2Node_FunctionEntry* SourceEntry = nullptr;
	for (UEdGraphNode* Node : SourceGraph->Nodes)
	{
		SourceEntry = Cast<UK2Node_FunctionEntry>(Node);
		if (SourceEntry) break;
	}
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	TestTrue(TEXT("function execution path connects entry to result"),
		SourceEntry && Schema
		&& Schema->TryCreateConnection(
			SourceEntry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output),
			SourceResult->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)));

	UK2Node_CallFunction* MakeTransform = NewObject<UK2Node_CallFunction>(SourceGraph);
	MakeTransform->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("MakeTransform")));
	MakeTransform->CreateNewGuid();
	SourceGraph->AddNode(MakeTransform, false, false);
	MakeTransform->AllocateDefaultPins();
	UEdGraphPin* TransformOutput = MakeTransform->GetReturnValuePin();
	TestNotNull(TEXT("MakeTransform return pin exists"), TransformOutput);
	if (!TransformOutput || !Schema) return false;

	UK2Node_CallFunction* GetComponentBounds = NewObject<UK2Node_CallFunction>(SourceGraph);
	GetComponentBounds->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("GetComponentBounds")));
	GetComponentBounds->CreateNewGuid();
	SourceGraph->AddNode(GetComponentBounds, false, false);
	GetComponentBounds->AllocateDefaultPins();
	TestTrue(TEXT("non-default output connects to MakeTransform input"),
		Schema->TryCreateConnection(
			GetComponentBounds->FindPin(TEXT("Origin"), EGPD_Output),
			MakeTransform->FindPin(TEXT("Location"), EGPD_Input)));

	const_cast<UEdGraphSchema_K2*>(Schema)->SplitPin(TransformOutput, false);

	UEdGraphPin* RotationChild = nullptr;
	for (UEdGraphPin* SubPin : TransformOutput->SubPins)
	{
		if (SubPin && SubPin->PinName.ToString().Contains(TEXT("Rotation")))
		{
			RotationChild = SubPin;
			break;
		}
	}
	TestNotNull(TEXT("split Rotation child exists"), RotationChild);

	UK2Node_CallFunction* ComposeRotators = NewObject<UK2Node_CallFunction>(SourceGraph);
	ComposeRotators->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("ComposeRotators")));
	ComposeRotators->CreateNewGuid();
	SourceGraph->AddNode(ComposeRotators, false, false);
	ComposeRotators->AllocateDefaultPins();
	UEdGraphPin* ComposeInput = ComposeRotators->FindPin(TEXT("A"), EGPD_Input);
	UEdGraphPin* ComposeOutput = ComposeRotators->GetReturnValuePin();
	UEdGraphPin* ResultInput = SourceResult->FindPin(TEXT("ReturnValue"), EGPD_Input);
	TestTrue(TEXT("split child connects to rotator consumer"),
		RotationChild && ComposeInput && Schema->TryCreateConnection(RotationChild, ComposeInput));
	TestTrue(TEXT("consumer connects to function result"),
		ComposeOutput && ResultInput && Schema->TryCreateConnection(ComposeOutput, ResultInput));

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(SourceGraph, ExportOptions);
	TestTrue(TEXT("split struct graph exports"), Exported.bSuccess);
	TestTrue(TEXT("split output uses explicit break-struct"), Exported.LispCode.Contains(TEXT("(break-struct :struct Transform")));
	TestTrue(TEXT("call result type describes the selected parent output"),
		Exported.LispCode.Contains(TEXT(":result-type-object \"/Script/CoreUObject.Transform\"")));
	TestFalse(TEXT("default call output omits redundant out-pin metadata"),
		Exported.LispCode.Contains(TEXT(":out-pin \"ReturnValue\"")));
	TestTrue(TEXT("non-default call output keeps out-pin metadata"),
		Exported.LispCode.Contains(TEXT(":out-pin \"Origin\"")));

	UEdGraph* DestinationGraph = nullptr;
	UK2Node_FunctionResult* DestinationResult = nullptr;
	UBlueprint* DestinationBlueprint = MakeRotatorFunction(TEXT("BP_BL_SplitStructDestination"), DestinationGraph, DestinationResult);
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = false;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(DestinationGraph, Exported.LispCode, ImportOptions);
	TestTrue(TEXT("split struct graph imports"), Imported.bSuccess);

	bool bHasMismatchedStructLink = false;
	for (UEdGraphNode* Node : DestinationGraph->Nodes)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				bHasMismatchedStructLink |= LinkedPin && LinkedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
					&& Pin->PinType.PinSubCategoryObject != LinkedPin->PinType.PinSubCategoryObject;
			}
		}
	}
	TestFalse(TEXT("import has no mismatched struct connection"), bHasMismatchedStructLink);
	return true;
}

namespace BlueprintLispLifecycleTest
{
	using namespace BlueprintLispImportLifecycle;

	class FRecordingHook : public IImportLifecycleHook
	{
	public:
		explicit FRecordingHook(int32 InPriority = 0) : Priority(InPriority) {}

		virtual int32 GetPriority(EImportLifecyclePhase Phase) const override
		{
			return Phase == EImportLifecyclePhase::PostNodeChanges ? Priority : 0;
		}

		virtual void OnNodePhase(const FImportNodePhaseEvent& Event) override
		{
			NodePhaseCount++;
			LastPhase = Event.Phase;
			LastBehaviors = Event.Context.RequestedBehaviors;
			LastChangeCount = Event.Changes.Num();
			OrderToken = NextGlobalOrder++;
		}

		int32 Priority = 0;
		int32 NodePhaseCount = 0;
		EImportLifecyclePhase LastPhase = EImportLifecyclePhase::PreNodeChanges;
		TSet<FName> LastBehaviors;
		int32 LastChangeCount = 0;
		int32 OrderToken = -1;

		static int32 NextGlobalOrder;
	};

	int32 FRecordingHook::NextGlobalOrder = 0;
}

namespace BlueprintLispFunctionReturnTest
{
	struct FFixture
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		UK2Node_FunctionEntry* Entry = nullptr;
		UK2Node_FunctionResult* Result = nullptr;
		UEdGraphPin* InputPin = nullptr;
		UEdGraphPin* ReturnPin = nullptr;
	};

	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FName Name, EEdGraphPinDirection Direction)
	{
		if (!Node) return nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName == Name && Pin->Direction == Direction)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static FFixture MakeFixture(const FName BlueprintName, bool bConnectData)
	{
		FFixture Fixture;
		Fixture.Blueprint = FKismetEditorUtilities::CreateBlueprint(
			UObject::StaticClass(), GetTransientPackage(), BlueprintName, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("BlueprintLispFunctionReturnTest")));
		Fixture.Graph = FBlueprintEditorUtils::CreateNewGraph(
			Fixture.Blueprint, TEXT("EchoBool"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Fixture.Blueprint, Fixture.Graph, true, nullptr);

		for (UEdGraphNode* Node : Fixture.Graph->Nodes)
		{
			if (!Fixture.Entry) Fixture.Entry = Cast<UK2Node_FunctionEntry>(Node);
			if (!Fixture.Result) Fixture.Result = Cast<UK2Node_FunctionResult>(Node);
		}
		if (!Fixture.Result)
		{
			Fixture.Result = NewObject<UK2Node_FunctionResult>(Fixture.Graph);
			Fixture.Result->CreateNewGuid();
			Fixture.Result->PostPlacedNewNode();
			Fixture.Result->AllocateDefaultPins();
			Fixture.Graph->AddNode(Fixture.Result, false, false);
		}

		FEdGraphPinType BoolType;
		BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		if (Fixture.Entry)
		{
			Fixture.Entry->CreateUserDefinedPin(TEXT("Input"), BoolType, EGPD_Output, false);
			Fixture.InputPin = FindPin(Fixture.Entry, TEXT("Input"), EGPD_Output);
		}
		if (Fixture.Result)
		{
			Fixture.Result->CreateUserDefinedPin(TEXT("ReturnValue"), BoolType, EGPD_Input, false);
			Fixture.ReturnPin = FindPin(Fixture.Result, TEXT("ReturnValue"), EGPD_Input);
		}

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		if (Schema && Fixture.Entry && Fixture.Result)
		{
			UEdGraphPin* ThenPin = FindPin(Fixture.Entry, UEdGraphSchema_K2::PN_Then, EGPD_Output);
			UEdGraphPin* ExecutePin = FindPin(Fixture.Result, UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			if (ThenPin && ExecutePin) Schema->TryCreateConnection(ThenPin, ExecutePin);
			if (bConnectData && Fixture.InputPin && Fixture.ReturnPin)
			{
				Schema->TryCreateConnection(Fixture.InputPin, Fixture.ReturnPin);
			}
		}
		return Fixture;
	}

	static UK2Node_FunctionResult* AddBoolResult(FFixture& Fixture, const FString& DefaultValue)
	{
		UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(Fixture.Graph);
		Result->CreateNewGuid();
		Result->PostPlacedNewNode();
		Result->AllocateDefaultPins();
		Fixture.Graph->AddNode(Result, false, false);

		FEdGraphPinType BoolType;
		BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		UEdGraphPin* ReturnPin = Result->CreateUserDefinedPin(TEXT("ReturnValue"), BoolType, EGPD_Input, false);
		if (ReturnPin)
		{
			ReturnPin->DefaultValue = DefaultValue;
		}
		return Result;
	}
}

BL_TEST(FunctionReturn_DirectParameterRoundTrips)
bool FFunctionReturn_DirectParameterRoundTrips::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	const FFixture Source = MakeFixture(TEXT("BP_BL_ReturnSource"), true);
	TestNotNull(TEXT("source function entry exists"), Source.Entry);
	TestNotNull(TEXT("source function result exists"), Source.Result);
	TestTrue(TEXT("source return is connected"), Source.ReturnPin && Source.ReturnPin->LinkedTo.Contains(Source.InputPin));
	if (!Source.Graph || !Source.ReturnPin || !Source.InputPin) return false;

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, ExportOptions);
	TestTrue(TEXT("source function exports"), Exported.bSuccess);
	TestTrue(TEXT("function result uses explicit return form"), Exported.LispCode.Contains(TEXT("(return ")));
	TestTrue(TEXT("return value preserves the parameter expression"),
		Exported.LispCode.Contains(TEXT(":value (ReturnValue Input)")));
	TestFalse(TEXT("localized generic return fallback is absent"), Exported.LispCode.Contains(TEXT("返回节点")));

	const FFixture Destination = MakeFixture(TEXT("BP_BL_ReturnDestination"), false);
	TestTrue(TEXT("destination starts disconnected"), Destination.ReturnPin && Destination.ReturnPin->LinkedTo.IsEmpty());
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = false;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(Destination.Graph, Exported.LispCode, ImportOptions);
	TestTrue(TEXT("explicit return form imports"), Imported.bSuccess);
	UEdGraphPin* ImportedInputPin = FindPin(Destination.Entry, TEXT("Input"), EGPD_Output);
	UEdGraphPin* ImportedReturnPin = FindPin(Destination.Result, TEXT("ReturnValue"), EGPD_Input);
	TestTrue(TEXT("import reconnects FunctionEntry to FunctionResult"),
		ImportedReturnPin && ImportedReturnPin->LinkedTo.Contains(ImportedInputPin));

	const FBlueprintLispResult ReExported = FBlueprintLispConverter::ExportGraph(Destination.Graph, ExportOptions);
	TestTrue(TEXT("imported function re-exports"), ReExported.bSuccess);
	TestTrue(TEXT("re-export preserves the return expression"),
		ReExported.LispCode.Contains(TEXT(":value (ReturnValue Input)")));
	return true;
}

BL_TEST(GenericCreateObject_RoundTripsExecAndResultDataflow)
bool FGenericCreateObject_RoundTripsExecAndResultDataflow::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	FFixture Source = MakeFixture(TEXT("BP_BL_CreateObjectSource"), true);
	if (!Source.Blueprint || !Source.Graph || !Source.Entry || !Source.Result) return false;

	FEdGraphPinType ObjectType;
	ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object;
	ObjectType.PinSubCategoryObject = UCurveFloat::StaticClass();
	TestTrue(TEXT("CreatedObject member variable added"),
		FBlueprintEditorUtils::AddMemberVariable(Source.Blueprint, TEXT("CreatedObject"), ObjectType));

	UK2Node_GenericCreateObject* CreateNode = NewObject<UK2Node_GenericCreateObject>(Source.Graph);
	CreateNode->CreateNewGuid();
	CreateNode->PostPlacedNewNode();
	CreateNode->AllocateDefaultPins();
	Source.Graph->AddNode(CreateNode, false, false);
	UEdGraphPin* ClassPin = CreateNode->GetClassPin();
	TestNotNull(TEXT("create-object class pin exists"), ClassPin);
	if (ClassPin)
	{
		ClassPin->DefaultObject = UCurveFloat::StaticClass();
		CreateNode->PinDefaultValueChanged(ClassPin);
	}

	UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Source.Graph);
	SetNode->VariableReference.SetSelfMember(TEXT("CreatedObject"));
	SetNode->CreateNewGuid();
	SetNode->PostPlacedNewNode();
	SetNode->AllocateDefaultPins();
	Source.Graph->AddNode(SetNode, false, false);

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* EntryThen = FindPin(Source.Entry, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* ResultExecute = FindPin(Source.Result, UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (EntryThen) EntryThen->BreakAllPinLinks();
	if (ResultExecute) ResultExecute->BreakAllPinLinks();
	TestTrue(TEXT("entry connects to create-object"), Schema && Schema->TryCreateConnection(EntryThen, CreateNode->GetExecPin()));
	TestTrue(TEXT("create-object connects to variable set"), Schema && Schema->TryCreateConnection(CreateNode->GetThenPin(), SetNode->GetExecPin()));
	TestTrue(TEXT("variable set connects to function result"), Schema && Schema->TryCreateConnection(SetNode->GetThenPin(), ResultExecute));
	UEdGraphPin* SetValuePin = FindPin(SetNode, TEXT("CreatedObject"), EGPD_Input);
	TestTrue(TEXT("created object result feeds variable set"), Schema && Schema->TryCreateConnection(CreateNode->GetResultPin(), SetValuePin));

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, ExportOptions);
	TestTrue(TEXT("generic create object graph exports without omission"), Exported.bSuccess);
	TestTrue(TEXT("generic create object has explicit DSL form"), Exported.LispCode.Contains(TEXT("(create-object ")));
	TestTrue(TEXT("created class is preserved"), Exported.LispCode.Contains(UCurveFloat::StaticClass()->GetPathName()));
	if (!Exported.bSuccess) return false;

	FFixture Destination = MakeFixture(TEXT("BP_BL_CreateObjectDestination"), true);
	TestTrue(TEXT("destination CreatedObject variable added"),
		FBlueprintEditorUtils::AddMemberVariable(Destination.Blueprint, TEXT("CreatedObject"), ObjectType));
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = false;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(Destination.Graph, Exported.LispCode, ImportOptions);
	TestTrue(TEXT("generic create object DSL imports"), Imported.bSuccess);

	UK2Node_GenericCreateObject* ImportedCreateNode = nullptr;
	UK2Node_VariableSet* ImportedSetNode = nullptr;
	for (UEdGraphNode* Node : Destination.Graph->Nodes)
	{
		if (!ImportedCreateNode) ImportedCreateNode = Cast<UK2Node_GenericCreateObject>(Node);
		if (!ImportedSetNode) ImportedSetNode = Cast<UK2Node_VariableSet>(Node);
	}
	TestNotNull(TEXT("import restores GenericCreateObject node"), ImportedCreateNode);
	TestNotNull(TEXT("import restores variable set node"), ImportedSetNode);
	if (ImportedCreateNode)
	{
		TestEqual(TEXT("import restores class to spawn"), ImportedCreateNode->GetClassToSpawn(), UCurveFloat::StaticClass());
	}
	if (ImportedCreateNode && ImportedSetNode)
	{
		UEdGraphPin* ImportedValuePin = FindPin(ImportedSetNode, TEXT("CreatedObject"), EGPD_Input);
		TestTrue(TEXT("import restores result dataflow"),
			ImportedValuePin && ImportedValuePin->LinkedTo.Contains(ImportedCreateNode->GetResultPin()));
	}

	const FBlueprintLispResult ReExported = FBlueprintLispConverter::ExportGraph(Destination.Graph, ExportOptions);
	TestTrue(TEXT("imported create-object graph re-exports"), ReExported.bSuccess);
	TestTrue(TEXT("re-export preserves create-object form"), ReExported.LispCode.Contains(TEXT("(create-object ")));
	return true;
}

BL_TEST(FunctionReturn_CreatesMissingResultFromSignature)
bool FFunctionReturn_CreatesMissingResultFromSignature::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	const FFixture Source = MakeFixture(TEXT("BP_BL_MissingResultSource"), true);
	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, ExportOptions);
	TestTrue(TEXT("source function exports"), Exported.bSuccess);
	if (!Exported.bSuccess) return false;

	FFixture Destination = MakeFixture(TEXT("BP_BL_MissingResultDestination"), false);
	Destination.Graph->RemoveNode(Destination.Result);
	TestFalse(TEXT("destination has no function result before import"),
		Destination.Graph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return Node && Node->IsA<UK2Node_FunctionResult>(); }));

	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = false;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(Destination.Graph, Exported.LispCode, ImportOptions);
	if (!Imported.bSuccess) AddError(TEXT("missing-result import: ") + Imported.Error);
	TestTrue(TEXT("function import creates missing result"), Imported.bSuccess);

	UK2Node_FunctionResult* CreatedResult = nullptr;
	for (UEdGraphNode* Node : Destination.Graph->Nodes)
	{
		if (UK2Node_FunctionResult* Candidate = Cast<UK2Node_FunctionResult>(Node))
		{
			CreatedResult = Candidate;
			break;
		}
	}
	TestNotNull(TEXT("result node exists after import"), CreatedResult);
	UEdGraphPin* CreatedReturnPin = FindPin(CreatedResult, TEXT("ReturnValue"), EGPD_Input);
	UEdGraphPin* EntryInputPin = FindPin(Destination.Entry, TEXT("Input"), EGPD_Output);
	TestTrue(TEXT("created result restores return dataflow"),
		CreatedReturnPin && CreatedReturnPin->LinkedTo.Contains(EntryInputPin));
	return true;
}

BL_TEST(FunctionReturn_BranchingResultsRoundTrip)
bool FFunctionReturn_BranchingResultsRoundTrip::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	FFixture Source = MakeFixture(TEXT("BP_BL_BranchReturnSource"), false);
	UK2Node_FunctionResult* FalseResult = AddBoolResult(Source, TEXT("false"));
	UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(Source.Graph);
	Branch->CreateNewGuid();
	Branch->PostPlacedNewNode();
	Branch->AllocateDefaultPins();
	Source.Graph->AddNode(Branch, false, false);

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* EntryThen = FindPin(Source.Entry, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* FirstExecute = FindPin(Source.Result, UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	UEdGraphPin* SecondExecute = FindPin(FalseResult, UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	if (EntryThen) EntryThen->BreakAllPinLinks();
	if (FirstExecute) FirstExecute->BreakAllPinLinks();
	TestTrue(TEXT("entry connects to branch"), Schema && EntryThen && Schema->TryCreateConnection(EntryThen, Branch->GetExecPin()));
	TestTrue(TEXT("input drives branch condition"), Schema && Source.InputPin && Schema->TryCreateConnection(Source.InputPin, Branch->GetConditionPin()));
	TestTrue(TEXT("true branch reaches first result"), Schema && FirstExecute && Schema->TryCreateConnection(Branch->GetThenPin(), FirstExecute));
	TestTrue(TEXT("false branch reaches second result"), Schema && SecondExecute && Schema->TryCreateConnection(Branch->GetElsePin(), SecondExecute));
	TestTrue(TEXT("true result returns input"), Schema && Source.ReturnPin && Schema->TryCreateConnection(Source.InputPin, Source.ReturnPin));

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, ExportOptions);
	TestTrue(TEXT("branching function exports"), Exported.bSuccess);
	TestTrue(TEXT("true branch contains a return value"), Exported.LispCode.Contains(TEXT(":true (return :value (ReturnValue Input)")));
	TestTrue(TEXT("false branch contains the default return value"), Exported.LispCode.Contains(TEXT(":false (return :value (ReturnValue false)")));

	FFixture Destination = MakeFixture(TEXT("BP_BL_BranchReturnDestination"), false);
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = false;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(Destination.Graph, Exported.LispCode, ImportOptions);
	TestTrue(TEXT("both function result paths import"), Imported.bSuccess);

	int32 ResultCount = 0;
	for (UEdGraphNode* Node : Destination.Graph->Nodes)
	{
		ResultCount += Node && Node->IsA<UK2Node_FunctionResult>() ? 1 : 0;
	}
	TestEqual(TEXT("import creates both FunctionResult nodes"), ResultCount, 2);
	const FBlueprintLispResult ReExported = FBlueprintLispConverter::ExportGraph(Destination.Graph, ExportOptions);
	TestTrue(TEXT("branching function re-exports"), ReExported.bSuccess);
	TestTrue(TEXT("re-export keeps the false default"), ReExported.LispCode.Contains(TEXT(":false (return :value (ReturnValue false)")));
	return true;
}

BL_TEST(FunctionReturn_UnconnectedBoolExportsFalse)
bool FFunctionReturn_UnconnectedBoolExportsFalse::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	FFixture Fixture = MakeFixture(TEXT("BP_BL_DefaultReturn"), false);
	Fixture.ReturnPin->DefaultValue.Empty();
	Fixture.ReturnPin->AutogeneratedDefaultValue.Empty();

	FBlueprintLispConverter::FExportOptions Options;
	Options.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Fixture.Graph, Options);
	TestTrue(TEXT("default bool function exports"), Exported.bSuccess);
	TestTrue(TEXT("unconnected bool return is explicit false"),
		Exported.LispCode.Contains(TEXT(":value (ReturnValue false)")));
	return true;
}

BL_TEST(FunctionReturn_UnknownConnectedSourceFailsExport)
bool FFunctionReturn_UnknownConnectedSourceFailsExport::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	FFixture Fixture = MakeFixture(TEXT("BP_BL_UnsupportedReturn"), false);
	UEdGraphNode* UnsupportedSource = NewObject<UEdGraphNode>(Fixture.Graph);
	UnsupportedSource->CreateNewGuid();
	UEdGraphPin* UnsupportedOutput = UnsupportedSource->CreatePin(
		EGPD_Output, UEdGraphSchema_K2::PC_Boolean, TEXT("Value"));
	Fixture.Graph->AddNode(UnsupportedSource, false, false);
	if (UnsupportedOutput && Fixture.ReturnPin)
	{
		UnsupportedOutput->MakeLinkTo(Fixture.ReturnPin);
	}
	TestTrue(TEXT("unsupported source connects to return"),
		UnsupportedOutput && Fixture.ReturnPin && Fixture.ReturnPin->LinkedTo.Contains(UnsupportedOutput));

	FBlueprintLispConverter::FExportOptions Options;
	Options.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Fixture.Graph, Options);
	TestFalse(TEXT("connected unrepresentable return source is a hard export failure"), Exported.bSuccess);
	TestTrue(TEXT("failure identifies FunctionResult pin"), Exported.Error.Contains(TEXT("ReturnValue")));
	return true;
}

BL_TEST(FunctionReturn_NestedUnknownSourceFailsExport)
bool FFunctionReturn_NestedUnknownSourceFailsExport::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	FFixture Fixture = MakeFixture(TEXT("BP_BL_NestedUnsupportedReturn"), false);

	UEdGraphNode* UnsupportedSource = NewObject<UEdGraphNode>(Fixture.Graph);
	UnsupportedSource->CreateNewGuid();
	UEdGraphPin* UnsupportedOutput = UnsupportedSource->CreatePin(
		EGPD_Output, UEdGraphSchema_K2::PC_Boolean, TEXT("Value"));
	Fixture.Graph->AddNode(UnsupportedSource, false, false);

	UK2Node_Select* SelectNode = NewObject<UK2Node_Select>(Fixture.Graph);
	SelectNode->CreateNewGuid();
	SelectNode->PostPlacedNewNode();
	SelectNode->AllocateDefaultPins();
	Fixture.Graph->AddNode(SelectNode, false, false);
	UEdGraphPin* SelectInput = nullptr;
	UEdGraphPin* SelectOutput = nullptr;
	for (UEdGraphPin* Pin : SelectNode->Pins)
	{
		if (!Pin) continue;
		if (!SelectInput && Pin->Direction == EGPD_Input && Pin->PinName != TEXT("Index")) SelectInput = Pin;
		if (!SelectOutput && Pin->Direction == EGPD_Output) SelectOutput = Pin;
	}
	TestNotNull(TEXT("select has a value input"), SelectInput);
	TestNotNull(TEXT("select has a value output"), SelectOutput);
	if (SelectInput && UnsupportedOutput) UnsupportedOutput->MakeLinkTo(SelectInput);
	if (SelectOutput && Fixture.ReturnPin) SelectOutput->MakeLinkTo(Fixture.ReturnPin);

	FBlueprintLispConverter::FExportOptions Options;
	Options.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Fixture.Graph, Options);
	TestFalse(TEXT("nested unrepresentable return source is a hard export failure"), Exported.bSuccess);
	TestTrue(TEXT("nested failure names its source class"), Exported.Error.Contains(TEXT("EdGraphNode")));
	return true;
}

BL_TEST(FunctionReturn_SelectRoundTripsLocaleIndependent)
bool FFunctionReturn_SelectRoundTripsLocaleIndependent::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	FFixture Source = MakeFixture(TEXT("BP_BL_SelectReturnSource"), false);
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	TestNotNull(TEXT("K2 schema exists"), Schema);
	if (!Source.Graph || !Source.InputPin || !Source.ReturnPin || !Schema) return false;

	UK2Node_Select* SelectNode = NewObject<UK2Node_Select>(Source.Graph);
	SelectNode->CreateNewGuid();
	Source.Graph->AddNode(SelectNode, false, false);
	SelectNode->AllocateDefaultPins();
	TestTrue(TEXT("bool input connects to select index"),
		Schema->TryCreateConnection(Source.InputPin, SelectNode->GetIndexPin()));

	TArray<UEdGraphPin*> OptionPins;
	SelectNode->GetOptionPins(OptionPins);
	TestEqual(TEXT("bool select has two options"), OptionPins.Num(), 2);
	if (OptionPins.Num() == 2)
	{
		OptionPins[0]->DefaultValue = TEXT("false");
		OptionPins[1]->DefaultValue = TEXT("true");
	}
	TestTrue(TEXT("select output connects to return"),
		Schema->TryCreateConnection(SelectNode->GetReturnValuePin(), Source.ReturnPin));

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, ExportOptions);
	TestTrue(TEXT("select function exports"), Exported.bSuccess);
	TestTrue(TEXT("select uses locale-independent DSL form"), Exported.LispCode.Contains(TEXT("(select :index")));

	FFixture Destination = MakeFixture(TEXT("BP_BL_SelectReturnDestination"), false);
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = false;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(Destination.Graph, Exported.LispCode, ImportOptions);
	TestTrue(TEXT("select function imports"), Imported.bSuccess);

	int32 SelectCount = 0;
	for (UEdGraphNode* Node : Destination.Graph->Nodes)
	{
		SelectCount += Node && Node->IsA<UK2Node_Select>() ? 1 : 0;
	}
	TestEqual(TEXT("import recreates one select node"), SelectCount, 1);

	const FBlueprintLispResult ReExported = FBlueprintLispConverter::ExportGraph(Destination.Graph, ExportOptions);
	TestTrue(TEXT("imported select re-exports"), ReExported.bSuccess);
	TestTrue(TEXT("re-export retains locale-independent select form"), ReExported.LispCode.Contains(TEXT("(select :index")));
	return true;
}

BL_TEST(FunctionMetadata_ThreadSafeRoundTrips)
bool FFunctionMetadata_ThreadSafeRoundTrips::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	FFixture Source = MakeFixture(TEXT("BP_BL_ThreadSafeSource"), true);
	TestNotNull(TEXT("source function entry exists"), Source.Entry);
	if (!Source.Entry || !Source.Graph) return false;
	Source.Entry->MetaData.bThreadSafe = true;

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, ExportOptions);
	TestTrue(TEXT("thread-safe function exports"), Exported.bSuccess);
	TestTrue(TEXT("thread-safe metadata is explicit in DSL"), Exported.LispCode.Contains(TEXT(":thread-safe true")));

	FFixture Destination = MakeFixture(TEXT("BP_BL_ThreadSafeDestination"), false);
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = false;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(Destination.Graph, Exported.LispCode, ImportOptions);
	TestTrue(TEXT("thread-safe function imports"), Imported.bSuccess);
	TestTrue(TEXT("function entry restores thread-safe metadata"), Destination.Entry && Destination.Entry->MetaData.bThreadSafe);
	return true;
}

BL_TEST(FunctionReturn_EnumSelectPreservesResultType)
bool FFunctionReturn_EnumSelectPreservesResultType::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	FFixture Source = MakeFixture(TEXT("BP_BL_EnumSelectSource"), false);
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEnum* MovementModeEnum = StaticEnum<EMovementMode>();
	TestNotNull(TEXT("movement mode enum exists"), MovementModeEnum);
	if (!Source.Graph || !Source.InputPin || !Source.ReturnPin || !Schema || !MovementModeEnum) return false;

	FEdGraphPinType EnumType;
	EnumType.PinCategory = UEdGraphSchema_K2::PC_Byte;
	EnumType.PinSubCategoryObject = MovementModeEnum;
	Source.Result->RemoveUserDefinedPinByName(TEXT("ReturnValue"));
	Source.ReturnPin = Source.Result->CreateUserDefinedPin(TEXT("ReturnValue"), EnumType, EGPD_Input, false);
	TestNotNull(TEXT("enum function return pin is created"), Source.ReturnPin);
	if (!Source.ReturnPin) return false;

	UK2Node_Select* SelectNode = NewObject<UK2Node_Select>(Source.Graph);
	SelectNode->CreateNewGuid();
	Source.Graph->AddNode(SelectNode, false, false);
	SelectNode->AllocateDefaultPins();
	TestTrue(TEXT("bool drives enum select index"), Schema->TryCreateConnection(Source.InputPin, SelectNode->GetIndexPin()));
	UEdGraphPin* SelectReturn = SelectNode->GetReturnValuePin();
	SelectReturn->PinType = EnumType;
	SelectNode->ChangePinType(SelectReturn);

	TArray<UEdGraphPin*> OptionPins;
	SelectNode->GetOptionPins(OptionPins);
	TestEqual(TEXT("enum select has two options"), OptionPins.Num(), 2);
	if (OptionPins.Num() != 2) return false;
	OptionPins[0]->DefaultValue = TEXT("MOVE_Walking");
	OptionPins[1]->DefaultValue = TEXT("MOVE_Falling");
	TestTrue(TEXT("enum select output connects to return"),
		Schema->TryCreateConnection(SelectNode->GetReturnValuePin(), Source.ReturnPin));

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, ExportOptions);
	TestTrue(TEXT("enum select exports"), Exported.bSuccess);
	TestTrue(TEXT("enum select DSL preserves the result type object"),
		Exported.LispCode.Contains(TEXT(":result-type-object \"/Script/Engine.EMovementMode\"")));

	FFixture Destination = MakeFixture(TEXT("BP_BL_EnumSelectDestination"), false);
	Destination.Result->RemoveUserDefinedPinByName(TEXT("ReturnValue"));
	Destination.ReturnPin = Destination.Result->CreateUserDefinedPin(TEXT("ReturnValue"), EnumType, EGPD_Input, false);
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = false;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(Destination.Graph, Exported.LispCode, ImportOptions);
	if (!Imported.bSuccess) AddError(TEXT("enum select import: ") + Imported.Error);
	TestTrue(TEXT("enum select imports"), Imported.bSuccess);

	UK2Node_Select* ImportedSelect = nullptr;
	for (UEdGraphNode* Node : Destination.Graph->Nodes)
	{
		ImportedSelect = Cast<UK2Node_Select>(Node);
		if (ImportedSelect) break;
	}
	TestNotNull(TEXT("enum select node is restored"), ImportedSelect);
	if (!ImportedSelect) return false;
	TestTrue(TEXT("enum select return keeps UEnum"),
		ImportedSelect->GetReturnValuePin()->PinType.PinSubCategoryObject.Get() == MovementModeEnum);
	ImportedSelect->GetOptionPins(OptionPins);
	TestTrue(TEXT("enum select options keep UEnum"), OptionPins.Num() == 2
		&& OptionPins[0]->PinType.PinSubCategoryObject == MovementModeEnum
		&& OptionPins[1]->PinType.PinSubCategoryObject == MovementModeEnum);
	return true;
}

BL_TEST(FunctionReturn_ExecKnotIsTransparent)
bool FFunctionReturn_ExecKnotIsTransparent::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	FFixture Source = MakeFixture(TEXT("BP_BL_ExecKnotSource"), true);
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	if (!Source.Graph || !Source.Entry || !Source.Result || !Schema) return false;

	UEdGraphPin* EntryThen = FindPin(Source.Entry, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* ResultExec = FindPin(Source.Result, UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	TestNotNull(TEXT("entry exec pin exists"), EntryThen);
	TestNotNull(TEXT("result exec pin exists"), ResultExec);
	if (!EntryThen || !ResultExec) return false;
	EntryThen->BreakAllPinLinks();

	UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(Source.Graph);
	Knot->CreateNewGuid();
	Source.Graph->AddNode(Knot, false, false);
	Knot->AllocateDefaultPins();
	Knot->GetInputPin()->PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
	Knot->GetOutputPin()->PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
	TestTrue(TEXT("entry connects to exec knot"), Schema->TryCreateConnection(EntryThen, Knot->GetInputPin()));
	TestTrue(TEXT("exec knot connects to result"), Schema->TryCreateConnection(Knot->GetOutputPin(), ResultExec));

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, ExportOptions);
	TestTrue(TEXT("function with exec knot exports"), Exported.bSuccess);
	TestFalse(TEXT("exec knot class is absent"), Exported.LispCode.Contains(TEXT("K2Node_Knot")));
	TestFalse(TEXT("localized reroute title is absent"), Exported.LispCode.Contains(TEXT("变更路线节点")));
	TestTrue(TEXT("downstream return remains present"), Exported.LispCode.Contains(TEXT("(return ")));

	FFixture Destination = MakeFixture(TEXT("BP_BL_ExecKnotDestination"), false);
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = false;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(Destination.Graph, Exported.LispCode, ImportOptions);
	TestTrue(TEXT("transparent exec knot function imports"), Imported.bSuccess);
	return true;
}

BL_TEST(FunctionReturn_CollapsedPureGraphRoundTrips)
bool FFunctionReturn_CollapsedPureGraphRoundTrips::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispFunctionReturnTest;
	FFixture Source = MakeFixture(TEXT("BP_BL_CollapsedReturnSource"), false);
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	TestNotNull(TEXT("K2 schema exists"), Schema);
	if (!Source.Graph || !Source.InputPin || !Source.ReturnPin || !Schema) return false;

	UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(Source.Graph);
	Composite->CreateNewGuid();
	Source.Graph->AddNode(Composite, false, false);
	Composite->PostPlacedNewNode();
	Composite->AllocateDefaultPins();
	UK2Node_Tunnel* EntryTunnel = Composite->GetEntryNode();
	UK2Node_Tunnel* ExitTunnel = Composite->GetExitNode();

	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	EntryTunnel->CreateUserDefinedPin(TEXT("Input"), BoolType, EGPD_Output, false);
	ExitTunnel->CreateUserDefinedPin(TEXT("Result"), BoolType, EGPD_Input, false);
	Composite->ReconstructNode();

	UFunction* NotFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Not_PreBool"));
	TestNotNull(TEXT("boolean not function exists"), NotFunction);
	if (!NotFunction) return false;
	UK2Node_CallFunction* NotNode = NewObject<UK2Node_CallFunction>(Composite->BoundGraph);
	NotNode->SetFromFunction(NotFunction);
	NotNode->CreateNewGuid();
	Composite->BoundGraph->AddNode(NotNode, false, false);
	NotNode->AllocateDefaultPins();

	UEdGraphPin* InnerInput = FindPin(EntryTunnel, TEXT("Input"), EGPD_Output);
	UEdGraphPin* NotInput = FindPin(NotNode, TEXT("A"), EGPD_Input);
	UEdGraphPin* NotOutput = FindPin(NotNode, TEXT("ReturnValue"), EGPD_Output);
	UEdGraphPin* InnerResult = FindPin(ExitTunnel, TEXT("Result"), EGPD_Input);
	UEdGraphPin* OuterInput = FindPin(Composite, TEXT("Input"), EGPD_Input);
	UEdGraphPin* OuterResult = FindPin(Composite, TEXT("Result"), EGPD_Output);
	TestTrue(TEXT("entry drives inner expression"), InnerInput && NotInput && Schema->TryCreateConnection(InnerInput, NotInput));
	TestTrue(TEXT("inner expression drives exit"), NotOutput && InnerResult && Schema->TryCreateConnection(NotOutput, InnerResult));
	TestTrue(TEXT("function input drives collapsed graph"), OuterInput && Schema->TryCreateConnection(Source.InputPin, OuterInput));
	TestTrue(TEXT("collapsed graph drives function result"), OuterResult && Schema->TryCreateConnection(OuterResult, Source.ReturnPin));

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, ExportOptions);
	TestTrue(TEXT("collapsed pure graph exports"), Exported.bSuccess);
	TestTrue(TEXT("collapsed graph has an explicit DSL form"), Exported.LispCode.Contains(TEXT("(collapsed-graph ")));
	TestTrue(TEXT("collapsed graph preserves its inner expression"), Exported.LispCode.Contains(TEXT("Not_PreBool")));
	TestFalse(TEXT("opaque composite class symbol is absent"), Exported.LispCode.Contains(TEXT("K2Node_Composite")));

	FFixture Destination = MakeFixture(TEXT("BP_BL_CollapsedReturnDestination"), false);
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = false;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(Destination.Graph, Exported.LispCode, ImportOptions);
	TestTrue(TEXT("collapsed graph imports"), Imported.bSuccess);

	UK2Node_Composite* ImportedComposite = nullptr;
	for (UEdGraphNode* Node : Destination.Graph->Nodes)
	{
		if (UK2Node_Composite* Candidate = Cast<UK2Node_Composite>(Node))
		{
			ImportedComposite = Candidate;
			break;
		}
	}
	TestNotNull(TEXT("import recreates UK2Node_Composite"), ImportedComposite);
	TestTrue(TEXT("import recreates the bound graph"), ImportedComposite && ImportedComposite->BoundGraph != nullptr);

	const FBlueprintLispResult ReExported = FBlueprintLispConverter::ExportGraph(Destination.Graph, ExportOptions);
	TestTrue(TEXT("imported collapsed graph re-exports"), ReExported.bSuccess);
	TestTrue(TEXT("re-export preserves collapsed graph semantics"),
		ReExported.LispCode.Contains(TEXT("(collapsed-graph ")) && ReExported.LispCode.Contains(TEXT("Not_PreBool")));
	return true;
}

BL_TEST(Lifecycle_PostNodeChanges_DeliveredWithAutoLayoutBehavior)
bool FLifecycle_PostNodeChanges_DeliveredWithAutoLayoutBehavior::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispImportLifecycle;
	using namespace BlueprintLispLifecycleTest;

	if (!FBlueprintLispModule::IsAvailable())
	{
		AddWarning(TEXT("BlueprintLisp module not loaded; skipping lifecycle test."));
		return true;
	}

	FBlueprintLispModule& Module = FBlueprintLispModule::Get();

	TSharedRef<FRecordingHook> Hook = MakeShared<FRecordingHook>();
	FImportLifecycleHookHandle Handle = Module.RegisterImportLifecycleHook(Hook);
	TestTrue(TEXT("hook handle valid"), Handle.IsValid());

	UEdGraph* Graph = NewObject<UEdGraph>(GetTransientPackage());
	UEdGraphNode* ChangedNode = NewObject<UEdGraphNode>(Graph);
	Graph->Nodes.Add(ChangedNode);

	FImportNodePhaseEvent Event;
	Event.Phase = EImportLifecyclePhase::PostNodeChanges;
	Event.Context.TargetGraph = Graph;
	Event.Context.bIsIncremental = true;
	Event.Context.RequestedBehaviors.Add(FName(TEXT("AutoLayout")));
	FImportNodeChange Change;
	Change.Node = ChangedNode;
	Change.ChangeType = EImportNodeChangeType::Added;
	Event.Changes.Add(Change);

	Module.BroadcastNodePhase(Event);

	TestEqual(TEXT("hook received exactly one node phase"), Hook->NodePhaseCount, 1);
	TestEqual(TEXT("phase is PostNodeChanges"),
		(int32)Hook->LastPhase, (int32)EImportLifecyclePhase::PostNodeChanges);
	TestTrue(TEXT("AutoLayout behavior propagated"),
		Hook->LastBehaviors.Contains(FName(TEXT("AutoLayout"))));
	TestEqual(TEXT("changed-node count propagated"), Hook->LastChangeCount, 1);

	Module.UnregisterImportLifecycleHook(Handle);

	Module.BroadcastNodePhase(Event);
	TestEqual(TEXT("no delivery after unregister"), Hook->NodePhaseCount, 1);

	return true;
}

BL_TEST(Lifecycle_HookPriorityOrdering)
bool FLifecycle_HookPriorityOrdering::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispImportLifecycle;
	using namespace BlueprintLispLifecycleTest;

	if (!FBlueprintLispModule::IsAvailable())
	{
		AddWarning(TEXT("BlueprintLisp module not loaded; skipping priority test."));
		return true;
	}

	FBlueprintLispModule& Module = FBlueprintLispModule::Get();

	FRecordingHook::NextGlobalOrder = 0;
	TSharedRef<FRecordingHook> EarlyHook = MakeShared<FRecordingHook>(/*Priority*/ 100);
	TSharedRef<FRecordingHook> LateHook  = MakeShared<FRecordingHook>(/*Priority*/ -10);

	FImportLifecycleHookHandle LateHandle  = Module.RegisterImportLifecycleHook(LateHook);
	FImportLifecycleHookHandle EarlyHandle = Module.RegisterImportLifecycleHook(EarlyHook);

	FImportNodePhaseEvent Event;
	Event.Phase = EImportLifecyclePhase::PostNodeChanges;
	Module.BroadcastNodePhase(Event);

	TestTrue(TEXT("high-priority hook ran before low-priority hook"),
		EarlyHook->OrderToken < LateHook->OrderToken);

	Module.UnregisterImportLifecycleHook(LateHandle);
	Module.UnregisterImportLifecycleHook(EarlyHandle);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

