/*
 * NovaCore: botai perka daiktus is aukciono - zr. PlayerbotAuctionMgr.h
 */

#include "PlayerbotAuctionMgr.h"

#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotRotation.h"
#include "ScriptMgr.h"
#include "Util.h"
#include "World.h"

PlayerbotAuctionMgr* PlayerbotAuctionMgr::instance()
{
    static PlayerbotAuctionMgr inst;
    return &inst;
}

uint32 PlayerbotAuctionMgr::Index(AuctionHouseId house)
{
    switch (house)
    {
        case AuctionHouseId::Alliance:
            return 0;
        case AuctionHouseId::Horde:
            return 1;
        default:
            return 2;
    }
}

AuctionHouseId PlayerbotAuctionMgr::HouseOf(Player* bot) const
{
    AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMap(bot->GetFaction());
    for (AuctionHouseId id : { AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral })
        if (sAuctionMgr->GetAuctionsMapByHouseId(id) == ah)
            return id;

    return AuctionHouseId::Neutral;
}

// ---------------------------------------------------------------- pagrindine gija

void PlayerbotAuctionMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig->auctionShopping)
        return;

    _refreshTimer += diff;
    if (_refreshTimer >= sPlayerbotAIConfig->auctionShoppingRefreshSeconds * 1000)
    {
        _refreshTimer = 0;
        Refresh();
    }

    _processTimer += diff;
    if (_processTimer >= 3000)
    {
        _processTimer = 0;
        ProcessPurchases();
    }

    // NovaCore: autopilotas - pardavimai ir aukciono pastas
    _saleTimer += diff;
    if (_saleTimer >= 3000)
    {
        _saleTimer = 0;
        ProcessSales();
    }

    _mailTimer += diff;
    if (_mailTimer >= 30000)
    {
        _mailTimer = 0;
        CollectMail();
    }
}

void PlayerbotAuctionMgr::Refresh()
{
    time_t const now = GameTime::GetGameTime().count();
    std::array<std::vector<BotAuctionOffer>, 3> fresh;

    for (AuctionHouseId id : { AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral })
    {
        AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMapByHouseId(id);
        if (!ah)
            continue;

        std::vector<BotAuctionOffer>& v = fresh[Index(id)];
        v.reserve(ah->Getcount());
        for (auto const& pair : ah->GetAuctions())
        {
            AuctionEntry const* a = pair.second;
            if (!a || !a->buyout || a->bidder || a->expire_time < now + 300)
                continue;

            v.push_back({ a->Id, a->item_template, a->itemCount, a->buyout, uint32(a->owner.GetCounter()) });
        }
    }

    std::lock_guard<std::mutex> guard(_lock);
    _snapshot = std::move(fresh);
}

void PlayerbotAuctionMgr::ProcessPurchases()
{
    std::deque<Intent> work;
    {
        std::lock_guard<std::mutex> guard(_lock);
        // ne daugiau 20 pirkimu per ciklą (kas 3 s) - kad nebūtų šuolio
        while (!_queue.empty() && work.size() < 20)
        {
            work.push_back(_queue.front());
            _queue.pop_front();
        }
    }

    for (Intent const& in : work)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(in.bot);
        if (!bot || !bot->IsInWorld())
            continue;

        AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMapByHouseId(in.house);
        AuctionEntry* auction = ah ? ah->GetAuction(in.auctionId) : nullptr;
        if (!auction || auction->bidder || !auction->buyout || auction->buyout > in.maxPrice || auction->owner == bot->GetGUID())
            continue;

        Item* item = sAuctionMgr->GetAItem(auction->item_guid);
        if (!item || !bot->HasEnoughMoney(auction->buyout))
            continue;

        ItemPosCountVec dest;
        if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
            continue;

        uint32 const itemEntry = auction->item_template;
        uint32 const count = auction->itemCount;
        uint32 const price = auction->buyout;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        bot->ModifyMoney(-int32(price));
        auction->bidder = bot->GetGUID();
        auction->bid = price;

        // pardavejui - atlygis (kaip pirkimas uz "buyout"), pats daiktas pereina botui
        sAuctionMgr->SendAuctionSalePendingMail(auction, trans);
        sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
        sScriptMgr->OnAuctionSuccessful(ah, auction);

        auction->DeleteFromDB(trans);
        sAuctionMgr->RemoveAItem(auction->item_guid);          // daiktas lieka gyvas - perduodamas botui
        ah->RemoveAuction(auction);                            // auction nebenaudojamas

        bot->MoveItemToInventory(dest, item, true);
        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        LOG_INFO("playerbots", "Bot {} <{}>: nupirko is aukciono {}x daikta {} uz {} vario", bot->GetGUID().ToString().c_str(),
                 bot->GetName().c_str(), count, itemEntry, price);
    }
}

// NovaCore: pardavimas / aukciono pastas zaidejo autopilotui. Viskas pagrindineje gijoje.

void PlayerbotAuctionMgr::ProcessSales()
{
    std::deque<SaleIntent> work;
    {
        std::lock_guard<std::mutex> guard(_lock);
        while (!_sales.empty() && work.size() < 10)
        {
            work.push_back(_sales.front());
            _sales.pop_front();
        }
    }

    for (SaleIntent const& s : work)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(s.bot);
        if (!bot || !bot->IsInWorld() || !NovaIsAutopilot(bot))
            continue;

        Item* item = bot->GetItemByGuid(s.item);
        if (!item || sAuctionMgr->GetAItem(item->GetGUID()) || !item->CanBeTraded() || item->IsNotEmptyBag() ||
            item->GetTemplate()->HasFlag(ITEM_FLAG_CONJURED) || item->GetUInt32Value(ITEM_FIELD_DURATION))
            continue;

        AuctionHouseEntry const* houseEntry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(bot->GetFaction());
        AuctionHouseObject* ah = sAuctionMgr->GetAuctionsMap(bot->GetFaction());
        if (!houseEntry || !ah)
            continue;

        uint32 const count = item->GetCount();
        uint32 const etime = 2 * MIN_AUCTION_TIME;      // 24 val.
        uint32 const deposit = sAuctionMgr->GetAuctionDeposit(houseEntry, etime, item, count);
        if (!bot->HasEnoughMoney(deposit))
            continue;

        bot->ModifyMoney(-int32(deposit));

        AuctionEntry* AH = new AuctionEntry;
        AH->Id = sObjectMgr->GenerateAuctionID();
        if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
            AH->houseId = AuctionHouseId::Neutral;
        else
            AH->houseId = AuctionHouseId(houseEntry->houseId);

        AH->item_guid = item->GetGUID();
        AH->item_template = item->GetEntry();
        AH->itemCount = count;
        AH->owner = bot->GetGUID();
        AH->startbid = std::max<uint32>(1, s.buyout / 100 * 80);
        AH->bidder = ObjectGuid::Empty;
        AH->bid = 0;
        AH->buyout = s.buyout;
        AH->expire_time = GameTime::GetGameTime().count() + uint32(etime * sWorld->getRate(RATE_AUCTION_TIME));
        AH->deposit = deposit;
        AH->auctionHouseEntry = houseEntry;

        uint32 const entry = item->GetEntry();
        sAuctionMgr->AddAItem(item);
        ah->AddAuction(AH);

        bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        item->DeleteFromInventoryDB(trans);
        item->SaveToDB(trans);
        AH->SaveToDB(trans);
        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        bot->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CREATE_AUCTION, 1);

        LOG_INFO("playerbots", "Autopilotas {} <{}>: idejo i aukciona {}x daikta {} uz {} vario (depozitas {})",
                 bot->GetGUID().ToString().c_str(), bot->GetName().c_str(), count, entry, s.buyout, deposit);
    }
}

// Aukciono pastas (parduotu daiktu pinigai, negrazinti / nesuteiktu pasiulymu daiktai): imama tiesiai, be pasto dezes.
void PlayerbotAuctionMgr::CollectMail()
{
    time_t const now = GameTime::GetGameTime().count();
    for (ObjectGuid const& guid : sPlayerbotRotationMgr->AutopilotPlayers())
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
            continue;

        std::vector<Mail*> mails(bot->GetMails().begin(), bot->GetMails().end());
        bool any = false;
        for (Mail* m : mails)
        {
            if (!m || m->state == MAIL_STATE_DELETED || m->messageType != MAIL_AUCTION || m->COD || m->deliver_time > now)
                continue;

            bool done = true;
            if (m->money)
            {
                if (bot->ModifyMoney(m->money, false))
                    m->money = 0;
                else
                    done = false;
            }

            std::vector<MailItemInfo> items(m->items.begin(), m->items.end());
            for (MailItemInfo const& ii : items)
            {
                Item* it = bot->GetMItem(ii.item_guid);
                if (!it)
                {
                    m->RemoveItem(ii.item_guid);
                    continue;
                }

                ItemPosCountVec dest;
                if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, it, false) != EQUIP_ERR_OK)
                {
                    done = false;
                    continue;
                }

                m->RemoveItem(ii.item_guid);
                m->removedItems.push_back(ii.item_guid);
                bot->RemoveMItem(it->GetGUID().GetCounter());
                it->SetState(ITEM_UNCHANGED);
                bot->MoveItemToInventory(dest, it, true);
            }

            if (done && m->items.empty() && !m->money)
            {
                m->state = MAIL_STATE_DELETED;
                sCharacterCache->DecreaseCharacterMailCount(bot->GetGUID());
            }
            else
                m->state = MAIL_STATE_CHANGED;

            bot->m_mailsUpdated = true;
            any = true;
        }

        if (any)
        {
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            bot->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);
        }
    }
}

uint32 PlayerbotAuctionMgr::SuggestBuyout(AuctionHouseId house, uint32 itemEntry, uint32 count, uint32 ownerLow)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    if (!proto || !count)
        return 0;

    uint64 bestUnit = 0;
    {
        std::lock_guard<std::mutex> guard(_lock);
        for (BotAuctionOffer const& o : _snapshot[Index(house)])
        {
            if (o.item != itemEntry || !o.count || o.ownerLow == ownerLow)
                continue;
            uint64 const unit = o.buyout / o.count;
            if (unit && (!bestUnit || unit < bestUnit))
                bestUnit = unit;
        }
    }

    uint64 unit;
    if (bestUnit)
        unit = bestUnit * 95 / 100;                       // truputi pigiau uz pigiausia dabartine
    else
    {
        // nera palyginimo – pagal vendoro kainas (aukciono kaina paprastai kelis kartus didesne uz pardavimo vendorui)
        uint64 buy = proto->BuyCount ? proto->BuyPrice / proto->BuyCount : proto->BuyPrice;
        unit = std::max<uint64>(uint64(proto->SellPrice) * 3, buy / 2);
    }

    uint64 total = std::max<uint64>(unit, 1) * count;
    return uint32(std::min<uint64>(total, 0x7FFFFFFF));
}

void PlayerbotAuctionMgr::QueueSale(ObjectGuid bot, ObjectGuid item, uint32 buyout)
{
    std::lock_guard<std::mutex> guard(_lock);
    if (_sales.size() < 100)
        _sales.push_back({ bot, item, buyout });
}

// ---------------------------------------------------------------- bot gijos

void PlayerbotAuctionMgr::Candidates(Player* bot, AuctionHouseId house, uint32 maxPrice, std::vector<BotAuctionOffer>& out, uint32 maxOut)
{
    std::lock_guard<std::mutex> guard(_lock);
    std::vector<BotAuctionOffer> const& v = _snapshot[Index(house)];
    if (v.empty())
        return;

    uint32 const level = bot->GetLevel();
    uint32 const guidLow = bot->GetGUID().GetCounter();
    size_t const start = urand(0, uint32(v.size() - 1));
    size_t const scan = std::min<size_t>(v.size(), 700);

    for (size_t i = 0; i < scan && out.size() < maxOut; ++i)
    {
        BotAuctionOffer const& o = v[(start + i) % v.size()];
        if (o.ownerLow == guidLow || !o.buyout || o.buyout > maxPrice)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(o.item);
        if (!proto)
            continue;

        // nenaudojami vietos zymekliai ("Deprecated ...", "[PH]", "(OLD)"), kuriuos ah-bot kartais isstato po 1-2 varius
        if (proto->Name1.find("Deprecated") != std::string::npos || proto->Name1.find("[PH]") != std::string::npos ||
            proto->Name1.find("(OLD)") != std::string::npos || proto->Name1.find("NYI") != std::string::npos)
            continue;

        switch (proto->Class)
        {
            case ITEM_CLASS_WEAPON:
            case ITEM_CLASS_ARMOR:
                if (proto->Quality < ITEM_QUALITY_UNCOMMON || proto->RequiredLevel > level || proto->RequiredLevel + 12 < level)
                    continue;
                if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
                    continue;
                break;
            case ITEM_CLASS_TRADE_GOODS:
            case ITEM_CLASS_REAGENT:
            case ITEM_CLASS_RECIPE:
            case ITEM_CLASS_GEM:
                if (o.count > 40)
                    continue;
                break;
            default:
                continue;
        }

        out.push_back(o);
    }
}

void PlayerbotAuctionMgr::QueuePurchase(ObjectGuid bot, AuctionHouseId house, uint32 auctionId, uint32 maxPrice)
{
    std::lock_guard<std::mutex> guard(_lock);
    if (_queue.size() < 500)
        _queue.push_back({ bot, house, auctionId, maxPrice });
}

// ---------------------------------------------------------------- scenarijus

class PlayerbotAuctionWorldScript : public WorldScript
{
public:
    PlayerbotAuctionWorldScript() : WorldScript("PlayerbotAuctionWorldScript", { WORLDHOOK_ON_UPDATE }) { }

    void OnUpdate(uint32 diff) override { sPlayerbotAuctionMgr->Update(diff); }
};

void AddPlayerbotAuctionScripts()
{
    new PlayerbotAuctionWorldScript();
}
