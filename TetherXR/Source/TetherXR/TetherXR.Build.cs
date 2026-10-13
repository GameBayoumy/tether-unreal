// Copyright (c) 2026 Tether contributors. MIT License.

using System.IO;
using UnrealBuildTool;

public class TetherXR : ModuleRules
{
	public TetherXR(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Tether's C API (tether_direct.h and the structs it shares with the app).
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "ThirdParty", "Tether"));

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ApplicationCore",
			"InputCore",
			"InputDevice",
			"HeadMountedDisplay",
			"XRBase",
			"RHI",
			"RenderCore",
			"Renderer",
			"Slate",
			"SlateCore",
			"Json",
		});

		// UE 5.8's Metal RHI only hands out its command queue through IMetalDynamicRHI (metal-cpp).
		bool bModernMetalRHI = Target.Version.MajorVersion > 5 || Target.Version.MinorVersion >= 8;
		if (bModernMetalRHI)
		{
			PrivateDependencyModuleNames.Add("MetalRHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "MetalCPP");
		}
	}
}
