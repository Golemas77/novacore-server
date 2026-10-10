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

    // atidavimas – tik kai nebeliko darbo nebaigtoms uzduotims (zr. NovaTurnInGateOpen), tada visos uzduotys is karto
    return NovaTurnInGateOpen(bot, botAI);
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
    if (NovaTurnInGateOpen(bot, botAI) || NovaNeedsVendor(bot))
        return false;

    // matomas uzduociu mobas – tuo pasirupina „attack anything“; cia reikia tik tada, kai matomu nera
    if (AI_VALUE(Unit*, "grind target"))
        return false;

    return NovaHasKillQuests(bot, botAI);
}

bool NovaQuestUseTrigger::IsActive()
{
    if (!NovaAutopilotFree(bot, botAI))
        return false;

    return NovaHasQuestUseTarget(bot, botAI);
}

bool NovaSaTrigger::IsActive()
{
    return NovaSaActive(bot);
}

bool NovaGatherTrigger::IsActive()
{
    if (!NovaAutopilotFree(bot, botAI))
        return false;

    return NovaHasGatherNode(bot, botAI);
}

bool NovaQuestFocusTrigger::IsActive()
{
    if (!NovaAutopilotFree(bot, botAI))
        return false;

    return NovaHasQuestFocus(bot, botAI);
}

bool NovaAmmoTrigger::IsActive()
{
    return NovaNeedsAmmo(bot, botAI);
}

bool NovaItemsTrigger::IsActive()
{
    if (!NovaAutopilotFree(bot, botAI))
        return false;

    return NovaHasUsefulItems(bot, botAI);
}

bool NovaZoneTrigger::IsActive()
{
    return NovaZoneNeedsMove(bot, botAI);
}

bool NovaStuckTrigger::IsActive()
{
    return NovaIsStuck(bot, botAI);
}

bool NovaHomeTrigger::IsActive()
{
    return NovaNeedsHome(bot, botAI);
}

bool NovaGridlockTrigger::IsActive()
{
    return NovaIsGridlocked(bot, botAI);
}

bool NovaFlightTrigger::IsActive()
{
    return NovaWantsFlight(bot, botAI);
}
