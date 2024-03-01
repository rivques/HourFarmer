#pragma once

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"
#include "GuiBase.h"
#include "PersistentStorage.h"

#include "version.h"
constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);

// Function TAGame.GameEvent_TrainingEditor_TA.GetScore
struct AGameEvent_TrainingEditor_TA_GetScore_Params
{
	int                                                ReturnValue;                                              // (CPF_Parm, CPF_OutParm, CPF_ReturnParm)
};

class HourFarmer: public BakkesMod::Plugin::BakkesModPlugin
	,public SettingsWindowBase // Uncomment if you wanna render your own tab in the settings menu
	,public PluginWindowBase // Uncomment if you want to render your own plugin window
{

	std::shared_ptr<PersistentStorage> persistent_storage;

	//Boilerplate
	void onLoad() override;
	//void onUnload() override; // Uncomment and implement if you need a unload method

	void awardPoints(int numPoints, std::string reason, bool silent);
	void timeBasedPointAward();
	float calculateTimeToWait();
	void dailyLimitsResetCheck();
	void ResetDailyLimits();
	void onMatchEnded(ServerWrapper server);

	void renderShopItem(std::string name, std::string description, int cost, std::function<void()> purchaseAction);
	void renderLimitedPerDayItem(std::string name, std::string description, int maxPurchasesPerDay, CVarWrapper numUsedTodayCvar, std::function<void()> purchaseAction);
	void QueueForMatch(Playlist playlist, PlaylistCategory playlistCategory);

	// state for queuing
	bool queueingIsAllowed = false;
	bool gamePlayedWasCompetitive = false;
	// state for winstreak
	bool didJustWin = false;

	// here's some vars for the GUI
	bool is_showing_overlay = true;
	bool is_dragging_overlay = false;
public:
	void RenderSettings() override; // Uncomment if you wanna render your own tab in the settings menu
	void RenderWindow() override; // Uncomment if you want to render your own plugin window
	bool IsActiveOverlay() override;
};
