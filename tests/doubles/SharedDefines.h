// Selftest stand-in for the core's SharedDefines.h: the enumerators the module's units name, with the core's values.
#ifndef OLLAMA_SELFTEST_SHARED_DEFINES_H
#define OLLAMA_SELFTEST_SHARED_DEFINES_H

enum Classes
{
    CLASS_WARRIOR = 1,
    CLASS_PALADIN = 2,
    CLASS_HUNTER = 3,
    CLASS_ROGUE = 4,
    CLASS_PRIEST = 5,
    CLASS_DEATH_KNIGHT = 6,
    CLASS_SHAMAN = 7,
    CLASS_MAGE = 8,
    CLASS_WARLOCK = 9,
    CLASS_DRUID = 11
};

enum Races
{
    RACE_HUMAN = 1,
    RACE_ORC = 2,
    RACE_DWARF = 3,
    RACE_NIGHTELF = 4,
    RACE_UNDEAD_PLAYER = 5,
    RACE_TAUREN = 6,
    RACE_GNOME = 7,
    RACE_TROLL = 8,
    RACE_BLOODELF = 10,
    RACE_DRAENEI = 11
};

enum Gender
{
    GENDER_MALE = 0,
    GENDER_FEMALE = 1
};

enum TeamId
{
    TEAM_ALLIANCE = 0,
    TEAM_HORDE = 1,
    TEAM_NEUTRAL = 2
};

enum Language
{
    LANG_UNIVERSAL = 0
};

#endif  // OLLAMA_SELFTEST_SHARED_DEFINES_H
