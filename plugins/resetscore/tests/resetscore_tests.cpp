// Tests for plugins/resetscore: the engine-free parts of the port.
//
//   1. resetscore.phrases.txt (the shipped file): the "language" key the original read (with the
//      same "en" default) and the ru/en resolution through pisexcompat, exactly the way the
//      plugin does it at startup.
//   2. The schema-write plan (src/reset_plan.hpp) against the original's field list - two
//      controller fields and the 13 stat fields, in the original's order, resolved through
//      pisexcompat's KeyValues-style spelling.
//
// The s2sdk bindings are null here (no engine), which is also the state the ported code must
// survive; only the pure data parts are checked.

#include <cstdio>
#include <string>
#include <vector>

#include <plg/plugin.hpp>

#include <pisex/keyvalues.hpp>
#include <pisex/paths.hpp>
#include <pisex/phrases.hpp>

#include "reset_plan.hpp"

// A real plugin gets these from PLUGIFY_PLUGIN; the test leaves them null on purpose.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

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

void TestPhrases() {
	pisex::SetTranslationsDir(RS_TEST_PHRASES_DIR);

	// The language key the plugin reads at startup (default "en").
	pisex::KeyValues kv("Phrases");
	Check(kv.LoadFromFile("resetscore.phrases.txt"), "the live resetscore.phrases.txt is readable");
	CheckEqStr(kv.GetString("language", "en"), "ru", "the file selects Russian");
	CheckEqStr(kv.GetString("no_such_key", "en"), "en", "a missing key keeps the original's \"en\" default");

	// ru: the shipped texts, with the colour tags the chat printer expects.
	pisex::SetPhrasesLanguage("ru");
	pisex::PhraseMap russian;
	Check(pisex::LoadPhrasesFile("resetscore.phrases.txt", russian), "ru phrases load");
	CheckEqStr(russian["Prefix"], "[{RED}RS{DEFAULT}]", "ru Prefix");
	CheckEqStr(russian["Success"], "{PURPLE}Вы успешно сбросили свой счёт", "ru Success");

	// en: the fallback column of the same file (the original would print the raw ru column for an
	// en server; pisexcompat resolves the column for the requested language instead - the
	// documented difference of the translated phrase loader).
	pisex::PhraseMap english;
	Check(pisex::LoadPhrasesFile("resetscore.phrases.txt", english, "en"), "en phrases load");
	CheckEqStr(english["Prefix"], "[{RED}RS{DEFAULT}]", "en Prefix");
	CheckEqStr(english["Success"], "{PURPLE}Your score has been reset!", "en Success");

	// The plugin prints "<Prefix> <Success>" through PrintToChat's "%s %s" - both keys must exist.
	Check(russian.count("Prefix") == 1 && russian.count("Success") == 1, "both phrases the command uses exist");
}

void TestResetPlan() {
	// CCSPlayerController::m_iScore / m_iMVPs, in the original's order.
	CheckEq(static_cast<int>(sizeof(resetscore::kControllerFields) / sizeof(resetscore::kControllerFields[0])), 2,
			"two controller fields");
	CheckEqStr(resetscore::kControllerFields[0].className, "CCSPlayerController", "field 0 class");
	CheckEqStr(resetscore::kControllerFields[0].fieldName, "m_iScore", "field 0 name");
	CheckEqStr(resetscore::kControllerFields[1].fieldName, "m_iMVPs", "field 1 name");

	// The path to the stats struct.
	CheckEqStr(resetscore::kActionTrackingPointerClass, "CCSPlayerController", "ATS pointer class");
	CheckEqStr(resetscore::kActionTrackingPointerField, "m_pActionTrackingServices", "ATS pointer field");
	CheckEqStr(resetscore::kMatchStatsClass, "CCSPlayerController_ActionTrackingServices", "stats container class");
	CheckEqStr(resetscore::kMatchStatsField, "m_matchStats", "stats container field");
	CheckEqStr(resetscore::kMatchStatsFieldClass, "CSPerRoundStats_t", "stats field class");

	// The 13 stat fields of the original, in its order.
	const std::vector<std::string> expected = {
			"m_iKills",		   "m_iDeaths",		  "m_iAssists",		 "m_iDamage",
			"m_iEquipmentValue", "m_iMoneySaved",	"m_iKillReward",	 "m_iLiveTime",
			"m_iHeadShotKills",  "m_iObjective",		"m_iCashEarned",	 "m_iUtilityDamage",
			"m_iEnemiesFlashed",
	};
	CheckEq(static_cast<int>(resetscore::kMatchStatFieldCount), static_cast<int>(expected.size()), "13 stat fields");
	for (std::size_t i = 0; i < expected.size() && i < resetscore::kMatchStatFieldCount; ++i) {
		CheckEqStr(resetscore::kMatchStatFields[i], expected[i], "stat field " + std::to_string(i));
	}
}

void TestHandleValidity() {
	// A real case: index 93, serial 102949 -> 0xC912805D = -921534371, a valid handle the
	// game hands out once its serial counter has passed 65535 (bit 31 set).
	Check(resetscore::IsValidHandle(-921534371), "-921534371 (0xC912805D) is a valid handle");
	Check(resetscore::IsValidHandle(0x7FFFFFFF), "0x7FFFFFFF is a valid handle");
	Check(resetscore::IsValidHandle(1), "handle 1 is valid");
	Check(!resetscore::IsValidHandle(0), "0 (the world / \"none\") is not a valid handle");
	Check(!resetscore::IsValidHandle(-1), "-1 (INVALID_EHANDLE_INDEX) is not a valid handle");
}

} // namespace

int main() {
	TestPhrases();
	TestResetPlan();
	TestHandleValidity();
	std::printf("resetscore_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
