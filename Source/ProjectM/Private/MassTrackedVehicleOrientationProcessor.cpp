// Fill out your copyright notice in the Description page of Project Settings.


#include "MassTrackedVehicleOrientationProcessor.h"

void UMassTrackedVehicleOrientationTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, UWorld& World) const
{
	UMassEntitySubsystem* EntitySubsystem = UWorld::GetSubsystem<UMassEntitySubsystem>(&World);
	check(EntitySubsystem);

	BuildContext.AddFragment<FMassMoveTargetFragment>();
	BuildContext.AddFragment<FTransformFragment>();
	BuildContext.AddTag<FMassTrackedVehicleOrientationTag>();

	const FConstSharedStruct OrientationFragment = EntitySubsystem->GetOrCreateConstSharedFragment(UE::StructUtils::GetStructCrc32(FConstStructView::Make(Orientation)), Orientation);
	BuildContext.AddConstSharedFragment(OrientationFragment);
}

UMassTrackedVehicleOrientationProcessor::UMassTrackedVehicleOrientationProcessor()
{
	ExecutionFlags = (int32)EProcessorExecutionFlags::All;
	ExecutionOrder.ExecuteInGroup = UE::Mass::ProcessorGroupNames::Movement;
}

void UMassTrackedVehicleOrientationProcessor::ConfigureQueries()
{
	EntityQuery.AddRequirement<FMassMoveTargetFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddConstSharedRequirement<FMassTrackedVehicleOrientationParameters>(EMassFragmentPresence::All);
}

void UMassTrackedVehicleOrientationProcessor::Execute(UMassEntitySubsystem& EntitySubsystem,
	FMassExecutionContext& Context)
{
	QUICK_SCOPE_CYCLE_COUNTER(UMassTrackedVehicleOrientationProcessor_Execute);

	// Clamp max delta time to avoid large values during initialization.
	const float DeltaTime = FMath::Min(0.1f, Context.GetDeltaTimeSeconds());

	EntityQuery.ForEachEntityChunk(EntitySubsystem, Context, [DeltaTime](FMassExecutionContext& Context)
	{
		const int32 NumEntities = Context.GetNumEntities();

		const FMassTrackedVehicleOrientationParameters& OrientationParams = Context.GetConstSharedFragment<FMassTrackedVehicleOrientationParameters>();

		const TConstArrayView<FMassMoveTargetFragment> MoveTargetList = Context.GetFragmentView<FMassMoveTargetFragment>();
		const TArrayView<FTransformFragment> LocationList = Context.GetMutableFragmentView<FTransformFragment>();

		for (int32 EntityIndex = 0; EntityIndex < NumEntities; ++EntityIndex)
		{
			const FMassMoveTargetFragment& MoveTarget = MoveTargetList[EntityIndex];

			FTransform& CurrentTransform = LocationList[EntityIndex].GetMutableTransform();
			const FVector CurrentForward = CurrentTransform.GetRotation().GetForwardVector();
			
			const float CurrentHeading = UE::MassNavigation::GetYawFromDirection(CurrentForward);
			const float DesiredHeading = UE::MassNavigation::GetYawFromDirection(MoveTarget.Forward);

			if (FMath::IsNearlyEqual(CurrentHeading, DesiredHeading))
			{
				return;
			}

			float NewHeading;
			float DeltaHeading = FMath::DegreesToRadians(OrientationParams.TurningSpeed) * DeltaTime;
			if (FMath::Abs(CurrentHeading - DesiredHeading) <= DeltaHeading)
			{
				NewHeading = DesiredHeading;
			}
			else
			{
				DeltaHeading = DesiredHeading > CurrentHeading ? DeltaHeading : -DeltaHeading;
				NewHeading = CurrentHeading + DeltaHeading;
			}

			FQuat Rotation(FVector::UpVector, NewHeading);
			CurrentTransform.SetRotation(Rotation);
		}
	});
}
