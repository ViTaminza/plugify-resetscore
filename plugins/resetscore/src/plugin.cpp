// resetscore — Plugify C++ port of the Pisex Metamod:Source plugin "ResetScore".
//
// Behaviour kept from the original (ref/src/cs2-resetscore/resetscore.cpp):
//   * "mm_rs", "sm_rs" and "rs" in a console and "!rs" / "rs" in chat reset the caller's score,
//     MVPs and the whole CSPerRoundStats_t stat block of CCSPlayerController_ActionTrackingServices,
//     then print "<Prefix> <Success>" in the caller's chat. The shipped phrases file is the
//     original's translations/resetscore.phrases.txt; its "language" key ("ru") selects the
//     language, with the same "en" default as the original.
//   * Every chat spelling counts as handled (the line is swallowed); a console command continues,
//     which is what the original's callback returning false did.
//   * No other feature: the original had no config file, no database, no permission check.
//
// This plugin calls s2sdk directly - no pisexcompat, no guard library (PLUGIN_STANDARD §7). The
// safety rules of handoff/weaponskins/PITFALLS.md therefore live right here:
//   * B1/B2 map gate: OnMapStart/OnMapEnd listeners plus the first OnPluginUpdate latch g_mapReady;
//     every client, entity and schema call checks it (s2sdk dereferences a torn-down client list
//     or a null serializer map otherwise). OnMapEnd only clears state.
//   * C2 client checks: HasController() = map ready, 0 <= slot < 64, IsClientConnected,
//     IsClientInGame, !IsClientSourceTV and a non-null PlayerSlotToEntPointer; ControllerHandle()
//     adds IsValidEntHandle + IsValidEntPointer on the controller handle before any schema call.
//   * C8 schema lookups: SchemaOffset()/SchemaChainOffset() answer -1 without a ready map and
//     memoise only successful results (>= 0 for a field offset, > 0 for a chain offset, whose
//     "none" must never reach s2sdk as 0 - SafeNetworkStateChanged would read the vtable as a
//     chainer).
//   * C11 commands: a name answers only the sources it was registered for. s2sdk dispatches the
//     chat spellings "!name" / "/name" to *any* console command with ConCommandContext::Chat, so
//     the console-only names ignore that context themselves. The bare chat word "rs" (no "!") only
//     reaches say/say_team listeners and is matched on the first word here.
//   * A7 unload: OnPluginEnd removes every command, every listener and the map listeners again, so
//     `plg reload` leaves nothing behind that points into the unloaded library. A command that
//     already existed (AddConsoleCommand answered false) is detached with RemoveCommandListener,
//     never RemoveCommand - that would unregister the other owner's callbacks.
//
// Data path: m_iScore/m_iMVPs are networked controller fields written through s2sdk's typed
// schema setter with a state change; the 13 stat fields are int32 leaves of the m_matchStats
// struct, reached through the controller's m_pActionTrackingServices pointer (read in the same
// tick, never cached) and written raw at <ATS> + off(m_matchStats) + off(field) with the ATS
// chain offset for the network announcement - the C6-safe path. If a pointer or a field is
// missing, the affected part is skipped instead of crashing.
//
// Fail soft everywhere: OnPluginStart never returns Failed (that stops every Plugify plugin), and
// nothing here touches a client before the map gate is open.

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include "chat_text.hpp"
#include "kv1_phrases.hpp"
#include "reset_plan.hpp"
#include "schema_cache.hpp"

namespace {

// s2sdk player slots are 0..63 (CS2_MAX_SPLITSCREEN_PLAYERS * ... - the engine's MAXPLAYERS).
constexpr int32_t kMaxPlayers = 64;

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

std::string ToString(const plg::string& text) {
	return std::string(std::string_view(text));
}

// --- map gate (PITFALLS B1/B2) ---------------------------------------------------------------

bool g_mapReady = false;      // client/entity/schema calls are safe right now
bool g_mapStartSeen = false;  // an OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;    // an OnMapEnd arrived since OnPluginStart
bool g_mapListenersRegistered = false;

// s2sdk listeners are plain function pointers called on the game thread; nothing here may throw.
void OnMapStartListener() {
	// The engine calls this once the new map and its entity system exist. Readiness is granted
	// here and by the next OnPluginUpdate (the PITFALLS B1 work is only legal from this point on).
	g_mapReady = true;
	g_mapStartSeen = true;
}

void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart s2sdk's client list and entity system are
	// torn down and any client query crashes - so this handler only drops state.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

void RegisterMapListeners() {
	if (g_mapListenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		LogError("resetscore: the s2sdk map listeners are unavailable - every client call stays refused");
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_mapListenersRegistered = true;
}

void UnregisterMapListeners() {
	if (!g_mapListenersRegistered) {
		return;
	}
	g_mapListenersRegistered = false;
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		s2sdk::OnMapStart_Unregister(&OnMapStartListener);
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
	}
}

// --- client and controller checks (PITFALLS C2) ----------------------------------------------

// A map is up and the slot is in range: from here on a client query is safe.
bool CanQueryClient(int32_t slot) {
	return g_mapReady && slot >= 0 && slot < kMaxPlayers;
}

bool HasController(int32_t slot) {
	if (!CanQueryClient(slot)) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot) && s2sdk::IsClientInGame(slot) &&
			   !s2sdk::IsClientSourceTV(slot) && s2sdk::PlayerSlotToEntPointer(slot) != nullptr;
	} catch (...) {
		return false;
	}
}

// The controller's entity handle (index | serial << 15); -1 whenever the slot has no usable
// controller, so every caller can treat -1 as "nothing to do".
int32_t ControllerHandle(int32_t slot) {
	if (!HasController(slot)) {
		return -1;
	}
	if (__s2sdk_PlayerSlotToEntHandle == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr ||
		__s2sdk_IsValidEntHandle == nullptr || __s2sdk_IsValidEntPointer == nullptr) {
		return -1;
	}
	try {
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!resetscore::IsValidHandle(handle) || !s2sdk::IsValidEntHandle(handle)) {
			return -1;
		}
		// The handle and the slot's entity pointer are two views of the same controller and both
		// have to be valid: the schema exports take the handle, while PlayerSlotToEntPointer is
		// what proves the slot still resolves to that entity (between the two reads the engine can
		// already have reclaimed it, and every s2sdk schema call dereferences the handle).
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (controller == nullptr || !s2sdk::IsValidEntPointer(controller)) {
			return -1;
		}
		return handle;
	} catch (...) {
		return -1;
	}
}

// --- phrases (the original's translations/resetscore.phrases.txt) ------------------------------

resetscore::PhraseMap g_phrases;

// <plg::GetConfigsDir()>/<relative>; "" when the plugin runs without the Plugify host.
std::string ConfigPath(std::string_view relative) {
	if (plg::GetConfigsDir == nullptr) {
		return {};
	}
	std::string base = ToString(plg::GetConfigsDir());
	if (base.empty()) {
		return {};
	}
	if (base.back() != '/') {
		base.push_back('/');
	}
	base.append(relative);
	return base;
}

void LoadPhrases() {
	const std::string path = ConfigPath(resetscore::kPhrasesRelativePath);
	if (path.empty()) {
		LogWarn("resetscore: no Plugify configs directory - the command will print empty messages");
		return;
	}
	const std::optional<resetscore::KvNode> root = resetscore::LoadKv1File(path);
	if (!root) {
		LogWarn("resetscore: failed to load " + path + " - the command will print empty messages");
		return;
	}
	// The original: std::string szLanguage = g_kvPhrases->GetString("language", "en");
	const std::string language(root->GetString("language", "en"));
	g_phrases = resetscore::ResolvePhrases(*root, language);
	LogInfo("resetscore: " + std::to_string(g_phrases.size()) + " phrases loaded from " + path +
			" (language \"" + language + "\")");
}

// g_vecPhrases[key] of the original: a missing key printed nothing.
const std::string& PhraseText(const std::string& key) {
	static const std::string empty;
	const auto it = g_phrases.find(key);
	return it == g_phrases.end() ? empty : it->second;
}

// ClientPrint() of the original: "<Prefix> <message>" through the chat, with the cs2-menus colour
// tags turned into their control bytes. The original formatted into a buffer and passed that
// buffer as the printf *format* (undefined behaviour for an empty or %-bearing phrase); the port
// joins the two phrases with the space the "%s %s" produced.
void ClientPrint(int32_t slot, const char* key) {
	if (!HasController(slot) || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	// Hoisted so no temporary is bound to the returned reference.
	const std::string prefixKey = "Prefix";
	const std::string textKey(key == nullptr ? "" : key);
	const std::string prefix(PhraseText(prefixKey));
	const std::string text(PhraseText(textKey));
	const std::string line = prefix + " " + text;
	try {
		s2sdk::PrintToChat(slot, plg::string(resetscore::Colorize(line)));
	} catch (...) {
		LogError("resetscore: PrintToChat threw");
	}
}

// --- schema offsets (PITFALLS C6/C8) ----------------------------------------------------------

resetscore::SchemaOffsetCache g_schemaOffsets;

int32_t LookupSchemaOffset(const char* className, const char* fieldName) {
	try {
		const int32_t offset =
				s2sdk::GetSchemaOffset(plg::string(className), plg::string(fieldName)); // -1: not found
		return offset;
	} catch (...) {
		return -1;
	}
}

int32_t LookupSchemaChainOffset(const char* className) {
	try {
		return s2sdk::GetSchemaChainOffset(plg::string(className));
	} catch (...) {
		return -1;
	}
}

// -1 without a ready map (GetSchemaOffset segfaults before the first map), and a failed lookup is
// retried on the next call instead of poisoning the field for the session.
int32_t SchemaOffset(const char* className, const char* fieldName) {
	if (__s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	return g_schemaOffsets.Offset(g_mapReady, className, fieldName, &LookupSchemaOffset);
}

int32_t SchemaChainOffset(const char* className) {
	if (__s2sdk_GetSchemaChainOffset == nullptr) {
		return -1;
	}
	return g_schemaOffsets.ChainOffset(g_mapReady, className, &LookupSchemaChainOffset);
}

// The ATS pointer of a controller handle (nullptr when the controller or the field is missing).
// The field is a raw pointer, which the typed schema read returns as a pointer-sized integer; the
// read itself validates the entity handle (an invalid handle / unknown field answers 0).
void* ActionTrackingServices(int32_t controllerHandle) {
	if (__s2sdk_GetEntSchema == nullptr || !resetscore::IsValidHandle(controllerHandle)) {
		return nullptr;
	}
	try {
		return reinterpret_cast<void*>(static_cast<uintptr_t>(s2sdk::GetEntSchema(
				controllerHandle, plg::string(resetscore::kActionTrackingPointerClass),
				plg::string(resetscore::kActionTrackingPointerField), 0)));
	} catch (...) {
		return nullptr;
	}
}

// --- the reset itself -------------------------------------------------------------------------

void ResetScore(int32_t slot) {
	// CCSPlayerController::FromSlot(slot): the controller's entity handle, -1 when the slot has no
	// usable controller.
	const int32_t controller = ControllerHandle(slot);
	if (!resetscore::IsValidHandle(controller)) {
		LogWarn("resetscore: slot " + std::to_string(slot) + " has no valid controller - nothing was reset");
		return;
	}

	// m_iScore / m_iMVPs - networked controller fields, written with a state change so the
	// scoreboard updates (the original's plain writes plus SetStateChanged).
	if (__s2sdk_SetEntSchema != nullptr) {
		for (const resetscore::ControllerField& field : resetscore::kControllerFields) {
			try {
				s2sdk::SetEntSchema(controller, plg::string(field.className), plg::string(field.fieldName), 0,
									/*changeState=*/true, 0);
			} catch (const std::exception& ex) {
				LogError(std::string("resetscore: writing ") + field.className + "::" + field.fieldName +
						 " failed: " + ex.what());
			} catch (...) {
				LogError(std::string("resetscore: writing ") + field.className + "::" + field.fieldName + " failed");
			}
		}
	}

	// m_ATS->m_matchStats().m_iXXX() = 0 for the 13 fields. The ATS pointer is read in this tick
	// and never kept (PITFALLS C3).
	void* ats = ActionTrackingServices(controller);
	if (ats == nullptr) {
		return; // the score/MVPs part above already ran; the stats need the struct
	}
	const int32_t containerOffset = SchemaOffset(resetscore::kMatchStatsClass, resetscore::kMatchStatsField);
	if (containerOffset < 0) {
		LogError("resetscore: schema field CCSPlayerController_ActionTrackingServices::m_matchStats not found");
		return;
	}
	if (__s2sdk_SetEntData2 == nullptr) {
		return;
	}
	// The ATS object is a component with a chainer of its own
	// (CCSPlayerController_ActionTrackingServices::__m_pChainEntity): announcing a change goes
	// through it, with the field's own offset inside the ATS as the local offset. Only the stat
	// fields themselves may be announced: each is a networked int32 LEAF, while announcing their
	// m_matchStats container makes the engine print "requested resolve all N changes, actually
	// resolved only M" plus the controller's whole send table on every reset (the same engine
	// behaviour WeaponSkins documents for m_AttributeManager / m_EconGloves). Without a resolvable
	// chainer nothing is announced: s2sdk's SafeNetworkStateChanged treats chain offset 0 as a real
	// chainer and -1 as "this object is an entity", neither of which is true for the ATS.
	const int32_t chainOffset = SchemaChainOffset(resetscore::kMatchStatsClass);
	const bool announce = chainOffset > 0;
	bool anyMissing = false;
	for (const char* field : resetscore::kMatchStatFields) {
		const int32_t fieldOffset = SchemaOffset(resetscore::kMatchStatsFieldClass, field);
		if (fieldOffset < 0) {
			anyMissing = true;
			continue;
		}
		try {
			// changeState=true is the s2sdk equivalent of the original's SetStateChanged: the write
			// is handed to the clients. SetEntData2 announces before it writes, which is fine - the
			// value is read when the snapshot is taken, later in the frame.
			s2sdk::SetEntData2(ats, containerOffset + fieldOffset, 0, 4, /*changeState=*/announce,
							   /*chainOffset=*/chainOffset);
		} catch (const std::exception& ex) {
			LogError(std::string("resetscore: writing CSPerRoundStats_t::") + field + " failed: " + ex.what());
		} catch (...) {
			LogError(std::string("resetscore: writing CSPerRoundStats_t::") + field + " failed");
		}
	}
	if (anyMissing) {
		LogWarn("resetscore: some match stats fields are missing from this game build");
	}
}

// OnRsCommand of the original: print first, then reset.
void RunResetCommand(int32_t caller) {
	if (caller < 0) {
		// The server console has no score of its own (the original dereferenced the controller of
		// slot -1 here and crashed).
		LogInfo("resetscore: the server console has no score to reset");
		return;
	}
	ClientPrint(caller, "Success");
	ResetScore(caller);
}

// The pisexcompat answer the original produced: every chat spelling is handled (the line is
// swallowed), a console command continues (the original's callback returned false).
s2sdk::ResultType Answer(int32_t caller, bool fromChat) {
	try {
		RunResetCommand(caller);
	} catch (const std::exception& ex) {
		LogError(std::string("resetscore: command failed: ") + ex.what());
		return s2sdk::ResultType::Handled; // never let an exception reach s2sdk
	} catch (...) {
		LogError("resetscore: command failed with an unknown exception");
		return s2sdk::ResultType::Handled;
	}
	return fromChat ? s2sdk::ResultType::Handled : s2sdk::ResultType::Continue;
}

// --- commands and the bare chat word (PITFALLS C11) -------------------------------------------

const resetscore::CommandSpec* FindCommand(std::string_view name) {
	for (const resetscore::CommandSpec& spec : resetscore::kCommands) {
		if (resetscore::detail::EqualNoCase(spec.name, name)) {
			return &spec;
		}
	}
	return nullptr;
}

// The callback of every command name the plugin registers.
s2sdk::ResultType CommandTrampoline(int32_t caller, s2sdk::ConCommandContext context,
									const plg::vector<plg::string>& args) {
	try {
		if (args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		const resetscore::CommandSpec* spec =
				FindCommand(std::string_view(args[0].data(), args[0].size()));
		if (spec == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		const bool fromChat = context == s2sdk::ConCommandContext::Chat;
		const resetscore::CommandSource source =
				fromChat ? resetscore::CommandSource::Chat : resetscore::CommandSource::Console;
		if (!resetscore::HasSource(spec->source, source)) {
			// "!mm_rs" in chat reaches this callback too (s2sdk does not check the ConVar flags on
			// the chat path): the name was not registered for chat, so leave the line alone.
			return s2sdk::ResultType::Continue;
		}
		return Answer(caller, fromChat);
	} catch (const std::exception& ex) {
		LogError(std::string("resetscore: command dispatch threw: ") + ex.what());
	} catch (...) {
		LogError("resetscore: command dispatch threw an unknown exception");
	}
	return s2sdk::ResultType::Handled;
}

// The say/say_team listener of the bare chat word "rs". s2sdk hands over (say, message): a CS2
// client sends the whole line as ONE quoted argument, a console `say rs top` sends one argument
// per word. Lines that start with "!" or "/" go through the chat-command path above and never
// reach this match.
s2sdk::ResultType ChatWordTrampoline(int32_t caller, s2sdk::ConCommandContext /*context*/,
									 const plg::vector<plg::string>& args) {
	try {
		if (args.size() < 2) {
			return s2sdk::ResultType::Continue;
		}
		std::vector<std::string> words;
		if (args.size() == 2) {
			words = resetscore::SplitChatWords(std::string_view(args[1].data(), args[1].size()));
		} else {
			words.reserve(args.size() - 1);
			for (std::size_t i = 1; i < args.size(); ++i) {
				words.emplace_back(args[i].data(), args[i].size());
			}
		}
		if (words.empty() || !resetscore::MatchesChatWord(words.front(), resetscore::kChatWord)) {
			return s2sdk::ResultType::Continue;
		}
		// Same answer the "!rs" spelling gets: the chat line is swallowed.
		return Answer(caller, /*fromChat=*/true);
	} catch (const std::exception& ex) {
		LogError(std::string("resetscore: chat word dispatch threw: ") + ex.what());
	} catch (...) {
		LogError("resetscore: chat word dispatch threw an unknown exception");
	}
	return s2sdk::ResultType::Handled;
}

struct RegisteredCommand {
	std::string name;
	bool owned = false; // true: AddConsoleCommand created it; false: only a listener on an existing name
};
std::vector<RegisteredCommand> g_registeredCommands;

struct SayListener {
	const char* name;
	bool attached = false;
};
SayListener g_sayListeners[] = {{"say", false}, {"say_team", false}};

void RegisterCommands() {
	if (__s2sdk_AddConsoleCommand == nullptr) {
		LogError("resetscore: s2sdk AddConsoleCommand is unavailable - the command is not registered");
		return;
	}
	// ClientCanExecute|Release: a player may type "rs" in their own console, like the original's
	// RegCommand registration (PITFALLS C11).
	const auto flags = static_cast<s2sdk::ConVarFlag>(static_cast<int64_t>(s2sdk::ConVarFlag::Release) |
													  static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute));
	const plg::string description("Resets your score, MVPs and match stats");
	for (const resetscore::CommandSpec& spec : resetscore::kCommands) {
		// false = the name already existed (another plugin, or our own entry kept by s2sdk across a
		// `plg reload` - s2sdk cannot destroy a console command); the callback is attached anyway,
		// and OnPluginEnd then removes only the listener.
		const bool owned = s2sdk::AddConsoleCommand(plg::string(spec.name), description, flags,
													&CommandTrampoline, s2sdk::HookMode::Pre);
		g_registeredCommands.push_back({spec.name, owned});
		if (!owned) {
			LogWarn("resetscore: the command name \"" + std::string(spec.name) +
					"\" already exists - hooked onto the existing entry");
		}
	}
}

void AttachChatWordListeners() {
	if (__s2sdk_AddCommandListener == nullptr) {
		LogError("resetscore: s2sdk AddCommandListener is unavailable - the bare chat word \"rs\" is disabled");
		return;
	}
	for (SayListener& say : g_sayListeners) {
		say.attached =
				s2sdk::AddCommandListener(plg::string(say.name), &ChatWordTrampoline, s2sdk::HookMode::Pre);
		if (!say.attached) {
			LogError(std::string("resetscore: could not listen on \"") + say.name + "\"");
		}
	}
}

void DetachCommands() {
	for (const RegisteredCommand& command : g_registeredCommands) {
		try {
			if (command.owned) {
				if (__s2sdk_RemoveCommand != nullptr) {
					s2sdk::RemoveCommand(plg::string(command.name), &CommandTrampoline);
				}
			} else if (__s2sdk_RemoveCommandListener != nullptr) {
				// The name was not ours: detaching the listener is all this plugin may undo
				// (RemoveCommand would take the other owner's callbacks with it).
				s2sdk::RemoveCommandListener(plg::string(command.name), &CommandTrampoline,
											 s2sdk::HookMode::Pre);
			}
		} catch (...) {
			LogError("resetscore: removing the command \"" + command.name + "\" failed");
		}
	}
	g_registeredCommands.clear();
}

void DetachChatWordListeners() {
	for (SayListener& say : g_sayListeners) {
		if (!say.attached) {
			continue;
		}
		say.attached = false;
		if (__s2sdk_RemoveCommandListener == nullptr) {
			continue;
		}
		try {
			s2sdk::RemoveCommandListener(plg::string(say.name), &ChatWordTrampoline, s2sdk::HookMode::Pre);
		} catch (...) {
			LogError(std::string("resetscore: removing the \"") + say.name + "\" listener failed");
		}
	}
}

class ResetScorePlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			// Only registers map listeners - no client, entity or schema call happens here
			// (PITFALLS B1: none of them exists before the first map).
			RegisterMapListeners();
			LoadPhrases();
			RegisterCommands();
			AttachChatWordListeners();
			LogInfo("resetscore: ready (mm_rs/sm_rs/rs in the console, !rs and the bare \"rs\" in chat)");
		} catch (const std::exception& ex) {
			LogError(std::string("resetscore: OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("resetscore: OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed (PITFALLS A1)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		try {
			// The map-ready gate. The host pumps this from the server frame, so the first update
			// proves a live map for a plugin loaded into a running map (it never saw an OnMapStart);
			// the host keeps pumping through a changelevel, so after an OnMapEnd only a new
			// OnMapStart re-opens the gate. Without the map listeners nothing could ever close the
			// gate again, so it is not opened at all then (fail safe, PITFALLS B2).
			if (!g_mapReady && g_mapListenersRegistered && (g_mapStartSeen || !g_mapEndSeen)) {
				g_mapReady = true;
			}
		} catch (...) {
			LogError("resetscore: OnPluginUpdate failed");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// Symmetrical teardown (PITFALLS A7): nothing may point into this library afterwards.
			g_mapReady = false; // no s2sdk call below depends on it, but be explicit
			DetachChatWordListeners();
			DetachCommands();
			UnregisterMapListeners();
			g_mapStartSeen = false;
			g_mapEndSeen = false;
			g_schemaOffsets.Clear();
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
