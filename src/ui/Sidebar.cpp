// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "Sidebar.h"

#include "Messages.h"
#include "Theme.h"
#include "natter/session.h"
#include "natter/util.h"

#include <LayoutBuilder.h>
#include <ListView.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>

#include <algorithm>

namespace natter::ui {

namespace {

enum : uint32 {
	kSelectionChanged = 'sbsl',
};

class SectionItem : public BListItem {
public:
	explicit SectionItem(const char* title) : fTitle(title) {}

	void Update(BView* owner, const BFont* font) override
	{
		BListItem::Update(owner, font);
		SetHeight(Height() + 8);
	}

	void DrawItem(BView* owner, BRect frame, bool complete) override
	{
		const Theme& theme = Theme::Current();
		owner->SetHighColor(theme.sidebar);
		owner->FillRect(frame);
		BFont font = theme.bold;
		font.SetSize(theme.small.Size());
		owner->SetFont(&font);
		owner->SetHighColor(theme.sidebarMuted);
		owner->DrawString(fTitle.c_str(), BPoint(frame.left + 10, frame.bottom - 5));
	}

private:
	std::string fTitle;
};

class ChannelItem : public BListItem {
public:
	std::string id;
	std::string label;
	std::string prefix;
	bool unread = false;
	int mentions = 0;
	int presence = -1;    // -1 not a DM, 0 away, 1 active

	void Update(BView* owner, const BFont* font) override
	{
		BListItem::Update(owner, font);
		SetHeight(Height() + 6);
	}

	void DrawItem(BView* owner, BRect frame, bool complete) override
	{
		const Theme& theme = Theme::Current();
		owner->SetHighColor(IsSelected() ? theme.sidebarSelection : theme.sidebar);
		owner->FillRect(frame);
		const BFont& font = unread ? theme.bold : theme.plain;
		owner->SetFont(&font);
		font_height metrics;
		font.GetHeight(&metrics);
		float baseline = frame.top + (frame.Height() + metrics.ascent - metrics.descent) / 2;
		rgb_color text = IsSelected() ? ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR)
			: unread ? theme.sidebarText : theme.sidebarMuted;
		float x = frame.left + 14;
		if (presence >= 0) {
			BRect dot(x, baseline - 8, x + 7, baseline - 1);
			owner->SetHighColor(presence ? theme.presenceActive : text);
			if (presence)
				owner->FillEllipse(dot);
			else
				owner->StrokeEllipse(dot);
			x += 13;
		} else {
			owner->SetHighColor(text);
			owner->DrawString(prefix.c_str(), BPoint(x, baseline));
			x += owner->StringWidth("#") + 6;
		}
		owner->SetHighColor(text);
		float right = frame.right - 8;
		if (mentions > 0) {
			std::string count = std::to_string(mentions);
			BFont badgeFont = theme.bold;
			badgeFont.SetSize(theme.small.Size());
			float width = std::max(18.0f, badgeFont.StringWidth(count.c_str()) + 10);
			BRect badge(right - width, frame.top + 3, right, frame.bottom - 3);
			owner->SetHighColor(theme.badge);
			owner->FillRoundRect(badge, 8, 8);
			owner->SetHighColor(theme.badgeText);
			owner->SetFont(&badgeFont);
			owner->DrawString(count.c_str(), BPoint(badge.left + (width - badgeFont.StringWidth(count.c_str())) / 2, baseline - 1));
			owner->SetFont(&font);
			owner->SetHighColor(text);
			right = badge.left - 6;
		}
		BString truncated(label.c_str());
		font.TruncateString(&truncated, B_TRUNCATE_END, right - x);
		owner->DrawString(truncated.String(), BPoint(x, baseline));
	}
};

class ChannelList : public BListView {
public:
	ChannelList() : BListView("conversations") {}

	void Draw(BRect updateRect) override
	{
		SetHighColor(Theme::Current().sidebar);
		FillRect(updateRect);
		BListView::Draw(updateRect);
	}
};

}  // namespace

Sidebar::Sidebar(Session* session)
	:
	BView("sidebar", B_WILL_DRAW),
	fSession(session)
{
	const Theme& theme = Theme::Current();
	SetViewColor(theme.sidebar);
	fTeam = new BStringView("team", "");
	fTeam->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	BFont teamFont = theme.bold;
	teamFont.SetSize(theme.plain.Size() * 1.2f);
	fTeam->SetFont(&teamFont);
	fFilter = new BTextControl("filter", nullptr, "", new BMessage(kFilterChanged));
	fFilter->SetModificationMessage(new BMessage(kFilterChanged));
	fFilter->TextView()->SetText("");
	fList = new ChannelList();
	fList->SetSelectionMessage(new BMessage(kSelectionChanged));
	auto* scroll = new BScrollView("conversations scroll", fList, 0, false, true, B_NO_BORDER);
	BLayoutBuilder::Group<>(this, B_VERTICAL, 4)
		.SetInsets(8, 8, 0, 0)
		.Add(fTeam)
		.AddGroup(B_HORIZONTAL, 0)
			.SetInsets(0, 0, 8, 4)
			.Add(fFilter)
		.End()
		.Add(scroll);
	SetExplicitMinSize(BSize(170, B_SIZE_UNSET));
	SetExplicitPreferredSize(BSize(230, B_SIZE_UNSET));
}

void Sidebar::AttachedToWindow()
{
	BView::AttachedToWindow();
	fFilter->SetTarget(this);
	fList->SetTarget(this);
	fFilter->TextView()->SetText("");
}

std::string Sidebar::Selected() const
{
	return fSelected;
}

void Sidebar::Reload()
{
	Store& store = fSession->store();
	fTeam->SetText(store.team().name.empty() ? fSession->credentials().teamName.c_str() : store.team().name.c_str());
	std::string filter = toLower(fFilter->Text());
	std::vector<Channel> channels = store.channels();
	std::vector<ChannelItem*> rooms, people;
	for (const Channel& channel : channels) {
		if (channel.isArchived)
			continue;
		auto* item = new ChannelItem();
		item->id = channel.id;
		item->unread = channel.hasUnreads || channel.unreadCount > 0;
		item->mentions = channel.mentionCount;
		if (channel.isIm) {
			auto user = store.user(channel.imUser);
			if (user && user->deleted) {
				delete item;
				continue;
			}
			item->label = user ? user->bestName() : channel.imUser;
			item->presence = user && user->presence == "active" ? 1 : 0;
			if (user && store.selfUserId() == user->id)
				item->label += " (you)";
		} else if (channel.isMpim) {
			std::string name = store.channelDisplayName(channel.id);
			item->label = name;
			item->presence = 0;
		} else {
			if (!channel.isMember) {
				delete item;
				continue;
			}
			item->label = channel.name;
			item->prefix = channel.isPrivate ? "🔒" : "#";
		}
		if (!filter.empty() && toLower(item->label).find(filter) == std::string::npos) {
			delete item;
			continue;
		}
		(channel.isDirect() ? people : rooms).push_back(item);
	}
	auto byName = [](const ChannelItem* a, const ChannelItem* b) { return toLower(a->label) < toLower(b->label); };
	std::sort(rooms.begin(), rooms.end(), byName);
	std::sort(people.begin(), people.end(), [&](const ChannelItem* a, const ChannelItem* b) {
		if (a->unread != b->unread)
			return a->unread;
		return byName(a, b);
	});

	fReloading = true;
	float scroll = fList->Bounds().top;
	while (BListItem* item = fList->RemoveItem(static_cast<int32>(0)))
		delete item;
	if (!rooms.empty())
		fList->AddItem(new SectionItem("CHANNELS"));
	for (ChannelItem* item : rooms)
		fList->AddItem(item);
	if (!people.empty())
		fList->AddItem(new SectionItem("DIRECT MESSAGES"));
	for (ChannelItem* item : people)
		fList->AddItem(item);
	for (int32 index = 0; index < fList->CountItems(); index++) {
		auto* item = dynamic_cast<ChannelItem*>(fList->ItemAt(index));
		if (item && item->id == fSelected)
			fList->Select(index);
	}
	fList->ScrollTo(BPoint(0, scroll));
	fReloading = false;
}

void Sidebar::Select(const std::string& channel)
{
	fSelected = channel;
	for (int32 index = 0; index < fList->CountItems(); index++) {
		auto* item = dynamic_cast<ChannelItem*>(fList->ItemAt(index));
		if (item && item->id == channel) {
			fReloading = true;
			fList->Select(index);
			fList->ScrollToSelection();
			fReloading = false;
			return;
		}
	}
}

void Sidebar::MessageReceived(BMessage* message)
{
	switch (message->what) {
	case kSelectionChanged: {
		if (fReloading)
			return;
		auto* item = dynamic_cast<ChannelItem*>(fList->ItemAt(fList->CurrentSelection()));
		if (!item) {
			// A section title: keep the conversation that was selected.
			Select(fSelected);
			return;
		}
		if (item->id == fSelected)
			return;
		fSelected = item->id;
		BMessage selected(kChannelSelected);
		selected.AddString("channel", item->id.c_str());
		Window()->PostMessage(&selected);
		break;
	}
	case kFilterChanged:
		Reload();
		break;
	case kJumpTo:
		fFilter->MakeFocus(true);
		fFilter->TextView()->SelectAll();
		break;
	default:
		BView::MessageReceived(message);
	}
}

}  // namespace natter::ui
