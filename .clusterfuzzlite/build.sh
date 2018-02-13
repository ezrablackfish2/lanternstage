#!/bin/bash -eu

SRC="${SRC:-$(cd "$(dirname "$0")/.." && pwd)}"
OUT="${OUT:-$SRC/out}"
WORK="${WORK:-$OUT/.objects}"
CXX="${CXX:-clang++}"
CXXFLAGS="${CXXFLAGS:-}"
LIB_FUZZING_ENGINE="${LIB_FUZZING_ENGINE:-}"
BUILD_JOBS="${LANTERNSTAGE_BUILD_JOBS:-4}"
mkdir -p "$OUT" "$WORK"
rm -f "$WORK"/*.o "$WORK"/*.a

COMMON_FLAGS=(-std=c++17 -I"$SRC/include" -I"$SRC/fuzz")
PROJECT_SOURCES=(
  asset_store command compositor device_registry engine expression_vm journal
  localization playlist_graph schedule_engine wire
)
OBJECTS=()
PIDS=()

wait_batch() {
  local status=0
  for pid in "${PIDS[@]}"; do
    if ! wait "$pid"; then status=1; fi
  done
  PIDS=()
  return "$status"
}

for unit in "${PROJECT_SOURCES[@]}"; do
  object="$WORK/$unit.o"
  OBJECTS+=("$object")
  "$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" -c "$SRC/src/$unit.cpp" -o "$object" &
  PIDS+=("$!")
  if [ "${#PIDS[@]}" -ge "$BUILD_JOBS" ]; then
    wait_batch
  fi
done
wait_batch

SUPPORT_OBJECT="$WORK/fuzz_support.o"
"$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" -c "$SRC/fuzz/fuzz_support.cpp" -o "$SUPPORT_OBJECT"

ARCHIVER="${AR:-ar}"
"$ARCHIVER" rcs "$WORK/liblanternstage_fuzz.a" "${OBJECTS[@]}"

TARGETS=(transaction package snapshot compositor expression)
for target in "${TARGETS[@]}"; do
  "$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" \
    "$SRC/fuzz/${target}_fuzzer.cc" "$SUPPORT_OBJECT" "$WORK/liblanternstage_fuzz.a" \
    $LIB_FUZZING_ENGINE -o "$OUT/${target}_fuzzer"
done

rm -rf "$WORK"
