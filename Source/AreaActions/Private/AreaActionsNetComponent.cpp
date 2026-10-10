#include "AreaActionsNetComponent.h"

#include "AreaActions.h"
#include "Blueprint/UserWidget.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

namespace
{
	// Names shared with the Blueprints.
	const FName NAME_RunAction(TEXT("RunAction"));
	const FName NAME_AddWidget(TEXT("AddWidget"));
	const FName NAME_Finished(TEXT("Finished"));
	const FName NAME_Done(TEXT("Done"));
	const FName NAME_Player(TEXT("Player"));
	const FName NAME_AreaActionsEquip(TEXT("AreaActionsEquip"));
	const FName NAME_RunsOnClient(TEXT("RunsOnClient"));
	const FName NAME_ServerSyncedProperties(TEXT("ServerSyncedProperties"));
	const FName NAME_CornerLocations(TEXT("CornerLocations"));
	const FName NAME_MinZ(TEXT("MinZ"));
	const FName NAME_MaxZ(TEXT("MaxZ"));
	const FName NAME_InteractObject(TEXT("mInteractObject"));

	bool IsDeclaredInBlueprint(const FProperty* Property)
	{
		return Cast<UBlueprintGeneratedClass>(Property->GetOwnerClass()) != nullptr;
	}

	/** Hard object references travel as objects; everything else, class references included, as text. */
	const FObjectProperty* AsObjectReference(const FProperty* Property)
	{
		const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property);
		return ObjectProperty && !CastField<FClassProperty>(Property) ? ObjectProperty : nullptr;
	}

	FAreaActionsNetValue ExportValue(const FProperty* Property, const void* Value)
	{
		FAreaActionsNetValue Result;
		Result.Name = Property->GetFName();
		if (const FObjectProperty* ObjectProperty = AsObjectReference(Property))
		{
			Result.bIsObjects = true;
			Result.Objects.Add(ObjectProperty->GetObjectPropertyValue(Value));
		}
		else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property); ArrayProperty && AsObjectReference(ArrayProperty->Inner))
		{
			Result.bIsObjects = true;
			FScriptArrayHelper Array(ArrayProperty, Value);
			const FObjectProperty* Inner = AsObjectReference(ArrayProperty->Inner);
			for (int32 Index = 0; Index < Array.Num(); ++Index)
			{
				Result.Objects.Add(Inner->GetObjectPropertyValue(Array.GetRawPtr(Index)));
			}
		}
		else
		{
			Property->ExportTextItem_Direct(Result.Text, Value, nullptr, nullptr, PPF_None);
		}
		return Result;
	}

	void ImportValue(const FProperty* Property, void* Value, const FAreaActionsNetValue& Source)
	{
		if (const FObjectProperty* ObjectProperty = AsObjectReference(Property))
		{
			UObject* Object = Source.Objects.Num() > 0 ? Source.Objects[0].Get() : nullptr;
			if (!Object || Object->IsA(ObjectProperty->PropertyClass))
			{
				ObjectProperty->SetObjectPropertyValue(Value, Object);
			}
		}
		else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property); ArrayProperty && AsObjectReference(ArrayProperty->Inner))
		{
			const FObjectProperty* Inner = AsObjectReference(ArrayProperty->Inner);
			FScriptArrayHelper Array(ArrayProperty, Value);
			Array.EmptyValues();
			for (UObject* Object : Source.Objects)
			{
				// Objects the receiver does not know (not replicated, out of reach) arrive as null; drop them.
				if (Object && Object->IsA(Inner->PropertyClass))
				{
					Inner->SetObjectPropertyValue(Array.GetRawPtr(Array.AddValue()), Object);
				}
			}
		}
		else if (!Source.bIsObjects)
		{
			if (!Property->ImportText_Direct(*Source.Text, Value, nullptr, PPF_None))
			{
				UE_LOG(LogAreaActions, Warning, TEXT("Network: could not read %s from '%s'"), *Property->GetName(), *Source.Text);
			}
		}
	}

	template <typename T>
	T* GetObject(const UObject* Owner, FName Name)
	{
		const FObjectPropertyBase* Property = Owner ? CastField<FObjectPropertyBase>(Owner->GetClass()->FindPropertyByName(Name)) : nullptr;
		return Property ? Cast<T>(Property->GetObjectPropertyValue_InContainer(Owner)) : nullptr;
	}

	void SetObject(UObject* Owner, FName Name, UObject* Value)
	{
		if (const FObjectPropertyBase* Property = CastField<FObjectPropertyBase>(Owner->GetClass()->FindPropertyByName(Name)))
		{
			if (!Value || Value->IsA(Property->PropertyClass))
			{
				Property->SetObjectPropertyValue_InContainer(Owner, Value);
			}
		}
	}

	bool GetBool(const UObject* Owner, FName Name)
	{
		const FBoolProperty* Property = CastField<FBoolProperty>(Owner->GetClass()->FindPropertyByName(Name));
		return Property && Property->GetPropertyValue_InContainer(Owner);
	}

	TArray<FName> GetNames(const UObject* Owner, FName Name)
	{
		TArray<FName> Result;
		const FArrayProperty* Property = CastField<FArrayProperty>(Owner->GetClass()->FindPropertyByName(Name));
		if (Property && CastField<FNameProperty>(Property->Inner))
		{
			Result = *Property->ContainerPtrToValuePtr<TArray<FName>>(Owner);
		}
		return Result;
	}

	TArray<FVector>* GetVectorArray(UObject* Owner, FName Name)
	{
		const FArrayProperty* Property = CastField<FArrayProperty>(Owner->GetClass()->FindPropertyByName(Name));
		const FStructProperty* Inner = Property ? CastField<FStructProperty>(Property->Inner) : nullptr;
		return Inner && Inner->Struct == TBaseStructure<FVector>::Get() ? Property->ContainerPtrToValuePtr<TArray<FVector>>(Owner) : nullptr;
	}

	double GetNumber(const UObject* Owner, FName Name)
	{
		const FNumericProperty* Property = CastField<FNumericProperty>(Owner->GetClass()->FindPropertyByName(Name));
		return Property ? Property->GetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Owner)) : 0.0;
	}

	void SetNumber(UObject* Owner, FName Name, double Value)
	{
		if (const FNumericProperty* Property = CastField<FNumericProperty>(Owner->GetClass()->FindPropertyByName(Name)))
		{
			Property->SetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Owner), Value);
		}
	}

	/** Calls a Blueprint function, filling its parameters by name from Arguments (or the first object parameter from FirstObject). */
	void CallBlueprintFunction(UObject* Target, UFunction* Function, const TArray<FAreaActionsNetValue>& Arguments, UObject* FirstObject = nullptr)
	{
		uint8* Params = static_cast<uint8*>(FMemory::Malloc(FMath::Max<int32>(Function->ParmsSize, 1), Function->GetMinAlignment()));
		FMemory::Memzero(Params, Function->ParmsSize);
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Params);
		}
		bool bFirstObjectSet = false;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			if (FirstObject && !bFirstObjectSet)
			{
				if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(*It); ObjectProperty && FirstObject->IsA(ObjectProperty->PropertyClass))
				{
					ObjectProperty->SetObjectPropertyValue_InContainer(Params, FirstObject);
					bFirstObjectSet = true;
				}
				continue;
			}
			if (const FAreaActionsNetValue* Argument = Arguments.FindByPredicate([&](const FAreaActionsNetValue& Value) { return Value.Name == It->GetFName(); }))
			{
				ImportValue(*It, It->ContainerPtrToValuePtr<void>(Params), *Argument);
			}
		}
		Target->ProcessEvent(Function, Params);
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Params);
		}
		FMemory::Free(Params);
	}

	void CallBlueprintFunction(UObject* Target, FName Name, UObject* FirstObject = nullptr)
	{
		if (UFunction* Function = Target ? Target->FindFunction(Name) : nullptr)
		{
			CallBlueprintFunction(Target, Function, {}, FirstObject);
		}
		else
		{
			UE_LOG(LogAreaActions, Warning, TEXT("Network: %s has no function %s"), *GetNameSafe(Target), *Name.ToString());
		}
	}

	FMulticastDelegateProperty* FindDelegate(const UObject* Owner, FName Name)
	{
		return Owner ? CastField<FMulticastDelegateProperty>(Owner->GetClass()->FindPropertyByName(Name)) : nullptr;
	}

	APlayerController* GetPlayerController(const AActor* Equip)
	{
		const APawn* Pawn = Equip ? Equip->GetInstigator() : nullptr;
		return Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	}
}

void UAreaActionsWidgetRelay::Fire()
{
	if (Component.IsValid())
	{
		Component->ServerWidgetEvent(WidgetId, DelegateName);
	}
}

UAreaActionsNetComponent::UAreaActionsNetComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

UAreaActionsNetComponent* UAreaActionsNetComponent::FindFor(const AActor* Equip)
{
	UAreaActionsNetComponent* Component = Equip ? Equip->FindComponentByClass<UAreaActionsNetComponent>() : nullptr;
	if (!Component && Equip)
	{
		UE_LOG(LogAreaActions, Error, TEXT("Network: %s has no AreaActionsNetComponent"), *Equip->GetName());
	}
	return Component;
}

bool UAreaActionsNetComponent::IsLocalTool(const AActor* Equip)
{
	const APawn* Pawn = Equip->GetInstigator();
	return Pawn ? Pawn->IsLocallyControlled() : Equip->HasAuthority();
}

bool UAreaActionsNetComponent::ForwardRunAction(AActor* Equip, AActor* Action)
{
	if (!Equip || !Action || Equip->HasAuthority() || GetBool(Action, NAME_RunsOnClient))
	{
		return false;
	}
	UAreaActionsNetComponent* Component = FindFor(Equip);
	if (!Component)
	{
		return false;
	}
	Component->SendStep(Action, NAME_RunAction, {});
	return true;
}

DEFINE_FUNCTION(UAreaActionsNetComponent::execForwardCallToServer)
{
	P_GET_OBJECT(AActor, Action);
	P_FINISH;
	P_NATIVE_BEGIN;
	// A native call runs in the frame of the Blueprint function that makes it: Node is that function and Locals
	// holds its parameters, which is what lets the call be repeated on the server.
	*static_cast<bool*>(RESULT_PARAM) = ForwardCall(Action, Stack.Node, Stack.Locals);
	P_NATIVE_END;
}

bool UAreaActionsNetComponent::ForwardCall(AActor* Action, const UFunction* Function, const void* Locals)
{
	AActor* Equip = GetObject<AActor>(Action, NAME_AreaActionsEquip);
	if (!Equip || Equip->HasAuthority() || !Function || !Locals)
	{
		return false;
	}
	UAreaActionsNetComponent* Component = FindFor(Equip);
	if (!Component)
	{
		return false;
	}
	TArray<FAreaActionsNetValue> Arguments;
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		const bool bOutput = It->HasAnyPropertyFlags(CPF_ReturnParm) || (It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReferenceParm));
		if (!bOutput)
		{
			Arguments.Add(ExportValue(*It, It->ContainerPtrToValuePtr<void>(const_cast<void*>(Locals))));
		}
	}
	Component->SendStep(Action, Function->GetFName(), MoveTemp(Arguments));
	return true;
}

void UAreaActionsNetComponent::SendStep(AActor* Action, FName Step, TArray<FAreaActionsNetValue>&& Arguments)
{
	TrackLocalAction(Action);

	TArray<FAreaActionsNetValue> Properties;
	for (const FName& Name : GetNames(Action, NAME_ServerSyncedProperties))
	{
		if (const FProperty* Property = Action->GetClass()->FindPropertyByName(Name))
		{
			Properties.Add(ExportValue(Property, Property->ContainerPtrToValuePtr<void>(Action)));
		}
		else
		{
			UE_LOG(LogAreaActions, Warning, TEXT("Network: %s lists unknown property %s"), *Action->GetClass()->GetName(), *Name.ToString());
		}
	}

	AActor* Equip = GetOwner();
	const TArray<FVector>* Corners = GetVectorArray(Equip, NAME_CornerLocations);
	UE_LOG(LogAreaActions, Log, TEXT("Network: sending %s.%s to the server"), *Action->GetClass()->GetName(), *Step.ToString());
	ServerRunStep(Action->GetClass(), Step, Corners ? *Corners : TArray<FVector>(), GetNumber(Equip, NAME_MinZ), GetNumber(Equip, NAME_MaxZ), Properties, Arguments);
}

void UAreaActionsNetComponent::TrackLocalAction(AActor* Action)
{
	if (LocalAction.Get() == Action)
	{
		return;
	}
	LocalAction = Action;
	if (FMulticastDelegateProperty* Done = FindDelegate(Action, NAME_Done))
	{
		FScriptDelegate Delegate;
		Delegate.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UAreaActionsNetComponent, OnLocalActionDone));
		Done->AddDelegate(MoveTemp(Delegate), Action);
	}
}

void UAreaActionsNetComponent::OnLocalActionDone(AActor* Action)
{
	if (Action && Action == LocalAction.Get())
	{
		LocalAction = nullptr;
		ServerEndAction();
	}
}

void UAreaActionsNetComponent::ServerRunStep_Implementation(UClass* ActionClass, FName Step, const TArray<FVector>& Corners, double MinZ, double MaxZ,
	const TArray<FAreaActionsNetValue>& Properties, const TArray<FAreaActionsNetValue>& Arguments)
{
	AActor* Equip = GetOwner();
	if (!Equip || !ActionClass || !ActionClass->IsChildOf(AActor::StaticClass()) || !ActionClass->FindPropertyByName(NAME_AreaActionsEquip))
	{
		UE_LOG(LogAreaActions, Warning, TEXT("Network: rejected step %s of %s"), *Step.ToString(), *GetNameSafe(ActionClass));
		return;
	}
	UE_LOG(LogAreaActions, Log, TEXT("Network: %s runs %s.%s"), *GetNameSafe(GetPlayerController(Equip)), *ActionClass->GetName(), *Step.ToString());

	// The tool's area on the server is only used for the client's actions, so it can simply be overwritten.
	if (TArray<FVector>* EquipCorners = GetVectorArray(Equip, NAME_CornerLocations))
	{
		*EquipCorners = Corners;
	}
	SetNumber(Equip, NAME_MinZ, MinZ);
	SetNumber(Equip, NAME_MaxZ, MaxZ);

	if (Step == NAME_RunAction || !Mirror || Mirror->GetClass() != ActionClass)
	{
		DestroyMirror();
		Mirror = SpawnMirror(ActionClass);
		if (!Mirror)
		{
			return;
		}
	}

	// Only what the action itself lists may be set by the client.
	const TArray<FName> Synced = GetNames(Mirror, NAME_ServerSyncedProperties);
	for (const FAreaActionsNetValue& Value : Properties)
	{
		const FProperty* Property = Synced.Contains(Value.Name) ? Mirror->GetClass()->FindPropertyByName(Value.Name) : nullptr;
		if (Property)
		{
			ImportValue(Property, Property->ContainerPtrToValuePtr<void>(Mirror), Value);
		}
	}

	if (Step == NAME_RunAction)
	{
		// Gathers the buildings in the area on the server, then runs the action.
		CallBlueprintFunction(Equip, NAME_RunAction, Mirror);
		return;
	}
	UFunction* Function = Mirror->FindFunction(Step);
	if (!Function || !Cast<UBlueprintGeneratedClass>(Function->GetOuter()) || Function->HasAnyFunctionFlags(FUNC_Net | FUNC_Delegate | FUNC_Static))
	{
		UE_LOG(LogAreaActions, Warning, TEXT("Network: %s has no Blueprint function %s"), *ActionClass->GetName(), *Step.ToString());
		return;
	}
	CallBlueprintFunction(Mirror, Function, Arguments);
}

AActor* UAreaActionsNetComponent::SpawnMirror(UClass* ActionClass)
{
	AActor* Equip = GetOwner();
	APawn* Pawn = Equip->GetInstigator();
	APlayerController* Controller = GetPlayerController(Equip);
	const FTransform Transform = Pawn ? FTransform(Pawn->GetActorLocation()) : FTransform::Identity;

	AActor* Action = GetWorld()->SpawnActorDeferred<AActor>(ActionClass, Transform, Controller, Pawn, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Action)
	{
		return nullptr;
	}
	// The client has its own instance; the mirror is the server's alone.
	Action->SetReplicates(false);
	SetObject(Action, NAME_Player, Controller);
	SetObject(Action, NAME_AreaActionsEquip, Equip);
	Action->FinishSpawning(Transform);

	if (FMulticastDelegateProperty* Done = FindDelegate(Action, NAME_Done))
	{
		FScriptDelegate Delegate;
		Delegate.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UAreaActionsNetComponent, OnMirrorDone));
		Done->AddDelegate(MoveTemp(Delegate), Action);
	}
	return Action;
}

void UAreaActionsNetComponent::OnMirrorDone(AActor* Action)
{
	if (!Action || Action != Mirror)
	{
		return;
	}
	Mirror = nullptr;
	ClientMirrorDone();
	// Other handlers of Done (e.g. the action's own cleanup) still run, so destroy it afterwards.
	TWeakObjectPtr<AActor> WeakAction = Action;
	GetWorld()->GetTimerManager().SetTimerForNextTick([WeakAction]
	{
		if (WeakAction.IsValid())
		{
			WeakAction->Destroy();
		}
	});
}

void UAreaActionsNetComponent::DestroyMirror()
{
	AActor* Action = Mirror;
	if (!Action)
	{
		return;
	}
	Mirror = nullptr;
	if (FMulticastDelegateProperty* Done = FindDelegate(Action, NAME_Done))
	{
		FScriptDelegate Delegate;
		Delegate.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UAreaActionsNetComponent, OnMirrorDone));
		Done->RemoveDelegate(Delegate, Action);
	}
	CallBlueprintFunction(Action, NAME_Finished);
	Action->Destroy();
}

void UAreaActionsNetComponent::ServerEndAction_Implementation()
{
	DestroyMirror();
}

void UAreaActionsNetComponent::ClientMirrorDone_Implementation()
{
	if (AActor* Action = LocalAction.Get())
	{
		// Cleared first, so finishing it does not report back to the server.
		LocalAction = nullptr;
		CallBlueprintFunction(Action, NAME_Finished);
	}
}

bool UAreaActionsNetComponent::ForwardWidgetToOwner(AActor* Equip, UUserWidget* Widget)
{
	if (!Equip || IsLocalTool(Equip))
	{
		return false;
	}
	if (!Equip->HasAuthority())
	{
		// Another player's tool replicated to this client: its windows are not ours to show.
		return true;
	}
	UAreaActionsNetComponent* Component = FindFor(Equip);
	if (!Component || !Widget)
	{
		UE_LOG(LogAreaActions, Warning, TEXT("Network: could not forward window %s"), *GetNameSafe(Widget));
		return true;
	}

	TArray<FAreaActionsNetValue> Properties;
	TArray<FName> Delegates;
	for (TFieldIterator<FProperty> It(Widget->GetClass()); It; ++It)
	{
		if (!IsDeclaredInBlueprint(*It))
		{
			continue;
		}
		if (CastField<FMulticastDelegateProperty>(*It))
		{
			Delegates.Add(It->GetFName());
		}
		// Only the settings of the window (Title, Message, ...); never references into its widget tree.
		else if (It->HasAnyPropertyFlags(CPF_Edit) && !AsObjectReference(*It))
		{
			Properties.Add(ExportValue(*It, It->ContainerPtrToValuePtr<void>(Widget)));
		}
	}
	const int32 WidgetId = Component->NextWidgetId++;
	Component->ForwardedWidgets.Add(WidgetId, Widget);
	UE_LOG(LogAreaActions, Log, TEXT("Network: showing %s on the client (%d)"), *Widget->GetClass()->GetName(), WidgetId);
	Component->ClientShowWidget(WidgetId, Widget->GetClass(), Properties, Delegates);
	return true;
}

void UAreaActionsNetComponent::ClientShowWidget_Implementation(int32 WidgetId, UClass* WidgetClass, const TArray<FAreaActionsNetValue>& Properties, const TArray<FName>& Delegates)
{
	AActor* Equip = GetOwner();
	APlayerController* Controller = GetPlayerController(Equip);
	UUserWidget* Widget = Controller && WidgetClass && WidgetClass->IsChildOf(UUserWidget::StaticClass())
		? CreateWidget<UUserWidget>(Controller, WidgetClass) : nullptr;
	if (!Widget)
	{
		UE_LOG(LogAreaActions, Warning, TEXT("Network: could not show %s"), *GetNameSafe(WidgetClass));
		return;
	}
	for (const FAreaActionsNetValue& Value : Properties)
	{
		const FProperty* Property = WidgetClass->FindPropertyByName(Value.Name);
		if (Property && IsDeclaredInBlueprint(Property) && Property->HasAnyPropertyFlags(CPF_Edit))
		{
			ImportValue(Property, Property->ContainerPtrToValuePtr<void>(Widget), Value);
		}
	}
	SetObject(Widget, NAME_InteractObject, Equip);

	Relays.RemoveAll([](const UAreaActionsWidgetRelay* Relay) { return !Relay || !Relay->Widget.IsValid(); });
	for (const FName& DelegateName : Delegates)
	{
		if (FMulticastDelegateProperty* Property = FindDelegate(Widget, DelegateName))
		{
			UAreaActionsWidgetRelay* Relay = NewObject<UAreaActionsWidgetRelay>(this);
			Relay->Component = this;
			Relay->Widget = Widget;
			Relay->WidgetId = WidgetId;
			Relay->DelegateName = DelegateName;
			FScriptDelegate Delegate;
			Delegate.BindUFunction(Relay, GET_FUNCTION_NAME_CHECKED(UAreaActionsWidgetRelay, Fire));
			Property->AddDelegate(MoveTemp(Delegate), Widget);
			Relays.Add(Relay);
		}
	}
	CallBlueprintFunction(Equip, NAME_AddWidget, Widget);
}

void UAreaActionsNetComponent::ServerWidgetEvent_Implementation(int32 WidgetId, FName DelegateName)
{
	TObjectPtr<UUserWidget> Widget;
	if (!ForwardedWidgets.RemoveAndCopyValue(WidgetId, Widget) || !Widget)
	{
		return;
	}
	const FMulticastDelegateProperty* Property = FindDelegate(Widget, DelegateName);
	if (!Property || !IsDeclaredInBlueprint(Property))
	{
		return;
	}
	UE_LOG(LogAreaActions, Log, TEXT("Network: client answered %s (%d) with %s"), *Widget->GetClass()->GetName(), WidgetId, *DelegateName.ToString());
	const UFunction* Signature = Property->SignatureFunction;
	const int32 ParamsSize = Signature ? Signature->ParmsSize : 0;
	uint8* Params = static_cast<uint8*>(FMemory::Malloc(FMath::Max(ParamsSize, 1)));
	FMemory::Memzero(Params, FMath::Max(ParamsSize, 1));
	if (const FMulticastScriptDelegate* Delegate = Property->GetMulticastDelegate(Property->ContainerPtrToValuePtr<void>(Widget.Get())))
	{
		Delegate->ProcessMulticastDelegate<UObject>(Params);
	}
	FMemory::Free(Params);
}

void UAreaActionsNetComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		DestroyMirror();
	}
	ForwardedWidgets.Empty();
	Relays.Empty();
	Super::EndPlay(EndPlayReason);
}
