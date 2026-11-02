// reset_plan — everything this plugin does, written down once, without a single engine call, so
// resetscore_tests can guard it against the original plugin (ref/src/cs2-resetscore/resetscore.cpp).
//
// The original (Pisex's "ResetScore", RS::OnRsCommand) zeroed the caller's two controller counters
// and then all 13 fields of the CSPerRoundStats_t block held by
// CCSPlayerController_ActionTrackingServices::m_matchStats, and registered its handler with
//
//     RegCommand({"mm_rs", "sm_rs", "rs"}, {"!rs", "rs"}, OnRsCommand);
//
// i.e. three console spellings plus two chat spellings: "!rs" (a chat trigger, which s2sdk routes
// to the console command "rs" as ConCommandContext::Chat) and the bare chat word "rs" (a plain
// say/say_team line, which only a command listener sees).
//
// The two controller counters are networked (the scoreboard), so they are written through s2sdk's
// typed schema setter with changeState=true; the 13 stat fields live in the m_matchStats struct,
// reached through the controller's m_pActionTrackingServices pointer and written raw at
// <ActionTrackingServices> + off(m_matchStats) + off(field) - the C6/C8-safe path, see plugin.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace resetscore {

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; EntIndexToEntHandle
// answers -1 for "no entity"). The sign means nothing - once the serial counter passes 65535 bit
// 31 is set and the handle is a negative int; a `h > 0` test silently drops those entities.
constexpr bool IsValidHandle(int32_t handle) {
	return handle != -1 && handle != 0;
}

// Fields of CCSPlayerController (written through the networked schema setter).
struct ControllerField {
	const char* className;
	const char* fieldName;
};
inline constexpr ControllerField kControllerFields[] = {
		{"CCSPlayerController", "m_iScore"},
		{"CCSPlayerController", "m_iMVPs"},
};
inline constexpr std::size_t kControllerFieldCount =
		sizeof(kControllerFields) / sizeof(kControllerFields[0]);

// The pointer that leads to the stats struct.
inline constexpr const char* kActionTrackingPointerClass = "CCSPlayerController";
inline constexpr const char* kActionTrackingPointerField = "m_pActionTrackingServices";

// The struct that holds the stat fields and the class whose fields they are declared in.
inline constexpr const char* kMatchStatsClass = "CCSPlayerController_ActionTrackingServices";
inline constexpr const char* kMatchStatsField = "m_matchStats";
inline constexpr const char* kMatchStatsFieldClass = "CSPerRoundStats_t";

// The 13 stat fields the original zeroed, in its order (all int32 in the schema).
inline constexpr const char* kMatchStatFields[] = {
		"m_iKills",
		"m_iDeaths",
		"m_iAssists",
		"m_iDamage",
		"m_iEquipmentValue",
		"m_iMoneySaved",
		"m_iKillReward",
		"m_iLiveTime",
		"m_iHeadShotKills",
		"m_iObjective",
		"m_iCashEarned",
		"m_iUtilityDamage",
		"m_iEnemiesFlashed",
};
inline constexpr std::size_t kMatchStatFieldCount =
		sizeof(kMatchStatFields) / sizeof(kMatchStatFields[0]);

// Which source a command name answers. s2sdk calls a console command's callback for the chat
// spellings "!name" / "/name" too, with ConCommandContext::Chat (PITFALLS C11), so a name that was
// registered for the console only has to recognise the chat context itself and leave the line
// alone. The sources mirror the original's RegCommand lists: "mm_rs"/"sm_rs" are console
// spellings, "rs" is both a console command and a chat command ("!rs"), and the bare "rs" word is
// additionally caught by the say/say_team listeners (kChatWord).
enum class CommandSource : uint8_t {
	Console = 1,
	Chat = 2,
	Both = Console | Chat,
};

constexpr bool HasSource(CommandSource command, CommandSource wanted) {
	return (static_cast<uint8_t>(command) & static_cast<uint8_t>(wanted)) != 0;
}

struct CommandSpec {
	const char* name;
	CommandSource source;
};
inline constexpr CommandSpec kCommands[] = {
		{"mm_rs", CommandSource::Console},
		{"sm_rs", CommandSource::Console},
		{"rs", CommandSource::Both},
};
inline constexpr std::size_t kCommandCount = sizeof(kCommands) / sizeof(kCommands[0]);

// The bare chat word of the original's chat list ("rs", without a trigger): matched against the
// first word of a say/say_team line. Lines that start with "!" or "/" never match - those belong
// to s2sdk's own chat-command path and would otherwise run the reset twice.
inline constexpr std::string_view kChatWord = "rs";

// The production phrases file, under <plg::GetConfigsDir()>/ (the shipped copy lives in
// plugins/resetscore/configs/, package.sh installs it into <package>/configs/).
inline constexpr std::string_view kPhrasesRelativePath = "resetscore/translations/resetscore.phrases.txt";

} // namespace resetscore
