// Fill out your copyright notice in the Description page of Project Settings.


#include "MilitaryStructureSubsystem.h"

#include "Internationalization/Internationalization.h"

#define LOCTEXT_NAMESPACE "MyNamespace" // TODO

typedef TTuple<const uint8, FName> TUnitPair;

static const TUnitPair MilitaryUnits[] = {
	// pairs are in order of (SubUnitCount, UnitName)
	TUnitPair(3, TEXT("Corps")),
	TUnitPair(3, TEXT("Division")),
	TUnitPair(4, TEXT("Brigade")),
	TUnitPair(3, TEXT("Battalion")),
	TUnitPair(3, TEXT("Company")),
	TUnitPair(3, TEXT("Platoon")),
	TUnitPair(2, TEXT("Squad")),
	TUnitPair(3, TEXT("Fire team")),
	TUnitPair(0, TEXT("Soldier")),
};
static const uint8 MilitaryUnits_Count = sizeof(MilitaryUnits) / sizeof(MilitaryUnits[0]);

int32 RecursivelyCreateUnits(UMilitaryUnit* Unit, UMilitaryUnit* Parent, uint8 Depth = 0, uint8 Index = 1)
{
	Unit->Parent = Parent;
	Unit->Depth = Depth;
	Unit->Name = FText::Format(LOCTEXT("TODO", "{0} {1}"), FText::FromName(MilitaryUnits[Depth].Value), Index);

	uint8 SubUnitCount = MilitaryUnits[Depth].Key;

	if (SubUnitCount == 0)
	{
		Unit->bIsSoldier = true;
		return 1;
	}

	UMilitaryUnit* Commander = NewObject<UMilitaryUnit>();
	Commander->bIsSoldier = true;
	Commander->bIsCommander = true;
	Commander->Depth = Depth + 1;
	Commander->Name = FText::Format(LOCTEXT("TODO", "{0} Commander"), Unit->Name);
	Commander->Parent = Unit;
	Unit->Commander = Commander;
	Unit->SubUnits.Add(Commander);
	int32 Result = 1;

	for (uint8 i = 0; i < SubUnitCount; i++)
	{
		UMilitaryUnit* SubUnit = NewObject<UMilitaryUnit>();
		Result += RecursivelyCreateUnits(SubUnit, Unit, Depth + 1, i + 1);
		Unit->SubUnits.Add(SubUnit);
	}

	return Result;
}

//----------------------------------------------------------------------//
//  UMilitaryStructureSubsystem
//----------------------------------------------------------------------//
int32 UMilitaryStructureSubsystem::CreateMilitaryUnit(uint8 MilitaryUnitIndex, bool bIsTeam1)
{
	UMilitaryUnit* RootUnit = NewObject<UMilitaryUnit>();
	int32 Count = RecursivelyCreateUnits(RootUnit, nullptr, MilitaryUnitIndex);
	if (bIsTeam1)
	{
		Team1RootUnit = RootUnit;
	}
	else
	{
		Team2RootUnit = RootUnit;
	}
	return Count;
}

void PrintMilitaryStructure(UMilitaryUnit* Unit, FString Prefix = FString(""))
{
	UE_LOG(LogTemp, Warning, TEXT("%sName: %s"), *Prefix, *Unit->Name.ToString());

	for (UMilitaryUnit* SubUnit : Unit->SubUnits)
	{
		PrintMilitaryStructure(SubUnit, Prefix + FString("  "));
	}
}

void UMilitaryStructureSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UMilitaryStructureSubsystem::BindUnitToMassEntity(UMilitaryUnit* MilitaryUnit, FMassEntityHandle Entity)
{
	if (EntityToUnitMap.Contains(Entity))
	{
		EntityToUnitMap.Remove(Entity);
	}
	MilitaryUnit->MassEntityIndex = Entity.Index;
	MilitaryUnit->MassEntitySerialNumber = Entity.SerialNumber;
	EntityToUnitMap.Add(Entity, MilitaryUnit);
}

void UMilitaryStructureSubsystem::DestroyEntity(FMassEntityHandle Entity)
{
	UMilitaryUnit** MilitaryUnit = EntityToUnitMap.Find(Entity);
	if (MilitaryUnit)
	{
		(*MilitaryUnit)->RemoveFromParent();
		EntityToUnitMap.Remove(Entity);
	}
}

UMilitaryUnit* UMilitaryStructureSubsystem::GetRootUnitForTeam(const bool bIsTeam1)
{
	return bIsTeam1 ? Team1RootUnit : Team2RootUnit;
}

UMilitaryUnit* UMilitaryStructureSubsystem::GetUnitForEntity(const FMassEntityHandle Entity)
{
	UMilitaryUnit** MilitaryUnit = EntityToUnitMap.Find(Entity);
	if (!MilitaryUnit)
	{
		return nullptr;
	}

	return *MilitaryUnit;
}

//----------------------------------------------------------------------//
//  UMilitaryUnit
//----------------------------------------------------------------------//
void UMilitaryUnit::RemoveFromParent()
{
	if (Parent->Commander == this)
	{
		Parent->Commander = nullptr;
	}
	Parent->SubUnits.Remove(this);
}

FMassEntityHandle UMilitaryUnit::GetMassEntityHandle()
{
	return FMassEntityHandle(MassEntityIndex, MassEntitySerialNumber);
}

bool UMilitaryUnit::IsChildOfUnit(const UMilitaryUnit* ParentUnit)
{
	if (!ParentUnit) {
		return false;
	}

	UMilitaryUnit* ChildUnit = this;
	if (ParentUnit->bIsSoldier)
	{
		return ChildUnit == ParentUnit;
	}

	// If we got here, ParentUnit is set to a non-soldier.

	while (ChildUnit)
	{
		if (ChildUnit == ParentUnit) {
			return true;
		}
		ChildUnit = ChildUnit->Parent;
	}

	return false;
}

void UMilitaryStructureSubsystem::DidCompleteAssigningEntitiesToMilitaryUnits(const bool bIsTeam1)
{
	(bIsTeam1 ? bDidCompleteAssigningEntitiesToMilitaryUnitsForTeam1 : bDidCompleteAssigningEntitiesToMilitaryUnitsForTeam2) = true;

	if (bDidCompleteAssigningEntitiesToMilitaryUnitsForTeam1 && bDidCompleteAssigningEntitiesToMilitaryUnitsForTeam2)
	{
		OnCompletedAssigningEntitiesToMilitaryUnitsEvent.Broadcast();
	}
}