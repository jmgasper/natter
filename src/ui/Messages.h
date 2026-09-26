// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include <SupportDefs.h>

namespace natter::ui {

// BMessage codes shared by the windows and views.
enum : uint32 {
	kSessionEvent = 'nsev',        // a SessionEvent: kind, channel, ts, thread, ...
	kBootstrapDone = 'nbok',       // "error" when it failed, "auth" if the session died
	kHistoryLoaded = 'nhis',       // channel, "older" (bool), "more" (bool)
	kThreadLoaded = 'nthl',        // channel, thread
	kActionFailed = 'nafl',        // "what", "error"
	kChannelSelected = 'nchs',     // channel
	kSendMessage = 'nsnd',         // channel, text, thread (optional)
	kOpenThread = 'nopt',          // channel, thread
	kCloseThread = 'nclt',
	kToggleReaction = 'nrea',      // channel, ts, name
	kEditMessage = 'nedt',         // channel, ts
	kDeleteMessage = 'ndel',       // channel, ts
	kCopyText = 'ncpy',            // text
	kOpenLink = 'nlnk',            // url
	kOpenChannel = 'nopc',         // channel
	kOpenUser = 'nopu',            // user
	kLoadOlder = 'nold',           // channel, thread (optional)
	kDownloadFile = 'ndlf',        // url, name
	kImageLoaded = 'nimg',         // url
	kTyping = 'ntyp',              // from the composer
	kFilterChanged = 'nflt',
	kSignedIn = 'nsin',            // "credentials" (JSON) from the sign-in window
	kSignInClosed = 'nsic',
	kBrowserSignIn = 'nsbr',       // sign in on Slack's web page instead
	kAddWorkspace = 'nadd',
	kOpenWorkspace = 'nopw',       // "team"
	kSignOut = 'nsgo',
	kRefresh = 'nref',
	kMarkRead = 'nmrd',
	kUploadFile = 'nupl',
	kFileChosen = 'nfch',          // refs from the file panel
	kJumpTo = 'njmp',
	kAbout = 'nabt',
};

}  // namespace natter::ui
