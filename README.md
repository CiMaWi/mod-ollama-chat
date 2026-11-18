<p align="center">
  <img src="./icon.png" alt="Ollama Chat Module" title="Ollama Chat Module Icon">
</p>

# AzerothCore + Playerbots Module: mod-ollama-chat (living-azeroth fork)

This fork started from DustinHendrickson's mod-ollama-chat and no longer follows it: since LC.42 the module's own
direct path to an LLM (its API client, prompts, memory, RAG, sentiment, personalities, random and event chatter) is
gone. Bots speak through one path, the **bridge**: the module sends a bounded, game-bound request to a local bridge
process on `127.0.0.1`, and the bridge answers with a validated line (and optional typed intents). Upstream is
reference only.

> [!IMPORTANT]
> To keep Playerbots' own chatter from interfering, set in `playerbots.conf`:
> `AiPlayerbot.EnableBroadcasts = 0`, `AiPlayerbot.RandomBotTalk = 0`, `AiPlayerbot.RandomBotEmote = 0`,
> `AiPlayerbot.RandomBotSuggestDungeons = 0`, `AiPlayerbot.EnableGreet = 0`, `AiPlayerbot.GuildFeedback = 0`,
> `AiPlayerbot.RandomBotSayWithoutMaster = 0`.

## Overview

- **Bridge only.** `OllamaChat.Enable` switches the bridge path on. An enabled module without valid bridge settings
  (`OllamaChat.Bridge.Instance`, `Endpoint`, `Token`) is idle: it logs one line at config load, listens on no port,
  starts no workers and generates nothing. The wire format is `docs/bridge-channel-v1.md`.
- **Replies to people.** When a player speaks in say, yell, party or a whisper, eligible bots may answer (say and yell
  only when a bot is addressed); the reply chances, distances and the conversation governor decide who and how often.
- **Embodiment.** Bots face whoever they answer, play gestures carried in a reply, and mirror or counter emotes aimed
  at them (no bridge call).
- **Roleplay voice.** Race and class voices and faction attitude shape the bridge's voice instruction; out-of-world
  vocabulary is filtered from replies.

## Installation

1. AzerothCore with mod-playerbots (this fork's core: the playerbots fork). Dependencies: fmt; nlohmann/json and
   cpp-httplib are bundled.
2. Put the module under `modules/`, re-run CMake and build.
3. Copy `conf/mod_ollama_chat.conf.dist` to your server's `modules` config folder as `mod_ollama_chat.conf` and fill
   in the bridge settings. Treat that file like `worldserver.conf`: it holds the bridge token.

## Configuration Options

Every option is documented in `conf/mod_ollama_chat.conf.dist`, which is the module's real documentation surface.

## Text Commands

All commands need `SEC_ADMINISTRATOR` and also work from the console without the leading dot.

- `.ollama reload`: reloads the module's configuration (and the roleplay voices) without a restart.
- `.ollama status`: whether the bridge is configured or idle, dispatcher queue depth and workers, delivery and drop
  counters, governor state (including why replies were suppressed), and the last error. Run this first when bots go
  quiet.
- `.ollama lab logout <botName>`: lab helper; logs one bot out through the holder that owns it.

## Conversation Control

Bot chat can run away in several ways, so there are independent brakes, configured in the `CONVERSATION GOVERNOR`
section of the config file:

| Brake | What it stops |
|---|---|
| **Cooldowns and rate limits** | One bot, or one crowd, dominating a channel. Per bot, per channel and server-wide. |
| **Repetition scoring** | The same line twice, and the same *opening phrase* twice, scored against the bot's own recent lines and the channel's recent traffic. |

Only a real player's line in say, yell, party or a whisper opens a bridge request. The bot-to-bot brakes (the
audience rule `OllamaChat.BotConversation.RequireRecentHuman`, the chain depth `MaxChainDepth` and `ChanceDecayPct`)
stay in the governor for a later bot-to-bot step; no path reaches them now.

## Roleplay Mode

Off by default (`OllamaChat.Roleplay.Enable`). Gives each race a speech register and each class a worldview, as part
of the bridge's voice instruction. `OllamaChat.Roleplay.Strictness` sets how strictly the instruction keeps a bot in
character; from 1 on, out-of-world vocabulary is filtered from replies. `OllamaChat.Roleplay.CrossFactionGibberish`
stops say/yell replies across factions. Voices can be overridden per server in the `mod_ollama_chat_voice` table.

## Body Language

- **Facing**: the bot turns toward whoever it answers, just before the line lands; skipped while moving, casting, in
  combat, in flight, on a transport or teleporting.
- **Gestures**: a reply may carry `[emote:wave]`, `*waves*` or a bare `/wave`; the tag is stripped and the emote
  played through the client's own code path.
- **Emote reactions**: bots mirror (a wave gets a wave) or counter (a flex gets a laugh) emotes aimed at them.

## Threading Model

Worker threads do HTTP and string work only. Every read or write of a `Player`, `Channel`, `Guild`, `Group` or `Map`
happens on the world thread. Requests are built on the world thread, handed to a bounded worker pool
(`OllamaChat.WorkerThreads`) and delivered back on the world thread by a completion queue drained each tick; queue
depth is capped (`OllamaChat.MaxQueueDepth`).

## Stored data

The direct path's tables (`mod_ollama_chat_personality`, `mod_ollama_chat_personality_templates`,
`mod_ollama_chat_history`, `mod_ollama_chat_bot_player_sentiments`, `mod_ollama_chat_memories`,
`mod_ollama_chat_relationships`) keep their SQL files and rows, but nothing reads or writes them any more.
`mod_ollama_chat_voice` (roleplay voices) and the persona card tables are in use.

## License

Released under the GNU GPL v3, consistent with AzerothCore's licensing. Originally developed by Dustin Hendrickson.
