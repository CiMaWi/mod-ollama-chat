#ifndef MOD_OLLAMA_CHAT_ROLEPLAY_H
#define MOD_OLLAMA_CHAT_ROLEPLAY_H

#include <string>
#include <cstdint>

class Player;

// --------------------------------------------------------------------------
// Roleplay mode.
//
// The old EnableRPPersonalities only swapped which personality string got
// appended. Race and class were passed as bare facts -- "Tauren", "Druid" --
// with no voice attached, while the shipped chatter variations actively pushed
// the other way ("Complain about nerfed abilities", "Mention Death Knights
// being OP"). That is player-forum talk, not in-world talk.
//
// This adds two data-driven voice packs and a strictness dial:
//
//   race voice  -- speech register, cultural touchstones, what they call the
//                  other faction.
//   class voice -- worldview: what this character NOTICES about a scene.
//
// Strictness 0 = flavour only, 1 = in character with meta terms filtered,
// 2 = hard in character with mechanical numbers described rather than quoted,
// 3 = in world: as 2, and the built-in instructions plus the numeric prompt
//     placeholders (levels, level gaps, gold, distances, bag space) are
//     rendered as words a person living in the world would use.
// --------------------------------------------------------------------------

void Roleplay_Load();     // seed defaults from conf, then overlay DB rows

// The prompt fragment describing how this character speaks and what they
// notice. Empty when roleplay mode is off.
std::string Roleplay_BuildVoicePrompt(Player* bot);

// Strip out-of-world vocabulary ("dps", "nerf", "proc", "respec", ...).
// Applied at strictness >= 1.
std::string Roleplay_FilterMetaTerms(const std::string& text);

// Replace exact figures with description: "340/450" -> "badly wounded".
// Applied at strictness >= 2 so bots stop reading their own health bar aloud.
std::string Roleplay_DescribeHealth(uint32_t current, uint32_t max);

// True when the two players could not understand each other in-game and
// roleplay mode is configured to respect that.
bool Roleplay_IsLanguageBarrier(Player* speaker, Player* listener);

// Which chatter variation lists to use. At strictness 2 the roleplay lists
// replace the shipped out-of-character ones.
bool Roleplay_UseRoleplayVariations();

// True at Strictness >= 3 with roleplay on: prompts carry no game figures.
bool Roleplay_InWorld();

// Prompt placeholder values. In-world mode returns a phrase; otherwise the
// figure exactly as a bare "{placeholder}" in a template rendered it before.
//   level:    "seasoned"                      (an adjective: "a {bot_level} mage")
//   gap:      "stronger than you"             (other's level minus the bot's)
//   gold:     "a modest purse"                (whole gold pieces)
//   distance: "a stone's throw away"          (yards; negative = unknown)
//   bagSlots: "plenty of room in your packs"  (free slots)
std::string Roleplay_LevelText(uint32_t level);
std::string Roleplay_LevelGapText(int32_t gap);
std::string Roleplay_GoldText(uint32_t gold);
std::string Roleplay_DistanceText(float yards);
std::string Roleplay_BagSlotsText(uint32_t freeSlots);

#endif // MOD_OLLAMA_CHAT_ROLEPLAY_H
