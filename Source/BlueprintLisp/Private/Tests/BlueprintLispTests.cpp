// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// BlueprintLispTests.cpp - UE Automation Tests for BlueprintLisp AST/Parser
//
// Run via:
//   UnrealEditor.exe <project> -run=AutomationTests -filter="BlueprintLisp"
// Or in Editor:
//   Window -> Developer Tools -> Session Frontend -> Automation

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "BlueprintLispAST.h"
#include "K2Node_Knot.h"

#if WITH_DEV_AUTOMATION_TESTS

// ============================================================================
// Helper macros
// ============================================================================

// Standard test flags: runs in Editor + Commandlet context, ProductFilter
constexpr EAutomationTestFlags BL_FLAGS = EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::CommandletContext
	| EAutomationTestFlags::ProductFilter;

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

BL_TEST(Converter_Validate_Timeline)
bool FConverter_Validate_Timeline::RunTest(const FString& Parameters)
{
	const FString Code = TEXT(
		"(timeline \"WallTimeline\" :length 1 :length-mode timeline-length "
		":autoplay false :loop false :replicated false :ignore-time-dilation false "
		":track (float \"WallDiss\" :external false "
		":curve (rich-curve :key (key :time 0 :value 0) :key (key :time 1 :value 1))) "
		":update (SetScalarParameterValueOnMaterials \"Dissolve\" "
		"(timeline-output :timeline \"WallTimeline\" :out-pin \"WallDiss\" :id \"11223344\")) "
		":id \"11223344\")\n"
		"(event PlayWallDissAnim "
		"(timeline-control :timeline \"WallTimeline\" :action play-from-start))");
	const FBlueprintLispResult Result = FBlueprintLispConverter::Validate(Code);
	if (!Result.bSuccess) AddError(TEXT("timeline validation: ") + Result.Error);
	TestTrue(TEXT("timeline is an accepted top-level form"), Result.bSuccess);
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
#include "K2Node_CustomEvent.h"
#include "K2Node_GenericCreateObject.h"
#include "K2Node_Timeline.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/MeshComponent.h"
#include "Curves/CurveFloat.h"
#include "Animation/AnimInstance.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TimelineTemplate.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace BlueprintLispTimelineTest
{
	struct FFixture
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
	};

	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FName PinName, EEdGraphPinDirection Direction)
	{
		if (!Node) return nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName == PinName && Pin->Direction == Direction)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static bool AreLinked(UEdGraphPin* A, UEdGraphPin* B)
	{
		return A && B && (A->LinkedTo.Contains(B) || B->LinkedTo.Contains(A));
	}

	static FFixture MakeFixture(const FName BlueprintName)
	{
		FFixture Fixture;
		Fixture.Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AStaticMeshActor::StaticClass(), GetTransientPackage(), BlueprintName, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
			TEXT("BlueprintLispTimelineTest"));
		if (!Fixture.Blueprint) return Fixture;

		Fixture.Graph = FBlueprintEditorUtils::CreateNewGraph(
			Fixture.Blueprint, TEXT("EventGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!Fixture.Graph) return Fixture;
		FBlueprintEditorUtils::AddUbergraphPage(Fixture.Blueprint, Fixture.Graph);
		FKismetEditorUtilities::CompileBlueprint(
			Fixture.Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		return Fixture;
	}

	static int32 CountTimelineNodes(UEdGraph* Graph, const FName TimelineName)
	{
		int32 Count = 0;
		if (!Graph) return Count;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node);
			if (TimelineNode && TimelineNode->TimelineName == TimelineName) ++Count;
		}
		return Count;
	}

	static int32 CountTimelineTemplates(UBlueprint* Blueprint, const FName TimelineName)
	{
		int32 Count = 0;
		if (!Blueprint) return Count;
		for (const UTimelineTemplate* Template : Blueprint->Timelines)
		{
			if (Template && Template->GetVariableName() == TimelineName) ++Count;
		}
		return Count;
	}

	static UK2Node_Timeline* FindTimelineNode(UEdGraph* Graph, const FName TimelineName)
	{
		if (!Graph) return nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node);
			if (TimelineNode && TimelineNode->TimelineName == TimelineName) return TimelineNode;
		}
		return nullptr;
	}

	static UK2Node_CustomEvent* FindCustomEvent(UEdGraph* Graph, const FName EventName)
	{
		if (!Graph) return nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_CustomEvent* EventNode = Cast<UK2Node_CustomEvent>(Node);
			if (EventNode && EventNode->CustomFunctionName == EventName) return EventNode;
		}
		return nullptr;
	}

	static UK2Node_CallFunction* FindFunctionCall(UEdGraph* Graph, const FName FunctionName)
	{
		if (!Graph) return nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
			const UFunction* Function = CallNode ? CallNode->GetTargetFunction() : nullptr;
			if (Function && Function->GetFName() == FunctionName) return CallNode;
		}
		return nullptr;
	}
}

BL_TEST(Timeline_RoundTripsControlCallbackAndFloatOutput)
bool FTimeline_RoundTripsControlCallbackAndFloatOutput::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("WallTimeline"));
	const FName TrackName(TEXT("WallDiss"));
	const FName EventName(TEXT("PlayWallDissAnim"));
	const FName SetScalarFunctionName(TEXT("SetScalarParameterValueOnMaterials"));
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	const FFixture Source = MakeFixture(TEXT("BP_BL_TimelineSource"));
	TestNotNull(TEXT("source AStaticMeshActor Blueprint is created"), Source.Blueprint);
	TestNotNull(TEXT("source EventGraph is created"), Source.Graph);
	TestNotNull(TEXT("K2 schema exists"), Schema);
	if (!Source.Blueprint || !Source.Graph || !Schema || !Source.Blueprint->GeneratedClass) return false;

	UTimelineTemplate* SourceTemplate = FBlueprintEditorUtils::AddNewTimeline(Source.Blueprint, TimelineName);
	TestNotNull(TEXT("source Timeline template is created"), SourceTemplate);
	if (!SourceTemplate) return false;
	SourceTemplate->TimelineLength = 1.0f;
	SourceTemplate->LengthMode = TL_TimelineLength;
	SourceTemplate->bAutoPlay = false;
	SourceTemplate->bLoop = true;
	SourceTemplate->bReplicated = false;
	SourceTemplate->bIgnoreTimeDilation = true;
	SourceTemplate->MetaDataArray.Emplace(TEXT("Category"), TEXT("Gameplay|BombSite"));
	SourceTemplate->MetaDataArray.Emplace(TEXT("Tooltip"), TEXT("Animates the bomb-site wall dissolve."));

	UCurveFloat* SourceCurve = NewObject<UCurveFloat>(
		Source.Blueprint->GeneratedClass, NAME_None, RF_Public | RF_Transactional);
	TestNotNull(TEXT("source internal float curve is created"), SourceCurve);
	if (!SourceCurve) return false;
	TArray<FRichCurveKey> SourceKeys;
	FRichCurveKey FirstSourceKey(0.0f, 0.0f);
	FirstSourceKey.InterpMode = RCIM_Cubic;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
	FirstSourceKey.TangentMode = RCTM_SmartAuto;
#endif
	SourceKeys.Add(FirstSourceKey);
	SourceKeys.Add(FRichCurveKey(0.99999f, 0.0000004f));
	SourceCurve->FloatCurve.SetKeys(SourceKeys);

	FTTFloatTrack FloatTrack;
	FloatTrack.SetTrackName(TrackName, SourceTemplate);
	FloatTrack.bIsExternalCurve = false;
	FloatTrack.CurveFloat = SourceCurve;
	const int32 FloatTrackIndex = SourceTemplate->FloatTracks.Add(FloatTrack);
	SourceTemplate->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, FloatTrackIndex));

	UK2Node_Timeline* SourceTimelineNode = NewObject<UK2Node_Timeline>(Source.Graph);
	SourceTimelineNode->TimelineName = TimelineName;
	SourceTimelineNode->TimelineGuid = SourceTemplate->TimelineGuid;
	SourceTimelineNode->CreateNewGuid();
	Source.Graph->AddNode(SourceTimelineNode, false, false);
	SourceTimelineNode->PostPlacedNewNode();
	SourceTimelineNode->AllocateDefaultPins();

	UK2Node_CustomEvent* SourceEventNode = NewObject<UK2Node_CustomEvent>(Source.Graph);
	SourceEventNode->CustomFunctionName = EventName;
	SourceEventNode->CreateNewGuid();
	Source.Graph->AddNode(SourceEventNode, false, false);
	SourceEventNode->PostPlacedNewNode();
	SourceEventNode->AllocateDefaultPins();

	UK2Node_VariableGet* SourceMeshGetter = NewObject<UK2Node_VariableGet>(Source.Graph);
	SourceMeshGetter->VariableReference.SetSelfMember(TEXT("StaticMeshComponent"));
	SourceMeshGetter->CreateNewGuid();
	Source.Graph->AddNode(SourceMeshGetter, false, false);
	SourceMeshGetter->PostPlacedNewNode();
	SourceMeshGetter->AllocateDefaultPins();

	UFunction* SetScalarFunction = UMeshComponent::StaticClass()->FindFunctionByName(SetScalarFunctionName);
	TestNotNull(TEXT("SetScalarParameterValueOnMaterials function exists"), SetScalarFunction);
	if (!SetScalarFunction) return false;
	UK2Node_CallFunction* SourceSetScalarNode = NewObject<UK2Node_CallFunction>(Source.Graph);
	SourceSetScalarNode->SetFromFunction(SetScalarFunction);
	SourceSetScalarNode->CreateNewGuid();
	Source.Graph->AddNode(SourceSetScalarNode, false, false);
	SourceSetScalarNode->AllocateDefaultPins();

	UEdGraphPin* SourceEventThen = FindPin(SourceEventNode, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* SourceMeshValue = FindPin(SourceMeshGetter, TEXT("StaticMeshComponent"), EGPD_Output);
	UEdGraphPin* SourceCallTarget = FindPin(SourceSetScalarNode, UEdGraphSchema_K2::PN_Self, EGPD_Input);
	UEdGraphPin* SourceParameterName = FindPin(SourceSetScalarNode, TEXT("ParameterName"), EGPD_Input);
	UEdGraphPin* SourceParameterValue = FindPin(SourceSetScalarNode, TEXT("ParameterValue"), EGPD_Input);
	UEdGraphPin* SourceTrackOutput = FindPin(SourceTimelineNode, TrackName, EGPD_Output);
	TestNotNull(TEXT("custom event Then pin exists"), SourceEventThen);
	TestNotNull(TEXT("StaticMeshComponent getter output exists"), SourceMeshValue);
	TestNotNull(TEXT("material call target pin exists"), SourceCallTarget);
	TestNotNull(TEXT("material parameter name pin exists"), SourceParameterName);
	TestNotNull(TEXT("material parameter value pin exists"), SourceParameterValue);
	TestNotNull(TEXT("Timeline float output pin exists"), SourceTrackOutput);
	if (!SourceEventThen || !SourceMeshValue || !SourceCallTarget || !SourceParameterName
		|| !SourceParameterValue || !SourceTrackOutput) return false;

	Schema->TrySetDefaultValue(*SourceParameterName, TEXT("Dissolve"));
	TestTrue(TEXT("CustomEvent drives PlayFromStart"),
		Schema->TryCreateConnection(SourceEventThen, SourceTimelineNode->GetPlayFromStartPin()));
	TestTrue(TEXT("Timeline Update drives material call"),
		Schema->TryCreateConnection(SourceTimelineNode->GetUpdatePin(), SourceSetScalarNode->GetExecPin()));
	TestTrue(TEXT("Timeline float output drives ParameterValue"),
		Schema->TryCreateConnection(SourceTrackOutput, SourceParameterValue));
	TestTrue(TEXT("StaticMeshComponent drives material call target"),
		Schema->TryCreateConnection(SourceMeshValue, SourceCallTarget));

	FBlueprintLispConverter::FExportOptions PureExpressionOptions;
	PureExpressionOptions.bPrettyPrint = false;
	PureExpressionOptions.bStableIds = true;
	const FBlueprintLispResult ExportedPureExpression =
		FBlueprintLispConverter::ExportPureExpression(SourceParameterValue, PureExpressionOptions);
	if (!ExportedPureExpression.bSuccess)
	{
		AddError(TEXT("Timeline pure-expression export: ") + ExportedPureExpression.Error);
	}
	TestTrue(TEXT("Timeline output exports as a standalone pure expression"), ExportedPureExpression.bSuccess);
	TestTrue(TEXT("standalone expression uses timeline-output"),
		ExportedPureExpression.LispCode.Contains(TEXT("(timeline-output :timeline \"WallTimeline\" :out-pin \"WallDiss\"")));
	if (!ExportedPureExpression.bSuccess) return false;

	SourceParameterValue->BreakAllPinLinks();
	TestEqual(TEXT("ParameterValue is disconnected before pure-expression import"), SourceParameterValue->LinkedTo.Num(), 0);
	const FBlueprintLispResult ImportedPureExpression = FBlueprintLispConverter::ImportPureExpression(
		Source.Graph, SourceParameterValue, ExportedPureExpression.LispCode);
	if (!ImportedPureExpression.bSuccess)
	{
		AddError(TEXT("Timeline pure-expression import: ") + ImportedPureExpression.Error);
	}
	TestTrue(TEXT("Timeline output imports into the same graph without a Timeline context map"),
		ImportedPureExpression.bSuccess);
	TestTrue(TEXT("pure-expression import restores WallDiss to ParameterValue"),
		AreLinked(SourceTrackOutput, SourceParameterValue));
	TestEqual(TEXT("pure-expression import does not duplicate the Timeline node"),
		CountTimelineNodes(Source.Graph, TimelineName), 1);
	if (!ImportedPureExpression.bSuccess) return false;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Source.Blueprint);
	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bIncludePositions = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, ExportOptions);
	if (!Exported.bSuccess) AddError(TEXT("Timeline export: ") + Exported.Error);
	TestTrue(TEXT("Timeline graph exports"), Exported.bSuccess);
	TestTrue(TEXT("export contains Timeline definition"),
		Exported.LispCode.Contains(TEXT("(timeline \"WallTimeline\"")));
	TestTrue(TEXT("export contains internal float track"),
		Exported.LispCode.Contains(TEXT(":track (float \"WallDiss\"")));
	TestTrue(TEXT("export contains Timeline Category metadata"),
		Exported.LispCode.Contains(TEXT(":metadata (\"Category\" \"Gameplay|BombSite\")")));
	TestTrue(TEXT("export contains Timeline Tooltip metadata"),
		Exported.LispCode.Contains(TEXT(":metadata (\"Tooltip\" \"Animates the bomb-site wall dissolve.\")")));
	TestTrue(TEXT("export contains PlayFromStart control"),
		Exported.LispCode.Contains(TEXT("(timeline-control :timeline \"WallTimeline\" :action play-from-start)")));
	TestTrue(TEXT("export contains Timeline float expression"),
		Exported.LispCode.Contains(TEXT("(timeline-output :timeline \"WallTimeline\" :out-pin \"WallDiss\"")));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
	TestTrue(TEXT("export preserves SmartAuto tangent mode"),
		Exported.LispCode.Contains(TEXT(":tangent smart-auto")));
#endif
	if (!Exported.bSuccess) return false;

	const FLispParseResult ExportedParse = FLispParser::Parse(Exported.LispCode);
	TestTrue(TEXT("exported Timeline DSL parses for numeric fidelity checks"), ExportedParse.bSuccess);
	FLispNodePtr ExportedTimelineForm;
	for (const FLispNodePtr& Form : ExportedParse.Nodes)
	{
		if (Form.IsValid() && Form->IsForm(TEXT("timeline")))
		{
			ExportedTimelineForm = Form;
			break;
		}
	}
	const FLispNodePtr ExportedTrackForm = ExportedTimelineForm.IsValid()
		? ExportedTimelineForm->GetKeywordArg(TEXT(":track")) : nullptr;
	const FLispNodePtr ExportedCurveForm = ExportedTrackForm.IsValid()
		? ExportedTrackForm->GetKeywordArg(TEXT(":curve")) : nullptr;
	TArray<FLispNodePtr> ExportedCurveKeys;
	if (ExportedCurveForm.IsValid())
	{
		for (int32 Index = 1; Index + 1 < ExportedCurveForm->Num(); ++Index)
		{
			const FLispNodePtr Keyword = ExportedCurveForm->Get(Index);
			if (Keyword.IsValid() && Keyword->IsKeyword()
				&& Keyword->StringValue.Equals(TEXT(":key"), ESearchCase::IgnoreCase))
			{
				ExportedCurveKeys.Add(ExportedCurveForm->Get(++Index));
			}
		}
	}
	TestEqual(TEXT("export retains both float curve keys"), ExportedCurveKeys.Num(), 2);
	if (ExportedCurveKeys.Num() == 2)
	{
		const FLispNodePtr PreciseTime = ExportedCurveKeys[1]->GetKeywordArg(TEXT(":time"));
		const FLispNodePtr PreciseValue = ExportedCurveKeys[1]->GetKeywordArg(TEXT(":value"));
		TestTrue(TEXT("export preserves curve key time 0.99999"),
			PreciseTime.IsValid() && PreciseTime->IsNumber()
			&& static_cast<float>(PreciseTime->NumberValue) == SourceKeys[1].Time);
		TestTrue(TEXT("export preserves curve key value 0.0000004"),
			PreciseValue.IsValid() && PreciseValue->IsNumber()
			&& static_cast<float>(PreciseValue->NumberValue) == SourceKeys[1].Value);
	}

	const FFixture Destination = MakeFixture(TEXT("BP_BL_TimelineDestination"));
	TestNotNull(TEXT("destination AStaticMeshActor Blueprint is created"), Destination.Blueprint);
	TestNotNull(TEXT("destination EventGraph is created"), Destination.Graph);
	if (!Destination.Blueprint || !Destination.Graph) return false;

	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = true;
	const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(
		Destination.Graph, Exported.LispCode, ImportOptions);
	if (!Imported.bSuccess) AddError(TEXT("Timeline import: ") + Imported.Error);
	TestTrue(TEXT("Timeline DSL imports"), Imported.bSuccess);
	TestTrue(TEXT("imported Blueprint compiles without errors"), Destination.Blueprint->Status != BS_Error);
	if (!Imported.bSuccess) return false;

	UK2Node_Timeline* ImportedTimelineNode = nullptr;
	UK2Node_CustomEvent* ImportedEventNode = nullptr;
	UK2Node_CallFunction* ImportedSetScalarNode = nullptr;
	for (UEdGraphNode* Node : Destination.Graph->Nodes)
	{
		if (UK2Node_Timeline* Candidate = Cast<UK2Node_Timeline>(Node))
		{
			if (Candidate->TimelineName == TimelineName) ImportedTimelineNode = Candidate;
		}
		if (UK2Node_CustomEvent* Candidate = Cast<UK2Node_CustomEvent>(Node))
		{
			if (Candidate->CustomFunctionName == EventName) ImportedEventNode = Candidate;
		}
		if (UK2Node_CallFunction* Candidate = Cast<UK2Node_CallFunction>(Node))
		{
			const UFunction* Function = Candidate->GetTargetFunction();
			if (Function && Function->GetFName() == SetScalarFunctionName) ImportedSetScalarNode = Candidate;
		}
	}
	TestNotNull(TEXT("import restores Timeline node"), ImportedTimelineNode);
	TestNotNull(TEXT("import restores control CustomEvent"), ImportedEventNode);
	TestNotNull(TEXT("import restores material parameter call"), ImportedSetScalarNode);

	UTimelineTemplate* ImportedTemplate =
		Destination.Blueprint->FindTimelineTemplateByVariableName(TimelineName);
	TestNotNull(TEXT("import restores Timeline template"), ImportedTemplate);
	if (ImportedTemplate)
	{
		TestTrue(TEXT("Timeline length is preserved"), FMath::IsNearlyEqual(ImportedTemplate->TimelineLength, 1.0f));
		TestEqual(TEXT("Timeline length mode is preserved"), ImportedTemplate->LengthMode.GetValue(), TL_TimelineLength);
		TestTrue(TEXT("Timeline loop flag is preserved"), ImportedTemplate->bLoop != 0);
		TestTrue(TEXT("Timeline ignore-time-dilation flag is preserved"), ImportedTemplate->bIgnoreTimeDilation != 0);
		FString ImportedCategory;
		FString ImportedTooltip;
		for (const FBPVariableMetaDataEntry& Entry : ImportedTemplate->MetaDataArray)
		{
			if (Entry.DataKey == TEXT("Category")) ImportedCategory = Entry.DataValue;
			if (Entry.DataKey == TEXT("Tooltip")) ImportedTooltip = Entry.DataValue;
		}
		TestEqual(TEXT("Timeline Category metadata is preserved"), ImportedCategory, FString(TEXT("Gameplay|BombSite")));
		TestEqual(TEXT("Timeline Tooltip metadata is preserved"),
			ImportedTooltip, FString(TEXT("Animates the bomb-site wall dissolve.")));
		TestEqual(TEXT("one float track is restored"), ImportedTemplate->FloatTracks.Num(), 1);
		if (ImportedTemplate->FloatTracks.Num() == 1)
		{
			const FTTFloatTrack& ImportedTrack = ImportedTemplate->FloatTracks[0];
			TestEqual(TEXT("float track name is preserved"), ImportedTrack.GetTrackName(), TrackName);
			UCurveFloat* ImportedCurve = ImportedTrack.CurveFloat;
			TestNotNull(TEXT("internal float curve is restored"), ImportedCurve);
			if (ImportedCurve)
			{
				const TArray<FRichCurveKey>& ImportedKeys = ImportedCurve->FloatCurve.GetConstRefOfKeys();
				TestEqual(TEXT("two float curve keys are restored"), ImportedKeys.Num(), 2);
				if (ImportedKeys.Num() == 2)
				{
					TestTrue(TEXT("first key is preserved"),
						FMath::IsNearlyEqual(ImportedKeys[0].Time, 0.0f)
						&& FMath::IsNearlyEqual(ImportedKeys[0].Value, 0.0f));
					TestTrue(TEXT("second key is preserved"),
						ImportedKeys[1].Time == SourceKeys[1].Time
						&& ImportedKeys[1].Value == SourceKeys[1].Value);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
					TestEqual(TEXT("SmartAuto tangent mode is restored"),
						ImportedKeys[0].TangentMode.GetValue(), RCTM_SmartAuto);
#endif
				}
			}
		}
	}

	if (ImportedTimelineNode && ImportedEventNode && ImportedSetScalarNode)
	{
		UEdGraphPin* ImportedEventThen = FindPin(ImportedEventNode, UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* ImportedTrackOutput = FindPin(ImportedTimelineNode, TrackName, EGPD_Output);
		UEdGraphPin* ImportedParameterValue = FindPin(ImportedSetScalarNode, TEXT("ParameterValue"), EGPD_Input);
		UEdGraphPin* ImportedParameterName = FindPin(ImportedSetScalarNode, TEXT("ParameterName"), EGPD_Input);
		TestTrue(TEXT("import restores CustomEvent to PlayFromStart link"),
			AreLinked(ImportedEventThen, ImportedTimelineNode->GetPlayFromStartPin()));
		TestTrue(TEXT("import restores Update to material call link"),
			AreLinked(ImportedTimelineNode->GetUpdatePin(), ImportedSetScalarNode->GetExecPin()));
		TestTrue(TEXT("import restores WallDiss to ParameterValue link"),
			AreLinked(ImportedTrackOutput, ImportedParameterValue));
		TestEqual(TEXT("import preserves material parameter name"),
			ImportedParameterName ? ImportedParameterName->DefaultValue : FString(), FString(TEXT("Dissolve")));
	}

	const FBlueprintLispResult ReExported = FBlueprintLispConverter::ExportGraph(Destination.Graph, ExportOptions);
	if (!ReExported.bSuccess) AddError(TEXT("Timeline re-export: ") + ReExported.Error);
	TestTrue(TEXT("imported Timeline graph re-exports"), ReExported.bSuccess);
	if (ReExported.bSuccess)
	{
		TestEqual(TEXT("Timeline DSL is stable across export/import/re-export"),
			BlueprintLisp::Minify(ReExported.LispCode), BlueprintLisp::Minify(Exported.LispCode));
	}
	return true;
}

BL_TEST(Timeline_RoundTripsCustomEventValueIntoCallback)
bool FTimeline_RoundTripsCustomEventValueIntoCallback::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("CrossRootTimeline"));
	const FName EventName(TEXT("Start"));
	const FName ValuePinName(TEXT("Value"));
	const FName FunctionName(TEXT("SetActorTickInterval"));
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	const FFixture Source = MakeFixture(TEXT("BP_BL_TimelineCrossRootSource"));
	TestNotNull(TEXT("cross-root source Blueprint is created"), Source.Blueprint);
	TestNotNull(TEXT("cross-root source EventGraph is created"), Source.Graph);
	TestNotNull(TEXT("cross-root K2 schema exists"), Schema);
	if (!Source.Blueprint || !Source.Graph || !Schema) return false;

	UTimelineTemplate* SourceTemplate =
		FBlueprintEditorUtils::AddNewTimeline(Source.Blueprint, TimelineName);
	TestNotNull(TEXT("cross-root source Timeline template is created"), SourceTemplate);
	if (!SourceTemplate) return false;
	SourceTemplate->TimelineLength = 1.0f;
	SourceTemplate->LengthMode = TL_TimelineLength;

	UK2Node_Timeline* SourceTimeline = NewObject<UK2Node_Timeline>(Source.Graph);
	SourceTimeline->TimelineName = TimelineName;
	SourceTimeline->TimelineGuid = SourceTemplate->TimelineGuid;
	SourceTimeline->CreateNewGuid();
	Source.Graph->AddNode(SourceTimeline, false, false);
	SourceTimeline->PostPlacedNewNode();
	SourceTimeline->AllocateDefaultPins();

	UK2Node_CustomEvent* SourceEvent = NewObject<UK2Node_CustomEvent>(Source.Graph);
	SourceEvent->CustomFunctionName = EventName;
	SourceEvent->CreateNewGuid();
	Source.Graph->AddNode(SourceEvent, false, false);
	SourceEvent->PostPlacedNewNode();
	SourceEvent->AllocateDefaultPins();
	FEdGraphPinType ValuePinType;
	ValuePinType.PinCategory = UEdGraphSchema_K2::PC_Real;
	ValuePinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	TestNotNull(TEXT("cross-root CustomEvent float parameter is created"),
		SourceEvent->CreateUserDefinedPin(ValuePinName, ValuePinType, EGPD_Output, false));
	SourceEvent->ReconstructNode();

	UFunction* SetTickIntervalFunction =
		AActor::StaticClass()->FindFunctionByName(FunctionName);
	TestNotNull(TEXT("cross-root callback function exists"), SetTickIntervalFunction);
	if (!SetTickIntervalFunction) return false;
	UK2Node_CallFunction* SourceCall = NewObject<UK2Node_CallFunction>(Source.Graph);
	SourceCall->SetFromFunction(SetTickIntervalFunction);
	SourceCall->CreateNewGuid();
	Source.Graph->AddNode(SourceCall, false, false);
	SourceCall->AllocateDefaultPins();

	UEdGraphPin* SourceEventThen = FindPin(SourceEvent, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* SourceEventValue = FindPin(SourceEvent, ValuePinName, EGPD_Output);
	UEdGraphPin* SourceTickInterval = FindPin(SourceCall, TEXT("TickInterval"), EGPD_Input);
	TestNotNull(TEXT("cross-root source event Then pin exists"), SourceEventThen);
	TestNotNull(TEXT("cross-root source event Value pin exists"), SourceEventValue);
	TestNotNull(TEXT("cross-root source call TickInterval pin exists"), SourceTickInterval);
	if (!SourceEventThen || !SourceEventValue || !SourceTickInterval) return false;
	TestTrue(TEXT("cross-root CustomEvent drives Timeline Play"),
		Schema->TryCreateConnection(SourceEventThen, SourceTimeline->GetPlayPin()));
	TestTrue(TEXT("cross-root Timeline Update drives impure call"),
		Schema->TryCreateConnection(SourceTimeline->GetUpdatePin(), SourceCall->GetExecPin()));
	TestTrue(TEXT("cross-root CustomEvent Value directly feeds callback call"),
		Schema->TryCreateConnection(SourceEventValue, SourceTickInterval));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Source.Blueprint);
	FKismetEditorUtilities::CompileBlueprint(
		Source.Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	TestTrue(TEXT("cross-root source Blueprint compiles"), Source.Blueprint->Status != BS_Error);

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bIncludePositions = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult Exported =
		FBlueprintLispConverter::ExportGraph(Source.Graph, ExportOptions);
	if (!Exported.bSuccess) AddError(TEXT("cross-root Timeline export: ") + Exported.Error);
	TestTrue(TEXT("cross-root Timeline graph exports"), Exported.bSuccess);
	TestTrue(TEXT("cross-root export declares the event float parameter"),
		Exported.LispCode.Contains(TEXT(":param (Value float)")));
	TestTrue(TEXT("cross-root export represents the callback data input as event Value"),
		Exported.LispCode.Contains(TEXT(":tickinterval Value")));
	if (!Exported.bSuccess) return false;

	const FFixture Destination = MakeFixture(TEXT("BP_BL_TimelineCrossRootDestination"));
	TestNotNull(TEXT("cross-root destination Blueprint is created"), Destination.Blueprint);
	TestNotNull(TEXT("cross-root destination EventGraph is created"), Destination.Graph);
	if (!Destination.Blueprint || !Destination.Graph) return false;
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = true;
	const FBlueprintLispResult Imported =
		FBlueprintLispConverter::ImportGraph(Destination.Graph, Exported.LispCode, ImportOptions);
	if (!Imported.bSuccess) AddError(TEXT("cross-root Timeline ReplaceGraph import: ") + Imported.Error);
	TestTrue(TEXT("cross-root Timeline DSL imports with ReplaceGraph"), Imported.bSuccess);
	TestTrue(TEXT("cross-root imported Blueprint compiles"), Destination.Blueprint->Status != BS_Error);
	if (!Imported.bSuccess) return false;

	UK2Node_Timeline* ImportedTimeline = FindTimelineNode(Destination.Graph, TimelineName);
	UK2Node_CustomEvent* ImportedEvent = FindCustomEvent(Destination.Graph, EventName);
	UK2Node_CallFunction* ImportedCall = FindFunctionCall(Destination.Graph, FunctionName);
	TestNotNull(TEXT("cross-root import restores Timeline"), ImportedTimeline);
	TestNotNull(TEXT("cross-root import restores CustomEvent"), ImportedEvent);
	TestNotNull(TEXT("cross-root import restores callback call"), ImportedCall);
	if (!ImportedTimeline || !ImportedEvent || !ImportedCall) return false;
	UEdGraphPin* ImportedEventThen = FindPin(ImportedEvent, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* ImportedEventValue = FindPin(ImportedEvent, ValuePinName, EGPD_Output);
	UEdGraphPin* ImportedTickInterval = FindPin(ImportedCall, TEXT("TickInterval"), EGPD_Input);
	TestTrue(TEXT("cross-root import restores Event to Timeline Play link"),
		AreLinked(ImportedEventThen, ImportedTimeline->GetPlayPin()));
	TestTrue(TEXT("cross-root import restores Timeline Update callback link"),
		AreLinked(ImportedTimeline->GetUpdatePin(), ImportedCall->GetExecPin()));
	TestTrue(TEXT("cross-root import restores direct Event.Value callback data link"),
		AreLinked(ImportedEventValue, ImportedTickInterval));
	TestEqual(TEXT("callback input has exactly one direct event-parameter source"),
		ImportedTickInterval ? ImportedTickInterval->LinkedTo.Num() : 0, 1);

	const FBlueprintLispResult ReExported =
		FBlueprintLispConverter::ExportGraph(Destination.Graph, ExportOptions);
	if (!ReExported.bSuccess) AddError(TEXT("cross-root Timeline re-export: ") + ReExported.Error);
	TestTrue(TEXT("cross-root imported graph re-exports"), ReExported.bSuccess);
	if (ReExported.bSuccess)
	{
		TestEqual(TEXT("cross-root Timeline DSL is stable across round-trip"),
			BlueprintLisp::Minify(ReExported.LispCode), BlueprintLisp::Minify(Exported.LispCode));
	}
	return true;
}

BL_TEST(Timeline_ConvergentCallbacksRoundTripThroughExecReference)
bool FTimeline_ConvergentCallbacksRoundTripThroughExecReference::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("ConvergentTimeline"));
	const FName FunctionName(TEXT("SetActorTickInterval"));
	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineConvergentRoots"));
	TestNotNull(TEXT("convergent-root test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("convergent-root test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	UTimelineTemplate* TimelineTemplate =
		FBlueprintEditorUtils::AddNewTimeline(Fixture.Blueprint, TimelineName);
	TestNotNull(TEXT("convergent-root Timeline template is created"), TimelineTemplate);
	if (!TimelineTemplate) return false;
	TimelineTemplate->TimelineLength = 1.0f;
	TimelineTemplate->LengthMode = TL_TimelineLength;

	UK2Node_Timeline* TimelineNode = NewObject<UK2Node_Timeline>(Fixture.Graph);
	TimelineNode->TimelineName = TimelineName;
	TimelineNode->TimelineGuid = TimelineTemplate->TimelineGuid;
	TimelineNode->CreateNewGuid();
	Fixture.Graph->AddNode(TimelineNode, false, false);
	TimelineNode->PostPlacedNewNode();
	TimelineNode->AllocateDefaultPins();

	UFunction* SetTickIntervalFunction =
		AActor::StaticClass()->FindFunctionByName(FunctionName);
	TestNotNull(TEXT("convergent-root impure function exists"), SetTickIntervalFunction);
	if (!SetTickIntervalFunction) return false;
	UK2Node_CallFunction* SharedCall = NewObject<UK2Node_CallFunction>(Fixture.Graph);
	SharedCall->SetFromFunction(SetTickIntervalFunction);
	SharedCall->CreateNewGuid();
	Fixture.Graph->AddNode(SharedCall, false, false);
	SharedCall->AllocateDefaultPins();

	UEdGraphPin* SharedExecInput = SharedCall->GetExecPin();
	UEdGraphPin* UpdatePin = TimelineNode->GetUpdatePin();
	UEdGraphPin* FinishedPin = TimelineNode->GetFinishedPin();
	TestNotNull(TEXT("convergent-root call exec input exists"), SharedExecInput);
	TestNotNull(TEXT("convergent-root Timeline Update pin exists"), UpdatePin);
	TestNotNull(TEXT("convergent-root Timeline Finished pin exists"), FinishedPin);
	if (!SharedExecInput || !UpdatePin || !FinishedPin) return false;
	UpdatePin->MakeLinkTo(SharedExecInput);
	FinishedPin->MakeLinkTo(SharedExecInput);
	TestEqual(TEXT("two independent Timeline roots converge on one impure call"),
		SharedExecInput->LinkedTo.Num(), 2);
	TestTrue(TEXT("Timeline Update reaches the shared call"), AreLinked(UpdatePin, SharedExecInput));
	TestTrue(TEXT("Timeline Finished reaches the shared call"), AreLinked(FinishedPin, SharedExecInput));

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bIncludePositions = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult Exported =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	if (!Exported.bSuccess) AddError(TEXT("convergent callback export: ") + Exported.Error);
	TestTrue(TEXT("export supports convergent callback ownership"), Exported.bSuccess);
	TestTrue(TEXT("export emits an execution reference for the shared call"),
		Exported.LispCode.Contains(TEXT("(exec-ref :id")));
	if (!Exported.bSuccess) return false;

	const FFixture Destination = MakeFixture(TEXT("BP_BL_TimelineConvergentDestination"));
	TestNotNull(TEXT("convergent destination Blueprint is created"), Destination.Blueprint);
	TestNotNull(TEXT("convergent destination EventGraph is created"), Destination.Graph);
	if (!Destination.Blueprint || !Destination.Graph) return false;
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = true;
	const FBlueprintLispResult Imported =
		FBlueprintLispConverter::ImportGraph(Destination.Graph, Exported.LispCode, ImportOptions);
	if (!Imported.bSuccess) AddError(TEXT("convergent callback import: ") + Imported.Error);
	TestTrue(TEXT("execution reference imports with ReplaceGraph"), Imported.bSuccess);
	TestTrue(TEXT("convergent imported Blueprint compiles"), Destination.Blueprint->Status != BS_Error);
	if (!Imported.bSuccess) return false;

	UK2Node_Timeline* ImportedTimeline = FindTimelineNode(Destination.Graph, TimelineName);
	UK2Node_CallFunction* ImportedCall = FindFunctionCall(Destination.Graph, FunctionName);
	TestNotNull(TEXT("convergent import restores Timeline"), ImportedTimeline);
	TestNotNull(TEXT("convergent import restores shared call"), ImportedCall);
	if (!ImportedTimeline || !ImportedCall) return false;
	UEdGraphPin* ImportedExecInput = ImportedCall->GetExecPin();
	TestEqual(TEXT("convergent import creates one shared call node"),
		Destination.Graph->Nodes.FilterByPredicate([FunctionName](const UEdGraphNode* Node)
		{
			const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
			return Call && Call->FunctionReference.GetMemberName() == FunctionName;
		}).Num(), 1);
	TestTrue(TEXT("Timeline Update reaches the imported shared call"),
		AreLinked(ImportedTimeline->GetUpdatePin(), ImportedExecInput));
	TestTrue(TEXT("Timeline Finished reaches the imported shared call"),
		AreLinked(ImportedTimeline->GetFinishedPin(), ImportedExecInput));
	TestEqual(TEXT("shared call keeps both incoming execution paths"),
		ImportedExecInput ? ImportedExecInput->LinkedTo.Num() : 0, 2);

	const FBlueprintLispResult ReExported =
		FBlueprintLispConverter::ExportGraph(Destination.Graph, ExportOptions);
	if (!ReExported.bSuccess) AddError(TEXT("convergent callback re-export: ") + ReExported.Error);
	TestTrue(TEXT("convergent imported graph re-exports"), ReExported.bSuccess);
	if (ReExported.bSuccess)
	{
		TestEqual(TEXT("convergent callback DSL is stable across round-trip"),
			BlueprintLisp::Minify(ReExported.LispCode), BlueprintLisp::Minify(Exported.LispCode));
	}
	return true;
}

BL_TEST(Timeline_FirstMergePreservesExportedShortStableIdGuid)
bool FTimeline_FirstMergePreservesExportedShortStableIdGuid::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("FirstMergeTimeline"));
	const FName FunctionName(TEXT("SetActorTickInterval"));
	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineFirstMergeStableId"));
	TestNotNull(TEXT("first-Merge stable-id test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("first-Merge stable-id test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	UTimelineTemplate* TimelineTemplate =
		FBlueprintEditorUtils::AddNewTimeline(Fixture.Blueprint, TimelineName);
	TestNotNull(TEXT("first-Merge Timeline template is created"), TimelineTemplate);
	if (!TimelineTemplate) return false;
	TimelineTemplate->TimelineLength = 1.0f;
	TimelineTemplate->LengthMode = TL_TimelineLength;

	UK2Node_Timeline* TimelineNode = NewObject<UK2Node_Timeline>(Fixture.Graph);
	TimelineNode->TimelineName = TimelineName;
	TimelineNode->TimelineGuid = TimelineTemplate->TimelineGuid;
	TimelineNode->CreateNewGuid();
	Fixture.Graph->AddNode(TimelineNode, false, false);
	TimelineNode->PostPlacedNewNode();
	TimelineNode->AllocateDefaultPins();

	UFunction* SetTickIntervalFunction =
		AActor::StaticClass()->FindFunctionByName(FunctionName);
	TestNotNull(TEXT("first-Merge callback function exists"), SetTickIntervalFunction);
	if (!SetTickIntervalFunction) return false;
	UK2Node_CallFunction* CallbackCall = NewObject<UK2Node_CallFunction>(Fixture.Graph);
	CallbackCall->SetFromFunction(SetTickIntervalFunction);
	CallbackCall->CreateNewGuid();
	Fixture.Graph->AddNode(CallbackCall, false, false);
	CallbackCall->AllocateDefaultPins();
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* TickInterval = FindPin(CallbackCall, TEXT("TickInterval"), EGPD_Input);
	TestNotNull(TEXT("first-Merge callback TickInterval pin exists"), TickInterval);
	TestTrue(TEXT("first-Merge Timeline Update connects to callback"),
		Schema && Schema->TryCreateConnection(TimelineNode->GetUpdatePin(), CallbackCall->GetExecPin()));
	if (Schema && TickInterval) Schema->TrySetDefaultValue(*TickInterval, TEXT("0.5"));

	const FGuid FullCallGuidBefore = CallbackCall->NodeGuid;
	const FGuid FullTimelineGuidBefore = TimelineNode->NodeGuid;
	const int32 NodeCountBefore = Fixture.Graph->Nodes.Num();
	TestTrue(TEXT("hand-authored callback starts with a valid full GUID"), FullCallGuidBefore.IsValid());

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bIncludePositions = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult Exported =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	if (!Exported.bSuccess) AddError(TEXT("first-Merge stable-id export: ") + Exported.Error);
	TestTrue(TEXT("hand-authored Timeline callback exports"), Exported.bSuccess);
	if (!Exported.bSuccess) return false;

	const FLispParseResult Parsed = FLispParser::Parse(Exported.LispCode);
	TestTrue(TEXT("exported first-Merge DSL parses"), Parsed.bSuccess);
	FLispNodePtr TimelineForm;
	for (const FLispNodePtr& Form : Parsed.Nodes)
	{
		if (Form.IsValid() && Form->IsForm(TEXT("timeline")))
		{
			TimelineForm = Form;
			break;
		}
	}
	const FLispNodePtr CallbackForm = TimelineForm.IsValid()
		? TimelineForm->GetKeywordArg(TEXT(":update")) : nullptr;
	const FLispNodePtr CallbackIdNode = CallbackForm.IsValid()
		? CallbackForm->GetKeywordArg(TEXT(":id")) : nullptr;
	const FString ExportedShortId = CallbackIdNode.IsValid() ? CallbackIdNode->StringValue : FString();
	const FString FullCallGuidDigits = FullCallGuidBefore.ToString(EGuidFormats::Digits);
	TestTrue(TEXT("export assigns a non-empty short callback id"),
		!ExportedShortId.IsEmpty() && ExportedShortId.Len() < FullCallGuidDigits.Len());
	TestTrue(TEXT("exported callback id is a prefix of the full GUID"),
		FullCallGuidDigits.StartsWith(ExportedShortId, ESearchCase::IgnoreCase));

	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::MergeAppend;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = true;
	const FBlueprintLispResult Imported =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, Exported.LispCode, ImportOptions);
	if (!Imported.bSuccess) AddError(TEXT("first MergeAppend of exported callback: ") + Imported.Error);
	TestTrue(TEXT("exported callback DSL imports on its first MergeAppend"), Imported.bSuccess);
	TestTrue(TEXT("first MergeAppend graph compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!Imported.bSuccess) return false;

	UK2Node_Timeline* TimelineAfter = FindTimelineNode(Fixture.Graph, TimelineName);
	UK2Node_CallFunction* CallbackCallAfter = FindFunctionCall(Fixture.Graph, FunctionName);
	TestTrue(TEXT("first MergeAppend reuses hand-authored Timeline pointer"), TimelineAfter == TimelineNode);
	TestTrue(TEXT("first MergeAppend reuses hand-authored callback Call pointer"),
		CallbackCallAfter == CallbackCall);
	TestTrue(TEXT("first MergeAppend preserves the callback full GUID"),
		CallbackCallAfter && CallbackCallAfter->NodeGuid == FullCallGuidBefore);
	TestTrue(TEXT("first MergeAppend preserves the Timeline full GUID"),
		TimelineAfter && TimelineAfter->NodeGuid == FullTimelineGuidBefore);
	TestEqual(TEXT("first MergeAppend does not grow the graph"),
		Fixture.Graph->Nodes.Num(), NodeCountBefore);
	TestTrue(TEXT("first MergeAppend preserves Update callback connection"),
		TimelineAfter && CallbackCallAfter
		&& AreLinked(TimelineAfter->GetUpdatePin(), CallbackCallAfter->GetExecPin()));
	return true;
}

BL_TEST(Timeline_ReplaceGraphDoesNotLeakTemplates)
bool FTimeline_ReplaceGraphDoesNotLeakTemplates::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("LeakTimeline"));
	const FString Code = TEXT(
		"(timeline \"LeakTimeline\" :length 1 :length-mode timeline-length "
		":autoplay false :loop false :replicated false :ignore-time-dilation false "
		":track (float \"Alpha\" :external false "
		":curve (rich-curve :pre-extrap constant :post-extrap constant "
		":key (key :time 0 :value 0) :key (key :time 1 :value 1))) "
		":id \"aabbccdd\")\n"
		"(event StartLeakTimeline :event-id \"eeff0011\" "
		"(timeline-control :timeline \"LeakTimeline\" :action play-from-start))");

	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineReplace"));
	TestNotNull(TEXT("replacement test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("replacement test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	FBlueprintLispConverter::FImportOptions Options;
	Options.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	Options.bAutoLayout = false;
	Options.bCompile = false;
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(Fixture.Graph, Code, Options);
		if (!Imported.bSuccess)
		{
			AddError(FString::Printf(TEXT("Timeline ReplaceGraph pass %d: %s"), Pass + 1, *Imported.Error));
		}
		TestTrue(FString::Printf(TEXT("ReplaceGraph pass %d imports"), Pass + 1), Imported.bSuccess);
		TestEqual(FString::Printf(TEXT("ReplaceGraph pass %d leaves one Timeline node"), Pass + 1),
			CountTimelineNodes(Fixture.Graph, TimelineName), 1);
		TestEqual(FString::Printf(TEXT("ReplaceGraph pass %d leaves one Timeline template"), Pass + 1),
			CountTimelineTemplates(Fixture.Blueprint, TimelineName), 1);
		TestEqual(FString::Printf(TEXT("ReplaceGraph pass %d leaves one owned Timeline template"), Pass + 1),
			Fixture.Blueprint->Timelines.Num(), 1);
		if (!Imported.bSuccess) return false;
	}
	return true;
}

BL_TEST(Timeline_MergeAppendReusesSharedStateAndCallbackBody)
bool FTimeline_MergeAppendReusesSharedStateAndCallbackBody::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("MergeTimeline"));
	const FName TrackName(TEXT("Alpha"));
	const FName EventName(TEXT("StartMergeTimeline"));
	const FName FunctionName(TEXT("SetActorTickInterval"));
	const FName PureFunctionName(TEXT("Abs"));
	const FString CodeWithPosition = TEXT(
		"(timeline \"MergeTimeline\" :length 1 :length-mode timeline-length "
		":autoplay false :loop false :replicated false :ignore-time-dilation false "
		":track (float \"Alpha\" :external false "
		":curve (rich-curve :pre-extrap constant :post-extrap constant "
		":key (key :time 0 :value 0) :key (key :time 1 :value 1))) "
		":update (SetActorTickInterval :owner \"/Script/Engine.Actor\" "
		":tickinterval (Abs :owner \"/Script/Engine.KismetMathLibrary\" "
		":a (timeline-output :timeline \"MergeTimeline\" :out-pin \"Alpha\" :id \"11223344\") "
		":id \"33445566\") "
		":id \"55667788\") :pos \"640,-320\" :id \"11223344\")\n"
		"(event StartMergeTimeline :event-id \"99aabbcc\" "
		"(timeline-control :timeline \"MergeTimeline\" :action play-from-start))");
	const FString CodeWithoutPosition =
		CodeWithPosition.Replace(TEXT(":pos \"640,-320\" "), TEXT(""));
	TestFalse(TEXT("second MergeAppend DSL omits Timeline position"),
		CodeWithoutPosition.Contains(TEXT(":pos \"")));

	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineMerge"));
	TestNotNull(TEXT("MergeAppend test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("MergeAppend test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	FBlueprintLispConverter::FImportOptions Options;
	Options.ImportMode = FBlueprintLispConverter::EImportMode::MergeAppend;
	Options.bAutoLayout = false;
	Options.bCompile = true;

	UK2Node_Timeline* FirstTimelineNode = nullptr;
	UTimelineTemplate* FirstTemplate = nullptr;
	UK2Node_CallFunction* FirstUpdateCall = nullptr;
	UK2Node_CallFunction* FirstPureCall = nullptr;
	UK2Node_CustomEvent* FirstEventNode = nullptr;
	FGuid FirstTimelineGuid;
	FGuid FirstUpdateCallGuid;
	FGuid FirstPureCallGuid;
	int32 FirstTimelinePosX = INDEX_NONE;
	int32 FirstTimelinePosY = INDEX_NONE;
	int32 FirstNodeCount = INDEX_NONE;

	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const FString& Code = Pass == 0 ? CodeWithPosition : CodeWithoutPosition;
		const FBlueprintLispResult Imported = FBlueprintLispConverter::ImportGraph(Fixture.Graph, Code, Options);
		if (!Imported.bSuccess)
		{
			AddError(FString::Printf(TEXT("Timeline MergeAppend pass %d: %s"), Pass + 1, *Imported.Error));
		}
		TestTrue(FString::Printf(TEXT("MergeAppend pass %d imports"), Pass + 1), Imported.bSuccess);
		TestTrue(FString::Printf(TEXT("MergeAppend pass %d compiles"), Pass + 1),
			Fixture.Blueprint->Status != BS_Error);
		if (!Imported.bSuccess) return false;

		UK2Node_Timeline* TimelineNode = FindTimelineNode(Fixture.Graph, TimelineName);
		UTimelineTemplate* Template = Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName);
		UK2Node_CallFunction* UpdateCall = FindFunctionCall(Fixture.Graph, FunctionName);
		UK2Node_CallFunction* PureCall = FindFunctionCall(Fixture.Graph, PureFunctionName);
		UK2Node_CustomEvent* EventNode = FindCustomEvent(Fixture.Graph, EventName);
		TestNotNull(FString::Printf(TEXT("MergeAppend pass %d has Timeline node"), Pass + 1), TimelineNode);
		TestNotNull(FString::Printf(TEXT("MergeAppend pass %d has Timeline template"), Pass + 1), Template);
		TestNotNull(FString::Printf(TEXT("MergeAppend pass %d has Update call"), Pass + 1), UpdateCall);
		TestNotNull(FString::Printf(TEXT("MergeAppend pass %d has callback pure call"), Pass + 1), PureCall);
		TestNotNull(FString::Printf(TEXT("MergeAppend pass %d has control event"), Pass + 1), EventNode);
		TestEqual(FString::Printf(TEXT("MergeAppend pass %d leaves one Timeline node"), Pass + 1),
			CountTimelineNodes(Fixture.Graph, TimelineName), 1);
		TestEqual(FString::Printf(TEXT("MergeAppend pass %d leaves one Timeline template"), Pass + 1),
			CountTimelineTemplates(Fixture.Blueprint, TimelineName), 1);
		TestEqual(FString::Printf(TEXT("MergeAppend pass %d leaves one owned Timeline template"), Pass + 1),
			Fixture.Blueprint->Timelines.Num(), 1);
		if (!TimelineNode || !Template || !UpdateCall || !PureCall || !EventNode) return false;

		UEdGraphPin* EventThen = FindPin(EventNode, UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* TrackOutput = FindPin(TimelineNode, TrackName, EGPD_Output);
		UEdGraphPin* TickInterval = FindPin(UpdateCall, TEXT("TickInterval"), EGPD_Input);
		UEdGraphPin* PureInput = FindPin(PureCall, TEXT("A"), EGPD_Input);
		UEdGraphPin* PureOutput = PureCall->GetReturnValuePin();
		TestTrue(FString::Printf(TEXT("MergeAppend pass %d preserves Event to PlayFromStart"), Pass + 1),
			AreLinked(EventThen, TimelineNode->GetPlayFromStartPin()));
		TestTrue(FString::Printf(TEXT("MergeAppend pass %d preserves Update to call"), Pass + 1),
			AreLinked(TimelineNode->GetUpdatePin(), UpdateCall->GetExecPin()));
		TestTrue(FString::Printf(TEXT("MergeAppend pass %d preserves Timeline to pure-node dataflow"), Pass + 1),
			AreLinked(TrackOutput, PureInput));
		TestTrue(FString::Printf(TEXT("MergeAppend pass %d preserves pure-node to callback dataflow"), Pass + 1),
			AreLinked(PureOutput, TickInterval));

		if (Pass == 0)
		{
			FirstTimelineNode = TimelineNode;
			FirstTemplate = Template;
			FirstUpdateCall = UpdateCall;
			FirstPureCall = PureCall;
			FirstEventNode = EventNode;
			FirstTimelineGuid = TimelineNode->NodeGuid;
			FirstUpdateCallGuid = UpdateCall->NodeGuid;
			FirstPureCallGuid = PureCall->NodeGuid;
			FirstTimelinePosX = TimelineNode->NodePosX;
			FirstTimelinePosY = TimelineNode->NodePosY;
			TestEqual(TEXT("first MergeAppend applies Timeline X position"), FirstTimelinePosX, 640);
			TestEqual(TEXT("first MergeAppend applies Timeline Y position"), FirstTimelinePosY, -320);
			FirstNodeCount = Fixture.Graph->Nodes.Num();
		}
		else
		{
			TestTrue(TEXT("MergeAppend reuses the Timeline node pointer"), TimelineNode == FirstTimelineNode);
			TestTrue(TEXT("MergeAppend preserves the Timeline node GUID"), TimelineNode->NodeGuid == FirstTimelineGuid);
			TestTrue(TEXT("MergeAppend reuses the Timeline template pointer"), Template == FirstTemplate);
			TestTrue(TEXT("MergeAppend reuses the Update call pointer"), UpdateCall == FirstUpdateCall);
			TestTrue(TEXT("MergeAppend preserves the Update call GUID"), UpdateCall->NodeGuid == FirstUpdateCallGuid);
			TestTrue(TEXT("MergeAppend reuses the callback pure-node pointer"), PureCall == FirstPureCall);
			TestTrue(TEXT("MergeAppend preserves the callback pure-node GUID"), PureCall->NodeGuid == FirstPureCallGuid);
			TestTrue(TEXT("MergeAppend reuses the control event pointer"), EventNode == FirstEventNode);
			TestEqual(TEXT("MergeAppend without :pos preserves Timeline X position"),
				TimelineNode->NodePosX, FirstTimelinePosX);
			TestEqual(TEXT("MergeAppend without :pos preserves Timeline Y position"),
				TimelineNode->NodePosY, FirstTimelinePosY);
			TestEqual(TEXT("MergeAppend does not grow the graph"), Fixture.Graph->Nodes.Num(), FirstNodeCount);
		}
	}
	return true;
}

BL_TEST(Timeline_MergeAppendPreservesExternallyConsumedCallbackPureDependency)
bool FTimeline_MergeAppendPreservesExternallyConsumedCallbackPureDependency::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("SharedDependencyTimeline"));
	const FName TrackName(TEXT("SharedAlpha"));
	const FName ExternalEventName(TEXT("ExternalPureConsumer"));
	const FString InitialCode = TEXT(
		"(timeline \"SharedDependencyTimeline\" :length 2 :length-mode timeline-length "
		":autoplay false :loop false :replicated false :ignore-time-dilation false "
		":track (float \"SharedAlpha\" :external false "
		":curve (rich-curve :key (key :time 0 :value 0) :key (key :time 2 :value 1))) "
		":update (SetActorTickInterval :owner \"/Script/Engine.Actor\" "
		":tickinterval (Abs :owner \"/Script/Engine.KismetMathLibrary\" "
		":a (timeline-output :timeline \"SharedDependencyTimeline\" "
		":out-pin \"SharedAlpha\" :id \"d1000001\") :id \"d3000003\") "
		":id \"d4000004\") :id \"d1000001\")\n"
		"(event ExternalPureConsumer :event-id \"d5000005\" "
		"(SetActorTickInterval :owner \"/Script/Engine.Actor\" "
		":tickinterval (Abs :owner \"/Script/Engine.KismetMathLibrary\" "
		":a (timeline-output :timeline \"SharedDependencyTimeline\" "
		":out-pin \"SharedAlpha\" :id \"d1000001\") :id \"d3000003\") "
		":id \"d6000006\"))");
	const FString WithoutUpdateCode = TEXT(
		"(timeline \"SharedDependencyTimeline\" :length 2 :length-mode timeline-length "
		":autoplay false :loop false :replicated false :ignore-time-dilation false "
		":track (float \"SharedAlpha\" :external false "
		":curve (rich-curve :key (key :time 0 :value 0) :key (key :time 2 :value 1))) "
		":id \"d1000001\")");

	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineExternalPureConsumer"));
	TestNotNull(TEXT("external-consumer test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("external-consumer test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	FBlueprintLispConverter::FImportOptions Options;
	Options.ImportMode = FBlueprintLispConverter::EImportMode::MergeAppend;
	Options.bAutoLayout = false;
	Options.bCompile = true;
	const FBlueprintLispResult InitialImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, InitialCode, Options);
	if (!InitialImport.bSuccess) AddError(TEXT("shared callback dependency initial import: ") + InitialImport.Error);
	TestTrue(TEXT("shared callback dependency fixture imports"), InitialImport.bSuccess);
	TestTrue(TEXT("shared callback dependency fixture compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!InitialImport.bSuccess) return false;

	UK2Node_Timeline* TimelineNode = FindTimelineNode(Fixture.Graph, TimelineName);
	UTimelineTemplate* TimelineTemplate = Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName);
	UK2Node_CustomEvent* ExternalEvent = FindCustomEvent(Fixture.Graph, ExternalEventName);
	TestNotNull(TEXT("shared callback dependency Timeline exists"), TimelineNode);
	TestNotNull(TEXT("shared callback dependency template exists"), TimelineTemplate);
	TestNotNull(TEXT("external consumer event exists"), ExternalEvent);
	if (!TimelineNode || !TimelineTemplate || !ExternalEvent) return false;

	UEdGraphPin* UpdatePin = TimelineNode->GetUpdatePin();
	UEdGraphPin* ExternalEventThen = FindPin(ExternalEvent, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UK2Node_CallFunction* UpdateCall = UpdatePin && UpdatePin->LinkedTo.Num() == 1
		? Cast<UK2Node_CallFunction>(UpdatePin->LinkedTo[0]->GetOwningNode()) : nullptr;
	UK2Node_CallFunction* ExternalCall = ExternalEventThen && ExternalEventThen->LinkedTo.Num() == 1
		? Cast<UK2Node_CallFunction>(ExternalEventThen->LinkedTo[0]->GetOwningNode()) : nullptr;
	TestNotNull(TEXT("Timeline Update callback call exists"), UpdateCall);
	TestNotNull(TEXT("ordinary event consumer call exists"), ExternalCall);
	if (!UpdateCall || !ExternalCall) return false;

	UEdGraphPin* UpdateTickInterval = FindPin(UpdateCall, TEXT("TickInterval"), EGPD_Input);
	UEdGraphPin* ExternalTickInterval = FindPin(ExternalCall, TEXT("TickInterval"), EGPD_Input);
	UK2Node_CallFunction* SharedPureNode = UpdateTickInterval && UpdateTickInterval->LinkedTo.Num() == 1
		? Cast<UK2Node_CallFunction>(UpdateTickInterval->LinkedTo[0]->GetOwningNode()) : nullptr;
	TestNotNull(TEXT("callback stable pure node exists"), SharedPureNode);
	if (!SharedPureNode) return false;
	UEdGraphPin* SharedPureInput = FindPin(SharedPureNode, TEXT("A"), EGPD_Input);
	UEdGraphPin* SharedPureOutput = SharedPureNode->GetReturnValuePin();
	UEdGraphPin* TimelineTrackOutput = FindPin(TimelineNode, TrackName, EGPD_Output);
	TestTrue(TEXT("stable pure node is shared by callback and external consumer"),
		AreLinked(SharedPureOutput, UpdateTickInterval)
		&& AreLinked(SharedPureOutput, ExternalTickInterval));
	TestTrue(TEXT("Timeline float output feeds the shared pure-node input"),
		AreLinked(TimelineTrackOutput, SharedPureInput));
	TestEqual(TEXT("shared pure output initially has two consumers"),
		SharedPureOutput ? SharedPureOutput->LinkedTo.Num() : 0, 2);

	const int32 InitialNodeCount = Fixture.Graph->Nodes.Num();
	const FGuid TimelineGuid = TimelineNode->NodeGuid;
	const FGuid ExternalEventGuid = ExternalEvent->NodeGuid;
	const FGuid ExternalCallGuid = ExternalCall->NodeGuid;
	const FGuid SharedPureGuid = SharedPureNode->NodeGuid;

	const FBlueprintLispResult Updated =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, WithoutUpdateCode, Options);
	if (!Updated.bSuccess) AddError(TEXT("shared callback dependency removal merge: ") + Updated.Error);
	TestTrue(TEXT("MergeAppend removes the Timeline Update callback"), Updated.bSuccess);
	TestTrue(TEXT("external consumer graph still compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!Updated.bSuccess) return false;

	UK2Node_Timeline* TimelineAfter = FindTimelineNode(Fixture.Graph, TimelineName);
	UK2Node_CustomEvent* ExternalEventAfter = FindCustomEvent(Fixture.Graph, ExternalEventName);
	TestTrue(TEXT("callback removal reuses Timeline pointer"), TimelineAfter == TimelineNode);
	TestTrue(TEXT("callback removal reuses Timeline template pointer"),
		Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName) == TimelineTemplate);
	TestTrue(TEXT("callback removal preserves the unmentioned external event pointer"),
		ExternalEventAfter == ExternalEvent);
	TestTrue(TEXT("callback removal preserves the external call node"),
		Fixture.Graph->Nodes.Contains(ExternalCall));
	TestTrue(TEXT("callback removal preserves the shared pure node"),
		Fixture.Graph->Nodes.Contains(SharedPureNode));
	TestFalse(TEXT("callback removal deletes only the obsolete Update call"),
		Fixture.Graph->Nodes.Contains(UpdateCall));
	TestEqual(TEXT("callback removal removes exactly one node"),
		Fixture.Graph->Nodes.Num(), InitialNodeCount - 1);
	if (!TimelineAfter || !ExternalEventAfter) return false;

	UEdGraphPin* ExternalEventThenAfter = FindPin(ExternalEventAfter, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* ExternalTickIntervalAfter = FindPin(ExternalCall, TEXT("TickInterval"), EGPD_Input);
	UEdGraphPin* SharedPureInputAfter = FindPin(SharedPureNode, TEXT("A"), EGPD_Input);
	UEdGraphPin* SharedPureOutputAfter = SharedPureNode->GetReturnValuePin();
	UEdGraphPin* TimelineTrackOutputAfter = FindPin(TimelineAfter, TrackName, EGPD_Output);
	TestEqual(TEXT("Timeline Update is disconnected after callback removal"),
		TimelineAfter->GetUpdatePin()->LinkedTo.Num(), 0);
	TestTrue(TEXT("unmentioned event remains connected to its call"),
		AreLinked(ExternalEventThenAfter, ExternalCall->GetExecPin()));
	TestTrue(TEXT("shared pure output remains connected to the external consumer"),
		AreLinked(SharedPureOutputAfter, ExternalTickIntervalAfter));
	TestTrue(TEXT("shared pure input chain remains connected to Timeline output"),
		AreLinked(TimelineTrackOutputAfter, SharedPureInputAfter));
	TestEqual(TEXT("shared pure output drops only the deleted callback consumer"),
		SharedPureOutputAfter ? SharedPureOutputAfter->LinkedTo.Num() : 0, 1);
	TestTrue(TEXT("callback removal preserves Timeline GUID"), TimelineAfter->NodeGuid == TimelineGuid);
	TestTrue(TEXT("callback removal preserves external event GUID"), ExternalEventAfter->NodeGuid == ExternalEventGuid);
	TestTrue(TEXT("callback removal preserves external call GUID"), ExternalCall->NodeGuid == ExternalCallGuid);
	TestTrue(TEXT("callback removal preserves shared pure-node GUID"), SharedPureNode->NodeGuid == SharedPureGuid);

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bIncludePositions = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult Exported =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	if (!Exported.bSuccess) AddError(TEXT("external consumer graph export: ") + Exported.Error);
	TestTrue(TEXT("external consumer graph re-exports after callback removal"), Exported.bSuccess);
	TestFalse(TEXT("re-export contains no removed Update callback"),
		Exported.LispCode.Contains(TEXT(":update")));
	TestTrue(TEXT("re-export retains the external consumer event"),
		Exported.LispCode.Contains(TEXT("(event ExternalPureConsumer")));
	TestTrue(TEXT("re-export retains the shared stable pure expression"),
		Exported.LispCode.Contains(TEXT("(Abs")));
	return true;
}

BL_TEST(Timeline_MergeAppendClearsRemovedCallbackTailControl)
bool FTimeline_MergeAppendClearsRemovedCallbackTailControl::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("CallbackTailTimeline"));
	const FName FunctionName(TEXT("SetActorTickInterval"));
	const FString TimelinePrefix = TEXT(
		"(timeline \"CallbackTailTimeline\" :length 1 :length-mode timeline-length "
		":autoplay false :loop false :replicated false :ignore-time-dilation false "
		":track (float \"Alpha\" :external false "
		":curve (rich-curve :key (key :time 0 :value 0) :key (key :time 1 :value 1))) ");
	const FString InitialCode = TimelinePrefix + TEXT(
		":update (seq "
		"(SetActorTickInterval :owner \"/Script/Engine.Actor\" :tickinterval 0.25 :id \"f2000002\") "
		"(timeline-control :timeline \"CallbackTailTimeline\" :action play)) "
		":id \"f1000001\")");
	const FString WithoutTailControlCode = TimelinePrefix + TEXT(
		":update (SetActorTickInterval :owner \"/Script/Engine.Actor\" "
		":tickinterval 0.25 :id \"f2000002\") :id \"f1000001\")");

	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineCallbackTail"));
	TestNotNull(TEXT("callback-tail test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("callback-tail test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	FBlueprintLispConverter::FImportOptions Options;
	Options.ImportMode = FBlueprintLispConverter::EImportMode::MergeAppend;
	Options.bAutoLayout = false;
	Options.bCompile = true;
	const FBlueprintLispResult InitialImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, InitialCode, Options);
	if (!InitialImport.bSuccess) AddError(TEXT("callback-tail initial import: ") + InitialImport.Error);
	TestTrue(TEXT("callback-tail fixture imports"), InitialImport.bSuccess);
	TestTrue(TEXT("callback-tail fixture compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!InitialImport.bSuccess) return false;

	UK2Node_Timeline* TimelineNode = FindTimelineNode(Fixture.Graph, TimelineName);
	UK2Node_CallFunction* CallbackCall = FindFunctionCall(Fixture.Graph, FunctionName);
	TestNotNull(TEXT("callback-tail Timeline exists"), TimelineNode);
	TestNotNull(TEXT("callback-tail call exists"), CallbackCall);
	if (!TimelineNode || !CallbackCall) return false;
	UEdGraphPin* CallbackThen = FindPin(CallbackCall, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	TestTrue(TEXT("callback call initially continues into Timeline Play"),
		AreLinked(CallbackThen, TimelineNode->GetPlayPin()));
	TestTrue(TEXT("Timeline Update initially drives callback call"),
		AreLinked(TimelineNode->GetUpdatePin(), CallbackCall->GetExecPin()));
	const int32 InitialNodeCount = Fixture.Graph->Nodes.Num();
	const FGuid CallbackGuid = CallbackCall->NodeGuid;

	const FBlueprintLispResult Updated =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, WithoutTailControlCode, Options);
	if (!Updated.bSuccess) AddError(TEXT("callback-tail removal merge: ") + Updated.Error);
	TestTrue(TEXT("MergeAppend removes a callback tail Timeline control"), Updated.bSuccess);
	TestTrue(TEXT("callback-tail removal graph compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!Updated.bSuccess) return false;

	UK2Node_Timeline* TimelineAfter = FindTimelineNode(Fixture.Graph, TimelineName);
	UK2Node_CallFunction* CallbackCallAfter = FindFunctionCall(Fixture.Graph, FunctionName);
	TestTrue(TEXT("tail-control removal reuses Timeline pointer"), TimelineAfter == TimelineNode);
	TestTrue(TEXT("tail-control removal reuses callback Call pointer"), CallbackCallAfter == CallbackCall);
	TestTrue(TEXT("tail-control removal preserves callback Call GUID"),
		CallbackCallAfter && CallbackCallAfter->NodeGuid == CallbackGuid);
	TestEqual(TEXT("tail-control removal does not change node count"),
		Fixture.Graph->Nodes.Num(), InitialNodeCount);
	if (!TimelineAfter || !CallbackCallAfter) return false;
	UEdGraphPin* CallbackThenAfter = FindPin(CallbackCallAfter, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	TestTrue(TEXT("Timeline Update remains connected to the reused callback Call"),
		AreLinked(TimelineAfter->GetUpdatePin(), CallbackCallAfter->GetExecPin()));
	TestFalse(TEXT("removed callback tail no longer links Call.Then to Timeline Play"),
		AreLinked(CallbackThenAfter, TimelineAfter->GetPlayPin()));
	TestEqual(TEXT("reused callback Call has no stale continuation links"),
		CallbackThenAfter ? CallbackThenAfter->LinkedTo.Num() : 0, 0);
	TestEqual(TEXT("Timeline Play has no stale callback links"),
		TimelineAfter->GetPlayPin()->LinkedTo.Num(), 0);
	return true;
}

BL_TEST(Timeline_MergeAppendPreflightFailureDoesNotMutateGraph)
bool FTimeline_MergeAppendPreflightFailureDoesNotMutateGraph::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("GuardedTimeline"));
	const FName TrackName(TEXT("GuardedAlpha"));
	const FName EventName(TEXT("StartGuardedTimeline"));
	const FName FunctionName(TEXT("SetActorTickInterval"));
	const FString ValidCode = TEXT(
		"(timeline \"GuardedTimeline\" :length 2 :length-mode timeline-length "
		":autoplay false :loop true :replicated false :ignore-time-dilation true "
		":track (float \"GuardedAlpha\" :external false "
		":curve (rich-curve :pre-extrap cycle :post-extrap oscillate "
		":key (key :time 0 :value 0.25) :key (key :time 2 :value 0.75))) "
		":update (SetActorTickInterval :owner \"/Script/Engine.Actor\" "
		":tickinterval (timeline-output :timeline \"GuardedTimeline\" :out-pin \"GuardedAlpha\" :id \"1234abcd\") "
		":id \"5678abcd\") :id \"1234abcd\")\n"
		"(event StartGuardedTimeline :event-id \"90abcdef\" "
		"(timeline-control :timeline \"GuardedTimeline\" :action play-from-start))");
	const FString InvalidCode = TEXT(
		"(timeline \"GuardedTimeline\" :length 99 :length-mode invalid-mode "
		":autoplay true :loop false :replicated true :ignore-time-dilation false "
		":track (float \"ReplacementTrack\" :external false "
		":curve (rich-curve :key (key :time 0 :value 99))) "
		":id \"1234abcd\")\n"
		"(event StartGuardedTimeline :event-id \"90abcdef\" "
		"(timeline-control :timeline \"GuardedTimeline\" :action stop))");

	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineFailedMerge"));
	TestNotNull(TEXT("failed MergeAppend test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("failed MergeAppend test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	FBlueprintLispConverter::FImportOptions Options;
	Options.ImportMode = FBlueprintLispConverter::EImportMode::MergeAppend;
	Options.bAutoLayout = false;
	Options.bCompile = true;
	const FBlueprintLispResult InitialImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, ValidCode, Options);
	if (!InitialImport.bSuccess) AddError(TEXT("initial guarded Timeline import: ") + InitialImport.Error);
	TestTrue(TEXT("initial guarded Timeline imports"), InitialImport.bSuccess);
	if (!InitialImport.bSuccess) return false;

	UK2Node_Timeline* TimelineNodeBefore = FindTimelineNode(Fixture.Graph, TimelineName);
	UTimelineTemplate* TemplateBefore = Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName);
	UK2Node_CallFunction* UpdateCallBefore = FindFunctionCall(Fixture.Graph, FunctionName);
	UK2Node_CustomEvent* EventNodeBefore = FindCustomEvent(Fixture.Graph, EventName);
	TestNotNull(TEXT("guarded Timeline node exists before failed merge"), TimelineNodeBefore);
	TestNotNull(TEXT("guarded Timeline template exists before failed merge"), TemplateBefore);
	TestNotNull(TEXT("guarded Update call exists before failed merge"), UpdateCallBefore);
	TestNotNull(TEXT("guarded control event exists before failed merge"), EventNodeBefore);
	if (!TimelineNodeBefore || !TemplateBefore || !UpdateCallBefore || !EventNodeBefore) return false;

	UEdGraphPin* EventThenBefore = FindPin(EventNodeBefore, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PlayFromStartBefore = TimelineNodeBefore->GetPlayFromStartPin();
	UEdGraphPin* UpdateBefore = TimelineNodeBefore->GetUpdatePin();
	UEdGraphPin* CallExecBefore = UpdateCallBefore->GetExecPin();
	UEdGraphPin* TrackOutputBefore = FindPin(TimelineNodeBefore, TrackName, EGPD_Output);
	UEdGraphPin* TickIntervalBefore = FindPin(UpdateCallBefore, TEXT("TickInterval"), EGPD_Input);
	TestTrue(TEXT("guarded Event is linked before failed merge"), AreLinked(EventThenBefore, PlayFromStartBefore));
	TestTrue(TEXT("guarded Update is linked before failed merge"), AreLinked(UpdateBefore, CallExecBefore));
	TestTrue(TEXT("guarded float output is linked before failed merge"), AreLinked(TrackOutputBefore, TickIntervalBefore));
	if (!EventThenBefore || !PlayFromStartBefore || !UpdateBefore || !CallExecBefore
		|| !TrackOutputBefore || !TickIntervalBefore) return false;

	const int32 NodeCountBefore = Fixture.Graph->Nodes.Num();
	const int32 OwnedTemplateCountBefore = Fixture.Blueprint->Timelines.Num();
	const FGuid TimelineGuidBefore = TimelineNodeBefore->NodeGuid;
	const FGuid UpdateCallGuidBefore = UpdateCallBefore->NodeGuid;
	const FGuid EventGuidBefore = EventNodeBefore->NodeGuid;
	const float TimelineLengthBefore = TemplateBefore->TimelineLength;
	const ETimelineLengthMode LengthModeBefore = TemplateBefore->LengthMode.GetValue();
	const bool bAutoPlayBefore = TemplateBefore->bAutoPlay != 0;
	const bool bLoopBefore = TemplateBefore->bLoop != 0;
	const bool bReplicatedBefore = TemplateBefore->bReplicated != 0;
	const bool bIgnoreTimeDilationBefore = TemplateBefore->bIgnoreTimeDilation != 0;
	const int32 EventTrackCountBefore = TemplateBefore->EventTracks.Num();
	const int32 FloatTrackCountBefore = TemplateBefore->FloatTracks.Num();
	const int32 VectorTrackCountBefore = TemplateBefore->VectorTracks.Num();
	const int32 ColorTrackCountBefore = TemplateBefore->LinearColorTracks.Num();
	const int32 DisplayTrackCountBefore = TemplateBefore->GetNumDisplayTracks();
	UCurveFloat* CurveBefore = FloatTrackCountBefore == 1 ? TemplateBefore->FloatTracks[0].CurveFloat : nullptr;
	const TArray<FRichCurveKey> CurveKeysBefore = CurveBefore
		? CurveBefore->FloatCurve.GetCopyOfKeys() : TArray<FRichCurveKey>();
	const int32 EventLinkCountBefore = EventThenBefore->LinkedTo.Num();
	const int32 UpdateLinkCountBefore = UpdateBefore->LinkedTo.Num();
	const int32 TrackLinkCountBefore = TrackOutputBefore->LinkedTo.Num();
	const EBlueprintStatus BlueprintStatusBefore = Fixture.Blueprint->Status;

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bIncludePositions = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult ExportBefore =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	TestTrue(TEXT("guarded Timeline exports before failed merge"), ExportBefore.bSuccess);
	if (!ExportBefore.bSuccess) return false;

	const FBlueprintLispResult FailedImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, InvalidCode, Options);
	TestFalse(TEXT("invalid Timeline MergeAppend fails preflight"), FailedImport.bSuccess);
	TestTrue(TEXT("failed Timeline MergeAppend reports invalid length mode"),
		FailedImport.Error.Contains(TEXT("invalid :length-mode")));

	UK2Node_Timeline* TimelineNodeAfter = FindTimelineNode(Fixture.Graph, TimelineName);
	UTimelineTemplate* TemplateAfter = Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName);
	UK2Node_CallFunction* UpdateCallAfter = FindFunctionCall(Fixture.Graph, FunctionName);
	UK2Node_CustomEvent* EventNodeAfter = FindCustomEvent(Fixture.Graph, EventName);
	TestTrue(TEXT("failed merge preserves Timeline node pointer"), TimelineNodeAfter == TimelineNodeBefore);
	TestTrue(TEXT("failed merge preserves Timeline template pointer"), TemplateAfter == TemplateBefore);
	TestTrue(TEXT("failed merge preserves Update call pointer"), UpdateCallAfter == UpdateCallBefore);
	TestTrue(TEXT("failed merge preserves control event pointer"), EventNodeAfter == EventNodeBefore);
	TestEqual(TEXT("failed merge preserves graph node count"), Fixture.Graph->Nodes.Num(), NodeCountBefore);
	TestEqual(TEXT("failed merge preserves owned Timeline count"),
		Fixture.Blueprint->Timelines.Num(), OwnedTemplateCountBefore);
	TestEqual(TEXT("failed merge leaves one matching Timeline template"),
		CountTimelineTemplates(Fixture.Blueprint, TimelineName), 1);
	if (!TimelineNodeAfter || !TemplateAfter || !UpdateCallAfter || !EventNodeAfter) return false;

	TestTrue(TEXT("failed merge preserves Timeline GUID"), TimelineNodeAfter->NodeGuid == TimelineGuidBefore);
	TestTrue(TEXT("failed merge preserves Update call GUID"), UpdateCallAfter->NodeGuid == UpdateCallGuidBefore);
	TestTrue(TEXT("failed merge preserves control event GUID"), EventNodeAfter->NodeGuid == EventGuidBefore);
	TestTrue(TEXT("failed merge preserves Timeline length"),
		FMath::IsNearlyEqual(TemplateAfter->TimelineLength, TimelineLengthBefore));
	TestTrue(TEXT("failed merge preserves Timeline length mode"),
		TemplateAfter->LengthMode.GetValue() == LengthModeBefore);
	TestEqual(TEXT("failed merge preserves autoplay flag"), TemplateAfter->bAutoPlay != 0, bAutoPlayBefore);
	TestEqual(TEXT("failed merge preserves loop flag"), TemplateAfter->bLoop != 0, bLoopBefore);
	TestEqual(TEXT("failed merge preserves replicated flag"), TemplateAfter->bReplicated != 0, bReplicatedBefore);
	TestEqual(TEXT("failed merge preserves ignore-time-dilation flag"),
		TemplateAfter->bIgnoreTimeDilation != 0, bIgnoreTimeDilationBefore);
	TestEqual(TEXT("failed merge preserves event track count"), TemplateAfter->EventTracks.Num(), EventTrackCountBefore);
	TestEqual(TEXT("failed merge preserves float track count"), TemplateAfter->FloatTracks.Num(), FloatTrackCountBefore);
	TestEqual(TEXT("failed merge preserves vector track count"), TemplateAfter->VectorTracks.Num(), VectorTrackCountBefore);
	TestEqual(TEXT("failed merge preserves color track count"), TemplateAfter->LinearColorTracks.Num(), ColorTrackCountBefore);
	TestEqual(TEXT("failed merge preserves display track count"),
		TemplateAfter->GetNumDisplayTracks(), DisplayTrackCountBefore);
	TestEqual(TEXT("failed merge preserves float track name"),
		TemplateAfter->FloatTracks.Num() == 1 ? TemplateAfter->FloatTracks[0].GetTrackName() : NAME_None,
		TrackName);
	UCurveFloat* CurveAfter = TemplateAfter->FloatTracks.Num() == 1
		? TemplateAfter->FloatTracks[0].CurveFloat : nullptr;
	TestTrue(TEXT("failed merge preserves internal curve pointer"), CurveAfter == CurveBefore);
	if (CurveAfter)
	{
		const TArray<FRichCurveKey>& CurveKeysAfter = CurveAfter->FloatCurve.GetConstRefOfKeys();
		TestEqual(TEXT("failed merge preserves curve key count"), CurveKeysAfter.Num(), CurveKeysBefore.Num());
		if (CurveKeysAfter.Num() == CurveKeysBefore.Num())
		{
			for (int32 KeyIndex = 0; KeyIndex < CurveKeysAfter.Num(); ++KeyIndex)
			{
				TestTrue(FString::Printf(TEXT("failed merge preserves curve key %d"), KeyIndex),
					CurveKeysAfter[KeyIndex] == CurveKeysBefore[KeyIndex]);
			}
		}
	}

	UEdGraphPin* EventThenAfter = FindPin(EventNodeAfter, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* UpdateAfter = TimelineNodeAfter->GetUpdatePin();
	UEdGraphPin* TrackOutputAfter = FindPin(TimelineNodeAfter, TrackName, EGPD_Output);
	UEdGraphPin* TickIntervalAfter = FindPin(UpdateCallAfter, TEXT("TickInterval"), EGPD_Input);
	TestTrue(TEXT("failed merge preserves exact Event pin object"), EventThenAfter == EventThenBefore);
	TestTrue(TEXT("failed merge preserves exact Update pin object"), UpdateAfter == UpdateBefore);
	TestTrue(TEXT("failed merge preserves exact float output pin object"), TrackOutputAfter == TrackOutputBefore);
	TestTrue(TEXT("failed merge preserves exact TickInterval pin object"), TickIntervalAfter == TickIntervalBefore);
	TestEqual(TEXT("failed merge preserves Event link count"),
		EventThenAfter ? EventThenAfter->LinkedTo.Num() : INDEX_NONE, EventLinkCountBefore);
	TestEqual(TEXT("failed merge preserves Update link count"),
		UpdateAfter ? UpdateAfter->LinkedTo.Num() : INDEX_NONE, UpdateLinkCountBefore);
	TestEqual(TEXT("failed merge preserves float-output link count"),
		TrackOutputAfter ? TrackOutputAfter->LinkedTo.Num() : INDEX_NONE, TrackLinkCountBefore);
	TestTrue(TEXT("failed merge preserves Event to PlayFromStart link"),
		AreLinked(EventThenAfter, TimelineNodeAfter->GetPlayFromStartPin()));
	TestTrue(TEXT("failed merge preserves Update to call link"),
		AreLinked(UpdateAfter, UpdateCallAfter->GetExecPin()));
	TestTrue(TEXT("failed merge preserves float output dataflow"),
		AreLinked(TrackOutputAfter, TickIntervalAfter));
	TestTrue(TEXT("failed merge preserves Blueprint status"), Fixture.Blueprint->Status == BlueprintStatusBefore);

	const FBlueprintLispResult ExportAfter =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	TestTrue(TEXT("guarded Timeline exports after failed merge"), ExportAfter.bSuccess);
	if (ExportAfter.bSuccess)
	{
		TestEqual(TEXT("failed merge leaves exported Timeline DSL unchanged"),
			BlueprintLisp::Minify(ExportAfter.LispCode), BlueprintLisp::Minify(ExportBefore.LispCode));
	}
	return true;
}

BL_TEST(Timeline_MergeAppendCallbackFailureDoesNotMutateGraph)
bool FTimeline_MergeAppendCallbackFailureDoesNotMutateGraph::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("AtomicCallbackTimeline"));
	const FName TrackName(TEXT("AtomicAlpha"));
	const FName EventName(TEXT("StartAtomicCallbackTimeline"));
	const FName FunctionName(TEXT("SetActorTickInterval"));
	const FString ValidCode = TEXT(
		"(timeline \"AtomicCallbackTimeline\" :length 2 :length-mode timeline-length "
		":autoplay false :loop true :replicated false :ignore-time-dilation true "
		":track (float \"AtomicAlpha\" :external false "
		":curve (rich-curve :key (key :time 0 :value 0.2) :key (key :time 2 :value 0.8))) "
		":update (SetActorTickInterval :owner \"/Script/Engine.Actor\" "
		":tickinterval (timeline-output :timeline \"AtomicCallbackTimeline\" "
		":out-pin \"AtomicAlpha\" :id \"ab100001\") :id \"ab200002\") "
		":id \"ab100001\")\n"
		"(event StartAtomicCallbackTimeline :event-id \"ab300003\" "
		"(timeline-control :timeline \"AtomicCallbackTimeline\" :action play-from-start))");
	const FString InvalidCallbackCode = TEXT(
		"(timeline \"AtomicCallbackTimeline\" :length 9 :length-mode timeline-length "
		":autoplay true :loop false :replicated true :ignore-time-dilation false "
		":track (float \"AtomicAlpha\" :external false "
		":curve (rich-curve :key (key :time 0 :value 9) :key (key :time 9 :value 0))) "
		":update (BlueprintLispNoSuchExec :id \"ab400004\") :id \"ab100001\")");
	const FString InvalidExplicitCallCode = TEXT(
		"(timeline \"AtomicCallbackTimeline\" :length 9 :length-mode timeline-length "
		":autoplay true :loop false :replicated true :ignore-time-dilation false "
		":track (float \"AtomicAlpha\" :external false "
		":curve (rich-curve :key (key :time 0 :value 9) :key (key :time 9 :value 0))) "
		":update (call self NoSuchFunction :id \"ab500005\") :id \"ab100001\")");
	const FString InvalidPureShorthandCode = TEXT(
		"(timeline \"AtomicCallbackTimeline\" :length 9 :length-mode timeline-length "
		":autoplay true :loop false :replicated true :ignore-time-dilation false "
		":track (float \"AtomicAlpha\" :external false "
		":curve (rich-curve :key (key :time 0 :value 9) :key (key :time 9 :value 0))) "
		":update (Abs :owner \"/Script/Engine.KismetMathLibrary\" :a -1 :id \"ab600006\") "
		":id \"ab100001\")");

	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineCallbackFailureAtomicity"));
	TestNotNull(TEXT("callback-failure test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("callback-failure test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	FBlueprintLispConverter::FImportOptions Options;
	Options.ImportMode = FBlueprintLispConverter::EImportMode::MergeAppend;
	Options.bAutoLayout = false;
	Options.bCompile = true;
	const FBlueprintLispResult InitialImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, ValidCode, Options);
	if (!InitialImport.bSuccess) AddError(TEXT("callback-failure initial import: ") + InitialImport.Error);
	TestTrue(TEXT("callback-failure fixture imports"), InitialImport.bSuccess);
	if (!InitialImport.bSuccess) return false;

	UK2Node_Timeline* TimelineBefore = FindTimelineNode(Fixture.Graph, TimelineName);
	UTimelineTemplate* TemplateBefore = Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName);
	UK2Node_CustomEvent* EventBefore = FindCustomEvent(Fixture.Graph, EventName);
	UK2Node_CallFunction* UpdateCallBefore = FindFunctionCall(Fixture.Graph, FunctionName);
	TestNotNull(TEXT("callback-failure fixture has Timeline"), TimelineBefore);
	TestNotNull(TEXT("callback-failure fixture has template"), TemplateBefore);
	TestNotNull(TEXT("callback-failure fixture has event"), EventBefore);
	TestNotNull(TEXT("callback-failure fixture has Update call"), UpdateCallBefore);
	if (!TimelineBefore || !TemplateBefore || !EventBefore || !UpdateCallBefore) return false;

	UEdGraphPin* EventThenBefore = FindPin(EventBefore, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* UpdateBefore = TimelineBefore->GetUpdatePin();
	UEdGraphPin* TrackOutputBefore = FindPin(TimelineBefore, TrackName, EGPD_Output);
	UEdGraphPin* TickIntervalBefore = FindPin(UpdateCallBefore, TEXT("TickInterval"), EGPD_Input);
	TestTrue(TEXT("callback-failure fixture Event controls Timeline"),
		AreLinked(EventThenBefore, TimelineBefore->GetPlayFromStartPin()));
	TestTrue(TEXT("callback-failure fixture Update drives call"),
		AreLinked(UpdateBefore, UpdateCallBefore->GetExecPin()));
	TestTrue(TEXT("callback-failure fixture float output drives call input"),
		AreLinked(TrackOutputBefore, TickIntervalBefore));

	const int32 NodeCountBefore = Fixture.Graph->Nodes.Num();
	const int32 TemplateCountBefore = Fixture.Blueprint->Timelines.Num();
	const FGuid TimelineGuidBefore = TimelineBefore->NodeGuid;
	const FGuid EventGuidBefore = EventBefore->NodeGuid;
	const FGuid UpdateCallGuidBefore = UpdateCallBefore->NodeGuid;
	const float TimelineLengthBefore = TemplateBefore->TimelineLength;
	const bool bAutoPlayBefore = TemplateBefore->bAutoPlay != 0;
	const bool bLoopBefore = TemplateBefore->bLoop != 0;
	const bool bReplicatedBefore = TemplateBefore->bReplicated != 0;
	const bool bIgnoreTimeDilationBefore = TemplateBefore->bIgnoreTimeDilation != 0;
	UCurveFloat* CurveBefore = TemplateBefore->FloatTracks.Num() == 1
		? TemplateBefore->FloatTracks[0].CurveFloat : nullptr;
	const TArray<FRichCurveKey> CurveKeysBefore = CurveBefore
		? CurveBefore->FloatCurve.GetConstRefOfKeys() : TArray<FRichCurveKey>();
	const EBlueprintStatus BlueprintStatusBefore = Fixture.Blueprint->Status;

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bIncludePositions = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult ExportBefore =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	TestTrue(TEXT("callback-failure fixture exports before invalid merge"), ExportBefore.bSuccess);
	if (!ExportBefore.bSuccess) return false;

	const FBlueprintLispResult FailedImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, InvalidCallbackCode, Options);
	TestFalse(TEXT("unresolvable Timeline callback fails MergeAppend"), FailedImport.bSuccess);
	TestTrue(TEXT("callback failure identifies the unresolved form"),
		FailedImport.Error.Contains(TEXT("BlueprintLispNoSuchExec"), ESearchCase::IgnoreCase));

	UK2Node_Timeline* TimelineAfter = FindTimelineNode(Fixture.Graph, TimelineName);
	UTimelineTemplate* TemplateAfter = Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName);
	UK2Node_CustomEvent* EventAfter = FindCustomEvent(Fixture.Graph, EventName);
	UK2Node_CallFunction* UpdateCallAfter = FindFunctionCall(Fixture.Graph, FunctionName);
	TestTrue(TEXT("callback failure preserves Timeline pointer"), TimelineAfter == TimelineBefore);
	TestTrue(TEXT("callback failure preserves template pointer"), TemplateAfter == TemplateBefore);
	TestTrue(TEXT("callback failure preserves event pointer"), EventAfter == EventBefore);
	TestTrue(TEXT("callback failure preserves Update call pointer"), UpdateCallAfter == UpdateCallBefore);
	TestEqual(TEXT("callback failure preserves node count"), Fixture.Graph->Nodes.Num(), NodeCountBefore);
	TestEqual(TEXT("callback failure preserves template count"),
		Fixture.Blueprint->Timelines.Num(), TemplateCountBefore);
	if (!TimelineAfter || !TemplateAfter || !EventAfter || !UpdateCallAfter) return false;
	TestTrue(TEXT("callback failure preserves Timeline GUID"), TimelineAfter->NodeGuid == TimelineGuidBefore);
	TestTrue(TEXT("callback failure preserves event GUID"), EventAfter->NodeGuid == EventGuidBefore);
	TestTrue(TEXT("callback failure preserves Update call GUID"), UpdateCallAfter->NodeGuid == UpdateCallGuidBefore);
	TestTrue(TEXT("callback failure preserves Timeline length"),
		FMath::IsNearlyEqual(TemplateAfter->TimelineLength, TimelineLengthBefore));
	TestEqual(TEXT("callback failure preserves autoplay"), TemplateAfter->bAutoPlay != 0, bAutoPlayBefore);
	TestEqual(TEXT("callback failure preserves loop"), TemplateAfter->bLoop != 0, bLoopBefore);
	TestEqual(TEXT("callback failure preserves replication"), TemplateAfter->bReplicated != 0, bReplicatedBefore);
	TestEqual(TEXT("callback failure preserves ignore-time-dilation"),
		TemplateAfter->bIgnoreTimeDilation != 0, bIgnoreTimeDilationBefore);
	UCurveFloat* CurveAfter = TemplateAfter->FloatTracks.Num() == 1
		? TemplateAfter->FloatTracks[0].CurveFloat : nullptr;
	TestTrue(TEXT("callback failure preserves internal curve pointer"), CurveAfter == CurveBefore);
	if (CurveAfter)
	{
		const TArray<FRichCurveKey>& CurveKeysAfter = CurveAfter->FloatCurve.GetConstRefOfKeys();
		TestEqual(TEXT("callback failure preserves curve key count"), CurveKeysAfter.Num(), CurveKeysBefore.Num());
		if (CurveKeysAfter.Num() == CurveKeysBefore.Num())
		{
			for (int32 Index = 0; Index < CurveKeysAfter.Num(); ++Index)
			{
				TestTrue(FString::Printf(TEXT("callback failure preserves curve key %d"), Index),
					CurveKeysAfter[Index] == CurveKeysBefore[Index]);
			}
		}
	}

	UEdGraphPin* EventThenAfter = FindPin(EventAfter, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* UpdateAfter = TimelineAfter->GetUpdatePin();
	UEdGraphPin* TrackOutputAfter = FindPin(TimelineAfter, TrackName, EGPD_Output);
	UEdGraphPin* TickIntervalAfter = FindPin(UpdateCallAfter, TEXT("TickInterval"), EGPD_Input);
	TestTrue(TEXT("callback failure preserves exact Event pin"), EventThenAfter == EventThenBefore);
	TestTrue(TEXT("callback failure preserves exact Update pin"), UpdateAfter == UpdateBefore);
	TestTrue(TEXT("callback failure preserves exact track output pin"), TrackOutputAfter == TrackOutputBefore);
	TestTrue(TEXT("callback failure preserves exact call input pin"), TickIntervalAfter == TickIntervalBefore);
	TestTrue(TEXT("callback failure preserves Event control link"),
		AreLinked(EventThenAfter, TimelineAfter->GetPlayFromStartPin()));
	TestTrue(TEXT("callback failure preserves Update callback link"),
		AreLinked(UpdateAfter, UpdateCallAfter->GetExecPin()));
	TestTrue(TEXT("callback failure preserves float data link"),
		AreLinked(TrackOutputAfter, TickIntervalAfter));
	TestTrue(TEXT("callback failure preserves Blueprint status"),
		Fixture.Blueprint->Status == BlueprintStatusBefore);

	const FBlueprintLispResult ExportAfter =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	TestTrue(TEXT("callback-failure fixture exports after invalid merge"), ExportAfter.bSuccess);
	if (ExportAfter.bSuccess)
	{
		TestEqual(TEXT("callback failure leaves exported DSL unchanged"),
			BlueprintLisp::Minify(ExportAfter.LispCode), BlueprintLisp::Minify(ExportBefore.LispCode));
	}

	struct FAdditionalInvalidCallback
	{
		const TCHAR* Label;
		const FString* Code;
		const TCHAR* ExpectedError;
	};
	const TArray<FAdditionalInvalidCallback> AdditionalInvalidCallbacks = {
		{ TEXT("missing explicit call"), &InvalidExplicitCallCode, TEXT("NoSuchFunction") },
		{ TEXT("pure shorthand call"), &InvalidPureShorthandCode, TEXT("pure") },
	};
	for (const FAdditionalInvalidCallback& InvalidCase : AdditionalInvalidCallbacks)
	{
		const FString Label(InvalidCase.Label);
		const FBlueprintLispResult AdditionalFailure =
			FBlueprintLispConverter::ImportGraph(Fixture.Graph, *InvalidCase.Code, Options);
		TestFalse(Label + TEXT(" fails Timeline callback preflight"), AdditionalFailure.bSuccess);
		TestTrue(Label + TEXT(" reports its callback validation reason"),
			AdditionalFailure.Error.Contains(InvalidCase.ExpectedError, ESearchCase::IgnoreCase));
		TestEqual(Label + TEXT(" preserves node count"), Fixture.Graph->Nodes.Num(), NodeCountBefore);
		TestEqual(Label + TEXT(" preserves template count"),
			Fixture.Blueprint->Timelines.Num(), TemplateCountBefore);
		TestTrue(Label + TEXT(" preserves Timeline pointer"),
			FindTimelineNode(Fixture.Graph, TimelineName) == TimelineBefore);
		TestTrue(Label + TEXT(" preserves template pointer"),
			Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName) == TemplateBefore);
		TestTrue(Label + TEXT(" preserves event pointer"),
			FindCustomEvent(Fixture.Graph, EventName) == EventBefore);
		TestTrue(Label + TEXT(" preserves Update call pointer"),
			FindFunctionCall(Fixture.Graph, FunctionName) == UpdateCallBefore);
		TestTrue(Label + TEXT(" preserves exact Event pin"),
			FindPin(EventBefore, UEdGraphSchema_K2::PN_Then, EGPD_Output) == EventThenBefore);
		TestTrue(Label + TEXT(" preserves exact Update pin"), TimelineBefore->GetUpdatePin() == UpdateBefore);
		TestTrue(Label + TEXT(" preserves exact track output pin"),
			FindPin(TimelineBefore, TrackName, EGPD_Output) == TrackOutputBefore);
		TestTrue(Label + TEXT(" preserves exact call input pin"),
			FindPin(UpdateCallBefore, TEXT("TickInterval"), EGPD_Input) == TickIntervalBefore);
		TestTrue(Label + TEXT(" preserves Event control link"),
			AreLinked(EventThenBefore, TimelineBefore->GetPlayFromStartPin()));
		TestTrue(Label + TEXT(" preserves Update callback link"),
			AreLinked(UpdateBefore, UpdateCallBefore->GetExecPin()));
		TestTrue(Label + TEXT(" preserves float data link"),
			AreLinked(TrackOutputBefore, TickIntervalBefore));
		TestTrue(Label + TEXT(" preserves Blueprint status"),
			Fixture.Blueprint->Status == BlueprintStatusBefore);
		const FBlueprintLispResult AdditionalExport =
			FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
		TestTrue(Label + TEXT(" leaves graph exportable"), AdditionalExport.bSuccess);
		if (AdditionalExport.bSuccess)
		{
			TestEqual(Label + TEXT(" leaves exported DSL unchanged"),
				BlueprintLisp::Minify(AdditionalExport.LispCode),
				BlueprintLisp::Minify(ExportBefore.LispCode));
		}
	}
	return true;
}

BL_TEST(Timeline_SetNewTimePureDependencyLifecycleAndConflictPreflight)
bool FTimeline_SetNewTimePureDependencyLifecycleAndConflictPreflight::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("ScrubTimeline"));
	const FName EventName(TEXT("SetScrubTime"));
	const FName PureFunctionName(TEXT("Abs"));
	const FString TimelineDefinition = TEXT(
		"(timeline \"ScrubTimeline\" :length 5 :length-mode timeline-length "
		":autoplay false :loop false :replicated false :ignore-time-dilation false "
		":track (float \"ScrubAlpha\" :external false "
		":curve (rich-curve :key (key :time 0 :value 0) :key (key :time 5 :value 1))) "
		":id \"a1000001\")\n");
	const FString SetNewTimeCode = TimelineDefinition + TEXT(
		"(event SetScrubTime :event-id \"a2000002\" "
		"(timeline-control :timeline \"ScrubTimeline\" :action set-new-time "
		":time (Abs :owner \"/Script/Engine.KismetMathLibrary\" :a -2.5 :id \"a3000003\")))");
	const FString UpdatedSetNewTimeCode = TimelineDefinition + TEXT(
		"(event SetScrubTime :event-id \"a2000002\" "
		"(timeline-control :timeline \"ScrubTimeline\" :action set-new-time "
		":time (Abs :owner \"/Script/Engine.KismetMathLibrary\" :a -3.5 :id \"a3000003\")))");
	const FString PlayCode = TimelineDefinition + TEXT(
		"(event SetScrubTime :event-id \"a2000002\" "
		"(timeline-control :timeline \"ScrubTimeline\" :action play))");
	const FString ConflictingCode = TimelineDefinition + TEXT(
		"(event SetScrubTimeA :event-id \"a4000004\" "
		"(timeline-control :timeline \"ScrubTimeline\" :action set-new-time "
		":time (Abs :owner \"/Script/Engine.KismetMathLibrary\" :a -1 :id \"a5000005\")))\n"
		"(event SetScrubTimeB :event-id \"a6000006\" "
		"(timeline-control :timeline \"ScrubTimeline\" :action set-new-time "
		":time (Abs :owner \"/Script/Engine.KismetMathLibrary\" :a -2 :id \"a7000007\")))");

	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineSetNewTime"));
	TestNotNull(TEXT("set-new-time test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("set-new-time test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	FBlueprintLispConverter::FImportOptions Options;
	Options.ImportMode = FBlueprintLispConverter::EImportMode::MergeAppend;
	Options.bAutoLayout = false;
	Options.bCompile = true;
	const FBlueprintLispResult InitialImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, SetNewTimeCode, Options);
	if (!InitialImport.bSuccess) AddError(TEXT("initial set-new-time import: ") + InitialImport.Error);
	TestTrue(TEXT("set-new-time with stable pure expression imports"), InitialImport.bSuccess);
	TestTrue(TEXT("initial set-new-time graph compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!InitialImport.bSuccess) return false;

	UK2Node_Timeline* TimelineNode = FindTimelineNode(Fixture.Graph, TimelineName);
	UK2Node_CustomEvent* EventNode = FindCustomEvent(Fixture.Graph, EventName);
	UK2Node_CallFunction* NewTimePureNode = FindFunctionCall(Fixture.Graph, PureFunctionName);
	TestNotNull(TEXT("set-new-time restores Timeline node"), TimelineNode);
	TestNotNull(TEXT("set-new-time restores control event"), EventNode);
	TestNotNull(TEXT("set-new-time restores stable pure expression node"), NewTimePureNode);
	if (!TimelineNode || !EventNode || !NewTimePureNode) return false;

	UEdGraphPin* EventThen = FindPin(EventNode, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PureOutput = NewTimePureNode->GetReturnValuePin();
	TestTrue(TEXT("event drives SetNewTime before action change"),
		AreLinked(EventThen, TimelineNode->GetSetNewTimePin()));
	TestTrue(TEXT("stable pure expression drives NewTime"),
		AreLinked(PureOutput, TimelineNode->GetNewTimePin()));
	const int32 NodeCountWithPureExpression = Fixture.Graph->Nodes.Num();
	const FGuid PureNodeGuid = NewTimePureNode->NodeGuid;
	TestTrue(TEXT("set-new-time pure expression has a stable GUID"), PureNodeGuid.IsValid());

	const FBlueprintLispResult RepeatedSetNewTimeImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, SetNewTimeCode, Options);
	if (!RepeatedSetNewTimeImport.bSuccess)
	{
		AddError(TEXT("repeated set-new-time MergeAppend: ") + RepeatedSetNewTimeImport.Error);
	}
	TestTrue(TEXT("identical set-new-time DSL merges repeatedly"), RepeatedSetNewTimeImport.bSuccess);
	TestTrue(TEXT("repeated set-new-time graph compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!RepeatedSetNewTimeImport.bSuccess) return false;
	UK2Node_Timeline* TimelineAfterRepeat = FindTimelineNode(Fixture.Graph, TimelineName);
	UK2Node_CustomEvent* EventAfterRepeat = FindCustomEvent(Fixture.Graph, EventName);
	UK2Node_CallFunction* PureAfterRepeat = FindFunctionCall(Fixture.Graph, PureFunctionName);
	TestTrue(TEXT("identical set-new-time merge reuses Timeline pointer"), TimelineAfterRepeat == TimelineNode);
	TestTrue(TEXT("identical set-new-time merge reuses event pointer"), EventAfterRepeat == EventNode);
	TestTrue(TEXT("identical set-new-time merge reuses pure-node pointer"), PureAfterRepeat == NewTimePureNode);
	TestTrue(TEXT("identical set-new-time merge preserves pure-node GUID"),
		PureAfterRepeat && PureAfterRepeat->NodeGuid == PureNodeGuid);
	TestEqual(TEXT("identical set-new-time merge does not grow the graph"),
		Fixture.Graph->Nodes.Num(), NodeCountWithPureExpression);
	if (!TimelineAfterRepeat || !EventAfterRepeat || !PureAfterRepeat) return false;
	TestTrue(TEXT("identical merge preserves Event to SetNewTime link"),
		AreLinked(FindPin(EventAfterRepeat, UEdGraphSchema_K2::PN_Then, EGPD_Output),
			TimelineAfterRepeat->GetSetNewTimePin()));
	TestTrue(TEXT("identical merge preserves pure expression to NewTime link"),
		AreLinked(PureAfterRepeat->GetReturnValuePin(), TimelineAfterRepeat->GetNewTimePin()));

	const FBlueprintLispResult UpdatedSetNewTimeImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, UpdatedSetNewTimeCode, Options);
	if (!UpdatedSetNewTimeImport.bSuccess)
	{
		AddError(TEXT("updated set-new-time MergeAppend: ") + UpdatedSetNewTimeImport.Error);
	}
	TestTrue(TEXT("set-new-time pure expression accepts a compatible input update"),
		UpdatedSetNewTimeImport.bSuccess);
	TestTrue(TEXT("updated set-new-time graph compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!UpdatedSetNewTimeImport.bSuccess) return false;
	UK2Node_Timeline* TimelineAfterTimeUpdate = FindTimelineNode(Fixture.Graph, TimelineName);
	UK2Node_CustomEvent* EventAfterTimeUpdate = FindCustomEvent(Fixture.Graph, EventName);
	UK2Node_CallFunction* PureAfterTimeUpdate = FindFunctionCall(Fixture.Graph, PureFunctionName);
	TestTrue(TEXT("updated set-new-time merge reuses Timeline pointer"), TimelineAfterTimeUpdate == TimelineNode);
	TestTrue(TEXT("updated set-new-time merge reuses event pointer"), EventAfterTimeUpdate == EventNode);
	TestTrue(TEXT("updated set-new-time merge reuses pure-node pointer"), PureAfterTimeUpdate == NewTimePureNode);
	TestTrue(TEXT("updated set-new-time merge preserves pure-node GUID"),
		PureAfterTimeUpdate && PureAfterTimeUpdate->NodeGuid == PureNodeGuid);
	TestEqual(TEXT("updated set-new-time merge does not grow the graph"),
		Fixture.Graph->Nodes.Num(), NodeCountWithPureExpression);
	if (!TimelineAfterTimeUpdate || !EventAfterTimeUpdate || !PureAfterTimeUpdate) return false;
	UEdGraphPin* UpdatedPureInput = FindPin(PureAfterTimeUpdate, TEXT("A"), EGPD_Input);
	TestTrue(TEXT("updated set-new-time pure input is applied in place"),
		UpdatedPureInput && FMath::IsNearlyEqual(FCString::Atod(*UpdatedPureInput->DefaultValue), -3.5));
	TestTrue(TEXT("updated merge preserves Event to SetNewTime link"),
		AreLinked(FindPin(EventAfterTimeUpdate, UEdGraphSchema_K2::PN_Then, EGPD_Output),
			TimelineAfterTimeUpdate->GetSetNewTimePin()));
	TestTrue(TEXT("updated merge preserves pure expression to NewTime link"),
		AreLinked(PureAfterTimeUpdate->GetReturnValuePin(), TimelineAfterTimeUpdate->GetNewTimePin()));

	const FBlueprintLispResult PlayImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, PlayCode, Options);
	if (!PlayImport.bSuccess) AddError(TEXT("set-new-time to play MergeAppend: ") + PlayImport.Error);
	TestTrue(TEXT("set-new-time action can be replaced with play"), PlayImport.bSuccess);
	TestTrue(TEXT("play replacement graph compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!PlayImport.bSuccess) return false;

	UK2Node_Timeline* TimelineAfterPlay = FindTimelineNode(Fixture.Graph, TimelineName);
	UK2Node_CustomEvent* EventAfterPlay = FindCustomEvent(Fixture.Graph, EventName);
	TestTrue(TEXT("play replacement reuses Timeline node"), TimelineAfterPlay == TimelineNode);
	TestTrue(TEXT("play replacement reuses control event"), EventAfterPlay == EventNode);
	TestNull(TEXT("play replacement removes obsolete NewTime pure node"),
		FindFunctionCall(Fixture.Graph, PureFunctionName));
	TestFalse(TEXT("obsolete NewTime pure node is no longer owned by the graph"),
		Fixture.Graph->Nodes.Contains(NewTimePureNode));
	TestEqual(TEXT("removing set-new-time removes exactly its pure dependency"),
		Fixture.Graph->Nodes.Num(), NodeCountWithPureExpression - 1);
	TestTrue(TEXT("changing to play does not grow the graph"),
		Fixture.Graph->Nodes.Num() <= NodeCountWithPureExpression);
	if (!TimelineAfterPlay || !EventAfterPlay) return false;
	UEdGraphPin* EventThenAfterPlay = FindPin(EventAfterPlay, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	TestTrue(TEXT("event drives Play after action change"),
		AreLinked(EventThenAfterPlay, TimelineAfterPlay->GetPlayPin()));
	TestEqual(TEXT("NewTime is disconnected after action change"),
		TimelineAfterPlay->GetNewTimePin()->LinkedTo.Num(), 0);

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bIncludePositions = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult ExportBeforeConflict =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	TestTrue(TEXT("play graph exports before conflicting preflight"), ExportBeforeConflict.bSuccess);
	if (!ExportBeforeConflict.bSuccess) return false;
	const int32 NodeCountBeforeConflict = Fixture.Graph->Nodes.Num();
	UTimelineTemplate* TemplateBeforeConflict =
		Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName);
	const FGuid TimelineGuidBeforeConflict = TimelineAfterPlay->NodeGuid;
	const FGuid EventGuidBeforeConflict = EventAfterPlay->NodeGuid;
	UEdGraphPin* PlayPinBeforeConflict = TimelineAfterPlay->GetPlayPin();
	const int32 PlayLinkCountBeforeConflict = PlayPinBeforeConflict->LinkedTo.Num();

	const FBlueprintLispResult ConflictImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, ConflictingCode, Options);
	TestFalse(TEXT("different shared NewTime expressions fail preflight"), ConflictImport.bSuccess);
	TestTrue(TEXT("conflicting NewTime error identifies the shared expression rule"),
		ConflictImport.Error.Contains(TEXT("same shared :time expression")));
	TestEqual(TEXT("conflicting NewTime preflight preserves node count"),
		Fixture.Graph->Nodes.Num(), NodeCountBeforeConflict);
	TestTrue(TEXT("conflicting NewTime preflight preserves Timeline pointer"),
		FindTimelineNode(Fixture.Graph, TimelineName) == TimelineAfterPlay);
	TestTrue(TEXT("conflicting NewTime preflight preserves Timeline template pointer"),
		Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName) == TemplateBeforeConflict);
	TestTrue(TEXT("conflicting NewTime preflight preserves event pointer"),
		FindCustomEvent(Fixture.Graph, EventName) == EventAfterPlay);
	TestTrue(TEXT("conflicting NewTime preflight preserves Timeline GUID"),
		TimelineAfterPlay->NodeGuid == TimelineGuidBeforeConflict);
	TestTrue(TEXT("conflicting NewTime preflight preserves event GUID"),
		EventAfterPlay->NodeGuid == EventGuidBeforeConflict);
	TestEqual(TEXT("conflicting NewTime preflight preserves Play link count"),
		PlayPinBeforeConflict->LinkedTo.Num(), PlayLinkCountBeforeConflict);
	TestTrue(TEXT("conflicting NewTime preflight preserves Event to Play link"),
		AreLinked(EventThenAfterPlay, PlayPinBeforeConflict));
	TestEqual(TEXT("conflicting NewTime preflight leaves NewTime disconnected"),
		TimelineAfterPlay->GetNewTimePin()->LinkedTo.Num(), 0);
	TestNull(TEXT("conflicting NewTime preflight creates no pure nodes"),
		FindFunctionCall(Fixture.Graph, PureFunctionName));

	const FBlueprintLispResult ExportAfterConflict =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	TestTrue(TEXT("play graph exports after conflicting preflight"), ExportAfterConflict.bSuccess);
	if (ExportAfterConflict.bSuccess)
	{
		TestEqual(TEXT("conflicting NewTime preflight leaves exported DSL unchanged"),
			BlueprintLisp::Minify(ExportAfterConflict.LispCode),
			BlueprintLisp::Minify(ExportBeforeConflict.LispCode));
	}
	return true;
}

BL_TEST(Timeline_MergeAppendRemovesNewTimeDependencyWhenControlDisappears)
bool FTimeline_MergeAppendRemovesNewTimeDependencyWhenControlDisappears::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("OrphanedNewTimeTimeline"));
	const FName EventName(TEXT("ClearNewTimeControl"));
	const FName PureFunctionName(TEXT("Abs"));
	const FString TimelineDefinition = TEXT(
		"(timeline \"OrphanedNewTimeTimeline\" :length 3 :length-mode timeline-length "
		":autoplay false :loop false :replicated false :ignore-time-dilation false "
		":track (float \"Alpha\" :external false "
		":curve (rich-curve :key (key :time 0 :value 0) :key (key :time 3 :value 1))) "
		":id \"e1000001\")\n");
	const FString InitialCode = TimelineDefinition + TEXT(
		"(event ClearNewTimeControl :event-id \"e2000002\" "
		"(timeline-control :timeline \"OrphanedNewTimeTimeline\" :action set-new-time "
		":time (Abs :owner \"/Script/Engine.KismetMathLibrary\" "
		":a -1.25 :id \"e3000003\")))");
	const FString EmptyEventCode = TimelineDefinition + TEXT(
		"(event ClearNewTimeControl :event-id \"e2000002\")");

	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineNewTimeControlRemoved"));
	TestNotNull(TEXT("removed-control test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("removed-control test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	FBlueprintLispConverter::FImportOptions Options;
	Options.ImportMode = FBlueprintLispConverter::EImportMode::MergeAppend;
	Options.bAutoLayout = false;
	Options.bCompile = true;
	const FBlueprintLispResult InitialImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, InitialCode, Options);
	if (!InitialImport.bSuccess) AddError(TEXT("removed-control initial import: ") + InitialImport.Error);
	TestTrue(TEXT("removed-control fixture imports"), InitialImport.bSuccess);
	TestTrue(TEXT("removed-control fixture compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!InitialImport.bSuccess) return false;

	UK2Node_Timeline* TimelineNode = FindTimelineNode(Fixture.Graph, TimelineName);
	UTimelineTemplate* TimelineTemplate = Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName);
	UK2Node_CustomEvent* EventNode = FindCustomEvent(Fixture.Graph, EventName);
	UK2Node_CallFunction* NewTimePureNode = FindFunctionCall(Fixture.Graph, PureFunctionName);
	TestNotNull(TEXT("removed-control Timeline exists"), TimelineNode);
	TestNotNull(TEXT("removed-control Timeline template exists"), TimelineTemplate);
	TestNotNull(TEXT("removed-control event exists"), EventNode);
	TestNotNull(TEXT("removed-control NewTime pure dependency exists"), NewTimePureNode);
	if (!TimelineNode || !TimelineTemplate || !EventNode || !NewTimePureNode) return false;

	UEdGraphPin* EventThen = FindPin(EventNode, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	TestTrue(TEXT("removed-control event initially drives SetNewTime"),
		AreLinked(EventThen, TimelineNode->GetSetNewTimePin()));
	TestTrue(TEXT("removed-control pure expression initially drives NewTime"),
		AreLinked(NewTimePureNode->GetReturnValuePin(), TimelineNode->GetNewTimePin()));
	const int32 InitialNodeCount = Fixture.Graph->Nodes.Num();
	const FGuid TimelineGuid = TimelineNode->NodeGuid;
	const FGuid EventGuid = EventNode->NodeGuid;

	const FBlueprintLispResult Updated =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, EmptyEventCode, Options);
	if (!Updated.bSuccess) AddError(TEXT("removed-control empty-event merge: ") + Updated.Error);
	TestTrue(TEXT("MergeAppend accepts an event with its Timeline control removed"), Updated.bSuccess);
	TestTrue(TEXT("empty-event replacement graph compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!Updated.bSuccess) return false;

	UK2Node_Timeline* TimelineAfter = FindTimelineNode(Fixture.Graph, TimelineName);
	UK2Node_CustomEvent* EventAfter = FindCustomEvent(Fixture.Graph, EventName);
	TestTrue(TEXT("control removal reuses Timeline pointer"), TimelineAfter == TimelineNode);
	TestTrue(TEXT("control removal reuses Timeline template pointer"),
		Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName) == TimelineTemplate);
	TestTrue(TEXT("control removal reuses event pointer"), EventAfter == EventNode);
	TestFalse(TEXT("control removal deletes obsolete NewTime pure dependency"),
		Fixture.Graph->Nodes.Contains(NewTimePureNode));
	TestNull(TEXT("control removal leaves no Abs node"),
		FindFunctionCall(Fixture.Graph, PureFunctionName));
	TestEqual(TEXT("control removal deletes exactly the NewTime pure dependency"),
		Fixture.Graph->Nodes.Num(), InitialNodeCount - 1);
	if (!TimelineAfter || !EventAfter) return false;

	UEdGraphPin* EventThenAfter = FindPin(EventAfter, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	TestEqual(TEXT("empty event has no execution links"),
		EventThenAfter ? EventThenAfter->LinkedTo.Num() : 0, 0);
	TestEqual(TEXT("Timeline SetNewTime has no execution links"),
		TimelineAfter->GetSetNewTimePin()->LinkedTo.Num(), 0);
	TestEqual(TEXT("Timeline NewTime is disconnected when all controls disappear"),
		TimelineAfter->GetNewTimePin()->LinkedTo.Num(), 0);
	TestTrue(TEXT("control removal preserves Timeline GUID"), TimelineAfter->NodeGuid == TimelineGuid);
	TestTrue(TEXT("control removal preserves event GUID"), EventAfter->NodeGuid == EventGuid);

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bIncludePositions = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult Exported =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	if (!Exported.bSuccess) AddError(TEXT("removed-control graph export: ") + Exported.Error);
	TestTrue(TEXT("removed-control graph re-exports"), Exported.bSuccess);
	TestFalse(TEXT("re-export contains no Timeline control"),
		Exported.LispCode.Contains(TEXT("timeline-control")));
	TestFalse(TEXT("re-export contains no obsolete NewTime pure expression"),
		Exported.LispCode.Contains(TEXT("(Abs")));
	return true;
}

BL_TEST(Timeline_ReplaceGraphBindsEventParameterToNewTime)
bool FTimeline_ReplaceGraphBindsEventParameterToNewTime::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("EventDrivenTimeline"));
	const FName EventName(TEXT("SetEventDrivenTime"));
	const FName TimeParameterName(TEXT("RequestedTime"));
	const FString Code = TEXT(
		"(timeline \"EventDrivenTimeline\" :length 4 :length-mode timeline-length "
		":autoplay false :loop false :replicated false :ignore-time-dilation false "
		":track (float \"Alpha\" :external false "
		":curve (rich-curve :key (key :time 0 :value 0) :key (key :time 4 :value 1))) "
		":id \"c1000001\")\n"
		"(event SetEventDrivenTime :event-id \"c2000002\" :param (RequestedTime float) "
		"(timeline-control :timeline \"EventDrivenTimeline\" :action set-new-time "
		":time RequestedTime))");

	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineEventParameter"));
	TestNotNull(TEXT("event-parameter test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("event-parameter test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	FBlueprintLispConverter::FImportOptions Options;
	Options.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	Options.bAutoLayout = false;
	Options.bCompile = true;
	const FBlueprintLispResult Imported =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, Code, Options);
	if (!Imported.bSuccess) AddError(TEXT("event-parameter set-new-time import: ") + Imported.Error);
	TestTrue(TEXT("ReplaceGraph imports event-parameter set-new-time"), Imported.bSuccess);
	TestTrue(TEXT("event-parameter Timeline graph compiles"), Fixture.Blueprint->Status != BS_Error);
	if (!Imported.bSuccess) return false;

	UK2Node_Timeline* TimelineNode = FindTimelineNode(Fixture.Graph, TimelineName);
	UK2Node_CustomEvent* EventNode = FindCustomEvent(Fixture.Graph, EventName);
	TestNotNull(TEXT("event-parameter import restores Timeline node"), TimelineNode);
	TestNotNull(TEXT("event-parameter import restores CustomEvent"), EventNode);
	if (!TimelineNode || !EventNode) return false;

	UEdGraphPin* EventThen = FindPin(EventNode, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* EventTime = FindPin(EventNode, TimeParameterName, EGPD_Output);
	UEdGraphPin* TimelineNewTime = TimelineNode->GetNewTimePin();
	TestNotNull(TEXT("CustomEvent float parameter pin is restored"), EventTime);
	TestTrue(TEXT("CustomEvent drives Timeline SetNewTime"),
		AreLinked(EventThen, TimelineNode->GetSetNewTimePin()));
	TestTrue(TEXT("CustomEvent float parameter directly drives Timeline NewTime"),
		AreLinked(EventTime, TimelineNewTime));
	TestEqual(TEXT("Timeline NewTime has exactly one direct source"),
		TimelineNewTime ? TimelineNewTime->LinkedTo.Num() : 0, 1);

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bIncludePositions = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult Exported =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	if (!Exported.bSuccess) AddError(TEXT("event-parameter set-new-time export: ") + Exported.Error);
	TestTrue(TEXT("event-parameter Timeline graph re-exports"), Exported.bSuccess);
	TestTrue(TEXT("re-export preserves the event parameter declaration"),
		Exported.LispCode.Contains(TEXT(":param (RequestedTime float)")));
	TestTrue(TEXT("re-export preserves the event parameter as NewTime expression"),
		Exported.LispCode.Contains(TEXT(":action set-new-time :time RequestedTime")));
	return true;
}

BL_TEST(Timeline_ReplaceGraphMissingDefinitionFailsBeforeClear)
bool FTimeline_ReplaceGraphMissingDefinitionFailsBeforeClear::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispTimelineTest;
	const FName TimelineName(TEXT("ExistingTimeline"));
	const FName EventName(TEXT("ControlExistingTimeline"));
	const FString InitialCode = TEXT(
		"(timeline \"ExistingTimeline\" :length 3 :length-mode timeline-length "
		":autoplay false :loop true :replicated false :ignore-time-dilation false "
		":track (float \"ExistingAlpha\" :external false "
		":curve (rich-curve :key (key :time 0 :value 0) :key (key :time 3 :value 1))) "
		":id \"b1000001\")\n"
		"(event ControlExistingTimeline :event-id \"b2000002\" "
		"(timeline-control :timeline \"ExistingTimeline\" :action play-from-start))");
	const FString MissingDefinitionCode = TEXT(
		"(event ControlExistingTimeline :event-id \"b2000002\" "
		"(timeline-control :timeline \"ExistingTimeline\" :action stop))");

	const FFixture Fixture = MakeFixture(TEXT("BP_BL_TimelineReplacePreflight"));
	TestNotNull(TEXT("ReplaceGraph preflight test Blueprint is created"), Fixture.Blueprint);
	TestNotNull(TEXT("ReplaceGraph preflight test EventGraph is created"), Fixture.Graph);
	if (!Fixture.Blueprint || !Fixture.Graph) return false;

	FBlueprintLispConverter::FImportOptions InitialOptions;
	InitialOptions.ImportMode = FBlueprintLispConverter::EImportMode::MergeAppend;
	InitialOptions.bAutoLayout = false;
	InitialOptions.bCompile = true;
	const FBlueprintLispResult InitialImport =
		FBlueprintLispConverter::ImportGraph(Fixture.Graph, InitialCode, InitialOptions);
	if (!InitialImport.bSuccess) AddError(TEXT("initial ReplaceGraph preflight fixture import: ") + InitialImport.Error);
	TestTrue(TEXT("initial ReplaceGraph preflight fixture imports"), InitialImport.bSuccess);
	if (!InitialImport.bSuccess) return false;

	UK2Node_Timeline* TimelineNodeBefore = FindTimelineNode(Fixture.Graph, TimelineName);
	UTimelineTemplate* TemplateBefore = Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName);
	UK2Node_CustomEvent* EventNodeBefore = FindCustomEvent(Fixture.Graph, EventName);
	TestNotNull(TEXT("existing Timeline node is present"), TimelineNodeBefore);
	TestNotNull(TEXT("existing Timeline template is present"), TemplateBefore);
	TestNotNull(TEXT("existing Timeline control event is present"), EventNodeBefore);
	if (!TimelineNodeBefore || !TemplateBefore || !EventNodeBefore) return false;
	UEdGraphPin* EventThenBefore = FindPin(EventNodeBefore, UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* PlayFromStartBefore = TimelineNodeBefore->GetPlayFromStartPin();
	TestTrue(TEXT("existing Event to Timeline link is present"),
		AreLinked(EventThenBefore, PlayFromStartBefore));
	const int32 NodeCountBefore = Fixture.Graph->Nodes.Num();
	const int32 TemplateCountBefore = Fixture.Blueprint->Timelines.Num();
	const FGuid TimelineGuidBefore = TimelineNodeBefore->NodeGuid;
	const FGuid EventGuidBefore = EventNodeBefore->NodeGuid;
	const int32 EventLinkCountBefore = EventThenBefore ? EventThenBefore->LinkedTo.Num() : INDEX_NONE;
	const EBlueprintStatus BlueprintStatusBefore = Fixture.Blueprint->Status;

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bIncludePositions = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult ExportBefore =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	TestTrue(TEXT("existing Timeline graph exports before ReplaceGraph preflight"), ExportBefore.bSuccess);
	if (!ExportBefore.bSuccess) return false;

	FBlueprintLispConverter::FImportOptions ReplaceOptions = InitialOptions;
	ReplaceOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	const FBlueprintLispResult FailedImport = FBlueprintLispConverter::ImportGraph(
		Fixture.Graph, MissingDefinitionCode, ReplaceOptions);
	TestFalse(TEXT("ReplaceGraph rejects Timeline reference without a definition"), FailedImport.bSuccess);
	TestTrue(TEXT("ReplaceGraph missing-definition error names unknown Timeline"),
		FailedImport.Error.Contains(TEXT("references unknown Timeline")));
	TestEqual(TEXT("failed ReplaceGraph preflight preserves node count"), Fixture.Graph->Nodes.Num(), NodeCountBefore);
	TestEqual(TEXT("failed ReplaceGraph preflight preserves Timeline template count"),
		Fixture.Blueprint->Timelines.Num(), TemplateCountBefore);
	TestTrue(TEXT("failed ReplaceGraph preflight preserves Timeline node pointer"),
		FindTimelineNode(Fixture.Graph, TimelineName) == TimelineNodeBefore);
	TestTrue(TEXT("failed ReplaceGraph preflight preserves Timeline template pointer"),
		Fixture.Blueprint->FindTimelineTemplateByVariableName(TimelineName) == TemplateBefore);
	TestTrue(TEXT("failed ReplaceGraph preflight preserves event pointer"),
		FindCustomEvent(Fixture.Graph, EventName) == EventNodeBefore);
	TestTrue(TEXT("failed ReplaceGraph preflight preserves Timeline GUID"),
		TimelineNodeBefore->NodeGuid == TimelineGuidBefore);
	TestTrue(TEXT("failed ReplaceGraph preflight preserves event GUID"),
		EventNodeBefore->NodeGuid == EventGuidBefore);
	TestEqual(TEXT("failed ReplaceGraph preflight preserves Event link count"),
		EventThenBefore ? EventThenBefore->LinkedTo.Num() : INDEX_NONE, EventLinkCountBefore);
	TestTrue(TEXT("failed ReplaceGraph preflight preserves Event to Timeline link"),
		AreLinked(EventThenBefore, PlayFromStartBefore));
	TestTrue(TEXT("failed ReplaceGraph preflight preserves Blueprint status"),
		Fixture.Blueprint->Status == BlueprintStatusBefore);

	const FBlueprintLispResult ExportAfter =
		FBlueprintLispConverter::ExportGraph(Fixture.Graph, ExportOptions);
	TestTrue(TEXT("existing Timeline graph exports after failed ReplaceGraph preflight"), ExportAfter.bSuccess);
	if (ExportAfter.bSuccess)
	{
		TestEqual(TEXT("failed ReplaceGraph preflight leaves exported DSL unchanged"),
			BlueprintLisp::Minify(ExportAfter.LispCode), BlueprintLisp::Minify(ExportBefore.LispCode));
	}
	return true;
}

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
	TestTrue(TEXT("destination starts disconnected"), Destination.ReturnPin && Destination.ReturnPin->LinkedTo.Num() == 0);
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
	TestTrue(TEXT("create-object connects to variable set"), Schema && Schema->TryCreateConnection(
		FindPin(CreateNode, UEdGraphSchema_K2::PN_Then, EGPD_Output), SetNode->GetExecPin()));
	TestTrue(TEXT("variable set connects to function result"), Schema && Schema->TryCreateConnection(
		FindPin(SetNode, UEdGraphSchema_K2::PN_Then, EGPD_Output), ResultExecute));
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

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
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
#endif

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

#endif // UE 5.8+

