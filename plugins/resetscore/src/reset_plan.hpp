// reset_plan — the exact list of schema writes the resetscore port performs, kept together so the
// tests can guard it against the original's field list
// (ref/src/cs2-resetscore/resetscore.cpp, OnRsCommand).
//
// The original:
//   pController->m_iScore() = 0;
//   pController->m_iMVPs() = 0;
//   m_ATS->m_matchStats().m_iKills() = 0;   ... 13 more of CSPerRoundStats_t
//
// The two controller fields are networked (the scoreboard), so the port writes them through
// s2sdk's schema setter with changeState=true (see plugin.cpp); the 13 stat fields live in the
// CCSPlayerController_ActionTrackingServices::m_matchStats struct, which is reached through the
// m_pActionTrackingServices pointer of the controller and written directly, like the original.

#pragma once

#include <cstddef>
#include <cstdint>

namespace resetscore {

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; EntIndexToEntHandle
// answers -1 for "no entity"). The sign means nothing - once the serial counter passes 65535 bit
// 31 is set and the handle is a negative int; a `h > 0` test silently drops those entities.
inline bool IsValidHandle(int32_t h) { return h != -1 && h != 0; }

// Fields of CCSPlayerController (written through the networked schema setter).
struct ControllerField {
	const char* className;
	const char* fieldName;
};
inline constexpr ControllerField kControllerFields[] = {
		{"CCSPlayerController", "m_iScore"},
		{"CCSPlayerController", "m_iMVPs"},
};

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
inline constexpr std::size_t kMatchStatFieldCount = sizeof(kMatchStatFields) / sizeof(kMatchStatFields[0]);

} // namespace resetscore
