// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include <Window.h>

class BButton;
class BStringView;
class BTabView;
class BTextControl;

namespace natter::ui {

// Signs in to a workspace, and sends kSignedIn with the validated
// credentials (JSON) to the application.
//  - Session: the workspace address and the browser's "d" cookie, as
//    Slack's own web client uses; Natter finds the session token itself.
//  - Token: a user token (xoxp-) from a Slack app of your own, and
//    optionally an app-level token (xapp-) for Socket Mode.
class SignInWindow : public BWindow {
public:
	SignInWindow();

	void MessageReceived(BMessage* message) override;
	bool QuitRequested() override;

private:
	void SetBusy(bool busy, const char* status);

	BTabView* fTabs;
	BTextControl* fWorkspace;
	BTextControl* fCookie;
	BTextControl* fToken;
	BTextControl* fAppToken;
	BButton* fSignIn;
	BStringView* fStatus;
	bool fBusy = false;
};

}  // namespace natter::ui
