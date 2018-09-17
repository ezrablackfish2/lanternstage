#pragma once

#include "lanternstage/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lanternstage {

struct Utf8Error {
  std::size_t offset{0};
  std::string message;
};

struct LocaleTag {
  std::string language;
  std::string script;
  std::string region;
  std::vector<std::string> variants;
  std::vector<std::string> extensions;
  std::string private_use;

  std::string canonical() const;
  bool empty() const;
};

bool validate_utf8(std::string_view text, Utf8Error* error = nullptr);
std::vector<std::uint32_t> decode_utf8(std::string_view text, Utf8Error* error = nullptr);
std::string encode_utf8(const std::vector<std::uint32_t>& codepoints);
std::string replace_invalid_utf8(std::string_view text, std::uint32_t replacement = 0xfffd);
std::string ascii_lower(std::string_view text);
std::string ascii_upper(std::string_view text);
std::string trim_ascii(std::string_view text);
std::string collapse_ascii_whitespace(std::string_view text);
std::vector<std::string> split_ascii(std::string_view text, char delimiter, bool keep_empty = false);
std::string join_strings(const std::vector<std::string>& parts, std::string_view separator);
std::string json_escape(std::string_view text);
std::string shell_escape(std::string_view text);
std::string percent_encode(std::string_view text);
std::optional<std::string> percent_decode(std::string_view text);
Result<LocaleTag> parse_locale_tag(std::string_view text);
bool locale_matches(const LocaleTag& requested, const LocaleTag& available);
std::vector<std::string> locale_fallback_chain(const LocaleTag& tag);
std::uint64_t stable_text_hash(std::string_view text);
std::vector<std::string> tokenize_words(std::string_view text);
std::size_t display_width(std::string_view text);
std::string truncate_display(std::string_view text, std::size_t maximum_columns,
                             std::string_view ellipsis = "…");

}  // namespace lanternstage
