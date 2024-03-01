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
	CVarWrapper cduels_num_used_today_cvar = persistent_storage->RegisterPersistentCvar("hf_num_used_today_casual_duels", "0", "How many times the player has used the casual duels queue today");
	CVarWrapper cdoubles_num_used_today_cvar = persistent_storage->RegisterPersistentCvar("hf_num_used_today_casual_doubles", "0", "How many times the player has used the casual doubles queue today");
	CVarWrapper cstandard_num_used_today_cvar = persistent_storage->RegisterPersistentCvar("hf_num_used_today_casual_standard", "0", "How many times the player has used the casual standard queue today");
	CVarWrapper last_reset_time_cvar = persistent_storage->RegisterPersistentCvar("hf_last_reset_time", "0", "The time at which the daily limits were last reset");
	CVarWrapper win_streak_cvar = persistent_storage->RegisterPersistentCvar("hf_win_streak", "0", "The player's current win streak");

	// kick off the time-based point awarding
	timeBasedPointAward();

	// kick off checking for daily limits reset (timeout to allow persistent storage to load)
	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		dailyLimitsResetCheck();
	}, 2);

	// show the overlay, in 0.1 secs to give the game time to load
	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		cvarManager->executeCommand("openmenu " + menuTitle_);
	}, 0.1);

	// hook training completion
	gameWrapper->HookEventWithCaller<ActorWrapper>("Function TAGame.GameEvent_TrainingEditor_TA.GetScore", [this](ActorWrapper caller, void* params, std::string eventName) {
		AGameEvent_TrainingEditor_TA_GetScore_Params* scoreParams = reinterpret_cast<AGameEvent_TrainingEditor_TA_GetScore_Params*>(params);
		DEBUGLOG("Training completed with score " + std::to_string(scoreParams->ReturnValue));
		});
	// hook trying to queue
	gameWrapper->HookEvent("Function TAGame.GFxData_Matchmaking_TA.StartMatchmaking", [this](...) {
		if (queueingIsAllowed) {
			DEBUGLOG("Queueing is allowed, continuing");
		}
		else {
			gameWrapper->SetTimeout([this](GameWrapper* gw) {
				gameWrapper->Execute([this](GameWrapper* gw) {
					gameWrapper->GetMatchmakingWrapper().CancelMatchmaking();
					ModalWrapper alertModal = gameWrapper->CreateModal("Queueing not allowed");
					alertModal.AddButton("OK", true);
					alertModal.SetBody("Queueing with the main menu is disabled while using Hour Farmer. Please purchase a queue via the Hour Farmer menu.");
					alertModal.ShowModal();
					queueingIsAllowed = false;
				});
			}, 0.5);	
		}

		});
	// if the queue is cancelled, disallow queueing again
	gameWrapper->HookEvent("Function TAGame.GFxData_Matchmaking_TA.CancelSearch", [this](...) {
		queueingIsAllowed = false;
		});

	gameWrapper->HookEventWithCallerPost<ServerWrapper>("Function TAGame.GameEvent_Soccar_TA.EventMatchEnded", [this](ServerWrapper caller, ...) {
		onMatchEnded(caller);
		});

	gameWrapper->HookEventPost("Function TAGame.GameEvent_Soccar_TA.Destroyed", [this](...) {
		DEBUGLOG("GameEvent_Soccar_TA destroyed, didJustWin is {}, gamePlayedWasCompetitive is {}", didJustWin, gamePlayedWasCompetitive);
		if (!didJustWin && gamePlayedWasCompetitive) {
			CVarWrapper win_streak_cvar = cvarManager->getCvar("hf_win_streak");
			if (!win_streak_cvar) {
				LOG("Win streak cvar not found!");
				return;
			}
			win_streak_cvar.setValue(0);
		}
		didJustWin = false;
		gamePlayedWasCompetitive = false;
		});
	gameWrapper->HookEventPost("Function TAGame.Team_TA.PostBeginPlay", [this](...) {
		// are we in a competitive match?
		ServerWrapper sw = gameWrapper->GetCurrentGameState();
		if (!sw) {
			LOG("Server was null in PostBeginPlay");
			return;
		}
		GameSettingPlaylistWrapper playlist = sw.GetPlaylist();
		if (!playlist) {
			LOG("Playlist was null in PostBeginPlay");
			return;
		}
		int playlistID = playlist.GetPlaylistId();
		gamePlayedWasCompetitive = playlistID == 10 || playlistID == 11 || playlistID == 13;
		DEBUGLOG("Joining a {} match!", gamePlayedWasCompetitive ? "competitive" : "non-competitive");
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

void HourFarmer::dailyLimitsResetCheck()
{
	CVarWrapper last_reset_time_cvar = cvarManager->getCvar("hf_last_reset_time");
	if (!last_reset_time_cvar) {
		LOG("Last reset time cvar not found!");
		return;
	}

	auto now = std::chrono::system_clock::now();
	std::time_t now_t = std::chrono::system_clock::to_time_t(now);
	std:tm* now_tm = std::localtime(&now_t);
	std::tm now_tm_copy = *now_tm;
	std::time_t last_reset_time = static_cast<std::time_t>(last_reset_time_cvar.getIntValue());
	std::tm* last_reset_tm = std::localtime(&last_reset_time);
	DEBUGLOG("Last reset: {}-{}-{} (unix {}), now: {}-{}-{} (unix {})", last_reset_tm->tm_year, last_reset_tm->tm_mon, last_reset_tm->tm_mday, last_reset_time, now_tm_copy.tm_year, now_tm_copy.tm_mon, now_tm_copy.tm_mday, now_t);
	if (last_reset_tm->tm_year != now_tm_copy.tm_year || last_reset_tm->tm_mon != now_tm_copy.tm_mon || last_reset_tm->tm_mday != now_tm_copy.tm_mday) {
		ResetDailyLimits();
		last_reset_time_cvar.setValue(static_cast<int>(std::chrono::system_clock::to_time_t(now)));
	}

	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		dailyLimitsResetCheck();
	}, 60.0);
}

void HourFarmer::ResetDailyLimits() {
	auto cvars_to_reset = { "hf_num_used_today_casual_duels", "hf_num_used_today_casual_doubles", "hf_num_used_today_casual_standard" };
	for (auto cvar_name : cvars_to_reset) {
		CVarWrapper cvar = cvarManager->getCvar(cvar_name);
		if (!cvar) {
			LOG("Cvar {} not found!", cvar_name);
			continue;
		}
		cvar.setValue(0);
	}
	gameWrapper->Toast("Daily limits reset!", "Your daily Hour Farmer limits have been reset", "default", 3.5, ToastType_OK);
}

void HourFarmer::onMatchEnded(ServerWrapper server)
{
	// points: 250 for win, 50 bonus for winstreak of 3 or more
	if (!server) {
		LOG("Server was null in onMatchEnded");
		return;
	}
	// make sure we're in competitive
	GameSettingPlaylistWrapper playlist = server.GetPlaylist();
	if (!playlist) {
		LOG("Playlist was null in onMatchEnded");
		return;
	}
	int playlistID = playlist.GetPlaylistId();
	if (playlistID != 10 && playlistID != 11 && playlistID != 13) {
		DEBUGLOG("Not in competitive, not awarding points");
		return;
	}
	auto teams = server.GetTeams();
	if (teams.Count() != 2) {
		LOG("Teams count was not 2 in onMatchEnded");
		return;
	}
	auto blueTeam = teams.Get(0);
	auto orangeTeam = teams.Get(1);
	if (!blueTeam || !orangeTeam) {
		LOG("Blue or orange team was null in onMatchEnded");
		return;
	}
	auto blueScore = blueTeam.GetScore();
	auto orangeScore = orangeTeam.GetScore();
	auto pc = gameWrapper->GetPlayerController();
	if (!pc) {
		LOG("PlayerControllerWrapper is null in onMatchEnded");
		return;
	}
	auto pri = pc.GetPRI();
	if (!pri) {
		LOG("PRIWrapper is null in onMatchEnded");
		return;
	}
	auto playerTeam = pri.GetTeamNum();

	CVarWrapper win_streak_cvar = cvarManager->getCvar("hf_win_streak");
	if (!win_streak_cvar) {
		LOG("Win streak cvar not found!");
		return;
	}
	if (playerTeam == (blueScore > orangeScore ? 0 : 1)) {
		// we won
		int win_streak = win_streak_cvar.getIntValue();
		awardPoints(250, "winning a ranked match!", false);
		if (win_streak >= 3) {
			awardPoints(50, "win streak bonus!", false);
		}
		win_streak_cvar.setValue(win_streak + 1);
	}
	else {
		// we lost
		win_streak_cvar.setValue(0);
	}

	didJustWin = playerTeam == (blueScore > orangeScore ? 0 : 1);
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

void HourFarmer::renderLimitedPerDayItem(std::string name, std::string description, int maxPurchasesPerDay, CVarWrapper numUsedTodayCvar, std::function<void()> purchaseAction)
{
	if (ImGui::Button((name + " (" + std::to_string(maxPurchasesPerDay - numUsedTodayCvar.getIntValue()) + " left)").c_str())) {
		if (numUsedTodayCvar.getIntValue() >= maxPurchasesPerDay) {
			gameWrapper->Toast("Out of stock!", "You've already purchased " + name + " the maximum number of times today", "default", 3.5, ToastType_Error);
			return;
		}
		else {
			numUsedTodayCvar.setValue(numUsedTodayCvar.getIntValue() + 1);
			gameWrapper->Toast("Purchased!", "You've purchased " + name + "! You have " + std::to_string(maxPurchasesPerDay - numUsedTodayCvar.getIntValue()) + " left today.", "default", 3.5, ToastType_OK);
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

	CVarWrapper cduels_num_used_today_cvar = cvarManager->getCvar("hf_num_used_today_casual_duels");
	if (!cduels_num_used_today_cvar) {
		LOG("Casual duels num used today cvar not found!");
	}
	else {
		renderLimitedPerDayItem("Casual duels", "Queues you for casual duels", 5, cduels_num_used_today_cvar, [this]() {
			QueueForMatch(Playlist::CASUAL_DUELS, PlaylistCategory::CASUAL);
		});
	}
	CVarWrapper cdoubles_num_used_today_cvar = cvarManager->getCvar("hf_num_used_today_casual_doubles");
	if (!cdoubles_num_used_today_cvar) {
		LOG("Casual doubles num used today cvar not found!");
	}
	else {
		renderLimitedPerDayItem("Casual doubles", "Queues you for casual doubles", 5, cdoubles_num_used_today_cvar, [this]() {
			QueueForMatch(Playlist::CASUAL_DOUBLES, PlaylistCategory::CASUAL);
			});
	}
	CVarWrapper cstandard_num_used_today_cvar = cvarManager->getCvar("hf_num_used_today_casual_standard");
	if (!cstandard_num_used_today_cvar) {
		LOG("Casual standard num used today cvar not found!");
	}
	else {
		renderLimitedPerDayItem("Casual standard", "Queues you for casual standard", 5, cstandard_num_used_today_cvar, [this]() {
			QueueForMatch(Playlist::CASUAL_STANDARD, PlaylistCategory::CASUAL);
		});
	}
	renderShopItem("Competitive duels", "Queues you for competitive duels", 1000, [this]() {
		QueueForMatch(Playlist::RANKED_DUELS, PlaylistCategory::RANKED);
	});
	renderShopItem("Competitive doubles", "Queues you for competitive doubles", 1000, [this]() {
		QueueForMatch(Playlist::RANKED_DOUBLES, PlaylistCategory::RANKED);
	});
	renderShopItem("Competitive standard", "Queues you for competitive standard", 1000, [this]() {
		QueueForMatch(Playlist::RANKED_STANDARD, PlaylistCategory::RANKED);
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
			if (ImGui::SliderFloat("Points per minute in training/custom maps", &points_per_min, 0.0f, 100.0f)) {
				points_per_min_cvar.setValue(points_per_min);
			}
		}
		CVarWrapper points_per_min_freeplay_cvar = cvarManager->getCvar("hf_points_per_min_freeplay");
		if (!points_per_min_freeplay_cvar) {
			LOG("Points per min freeplay cvar not found!");
		}
		else {
			float points_per_min_freeplay = points_per_min_freeplay_cvar.getFloatValue();
			if (ImGui::SliderFloat("Points per minute in freeplay", &points_per_min_freeplay, 0.0f, 100.0f)) {
				points_per_min_freeplay_cvar.setValue(points_per_min_freeplay);
			}
		}
		CVarWrapper points_per_min_replay_cvar = cvarManager->getCvar("hf_points_per_min_replay");
		if (!points_per_min_replay_cvar) {
			LOG("Points per min replay cvar not found!");
		}
		else {
			float points_per_min_replay = points_per_min_replay_cvar.getFloatValue();
			if (ImGui::SliderFloat("Points per minute in replays", &points_per_min_replay, 0.0f, 100.0f)) {
				points_per_min_replay_cvar.setValue(points_per_min_replay);
			}
		}

		if (ImGui::Button("Force reset daily limits")) {
			ResetDailyLimits();
		}

		if (ImGui::Button("Force allow a queue through the normal menus")) {
			queueingIsAllowed = true;
		}

		// points override: add or subtract points
		static int points_override_amount = 0;
		ImGui::InputInt("Amount", &points_override_amount);
		if (ImGui::Button("Award points")) {
			awardPoints(points_override_amount, "manual override add", false);
		}
		if (ImGui::Button("Remove points")) {
			awardPoints(-points_override_amount, "manual override subtract", false);
		}

		// winstreak override: set winstreak
		CVarWrapper win_streak_cvar = cvarManager->getCvar("hf_win_streak");
		if (!win_streak_cvar) {
			LOG("Win streak cvar not found!");
		}
		else {
			int win_streak = win_streak_cvar.getIntValue();
			// show winstreak, button for reset and button for adding one
			ImGui::Text("Current win streak: %d", win_streak);
			ImGui::SameLine();
			if (ImGui::Button("Reset")) {
				win_streak_cvar.setValue(0);
			}
			ImGui::SameLine();
			if (ImGui::Button("+1")) {
				win_streak_cvar.setValue(win_streak + 1);
			}
		}
	}
}

void HourFarmer::QueueForMatch(Playlist playlist, PlaylistCategory playlistCategory) {
	queueingIsAllowed = true;
	gameWrapper->Execute([this, playlist, playlistCategory](GameWrapper* gw) {
		MatchmakingWrapper matchmaker = gameWrapper->GetMatchmakingWrapper();
		matchmaker.SetPlaylistSelection(playlist, 1);
		matchmaker.StartMatchmaking(playlistCategory);
		});
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
	ImGui::Text("Current winstreak in competitive: %d", cvarManager->getCvar("hf_win_streak").getIntValue());
	//ImGui::Text("Current points per minute: %d", calculateTimeToWait() == 0 ? 0 : round(60.0 / calculateTimeToWait()));
	ImGui::End();
}

bool HourFarmer::IsActiveOverlay() {
	return false; // don't close overlay on esc
}