/*
 * NovaCore: botai perka daiktus is aukciono (zr. PlayerbotAuctionMgr.h).
 */

#ifndef _PLAYERBOT_AUCTIONSHOPPINGACTION_H
#define _PLAYERBOT_AUCTIONSHOPPINGACTION_H

#include "Action.h"

class PlayerbotAI;

class AuctionShoppingAction : public Action
{
public:
    AuctionShoppingAction(PlayerbotAI* botAI) : Action(botAI, "ah shopping") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
