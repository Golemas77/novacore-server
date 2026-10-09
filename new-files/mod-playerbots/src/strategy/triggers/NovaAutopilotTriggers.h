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

class NovaSaTrigger : public Trigger
{
public:
    NovaSaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nova sa", 1) {}
    bool IsActive() override;
};

class NovaItemsTrigger : public Trigger
{
public:
    NovaItemsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nova items", 3) {}
    bool IsActive() override;
};

class NovaZoneTrigger : public Trigger
{
public:
    NovaZoneTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nova zone", 30) {}
    bool IsActive() override;
};

class NovaAmmoTrigger : public Trigger
{
public:
    NovaAmmoTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nova ammo", 5) {}
    bool IsActive() override;
};

class NovaQuestFocusTrigger : public Trigger
{
public:
    NovaQuestFocusTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nova quest focus", 1) {}
    bool IsActive() override;
};

class NovaGatherTrigger : public Trigger
{
public:
    NovaGatherTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nova gather", 1) {}
    bool IsActive() override;
};

class NovaQuestUseTrigger : public Trigger
{
public:
    NovaQuestUseTrigger(PlayerbotAI* botAI) : Trigger(botAI, "nova quest use", 1) {}
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
