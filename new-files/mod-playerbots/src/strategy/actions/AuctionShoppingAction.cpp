/*
 * NovaCore: botai perka daiktus is aukciono (zr. PlayerbotAuctionMgr.h).
 */

#include "AuctionShoppingAction.h"

#include "Config.h"
#include "Event.h"
#include "ItemUsageValue.h"
#include "ObjectMgr.h"
#include "PlayerbotAuctionMgr.h"
#include "PlayerbotRotation.h"
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

    bool const autopilot = NovaIsAutopilot(bot);
    if (!autopilot && (!sRandomPlayerbotMgr->IsRandomBot(bot) || botAI->HasRealPlayerMaster()))
        return false;

    if (bot->GetLevel() < (autopilot ? 1u : 10u) || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->InBattleground() || bot->InBattlegroundQueue())
        return false;

    Map* map = bot->GetMap();
    if (!map || map->Instanceable())
        return false;

    // autopilotas gali tik parduoti net be pinigu; botai ir pirkimui reikia bent 50 sidabro
    return autopilot || bot->GetMoney() >= 5000;
}

bool AuctionShoppingAction::Execute(Event /*event*/)
{
    bool const sold = TrySell();
    bool const bought = bot->GetMoney() >= 5000 && TryBuy();
    return sold || bought;
}

// NovaCore: zaidejo autopilotas deda i aukciona daiktus, kuriu pats nenaudos (AI „item usage“ = AH), jei verta (>= 10 sidabro).
bool AuctionShoppingAction::TrySell()
{
    if (!NovaIsAutopilot(bot) || !sConfigMgr->GetOption<bool>("AiPlayerbot.NovaAutopilotAuctionSell", true))
        return false;

    uint32 const minPrice = sConfigMgr->GetOption<uint32>("AiPlayerbot.NovaAutopilotAuctionMinPrice", 1000);
    uint32 const maxPerRun = 4;
    AuctionHouseId const house = sPlayerbotAuctionMgr->HouseOf(bot);
    uint32 queued = 0;

    auto consider = [&](Item* item)
    {
        if (!item || queued >= maxPerRun)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || !item->CanBeTraded() || item->IsNotEmptyBag() || proto->HasFlag(ITEM_FLAG_CONJURED) ||
            item->GetUInt32Value(ITEM_FIELD_DURATION))
            return;

        // pardavimui - tik tai, ko AI pats nenaudos (nereikalinga pagerinimui, uzduotims ar profesijai)
        if (AI_VALUE2(ItemUsage, "item usage", item->GetEntry()) != ITEM_USAGE_AH)
            return;

        uint32 const buyout = sPlayerbotAuctionMgr->SuggestBuyout(house, item->GetEntry(), item->GetCount(), bot->GetGUID().GetCounter());
        if (buyout < minPrice)
            return;

        sPlayerbotAuctionMgr->QueueSale(bot->GetGUID(), item->GetGUID(), buyout);
        ++queued;
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                consider(bag->GetItemByPos(i));
    }

    return queued > 0;
}

bool AuctionShoppingAction::TryBuy()
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
