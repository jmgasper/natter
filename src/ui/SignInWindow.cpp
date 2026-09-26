// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "SignInWindow.h"

#include "Messages.h"

#include "natter/credentials.h"
#include "natter/http.h"
#include "natter/util.h"

#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>

#include <cstdlib>
#include <thread>

namespace natter::ui {

namespace {

enum : uint32 {
	kSignIn = 'sisi',
	kSignInResult = 'sisr',
	kBrowser = 'sibr',
};

BTextView* Explanation(const char* text)
{
	auto* view = new BTextView("explanation");
	view->SetText(text);
	view->MakeEditable(false);
	view->MakeSelectable(false);
	view->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	view->SetWordWrap(true);
	view->SetExplicitMinSize(BSize(420, 70));
	return view;
}

}  // namespace

SignInWindow::SignInWindow()
	:
	BWindow(BRect(0, 0, 520, 360), "Sign in to Slack", B_TITLED_WINDOW,
		B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS)
{
	fTabs = new BTabView("method", B_WIDTH_FROM_LABEL);
	fWorkspace = new BTextControl("Workspace:", "", nullptr);
	fWorkspace->TextView()->SetText("");
	fCookie = new BTextControl("Cookie \"d\":", "", nullptr);
	auto* session = new BGroupView(B_VERTICAL);
	BLayoutBuilder::Group<>(session, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.Add(Explanation("Sign in to your workspace in a web browser, then copy the value of its "
			"\"d\" cookie for slack.com (it starts with xoxd-). Natter uses it as Slack's own web "
			"client does. Enter the workspace's address, such as acme.slack.com."))
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.AddTextControl(fWorkspace, 0, 0)
			.AddTextControl(fCookie, 0, 1)
		.End()
		.AddGlue();
	session->SetName("Browser session");
	fTabs->AddTab(session);

	fToken = new BTextControl("User token:", "", nullptr);
	fAppToken = new BTextControl("App token:", "", nullptr);
	auto* token = new BGroupView(B_VERTICAL);
	BLayoutBuilder::Group<>(token, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.Add(Explanation("With a Slack app of your own, enter its user OAuth token (xoxp-). "
			"An app-level token (xapp-) with Socket Mode enabled gives instant updates; "
			"without one, Natter checks for new messages every few seconds."))
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.AddTextControl(fToken, 0, 0)
			.AddTextControl(fAppToken, 0, 1)
		.End()
		.AddGlue();
	token->SetName("App token");
	fTabs->AddTab(token);

	// Both are passwords to the account.
	fCookie->TextView()->HideTyping(true);
	fToken->TextView()->HideTyping(true);
	fAppToken->TextView()->HideTyping(true);
	fStatus = new BStringView("status", "");
	fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fSignIn = new BButton("Sign in", new BMessage(kSignIn));
	BLayoutBuilder::Group<> layout(this, B_VERTICAL, B_USE_DEFAULT_SPACING);
	layout.SetInsets(B_USE_WINDOW_INSETS);
#if NATTER_WEBKIT
	// Signing in on Slack's own page needs Summit's engine.
	auto* browser = new BButton("Sign in with Slack's web page…", new BMessage(kBrowser));
	browser->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT, B_ALIGN_MIDDLE));
	layout.Add(browser);
#endif
	layout
		.Add(fTabs)
		.AddGroup(B_HORIZONTAL)
			.Add(fStatus)
			.AddGlue()
			.Add(new BButton("Cancel", new BMessage(B_QUIT_REQUESTED)))
			.Add(fSignIn)
		.End();
	SetDefaultButton(fSignIn);
	// As tall as the content needs at this width.
	float width = Bounds().Width();
	float minHeight, maxHeight, height = GetLayout()->PreferredSize().Height();
	if (GetLayout()->HasHeightForWidth())
		GetLayout()->GetHeightForWidth(width, &minHeight, &maxHeight, &height);
	ResizeTo(width, height);
	CenterOnScreen();
	fWorkspace->MakeFocus(true);
}

void SignInWindow::SetBusy(bool busy, const char* status)
{
	fBusy = busy;
	fSignIn->SetEnabled(!busy);
	fStatus->SetText(status);
}

bool SignInWindow::QuitRequested()
{
	BMessage closed(kSignInClosed);
	closed.AddMessenger("window", BMessenger(this));
	be_app->PostMessage(&closed);
	return true;
}

void SignInWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
	case kSignIn: {
		if (fBusy)
			return;
		bool session = fTabs->Selection() == 0;
		std::string workspace = trim(fWorkspace->Text());
		std::string cookie = trim(fCookie->Text());
		std::string token = trim(fToken->Text());
		std::string appToken = trim(fAppToken->Text());
		if (session && (workspace.empty() || cookie.empty())) {
			fStatus->SetText("Enter the workspace and the cookie.");
			return;
		}
		if (!session && token.empty()) {
			fStatus->SetText("Enter a user token.");
			return;
		}
		SetBusy(true, "Signing in…");
		BMessenger target(this);
		// For tests: Slack's API somewhere else (tests/mock/mock_slack.py).
		std::string apiBase = getenv("NATTER_API_BASE") ? getenv("NATTER_API_BASE") : "";
		std::thread([=] {
			auto transport = defaultTransport();
			BMessage result(kSignInResult);
			if (session) {
				auto credentials = signin::signInWithCookie(*transport, workspace, cookie, apiBase);
				if (credentials)
					result.AddString("credentials", credentials->toJson().dump().c_str());
				else
					result.AddString("error", credentials.error().describe().c_str());
			} else {
				Credentials credentials;
				credentials.token = token;
				credentials.appToken = appToken;
				credentials.apiBase = apiBase;
				auto info = signin::validate(*transport, credentials);
				if (info)
					result.AddString("credentials", credentials.toJson().dump().c_str());
				else
					result.AddString("error", info.error().describe().c_str());
			}
			target.SendMessage(&result);
		}).detach();
		break;
	}
	case kBrowser:
		be_app->PostMessage(kBrowserSignIn);
		break;
	case kSignInResult: {
		const char* error;
		if (message->FindString("error", &error) == B_OK) {
			SetBusy(false, error);
			return;
		}
		BMessage signedIn(kSignedIn);
		signedIn.AddString("credentials", message->GetString("credentials", "{}"));
		be_app->PostMessage(&signedIn);
		Quit();
		break;
	}
	default:
		BWindow::MessageReceived(message);
	}
}

}  // namespace natter::ui
