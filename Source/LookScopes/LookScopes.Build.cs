// Copyright KuoYu. All Rights Reserved.

using UnrealBuildTool;

public class LookScopes : ModuleRules
{
	public LookScopes(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"RenderCore",
				"RHI",
				"Renderer",
				"Projects",
				"RHICore"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"UnrealEd",
				"InputCore",
				"EditorSubsystem",
				"ToolMenus",
				"EditorFramework",
				"WorkspaceMenuStructure"
			}
		);
	}
}
