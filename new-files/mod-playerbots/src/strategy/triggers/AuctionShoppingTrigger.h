/*
 * NovaCore: botai perka daiktus is aukciono (zr. PlayerbotAuctionMgr.h).
 */

#ifndef _PLAYERBOT_AUCTIONSHOPPINGTRIGGER_H
#define _PLAYERBOT_AUCTIONSHOPPINGTRIGGER_H

#include "Trigger.h"

class PlayerbotAI;

// Aktyvi ne daznau nei kas `AiPlayerbot.AuctionShoppingIntervalMin..Max` sekundziu kiekvienam botui.
class AuctionShoppingTrigger : public Trigger
{
public:
    AuctionShoppingTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ah shopping", 30) {}

    bool IsActive() override;

private:
    time_t nextShopping = 0;
};

#endif
