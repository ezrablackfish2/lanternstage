#pragma once

#include "lanternstage/command.h"
#include "lanternstage/engine.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lanternstage::fuzzing {

using MutateFunction = std::size_t (*)(std::uint8_t*, std::size_t, std::size_t);

std::size_t mutate_bundle(std::uint8_t* data,
                          std::size_t size,
                          std::size_t maximum_size,
                          unsigned seed,
                          ReplayMode mode,
                          MutateFunction mutate);

CommandBundle make_seed_bundle(ReplayMode mode, std::uint32_t stream_id = 0x4c535447u);
std::vector<Opcode> opcodes_for_mode(ReplayMode mode);
std::vector<std::uint8_t> default_payload(Opcode opcode, unsigned seed);
void normalize_sequences(CommandBundle& bundle);
void repair_transactions(CommandBundle& bundle, unsigned seed);

}  // namespace lanternstage::fuzzing
