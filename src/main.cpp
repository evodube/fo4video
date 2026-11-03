#include "Manager.h"
#include "Media.h"
#include "Papyrus.h"
#include "Remote.h"
#include "Renderer.h"
#include "Settings.h"

namespace
{
	constexpr auto kName = "RealVideos";

	void OnMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case F4SE::MessagingInterface::kPostPostLoad:
			fv::Remote::Register();
			break;
		case F4SE::MessagingInterface::kGameDataReady:
			fv::log::info("game data ready");
			fv::Settings::Get().Load();
			fv::MediaSource::StartupMF();
			fv::Renderer::Install();
			break;
		case F4SE::MessagingInterface::kPreLoadGame:
			fv::Manager::Get().StopAll();
			break;
		case F4SE::MessagingInterface::kPostLoadGame:
		case F4SE::MessagingInterface::kNewGame:
			fv::Renderer::Install();  // no-op if already hooked
			break;
		default:
			break;
		}
	}
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = kName;
	a_info->version = 1;

	if (a_f4se->IsEditor()) {
		return false;
	}
	return a_f4se->RuntimeVersion() >= F4SE::RUNTIME_1_10_163;
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se, { .log = true, .logName = kName, .trampoline = false });

	fv::log::info("Real Videos ({}) 0.56.0 loading on runtime {} ({})", kName, a_f4se->RuntimeVersion().string(),
		REX::FModule::IsRuntimeOG() ? "OG" : REX::FModule::IsRuntimeNG() ? "NG" : "AE");
	if (!REX::FModule::IsRuntimeOG()) {
		fv::log::warn("game {}: supported, but tested on 1.10.163 so far; please report problems with RealVideos.log",
			a_f4se->RuntimeVersion().string());
	}

	auto* messaging = F4SE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnMessage)) {
		fv::log::error("could not register F4SE message listener");
		return false;
	}

	// remember what the screens were playing in the save game
	if (auto* ser = F4SE::GetSerializationInterface()) {
		ser->SetUniqueID('RVID');
		ser->SetSaveCallback([](const F4SE::SerializationInterface* a_intfc) { fv::Manager::Get().SaveScreens(a_intfc); });
		ser->SetLoadCallback([](const F4SE::SerializationInterface* a_intfc) { fv::Manager::Get().LoadScreens(a_intfc); });
		ser->SetRevertCallback([](const F4SE::SerializationInterface*) { fv::Manager::Get().ForgetScreens(); });
	}

	if (auto* papyrus = F4SE::GetPapyrusInterface()) {
		papyrus->Register(fv::Papyrus::Register);
	}

	fv::log::info("loaded; waiting for game data");
	return true;
}
