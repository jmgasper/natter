// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "BrowserSignInWindow.h"

#include "Messages.h"
#include "natter/credentials.h"
#include "natter/http.h"

#include <Application.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <WebKit/WebKitContext.h>
#include <WebKit/WebKitEmbedding.h>
#include <WebKit/WebKitView.h>

#include <thread>

namespace natter::ui {

namespace {

enum : uint64 {
	kReadTokens = 1,
	kReadCookies = 2,
};

enum : uint32 {
	kSignInResult = 'bsir',
};

}  // namespace

bool BrowserSignInWindow::Available()
{
	return true;
}

BrowserSignInWindow::BrowserSignInWindow()
	:
	BWindow(BRect(0, 0, 900, 700), "Sign in to Slack", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS)
{
	// A private profile: nothing of the sign-in outlives this window.
	fContext = std::make_shared<BWebKitContext>(nullptr, true);
	fView = new BWebKitView(BRect(0, 0, 900, 660), "slack sign-in", BMessenger(this), B_FOLLOW_ALL, fContext);
	fView->SetExplicitMinSize(BSize(600, 400));
	fStatus = new BStringView("status", "Sign in to your workspace. Natter continues once Slack has opened it.");
	fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(fView, 1)
		.AddGroup(B_HORIZONTAL)
			.SetInsets(8, 4, 8, 4)
			.Add(fStatus)
		.End();
	CenterOnScreen();
	fView->LoadURL("https://slack.com/signin");
}

BrowserSignInWindow::~BrowserSignInWindow()
{
}

bool BrowserSignInWindow::QuitRequested()
{
	BMessage closed(kSignInClosed);
	closed.AddMessenger("window", BMessenger(this));
	be_app->PostMessage(&closed);
	return true;
}

void BrowserSignInWindow::Check()
{
	if (fChecking || fDone)
		return;
	fChecking = true;
	fView->EvaluateJavaScript("return localStorage.getItem('localConfig_v2') || ''", BMessenger(this), kReadTokens, true);
}

void BrowserSignInWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
	case B_WEBKIT_STATE_CHANGED: {
		std::string url = message->GetString("url", "");
		// The web client lives at app.slack.com/client/<team>/...
		if (url.find("app.slack.com/client") != std::string::npos && !message->GetBool("loading", true))
			Check();
		break;
	}
	case B_WEBKIT_JAVASCRIPT_RESULT: {
		if (message->GetUInt64("identifier", 0) != kReadTokens)
			break;
		fChecking = false;
		std::string tokens = message->GetString("result", "");
		if (tokens.empty() || tokens.find("xoxc-") == std::string::npos)
			break;   // not signed in yet
		fTokens = tokens;
		fChecking = true;
		fView->GetCookies("https://slack.com/", BMessenger(this), kReadCookies);
		break;
	}
	case B_WEBKIT_COOKIES: {
		if (message->GetUInt64("identifier", 0) != kReadCookies)
			break;
		std::string cookie;
		BMessage entry;
		for (int32 index = 0; message->FindMessage("cookie", index, &entry) == B_OK; index++) {
			if (std::string(entry.GetString("name", "")) == "d")
				cookie = entry.GetString("value", "");
		}
		auto teams = signin::parseLocalConfig(fTokens);
		if (cookie.empty() || !teams || teams->empty()) {
			fChecking = false;
			fStatus->SetText("Slack opened, but its session could not be read. Try again, or use a session cookie.");
			break;
		}
		fDone = true;
		fStatus->SetText("Signed in; checking with Slack…");
		std::vector<signin::LocalConfigTeam> list = teams.value();
		BMessenger target(this);
		std::thread([list, cookie, target] {
			auto transport = defaultTransport();
			for (const auto& team : list) {
				Credentials credentials;
				credentials.token = team.token;
				credentials.cookie = normalizeCookie(cookie);
				credentials.workspace = team.url.empty() ? team.domain + ".slack.com" : team.url;
				auto info = signin::validate(*transport, credentials);
				BMessage result(kSignInResult);
				if (info)
					result.AddString("credentials", credentials.toJson().dump().c_str());
				else
					result.AddString("error", (team.name + ": " + info.error().describe()).c_str());
				target.SendMessage(&result);
			}
			BMessage finished(kSignInResult);
			finished.AddBool("finished", true);
			target.SendMessage(&finished);
		}).detach();
		break;
	}
	case kSignInResult: {
		if (message->GetBool("finished", false)) {
			PostMessage(B_QUIT_REQUESTED);
			break;
		}
		const char* error;
		if (message->FindString("error", &error) == B_OK) {
			fStatus->SetText(error);
			break;
		}
		BMessage signedIn(kSignedIn);
		signedIn.AddString("credentials", message->GetString("credentials", "{}"));
		be_app->PostMessage(&signedIn);
		break;
	}
	default:
		BWindow::MessageReceived(message);
	}
}

}  // namespace natter::ui
