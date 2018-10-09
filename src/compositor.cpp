#include "lanternstage/compositor.h"

#include "lanternstage/wire.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace lanternstage {
namespace {

bool checked_plane_size(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                        std::size_t& size) {
  if (width == 0 || height == 0 || stride < width || width > 16384 || height > 16384) return false;
  if (static_cast<std::uint64_t>(stride) * height > 128ull * 1024ull * 1024ull) return false;
  size = static_cast<std::size_t>(stride) * height;
  return true;
}

}  // namespace

Compositor::Compositor() = default;
Compositor::~Compositor() = default;

Status Compositor::create_plane(WireReader& payload) {
  auto plane = std::make_unique<Plane>();
  std::uint16_t palette_count = 0;
  std::uint32_t pixel_size = 0;
  if (!payload.read_u32(plane->id) || !payload.read_u32(plane->revision) ||
      !payload.read_u32(plane->width) || !payload.read_u32(plane->height) ||
      !payload.read_u32(plane->stride) || !payload.read_u16(palette_count) ||
      !payload.read_u32(pixel_size) || palette_count == 0 || palette_count > 256) {
    return Status::failure(Status::Code::malformed, "plane header is invalid");
  }
  std::size_t expected_size = 0;
  if (!checked_plane_size(plane->width, plane->height, plane->stride, expected_size) ||
      pixel_size != expected_size) {
    return Status::failure(Status::Code::rejected, "plane geometry is invalid");
  }
  plane->crop = {0, 0, static_cast<std::int32_t>(plane->width), static_cast<std::int32_t>(plane->height)};
  for (std::uint16_t i = 0; i < palette_count; ++i) {
    std::uint32_t color = 0;
    if (!payload.read_u32(color)) return Status::failure(Status::Code::malformed, "plane palette is truncated");
    plane->palette.push_back(color);
  }
  ByteSpan pixels;
  if (!payload.read_bytes(pixel_size, pixels) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "plane pixels are truncated");
  }
  plane->pixels.assign(pixels.data, pixels.data + pixels.size);
  planes_[plane->id] = std::move(plane);
  return Status::success();
}

Status Compositor::update_palette(WireReader& payload) {
  std::uint32_t plane_id = 0;
  Revision revision = 0;
  std::uint16_t first = 0;
  std::uint16_t count = 0;
  if (!payload.read_u32(plane_id) || !payload.read_u32(revision) ||
      !payload.read_u16(first) || !payload.read_u16(count) || count == 0 || count > 256) {
    return Status::failure(Status::Code::malformed, "palette update header is invalid");
  }
  auto plane = planes_.find(plane_id);
  if (plane == planes_.end() || revision < plane->second->revision ||
      static_cast<std::size_t>(first) + count > plane->second->palette.size()) {
    return Status::failure(Status::Code::conflict, "palette update range is invalid");
  }
  for (std::uint16_t i = 0; i < count; ++i) {
    std::uint32_t color = 0;
    if (!payload.read_u32(color)) return Status::failure(Status::Code::malformed, "palette colors are truncated");
    plane->second->palette[first + i] = color;
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing palette bytes");
  plane->second->revision = revision;
  return Status::success();
}

Status Compositor::crop_plane(WireReader& payload, Revision transaction_revision) {
  PatchPlan plan;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::uint16_t patch_size = 0;
  if (!payload.read_u32(plan.id) || !payload.read_u32(plan.plane_id) ||
      !payload.read_i32(x) || !payload.read_i32(y) ||
      !payload.read_i32(width) || !payload.read_i32(height) ||
      !payload.read_u16(patch_size) || transaction_revision == 0) {
    return Status::failure(Status::Code::malformed, "plane crop header is invalid");
  }
  auto plane = planes_.find(plan.plane_id);
  if (plane == planes_.end() || x < 0 || y < 0 || width <= 0 || height <= 0 ||
      static_cast<std::uint64_t>(x + width) > plane->second->width ||
      static_cast<std::uint64_t>(y + height) > plane->second->height) {
    return Status::failure(Status::Code::rejected, "plane crop is outside the image");
  }
  ByteSpan patch;
  if (!payload.read_bytes(patch_size, patch) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "plane patch body is truncated");
  }
  plan.plane = plane->second.get();
  plan.revision = transaction_revision;
  plan.captured_stride = plane->second->stride;
  plan.captured_crop = {x, y, width, height};
  plan.patch.assign(patch.data, patch.data + patch.size);
  plane->second->crop = plan.captured_crop;
  patch_plans_[plan.id] = std::move(plan);
  return Status::success();
}

Status Compositor::patch_plane(WireReader& payload, Revision transaction_revision) {
  std::uint32_t plan_id = 0;
  std::uint32_t run_count = 0;
  std::uint32_t selector = 0;
  if (!payload.read_u32(plan_id) || !payload.read_u32(run_count) ||
      !payload.read_u32(selector) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "plane patch commit payload is invalid");
  }
  auto plan = patch_plans_.find(plan_id);
  if (plan == patch_plans_.end() || plan->second.revision != transaction_revision ||
      plan->second.plane == nullptr || plan->second.patch.empty()) {
    return Status::failure(Status::Code::conflict, "plane patch plan changed");
  }
  Plane* plane = plan->second.plane;
  const Rect crop = plan->second.captured_crop;
  std::size_t patch_cursor = 0;
  for (std::uint32_t run = 0; run < run_count; ++run) {
    const std::uint32_t row = (run * 17u + selector) % static_cast<std::uint32_t>(crop.height);
    const std::uint32_t column = (run * 29u + selector / 3u) % static_cast<std::uint32_t>(crop.width);
    const std::size_t destination = static_cast<std::size_t>(crop.y + row) * plan->second.captured_stride +
                                    static_cast<std::size_t>(crop.x + column);
    const std::uint8_t count = plan->second.patch[patch_cursor++ % plan->second.patch.size()];
    for (std::uint8_t i = 0; i < count; ++i) {
      plane->pixels[destination + i] =
          plan->second.patch[(patch_cursor + i) % plan->second.patch.size()] ^
          static_cast<std::uint8_t>(plane->palette[(column + i) % plane->palette.size()]);
    }
  }
  observation_hash_ ^= mix64(plane->id ^ run_count ^ selector ^ plane->pixels.size());
  return Status::success();
}

Status Compositor::plan_tiles(WireReader& payload) {
  TilePlan plan;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  std::uint16_t tile_count = 0;
  if (!payload.read_u32(plan.id) || !payload.read_u32(plan.buffer_id) ||
      !payload.read_u32(plan.revision) || !payload.read_u32(width) ||
      !payload.read_u32(height) || !payload.read_u32(stride) ||
      !payload.read_u32(plan.tile_width) || !payload.read_u32(plan.tile_height) ||
      !payload.read_u16(tile_count) || tile_count == 0 || tile_count > 4096) {
    return Status::failure(Status::Code::malformed, "tile plan header is invalid");
  }
  std::size_t size = 0;
  if (!checked_plane_size(width, height, stride, size) || plan.tile_width == 0 || plan.tile_height == 0) {
    return Status::failure(Status::Code::rejected, "tile buffer geometry is invalid");
  }
  auto buffer = std::make_unique<TileBuffer>();
  buffer->id = plan.buffer_id;
  buffer->revision = plan.revision;
  buffer->width = width;
  buffer->height = height;
  buffer->stride = stride;
  buffer->bytes.assign(size, 0);
  for (std::uint16_t i = 0; i < tile_count; ++i) {
    std::uint32_t offset = 0;
    if (!payload.read_u32(offset) || offset >= size) {
      return Status::failure(Status::Code::rejected, "tile offset is outside buffer");
    }
    plan.tile_offsets.push_back(offset);
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing tile plan bytes");
  TileBuffer* raw = buffer.get();
  tile_buffers_[buffer->id] = std::move(buffer);
  plan.buffer = raw;
  tile_plans_[plan.id] = std::move(plan);
  return Status::success();
}

Status Compositor::resize_tiles(WireReader& payload, Epoch epoch) {
  std::uint32_t buffer_id = 0;
  Revision revision = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  std::uint32_t selector = 0;
  if (!payload.read_u32(buffer_id) || !payload.read_u32(revision) ||
      !payload.read_u32(width) || !payload.read_u32(height) ||
      !payload.read_u32(stride) || !payload.read_u32(selector) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "tile resize payload is invalid");
  }
  auto existing = tile_buffers_.find(buffer_id);
  std::size_t size = 0;
  if (existing == tile_buffers_.end() || revision <= existing->second->revision ||
      !checked_plane_size(width, height, stride, size)) {
    return Status::failure(Status::Code::conflict, "tile resize precondition failed");
  }
  auto replacement = std::make_unique<TileBuffer>();
  replacement->id = buffer_id;
  replacement->revision = revision;
  replacement->width = width;
  replacement->height = height;
  replacement->stride = stride;
  replacement->bytes.assign(size, static_cast<std::uint8_t>(selector ^ epoch));
  tile_buffers_[buffer_id] = std::move(replacement);
  return Status::success();
}

Status Compositor::retain_tiles(WireReader& payload) {
  std::uint32_t plan_id = 0;
  std::uint32_t expected_offsets = 0;
  Revision revision = 0;
  if (!payload.read_u32(plan_id) || !payload.read_u32(expected_offsets) ||
      !payload.read_u32(revision) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "tile retain payload is invalid");
  }
  auto plan = tile_plans_.find(plan_id);
  if (plan == tile_plans_.end() || plan->second.tile_offsets.size() != expected_offsets ||
      plan->second.revision != revision) {
    return Status::failure(Status::Code::conflict, "tile plan changed");
  }
  retained_tile_plans_.push_back(&plan->second);
  return Status::success();
}

Status Compositor::render_tiles(WireReader& payload) {
  std::uint32_t plan_id = 0;
  std::uint32_t expected_retained = 0;
  std::uint32_t pass_count = 0;
  std::uint32_t cookie = 0;
  if (!payload.read_u32(plan_id) || !payload.read_u32(expected_retained) ||
      !payload.read_u32(pass_count) || !payload.read_u32(cookie) || !payload.empty() ||
      retained_tile_plans_.size() != expected_retained) {
    return Status::failure(Status::Code::conflict, "retained tile set changed");
  }
  TilePlan* selected = nullptr;
  for (TilePlan* plan : retained_tile_plans_) {
    if (plan != nullptr && plan->id == plan_id) selected = plan;
  }
  if (selected == nullptr || selected->buffer == nullptr || selected->tile_offsets.empty()) {
    return Status::failure(Status::Code::not_found, "retained tile plan missing");
  }
  TileBuffer* buffer = selected->buffer;
  for (std::uint32_t pass = 0; pass < pass_count; ++pass) {
    const std::uint32_t tile = (pass * 31u + cookie) % selected->tile_offsets.size();
    const std::size_t origin = selected->tile_offsets[tile];
    for (std::uint32_t y = 0; y < selected->tile_height; ++y) {
      for (std::uint32_t x = 0; x < selected->tile_width; ++x) {
        const std::size_t offset = origin + static_cast<std::size_t>(y) * buffer->stride + x;
        buffer->bytes[offset] ^= static_cast<std::uint8_t>(cookie + pass + x + y);
      }
    }
  }
  observation_hash_ ^= mix64(buffer->id ^ buffer->bytes.size() ^ pass_count ^ cookie);
  return Status::success();
}

void Compositor::begin_transaction(Revision revision) { active_revision_ = revision; }
void Compositor::commit_transaction(Revision revision) {
  if (active_revision_ == revision) patch_plans_.clear();
  active_revision_ = 0;
}
void Compositor::abort_transaction(Revision revision) {
  if (active_revision_ == revision) {
    for (auto& [id, plan] : patch_plans_) {
      if (plan.revision == revision && plan.plane != nullptr) plan.plane->crop = {0, 0, static_cast<std::int32_t>(plan.plane->width), static_cast<std::int32_t>(plan.plane->height)};
    }
  }
  active_revision_ = 0;
}

void Compositor::reset() {
  planes_.clear();
  patch_plans_.clear();
  tile_buffers_.clear();
  tile_plans_.clear();
  retained_tile_plans_.clear();
  active_revision_ = 0;
  observation_hash_ = 0;
}

std::size_t Compositor::plane_count() const { return planes_.size(); }
std::uint64_t Compositor::observation_hash() const { return observation_hash_; }

}  // namespace lanternstage
