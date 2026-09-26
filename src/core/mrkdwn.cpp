// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/mrkdwn.h"
#include "natter/emoji.h"

#include <cctype>
#include <cstdlib>

namespace natter {

std::string
FormattedText::plainText() const
{
	std::string text;
	for (const Run& run : runs)
		text += run.text;
	return text;
}


std::string
unescapeEntities(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '&') {
			std::string_view rest = text.substr(i);
			if (startsWith(rest, "&amp;")) {
				out += '&';
				i += 4;
				continue;
			}
			if (startsWith(rest, "&lt;")) {
				out += '<';
				i += 3;
				continue;
			}
			if (startsWith(rest, "&gt;")) {
				out += '>';
				i += 3;
				continue;
			}
		}
		out += text[i];
	}
	return out;
}


namespace {

bool
isSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}


bool
isAsciiPunct(char c)
{
	unsigned char u = static_cast<unsigned char>(c);
	return u < 0x80 && std::ispunct(u) != 0;
}


bool
isEmojiNameChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+'
		|| c == '-' || c == '\'' || (c >= 'A' && c <= 'Z');
}


uint32_t
styleFor(char marker)
{
	switch (marker) {
		case '*': return kStyleBold;
		case '_': return kStyleItalic;
		case '~': return kStyleStrike;
	}
	return kStyleNone;
}


// Collects runs, merging neighbouring text runs that look the same.
class Builder {
public:
	explicit Builder(const FormatContext& context)
		:
		fContext(context)
	{
	}

	FormattedText result;

	void add(Run run)
	{
		if (run.kind == RunKind::Text && run.text.empty())
			return;
		if (run.highlight)
			result.mentionsSelf = true;
		if (run.kind == RunKind::Text && !result.runs.empty()) {
			Run& last = result.runs.back();
			if (last.kind == RunKind::Text && last.style == run.style
				&& last.indent == run.indent && last.highlight == run.highlight) {
				last.text += run.text;
				return;
			}
		}
		result.runs.push_back(std::move(run));
	}

	void lineBreak(uint32_t style = kStyleNone)
	{
		Run run;
		run.kind = RunKind::LineBreak;
		run.text = "\n";
		run.style = style & (kStyleQuote | kStyleCodeBlock);
		result.runs.push_back(std::move(run));
	}

	bool endsWithBreak() const
	{
		return result.runs.empty() || result.runs.back().kind == RunKind::LineBreak;
	}

	void ensureBreak(uint32_t style = kStyleNone)
	{
		if (!endsWithBreak())
			lineBreak(style);
	}

	void trimTrailingBreaks()
	{
		while (!result.runs.empty() && result.runs.back().kind == RunKind::LineBreak)
			result.runs.pop_back();
	}

	// Plain text (already unescaped) with :emoji: detection.
	void text(std::string_view text, uint32_t style, int indent)
	{
		if ((style & (kStyleCode | kStyleCodeBlock)) != 0) {
			plain(text, style, indent);
			return;
		}
		std::string pending;
		size_t i = 0;
		while (i < text.size()) {
			if (text[i] == ':') {
				size_t j = i + 1;
				while (j < text.size() && isEmojiNameChar(text[j]))
					j++;
				if (j < text.size() && text[j] == ':' && j > i + 1) {
					std::string name(text.substr(i + 1, j - i - 1));
					size_t end = j + 1;
					int tone = 0;
					std::string_view after = text.substr(j);
					// ":wave::skin-tone-3:" - the tone shares no colon.
					if (startsWith(after, "::skin-tone-") && after.size() >= 14
						&& after[13] == ':') {
						tone = emoji::skinToneFromName(after.substr(2, 11));
						if (tone != 0)
							end = j + 14;
					}
					if (emojiRun(name, tone, style, indent, pending)) {
						i = end;
						continue;
					}
				}
			}
			pending += text[i];
			i++;
		}
		plain(pending, style, indent);
	}

	void plain(std::string_view text, uint32_t style, int indent)
	{
		// Line breaks inside a run become LineBreak runs.
		size_t start = 0;
		while (start <= text.size()) {
			size_t newline = text.find('\n', start);
			std::string_view piece = text.substr(start,
				newline == std::string_view::npos ? std::string_view::npos : newline - start);
			if (!piece.empty()) {
				Run run;
				run.text = std::string(piece);
				run.style = style;
				run.indent = indent;
				add(std::move(run));
			}
			if (newline == std::string_view::npos)
				break;
			lineBreak(style);
			start = newline + 1;
		}
	}

	// Emits the emoji and returns true when `name` is one.
	bool emojiRun(const std::string& name, int tone, uint32_t style, int indent,
		std::string& pending)
	{
		std::optional<std::string> unicode = emoji::lookup(name, tone);
		std::string target = name;
		if (tone != 0)
			target += "::skin-tone-" + std::to_string(tone);
		if (!unicode && fContext.customEmoji) {
			std::string resolved = fContext.customEmoji(name);
			if (!resolved.empty()) {
				unicode = emoji::lookup(resolved, tone);
				if (!unicode) {
					flushPending(pending, style, indent);
					Run run;
					run.kind = RunKind::CustomEmoji;
					run.text = ":" + name + ":";
					run.target = resolved;
					run.style = style;
					run.indent = indent;
					add(std::move(run));
					return true;
				}
			}
		}
		if (!unicode)
			return false;
		flushPending(pending, style, indent);
		Run run;
		run.kind = RunKind::Emoji;
		run.text = *unicode;
		run.target = target;
		run.style = style;
		run.indent = indent;
		add(std::move(run));
		return true;
	}

	void flushPending(std::string& pending, uint32_t style, int indent)
	{
		plain(pending, style, indent);
		pending.clear();
	}

	// The inside of <...>, still escaped.
	Run angleRun(std::string_view content, uint32_t style, int indent)
	{
		std::string_view id = content;
		std::string label;
		size_t bar = content.find('|');
		if (bar != std::string_view::npos) {
			id = content.substr(0, bar);
			label = unescapeEntities(content.substr(bar + 1));
		}
		Run run;
		run.style = style;
		run.indent = indent;

		if (startsWith(id, "@")) {
			run.kind = RunKind::UserMention;
			run.target = std::string(id.substr(1));
			std::string name = fContext.userName ? fContext.userName(run.target) : std::string();
			if (name.empty())
				name = startsWith(label, "@") ? label.substr(1) : label;
			if (name.empty())
				name = run.target;
			run.text = "@" + name;
			run.highlight = !fContext.selfUserId.empty() && run.target == fContext.selfUserId;
			return run;
		}
		if (startsWith(id, "#")) {
			run.kind = RunKind::ChannelMention;
			run.target = std::string(id.substr(1));
			std::string name = label;
			if (name.empty() && fContext.channelName)
				name = fContext.channelName(run.target);
			if (name.empty())
				name = run.target;
			run.text = "#" + name;
			return run;
		}
		if (startsWith(id, "!")) {
			std::string_view command = id.substr(1);
			if (command == "here" || command == "channel" || command == "everyone"
				|| command == "group") {
				run.kind = RunKind::Broadcast;
				run.target = command == "group" ? "channel" : std::string(command);
				run.text = "@" + run.target;
				run.highlight = true;
				return run;
			}
			if (startsWith(command, "subteam^")) {
				run.kind = RunKind::UserGroupMention;
				run.target = std::string(command.substr(8));
				std::string name = label;
				if (name.empty() && fContext.userGroupName)
					name = fContext.userGroupName(run.target);
				if (name.empty())
					name = run.target;
				run.text = startsWith(name, "@") ? name : "@" + name;
				return run;
			}
			if (startsWith(command, "date^")) {
				run.kind = RunKind::Date;
				std::string_view rest = command.substr(5);
				run.target = std::string(rest.substr(0, rest.find('^')));
				run.text = label.empty() ? run.target : label;
				return run;
			}
			run.kind = RunKind::Text;
			run.text = label.empty() ? "<" + unescapeEntities(command) + ">" : label;
			return run;
		}

		run.kind = RunKind::Link;
		run.target = unescapeEntities(id);
		if (!label.empty())
			run.text = label;
		else if (startsWith(run.target, "mailto:"))
			run.text = run.target.substr(7);
		else
			run.text = run.target;
		return run;
	}

	// Text with <...> tokens but no emphasis or emoji (code).
	void literal(std::string_view raw, uint32_t style, int indent)
	{
		size_t i = 0;
		std::string pending;
		while (i < raw.size()) {
			if (raw[i] == '<') {
				size_t close = raw.find('>', i + 1);
				if (close != std::string_view::npos && close > i + 1) {
					plain(unescapeEntities(pending), style, indent);
					pending.clear();
					add(angleRun(raw.substr(i + 1, close - i - 1), style, indent));
					i = close + 1;
					continue;
				}
			}
			pending += raw[i];
			i++;
		}
		plain(unescapeEntities(pending), style, indent);
	}

	// ---- inline mrkdwn -----------------------------------------------------------------

	struct Item {
		char c = 0;
		int atom = -1;    // index into atoms when this item is a token
	};

	struct Atom {
		bool code = false;       // `code`, else <...>
		std::string raw;
	};

	void inlineLine(std::string_view line, uint32_t style, int indent)
	{
		std::vector<Item> items;
		std::vector<Atom> atoms;
		items.reserve(line.size());
		size_t i = 0;
		while (i < line.size()) {
			char c = line[i];
			if (c == '<') {
				size_t close = line.find('>', i + 1);
				if (close != std::string_view::npos && close > i + 1) {
					atoms.push_back(Atom{false, std::string(line.substr(i + 1, close - i - 1))});
					items.push_back(Item{0, static_cast<int>(atoms.size() - 1)});
					i = close + 1;
					continue;
				}
			} else if (c == '`') {
				size_t close = line.find('`', i + 1);
				if (close != std::string_view::npos && close > i + 1) {
					atoms.push_back(Atom{true, std::string(line.substr(i + 1, close - i - 1))});
					items.push_back(Item{0, static_cast<int>(atoms.size() - 1)});
					i = close + 1;
					continue;
				}
			}
			items.push_back(Item{c, -1});
			i++;
		}
		emitRange(items, atoms, 0, items.size(), style, indent);
	}

	bool canOpen(const std::vector<Item>& items, size_t i) const
	{
		char marker = items[i].c;
		bool before = i == 0 || items[i - 1].atom >= 0 || isSpace(items[i - 1].c)
			|| isAsciiPunct(items[i - 1].c);
		if (!before || i + 1 >= items.size())
			return false;
		const Item& next = items[i + 1];
		return next.atom >= 0 || (!isSpace(next.c) && next.c != marker);
	}

	size_t findCloser(const std::vector<Item>& items, size_t open, size_t end) const
	{
		char marker = items[open].c;
		for (size_t j = open + 2; j < end; j++) {
			if (items[j].atom >= 0 || items[j].c != marker)
				continue;
			const Item& previous = items[j - 1];
			bool before = previous.atom >= 0 || !isSpace(previous.c);
			bool after = j + 1 >= items.size() || items[j + 1].atom >= 0
				|| isSpace(items[j + 1].c) || isAsciiPunct(items[j + 1].c);
			if (before && after)
				return j;
		}
		return std::string::npos;
	}

	void emitRange(const std::vector<Item>& items, const std::vector<Atom>& atoms,
		size_t start, size_t end, uint32_t style, int indent)
	{
		std::string pending;
		auto flush = [&]() {
			if (!pending.empty()) {
				text(unescapeEntities(pending), style, indent);
				pending.clear();
			}
		};
		size_t i = start;
		while (i < end) {
			const Item& item = items[i];
			if (item.atom >= 0) {
				flush();
				const Atom& atom = atoms[static_cast<size_t>(item.atom)];
				if (atom.code)
					literal(atom.raw, style | kStyleCode, indent);
				else
					add(angleRun(atom.raw, style, indent));
				i++;
				continue;
			}
			uint32_t emphasis = styleFor(item.c);
			if (emphasis != kStyleNone && canOpen(items, i)) {
				size_t close = findCloser(items, i, end);
				if (close != std::string::npos) {
					flush();
					emitRange(items, atoms, i + 1, close, style | emphasis, indent);
					i = close + 1;
					continue;
				}
			}
			pending += item.c;
			i++;
		}
		flush();
	}

	// ---- lines and blocks ---------------------------------------------------------------

	// One line of mrkdwn outside code blocks.
	void line(std::string_view line, bool& quoteRest)
	{
		uint32_t style = kStyleNone;
		if (quoteRest) {
			style |= kStyleQuote;
		} else if (startsWith(line, "&gt;&gt;&gt;") || startsWith(line, ">>>")) {
			quoteRest = true;
			style |= kStyleQuote;
			line.remove_prefix(startsWith(line, "&gt;") ? 12 : 3);
			if (startsWith(line, " "))
				line.remove_prefix(1);
		} else if (startsWith(line, "&gt;") || startsWith(line, ">")) {
			style |= kStyleQuote;
			line.remove_prefix(startsWith(line, "&gt;") ? 4 : 1);
			if (startsWith(line, " "))
				line.remove_prefix(1);
		}

		// List items: "• x", "- x", "1. x", with leading spaces for nesting.
		// ("* x" is not one: Slack shows it as typed.)
		size_t p = 0;
		int spaces = 0;
		while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) {
			spaces += line[p] == '\t' ? 4 : 1;
			p++;
		}
		std::string_view rest = line.substr(p);
		static const char* kBullets[] = {"\xE2\x80\xA2 ", "\xE2\x97\xA6 ",
			"\xE2\x96\xAA ", "\xE2\x96\xAB ", "\xE2\x80\xA3 ", "- "};
		std::string marker;
		size_t markerLength = 0;
		for (const char* bullet : kBullets) {
			if (startsWith(rest, bullet)) {
				marker = "\xE2\x80\xA2 ";
				markerLength = std::char_traits<char>::length(bullet);
				break;
			}
		}
		if (marker.empty()) {
			size_t digits = 0;
			while (digits < rest.size() && digits < 3 && std::isdigit(
					static_cast<unsigned char>(rest[digits])) != 0)
				digits++;
			if (digits > 0 && digits + 1 < rest.size()
				&& (rest[digits] == '.' || rest[digits] == ')') && rest[digits + 1] == ' ') {
				marker = std::string(rest.substr(0, digits)) + ". ";
				markerLength = digits + 2;
			}
		}
		int indent = 0;
		if (!marker.empty()) {
			indent = (spaces + 2) / 4;
			Run run;
			run.kind = RunKind::ListMarker;
			run.text = marker;
			run.style = style;
			run.indent = indent;
			add(std::move(run));
			line = rest.substr(markerLength);
		}
		inlineLine(line, style, indent);
	}

	void lines(std::string_view segment, bool& quoteRest)
	{
		size_t start = 0;
		for (;;) {
			size_t newline = segment.find('\n', start);
			std::string_view piece = segment.substr(start,
				newline == std::string_view::npos ? std::string_view::npos : newline - start);
			line(piece, quoteRest);
			if (newline == std::string_view::npos)
				break;
			lineBreak(quoteRest ? kStyleQuote : kStyleNone);
			start = newline + 1;
		}
	}

	const FormatContext& fContext;
};

}  // namespace


FormattedText
formatMrkdwn(std::string_view text, const FormatContext& context)
{
	Builder builder(context);
	bool quoteRest = false;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t open = text.find("```", pos);
		size_t close = open == std::string_view::npos ? open : text.find("```", open + 3);
		if (open == std::string_view::npos || close == std::string_view::npos) {
			builder.lines(text.substr(pos), quoteRest);
			break;
		}
		if (open > pos) {
			std::string_view before = text.substr(pos, open - pos);
			// A newline right before the fence belongs to the fence.
			if (endsWith(before, "\n"))
				before.remove_suffix(1);
			builder.lines(before, quoteRest);
		}
		// Breaks around the block separate it from the text; only the ones
		// inside carry the code-block style.
		uint32_t outside = quoteRest ? kStyleQuote : kStyleNone;
		uint32_t style = kStyleCodeBlock | outside;
		builder.ensureBreak(outside);
		std::string_view code = text.substr(open + 3, close - open - 3);
		if (startsWith(code, "\n"))
			code.remove_prefix(1);
		if (endsWith(code, "\n"))
			code.remove_suffix(1);
		builder.literal(code, style, 0);
		pos = close + 3;
		if (pos < text.size()) {
			builder.lineBreak(outside);
			if (text[pos] == '\n' || text[pos] == ' ')
				pos++;
		}
	}
	builder.trimTrailingBreaks();
	return std::move(builder.result);
}

// ---- rich_text blocks --------------------------------------------------------------------

namespace {

uint32_t
richStyle(const json& element)
{
	const json& style = jObj(element, "style");
	uint32_t flags = kStyleNone;
	if (jBool(style, "bold"))
		flags |= kStyleBold;
	if (jBool(style, "italic"))
		flags |= kStyleItalic;
	if (jBool(style, "strike"))
		flags |= kStyleStrike;
	if (jBool(style, "code"))
		flags |= kStyleCode;
	return flags;
}


void
richInline(Builder& builder, const json& elements, uint32_t base, int indent,
	const FormatContext& context)
{
	for (const json& element : elements) {
		std::string type = jStr(element, "type");
		uint32_t style = base | richStyle(element);
		if (type == "text") {
			// rich_text is not entity-escaped, and emoji are their own elements.
			builder.plain(jStr(element, "text"), style, indent);
		} else if (type == "link") {
			Run run;
			run.kind = RunKind::Link;
			run.target = jStr(element, "url");
			run.text = jStr(element, "text", run.target);
			if (run.text.empty())
				run.text = run.target;
			run.style = style;
			run.indent = indent;
			builder.add(std::move(run));
		} else if (type == "user") {
			builder.add(builder.angleRun("@" + jStr(element, "user_id"), style, indent));
		} else if (type == "channel") {
			builder.add(builder.angleRun("#" + jStr(element, "channel_id"), style, indent));
		} else if (type == "usergroup") {
			builder.add(builder.angleRun("!subteam^" + jStr(element, "usergroup_id"),
				style, indent));
		} else if (type == "broadcast") {
			builder.add(builder.angleRun("!" + jStr(element, "range", "here"), style, indent));
		} else if (type == "emoji") {
			std::string name = jStr(element, "name");
			int tone = static_cast<int>(jInt(element, "skin_tone"));
			std::string pending;
			if (!builder.emojiRun(name, tone, style, indent, pending)) {
				// Unknown here but Slack sent its code points.
				std::string unicode;
				std::string hex = jStr(element, "unicode");
				size_t start = 0;
				while (!hex.empty() && start < hex.size()) {
					size_t dash = hex.find('-', start);
					std::string part = hex.substr(start,
						dash == std::string::npos ? std::string::npos : dash - start);
					appendUtf8(unicode, static_cast<uint32_t>(std::strtoul(part.c_str(),
						nullptr, 16)));
					if (dash == std::string::npos)
						break;
					start = dash + 1;
				}
				Run run;
				run.kind = unicode.empty() ? RunKind::CustomEmoji : RunKind::Emoji;
				run.text = unicode.empty() ? ":" + name + ":" : unicode;
				run.target = name;
				run.style = style;
				run.indent = indent;
				builder.add(std::move(run));
			}
		} else if (type == "date") {
			Run run;
			run.kind = RunKind::Date;
			run.target = jStr(element, "timestamp");
			run.text = jStr(element, "fallback", run.target);
			run.style = style;
			run.indent = indent;
			builder.add(std::move(run));
		} else if (type == "color") {
			builder.plain(jStr(element, "value"), style, indent);
		} else if (type == "team") {
			builder.plain(jStr(element, "team_id"), style, indent);
		} else if (jHas(element, "text")) {
			builder.plain(jStr(element, "text"), style, indent);
		}
	}
	(void)context;
}


void
richBlockElement(Builder& builder, const json& element, const FormatContext& context)
{
	std::string type = jStr(element, "type");
	const json& elements = jArr(element, "elements");
	if (type == "rich_text_section") {
		richInline(builder, elements, kStyleNone, 0, context);
	} else if (type == "rich_text_preformatted") {
		richInline(builder, elements, kStyleCodeBlock, 0, context);
	} else if (type == "rich_text_quote") {
		richInline(builder, elements, kStyleQuote, 0, context);
	} else if (type == "rich_text_list") {
		bool ordered = jStr(element, "style") == "ordered";
		int indent = static_cast<int>(jInt(element, "indent"));
		int number = static_cast<int>(jInt(element, "offset")) + 1;
		for (const json& item : elements) {
			builder.ensureBreak();
			Run marker;
			marker.kind = RunKind::ListMarker;
			marker.text = ordered ? std::to_string(number++) + ". " : "\xE2\x80\xA2 ";
			marker.indent = indent;
			builder.add(std::move(marker));
			richInline(builder, jArr(item, "elements"), kStyleNone, indent, context);
		}
	}
}

}  // namespace


FormattedText
formatRichText(const json& blocks, const FormatContext& context)
{
	Builder builder(context);
	if (!blocks.is_array())
		return std::move(builder.result);
	for (const json& block : blocks) {
		if (jStr(block, "type") != "rich_text")
			continue;
		for (const json& element : jArr(block, "elements")) {
			builder.ensureBreak();
			richBlockElement(builder, element, context);
		}
	}
	builder.trimTrailingBreaks();
	return std::move(builder.result);
}


FormattedText
formatMessage(const Message& message, const FormatContext& context)
{
	if (message.blocks.is_array()) {
		for (const json& block : message.blocks) {
			if (jStr(block, "type") == "rich_text")
				return formatRichText(message.blocks, context);
		}
	}
	return formatMrkdwn(message.text, context);
}

}  // namespace natter
