/*
 * NovaCore: mod-gm-island - GM testu sala (map 1, "GM Island").
 *
 *   - prieiga: i sala gali patekti tik GM paskyros (security >= GAMEMASTER) ir jų grupės nariai (botai); kitiems
 *     teleportas atmetamas, o jei jie vis tiek atsiduria saloje (prisijungimas, zonos pasikeitimas) - iskeliami;
 *   - komanda `.testsala` (GM) - nukelia i sala ir atsimena, is kur atvykta; `.testsala atgal` - grazina;
 *   - NPC "Visu uzkerejimu meistras" (npc_gmisland_enchanter) - uzkeria ekipiruotus daiktus (duomenys lenteleje gm_island_enchant,
 *     ja sugeneruoja .tools/build_gm_island.py), prideda prizminius lizdus;
 *   - NPC "Testuotojo pagalbininkas" (npc_gmisland_helper) - auksas, 80 lygis, atstatymas, itaisu/profesiju iki maks., teleportai.
 *   - Daiktu prekeivis (creature 9000300) - grynas DB: gossip meniu -> virtualus npc_vendor (be C++).
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // GM Island (game_tele GMIsland): Kalimdoro (map 1) siaurės vakarų kampas
    constexpr uint32 ISLAND_MAP = 1;
    constexpr float ISLAND_X = 16226.2f, ISLAND_Y = 16257.0f, ISLAND_Z = 13.2022f, ISLAND_O = 1.65007f;
    constexpr float ISLAND_BOX_MIN = 15900.0f;

    // npc_text (zr. build_gm_island.py)
    constexpr uint32 TEXT_HELPER = 9100004;
    constexpr uint32 TEXT_ENCHANTER = 9100005;
    constexpr uint32 TEXT_ENCHANT_LIST = 9100006;
    constexpr uint32 TEXT_TRAINER = 9100007;

    struct GmIslandConfig
    {
        bool enable;
        bool allowGroupWithGm;
        uint32 gold;
    };

    GmIslandConfig Cfg()
    {
        GmIslandConfig c;
        c.enable           = sConfigMgr->GetOption<bool>("GmIsland.Enable", true);
        c.allowGroupWithGm = sConfigMgr->GetOption<bool>("GmIsland.AllowGroupWithGM", true);
        c.gold             = sConfigMgr->GetOption<uint32>("GmIsland.Gold", 200000);
        return c;
    }

    bool InIsland(uint32 mapId, float x, float y)
    {
        return mapId == ISLAND_MAP && x >= ISLAND_BOX_MIN && y >= ISLAND_BOX_MIN;
    }

    bool IsGmAccount(Player* player)
    {
        return player && player->GetSession() && player->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    }

    // Salos prieiga: GM paskyra arba grupe, kurioje yra GM paskyra (pvz. botai su GM).
    bool CanUseIsland(Player* player)
    {
        if (!Cfg().enable || IsGmAccount(player))
            return true;

        if (!Cfg().allowGroupWithGm || !player)
            return false;

        if (Group* group = player->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    if (member != player && IsGmAccount(member))
                        return true;

        return false;
    }

    void Say(Player* player, std::string const& text)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    struct SavedLocation
    {
        uint32 map;
        float x, y, z, o;
    };
    std::unordered_map<uint32, SavedLocation> g_return;     // guid -> kur buvo prieš nukeliant (tik atmintyje)

    void SendHome(Player* player)
    {
        player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ, 0.0f);
    }

    bool TeleportToIsland(Player* player)
    {
        if (!InIsland(player->GetMapId(), player->GetPositionX(), player->GetPositionY()))
            g_return[player->GetGUID().GetCounter()] = { player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                                                          player->GetPositionZ(), player->GetOrientation() };

        return player->TeleportTo(ISLAND_MAP, ISLAND_X, ISLAND_Y, ISLAND_Z, ISLAND_O);
    }

    bool TeleportBack(Player* player)
    {
        auto it = g_return.find(player->GetGUID().GetCounter());
        if (it == g_return.end())
        {
            SendHome(player);
            return true;
        }

        SavedLocation const loc = it->second;
        return player->TeleportTo(loc.map, loc.x, loc.y, loc.z, loc.o);
    }

    // ------------------------------------------------------------ užkerėjimai
    struct EnchantRow
    {
        uint32 spellId;
        uint32 enchantId;
        std::string label;
        int32 itemClass;
        uint32 subMask;
        uint32 invMask;
        bool prismatic;
    };

    std::vector<EnchantRow> g_enchants;

    void LoadEnchants()
    {
        g_enchants.clear();
        QueryResult result = WorldDatabase.Query(
            "SELECT `spell_id`, `enchant_id`, `label`, `item_class`, `sub_mask`, `inv_mask`, `kind` FROM `gm_island_enchant` ORDER BY `kind`, `sort_key`, `label`");
        if (!result)
        {
            LOG_WARN("module", "mod-gm-island: lentelė `gm_island_enchant` tuščia arba neegzistuoja (.tools/build_gm_island.py --rasyti)");
            return;
        }

        do
        {
            Field* f = result->Fetch();
            EnchantRow row;
            row.spellId   = f[0].Get<uint32>();
            row.enchantId = f[1].Get<uint32>();
            row.label     = f[2].Get<std::string>();
            row.itemClass = f[3].Get<int32>();
            row.subMask   = f[4].Get<uint32>();
            row.invMask   = f[5].Get<uint32>();
            row.prismatic = f[6].Get<std::string>() == "prismatic";
            g_enchants.push_back(std::move(row));
        } while (result->NextRow());

        LOG_INFO("module", "mod-gm-island: įkelta {} užkerėjimų", g_enchants.size());
    }

    bool Eligible(ItemTemplate const* proto, EnchantRow const& row)
    {
        if (!proto)
            return false;

        if (row.itemClass >= 0 && int32(proto->Class) != row.itemClass)
            return false;

        if (row.subMask && !(row.subMask & (1u << proto->SubClass)))
            return false;

        if (row.invMask && !(row.invMask & (1u << proto->InventoryType)))
            return false;

        return true;
    }

    struct SlotInfo
    {
        uint8 slot;
        char const* label;
    };

    constexpr SlotInfo SLOTS[] =
    {
        { EQUIPMENT_SLOT_HEAD, "Galva" }, { EQUIPMENT_SLOT_SHOULDERS, "Pečiai" }, { EQUIPMENT_SLOT_BACK, "Nugara" },
        { EQUIPMENT_SLOT_CHEST, "Krūtinė" }, { EQUIPMENT_SLOT_WRISTS, "Riešai" }, { EQUIPMENT_SLOT_HANDS, "Plaštakos" },
        { EQUIPMENT_SLOT_WAIST, "Juosmuo" }, { EQUIPMENT_SLOT_LEGS, "Kojos" }, { EQUIPMENT_SLOT_FEET, "Pėdos" },
        { EQUIPMENT_SLOT_NECK, "Kaklas" }, { EQUIPMENT_SLOT_FINGER1, "Pirmas pirštas" }, { EQUIPMENT_SLOT_FINGER2, "Antras pirštas" },
        { EQUIPMENT_SLOT_TRINKET1, "Pirmas papuošalas" }, { EQUIPMENT_SLOT_TRINKET2, "Antras papuošalas" },
        { EQUIPMENT_SLOT_MAINHAND, "Pagrindinė ranka" }, { EQUIPMENT_SLOT_OFFHAND, "Antra ranka" }, { EQUIPMENT_SLOT_RANGED, "Tolimoji" }
    };

    char const* SlotLabel(uint8 slot)
    {
        for (SlotInfo const& s : SLOTS)
            if (s.slot == slot)
                return s.label;
        return "Daiktas";
    }

    // gossip sender kodai
    enum : uint32
    {
        SENDER_SLOT   = 100,        // action = ekipiruotės vieta -> rodyti užkerėjimus
        SENDER_PAGE   = 200,        // action = vieta * 100 + puslapis
        SENDER_CLEAR  = 300,        // action = ekipiruotės vieta -> pašalinti užkerėjimus
        SENDER_BACK   = 400,
        SENDER_APPLY  = 1000        // sender = 1000 + vieta, action = spell_id
    };

    constexpr uint32 PAGE_SIZE = 25;
}

// ============================================================ prieiga
class GmIslandPlayerScript : public PlayerScript
{
public:
    GmIslandPlayerScript() : PlayerScript("GmIslandPlayerScript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_UPDATE_ZONE,
            PLAYERHOOK_ON_BEFORE_TELEPORT
        }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (player && InIsland(player->GetMapId(), player->GetPositionX(), player->GetPositionY()) && !CanUseIsland(player))
            SendHome(player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        if (player && InIsland(player->GetMapId(), player->GetPositionX(), player->GetPositionY()) && !CanUseIsland(player))
        {
            Say(player, "|cffff8000[GM sala]|r Čia gali būti tik GM paskyros ir jų grupės nariai.");
            SendHome(player);
        }
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float x, float y, float /*z*/, float /*orientation*/,
        uint32 /*options*/, Unit* /*target*/) override
    {
        if (!player || !InIsland(mapid, x, y) || CanUseIsland(player))
            return true;

        Say(player, "|cffff8000[GM sala]|r Į šią vietą gali patekti tik GM paskyros ir jų grupės nariai.");
        return false;
    }
};

class GmIslandWorldScript : public WorldScript
{
public:
    GmIslandWorldScript() : WorldScript("GmIslandWorldScript", { WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override { LoadEnchants(); }
};

// ============================================================ komanda
class GmIslandCommandScript : public CommandScript
{
public:
    GmIslandCommandScript() : CommandScript("GmIslandCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table =
        {
            { "testsala", HandleIsland, SEC_GAMEMASTER, Console::No }
        };
        return table;
    }

    // .testsala [atgal]
    static bool HandleIsland(ChatHandler* handler, Optional<std::string> arg)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (arg && (*arg == "atgal" || *arg == "back"))
        {
            TeleportBack(player);
            handler->SendSysMessage("Grįžai iš GM salos.");
            return true;
        }

        if (!Cfg().enable)
        {
            handler->SendSysMessage("GM sala išjungta (GmIsland.Enable = 0).");
            return true;
        }

        TeleportToIsland(player);
        handler->SendSysMessage("Sveikas atvykęs į GM salą. Grįžti: .testsala atgal arba pas pagalbininką.");
        return true;
    }
};

// ============================================================ užkerėtojas
class npc_gmisland_enchanter : public CreatureScript
{
public:
    npc_gmisland_enchanter() : CreatureScript("npc_gmisland_enchanter") { }

    static void ShowSlots(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        bool any = false;
        for (SlotInfo const& s : SLOTS)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, s.slot);
            if (!item)
                continue;

            any = true;
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, Acore::StringFormat("{}: {}", s.label, item->GetTemplate()->Name1),
                SENDER_SLOT, s.slot);
        }

        SendGossipMenuFor(player, any ? TEXT_ENCHANTER : TEXT_ENCHANTER, creature);
    }

    static void ShowEnchants(Player* player, Creature* creature, uint8 slot, uint32 page)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
        {
            ShowSlots(player, creature);
            return;
        }

        std::vector<EnchantRow const*> list;
        for (EnchantRow const& row : g_enchants)
            if (Eligible(item->GetTemplate(), row))
                list.push_back(&row);

        ClearGossipMenuFor(player);
        uint32 const total = uint32(list.size());
        uint32 const pages = std::max<uint32>(1, (total + PAGE_SIZE - 1) / PAGE_SIZE);
        page = std::min(page, pages - 1);

        for (uint32 i = page * PAGE_SIZE; i < std::min(total, (page + 1) * PAGE_SIZE); ++i)
            AddGossipItemFor(player, list[i]->prismatic ? GOSSIP_ICON_MONEY_BAG : GOSSIP_ICON_VENDOR, list[i]->label,
                SENDER_APPLY + slot, list[i]->spellId);

        if (page > 0)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "<< Ankstesnis puslapis", SENDER_PAGE, slot * 100 + page - 1);
        if (page + 1 < pages)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Kitas puslapis >>", SENDER_PAGE, slot * 100 + page + 1);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Pašalinti visus šio daikto užkerėjimus ir lizdą", SENDER_CLEAR, slot);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "<< Atgal į ekipiruotės vietas", SENDER_BACK, 0);
        SendGossipMenuFor(player, TEXT_ENCHANT_LIST, creature);
    }

    static void ApplyEnchant(Player* player, Item* item, EnchantRow const& row)
    {
        EnchantmentSlot const slot = row.prismatic ? PRISMATIC_ENCHANTMENT_SLOT : PERM_ENCHANTMENT_SLOT;
        bool const equipped = item->IsEquipped();
        if (equipped)
            player->ApplyEnchantment(item, slot, false);

        item->SetEnchantment(slot, row.enchantId, 0, 0);
        if (equipped)
            player->ApplyEnchantment(item, slot, true);
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ShowSlots(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (sender == SENDER_SLOT)
        {
            ShowEnchants(player, creature, uint8(action), 0);
            return true;
        }

        if (sender == SENDER_PAGE)
        {
            ShowEnchants(player, creature, uint8(action / 100), action % 100);
            return true;
        }

        if (sender == SENDER_BACK)
        {
            ShowSlots(player, creature);
            return true;
        }

        if (sender == SENDER_CLEAR)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(action)))
            {
                bool const equipped = item->IsEquipped();
                for (EnchantmentSlot s : { PERM_ENCHANTMENT_SLOT, PRISMATIC_ENCHANTMENT_SLOT })
                {
                    if (!item->GetEnchantmentId(s))
                        continue;

                    if (equipped)
                        player->ApplyEnchantment(item, s, false);
                    item->ClearEnchantment(s);
                }
                Say(player, Acore::StringFormat("|cff00ff00[GM sala]|r Daikto „{}“ užkerėjimai pašalinti.", item->GetTemplate()->Name1));
            }
            ShowEnchants(player, creature, uint8(action), 0);
            return true;
        }

        if (sender >= SENDER_APPLY && sender < SENDER_APPLY + 40)
        {
            uint8 const slot = uint8(sender - SENDER_APPLY);
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (item)
            {
                for (EnchantRow const& row : g_enchants)
                    if (row.spellId == action && Eligible(item->GetTemplate(), row))
                    {
                        ApplyEnchant(player, item, row);
                        Say(player, Acore::StringFormat("|cff00ff00[GM sala]|r {}: {}", item->GetTemplate()->Name1, row.label));
                        break;
                    }
            }

            ShowEnchants(player, creature, slot, 0);
            return true;
        }

        CloseGossipMenuFor(player);
        return true;
    }
};

// ============================================================ pagalbininkas
class npc_gmisland_helper : public CreatureScript
{
public:
    npc_gmisland_helper() : CreatureScript("npc_gmisland_helper") { }

    enum Action : uint32
    {
        ACT_GOLD = 1, ACT_LEVEL, ACT_RESTORE, ACT_SKILLS, ACT_PROFESSIONS, ACT_TP_STORMWIND, ACT_TP_ORGRIMMAR, ACT_TP_DALARAN,
        ACT_TP_SHATTRATH, ACT_BACK, ACT_CLOSE
    };

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, Acore::StringFormat("Gauti {} aukso", Cfg().gold), GOSSIP_SENDER_MAIN, ACT_GOLD);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Pakelti iki 80 lygio (talentai ir įgūdžiai atnaujinami)", GOSSIP_SENDER_MAIN, ACT_LEVEL);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Atstatyti gyvybes, išteklius, atšaukti atsinaujinimus ir suremontuoti daiktus", GOSSIP_SENDER_MAIN, ACT_RESTORE);
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "Visi ginklų ir gynybos įgūdžiai iki maksimumo", GOSSIP_SENDER_MAIN, ACT_SKILLS);
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "Turimos profesijos iki 450", GOSSIP_SENDER_MAIN, ACT_PROFESSIONS);
        AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Teleportas: Stormvindas", GOSSIP_SENDER_MAIN, ACT_TP_STORMWIND);
        AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Teleportas: Orgrimaras", GOSSIP_SENDER_MAIN, ACT_TP_ORGRIMMAR);
        AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Teleportas: Dalaranas", GOSSIP_SENDER_MAIN, ACT_TP_DALARAN);
        AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Teleportas: Šatratas", GOSSIP_SENDER_MAIN, ACT_TP_SHATTRATH);
        AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Grįžti, iš kur atvykau", GOSSIP_SENDER_MAIN, ACT_BACK);
        SendGossipMenuFor(player, TEXT_HELPER, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        switch (action)
        {
            case ACT_GOLD:
            {
                uint32 const add = Cfg().gold * 10000u;
                uint32 const money = player->GetMoney();
                uint32 const grant = (uint64(money) + add > MAX_MONEY_AMOUNT) ? uint32(MAX_MONEY_AMOUNT - money) : add;
                player->ModifyMoney(grant);
                Say(player, Acore::StringFormat("|cff00ff00[GM sala]|r Gavai {} aukso.", grant / 10000));
                break;
            }
            case ACT_LEVEL:
                if (player->GetLevel() < 80)
                {
                    player->GiveLevel(80);
                    player->InitTalentForLevel();
                    player->SetUInt32Value(PLAYER_XP, 0);
                }
                player->UpdateSkillsToMaxSkillsForLevel();
                Say(player, "|cff00ff00[GM sala]|r Esi 80 lygio.");
                break;
            case ACT_RESTORE:
                if (!player->IsAlive())
                    player->ResurrectPlayer(1.0f);
                player->RemoveAurasDueToSpell(15007);       // prisikėlimo liga
                player->RemoveAllSpellCooldown();
                player->SetHealth(player->GetMaxHealth());
                if (player->getPowerType() == POWER_MANA)
                    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
                player->DurabilityRepairAll(false, 0.0f, false);
                Say(player, "|cff00ff00[GM sala]|r Atstatyta.");
                break;
            case ACT_SKILLS:
                player->UpdateSkillsToMaxSkillsForLevel();
                Say(player, "|cff00ff00[GM sala]|r Ginklų ir gynybos įgūdžiai pakelti.");
                break;
            case ACT_PROFESSIONS:
            {
                static uint16 const professions[] = { 171, 164, 333, 202, 182, 773, 755, 165, 186, 393, 197, 185, 129, 356 };
                uint32 n = 0;
                for (uint16 skill : professions)
                    if (player->HasSkill(skill))
                    {
                        player->SetSkill(skill, 6, 450, 450);
                        ++n;
                    }
                Say(player, Acore::StringFormat("|cff00ff00[GM sala]|r Profesijos pakeltos iki 450 (viso: {}). Visas profesijas ir receptus galima išmokti GM komanda .learn all_crafts.", n));
                break;
            }
            case ACT_TP_STORMWIND:
                player->TeleportTo(0, -8842.09f, 626.358f, 94.0867f, 3.61363f);
                break;
            case ACT_TP_ORGRIMMAR:
                player->TeleportTo(1, 1601.08f, -4378.69f, 9.9846f, 2.14362f);
                break;
            case ACT_TP_DALARAN:
                player->TeleportTo(571, 5804.15f, 624.771f, 647.767f, 1.64f);
                break;
            case ACT_TP_SHATTRATH:
                player->TeleportTo(530, -1838.16f, 5301.79f, -12.428f, 5.9517f);
                break;
            case ACT_BACK:
                TeleportBack(player);
                break;
            default:
                break;
        }

        if (action >= ACT_TP_STORMWIND && action <= ACT_BACK)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        OnGossipHello(player, creature);
        return true;
    }
};

// ============================================================ mokytojas (burtai, jojimas/skraidymas, ginklai)
namespace
{
    // Visi klasės burtai iki žaidėjo lygio (kaip `.learn all_myspells`, bet ne aukštesnio lygio nei žaidėjas; talentai praleidžiami)
    uint32 LearnClassSpells(Player* player)
    {
        ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(player->getClass());
        if (!classEntry)
            return 0;

        uint32 learned = 0;
        for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        {
            SkillLineAbilityEntry const* entry = sSkillLineAbilityStore.LookupEntry(i);
            if (!entry)
                continue;

            SpellInfo const* spell = sSpellMgr->GetSpellInfo(entry->Spell);
            if (!spell || spell->SpellLevel == 0 || spell->SpellLevel > player->GetLevel())
                continue;

            if (!player->IsSpellFitByClassAndRace(spell->Id) || spell->SpellFamilyName != classEntry->spellfamily)
                continue;

            if (GetTalentSpellCost(sSpellMgr->GetFirstSpellInChain(spell->Id)) > 0)
                continue;

            if (!SpellMgr::IsSpellValid(spell) || player->HasSpell(spell->Id))
                continue;

            player->learnSpell(spell->Id);
            ++learned;
        }
        return learned;
    }

    // Jojimas (Pameistrys..Meistras), skraidymas ir skraidymas Šaltuosiuose kraštuose
    uint32 LearnRiding(Player* player)
    {
        static uint32 const spells[] = { 33388, 33391, 34090, 34091, 54197 };
        uint32 learned = 0;
        for (uint32 spell : spells)
            if (!player->HasSpell(spell))
            {
                player->learnSpell(spell);
                ++learned;
            }
        return learned;
    }

    // Visi ginklų ir šarvų įgūdžiai, kuriuos gali turėti žaidėjo rasė ir klasė, + jų „proficiency“ burtai; įgūdžiai pakeliami iki maksimumo
    uint32 LearnProficiencies(Player* player)
    {
        std::vector<uint32> skills;
        for (uint32 i = 0; i < sSkillLineStore.GetNumRows(); ++i)
        {
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(i);
            if (!line || (line->categoryId != SKILL_CATEGORY_WEAPON && line->categoryId != SKILL_CATEGORY_ARMOR))
                continue;

            if (GetSkillRaceClassInfo(line->id, player->getRace(), player->getClass()))
                skills.push_back(line->id);
        }

        uint32 learned = 0;
        for (uint32 skill : skills)
            if (!player->HasSkill(skill))
            {
                player->LearnDefaultSkill(skill, 0);
                ++learned;
            }

        for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        {
            SkillLineAbilityEntry const* entry = sSkillLineAbilityStore.LookupEntry(i);
            if (!entry || std::find(skills.begin(), skills.end(), entry->SkillLine) == skills.end())
                continue;

            SpellInfo const* spell = sSpellMgr->GetSpellInfo(entry->Spell);
            if (!spell || !player->IsSpellFitByClassAndRace(spell->Id) || !SpellMgr::IsSpellValid(spell) || player->HasSpell(spell->Id))
                continue;

            player->learnSpell(spell->Id);
            ++learned;
        }

        player->UpdateSkillsToMaxSkillsForLevel();
        return learned;
    }
}

class npc_gmisland_trainer : public CreatureScript
{
public:
    npc_gmisland_trainer() : CreatureScript("npc_gmisland_trainer") { }

    enum Action : uint32 { ACT_SPELLS = 1, ACT_RIDING, ACT_WEAPONS, ACT_ALL };

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "Išmokti visus mano klasės burtus (iki mano lygio)", GOSSIP_SENDER_MAIN, ACT_SPELLS);
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "Išmokti visus jojimo ir skraidymo įgūdžius (ir skraidymą Šaltuosiuose kraštuose)", GOSSIP_SENDER_MAIN, ACT_RIDING);
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "Išmokti visus ginklų ir šarvų įgūdžius, tinkamus mano klasei", GOSSIP_SENDER_MAIN, ACT_WEAPONS);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Išmokti viską iš karto", GOSSIP_SENDER_MAIN, ACT_ALL);
        SendGossipMenuFor(player, TEXT_TRAINER, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        uint32 spells = 0, riding = 0, weapons = 0;
        if (action == ACT_SPELLS || action == ACT_ALL)
            spells = LearnClassSpells(player);
        if (action == ACT_RIDING || action == ACT_ALL)
            riding = LearnRiding(player);
        if (action == ACT_WEAPONS || action == ACT_ALL)
            weapons = LearnProficiencies(player);

        Say(player, Acore::StringFormat("|cff00ff00[GM sala]|r Išmokta naujų: burtų {}, jojimo/skraidymo {}, ginklų ir šarvų {}.", spells, riding, weapons));
        OnGossipHello(player, creature);
        return true;
    }
};

void AddSC_GmIsland()
{
    new GmIslandPlayerScript();
    new GmIslandWorldScript();
    new GmIslandCommandScript();
    new npc_gmisland_enchanter();
    new npc_gmisland_helper();
    new npc_gmisland_trainer();
}
