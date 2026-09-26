// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include <Messenger.h>
#include <Window.h>

class BButton;
class BListView;
class BStringView;
class BTextControl;

namespace natter {
class Session;
}

namespace natter::ui {

// Searches a workspace's messages (search.messages) and shows a result in
// its conversation: kShowMessage to the workspace window.
class SearchWindow : public BWindow {
public:
	SearchWindow(Session* session, BMessenger workspace, const char* workspaceName);

	void MessageReceived(BMessage* message) override;

private:
	void Search();

	Session* fSession;
	BMessenger fWorkspace;
	BTextControl* fQuery;
	BButton* fSearch;
	BListView* fResults;
	BStringView* fStatus;
	int32 fGeneration = 0;
};

}  // namespace natter::ui
