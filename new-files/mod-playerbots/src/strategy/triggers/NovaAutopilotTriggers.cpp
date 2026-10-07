/*
 * NovaCore: zaidejo autopilotas – triggeriai (zr. NovaAutopilotActions.h).
 */

#include "NovaAutopilotTriggers.h"

#include "NovaAutopilotActions.h"
#include "Playerbots.h"

bool NovaTurnInTrigger::IsActive()
{
    if (!NovaAutopilotFree(bot, botAI))
        return false;

    uint32 questId = 0;
    WorldPosition pos;
    return NovaFindCompletedQuest(bot, botAI, questId, pos);
}

bool NovaQuestObjectsTrigger::IsActive()
{
    return NovaAutopilotFree(bot, botAI) && NovaHasObjectQuests(bot, botAI);
}

bool NovaBagsFullTrigger::IsActive()
{
    return NovaAutopilotFree(bot, botAI) && NovaNeedsVendor(bot);
}

bool NovaQuestMobsTrigger::IsActive()
{
    if (!NovaAutopilotFree(bot, botAI))
        return false;

    // pirmenybe: atiduoti ivykdyta uzduoti ir parduoti pilna kuprine (jie turi savo trigerius) – kitaip botas blaskosi
    uint32 pendingQuest = 0;
    WorldPosition pendingPos;
    if (NovaFindCompletedQuest(bot, botAI, pendingQuest, pendingPos) || NovaNeedsVendor(bot))
        return false;

    // matomas uzduociu mobas – tuo pasirupina „attack anything“; cia reikia tik tada, kai matomu nera
    if (AI_VALUE(Unit*, "grind target"))
        return false;

    return NovaHasKillQuests(bot, botAI);
}
