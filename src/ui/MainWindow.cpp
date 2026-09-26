// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "MainWindow.h"

#include "Composer.h"
#include "ImageCache.h"
#include "MessageView.h"
#include "SearchWindow.h"
#include "Messages.h"
#include "Sidebar.h"
#include "Theme.h"
#include "natter/session.h"
#include "natter/util.h"

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <Clipboard.h>
#include <Entry.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Notification.h>
#include <Path.h>
#include <Roster.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <StringView.h>
#include <TextView.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace natter::ui {

namespace {

enum : uint32 {
	kSidebarTick = 'nstk',
	kEditSave = 'neds',
	kDownloaded = 'ndld',
	kUploaded = 'nupd',
	kSent = 'nsnt',
	kQuit = 'nqit',
};

const char* realtimeSourceName(EventSource source)
{
	switch (source) {
	case EventSource::Rtm:
		return "real-time messaging";
	case EventSource::SocketMode:
		return "Socket Mode";
	case EventSource::Polling:
		return "polling";
	default:
		return "local";
	}
}

std::string DownloadsDirectory()
{
	BPath path;
	if (find_directory(B_USER_DIRECTORY, &path) != B_OK)
		return "/boot/home";
	path.Append("Downloads");
	std::error_code error;
	std::filesystem::create_directories(path.Path(), error);
	return path.Path();
}

// A small window to edit one of your messages.
class EditWindow : public BWindow {
public:
	EditWindow(BWindow* owner, const std::string& channel, const std::string& ts, const std::string& text)
		:
		BWindow(BRect(0, 0, 460, 180), "Edit message", B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
			B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
		fOwner(owner),
		fChannel(channel),
		fTs(ts)
	{
		AddToSubset(owner);
		fText = new BTextView("text");
		fText->SetText(text.c_str());
		fText->SetWordWrap(true);
		fText->SetInsets(4, 4, 4, 4);
		auto* scroll = new BScrollView("scroll", fText, 0, false, true);
		scroll->SetExplicitMinSize(BSize(420, 100));
		auto* save = new BButton("Save", new BMessage(kEditSave));
		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
			.SetInsets(B_USE_WINDOW_INSETS)
			.Add(scroll)
			.AddGroup(B_HORIZONTAL)
				.AddGlue()
				.Add(new BButton("Cancel", new BMessage(B_QUIT_REQUESTED)))
				.Add(save)
			.End();
		SetDefaultButton(save);
		CenterIn(owner->Frame());
		fText->MakeFocus(true);
	}

	void MessageReceived(BMessage* message) override
	{
		if (message->what == kEditSave) {
			BMessage save(kEditSave);
			save.AddString("channel", fChannel.c_str());
			save.AddString("ts", fTs.c_str());
			save.AddString("text", fText->Text());
			fOwner->PostMessage(&save);
			Quit();
			return;
		}
		BWindow::MessageReceived(message);
	}

private:
	BWindow* fOwner;
	std::string fChannel;
	std::string fTs;
	BTextView* fText;
};

}  // namespace

MainWindow::MainWindow(const Credentials& credentials, const std::string& settingsDirectory)
	:
	BWindow(BRect(80, 60, 1180, 800), "Natter", B_DOCUMENT_WINDOW, B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
	fTeamId(credentials.teamId),
	fSettingsDirectory(settingsDirectory)
{
	SessionOptions options;
	if (!credentials.teamId.empty())
		options.cacheDirectory = settingsDirectory + "/cache/" + credentials.teamId;
	fSession = std::make_unique<Session>(credentials, options);
	Session* session = fSession.get();

	ImageCache::Shared().SetAuthenticatedFetcher([api = session->api()](const std::string& url, std::string& data) {
		auto result = api->download(url);
		if (!result)
			return false;
		data = std::move(result.value());
		return true;
	});

	const Theme& theme = Theme::Current();
	BMenu* workspaces = nullptr;
	BuildMenu(workspaces);

	fSidebar = new Sidebar(session);
	fTitle = new BStringView("title", "");
	BFont titleFont = theme.bold;
	titleFont.SetSize(theme.plain.Size() * 1.25f);
	fTitle->SetFont(&titleFont);
	fTopic = new BStringView("topic", "");
	fTopic->SetHighColor(theme.muted);
	fTopic->SetExplicitMinSize(BSize(50, B_SIZE_UNSET));
	// A BStringView is only as wide as its text at most; with
	// B_AUTO_UPDATE_SIZE_LIMITS that would cap the whole window's width.
	fTitle->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fTopic->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fMessages = new MessageView(session, false);
	auto* messagesScroll = new BScrollView("messages scroll", fMessages, 0, false, true, B_NO_BORDER);
	fComposer = new Composer("composer");

	fThreadTitle = new BStringView("thread title", "Thread");
	fThreadTitle->SetFont(&titleFont);
	fThreadTitle->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fThreadMessages = new MessageView(session, true);
	auto* threadScroll = new BScrollView("thread scroll", fThreadMessages, 0, false, true, B_NO_BORDER);
	fThreadComposer = new Composer("thread composer");
	fThreadPanel = new BGroupView(B_VERTICAL, 0);
	BLayoutBuilder::Group<>(fThreadPanel)
		.AddGroup(B_HORIZONTAL)
			.SetInsets(10, 8, 8, 8)
			.Add(fThreadTitle)
			.AddGlue()
			.Add(new BButton("close thread", "Close", new BMessage(kCloseThread)))
		.End()
		.Add(threadScroll, 1)
		.Add(fThreadComposer);
	fThreadPanel->SetExplicitMinSize(BSize(260, B_SIZE_UNSET));

	auto* conversation = new BGroupView(B_VERTICAL, 0);
	BLayoutBuilder::Group<>(conversation)
		.AddGroup(B_VERTICAL, 2)
			.SetInsets(12, 8, 12, 8)
			.Add(fTitle)
			.Add(fTopic)
		.End()
		.Add(messagesScroll, 1)
		.Add(fComposer);

	fStatus = new BStringView("status", "Connecting…");
	fStatus->SetFont(&theme.small);
	fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	auto* split = new BSplitView(B_HORIZONTAL, 0);
	BLayoutBuilder::Split<>(split)
		.Add(fSidebar, 0.22f)
		.Add(conversation, 0.53f)
		.Add(fThreadPanel, 0.25f);
	split->SetCollapsible(false);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(KeyMenuBar())
		.Add(split, 1)
		.AddGroup(B_HORIZONTAL)
			.SetInsets(8, 2, 8, 3)
			.Add(fStatus)
		.End();
	fThreadPanel->Hide();

	fListener = session->addListener([messenger = BMessenger(this)](const natter::SessionEvent& event) {
		BMessage message(kSessionEvent);
		message.AddInt32("kind", event.change.kind);
		message.AddString("channel", event.change.channel.c_str());
		message.AddString("ts", event.change.ts.c_str());
		message.AddString("thread", event.change.threadTs.c_str());
		message.AddBool("newUnread", event.change.newUnread);
		message.AddBool("mentionsSelf", event.change.mentionsSelf);
		message.AddInt32("source", static_cast<int32>(event.source));
		if (auto* connection = std::get_if<ConnectionEvent>(&event.event)) {
			message.AddInt32("connection", static_cast<int32>(connection->state));
			message.AddString("connectionSource", realtimeSourceName(connection->source));
			message.AddString("detail", connection->detail.c_str());
			message.AddInt32("retry", connection->retryInSeconds);
		} else if (auto* typing = std::get_if<TypingEvent>(&event.event)) {
			message.AddString("typingChannel", typing->channel.c_str());
			message.AddString("typingUser", typing->user.c_str());
		}
		messenger.SendMessage(&message, static_cast<BHandler*>(nullptr), 200000);
	});

	// The cache draws the last session at once; the network brings it up to date.
	session->loadCache();
	fSidebar->Reload();
	UpdateTitle();
	session->post([session, messenger = BMessenger(this)] {
		Status status = session->bootstrap();
		BMessage done(kBootstrapDone);
		if (!status) {
			done.AddString("error", status.error().describe().c_str());
			done.AddBool("auth", status.error().isAuth());
		}
		messenger.SendMessage(&done);
		if (status)
			session->startRealtime();
	});
	BMessage tick(kSidebarTick);
	new BMessageRunner(BMessenger(this), &tick, 500000);
}

MainWindow::~MainWindow()
{
	ImageCache::Shared().SetAuthenticatedFetcher({});
	if (fSession) {
		fSession->removeListener(fListener);
		fSession->saveCache();
	}
	delete fOpenPanel;
}

void MainWindow::BuildMenu(BMenu*& workspaces)
{
	auto* bar = new BMenuBar("menu");
	auto* file = new BMenu("File");
	file->AddItem(new BMenuItem("Jump to conversation…", new BMessage(kJumpTo), 'K'));
	file->AddItem(new BMenuItem("Search messages…", new BMessage(kSearch), 'F'));
	file->AddItem(new BMenuItem("Refresh", new BMessage(kRefresh), 'R'));
	file->AddSeparatorItem();
	file->AddItem(new BMenuItem("Add workspace…", new BMessage(kAddWorkspace)));
	file->AddItem(new BMenuItem("Sign out of this workspace", new BMessage(kSignOut)));
	file->AddSeparatorItem();
	file->AddItem(new BMenuItem("About Natter", new BMessage(kAbout)));
	file->AddItem(new BMenuItem("Close", new BMessage(B_QUIT_REQUESTED), 'W'));
	file->AddItem(new BMenuItem("Quit", new BMessage(kQuit), 'Q'));
	bar->AddItem(file);
	auto* edit = new BMenu("Edit");
	edit->AddItem(new BMenuItem("Cut", new BMessage(B_CUT), 'X'));
	edit->AddItem(new BMenuItem("Copy", new BMessage(B_COPY), 'C'));
	edit->AddItem(new BMenuItem("Paste", new BMessage(B_PASTE), 'V'));
	edit->AddItem(new BMenuItem("Select all", new BMessage(B_SELECT_ALL), 'A'));
	edit->SetTargetForItems(static_cast<BHandler*>(nullptr));
	bar->AddItem(edit);
	auto* view = new BMenu("Conversation");
	view->AddItem(new BMenuItem("Mark as read", new BMessage(kMarkRead)));
	view->AddItem(new BMenuItem("Upload file…", new BMessage(kUploadFile), 'U'));
	view->AddItem(new BMenuItem("Close thread", new BMessage(kCloseThread)));
	bar->AddItem(view);
	AddChild(bar);
	SetKeyMenuBar(bar);
	workspaces = nullptr;
}

void MainWindow::UpdateTitle()
{
	std::string team = fSession->store().team().name;
	if (team.empty())
		team = fSession->credentials().teamName;
	std::string title = "Natter";
	if (!fChannel.empty())
		title = fSession->store().channelDisplayName(fChannel) + " – " + (team.empty() ? "Natter" : team);
	else if (!team.empty())
		title = team + " – Natter";
	SetTitle(title.c_str());
}

void MainWindow::UpdateHeader()
{
	if (fChannel.empty()) {
		fTitle->SetText("");
		fTopic->SetText("");
		return;
	}
	Store& store = fSession->store();
	fTitle->SetText(store.channelDisplayName(fChannel).c_str());
	std::string topic;
	if (auto channel = store.channel(fChannel)) {
		topic = channel->topic.value.empty() ? channel->purpose.value : channel->topic.value;
		if (channel->isIm) {
			if (auto user = store.user(channel->imUser)) {
				topic = user->title;
				if (!user->statusText.empty())
					topic = user->statusText + (topic.empty() ? "" : " · " + topic);
			}
		}
	}
	fTopic->SetText(formatMrkdwn(topic, fSession->formatContext()).plainText().c_str());
}

void MainWindow::SetStatus(const std::string& text)
{
	fStatus->SetText(text.c_str());
}

void MainWindow::SelectConversation(const std::string& channel)
{
	if (channel.empty())
		return;
	// A thread belongs to its conversation.
	if (!fThread.empty() && channel != fChannel)
		CloseThread();
	fChannel = channel;
	fSidebar->Select(channel);
	fMessages->SetConversation(channel);
	std::string placeholder = "Message " + fSession->store().channelDisplayName(channel);
	fComposer->SetConversation(channel, "", placeholder);
	fComposer->Focus();
	UpdateHeader();
	UpdateTitle();
	fSession->setActiveConversation(channel, fThread.empty() ? std::string() : fThread);
	Session* session = fSession.get();
	fMessages->SetLoadingOlder(true, true);
	session->post([session, channel, messenger = BMessenger(this)] {
		auto page = session->loadHistory(channel);
		BMessage loaded(kHistoryLoaded);
		loaded.AddString("channel", channel.c_str());
		loaded.AddBool("more", page && page->hasMore);
		if (!page)
			loaded.AddString("error", page.error().describe().c_str());
		messenger.SendMessage(&loaded);
	});
	MarkReadSoon();
	// Remember it for the next start.
	BMessage settings(kChannelSelected);
	settings.AddString("team", fTeamId.c_str());
	settings.AddString("channel", channel.c_str());
	be_app->PostMessage(&settings);
}

void MainWindow::ShowMessage(const std::string& channel, const std::string& ts, const std::string& thread)
{
	if (channel.empty() || ts.empty())
		return;
	Activate(true);
	if (channel != fChannel)
		SelectConversation(channel);
	if (!thread.empty()) {
		// A reply: its thread, with the parent in view if it is loaded.
		fRevealReply = ts;
		OpenThread(channel, thread);
		fMessages->Reveal(thread);
		return;
	}
	fReveal = ts;
	if (fMessages->Reveal(ts)) {
		fReveal.clear();
		return;
	}
	// Not loaded: the messages up to it and a page after it.
	Session* session = fSession.get();
	session->post([session, channel, ts, messenger = BMessenger(this)] {
		HistoryOptions before;
		before.latest = ts;
		before.inclusive = true;
		before.limit = 30;
		auto page = session->loadHistory(channel, before);
		HistoryOptions after;
		after.oldest = ts;
		after.limit = 30;
		session->loadHistory(channel, after);
		BMessage loaded(kHistoryLoaded);
		loaded.AddString("channel", channel.c_str());
		loaded.AddBool("more", page && page->hasMore);
		if (!page)
			loaded.AddString("error", page.error().describe().c_str());
		messenger.SendMessage(&loaded);
	});
}

void MainWindow::MarkReadSoon()
{
	if (fChannel.empty() || !fActive)
		return;
	std::string newest = fSession->store().newestTs(fChannel);
	auto channel = fSession->store().channel(fChannel);
	if (newest.empty() || !channel || (!channel->lastRead.empty() && compareTs(channel->lastRead, newest) >= 0))
		return;
	Session* session = fSession.get();
	std::string id = fChannel;
	session->post([session, id, newest] { session->markRead(id, newest); });
}

void MainWindow::OpenThread(const std::string& channel, const std::string& thread)
{
	fThread = thread;
	if (fThreadPanel->IsHidden(fThreadPanel))
		fThreadPanel->Show();
	fThreadMessages->SetConversation(channel, thread);
	fThreadTitle->SetText(("Thread in " + fSession->store().channelDisplayName(channel)).c_str());
	fThreadComposer->SetConversation(channel, thread, "Reply…");
	fThreadComposer->Focus();
	fSession->setActiveConversation(fChannel, thread);
	Session* session = fSession.get();
	session->post([session, channel, thread, messenger = BMessenger(this)] {
		auto page = session->loadThread(channel, thread);
		BMessage loaded(kThreadLoaded);
		loaded.AddString("channel", channel.c_str());
		loaded.AddString("thread", thread.c_str());
		if (!page)
			loaded.AddString("error", page.error().describe().c_str());
		messenger.SendMessage(&loaded);
	});
}

void MainWindow::CloseThread()
{
	fThread.clear();
	if (!fThreadPanel->IsHidden(fThreadPanel))
		fThreadPanel->Hide();
	fSession->setActiveConversation(fChannel);
	fComposer->Focus();
}

void MainWindow::Failed(const std::string& what, const std::string& error)
{
	std::string text = what + "\n\n" + error;
	auto* alert = new BAlert("Natter", text.c_str(), "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->Go(nullptr);
}

void MainWindow::OpenURL(const std::string& url)
{
	std::string scheme = url.substr(0, url.find(':'));
	if (scheme.empty() || scheme.size() == url.size())
		return;
	std::string mime = "application/x-vnd.Be.URL." + scheme;
	const char* arguments[] = { url.c_str() };
	be_roster->Launch(mime.c_str(), 1, const_cast<char**>(arguments));
}

void MainWindow::Notify(const std::string& channel, const std::string& ts)
{
	auto message = fSession->store().message(channel, ts);
	if (!message)
		return;
	Store& store = fSession->store();
	std::string sender = store.userName(message->user);
	std::string where = store.channelDisplayName(channel);
	auto conversation = store.channel(channel);
	std::string title = conversation && conversation->isIm ? sender : sender + " in " + where;
	std::string body = formatMessage(*message, fSession->formatContext()).plainText();
	BNotification notification(B_INFORMATION_NOTIFICATION);
	notification.SetGroup("Natter");
	notification.SetTitle(title.c_str());
	notification.SetContent(body.c_str());
	notification.SetMessageID(("natter-" + channel).c_str());
	notification.Send();
}

void MainWindow::EditMessage(const std::string& channel, const std::string& ts)
{
	auto message = fSession->store().message(channel, ts);
	if (!message) {
		for (const Message& reply : fSession->store().thread(channel, fThread)) {
			if (reply.ts == ts)
				message = reply;
		}
	}
	if (!message)
		return;
	(new EditWindow(this, channel, ts, editableText(message->text, fSession->store().formatContext())))->Show();
}

void MainWindow::SessionEvent(BMessage* message)
{
	int32 kind = message->GetInt32("kind", StoreChange::None);
	std::string channel = message->GetString("channel", "");
	std::string thread = message->GetString("thread", "");
	std::string ts = message->GetString("ts", "");
	int32 connection;
	if (message->FindInt32("connection", &connection) == B_OK) {
		auto state = static_cast<ConnectionState>(connection);
		std::string source = message->GetString("connectionSource", "");
		std::string detail = message->GetString("detail", "");
		if (state == ConnectionState::Connected)
			fConnection = "Connected (" + source + ")";
		else if (state == ConnectionState::Polling)
			fConnection = "Connected (checking every few seconds)";
		else if (state == ConnectionState::Connecting)
			fConnection = "Connecting…";
		else {
			int32 retry = message->GetInt32("retry", 0);
			fConnection = "Offline" + (retry > 0 ? " – retrying in " + std::to_string(retry) + " s" : std::string())
				+ (detail.empty() ? "" : " (" + detail + ")");
		}
		SetStatus(fConnection);
		return;
	}
	const char* typingUser;
	if (message->FindString("typingUser", &typingUser) == B_OK) {
		if (message->GetString("typingChannel", "") == fChannel) {
			fTyping = fSession->store().userName(typingUser) + " is typing…";
			fTypingUntil = system_time() + 5000000;
			SetStatus(fTyping);
		}
		return;
	}
	switch (kind) {
	case StoreChange::Messages:
		if (channel == fChannel) {
			fMessages->Reload();
			if (fActive)
				MarkReadSoon();
		}
		if (!fThread.empty() && channel == fChannel && ts == fThread)
			fThreadMessages->Reload();
		fSidebarDirty = true;
		break;
	case StoreChange::Thread:
		if (channel == fChannel) {
			fMessages->Reload();
			if (!fThread.empty() && (thread == fThread || ts == fThread))
				fThreadMessages->Reload();
		}
		break;
	case StoreChange::Users:
	case StoreChange::Emoji:
		fMessages->Reload();
		if (!fThread.empty())
			fThreadMessages->Reload();
		fSidebarDirty = true;
		break;
	case StoreChange::Channels:
	case StoreChange::ReadState:
	case StoreChange::Team:
		fSidebarDirty = true;
		if (channel == fChannel || channel.empty())
			UpdateHeader();
		break;
	default:
		break;
	}
	if (message->GetBool("newUnread", false)) {
		auto conversation = fSession->store().channel(channel);
		bool direct = conversation && conversation->isDirect();
		bool visible = fActive && channel == fChannel;
		if (!visible && (direct || message->GetBool("mentionsSelf", false)))
			Notify(channel, ts);
	}
}

void MainWindow::WindowActivated(bool active)
{
	BWindow::WindowActivated(active);
	fActive = active;
	if (active)
		MarkReadSoon();
}

void MainWindow::MessageReceived(BMessage* message)
{
	Session* session = fSession.get();
	switch (message->what) {
	case kSessionEvent:
		SessionEvent(message);
		break;
	case kSidebarTick:
		if (fSidebarDirty) {
			fSidebarDirty = false;
			fSidebar->Reload();
		}
		if (fTypingUntil && system_time() > fTypingUntil) {
			fTypingUntil = 0;
			SetStatus(fConnection);
		}
		break;
	case kBootstrapDone: {
		const char* error;
		if (message->FindString("error", &error) == B_OK) {
			if (message->GetBool("auth", false)) {
				Failed("This workspace's sign-in has expired.", "Use File ▸ Add workspace… to sign in again.");
			} else {
				SetStatus(std::string("Could not reach Slack: ") + error);
			}
			break;
		}
		fBootstrapped = true;
		fSidebar->Reload();
		UpdateTitle();
		if (fChannel.empty()) {
			BMessage question(kOpenChannel);
			question.AddString("team", fTeamId.c_str());
			// The application remembers the last conversation per workspace.
			BMessage reply;
			be_app_messenger.SendMessage(&question, &reply, 1000000, 1000000);
			std::string last = reply.GetString("channel", "");
			if (last.empty() || !session->store().channel(last)) {
				for (const Channel& channel : session->store().channels()) {
					if (channel.isGeneral || (last.empty() && channel.isMember && !channel.isDirect())) {
						last = channel.id;
						if (channel.isGeneral)
							break;
					}
				}
			}
			SelectConversation(last);
		}
		break;
	}
	case kHistoryLoaded: {
		std::string channel = message->GetString("channel", "");
		if (channel == fChannel) {
			fMessages->SetLoadingOlder(false, message->GetBool("more", false));
			fMessages->Reload();
			if (!fReveal.empty() && fMessages->Reveal(fReveal))
				fReveal.clear();
			MarkReadSoon();
		}
		const char* error;
		if (message->FindString("error", &error) == B_OK)
			SetStatus(std::string("Could not load messages: ") + error);
		break;
	}
	case kThreadLoaded:
		if (message->GetString("thread", "") == fThread) {
			fThreadMessages->Reload();
			if (!fRevealReply.empty() && fThreadMessages->Reveal(fRevealReply))
				fRevealReply.clear();
		}
		// The parent's reply count and last reply may have changed.
		if (message->GetString("channel", "") == fChannel)
			fMessages->Reload();
		break;
	case kChannelSelected:
		SelectConversation(message->GetString("channel", ""));
		break;
	case kSendMessage: {
		std::string channel = message->GetString("channel", "");
		std::string thread = message->GetString("thread", "");
		std::string text = encodeMessageText(message->GetString("text", ""), session->store().encodeContext());
		// Show the end of the conversation, where the message will appear.
		(thread.empty() ? fMessages : fThreadMessages)->ScrollToEnd();
		session->post([session, channel, thread, text, messenger = BMessenger(this)] {
			PostOptions options;
			options.threadTs = thread;
			auto sent = session->send(channel, text, options);
			if (!sent) {
				BMessage failed(kActionFailed);
				failed.AddString("what", "The message could not be sent.");
				failed.AddString("error", sent.error().describe().c_str());
				messenger.SendMessage(&failed);
			}
		});
		break;
	}
	case kActionFailed:
		Failed(message->GetString("what", "Something went wrong."), message->GetString("error", ""));
		break;
	case kOpenThread:
		OpenThread(message->GetString("channel", ""), message->GetString("thread", ""));
		break;
	case kCloseThread:
		CloseThread();
		break;
	case kToggleReaction: {
		std::string channel = message->GetString("channel", "");
		std::string ts = message->GetString("ts", "");
		std::string name = message->GetString("name", "");
		bool add = true;
		std::optional<Message> target = session->store().message(channel, ts);
		if (!target && !fThread.empty()) {
			for (const Message& reply : session->store().thread(channel, fThread)) {
				if (reply.ts == ts)
					target = reply;
			}
		}
		std::string me = session->store().selfUserId();
		if (target) {
			for (const Reaction& reaction : target->reactions) {
				if (reaction.name == name && std::find(reaction.users.begin(), reaction.users.end(), me) != reaction.users.end())
					add = false;
			}
		}
		session->post([session, channel, ts, name, add, messenger = BMessenger(this)] {
			Status status = session->react(channel, ts, name, add);
			if (!status) {
				BMessage failed(kActionFailed);
				failed.AddString("what", "The reaction could not be changed.");
				failed.AddString("error", status.error().describe().c_str());
				messenger.SendMessage(&failed);
			}
		});
		break;
	}
	case kEditMessage:
		EditMessage(message->GetString("channel", ""), message->GetString("ts", ""));
		break;
	case kEditSave: {
		std::string channel = message->GetString("channel", ""), ts = message->GetString("ts", "");
		std::string text = encodeMessageText(message->GetString("text", ""), session->store().encodeContext());
		session->post([session, channel, ts, text, messenger = BMessenger(this)] {
			auto edited = session->edit(channel, ts, text);
			if (!edited) {
				BMessage failed(kActionFailed);
				failed.AddString("what", "The message could not be edited.");
				failed.AddString("error", edited.error().describe().c_str());
				messenger.SendMessage(&failed);
			}
		});
		break;
	}
	case kDeleteMessage: {
		auto* alert = new BAlert("Delete message", "Delete this message? This cannot be undone.", "Cancel", "Delete",
			nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		if (alert->Go() != 1)
			break;
		std::string channel = message->GetString("channel", ""), ts = message->GetString("ts", "");
		session->post([session, channel, ts, messenger = BMessenger(this)] {
			Status status = session->remove(channel, ts);
			if (!status) {
				BMessage failed(kActionFailed);
				failed.AddString("what", "The message could not be deleted.");
				failed.AddString("error", status.error().describe().c_str());
				messenger.SendMessage(&failed);
			}
		});
		break;
	}
	case kCopyText:
		if (be_clipboard->Lock()) {
			be_clipboard->Clear();
			const char* text = message->GetString("text", "");
			be_clipboard->Data()->AddData("text/plain", B_MIME_TYPE, text, strlen(text));
			be_clipboard->Commit();
			be_clipboard->Unlock();
		}
		break;
	case kOpenLink:
		OpenURL(message->GetString("url", ""));
		break;
	case kOpenChannel:
		if (message->HasString("channel"))
			SelectConversation(message->GetString("channel", ""));
		break;
	case kOpenUser: {
		std::string user = message->GetString("user", "");
		session->post([session, user, messenger = BMessenger(this)] {
			auto channel = session->openDirectMessage({ user });
			if (channel) {
				BMessage open(kOpenChannel);
				open.AddString("channel", channel->id.c_str());
				messenger.SendMessage(&open);
			}
		});
		break;
	}
	case kLoadOlder: {
		std::string channel = message->GetString("channel", "");
		std::string thread = message->GetString("thread", "");
		if (!thread.empty())
			break;   // threads load whole
		std::string oldest = session->store().oldestTs(channel);
		session->post([session, channel, oldest, messenger = BMessenger(this)] {
			HistoryOptions options;
			options.latest = oldest;
			auto page = session->loadHistory(channel, options);
			BMessage loaded(kHistoryLoaded);
			loaded.AddString("channel", channel.c_str());
			loaded.AddBool("more", page && page->hasMore && !page->messages.empty());
			messenger.SendMessage(&loaded);
		});
		break;
	}
	case kDownloadFile: {
		std::string url = message->GetString("url", "");
		std::string name = message->GetString("name", "download");
		std::string path = DownloadsDirectory() + "/" + name;
		SetStatus("Downloading " + name + "…");
		session->post([session, url, path, messenger = BMessenger(this)] {
			Status status = session->api()->downloadToFile(url, path, {});
			BMessage done(kDownloaded);
			done.AddString("path", path.c_str());
			if (!status)
				done.AddString("error", status.error().describe().c_str());
			messenger.SendMessage(&done);
		});
		break;
	}
	case kDownloaded: {
		const char* error;
		std::string path = message->GetString("path", "");
		if (message->FindString("error", &error) == B_OK) {
			SetStatus(fConnection);
			Failed("The file could not be downloaded.", error);
			break;
		}
		SetStatus("Saved " + path);
		entry_ref ref;
		if (get_ref_for_path(path.c_str(), &ref) == B_OK)
			be_roster->Launch(&ref);
		break;
	}
	case kUploadFile: {
		if (!fOpenPanel) {
			BMessenger target(this);
			fOpenPanel = new BFilePanel(B_OPEN_PANEL, &target, nullptr, B_FILE_NODE, false, new BMessage(kFileChosen));
		}
		BMessage* chosen = new BMessage(kFileChosen);
		chosen->AddString("channel", message->GetString("channel", fChannel.c_str()));
		chosen->AddString("thread", message->GetString("thread", ""));
		fOpenPanel->SetMessage(chosen);
		delete chosen;
		fOpenPanel->Window()->SetTitle("Natter: Upload a file");
		fOpenPanel->Show();
		break;
	}
	case kFileChosen: {
		entry_ref ref;
		if (message->FindRef("refs", &ref) != B_OK)
			break;
		BPath path(&ref);
		std::string channel = message->GetString("channel", fChannel.c_str());
		std::string thread = message->GetString("thread", "");
		std::string file = path.Path();
		SetStatus(std::string("Uploading ") + path.Leaf() + "…");
		session->post([session, channel, thread, file, messenger = BMessenger(this)] {
			UploadOptions options;
			options.threadTs = thread;
			auto uploaded = session->api()->uploadFileFromPath(channel, file, options);
			BMessage done(kUploaded);
			if (!uploaded)
				done.AddString("error", uploaded.error().describe().c_str());
			messenger.SendMessage(&done);
		});
		break;
	}
	case kUploaded: {
		SetStatus(fConnection);
		const char* error;
		if (message->FindString("error", &error) == B_OK)
			Failed("The file could not be uploaded.", error);
		break;
	}
	case kTyping:
		if (!fChannel.empty() && system_time() - fLastTypingSent > 3000000) {
			fLastTypingSent = system_time();
			std::string channel = fChannel, thread = fThread;
			session->post([session, channel, thread] { session->sendTyping(channel, thread); });
		}
		break;
	case kMarkRead:
		MarkReadSoon();
		break;
	case kRefresh:
		SetStatus("Refreshing…");
		session->post([session, messenger = BMessenger(this)] {
			Status status = session->bootstrap();
			BMessage done(kBootstrapDone);
			if (!status)
				done.AddString("error", status.error().describe().c_str());
			messenger.SendMessage(&done);
		});
		break;
	case kJumpTo:
		fSidebar->MessageReceived(message);
		break;
	case kSearch: {
		if (fSearch.IsValid()) {
			BMessage activate(B_WINDOW_ACTIVATED);
			fSearch.SendMessage(&activate);
			break;
		}
		std::string team = fSession->store().team().name;
		auto* search = new SearchWindow(fSession.get(), BMessenger(this), team.empty() ? "Slack" : team.c_str());
		fSearch = BMessenger(search);
		search->Show();
		break;
	}
	case kShowMessage:
		ShowMessage(message->GetString("channel", ""), message->GetString("ts", ""), message->GetString("thread", ""));
		break;
	case kImageLoaded:
		fMessages->Invalidate();
		fThreadMessages->Invalidate();
		break;
	case kAddWorkspace:
	case kAbout:
		be_app->PostMessage(message);
		break;
	case kSignOut: {
		auto* alert = new BAlert("Sign out", "Sign out of this workspace? Natter forgets its sign-in and cached messages.",
			"Cancel", "Sign out", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		if (alert->Go() != 1)
			break;
		BMessage signOut(kSignOut);
		signOut.AddString("team", fTeamId.c_str());
		be_app->PostMessage(&signOut);
		PostMessage(B_QUIT_REQUESTED);
		break;
	}
	case kQuit:
		be_app->PostMessage(B_QUIT_REQUESTED);
		break;
	case B_COLORS_UPDATED:
	case B_FONTS_UPDATED:
		Theme::Reload();
		fMessages->Reload();
		fThreadMessages->Reload();
		fSidebar->Reload();
		break;
	default:
		BWindow::MessageReceived(message);
	}
}

bool MainWindow::QuitRequested()
{
	// The search window uses this window's session.
	if (fSearch.LockTarget()) {
		BLooper* search = nullptr;
		fSearch.Target(&search);
		search->Quit();
	}
	// The session's real-time threads must stop before the window goes away.
	if (fSession) {
		fSession->removeListener(fListener);
		fSession->stopRealtime();
		fSession->saveCache();
	}
	BMessage closed('nwcl');
	closed.AddString("team", fTeamId.c_str());
	be_app->PostMessage(&closed);
	return true;
}

}  // namespace natter::ui
