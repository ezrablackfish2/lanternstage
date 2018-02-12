# LanternStage

LanternStage is an offline-first C++17 library and command-line tool for preparing,
validating, scheduling, and replaying digital-signage packages. It is intended for
installations where displays may be intermittently connected—museum galleries,
transit concourses, campuses, retail sites, and temporary exhibitions—and where a
package must be checked completely before it is activated on a device.

A LanternStage package describes media assets, playlists, recurrence windows,
locales, device groups, and deployment constraints. The library can normalize a
manifest, calculate a deterministic package digest, select content for a device,
plan bandwidth-limited deployment waves, encode engine snapshots, and replay a
checksummed transaction stream without requiring a database or network service.

## Why LanternStage exists

Conventional signage controllers often combine package parsing, scheduling, device
state, and playback mutation in a single online service. That design is awkward for
sites that stage content on removable media, approve a release in an isolated
network, or need to reproduce exactly what a player did after an incident.
LanternStage separates those concerns into a portable library:

- **Package validation** checks paths, media types, dimensions, playlist links,
  locale tags, recurrence windows, and device-group references.
- **Deployment planning** determines the assets each device needs, reuses matching
  inventory, checks storage and network limits, and groups downloads into waves.
- **Deterministic replay** applies a compact command journal with sequence numbers,
  per-record checksums, transaction boundaries, and a final observation digest.
- **Snapshot utilities** encode versioned engine state, coalesce adjacent chunks,
  and extract logical byte ranges for inspection or transfer.
- **Playback services** maintain asset variants, playlist graphs, schedules,
  translations, device leases, journals, expression programs, and compositor
  buffers behind one replay interface.

The project has no runtime network dependency and does not need credentials. All
formats are designed to be inspectable or reproducible from a clean checkout.

## Build

Requirements:

- A C++17 compiler (Clang 12+, GCC 10+, or a compatible compiler)
- CMake 3.16+

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The build produces:

- `liblanternstage.a` — the embeddable library
- `lanternctl` — package, snapshot, query, deployment, and replay utility
- `lanternstage_tests` — unit and integration tests

To install the library and CLI:

```sh
cmake --install build --prefix /usr/local
```

## Quick tour

Validate a package manifest:

```sh
./build/lanternctl validate examples/gallery.lst
```

Write a canonical representation suitable for review or signing:

```sh
./build/lanternctl normalize examples/gallery.lst normalized.lst
./build/lanternctl digest normalized.lst
```

Plan a release for a device inventory at Unix time `1782550800`:

```sh
./build/lanternctl plan \
  examples/gallery.lst \
  examples/devices.tsv \
  1782550800 \
  1782590400 \
  deployment-plan.json
```

Evaluate a device-selection expression:

```sh
./build/lanternctl query \
  'capabilities >= 3 and locale starts_with "en" and online = true' \
  capabilities=7 locale=en-US online=true
```

Inspect a binary snapshot:

```sh
./build/lanternctl snapshot-info player-state.lss
```

Replay a recorded engine stream:

```sh
./build/lanternctl replay player-journal.lstg full
```

## Manifest format

Manifests are UTF-8 text files divided into named sections. The package section
contains release metadata; asset sections identify immutable media; playlist item
sections connect assets into a graph; and group sections assign playlists to
compatible devices.

```ini
[package]
format=1.0.0
id=gallery-east
revision=12
title=East Gallery Rotation
default_locale=en-US
property.owner=exhibitions

[asset.10]
path=media/welcome.png
type=image/png
size=8192
hash=1111
width=1920
height=1080
locale=en-US
tags=welcome,day

[playlist.20]
name=Entrance sequence
root=1
starts=2026-01-01T08:00:00Z
ends=2027-01-01T22:00:00Z

[playlist.20.item.1]
asset=10
duration_ms=8000
transition=fade

[group.30]
name=Entrance displays
capabilities=3
devices=100,101
playlists=20
```

Asset paths must be relative and cannot traverse parent directories. Playlist item
identifiers are local to their playlist. Supported transitions are `cut`, `fade`,
`wipe`, and `crossfade`. Time values use ISO 8601, and recurrences use a constrained
RRULE representation. See [docs/manifest.md](docs/manifest.md) for the complete
field reference.

## Deployment inventory

`lanternctl plan` reads a tab-separated device file. Blank lines and lines beginning
with `#` are ignored. The required columns are:

1. device id
2. display name
3. locale
4. capability bit mask
5. total storage bytes
6. currently available storage bytes
7. viewport width
8. viewport height
9. transfer rate in KiB/s

Optional columns select the network class (`offline`, `metered`, `standard`, or
`high-bandwidth`) and set the beginning and end of the device availability window.
The planner reports missing assignments, locale fallback, viewport scaling, storage
pressure, metered-network limits, transfer duration, and activation feasibility.

## Transaction stream

The replay engine consumes the `LSTG` binary format. A stream has a versioned header,
a stream identifier, a command count, a declared length, and a checksum over the
record region. Each record has an opcode, flags, monotonically increasing sequence,
payload length, and a checksum seeded by the stream identity and sequence.

Transactions carry a revision and cookie and may be nested to a bounded depth.
Subsystems receive begin, commit, and abort notifications so a replay can model a
release that spans assets, scheduling, localization, devices, journal state,
expressions, and compositor resources. Replay returns applied/rejected counts,
transaction totals, the final logical clock, the final revision, and an observation
hash. See [docs/command-stream.md](docs/command-stream.md).

## Library integration

```cpp
#include <lanternstage/engine.h>
#include <lanternstage/manifest.h>

lanternstage::DiagnosticSink diagnostics;
auto manifest = lanternstage::parse_manifest_text(text, &diagnostics, "release.lst");
if (!manifest) {
  // Present diagnostics to the operator.
}

lanternstage::PlaybackEngine engine;
auto result = engine.replay({journal.data(), journal.size()});
if (result) {
  std::cout << result.value.observation_hash << "\n";
}
```

Public headers are self-contained under `include/lanternstage`. Most parsing APIs
return `Result<T>`, while mutation APIs return `Status`. The value remains available
for diagnostics and partial inspection, but callers should only use it as accepted
state when the status is successful.

## Source layout

```text
include/lanternstage/   public C++ API
src/                    library implementation
tools/lanternctl.cpp    command-line utility
tests/                  unit and integration tests
examples/               sample package and device inventory
fuzz/                   structured robustness test entry points
fuzz/corpus/            entry-point-specific valid replay seeds
.clusterfuzzlite/       reproducible sanitizer build
```

The playback engine is intentionally split into domain services rather than one
large parser. `AssetStore`, `PlaylistGraph`, `ScheduleEngine`,
`LocalizationCatalog`, `DeviceRegistry`, `JournalStore`, `ExpressionVm`, and
`Compositor` each own their state and contribute to the replay observation digest.
The manifest, query, time, layout, diagnostics, snapshot, and deployment-planning
modules are also usable without the replay engine.

## Robustness testing

The repository includes five structure-aware libFuzzer entry points. Their custom
mutator decodes valid `LSTG` bundles, mutates commands and payloads, restores command
sequence invariants, recalculates checksums, and re-encodes the stream. This lets
sanitizer testing exercise normal state transitions instead of spending most of its
budget on invalid headers.

ClusterFuzzLite-compatible builds use:

```sh
SRC="$PWD" OUT="$PWD/out" \
CXX=clang++ \
CXXFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address -fsanitize=fuzzer-no-link' \
LIB_FUZZING_ENGINE='-fsanitize=fuzzer,address' \
.clusterfuzzlite/build.sh
```

The script writes all five target executables to `$OUT`:

- `transaction_fuzzer`
- `package_fuzzer`
- `snapshot_fuzzer`
- `compositor_fuzzer`
- `expression_fuzzer`

Each target has its own valid seed directory under `fuzz/corpus/`. The build is local,
deterministic, non-interactive, and performs no downloads.

## Design guarantees

- File and stream parsing is deterministic.
- Normal builds do not access the network.
- Package digests do not depend on hash-map iteration order.
- Serialized manifests and deployment plans use a stable ordering.
- Device plans explicitly expose warnings and errors rather than silently dropping
  content.
- Replay state is isolated to an engine instance and can be reset between streams.
- Binary formats carry versions and declared sizes for forward compatibility.

## License

LanternStage is available under the Apache License 2.0. See [LICENSE](LICENSE).

## Repository Reconstruction Notice

This repository was reconstructed from recovered local project files and available timestamp metadata after the original repository was lost. Commit dates reflect the recovered timestamp data where available. The history is intended to preserve the original project timeline as accurately as possible, not to fabricate or inflate contribution activity.

