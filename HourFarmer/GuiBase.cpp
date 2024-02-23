#include "pch.h"
#include "GuiBase.h"

std::string SettingsWindowBase::GetPluginName()
{
	return "HourFarmer";
}

void SettingsWindowBase::SetImGuiContext(uintptr_t ctx)
{
	ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

std::string PluginWindowBase::GetMenuName()
{
	return "HourFarmer";
}

std::string PluginWindowBase::GetMenuTitle()
{
	return menuTitle_;
}

void PluginWindowBase::SetImGuiContext(uintptr_t ctx)
{
	ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

bool PluginWindowBase::ShouldBlockInput()
{
	return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard;
}

bool PluginWindowBase::IsActiveOverlay()
{
	return true;
}

void PluginWindowBase::OnOpen()
{
	isWindowOpen_ = true;
}

void PluginWindowBase::OnClose()
{
	isWindowOpen_ = false;
}

void PluginWindowBase::Render()
{

	RenderWindow();

	if (!isWindowOpen_)
	{
		_globalCvarManager->executeCommand("togglemenu " + GetMenuName());
	}
}
