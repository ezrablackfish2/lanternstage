#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lanternstage {
class WireReader;

class LocalizationCatalog {
 public:
  LocalizationCatalog();
  ~LocalizationCatalog();
  LocalizationCatalog(const LocalizationCatalog&) = delete;
  LocalizationCatalog& operator=(const LocalizationCatalog&) = delete;

  Status add_locale(WireReader& payload);
  Status add_fallback(WireReader& payload);
  Status compile_fallbacks(WireReader& payload, Revision transaction_revision);
  Status compact_locales(WireReader& payload, Epoch epoch);
  Status lookup(WireReader& payload);

  Status create_snapshot(WireReader& payload);
  Status mutate_catalog(WireReader& payload, Epoch epoch);
  Status restore_snapshot(WireReader& payload, Revision transaction_revision);
  Status lookup_snapshot(WireReader& payload);

  void begin_transaction(Revision revision);
  void commit_transaction(Revision revision);
  void abort_transaction(Revision revision);
  void reset();

  std::size_t locale_count() const;
  std::uint64_t observation_hash() const;

 private:
  struct Entry {
    std::uint32_t key{0};
    Revision revision{0};
    std::string value;
  };

  struct Locale {
    std::uint32_t id{0};
    Revision revision{0};
    std::string tag;
    std::unordered_map<std::uint32_t, Entry> entries;
    std::vector<std::uint32_t> fallback_ids;
    bool retired{false};
  };

  struct CompiledChain {
    std::uint32_t locale_id{0};
    Revision revision{0};
    std::vector<Locale*> locales;
    std::vector<std::uint64_t> bloom;
  };

  struct SnapshotEntry {
    std::uint32_t key{0};
    Entry* cached{nullptr};
    Revision revision{0};
  };

  struct CatalogSnapshot {
    std::uint32_t id{0};
    Revision revision{0};
    std::uint32_t locale_id{0};
    std::vector<SnapshotEntry> entries;
    std::uint64_t digest{0};
  };

  std::unordered_map<std::uint32_t, std::unique_ptr<Locale>> locales_;
  std::unordered_map<std::uint32_t, CompiledChain> chains_;
  std::unordered_map<std::uint32_t, CatalogSnapshot> snapshots_;
  Revision active_revision_{0};
  std::uint64_t observation_hash_{0};
};

}  // namespace lanternstage
