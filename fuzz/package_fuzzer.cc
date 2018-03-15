#include "fuzz_support.h"

#include "lanternstage/engine.h"

#include <cstddef>
#include <cstdint>

extern "C" std::size_t LLVMFuzzerMutate(std::uint8_t*, std::size_t, std::size_t);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  lanternstage::PlaybackEngine engine;
  (void)engine.replay({data, size}, lanternstage::ReplayMode::package);
  return 0;
}

extern "C" std::size_t LLVMFuzzerCustomMutator(std::uint8_t* data,
                                                std::size_t size,
                                                std::size_t maximum_size,
                                                unsigned seed) {
  return lanternstage::fuzzing::mutate_bundle(
      data, size, maximum_size, seed, lanternstage::ReplayMode::package, LLVMFuzzerMutate);
}
