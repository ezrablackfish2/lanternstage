#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lanternstage {
class WireReader;

class JournalStore {
 public:
  JournalStore();
  ~JournalStore();
  JournalStore(const JournalStore&) = delete;
  JournalStore& operator=(const JournalStore&) = delete;

  Status append(WireReader& payload, Tick now);
  Status create_cursor(WireReader& payload);
  Status compact(WireReader& payload, Epoch epoch);
  Status read_cursor(WireReader& payload);

  Status fork_snapshot(WireReader& payload);
  Status intern_snapshot_value(WireReader& payload);
  Status merge_snapshot(WireReader& payload, Revision transaction_revision);
  Status read_snapshot(WireReader& payload);

  Status begin_token_scope(WireReader& payload, Revision transaction_revision);
  Status checkpoint_tokens(WireReader& payload);
  Status rollback_tokens(WireReader& payload, Revision transaction_revision);
  Status release_tokens(WireReader& payload, Revision transaction_revision);

  void begin_transaction(Revision revision);
  void commit_transaction(Revision revision);
  void abort_transaction(Revision revision);
  void reset();

  std::size_t segment_count() const;
  std::uint64_t observation_hash() const;

 private:
  struct Record {
    std::uint64_t sequence{0};
    Tick timestamp{0};
    std::uint32_t type{0};
    Revision revision{0};
    std::vector<std::uint8_t> payload;
  };

  struct Segment {
    std::uint32_t id{0};
    Revision revision{0};
    std::uint64_t first_sequence{0};
    std::vector<Record> records;
    bool sealed{false};
  };

  struct Cursor {
    std::uint32_t id{0};
    std::uint32_t segment_id{0};
    Segment* segment{nullptr};
    std::size_t record_index{0};
    Revision revision{0};
  };

  struct InternedValue {
    std::uint32_t id{0};
    Revision revision{0};
    std::string value;
    std::uint32_t references{0};
  };

  struct Snapshot {
    std::uint32_t id{0};
    Revision revision{0};
    std::uint32_t parent_id{0};
    std::vector<InternedValue*> values;
    std::vector<std::uint32_t> value_ids;
    bool merged{false};
  };

  struct Token {
    std::uint32_t id{0};
    std::uint32_t owner{0};
    Revision revision{0};
    std::string text;
    bool released{false};
  };

  struct TokenScope {
    std::uint32_t id{0};
    Revision revision{0};
    std::vector<Token*> tokens;
    std::size_t checkpoint{0};
    bool rolled_back{false};
  };

  std::unordered_map<std::uint32_t, std::unique_ptr<Segment>> segments_;
  std::unordered_map<std::uint32_t, Cursor> cursors_;
  std::unordered_map<std::uint32_t, std::unique_ptr<InternedValue>> interned_values_;
  std::unordered_map<std::uint32_t, Snapshot> snapshots_;
  std::unordered_map<std::uint32_t, std::unique_ptr<Token>> tokens_;
  std::unordered_map<std::uint32_t, TokenScope> token_scopes_;
  std::vector<Token*> token_release_queue_;

  std::uint32_t active_segment_{0};
  std::uint64_t next_sequence_{1};
  Revision active_revision_{0};
  std::uint64_t observation_hash_{0};
};

}  // namespace lanternstage
