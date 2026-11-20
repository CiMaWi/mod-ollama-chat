#ifndef MOD_OLLAMA_CHAT_ROLEPLAY_H
#define MOD_OLLAMA_CHAT_ROLEPLAY_H

#include <string>
#include <cstdint>

class Player;

// --------------------------------------------------------------------------
// Roleplay mode: the voice part of the bridge's context.
//
// Two data-driven voice packs and a strictness dial:
//
//   race voice  -- speech register, cultural touchstones, what they call the
//                  other faction.
//   class voice -- worldview: what this character NOTICES about a scene.
//
// Strictness 0 = flavour only, 1 = in character with meta terms filtered,
// 2 = hard in character with mechanical numbers described rather than quoted,
// 3 = in world: as 2, and the instruction is worded without naming the things
//     it forbids.
// --------------------------------------------------------------------------

void Roleplay_Load();     // seed defaults from conf, then overlay DB rows

// The prompt fragment describing how this character speaks and what they
// notice. Empty when roleplay mode is off.
std::string Roleplay_BuildVoicePrompt(Player* bot);

// Strip out-of-world vocabulary ("dps", "nerf", "proc", "respec", ...).
// Applied at strictness >= 1.
std::string Roleplay_FilterMetaTerms(const std::string& text);

// True when the two players could not understand each other in-game and
// roleplay mode is configured to respect that.
bool Roleplay_IsLanguageBarrier(Player* speaker, Player* listener);

#endif // MOD_OLLAMA_CHAT_ROLEPLAY_H
