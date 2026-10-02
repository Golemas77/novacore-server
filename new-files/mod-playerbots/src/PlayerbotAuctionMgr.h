/*
 * NovaCore: botai perka daiktus is aukciono.
 *
 * Skirta "random" botams: kas kelias minutes bot (veiksmas "ah shopping", bot gijoje) paziuri i savo frakcijos aukciono vaizda,
 * ISSIRINKDAMAS pagal savo AI "item usage" tik tai, kas jam reikalinga: ISPUOLIMAS / PAKEITIMAS (ekipiruotes pagerinimas) ir
 * SKILL (profesijos medziagos / receptai). Pirkimas ivykdomas PAGRINDINEJE gijoje (kaip tikro zaidejo CMSG_AUCTION_PLACE_BID
 * "buyout"): nuskaiciuojami pinigai, pardavejui issiunciamas atlygis, daiktas perkeliamas tiesiai i boto kuprine (ne pastu).
 *
 * Gijos: aukciono duomenys skaitomi / keiciami tik pagrindineje gijoje (Update), botai mato tik jos paruosta kopija (snapshot)
 * ir i pirkimo eile deda uzklausas.
 */

#ifndef _PLAYERBOT_AUCTIONMGR_H
#define _PLAYERBOT_AUCTIONMGR_H

#include "AuctionHouseMgr.h"
#include "Common.h"
#include "ObjectGuid.h"

#include <array>
#include <deque>
#include <mutex>
#include <vector>

class Player;

struct BotAuctionOffer
{
    uint32 id;
    uint32 item;
    uint32 count;
    uint32 buyout;
    uint32 ownerLow;
};

class PlayerbotAuctionMgr
{
public:
    static PlayerbotAuctionMgr* instance();

    // pagrindine gija (WorldScript::OnUpdate)
    void Update(uint32 diff);

    // bot gijos
    AuctionHouseId HouseOf(Player* bot) const;
    void Candidates(Player* bot, AuctionHouseId house, uint32 maxPrice, std::vector<BotAuctionOffer>& out, uint32 maxOut);
    void QueuePurchase(ObjectGuid bot, AuctionHouseId house, uint32 auctionId, uint32 maxPrice);

private:
    struct Intent
    {
        ObjectGuid bot;
        AuctionHouseId house;
        uint32 auctionId;
        uint32 maxPrice;
    };

    static uint32 Index(AuctionHouseId house);
    void Refresh();
    void ProcessPurchases();

    std::mutex _lock;                                      // saugo _snapshot ir _queue
    std::array<std::vector<BotAuctionOffer>, 3> _snapshot;
    std::deque<Intent> _queue;
    uint32 _refreshTimer = 60000;                          // pirmas perskaitymas po ~1 min nuo startavimo
    uint32 _processTimer = 0;
};

#define sPlayerbotAuctionMgr PlayerbotAuctionMgr::instance()

void AddPlayerbotAuctionScripts();

#endif
