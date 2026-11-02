// The map-gated schema-offset cache (PITFALLS C8).
//
// s2sdk's GetSchemaOffset builds its field list from the network serializer map, which does not
// exist before the first map: calling it from OnPluginStart segfaults inside
// CollectSchemaFields (real crash in game_fixes). Both lookups are therefore only performed while
// the plugin's map gate is open, and only a *successful* result is remembered:
//
//   * GetSchemaOffset answers -1 for an unknown field; caching that -1 would hide the field for
//     the whole session (a game build where the field appears later, or a lookup that ran a frame
//     too early), so only offsets >= 0 are cached;
//   * GetSchemaChainOffset answers <= 0 for "no chainer" / unknown class, and s2sdk's
//     SafeNetworkStateChanged reads a chain offset >= 0 as "the object's __m_pChainEntity is at
//     object + offset" - 0 would make the engine treat the vtable as a chainer. The plugin's
//     SchemaChainOffset therefore answers -1 for every "none", and only offsets > 0 are cached.
//
// The lookups are injected as function pointers so resetscore_tests can drive the whole policy
// (gate first, failures retried, successes memoised) without an engine.
//
// Header-only: it is included by plugin.cpp and by the tests.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace resetscore {

class SchemaOffsetCache {
public:
	// Both return -1 for "unknown"; the cache decides what is worth remembering.
	using OffsetLookup = int32_t (*)(const char* className, const char* fieldName);
	using ChainLookup = int32_t (*)(const char* className);

	// >= 0 offset of className::fieldName, or -1 when no map is up / the field is unknown /
	// the caller passed an empty name. A cached hit never calls `lookup`.
	int32_t Offset(bool mapReady, const char* className, const char* fieldName, OffsetLookup lookup) {
		if (className == nullptr || fieldName == nullptr || *className == '\0' || *fieldName == '\0' ||
			lookup == nullptr) {
			return -1;
		}
		std::string key(className);
		key.push_back('\0');
		key.append(fieldName);
		if (const auto cached = m_offsets.find(key); cached != m_offsets.end()) {
			return cached->second;
		}
		if (!mapReady) {
			return -1; // GetSchemaOffset would dereference the null serializer map
		}
		const int32_t offset = lookup(className, fieldName);
		if (offset >= 0) {
			m_offsets.emplace(std::move(key), offset);
		}
		return offset;
	}

	// > 0 offset of className's __m_pChainEntity, or -1 for every "none" (no map, unknown class,
	// offset <= 0). Only a positive offset is cached.
	int32_t ChainOffset(bool mapReady, const char* className, ChainLookup lookup) {
		if (className == nullptr || *className == '\0' || lookup == nullptr) {
			return -1;
		}
		const std::string key(className);
		if (const auto cached = m_chainOffsets.find(key); cached != m_chainOffsets.end()) {
			return cached->second;
		}
		if (!mapReady) {
			return -1;
		}
		const int32_t offset = lookup(className);
		if (offset <= 0) {
			return -1;
		}
		m_chainOffsets.emplace(key, offset);
		return offset;
	}

	// Forgets everything; the plugin calls this in OnPluginEnd (a reload re-resolves the fields).
	void Clear() {
		m_offsets.clear();
		m_chainOffsets.clear();
	}

	// Bookkeeping for the tests.
	std::size_t CachedOffsetCount() const {
		return m_offsets.size();
	}
	std::size_t CachedChainOffsetCount() const {
		return m_chainOffsets.size();
	}

private:
	std::unordered_map<std::string, int32_t> m_offsets;      // class + '\0' + field -> offset >= 0
	std::unordered_map<std::string, int32_t> m_chainOffsets; // class -> offset > 0
};

} // namespace resetscore
