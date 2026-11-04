// Copyright (c) 2026 cc8887. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "BlueprintLispSemanticTestTypes.generated.h"

// Native, asset-independent fixtures for converter regression tests. Kept in the
// Editor module so generated Blueprint classes can also be reloaded in a new process.
UCLASS(BlueprintType, Blueprintable)
class UBlueprintLispSemanticTestObject : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="BlueprintLispTests")
	UBlueprintLispSemanticTestObject* Other = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="BlueprintLispTests")
	float Value = 17.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="BlueprintLispTests")
	FVector Location = FVector(11, 22, 33);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="BlueprintLispTests")
	TSubclassOf<UObject> ClassValue;
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLispTests")
	int32 InvocationCount = 0;
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLispTests")
	float ObservedScalar = 0;
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLispTests")
	int32 ObservedNumber = 0;
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLispTests")
	bool bObservedPositive = false;
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLispTests")
	TArray<int32> ObservedValues;

	UFUNCTION(BlueprintCallable, Category="BlueprintLispTests")
	void RecordScalar(float Input) { ObservedScalar = Input; }
	UFUNCTION(BlueprintCallable, Category="BlueprintLispTests")
	int32 ProduceValues(int32 Seed, bool& bPositive, TArray<int32>& Values)
	{
		++InvocationCount;
		bPositive = Seed > 0;
		Values = { Seed, Seed + 1 };
		return Seed * 2;
	}
	UFUNCTION(BlueprintCallable, Category="BlueprintLispTests")
	void RecordValues(int32 Number, bool bPositive, const TArray<int32>& Values)
	{
		ObservedNumber = Number;
		bObservedPositive = bPositive;
		ObservedValues = Values;
	}
	UFUNCTION(BlueprintPure, Category="BlueprintLispTests")
	int32 PureValues(int32 Seed, bool& bPositive, TArray<int32>& Values) const
	{
		bPositive = Seed > 0;
		Values = { Seed, Seed + 1 };
		return Seed * 2;
	}
};
