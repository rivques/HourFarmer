#include "pch.h"
#include "HourFarmer.h"


BAKKESMOD_PLUGIN(HourFarmer, "HourFarmer", plugin_version, PLUGINTYPE_FREEPLAY)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

void HourFarmer::onLoad()
{
	_globalCvarManager = cvarManager;

	persistent_storage = std::make_shared<PersistentStorage>(this, "HourFarmer", true, true);
	CVarWrapper points_cvar = persistent_storage->RegisterPersistentCvar("hf_points", "0", "How many Hour Farmer points this player has");
	CVarWrapper points_accumulating_cvar = persistent_storage->RegisterPersistentCvar("hf_now_accumulating", "1", "Whether the player is currently accumulating Hour Farmer points");
	CVarWrapper points_per_min_cvar = persistent_storage->RegisterPersistentCvar("hf_points_per_min", "20", "How many points the player will accumulate every minute in training/custom maps");
	CVarWrapper points_per_min_freeplay_cvar = persistent_storage->RegisterPersistentCvar("hf_points_per_min_freeplay", "5", "How many points the player will accumulate every minute in freeplay");
	CVarWrapper points_per_min_replay_cvar = persistent_storage->RegisterPersistentCvar("hf_points_per_min_replay", "10", "How many points the player will accumulate every minute in replays");

	// kick off the time-based point awarding
	timeBasedPointAward();

	// show the overlay, in 0.1 secs to give the game time to load
	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		cvarManager->executeCommand("openmenu " + menuTitle_);
	}, 0.1);

	// hook training completion
	gameWrapper->HookEventWithCaller<ActorWrapper>("Function TAGame.GameEvent_TrainingEditor_TA.GetScore", [this](ActorWrapper caller, void* params, std::string eventName) {
		AGameEvent_TrainingEditor_TA_GetScore_Params* scoreParams = reinterpret_cast<AGameEvent_TrainingEditor_TA_GetScore_Params*>(params);
		DEBUGLOG("Training completed with score " + std::to_string(scoreParams->ReturnValue));
		});
}

void HourFarmer::awardPoints(int numPoints, std::string reason, bool silent)
{
	CVarWrapper points_cvar = cvarManager->getCvar("hf_points");
	if (!points_cvar) {
		LOG("Points cvar not found!");
		return;
	}
	points_cvar.setValue(points_cvar.getIntValue() + numPoints);
	if (!silent) {
		gameWrapper->Toast("Points awarded!", "You've been awarded " + std::to_string(numPoints) + " points for " + reason, "default", 3.5, ToastType_OK);
	}
	DEBUGLOG("Awarded " + std::to_string(numPoints) + " points for " + reason + (silent ? ", silently" : ", loudly") );
}

void HourFarmer::timeBasedPointAward()
{
	CVarWrapper points_accumulating_cvar = cvarManager->getCvar("hf_now_accumulating");
	if (!points_accumulating_cvar) {
		LOG("Points accumulating cvar not found!");
		return;
	}

	float timeToWait = calculateTimeToWait();

	if (points_accumulating_cvar.getBoolValue() && timeToWait > 0) {
		awardPoints(1, "time", true);
	}

	if (timeToWait == 0)
	{
		timeToWait = 6;
	}
	DEBUGLOG("Waiting " + std::to_string(timeToWait) + " seconds before awarding points again");
	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		timeBasedPointAward();
	}, timeToWait);
}

float HourFarmer::calculateTimeToWait()
{
	ServerWrapper sw = gameWrapper->GetCurrentGameState();
	if (!sw) return 0;
	GameSettingPlaylistWrapper playlist = sw.GetPlaylist();
	if (!playlist) return 0;
	int playlistID = playlist.GetPlaylistId(); // caution: playlistID is spoofed in replays

	DEBUGLOG("Determining time to wait for playlist {}", playlistID);

	if (playlistID == 21 || playlistID == 24 || playlistID == 19) { // training/custom map
		CVarWrapper points_per_min_cvar = cvarManager->getCvar("hf_points_per_min");
		if (!points_per_min_cvar) {
			LOG("Points per min cvar not found!");
			return 0;
		}

		return 60.0 / points_per_min_cvar.getFloatValue();
	}
	
	if (gameWrapper->IsInFreeplay()) {
		CVarWrapper points_per_min_freeplay_cvar = cvarManager->getCvar("hf_points_per_min_freeplay");
		if (!points_per_min_freeplay_cvar) {
			LOG("Points per min freeplay cvar not found!");
			return 0;
		
		}
		return 60.0 / points_per_min_freeplay_cvar.getFloatValue();
	}
	if (gameWrapper->IsInReplay()) {
		CVarWrapper points_per_min_replay_cvar = cvarManager->getCvar("hf_points_per_min_replay");
		if (!points_per_min_replay_cvar) {
			LOG("Points per min replay cvar not found!");
			return 0;
		}
		return 60.0 / points_per_min_replay_cvar.getFloatValue();
	}

	return 0;
}

void HourFarmer::renderShopItem(std::string name, std::string description, int cost, std::function<void()> purchaseAction)
{
	CVarWrapper points_cvar = cvarManager->getCvar("hf_points");
	if (!points_cvar) {
		LOG("Points cvar not found!");
		return;
	}
	int points = points_cvar.getIntValue();

	if (ImGui::Button((name + " (" + std::to_string(cost) + " points)").c_str())) {
		if (points < cost) {
			gameWrapper->Toast("Not enough points!", "You don't have enough points to purchase " + name, "default", 3.5, ToastType_Error);
			return;
		}
		else {
			points_cvar.setValue(points - cost);
			gameWrapper->Toast("Purchased!", "You've purchased " + name + " for " + std::to_string(cost) + " points", "default", 3.5, ToastType_OK);
			purchaseAction();
		}
	}
	ImGui::SameLine();
	ImGui::TextUnformatted(description.c_str());
}

void HourFarmer::RenderSettings()
{	
	ImGui::TextUnformatted("Welcome to the Hour Farmer shop!");
	ImGui::TextUnformatted("Here you can spend your hard-earned points on various items and upgrades.");

	renderShopItem("Free points!", "Gives you 10 free points.", 5, [this]() {
		awardPoints(10, " paying to win!", false);
	});
	renderShopItem("Crash!", "Crashes the game in 10 seconds!", 100, [this]() {
		gameWrapper->SetTimeout([this](GameWrapper* gw) {
			cvarManager->executeCommand("crash");
		}, 10.0);
	});


	if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_None)) {
		ImGui::Checkbox("Make overlay draggable", &is_dragging_overlay);

		CVarWrapper points_accumulating_cvar = cvarManager->getCvar("hf_now_accumulating");
		if (!points_accumulating_cvar) {
			LOG("Points accumulating cvar not found!");
		}
		else {
			bool points_accumulating = points_accumulating_cvar.getBoolValue();
			if (ImGui::Checkbox("Accumulate points over time", &points_accumulating)) {
				points_accumulating_cvar.setValue(points_accumulating);
			}
		}
		CVarWrapper points_per_min_cvar = cvarManager->getCvar("hf_points_per_min");
		if (!points_per_min_cvar) {
			LOG("Points per min cvar not found!");
		}
		else {
			float points_per_min = points_per_min_cvar.getFloatValue();
			if (ImGui::SliderFloat("Points per minute", &points_per_min, 0.0f, 100.0f)) {
				points_per_min_cvar.setValue(points_per_min);
			}
		}
	}
}

void HourFarmer::RenderWindow()
{
	ImGuiWindowFlags WindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
		| ImGuiWindowFlags_NoFocusOnAppearing;
	if (!is_dragging_overlay) {
		WindowFlags |= ImGuiWindowFlags_NoInputs;
	}

	// creates the window with the given flags
	if (!ImGui::Begin(menuTitle_.c_str(), &isWindowOpen_, WindowFlags))
	{
		ImGui::End();
		return;
	}

	// Do your overlay rendering with full ImGui here!
	ImGui::Text("Current points: %d", cvarManager->getCvar("hf_points").getIntValue());

	ImGui::End();
}

bool HourFarmer::IsActiveOverlay() {
	return false; // don't close overlay on esc
}