# Shared game channel policy and runtime integration (J5)

The Codex adapter (observer routes, native listener) left the live paths with LCB0; parked in
`retired/codex-observer/` (CS.20).

`src/mod-ollama-chat_bridge_channel.{h,cpp}` is a production plain-value/state helper. It does not create a
listener, acknowledge game activation, read game objects, queue work, execute commands or integrate native tools.
The shared runtime installs bounded physical mailboxes and filtered query assembly.
J6 owns execution; J9C/X owns live proof. The transport is J5d's `src/mod-ollama-chat_bridge_listener.{h,cpp}`: a
127.0.0.1 HTTP listener (`OllamaChat.Bridge.ChannelPort`) that authenticates the link token before reading a body,
bounds bytes and hands each envelope to `WorldChannel::Receive` with its own `ChannelAnswer`, on which the handler
waits (wire: HUB `bridge/game/README.md`). J11C/X owns the tools. The channel runs whenever the Bridge settings are
valid and the provider is the bridge; it has no switch of its own (LCB6b: `ChannelEnabled` is gone).

The semantic source is HUB `bridge/game/README.md`, `boundary.js` and `examples.json` (frozen J2 v1).
The listener's `BearerMatches` is the one check of the local game-link bearer, made before any body byte is read;
`Receive` takes no bearer. A channel that is not bound (never bound, retired, or after a refused rebind)
answers `inactive` (503) before checking body size or parsing JSON. Subscription credentials never belong here.
`src/mod-ollama-chat_bridge_json.h` is the one home of the wire checks the bridge files share: the strict parse,
identifiers, received and sent text, the envelope and the bearer compare (LCB6a). The helper returns fixed `ChannelErrorName` strings, never payloads, tokens or
exception text. The transport must bound bytes while receiving, authenticate every operation and bind literal
127.0.0.1. A received body is limited to 65536 bytes before JSON parsing, nesting to 16, with duplicate keys refused.
All JSON objects have exact fields; numeric integer fields reject floating-point JSON representations.

Every envelope has exactly `version:1`, `instance`, `provider`, `epoch`, `requestId`, `deadlineUnixMs`, `type`,
plus the type's fields below. The game always sends `provider` as `claude` (no option sets it since LCP2a). Identifiers use J2's ASCII syntax, 1..128 bytes. GUIDs are canonical decimal uint64
strings; zero cannot identify an actual game unit. Times are integral milliseconds within JavaScript's exact range.
Expiry is at equality on either wall or captured monotonic clock. Maximum captured remaining lifetime is 120000 ms;
work replies must preserve the original wall deadline exactly. Wall-clock rollback cannot extend monotonic expiry.

| Type | Additional exact fields | Owner/direction |
| --- | --- | --- |
| query | controllerGuid, botGuid, query:"observable_group" or "catalogue", or query:"bot_state" with topic (route: and place) | Authenticated native to game |
| command | commandId, issuerGuid, botGuid, steps (LC3b; LCB2: the only form) | Authenticated native to game; staged typed proposal |
| failure | code: unavailable/timeout/invalid_input/cancelled | Game-created by FinishFailure (a query the game cannot answer, a route the game cancels); never inbound |
| query.result | units; a catalogue query: catalogue | Game-created by FinishQuery, FinishCatalogue |
| command.result | commandId, outcome: applied/rejected/failed; a stepped command: steps | Game-created by FinishCommand |

Generation encoding remains the existing J4 builder; `OpenGeneration` registers its exact game-owned id/scope/deadline
without duplicating that builder. There is no event route (LC.35): `events` and `events.result` left the wire with
the Codex observer adapter (CS.20), and the hub bridge never sent either; the game pushes no mark change to the
bridge (DS.13: one-shot reads only). Query units have guid/name and optional
healthPercent (0..100), role (tank/healer/damage) and visibleTargetGuid. Unknown fields are omitted; at most 40
distinct units and 32768 encoded result bytes. The world adapter MUST establish group/visibility and observed
provenance before supplying units; typed shape alone does not prove perception.

A command is the stepped form (LC3b, below); LCB2 removed the old form (`intent` and `params`), and a body with
either key is `invalid_input`. Parsing emits typed steps only. No literal command
syntax is built. A staged proposal reserves its effect id before any possible later attempt, including failure.
`Receive` is NEVER an execution grant. Playerbots' gate re-resolves GUIDs and names, actual current controller/group,
target observation/eligibility, map/phase/pathability, and the world thread rate and activation/deadline, at the
actual execution seam. Proposed text
must not enter order parsing. Acts: attack_own_target, mark, flee and position are one-time moves;
follow, stay, formation, save_mana and focus_healing are lasting actions the bot takes itself (LC.36): one holds
until the bot changes or reverts it, or playerbots' own group and master resets end it, exactly as the matching
playerbots command would. The module keeps no record of them, restores nothing and holds no step back; a later
action of the same part simply replaces the earlier one. The goal acts (WT18b) hand the
bot's planner a goal through playerbots' plausibility gate; they need no group and are refused with not_planner, goal_held, unknown_place, no_route, no_such_quest, nothing_here or
already_there. A bot_state query names one topic; route, and only route, also names its place (WT18b), which the
world thread hands to playerbots' topic reader as a value. The planner topics (plan, options, route, quest_plan, bags,
places) share one entry shape: row, name, value and extra, and text when the game sends a sentence for the row.
LCB1e: the row word is the game's (playerbots' `BotStateItem::word`, its words in `BridgeTexts.h`), forwarded as
it comes: any word of 1..64 characters `[a-z_]` passes, an entry without such a word is dropped, and the
bridge decides what each word shows. A topic's result holds at most its catalogue row's limit of entries (the
`limit` of the catalogue answer) within the 8 KB bound; the module keeps no word or limit table of its own.
The record topics (LC7: history, lately, known) add text, one in-world sentence of 1..200 bytes
(`ChannelMaxTopicText`, cut on a character boundary); an entry without one is dropped.
Retirement selects no new epoch until the old work has drained.

LC3: the acts and the topic words are playerbots' (its catalogue rows and `TopicRows`).
The game adapter hands them to the channel once, on the world thread at startup (`OllamaDispatch_Start` calls
`OllamaChat_BridgeInstallAbilities`), as a plain-data `AbilityIndex`: the catalogue rows and the hash. The module
keeps no word list of its own; a step's act is its row's code, playerbots' `Intent` value (LCB2).
A new topic still needs its `StateTopic` value and entry shape here (playerbots' `.agents/docs/ai-engine.md`).
The install is fail-closed. An index whose row names, act codes or topic codes repeat, or whose names are not words,
installs
nothing, and the module then takes no command and no bot_state query; the log says `Bridge ability catalogue
refused` (error). LC3b: an index whose catalogue cannot be sent (text that is not UTF-8, or over 24576 encoded
bytes) installs without it: commands and bot_state queries stand, the catalogue query answers `unavailable`, and
the log says `Bridge ability catalogue not sent` (error). Otherwise the log says `Bridge ability catalogue:
N rows, hash H, B bytes.` (info). A `catalogue` query carries observable_group's exact keys and is route-bound
like every query. Its result is the envelope plus `catalogue`: `hash` (FNV-1a 64 of playerbots' `CatalogueText`
as 16 lowercase hex digits, compared as a string, never recomputed), `version` (the catalogue format, 1),
`abilities` (what this module takes beyond the old wire, `["catalogue", "names", "steps"]` today; not hashed), `maxSteps` and
`rows`. Each row has kind (act or topic), name, sentence, params (name, kind, sentence, choices), refusals (word,
sentence), effect (`none` for every row since LC.36), done, state, event and limit. The answer is built once at install and stays under 24 KB of
the 32768-byte response limit (selftested; 15187 bytes for the 29 rows of LC3); a later step pages it when the
rows outgrow that. Without an installed index the query fails `unavailable`; a module older than LC3 answers it
`invalid_input`. An order that names a unit or quest the bot cannot find is refused `unknown_name` ("I do not know
what you mean by that.").

LC3b, the stepped command (abilities `names` and `steps`): a `command` with `steps`. `steps` holds 1..`maxSteps` (3) entries, each exactly
`{"act": <an act row's name>, "params": {...}}` with exactly that row's parameters by their catalogue names: a
`unit` parameter as `<name>` (the unit's name) or `<name>Guid` (a guid string), a `quest` parameter as `<name>`
(its title) or `<name>Id` (1..4294967295), exactly one of the two; a `place` parameter as a place name (go_to,
meet_at) or, for `position`, the spot `"x,y,z,map"` (three finite decimal numbers and a map id, nothing else); a
`choice` parameter as one of the row's choice words (an icon by its word, `star` .. `skull`, not 0-7: one
catalogue-driven rule); a `flag` as a boolean. Names and places are 1..64 bytes without control characters. A
topic, a word without an act row (regroup, pull, assign_role, dungeon_clear), a missing or an extra key
are `invalid_input`. The issuer, bot and group are the scope's for every step. The order takes one command id,
one rate slot and one authorization: the module builds one playerbots order per step and calls the span
`Execute` once; playerbots' gate resolves each name among what the bot perceives or knows, checks visibility,
group and the row's check per step (for `position` a reachable path), and the
first step not applied ends the order. A lasting step runs like any other. The result keeps the old
shape: `outcome` is applied only when every step applied, else the ending step's (an unavailable step is
`rejected` with `unavailable`); `reason`/`reasonText` are the ending step's, in the catalogue's words, or the
order's own when it was refused before any step ran (`too_soon`, `unavailable`, `cannot_now`). It adds
`steps`: one entry per submitted step in order, `{"act", "outcome": applied/rejected/failed/skipped, "reason",
"reasonText"}`, the reason only on rejected and failed; a step that did not run is `skipped`, so a fully refused
order lists every step skipped and carries its own reason.

The single world owner calls `Bind` only AFTER the existing activation owner has selected a fresh epoch and proved
actual old-work drain. Bind itself does not prove it. Every bind retires the prior helper
state immediately; invalid binding stays inactive. Reconnect requires a fresh epoch and fresh registered scope/work;
it cannot revive terminal/failed work. Only the previous binding's epoch is refused (LC.35: no longer history is
kept); this is not an RNG: the activation owner supplies 128-bit cryptographically fresh epochs on process restart.
Do not destroy/recreate this helper under a still-active epoch to reset replay state.

The ledger keeps no scope table (LCB6b). `OpenGeneration` checks the trusted scope against the bound identity
(instance, provider, epoch, guids); `Current` compares only the bot, the controller and the context fields. Whether a
conversation is still current (its liveness, group and membership revisions, perception, a logout) has one owner,
`WorldPolicy`, asked by `WorldChannel` once before each read or effect and once after the executor. `current`
arguments are fresh world captures, never native JSON.

Generations must be registered by the game. Inbound queries may create a new request reservation only under a live
generation route, against an already registered current scope and matching controller/bot. Queries share the
request-id replay namespace but can never authorize commands. Commands need an open generation request and matching
controller/bot/deadline. The route ends by the game (`Close`/`Cancel`) or at its deadline; an inbound
`generation.result` or `failure` (like any other type) is `invalid_input` and closes nothing (LCB6a). A
command's result is terminal once and is reported only while its route is open: `WorldChannel` finishes every
command in the tick that received it, and the route's terminal drops its ids (LC.35). `Current(...,
requireOpen=true)` rejects closed requests. The ledger itself returns only its error word for duplicate, stale or
malformed input and clears output bodies before refused operations; `WorldChannel` answers that word to the caller.

The ledger holds live routes only (LC.35). A generation route keeps its own id and the query and command ids opened
under it, at most 64 of those per route, until its terminal (the game's Close or failure), its
deadline (at most 120000 ms) or `WorldChannel::Close`; then all of them are dropped together. Within a live route a
repeated id stays refused (duplicate, or closed for a finished query); once the route is gone every one of its ids,
a command id included, finds no route and is refused as `unknown_request`, so a late replay can never land on the
dropped route. A command or watch message names its route in its envelope and is admitted through that route only,
so it cannot be replayed through another one; `FinishCommand`/`FinishWatch` answer an id only under the route that
received it. What the ledger no longer refuses is a new message on another live route that reuses a dropped route's
query or command id: it is a new request of that route (the hub bridge derives every id from its route's request
id, so it never does this). Generation ids come from the world policy's counter and are never reissued in a
process, and `WorldChannel::Register` refuses a ticket whose route is still bound; nothing else may open a route a
second time, because a reopened route would not remember the command ids it consumed. Pending requests are counted, not capped
(LC.38); the per-route 64 ids are the ledger's only count limit and can only be reduced in the constructor. Retiring clears maps because the old epoch is
unusable. These limits DO NOT account physical receive/worker/completion payload slots:
J5b must hold a bounded slot until actual disposal, independently of logical terminal/cancel/expiry and worker drain.
Reservation expiry/invalidations run on explicit work/scope operations; do not install an all-reservations scan
on every game tick. The helper has no autonomous timer or background thread.

Offline proof compiles this exact production cpp with `tests/bridge-channel-selftest.cpp`. It covers authentication,
strict decode, epoch retirement/reconnect, exact expiry/rollback, changed bot/controller/context, terminal/replay
state, typed command staging, bounded query shapes, route drop and 200 open routes without a cap. It is no full module/world/native
proof.


## J5c filtered observations and runtime integration

`mod-ollama-chat_bridge_query.{h,cpp}` supplies production capture/revalidation logic against a synchronous,
world-only `QueryWorldSource`. The source must re-resolve GUIDs and actual group, map, instance, phase,
distance and detection on every call. Native JSON cannot implement this source or supply its observations.
The helper independently requires the exact current game `WorldTicket` and `DispatchGuard` before and after
capture. No request-created world authority, role inference, private AI state or selected-target access exists.
Only guid/name and observed UI health are emitted; role and visibleTargetGuid stay omitted until a separately
reviewed observable provenance source exists. Missing/hidden group units are omitted. Malformed or more than 40
roster entries fail closed; a non-group scope returns unavailable, not invented party members.

`FilteredQueryAdapter::Units` reads the units in the same world tick, right after the channel's one currentness
check (LCB6b); it checks nothing again, so a query packet costs one `WorldPolicy::Current` call (`Checks()` counts
them). Never queue the source, a game pointer, or a world-reading closure to a worker. Observer callbacks stay
synchronous and bounded on the world thread.

## The transport (LCB6b)

Every packet is answered; none is left to time out. `Receive` answers at once `inactive` (no bound epoch),
`too_large`, `closed` (the route is not open) or `overload` (429: all 64 packet slots taken, one waiting listener
thread each). A queued packet is answered by `Update` (8 per tick) with its result or its word: `closed` when the
route closed before or during its turn (`Close` answers the route's queued packets at once), `stale` when it is
older than 5 s, its binding lapsed, or the world check before the read or effect or after the executor fails, the
ledger's word for a refused envelope. `Retire` answers every queued packet `inactive`; stopping the listener answers
every waiting handler `inactive`. The handler waits on its answer's signal (capped at 5 s), not on a poll. A command
outcome that is not published counts as `withheld`. The listener logs a warning at the 1st, 2nd, 4th, ... overload.
The channel accepts only query, command and watch. Its tables (world scopes, bindings, watches, command intervals)
hold one entry per bot in talk and have no fixed count (LC.38).

The standalone `tests/bridge-query-selftest.cpp` links the actual helper, world policy, channel policy and physical
mailbox with controlled current world objects. It checks visibility/missing units, omitted provenance, real query
encoding, currentness/expiry/reconnect and returned packet lifetime. Its caller
illustrates the handoff, but is not production dispatcher wiring, real game-object adapter, native socket/tool
integration or live J9C/X acceptance.

`tests/bridge-query-dispatch-selftest.cpp` additionally links the production bridge/query/world/channel code and
compiles the world adapter and dispatcher units whole against the controlled game-object doubles of `tests/doubles`
and its own. It covers the runtime query joins and final publication checks offline. It does not prove a full
module build or live game delivery. The full J5 parent remains open pending independent review; native J11C/X
adapters, J6 effects and live J9C/X delivery remain later work.

`tests/bridge-listener-selftest.cpp` (J5d) links the real listener and its cpp-httplib against a counting fake mailbox
and against the production `WorldChannel`, with the test thread playing the world tick: token, bind address,
malformed and oversize input, thread handoff, deadline, shutdown and configuration. Live calls are J9C/X work.
