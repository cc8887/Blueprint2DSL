// Copyright (c) 2026 cc8887. All Rights Reserved.
#include "BlueprintLispGraphMaintenance.h"

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#include "BlueprintLispConverter.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#if __has_include("StructUtils/UserDefinedStruct.h")
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CreateDelegate.h"
#include "AnimGraphNode_Base.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace BLGraphMaintenance
{
static FString Json(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text));
	return Text;
}
static FBlueprintLispPythonResult Result(bool Success, const FString& Message)
{
	FBlueprintLispPythonResult R; R.bSuccess = Success; R.Message = Message; return R;
}
static UEdGraph* Graph(UBlueprint* BP, const FString& Name)
{
	TArray<UEdGraph*> Graphs;
	if (BP) BP->GetAllGraphs(Graphs);
	for (auto* G : Graphs) if (G && G->GetName() == Name) return G;
	return nullptr;
}
}

FString UBlueprintLispGraphMaintenance::InspectBlueprint(UBlueprint* Blueprint)
{
	using namespace BLGraphMaintenance;
	auto Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Nodes;
	TArray<UEdGraph*> Graphs;
	if (Blueprint) Blueprint->GetAllGraphs(Graphs);
	for (auto* G : Graphs) for (UEdGraphNode* N : G->Nodes)
	{
		if (!N) continue;
		auto Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("path"), N->GetPathName());
		Item->SetStringField(TEXT("graph"), G->GetName());
		Item->SetStringField(TEXT("class"), N->GetClass()->GetName());
		Item->SetStringField(TEXT("title"), N->GetNodeTitle(ENodeTitleType::ListView).ToString());
		if (auto* V = Cast<UK2Node_Variable>(N))
		{
			Item->SetStringField(TEXT("member"), V->GetVarName().ToString());
			Item->SetBoolField(TEXT("member_resolved"), V->GetPropertyForVariable() != nullptr);
		}
		if (auto* C = Cast<UK2Node_CallFunction>(N))
		{
			Item->SetStringField(TEXT("function"), C->GetFunctionName().ToString());
			Item->SetBoolField(TEXT("pure"), C->IsNodePure());
			if (auto* F = C->GetTargetFunction()) Item->SetStringField(TEXT("owner"), F->GetOwnerClass()->GetPathName());
		}
		if (auto* C = Cast<UK2Node_DynamicCast>(N))
			Item->SetStringField(TEXT("cast_target"), C->TargetType ? C->TargetType->GetPathName() : FString());
		if (auto* Anim = Cast<UAnimGraphNode_Base>(N))
		{
			const UObject* Binding = reinterpret_cast<const UObject*>(Anim->GetBinding());
			const auto* Property = Binding ? FindFProperty<FMapProperty>(Binding->GetClass(), TEXT("PropertyBindings")) : nullptr;
			if (Property)
			{
				const auto* Map = reinterpret_cast<const TMap<FName, FAnimGraphNodePropertyBinding>*>(Property->ContainerPtrToValuePtr<void>(Binding));
				TArray<TSharedPtr<FJsonValue>> Bindings;
				for (const auto& Pair : *Map)
				{
					auto Entry = MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("property"), Pair.Key.ToString());
					Entry->SetStringField(TEXT("path"), FString::Join(Pair.Value.PropertyPath, TEXT(".")));
					Entry->SetStringField(TEXT("context"), Pair.Value.ContextId.ToString());
					Entry->SetBoolField(TEXT("bound"), Pair.Value.bIsBound); Bindings.Add(MakeShared<FJsonValueObject>(Entry));
				}
				Item->SetArrayField(TEXT("bindings"), Bindings);
			}
		}
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (auto* P : N->Pins)
		{
			if (!P) continue;
			auto Pin = MakeShared<FJsonObject>();
			Pin->SetStringField(TEXT("name"), P->PinName.ToString());
			Pin->SetStringField(TEXT("direction"), P->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			Pin->SetStringField(TEXT("category"), P->PinType.PinCategory.ToString());
			Pin->SetStringField(TEXT("subcategory"), P->PinType.PinSubCategory.ToString());
			Pin->SetStringField(TEXT("type_object"), P->PinType.PinSubCategoryObject.IsValid() ? P->PinType.PinSubCategoryObject->GetPathName() : FString());
			Pin->SetBoolField(TEXT("reference"), P->PinType.bIsReference);
			Pin->SetBoolField(TEXT("orphaned"), P->bOrphanedPin);
			Pin->SetBoolField(TEXT("array"), P->PinType.IsArray());
			Pin->SetStringField(TEXT("value"), P->DefaultValue);
			Pin->SetStringField(TEXT("object"), P->DefaultObject ? P->DefaultObject->GetPathName() : FString());
			TArray<TSharedPtr<FJsonValue>> Links;
			for (auto* L : P->LinkedTo)
			{
				if (!L || !L->GetOwningNode()) continue;
				auto Link = MakeShared<FJsonObject>();
				Link->SetStringField(TEXT("node"), L->GetOwningNode()->GetPathName());
				Link->SetStringField(TEXT("pin"), L->PinName.ToString());
				Links.Add(MakeShared<FJsonValueObject>(Link));
			}
			Pin->SetArrayField(TEXT("links"), Links);
			Pins.Add(MakeShared<FJsonValueObject>(Pin));
		}
		Item->SetArrayField(TEXT("pins"), Pins);
		Nodes.Add(MakeShared<FJsonValueObject>(Item));
	}
	Root->SetArrayField(TEXT("nodes"), Nodes);
	return Json(Root);
}

FBlueprintLispPythonResult UBlueprintLispGraphMaintenance::SetPinExpression(UBlueprint* Blueprint,
	const FString& GraphName, const FString& NodeName, const FString& PinName, const FString& Expression)
{
	using namespace BLGraphMaintenance;
	auto* G = Graph(Blueprint, GraphName);
	if (G) for (UEdGraphNode* N : G->Nodes) if (N && N->GetName() == NodeName)
	{
		auto* P = N->FindPin(FName(*PinName), EGPD_Input);
		if (!P) return Result(false, TEXT("Input pin not found"));
		const auto R = FBlueprintLispConverter::ImportPureExpression(G, P, Expression);
		return Result(R.bSuccess, R.bSuccess ? TEXT("Input expression replaced; not saved") : R.Error);
	}
	return Result(false, TEXT("Graph or node not found"));
}

FBlueprintLispPythonResult UBlueprintLispGraphMaintenance::RepairGraph(UBlueprint* Blueprint, const FString& GraphName, bool bRemoveIsolatedMissingVariables)
{
	using namespace BLGraphMaintenance;
	auto* G = Graph(Blueprint, GraphName);
	if (!G) return Result(false, TEXT("Graph not found"));
	TArray<TSharedPtr<FJsonValue>> Changes;
	TArray<FString> Errors;
	const auto OriginalNodes = G->Nodes;
	for (UEdGraphNode* N : OriginalNodes)
	{
		if (!N) continue;
		if (auto* Anim = Cast<UAnimGraphNode_Base>(N))
		{
			const UObject* Binding = reinterpret_cast<const UObject*>(Anim->GetBinding());
			const auto* Property = Binding ? FindFProperty<FMapProperty>(Binding->GetClass(), TEXT("PropertyBindings")) : nullptr;
			if (Property)
			{
				const auto* Map = reinterpret_cast<const TMap<FName, FAnimGraphNodePropertyBinding>*>(Property->ContainerPtrToValuePtr<void>(Binding));
				for (const auto& Pair : *Map)
				{
					if (!Pair.Value.bIsBound || Pair.Value.PropertyPath.Num() == 0) continue;
					const auto Index = Anim->ShowPinForProperties.IndexOfByPredicate([&](const FOptionalPinFromProperty& Pin) { return Pin.PropertyName == Pair.Key; });
					if (Index != INDEX_NONE && !Anim->ShowPinForProperties[Index].bShowPin)
					{
						Anim->SetPinVisibility(true, Index);
						Changes.Add(MakeShared<FJsonValueString>(N->GetPathName() + TEXT(": exposed bound destination pin ") + Pair.Key.ToString()));
					}
				}
			}
		}
		if (auto* Variable = Cast<UK2Node_Variable>(N))
		{
			if (bRemoveIsolatedMissingVariables && !Variable->GetVarName().IsNone() && !Variable->GetPropertyForVariable()
				&& !N->Pins.ContainsByPredicate([](const UEdGraphPin* Pin) { return Pin && Pin->LinkedTo.Num() != 0; }))
			{
				Changes.Add(MakeShared<FJsonValueString>(N->GetPathName() + TEXT(": removed already isolated missing-variable artifact: ") + Variable->GetVarName().ToString()));
				N->DestroyNode(); continue;
			}
		}
		if (auto* Call = Cast<UK2Node_CallFunction>(N))
		{
			auto* F = Call->GetTargetFunction();
			if (F && F->GetOwnerClass()->GetPathName() == TEXT("/Script/Engine.KismetMathLibrary")
				&& (F->GetFName() == TEXT("Conv_FloatToDouble") || F->GetFName() == TEXT("Conv_DoubleToFloat")))
			{
				UEdGraphPin* Input = nullptr; UEdGraphPin* Output = nullptr;
				for (auto* P : N->Pins) if (P && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
					(P->Direction == EGPD_Input ? Input : Output) = P;
				if (Input && Output)
				{
					// Widening is lossless. Narrowing must retain its rounding step;
					// replacing it with a direct double wire changes observable values.
					bool Safe = true;
					const bool Narrowing = F->GetFName() == TEXT("Conv_DoubleToFloat");
					const auto Consumers = Output->LinkedTo;
					for (auto* Dest : Consumers)
					{
						// A float destination retains the same narrowing at its input.
						if (Narrowing && Dest->PinType.PinSubCategory != UEdGraphSchema_K2::PC_Float) Safe = false;
						if (Input->LinkedTo.Num() == 1)
						{
							const auto Response = G->GetSchema()->CanCreateConnection(Input->LinkedTo[0], Dest);
							Safe &= Response.Response == CONNECT_RESPONSE_MAKE || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B;
						}
						else Safe = false;
					}
					if (!Safe || Input->LinkedTo.Num() != 1
						|| (!Narrowing && Input->LinkedTo[0]->PinType.PinSubCategory != UEdGraphSchema_K2::PC_Float)) continue;
					auto* Source = Input->LinkedTo[0];
					for (auto* Dest : Consumers) { Dest->BreakLinkTo(Output); G->GetSchema()->TryCreateConnection(Source, Dest); }
					Changes.Add(MakeShared<FJsonValueString>(N->GetPathName() + TEXT(": bypassed obsolete conversion preserving destination precision")));
					N->DestroyNode();
					continue;
				}
			}
		}
		const auto Pins = N->Pins;
		for (auto* P : Pins)
		{
			if (P && P->bOrphanedPin && !P->ParentPin && !P->SubPins.Num() && !P->LinkedTo.Num())
			{
				const auto* Live = N->Pins.FindByPredicate([&](const UEdGraphPin* Other)
				{
					return Other && Other != P && !Other->bOrphanedPin && Other->PinName == P->PinName
						&& Other->Direction == P->Direction && Other->DefaultValue == P->DefaultValue
						&& Other->DefaultObject == P->DefaultObject && Other->DefaultTextValue.EqualTo(P->DefaultTextValue);
				});
				if (Live)
				{
					Changes.Add(MakeShared<FJsonValueString>(N->GetPathName() + TEXT(": removed redundant orphan pin after preserving its default: ") + P->PinName.ToString()));
					N->RemovePin(P); continue;
				}
			}
			if (!P || P->Direction != EGPD_Input || !P->PinType.IsArray() || P->LinkedTo.Num() || !P->DefaultValue.TrimStart().StartsWith(TEXT("["))) continue;
			const auto R = FBlueprintLispConverter::ImportPureExpression(G, P,
				FString::Printf(TEXT("\"%s\""), *P->DefaultValue.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""))));
			if (R.bSuccess) Changes.Add(MakeShared<FJsonValueString>(N->GetPathName() + TEXT(".") + P->PinName.ToString() + TEXT(": expanded array literal")));
			else Errors.Add(R.Error);
		}
	}
	if (Changes.Num()) FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	auto Details = MakeShared<FJsonObject>(); Details->SetArrayField(TEXT("changes"), Changes);
	auto R = Result(Errors.IsEmpty(), FString::Join(Errors, TEXT("\n")));
	R.DSLText = Json(Details); return R;
}

bool UBlueprintLispGraphMaintenance::SetMemberRealPrecision(UBlueprint* Blueprint, const FString& VariableName, bool bDoublePrecision)
{
	if (!Blueprint) return false;
	const auto Index = Blueprint->NewVariables.IndexOfByPredicate([&](const FBPVariableDescription& V) { return V.VarName == FName(*VariableName); });
	if (Index == INDEX_NONE) return false;
	auto& Type = Blueprint->NewVariables[Index].VarType;
	if (Type.PinCategory != UEdGraphSchema_K2::PC_Real || Type.ContainerType != EPinContainerType::None) return false;
	Type.PinSubCategory = bDoublePrecision ? UEdGraphSchema_K2::PC_Double : UEdGraphSchema_K2::PC_Float;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
	for (auto* G : Graphs) for (UEdGraphNode* N : G->Nodes)
		if (auto* V = Cast<UK2Node_Variable>(N)) if (V->GetVarName() == FName(*VariableName)) V->ReconstructNode();
	return true;
}

FBlueprintLispPythonResult UBlueprintLispGraphMaintenance::BindDelegateInput(UBlueprint* Blueprint,
	const FString& GraphName, const FString& NodeName, const FString& PinName, const FString& FunctionName)
{
	using namespace BLGraphMaintenance;
	auto* G = Graph(Blueprint, GraphName);
	if (!G || !Blueprint->SkeletonGeneratedClass || !Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*FunctionName)))
		return Result(false, TEXT("Graph or self function not found"));
	for (UEdGraphNode* N : G->Nodes) if (N && N->GetName() == NodeName)
	{
		auto* Input = N->FindPin(FName(*PinName), EGPD_Input);
		if (!Input || Input->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate || Input->LinkedTo.Num())
			return Result(false, TEXT("An unconnected delegate input is required"));
		auto* Delegate = NewObject<UK2Node_CreateDelegate>(G);
		G->AddNode(Delegate, false, false); Delegate->CreateNewGuid(); Delegate->AllocateDefaultPins();
		if (!G->GetSchema()->TryCreateConnection(Delegate->GetDelegateOutPin(), Input))
		{ Delegate->DestroyNode(); return Result(false, TEXT("Delegate signature connection rejected")); }
		Delegate->SetFunction(FName(*FunctionName)); Delegate->HandleAnyChangeWithoutNotifying();
		const auto* Signature = Delegate->GetDelegateSignature();
		const auto* Function = Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*FunctionName));
		if (!Signature || !Function || !Function->IsSignatureCompatibleWith(Signature)
			|| Delegate->GetFunctionName() != FName(*FunctionName))
		{ Delegate->DestroyNode(); return Result(false, TEXT("Self callback does not match the delegate signature")); }
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return Result(true, TEXT("Bound explicit self callback; not saved"));
	}
	return Result(false, TEXT("Node not found"));
}

FBlueprintLispPythonResult UBlueprintLispGraphMaintenance::ScheduleCallBeforeConsumer(UBlueprint* Blueprint,
	const FString& GraphName, const FString& CallNodeName, const FString& ConsumerNodeName)
{
	using namespace BLGraphMaintenance;
	auto* G = Graph(Blueprint, GraphName);
	UK2Node_CallFunction* Call = nullptr; UEdGraphNode* Consumer = nullptr;
	if (G) for (UEdGraphNode* N : G->Nodes)
	{
		if (N->GetName() == CallNodeName) Call = Cast<UK2Node_CallFunction>(N);
		if (N->GetName() == ConsumerNodeName) Consumer = N;
	}
	auto* ConsumerExec = Consumer ? Consumer->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input) : nullptr;
	if (!Call || Call->IsNodePure() || !Call->GetExecPin() || Call->GetExecPin()->LinkedTo.Num()
		|| !ConsumerExec || ConsumerExec->LinkedTo.Num() != 1 || !Call->GetTargetFunction())
		return Result(false, TEXT("An unexecuted call and a consumer with one execution predecessor are required"));
	TSet<UEdGraphPin*> Visiting;
	TMap<UEdGraphPin*, bool> Dependencies;
	TFunction<bool(UEdGraphPin*)> Depends = [&](UEdGraphPin* Pin)
	{
		if (const auto* Cached = Dependencies.Find(Pin)) return *Cached;
		if (!Pin || Visiting.Contains(Pin)) return false;
		Visiting.Add(Pin);
		bool Found = Pin->GetOwningNode() == Call;
		if (!Found) for (auto* Input : Pin->GetOwningNode()->Pins)
			if (Input && Input->Direction == EGPD_Input && Input->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				for (auto* Source : Input->LinkedTo) Found |= Depends(Source);
		Visiting.Remove(Pin); Dependencies.Add(Pin, Found); return Found;
	};
	TArray<UEdGraphPin*> Destinations;
	for (auto* Pin : Consumer->Pins)
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
			&& Pin->LinkedTo.Num() == 1 && Depends(Pin->LinkedTo[0])) Destinations.Add(Pin);
	if (!Destinations.Num()) return Result(false, TEXT("Consumer does not read this call"));
	// Preflight the complete copied data slice before changing any live links.
	for (const auto& Pair : Dependencies) if (Pair.Value)
	{
		auto* N = Cast<UK2Node_CallFunction>(Pair.Key->GetOwningNode());
		if (!N || !N->GetTargetFunction() || (N != Call && !N->IsNodePure()))
			return Result(false, TEXT("Only call/pure-function data slices are supported; no execution order was guessed"));
		for (auto* P : N->Pins) if (P->ParentPin || P->SubPins.Num())
			return Result(false, TEXT("Split-pin scheduling requires an explicit migration"));
	}
	TMap<UK2Node_CallFunction*, UK2Node_CallFunction*> Copies;
	TArray<UEdGraphNode*> Created;
	bool Valid = true;
	TFunction<UEdGraphPin*(UEdGraphPin*)> CopyOutput;
	CopyOutput = [&](UEdGraphPin* Source) -> UEdGraphPin*
	{
		if (!Depends(Source)) return Source;
		auto* Original = CastChecked<UK2Node_CallFunction>(Source->GetOwningNode());
		auto* Copy = Copies.FindRef(Original);
		if (!Copy)
		{
			Copy = NewObject<UK2Node_CallFunction>(G, Original->GetClass());
			Copy->SetFromFunction(Original->GetTargetFunction());
			G->AddNode(Copy, false, false); Copy->CreateNewGuid(); Copy->AllocateDefaultPins();
			Copies.Add(Original, Copy); Created.Add(Copy);
			Copy->NodePosX = Consumer->NodePosX - 250; Copy->NodePosY = Consumer->NodePosY;
			for (auto* OldPin : Original->Pins)
			{
				auto* NewPin = Copy->FindPin(OldPin->PinName, OldPin->Direction);
				if (!NewPin) { Valid = false; continue; }
				NewPin->PinType = OldPin->PinType;
				NewPin->DefaultValue = OldPin->DefaultValue; NewPin->DefaultObject = OldPin->DefaultObject; NewPin->DefaultTextValue = OldPin->DefaultTextValue;
				if (OldPin->Direction == EGPD_Input && OldPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					for (auto* Link : OldPin->LinkedTo)
					{
						auto* CopiedSource = CopyOutput(Link);
						Valid &= CopiedSource && G->GetSchema()->TryCreateConnection(CopiedSource, NewPin);
					}
			}
		}
		return Copy->FindPin(Source->PinName, EGPD_Output);
	};
	TArray<UEdGraphPin*> Outputs;
	for (auto* Dest : Destinations) Outputs.Add(CopyOutput(Dest->LinkedTo[0]));
	auto* Scheduled = Copies.FindRef(Call);
	if (!Valid || !Scheduled || Outputs.Contains(nullptr))
	{
		for (auto* N : Created) N->DestroyNode(); return Result(false, TEXT("Could not copy the data slice; original flow retained"));
	}
	for (int32 I = 0; I < Destinations.Num(); ++I)
	{
		Destinations[I]->BreakAllPinLinks();
		G->GetSchema()->TryCreateConnection(Outputs[I], Destinations[I]);
	}
	auto* Predecessor = ConsumerExec->LinkedTo[0];
	ConsumerExec->BreakAllPinLinks();
	const bool Connected = G->GetSchema()->TryCreateConnection(Predecessor, Scheduled->GetExecPin())
		&& G->GetSchema()->TryCreateConnection(Scheduled->GetThenPin(), ConsumerExec);
	if (!Connected) return Result(false, TEXT("Execution scheduling failed; do not save this graph"));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return Result(true, TEXT("Scheduled explicit call before consumer with its own data slice; not saved"));
}

bool UBlueprintLispGraphMaintenance::SetFunctionObjectOutputClass(UBlueprint* Blueprint, const FString& FunctionName, const FString& ClassPath)
{
	auto* G = BLGraphMaintenance::Graph(Blueprint, FunctionName);
	auto* Class = LoadObject<UClass>(nullptr, *ClassPath);
	if (!G || !Class) return false;
	bool Changed = false;
	for (UEdGraphNode* N : G->Nodes) if (auto* Result = Cast<UK2Node_FunctionResult>(N))
	{
		for (auto& Pin : Result->UserDefinedPins) if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
		{ Pin->PinType.PinSubCategoryObject = Class; Changed = true; }
		Result->ReconstructNode();
		// Overrides obtain their pins from the parent signature and intentionally
		// have no editable UserDefinedPins. An already-matching signature is valid.
		for (auto* Pin : Result->Pins)
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object && Pin->PinType.PinSubCategoryObject == Class) Changed = true;
	}
	if (Changed)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		for (TObjectIterator<UK2Node_CallFunction> It; It; ++It)
		{
			auto* Call = *It;
			auto* OwnerBP = Call->GetBlueprint();
			if (Call->GetFunctionName() == FName(*FunctionName) && Call->FunctionReference.IsSelfContext()
				&& OwnerBP && OwnerBP->GeneratedClass && Blueprint->GeneratedClass
				&& OwnerBP->GeneratedClass->IsChildOf(Blueprint->GeneratedClass)) Call->ReconstructNode();
		}
	}
	return Changed;
}

bool UBlueprintLispGraphMaintenance::SetFunctionClassInput(UBlueprint* Blueprint, const FString& FunctionName,
	const FString& ParameterName, const FString& BaseClassPath)
{
	auto* G = BLGraphMaintenance::Graph(Blueprint, FunctionName);
	auto* Base = LoadObject<UClass>(nullptr, *BaseClassPath);
	if (!G || !Base) return false;
	bool Changed = false;
	for (UEdGraphNode* N : G->Nodes) if (auto* Entry = Cast<UK2Node_FunctionEntry>(N))
	{
		for (auto& Pin : Entry->UserDefinedPins) if (Pin->PinName == FName(*ParameterName))
		{ Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Class; Pin->PinType.PinSubCategoryObject = Base; Changed = true; }
		Entry->ReconstructNode();
	}
	if (!Changed) return false;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	for (TObjectIterator<UK2Node_CallFunction> It; It; ++It)
	{
		auto* Function = It->GetTargetFunction();
		if (Function && Function->GetFName() == FName(*FunctionName) && Function->GetOwnerClass()->ClassGeneratedBy == Blueprint)
			It->ReconstructNode();
	}
	return true;
}

FBlueprintLispPythonResult UBlueprintLispGraphMaintenance::CreateDataTable(const FString& StructPath,
	const FString& TablePath, const FString& FieldsJson, const FString& RowsJson)
{
	using namespace BLGraphMaintenance;
	if (!StructPath.StartsWith(TEXT("/Game/")) || !TablePath.StartsWith(TEXT("/Game/"))
		|| !FPackageName::IsValidLongPackageName(StructPath) || !FPackageName::IsValidLongPackageName(TablePath)
		|| FPackageName::DoesPackageExist(StructPath) || FPackageName::DoesPackageExist(TablePath)
		|| FindPackage(nullptr, *StructPath) || FindPackage(nullptr, *TablePath))
		return Result(false, TEXT("Use distinct, missing /Game package paths; existing assets are never overwritten"));
	TArray<TSharedPtr<FJsonValue>> Fields;
	if (StructPath == TablePath || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FieldsJson), Fields) || Fields.IsEmpty())
		return Result(false, TEXT("A nonempty fields array and distinct paths are required"));
	TArray<TPair<FString, FEdGraphPinType>> Types;
	for (const auto& Value : Fields)
	{
		const auto Field = Value->AsObject(); FString Name, ClassPath;
		if (!Field || !Field->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty()
			|| !Field->TryGetStringField(TEXT("class"), ClassPath)) return Result(false, TEXT("Each field requires name and class (base class path)"));
		auto* BaseClass = LoadObject<UClass>(nullptr, *ClassPath);
		if (!BaseClass) return Result(false, TEXT("Field base class could not be resolved"));
		FEdGraphPinType Type; Type.PinCategory = UEdGraphSchema_K2::PC_Class; Type.PinSubCategoryObject = BaseClass;
		Types.Emplace(Name, Type);
	}
	TArray<TSharedPtr<FJsonValue>> Rows;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RowsJson), Rows)) return Result(false, TEXT("Rows must be a JSON array"));
	auto* Struct = FStructureEditorUtils::CreateUserDefinedStruct(CreatePackage(*StructPath),
		FName(*FPackageName::GetLongPackageAssetName(StructPath)), RF_Public | RF_Standalone | RF_Transactional);
	if (!Struct) return Result(false, TEXT("Could not create row struct"));
	const auto DefaultFields = FStructureEditorUtils::GetVarDesc(Struct);
	for (const auto& Type : Types)
	{
		if (!FStructureEditorUtils::AddVariable(Struct, Type.Value)) return Result(false, TEXT("Could not add row field"));
		const auto& FieldsNow = FStructureEditorUtils::GetVarDesc(Struct);
		if (!FStructureEditorUtils::RenameVariable(Struct, FieldsNow.Last().VarGuid, Type.Key)) return Result(false, TEXT("Could not name row field"));
	}
	for (const auto& Field : DefaultFields) FStructureEditorUtils::RemoveVariable(Struct, Field.VarGuid);
	auto* Table = NewObject<UDataTable>(CreatePackage(*TablePath),
		FName(*FPackageName::GetLongPackageAssetName(TablePath)), RF_Public | RF_Standalone | RF_Transactional);
	Table->RowStruct = Struct;
	const auto Problems = Table->CreateTableFromJSONString(RowsJson);
	if (Problems.Num()) return Result(false, FString::Join(Problems, TEXT("\n")));
	Struct->MarkPackageDirty(); Table->MarkPackageDirty();
	return Result(true, TEXT("Created explicit row schema and data; not saved"));
}

#else
namespace
{
FBlueprintLispPythonResult MaintenanceUnavailable()
{
	FBlueprintLispPythonResult Result;
	Result.Message = TEXT("BlueprintLisp graph maintenance requires Unreal Engine 5.8 or later");
	return Result;
}
}
FString UBlueprintLispGraphMaintenance::InspectBlueprint(UBlueprint*)
{ return TEXT("{\"supported\":false,\"error\":\"Graph maintenance requires UE 5.8 or later\"}"); }
FBlueprintLispPythonResult UBlueprintLispGraphMaintenance::RepairGraph(UBlueprint*, const FString&, bool)
{ return MaintenanceUnavailable(); }
bool UBlueprintLispGraphMaintenance::SetMemberRealPrecision(UBlueprint*, const FString&, bool) { return false; }
bool UBlueprintLispGraphMaintenance::SetFunctionObjectOutputClass(UBlueprint*, const FString&, const FString&) { return false; }
bool UBlueprintLispGraphMaintenance::SetFunctionClassInput(UBlueprint*, const FString&, const FString&, const FString&) { return false; }
FBlueprintLispPythonResult UBlueprintLispGraphMaintenance::ScheduleCallBeforeConsumer(UBlueprint*, const FString&, const FString&, const FString&)
{ return MaintenanceUnavailable(); }
FBlueprintLispPythonResult UBlueprintLispGraphMaintenance::BindDelegateInput(UBlueprint*, const FString&, const FString&, const FString&, const FString&)
{ return MaintenanceUnavailable(); }
FBlueprintLispPythonResult UBlueprintLispGraphMaintenance::SetPinExpression(UBlueprint*, const FString&, const FString&, const FString&, const FString&)
{ return MaintenanceUnavailable(); }
FBlueprintLispPythonResult UBlueprintLispGraphMaintenance::CreateDataTable(const FString&, const FString&, const FString&, const FString&)
{ return MaintenanceUnavailable(); }
#endif
