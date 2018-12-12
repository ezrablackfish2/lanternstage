#include "lanternstage/text.h"

#include "lanternstage/wire.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <sstream>

namespace lanternstage {
namespace {

bool is_ascii_alpha(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

bool is_ascii_alnum(char c) { return is_ascii_alpha(c) || is_ascii_digit(c); }

bool all_alpha(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), is_ascii_alpha);
}

bool all_digit(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), is_ascii_digit);
}

bool all_alnum(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), is_ascii_alnum);
}

void set_error(Utf8Error* error, std::size_t offset, std::string message) {
  if (error != nullptr) {
    error->offset = offset;
    error->message = std::move(message);
  }
}

std::size_t utf8_length(std::uint8_t lead) {
  if ((lead & 0x80u) == 0) return 1;
  if ((lead & 0xe0u) == 0xc0u) return 2;
  if ((lead & 0xf0u) == 0xe0u) return 3;
  if ((lead & 0xf8u) == 0xf0u) return 4;
  return 0;
}

bool decode_one(std::string_view text, std::size_t offset, std::uint32_t& cp,
                std::size_t& length, Utf8Error* error) {
  const auto lead = static_cast<std::uint8_t>(text[offset]);
  length = utf8_length(lead);
  if (length == 0) {
    set_error(error, offset, "invalid UTF-8 leading byte");
    return false;
  }
  if (offset + length > text.size()) {
    set_error(error, offset, "truncated UTF-8 sequence");
    return false;
  }
  if (length == 1) {
    cp = lead;
    return true;
  }
  cp = lead & ((1u << (7u - static_cast<unsigned>(length))) - 1u);
  for (std::size_t i = 1; i < length; ++i) {
    const auto byte = static_cast<std::uint8_t>(text[offset + i]);
    if ((byte & 0xc0u) != 0x80u) {
      set_error(error, offset + i, "invalid UTF-8 continuation byte");
      return false;
    }
    cp = (cp << 6u) | (byte & 0x3fu);
  }
  const std::array<std::uint32_t, 5> minimum{{0, 0, 0x80, 0x800, 0x10000}};
  if (cp < minimum[length]) {
    set_error(error, offset, "overlong UTF-8 sequence");
    return false;
  }
  if (cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
    set_error(error, offset, "invalid Unicode scalar value");
    return false;
  }
  return true;
}

std::string title_ascii(std::string_view text) {
  std::string out = ascii_lower(text);
  if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
  return out;
}

bool is_wide(std::uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2329 && cp <= 0x232a) ||
         (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
         (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
         (cp >= 0xfe10 && cp <= 0xfe19) || (cp >= 0xfe30 && cp <= 0xfe6f) ||
         (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
         (cp >= 0x1f300 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd);
}

bool is_combining(std::uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
         (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x20d0 && cp <= 0x20ff) ||
         (cp >= 0xfe20 && cp <= 0xfe2f) || cp == 0x200d ||
         (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef);
}

}  // namespace

bool LocaleTag::empty() const { return language.empty(); }

std::string LocaleTag::canonical() const {
  if (language.empty()) return {};
  std::vector<std::string> parts;
  parts.push_back(ascii_lower(language));
  if (!script.empty()) parts.push_back(title_ascii(script));
  if (!region.empty()) parts.push_back(region.size() == 2 ? ascii_upper(region) : region);
  for (const std::string& variant : variants) parts.push_back(ascii_lower(variant));
  for (const std::string& extension : extensions) parts.push_back(ascii_lower(extension));
  if (!private_use.empty()) {
    parts.push_back("x");
    std::vector<std::string> private_parts = split_ascii(private_use, '-', false);
    parts.insert(parts.end(), private_parts.begin(), private_parts.end());
  }
  return join_strings(parts, "-");
}

bool validate_utf8(std::string_view text, Utf8Error* error) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t cp = 0;
    std::size_t length = 0;
    if (!decode_one(text, offset, cp, length, error)) return false;
    offset += length;
  }
  return true;
}

std::vector<std::uint32_t> decode_utf8(std::string_view text, Utf8Error* error) {
  std::vector<std::uint32_t> result;
  std::size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t cp = 0;
    std::size_t length = 0;
    if (!decode_one(text, offset, cp, length, error)) {
      result.clear();
      return result;
    }
    result.push_back(cp);
    offset += length;
  }
  return result;
}

std::string encode_utf8(const std::vector<std::uint32_t>& codepoints) {
  std::string out;
  for (std::uint32_t cp : codepoints) {
    if (cp <= 0x7f) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
      out.push_back(static_cast<char>(0xc0u | (cp >> 6u)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else if (cp <= 0xffff && !(cp >= 0xd800 && cp <= 0xdfff)) {
      out.push_back(static_cast<char>(0xe0u | (cp >> 12u)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else if (cp <= 0x10ffff) {
      out.push_back(static_cast<char>(0xf0u | (cp >> 18u)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    }
  }
  return out;
}

std::string replace_invalid_utf8(std::string_view text, std::uint32_t replacement) {
  std::vector<std::uint32_t> codepoints;
  std::size_t offset = 0;
  while (offset < text.size()) {
    std::uint32_t cp = 0;
    std::size_t length = 0;
    if (decode_one(text, offset, cp, length, nullptr)) {
      codepoints.push_back(cp);
      offset += length;
    } else {
      codepoints.push_back(replacement);
      ++offset;
    }
  }
  return encode_utf8(codepoints);
}

std::string ascii_lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) if (c >= 'A' && c <= 'Z') c += 32;
  return out;
}

std::string ascii_upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) if (c >= 'a' && c <= 'z') c -= 32;
  return out;
}

std::string trim_ascii(std::string_view text) {
  std::size_t first = 0;
  while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
  std::size_t last = text.size();
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
  return std::string(text.substr(first, last - first));
}

std::string collapse_ascii_whitespace(std::string_view text) {
  std::string out;
  bool pending_space = false;
  for (char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      pending_space = !out.empty();
    } else {
      if (pending_space) out.push_back(' ');
      out.push_back(c);
      pending_space = false;
    }
  }
  return out;
}

std::vector<std::string> split_ascii(std::string_view text, char delimiter, bool keep_empty) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == delimiter) {
      if (keep_empty || i != start) parts.emplace_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  return parts;
}

std::string join_strings(const std::vector<std::string>& parts, std::string_view separator) {
  std::size_t size = 0;
  for (const std::string& part : parts) size += part.size();
  if (!parts.empty()) size += separator.size() * (parts.size() - 1);
  std::string out;
  out.reserve(size);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out.append(separator);
    out.append(parts[i]);
  }
  return out;
}

std::string json_escape(std::string_view text) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() + 8);
  for (unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(hex[c >> 4u]);
          out.push_back(hex[c & 0x0fu]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

std::string shell_escape(std::string_view text) {
  if (text.empty()) return "''";
  bool safe = std::all_of(text.begin(), text.end(), [](char c) {
    return is_ascii_alnum(c) || c == '_' || c == '-' || c == '.' || c == '/' || c == ':';
  });
  if (safe) return std::string(text);
  std::string out("'");
  for (char c : text) {
    if (c == '\'') out += "'\\''";
    else out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

std::string percent_encode(std::string_view text) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : text) {
    if (is_ascii_alnum(static_cast<char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4u]);
      out.push_back(hex[c & 0x0fu]);
    }
  }
  return out;
}

std::optional<std::string> percent_decode(std::string_view text) {
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out.push_back(text[i]);
      continue;
    }
    if (i + 2 >= text.size()) return std::nullopt;
    const int high = nibble(text[i + 1]);
    const int low = nibble(text[i + 2]);
    if (high < 0 || low < 0) return std::nullopt;
    out.push_back(static_cast<char>((high << 4) | low));
    i += 2;
  }
  if (!validate_utf8(out)) return std::nullopt;
  return out;
}

Result<LocaleTag> parse_locale_tag(std::string_view text) {
  Result<LocaleTag> result;
  std::vector<std::string> parts = split_ascii(trim_ascii(text), '-', false);
  if (parts.empty() || parts[0].size() < 2 || parts[0].size() > 8 || !all_alpha(parts[0])) {
    result.status = Status::failure(Status::Code::malformed, "locale language subtag is invalid");
    return result;
  }
  result.value.language = ascii_lower(parts[0]);
  std::size_t index = 1;
  if (index < parts.size() && parts[index].size() == 4 && all_alpha(parts[index])) {
    result.value.script = title_ascii(parts[index++]);
  }
  if (index < parts.size() && ((parts[index].size() == 2 && all_alpha(parts[index])) ||
                               (parts[index].size() == 3 && all_digit(parts[index])))) {
    result.value.region = parts[index].size() == 2 ? ascii_upper(parts[index]) : parts[index];
    ++index;
  }
  while (index < parts.size()) {
    const std::string& part = parts[index];
    if (ascii_lower(part) == "x") {
      ++index;
      if (index == parts.size()) {
        result.status = Status::failure(Status::Code::malformed, "private-use locale tag is empty");
        return result;
      }
      std::vector<std::string> tail(parts.begin() + static_cast<std::ptrdiff_t>(index), parts.end());
      for (const std::string& item : tail) {
        if (item.empty() || item.size() > 8 || !all_alnum(item)) {
          result.status = Status::failure(Status::Code::malformed, "private-use locale subtag is invalid");
          return result;
        }
      }
      result.value.private_use = join_strings(tail, "-");
      index = parts.size();
      break;
    }
    if (part.size() == 1 && all_alnum(part)) {
      std::string extension = ascii_lower(part);
      ++index;
      std::size_t components = 0;
      while (index < parts.size() && parts[index].size() >= 2 && parts[index].size() <= 8 &&
             all_alnum(parts[index])) {
        extension += "-" + ascii_lower(parts[index++]);
        ++components;
      }
      if (components == 0) {
        result.status = Status::failure(Status::Code::malformed, "locale extension has no value");
        return result;
      }
      result.value.extensions.push_back(std::move(extension));
      continue;
    }
    const bool variant_shape = (part.size() >= 5 && part.size() <= 8 && all_alnum(part)) ||
                               (part.size() == 4 && is_ascii_digit(part[0]) && all_alnum(part));
    if (!variant_shape) {
      result.status = Status::failure(Status::Code::malformed, "locale variant is invalid");
      return result;
    }
    result.value.variants.push_back(ascii_lower(part));
    ++index;
  }
  result.status = Status::success();
  return result;
}

bool locale_matches(const LocaleTag& requested, const LocaleTag& available) {
  if (ascii_lower(requested.language) != ascii_lower(available.language)) return false;
  if (!requested.script.empty() && !available.script.empty() &&
      ascii_lower(requested.script) != ascii_lower(available.script)) return false;
  if (!requested.region.empty() && !available.region.empty() &&
      ascii_upper(requested.region) != ascii_upper(available.region)) return false;
  for (const std::string& variant : available.variants) {
    if (std::find(requested.variants.begin(), requested.variants.end(), variant) == requested.variants.end())
      return false;
  }
  return true;
}

std::vector<std::string> locale_fallback_chain(const LocaleTag& tag) {
  std::vector<std::string> chain;
  LocaleTag current = tag;
  if (!current.private_use.empty()) current.private_use.clear();
  current.extensions.clear();
  chain.push_back(current.canonical());
  while (!current.variants.empty()) {
    current.variants.pop_back();
    chain.push_back(current.canonical());
  }
  if (!current.region.empty()) {
    current.region.clear();
    chain.push_back(current.canonical());
  }
  if (!current.script.empty()) {
    current.script.clear();
    chain.push_back(current.canonical());
  }
  std::sort(chain.begin(), chain.end());
  chain.erase(std::unique(chain.begin(), chain.end()), chain.end());
  std::sort(chain.begin(), chain.end(), [](const std::string& a, const std::string& b) {
    return std::count(a.begin(), a.end(), '-') > std::count(b.begin(), b.end(), '-');
  });
  return chain;
}

std::uint64_t stable_text_hash(std::string_view text) {
  const ByteSpan bytes{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
  return mix64(fnv1a64(bytes) ^ text.size());
}

std::vector<std::string> tokenize_words(std::string_view text) {
  std::vector<std::string> words;
  std::string current;
  for (std::uint32_t cp : decode_utf8(replace_invalid_utf8(text))) {
    const bool ascii_word = cp < 128 && (std::isalnum(static_cast<unsigned char>(cp)) || cp == '_');
    const bool unicode_word = cp >= 128 && !is_combining(cp) && cp != 0x3000;
    if (ascii_word || unicode_word) {
      std::vector<std::uint32_t> one{cp};
      current += encode_utf8(one);
    } else if (!current.empty()) {
      words.push_back(ascii_lower(current));
      current.clear();
    }
  }
  if (!current.empty()) words.push_back(ascii_lower(current));
  return words;
}

std::size_t display_width(std::string_view text) {
  std::size_t width = 0;
  for (std::uint32_t cp : decode_utf8(replace_invalid_utf8(text))) {
    if (cp == '\n' || cp == '\r' || cp == '\t' || cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) continue;
    if (is_combining(cp)) continue;
    width += is_wide(cp) ? 2 : 1;
  }
  return width;
}

std::string truncate_display(std::string_view text, std::size_t maximum_columns,
                             std::string_view ellipsis) {
  if (display_width(text) <= maximum_columns) return std::string(text);
  const std::size_t ellipsis_width = display_width(ellipsis);
  if (ellipsis_width >= maximum_columns) return std::string(ellipsis);
  std::vector<std::uint32_t> result;
  std::size_t width = 0;
  for (std::uint32_t cp : decode_utf8(replace_invalid_utf8(text))) {
    const std::size_t step = is_combining(cp) ? 0 : (is_wide(cp) ? 2 : 1);
    if (width + step + ellipsis_width > maximum_columns) break;
    result.push_back(cp);
    width += step;
  }
  return encode_utf8(result) + std::string(ellipsis);
}

}  // namespace lanternstage
