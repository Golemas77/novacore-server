/*
 * NovaCore: zaidejo autopilotas – triggeriai (zr. NovaAutopilotActions.h).
 */

#ifndef _PLAYERBOT_NOVAAUTOPILOTTRIGGERS_H
#define _PLAYERBOT_NOVAAUTOPILOTTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

class NovaTurnInTrigger : public Trigger
{
public:
    NovaTurnInTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nova turn in", 1) {}
    bool IsActive() override;
};

class NovaQuestObjectsTrigger : public Trigger
{
public:
    NovaQuestObjectsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nova quest objects", 1) {}
    bool IsActive() override;
};

class NovaQuestMobsTrigger : public Trigger
{
public:
    NovaQuestMobsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nova quest mobs", 1) {}
    bool IsActive() override;
};

class NovaBagsFullTrigger : public Trigger
{
public:
    NovaBagsFullTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nova bags full", 1) {}
    bool IsActive() override;
};

#endif
