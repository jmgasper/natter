// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "SearchWindow.h"

#include "Messages.h"
#include "TextLayout.h"
#include "Theme.h"
#include "natter/session.h"
#include "natter/util.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <ListItem.h>
#include <ListView.h>
#include <ScrollView.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>

#include <ctime>

namespace natter::ui {

namespace {

enum : uint32 {
	kRunSearch = 'srun',
	kResults = 'sres',
	kInvoked = 'sinv',
};

std::string When(int64_t seconds)
{
	time_t value = static_cast<time_t>(seconds);
	struct tm local;
	localtime_r(&value, &local);
	char text[64];
	strftime(text, sizeof(text), "%b %e %Y, %H:%M", &local);
	return text;
}

// "thread_ts=..." in a permalink: the thread a reply belongs to.
std::string ThreadOf(const std::string& permalink)
{
	size_t at = permalink.find("thread_ts=");
	if (at == std::string::npos)
		return {};
	size_t end = permalink.find('&', at);
	return permalink.substr(at + 10, end == std::string::npos ? std::string::npos : end - at - 10);
}

// A result: where and who on the first line, the message on the second.
class ResultItem : public BListItem {
public:
	ResultItem(const BMessage& fields)
		:
		fChannel(fields.GetString("channel", "")),
		fTs(fields.GetString("ts", "")),
		fThread(fields.GetString("thread", "")),
		fHeading(fields.GetString("heading", "")),
		fText(fields.GetString("text", ""))
	{
	}

	const std::string& Channel() const { return fChannel; }
	const std::string& Ts() const { return fTs; }
	const std::string& Thread() const { return fThread; }

	void Update(BView* owner, const BFont* font) override
	{
		BListItem::Update(owner, font);
		const Theme& theme = Theme::Current();
		font_height bold, plain;
		theme.bold.GetHeight(&bold);
		theme.plain.GetHeight(&plain);
		SetHeight(std::ceil(bold.ascent + bold.descent + plain.ascent + plain.descent + plain.leading) + 14);
	}

	void DrawItem(BView* owner, BRect frame, bool complete) override
	{
		const Theme& theme = Theme::Current();
		owner->SetHighColor(IsSelected() ? ui_color(B_LIST_SELECTED_BACKGROUND_COLOR) : ui_color(B_LIST_BACKGROUND_COLOR));
		owner->FillRect(frame);
		font_height bold, plain;
		theme.bold.GetHeight(&bold);
		theme.plain.GetHeight(&plain);
		float width = frame.Width() - 16;
		BString heading(fHeading.c_str());
		theme.bold.TruncateString(&heading, B_TRUNCATE_END, width);
		owner->SetFont(&theme.bold);
		owner->SetHighColor(IsSelected() ? ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR) : ui_color(B_LIST_ITEM_TEXT_COLOR));
		float y = frame.top + 5 + std::ceil(bold.ascent);
		owner->DrawString(heading.String(), BPoint(frame.left + 8, y));
		BString text(fText.c_str());
		text.ReplaceAll('\n', ' ');
		theme.plain.TruncateString(&text, B_TRUNCATE_END, width);
		owner->SetFont(&theme.plain);
		y += std::ceil(bold.descent + plain.ascent) + 2;
		owner->DrawString(text.String(), BPoint(frame.left + 8, y));
		owner->SetHighColor(theme.separator);
		owner->StrokeLine(BPoint(frame.left, frame.bottom), BPoint(frame.right, frame.bottom));
	}

private:
	std::string fChannel;
	std::string fTs;
	std::string fThread;
	std::string fHeading;
	std::string fText;
};

}  // namespace

SearchWindow::SearchWindow(Session* session, BMessenger workspace, const char* workspaceName)
	:
	BWindow(BRect(0, 0, 560, 480), (std::string("Search ") + workspaceName).c_str(), B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
	fSession(session),
	fWorkspace(workspace)
{
	fQuery = new BTextControl("query", nullptr, "", new BMessage(kRunSearch));
	fSearch = new BButton("Search", new BMessage(kRunSearch));
	fResults = new BListView("results", B_SINGLE_SELECTION_LIST);
	fResults->SetInvocationMessage(new BMessage(kInvoked));
	auto* scroll = new BScrollView("results scroll", fResults, 0, false, true);
	scroll->SetExplicitMinSize(BSize(300, 200));
	fStatus = new BStringView("status", "Words, \"a phrase\", in:#channel, from:@someone");
	fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fStatus->SetFont(&Theme::Current().small);
	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_WINDOW_INSETS)
		.AddGroup(B_HORIZONTAL)
			.Add(fQuery, 1)
			.Add(fSearch)
		.End()
		.Add(scroll, 1)
		.Add(fStatus);
	// No default button: Enter in the query searches, in the list it shows
	// the chosen message.
	CenterOnScreen();
	fQuery->MakeFocus(true);
}

void SearchWindow::Search()
{
	std::string query = trim(fQuery->Text());
	if (query.empty())
		return;
	int32 generation = ++fGeneration;
	fStatus->SetText("Searching…");
	Session* session = fSession;
	BMessenger target(this);
	session->post([session, query, generation, target] {
		SearchOptions options;
		options.count = 50;
		auto page = session->api()->searchMessages(query, options);
		BMessage results(kResults);
		results.AddInt32("generation", generation);
		if (!page) {
			results.AddString("error", page.error().describe().c_str());
			target.SendMessage(&results);
			return;
		}
		const Store& store = session->store();
		FormatContext context = store.formatContext();
		results.AddInt32("total", page->total);
		for (const SearchMatch& match : page->matches) {
			const Message& message = match.message;
			BMessage fields;
			fields.AddString("channel", message.channel.c_str());
			fields.AddString("ts", message.ts.c_str());
			std::string thread = message.threadTs.empty() ? ThreadOf(message.permalink) : message.threadTs;
			fields.AddString("thread", thread == message.ts ? "" : thread.c_str());
			std::string where = store.channelDisplayName(message.channel);
			if (where.empty() || where == message.channel)
				where = match.channelName.empty() ? message.channel : "#" + match.channelName;
			std::string who = store.userName(message.user);
			if (who.empty())
				who = message.username.empty() ? message.user : message.username;
			fields.AddString("heading", (where + " · " + who + " · " + When(tsSeconds(message.ts))).c_str());
			fields.AddString("text", DrawableEmoji(formatMessage(message, context).plainText()).c_str());
			results.AddMessage("match", &fields);
		}
		target.SendMessage(&results);
	});
}

void SearchWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
	case kRunSearch:
		Search();
		break;
	case kResults: {
		if (message->GetInt32("generation", 0) != fGeneration)
			break;   // an earlier search
		for (int32 index = fResults->CountItems() - 1; index >= 0; index--)
			delete fResults->RemoveItem(index);
		const char* error;
		if (message->FindString("error", &error) == B_OK) {
			fStatus->SetText((std::string("The search failed: ") + error).c_str());
			break;
		}
		BMessage fields;
		for (int32 index = 0; message->FindMessage("match", index, &fields) == B_OK; index++)
			fResults->AddItem(new ResultItem(fields));
		int32 total = message->GetInt32("total", 0);
		int32 shown = fResults->CountItems();
		std::string status = shown == 0 ? "Nothing found."
			: total > shown ? "The newest " + std::to_string(shown) + " of " + std::to_string(total) + " messages. Double-click one to show it."
			: std::to_string(shown) + (shown == 1 ? " message." : " messages.") + " Double-click one to show it.";
		fStatus->SetText(status.c_str());
		break;
	}
	case kInvoked: {
		auto* item = dynamic_cast<ResultItem*>(fResults->ItemAt(fResults->CurrentSelection()));
		if (!item)
			break;
		BMessage show(kShowMessage);
		show.AddString("channel", item->Channel().c_str());
		show.AddString("ts", item->Ts().c_str());
		show.AddString("thread", item->Thread().c_str());
		fWorkspace.SendMessage(&show);
		break;
	}
	default:
		BWindow::MessageReceived(message);
	}
}

}  // namespace natter::ui
