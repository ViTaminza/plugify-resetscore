// Chat text helpers: the cs2-menus colour tags of the phrase files and the first-word matching of
// the bare chat word "rs".
//
// s2sdk's PrintToChat sends a message to the client as it is and its PrintToChatColored uses a
// different tag table, so the {TAG} words of the production resetscore.phrases.txt
// ("[{RED}RS{DEFAULT}]", "{PURPLE}...") are replaced here with exactly the control bytes the
// Metamod cs2-menus servers sent (pisexcompat carried the same table).
//
// Header-only and engine-free: resetscore_tests checks every tag byte-for-byte and the chat-word
// matcher against the spellings of PITFALLS C11 ("rs", "rs args", "!rs", "/rs").

#pragma once

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace resetscore {
namespace detail {

// cs2-menus' utils.h colors_text[]/colors_hex[], same order and bytes as pisexcompat. The last
// entry is the chat line break: the phrase files spell it "\n" (backslash + n) and a chat line
// wants U+2029, the engine's line separator.
struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

inline constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
														 {"{WHITE}", "\x01"},
														 {"{RED}", "\x02"},
														 {"{LIGHTPURPLE}", "\x03"},
														 {"{GREEN}", "\x04"},
														 {"{LIME}", "\x05"},
														 {"{LIGHTGREEN}", "\x06"},
														 {"{DARKRED}", "\x07"},
														 {"{GRAY}", "\x08"},
														 {"{LIGHTOLIVE}", "\x09"},
														 {"{OLIVE}", "\x10"},
														 {"{LIGHTBLUE}", "\x0B"},
														 {"{BLUE}", "\x0C"},
														 {"{PURPLE}", "\x0E"},
														 {"{LIGHTRED}", "\x0F"},
														 {"{GRAYBLUE}", "\x0A"},
														 {"\\n", "\xe2\x80\xa9"}}};

inline std::string ReplaceTags(std::string_view text, bool strip) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		const std::string_view replacement =
				strip ? (color.tag == "\\n" ? std::string_view("\n") : std::string_view()) : color.bytes;
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), replacement);
			pos += replacement.size();
		}
	}
	return out;
}

inline char ToLower(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

} // namespace detail

// The phrase text with the colour tags turned into their chat control bytes.
inline std::string Colorize(std::string_view text) {
	return detail::ReplaceTags(text, /*strip=*/false);
}

// The same tags removed, and the "\n" tag turned into a real newline (server console output).
inline std::string StripColorTags(std::string_view text) {
	return detail::ReplaceTags(text, /*strip=*/true);
}

// Whitespace-separated words of a chat line; a double-quoted run groups one word and the quotes
// are removed (the tokenizer Pisex's say hook used). Empty for an empty / blank line.
inline std::vector<std::string> SplitChatWords(std::string_view text) {
	std::vector<std::string> words;
	std::size_t i = 0;
	while (i < text.size()) {
		while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
			++i;
		}
		if (i >= text.size()) {
			break;
		}
		std::string word;
		if (text[i] == '"') {
			++i;
			while (i < text.size() && text[i] != '"') {
				word.push_back(text[i++]);
			}
			if (i < text.size()) {
				++i;
			}
		} else {
			while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
				word.push_back(text[i++]);
			}
		}
		words.push_back(std::move(word));
	}
	return words;
}

// True when `firstWord` is the bare chat word `word`: case-insensitive, and without a '!' or '/'
// trigger in front - "!rs" / "/rs" belong to s2sdk's chat-command path and must not run twice.
inline bool MatchesChatWord(std::string_view firstWord, std::string_view word) {
	if (firstWord.empty() || word.empty()) {
		return false;
	}
	if (firstWord.front() == '!' || firstWord.front() == '/') {
		return false;
	}
	if (firstWord.size() != word.size()) {
		return false;
	}
	for (std::size_t i = 0; i < word.size(); ++i) {
		if (detail::ToLower(firstWord[i]) != detail::ToLower(word[i])) {
			return false;
		}
	}
	return true;
}

} // namespace resetscore
