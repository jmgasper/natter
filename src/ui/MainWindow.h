// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include "natter/credentials.h"

#include <Window.h>

#include <memory>
#include <string>

class BButton;
class BFilePanel;
class BGroupView;
class BMenu;
class BStringView;

namespace natter {
class Session;
}

namespace natter::ui {

class Composer;
class MessageView;
class Sidebar;

// One workspace: the sidebar of conversations, the open conversation with
// its composer, and a thread beside it when one is open.
class MainWindow : public BWindow {
public:
	MainWindow(const Credentials& credentials, const std::string& settingsDirectory);
	~MainWindow() override;

	const std::string& TeamId() const { return fTeamId; }

	void MessageReceived(BMessage* message) override;
	bool QuitRequested() override;
	void WindowActivated(bool active) override;

private:
	void BuildMenu(BMenu*& workspaces);
	void SessionEvent(BMessage* message);
	void SelectConversation(const std::string& channel);
	void OpenThread(const std::string& channel, const std::string& thread);
	void CloseThread();
	void UpdateHeader();
	void UpdateTitle();
	void SetStatus(const std::string& text);
	void MarkReadSoon();
	void Notify(const std::string& channel, const std::string& ts);
	void Failed(const std::string& what, const std::string& error);
	void OpenURL(const std::string& url);
	void EditMessage(const std::string& channel, const std::string& ts);

	std::unique_ptr<Session> fSession;
	int fListener = 0;
	std::string fTeamId;
	std::string fSettingsDirectory;
	Sidebar* fSidebar;
	BStringView* fTitle;
	BStringView* fTopic;
	MessageView* fMessages;
	Composer* fComposer;
	BGroupView* fThreadPanel;
	BStringView* fThreadTitle;
	MessageView* fThreadMessages;
	Composer* fThreadComposer;
	BStringView* fStatus;
	BFilePanel* fOpenPanel = nullptr;
	std::string fChannel;
	std::string fThread;
	std::string fConnection;
	std::string fTyping;
	bigtime_t fTypingUntil = 0;
	bigtime_t fLastTypingSent = 0;
	bool fActive = false;
	bool fSidebarDirty = false;
	bool fBootstrapped = false;
};

}  // namespace natter::ui
