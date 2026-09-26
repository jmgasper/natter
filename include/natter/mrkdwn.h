// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

// Slack message text ("mrkdwn") and rich_text blocks to a flat list of styled
// runs that a UI can lay out and draw.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "natter/models.h"

namespace natter {

enum Style : uint32_t {
	kStyleNone = 0,
	kStyleBold = 1 << 0,
	kStyleItalic = 1 << 1,
	kStyleStrike = 1 << 2,
	kStyleCode = 1 << 3,        // inline `code`
	kStyleCodeBlock = 1 << 4,   // inside ``` ```
	kStyleQuote = 1 << 5,       // a > quoted line
};

enum class RunKind {
	Text,
	Link,               // target = URL
	UserMention,        // target = user id; text = "@name"
	ChannelMention,     // target = channel id; text = "#name"
	Broadcast,          // target = "here", "channel" or "everyone"
	UserGroupMention,   // target = subteam id; text = "@group"
	Emoji,              // text = the Unicode; target = the shortcode
	CustomEmoji,        // target = workspace emoji name (aliases resolved);
	                    // text = ":name:" as a fallback
	Date,               // target = unix seconds; text = Slack's fallback
	ListMarker,         // text = "• " or "1. "; indent = nesting level
	LineBreak,          // text = "\n"
};

struct Run {
	RunKind kind = RunKind::Text;
	std::string text;        // what to draw
	uint32_t style = kStyleNone;
	std::string target;
	int indent = 0;          // list nesting (ListMarker and the item's runs)
	bool highlight = false;  // mentions the signed-in user (@me, @here, ...)

	bool operator==(const Run& other) const = default;
};

struct FormattedText {
	std::vector<Run> runs;
	bool mentionsSelf = false;

	// The runs' text joined, as a plain-text rendering (notifications,
	// search snippets, tests).
	std::string plainText() const;
};

// Lookups the formatter needs; any may be left empty. Store::formatContext()
// fills them from the store.
struct FormatContext {
	// Display name for a user id ("" when unknown).
	std::function<std::string(const std::string& userId)> userName;
	std::function<std::string(const std::string& channelId)> channelName;
	std::function<std::string(const std::string& subteamId)> userGroupName;
	// For a workspace emoji name: the name it finally resolves to after
	// "alias:" chains (a standard name when it aliases one), or "" when the
	// workspace has no such emoji.
	std::function<std::string(const std::string& name)> customEmoji;
	std::string selfUserId;
};

// &amp; &lt; &gt; -> & < >
std::string unescapeEntities(std::string_view text);

FormattedText formatMrkdwn(std::string_view text, const FormatContext& context = {});

// Render the rich_text blocks in a message's "blocks" array; other block
// types are skipped. Empty when there is no rich_text block.
FormattedText formatRichText(const json& blocks, const FormatContext& context = {});

// Blocks when the message has a rich_text block, its mrkdwn text otherwise.
FormattedText formatMessage(const Message& message, const FormatContext& context = {});

// ---- text people type ---------------------------------------------------------

// Lookups encodeMessageText needs; Store::encodeContext() fills them.
struct EncodeContext {
	// The user whose name `text` (what follows an '@') starts with: their id
	// and the length of the name, the longest name first; {"", 0} for none.
	std::function<std::pair<std::string, size_t>(std::string_view text)> matchUser;
	// The id of the channel with this name ("general"), "" when unknown.
	std::function<std::string(const std::string& name)> channelId;
};

// What a person typed -> Slack message text: '&', '<' and '>' escaped;
// @name of a known user -> <@U..>; @here, @channel, @everyone -> <!here>..;
// #name of a known channel -> <#C..>. Nothing changes inside `code`.
std::string encodeMessageText(std::string_view text, const EncodeContext& context = {});

// Slack message text -> the text to edit, the inverse of encodeMessageText:
// entities decoded, mentions and channels as @name and #name, dates as their
// fallback, links as their URL or "label (URL)".
std::string editableText(std::string_view text, const FormatContext& context = {});

}  // namespace natter
