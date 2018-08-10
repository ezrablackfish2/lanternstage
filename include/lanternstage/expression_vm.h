#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lanternstage {
class WireReader;

class ExpressionVm {
 public:
  ExpressionVm();
  ~ExpressionVm();
  ExpressionVm(const ExpressionVm&) = delete;
  ExpressionVm& operator=(const ExpressionVm&) = delete;

  Status define_schema(WireReader& payload);
  Status compile_expression(WireReader& payload, Revision transaction_revision);
  Status migrate_schema(WireReader& payload, Epoch epoch);
  Status evaluate(WireReader& payload);

  Status pin_cache(WireReader& payload);
  Status evict_cache(WireReader& payload, Epoch epoch);
  Status evaluate_cache(WireReader& payload);

  void begin_transaction(Revision revision);
  void commit_transaction(Revision revision);
  void abort_transaction(Revision revision);
  void reset();

  std::size_t expression_count() const;
  std::uint64_t observation_hash() const;

 private:
  enum class FieldType : std::uint8_t { integer = 1, boolean = 2, text = 3 };
  enum class Op : std::uint8_t {
    push_const = 1,
    load_field = 2,
    add = 3,
    subtract = 4,
    multiply = 5,
    equal = 6,
    less = 7,
    logical_and = 8,
    logical_or = 9,
    logical_not = 10,
    select = 11,
    finish = 12,
  };

  struct Field {
    std::uint32_t id{0};
    FieldType type{FieldType::integer};
    std::uint32_t slot{0};
    std::string name;
  };

  struct Schema {
    std::uint32_t id{0};
    Revision revision{0};
    std::vector<Field> fields;
    std::unordered_map<std::uint32_t, std::size_t> by_id;
    std::size_t integer_slots{0};
    std::size_t boolean_slots{0};
    std::size_t text_slots{0};
  };

  struct Instruction {
    Op op{Op::finish};
    std::int32_t operand{0};
  };

  struct Program {
    std::uint32_t id{0};
    Revision revision{0};
    std::uint32_t schema_id{0};
    Schema* schema{nullptr};
    std::vector<Instruction> instructions;
    std::vector<std::int64_t> constants;
    std::vector<std::uint32_t> field_slots;
  };

  struct EvalFrame {
    std::vector<std::int64_t> integers;
    std::vector<std::uint8_t> booleans;
    std::vector<std::string> texts;
  };

  struct CacheEntry {
    std::uint32_t id{0};
    std::uint32_t program_id{0};
    Program* program{nullptr};
    Revision revision{0};
    std::vector<std::int64_t> captured_values;
    Epoch last_used{0};
  };

  Result<std::int64_t> run_program(Program& program, const EvalFrame& frame,
                                   std::uint32_t execution_limit);

  std::unordered_map<std::uint32_t, std::unique_ptr<Schema>> schemas_;
  std::unordered_map<std::uint32_t, std::unique_ptr<Program>> programs_;
  std::unordered_map<std::uint32_t, std::unique_ptr<CacheEntry>> caches_;
  std::vector<CacheEntry*> pinned_caches_;
  Revision active_revision_{0};
  std::uint64_t observation_hash_{0};
};

}  // namespace lanternstage
