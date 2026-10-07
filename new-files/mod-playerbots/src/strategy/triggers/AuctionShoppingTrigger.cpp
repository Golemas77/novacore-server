/*
 * NovaCore: botai perka daiktus is aukciono (zr. PlayerbotAuctionMgr.h).
 */

#include "AuctionShoppingTrigger.h"

#include "Playerbots.h"
#include "PlayerbotRotation.h"

bool AuctionShoppingTrigger::IsActive()
{
    if (!sPlayerbotAIConfig->auctionShopping)
        return false;

    time_t const now = time(nullptr);
    if (NovaIsAutopilot(bot))
    {
        // NovaCore: zaidejo autopilotas - kas 3-6 min (pardavimas + pirkimas)
        if (now < nextShopping)
            return false;
        nextShopping = now + urand(180, 360);
        return true;
    }

    if (!nextShopping)
    {
        // pirmas kartas - isskirstom botus per laika, kad visi nesuskaitytu vienu metu
        nextShopping = now + urand(60, sPlayerbotAIConfig->auctionShoppingIntervalMax);
        return false;
    }

    if (now < nextShopping)
        return false;

    nextShopping = now + urand(sPlayerbotAIConfig->auctionShoppingIntervalMin, sPlayerbotAIConfig->auctionShoppingIntervalMax);
    return true;
}
