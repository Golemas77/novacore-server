/*
 * NovaCore: zr. NovaRotationGuardStrategy.h
 */

#include "NovaRotationGuardStrategy.h"

#include <string>
#include <unordered_set>

#include "Action.h"
#include "PlayerbotRotation.h"
#include "Playerbots.h"

namespace
{
    // Visi veiksmai, kuriuos klasiu „nc“ strategijos (ir bendra NonCombatStrategy) gali paleisti, bet kurie zaidejo
    // veikejui netinka: keicia daiktus / uzduociu zurnala / pozas / forma, nuima slepima ar sprinta, juda i portalus ir pan.
    std::unordered_set<std::string> const& Blocked()
    {
        static std::unordered_set<std::string> const names = {
            "clean quest log", "check mount state", "move to dark portal", "use dark portal azeroth", "move from dark portal",
            "enter vehicle", "leave vehicle", "apply oil", "apply stone", "equip upgrades", "equip upgrade", "switch to melee",
            "switch to ranged", "say::low ammo", "unstealth", "sprint", "aquatic", "aquatic form", "levitate", "track humanoids",
            "destroy soul shard", "battle stance", "defensive stance", "berserker stance", "mount", "world buff",
            "move out of collision", "reveal gathering item", "add gathering loot", "add all loot", "loot", "open loot",
            "move to loot", "auto maintenance on levelup", "maintenance"
        };
        return names;
    }
}

float NovaRotationGuardMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    Player* bot = botAI->GetBot();
    if (!bot || !botAI->IsRealPlayer() || NovaIsAutopilot(bot))
        return 1.0f;

    if (Blocked().find(action->getName()) != Blocked().end())
        return 0.0f;

    // Zaidejas ka nors daro – priežiūra netrukdo.
    if (bot->IsMounted() || bot->IsInFlight() || bot->IsSitState() || bot->HasStealthAura() || bot->IsInWater() ||
        bot->GetVehicle() || bot->GetShapeshiftForm() != FORM_NONE)
        return 0.0f;

    return 1.0f;
}

void NovaRotationGuardStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new NovaRotationGuardMultiplier(botAI));
}
