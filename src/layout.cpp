#include "lanternstage/layout.h"

#include "lanternstage/text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>

namespace lanternstage {
namespace {

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::optional<std::uint8_t> hex_byte(char high, char low) {
  const int a = hex_nibble(high);
  const int b = hex_nibble(low);
  if (a < 0 || b < 0) return std::nullopt;
  return static_cast<std::uint8_t>((a << 4) | b);
}

std::uint8_t clamp_channel(double value) {
  return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0l, 255l));
}

double linear_channel(std::uint8_t value) {
  const double normalized = value / 255.0;
  return normalized <= 0.04045 ? normalized / 12.92
                               : std::pow((normalized + 0.055) / 1.055, 2.4);
}

std::int32_t aligned_offset(std::int32_t available, std::int32_t used,
                            HorizontalAlign align) {
  switch (align) {
    case HorizontalAlign::left: return 0;
    case HorizontalAlign::center: return (available - used) / 2;
    case HorizontalAlign::right: return available - used;
  }
  return 0;
}

std::int32_t aligned_offset(std::int32_t available, std::int32_t used,
                            VerticalAlign align) {
  switch (align) {
    case VerticalAlign::top: return 0;
    case VerticalAlign::middle: return (available - used) / 2;
    case VerticalAlign::bottom: return available - used;
  }
  return 0;
}

std::int32_t floor_div(std::int32_t value, std::int32_t divisor) {
  const std::int32_t quotient = value / divisor;
  const std::int32_t remainder = value % divisor;
  return quotient - ((remainder != 0 && ((remainder < 0) != (divisor < 0))) ? 1 : 0);
}

}  // namespace

std::optional<Color> parse_color(std::string_view text) {
  const std::string value = ascii_lower(trim_ascii(text));
  static const std::array<std::pair<std::string_view, Color>, 10> named{{
      {"black", {0, 0, 0, 255}}, {"white", {255, 255, 255, 255}},
      {"red", {255, 0, 0, 255}}, {"green", {0, 128, 0, 255}},
      {"blue", {0, 0, 255, 255}}, {"yellow", {255, 255, 0, 255}},
      {"cyan", {0, 255, 255, 255}}, {"magenta", {255, 0, 255, 255}},
      {"transparent", {0, 0, 0, 0}}, {"gray", {128, 128, 128, 255}},
  }};
  for (const auto& [name, color] : named) if (value == name) return color;
  if (value.empty() || value[0] != '#') return std::nullopt;
  if (value.size() == 4 || value.size() == 5) {
    const int r = hex_nibble(value[1]);
    const int g = hex_nibble(value[2]);
    const int b = hex_nibble(value[3]);
    const int a = value.size() == 5 ? hex_nibble(value[4]) : 15;
    if (r < 0 || g < 0 || b < 0 || a < 0) return std::nullopt;
    return Color{static_cast<std::uint8_t>(r * 17), static_cast<std::uint8_t>(g * 17),
                 static_cast<std::uint8_t>(b * 17), static_cast<std::uint8_t>(a * 17)};
  }
  if (value.size() == 7 || value.size() == 9) {
    auto r = hex_byte(value[1], value[2]);
    auto g = hex_byte(value[3], value[4]);
    auto b = hex_byte(value[5], value[6]);
    auto a = value.size() == 9 ? hex_byte(value[7], value[8]) : std::optional<std::uint8_t>(255);
    if (!r || !g || !b || !a) return std::nullopt;
    return Color{*r, *g, *b, *a};
  }
  return std::nullopt;
}

std::string format_color(Color color, bool include_alpha) {
  char buffer[16];
  if (include_alpha) {
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", color.red, color.green,
                  color.blue, color.alpha);
  } else {
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", color.red, color.green, color.blue);
  }
  return buffer;
}

Color blend_over(Color foreground, Color background) {
  const std::uint32_t fa = foreground.alpha;
  const std::uint32_t ba = background.alpha;
  const std::uint32_t out_a = fa + (ba * (255u - fa) + 127u) / 255u;
  if (out_a == 0) return {};
  auto channel = [&](std::uint8_t f, std::uint8_t b) {
    const std::uint32_t numerator = static_cast<std::uint32_t>(f) * fa * 255u +
                                    static_cast<std::uint32_t>(b) * ba * (255u - fa);
    return static_cast<std::uint8_t>((numerator + out_a * 127u) / (out_a * 255u));
  };
  return {channel(foreground.red, background.red),
          channel(foreground.green, background.green),
          channel(foreground.blue, background.blue),
          static_cast<std::uint8_t>(out_a)};
}

Color premultiply(Color color) {
  color.red = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color.red) * color.alpha + 127u) / 255u);
  color.green = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color.green) * color.alpha + 127u) / 255u);
  color.blue = static_cast<std::uint8_t>((static_cast<std::uint32_t>(color.blue) * color.alpha + 127u) / 255u);
  return color;
}

Color unpremultiply(Color color) {
  if (color.alpha == 0) return {};
  color.red = static_cast<std::uint8_t>(std::min(255u, (static_cast<std::uint32_t>(color.red) * 255u + color.alpha / 2u) / color.alpha));
  color.green = static_cast<std::uint8_t>(std::min(255u, (static_cast<std::uint32_t>(color.green) * 255u + color.alpha / 2u) / color.alpha));
  color.blue = static_cast<std::uint8_t>(std::min(255u, (static_cast<std::uint32_t>(color.blue) * 255u + color.alpha / 2u) / color.alpha));
  return color;
}

std::uint8_t luminance(Color color) {
  return clamp_channel(0.2126 * color.red + 0.7152 * color.green + 0.0722 * color.blue);
}

double contrast_ratio(Color left, Color right) {
  const double l1 = 0.2126 * linear_channel(left.red) + 0.7152 * linear_channel(left.green) +
                    0.0722 * linear_channel(left.blue);
  const double l2 = 0.2126 * linear_channel(right.red) + 0.7152 * linear_channel(right.green) +
                    0.0722 * linear_channel(right.blue);
  return (std::max(l1, l2) + 0.05) / (std::min(l1, l2) + 0.05);
}

Color interpolate_color(Color left, Color right, double amount) {
  amount = std::clamp(amount, 0.0, 1.0);
  auto mix = [amount](std::uint8_t a, std::uint8_t b) {
    return clamp_channel(a + (b - a) * amount);
  };
  return {mix(left.red, right.red), mix(left.green, right.green),
          mix(left.blue, right.blue), mix(left.alpha, right.alpha)};
}

Color adjust_brightness(Color color, double factor) {
  color.red = clamp_channel(color.red * factor);
  color.green = clamp_channel(color.green * factor);
  color.blue = clamp_channel(color.blue * factor);
  return color;
}

Color adjust_saturation(Color color, double factor) {
  const double gray = 0.2126 * color.red + 0.7152 * color.green + 0.0722 * color.blue;
  color.red = clamp_channel(gray + (color.red - gray) * factor);
  color.green = clamp_channel(gray + (color.green - gray) * factor);
  color.blue = clamp_channel(gray + (color.blue - gray) * factor);
  return color;
}

Rect inset_rect(Rect rect, Insets insets) {
  rect.x += insets.left;
  rect.y += insets.top;
  rect.width -= insets.left + insets.right;
  rect.height -= insets.top + insets.bottom;
  if (rect.width < 0) rect.width = 0;
  if (rect.height < 0) rect.height = 0;
  return rect;
}

Rect intersect_rect(Rect left, Rect right) {
  const std::int64_t x1 = std::max<std::int64_t>(left.x, right.x);
  const std::int64_t y1 = std::max<std::int64_t>(left.y, right.y);
  const std::int64_t x2 = std::min<std::int64_t>(static_cast<std::int64_t>(left.x) + left.width,
                                                static_cast<std::int64_t>(right.x) + right.width);
  const std::int64_t y2 = std::min<std::int64_t>(static_cast<std::int64_t>(left.y) + left.height,
                                                static_cast<std::int64_t>(right.y) + right.height);
  if (x2 <= x1 || y2 <= y1) return {static_cast<std::int32_t>(x1), static_cast<std::int32_t>(y1), 0, 0};
  return {static_cast<std::int32_t>(x1), static_cast<std::int32_t>(y1),
          static_cast<std::int32_t>(x2 - x1), static_cast<std::int32_t>(y2 - y1)};
}

Rect union_rect(Rect left, Rect right) {
  if (left.empty()) return right;
  if (right.empty()) return left;
  const std::int64_t x1 = std::min<std::int64_t>(left.x, right.x);
  const std::int64_t y1 = std::min<std::int64_t>(left.y, right.y);
  const std::int64_t x2 = std::max<std::int64_t>(static_cast<std::int64_t>(left.x) + left.width,
                                                static_cast<std::int64_t>(right.x) + right.width);
  const std::int64_t y2 = std::max<std::int64_t>(static_cast<std::int64_t>(left.y) + left.height,
                                                static_cast<std::int64_t>(right.y) + right.height);
  return {static_cast<std::int32_t>(x1), static_cast<std::int32_t>(y1),
          static_cast<std::int32_t>(x2 - x1), static_cast<std::int32_t>(y2 - y1)};
}

bool contains_point(Rect rect, std::int32_t x, std::int32_t y) {
  return !rect.empty() && x >= rect.x && y >= rect.y &&
         static_cast<std::int64_t>(x) < static_cast<std::int64_t>(rect.x) + rect.width &&
         static_cast<std::int64_t>(y) < static_cast<std::int64_t>(rect.y) + rect.height;
}

bool contains_rect(Rect outer, Rect inner) {
  if (inner.empty()) return true;
  return !outer.empty() && inner.x >= outer.x && inner.y >= outer.y &&
         static_cast<std::int64_t>(inner.x) + inner.width <= static_cast<std::int64_t>(outer.x) + outer.width &&
         static_cast<std::int64_t>(inner.y) + inner.height <= static_cast<std::int64_t>(outer.y) + outer.height;
}

std::int64_t rect_area(Rect rect) {
  return rect.empty() ? 0 : static_cast<std::int64_t>(rect.width) * rect.height;
}

Placement fit_content(Size source, Rect destination, FitMode mode,
                      HorizontalAlign horizontal, VerticalAlign vertical) {
  Placement result;
  result.source = {0, 0, source.width, source.height};
  result.destination = destination;
  if (source.width <= 0 || source.height <= 0 || destination.empty()) return result;
  if (mode == FitMode::stretch) {
    result.scale_x = {destination.width, source.width};
    result.scale_y = {destination.height, source.height};
    return result;
  }
  if (mode == FitMode::center || mode == FitMode::pixel_perfect) {
    const std::int32_t width = mode == FitMode::pixel_perfect
                                   ? std::min(source.width, destination.width)
                                   : source.width;
    const std::int32_t height = mode == FitMode::pixel_perfect
                                    ? std::min(source.height, destination.height)
                                    : source.height;
    result.destination = {destination.x + aligned_offset(destination.width, width, horizontal),
                          destination.y + aligned_offset(destination.height, height, vertical),
                          width, height};
    result.source = {aligned_offset(source.width, width, horizontal),
                     aligned_offset(source.height, height, vertical), width, height};
    result.scale_x = {1, 1};
    result.scale_y = {1, 1};
    result.clipped = width != source.width || height != source.height;
    return result;
  }
  const double scale_x = static_cast<double>(destination.width) / source.width;
  const double scale_y = static_cast<double>(destination.height) / source.height;
  const double scale = mode == FitMode::contain ? std::min(scale_x, scale_y) : std::max(scale_x, scale_y);
  const std::int32_t width = std::max(1, static_cast<int>(std::lround(source.width * scale)));
  const std::int32_t height = std::max(1, static_cast<int>(std::lround(source.height * scale)));
  result.destination = {destination.x + aligned_offset(destination.width, width, horizontal),
                        destination.y + aligned_offset(destination.height, height, vertical),
                        width, height};
  result.scale_x = {width, source.width};
  result.scale_y = {height, source.height};
  result.clipped = mode == FitMode::cover && (width > destination.width || height > destination.height);
  return result;
}

std::vector<Rect> tile_rect(Rect bounds, Size tile_size, Insets gaps) {
  std::vector<Rect> result;
  if (bounds.empty() || tile_size.width <= 0 || tile_size.height <= 0) return result;
  const std::int32_t step_x = tile_size.width + gaps.left + gaps.right;
  const std::int32_t step_y = tile_size.height + gaps.top + gaps.bottom;
  if (step_x <= 0 || step_y <= 0) return result;
  for (std::int32_t y = bounds.y + gaps.top; y < bounds.y + bounds.height; y += step_y) {
    for (std::int32_t x = bounds.x + gaps.left; x < bounds.x + bounds.width; x += step_x) {
      Rect tile{x, y, tile_size.width, tile_size.height};
      Rect clipped = intersect_rect(tile, bounds);
      if (!clipped.empty()) result.push_back(clipped);
    }
  }
  return result;
}

std::vector<Rect> distribute_columns(Rect bounds, std::size_t columns,
                                     std::int32_t gap,
                                     const std::vector<std::uint32_t>& weights) {
  std::vector<Rect> result;
  if (bounds.empty() || columns == 0 || gap < 0 ||
      static_cast<std::int64_t>(gap) * static_cast<std::int64_t>(columns - 1) >= static_cast<std::int64_t>(bounds.width)) return result;
  std::vector<std::uint32_t> normalized(columns, 1);
  if (!weights.empty()) {
    for (std::size_t i = 0; i < columns && i < weights.size(); ++i) normalized[i] = std::max(1u, weights[i]);
  }
  const std::uint64_t total_weight = std::accumulate(normalized.begin(), normalized.end(), std::uint64_t{0});
  const std::int32_t available = bounds.width - gap * static_cast<std::int32_t>(columns - 1);
  std::int32_t cursor = bounds.x;
  std::int32_t consumed = 0;
  for (std::size_t i = 0; i < columns; ++i) {
    const std::int32_t width = i + 1 == columns
                                   ? available - consumed
                                   : static_cast<std::int32_t>((static_cast<std::int64_t>(available) * normalized[i]) / total_weight);
    result.push_back({cursor, bounds.y, width, bounds.height});
    cursor += width + gap;
    consumed += width;
  }
  return result;
}

std::vector<Rect> flow_rows(Rect bounds, const std::vector<Size>& items,
                            std::int32_t horizontal_gap,
                            std::int32_t vertical_gap,
                            HorizontalAlign align) {
  std::vector<Rect> result(items.size());
  struct Row { std::size_t begin; std::size_t end; std::int32_t width; std::int32_t height; };
  std::vector<Row> rows;
  std::size_t begin = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const std::int32_t candidate = width == 0 ? items[i].width : width + horizontal_gap + items[i].width;
    if (width != 0 && candidate > bounds.width) {
      rows.push_back({begin, i, width, height});
      begin = i;
      width = items[i].width;
      height = items[i].height;
    } else {
      width = candidate;
      height = std::max(height, items[i].height);
    }
  }
  if (begin < items.size()) rows.push_back({begin, items.size(), width, height});
  std::int32_t y = bounds.y;
  for (const Row& row : rows) {
    std::int32_t x = bounds.x + aligned_offset(bounds.width, row.width, align);
    for (std::size_t i = row.begin; i < row.end; ++i) {
      result[i] = {x, y, items[i].width, items[i].height};
      x += items[i].width + horizontal_gap;
    }
    y += row.height + vertical_gap;
    if (y > bounds.y + bounds.height) break;
  }
  return result;
}

Rect snap_rect(Rect rect, std::int32_t grid) {
  if (grid <= 1) return rect;
  const std::int32_t x1 = floor_div(rect.x, grid) * grid;
  const std::int32_t y1 = floor_div(rect.y, grid) * grid;
  const std::int32_t x2 = static_cast<std::int32_t>(std::ceil(static_cast<double>(rect.x + rect.width) / grid)) * grid;
  const std::int32_t y2 = static_cast<std::int32_t>(std::ceil(static_cast<double>(rect.y + rect.height) / grid)) * grid;
  return {x1, y1, x2 - x1, y2 - y1};
}

}  // namespace lanternstage
