// Tests for plugins/resetscore: everything in the port that does not need an engine.
//
//   1. The shipped phrases file (configs/resetscore/translations/resetscore.phrases.txt, i.e. the
//      original's translations file) through the plugin's own KV1 reader: the "language" key the
//      original read (same "en" default), the ru/en/ua/pl columns, the language fallback, and the
//      malformed-input path (a broken file must be rejected, not half-read).
//   2. Colorize()/StripColorTags(): the cs2-menus tag table byte-for-byte, plus the exact chat line
//      the plugin prints.
//   3. The bare chat word matcher: word splitting and the C11 spellings ("rs", "rs args", "!rs",
//      "/rs").
//   4. The schema-write plan (src/reset_plan.hpp) against the original's field and command lists.
//   5. The map-gated schema offset cache (src/schema_cache.hpp): -1 without a map, a failed lookup
//      retried, only successful offsets cached, and the chain-offset "none" never cached as 0.
//   6. Handle validity (resetscore::IsValidHandle) on real handle values.
//
// No s2sdk, no Plugify host, no engine: the headers under test are included directly, which is
// exactly the code plugin.cpp runs.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "chat_text.hpp"
#include "kv1_phrases.hpp"
#include "reset_plan.hpp"
#include "schema_cache.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEq(int actual, int expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s\n  expected: %d\n  actual:   %d\n", what.c_str(), expected, actual);
	}
}

void CheckEqStr(const std::string& actual, const std::string& expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s\n  expected: [%s]\n  actual:   [%s]\n", what.c_str(), expected.c_str(),
					actual.c_str());
	}
}

// The same path the plugin builds from <GetConfigsDir()> and its kPhrasesRelativePath.
std::string PhrasesPath() {
	return std::string(RS_TEST_DATA_DIR) + "/resetscore.phrases.txt";
}

// --- 1. the shipped phrases file ----------------------------------------------------------------

void TestPhrases() {
	const std::optional<resetscore::KvNode> root = resetscore::LoadKv1File(PhrasesPath());
	Check(root.has_value(), "the shipped resetscore.phrases.txt is readable: " + PhrasesPath());
	if (!root) {
		return;
	}

	// The language key the plugin reads at startup (default "en"), and the plain value next to the
	// phrase blocks is not a phrase itself.
	CheckEqStr(std::string(root->GetString("language", "en")), "ru", "the file selects Russian");
	CheckEqStr(std::string(root->GetString("no_such_key", "en")), "en",
			   "a missing key keeps the original's \"en\" default");
	Check(root->Find("language") != nullptr && root->Find("language")->children.empty(),
		  "\"language\" is a plain value, not a phrase block");

	// ru: the shipped texts, with the colour tags the chat printer expects.
	const resetscore::PhraseMap russian = resetscore::ResolvePhrases(*root, "ru");
	Check(!russian.empty(), "ru phrases load");
	CheckEqStr(russian.at("Prefix"), "[{RED}RS{DEFAULT}]", "ru Prefix");
	CheckEqStr(russian.at("Success"), "{PURPLE}Вы успешно сбросили свой счёт", "ru Success");

	// en: the fallback column of the same file (the original would have printed the raw ru text or
	// nothing; both pisexcompat and this reader resolve the column for the requested language).
	const resetscore::PhraseMap english = resetscore::ResolvePhrases(*root, "en");
	Check(!english.empty(), "en phrases load");
	CheckEqStr(english.at("Prefix"), "[{RED}RS{DEFAULT}]", "en Prefix");
	CheckEqStr(english.at("Success"), "{PURPLE}Your score has been reset!", "en Success");

	// The other columns the original shipped - the file is used as it is, they all stay.
	const resetscore::PhraseMap ukrainian = resetscore::ResolvePhrases(*root, "ua");
	CheckEqStr(ukrainian.at("Success"), "{PURPLE}Ви обнулили свій рахунок!", "ua Success");
	const resetscore::PhraseMap polish = resetscore::ResolvePhrases(*root, "pl");
	CheckEqStr(polish.at("Success"), "{PURPLE}Twój wynik został zresetowany!", "pl Success");

	// A language the file does not carry falls back to "en" (never to an empty message).
	const resetscore::PhraseMap german = resetscore::ResolvePhrases(*root, "de");
	CheckEqStr(german.at("Success"), english.at("Success"), "an unknown language falls back to en");

	// The plugin prints "<Prefix> <Success>" through PrintToChat - both keys must exist and nothing
	// else is needed.
	Check(russian.count("Prefix") == 1 && russian.count("Success") == 1,
		  "both phrases the command uses exist");
	Check(russian.size() == 2, "the file carries exactly the two phrase blocks the plugin prints");
}

// --- 1b. the KV1 reader's failure modes ---------------------------------------------------------

void TestKv1FailureModes() {
	// A missing file is reported, not thrown.
	Check(!resetscore::LoadKv1File(std::string(RS_TEST_DATA_DIR) + "/no/such/file.txt").has_value(),
		  "a missing phrases file is nullopt");

	// The three malformed shapes the reader rejects as a whole.
	Check(!resetscore::ParseKv1(R"("Phrases" { "Prefix" { "ru" "broken } })").has_value(),
		  "an unterminated string rejects the file");
	Check(!resetscore::ParseKv1(R"("Phrases" { "Prefix" { "ru" "ok" })").has_value(),
		  "a missing '}' rejects the file");
	Check(!resetscore::ParseKv1(R"("Phrases" { } })").has_value(), "a stray '}' rejects the file");

	// Balanced empty input is fine (a plugin with an empty translation table).
	Check(resetscore::ParseKv1("").has_value(), "an empty file parses as an empty tree");

	// The rest of the accepted grammar: comments, #base lines and the quoted escapes.
	const std::optional<resetscore::KvNode> root = resetscore::ParseKv1(
			"// a comment\n"
			"#base \"other.phrases.txt\"\n"
			"\"Phrases\"\n"
			"{\n"
			"    /* block comment */\n"
			"    \"language\" \"ru\"\n"
			"    \"K\" { \"ru\" \"a\\tb\\nc\\\"d\\\\e\" }\n"
			"}\n");
	Check(root.has_value(), "comments and #base lines parse");
	if (!root) {
		return;
	}
	CheckEqStr(std::string(root->GetString("language", "en")), "ru", "#base did not replace the tree");
	Check(root->Find("k") != nullptr, "key lookup is case-insensitive");
	const resetscore::PhraseMap ru = resetscore::ResolvePhrases(*root, "ru");
	CheckEqStr(ru.at("K"), "a\tb\nc\"d\\e", "the \\t \\n \\\" and \\\\ escapes are decoded");
	// A block without a language matching either the request or "en" falls back to its first value.
	const resetscore::PhraseMap miss = resetscore::ResolvePhrases(*root, "en");
	CheckEqStr(miss.at("K"), "a\tb\nc\"d\\e", "a phrase without en falls back to the first column");
}

// --- 2. Colorize --------------------------------------------------------------------------------

void TestColorize() {
	// The whole cs2-menus table, byte for byte (pisexcompat sent these same bytes to the chat).
	struct Tag {
		const char* tag;
		const char* bytes;
	};
	const Tag tags[] = {{"{DEFAULT}", "\x01"},	  {"{WHITE}", "\x01"},	{"{RED}", "\x02"},
						{"{LIGHTPURPLE}", "\x03"}, {"{GREEN}", "\x04"},	{"{LIME}", "\x05"},
						{"{LIGHTGREEN}", "\x06"},  {"{DARKRED}", "\x07"},	{"{GRAY}", "\x08"},
						{"{LIGHTOLIVE}", "\x09"},  {"{OLIVE}", "\x10"},	{"{LIGHTBLUE}", "\x0B"},
						{"{BLUE}", "\x0C"},		   {"{PURPLE}", "\x0E"},	{"{LIGHTRED}", "\x0F"},
						{"{GRAYBLUE}", "\x0A"}};
	for (const Tag& tag : tags) {
		CheckEqStr(resetscore::Colorize(tag.tag), tag.bytes, std::string("Colorize ") + tag.tag);
	}

	// The chat line break the phrase files spell "\n" (backslash + n) becomes U+2029.
	CheckEqStr(resetscore::Colorize("a\\nb"), "a" "\xe2\x80\xa9" "b", "the \\n line-break tag");

	// Several tags in one string, mixed with plain text; unknown tags and lone braces stay as they
	// are (only the exact upper-case spellings of the table are replaced).
	CheckEqStr(resetscore::Colorize("[{RED}RS{DEFAULT}] {PURPLE}your score"),
			   "[" "\x02" "RS" "\x01" "] " "\x0E" "your score", "several tags in one line");
	CheckEqStr(resetscore::Colorize("{PINK}keep {red} {RED"), "{PINK}keep {red} {RED",
			   "unknown and lower-case tags stay");
	CheckEqStr(resetscore::Colorize(""), "", "empty text stays empty");
	CheckEqStr(resetscore::Colorize("no tags"), "no tags", "plain text is unchanged");

	// The production lines the plugin prints, byte for byte (<Prefix> " " <Success>).
	CheckEqStr(resetscore::Colorize("[{RED}RS{DEFAULT}] {PURPLE}Your score has been reset!"),
			   "[" "\x02" "RS" "\x01" "] " "\x0E" "Your score has been reset!", "the en chat line");
	CheckEqStr(resetscore::Colorize("[{RED}RS{DEFAULT}] {PURPLE}Вы успешно сбросили свой счёт"),
			   "[" "\x02" "RS" "\x01" "] " "\x0E" "Вы успешно сбросили свой счёт", "the ru chat line");

	// Stripping: the same tags removed, the line break kept as a real newline (console output).
	CheckEqStr(resetscore::StripColorTags("[{RED}RS{DEFAULT}] {PURPLE}ok"), "[RS] ok", "StripColorTags text");
	CheckEqStr(resetscore::StripColorTags("a\\nb"), "a\nb", "StripColorTags keeps the line break");
}

// --- 3. the bare chat word ----------------------------------------------------------------------

void TestChatWord() {
	// A CS2 client sends the whole say line as ONE argument, so the plugin splits it like Pisex's
	// say hook did; a console `say rs now` arrives one word per argument instead.
	const std::vector<std::string> single = resetscore::SplitChatWords("rs");
	Check(single.size() == 1 && single.front() == "rs", "a one-word line");
	const std::vector<std::string> withArgs = resetscore::SplitChatWords("rs  top now");
	Check(withArgs.size() == 3 && withArgs.front() == "rs", "\"rs args\" keeps the first token");
	Check(resetscore::SplitChatWords("").empty(), "an empty line has no words");
	Check(resetscore::SplitChatWords("   ").empty(), "a blank line has no words");
	const std::vector<std::string> quoted = resetscore::SplitChatWords("\"rs two words\" x");
	Check(quoted.size() == 2 && quoted.front() == "rs two words", "a quoted run groups one word");

	// The match itself: case-insensitive, bare, exact first token.
	Check(resetscore::MatchesChatWord("rs", resetscore::kChatWord), "\"rs\" matches");
	Check(resetscore::MatchesChatWord("RS", resetscore::kChatWord), "\"RS\" matches (case-insensitive)");
	Check(resetscore::MatchesChatWord("Rs", resetscore::kChatWord), "\"Rs\" matches");
	Check(!resetscore::MatchesChatWord("rsx", resetscore::kChatWord), "\"rsx\" does not match");
	Check(!resetscore::MatchesChatWord("r", resetscore::kChatWord), "\"r\" does not match");
	Check(!resetscore::MatchesChatWord("!rs", resetscore::kChatWord),
		  "\"!rs\" belongs to s2sdk's chat-command path, not to the bare word");
	Check(!resetscore::MatchesChatWord("/rs", resetscore::kChatWord), "\"/rs\" does not match either");
	Check(!resetscore::MatchesChatWord("", resetscore::kChatWord), "an empty first word does not match");
	Check(!resetscore::MatchesChatWord("rs", ""), "an empty word does not match");

	// Both say shapes end up at the same first word.
	const std::vector<std::string> clientLine = resetscore::SplitChatWords("rs");
	const std::vector<std::string> consoleLine = {"rs", "now"};
	Check(!clientLine.empty() && !consoleLine.empty() &&
				  resetscore::MatchesChatWord(clientLine.front(), resetscore::kChatWord) &&
				  resetscore::MatchesChatWord(consoleLine.front(), resetscore::kChatWord),
		  "both say shapes match the same word");
}

// --- 4. the reset plan --------------------------------------------------------------------------

void TestResetPlan() {
	// CCSPlayerController::m_iScore / m_iMVPs, in the original's order.
	CheckEq(static_cast<int>(resetscore::kControllerFieldCount), 2, "two controller fields");
	CheckEqStr(resetscore::kControllerFields[0].className, "CCSPlayerController", "field 0 class");
	CheckEqStr(resetscore::kControllerFields[0].fieldName, "m_iScore", "field 0 name");
	CheckEqStr(resetscore::kControllerFields[1].fieldName, "m_iMVPs", "field 1 name");

	// The path to the stats struct.
	CheckEqStr(resetscore::kActionTrackingPointerClass, "CCSPlayerController", "ATS pointer class");
	CheckEqStr(resetscore::kActionTrackingPointerField, "m_pActionTrackingServices", "ATS pointer field");
	CheckEqStr(resetscore::kMatchStatsClass, "CCSPlayerController_ActionTrackingServices",
			   "stats container class");
	CheckEqStr(resetscore::kMatchStatsField, "m_matchStats", "stats container field");
	CheckEqStr(resetscore::kMatchStatsFieldClass, "CSPerRoundStats_t", "stats field class");

	// The 13 stat fields of the original, in its order.
	const std::vector<std::string> expected = {
			"m_iKills",		 "m_iDeaths",		"m_iAssists",	   "m_iDamage",		 "m_iEquipmentValue",
			"m_iMoneySaved", "m_iKillReward",	"m_iLiveTime",	   "m_iHeadShotKills", "m_iObjective",
			"m_iCashEarned", "m_iUtilityDamage", "m_iEnemiesFlashed",
	};
	CheckEq(static_cast<int>(resetscore::kMatchStatFieldCount), static_cast<int>(expected.size()),
			"13 stat fields");
	for (std::size_t i = 0; i < expected.size() && i < resetscore::kMatchStatFieldCount; ++i) {
		CheckEqStr(resetscore::kMatchStatFields[i], expected[i], "stat field " + std::to_string(i));
	}

	// The command set of the original's RegCommand({"mm_rs", "sm_rs", "rs"}, {"!rs", "rs"}, ...).
	CheckEq(static_cast<int>(resetscore::kCommandCount), 3, "three console spellings");
	CheckEqStr(resetscore::kCommands[0].name, "mm_rs", "command 0");
	CheckEqStr(resetscore::kCommands[1].name, "sm_rs", "command 1");
	CheckEqStr(resetscore::kCommands[2].name, "rs", "command 2");
	Check(!resetscore::HasSource(resetscore::kCommands[0].source, resetscore::CommandSource::Chat),
		  "mm_rs is console-only");
	Check(!resetscore::HasSource(resetscore::kCommands[1].source, resetscore::CommandSource::Chat),
		  "sm_rs is console-only");
	Check(resetscore::HasSource(resetscore::kCommands[2].source, resetscore::CommandSource::Chat) &&
				  resetscore::HasSource(resetscore::kCommands[2].source, resetscore::CommandSource::Console),
		  "rs answers console and chat");
	Check(resetscore::HasSource(resetscore::CommandSource::Both, resetscore::CommandSource::Console) &&
				  resetscore::HasSource(resetscore::CommandSource::Both, resetscore::CommandSource::Chat),
		  "Both means both sources");
	CheckEqStr(std::string(resetscore::kChatWord), "rs", "the bare chat word");
	CheckEqStr(std::string(resetscore::kPhrasesRelativePath),
			   "resetscore/translations/resetscore.phrases.txt",
			   "the production phrases path (under <GetConfigsDir()>)");
}

// --- 5. the map-gated schema offset cache -------------------------------------------------------

int g_offsetCalls = 0;
int g_chainCalls = 0;
int32_t g_offsetAnswer = -1;
int32_t g_chainAnswer = -1;
const char* g_lastField = nullptr;

int32_t FakeOffsetLookup(const char* /*className*/, const char* fieldName) {
	++g_offsetCalls;
	g_lastField = fieldName;
	return g_offsetAnswer;
}

int32_t FakeChainLookup(const char* /*className*/) {
	++g_chainCalls;
	return g_chainAnswer;
}

void ResetFakes() {
	g_offsetCalls = 0;
	g_chainCalls = 0;
	g_offsetAnswer = -1;
	g_chainAnswer = -1;
	g_lastField = nullptr;
}

void TestSchemaGating() {
	resetscore::SchemaOffsetCache cache;

	// 1. No map: -1 and the lookup never runs (GetSchemaOffset would segfault on the null
	//    serializer map, PITFALLS C8).
	ResetFakes();
	g_offsetAnswer = 8; // a hint that must be ignored while the gate is closed
	CheckEq(cache.Offset(/*mapReady=*/false, "CCSPlayerController_ActionTrackingServices", "m_matchStats",
						 &FakeOffsetLookup),
			-1, "Offset without a map is -1");
	CheckEq(g_offsetCalls, 0, "Offset without a map does not call s2sdk");
	g_chainAnswer = 16;
	CheckEq(cache.ChainOffset(/*mapReady=*/false, "CCSPlayerController_ActionTrackingServices",
							  &FakeChainLookup),
			-1, "ChainOffset without a map is -1");
	CheckEq(g_chainCalls, 0, "ChainOffset without a map does not call s2sdk");
	CheckEq(static_cast<int>(cache.CachedOffsetCount()), 0, "nothing was cached without a map");
	CheckEq(static_cast<int>(cache.CachedChainOffsetCount()), 0, "no chain offset was cached without a map");

	// 2. A successful field offset is returned and memoised (the second call skips the lookup).
	ResetFakes();
	g_offsetAnswer = 16;
	CheckEq(cache.Offset(true, "CSPerRoundStats_t", "m_iKills", &FakeOffsetLookup), 16, "a field offset");
	CheckEq(g_offsetCalls, 1, "the first lookup ran");
	CheckEqStr(g_lastField == nullptr ? "" : g_lastField, "m_iKills", "the lookup got the field name");
	CheckEq(cache.Offset(true, "CSPerRoundStats_t", "m_iKills", &FakeOffsetLookup), 16, "the cached offset");
	CheckEq(g_offsetCalls, 1, "a cached offset is not looked up again");
	CheckEq(static_cast<int>(cache.CachedOffsetCount()), 1, "one cached field offset");

	// 3. Offset 0 is a real offset (the first schema member) and is cached like any success.
	ResetFakes();
	g_offsetAnswer = 0;
	CheckEq(cache.Offset(true, "CSPerRoundStats_t", "m_iDeaths", &FakeOffsetLookup), 0, "offset 0 is valid");
	CheckEq(cache.Offset(true, "CSPerRoundStats_t", "m_iDeaths", &FakeOffsetLookup), 0, "offset 0 is cached");
	CheckEq(g_offsetCalls, 1, "offset 0 is cached (>= 0)");

	// 4. A failed lookup is retried on the next call instead of poisoning the field for the session.
	ResetFakes();
	g_offsetAnswer = -1;
	CheckEq(cache.Offset(true, "CSPerRoundStats_t", "m_iAssists", &FakeOffsetLookup), -1, "a missing field");
	g_offsetAnswer = 24; // e.g. the serializer came up in the meantime
	CheckEq(cache.Offset(true, "CSPerRoundStats_t", "m_iAssists", &FakeOffsetLookup), 24, "the retry succeeds");
	CheckEq(g_offsetCalls, 2, "the failure was not cached");
	CheckEq(cache.Offset(true, "CSPerRoundStats_t", "m_iAssists", &FakeOffsetLookup), 24, "then cached");
	CheckEq(g_offsetCalls, 2, "the success is cached");

	// 5. Empty names and a null lookup are refused without touching s2sdk.
	ResetFakes();
	CheckEq(cache.Offset(true, nullptr, "m_iKills", &FakeOffsetLookup), -1, "null class");
	CheckEq(cache.Offset(true, "", "m_iKills", &FakeOffsetLookup), -1, "empty class");
	CheckEq(cache.Offset(true, "CSPerRoundStats_t", nullptr, &FakeOffsetLookup), -1, "null field");
	CheckEq(cache.Offset(true, "CSPerRoundStats_t", "", &FakeOffsetLookup), -1, "empty field");
	CheckEq(cache.Offset(true, "CSPerRoundStats_t", "m_iKills", nullptr), -1, "no lookup");
	CheckEq(cache.ChainOffset(true, "", &FakeChainLookup), -1, "empty chain class");
	CheckEq(cache.ChainOffset(true, nullptr, &FakeChainLookup), -1, "null chain class");
	CheckEq(cache.ChainOffset(true, "C", nullptr), -1, "no chain lookup");
	CheckEq(g_offsetCalls, 0, "no s2sdk call for an invalid offset request");
	CheckEq(g_chainCalls, 0, "no s2sdk call for an invalid chain request");

	// 6. The chain offset: only > 0 is a usable chainer, and only > 0 is cached (0 would make
	//    s2sdk's SafeNetworkStateChanged read the vtable as the chainer).
	ResetFakes();
	g_chainAnswer = 0;
	CheckEq(cache.ChainOffset(true, "CCSPlayerController_ActionTrackingServices", &FakeChainLookup), -1,
			"chain offset 0 is \"none\"");
	CheckEq(static_cast<int>(cache.CachedChainOffsetCount()), 0, "chain offset 0 is not cached");
	g_chainAnswer = -1;
	CheckEq(cache.ChainOffset(true, "CCSPlayerController_ActionTrackingServices", &FakeChainLookup), -1,
			"an unknown chain offset is -1");
	CheckEq(g_chainCalls, 2, "both failures were retried");
	g_chainAnswer = 32;
	CheckEq(cache.ChainOffset(true, "CCSPlayerController_ActionTrackingServices", &FakeChainLookup), 32,
			"a real chainer");
	CheckEq(g_chainCalls, 3, "the positive chain offset was looked up once");
	CheckEq(cache.ChainOffset(true, "CCSPlayerController_ActionTrackingServices", &FakeChainLookup), 32,
			"the cached chain offset");
	CheckEq(g_chainCalls, 3, "a cached chain offset is not looked up again");
	CheckEq(static_cast<int>(cache.CachedChainOffsetCount()), 1, "one cached chain offset");

	// 7. Clear() forgets everything (OnPluginEnd / a reload re-resolves the fields).
	cache.Clear();
	CheckEq(static_cast<int>(cache.CachedOffsetCount()), 0, "Clear drops the field offsets");
	CheckEq(static_cast<int>(cache.CachedChainOffsetCount()), 0, "Clear drops the chain offsets");
	ResetFakes();
	g_offsetAnswer = 4;
	CheckEq(cache.Offset(true, "CSPerRoundStats_t", "m_iKills", &FakeOffsetLookup), 4,
			"after Clear a lookup runs again");
	CheckEq(g_offsetCalls, 1, "the field was looked up again after Clear");
}

// --- 6. handle validity -------------------------------------------------------------------------

void TestHandleValidity() {
	// A real case: index 93, serial 102949 -> 0xC912805D = -921534371, a valid handle the game
	// hands out once its serial counter has passed 65535 (bit 31 set).
	Check(resetscore::IsValidHandle(-921534371), "-921534371 (0xC912805D) is a valid handle");
	Check(resetscore::IsValidHandle(0x7FFFFFFF), "0x7FFFFFFF is a valid handle");
	Check(resetscore::IsValidHandle(1), "handle 1 is valid");
	Check(!resetscore::IsValidHandle(0), "0 (the world / \"none\") is not a valid handle");
	Check(!resetscore::IsValidHandle(-1), "-1 (INVALID_EHANDLE_INDEX) is not a valid handle");
}

} // namespace

int main() {
	TestPhrases();
	TestKv1FailureModes();
	TestColorize();
	TestChatWord();
	TestResetPlan();
	TestSchemaGating();
	TestHandleValidity();
	std::printf("resetscore_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
