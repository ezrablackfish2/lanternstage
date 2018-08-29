#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lanternstage {

struct Color {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};
  std::uint8_t alpha{255};
};

struct Insets {
  std::int32_t left{0};
  std::int32_t top{0};
  std::int32_t right{0};
  std::int32_t bottom{0};
};

struct Size {
  std::int32_t width{0};
  std::int32_t height{0};
};

enum class FitMode : std::uint8_t { contain, cover, stretch, center, pixel_perfect };
enum class HorizontalAlign : std::uint8_t { left, center, right };
enum class VerticalAlign : std::uint8_t { top, middle, bottom };

struct Placement {
  Rect source;
  Rect destination;
  Rational scale_x;
  Rational scale_y;
  bool clipped{false};
};

std::optional<Color> parse_color(std::string_view text);
std::string format_color(Color color, bool include_alpha = true);
Color blend_over(Color foreground, Color background);
Color premultiply(Color color);
Color unpremultiply(Color color);
std::uint8_t luminance(Color color);
double contrast_ratio(Color left, Color right);
Color interpolate_color(Color left, Color right, double amount);
Color adjust_brightness(Color color, double factor);
Color adjust_saturation(Color color, double factor);

Rect inset_rect(Rect rect, Insets insets);
Rect intersect_rect(Rect left, Rect right);
Rect union_rect(Rect left, Rect right);
bool contains_point(Rect rect, std::int32_t x, std::int32_t y);
bool contains_rect(Rect outer, Rect inner);
std::int64_t rect_area(Rect rect);
Placement fit_content(Size source, Rect destination, FitMode mode,
                      HorizontalAlign horizontal = HorizontalAlign::center,
                      VerticalAlign vertical = VerticalAlign::middle);
std::vector<Rect> tile_rect(Rect bounds, Size tile_size, Insets gaps = {});
std::vector<Rect> distribute_columns(Rect bounds, std::size_t columns,
                                     std::int32_t gap,
                                     const std::vector<std::uint32_t>& weights = {});
std::vector<Rect> flow_rows(Rect bounds, const std::vector<Size>& items,
                            std::int32_t horizontal_gap,
                            std::int32_t vertical_gap,
                            HorizontalAlign align = HorizontalAlign::left);
Rect snap_rect(Rect rect, std::int32_t grid);

}  // namespace lanternstage
