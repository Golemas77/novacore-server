/*
 * NovaCore: mod-transmog - transmogrifikacija (daikto isvaizdos keitimas) kaip Retail.
 *
 * Zaidejas pas NPC „Isvaizdos meistre“ (pagrindiniuose miestuose) parenka is iranga lizdo (galva, peciai, nugara, krutine,
 * marskiniai, tabardas, riesai, rankos, juosmuo, kojos, pedos, pagrindine/salutine ranka, nuotolinis ginklas) bet kuria
 * SAVO PASKYROS kolekcijoje esancia tos pacios rusies daikto isvaizda. Daikto savybes nesikeicia - keiciasi tik matomas modelis.
 *
 * Taisykles (Retail principai):
 *   - isvaizda atrakinama ir saugoma VISAI PASKYRAI (lentele `account_transmog_collection`): ja atrakina daikto
 *     uzsidejimas, arba gavimas (jei daiktas susiristi ji gaunant / uzduoties daiktas), arba NPC aplankymas
 *     turint susietus (soulbound) daiktus kuprineje/banke;
 *   - tinka tik tos pacios grupes daiktai: tas pats ginklu tipas (1r / 2r / nuotolinis / lazdele / metamas), tas pats
 *     sarvu tipas (audinys / oda / grandine / plokste) ir ta pati vieta (galva, peciai ir t.t.), be to zaidejui turi
 *     tikti klase / rase / ginklo igudis;
 *   - pasirinkimas saugomas LIZDUI (`character_transmog`: zaidejas + lizdas -> daikto ID) ir pritaikomas bet kokiam
 *     tinkamam daiktui tame lizde; netinkamam daiktui jis tiesiog nerodomas;
 *   - keitimas kainuoja `Transmog.CostCopper` (numatyta 10 auksiniu; kaina rodoma NPC meniu ir patvirtinimo lange); nuemimas nemokamas;
 *   - kabliukas `OnPlayerAfterSetVisibleItemSlot` perraso matoma daikto ID (PLAYER_VISIBLE_ITEM_x_ENTRYID).
 *
 * Tekstai - `module_string` (world DB, `mod-transmog`). Boty (playerbots) sis modulis neliecia.
 */

#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* TM_MODULE = "mod-transmog";

    // npc_text ID (world DB, zr. data/sql/db-world/base/mod_transmog_npc.sql)
    constexpr uint32 TM_NPC_TEXT_MAIN     = 9000020;
    constexpr uint32 TM_NPC_TEXT_SLOT     = 9000021;
    constexpr uint32 TM_NPC_TEXT_DISABLED = 9000022;

    // module_string ID (zr. data/sql/db-world/base/mod_transmog_module_string.sql)
    enum TransmogString : uint32
    {
        TM_STR_SLOT_BASE        = 1,    // 1..19 - lizdu pavadinimai pagal EQUIPMENT_SLOT_* + 1
        TM_STR_EMPTY            = 30,
        TM_STR_SLOT_LINE        = 31,   // "%s: %s"
        TM_STR_SLOT_LINE_FAKE   = 32,   // "%s: %s -> %s"
        TM_STR_REMOVE_ALL       = 33,
        TM_STR_REMOVE_ALL_ASK   = 34,
        TM_STR_BACK             = 35,
        TM_STR_NEXT             = 36,
        TM_STR_PREV             = 37,
        TM_STR_SEARCH           = 38,
        TM_STR_SEARCH_ASK       = 39,
        TM_STR_SEARCH_CLEAR     = 40,
        TM_STR_REMOVE_SLOT      = 41,
        TM_STR_HEADER_REAL      = 42,   // "%s (%s): %s"  lizdas, isvaizda, daiktas
        TM_STR_LOOK_DEFAULT     = 43,
        TM_STR_LOOK_FAKE        = 44,   // "%s"
        TM_STR_PAGE             = 45,   // "Puslapis %u / %u, isvaizdu: %u"
        TM_STR_NONE_AVAILABLE   = 46,
        TM_STR_CONFIRM          = 47,   // "Pakeisti %s isvaizda i %s?"
        TM_STR_CONFIRM_FREE     = 48,
        TM_STR_APPLIED          = 49,   // "%s: isvaizda pakeista i %s."
        TM_STR_REMOVED          = 50,   // "%s: transmogrifikacija pasalinta."
        TM_STR_REMOVED_ALL      = 51,   // "Pasalintos visos transmogrifikacijos (%u)."
        TM_STR_NOT_ENOUGH_GOLD  = 52,
        TM_STR_NO_ITEM          = 53,
        TM_STR_NOT_COMPATIBLE   = 54,
        TM_STR_NOT_UNLOCKED     = 55,
        TM_STR_UNLOCKED         = 56,   // "Isvaizda atrakinta: %s."
        TM_STR_DISABLED         = 57,
        TM_STR_MUST_BE_ALIVE    = 58,
        TM_STR_INFO             = 59,   // "Transmogrifikacija: atrakinta isvaizdu: %u, pakeista lizdu: %u. Kaina: %s."
        TM_STR_COST_FREE        = 60,
        TM_STR_COST_GOLD        = 61,   // "%u auksinas(-ai)" ... formatuojama kode
        TM_STR_GM_UNLOCKED      = 62,   // "Atrakinta isvaizdu: %u (paskyra: %s)."
        TM_STR_GM_RESET         = 63,
        TM_STR_GM_NOT_FOUND     = 64,
        TM_STR_FILTER_ACTIVE    = 65,   // "Paieska: %s"
        TM_STR_NOTHING_TO_DO    = 66,
        TM_STR_BOTS_DENIED      = 67,
        TM_STR_COST_LINE        = 68    // "Vieno lizdo keitimo kaina: %s"
    };

    // Siuntejo kodai gossip meniu (action - pagal tipa).
    enum TransmogSender : uint32
    {
        TM_S_MAIN         = 1000,   // action: nenaudojamas - pagrindinis meniu
        TM_S_LIST         = 1001,   // action: lizdas | (puslapis << 8)
        TM_S_REMOVE       = 1002,   // action: lizdas
        TM_S_REMOVE_ALL   = 1003,
        TM_S_SEARCH       = 1004,   // action: lizdas (su kodo ivedimu)
        TM_S_SEARCH_CLEAR = 1005,   // action: lizdas
        TM_S_APPLY_BASE   = 1100    // sender = TM_S_APPLY_BASE + lizdas, action: daikto ID
    };

    // Lizdu eile meniu.
    constexpr std::array<uint8, 14> const g_slots =
    {
        EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_BACK, EQUIPMENT_SLOT_CHEST, EQUIPMENT_SLOT_BODY,
        EQUIPMENT_SLOT_TABARD, EQUIPMENT_SLOT_WRISTS, EQUIPMENT_SLOT_HANDS, EQUIPMENT_SLOT_WAIST, EQUIPMENT_SLOT_LEGS,
        EQUIPMENT_SLOT_FEET, EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED
    };

    struct TransmogConfig
    {
        bool enable;
        uint32 costCopper;
        uint32 minQuality;
        uint32 pageSize;
        bool announceUnlock;
    };

    TransmogConfig Cfg()
    {
        TransmogConfig c;
        c.enable         = sConfigMgr->GetOption<bool>("Transmog.Enable", true);
        c.costCopper     = sConfigMgr->GetOption<uint32>("Transmog.CostCopper", 100000);
        c.minQuality     = sConfigMgr->GetOption<uint32>("Transmog.MinQuality", 1);
        c.pageSize       = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Transmog.PageSize", 12), 4, 20);
        c.announceUnlock = sConfigMgr->GetOption<bool>("Transmog.AnnounceUnlock", true);
        return c;
    }

    std::shared_mutex g_mutex;
    std::unordered_map<uint32, std::unordered_set<uint32>> g_collection;                       // paskyra -> atrakinti daiktai
    std::unordered_map<uint32, std::array<uint32, EQUIPMENT_SLOT_END>> g_overrides;            // zaidejas -> lizdas -> daikto ID

    struct Browse
    {
        std::wstring filter;       // paieskos eilute (mazosiomis raidemis)
        std::string  filterText;   // kaip ivesta
    };
    std::unordered_map<uint32, Browse> g_browse;                                               // zaidejas -> paieska

    bool IsRealPlayer(Player* player)
    {
        return player && player->GetSession() && !player->GetSession()->IsBot();
    }

    std::string Str(uint32 id)
    {
        if (std::string const* s = sObjectMgr->GetModuleString(TM_MODULE, id, LOCALE_enUS))
            return *s;
        return "<transmog>";
    }

    template <typename... Args>
    std::string Fmt(uint32 id, Args&&... args)
    {
        std::string const format = Str(id);
        return Acore::StringFormat(format.c_str(), std::forward<Args>(args)...);
    }

    void SendTo(Player* player, std::string const& text)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    // ---------- daiktu taisykles ----------

    // Grupe: 0 = netinkamas. Tos paches grupes daiktai gali keistis isvaizdomis.
    uint32 VisualGroup(ItemTemplate const* p)
    {
        if (!p || !p->DisplayInfoID)
            return 0;

        if (p->Class == ITEM_CLASS_ARMOR)
        {
            switch (p->InventoryType)
            {
                case INVTYPE_HEAD:
                case INVTYPE_SHOULDERS:
                case INVTYPE_CHEST:
                case INVTYPE_ROBE:
                case INVTYPE_WAIST:
                case INVTYPE_LEGS:
                case INVTYPE_FEET:
                case INVTYPE_WRISTS:
                case INVTYPE_HANDS:
                {
                    uint32 const inv = (p->InventoryType == INVTYPE_ROBE) ? uint32(INVTYPE_CHEST) : p->InventoryType;
                    return 0x1000 | (inv << 4) | (p->SubClass & 0xF);
                }
                case INVTYPE_CLOAK:    return 0x2000;
                case INVTYPE_BODY:     return 0x2001;
                case INVTYPE_TABARD:   return 0x2002;
                case INVTYPE_SHIELD:   return 0x2003;
                case INVTYPE_HOLDABLE: return 0x2004;
                default:               return 0;
            }
        }

        if (p->Class == ITEM_CLASS_WEAPON)
        {
            switch (p->SubClass)
            {
                case ITEM_SUBCLASS_WEAPON_AXE:
                case ITEM_SUBCLASS_WEAPON_MACE:
                case ITEM_SUBCLASS_WEAPON_SWORD:
                case ITEM_SUBCLASS_WEAPON_FIST:
                case ITEM_SUBCLASS_WEAPON_DAGGER:
                    return 0x3000;   // vienos rankos
                case ITEM_SUBCLASS_WEAPON_AXE2:
                case ITEM_SUBCLASS_WEAPON_MACE2:
                case ITEM_SUBCLASS_WEAPON_POLEARM:
                case ITEM_SUBCLASS_WEAPON_SWORD2:
                case ITEM_SUBCLASS_WEAPON_STAFF:
                    return 0x3001;   // dvieju rankju
                case ITEM_SUBCLASS_WEAPON_BOW:
                case ITEM_SUBCLASS_WEAPON_GUN:
                case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                    return 0x3002;   // saudomieji
                case ITEM_SUBCLASS_WEAPON_WAND:
                    return 0x3003;
                case ITEM_SUBCLASS_WEAPON_THROWN:
                    return 0x3004;
                default:
                    return 0;
            }
        }

        return 0;
    }

    bool Collectable(ItemTemplate const* p, TransmogConfig const& cfg)
    {
        if (!p || !VisualGroup(p) || p->Name1.empty())
            return false;
        return p->Quality >= cfg.minQuality && p->Quality <= ITEM_QUALITY_HEIRLOOM;
    }

    // Ar zaidejas gali naudoti `source` isvaizda ant `target` (uzsidetas daiktas) daikto.
    bool Compatible(Player* player, ItemTemplate const* target, ItemTemplate const* source, bool checkPlayer)
    {
        uint32 const group = VisualGroup(target);
        if (!group || group != VisualGroup(source))
            return false;
        if (!checkPlayer)
            return true;

        if (source->AllowableClass && (source->AllowableClass & player->getClassMask()) == 0)
            return false;
        if (source->AllowableRace && (source->AllowableRace & player->getRaceMask()) == 0)
            return false;
        if (source->Class == ITEM_CLASS_WEAPON)
            if (uint32 const skill = source->GetSkill())
                if (!player->HasSkill(skill))
                    return false;
        return true;
    }

    std::string ItemLink(ItemTemplate const* p)
    {
        static char const* colors[] = { "9d9d9d", "ffffff", "1eff00", "0070dd", "a335ee", "ff8000", "e6cc80", "e6cc80" };
        char const* color = colors[std::min<uint32>(p->Quality, 7)];
        return Acore::StringFormat("|cff{}|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r", color, p->ItemId, p->Name1);
    }

    // Lietuviska daugiskaita: 1 auksinas, 2-9 auksinai, 10-20 auksiniu, 21 auksinas ...
    char const* CoinWord(uint32 n, char const* one, char const* few, char const* many)
    {
        uint32 const last = n % 10;
        uint32 const tail = n % 100;
        if (last == 1 && tail != 11)
            return one;
        if (last == 0 || (tail >= 11 && tail <= 19))
            return many;
        return few;
    }

    // Kaina zodziais ir spalvomis (pvz. „10 auksiniu“), kad zaidejas is karto matytu, kiek sumokes.
    std::string CostText(uint32 copper)
    {
        if (!copper)
            return Str(TM_STR_COST_FREE);
        uint32 const g = copper / 10000;
        uint32 const s = (copper / 100) % 100;
        uint32 const c = copper % 100;
        std::string out;
        auto add = [&out](uint32 n, char const* color, char const* word)
        {
            if (!out.empty())
                out += ' ';
            out += Acore::StringFormat("|cff{}{} {}|r", color, n, word);
        };
        if (g) add(g, "ffd700", CoinWord(g, "auksinas", "auksinai", "auksinių"));
        if (s) add(s, "c7c7cf", CoinWord(s, "sidabrinis", "sidabriniai", "sidabrinių"));
        if (c) add(c, "eda55f", CoinWord(c, "varinis", "variniai", "varinių"));
        return out;
    }

    // ---------- duomenys ----------

    std::unordered_set<uint32>& CollectionFor(uint32 accountId)
    {
        // kviesti tik laikant g_mutex (unique)
        return g_collection[accountId];
    }

    void LoadCollection(uint32 accountId)
    {
        {
            std::shared_lock<std::shared_mutex> lock(g_mutex);
            if (g_collection.count(accountId))
                return;
        }

        std::unordered_set<uint32> loaded;
        if (QueryResult result = CharacterDatabase.Query("SELECT `item_entry` FROM `account_transmog_collection` WHERE `account_id` = {}", accountId))
        {
            do
            {
                loaded.insert((*result)[0].Get<uint32>());
            } while (result->NextRow());
        }

        std::unique_lock<std::shared_mutex> lock(g_mutex);
        g_collection.emplace(accountId, std::move(loaded));
    }

    bool IsUnlocked(uint32 accountId, uint32 entry)
    {
        std::shared_lock<std::shared_mutex> lock(g_mutex);
        auto it = g_collection.find(accountId);
        return it != g_collection.end() && it->second.count(entry) > 0;
    }

    // Grazina true, jei isvaizda buvo nauja.
    bool Unlock(Player* player, uint32 entry, bool announce)
    {
        if (!IsRealPlayer(player))
            return false;

        TransmogConfig const cfg = Cfg();
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!Collectable(proto, cfg))
            return false;

        uint32 const accountId = player->GetSession()->GetAccountId();
        LoadCollection(accountId);

        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            if (!CollectionFor(accountId).insert(entry).second)
                return false;
        }

        CharacterDatabase.Execute("INSERT IGNORE INTO `account_transmog_collection` (`account_id`, `item_entry`) VALUES ({}, {})", accountId, entry);

        if (announce && cfg.announceUnlock)
            SendTo(player, Fmt(TM_STR_UNLOCKED, ItemLink(proto)));
        return true;
    }

    uint32 GetOverride(uint32 guid, uint8 slot)
    {
        std::shared_lock<std::shared_mutex> lock(g_mutex);
        auto it = g_overrides.find(guid);
        return it == g_overrides.end() ? 0 : it->second[slot];
    }

    void SetOverride(uint32 guid, uint8 slot, uint32 entry)
    {
        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            auto& arr = g_overrides[guid];
            arr[slot] = entry;
        }

        if (entry)
            CharacterDatabase.Execute("REPLACE INTO `character_transmog` (`guid`, `slot`, `item_entry`) VALUES ({}, {}, {})", guid, uint32(slot), entry);
        else
            CharacterDatabase.Execute("DELETE FROM `character_transmog` WHERE `guid` = {} AND `slot` = {}", guid, uint32(slot));
    }

    uint32 OverrideCount(uint32 guid)
    {
        std::shared_lock<std::shared_mutex> lock(g_mutex);
        auto it = g_overrides.find(guid);
        if (it == g_overrides.end())
            return 0;
        uint32 n = 0;
        for (uint32 v : it->second)
            n += v ? 1 : 0;
        return n;
    }

    void Refresh(Player* player, uint8 slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        player->SetVisibleItemSlot(slot, item);   // kabliukas pats uzdes isvaizda
    }

    // Visi zaidejo turimi daiktai: ekipiruote, kuprine, krepsiai, bankas.
    template <typename Fn>
    void ForEachOwnedItem(Player* player, Fn&& fn)
    {
        auto bagItems = [&](uint8 bagSlot)
        {
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    if (Item* item = bag->GetItemByPos(i))
                        fn(item);
        };

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(item);
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(item);
        for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(item);
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            bagItems(slot);
        for (uint8 slot = BANK_SLOT_BAG_START; slot < BANK_SLOT_BAG_END; ++slot)
            bagItems(slot);
    }

    // NPC aplankymo metu: atrakina susietu (soulbound) daiktu isvaizdas.
    uint32 ScanBoundItems(Player* player)
    {
        uint32 added = 0;
        ForEachOwnedItem(player, [&](Item* item)
        {
            if (item->IsSoulBound() && Unlock(player, item->GetEntry(), false))
                ++added;
        });
        return added;
    }

    // ---------- vaizdas (gossip) ----------

    struct Candidate
    {
        ItemTemplate const* proto;
        std::wstring lowerName;
    };

    std::vector<ItemTemplate const*> BuildList(Player* player, uint8 slot, std::wstring const& filter)
    {
        std::vector<ItemTemplate const*> out;
        Item* equipped = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!equipped)
            return out;

        ItemTemplate const* target = equipped->GetTemplate();
        uint32 const guid = player->GetGUID().GetCounter();
        uint32 const fake = GetOverride(guid, slot);
        ItemTemplate const* fakeProto = fake ? sObjectMgr->GetItemTemplate(fake) : nullptr;
        uint32 const currentDisplay = fakeProto ? fakeProto->DisplayInfoID : target->DisplayInfoID;

        std::vector<uint32> entries;
        {
            std::shared_lock<std::shared_mutex> lock(g_mutex);
            auto it = g_collection.find(player->GetSession()->GetAccountId());
            if (it != g_collection.end())
                entries.assign(it->second.begin(), it->second.end());
        }

        std::vector<ItemTemplate const*> all;
        for (uint32 entry : entries)
        {
            ItemTemplate const* p = sObjectMgr->GetItemTemplate(entry);
            if (!p || !Compatible(player, target, p, true) || p->DisplayInfoID == currentDisplay)
                continue;
            all.push_back(p);
        }

        // geriausia kokybe pirma, toliau pagal pavadinima, ta pati modelis - tik viena karta
        std::sort(all.begin(), all.end(), [](ItemTemplate const* a, ItemTemplate const* b)
        {
            if (a->Quality != b->Quality)
                return a->Quality > b->Quality;
            if (a->Name1 != b->Name1)
                return a->Name1 < b->Name1;
            return a->ItemId < b->ItemId;
        });

        std::unordered_set<uint32> seenDisplays;
        for (ItemTemplate const* p : all)
        {
            if (!seenDisplays.insert(p->DisplayInfoID).second)
                continue;
            if (!filter.empty())
            {
                std::wstring name;
                if (!Utf8toWStr(p->Name1, name))
                    continue;
                wstrToLower(name);
                if (name.find(filter) == std::wstring::npos)
                    continue;
            }
            out.push_back(p);
        }
        return out;
    }

    void ShowMain(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);

        TransmogConfig const cfg = Cfg();
        if (!cfg.enable)
        {
            SendGossipMenuFor(player, TM_NPC_TEXT_DISABLED, creature);
            return;
        }

        // kaina rodoma is karto, kad zaidejas zinotu, kiek kainuos keitimas
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, Fmt(TM_STR_COST_LINE, CostText(cfg.costCopper)), TM_S_MAIN, 0);

        uint32 const guid = player->GetGUID().GetCounter();
        for (uint8 slot : g_slots)
        {
            std::string const slotName = Str(TM_STR_SLOT_BASE + slot);
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            std::string line;
            if (!item)
                line = Fmt(TM_STR_SLOT_LINE, slotName, Str(TM_STR_EMPTY));
            else
            {
                uint32 const fake = GetOverride(guid, slot);
                ItemTemplate const* fakeProto = fake ? sObjectMgr->GetItemTemplate(fake) : nullptr;
                if (fakeProto && VisualGroup(fakeProto) == VisualGroup(item->GetTemplate()))
                    line = Fmt(TM_STR_SLOT_LINE_FAKE, slotName, ItemLink(item->GetTemplate()), ItemLink(fakeProto));
                else
                    line = Fmt(TM_STR_SLOT_LINE, slotName, ItemLink(item->GetTemplate()));
            }
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, line, TM_S_LIST, uint32(slot));
        }

        if (OverrideCount(guid))
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Str(TM_STR_REMOVE_ALL), TM_S_REMOVE_ALL, 0, Str(TM_STR_REMOVE_ALL_ASK), 0, false);

        SendGossipMenuFor(player, TM_NPC_TEXT_MAIN, creature);
    }

    void ShowSlot(Player* player, Creature* creature, uint8 slot, uint32 page)
    {
        ClearGossipMenuFor(player);

        TransmogConfig const cfg = Cfg();
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
        {
            SendTo(player, Fmt(TM_STR_NO_ITEM, Str(TM_STR_SLOT_BASE + slot)));
            ShowMain(player, creature);
            return;
        }

        uint32 const guid = player->GetGUID().GetCounter();
        std::wstring filter;
        std::string filterText;
        {
            std::shared_lock<std::shared_mutex> lock(g_mutex);
            auto it = g_browse.find(guid);
            if (it != g_browse.end())
            {
                filter = it->second.filter;
                filterText = it->second.filterText;
            }
        }

        std::vector<ItemTemplate const*> const list = BuildList(player, slot, filter);
        uint32 const pages = std::max<uint32>(1, (list.size() + cfg.pageSize - 1) / cfg.pageSize);
        page = std::min(page, pages - 1);

        uint32 const fake = GetOverride(guid, slot);
        ItemTemplate const* fakeProto = fake ? sObjectMgr->GetItemTemplate(fake) : nullptr;
        bool const fakeActive = fakeProto && VisualGroup(fakeProto) == VisualGroup(item->GetTemplate());

        std::string header = Fmt(TM_STR_HEADER_REAL, Str(TM_STR_SLOT_BASE + slot),
                                 fakeActive ? Fmt(TM_STR_LOOK_FAKE, ItemLink(fakeProto)) : Str(TM_STR_LOOK_DEFAULT),
                                 ItemLink(item->GetTemplate()));
        AddGossipItemFor(player, GOSSIP_ICON_DOT, header, TM_S_LIST, uint32(slot) | (page << 8));
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, Fmt(TM_STR_COST_LINE, CostText(cfg.costCopper)), TM_S_LIST, uint32(slot) | (page << 8));

        if (list.empty())
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(TM_STR_NONE_AVAILABLE), TM_S_LIST, uint32(slot));

        uint32 const begin = page * cfg.pageSize;
        uint32 const end = std::min<uint32>(begin + cfg.pageSize, list.size());
        for (uint32 i = begin; i < end; ++i)
        {
            ItemTemplate const* p = list[i];
            std::string const confirm = Fmt(TM_STR_CONFIRM, ItemLink(item->GetTemplate()), ItemLink(p), CostText(cfg.costCopper));
            AddGossipItemFor(player, GOSSIP_ICON_TABARD, ItemLink(p), TM_S_APPLY_BASE + slot, p->ItemId, confirm, cfg.costCopper, false);
        }

        if (page > 0)
            AddGossipItemFor(player, GOSSIP_ICON_TALK, Str(TM_STR_PREV), TM_S_LIST, uint32(slot) | ((page - 1) << 8));
        if (page + 1 < pages)
            AddGossipItemFor(player, GOSSIP_ICON_TALK, Fmt(TM_STR_NEXT, page + 2, pages), TM_S_LIST, uint32(slot) | ((page + 1) << 8));

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(TM_STR_SEARCH), TM_S_SEARCH, uint32(slot), Str(TM_STR_SEARCH_ASK), 0, true);
        if (!filter.empty())
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Fmt(TM_STR_SEARCH_CLEAR, filterText), TM_S_SEARCH_CLEAR, uint32(slot));
        if (fakeActive)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Str(TM_STR_REMOVE_SLOT), TM_S_REMOVE, uint32(slot));
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(TM_STR_BACK), TM_S_MAIN, 0);

        SendGossipMenuFor(player, TM_NPC_TEXT_SLOT, creature);
    }

    bool TryApply(Player* player, uint8 slot, uint32 entry)
    {
        TransmogConfig const cfg = Cfg();
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
        {
            SendTo(player, Fmt(TM_STR_NO_ITEM, Str(TM_STR_SLOT_BASE + slot)));
            return false;
        }

        ItemTemplate const* source = sObjectMgr->GetItemTemplate(entry);
        if (!source || !Collectable(source, cfg))
            return false;
        if (!IsUnlocked(player->GetSession()->GetAccountId(), entry))
        {
            SendTo(player, Str(TM_STR_NOT_UNLOCKED));
            return false;
        }
        if (!Compatible(player, item->GetTemplate(), source, true))
        {
            SendTo(player, Str(TM_STR_NOT_COMPATIBLE));
            return false;
        }
        if (cfg.costCopper && !player->HasEnoughMoney(cfg.costCopper))
        {
            SendTo(player, Fmt(TM_STR_NOT_ENOUGH_GOLD, CostText(cfg.costCopper)));
            return false;
        }

        if (cfg.costCopper)
            player->ModifyMoney(-int32(cfg.costCopper));

        SetOverride(player->GetGUID().GetCounter(), slot, entry);
        Refresh(player, slot);
        SendTo(player, Fmt(TM_STR_APPLIED, Str(TM_STR_SLOT_BASE + slot), ItemLink(source), CostText(cfg.costCopper)));
        return true;
    }
}

class TransmogPlayerScript : public PlayerScript
{
public:
    TransmogPlayerScript() : PlayerScript("TransmogPlayerScript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_AFTER_SET_VISIBLE_ITEM_SLOT,
            PLAYERHOOK_ON_EQUIP,
            PLAYERHOOK_ON_LOOT_ITEM,
            PLAYERHOOK_ON_STORE_NEW_ITEM,
            PLAYERHOOK_ON_CREATE_ITEM,
            PLAYERHOOK_ON_QUEST_REWARD_ITEM,
            PLAYERHOOK_ON_DELETE_FROM_DB
        }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!IsRealPlayer(player) || !Cfg().enable)
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        LoadCollection(player->GetSession()->GetAccountId());

        std::array<uint32, EQUIPMENT_SLOT_END> loaded{};
        bool any = false;
        if (QueryResult result = CharacterDatabase.Query("SELECT `slot`, `item_entry` FROM `character_transmog` WHERE `guid` = {}", guid))
        {
            do
            {
                Field* f = result->Fetch();
                uint8 const slot = f[0].Get<uint8>();
                if (slot < EQUIPMENT_SLOT_END)
                {
                    loaded[slot] = f[1].Get<uint32>();
                    any = true;
                }
            } while (result->NextRow());
        }

        if (any)
        {
            {
                std::unique_lock<std::shared_mutex> lock(g_mutex);
                g_overrides[guid] = loaded;
            }
            for (uint8 slot : g_slots)
                if (loaded[slot])
                    Refresh(player, slot);
        }

        // jau uzsidetu daiktu isvaizdos (seniems veikejams)
        for (uint8 slot : g_slots)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                Unlock(player, item->GetEntry(), false);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;
        uint32 const guid = player->GetGUID().GetCounter();
        std::unique_lock<std::shared_mutex> lock(g_mutex);
        g_overrides.erase(guid);
        g_browse.erase(guid);
    }

    void OnPlayerAfterSetVisibleItemSlot(Player* player, uint8 slot, Item* item) override
    {
        if (!item || slot >= EQUIPMENT_SLOT_END || !IsRealPlayer(player))
            return;

        uint32 const fake = GetOverride(player->GetGUID().GetCounter(), slot);
        if (!fake)
            return;

        ItemTemplate const* fakeProto = sObjectMgr->GetItemTemplate(fake);
        if (!fakeProto || VisualGroup(fakeProto) != VisualGroup(item->GetTemplate()))
            return;

        player->SetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + (slot * 2), fake);
    }

    void OnPlayerEquip(Player* player, Item* item, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
    {
        if (item && IsRealPlayer(player))
            Unlock(player, item->GetEntry(), true);
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid /*lootguid*/) override { UnlockIfBound(player, item); }
    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 /*count*/) override { UnlockIfBound(player, item); }
    void OnPlayerCreateItem(Player* player, Item* item, uint32 /*count*/) override { UnlockIfBound(player, item); }
    void OnPlayerQuestRewardItem(Player* player, Item* item, uint32 /*count*/) override { UnlockIfBound(player, item); }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `character_transmog` WHERE `guid` = {}", guid);
    }

private:
    // Susiriancius gavimo metu daiktus (BoP / uzduociu) atrakina iskart.
    static void UnlockIfBound(Player* player, Item* item)
    {
        if (!item || !IsRealPlayer(player))
            return;
        ItemTemplate const* p = item->GetTemplate();
        if (p && (p->Bonding == BIND_WHEN_PICKED_UP || p->Bonding == BIND_QUEST_ITEM))
            Unlock(player, item->GetEntry(), true);
    }
};

class npc_transmog_script : public CreatureScript
{
public:
    npc_transmog_script() : CreatureScript("npc_transmog") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!IsRealPlayer(player))
            return true;

        if (Cfg().enable)
        {
            LoadCollection(player->GetSession()->GetAccountId());
            ScanBoundItems(player);
        }
        ShowMain(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (!IsRealPlayer(player) || !Cfg().enable)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        if (!player->IsAlive())
        {
            SendTo(player, Str(TM_STR_MUST_BE_ALIVE));
            CloseGossipMenuFor(player);
            return true;
        }

        uint32 const guid = player->GetGUID().GetCounter();

        if (sender >= TM_S_APPLY_BASE && sender < TM_S_APPLY_BASE + EQUIPMENT_SLOT_END)
        {
            uint8 const slot = uint8(sender - TM_S_APPLY_BASE);
            TryApply(player, slot, action);
            ShowSlot(player, creature, slot, 0);
            return true;
        }

        switch (sender)
        {
            case TM_S_MAIN:
            {
                std::unique_lock<std::shared_mutex> lock(g_mutex);
                g_browse.erase(guid);
                lock.unlock();
                ShowMain(player, creature);
                break;
            }
            case TM_S_LIST:
            {
                uint8 const slot = uint8(action & 0xFF);
                if (slot >= EQUIPMENT_SLOT_END)
                {
                    ShowMain(player, creature);
                    break;
                }
                ShowSlot(player, creature, slot, action >> 8);
                break;
            }
            case TM_S_REMOVE:
            {
                uint8 const slot = uint8(action & 0xFF);
                if (slot < EQUIPMENT_SLOT_END && GetOverride(guid, slot))
                {
                    SetOverride(guid, slot, 0);
                    Refresh(player, slot);
                    SendTo(player, Fmt(TM_STR_REMOVED, Str(TM_STR_SLOT_BASE + slot)));
                }
                ShowSlot(player, creature, slot < EQUIPMENT_SLOT_END ? slot : 0, 0);
                break;
            }
            case TM_S_REMOVE_ALL:
            {
                uint32 n = 0;
                for (uint8 slot : g_slots)
                {
                    if (GetOverride(guid, slot))
                    {
                        SetOverride(guid, slot, 0);
                        Refresh(player, slot);
                        ++n;
                    }
                }
                SendTo(player, n ? Fmt(TM_STR_REMOVED_ALL, n) : Str(TM_STR_NOTHING_TO_DO));
                ShowMain(player, creature);
                break;
            }
            case TM_S_SEARCH:   // tuscias kodo laukelis ateina kaip paprastas pasirinkimas
            {
                ShowSlot(player, creature, uint8(std::min<uint32>(action & 0xFF, EQUIPMENT_SLOT_END - 1)), 0);
                break;
            }
            case TM_S_SEARCH_CLEAR:
            {
                {
                    std::unique_lock<std::shared_mutex> lock(g_mutex);
                    g_browse.erase(guid);
                }
                ShowSlot(player, creature, uint8(action & 0xFF), 0);
                break;
            }
            default:
                CloseGossipMenuFor(player);
                break;
        }
        return true;
    }

    bool OnGossipSelectCode(Player* player, Creature* creature, uint32 sender, uint32 action, char const* code) override
    {
        if (!IsRealPlayer(player) || !Cfg().enable || sender != TM_S_SEARCH)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        uint8 const slot = uint8(action & 0xFF);
        std::string text = code ? code : "";
        if (text.size() > 40)
            text.resize(40);

        std::wstring wide;
        Browse b;
        if (!text.empty() && Utf8toWStr(text, wide))
        {
            wstrToLower(wide);
            b.filter = wide;
            b.filterText = text;
        }

        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            uint32 const guid = player->GetGUID().GetCounter();
            if (b.filter.empty())
                g_browse.erase(guid);
            else
                g_browse[guid] = std::move(b);
        }

        ShowSlot(player, creature, slot < EQUIPMENT_SLOT_END ? slot : 0, 0);
        return true;
    }
};

class TransmogCommandScript : public CommandScript
{
public:
    TransmogCommandScript() : CommandScript("TransmogCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable transmogTable =
        {
            { "",          HandleInfo,      SEC_PLAYER,     Console::No },
            { "info",      HandleInfo,      SEC_PLAYER,     Console::No },
            { "reset",     HandleReset,     SEC_PLAYER,     Console::No },
            { "unlock",    HandleUnlock,    SEC_GAMEMASTER, Console::No },
            { "unlockall", HandleUnlockAll, SEC_GAMEMASTER, Console::No }
        };

        static ChatCommandTable rootTable =
        {
            { "transmog", transmogTable }
        };

        return rootTable;
    }

    static bool HandleInfo(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!IsRealPlayer(player))
            return false;

        LoadCollection(player->GetSession()->GetAccountId());
        uint32 collected = 0;
        {
            std::shared_lock<std::shared_mutex> lock(g_mutex);
            auto it = g_collection.find(player->GetSession()->GetAccountId());
            if (it != g_collection.end())
                collected = it->second.size();
        }
        handler->SendSysMessage(Fmt(TM_STR_INFO, collected, OverrideCount(player->GetGUID().GetCounter()), CostText(Cfg().costCopper)));
        return true;
    }

    // Pasalina visas savo transmogrifikacijas (kaip pas NPC, bet is bet kur).
    static bool HandleReset(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!IsRealPlayer(player))
            return false;

        uint32 const guid = player->GetGUID().GetCounter();
        uint32 n = 0;
        for (uint8 slot : g_slots)
        {
            if (GetOverride(guid, slot))
            {
                SetOverride(guid, slot, 0);
                Refresh(player, slot);
                ++n;
            }
        }
        handler->SendSysMessage(n ? Fmt(TM_STR_REMOVED_ALL, n) : Str(TM_STR_NOTHING_TO_DO));
        return true;
    }

    static bool HandleUnlock(ChatHandler* handler, uint32 itemId)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!IsRealPlayer(player))
            return false;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
        {
            handler->SendSysMessage(Str(TM_STR_GM_NOT_FOUND));
            return true;
        }
        if (!Unlock(player, itemId, true))
            handler->SendSysMessage(Str(TM_STR_NOTHING_TO_DO));
        return true;
    }

    // Testavimui: atrakina VISAS galimas isvaizdas savo paskyrai.
    static bool HandleUnlockAll(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!IsRealPlayer(player))
            return false;

        TransmogConfig const cfg = Cfg();
        uint32 const accountId = player->GetSession()->GetAccountId();
        LoadCollection(accountId);

        std::vector<uint32> added;
        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            auto& set = CollectionFor(accountId);
            for (auto const& kv : *sObjectMgr->GetItemTemplateStore())
                if (Collectable(&kv.second, cfg) && set.insert(kv.first).second)
                    added.push_back(kv.first);
        }

        // irasymas DB paketais
        for (size_t i = 0; i < added.size(); i += 500)
        {
            std::string sql = "INSERT IGNORE INTO `account_transmog_collection` (`account_id`, `item_entry`) VALUES ";
            size_t const end = std::min(added.size(), i + 500);
            for (size_t j = i; j < end; ++j)
            {
                if (j > i)
                    sql += ',';
                sql += Acore::StringFormat("({}, {})", accountId, added[j]);
            }
            CharacterDatabase.Execute(sql);
        }

        handler->SendSysMessage(Fmt(TM_STR_GM_UNLOCKED, uint32(added.size()), accountId));
        return true;
    }
};

void AddSC_Transmog()
{
    new TransmogPlayerScript();
    new npc_transmog_script();
    new TransmogCommandScript();
}
