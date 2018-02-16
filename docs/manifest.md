# Package manifest reference

A manifest is UTF-8 text with `key=value` records grouped by `[section]` headers.
Whitespace around keys and values is ignored. Blank lines and lines beginning with
`#` are comments. The implicit section before the first header is `package`, though
canonical files include `[package]` explicitly.

## Package

Required fields are `format`, `id`, `title`, and `default_locale`. `format` is a
three-part semantic version; version 1 is currently supported. `revision` is an
unsigned release revision. Arbitrary package metadata uses `property.<name>`.

## Assets

An asset section is `[asset.<id>]`. Required fields are `path` and `type`. `path` is
relative to the package root and cannot contain parent traversal. `type` is an IANA-
style media type containing a slash. Optional fields are `size`, `hash`, `width`,
`height`, `locale`, and comma-separated `tags`. Width and height must be supplied
together.

## Playlists

A playlist section is `[playlist.<id>]` and requires `name`. `root` identifies the
first item. Optional `starts`, `ends`, and `recurrence` restrict its active window.

An item section is `[playlist.<playlist-id>.item.<item-id>]`. It contains `asset`,
`duration_ms`, `transition`, and optional `next`. Metadata uses `meta.<name>`. Every
asset and next-item reference must resolve. Durations are positive and limited to 24
hours.

## Device groups

A group section is `[group.<id>]` and requires `name`. `capabilities` is a bit mask;
a device must provide every required bit. `devices` and `playlists` are comma-
separated numeric identifiers. Groups may overlap, and playlist selection removes
duplicates.

## Recurrence

Recurrences use semicolon-separated fields, for example:

```text
FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE,FR;COUNT=20
```

Supported frequencies are daily, weekly, monthly, and yearly. The parser also
supports interval, count, until, weekday, month-day, month, hour, and minute filters.
Invalid or contradictory civil dates are rejected before planning.
