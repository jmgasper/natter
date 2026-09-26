// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include <Application.h>
#include <Messenger.h>

#include <map>
#include <string>

#include "natter/util.h"

namespace natter::ui {

// Keeps the signed-in workspaces (one credentials file each in
// ~/config/settings/Natter/accounts, mode 0600) and a window per open one.
class App : public BApplication {
public:
	App();

	void ReadyToRun() override;
	void MessageReceived(BMessage* message) override;
	bool QuitRequested() override;
	void AboutRequested() override;

private:
	void OpenWorkspace(const std::string& teamId);
	void ShowSignIn();
	void LoadSettings();
	void SaveSettings();
	void QuitIfIdle();

	std::string fDirectory;
	std::map<std::string, BMessenger> fWindows;
	BMessenger fSignIn;
	json fSettings;
};

}  // namespace natter::ui
