#include "lanternstage/expression_vm.h"

#include "lanternstage/wire.h"

#include <algorithm>
#include <limits>
#include <stack>

namespace lanternstage {

ExpressionVm::ExpressionVm() = default;
ExpressionVm::~ExpressionVm() = default;

Status ExpressionVm::define_schema(WireReader& payload) {
  std::uint32_t id = 0;
  Revision revision = 0;
  std::uint16_t field_count = 0;
  if (!payload.read_u32(id) || !payload.read_u32(revision) ||
      !payload.read_u16(field_count) || field_count == 0 || field_count > 256) {
    return Status::failure(Status::Code::malformed, "expression schema header is invalid");
  }
  auto schema = std::make_unique<Schema>();
  schema->id = id;
  schema->revision = revision;
  for (std::uint16_t index = 0; index < field_count; ++index) {
    Field field;
    std::uint8_t type = 0;
    std::uint8_t name_size = 0;
    if (!payload.read_u32(field.id) || !payload.read_u8(type) ||
        !payload.read_u8(name_size) || name_size == 0 || name_size > 64) {
      return Status::failure(Status::Code::malformed, "expression field header is invalid");
    }
    if (!payload.read_string(name_size, field.name)) {
      return Status::failure(Status::Code::malformed, "expression field name is truncated");
    }
    if (type == static_cast<std::uint8_t>(FieldType::integer)) {
      field.type = FieldType::integer;
      field.slot = static_cast<std::uint32_t>(schema->integer_slots++);
    } else if (type == static_cast<std::uint8_t>(FieldType::boolean)) {
      field.type = FieldType::boolean;
      field.slot = static_cast<std::uint32_t>(schema->boolean_slots++);
    } else if (type == static_cast<std::uint8_t>(FieldType::text)) {
      field.type = FieldType::text;
      field.slot = static_cast<std::uint32_t>(schema->text_slots++);
    } else {
      return Status::failure(Status::Code::unsupported, "unknown expression field type");
    }
    if (!schema->by_id.emplace(field.id, schema->fields.size()).second) {
      return Status::failure(Status::Code::conflict, "duplicate expression field id");
    }
    schema->fields.push_back(std::move(field));
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing schema bytes");
  auto existing = schemas_.find(id);
  if (existing != schemas_.end() && existing->second->revision >= revision) {
    return Status::failure(Status::Code::conflict, "expression schema revision is stale");
  }
  schemas_[id] = std::move(schema);
  return Status::success();
}

Status ExpressionVm::compile_expression(WireReader& payload, Revision transaction_revision) {
  std::uint32_t program_id = 0;
  std::uint32_t schema_id = 0;
  Revision revision = 0;
  std::uint16_t constant_count = 0;
  std::uint16_t instruction_count = 0;
  if (!payload.read_u32(program_id) || !payload.read_u32(schema_id) ||
      !payload.read_u32(revision) || !payload.read_u16(constant_count) ||
      !payload.read_u16(instruction_count) || constant_count > 256 ||
      instruction_count == 0 || instruction_count > 2048 || transaction_revision == 0) {
    return Status::failure(Status::Code::malformed, "expression program header is invalid");
  }
  auto schema = schemas_.find(schema_id);
  if (schema == schemas_.end()) return Status::failure(Status::Code::not_found, "expression schema missing");
  auto program = std::make_unique<Program>();
  program->id = program_id;
  program->revision = revision;
  program->schema_id = schema_id;
  program->schema = schema->second.get();
  for (std::uint16_t i = 0; i < constant_count; ++i) {
    std::uint64_t raw = 0;
    if (!payload.read_u64(raw)) return Status::failure(Status::Code::malformed, "expression constants are truncated");
    program->constants.push_back(static_cast<std::int64_t>(raw));
  }
  int stack_depth = 0;
  bool finished = false;
  for (std::uint16_t i = 0; i < instruction_count; ++i) {
    std::uint8_t raw_op = 0;
    std::int32_t operand = 0;
    if (!payload.read_u8(raw_op) || !payload.read_i32(operand)) {
      return Status::failure(Status::Code::malformed, "expression instructions are truncated");
    }
    const Op op = static_cast<Op>(raw_op);
    switch (op) {
      case Op::push_const:
        if (operand < 0 || static_cast<std::size_t>(operand) >= program->constants.size())
          return Status::failure(Status::Code::rejected, "constant index outside program");
        ++stack_depth;
        break;
      case Op::load_field: {
        auto field = schema->second->by_id.find(static_cast<std::uint32_t>(operand));
        if (field == schema->second->by_id.end())
          return Status::failure(Status::Code::not_found, "program field missing from schema");
        const Field& definition = schema->second->fields[field->second];
        if (definition.type != FieldType::integer && definition.type != FieldType::boolean)
          return Status::failure(Status::Code::unsupported, "text fields are not scalar expression operands");
        program->field_slots.push_back(definition.slot);
        ++stack_depth;
        break;
      }
      case Op::add:
      case Op::subtract:
      case Op::multiply:
      case Op::equal:
      case Op::less:
      case Op::logical_and:
      case Op::logical_or:
        if (stack_depth < 2) return Status::failure(Status::Code::rejected, "expression stack underflow");
        --stack_depth;
        break;
      case Op::logical_not:
        if (stack_depth < 1) return Status::failure(Status::Code::rejected, "expression stack underflow");
        break;
      case Op::select:
        if (stack_depth < 3) return Status::failure(Status::Code::rejected, "expression stack underflow");
        stack_depth -= 2;
        break;
      case Op::finish:
        if (stack_depth != 1 || i + 1 != instruction_count)
          return Status::failure(Status::Code::rejected, "finish must terminate a single-valued expression");
        finished = true;
        break;
      default:
        return Status::failure(Status::Code::unsupported, "unknown expression opcode");
    }
    program->instructions.push_back({op, operand});
  }
  if (!finished || !payload.empty()) return Status::failure(Status::Code::malformed, "expression program is incomplete");
  programs_[program_id] = std::move(program);
  return Status::success();
}

Status ExpressionVm::migrate_schema(WireReader& payload, Epoch epoch) {
  std::uint32_t schema_id = 0;
  Revision revision = 0;
  std::uint16_t retained_count = 0;
  std::uint16_t new_count = 0;
  std::uint32_t selector = 0;
  if (!payload.read_u32(schema_id) || !payload.read_u32(revision) ||
      !payload.read_u16(retained_count) || !payload.read_u16(new_count) ||
      !payload.read_u32(selector) || retained_count > 256 || new_count > 256) {
    return Status::failure(Status::Code::malformed, "schema migration header is invalid");
  }
  auto old = schemas_.find(schema_id);
  if (old == schemas_.end() || revision <= old->second->revision) {
    return Status::failure(Status::Code::conflict, "schema migration revision is stale");
  }
  auto replacement = std::make_unique<Schema>();
  replacement->id = schema_id;
  replacement->revision = revision;
  for (std::uint16_t i = 0; i < retained_count; ++i) {
    std::uint32_t field_id = 0;
    if (!payload.read_u32(field_id)) return Status::failure(Status::Code::malformed, "retained field ids are truncated");
    auto existing = old->second->by_id.find(field_id);
    if (existing == old->second->by_id.end()) return Status::failure(Status::Code::not_found, "retained field missing");
    Field field = old->second->fields[existing->second];
    if (field.type == FieldType::integer) field.slot = static_cast<std::uint32_t>(replacement->integer_slots++);
    else if (field.type == FieldType::boolean) field.slot = static_cast<std::uint32_t>(replacement->boolean_slots++);
    else field.slot = static_cast<std::uint32_t>(replacement->text_slots++);
    replacement->by_id[field.id] = replacement->fields.size();
    replacement->fields.push_back(std::move(field));
  }
  for (std::uint16_t i = 0; i < new_count; ++i) {
    Field field;
    std::uint8_t type = 0;
    std::uint8_t name_size = 0;
    if (!payload.read_u32(field.id) || !payload.read_u8(type) || !payload.read_u8(name_size) ||
        !payload.read_string(name_size, field.name)) {
      return Status::failure(Status::Code::malformed, "new schema field is truncated");
    }
    field.type = static_cast<FieldType>(type);
    if (field.type == FieldType::integer) field.slot = static_cast<std::uint32_t>(replacement->integer_slots++);
    else if (field.type == FieldType::boolean) field.slot = static_cast<std::uint32_t>(replacement->boolean_slots++);
    else if (field.type == FieldType::text) field.slot = static_cast<std::uint32_t>(replacement->text_slots++);
    else return Status::failure(Status::Code::unsupported, "new field type unsupported");
    replacement->by_id[field.id] = replacement->fields.size();
    replacement->fields.push_back(std::move(field));
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing schema migration bytes");
  const bool replace_storage = ((selector ^ schema_id ^ epoch) & 1u) == 0u;
  schemas_[schema_id] = std::move(replacement);
  if (!replace_storage) observation_hash_ ^= mix64(selector ^ revision);
  return Status::success();
}

Result<std::int64_t> ExpressionVm::run_program(Program& program, const EvalFrame& frame,
                                               std::uint32_t execution_limit) {
  Result<std::int64_t> result;
  std::vector<std::int64_t> stack;
  stack.reserve(64);
  std::uint32_t steps = 0;
  for (const Instruction& instruction : program.instructions) {
    if (++steps > execution_limit) {
      result.status = Status::failure(Status::Code::exhausted, "expression execution limit reached");
      return result;
    }
    auto pop = [&stack]() {
      const std::int64_t value = stack.back();
      stack.pop_back();
      return value;
    };
    switch (instruction.op) {
      case Op::push_const:
        stack.push_back(program.constants[static_cast<std::size_t>(instruction.operand)]);
        break;
      case Op::load_field: {
        auto field = program.schema->by_id.find(static_cast<std::uint32_t>(instruction.operand));
        const Field& definition = program.schema->fields[field->second];
        if (definition.type == FieldType::integer)
          stack.push_back(frame.integers[definition.slot]);
        else
          stack.push_back(frame.booleans[definition.slot] != 0);
        break;
      }
      case Op::add: { auto b = pop(); auto a = pop(); stack.push_back(a + b); break; }
      case Op::subtract: { auto b = pop(); auto a = pop(); stack.push_back(a - b); break; }
      case Op::multiply: { auto b = pop(); auto a = pop(); stack.push_back(a * b); break; }
      case Op::equal: { auto b = pop(); auto a = pop(); stack.push_back(a == b); break; }
      case Op::less: { auto b = pop(); auto a = pop(); stack.push_back(a < b); break; }
      case Op::logical_and: { auto b = pop(); auto a = pop(); stack.push_back(a && b); break; }
      case Op::logical_or: { auto b = pop(); auto a = pop(); stack.push_back(a || b); break; }
      case Op::logical_not: { auto a = pop(); stack.push_back(!a); break; }
      case Op::select: {
        auto when_false = pop();
        auto when_true = pop();
        auto condition = pop();
        stack.push_back(condition ? when_true : when_false);
        break;
      }
      case Op::finish:
        result.value = stack.back();
        result.status = Status::success();
        return result;
    }
  }
  result.status = Status::failure(Status::Code::internal_error, "expression terminated without finish");
  return result;
}

Status ExpressionVm::evaluate(WireReader& payload) {
  std::uint32_t program_id = 0;
  Revision expected_schema_revision = 0;
  std::uint16_t integer_count = 0;
  std::uint16_t boolean_count = 0;
  std::uint32_t limit = 0;
  if (!payload.read_u32(program_id) || !payload.read_u32(expected_schema_revision) ||
      !payload.read_u16(integer_count) || !payload.read_u16(boolean_count) ||
      !payload.read_u32(limit) || integer_count > 1024 || boolean_count > 1024) {
    return Status::failure(Status::Code::malformed, "expression evaluation header is invalid");
  }
  EvalFrame frame;
  frame.integers.reserve(integer_count);
  for (std::uint16_t i = 0; i < integer_count; ++i) {
    std::uint64_t raw = 0;
    if (!payload.read_u64(raw)) return Status::failure(Status::Code::malformed, "integer frame is truncated");
    frame.integers.push_back(static_cast<std::int64_t>(raw));
  }
  for (std::uint16_t i = 0; i < boolean_count; ++i) {
    std::uint8_t value = 0;
    if (!payload.read_u8(value)) return Status::failure(Status::Code::malformed, "boolean frame is truncated");
    frame.booleans.push_back(value != 0);
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing evaluation bytes");
  auto program = programs_.find(program_id);
  if (program == programs_.end() || program->second->schema == nullptr ||
      program->second->schema->revision != expected_schema_revision) {
    return Status::failure(Status::Code::conflict, "program schema revision changed");
  }
  Result<std::int64_t> value = run_program(*program->second, frame, limit);
  if (!value) return value.status;
  observation_hash_ ^= mix64(static_cast<std::uint64_t>(value.value) ^ program_id ^ limit);
  return Status::success();
}

Status ExpressionVm::pin_cache(WireReader& payload) {
  std::uint32_t cache_id = 0;
  std::uint32_t program_id = 0;
  Revision revision = 0;
  std::uint16_t captured_count = 0;
  if (!payload.read_u32(cache_id) || !payload.read_u32(program_id) ||
      !payload.read_u32(revision) || !payload.read_u16(captured_count) || captured_count > 512) {
    return Status::failure(Status::Code::malformed, "expression cache header is invalid");
  }
  auto program = programs_.find(program_id);
  if (program == programs_.end()) return Status::failure(Status::Code::not_found, "cache program missing");
  auto cache = std::make_unique<CacheEntry>();
  cache->id = cache_id;
  cache->program_id = program_id;
  cache->program = program->second.get();
  cache->revision = revision;
  for (std::uint16_t i = 0; i < captured_count; ++i) {
    std::uint64_t raw = 0;
    if (!payload.read_u64(raw)) return Status::failure(Status::Code::malformed, "captured values are truncated");
    cache->captured_values.push_back(static_cast<std::int64_t>(raw));
  }
  if (!payload.empty()) return Status::failure(Status::Code::malformed, "trailing cache bytes");
  CacheEntry* raw = cache.get();
  caches_[cache_id] = std::move(cache);
  pinned_caches_.push_back(raw);
  return Status::success();
}

Status ExpressionVm::evict_cache(WireReader& payload, Epoch epoch) {
  Revision before_revision = 0;
  std::uint32_t selector = 0;
  std::uint32_t keep_id = 0;
  if (!payload.read_u32(before_revision) || !payload.read_u32(selector) ||
      !payload.read_u32(keep_id) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "cache eviction payload is invalid");
  }
  for (auto it = caches_.begin(); it != caches_.end();) {
    CacheEntry& cache = *it->second;
    if (cache.id != keep_id && cache.revision <= before_revision &&
        ((mix32(cache.id ^ selector) + epoch) & 3u) == 2u) {
      it = caches_.erase(it);
    } else {
      ++it;
    }
  }
  return Status::success();
}

Status ExpressionVm::evaluate_cache(WireReader& payload) {
  std::uint32_t cache_id = 0;
  std::uint32_t expected_pins = 0;
  std::uint32_t execution_limit = 0;
  std::uint32_t cookie = 0;
  if (!payload.read_u32(cache_id) || !payload.read_u32(expected_pins) ||
      !payload.read_u32(execution_limit) || !payload.read_u32(cookie) || !payload.empty() ||
      pinned_caches_.size() != expected_pins) {
    return Status::failure(Status::Code::conflict, "expression pin set changed");
  }
  CacheEntry* selected = nullptr;
  for (CacheEntry* cache : pinned_caches_) {
    if (cache != nullptr && cache->id == cache_id) {
      selected = cache;
      break;
    }
  }
  if (selected == nullptr || selected->program == nullptr) {
    return Status::failure(Status::Code::not_found, "pinned cache missing");
  }
  EvalFrame frame;
  frame.integers = selected->captured_values;
  if (frame.integers.empty()) frame.integers.push_back(cookie);
  frame.booleans.assign(16, static_cast<std::uint8_t>(cookie & 1u));
  Result<std::int64_t> value = run_program(*selected->program, frame, execution_limit);
  if (!value) return value.status;
  selected->last_used += (cookie & 7u) + 1u;
  observation_hash_ ^= mix64(value.value ^ selected->id ^ selected->last_used);
  return Status::success();
}

void ExpressionVm::begin_transaction(Revision revision) { active_revision_ = revision; }
void ExpressionVm::commit_transaction(Revision revision) {
  if (active_revision_ == revision) {
    pinned_caches_.erase(std::remove(pinned_caches_.begin(), pinned_caches_.end(), nullptr),
                         pinned_caches_.end());
  }
  active_revision_ = 0;
}
void ExpressionVm::abort_transaction(Revision revision) {
  if (active_revision_ == revision) {
    for (auto it = programs_.begin(); it != programs_.end();) {
      if (it->second->revision == revision) it = programs_.erase(it);
      else ++it;
    }
  }
  active_revision_ = 0;
}

void ExpressionVm::reset() {
  schemas_.clear();
  programs_.clear();
  caches_.clear();
  pinned_caches_.clear();
  active_revision_ = 0;
  observation_hash_ = 0;
}

std::size_t ExpressionVm::expression_count() const { return programs_.size(); }
std::uint64_t ExpressionVm::observation_hash() const { return observation_hash_; }

}  // namespace lanternstage
