# Shared game channel policy and runtime integration (J5)

`src/mod-ollama-chat_bridge_channel.{h,cpp}` is a production plain-value/state helper. It does not create a
listener, acknowledge game activation, read game objects, queue work, execute commands or integrate native tools.
The shared runtime installs bounded physical mailboxes, filtered query assembly and current-mark event production.
J6 owns execution; J9C/X owns live proof. The transport is J5d's `src/mod-ollama-chat_bridge_listener.{h,cpp}`: a
127.0.0.1 HTTP listener (`OllamaChat.Bridge.ChannelPort`) that authenticates the link token before reading a body,
bounds bytes and hands each envelope to `Receive`/`Take` (wire: HUB `bridge/game/README.md`). J11C/X owns the tools.

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
| query | controllerGuid, botGuid, query:"observable_group" or "catalogue", or query:"bot_state" with topic (route: and place) | Authenticated native to game |
| command | commandId, issuerGuid, botGuid, and intent, params (the old form) or steps (LC3b) | Authenticated native to game; staged typed proposal |
| generation.result | text (1..4096 bytes, usable text, no NUL) | Authenticated native to game |
| events.result | none | Authenticated native to game |
| failure | code: unavailable/timeout/invalid_input/cancelled | Authenticated native to game, or game FinishFailure; terminal |
| events | controllerGuid, botGuid, events | Game-created by OpenEvents |
| query.result | units; a catalogue query: catalogue | Game-created by FinishQuery, FinishCatalogue |
| command.result | commandId, outcome: applied/rejected/failed; a stepped command: steps | Game-created by FinishCommand |

Generation encoding remains the existing J4 builder; `OpenGeneration` registers its exact game-owned id/scope/deadline
without duplicating that builder. Events contain 1..32 typed `observed_mark` entries with exactly kind, targetGuid
and icon (integer 0..7). No other event schema or capability is implied. Query units have guid/name and optional
healthPercent (0..100), role (tank/healer/damage) and visibleTargetGuid. Unknown fields are omitted; at most 40
distinct units and 32768 encoded result bytes. The world adapter MUST establish group/visibility and observed
provenance before supplying units or marks; typed shape alone does not prove perception.

Command intent/parameter shapes match J2 exactly: follow/stay/flee/regroup/attack_own_target `{}`;
pull/focus_healing `{targetGuid}`; mark `{targetGuid,icon}`; assign_role `{role:tank|healer|damage}`;
save_mana `{enabled:boolean}`; formation `{shape:close|spread|line}`; position `{x,y,z,mapId}` (finite numbers,
uint32 map id); dungeon_clear `{mode:on|off|pause}`; WT18b's goal verbs go_to/meet_at `{place}` (1..64 bytes of
text, not blank, no control characters), quest `{questId}` (uint32, nonzero) and
errands/set_hearth/grind_here/gather_here/pause_plan/resume_plan `{}`. Parsing emits enums and variant values only. No literal command
syntax is built. A staged proposal reserves its effect id before any possible later attempt, including failure.
`Receive` is NEVER an execution grant. The world executor re-resolves GUIDs, actual current controller/group, target
observation/eligibility, map/phase/pathability/rate and activation/deadline at the actual execution seam. Proposed text
must not enter order parsing. Executed verbs: attack_own_target, mark, flee and position are one-time moves;
follow, stay, formation, save_mana and focus_healing are lasting plans (J12): the bot's state of that part is captured
before every attempt, and the world tick restores it on expiry (10 minutes after the latest repeat), a group change,
loss of the controller's authority (master, or leader of a masterless bot), or bridge retirement (a reload or
switch, also one that sets `OllamaChat.Enable = 0`: the disabled tick still runs these restores). Any later change of
that part wins and is left alone, whoever makes it: another person, the controller's own playerbots command, or
playerbots itself; a change that leaves the part as the bridge set it (the same stay again) does not count. A
logged-out bot's plan is dropped unrestored, and playerbots' own logout save can keep the bridge-set state in its
database for the next login, as it does for its own commands. An attempt that may have changed the bot but left no
plan (unreachable today) is counted as `unrecorded` in the "Bridge plans:" log line. regroup, pull, assign_role
and dungeon_clear have no safe playerbots effect and stay a fixed refusal (DS.9). The goal verbs (WT18b) hand the
bot's planner a goal through playerbots' plausibility gate; they need no group, have no lasting part (no capture, no
plan, no restore) and are refused with not_planner, goal_held, unknown_place, no_route, no_such_quest, nothing_here or
already_there. A bot_state query names one topic; route, and only route, also names its place (WT18b), which the
world thread hands to playerbots' topic reader as a value. The planner topics (plan, options, route, quest_plan, bags,
places) share one entry shape: row (a fixed word per topic, from `StateTopicRow`), name, value and extra.
Retirement selects no new epoch until every retired plan is restored or dropped.

LC3: the verbs and the topic words are playerbots' (`PlayerbotBridge::IntentWord`/`FindIntent` and `TopicRows`).
The game adapter hands them to the channel once, on the world thread at startup (`OllamaDispatch_Start` calls
`OllamaChat_BridgeInstallAbilities`), as a plain-data `AbilityIndex`: the catalogue rows, the 22 verbs and the
hash. The module keeps no word list of its own; the 22 verbs and 11 topic words of the old wire are unchanged.
A new topic still needs its `StateTopic` value and entry shape here (playerbots' `.agents/docs/ai-engine.md`).
The install is fail-closed. An index whose verbs, row names, act/verb pairs or topic codes disagree installs
nothing, and the module then takes no command and no bot_state query; the log says `Bridge ability catalogue
refused` (error). LC3b: an index whose catalogue cannot be sent (text that is not UTF-8, or over 24576 encoded
bytes) installs without it: commands and bot_state queries stand, the catalogue query answers `unavailable`, and
the log says `Bridge ability catalogue not sent` (error). Otherwise the log says `Bridge ability catalogue:
N rows, hash H, B bytes.` (info). The handler's static_assert ties playerbots' `IntentCount` to the module's
`ChannelIntentCount` and its intent pairs, so a new playerbots act fails the build until the module has its
`ChannelIntent` value and pair. A `catalogue` query carries observable_group's exact keys and is route-bound
like every query. Its result is the envelope plus `catalogue`: `hash` (FNV-1a 64 of playerbots' `CatalogueText`
as 16 lowercase hex digits, compared as a string, never recomputed), `version` (the catalogue format, 1),
`abilities` (what this module takes beyond the old wire, `["catalogue", "names", "steps"]` today; not hashed), `maxSteps` and
`rows`. Each row has kind (act or topic), name, sentence, params (name, kind, sentence, choices), refusals (word,
sentence), effect, done, state, event and limit. The answer is built once at install and stays under 24 KB of
the 32768-byte response limit (selftested; 15187 bytes for the 29 rows of LC3); a later step pages it when the
rows outgrow that. Without an installed index the query fails `unavailable`; a module older than LC3 answers it
`invalid_input`. An order that names a unit or quest the bot cannot find is refused `unknown_name` ("I do not know
what you mean by that.").

LC3b, the stepped command (abilities `names` and `steps`): a `command` with `steps` in place of `intent` and
`params`; both forms at once is `invalid_input`. `steps` holds 1..`maxSteps` (3) entries, each exactly
`{"act": <an act row's name>, "params": {...}}` with exactly that row's parameters by their catalogue names: a
`unit` parameter as `<name>` (the unit's name) or `<name>Guid` (a guid string), a `quest` parameter as `<name>`
(its title) or `<name>Id` (1..4294967295), exactly one of the two; a `place` parameter as a place name (go_to,
meet_at) or, for `position`, the spot `"x,y,z,map"` (three finite decimal numbers and a map id, nothing else); a
`choice` parameter as one of the row's choice words (an icon by its word, `star` .. `skull`, not 0-7: one
catalogue-driven rule); a `flag` as a boolean. Names and places are 1..64 bytes without control characters. A
topic, a verb without a row (regroup, pull, assign_role, dungeon_clear), an unknown word, a missing or an extra key
are `invalid_input`. The issuer, bot and group are the scope's for every step. The order takes one command id,
one rate slot and one authorization: the module builds one playerbots order per step and calls the span
`Execute` once; playerbots' gate resolves each name among what the bot perceives or knows, checks visibility,
group and the row's check per step (for `position` the same reachable path the old form's pre-check used), and the
first step not applied ends the order. Before it runs, every lasting step is admitted to the plan ledger and its
part captured (`busy` when a plan refuses it, `cannot_now` for two steps of one lasting part, both before any step
runs); each lasting step that ran and applied or failed is recorded after the order. The result keeps the old
shape: `outcome` is applied only when every step applied, else the ending step's (an unavailable step is
`rejected` with `unavailable`); `reason`/`reasonText` are the ending step's, in the catalogue's words, or the
order's own when it was refused before any step ran (`too_soon`, `unavailable`, `busy`, `cannot_now`). It adds
`steps`: one entry per submitted step in order, `{"act", "outcome": applied/rejected/failed/skipped, "reason",
"reasonText"}`, the reason only on rejected and failed; a step that did not run is `skipped`, so a fully refused
order lists every step skipped and carries its own reason. The old form's result has no `steps`.

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


## J5c filtered observations and runtime integration

`mod-ollama-chat_bridge_query.{h,cpp}` supplies production capture/revalidation logic against a synchronous,
world-only `QueryWorldSource`. The source must re-resolve GUIDs and actual group, map, instance, phase,
distance and detection on every call. Native JSON cannot implement this source or supply its observations.
The helper independently requires the exact current game `WorldTicket` and `DispatchGuard` before and after
capture. No request-created world authority, role inference, private AI state or selected-target access exists.
Only guid/name and observed UI health are emitted; role and visibleTargetGuid stay omitted until a separately
reviewed observable provenance source exists. Missing/hidden group units are omitted. Malformed or more than 40
roster entries fail closed; a non-group scope returns unavailable, not invented party members.

`QueryObservation` and `MarkObservation` contain owned values and a captured 5-second wall/monotonic deadline.
They confer no authority. `Current` resolves the world and observations again, compares the current values and
rejects equality expiry, authority/group/perception/revision changes, removed units and changed marks. Call it
immediately before publication; never queue the source, a game pointer, or a world-reading closure to a worker.
Marks are current group-UI icons 0..7 on currently visible targets (including visible enemies), requested by a
trusted game hook. Unknown/removed/hidden marks and duplicate icon entries fail closed. At most 32 inputs are
accepted; eight distinct current icons is the actual schema's maximum. No historical mark stream, removal event,
new event kind, synchronous network call or per-tick world/group scan is introduced.

The runtime source uses real Player/Group reads and wires the query branch of `WorldChannel::Update` to these
observations. It revalidates provenance after serialization and immediately before inserting the packet into the
completion map. `ObserveEvents` supplies the final-validation callback to `RegisterEvents`, which checks the marks
before `_done` publication and before exposing the receipt. Observer callbacks stay synchronous and bounded on
the world thread; publication does not rely on later receipt retraction.

An event producer must reserve a distinct game `WorldTicket` against an already observed current scope, with
lifetime at most 5000 ms, capture marks, validate the source at the final channel publication seam, and cancel/close
that ticket on every failed registration. Cap both event ticket clocks at the captured observation deadline;
registration must not renew the age of an earlier observation. Return both the opaque route and receipt to the native
producer only after successful publication; the producer takes the receipt with the local bearer and retains the
move-only `ChannelPacket` throughout an asynchronous write/cancellation until payload disposal. Inbound queries,
commands and outgoing events share 64 physical packet slots and dequeue remains 8 per tick. Healthy heartbeat
refresh remains bounded by 64 scopes and must not scan the 4096-entry replay ledger. J6 effects remain CLOSED.

The standalone `tests/bridge-query-selftest.cpp` links the actual helper, world policy, channel policy and physical
mailbox with controlled current world objects. It checks visibility/missing units, omitted provenance, real query
encoding, currentness/expiry/reconnect, separate event reservation/receipt and returned packet lifetime. Its caller
illustrates the handoff, but is not production dispatcher wiring, real game-object adapter, native socket/tool
integration or live J9C/X acceptance.

`tests/bridge-query-dispatch-selftest.cpp` additionally links the production bridge/query/world/channel code and
extracts the actual handler adapter, refresh and dispatcher channel bodies verbatim against controlled game-object
stand-ins. It covers the runtime query/event joins and final publication checks offline. It does not prove a full
module build or live game delivery. The full J5 parent remains open pending independent review; native J11C/X
adapters, J6 effects and live J9C/X delivery remain later work.

`tests/bridge-listener-selftest.cpp` (J5d) links the real listener and its cpp-httplib against a counting fake mailbox
and against the production `WorldChannel`, with the test thread playing the world tick: token, bind address,
malformed and oversize input, thread handoff, deadline, shutdown and configuration. Live calls are J9C/X work.
