#include "TextureProviderBridge.h"

#include <PCH.h>

bool TextureProviderBridge::Resolve()
{
	if (provider_) {
		return true;
	}

	const auto module = ::GetModuleHandleW(L"TextureDownscaler.dll");
	if (!module) {
		status_ = "TextureDownscaler.dll not loaded";
		return false;
	}

	const auto getProvider = reinterpret_cast<SolFGTextureProviderAPI::GetProviderFn>(
		::GetProcAddress(module, "TextureDownscaler_GetSolFGProvider"));
	if (!getProvider) {
		status_ = "provider loaded without the TheosRenderPipeline control API";
		return false;
	}

	const auto candidate = getProvider(SolFGTextureProviderAPI::kVersion1);
	if (!candidate || candidate->structSize < sizeof(SolFGTextureProviderAPI::ProviderV1) ||
		candidate->apiVersion != SolFGTextureProviderAPI::kVersion1 ||
		!candidate->getSettings || !candidate->applySettings || !candidate->saveSettings ||
		!candidate->reloadSettings || !candidate->getTelemetry) {
		status_ = "provider rejected API v1 or returned an incomplete table";
		return false;
	}

	provider_ = candidate;
	status_ = "TextureDownscaler 2.3.3 API v1 connected";
	logger::info("[TextureProvider] {}", status_);
	return true;
}

bool TextureProviderBridge::Read(Settings& a_settings, Telemetry* a_telemetry)
{
	if (!Resolve()) {
		return false;
	}

	SolFGTextureProviderAPI::SettingsV1 rawSettings{};
	if (!provider_->getSettings(&rawSettings)) {
		status_ = "provider settings read failed";
		return false;
	}

	a_settings.enabled = rawSettings.enabled != 0;
	for (std::size_t i = 0; i < a_settings.maxSize.size(); ++i) {
		a_settings.maxSize[i] = rawSettings.maxSize[i];
	}

	if (a_telemetry) {
		SolFGTextureProviderAPI::TelemetryV1 rawTelemetry{};
		if (!provider_->getTelemetry(&rawTelemetry)) {
			status_ = "provider telemetry read failed";
			return false;
		}
		a_telemetry->hooksInstalled = rawTelemetry.hooksInstalled != 0;
		a_telemetry->reducedTextures = rawTelemetry.reducedTextures;
		a_telemetry->estimatedBytesAvoided = rawTelemetry.estimatedBytesAvoided;
		a_telemetry->namesResolved = rawTelemetry.namesResolved;
		a_telemetry->nameLookups = rawTelemetry.nameLookups;
	}

	return true;
}

bool TextureProviderBridge::Apply(const Settings& a_settings, bool a_save)
{
	if (!Resolve()) {
		return false;
	}

	SolFGTextureProviderAPI::SettingsV1 raw{};
	raw.enabled = a_settings.enabled ? 1 : 0;
	for (std::size_t i = 0; i < a_settings.maxSize.size(); ++i) {
		raw.maxSize[i] = a_settings.maxSize[i];
	}

	if (!provider_->applySettings(&raw)) {
		status_ = "provider rejected staged texture settings";
		return false;
	}
	if (a_save && !provider_->saveSettings()) {
		status_ = "texture settings are active but TextureDownscaler.ini could not be written";
		return false;
	}

	status_ = a_save ? "texture settings applied and saved" : "texture settings applied for this session";
	logger::info("[TextureProvider] {}", status_);
	return true;
}
