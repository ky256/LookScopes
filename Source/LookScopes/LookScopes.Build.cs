// Copyright KuoYu. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class LookScopes : ModuleRules
{
	public LookScopes(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
		PrivateIncludePaths.Add(Path.Combine(EngineDir, "Source", "Runtime", "Renderer", "Internal"));

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
				"WorkspaceMenuStructure",
				"MediaIOCore",
				"NNE"
			}
		);
	}
}
