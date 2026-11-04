# Shared game channel policy (J5a)

`src/mod-ollama-chat_bridge_channel.{h,cpp}` is a production plain-value/state helper. It does not create a
listener, acknowledge game activation, read game objects, queue work, execute commands or integrate native tools.
No current runtime caller changes. J5b owns transport/physical queues; J5c owns actual filtered observations;
J6 owns execution; J9C/X owns live proof. Native C and X transports remain their respective owners' work.

The semantic source is HUB `bridge/game/README.md`, `boundary.js` and `examples.json` (frozen J2 v1).
`Receive` authenticates the local game-link bearer before checking body size or parsing JSON. Subscription
credentials never belong here. The helper returns fixed `ChannelErrorName` strings, never payloads, tokens or
exception text. The transport must bound bytes while receiving, authenticate every operation and bind literal
127.0.0.1. A received body is limited to 65536 bytes before JSON parsing, nesting to 16, with duplicate keys refused.
All JSON objects have exact fields; numeric integer fields reject floating-point JSON representations.

Every envelope has exactly `version:1`, `instance`, `provider`, `epoch`, `requestId`, `deadlineUnixMs`, `type`,
plus the type's fields below. Identifiers use J2's ASCII syntax, 1..128 bytes. GUIDs are canonical decimal uint64
strings; zero cannot identify an actual game unit. Times are integral milliseconds within JavaScript's exact range.
Expiry is at equality on either wall or captured monotonic clock. Maximum captured remaining lifetime is 120000 ms;
work replies must preserve the original wall deadline exactly. Wall-clock rollback cannot extend monotonic expiry.

| Type | Additional exact fields | Owner/direction |
| --- | --- | --- |
| query | controllerGuid, botGuid, query:"observable_group" | Authenticated native to game |
| command | commandId, issuerGuid, botGuid, intent, params | Authenticated native to game; staged typed proposal |
| generation.result | text (1..4096 bytes, usable text, no NUL) | Authenticated native to game |
| events.result | none | Authenticated native to game |
| failure | code: unavailable/timeout/invalid_input/cancelled | Authenticated native to game, or game FinishFailure; terminal |
| events | controllerGuid, botGuid, events | Game-created by OpenEvents |
| query.result | units | Game-created by FinishQuery |
| command.result | commandId, outcome: applied/rejected/failed | Game-created by FinishCommand |

Generation encoding remains the existing J4 builder; `OpenGeneration` registers its exact game-owned id/scope/deadline
without duplicating that builder. Events contain 1..32 typed `observed_mark` entries with exactly kind, targetGuid
and icon (integer 0..7). No other event schema or capability is implied. Query units have guid/name and optional
healthPercent (0..100), role (tank/healer/damage) and visibleTargetGuid. Unknown fields are omitted; at most 40
distinct units and 32768 encoded result bytes. The world adapter MUST establish group/visibility and observed
provenance before supplying units or marks; typed shape alone does not prove perception.

Command intent/parameter shapes match J2 exactly: follow/stay/flee/regroup/attack_own_target `{}`;
pull/focus_healing `{targetGuid}`; mark `{targetGuid,icon}`; assign_role `{role:tank|healer|damage}`;
save_mana `{enabled:boolean}`; formation `{shape:close|spread|line}`; position `{x,y,z,mapId}` (finite numbers,
uint32 map id); dungeon_clear `{mode:on|off|pause}`. Parsing emits enums and variant values only. No literal command
syntax is built. A staged proposal reserves its effect id before any possible later attempt, including failure.
`Receive` is NEVER an execution grant: until J6, consumers must reject unsupported execution and cannot report an
applied effect. J6 must re-resolve GUIDs, actual current controller/group, target observation/eligibility,
map/phase/pathability/rate and activation/deadline at the actual execution seam. Proposed text must not enter order parsing.

The single world owner calls `Bind` only AFTER the existing activation owner has selected a fresh epoch and proved
actual old-work drain and strategy restoration. Bind itself proves neither. Every bind retires the prior helper
state immediately; invalid binding stays inactive. Reconnect requires a fresh epoch and fresh registered scope/work;
it cannot revive terminal/failed work. Reused epochs are rejected using bounded helper-lifetime history (4096 epochs,
then fail closed). This is not an RNG: the activation owner supplies cryptographically fresh epochs on process restart.
Do not destroy/recreate this helper under a still-active epoch to reset replay state.

`ObserveScope` accepts trusted game captures only: exact identity, controller/bot, context/channel, perception,
context/membership/persona revisions. At most 64 scopes, live for 5000 ms after a trusted observation. Scope removal,
revision change or lapse invalidates existing work permanently, even if an identical scope is registered later.
`current` arguments are fresh world captures, never native JSON. J5b must also consult the actual world's observer/
activation/currentness guards before acceptance and publication; this in-process state cannot discover a logout.

Generation/events must be registered by the game. Inbound queries may create a new request reservation only against
an already registered current scope and matching controller/bot. Queries share the request-id replay namespace but
can never authorize commands. Commands need an open generation/events request and matching controller/bot/deadline.
Only one terminal generation/events result or failure is accepted; commands must precede it. A command's result is
terminal once and can report a previously staged command after its parent terminal, only while scope/deadline remain
current; that is reporting, not permission to execute after closure. `Current(..., requireOpen=true)` rejects closed
requests. Duplicate/stale/malformed input yields no output message; output bodies clear before refused operations.

Each epoch retains at most 4096 request ids and independently 4096 command ids, including failed/expired/terminal ids.
No eviction or timeout reuse. Logical pending requests are limited to 64; limits can only be reduced in the constructor.
Expiry invalidates records and frees logical pending counts, never replay reservations. Retiring clears maps only
because the old epoch is unusable. These limits DO NOT account physical receive/worker/completion payload slots:
J5b must hold a bounded slot until actual disposal, independently of logical terminal/cancel/expiry and worker drain.
Reservation expiry/invalidations run on explicit work/scope operations; do not install an all-reservations scan
on every game tick. The helper has no autonomous timer or background thread.

Offline proof compiles this exact production cpp with `tests/bridge-channel-selftest.cpp`. It covers authentication,
strict decode, epoch retirement/reconnect, exact expiry/rollback, removed/revised/lapsed scope, terminal/replay state,
typed command staging, bounded query/event shapes and capacity fail-closed. It is no full module/world/native proof.
