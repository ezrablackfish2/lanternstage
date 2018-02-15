# LSTG command stream

`LSTG` is the appendable replay representation used by LanternStage players and
administrative tools. It is not a memory dump. Every integer has an explicit
little-endian encoding, and every variable-length region is preceded by a bounded
length.

## Bundle header

The fixed header contains the four-byte magic, version, feature bits, stream id,
record count, reserved field, declared byte length, and a seeded FNV-1a checksum of
the complete record region. Version 1 requires the reserved field to be zero.

## Command record

A record contains an opcode, flags, payload length, sequence, and payload checksum.
Sequences are unique and strictly increasing. The checksum seed combines stream id,
sequence, opcode, and flags, preventing a record copied from another stream from
being accepted accidentally.

## Transactions

Begin, commit, and abort commands carry a revision and transaction cookie. Nested
transactions are bounded. Commands marked transactional are valid only within an
open transaction. Services may stage work under the active revision and receive a
matching lifecycle notification when the transaction ends.

## Replay modes

The full mode enables every command family. Package, snapshot, compositor, and
expression modes restrict dispatch to the state needed by specialized importers.
Framing and checksums are identical in every mode, allowing the same journal tools
to inspect all streams.

## Evolution

Readers reject unsupported major versions and unknown structural requirements.
Feature bits are reserved for compatible additions. New opcodes should remain
self-delimiting, have deterministic effects, and preserve the rule that a rejected
command cannot consume bytes from a following record.
