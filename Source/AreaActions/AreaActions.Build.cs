using System.IO;
using UnrealBuildTool;

public class AreaActions : ModuleRules
{
	public AreaActions(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// FactoryGame transitive dependencies (subset of the Alpakit CPPAndBlueprintBlank template)
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject",
			"Engine",
			"DeveloperSettings",
			"PhysicsCore",
			"InputCore",
			"GeometryCollectionEngine",
			"AnimGraphRuntime",
			"AssetRegistry",
			"NavigationSystem",
			"AIModule",
			"GameplayTasks",
			"SlateCore", "Slate", "UMG",
			"RenderCore",
			"CinematicCamera",
			"Foliage",
			"NetCore",
			"GameplayTags",
			"Json", "JsonUtilities"
		});

		// FactoryGame plugins: the lightweight buildable system is built on top of AbstractInstance
		PublicDependencyModuleNames.AddRange(new string[] {
			"AbstractInstance",
		});

		// Header stubs
		PublicDependencyModuleNames.AddRange(new string[] {
			"DummyHeaders",
		});

		if (Target.Type == TargetRules.TargetType.Editor) {
			PublicDependencyModuleNames.AddRange(new string[] {"AnimGraph"});
		}
		PublicDependencyModuleNames.AddRange(new string[] {"FactoryGame", "SML"});

		// Lets Private/Tests include the private geometry header
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));
	}
}
