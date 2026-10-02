/*
 * NovaCore: botai perka daiktus is aukciono (zr. PlayerbotAuctionMgr.h).
 */

#include "AuctionShoppingTrigger.h"

#include "Playerbots.h"

bool AuctionShoppingTrigger::IsActive()
{
    if (!sPlayerbotAIConfig->auctionShopping)
        return false;

    time_t const now = time(nullptr);
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
