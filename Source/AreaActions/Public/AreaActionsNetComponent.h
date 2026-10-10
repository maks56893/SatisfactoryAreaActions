#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AreaActionsNetComponent.generated.h"

class UUserWidget;

/** One value sent over the network: a property of an action, or a parameter of an action function. */
USTRUCT()
struct FAreaActionsNetValue
{
	GENERATED_BODY()

	UPROPERTY()
	FName Name;

	/** Exported text of the value; unused for object references. */
	UPROPERTY()
	FString Text;

	/** Object references (a single object or an array of them). Only networked objects, e.g. buildings, arrive. */
	UPROPERTY()
	TArray<TObjectPtr<UObject>> Objects;

	UPROPERTY()
	bool bIsObjects = false;
};

/** Forwards one delegate of a widget shown on a client back to the widget the server created. */
UCLASS(Transient)
class UAreaActionsWidgetRelay : public UObject
{
	GENERATED_BODY()
public:
	UFUNCTION()
	void Fire();

	TWeakObjectPtr<class UAreaActionsNetComponent> Component;
	TWeakObjectPtr<UUserWidget> Widget;
	int32 WidgetId = 0;
	FName DelegateName;
};

/**
 * Multiplayer support for the Area Actions tool. Lives on Equip_AreaActions.
 *
 * Actions always run on the machine of the player who uses the tool, because that is where their windows and
 * previews are. On a client, the steps that change the world are not run locally but sent to the server, which runs
 * them on a "mirror" of the action: a server-side instance of the same class, set up with the client's area and
 * choices. Which steps those are is decided in Blueprint:
 *  - Equip_AreaActions.RunAction calls ForwardRunAction: the server gathers the buildings in the area and runs the
 *    action's Run there (unless the action sets RunsOnClient, like Copy and Fill, which run Run themselves).
 *  - Any action function can start with ForwardCallToServer (e.g. Copy.Preview); the call, with its parameters, is
 *    then made on the mirror instead.
 * Before every step the mirror receives the client action's variables listed in its ServerSyncedProperties.
 *
 * Windows the mirror opens (results, confirmations) go through Equip_AreaActions.AddWidget, which calls
 * ForwardWidgetToOwner: the window is shown on the client instead of the host's screen, and its buttons are sent back.
 * When the mirror finishes, the client's action finishes too, and the other way around.
 */
UCLASS(ClassGroup = (AreaActions), meta = (BlueprintSpawnableComponent))
class AREAACTIONS_API UAreaActionsNetComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UAreaActionsNetComponent();

	/**
	 * Called at the start of Equip_AreaActions.RunAction. On a client, sends the action to the server, which gathers
	 * the buildings in the area and runs it there. Returns true if the caller must stop (the server runs the action).
	 */
	UFUNCTION(BlueprintCallable, Category = "Area Actions|Network", meta = (DefaultToSelf = "Equip"))
	static bool ForwardRunAction(AActor* Equip, AActor* Action);

	/**
	 * Placed at the start of an action function. On a client, makes the same call, with the same parameters, on the
	 * server's mirror of the action. Returns true if the caller must stop (the server runs the function).
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Area Actions|Network", meta = (DefaultToSelf = "Action"))
	static bool ForwardCallToServer(AActor* Action);
	DECLARE_FUNCTION(execForwardCallToServer);

	/**
	 * Called at the start of Equip_AreaActions.AddWidget. On the server, for another player's tool, shows the window
	 * on that player's screen instead. Returns true if the caller must stop (the window was forwarded or dropped).
	 */
	UFUNCTION(BlueprintCallable, Category = "Area Actions|Network", meta = (DefaultToSelf = "Equip"))
	static bool ForwardWidgetToOwner(AActor* Equip, UUserWidget* Widget);

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION(Server, Reliable)
	void ServerRunStep(UClass* ActionClass, FName Step, const TArray<FVector>& Corners, double MinZ, double MaxZ,
		const TArray<FAreaActionsNetValue>& Properties, const TArray<FAreaActionsNetValue>& Arguments);

	UFUNCTION(Server, Reliable)
	void ServerEndAction();

	UFUNCTION(Server, Reliable)
	void ServerWidgetEvent(int32 WidgetId, FName DelegateName);

	UFUNCTION(Client, Reliable)
	void ClientShowWidget(int32 WidgetId, UClass* WidgetClass, const TArray<FAreaActionsNetValue>& Properties, const TArray<FName>& Delegates);

	UFUNCTION(Client, Reliable)
	void ClientMirrorDone();

private:
	static UAreaActionsNetComponent* FindFor(const AActor* Equip);
	static bool IsLocalTool(const AActor* Equip);
	static bool ForwardCall(AActor* Action, const UFunction* Function, const void* Locals);

	void SendStep(AActor* Action, FName Step, TArray<FAreaActionsNetValue>&& Arguments);
	void TrackLocalAction(AActor* Action);
	AActor* SpawnMirror(UClass* ActionClass);
	/** Finishes the mirror (so it can clean up, e.g. remove a preview) and destroys it. */
	void DestroyMirror();

	UFUNCTION()
	void OnMirrorDone(AActor* Action);

	UFUNCTION()
	void OnLocalActionDone(AActor* Action);

	/** Server: the mirror of the action this player is running. */
	UPROPERTY(Transient)
	TObjectPtr<AActor> Mirror;

	/** Server: windows shown on the client, by id, waiting for a button. */
	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<UUserWidget>> ForwardedWidgets;
	int32 NextWidgetId = 1;

	/** Client: the action whose steps run on the server. */
	TWeakObjectPtr<AActor> LocalAction;

	/** Client: relays bound to the windows the server forwarded. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAreaActionsWidgetRelay>> Relays;
};
