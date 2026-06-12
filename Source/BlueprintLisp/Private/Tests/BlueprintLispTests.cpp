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
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

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

