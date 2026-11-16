// Parked code of the retired Codex bridge's world observer (CS.20, LC.35). Not compiled, not tested.
// Cut verbatim from src/mod-ollama-chat_dispatch.cpp at b81bf8c by LCB0.

// Globals:
OllamaBridge::WorldObserver g_nativeObserver(OllamaHttp_PostObserver);
OllamaBridge::WorldPolicy g_bridgeWorld(&g_nativeObserver);
bool g_nativeSelected = false;  // World-thread cached selection, including fail-closed invalid settings.
OllamaBridge::Settings g_nativeSettings;

// Guards: the channel lambda, the worker's generation check, the delivery check and the wake submit each also
// required, for provider "codex" with observerEnabled, g_nativeObserver.Current(scope, now).
// OllamaDispatch_Start: if (g_nativeSelected) g_nativeObserver.Start();
// OllamaDispatch_Stop: g_nativeSelected = false; g_nativeSettings = {}; g_nativeObserver.Stop();
// The tick: UpdateNativeObserver() before Expire; g_nativeObserver.Publish(scopes, drained, now) after Refresh;
// generic ObserveDrain/ActivateBridge only when !g_nativeSelected.

// OllamaDispatch_ChannelEvents:
bool OllamaDispatch_ChannelEvents(OllamaBridge::DispatchGuard const& guard, std::vector<uint8_t> const& icons)
{
    // J6e-c: only the native (Codex observer) adapter takes event routes. The generic 127.0.0.1 port has no events
    // endpoint, so an event route there held a packet slot until retirement (64 mark changes in an epoch refused every
    // later query and command with capacity) and a world reservation of the binding's scope for nothing.
    if (!g_nativeSelected || g_eventPublications.size() >= OllamaBridge::WorldChannel::MaxPackets)
        return false;
    OllamaBridge::EventPublication publication;
    if (!g_bridgeChannel.ObserveEvents(guard, icons, publication))
        return false;
    g_eventPublications.push_back(std::move(publication));
    return true;
}

// OllamaDispatch_BridgeConfigured, after the snapshot:
    g_nativeSelected = g_Enable && settings.provider == OllamaProvider::Bridge && settings.bridge.provider == "codex" &&
                       settings.bridge.observerEnabled;
    g_nativeSettings = g_nativeSelected ? settings.bridge : OllamaBridge::Settings{};
    g_nativeObserver.Configured(g_nativeSelected, g_nativeSettings, OllamaBridge::Now());
    if (g_nativeSelected)
    {
        g_nativeObserver.Start();
        return;  // Tick waits for actual retired workers and authenticated open before creating an epoch.
    }

static void UpdateNativeObserver()
{
    using namespace OllamaBridge;
    if (!g_nativeSelected)
        return;
    auto const now = Now();
    g_nativeObserver.Poll(now);
    auto state = g_nativeObserver.State();
    if (state.epoch.empty() && !g_bridgeWorld.Selected().epoch.empty())
        RetireBridgeWork();
    // Count physical queued/completed slots as well as workers, never cancelled reservations, and wait for the
    // restores of retired lasting plans (J12) instead of retiring again.
    bool const drained = g_bridgeInFlight.load() == 0 && DispatchSlot::Used() == 0 && g_bridgeChannel.Drained() &&
                         OllamaChat_BridgePlansEmpty();
    if (state.enabled && state.epoch.empty())
    {
        if (state.incarnation.empty())
            g_nativeObserver.Open(drained, now);  // Native open independently checks its own actual work drain.
        else if (drained)
        {
            auto const bytes = Acore::Crypto::GetRandomBytes<16>();
            char const hex[] = "0123456789abcdef";
            std::string epoch = state.incarnation + ".";
            for (auto byte : bytes)
            {
                epoch.push_back(hex[byte >> 4]);
                epoch.push_back(hex[byte & 15]);
            }
            // Only this world-thread owner supplies entropy and selection after actual retirement.
            g_bridgeWorld.Configured(true, g_nativeSettings, epoch);
            if (g_bridgeWorld.ObserveDrain(drained, BridgeRetirementProof()) &&
                g_nativeObserver.BindEpoch(std::move(epoch), drained, now))
                ActivateBridge();
            else
                RetireBridgeWork();
        }
    }
}


// src/mod-ollama-chat_handler.cpp, the binding of a new conversation:
    auto const bridgeSettings = OllamaDispatch_BridgeWorld().Snapshot();
    if (bridgeSettings.provider == "codex" && bridgeSettings.observerEnabled)
    {
        // Native permits one exact context per bot/controller/channel peer. Replace, never alias to newest.
        for (auto prior = g_bridgeBindings.begin(); prior != g_bridgeBindings.end();)
        {
            auto const& scope = prior->view.guard.scope;
            if (scope.botGuid == view.guard.scope.botGuid && scope.controllerGuid == view.guard.scope.controllerGuid &&
                scope.channel == view.guard.scope.channel && scope.contextId != view.guard.scope.contextId)
            {
                auto const index = prior - g_bridgeBindings.begin();
                BridgeErase(prior);
                prior = g_bridgeBindings.begin() + index;
            }
            else
                ++prior;
        }
    }
