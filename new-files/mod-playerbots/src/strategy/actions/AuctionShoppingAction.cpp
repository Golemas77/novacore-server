/*
 * NovaCore: botai perka daiktus is aukciono (zr. PlayerbotAuctionMgr.h).
 */

#include "AuctionShoppingAction.h"

#include "Event.h"
#include "ItemUsageValue.h"
#include "ObjectMgr.h"
#include "PlayerbotAuctionMgr.h"
#include "Playerbots.h"

namespace
{
    // kuo mazesnis skaicius - tuo svarbiau
    uint32 UsagePriority(ItemUsage usage)
    {
        switch (usage)
        {
            case ITEM_USAGE_EQUIP:
                return 0;       // tuscias lizdas - didziausias pagerinimas
            case ITEM_USAGE_REPLACE:
                return 1;
            case ITEM_USAGE_SKILL:
                return 2;
            default:
                return 99;
        }
    }
}

bool AuctionShoppingAction::isUseful()
{
    if (!sPlayerbotAIConfig->auctionShopping)
        return false;

    if (!sRandomPlayerbotMgr->IsRandomBot(bot) || botAI->HasRealPlayerMaster())
        return false;

    if (bot->GetLevel() < 10 || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->InBattleground() || bot->InBattlegroundQueue())
        return false;

    Map* map = bot->GetMap();
    if (!map || map->Instanceable())
        return false;

    return bot->GetMoney() >= 5000;         // bent 50 sidabro
}

bool AuctionShoppingAction::Execute(Event /*event*/)
{
    uint64 const money = bot->GetMoney();
    uint32 const maxPrice = uint32(std::min<uint64>(money * sPlayerbotAIConfig->auctionShoppingMoneyPercent / 100, 0x7FFFFFFF));
    if (!maxPrice)
        return false;

    AuctionHouseId const house = sPlayerbotAuctionMgr->HouseOf(bot);
    std::vector<BotAuctionOffer> offers;
    sPlayerbotAuctionMgr->Candidates(bot, house, maxPrice, offers, 12);
    if (offers.empty())
        return false;

    BotAuctionOffer const* best = nullptr;
    uint32 bestPriority = 99;
    for (BotAuctionOffer const& o : offers)
    {
        ItemUsage const usage = AI_VALUE2(ItemUsage, "item usage", o.item);
        uint32 const priority = UsagePriority(usage);
        if (priority >= 99)
            continue;

        if (!best || priority < bestPriority || (priority == bestPriority && o.buyout < best->buyout))
        {
            best = &o;
            bestPriority = priority;
        }
    }

    if (!best)
        return false;

    sPlayerbotAuctionMgr->QueuePurchase(bot->GetGUID(), house, best->id, maxPrice);
    return true;
}
