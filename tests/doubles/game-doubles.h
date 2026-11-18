// The game types a production unit reads (ObjectGuid, Player, Group, PlayerbotAI, Channel, the grid search, ...) have
// no shared stand-in: each selftest defines its own doubles, with the reads it counts, and then compiles the
// production unit whole by including its .cpp. The core and playerbots header names in this folder lead here, so a
// unit's own includes resolve without the game.
#ifndef OLLAMA_SELFTEST_GAME_DOUBLES
#error "define the game doubles and OLLAMA_SELFTEST_GAME_DOUBLES before including a production unit"
#endif
