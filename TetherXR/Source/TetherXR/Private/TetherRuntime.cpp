// Copyright (c) 2026 Tether contributors. MIT License.

#include "TetherRuntime.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// Tether keeps this manifest current; "runtime.library_path" points at the bundled runtime.
	const TCHAR* ManifestPath = TEXT("Library/Application Support/Tether/openxr_runtime.json");
	const TCHAR* FallbackLibrary = TEXT("/Applications/Tether.app/Contents/Resources/openxr/libTetherOpenXR.dylib");
}

FTetherRuntime& FTetherRuntime::Get()
{
	static FTetherRuntime Runtime;
	return Runtime;
}

FString FTetherRuntime::FindLibrary()
{
	const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	const FString Manifest = FPaths::Combine(Home, ManifestPath);

	FString Json;
	if (!Home.IsEmpty() && FFileHelper::LoadFileToString(Json, *Manifest))
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		const TSharedPtr<FJsonObject>* Runtime = nullptr;
		FString Path;
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid() &&
			Root->TryGetObjectField(TEXT("runtime"), Runtime) && (*Runtime)->TryGetStringField(TEXT("library_path"), Path) &&
			FPaths::FileExists(Path))
		{
			return Path;
		}
		UE_LOG(LogTetherXR, Log, TEXT("%s has no usable runtime.library_path"), *Manifest);
	}

	return FPaths::FileExists(FallbackLibrary) ? FString(FallbackLibrary) : FString();
}

bool FTetherRuntime::Load()
{
	if (bAttempted)
	{
		return IsLoaded();
	}
	bAttempted = true;

	LibraryPath = FindLibrary();
	if (LibraryPath.IsEmpty())
	{
		UE_LOG(LogTetherXR, Log, TEXT("Tether isn't installed (no runtime library found); TetherXR stays dormant"));
		return false;
	}

	Handle = FPlatformProcess::GetDllHandle(*LibraryPath);
	if (!Handle)
	{
		UE_LOG(LogTetherXR, Warning, TEXT("Couldn't load %s; TetherXR stays dormant"), *LibraryPath);
		return false;
	}

	auto Resolve = [this](const TCHAR* Name) { return FPlatformProcess::GetDllExport(Handle, Name); };
	ReadStateFn = reinterpret_cast<decltype(ReadStateFn)>(Resolve(TEXT("tether_direct_read_state")));
	CreateFn = reinterpret_cast<decltype(CreateFn)>(Resolve(TEXT("tether_direct_create")));
	SubmitFn = reinterpret_cast<decltype(SubmitFn)>(Resolve(TEXT("tether_direct_submit")));
	DestroyFn = reinterpret_cast<decltype(DestroyFn)>(Resolve(TEXT("tether_direct_destroy")));

	if (!ReadStateFn || !CreateFn || !SubmitFn || !DestroyFn)
	{
		UE_LOG(LogTetherXR, Warning, TEXT("%s doesn't export the tether_direct API (Tether too old?); TetherXR stays dormant"), *LibraryPath);
		Unload();
		return false;
	}

	UE_LOG(LogTetherXR, Log, TEXT("Loaded Tether runtime from %s"), *LibraryPath);
	return true;
}

void FTetherRuntime::Unload()
{
	ReadStateFn = nullptr;
	CreateFn = nullptr;
	SubmitFn = nullptr;
	DestroyFn = nullptr;
	// The library stays mapped: its Swift runtime and Metal objects can't be safely torn down mid-process.
	Handle = nullptr;
}

bool FTetherRuntime::ReadState(tether_xr_state& OutState) const
{
	return ReadStateFn && ReadStateFn(&OutState);
}

void* FTetherRuntime::CreateSubmitter(void* MetalCommandQueue) const
{
	return CreateFn && MetalCommandQueue ? CreateFn(MetalCommandQueue) : nullptr;
}

bool FTetherRuntime::Submit(void* Submitter, void* MetalTexture, const int32 LeftRect[4], const int32 RightRect[4], const qlh_pose& Pose) const
{
	return SubmitFn && Submitter && MetalTexture && SubmitFn(Submitter, MetalTexture, LeftRect, RightRect, &Pose);
}

void FTetherRuntime::DestroySubmitter(void* Submitter) const
{
	if (DestroyFn && Submitter)
	{
		DestroyFn(Submitter);
	}
}
