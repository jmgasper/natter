// Natter - mrkdwn, rich_text and emoji.
// SPDX-License-Identifier: MIT

#include "testing.h"
#include "natter/emoji.h"
#include "natter/mrkdwn.h"
#include "natter/store.h"

using namespace natter;

namespace {

const std::string kThumbsUp = "\xF0\x9F\x91\x8D";
const std::string kToneMedium = "\xF0\x9F\x8F\xBC";   // U+1F3FC, skin-tone-3
const std::string kSmile = "\xF0\x9F\x98\x84";
const std::string kRocket = "\xF0\x9F\x9A\x80";

FormatContext
testContext()
{
	FormatContext context;
	context.selfUserId = "U03SELF";
	context.userName = [](const std::string& id) -> std::string {
		if (id == "U01ALICE")
			return "ali";
		if (id == "U02BOB")
			return "Bob Brown";
		if (id == "U03SELF")
			return "natter";
		return {};
	};
	context.channelName = [](const std::string& id) -> std::string {
		return id == "C01GENERAL" ? "general" : "";
	};
	context.userGroupName = [](const std::string& id) -> std::string {
		return id == "S0DEVS" ? "devs" : "";
	};
	context.customEmoji = [](const std::string& name) -> std::string {
		if (name == "partyparrot" || name == "pp")
			return "partyparrot";
		if (name == "thumbs")
			return "thumbsup";
		return {};
	};
	return context;
}


Run
textRun(const std::string& text, uint32_t style = kStyleNone, int indent = 0)
{
	Run run;
	run.text = text;
	run.style = style;
	run.indent = indent;
	return run;
}


Run
lineBreak(uint32_t style = kStyleNone)
{
	Run run;
	run.kind = RunKind::LineBreak;
	run.text = "\n";
	run.style = style;
	return run;
}


std::string
dump(const std::vector<Run>& runs)
{
	std::string out;
	for (const Run& run : runs) {
		out += "[" + std::to_string(static_cast<int>(run.kind)) + ":" + run.text + ":"
			+ std::to_string(run.style) + ":" + run.target + "]";
	}
	return out;
}


void
expectRuns(const FormattedText& text, const std::vector<Run>& expected, int line)
{
	if (text.runs != expected) {
		testing::fail(__FILE__, line, "runs differ\n      got:  " + dump(text.runs)
			+ "\n      want: " + dump(expected));
	}
}

}  // namespace

// ---- emoji --------------------------------------------------------------------------

TEST(emoji_lookup_and_skin_tones)
{
	CHECK_EQ(emoji::lookup("+1").value_or(""), kThumbsUp);
	CHECK_EQ(emoji::lookup("thumbsup").value_or(""), kThumbsUp);
	CHECK_EQ(emoji::lookup("smile").value_or(""), kSmile);
	CHECK(!emoji::lookup("definitely_not_an_emoji").has_value());
	CHECK_EQ(emoji::lookup("+1", 3).value_or(""), kThumbsUp + kToneMedium);
	CHECK_EQ(emoji::lookup("+1::skin-tone-3").value_or(""), kThumbsUp + kToneMedium);
	CHECK_EQ(emoji::lookup("thumbsup:skin-tone-3").value_or(""), kThumbsUp + kToneMedium);
	// No tone for emoji that do not take one; out-of-range tones are ignored.
	CHECK_EQ(emoji::lookup("smile", 4).value_or(""), kSmile);
	CHECK_EQ(emoji::lookup("+1", 9).value_or(""), kThumbsUp);
	CHECK(emoji::takesSkinTone("wave"));
	CHECK(!emoji::takesSkinTone("smile"));
	// Two-person emoji take the tone on both people.
	std::string holding = emoji::lookup("people_holding_hands", 2).value_or("");
	CHECK_EQ(holding, std::string("\xF0\x9F\xA7\x91\xF0\x9F\x8F\xBB\xE2\x80\x8D\xF0\x9F\xA4\x9D"
		"\xE2\x80\x8D\xF0\x9F\xA7\x91\xF0\x9F\x8F\xBB"));
	// A variation selector after the base is dropped when toned.
	std::string raised = emoji::lookup("raised_hand_with_fingers_splayed", 5).value_or("");
	CHECK_EQ(raised, std::string("\xF0\x9F\x96\x90\xF0\x9F\x8F\xBE"));
	CHECK_EQ(emoji::lookup("skin-tone-2").value_or(""), std::string("\xF0\x9F\x8F\xBB"));
	CHECK_EQ(emoji::skinToneFromName("skin-tone-6"), 6);
	CHECK_EQ(emoji::skinToneFromName("skin-tone-7"), 0);
	std::string name;
	int tone = 0;
	emoji::splitSkinTone("wave::skin-tone-4", name, tone);
	CHECK_EQ(name, std::string("wave"));
	CHECK_EQ(tone, 4);
	CHECK(emoji::count() > 1900);
}

// ---- inline styles -------------------------------------------------------------------

TEST(mrkdwn_plain_and_entities)
{
	expectRuns(formatMrkdwn("hello world"), {textRun("hello world")}, __LINE__);
	expectRuns(formatMrkdwn("a &amp; b &lt;c&gt; &amp;amp;"), {textRun("a & b <c> &amp;")},
		__LINE__);
	CHECK_EQ(unescapeEntities("&lt;&gt;&amp;&quot;"), std::string("<>&&quot;"));
	CHECK(formatMrkdwn("").runs.empty());
}


TEST(mrkdwn_bold_italic_strike)
{
	expectRuns(formatMrkdwn("*bold* _it_ ~gone~"), {
		textRun("bold", kStyleBold), textRun(" "), textRun("it", kStyleItalic),
		textRun(" "), textRun("gone", kStyleStrike)}, __LINE__);
	expectRuns(formatMrkdwn("*_both_*"), {textRun("both", kStyleBold | kStyleItalic)},
		__LINE__);
	expectRuns(formatMrkdwn("say *hi there*, ok"), {textRun("say "),
		textRun("hi there", kStyleBold), textRun(", ok")}, __LINE__);
	expectRuns(formatMrkdwn("(*a*)"), {textRun("("), textRun("a", kStyleBold), textRun(")")},
		__LINE__);
}


TEST(mrkdwn_markers_that_are_not_styles)
{
	expectRuns(formatMrkdwn("snake_case_name"), {textRun("snake_case_name")}, __LINE__);
	expectRuns(formatMrkdwn("2*3*4 = 24"), {textRun("2*3*4 = 24")}, __LINE__);
	expectRuns(formatMrkdwn("*oops"), {textRun("*oops")}, __LINE__);
	expectRuns(formatMrkdwn("* not bold *"), {textRun("* not bold *")}, __LINE__);
	expectRuns(formatMrkdwn("**"), {textRun("**")}, __LINE__);
	expectRuns(formatMrkdwn("*a\nb*"), {textRun("*a"), lineBreak(), textRun("b*")}, __LINE__);
}


TEST(mrkdwn_inline_code)
{
	FormattedText text = formatMrkdwn("run `make *all* &amp; <https://x.io|x>` now");
	REQUIRE(text.runs.size() == 4);
	CHECK_EQ(text.runs[0], textRun("run "));
	CHECK_EQ(text.runs[1], textRun("make *all* & ", kStyleCode));
	CHECK(text.runs[2].kind == RunKind::Link);
	CHECK_EQ(text.runs[2].style, uint32_t(kStyleCode));
	CHECK_EQ(text.runs[2].text, std::string("x"));
	CHECK_EQ(text.runs[3], textRun(" now"));
	expectRuns(formatMrkdwn("*bold `code`*"), {textRun("bold ", kStyleBold),
		textRun("code", kStyleBold | kStyleCode)}, __LINE__);
	expectRuns(formatMrkdwn("a ` b"), {textRun("a ` b")}, __LINE__);
}


TEST(mrkdwn_code_blocks)
{
	expectRuns(formatMrkdwn("```\nline1\n*x* :smile:\n```"), {
		textRun("line1", kStyleCodeBlock), lineBreak(kStyleCodeBlock),
		textRun("*x* :smile:", kStyleCodeBlock)}, __LINE__);
	expectRuns(formatMrkdwn("before\n```code```\nafter"), {
		textRun("before"), lineBreak(), textRun("code", kStyleCodeBlock),
		lineBreak(), textRun("after")}, __LINE__);
	expectRuns(formatMrkdwn("see ```x &lt; y``` done"), {
		textRun("see "), lineBreak(), textRun("x < y", kStyleCodeBlock),
		lineBreak(), textRun("done")}, __LINE__);
	// An unclosed fence is literal text.
	expectRuns(formatMrkdwn("```not closed"), {textRun("```not closed")}, __LINE__);
}


TEST(mrkdwn_quotes)
{
	expectRuns(formatMrkdwn("&gt; quoted *text*\nnot quoted"), {
		textRun("quoted ", kStyleQuote), textRun("text", kStyleQuote | kStyleBold),
		lineBreak(), textRun("not quoted")}, __LINE__);
	expectRuns(formatMrkdwn("intro\n&gt;&gt;&gt; all\nof this"), {
		textRun("intro"), lineBreak(), textRun("all", kStyleQuote), lineBreak(kStyleQuote),
		textRun("of this", kStyleQuote)}, __LINE__);
	expectRuns(formatMrkdwn("a &gt; b"), {textRun("a > b")}, __LINE__);
}


TEST(mrkdwn_lists)
{
	FormattedText text = formatMrkdwn(
		"\xE2\x80\xA2 one\n- two\n1. three\n    \xE2\x97\xA6 nested *b*");
	std::vector<Run> markers;
	for (const Run& run : text.runs) {
		if (run.kind == RunKind::ListMarker)
			markers.push_back(run);
	}
	REQUIRE(markers.size() == 4);
	CHECK_EQ(markers[0].text, std::string("\xE2\x80\xA2 "));
	CHECK_EQ(markers[1].text, std::string("\xE2\x80\xA2 "));
	CHECK_EQ(markers[2].text, std::string("1. "));
	CHECK_EQ(markers[3].indent, 1);
	CHECK_EQ(text.plainText(),
		std::string("\xE2\x80\xA2 one\n\xE2\x80\xA2 two\n1. three\n\xE2\x80\xA2 nested b"));
	CHECK_EQ(text.runs.back(), textRun("b", kStyleBold, 1));
	// "-1 degrees" and "*bold*" are not list items.
	CHECK(formatMrkdwn("-1 degrees").runs.at(0).kind == RunKind::Text);
	CHECK(formatMrkdwn("*bold* start").runs.at(0).style == kStyleBold);
}

// ---- links and mentions -----------------------------------------------------------------

TEST(mrkdwn_links)
{
	FormattedText text = formatMrkdwn(
		"<https://example.com/a?b=1&amp;c=2|Example &amp; Co> <https://x.org> "
		"<mailto:a@b.com|a@b.com> <mailto:c@d.com>");
	std::vector<Run> links;
	for (const Run& run : text.runs) {
		if (run.kind == RunKind::Link)
			links.push_back(run);
	}
	REQUIRE(links.size() == 4);
	CHECK_EQ(links[0].text, std::string("Example & Co"));
	CHECK_EQ(links[0].target, std::string("https://example.com/a?b=1&c=2"));
	CHECK_EQ(links[1].text, std::string("https://x.org"));
	CHECK_EQ(links[2].target, std::string("mailto:a@b.com"));
	CHECK_EQ(links[3].text, std::string("c@d.com"));
	FormattedText bold = formatMrkdwn("*see <https://x.io|here>*");
	REQUIRE(bold.runs.size() == 2);
	CHECK_EQ(bold.runs[1].style, uint32_t(kStyleBold));
	CHECK(bold.runs[1].kind == RunKind::Link);
	// A lone '<' (already escaped by Slack as &lt;) stays text.
	expectRuns(formatMrkdwn("1 &lt; 2"), {textRun("1 < 2")}, __LINE__);
}


TEST(mrkdwn_mentions)
{
	FormatContext context = testContext();
	FormattedText text = formatMrkdwn(
		"hi <@U01ALICE> and <@U03SELF>, also <@U999|ghost> <@U998>", context);
	std::vector<Run> mentions;
	for (const Run& run : text.runs) {
		if (run.kind == RunKind::UserMention)
			mentions.push_back(run);
	}
	REQUIRE(mentions.size() == 4);
	CHECK_EQ(mentions[0].text, std::string("@ali"));
	CHECK_EQ(mentions[0].target, std::string("U01ALICE"));
	CHECK(!mentions[0].highlight);
	CHECK(mentions[1].highlight);
	CHECK_EQ(mentions[2].text, std::string("@ghost"));
	CHECK_EQ(mentions[3].text, std::string("@U998"));
	CHECK(text.mentionsSelf);
	CHECK(!formatMrkdwn("<@U01ALICE>", context).mentionsSelf);

	FormattedText channels = formatMrkdwn("<#C02RANDOM|random> <#C01GENERAL> <#C0UNKNOWN>",
		context);
	CHECK_EQ(channels.runs[0].text, std::string("#random"));
	CHECK(channels.runs[0].kind == RunKind::ChannelMention);
	CHECK_EQ(channels.runs[2].text, std::string("#general"));
	CHECK_EQ(channels.runs[4].text, std::string("#C0UNKNOWN"));
}


TEST(mrkdwn_broadcasts_groups_dates)
{
	FormatContext context = testContext();
	FormattedText text = formatMrkdwn("<!here> <!channel|@channel> <!everyone> "
		"<!subteam^S0DEVS|@devs> <!subteam^S0DEVS> "
		"<!date^1392734382^{date_short}|Feb 18, 2014> <!unknown>", context);
	std::vector<Run> runs;
	for (const Run& run : text.runs) {
		if (run.kind != RunKind::Text || run.text != " ")
			runs.push_back(run);
	}
	REQUIRE(runs.size() == 7);
	CHECK(runs[0].kind == RunKind::Broadcast);
	CHECK_EQ(runs[0].text, std::string("@here"));
	CHECK(runs[0].highlight);
	CHECK_EQ(runs[1].target, std::string("channel"));
	CHECK_EQ(runs[2].text, std::string("@everyone"));
	CHECK(runs[3].kind == RunKind::UserGroupMention);
	CHECK_EQ(runs[3].text, std::string("@devs"));
	CHECK_EQ(runs[3].target, std::string("S0DEVS"));
	CHECK_EQ(runs[4].text, std::string("@devs"));
	CHECK(runs[5].kind == RunKind::Date);
	CHECK_EQ(runs[5].text, std::string("Feb 18, 2014"));
	CHECK_EQ(runs[5].target, std::string("1392734382"));
	CHECK(endsWith(runs[6].text, "<unknown>"));
	CHECK(text.mentionsSelf);
}

// ---- emoji in text -----------------------------------------------------------------------

TEST(mrkdwn_emoji_shortcodes)
{
	FormatContext context = testContext();
	FormattedText text = formatMrkdwn(
		"hi :smile: :+1::skin-tone-3: :partyparrot: :pp: :thumbs: :nope: at 10:30:45",
		context);
	std::vector<Run> emoji;
	for (const Run& run : text.runs) {
		if (run.kind == RunKind::Emoji || run.kind == RunKind::CustomEmoji)
			emoji.push_back(run);
	}
	REQUIRE(emoji.size() == 5);
	CHECK_EQ(emoji[0].text, kSmile);
	CHECK_EQ(emoji[0].target, std::string("smile"));
	CHECK_EQ(emoji[1].text, kThumbsUp + kToneMedium);
	CHECK_EQ(emoji[1].target, std::string("+1::skin-tone-3"));
	CHECK(emoji[2].kind == RunKind::CustomEmoji);
	CHECK_EQ(emoji[2].target, std::string("partyparrot"));
	CHECK_EQ(emoji[2].text, std::string(":partyparrot:"));
	CHECK(emoji[3].kind == RunKind::CustomEmoji);
	CHECK_EQ(emoji[3].target, std::string("partyparrot"));
	CHECK(emoji[4].kind == RunKind::Emoji);   // alias of a standard emoji
	CHECK_EQ(emoji[4].text, kThumbsUp);
	CHECK(text.plainText().find(":nope:") != std::string::npos);
	CHECK(text.plainText().find("10:30:45") != std::string::npos);

	// Without a context, unknown names are just text.
	FormattedText bare = formatMrkdwn(":partyparrot:");
	REQUIRE(bare.runs.size() == 1);
	CHECK(bare.runs[0].kind == RunKind::Text);
	FormattedText styled = formatMrkdwn("*yay :tada:*");
	REQUIRE(styled.runs.size() == 2);
	CHECK(styled.runs[1].kind == RunKind::Emoji);
	CHECK_EQ(styled.runs[1].style, uint32_t(kStyleBold));
}

// ---- rich_text blocks ---------------------------------------------------------------------

TEST(mrkdwn_rich_text_blocks)
{
	json history = parseJson(testing::fixture("conversations.history.json"));
	Message message = Message::fromJson(history["messages"][4], "C01GENERAL");
	FormattedText text = formatMessage(message, testContext());
	std::vector<Run> expected;
	expected.push_back(textRun("Plan for "));
	expected.push_back(textRun("today", kStyleBold));
	expected.push_back(textRun(" with "));
	Run bob;
	bob.kind = RunKind::UserMention;
	bob.text = "@Bob Brown";
	bob.target = "U02BOB";
	expected.push_back(bob);
	expected.push_back(textRun(" "));
	Run rocket;
	rocket.kind = RunKind::Emoji;
	rocket.text = kRocket;
	rocket.target = "rocket";
	expected.push_back(rocket);
	expected.push_back(lineBreak());
	Run bullet;
	bullet.kind = RunKind::ListMarker;
	bullet.text = "\xE2\x80\xA2 ";
	expected.push_back(bullet);
	expected.push_back(textRun("ship it"));
	expected.push_back(lineBreak());
	expected.push_back(bullet);
	Run link;
	link.kind = RunKind::Link;
	link.text = "read the doc";
	link.target = "https://example.com/doc";
	expected.push_back(link);
	expected.push_back(lineBreak());
	expected.push_back(textRun("make test", kStyleCodeBlock));
	expected.push_back(lineBreak());
	expected.push_back(textRun("quoted words", kStyleQuote));
	expectRuns(text, expected, __LINE__);
}


TEST(mrkdwn_rich_text_details)
{
	json blocks = parseJson(R"([
		{"type":"section","text":{"type":"mrkdwn","text":"ignored"}},
		{"type":"rich_text","elements":[
			{"type":"rich_text_list","style":"ordered","indent":1,"offset":2,"elements":[
				{"type":"rich_text_section","elements":[
					{"type":"text","text":"x","style":{"strike":true,"code":true}}]}]},
			{"type":"rich_text_section","elements":[
				{"type":"broadcast","range":"channel"},
				{"type":"emoji","name":"wave","skin_tone":6},
				{"type":"emoji","name":"partyparrot"},
				{"type":"emoji","name":"brand_new","unicode":"1f9ea"},
				{"type":"channel","channel_id":"C01GENERAL"},
				{"type":"usergroup","usergroup_id":"S0DEVS"},
				{"type":"date","timestamp":1700000000,"format":"{date}","fallback":"Nov 14"}]}]}])");
	FormattedText text = formatRichText(blocks, testContext());
	REQUIRE(text.runs.size() >= 9);
	CHECK(text.runs[0].kind == RunKind::ListMarker);
	CHECK_EQ(text.runs[0].text, std::string("3. "));
	CHECK_EQ(text.runs[0].indent, 1);
	CHECK_EQ(text.runs[1].style, uint32_t(kStyleStrike | kStyleCode));
	CHECK(text.runs[2].kind == RunKind::LineBreak);
	CHECK(text.runs[3].kind == RunKind::Broadcast);
	CHECK(text.mentionsSelf);
	CHECK_EQ(text.runs[4].text, std::string("\xF0\x9F\x91\x8B\xF0\x9F\x8F\xBF"));
	CHECK(text.runs[5].kind == RunKind::CustomEmoji);
	CHECK_EQ(text.runs[6].text, std::string("\xF0\x9F\xA7\xAA"));
	CHECK_EQ(text.runs[7].text, std::string("#general"));
	CHECK_EQ(text.runs[8].text, std::string("@devs"));
	CHECK(text.runs[9].kind == RunKind::Date);
	CHECK_EQ(text.runs[9].text, std::string("Nov 14"));

	// No rich_text: formatMessage falls back to the mrkdwn text.
	Message message;
	message.text = "*fallback*";
	message.blocks = parseJson(R"([{"type":"section"}])");
	FormattedText fallback = formatMessage(message);
	REQUIRE(fallback.runs.size() == 1);
	CHECK_EQ(fallback.runs[0].style, uint32_t(kStyleBold));
	CHECK(formatRichText(json()).runs.empty());
}


TEST(mrkdwn_store_context)
{
	Store store;
	store.setSelf("U03SELF");
	User alice;
	alice.id = "U01ALICE";
	alice.name = "alice";
	alice.displayName = "ali";
	store.upsertUser(alice);
	Channel general;
	general.id = "C01GENERAL";
	general.name = "general";
	store.upsertChannel(general);
	store.setEmoji({{"partyparrot", "https://e/p.gif"}, {"pp", "alias:partyparrot"},
		{"yes", "alias:white_check_mark"}});
	FormatContext context = store.formatContext();
	FormattedText text = formatMrkdwn("<@U01ALICE> in <#C01GENERAL> :pp: :yes: <@U03SELF>",
		context);
	CHECK_EQ(text.plainText(), std::string("@ali in #general :pp: \xE2\x9C\x85 @U03SELF"));
	CHECK(text.mentionsSelf);
	CHECK_EQ(store.customEmojiUrl("pp"), std::string("https://e/p.gif"));
	CHECK_EQ(store.resolveCustomEmoji("yes"), std::string("white_check_mark"));
	CHECK_EQ(store.resolveCustomEmoji("nothing"), std::string(""));
}
