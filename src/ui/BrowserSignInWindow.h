// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include <Window.h>

#include <memory>
#include <string>

class BStringView;
class BWebKitContext;
class BWebKitView;

namespace natter::ui {

// Slack's own sign-in page, in Summit's WebKit engine and a private web
// profile. Once the web client has loaded, Natter reads its session tokens
// (localStorage "localConfig_v2") and the "d" cookie, which is HttpOnly and
// therefore only the engine can see, and sends kSignedIn for each workspace.
// Built when Natter is compiled against the engine (NATTER_WEBKIT).
class BrowserSignInWindow : public BWindow {
public:
	BrowserSignInWindow();
	~BrowserSignInWindow() override;

	static bool Available();

	void MessageReceived(BMessage* message) override;
	bool QuitRequested() override;

private:
	void Check();

	std::shared_ptr<BWebKitContext> fContext;
	BWebKitView* fView;
	BStringView* fStatus;
	std::string fTokens;
	bool fChecking = false;
	bool fDone = false;
};

}  // namespace natter::ui
