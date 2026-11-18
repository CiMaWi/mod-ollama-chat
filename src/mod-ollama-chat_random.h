#ifndef MOD_OLLAMA_CHAT_RANDOM_H
#define MOD_OLLAMA_CHAT_RANDOM_H

#include "ScriptMgr.h"
#include "ObjectGuid.h"

// Hosts the module's world tick: the dispatcher, the governor and the periodic saves are pumped from here.
class OllamaBotRandomChatter : public WorldScript
{
public:
    OllamaBotRandomChatter();
    void OnUpdate(uint32 diff) override;

};


#endif // MOD_OLLAMA_CHAT_RANDOM_H
