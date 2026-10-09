// resetscore — Plugify C++ port of the Pisex Metamod:Source plugin "ResetScore"
// (ref/src/cs2-resetscore/resetscore.cpp, 167 lines).
//
// Behaviour kept from the original:
//   * "mm_rs", "sm_rs" and "rs" (console) and "!rs"/"rs" (chat) reset the caller's score, MVPs and
//     the whole CSPerRoundStats_t stat block of CCSPlayerController_ActionTrackingServices, then
//     print "<Prefix> <Success>" in the caller's chat (the production phrases file is used, and its
//     "language" key - "ru" - selects the language like the original's GetString("language","en")).
//   * The command handler returns false, exactly like the original (the "!rs" text is not swallowed
//     by the utils hook there either - see the porting note below).
//
// Porting notes:
//   * The original wrote m_iScore/m_iMVPs with SchemaEntity's plain accessors and then called
//     SetStateChanged(pController, "CCSPlayerController", "m_pInventoryServices"). The port writes
//     the two networked controller fields through s2sdk's schema setter with changeState=true, so
//     the scoreboard really updates; the original's SetStateChanged call is dropped because
//     m_pInventoryServices is a pointer field that carries none of the reset values (announcing it
//     would set a change bit on a field this command never writes). The stat struct is written directly; each written field
//     is announced as changed through its own offset (a networked leaf, chained through the ATS
//     class's __m_pChainEntity) so the reset reaches the clients. See API notes in package.sh /
//     the report.
//   * pController->m_pActionTrackingServices() becomes a typed schema read on the controller's
//     entity handle (GetEntSchema returns the raw pointer as a pointer-sized integer); the stat
//     fields are zeroed at <ATS> + off(m_matchStats) + off(field). If the pointer or a field is
//     missing the corresponding part is skipped instead of crashing (s2sdk logs the missing
//     field).
//   * GetCGameEntitySystem()->GetEntityInstance(CEntityIndex(slot + 1)) is pisex::GetControllerEntity
//     (the controller of a slot is entity slot+1, which is what s2sdk's PlayerSlotToEntPointer does).
//
// Fail soft everywhere: OnPluginStart never returns Failed (that stops every Plugify plugin), and
// no client is queried before a map exists (commands only fire with a live map).

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <pisex/pisex.hpp>
#include <pisex/schema.hpp>

#include <s2sdk/s2sdk.hpp>

#include "reset_plan.hpp"

namespace {

constexpr const char* kPhrasesFile = "resetscore.phrases.txt";

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogWarn(std::string_view message) {
	LogMessage(message, plg::Severity::Warning);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

pisex::PhraseMap g_phrases;

// g_vecPhrases[key] of the original (std::map::operator[] on a missing key printed nothing).
const std::string& PhraseText(const std::string& key) {
	static const std::string empty;
	const auto it = g_phrases.find(key);
	return it == g_phrases.end() ? empty : it->second;
}

void LoadPhrases() {
	pisex::KeyValues kv("Phrases");
	if (!kv.LoadFromFile(kPhrasesFile)) {
		LogWarn(std::string("resetscore: failed to load ") + kPhrasesFile +
				" - the command will print empty messages");
		return;
	}
	// The original: std::string szLanguage = g_kvPhrases->GetString("language", "en");
	pisex::SetPhrasesLanguage(kv.GetString("language", "en"));
	pisex::LoadPhrasesFile(kPhrasesFile, g_phrases);
}

// ClientPrint() of the original: "<Prefix> <message>" through PrintToChat. The original formatted
// into a buffer and passed that buffer as the printf *format* (an empty or %-bearing phrase would
// have been undefined behaviour); the port passes the two phrases as arguments of "%s %s".
void ClientPrint(int iSlot, const char* key) {
	// The keys are hoisted so no temporary is bound to the returned reference (GCC's
	// -Wdangling-reference flags the direct PhraseText("...") spelling).
	const std::string prefixKey = "Prefix";
	const std::string textKey(key == nullptr ? "" : key);
	pisex::PrintToChat(iSlot, "%s %s", PhraseText(prefixKey).c_str(), PhraseText(textKey).c_str());
}

// The ATS pointer of a controller (nullptr when the controller or the field is missing). The
// field is a raw pointer, which the typed schema read returns as a pointer-sized integer; the
// read itself validates the entity handle (an invalid handle / unknown field answers 0).
void* ActionTrackingServices(int iController) {
	if (__s2sdk_GetEntSchema == nullptr) {
		return nullptr;
	}
	try {
		// -1 for an index without a live entity (pisex throws for an invalid one).
		const int iHandle = pisex::EntIndexToEntHandle(iController);
		if (!resetscore::IsValidHandle(iHandle)) {
			return nullptr;
		}
		return reinterpret_cast<void*>(static_cast<uintptr_t>(s2sdk::GetEntSchema(
				iHandle, plg::string(resetscore::kActionTrackingPointerClass),
				plg::string(resetscore::kActionTrackingPointerField), 0)));
	} catch (...) {
		return nullptr;
	}
}

// OnRsCommand of the original.
void ResetScore(int iSlot) {
	// CCSPlayerController::FromSlot(iSlot) (0 when the slot has no controller).
	const int iController = pisex::GetControllerEntity(iSlot);
	if (iController == 0) {
		LogWarn("resetscore: slot " + std::to_string(iSlot) + " has no controller - nothing was reset");
		return;
	}

	// m_iScore / m_iMVPs - networked controller fields, written with a state change so the
	// scoreboard updates (the original's plain writes plus SetStateChanged).
	if (__s2sdk_SetEntSchema != nullptr) {
		const int iHandle = pisex::EntIndexToEntHandle(iController);
		if (resetscore::IsValidHandle(iHandle)) {
			for (const resetscore::ControllerField& field : resetscore::kControllerFields) {
				try {
					s2sdk::SetEntSchema(iHandle, plg::string(field.className), plg::string(field.fieldName), 0,
										/*changeState=*/true, 0);
				} catch (const std::exception& ex) {
					LogError(std::string("resetscore: writing ") + field.className + "::" + field.fieldName +
							 " failed: " + ex.what());
				} catch (...) {
					LogError(std::string("resetscore: writing ") + field.className + "::" + field.fieldName +
							 " failed");
				}
			}
		}
	}

	// m_ATS->m_matchStats().m_iXXX() = 0 for the 13 fields.
	void* ats = ActionTrackingServices(iController);
	if (ats == nullptr) {
		return; // the score/MVPs part above already ran; the stats need the struct
	}
	const int32_t containerOffset = pisex::SchemaOffset(resetscore::kMatchStatsClass, resetscore::kMatchStatsField);
	if (containerOffset < 0) {
		LogError("resetscore: schema field CCSPlayerController_ActionTrackingServices::m_matchStats not found");
		return;
	}
	if (!__s2sdk_SetEntData2) {
		return;
	}
	// The ATS object is a component with a chainer of its own (CCSPlayerController_ActionTracking
	// Services::__m_pChainEntity): announcing a change goes through it, with the field's own offset
	// inside the ATS as the local offset. Asked per reset (a command, cold): a cached failure would disable the announcement for good.
	const int32_t chainOffset = pisex::SchemaChainOffset(resetscore::kMatchStatsClass);
	// Only the stat fields themselves may be announced: each is a networked int32 LEAF, while
	// announcing their m_matchStats container (what this used to do) makes the engine print
	// "requested resolve all N changes, actually resolved only M" plus the controller's whole send
	// table on every reset - the same engine behaviour WeaponSkins documents for m_AttributeManager
	// / m_EconGloves. Without a resolvable chainer nothing is announced: s2sdk's
	// SafeNetworkStateChanged falls back to chain offset 0 there, which makes the engine read the
	// object itself as a chainer.
	const bool announce = chainOffset > 0;
	bool anyMissing = false;
	for (const char* field : resetscore::kMatchStatFields) {
		const int32_t fieldOffset = pisex::SchemaOffset(resetscore::kMatchStatsFieldClass, field);
		if (fieldOffset < 0) {
			anyMissing = true;
			continue;
		}
		// changeState=true is the s2sdk equivalent of the original's SetStateChanged: the write is
		// handed to the clients. SetEntData2 announces before it writes, which is fine - the value
		// is read when the snapshot is taken, later in the frame.
		s2sdk::SetEntData2(ats, containerOffset + fieldOffset, 0, 4, /*changeState=*/announce,
						   /*chainOffset=*/chainOffset);
	}
	if (anyMissing) {
		LogWarn("resetscore: some match stats fields are missing from this game build");
	}
}

class ResetScorePlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			pisex::Init("resetscore");
			LoadPhrases();
			// RegCommand(g_PLID, {"mm_rs", "sm_rs", "rs"}, {"!rs", "rs"}, OnRsCommand).
			pisex::g_pUtils->RegCommand({"mm_rs", "sm_rs", "rs"}, {"!rs", "rs"}, [](int iSlot, const char*) -> bool {
				try {
					// The original printed first, then reset.
					ClientPrint(iSlot, "Success");
					ResetScore(iSlot);
				} catch (const std::exception& ex) {
					LogError(std::string("resetscore: command failed: ") + ex.what());
				} catch (...) {
					LogError("resetscore: command failed with an unknown exception");
				}
				return false; // the original returned false (the engine keeps the command/message)
			});
			LogInfo("resetscore: ready ('mm_rs'/'sm_rs'/'rs' and '!rs'/'rs' registered)");
		} catch (const std::exception& ex) {
			LogError(std::string("resetscore: OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("resetscore: OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		try {
			pisex::Poll();
		} catch (...) {
			LogError("resetscore: Poll() threw");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			pisex::Shutdown(); // removes the commands and the s2sdk listeners
		} catch (const std::exception& ex) {
			LogError(std::string("resetscore: OnPluginEnd failed: ") + ex.what());
		} catch (...) {
			LogError("resetscore: OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

ResetScorePlugin g_resetScorePlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_resetScorePlugin)
