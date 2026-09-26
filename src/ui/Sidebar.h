// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include <View.h>

#include <string>

class BListView;
class BStringView;
class BTextControl;

namespace natter {
class Session;
}

namespace natter::ui {

// The workspace's conversations: channels, then direct messages, with unread
// ones in bold and a badge for mentions. Selecting one sends
// kChannelSelected with "channel" to the window.
class Sidebar : public BView {
public:
	explicit Sidebar(Session* session);

	void Reload();
	void Select(const std::string& channel);
	std::string Selected() const;

	void AttachedToWindow() override;
	void MessageReceived(BMessage* message) override;

private:
	Session* fSession;
	BStringView* fTeam;
	BTextControl* fFilter;
	BListView* fList;
	std::string fSelected;
	bool fReloading = false;
};

}  // namespace natter::ui
