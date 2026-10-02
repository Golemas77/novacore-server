/*
 * NovaCore: mod-warmode - „karo rezimas“ (kaip Retail) PvE serveriui.
 *
 * Zaidejas ji ijungia/isjungia PAGRINDINIUOSE MIESTUOSE (komanda `.warmode` arba pas NPC „Karo vadas“). Kol rezimas ijungtas:
 *   - zaidejas visada PvP zymetas ir gali kovoti su KITAIS karo rezimo zaidejais;
 *   - gauna +20% patirties uz priesus ir uzduotis (konfiguruojama);
 *   - virs zaidejo rodoma aura „Karo rezimas“ (burtas 90002).
 * Zaidejai, kurie karo rezimo NEturi (iprasti ir hardkoro), pulti negalima: zala tarp zaideju leidziama tik jei abu
 * karo rezime (ar dvikova / musio laukas / arena / Wintergrasp / FFA zona). Hardkoro zaidejas karo rezimo ijungti negali.
 *
 * Busena saugoma lenteleje `character_warmode` (characters DB), tekstai - `module_string` (world DB, `mod-warmode`).
 * Boty (playerbots) sis modulis neliecia.
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* WM_MODULE = "mod-warmode";

    constexpr uint32 WM_SPELL_MARKER = 90002;   // aura „Karo rezimas“
    constexpr uint32 HC_SPELL_MARKER = 90001;   // mod-hardcore aura „Vienos gyvybes rezimas“ (hardkoras aktyvus)
    constexpr uint32 ZONE_WINTERGRASP = 4197;

    // module_string ID (zr. data/sql/db-world/base/mod_warmode_module_string.sql)
    enum WarModeString : uint32
    {
        WM_STR_HEADER          = 1,
        WM_STR_RULES_1         = 2,
        WM_STR_RULES_XP        = 3,
        WM_STR_RULES_PVP       = 4,
        WM_STR_RULES_TOGGLE    = 5,
        WM_STR_STATUS_ON       = 6,
        WM_STR_STATUS_OFF      = 7,
        WM_STR_ENABLED         = 8,
        WM_STR_DISABLED        = 9,
        WM_STR_ALREADY_ON      = 10,
        WM_STR_ALREADY_OFF     = 11,
        WM_STR_NEED_CAPITAL    = 12,
        WM_STR_IN_COMBAT       = 13,
        WM_STR_HARDCORE_DENIED = 14,
        WM_STR_SERVER_OFF      = 15,
        WM_STR_BOTS_DENIED     = 16,
        WM_STR_MUST_BE_ALIVE   = 17,
        WM_STR_INSTANCE_DENIED = 18,
        WM_STR_PVP_LOCKED      = 19,
        WM_STR_LOGIN_ON        = 20,
        WM_STR_GOSSIP_ENABLE   = 21,
        WM_STR_GOSSIP_DISABLE  = 22,
        WM_STR_GOSSIP_DECLINE  = 23,
        WM_STR_GOSSIP_OK       = 24,
        WM_STR_GM_SET          = 25,
        WM_STR_GM_NOT_FOUND    = 26,
        WM_STR_WORD_ON         = 27,
        WM_STR_WORD_OFF        = 28
    };

    // npc_text ID (world DB, zr. data/sql/db-world/base/mod_warmode_npc.sql)
    enum WarModeNpc : uint32
    {
        WM_NPC_TEXT_INFO     = 9000010,
        WM_NPC_TEXT_ON       = 9000011,
        WM_NPC_TEXT_HARDCORE = 9000012,
        WM_NPC_TEXT_DISABLED = 9000013
    };

    enum WarModeGossipAction : uint32
    {
        WM_ACTION_ENABLE  = 1,
        WM_ACTION_DISABLE = 2,
        WM_ACTION_CLOSE   = 3
    };

    // Pagrindiniai miestai (zonu ID): Stormwind, Ironforge, Darnassus, Exodar, Orgrimmar, Thunder Bluff, Undercity,
    // Silvermoon, Shattrath, Dalaran.
    std::unordered_set<uint32> const g_capitals = { 1519, 1537, 1657, 3557, 1637, 1638, 1497, 3487, 3703, 4395 };

    std::shared_mutex g_mutex;
    std::unordered_set<uint32> g_enabled;     // prisijungusiu zaideju su ijungtu karo rezimu GUID

    struct WarModeConfig
    {
        bool enable;
        uint32 xpBonusPercent;
        bool onlyInCapitals;
        bool blockNonWarModePvP;
    };

    WarModeConfig Cfg()
    {
        WarModeConfig c;
        c.enable             = sConfigMgr->GetOption<bool>("WarMode.Enable", true);
        c.xpBonusPercent     = sConfigMgr->GetOption<uint32>("WarMode.XpBonusPercent", 20);
        c.onlyInCapitals     = sConfigMgr->GetOption<bool>("WarMode.OnlyInCapitals", true);
        c.blockNonWarModePvP = sConfigMgr->GetOption<bool>("WarMode.BlockNonWarModePvP", true);
        return c;
    }

    bool IsRealPlayer(Player* player)
    {
        return player && player->GetSession() && !player->GetSession()->IsBot();
    }

    std::string Str(uint32 id)
    {
        if (std::string const* s = sObjectMgr->GetModuleString(WM_MODULE, id, LOCALE_enUS))
            return *s;
        return "<warmode>";
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

    bool IsWarMode(Player* player)
    {
        if (!IsRealPlayer(player))
            return false;

        std::shared_lock<std::shared_mutex> lock(g_mutex);
        return g_enabled.count(player->GetGUID().GetCounter()) > 0;
    }

    bool IsHardcore(Player* player)
    {
        return player && player->HasAura(HC_SPELL_MARKER);
    }

    void SaveState(uint32 guid, bool enabled)
    {
        CharacterDatabase.Execute("REPLACE INTO `character_warmode` (`guid`, `enabled`, `changed_at`) VALUES ({}, {}, {})",
            guid, enabled ? 1 : 0, uint32(GameTime::GetGameTime().count()));
    }

    void ApplyMarker(Player* player)
    {
        if (player && player->IsAlive() && !player->HasAura(WM_SPELL_MARKER))
            player->AddAura(WM_SPELL_MARKER, player);
    }

    void RemoveMarker(Player* player)
    {
        if (player)
            player->RemoveAurasDueToSpell(WM_SPELL_MARKER);
    }

    bool InCapital(Player* player)
    {
        return g_capitals.count(player->GetZoneId()) > 0;
    }

    // Ar zaidejas gali keisti karo rezima? 0 - taip, kitaip module_string ID su priezastimi.
    uint32 CheckCanToggle(Player* player, bool enable, WarModeConfig const& cfg)
    {
        if (!cfg.enable)
            return WM_STR_SERVER_OFF;

        if (!IsRealPlayer(player))
            return WM_STR_BOTS_DENIED;

        bool const on = IsWarMode(player);
        if (enable && on)
            return WM_STR_ALREADY_ON;

        if (!enable && !on)
            return WM_STR_ALREADY_OFF;

        if (enable && IsHardcore(player))
            return WM_STR_HARDCORE_DENIED;

        if (player->IsGameMaster())
            return 0;

        if (!player->IsAlive())
            return WM_STR_MUST_BE_ALIVE;

        if (player->IsInCombat())
            return WM_STR_IN_COMBAT;

        if (Map* map = player->GetMap())
            if (map->Instanceable())
                return WM_STR_INSTANCE_DENIED;

        if (cfg.onlyInCapitals && !InCapital(player))
            return WM_STR_NEED_CAPITAL;

        return 0;
    }

    void Enable(Player* player)
    {
        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            g_enabled.insert(player->GetGUID().GetCounter());
        }
        SaveState(player->GetGUID().GetCounter(), true);
        ApplyMarker(player);
        player->UpdatePvP(true, true);
    }

    void Disable(Player* player)
    {
        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            g_enabled.erase(player->GetGUID().GetCounter());
        }
        SaveState(player->GetGUID().GetCounter(), false);
        RemoveMarker(player);
        player->UpdatePvP(false, true);
    }

    // Bendras perjungimas (komandai ir NPC). Grazina true, jei pakeista; reply - zinute zaidejui.
    bool Toggle(Player* player, bool enable, std::string& reply)
    {
        WarModeConfig const cfg = Cfg();
        if (uint32 const denied = CheckCanToggle(player, enable, cfg))
        {
            reply = Str(denied);
            return false;
        }

        if (enable)
        {
            Enable(player);
            reply = Fmt(WM_STR_ENABLED, cfg.xpBonusPercent);
        }
        else
        {
            Disable(player);
            reply = Str(WM_STR_DISABLED);
        }

        return true;
    }

    // Ar sitas zaidejas gali pulti kita (zala)? Leidziama: abu karo rezime, dvikova, musio laukas / arena / Wintergrasp / FFA.
    bool PvPAllowed(Player* a, Player* b)
    {
        if (a->InBattleground() || a->InArena() || b->InBattleground() || b->InArena())
            return true;

        if (a->GetZoneId() == ZONE_WINTERGRASP || b->GetZoneId() == ZONE_WINTERGRASP)
            return true;

        if (a->IsFFAPvP() && b->IsFFAPvP())
            return true;

        if (a->duel && a->duel->Opponent == b && a->duel->State == DUEL_STATE_IN_PROGRESS)
            return true;

        return IsWarMode(a) && IsWarMode(b);
    }
}

class WarModePlayerScript : public PlayerScript
{
public:
    WarModePlayerScript() : PlayerScript("WarModePlayerScript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_PLAYER_RESURRECT,
            PLAYERHOOK_ON_PLAYER_PVP_FLAG_CHANGE,
            PLAYERHOOK_ON_GIVE_EXP,
            PLAYERHOOK_ON_DELETE_FROM_DB
        }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!IsRealPlayer(player))
            return;

        WarModeConfig const cfg = Cfg();
        uint32 const guid = player->GetGUID().GetCounter();

        bool enabled = false;
        if (QueryResult result = CharacterDatabase.Query("SELECT `enabled` FROM `character_warmode` WHERE `guid` = {}", guid))
            enabled = (*result)[0].Get<uint8>() != 0;

        // Hardkoras ir karo rezimas nesuderinami (hardkoras laimi).
        if (enabled && (!cfg.enable || IsHardcore(player)))
        {
            SaveState(guid, false);
            enabled = false;
        }

        if (!enabled)
        {
            RemoveMarker(player);   // pasilikusi aura - nuimam
            return;
        }

        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            g_enabled.insert(guid);
        }

        ApplyMarker(player);
        player->UpdatePvP(true, true);
        SendTo(player, Fmt(WM_STR_LOGIN_ON, cfg.xpBonusPercent));
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        std::unique_lock<std::shared_mutex> lock(g_mutex);
        g_enabled.erase(player->GetGUID().GetCounter());
    }

    // Mirtis nuima aura - po prisikelimo uzdedam is naujo.
    void OnPlayerResurrect(Player* player, float /*restorePercent*/, bool /*applySickness*/) override
    {
        if (IsWarMode(player))
            ApplyMarker(player);
    }

    // Karo rezime PvP zymes nuimti negalima; be karo rezimo (ne musio lauke ir pan.) PvP zymes ijungti negalima.
    void OnPlayerPVPFlagChange(Player* player, bool state) override
    {
        if (!IsRealPlayer(player) || !Cfg().enable)
            return;

        if (IsWarMode(player))
        {
            if (!state)
                player->UpdatePvP(true, true);
            return;
        }

        if (!state || !Cfg().blockNonWarModePvP || player->IsGameMaster())
            return;

        if (player->InBattleground() || player->InArena() || player->GetZoneId() == ZONE_WINTERGRASP)
            return;

        player->UpdatePvP(false, true);
    }

    // +X% patirties uz priesus ir uzduotis karo rezime.
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 xpSource) override
    {
        if (amount == 0 || !IsWarMode(player))
            return;

        switch (xpSource)
        {
            case XPSOURCE_KILL:
            case XPSOURCE_QUEST:
            case XPSOURCE_QUEST_DF:
                break;
            default:
                return;
        }

        uint32 const percent = Cfg().xpBonusPercent;
        amount = uint32((uint64(amount) * (100 + percent)) / 100);
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `character_warmode` WHERE `guid` = {}", guid);
    }
};

// Zala tarp zaideju: leidziama tik kai abu karo rezime (ar dvikova / musio laukas / arena / Wintergrasp / FFA).
class WarModeUnitScript : public UnitScript
{
public:
    WarModeUnitScript() : UnitScript("WarModeUnitScript", { UNITHOOK_ON_DAMAGE }) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!attacker || !victim || damage == 0)
            return;

        Player* a = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        Player* v = victim->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!a || !v || a == v)
            return;

        // Botu tarpusavio kovai (musio laukuose ir pan.) nelieciam - tik kai ivelgtas tikras zaidejas.
        if (!IsRealPlayer(a) && !IsRealPlayer(v))
            return;

        if (a->IsGameMaster() || !Cfg().blockNonWarModePvP || !Cfg().enable)
            return;

        if (!PvPAllowed(a, v))
            damage = 0;
    }
};

class npc_warmode_script : public CreatureScript
{
public:
    npc_warmode_script() : CreatureScript("npc_warmode") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);

        WarModeConfig const cfg = Cfg();
        uint32 textId = WM_NPC_TEXT_INFO;

        if (!cfg.enable)
        {
            textId = WM_NPC_TEXT_DISABLED;
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(WM_STR_GOSSIP_OK), GOSSIP_SENDER_MAIN, WM_ACTION_CLOSE);
        }
        else if (IsHardcore(player))
        {
            textId = WM_NPC_TEXT_HARDCORE;
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(WM_STR_GOSSIP_OK), GOSSIP_SENDER_MAIN, WM_ACTION_CLOSE);
        }
        else if (IsWarMode(player))
        {
            textId = WM_NPC_TEXT_ON;
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Str(WM_STR_GOSSIP_DISABLE), GOSSIP_SENDER_MAIN, WM_ACTION_DISABLE);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(WM_STR_GOSSIP_DECLINE), GOSSIP_SENDER_MAIN, WM_ACTION_CLOSE);
        }
        else
        {
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Str(WM_STR_GOSSIP_ENABLE), GOSSIP_SENDER_MAIN, WM_ACTION_ENABLE);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(WM_STR_GOSSIP_DECLINE), GOSSIP_SENDER_MAIN, WM_ACTION_CLOSE);
        }

        SendGossipMenuFor(player, textId, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* /*creature*/, uint32 /*sender*/, uint32 action) override
    {
        if (action == WM_ACTION_ENABLE || action == WM_ACTION_DISABLE)
        {
            std::string reply;
            Toggle(player, action == WM_ACTION_ENABLE, reply);
            SendTo(player, reply);
        }

        CloseGossipMenuFor(player);
        return true;
    }
};

class WarModeCommandScript : public CommandScript
{
public:
    WarModeCommandScript() : CommandScript("WarModeCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable forceTable =
        {
            { "on",  HandleForceOn,  SEC_GAMEMASTER, Console::No },
            { "off", HandleForceOff, SEC_GAMEMASTER, Console::No }
        };

        static ChatCommandTable warmodeTable =
        {
            { "",      HandleInfo, SEC_PLAYER, Console::No },
            { "info",  HandleInfo, SEC_PLAYER, Console::No },
            { "on",    HandleOn,   SEC_PLAYER, Console::No },
            { "off",   HandleOff,  SEC_PLAYER, Console::No },
            { "force", forceTable }
        };

        static ChatCommandTable rootTable =
        {
            { "warmode", warmodeTable }
        };

        return rootTable;
    }

    static bool HandleInfo(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        WarModeConfig const cfg = Cfg();
        if (!cfg.enable)
        {
            handler->SendSysMessage(Str(WM_STR_SERVER_OFF));
            return true;
        }

        handler->SendSysMessage(Str(WM_STR_HEADER));
        handler->SendSysMessage(Str(WM_STR_RULES_1));
        handler->SendSysMessage(Fmt(WM_STR_RULES_XP, cfg.xpBonusPercent));
        handler->SendSysMessage(Str(WM_STR_RULES_PVP));
        handler->SendSysMessage(Str(WM_STR_RULES_TOGGLE));
        handler->SendSysMessage(Str(IsWarMode(player) ? WM_STR_STATUS_ON : WM_STR_STATUS_OFF));
        return true;
    }

    static bool HandleToggle(ChatHandler* handler, bool enable)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string reply;
        Toggle(player, enable, reply);
        handler->SendSysMessage(reply);
        return true;
    }

    static bool HandleOn(ChatHandler* handler) { return HandleToggle(handler, true); }
    static bool HandleOff(ChatHandler* handler) { return HandleToggle(handler, false); }

    // .warmode force on|off [zaidejas] - GM, be miesto / kovos apribojimu (testavimui)
    static bool HandleForce(ChatHandler* handler, Optional<PlayerIdentifier> target, bool enable)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);

        Player* player = target ? target->GetConnectedPlayer() : nullptr;
        if (!player || !IsRealPlayer(player))
        {
            handler->SendSysMessage(Str(WM_STR_GM_NOT_FOUND));
            return true;
        }

        if (IsWarMode(player) != enable)
        {
            if (enable)
                Enable(player);
            else
                Disable(player);
        }

        SendTo(player, enable ? Fmt(WM_STR_ENABLED, Cfg().xpBonusPercent) : Str(WM_STR_DISABLED));
        handler->SendSysMessage(Fmt(WM_STR_GM_SET, player->GetName(), Str(enable ? WM_STR_WORD_ON : WM_STR_WORD_OFF)));
        return true;
    }

    static bool HandleForceOn(ChatHandler* handler, Optional<PlayerIdentifier> target) { return HandleForce(handler, target, true); }
    static bool HandleForceOff(ChatHandler* handler, Optional<PlayerIdentifier> target) { return HandleForce(handler, target, false); }
};

void AddSC_WarMode()
{
    new WarModePlayerScript();
    new WarModeUnitScript();
    new npc_warmode_script();
    new WarModeCommandScript();
}
