/*
 * NovaCore: mod-personal-loot - asmeninis grobis (Personal Loot) kaip Retail.
 *
 * Kai grupe nukauna sutveri, kiekvienam teise gauti grobi turinciam nariui (zaidejui ar botui, esanciam instancijoje /
 * pakankamai arti) ATSKIRAI isridenama sutverio grobio lentele.
 *   - TIKRI ZAIDEJAI: ju daiktai lieka sutverio lavone, bet kiekvienas mato ir gali paimti TIK SAVO daiktus (kaip grobio lange:
 *     LootItem::rollWinnerGUID = savininkas). Kai zaidejas paima daikta, visa grupe mato "X gauna grobi: [daiktas]" - tada
 *     galima susitarti del apsikeitimo (BoP daiktai 2 val. perduodami grupes nariams, kaip Retail).
 *   - BOTAI (ir persipildymas - daugiau nei telpa i grobio langa): daiktai isduodami TIESIOG i kuprine.
 * Niekas nebesirenka "Need/Greed", niekas negali "pavogti" kito daikto; grobis, kuris iskrito tau - tavo.
 *
 * Taisykles:
 *   - veikia tik grupese ir tik kai grupeje yra bent `PersonalLoot.MinPlayers` tikru zaideju, esanciu pakankamai arti
 *     (instancijoje - visi); `PersonalLoot.Scope`: 0 = tik pozemiai ir reidai, 1 = visur (numatyta);
 *   - grobio lango rezimas veikia grupese su "Group Loot" ir "Need Before Greed"; "Round Robin" grupese (ir kai
 *     `PersonalLoot.AutoDeliver = 1`) daiktai isduodami tiesiai i kuprine; Free-For-All ir (pagal nustatyma) Master Loot
 *     paliekami kaip yra;
 *   - auksas lavone lieka standartinis (kas paima - dalijamas po lygiai); automatinio isdavimo rezime dalijamas is karto;
 *   - uzduociu daiktai isduodami tiesiai i kuprine (kaip standartiniame grobyje juos mato tik kam reikia);
 *   - jei kuprine pilna (automatinis isdavimas) - pilkas daiktas parduodamas uz auksa, kitas issiunciamas pastu;
 *   - grupes vadovas gali isjungti / ijungti savo grupei: `.grobis isjungti` / `.grobis ijungti` (iki serverio perkrovimo).
 *
 * Kabliukas: UnitScript::OnUnitDeath (kvieciamas Unit::Kill pabaigoje, kai standartinis grobis jau sugeneruotas).
 * Tekstai - `module_string` (world DB, `mod-personal-loot`). Zaidime neisbandyta.
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* PL_MODULE = "mod-personal-loot";

    // module_string ID (zr. data/sql/db-world/base/mod_personal_loot_module_string.sql)
    enum PersonalLootString : uint32
    {
        PL_STR_FIRST_NOTICE      = 1,   // pirmas pranesimas grupei (automatinis isdavimas)
        PL_STR_FIRST_NOTICE_WINDOW = 4, // pirmas pranesimas grupei (grobis lavone)
        PL_STR_MAIL_FULL         = 2,   // "{}" - daikto nuoroda
        PL_STR_GREY_SOLD         = 3,   // "{}" - suma
        PL_STR_INFO              = 10,  // "Asmeninis grobis: {} | {} | Jusu grupei: {}"
        PL_STR_SERVER_ON         = 11,
        PL_STR_SERVER_OFF        = 12,
        PL_STR_SCOPE_INSTANCES   = 13,
        PL_STR_SCOPE_ALL         = 14,
        PL_STR_GROUP_ACTIVE      = 15,
        PL_STR_GROUP_OFF         = 16,
        PL_STR_GROUP_NONE        = 17,
        PL_STR_GROUP_MASTER      = 18,
        PL_STR_GROUP_FFA         = 19,
        PL_STR_LEADER_OFF        = 20,
        PL_STR_LEADER_ON         = 21,
        PL_STR_LEADER_ONLY       = 22,
        PL_STR_NOT_IN_GROUP      = 23,
        PL_STR_HELP              = 24
    };

    struct PersonalLootConfig
    {
        bool   enable;
        uint32 scope;             // 0 - instancijos, 1 - visur
        uint32 minHumans;         // maziausias tikru zaideju skaicius
        bool   includeBots;
        bool   respectMaster;
        uint32 broadcastQuality;  // nuo kokios kokybes grupei rodoma "X gauna: [daiktas]"
        bool   greySell;
        bool   announce;
        bool   autoDeliver;       // true - viskas tiesiai i kuprine; false - grobio lange, kiekvienas savo daiktus
    };

    PersonalLootConfig Cfg()
    {
        PersonalLootConfig c;
        c.enable           = sConfigMgr->GetOption<bool>("PersonalLoot.Enable", true);
        c.scope            = sConfigMgr->GetOption<uint32>("PersonalLoot.Scope", 1);
        c.minHumans        = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PersonalLoot.MinPlayers", 1));
        c.includeBots      = sConfigMgr->GetOption<bool>("PersonalLoot.IncludeBots", true);
        c.respectMaster    = sConfigMgr->GetOption<bool>("PersonalLoot.RespectMasterLoot", true);
        c.broadcastQuality = sConfigMgr->GetOption<uint32>("PersonalLoot.BroadcastMinQuality", 3);
        c.greySell         = sConfigMgr->GetOption<bool>("PersonalLoot.SellGreyWhenFull", true);
        c.announce         = sConfigMgr->GetOption<bool>("PersonalLoot.Announce", true);
        c.autoDeliver      = sConfigMgr->GetOption<bool>("PersonalLoot.AutoDeliver", false);
        return c;
    }

    std::mutex g_mutex;
    std::unordered_set<ObjectGuid::LowType> g_groupOff;     // grupes, kuriu vadovas isjunge asmeninį grobį
    std::unordered_set<ObjectGuid::LowType> g_informed;     // grupes, kurioms jau parodytas pirmas pranesimas

    std::string Str(uint32 id)
    {
        if (std::string const* s = sObjectMgr->GetModuleString(PL_MODULE, id, LOCALE_enUS))
            return *s;
        return "<personal-loot>";
    }

    template <typename... Args>
    std::string Fmt(uint32 id, Args&&... args)
    {
        std::string const format = Str(id);
        return Acore::StringFormat(format.c_str(), std::forward<Args>(args)...);
    }

    void SendTo(Player* player, std::string const& text)
    {
        if (player && player->GetSession() && !player->GetSession()->IsBot())
            ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    std::string ItemLink(ItemTemplate const* p)
    {
        static char const* colors[] = { "9d9d9d", "ffffff", "1eff00", "0070dd", "a335ee", "ff8000", "e6cc80", "e6cc80" };
        char const* color = colors[std::min<uint32>(p->Quality, 7)];
        return Acore::StringFormat("|cff{}|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r", color, p->ItemId, p->Name1);
    }

    std::string MoneyText(uint32 copper)
    {
        return Acore::StringFormat("{}|cffffd700g|r {}|cffc7c7cfs|r {}|cffeda55fc|r", copper / 10000, (copper / 100) % 100, copper % 100);
    }

    bool IsGroupOff(Group const* group)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_groupOff.count(group->GetGUID().GetCounter()) > 0;
    }

    // Vienas daiktas vienam nariui. Grazina auksa (variu), gauta uz parduota pilka daikta, kai kuprine pilna.
    uint32 DeliverItem(Player* member, LootItem const& li, Creature* creature, AllowedLooterSet& looters, PersonalLootConfig const& c)
    {
        if (!li.count)
            return 0;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(li.itemid);
        if (!proto)
            return 0;

        // salygos, klases/rases/frakcijos apribojimai, uzduociu daiktai, jau zinomi receptai
        if (!li.AllowedForPlayer(member, creature->GetGUID()))
            return 0;

        ItemPosCountVec dest;
        InventoryResult const msg = member->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, li.itemid, li.count);
        if (msg == EQUIP_ERR_OK)
        {
            Item* item = member->StoreNewItem(dest, li.itemid, true, li.randomPropertyId, looters);
            if (!item)
                return 0;

            member->SendNewItem(item, li.count, false, false, proto->Quality >= c.broadcastQuality);
            sScriptMgr->OnPlayerLootItem(member, item, li.count, creature->GetGUID());
            return 0;
        }

        if (msg == EQUIP_ERR_INVENTORY_FULL)
        {
            if (c.greySell && proto->Quality == ITEM_QUALITY_POOR && proto->SellPrice)
                return proto->SellPrice * li.count;

            member->SendItemRetrievalMail(li.itemid, li.count);
            SendTo(member, Fmt(PL_STR_MAIL_FULL, ItemLink(proto)));
            return 0;
        }

        // unikalus daiktas (jau turi), neleistinas ir pan. - praleidziama tyliai (kaip standartiniame grobyje)
        return 0;
    }

    // Daiktas lieka sutverio lavone, bet ji mato ir gali paimti TIK `member` (LootItem::rollWinnerGUID).
    bool QueueWindowItem(Player* member, LootItem const& li, Creature* creature, AllowedLooterSet const& looters, std::vector<LootItem>& out)
    {
        if (!li.count || !sObjectMgr->GetItemTemplate(li.itemid))
            return false;

        // klases/rases/frakcijos apribojimai, salygos, jau zinomi receptai ir pan. - tikrinama dabar, rodymo metu - dar kartą
        if (!li.AllowedForPlayer(member, creature->GetGUID()))
            return false;

        LootItem item = li;
        item.rollWinnerGUID    = member->GetGUID();
        item.is_looted         = false;
        item.is_blocked        = false;
        item.is_underthreshold = false;
        item.freeforall        = false;
        item.needs_quest       = false;
        item.follow_loot_rules = false;
        item.conditions.clear();                // jau patikrinta; salygos nebeleidžia rodyti daikto bendrame lange
        item.allowedGUIDs      = looters;       // BoP daiktas 2 val. perduodamas siems grupes nariams
        out.push_back(std::move(item));
        return true;
    }

    void GiveMoney(Player* member, uint32 amount, bool shared)
    {
        if (!amount)
            return;

        member->ModifyMoney(amount);
        member->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, amount);

        WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
        data << uint32(amount);
        data << uint8(shared ? 0 : 1);       // 0 = "Jusu dalis", 1 = "Jus gaunate"
        member->GetSession()->SendPacket(&data);

        sScriptMgr->OnLootMoney(member, amount);
    }
}

class PersonalLootUnitScript : public UnitScript
{
public:
    PersonalLootUnitScript() : UnitScript("PersonalLootUnitScript", true, { UNITHOOK_ON_UNIT_DEATH }) { }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || creature->IsPet() || !creature->hasLootRecipient() || !creature->GetLootMode())
            return;

        PersonalLootConfig const c = Cfg();
        if (!c.enable)
            return;

        Group* group = creature->GetLootRecipientGroup();
        if (!group || group->isBGGroup() || group->isBFGroup())
            return;

        if (c.scope == 0 && !creature->GetMap()->IsDungeon())
            return;

        LootMethod const method = group->GetLootMethod();
        if (method == FREE_FOR_ALL || (method == MASTER_LOOT && c.respectMaster))
            return;

        if (IsGroupOff(group))
            return;

        CreatureTemplate const* cinfo = creature->GetCreatureTemplate();
        uint32 const lootId = cinfo->lootid;
        uint32 const gold = creature->loot.gold;
        if (!lootId && !gold)
            return;

        // teise gauti grobi turintys nariai
        std::vector<Player*> members;
        uint32 humans = 0;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsInWorld() || !member->GetSession())
                continue;

            bool const bot = member->GetSession()->IsBot();
            if (bot && !c.includeBots)
                continue;

            if (!member->IsAtLootRewardDistance(creature))
                continue;

            members.push_back(member);
            if (!bot)
                ++humans;
        }

        if (members.empty() || humans < c.minHumans)
            return;

        AllowedLooterSet looters;
        for (Player* member : members)
            looters.insert(member->GetGUID());

        // grobio lango rezimas: tik "Group Loot" / "Need Before Greed" grupese (kiti rezimai rodo daiktus visiems)
        bool const windowMode = !c.autoDeliver && (method == GROUP_LOOT || method == NEED_BEFORE_GREED);
        bool const shared = members.size() > 1;
        uint32 const share = gold / members.size();
        uint32 remainder = gold - share * members.size();
        uint16 const lootMode = creature->GetLootMode();

        std::vector<LootItem> windowItems;

        for (Player* member : members)
        {
            uint32 money = windowMode ? 0 : share + remainder;
            remainder = 0;
            uint32 soldGrey = 0;
            bool const bot = member->GetSession()->IsBot();

            if (lootId)
            {
                Loot personal;
                if (personal.FillLoot(lootId, LootTemplates_Creature, member, true, true, lootMode, creature))
                {
                    for (LootItem const& li : personal.items)
                    {
                        if (windowMode && !bot && windowItems.size() < MAX_NR_LOOT_ITEMS && QueueWindowItem(member, li, creature, looters, windowItems))
                            continue;
                        if (windowMode && !bot && windowItems.size() < MAX_NR_LOOT_ITEMS)
                            continue;       // daiktas netinka siam nariui (QueueWindowItem atmete) - praleidziama
                        soldGrey += DeliverItem(member, li, creature, looters, c);
                    }

                    // uzduociu daiktai visada tiesiai i kuprine
                    for (LootItem const& li : personal.quest_items)
                        soldGrey += DeliverItem(member, li, creature, looters, c);
                }
            }

            if (soldGrey)
                SendTo(member, Fmt(PL_STR_GREY_SOLD, MoneyText(soldGrey)));

            if (money + soldGrey)
                GiveMoney(member, money + soldGrey, windowMode ? false : shared);
        }

        if (windowMode)
        {
            // lavono grobis perstatomas: auksas (bendras, dalijamas kaip standartiniame grupes grobyje) + kiekvieno nario asmeniniai daiktai
            Loot& loot = creature->loot;
            uint32 const keepGold = loot.gold;
            loot.clear();
            loot.gold = keepGold;
            for (LootItem& item : windowItems)
            {
                item.itemIndex = uint32(loot.items.size());
                loot.items.push_back(item);
            }
            loot.unlootedCount = uint8(loot.items.size());
            loot.loot_type = LOOT_CORPSE;       // kitaip pirmas atidarymas paleistu Group Loot / Need Before Greed ridenima

            if (loot.isLooted())
            {
                creature->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                creature->AllLootRemovedFromCorpse();
            }
            else
                creature->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
        }
        else
        {
            // standartinis (bendras) lavono grobis nebereikalingas
            creature->loot.clear();
            creature->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
            creature->AllLootRemovedFromCorpse();
        }

        if (c.announce)
        {
            bool first = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                first = g_informed.insert(group->GetGUID().GetCounter()).second;
            }
            if (first)
                for (Player* member : members)
                    SendTo(member, Str(windowMode ? PL_STR_FIRST_NOTICE_WINDOW : PL_STR_FIRST_NOTICE));
        }
    }
};

class PersonalLootCommandScript : public CommandScript
{
public:
    PersonalLootCommandScript() : CommandScript("PersonalLootCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable lootTable =
        {
            { "",         HandleInfo,    SEC_PLAYER, Console::No },
            { "info",     HandleInfo,    SEC_PLAYER, Console::No },
            { "isjungti", HandleOff,     SEC_PLAYER, Console::No },
            { "ijungti",  HandleOn,      SEC_PLAYER, Console::No }
        };

        static ChatCommandTable rootTable =
        {
            { "grobis", lootTable }
        };

        return rootTable;
    }

    static Player* Me(ChatHandler* handler)
    {
        return handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
    }

    static bool HandleInfo(ChatHandler* handler)
    {
        Player* player = Me(handler);
        if (!player)
            return false;

        PersonalLootConfig const c = Cfg();
        Group* group = player->GetGroup();

        std::string groupState;
        if (!group)
            groupState = Str(PL_STR_GROUP_NONE);
        else if (group->GetLootMethod() == FREE_FOR_ALL)
            groupState = Str(PL_STR_GROUP_FFA);
        else if (group->GetLootMethod() == MASTER_LOOT && c.respectMaster)
            groupState = Str(PL_STR_GROUP_MASTER);
        else if (IsGroupOff(group))
            groupState = Str(PL_STR_GROUP_OFF);
        else
            groupState = Str(PL_STR_GROUP_ACTIVE);

        handler->SendSysMessage(Fmt(PL_STR_INFO, Str(c.enable ? PL_STR_SERVER_ON : PL_STR_SERVER_OFF),
                                    Str(c.scope == 0 ? PL_STR_SCOPE_INSTANCES : PL_STR_SCOPE_ALL), groupState));
        handler->SendSysMessage(Str(PL_STR_HELP));
        return true;
    }

    static bool Toggle(ChatHandler* handler, bool off)
    {
        Player* player = Me(handler);
        if (!player)
            return false;

        Group* group = player->GetGroup();
        if (!group)
        {
            handler->SendSysMessage(Str(PL_STR_NOT_IN_GROUP));
            return true;
        }

        if (!group->IsLeader(player->GetGUID()))
        {
            handler->SendSysMessage(Str(PL_STR_LEADER_ONLY));
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (off)
                g_groupOff.insert(group->GetGUID().GetCounter());
            else
                g_groupOff.erase(group->GetGUID().GetCounter());
        }

        std::string const text = Str(off ? PL_STR_LEADER_OFF : PL_STR_LEADER_ON);
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            SendTo(itr->GetSource(), text);
        return true;
    }

    static bool HandleOff(ChatHandler* handler) { return Toggle(handler, true); }
    static bool HandleOn(ChatHandler* handler)  { return Toggle(handler, false); }
};

void AddSC_PersonalLoot()
{
    new PersonalLootUnitScript();
    new PersonalLootCommandScript();
}
