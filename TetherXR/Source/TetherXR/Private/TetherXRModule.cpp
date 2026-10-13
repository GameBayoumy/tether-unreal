// Copyright (c) 2026 Tether contributors. MIT License.

#include "Features/IModularFeatures.h"
#include "HAL/IConsoleManager.h"
#include "IHeadMountedDisplayModule.h"
#include "IInputDeviceModule.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "TetherRuntime.h"
#include "TetherXRHMD.h"
#include "TetherXRInput.h"

DEFINE_LOG_CATEGORY(LogTetherXR);

namespace
{
	/** Default [HMDPluginPriority] for TetherXR: above OpenXRHMD (20), OculusHMD (40) and SteamVR (30). */
	constexpr float DefaultPriority = 60.f;

	/** Creates the Touch controller device once the platform polls input devices. */
	class FTetherXRInputDeviceFactory : public IInputDeviceModule
	{
	public:
		virtual TSharedPtr<IInputDevice> CreateInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override
		{
			FTetherXRInput::RegisterKeys();
			return MakeShared<FTetherXRInput>(InMessageHandler);
		}
	};

	void LogStatus()
	{
		const FTetherRuntime& Runtime = FTetherRuntime::Get();
		if (!Runtime.IsLoaded())
		{
			UE_LOG(LogTetherXR, Display, TEXT("Status: dormant (Tether runtime not found)"));
			return;
		}
		tether_xr_state State;
		if (!Runtime.ReadState(State))
		{
			UE_LOG(LogTetherXR, Display, TEXT("Status: runtime %s loaded; Tether isn't running"), *Runtime.GetLibraryPath());
			return;
		}
		UE_LOG(LogTetherXR, Display, TEXT("Status: Tether running, %s; %ux%u per eye at %.0f Hz, IPD %.1f mm, FOV tangents L[%.3f %.3f %.3f %.3f] R[%.3f %.3f %.3f %.3f], controllers %s/%s, recenter count %u"),
			State.streaming ? TEXT("headset streaming") : TEXT("no Link session"),
			State.eye_width, State.eye_height, State.refresh_hz, State.ipd * 1000.f,
			State.fov_tangents[0][0], State.fov_tangents[0][1], State.fov_tangents[0][2], State.fov_tangents[0][3],
			State.fov_tangents[1][0], State.fov_tangents[1][1], State.fov_tangents[1][2], State.fov_tangents[1][3],
			State.controllers[0].valid ? TEXT("L") : TEXT("-"), State.controllers[1].valid ? TEXT("R") : TEXT("-"),
			State.recenter_count);
	}

	FAutoConsoleCommand StatusCommand(TEXT("TetherXR.Status"), TEXT("Logs whether Tether is installed, running and streaming."), FConsoleCommandDelegate::CreateStatic(&LogStatus));
}

class FTetherXRModule : public IHeadMountedDisplayModule
{
public:
	virtual void StartupModule() override
	{
		IHeadMountedDisplayModule::StartupModule();

		if (!FTetherRuntime::Get().Load())
		{
			return;
		}

		// Win HMD selection on Mac unless the project configured a priority itself.
		float Priority = 0.f;
		if (GConfig && !GConfig->GetFloat(TEXT("HMDPluginPriority"), *GetModuleKeyName(), Priority, GEngineIni))
		{
			GConfig->SetFloat(TEXT("HMDPluginPriority"), *GetModuleKeyName(), DefaultPriority, GEngineIni);
		}

		IModularFeatures::Get().RegisterModularFeature(IInputDeviceModule::GetModularFeatureName(), &InputDeviceFactory);
		bInputRegistered = true;
	}

	virtual void ShutdownModule() override
	{
		if (bInputRegistered)
		{
			IModularFeatures::Get().UnregisterModularFeature(IInputDeviceModule::GetModularFeatureName(), &InputDeviceFactory);
			bInputRegistered = false;
		}
		IHeadMountedDisplayModule::ShutdownModule();
	}

	virtual FString GetModuleKeyName() const override { return TEXT("TetherXR"); }
	virtual FString GetDeviceSystemName() override { return FTetherXRHMD::SystemName.ToString(); }

	/** "Connected" means Tether is installed: the headset can be started from Tether at any time. */
	virtual bool IsHMDConnected() override { return FTetherRuntime::Get().IsLoaded(); }

	virtual TSharedPtr<IXRTrackingSystem, ESPMode::ThreadSafe> CreateTrackingSystem() override
	{
		if (!FTetherRuntime::Get().IsLoaded())
		{
			return nullptr;
		}
		UE_LOG(LogTetherXR, Log, TEXT("Creating the Tether head-mounted display"));
		LogStatus();
		return MakeShared<FTetherXRHMD, ESPMode::ThreadSafe>();
	}

private:
	FTetherXRInputDeviceFactory InputDeviceFactory;
	bool bInputRegistered = false;
};

IMPLEMENT_MODULE(FTetherXRModule, TetherXR)
