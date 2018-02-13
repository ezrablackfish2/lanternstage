# Architecture

LanternStage has three independent public surfaces: package preparation, deployment
planning, and deterministic engine replay. They share small value types and format
utilities but can be linked separately by applications that do not need every
feature.

## Package preparation

`manifest.cpp` parses UTF-8 sectioned manifests into `PackageManifest`. Validation
is a separate pass so an editor can retain a partially parsed document and present
multiple source-mapped diagnostics at once. Manifest serialization sorts map-backed
fields and emits a canonical section order. The manifest digest is calculated from
that canonical representation.

`text.cpp`, `time_math.cpp`, and `diagnostics.cpp` provide locale parsing, Unicode
checks, recurrence handling, civil-time conversion, and consistent errors.
`query.cpp` implements a small predicate language for device and content selection.
`layout.cpp` contains deterministic viewport and color calculations.

## Deployment planning

`DeploymentPlanner` combines a manifest, device inventory, cached asset inventory,
and policy. For every device it selects assigned playlists, walks referenced media,
filters assets already present with the same content hash and size, checks locale and
viewport constraints, estimates transfer time, and verifies storage headroom.

Feasible devices are packed into waves under a parallel-transfer limit. Waves are
scheduled backward from activation so slower devices begin first. The resulting plan
is self-validating and has a stable digest. It can be serialized as JSON for an
operator or downstream transfer service.

## Replay engine

`PlaybackEngine` parses a command bundle, checks stream and record checksums, enforces
sequence ordering, and dispatches accepted commands to domain services. A command
may be rejected without terminating replay; malformed framing rejects the complete
bundle. Transactions propagate begin, commit, and abort events to all services.

The services are:

- `AssetStore`: media definitions, variants, aliases, deltas, dictionaries, fonts.
- `PlaylistGraph`: linked playlist items, overlays, triggers, preview state.
- `ScheduleEngine`: time zones, recurrence rules, intervals, occurrences.
- `LocalizationCatalog`: locale fallback and translated catalog snapshots.
- `DeviceRegistry`: device identity, tenant leases, playback reservations.
- `JournalStore`: segmented event records, snapshots, rollback tokens.
- `ExpressionVm`: typed schemas, compiled expressions, evaluation cache.
- `Compositor`: indexed planes, palette updates, crops, patches, tiled rendering.

Each service owns its allocations and maintains an observation hash. The engine folds
those hashes with the logical clock and revision to make replay results comparable
across machines.

## Binary formats

The command stream (`LSTG`) and snapshot container (`LSSN`) are little-endian. Both
have fixed headers, explicit versions, declared lengths, bounded counts, checksums or
digests, and no native-structure serialization. Readers use `WireReader`, which
advances only after validating the requested span. Writers use `WireWriter` and are
therefore independent of host alignment.

## Testing boundaries

Unit tests cover format round trips, manifest validation, recurrence and locale
utilities, query evaluation, snapshot extraction, layout, deployment planning, and
empty replay. Structured sanitizer targets drive the same engine APIs as production
callers. The fuzz build compiles only the replay dependency graph, while CMake builds
the complete library and CLI.
