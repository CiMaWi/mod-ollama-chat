#include "mod-ollama-chat_bridge_world.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "mod-ollama-chat_bridge_json.h"
#include "mod-ollama-chat_bridge_query.h"

namespace OllamaBridge
{
namespace
{
Deadline ChannelDeadline(Deadline captured, Clock now)
{
    // Separate clock reads may cross a millisecond boundary. Tighten the monotonic cap to the
    // shorter captured remainder; never renew either clock or change the captured wire deadline.
    captured.steadyMs = now.steadyMs + RemainingMs(captured, now);
    return captured;
}
}  // namespace

WorldChannel::WorldChannel(WorldPolicy& world, Observer observer, ReadClock clock, QueryWorldSource* source,
                           CommandExecutor* executor)
    : _world(world), _source(source), _executor(executor), _observer(std::move(observer)), _clock(std::move(clock))
{
}

void ChannelAnswer::Set(std::unique_ptr<ChannelPacket> packet)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_set)
            return;  // The first answer wins; this packet is disposed of here.
        _set = true;
        _packet = std::move(packet);
    }
    _ready.notify_one();
}

bool ChannelAnswer::Wait(std::chrono::steady_clock::time_point until, std::unique_ptr<ChannelPacket>& packet)
{
    std::unique_lock<std::mutex> lock(_mutex);
    if (!_ready.wait_until(lock, until, [this] { return _set; }))
        return false;
    packet = std::move(_packet);
    return true;
}

void WorldChannel::Answer(std::unique_ptr<ChannelPacket> packet, ChannelError error)
{
    // LCB6b: every packet the mailbox took ends here, with its result or the word it was discarded with.
    if (error != ChannelError::None)
        packet->body.clear();
    packet->error = error;
    if (auto const answer = packet->_answer.lock())
        answer->Set(std::move(packet));
}

void WorldChannel::Forget(std::string const& route, ChannelError error)
{
    std::vector<std::unique_ptr<ChannelPacket>> queued;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _open.erase(route);
        for (auto it = _receive.begin(); it != _receive.end();)
            if ((*it)->_route == route)
            {
                queued.push_back(std::move(*it));
                it = _receive.erase(it);
            }
            else
                ++it;
    }
    for (auto& packet : queued)
        Answer(std::move(packet), error);
}

void WorldChannel::Retire()
{
    _policy.Retire();
    _selected = {};
    _bindings.clear();
    _watches.Retire();  // J6e-c: module watches end with their epoch (counted dropped).
    std::deque<std::unique_ptr<ChannelPacket>> queued;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _bound = false;
        _open.clear();
        queued.swap(_receive);
    }
    for (auto& packet : queued)
        Answer(std::move(packet), ChannelError::Inactive);  // LCB6b: answered, never left to time out.
}

void WorldChannel::Configure(bool enabled)
{
    Retire();
    _enabled = enabled;
}

bool WorldChannel::Activate()
{
    auto const selected = _world.Selected();
    if (!_enabled || _world.Retiring() || selected.epoch.empty())
        return false;
    if (_selected == selected)
        return true;
    if (!Drained() || !_policy.Bind(selected))
        return false;
    _selected = selected;
    std::lock_guard<std::mutex> lock(_mutex);
    _bound = true;
    return true;
}

bool WorldChannel::Live(Binding const& binding) const
{
    // The adapter's capture may erase its binding and invalidate the scope, so WorldPolicy is asked after it.
    DispatchGuard current;
    return _enabled && _selected == _world.Selected() && _observer && _observer(binding.ticket, current) &&
           _world.Current(binding.ticket, _clock()) &&
           DispatchGuardCurrent(binding.ticket, binding.guard, current, _clock());
}

bool WorldChannel::Register(WorldTicket const& ticket, DispatchGuard const& guard)
{
    auto const now = _clock();
    Binding binding{ticket, guard};
    if (!Live(binding))
        return false;
    // Explicit admission work only; no all-request sweep is installed on the world tick. A lapsed binding's route
    // leaves the replay ledger with it (LC.35), and its queued packets are answered stale (LCB6b).
    std::vector<std::string> lapsed;
    std::erase_if(_bindings,
                  [&](auto const& pair)
                  {
                      if (_world.Current(pair.second.ticket, now))
                          return false;
                      _policy.Drop(pair.first);
                      lapsed.push_back(pair.first);
                      return true;
                  });
    for (auto const& route : lapsed)
        Forget(route, ChannelError::Stale);
    // A bound route registers once: the ledger forgets a route at its terminal (LC.35), so a second OpenGeneration
    // would reopen it without the command ids it already consumed.
    if (_bindings.contains(ticket.request.requestId) ||
        _policy.OpenGeneration(ticket.scope, ticket.request.requestId, ChannelDeadline(ticket.request.deadline, now),
                               now) != ChannelError::None)
        return false;
    _bindings.emplace(ticket.request.requestId, std::move(binding));
    std::lock_guard<std::mutex> lock(_mutex);
    _open.insert(ticket.request.requestId);
    return true;
}

bool WorldChannel::Close(WorldTicket const& ticket)
{
    auto it = _bindings.find(ticket.request.requestId);
    if (it == _bindings.end())
        return false;
    bool const acted = it->second.acted;
    _policy.Drop(ticket.request.requestId);  // LC.35: also a route whose scope or deadline already lapsed.
    _bindings.erase(it);
    Forget(ticket.request.requestId, ChannelError::Closed);  // LCB6b: its waiting tool calls hear closed at once.
    return acted;
}

ChannelError WorldChannel::Receive(std::string_view route, std::string_view body,
                                   std::shared_ptr<ChannelAnswer> const& answer)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_bound)
        return ChannelError::Inactive;
    if (body.size() > MaxRequestBytes || route.size() > 128)
        return ChannelError::TooLarge;
    if (route.empty() || !answer)
        return ChannelError::Invalid;
    // LCB6b: a route with no open generation (closed, lapsed or never registered) is answered at once.
    if (!_open.contains(std::string(route)))
        return ChannelError::Closed;
    if (Used() >= MaxPackets)
        return ChannelError::Capacity;  // A real bound (one listener thread per packet): answered overload, logged.
    auto packet = std::make_unique<ChannelPacket>();
    packet->_lease = std::make_unique<ChannelPacket::Lease>(_used);
    packet->_until = CaptureDeadline(_clock(), WorldLivenessMs);
    packet->_route.assign(route);
    packet->_answer = answer;
    packet->body.assign(body);
    _receive.push_back(std::move(packet));
    return ChannelError::None;
}

ChannelError WorldChannel::Finish(ChannelEnvelope const& envelope, std::string const& commandId, ChannelOutcome outcome,
                                  WorldScope const& scope, Clock now, std::string& body, CommandRefusal refusal,
                                  std::span<ChannelStep const> steps, std::span<ChannelStepResult const> ran)
{
    ++(outcome == ChannelOutcome::Applied  ? _stats.applied
       : outcome == ChannelOutcome::Failed ? _stats.failed
                                           : _stats.refused);
    return _policy.FinishCommand(envelope, commandId, outcome, scope, now, body, refusal, steps, ran);
}

ChannelError WorldChannel::Command(Binding const& binding, ChannelEnvelope const& envelope,
                                   ChannelCommand const& command, EffectPhase const* phase, Deadline until,
                                   bool& attempted, std::string& body)
{
    // Receive already consumed this command id in the replay ledger; no path below can run it twice.
    attempted = false;
    ++_stats.proposed;
    // DS.3 fixed refusal before any game read: no executor or world phase, or rate denial. J5e: each refusal names
    // its reason (the rate gate is consulted only when the other passes). LC3: the acts are catalogue rows, each with
    // its effect (Receive takes no other), and the rate gate admits the whole order once: one slot, one
    // authorization, however many steps it holds.
    CommandRefusal const fixed = !_executor || !phase                                   ? CommandRefusal::Unavailable
                                 : !_rate.Admit(binding.ticket.scope.botGuid, _clock()) ? CommandRefusal::TooSoon
                                                                                        : CommandRefusal::None;
    if (fixed != CommandRefusal::None)
        return Finish(envelope, command.commandId, ChannelOutcome::Rejected, binding.ticket.scope, _clock(), body,
                      fixed, command.steps);
    Binding const bound = binding;  // Executor game code must not depend on map storage it could disturb.
    bool asked = false;
    std::optional<Clock> authorized;
    // LCB6b: Update checked the conversation just before this call (Live) and checks it again after the executor
    // (ReceiptCurrent); between them only this world-thread call runs. The one authorization keeps the deadline.
    std::function<bool()> const authorize = [&]() -> bool
    {
        if (std::exchange(asked, true))
            return false;  // One authorization per command.
        auto const now = _clock();
        if (Expired(until, now))
            return false;
        authorized = now;
        return true;
    };
    ++_stats.attempts;
    auto const result = _executor->Execute(*phase, bound.ticket, bound.guard, command, authorize);
    auto const ran = command.steps.empty() ? std::vector<ChannelStepResult>() : _executor->Steps();
    // LC3: an earlier step of a stepped order may have written although a later one ended it.
    attempted = authorized && (result == EffectResult::Applied || result == EffectResult::Failed ||
                               std::any_of(ran.begin(), ran.end(), [](ChannelStepResult const& step)
                                           { return step.outcome != ChannelOutcome::Rejected; }));
    // The outcome is recorded once at its authorization time; receipt exposure is joined separately afterwards.
    auto const outcome = EffectOutcome(result, authorized.has_value());
    return Finish(envelope, command.commandId, outcome, bound.ticket.scope, authorized.value_or(_clock()), body,
                  outcome == ChannelOutcome::Rejected ? _executor->Refusal() : CommandRefusal::None, command.steps,
                  ran);
}

ChannelError WorldChannel::Arm(Binding const& binding, ChannelEnvelope const& envelope, ChannelWatch const& watch,
                               EffectPhase const* phase, bool& armed, std::string& body)
{
    // Receive already consumed this watch id in the replay ledger; no path below runs it twice.
    armed = false;
    auto const& ticket = binding.ticket;
    auto const now = _clock();
    if (watch.condition == WatchCondition::None)
    {
        _watches.Clear(ticket.scope.botGuid);
        return _policy.FinishWatch(envelope, watch.commandId, WatchOutcome::Cleared, ticket.scope, now, body);
    }
    // The bot's own facts, read now with the post-map-join phase (J5f reader); unreadable counts as not ready.
    WatchFacts facts;
    if (!phase || !_source || !_source->ReadWatch(ticket.scope.botGuid, ticket.scope.controllerGuid, facts))
        facts = {};
    Watch entry;
    entry.id = watch.commandId;
    entry.botGuid = ticket.scope.botGuid;
    entry.partnerGuid = ticket.scope.controllerGuid;
    entry.source = binding.guard.source;
    entry.channel = ticket.scope.channel;
    entry.contextId = ticket.scope.contextId;
    entry.condition = watch.condition;
    WatchRoute const route{ticket.request.input.wakeDepth, ticket.request.input.originUnixMs};
    auto const refusal = _watches.Arm(std::move(entry), route, facts, watch.untilUnixMs, now);
    armed = refusal == CommandRefusal::None;
    return _policy.FinishWatch(envelope, watch.commandId, armed ? WatchOutcome::Armed : WatchOutcome::Rejected,
                               ticket.scope, now, body, refusal);
}

ChannelError WorldChannel::Store(Binding const& binding, ChannelEnvelope const& envelope, ChannelStore const& store,
                                 EffectPhase const* phase, std::string& body)
{
    // Receive already consumed this store id in the replay ledger; no path below writes twice. The checks and the
    // write run here on the world thread, right after Live.
    StoreWorld* const world = phase && _source ? _source->Store() : nullptr;
    auto const reasons =
        world ? ApplyStore(store, *world) : std::vector<std::string>(store.rows.size(), std::string("unavailable"));
    return _policy.FinishStore(envelope, store, reasons, binding.ticket.scope, _clock(), body);
}

bool WorldChannel::ReceiptCurrent(WorldTicket const& ticket) const
{
    // An attempted effect may change facts Live checks (target, combat, marks). Its receipt needs only the selected
    // epoch and the current ticket; a stale one is withheld.
    return _enabled && _selected == _world.Selected() && _world.Current(ticket, _clock());
}

ChannelError WorldChannel::Read(Binding const& binding, ChannelEnvelope const& envelope, ChannelQuery const& query,
                                std::string& body)
{
    // The read follows the check at once on the world thread, so nothing it names can change before it is sent.
    auto const& ticket = binding.ticket;
    auto const unavailable = [&]
    { return _policy.FinishFailure(envelope, ChannelFailure::Unavailable, ticket.scope, _clock(), body); };
    // LC3: plain data installed at startup; without an index, or with a catalogue it could not send, the module has
    // no catalogue.
    if (query.kind == QueryKind::Catalogue)
        return CatalogueInstalled() ? _policy.FinishCatalogue(envelope, ticket.scope, _clock(), body) : unavailable();
    // J5f-b: the bot's own state, read now on the world thread.
    if (query.kind == QueryKind::BotState)
    {
        std::vector<StateEntry> entries;
        return _source && _source->ReadTopic(ticket, query.topic, query.place, entries)
                   ? _policy.FinishTopic(envelope, ticket.scope, query.topic, entries, _clock(), body)
                   : unavailable();
    }
    std::vector<Participant> units;
    return _source && FilteredQueryAdapter(*_source).Units(ticket, binding.guard, units)
               ? _policy.FinishQuery(envelope, ticket.scope, units, _clock(), body)
               : unavailable();
}

ChannelError WorldChannel::Handle(ChannelPacket& packet, EffectPhase const* phase)
{
    auto const it = _bindings.find(packet._route);
    if (it == _bindings.end())
        return ChannelError::Closed;  // Close answers its queued packets itself; this one was in hand.
    if (Expired(packet._until, _clock()))
        return ChannelError::Stale;
    ChannelMessage message;
    ChannelError error = ChannelError::None;
    {
        std::string const request = std::move(packet.body);  // Disposed of before any answer is published.
        packet.body.clear();
        error = _policy.Receive(request, it->second.ticket.scope, _clock(), message, packet._route);
    }
    if (error == ChannelError::Duplicate)
        ++_stats.duplicates;
    if (error != ChannelError::None)
        return error;
    // LCB6b: the one check before the read or effect.
    if (!Live(it->second))
        return ChannelError::Stale;
    if (auto const* query = std::get_if<ChannelQuery>(&message.payload))
        return Read(it->second, message.envelope, *query, packet.body);
    if (auto const* store = std::get_if<ChannelStore>(&message.payload))
    {
        // LC20b: a write of the bots' past, no effect in the world (DS.14's acted stays); its receipt after the write
        // needs the conversation current, as a command's.
        error = Store(it->second, message.envelope, *store, phase, packet.body);
        auto const route = _bindings.find(packet._route);
        if (route == _bindings.end())
            return ChannelError::Closed;
        return error == ChannelError::None && !ReceiptCurrent(route->second.ticket) ? ChannelError::Stale : error;
    }
    bool acted = false;  // An authorized effect attempt, or an armed watch: a promise to act later (DS.14, J6e-c).
    bool const command = std::holds_alternative<ChannelCommand>(message.payload);
    error = command
                ? Command(it->second, message.envelope, std::get<ChannelCommand>(message.payload), phase, packet._until,
                          acted, packet.body)
                : Arm(it->second, message.envelope, std::get<ChannelWatch>(message.payload), phase, acted, packet.body);
    // The executor ran game code: resolve the route again instead of trusting the earlier map reference.
    auto const route = _bindings.find(packet._route);
    if (route != _bindings.end() && acted)
        route->second.acted = true;  // DS.14: the game did something on this route; a silent answer may stand.
    // LCB6b: the one check after the executor. A route closed or retired meanwhile is answered closed; a decided
    // command outcome that is not published is counted withheld.
    if (route == _bindings.end())
        error = ChannelError::Closed;
    else if (error == ChannelError::None && !ReceiptCurrent(route->second.ticket))
        error = ChannelError::Stale;
    if (error != ChannelError::None && command)
        ++_stats.withheld;
    return error;
}

std::size_t WorldChannel::Update(EffectPhase const* phase)
{
    if (!_enabled || !Activate())
        return 0;
    std::size_t processed = 0;
    while (processed < TickBudget)
    {
        std::unique_ptr<ChannelPacket> packet;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_receive.empty())
                break;
            packet = std::move(_receive.front());
            _receive.pop_front();
        }
        ++processed;
        auto const error = Handle(*packet, phase);  // No lock is held while the executor runs.
        Answer(std::move(packet), error);
    }
    return processed;
}

std::atomic<std::size_t> DispatchSlot::_used{0};

DispatchSlot::~DispatchSlot() { Release(); }
DispatchSlot::DispatchSlot(DispatchSlot&& other) noexcept : _held(std::exchange(other._held, false)) {}

DispatchSlot& DispatchSlot::operator=(DispatchSlot&& other) noexcept
{
    if (this != &other)
    {
        Release();
        _held = std::exchange(other._held, false);
    }
    return *this;
}

void DispatchSlot::Release()
{
    if (std::exchange(_held, false))
        --_used;
}

DispatchSlot DispatchSlot::Acquire()
{
    DispatchSlot slot;
    ++_used;
    slot._held = true;
    return slot;
}

std::size_t DispatchSlot::Used() { return _used.load(); }

bool DispatchGuardCurrent(WorldTicket const& ticket, DispatchGuard const& captured, DispatchGuard const& current,
                          Clock now)
{
    if (!ticket.generation || Expired(ticket.request.deadline, now) || !(ticket.scope == captured.scope) ||
        !(captured.scope == current.scope) || !current.humanSpeaker || !current.authority || !current.perceptible ||
        !current.audienceOnline || !captured.humanSpeaker || !captured.authority || !captured.perceptible ||
        !captured.audienceOnline || current.source == DispatchSource::Unsupported ||
        current.source != captured.source || !current.speakerGuid || current.speakerGuid != captured.speakerGuid ||
        current.speakerGuid != ticket.request.input.speakerGuid || current.groupGuid != captured.groupGuid ||
        current.mapId != captured.mapId || current.instanceId != captured.instanceId || !current.phaseMask ||
        current.phaseMask != captured.phaseMask || current.audience != captured.audience || current.audience.empty() ||
        current.audience.size() > 40 || !std::is_sorted(current.audience.begin(), current.audience.end()) ||
        current.audience.front() == 0 ||
        std::adjacent_find(current.audience.begin(), current.audience.end()) != current.audience.end() ||
        !std::binary_search(current.audience.begin(), current.audience.end(), current.speakerGuid) ||
        !std::binary_search(current.audience.begin(), current.audience.end(), ticket.request.botGuid))
        return false;
    if (current.source == DispatchSource::Party)
        return current.groupGuid && current.nonRaid && captured.nonRaid && ticket.scope.channel == "party";
    return ticket.scope.channel == "direct";
}

bool SameScopeAndAudience(BindingCapture const& bound, BindingCapture const& current)
{
    auto const& was = bound.guard;
    auto const& now = current.guard;
    return was.scope.instance == now.scope.instance && was.scope.provider == now.scope.provider &&
           was.scope.epoch == now.scope.epoch && was.scope.botGuid == now.scope.botGuid &&
           was.scope.controllerGuid == now.scope.controllerGuid && was.scope.channel == now.scope.channel &&
           was.scope.contextId == now.scope.contextId && was.source == now.source &&
           was.speakerGuid == now.speakerGuid &&
           (was.source != DispatchSource::Party || was.groupGuid == now.groupGuid) && was.audience == now.audience;
}

TerminalAction DispatchTerminal(WorldOutcome outcome, bool addressedHuman)
{
    switch (outcome)
    {
        case WorldOutcome::Success:
            return TerminalAction::Reply;
        case WorldOutcome::Silent:
            return TerminalAction::Silence;  // DS.14: the act was the answer; no line and no outage.
        case WorldOutcome::Unavailable:
        case WorldOutcome::Empty:
        case WorldOutcome::Filtered:
        case WorldOutcome::Malformed:
        case WorldOutcome::Timeout:
        case WorldOutcome::QueueFull:
            return addressedHuman ? TerminalAction::Outage : TerminalAction::Silence;
        default:
            return TerminalAction::Silence;
    }
}

char const* DispatchOutageText() { return "Give me a moment to gather my thoughts."; }

bool WorldScope::operator==(WorldScope const& other) const
{
    return version == other.version && instance == other.instance && provider == other.provider &&
           epoch == other.epoch && botGuid == other.botGuid && controllerGuid == other.controllerGuid &&
           contextId == other.contextId && channel == other.channel && perceptionScope == other.perceptionScope &&
           contextRevision == other.contextRevision && membershipRevision == other.membershipRevision;
}

void WorldPolicy::Retire()
{
    _selected = {};
    _scopes.clear();
    _pending.clear();
    _retiring = true;
}

void WorldPolicy::Configured(bool enabled, Settings settings, std::string epoch)
{
    Retire();
    _enabled = false;
    _desired = {};
    _settings = {};
    if (_generation == std::numeric_limits<uint64_t>::max())
        return;
    ++_generation;
    if (!enabled || !ValidSettings(settings) || !ValidIdentifier(epoch))
        return;
    _settings = std::move(settings);
    _desired = {_settings.instance, _settings.provider, _settings.endpoint, std::move(epoch)};
    _enabled = true;
}

void WorldPolicy::Shutdown() { Configured(false, {}, {}); }

bool WorldPolicy::ObserveDrain(bool drained)
{
    if (!_retiring || !drained)
        return false;
    _retiring = false;
    if (_enabled)
        _selected = _desired;
    return !_selected.epoch.empty();
}

bool WorldPolicy::ObserveScope(WorldScope scope, Clock now)
{
    if (_selected.epoch.empty() || scope.version != 1 || scope.instance != _selected.instance ||
        scope.provider != _selected.provider || scope.epoch != _selected.epoch || !scope.botGuid ||
        !scope.controllerGuid || (scope.channel != "direct" && scope.channel != "party") ||
        !ValidIdentifier(scope.contextId) || !ValidIdentifier(scope.perceptionScope) ||
        !ValidIdentifier(scope.contextRevision) || !ValidIdentifier(scope.membershipRevision))
        return false;
    Deadline const liveUntil = CaptureDeadline(now, WorldLivenessMs);
    if (Expired(liveUntil, now))
        return false;
    // LCB6b: no full Expire per heartbeat (the adapter observes every binding on every tick). A heartbeat after a gap
    // must not resurrect requests bound to the lapsed observation: a lapsed or changed entry invalidates its work
    // before the fresh one is taken.
    auto const [first, last] = _scopes.equal_range(scope.botGuid);
    auto const it =
        std::find_if(first, last, [&](auto const& entry) { return entry.second.scope.contextId == scope.contextId; });
    if (it != last && it->second.scope == scope && !Expired(it->second.liveUntil, now))
    {
        it->second.liveUntil = liveUntil;
        return true;
    }
    if (it != last)
        InvalidateScope(scope.botGuid, scope.contextId);
    uint64_t const bot = scope.botGuid;
    _scopes.emplace(bot, ScopeEntry{std::move(scope), liveUntil});
    return true;
}

void WorldPolicy::InvalidateScope(uint64_t botGuid, std::string const& contextId)
{
    auto [first, last] = _scopes.equal_range(botGuid);
    while (first != last)
        first = first->second.scope.contextId == contextId ? _scopes.erase(first) : std::next(first);
    std::erase_if(
        _pending, [&](auto const& entry)
        { return entry.second.ticket.scope.botGuid == botGuid && entry.second.ticket.scope.contextId == contextId; });
}

bool WorldPolicy::ScopeCurrent(WorldScope const& scope, Clock now) const
{
    if (_selected.epoch.empty())
        return false;
    auto const [first, last] = _scopes.equal_range(scope.botGuid);
    return std::any_of(first, last, [&](auto const& entry)
                       { return entry.second.scope == scope && !Expired(entry.second.liveUntil, now); });
}

void WorldPolicy::Expire(Clock now)
{
    std::erase_if(_scopes, [&](auto const& entry) { return Expired(entry.second.liveUntil, now); });
    std::erase_if(_pending,
                  [&](auto const& entry)
                  {
                      return Expired(entry.second.ticket.request.deadline, now) ||
                             !ScopeCurrent(entry.second.ticket.scope, now);
                  });
}

WorldTicket WorldPolicy::Reserve(WorldScope const& scope, Context input, Clock now, int64_t lifetimeMs)
{
    Expire(now);
    if (!ScopeCurrent(scope, now) || _sequence == std::numeric_limits<uint64_t>::max())
        return {};
    Deadline const deadline = CaptureDeadline(now, lifetimeMs);
    if (Expired(deadline, now) || input.channel != scope.channel)
        return {};
    WorldTicket ticket;
    ticket.generation = _generation;
    ticket.scope = scope;
    ticket.request = {_selected.epoch,
                      _selected.epoch + "-" + std::to_string(++_sequence),
                      deadline,
                      scope.controllerGuid,
                      scope.botGuid,
                      std::move(input),
                      scope};
    // Reuse the actual builder's bounds/encoding validation before allocating a reservation.
    std::string body;
    if (!Build(_settings, ticket.request, now, body))
        return {};
    _pending.emplace(ticket.request.requestId, PendingEntry{ticket, Stage::Reserved});
    return ticket;
}

bool WorldPolicy::Current(WorldTicket const& ticket, Clock now) const
{
    ++_checks;
    auto const it = _pending.find(ticket.request.requestId);
    return ticket.generation == _generation && ticket.request.epoch == _selected.epoch &&
           ticket.request.scope == ticket.scope && !Expired(ticket.request.deadline, now) &&
           ScopeCurrent(ticket.scope, now) && it != _pending.end() &&
           it->second.ticket.generation == ticket.generation && it->second.ticket.scope == ticket.scope &&
           it->second.ticket.request.deadline.unixMs == ticket.request.deadline.unixMs &&
           it->second.ticket.request.deadline.steadyMs == ticket.request.deadline.steadyMs &&
           it->second.ticket.request.botGuid == ticket.request.botGuid &&
           it->second.ticket.request.controllerGuid == ticket.request.controllerGuid;
}

bool WorldPolicy::PendingFor(WorldScope const& scope, Clock now) const
{
    return ScopeCurrent(scope, now) && std::any_of(_pending.begin(), _pending.end(),
                                                   [&](auto const& entry)
                                                   {
                                                       auto const& ticket = entry.second.ticket;
                                                       return ticket.generation == _generation &&
                                                              ticket.scope == scope &&
                                                              !Expired(ticket.request.deadline, now);
                                                   });
}

bool WorldPolicy::MarkQueued(WorldTicket const& ticket, Clock now)
{
    if (!Current(ticket, now))
        return false;
    auto& entry = _pending.at(ticket.request.requestId);
    if (entry.stage != Stage::Reserved)
        return false;
    entry.stage = Stage::Queued;
    return true;
}

bool WorldPolicy::Complete(WorldTicket const& ticket, WorldOutcome outcome, Clock now)
{
    if (!Current(ticket, now))
        return false;
    auto& entry = _pending.at(ticket.request.requestId);
    if (entry.stage != Stage::Queued && !(entry.stage == Stage::Reserved && outcome == WorldOutcome::QueueFull))
        return false;
    if (outcome == WorldOutcome::Cancelled)
    {
        Cancel(ticket);
        return false;
    }
    entry.stage = Stage::Completed;
    return true;  // Caller may stage one reply/fallback; this grants no game effect.
}

bool WorldPolicy::Consume(WorldTicket const& ticket, Clock now)
{
    if (!Current(ticket, now) || _pending.at(ticket.request.requestId).stage != Stage::Completed)
        return false;
    _pending.at(ticket.request.requestId).stage = Stage::Consumed;
    return true;  // Retain scope freshness until caller finishes all effects and cancels this claim.
}

void WorldPolicy::Cancel(WorldTicket const& ticket)
{
    if (ticket.generation == _generation)
        _pending.erase(ticket.request.requestId);
}

}  // namespace OllamaBridge
