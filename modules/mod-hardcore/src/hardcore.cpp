/*
 * NovaCore: mod-hardcore - „hardkoro“ (viena gyvybe) rezimas.
 *
 * Zaidejas savanoriskai ijungia rezima pas NPC „Hardkoras“ (arba komanda `.hardcore start`). Kol rezimas aktyvus:
 *   - negalima buti grupeje su kitais zaidejais;
 *   - negalima ijungti PvP, pulti kitu zaideju ar dvikovoti;
 *   - negalima eiti i musio laukus ir pozemius (instancijas) ir naudotis aukcionu;
 *   - virs zaidejo rodoma aura "Vienos gyvybes rezimas" (dingsta zuvus);
 *   - rezima galima pradeti TIK ka sukurus personaza (dar negauta patirties);
 *   - patirties tasku reitas fiksuotas (pagal nutylejima x1, nepriklausomai nuo serverio reitu);
 *   - priesai turi daugiau gyvybiu ir daro daugiau zalos (pagal nutylejima x1.5).
 * Mirtis rezima PANAIKINA: personazas NEISTRINAMAS, o zaidejas toliau zaidzia kaip iprastas zaidejas.
 * Pasiekus maksimalu lygi GYVAM rezimas ivyksta (status 3): isduodamas pasiekimas 9001 + titulas 178 + jojamojo gyvuno
 * burtas 60002 (Hardcore.Reward.*), apribojimai nuimami (Hardcore.EndAtMaxLevel). Testui: `.hardcore gm reward`.
 *
 * Busena saugoma lenteleje `character_hardcore` (characters DB), tekstai - `module_string` (world DB),
 * NPC - creature_template 9000001 (ScriptName `npc_hardcore`), nustatymai - mod_hardcore.conf.
 * Boty (playerbots) sis modulis neliecia: jie nenaudoja komandu, o prisijungimo metu praleidziami.
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "StringFormat.h"
#include "World.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* HC_MODULE = "mod-hardcore";

    // Aura "Vienos gyvybes rezimas" (kliento Spell.dbc + serverio spell_dbc, zr. .tools/add_custom_client_spell.py)
    constexpr uint32 HC_SPELL_MARKER = 90001;

    enum HardcoreStatus : uint8
    {
        HC_STATUS_NONE   = 0,   // niekada nepradeta
        HC_STATUS_ACTIVE = 1,   // aktyvus
        HC_STATUS_FAILED = 2,   // zuvo (rezimas panaikintas)
        HC_STATUS_COMPLETED = 3 // pasieke maksimalu lygi gyvas (apdovanotas, apribojimai nuimti)
    };

    // module_string ID (zr. data/sql/db-world/base/mod_hardcore_module_string.sql)
    enum HardcoreString : uint32
    {
        HC_STR_INFO_HEADER        = 1,
        HC_STR_INFO_RULES         = 2,
        HC_STR_STATUS_NONE        = 3,
        HC_STR_STATUS_ACTIVE      = 4,
        HC_STR_STATUS_FAILED      = 5,
        HC_STR_HOWTO_START        = 6,
        HC_STR_START_WARNING      = 7,
        HC_STR_STARTED            = 8,
        HC_STR_ALREADY_ACTIVE     = 9,
        HC_STR_TOO_HIGH_LEVEL     = 10,
        HC_STR_ALREADY_FAILED     = 11,
        HC_STR_DIED_SELF          = 12,
        HC_STR_ANNOUNCE_START     = 13,
        HC_STR_ANNOUNCE_DEATH     = 14,
        HC_STR_ANNOUNCE_DEATH_ENV = 15,
        HC_STR_ANNOUNCE_MILESTONE = 16,
        HC_STR_LOGIN_ACTIVE       = 17,
        HC_STR_LOGIN_HINT         = 18,
        HC_STR_GM_ON              = 19,
        HC_STR_GM_RESET           = 20,
        HC_STR_GM_NOT_FOUND       = 21,
        HC_STR_DISABLED           = 22,
        HC_STR_MUST_BE_ALIVE      = 23,
        HC_STR_KILLER_PLAYER      = 24,
        HC_STR_BOTS_DENIED        = 25,
        HC_STR_START_LEAVE_GROUP  = 26,
        HC_STR_START_OPEN_WORLD   = 27,
        HC_STR_GROUP_DENIED_SELF  = 28,
        HC_STR_GROUP_DENIED_OTHER = 29,
        HC_STR_BG_DENIED          = 30,
        HC_STR_DUNGEON_DENIED     = 31,
        HC_STR_PVP_DENIED         = 32,
        HC_STR_DUEL_DENIED        = 33,
        HC_STR_AUCTION_DENIED     = 34,
        HC_STR_GOSSIP_ACCEPT      = 35,
        HC_STR_GOSSIP_DECLINE     = 36,
        HC_STR_GOSSIP_OK          = 37,
        HC_STR_GROUP_LEFT         = 38,
        HC_STR_RULE_GROUPS        = 40,
        HC_STR_RULE_PVP           = 41,
        HC_STR_RULE_BG            = 42,
        HC_STR_RULE_DUNGEONS      = 43,
        HC_STR_RULE_XP            = 44,
        HC_STR_RULE_ENEMIES       = 45,
        HC_STR_RULE_DEATH         = 46,
        HC_STR_RULE_AUCTION       = 47,
        HC_STR_RULE_REWARD        = 48,
        HC_STR_STATUS_COMPLETED   = 50,
        HC_STR_COMPLETED_SELF     = 51,
        HC_STR_ANNOUNCE_COMPLETED = 52,
        HC_STR_ALREADY_COMPLETED  = 53,
        HC_STR_GM_REWARD          = 54,
        HC_STR_BANNER_COMPLETED   = 55
    };

    // npc_text ID (world DB, zr. data/sql/db-world/base/mod_hardcore_npc.sql)
    enum HardcoreNpc : uint32
    {
        HC_NPC_ENTRY          = 9000001,
        HC_NPC_TEXT_RULES     = 9000001,
        HC_NPC_TEXT_ACTIVE    = 9000002,
        HC_NPC_TEXT_FAILED    = 9000003,
        HC_NPC_TEXT_TOO_HIGH  = 9000004,
        HC_NPC_TEXT_DISABLED  = 9000005,
        HC_NPC_TEXT_COMPLETED = 9000006
    };

    enum HardcoreGossipAction : uint32
    {
        HC_ACTION_ACCEPT  = 1,
        HC_ACTION_DECLINE = 2,
        HC_ACTION_CLOSE   = 3
    };

    struct HardcoreState
    {
        uint8 status = HC_STATUS_NONE;
        uint32 startTime = 0;
        uint8 startLevel = 0;
        uint32 endTime = 0;
        uint8 endLevel = 0;
        uint16 endMap = 0;
        uint16 endZone = 0;
        uint8 failures = 0;
        std::string killer;
        std::string pendingKiller;      // tik atmintyje: zudikas, uzfiksuotas PRIES mirties busenos irasyma
    };

    std::shared_mutex g_mutex;
    std::unordered_map<uint32, HardcoreState> g_cache;

    struct HardcoreConfig
    {
        bool enable;
        uint32 maxLevelToStart;
        bool allowRestartAfterDeath;
        bool announceStart;
        bool announceDeath;
        uint32 milestoneEvery;
        bool loginHint;
        bool requireConfirmation;
        bool blockGroups;
        bool allowBotGroups;
        bool blockPvP;
        bool blockBattlegrounds;
        bool blockDungeons;
        bool blockAuction;
        bool requireZeroXp;
        float xpRate;
        float enemyHealthMult;
        float enemyDamageMult;
        bool rewardEnable;
        uint32 rewardAchievement;
        uint32 rewardTitle;
        uint32 rewardMountSpell;
        bool endAtMaxLevel;
        bool announceComplete;
        uint32 announceSound;
    };

    HardcoreConfig Cfg()
    {
        HardcoreConfig c;
        c.enable                 = sConfigMgr->GetOption<bool>("Hardcore.Enable", true);
        c.maxLevelToStart        = sConfigMgr->GetOption<uint32>("Hardcore.MaxLevelToStart", 1);
        c.allowRestartAfterDeath = sConfigMgr->GetOption<bool>("Hardcore.AllowRestartAfterDeath", false);
        c.announceStart          = sConfigMgr->GetOption<bool>("Hardcore.AnnounceStart", true);
        c.announceDeath          = sConfigMgr->GetOption<bool>("Hardcore.AnnounceDeath", true);
        c.milestoneEvery         = sConfigMgr->GetOption<uint32>("Hardcore.AnnounceMilestoneEvery", 10);
        c.loginHint              = sConfigMgr->GetOption<bool>("Hardcore.LoginHint", true);
        c.requireConfirmation    = sConfigMgr->GetOption<bool>("Hardcore.RequireConfirmation", true);
        c.blockGroups            = sConfigMgr->GetOption<bool>("Hardcore.BlockGroups", true);
        c.allowBotGroups         = sConfigMgr->GetOption<bool>("Hardcore.AllowBotGroups", false);
        c.blockPvP               = sConfigMgr->GetOption<bool>("Hardcore.BlockPvP", true);
        c.blockBattlegrounds     = sConfigMgr->GetOption<bool>("Hardcore.BlockBattlegrounds", true);
        c.blockDungeons          = sConfigMgr->GetOption<bool>("Hardcore.BlockDungeons", true);
        c.blockAuction           = sConfigMgr->GetOption<bool>("Hardcore.BlockAuctionHouse", true);
        c.requireZeroXp          = sConfigMgr->GetOption<bool>("Hardcore.RequireZeroXp", true);
        c.xpRate                 = sConfigMgr->GetOption<float>("Hardcore.XpRate", 1.0f);
        c.enemyHealthMult        = std::max(0.1f, sConfigMgr->GetOption<float>("Hardcore.EnemyHealthMultiplier", 1.5f));
        c.enemyDamageMult        = std::max(0.1f, sConfigMgr->GetOption<float>("Hardcore.EnemyDamageMultiplier", 1.5f));
        // NovaCore: apdovanojimas uz maksimalu lygi (pasiekimas 9001 + titulas 178 + Laike pasiklydusio protodrako burtas 60002)
        c.rewardEnable           = sConfigMgr->GetOption<bool>("Hardcore.Reward.Enable", true);
        c.rewardAchievement      = sConfigMgr->GetOption<uint32>("Hardcore.Reward.Achievement", 9001);
        c.rewardTitle            = sConfigMgr->GetOption<uint32>("Hardcore.Reward.Title", 178);
        c.rewardMountSpell       = sConfigMgr->GetOption<uint32>("Hardcore.Reward.MountSpell", 60002);
        c.endAtMaxLevel          = sConfigMgr->GetOption<bool>("Hardcore.EndAtMaxLevel", true);
        c.announceComplete       = sConfigMgr->GetOption<bool>("Hardcore.AnnounceComplete", true);
        c.announceSound          = sConfigMgr->GetOption<uint32>("Hardcore.AnnounceSound", 12891);   // AchievementSound
        return c;
    }

    bool IsRealPlayer(Player* player)
    {
        return player && player->GetSession() && !player->GetSession()->IsBot();
    }

    std::string Str(uint32 id)
    {
        if (std::string const* s = sObjectMgr->GetModuleString(HC_MODULE, id, LOCALE_enUS))
            return *s;
        return "<hardcore>";
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

    void Broadcast(std::string const& text)
    {
        sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING, text);
    }

    // Pranesimas VISIEMS prisijungusiems zaidejams ekrano virsuje (kaip "Raid Warning": dideli raidai viduryje, 5 s, su
    // kliento garsu) + papildomas garsas (SoundEntries ID; 0 = be garso). Boty sesijoms nesiunciama. Saugu kviesti is zemelapio gijos.
    void BroadcastBanner(Player* source, std::string const& text, uint32 soundId)
    {
        WorldPacket chat;
        ChatHandler::BuildChatPacket(chat, CHAT_MSG_RAID_WARNING, LANG_UNIVERSAL, source, nullptr, text);

        WorldPacket sound(SMSG_PLAY_SOUND, 4);
        sound << uint32(soundId);

        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* receiver = pair.second;
            if (!receiver || !receiver->IsInWorld() || !receiver->GetSession() || receiver->GetSession()->IsBot())
                continue;

            receiver->GetSession()->SendPacket(&chat);
            if (soundId)
                receiver->GetSession()->SendPacket(&sound);
        }
    }

    HardcoreState GetState(uint32 guid)
    {
        std::shared_lock<std::shared_mutex> lock(g_mutex);
        auto it = g_cache.find(guid);
        return it != g_cache.end() ? it->second : HardcoreState();
    }

    // Greita patikra be busenos kopijavimo (kviecama is zalos kablio - kiekvienam smugiui).
    bool IsActive(Player* player)
    {
        if (!IsRealPlayer(player))
            return false;

        std::shared_lock<std::shared_mutex> lock(g_mutex);
        auto it = g_cache.find(player->GetGUID().GetCounter());
        return it != g_cache.end() && it->second.status == HC_STATUS_ACTIVE;
    }

    void SaveState(uint32 guid, HardcoreState const& s)
    {
        std::string killer = s.killer;
        CharacterDatabase.EscapeString(killer);
        CharacterDatabase.Execute(
            "REPLACE INTO `character_hardcore` (`guid`, `status`, `start_time`, `start_level`, `end_time`, `end_level`, "
            "`end_map`, `end_zone`, `failures`, `killer`) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, '{}')",
            guid, uint32(s.status), s.startTime, uint32(s.startLevel), s.endTime, uint32(s.endLevel),
            uint32(s.endMap), uint32(s.endZone), uint32(s.failures), killer);
    }

    void LoadState(uint32 guid)
    {
        HardcoreState st;
        if (QueryResult result = CharacterDatabase.Query(
            "SELECT `status`, `start_time`, `start_level`, `end_time`, `end_level`, `end_map`, `end_zone`, `failures`, `killer` "
            "FROM `character_hardcore` WHERE `guid` = {}", guid))
        {
            Field* f = result->Fetch();
            st.status     = f[0].Get<uint8>();
            st.startTime  = f[1].Get<uint32>();
            st.startLevel = f[2].Get<uint8>();
            st.endTime    = f[3].Get<uint32>();
            st.endLevel   = f[4].Get<uint8>();
            st.endMap     = f[5].Get<uint16>();
            st.endZone    = f[6].Get<uint16>();
            st.failures   = f[7].Get<uint8>();
            st.killer     = f[8].Get<std::string>();
        }

        std::unique_lock<std::shared_mutex> lock(g_mutex);
        g_cache[guid] = st;
    }

    void ApplyMarker(Player* player)
    {
        if (player && player->IsAlive() && !player->HasAura(HC_SPELL_MARKER))
            player->AddAura(HC_SPELL_MARKER, player);
    }

    void RemoveMarker(Player* player)
    {
        if (player)
            player->RemoveAurasDueToSpell(HC_SPELL_MARKER);
    }

    // Hardkora galima pradeti TIK ka sukurus personaza: kol lygis nevirsija ribos ir dar negauta jokios patirties.
    bool IsFreshCharacter(Player* player, HardcoreConfig const& cfg)
    {
        if (player->GetLevel() > cfg.maxLevelToStart)
            return false;

        return !cfg.requireZeroXp || player->GetUInt32Value(PLAYER_XP) == 0;
    }

    // Aktyvuoja hardkora zaidejui (patikras atlieka kvietejas).
    void Activate(Player* player)
    {
        uint32 const guid = player->GetGUID().GetCounter();
        HardcoreState st;
        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            HardcoreState& s = g_cache[guid];
            s.status = HC_STATUS_ACTIVE;
            s.startTime = uint32(GameTime::GetGameTime().count());
            s.startLevel = player->GetLevel();
            s.endTime = 0;
            s.endLevel = 0;
            s.endMap = 0;
            s.endZone = 0;
            s.killer.clear();
            s.pendingKiller.clear();
            st = s;
        }
        SaveState(guid, st);

        // Hardkore PvP zymos nebus - nuimam, jei buvo ijungta.
        if (player->IsPvP())
            player->UpdatePvP(false, true);

        ApplyMarker(player);
    }

    void AnnounceDeath(std::string const& name, uint8 level, std::string const& killer)
    {
        if (!Cfg().announceDeath)
            return;

        if (killer.empty())
            Broadcast(Fmt(HC_STR_ANNOUNCE_DEATH_ENV, name, uint32(level)));
        else
            Broadcast(Fmt(HC_STR_ANNOUNCE_DEATH, name, uint32(level), killer));
    }

    uint8 MaxPlayerLevel()
    {
        return uint8(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
    }

    // Isduoda apdovanojima: pasiekima (jis per `achievement_reward` suteikia ir titula), titula (saugiklis, jei pasiekimas
    // jo neidave, pvz. GM rezimas) ir jojamojo gyvuno burta. Idempotentiska - galima kviesti pakartotinai.
    void GrantReward(Player* player, HardcoreConfig const& cfg)
    {
        if (!cfg.rewardEnable)
            return;

        if (cfg.rewardAchievement)
        {
            if (AchievementEntry const* achievement = sAchievementStore.LookupEntry(cfg.rewardAchievement))
                player->CompletedAchievement(achievement);
            else
                LOG_ERROR("module", "mod-hardcore: pasiekimas {} nerastas (Achievement.dbc / achievement_dbc)", cfg.rewardAchievement);
        }

        if (cfg.rewardTitle)
        {
            if (CharTitlesEntry const* title = sCharTitlesStore.LookupEntry(cfg.rewardTitle))
            {
                if (!player->HasTitle(title))
                    player->SetTitle(title);
            }
            else
                LOG_ERROR("module", "mod-hardcore: titulas {} nerastas (CharTitles.dbc / chartitles_dbc)", cfg.rewardTitle);
        }

        if (cfg.rewardMountSpell && !player->HasSpell(cfg.rewardMountSpell))
            player->learnSpell(cfg.rewardMountSpell);
    }

    // Hardkoras ivyktas: zaidejas pasieke maksimalu lygi GYVAS. Isduodamas apdovanojimas; jei Hardcore.EndAtMaxLevel,
    // rezimas baigiamas (apribojimai nuimami, aura dingsta), kitaip lieka aktyvus. Kviecama is OnPlayerLevelChanged ir prisijungus.
    void CompleteHardcore(Player* player, HardcoreConfig const& cfg)
    {
        uint32 const guid = player->GetGUID().GetCounter();
        HardcoreState st;
        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            auto it = g_cache.find(guid);
            if (it == g_cache.end() || it->second.status != HC_STATUS_ACTIVE)
                return;

            HardcoreState& s = it->second;
            if (cfg.endAtMaxLevel)
            {
                s.status = HC_STATUS_COMPLETED;
                s.endTime = uint32(GameTime::GetGameTime().count());
                s.endLevel = player->GetLevel();
                s.endMap = uint16(player->GetMapId());
                s.endZone = uint16(player->GetZoneId());
                s.killer.clear();
                s.pendingKiller.clear();
            }
            st = s;
        }

        // pasiekimo (ir jo titulo) pakartotinis kvietimas nieko nedaro, o pranesimas siunciamas tik pirma karta
        bool const firstTime = !(cfg.rewardAchievement && player->HasAchieved(cfg.rewardAchievement));

        if (cfg.endAtMaxLevel)
        {
            SaveState(guid, st);
            RemoveMarker(player);
        }

        GrantReward(player, cfg);

        if (!firstTime)
            return;

        SendTo(player, Fmt(HC_STR_COMPLETED_SELF, uint32(player->GetLevel())));
        if (cfg.announceComplete)
            BroadcastBanner(player, Fmt(HC_STR_BANNER_COMPLETED, player->GetName(), uint32(player->GetLevel())), cfg.announceSound);

        LOG_INFO("module", "mod-hardcore: {} (guid {}) pasieke {} lygi gyvas - hardkoras ivyktas.",
            player->GetName(), guid, uint32(player->GetLevel()));
    }

    // Kvieciama is OnPlayerKilledByCreature / OnPlayerPVPKill. SVARBU: sie kvietimai ivyksta PRIES OnPlayerJustDied
    // (Unit::Kill nustato JustDied busena, o Player::KillPlayer - kitame zaidejo Update), todel zudika
    // uzsirasom laikinai ir panaudojam mirties apdorojime.
    void SetKiller(uint32 guid, std::string const& killerText)
    {
        std::unique_lock<std::shared_mutex> lock(g_mutex);
        auto it = g_cache.find(guid);
        if (it != g_cache.end() && it->second.status == HC_STATUS_ACTIVE)
            it->second.pendingKiller = killerText;
    }

    // Ar galima pradeti hardkora? Grazina 0 (galima) arba module_string ID su priezastimi.
    uint32 CheckCanStart(Player* player, HardcoreConfig const& cfg)
    {
        if (!cfg.enable)
            return HC_STR_DISABLED;

        if (!IsRealPlayer(player))
            return HC_STR_BOTS_DENIED;

        HardcoreState const st = GetState(player->GetGUID().GetCounter());
        if (st.status == HC_STATUS_ACTIVE)
            return HC_STR_ALREADY_ACTIVE;

        if (st.status == HC_STATUS_COMPLETED)
            return HC_STR_ALREADY_COMPLETED;

        if (st.status == HC_STATUS_FAILED && !cfg.allowRestartAfterDeath)
            return HC_STR_ALREADY_FAILED;

        if (!player->IsAlive())
            return HC_STR_MUST_BE_ALIVE;

        if (!IsFreshCharacter(player, cfg))
            return HC_STR_TOO_HIGH_LEVEL;

        if (player->GetGroup())
            return HC_STR_START_LEAVE_GROUP;

        if (Map* map = player->GetMap())
            if (map->IsDungeon() || map->IsBattlegroundOrArena())
                return HC_STR_START_OPEN_WORLD;

        return 0;
    }

    std::string DenyText(uint32 code, Player* player, HardcoreConfig const& cfg)
    {
        (void)player;
        (void)cfg;
        return Str(code);
    }

    // Pradeda hardkora (bendra komandai ir NPC). Grazina true, jei pradeta.
    bool StartHardcore(Player* player, std::string& reply)
    {
        HardcoreConfig const cfg = Cfg();
        if (uint32 const denied = CheckCanStart(player, cfg))
        {
            reply = DenyText(denied, player, cfg);
            return false;
        }

        Activate(player);
        reply = Str(HC_STR_STARTED);
        if (cfg.announceStart)
            Broadcast(Fmt(HC_STR_ANNOUNCE_START, player->GetName(), uint32(player->GetLevel())));

        return true;
    }

    bool IsBotPlayer(Player* player)
    {
        return player && player->GetSession() && player->GetSession()->IsBot();
    }

    // Grupes patikra: ar sie du zaidejai gali buti vienoje grupeje?
    // Neleidziama, jei bent vienas is ju hardkoro zaidejas (nebent kitas - botas, o botai leidziami).
    bool GroupAllowed(Player* a, Player* b, HardcoreConfig const& cfg)
    {
        bool const aHc = IsActive(a);
        bool const bHc = IsActive(b);
        if (!aHc && !bHc)
            return true;

        if (aHc && bHc)
            return false;

        Player* other = aHc ? b : a;
        return cfg.allowBotGroups && IsBotPlayer(other);
    }

    // Kelio i musio lauka / pozemi tikrinimas pagal zemelapio ID.
    bool IsRestrictedMap(uint32 mapId, HardcoreConfig const& cfg, bool& isBattleground)
    {
        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        if (!entry)
            return false;

        isBattleground = entry->IsBattlegroundOrArena();
        if (isBattleground)
            return cfg.blockBattlegrounds;

        return entry->IsDungeon() && cfg.blockDungeons;
    }
}

class HardcorePlayerScript : public PlayerScript
{
public:
    HardcorePlayerScript() : PlayerScript("HardcorePlayerScript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_PLAYER_JUST_DIED,
            PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
            PLAYERHOOK_ON_PVP_KILL,
            PLAYERHOOK_ON_LEVEL_CHANGED,
            PLAYERHOOK_ON_DELETE_FROM_DB,
            PLAYERHOOK_ON_GIVE_EXP,
            PLAYERHOOK_ON_PLAYER_PVP_FLAG_CHANGE,
            PLAYERHOOK_ON_DUEL_REQUEST,
            PLAYERHOOK_CAN_GROUP_INVITE,
            PLAYERHOOK_CAN_GROUP_ACCEPT,
            PLAYERHOOK_CAN_JOIN_IN_BATTLEGROUND_QUEUE,
            PLAYERHOOK_CAN_JOIN_IN_ARENA_QUEUE,
            PLAYERHOOK_CAN_JOIN_LFG,
            PLAYERHOOK_CAN_ENTER_MAP,
            PLAYERHOOK_ON_BEFORE_TELEPORT
        }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!IsRealPlayer(player))
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        LoadState(guid);

        HardcoreConfig const cfg = Cfg();
        if (!cfg.enable)
            return;

        HardcoreState const st = GetState(guid);
        if (st.status != HC_STATUS_ACTIVE)
            RemoveMarker(player);   // pasilikusi aura (pvz. po GM atstatymo) - nuimam

        // Pasiekes maksimalu lygi dar buvo aktyvus (pvz. serveris sustojo prie issaugant) - apdovanojam dabar.
        if (st.status == HC_STATUS_ACTIVE && cfg.rewardEnable && player->GetLevel() >= MaxPlayerLevel())
        {
            CompleteHardcore(player, cfg);
            if (!IsActive(player))
                return;
        }

        if (st.status == HC_STATUS_ACTIVE)
        {
            SendTo(player, Str(HC_STR_LOGIN_ACTIVE));
            ApplyMarker(player);

            // Grupe galejo islikti is ankstesnes sesijos - hardkoras grupese buti negali.
            if (cfg.blockGroups)
                if (Group* group = player->GetGroup())
                {
                    bool violation = false;
                    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    {
                        Player* member = ref->GetSource();
                        if (member && member != player && !GroupAllowed(player, member, cfg))
                            violation = true;
                    }

                    if (violation)
                    {
                        group->RemoveMember(player->GetGUID());
                        SendTo(player, Str(HC_STR_GROUP_LEFT));
                    }
                }

            if (cfg.blockPvP && player->IsPvP())
                player->UpdatePvP(false, true);
        }
        else if (cfg.loginHint && IsFreshCharacter(player, cfg) &&
            (st.status == HC_STATUS_NONE || cfg.allowRestartAfterDeath))
            SendTo(player, Str(HC_STR_LOGIN_HINT));
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        std::unique_lock<std::shared_mutex> lock(g_mutex);
        g_cache.erase(player->GetGUID().GetCounter());
    }

    // Mirtis: hardkoras panaikinamas, personazas lieka ir toliau zaidzia kaip iprastas.
    void OnPlayerJustDied(Player* player) override
    {
        if (!IsRealPlayer(player) || !Cfg().enable)
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        HardcoreState st;
        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            auto it = g_cache.find(guid);
            if (it == g_cache.end() || it->second.status != HC_STATUS_ACTIVE)
                return;

            HardcoreState& s = it->second;
            s.status = HC_STATUS_FAILED;
            s.endTime = uint32(GameTime::GetGameTime().count());
            s.endLevel = player->GetLevel();
            s.endMap = uint16(player->GetMapId());
            s.endZone = uint16(player->GetZoneId());
            s.failures = uint8(std::min<uint32>(255, uint32(s.failures) + 1));
            s.killer = s.pendingKiller;
            s.pendingKiller.clear();
            st = s;
        }

        SaveState(guid, st);
        RemoveMarker(player);
        SendTo(player, Str(HC_STR_DIED_SELF));
        AnnounceDeath(player->GetName(), player->GetLevel(), st.killer);
        LOG_INFO("module", "mod-hardcore: {} (guid {}) zuvo {} lygyje, hardkoras panaikintas (zudikas: {}).",
            player->GetName(), guid, uint32(player->GetLevel()), st.killer.empty() ? "aplinka" : st.killer);
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (!killer || !killed)
            return;

        SetKiller(killed->GetGUID().GetCounter(), killer->GetName());
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (!killer || !killed)
            return;

        SetKiller(killed->GetGUID().GetCounter(), Fmt(HC_STR_KILLER_PLAYER, killer->GetName()));
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (!IsRealPlayer(player))
            return;

        HardcoreConfig const cfg = Cfg();
        if (!cfg.enable)
            return;

        uint8 const level = player->GetLevel();
        if (level <= oldLevel)
            return;

        // Maksimalus lygis gyvam: apdovanojimas (vietoj iprasto "kas 10 lygiu" pranesimo)
        if (cfg.rewardEnable && level >= MaxPlayerLevel() && IsActive(player))
        {
            CompleteHardcore(player, cfg);
            return;
        }

        if (cfg.milestoneEvery == 0 || (level % cfg.milestoneEvery) != 0)
            return;

        if (!IsActive(player))
            return;

        Broadcast(Fmt(HC_STR_ANNOUNCE_MILESTONE, player->GetName(), uint32(level)));
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `character_hardcore` WHERE `guid` = {}", guid);
    }

    // Patirties tasku reitas: is gauto kiekio pasaliname serverio reita ir taikome hardkoro reita.
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 xpSource) override
    {
        if (!IsActive(player) || amount == 0)
            return;

        float worldRate = 1.0f;
        switch (xpSource)
        {
            case XPSOURCE_KILL:     worldRate = sWorld->getRate(RATE_XP_KILL); break;
            case XPSOURCE_QUEST:    worldRate = sWorld->getRate(RATE_XP_QUEST); break;
            case XPSOURCE_QUEST_DF: worldRate = sWorld->getRate(RATE_XP_QUEST_DF); break;
            case XPSOURCE_EXPLORE:  worldRate = sWorld->getRate(RATE_XP_EXPLORE); break;
            default: return;
        }

        if (worldRate <= 0.0f)
            return;

        HardcoreConfig const cfg = Cfg();
        amount = uint32(float(amount) / worldRate * cfg.xpRate);
    }

    // PvP zymos hardkoro zaidejui ijungti negalima.
    void OnPlayerPVPFlagChange(Player* player, bool state) override
    {
        if (!state || !player || !Cfg().blockPvP || !IsActive(player))
            return;

        player->UpdatePvP(false, true);
    }

    // Dvikovos su hardkoro zaidejais draudziamos.
    void OnPlayerDuelRequest(Player* target, Player* challenger) override
    {
        if (!target || !challenger || !Cfg().blockPvP)
            return;

        if (!IsActive(target) && !IsActive(challenger))
            return;

        challenger->DuelComplete(DUEL_INTERRUPTED);
        SendTo(challenger, Str(HC_STR_DUEL_DENIED));
        SendTo(target, Str(HC_STR_DUEL_DENIED));
    }

    bool OnPlayerCanGroupInvite(Player* player, std::string& membername) override
    {
        HardcoreConfig const cfg = Cfg();
        if (!cfg.enable || !cfg.blockGroups || !player)
            return true;

        Player* invited = ObjectAccessor::FindPlayerByName(membername, false);
        if (GroupAllowed(player, invited, cfg))
            return true;

        if (IsActive(player))
            SendTo(player, Str(HC_STR_GROUP_DENIED_SELF));
        else
            SendTo(player, Str(HC_STR_GROUP_DENIED_OTHER));
        return false;
    }

    bool OnPlayerCanGroupAccept(Player* player, Group* group) override
    {
        HardcoreConfig const cfg = Cfg();
        if (!cfg.enable || !cfg.blockGroups || !player || !group)
            return true;

        bool allowed = true;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != player && !GroupAllowed(player, member, cfg))
                allowed = false;
        }

        if (allowed)
            return true;

        SendTo(player, IsActive(player) ? Str(HC_STR_GROUP_DENIED_SELF) : Str(HC_STR_GROUP_DENIED_OTHER));
        return false;
    }

    bool OnPlayerCanJoinInBattlegroundQueue(Player* player, ObjectGuid /*battlemasterGuid*/, BattlegroundTypeId /*bgTypeId*/,
        uint8 /*joinAsGroup*/, GroupJoinBattlegroundResult& err) override
    {
        if (!Cfg().blockBattlegrounds || !IsActive(player))
            return true;

        SendTo(player, Str(HC_STR_BG_DENIED));
        err = ERR_GROUP_JOIN_BATTLEGROUND_FAIL;
        return false;
    }

    bool OnPlayerCanJoinInArenaQueue(Player* player, ObjectGuid /*battlemasterGuid*/, uint8 /*arenaslot*/,
        BattlegroundTypeId /*bgTypeId*/, uint8 /*joinAsGroup*/, uint8 /*isRated*/, GroupJoinBattlegroundResult& err) override
    {
        if (!Cfg().blockBattlegrounds || !IsActive(player))
            return true;

        SendTo(player, Str(HC_STR_BG_DENIED));
        err = ERR_GROUP_JOIN_BATTLEGROUND_FAIL;
        return false;
    }

    bool OnPlayerCanJoinLfg(Player* player, uint8 /*roles*/, std::set<uint32>& /*dungeons*/, std::string const& /*comment*/) override
    {
        if (!Cfg().blockDungeons || !IsActive(player))
            return true;

        SendTo(player, Str(HC_STR_DUNGEON_DENIED));
        return false;
    }

    // Pozemiai: neleidziam iejimo (taip pat kai zaidejas prisijungia esantis pozemyje - bus iskeltas prie ieijimo).
    bool OnPlayerCanEnterMap(Player* player, MapEntry const* entry, InstanceTemplate const* /*instance*/,
        MapDifficulty const* /*mapDiff*/, bool /*loginCheck*/) override
    {
        if (!entry || !entry->IsDungeon() || !Cfg().blockDungeons || !IsActive(player))
            return true;

        SendTo(player, Str(HC_STR_DUNGEON_DENIED));
        return false;
    }

    // Kad hardkoro zaidejas nepatektu i musio lauka ar pozemi pro kitus kelius (portalai, iskvietimai ir pan.).
    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float /*x*/, float /*y*/, float /*z*/, float /*orientation*/,
        uint32 /*options*/, Unit* /*target*/) override
    {
        if (!player || player->GetMapId() == mapid || !IsActive(player))
            return true;

        HardcoreConfig const cfg = Cfg();
        bool isBattleground = false;
        if (!IsRestrictedMap(mapid, cfg, isBattleground))
            return true;

        SendTo(player, Str(isBattleground ? HC_STR_BG_DENIED : HC_STR_DUNGEON_DENIED));
        return false;
    }
};

// Zalos pakeitimai: hardkoro zaidejas negali zaloti kitu zaideju, priesai jam daro daugiau zalos,
// o jo (ir jo augintiniu) zala priesams sumazinama tiek pat, kiek padidinamos priesu gyvybes.
class HardcoreUnitScript : public UnitScript
{
public:
    HardcoreUnitScript() : UnitScript("HardcoreUnitScript", { UNITHOOK_ON_DAMAGE }) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!victim || damage == 0)
            return;

        Player* victimOwner = victim->GetCharmerOrOwnerPlayerOrPlayerItself();
        Player* attackerOwner = attacker ? attacker->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;

        // Greitas iseitis: nei vienoje puseje nera zaidejo.
        if (!victimOwner && !attackerOwner)
            return;

        // 1) hardkoro zaidejas -> kitas zaidejas: zalos nera
        if (attackerOwner && victimOwner && attackerOwner != victimOwner)
        {
            if (IsActive(attackerOwner) && Cfg().blockPvP)
                damage = 0;
            return;
        }

        // 2) priesas -> hardkoro zaidejas (ar jo augintinis): daugiau zalos
        if (victimOwner && !attackerOwner && attacker)
        {
            if (IsActive(victimOwner))
                damage = uint32(float(damage) * Cfg().enemyDamageMult + 0.5f);
            return;
        }

        // 3) hardkoro zaidejas (ar jo augintinis) -> priesas: efektyviai daugiau gyvybiu
        if (attackerOwner && !victimOwner)
        {
            if (IsActive(attackerOwner))
                damage = std::max<uint32>(1, uint32(float(damage) / Cfg().enemyHealthMult));
        }
    }
};

// Saugiklis: jei hardkoro zaidejas vis tiek atsiduria grupeje (botu sistema, GM komandos ir pan.), pasalinam nauja nari.
class HardcoreGroupScript : public GroupScript
{
public:
    HardcoreGroupScript() : GroupScript("HardcoreGroupScript", { GROUPHOOK_ON_ADD_MEMBER }) { }

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        HardcoreConfig const cfg = Cfg();
        if (!cfg.enable || !cfg.blockGroups || !group || group->GetMembersCount() < 2)
            return;

        std::vector<Player*> members;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                members.push_back(member);

        bool violation = false;
        for (size_t i = 0; i < members.size() && !violation; ++i)
            for (size_t j = i + 1; j < members.size(); ++j)
                if (!GroupAllowed(members[i], members[j], cfg))
                {
                    violation = true;
                    break;
                }

        if (!violation)
            return;

        // Pasaliname NAUJA nari su nedideliu uzdelsimu - grupes viduje kablio jos nekeiciam.
        Player* newcomer = ObjectAccessor::FindConnectedPlayer(guid);
        if (!newcomer)
            return;

        SendTo(newcomer, IsActive(newcomer) ? Str(HC_STR_GROUP_DENIED_SELF) : Str(HC_STR_GROUP_DENIED_OTHER));
        newcomer->m_Events.AddEventAtOffset([guid]()
        {
            if (Player* p = ObjectAccessor::FindConnectedPlayer(guid))
                if (Group* g = p->GetGroup())
                    g->RemoveMember(guid);
        }, Milliseconds(200));
    }
};

// Aukcionas: hardkoro zaidejas jo naudotis negali (blokuojam aukciono paketus).
class HardcoreServerScript : public ServerScript
{
public:
    HardcoreServerScript() : ServerScript("HardcoreServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket& packet) override
    {
        switch (packet.GetOpcode())
        {
            case MSG_AUCTION_HELLO:
            case CMSG_AUCTION_SELL_ITEM:
            case CMSG_AUCTION_REMOVE_ITEM:
            case CMSG_AUCTION_LIST_ITEMS:
            case CMSG_AUCTION_LIST_OWNER_ITEMS:
            case CMSG_AUCTION_PLACE_BID:
            case CMSG_AUCTION_LIST_BIDDER_ITEMS:
            case CMSG_AUCTION_LIST_PENDING_SALES:
                break;
            default:
                return true;
        }

        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !Cfg().blockAuction || !IsActive(player))
            return true;

        SendTo(player, Str(HC_STR_AUCTION_DENIED));
        return false;
    }
};

class npc_hardcore_script : public CreatureScript
{
public:
    npc_hardcore_script() : CreatureScript("npc_hardcore") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);

        HardcoreConfig const cfg = Cfg();
        uint32 textId = HC_NPC_TEXT_RULES;

        if (!cfg.enable)
        {
            textId = HC_NPC_TEXT_DISABLED;
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(HC_STR_GOSSIP_OK), GOSSIP_SENDER_MAIN, HC_ACTION_CLOSE);
        }
        else
        {
            HardcoreState const st = IsRealPlayer(player) ? GetState(player->GetGUID().GetCounter()) : HardcoreState();
            if (st.status == HC_STATUS_ACTIVE)
            {
                textId = HC_NPC_TEXT_ACTIVE;
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(HC_STR_GOSSIP_OK), GOSSIP_SENDER_MAIN, HC_ACTION_CLOSE);
            }
            else if (st.status == HC_STATUS_COMPLETED)
            {
                textId = HC_NPC_TEXT_COMPLETED;
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(HC_STR_GOSSIP_OK), GOSSIP_SENDER_MAIN, HC_ACTION_CLOSE);
            }
            else if (st.status == HC_STATUS_FAILED && !cfg.allowRestartAfterDeath)
            {
                textId = HC_NPC_TEXT_FAILED;
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(HC_STR_GOSSIP_OK), GOSSIP_SENDER_MAIN, HC_ACTION_CLOSE);
            }
            else if (!IsFreshCharacter(player, cfg))
            {
                textId = HC_NPC_TEXT_TOO_HIGH;
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(HC_STR_GOSSIP_OK), GOSSIP_SENDER_MAIN, HC_ACTION_CLOSE);
            }
            else
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(HC_STR_GOSSIP_ACCEPT), GOSSIP_SENDER_MAIN, HC_ACTION_ACCEPT);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Str(HC_STR_GOSSIP_DECLINE), GOSSIP_SENDER_MAIN, HC_ACTION_DECLINE);
            }
        }

        SendGossipMenuFor(player, textId, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* /*creature*/, uint32 /*sender*/, uint32 action) override
    {
        if (action == HC_ACTION_ACCEPT)
        {
            std::string reply;
            StartHardcore(player, reply);
            SendTo(player, reply);
        }

        CloseGossipMenuFor(player);
        return true;
    }
};

class HardcoreCommandScript : public CommandScript
{
public:
    HardcoreCommandScript() : CommandScript("HardcoreCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable gmTable =
        {
            { "on",     HandleGmOn,     SEC_GAMEMASTER, Console::No },
            { "reset",  HandleGmReset,  SEC_GAMEMASTER, Console::No },
            { "reward", HandleGmReward, SEC_GAMEMASTER, Console::No },
            { "banner", HandleGmBanner, SEC_GAMEMASTER, Console::No }
        };

        static ChatCommandTable hardcoreTable =
        {
            { "",       HandleInfo,  SEC_PLAYER, Console::No },
            { "info",   HandleInfo,  SEC_PLAYER, Console::No },
            { "status", HandleInfo,  SEC_PLAYER, Console::No },
            { "start",  HandleStart, SEC_PLAYER, Console::No },
            { "gm",     gmTable }
        };

        static ChatCommandTable rootTable =
        {
            { "hardcore", hardcoreTable }
        };

        return rootTable;
    }

    static bool HandleInfo(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        HardcoreConfig const cfg = Cfg();
        if (!cfg.enable)
        {
            handler->SendSysMessage(Str(HC_STR_DISABLED));
            return true;
        }

        handler->SendSysMessage(Str(HC_STR_INFO_HEADER));
        handler->SendSysMessage(Str(HC_STR_INFO_RULES));
        if (cfg.blockGroups)
            handler->SendSysMessage(Str(HC_STR_RULE_GROUPS));
        if (cfg.blockPvP)
            handler->SendSysMessage(Str(HC_STR_RULE_PVP));
        if (cfg.blockBattlegrounds)
            handler->SendSysMessage(Str(HC_STR_RULE_BG));
        if (cfg.blockDungeons)
            handler->SendSysMessage(Str(HC_STR_RULE_DUNGEONS));
        if (cfg.blockAuction)
            handler->SendSysMessage(Str(HC_STR_RULE_AUCTION));
        handler->SendSysMessage(Fmt(HC_STR_RULE_XP, cfg.xpRate));
        handler->SendSysMessage(Fmt(HC_STR_RULE_ENEMIES, cfg.enemyHealthMult, cfg.enemyDamageMult));
        handler->SendSysMessage(Str(HC_STR_RULE_DEATH));
        if (cfg.rewardEnable)
            handler->SendSysMessage(Fmt(HC_STR_RULE_REWARD, uint32(MaxPlayerLevel())));

        HardcoreState const st = GetState(player->GetGUID().GetCounter());
        switch (st.status)
        {
            case HC_STATUS_ACTIVE:
                handler->SendSysMessage(Fmt(HC_STR_STATUS_ACTIVE, uint32(st.startLevel)));
                break;
            case HC_STATUS_COMPLETED:
                handler->SendSysMessage(Fmt(HC_STR_STATUS_COMPLETED, uint32(st.endLevel)));
                break;
            case HC_STATUS_FAILED:
                handler->SendSysMessage(Fmt(HC_STR_STATUS_FAILED, uint32(st.endLevel)));
                if (cfg.allowRestartAfterDeath)
                    handler->SendSysMessage(Str(HC_STR_HOWTO_START));
                break;
            default:
                handler->SendSysMessage(Str(HC_STR_STATUS_NONE));
                handler->SendSysMessage(Str(HC_STR_HOWTO_START));
                break;
        }

        return true;
    }

    static bool HandleStart(ChatHandler* handler, Optional<std::string> confirm)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        HardcoreConfig const cfg = Cfg();

        // Patikros pirma - kad zaidejas nebutu klausiamas patvirtinimo veltui.
        if (uint32 const denied = CheckCanStart(player, cfg))
        {
            handler->SendSysMessage(DenyText(denied, player, cfg));
            return true;
        }

        if (cfg.requireConfirmation)
        {
            std::string answer = confirm ? *confirm : std::string();
            std::transform(answer.begin(), answer.end(), answer.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (answer != "taip" && answer != "yes" && answer != "confirm")
            {
                handler->SendSysMessage(Str(HC_STR_START_WARNING));
                return true;
            }
        }

        std::string reply;
        StartHardcore(player, reply);
        handler->SendSysMessage(reply);
        return true;
    }

    // .hardcore gm on [zaidejas] - priverstinai ijungia (testavimui)
    static bool HandleGmOn(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);

        Player* player = target ? target->GetConnectedPlayer() : nullptr;
        if (!player || !IsRealPlayer(player))
        {
            handler->SendSysMessage(Str(HC_STR_GM_NOT_FOUND));
            return true;
        }

        Activate(player);
        SendTo(player, Str(HC_STR_STARTED));
        handler->SendSysMessage(Fmt(HC_STR_GM_ON, player->GetName()));
        return true;
    }

    // .hardcore gm reward [zaidejas] - testavimui: aktyviam hardkoro zaidejui paleidzia visa "ivykdyta" eiga (busena,
    // pranesimai, apdovanojimas), kitiems tik isduoda apdovanojima. Zaidejas turi buti ne GM rezime (.gm off) - kitaip pasiekimo negaus.
    static bool HandleGmReward(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);

        Player* player = target ? target->GetConnectedPlayer() : nullptr;
        if (!player || !IsRealPlayer(player))
        {
            handler->SendSysMessage(Str(HC_STR_GM_NOT_FOUND));
            return true;
        }

        HardcoreConfig const cfg = Cfg();
        if (IsActive(player))
            CompleteHardcore(player, cfg);
        else
            GrantReward(player, cfg);

        handler->SendSysMessage(Fmt(HC_STR_GM_REWARD, player->GetName()));
        return true;
    }

    // .hardcore gm banner [zaidejas] - testavimui: visiems parodo "hardkoras ivyktas" pranesima ekrano virsuje su garsu
    // (nieko neisduoda ir busenos nekeicia).
    static bool HandleGmBanner(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);

        Player* player = target ? target->GetConnectedPlayer() : nullptr;
        if (!player || !IsRealPlayer(player))
        {
            handler->SendSysMessage(Str(HC_STR_GM_NOT_FOUND));
            return true;
        }

        HardcoreConfig const cfg = Cfg();
        BroadcastBanner(player, Fmt(HC_STR_BANNER_COMPLETED, player->GetName(), uint32(player->GetLevel())), cfg.announceSound);
        return true;
    }

    // .hardcore gm reset [zaidejas] - isvalo busena (galima pradeti is naujo)
    static bool HandleGmReset(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);

        Player* player = target ? target->GetConnectedPlayer() : nullptr;
        if (!player || !IsRealPlayer(player))
        {
            handler->SendSysMessage(Str(HC_STR_GM_NOT_FOUND));
            return true;
        }

        uint32 const guid = player->GetGUID().GetCounter();
        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            g_cache[guid] = HardcoreState();
        }
        CharacterDatabase.Execute("DELETE FROM `character_hardcore` WHERE `guid` = {}", guid);
        RemoveMarker(player);
        handler->SendSysMessage(Fmt(HC_STR_GM_RESET, player->GetName()));
        return true;
    }
};

void AddSC_Hardcore()
{
    new HardcorePlayerScript();
    new HardcoreUnitScript();
    new HardcoreGroupScript();
    new HardcoreServerScript();
    new npc_hardcore_script();
    new HardcoreCommandScript();
}
