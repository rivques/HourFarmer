#include "pch.h"
#include "HourFarmer.h"

#include "RenderingTools/RenderingTools.h"

#define TOAST_SCALE 1.0f

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
	CVarWrapper points_goal_weekday_cvar = persistent_storage->RegisterPersistentCvar("hf_points_goal_weekday", "10", "How many points the player will accumulate every time they score a goal on a Wednesday");
	CVarWrapper quadrant_size_cvar = persistent_storage->RegisterPersistentCvar("hf_quadrant_size", "20", "The size of the quadrants for goal accuracy awards");
	CVarWrapper doublexpweekendss_active_cvar = persistent_storage->RegisterPersistentCvar("hf_doublexpweekends_active", "1", "Whether double XP weekends are active");

	// kick off the time-based point awarding
	timeBasedPointAward();

	// kick off checking for daily limits reset (timeout to allow persistent storage to load)
	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		dailyLimitsResetCheck();
	}, 2);

	// show the overlay, in 0.1 secs to give the game time to load
	gameWrapper->SetTimeout([this](GameWrapper* gw) {
		CVarWrapper points_cvar = cvarManager->getCvar("hf_points");
		sessionStartPoints = points_cvar.getIntValue();
		cvarManager->executeCommand("openmenu " + menuTitle_);
	}, 0.1);

	// hook training completion
	//gameWrapper->HookEventWithCaller<ActorWrapper>("Function TAGame.GameEvent_TrainingEditor_TA.GetScore", [this](ActorWrapper caller, void* params, std::string eventName) {
	//	AGameEvent_TrainingEditor_TA_GetScore_Params* scoreParams = reinterpret_cast<AGameEvent_TrainingEditor_TA_GetScore_Params*>(params);
	//	DEBUGLOG("Training completed with score " + std::to_string(scoreParams->ReturnValue));
	//	});
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
		CVarWrapper points_cvar = cvarManager->getCvar("hf_points");
		if (!points_cvar) {
			LOG("Points cvar not found!");
			return;
		}
		points_cvar.setValue(points_cvar.getIntValue() + queueingCancelRefund);
		if (queueingCancelRefund > 0) {
			gameWrapper->Toast("Queue cancelled", "You've been refunded " + std::to_string(queueingCancelRefund) + " points for cancelling the queue", "default", 3.5, ToastType_OK, 290.0f*TOAST_SCALE, 60.0f*TOAST_SCALE);
		}
		queueingCancelRefund = 0;
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
		queueingIsAllowed = false;
		didJustWin = false;
		gamePlayedWasCompetitive = false;
		});
	gameWrapper->HookEventPost("Function TAGame.Team_TA.PostBeginPlay", [this](...) {
		queueingCancelRefund = 0;
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


	//  We need the params so we hook with caller, but there is no wrapper for the HUD
	gameWrapper->HookEventWithCallerPost<ServerWrapper>("Function TAGame.GFxHUD_TA.HandleStatTickerMessage",
		[this](ServerWrapper caller, void* params, std::string eventname) {
			onStatTickerMessage(params);
		});

	gameWrapper->HookEvent("Function GameEvent_Soccar_TA.ReplayPlayback.BeginState", [this](...) {
		isInGoalReplay = true;
		});
	gameWrapper->HookEvent("Function GameEvent_Soccar_TA.ReplayPlayback.EndState", [this](...) {
		isInGoalReplay = false;
		});

	gameWrapper->HookEventWithCallerPost<BallWrapper>("Function TAGame.Ball_TA.OnHitGoal", [this](BallWrapper caller, void* params, std::string eventname) {
		if (isInGoalReplay) {
				return;
			}
		onGoalScored(caller);
		});
	gameWrapper->HookEvent("Function TAGame.GFxHUD_Soccar_TA.HandlePlayerRemoved", [this](...){
		onPlayerRemoved();
	});

	gameWrapper->RegisterDrawable([this](CanvasWrapper canvas) {
		drawAccuracyOverlay(canvas);
		});
	gameWrapper->HookEvent("Function TAGame.GameEvent_TrainingEditor_TA.EventRoundFinished", [this](...) {
		lastEventRoundFinished = std::chrono::steady_clock::now();
		});
	gameWrapper->HookEvent("Function TAGame.GameEvent_TrainingEditor_TA.EndTraining", [this](...) {
		auto now = std::chrono::steady_clock::now();
		if ((now - lastEventRoundFinished) < std::chrono::milliseconds(500)) {
			onTrainingEnd();
		}
		else {
			DEBUGLOG("Training ended, but not <500ms after eventroundfinish");
		}
		});
}
void HourFarmer::onStatTickerMessage(void* params) {
	StatTickerParams* pStruct = (StatTickerParams*)params;
	PriWrapper receiver = PriWrapper(pStruct->Receiver);
	PriWrapper victim = PriWrapper(pStruct->Victim);
	StatEventWrapper statEvent = StatEventWrapper(pStruct->StatEvent);
	std::time_t t = std::time(nullptr);
	std::tm* now = std::localtime(&t);
	if (statEvent.GetEventName() == "Goal" && gameWrapper->IsInOnlineGame() && now->tm_wday == 3 && receiver.memory_address == gameWrapper->GetPlayerController().GetPRI().memory_address) {
		CVarWrapper points_goal_weekday_cvar = cvarManager->getCvar("hf_points_goal_weekday");
		if (!points_goal_weekday_cvar) {
			LOG("Points goal weekday cvar not found!");
			return;
		}
		awardPoints(points_goal_weekday_cvar.getIntValue(), "scoring a goal on Wednesday", false);
	}
}

void HourFarmer::onGoalScored(BallWrapper ball)
{
	if (!gameWrapper->IsInCustomTraining()) {
		return;
	}
	CVarWrapper quadrant_size_cvar = cvarManager->getCvar("hf_quadrant_size");
	if (!quadrant_size_cvar) {
		LOG("Quadrant size cvar not found!");
		return;
	}
	double quadrant_size_percent = quadrant_size_cvar.getIntValue()/100.0;
	const double GOAL_HEIGHT = 642;
	const double GOAL_WIDTH = 892;
	double goalX = abs(ball.GetLocation().X);
	double goalZ = abs(ball.GetLocation().Z);
	if (goalX < 0 || goalX > GOAL_WIDTH || goalZ < 0 || goalZ > GOAL_HEIGHT) {
		return;
	}
	double goalXPercent = goalX / (GOAL_WIDTH);
	double goalZPercent = goalZ / (GOAL_HEIGHT);
	DEBUGLOG("Goal scored at x: {} ({}%), z: {} ({}%) (target {}%)", goalX, goalXPercent, goalZ, goalZPercent, quadrant_size_percent);
	if (goalXPercent > 1 - quadrant_size_percent && (goalZPercent < quadrant_size_percent || goalZPercent > (1 - quadrant_size_percent))) {
		awardPoints(10, "shot accuracy!", false);
	}
}

void HourFarmer::onPlayerRemoved(){
	ServerWrapper sw = gameWrapper->GetCurrentGameState();
	if (!sw) {
		LOG("Server was null in onPlayerRemoved");
		return;
	}
	if (gameWrapper->IsInReplay()) {
		DEBUGLOG("A player left the match, but we're in a replay");
		return;
	}
	if(!sw.ShouldHaveLeaveMatchPenalty()){
		DEBUGLOG("A player left the match, but no penalty should be applied");
		return;
	}
	if(sw.GetGameTimeRemaining() > 210 && sw.GetbOverTime() == 0){
		awardPoints(900, "teammate abandoning!", false);
	} else {
		awardPoints(100, "teammate abandoning!", false);
	}
}

void HourFarmer::onTrainingEnd()
{
	auto gtd = gameWrapper->GetGfxTrainingData();
	if (!gtd) {
		LOG("Training wrapper was null in onTrainingEnd");
		return;
	}
	DEBUGLOG("Training ended with score {}, total rounds {}", gtd.GetCurrentScore(), gtd.GetTotalRounds());
	if (gtd.GetCurrentScore() == gtd.GetTotalRounds()) {
		awardPoints(30, "100%%ing training!", false);
	}
}

void HourFarmer::drawAccuracyOverlay(CanvasWrapper canvas)
{
	if (!gameWrapper->IsInCustomTraining() || !showAccuracyOverlay) {
		return;
	}
	const float GOAL_HEIGHT = 642;
	const float GOAL_WIDTH = 892;
	const float GOAL_X = 0;
	const float GOAL_Z = GOAL_HEIGHT/2;
	const float GOAL_Y = 5120;

	CVarWrapper quadrant_size_cvar = cvarManager->getCvar("hf_quadrant_size");
	if (!quadrant_size_cvar) {
		LOG("Quadrant size cvar not found!");
		return;
	}
	float quadrant_size_percent = quadrant_size_cvar.getIntValue() / 100.0;
	float quadrant_size_h = GOAL_HEIGHT * quadrant_size_percent;
	float quadrant_size_w = GOAL_WIDTH * quadrant_size_percent;
	auto camera = gameWrapper->GetCamera();
	if (camera.IsNull()) { LOG("Null camera"); return; }
	RT::Frustum frust{ canvas, camera };

	// remember that these are the center of the grid, so we need to subtract half the grid width/height
	Vector topLeft = { GOAL_X - GOAL_WIDTH + quadrant_size_w/2, GOAL_Y, GOAL_Z + GOAL_HEIGHT / 2 -quadrant_size_h/2};
	Vector topRight = { GOAL_X + GOAL_WIDTH - quadrant_size_w/2, GOAL_Y, GOAL_Z + GOAL_HEIGHT / 2 - quadrant_size_h/2 };
	Vector bottomLeft = { GOAL_X - GOAL_WIDTH + quadrant_size_w/2, GOAL_Y, GOAL_Z - GOAL_HEIGHT / 2 + quadrant_size_h/2 };
	Vector bottomRight = { GOAL_X + GOAL_WIDTH - quadrant_size_w/2, GOAL_Y, GOAL_Z - GOAL_HEIGHT / 2 + quadrant_size_h/2 };

	canvas.SetColor(LinearColor{ 0, 255, 0, 255 });

	RT::Grid( topLeft, {0.707,0,0,0.707}, quadrant_size_w,quadrant_size_h, 0, 0 ).Draw(canvas, frust, false);
	RT::Grid( topRight, {0.707,0,0,0.707}, quadrant_size_w,quadrant_size_h, 0, 0 ).Draw(canvas, frust, false);
	RT::Grid( bottomLeft, {0.707,0,0,0.707}, quadrant_size_w,quadrant_size_h, 0, 0 ).Draw(canvas, frust, false);
	RT::Grid( bottomRight, {0.707,0,0,0.707}, quadrant_size_w,quadrant_size_h, 0, 0 ).Draw(canvas, frust, false);
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
		gameWrapper->Toast("Points awarded!", "You've been awarded " + std::to_string(numPoints) + " points for " + reason, "default", 3.5, ToastType_OK, 290.0f * TOAST_SCALE, 60.0f * TOAST_SCALE);
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
	float result = 0;
	DEBUGLOG("Determining time to wait for playlist {}", playlistID);

	if (playlistID == 21 || playlistID == 24 || playlistID == 19) { // training/custom map
		CVarWrapper points_per_min_cvar = cvarManager->getCvar("hf_points_per_min");
		if (!points_per_min_cvar) {
			LOG("Points per min cvar not found!");
			return 0;
		}

		result = 60.0 / points_per_min_cvar.getFloatValue();
	} else if (gameWrapper->IsInFreeplay()) {
		CVarWrapper points_per_min_freeplay_cvar = cvarManager->getCvar("hf_points_per_min_freeplay");
		if (!points_per_min_freeplay_cvar) {
			LOG("Points per min freeplay cvar not found!");
			return 0;
		
		}
		result = 60.0 / points_per_min_freeplay_cvar.getFloatValue();
	} else if (gameWrapper->IsInReplay()) {
		CVarWrapper points_per_min_replay_cvar = cvarManager->getCvar("hf_points_per_min_replay");
		if (!points_per_min_replay_cvar) {
			LOG("Points per min replay cvar not found!");
			return 0;
		}
		result = 60.0 / points_per_min_replay_cvar.getFloatValue();
	}
	// double xp weekends
	CVarWrapper doublexpweekends_active_cvar = cvarManager->getCvar("hf_doublexpweekends_active");
	if (!doublexpweekends_active_cvar) {
		LOG("Double XP weekends active cvar not found!");
		return result;
	}
	if (!doublexpweekends_active_cvar.getBoolValue()) {
		return result;
	}
	std::time_t t = std::time(nullptr);
	std::tm* now = std::localtime(&t);
	if (now->tm_wday == 6 || now->tm_wday == 0) {
		result /= 2.0f; // double xp weekends
	}
	return result;
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
	gameWrapper->Toast("Daily limits reset!", "Your daily Hour Farmer limits have been reset", "default", 3.5, ToastType_OK, 290.0f * TOAST_SCALE, 60.0f * TOAST_SCALE);
}

void HourFarmer::onMatchEnded(ServerWrapper server)
{
	// points: 250 for win, 50 bonus for winstreak of 3 or more
	if (!server) {
		LOG("Server was null in onMatchEnded");
		return;
	}
	queueingIsAllowed = false;
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
		if (win_streak >= 2) { // starts at 3 because incr is after this
			awardPoints(500, "win streak bonus!", false);
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
			gameWrapper->Toast("Not enough points!", "You don't have enough points to purchase " + name, "default", 3.5, ToastType_Error, 290.0f * TOAST_SCALE, 60.0f * TOAST_SCALE);
			return;
		}
		else {
			points_cvar.setValue(points - cost);
			gameWrapper->Toast("Purchased!", "You've purchased " + name + " for " + std::to_string(cost) + " points", "default", 3.5, ToastType_OK, 290.0f * TOAST_SCALE, 60.0f * TOAST_SCALE);
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
			gameWrapper->Toast("Out of stock!", "You've already purchased " + name + " the maximum number of times today", "default", 3.5, ToastType_Error, 290.0f * TOAST_SCALE, 60.0f * TOAST_SCALE);
			return;
		}
		else {
			numUsedTodayCvar.setValue(numUsedTodayCvar.getIntValue() + 1);
			gameWrapper->Toast("Purchased!", "You've purchased " + name + "! You have " + std::to_string(maxPurchasesPerDay - numUsedTodayCvar.getIntValue()) + " left today.", "default", 3.5, ToastType_OK, 290.0f * TOAST_SCALE, 60.0f * TOAST_SCALE);
			purchaseAction();
		}
	}
	ImGui::SameLine();
	ImGui::TextUnformatted(description.c_str());
}



void HourFarmer::RenderSettings()
{	
	ImGui::SetWindowFontScale(2);
	ImGui::TextUnformatted("Welcome to the Hour Farmer shop!");
	ImGui::TextUnformatted("Here you can spend your hard-earned points on various items and upgrades.");
	ImGui::Separator();
	ImGui::TextUnformatted("Casual Queues");

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
	ImGui::Separator();
	ImGui::TextUnformatted("Competitive Queues");

	renderShopItem("Competitive duels", ("now free always!"), 0, [this]() {
		QueueForMatch(Playlist::RANKED_DUELS, PlaylistCategory::RANKED);
		queueingCancelRefund = 0;
	});
	renderShopItem("Competitive doubles", "Queues you for competitive doubles", 1000, [this]() {
		QueueForMatch(Playlist::RANKED_DOUBLES, PlaylistCategory::RANKED);
		queueingCancelRefund = 1000;
	});
	renderShopItem("Competitive standard", "Queues you for competitive standard", 1000, [this]() {
		QueueForMatch(Playlist::RANKED_STANDARD, PlaylistCategory::RANKED);
		queueingCancelRefund = 1000;
	});
	ImGui::Separator();
	ImGui::TextUnformatted("Powerups");
	renderShopItem("Coaching session", "30 mins of coaching", 5000, [this]() {});
	renderShopItem("Workshop map", "Get a new workshop map", 5000, [this]() {});
	renderShopItem("Queue with pro", "Add a pro to your party for a queue", 7500, [this]() {});

	ImGui::SetWindowFontScale(1);
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

		// change quadrant size (percent of goal)
		CVarWrapper quadrant_size_cvar = cvarManager->getCvar("hf_quadrant_size");
		if (!quadrant_size_cvar) {
			LOG("Quadrant size cvar not found!");
		}
		else {
			int quadrant_size = quadrant_size_cvar.getIntValue();
			if (ImGui::SliderInt("Quadrant bonus target size", &quadrant_size, 0, 100, "%d%%")) {
				quadrant_size_cvar.setValue(quadrant_size);
			}
		}
		ImGui::Checkbox("Show goal accuracy overlay", &showAccuracyOverlay);
		// double xp weekends
		CVarWrapper doublexpweekends_active_cvar = cvarManager->getCvar("hf_doublexpweekends_active");
		if (!doublexpweekends_active_cvar) {
			LOG("Double XP weekends active cvar not found!");
		}
		else {
			bool doublexpweekends_active = doublexpweekends_active_cvar.getBoolValue();
			if (ImGui::Checkbox("Double XP weekends active", &doublexpweekends_active)) {
				doublexpweekends_active_cvar.setValue(doublexpweekends_active);
			}
		}
	}
}

void HourFarmer::QueueForMatch(Playlist playlist, PlaylistCategory playlistCategory) {
	queueingIsAllowed = true;
	gameWrapper->Execute([this, playlist, playlistCategory](GameWrapper* gw) {
		MatchmakingWrapper matchmaker = gameWrapper->GetMatchmakingWrapper();
		Playlist playlistsToClear[] = {Playlist::CASUAL_DUELS, Playlist::CASUAL_DOUBLES, Playlist::CASUAL_STANDARD, Playlist::CASUAL_CHAOS, Playlist::RANKED_DUELS, Playlist::RANKED_DOUBLES, Playlist::RANKED_STANDARD, Playlist::AUTO_TOURNAMENT, Playlist:: EXTRAS_DROPSHOT, Playlist::EXTRAS_HOOPS, Playlist::EXTRAS_RUMBLE, Playlist::EXTRAS_SNOWDAY};
		for (auto playlistToClear : playlistsToClear) {
			matchmaker.SetPlaylistSelection(playlistToClear, 0);
		}

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

	int minPointsToPurchaseSomething = 1000;
	CVarWrapper points_cvar = cvarManager->getCvar("hf_points");
	if (!points_cvar) {
		LOG("Points cvar not found!");
		return;
	}
	if (points_cvar.getIntValue() < minPointsToPurchaseSomething) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
	}
	else {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
	}
	ImGui::SetWindowFontScale(2.5);
	ImGui::Text("Current points: %d", points_cvar.getIntValue());
	ImGui::PopStyleColor();
	ImGui::SetWindowFontScale(2);
	ImGui::Text("Current winstreak in competitive: %d", cvarManager->getCvar("hf_win_streak").getIntValue());
	ImGui::Text("Points this session: %d", cvarManager->getCvar("hf_points").getIntValue() - sessionStartPoints);
	//ImGui::Text("Current points per minute: %d", calculateTimeToWait() == 0 ? 0 : round(60.0 / calculateTimeToWait()));
	ImGui::End();
}

bool HourFarmer::IsActiveOverlay() {
	return false; // don't close overlay on esc
}
