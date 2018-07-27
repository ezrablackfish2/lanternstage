#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lanternstage {
class WireReader;

class Compositor {
 public:
  Compositor();
  ~Compositor();
  Compositor(const Compositor&) = delete;
  Compositor& operator=(const Compositor&) = delete;

  Status create_plane(WireReader& payload);
  Status update_palette(WireReader& payload);
  Status crop_plane(WireReader& payload, Revision transaction_revision);
  Status patch_plane(WireReader& payload, Revision transaction_revision);

  Status plan_tiles(WireReader& payload);
  Status resize_tiles(WireReader& payload, Epoch epoch);
  Status retain_tiles(WireReader& payload);
  Status render_tiles(WireReader& payload);

  void begin_transaction(Revision revision);
  void commit_transaction(Revision revision);
  void abort_transaction(Revision revision);
  void reset();

  std::size_t plane_count() const;
  std::uint64_t observation_hash() const;

 private:
  struct Plane {
    std::uint32_t id{0};
    Revision revision{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride{0};
    Rect crop;
    std::vector<std::uint32_t> palette;
    std::vector<std::uint8_t> pixels;
  };

  struct PatchPlan {
    std::uint32_t id{0};
    std::uint32_t plane_id{0};
    Plane* plane{nullptr};
    Revision revision{0};
    std::uint32_t captured_stride{0};
    Rect captured_crop;
    std::vector<std::uint8_t> patch;
  };

  struct TileBuffer {
    std::uint32_t id{0};
    Revision revision{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride{0};
    std::vector<std::uint8_t> bytes;
  };

  struct TilePlan {
    std::uint32_t id{0};
    std::uint32_t buffer_id{0};
    TileBuffer* buffer{nullptr};
    Revision revision{0};
    std::uint32_t tile_width{0};
    std::uint32_t tile_height{0};
    std::vector<std::uint32_t> tile_offsets;
  };

  std::unordered_map<std::uint32_t, std::unique_ptr<Plane>> planes_;
  std::unordered_map<std::uint32_t, PatchPlan> patch_plans_;
  std::unordered_map<std::uint32_t, std::unique_ptr<TileBuffer>> tile_buffers_;
  std::unordered_map<std::uint32_t, TilePlan> tile_plans_;
  std::vector<TilePlan*> retained_tile_plans_;
  Revision active_revision_{0};
  std::uint64_t observation_hash_{0};
};

}  // namespace lanternstage
