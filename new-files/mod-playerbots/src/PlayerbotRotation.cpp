/*
 * NovaCore: zaidejo veikejo AI rezimai (`.rotacija`, `.autopilotas`) - zr. PlayerbotRotation.h
 */

#include "PlayerbotRotation.h"

#include "Chat.h"
#include "Config.h"
#include "Event.h"
#include "Timer.h"
#include "LfgTriggers.h"   // NovaClearStaleFalling
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "NewRpgInfo.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <mutex>

using namespace Acore::ChatCommands;

namespace
{
    // 0 = isjungta, 1 = tik GM (numatyta, kol neisbandyta zaidime), 2 = visi
    int32 RotationLevel()
    {
        return sConfigMgr->GetOption<int32>("AiPlayerbot.NovaRotationLevel", 1);
    }

    int32 AutopilotLevel()
    {
        return sConfigMgr->GetOption<int32>("AiPlayerbot.NovaAutopilotLevel", 1);
    }

    bool LevelAllows(int32 level, Player* player)
    {
        return level >= 2 || (level == 1 && player->GetSession()->GetSecurity() >= SEC_GAMEMASTER);
    }

    constexpr char const* TXT_ON = "|cff00ff00Automatinė rotacija: ĮJUNGTA.|r";
    constexpr char const* TXT_ON_MOVE = "|cff00ff00Automatinė rotacija (su judėjimu): ĮJUNGTA.|r";
    constexpr char const* TXT_OFF = "|cffff8000Automatinė rotacija: IŠJUNGTA.|r";
    constexpr char const* TXT_AP_ON = "|cff00ff00Autopilotas: ĮJUNGTAS.|r Veikėją valdo serveris (lygis, užduotys, profesijos, aukcionas, mūšio laukai). Grąžinti valdymą: |cffffff00.autopilotas off|r";
    constexpr char const* TXT_AP_OFF = "|cffff8000Autopilotas: IŠJUNGTAS.|r Valdymas grąžintas tau.";

    // Pirminiu profesiju skaicius (iki 2 leidziama pagal zaidimo taisykles).
    uint32 PrimaryProfessionCount(Player* p)
    {
        static uint32 const primary[] = { SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING, SKILL_HERBALISM,
                                          SKILL_INSCRIPTION, SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING, SKILL_MINING,
                                          SKILL_SKINNING, SKILL_TAILORING };
        uint32 n = 0;
        for (uint32 s : primary)
            if (p->HasSkill(s))
                ++n;
        return n;
    }
}

PlayerbotRotationMgr* PlayerbotRotationMgr::instance()
{
    static PlayerbotRotationMgr inst;
    return &inst;
}

bool PlayerbotRotationMgr::IsEnabled(ObjectGuid guid) const
{
    std::shared_lock<std::shared_mutex> g(_lock);
    return _states.find(guid) != _states.end();
}

bool PlayerbotRotationMgr::IsStationary(ObjectGuid guid) const
{
    std::shared_lock<std::shared_mutex> g(_lock);
    auto it = _states.find(guid);
    return it != _states.end() && !it->second.autopilot && !it->second.allowMove;
}

bool PlayerbotRotationMgr::IsAutopilot(ObjectGuid guid) const
{
    if (!_autopilotCount.load(std::memory_order_relaxed))
        return false;

    std::shared_lock<std::shared_mutex> g(_lock);
    auto it = _states.find(guid);
    return it != _states.end() && it->second.autopilot;
}

std::vector<ObjectGuid> PlayerbotRotationMgr::AutopilotPlayers() const
{
    std::vector<ObjectGuid> out;
    if (!_autopilotCount.load(std::memory_order_relaxed))
        return out;

    std::shared_lock<std::shared_mutex> g(_lock);
    for (auto const& kv : _states)
        if (kv.second.autopilot)
            out.push_back(kv.first);
    return out;
}

// Bendri reikalavimai abiem rezimams; sukuria zaidejo „self“ AI, jei jo dar nera.
bool PlayerbotRotationMgr::AttachAI(Player* player, std::string& reply, PlayerbotAI*& botAI)
{
    if (!player || !player->GetSession())
        return false;

    if (!sPlayerbotAIConfig->enabled)
    {
        reply = "Ši funkcija šiuo metu nepasiekiama.";
        return false;
    }

    if (player->GetSession()->IsBot())
    {
        reply = "Šios komandos botai nenaudoja.";
        return false;
    }

    botAI = GET_PLAYERBOT_AI(player);
    if (botAI && !botAI->IsRealPlayer())
    {
        reply = "Šio veikėjo valdyti negalima.";
        return false;
    }

    if (!botAI)
    {
        sPlayerbotsMgr->AddPlayerbotData(player, true);
        botAI = GET_PLAYERBOT_AI(player);
        if (!botAI)
        {
            reply = "Nepavyko paleisti AI.";
            return false;
        }
        botAI->SetMaster(player);
    }

    // NovaCore: zaidejo veikejas niekada neturi likti „Pasitraukes“ (AFK) del AI aktyvumo perjungimo
    if (player->isAFK())
        player->ToggleAFK();

    return true;
}

bool PlayerbotRotationMgr::Enable(Player* player, bool allowMove, std::string& reply)
{
    if (!player || !player->GetSession())
        return false;

    if (!LevelAllows(RotationLevel(), player))
    {
        reply = "Automatinė rotacija šiame serveryje išjungta.";
        return false;
    }

    if (player->InBattleground() || player->InArena())
    {
        reply = "Mūšio laukuose ir arenose automatinė rotacija negalima.";
        return false;
    }

    // Jei veike autopilotas – isjungiam pries pradedant (grazinama kliento kontrole, AI sukuriamas is naujo).
    if (IsAutopilot(player->GetGUID()))
    {
        std::string ignored;
        Disable(player, ignored);
    }

    PlayerbotAI* botAI = nullptr;
    if (!AttachAI(player, reply, botAI))
        return false;

    {
        std::unique_lock<std::shared_mutex> g(_lock);
        State& st = _states[player->GetGUID()];
        st.allowMove = allowMove;
        st.autopilot = false;
    }

    // Pilnas strategiju perkrovimas pagal dabartinę klasę, specializaciją ir lygį; Apply() jį apriboja.
    botAI->ResetStrategies();
    Apply(botAI);

    reply = allowMove ? TXT_ON_MOVE : TXT_ON;
    return true;
}

bool PlayerbotRotationMgr::EnableAutopilot(Player* player, std::string& reply)
{
    if (!player || !player->GetSession())
        return false;

    if (!LevelAllows(AutopilotLevel(), player))
    {
        reply = "Autopilotas šiame serveryje išjungtas.";
        return false;
    }

    if (player->InArena())
    {
        reply = "Arenose autopilotas negalimas.";
        return false;
    }

    // Pradedam is svaraus AI (jei veike rotacija – nuimam).
    if (IsEnabled(player->GetGUID()))
    {
        std::string ignored;
        Disable(player, ignored);
    }

    PlayerbotAI* botAI = nullptr;
    if (!AttachAI(player, reply, botAI))
        return false;

    {
        std::unique_lock<std::shared_mutex> g(_lock);
        State& st = _states[player->GetGUID()];
        st.allowMove = true;
        st.autopilot = true;
        _autopilotCount.fetch_add(1, std::memory_order_relaxed);
    }

    LOG_INFO("playerbots", "NovaCore autopilotas: ijungtas veikejui {} ({}, lygis {})", player->GetName(),
             player->GetGUID().ToString(), player->GetLevel());

    // Pilnas strategiju perkrovimas, Apply() pritaiko autopilotui (ResetStrategies jau kviecia Apply, bet is anksto aisku).
    botAI->ResetStrategies();
    Apply(botAI);

    SetupAutopilotCharacter(player, botAI);

    // Serverio varomas judejimas: klientas neturi siusti savo pozicijos, kitaip abu tempia veikeja i skirtingas puses.
    player->SetFallInformation(0, player->GetPositionZ());
    player->SetClientControl(player, false);

    reply = TXT_AP_ON;
    return true;
}

// Vienkartinis paruosimas: profesijos (jei nera) ir lygio „priežiūra“ (talentai, burtai pas treneri nereikalingi).
void PlayerbotRotationMgr::SetupAutopilotCharacter(Player* player, PlayerbotAI* botAI)
{
    // Dvi pirminės profesijos – medžiagų rinkimas (kasyba, žolininkystė); aukštesnius lygius veikėjas išmoksta gaudamas
    // įgūdžių taškus renkant, o naujus rangus – pas trenerius (TrainerAction).
    if (PrimaryProfessionCount(player) < 2)
    {
        if (!player->HasSkill(SKILL_MINING))
        {
            player->learnSpell(2575);   // Mining (Apprentice)
            if (!player->HasSkill(SKILL_MINING))
                player->SetSkill(SKILL_MINING, 1, 1, 75);
        }

        if (PrimaryProfessionCount(player) < 2 && !player->HasSkill(SKILL_HERBALISM))
        {
            player->learnSpell(2366);   // Herb Gathering (Apprentice)
            if (!player->HasSkill(SKILL_HERBALISM))
                player->SetSkill(SKILL_HERBALISM, 1, 1, 75);
        }
    }

    // Talentai ir naujai prieinami klasės burtai (kaip level-up botams). Burtai issaugomi DB (zr. NovaPersistNewTemporarySpells).
    botAI->DoSpecificAction("auto maintenance on levelup", Event(), true);
}

bool PlayerbotRotationMgr::Disable(Player* player, std::string& reply)
{
    if (!player)
        return false;

    bool was = false;
    bool wasAutopilot = false;
    {
        std::unique_lock<std::shared_mutex> g(_lock);
        auto it = _states.find(player->GetGUID());
        if (it != _states.end())
        {
            was = true;
            wasAutopilot = it->second.autopilot;
            if (wasAutopilot)
                _autopilotCount.fetch_sub(1, std::memory_order_relaxed);
            _states.erase(it);
        }
    }

    if (was)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
        if (botAI && botAI->IsRealPlayer())
            delete botAI;   // PlayerbotAI destruktorius pats pasalina irasa is PlayerbotsMgr

        if (player->isAFK())
            player->ToggleAFK();

        if (wasAutopilot && player->GetSession())
        {
            // Diagnostika: zaidejai skundesi, kad isjungus autopiloto mygtuka veikejas netenka ~puses gyvybiu (kritimo zala?).
            float const gz = player->GetMap() ? player->GetMap()->GetHeight(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
                                                                          player->GetPositionZ() + 2.0f, true) : player->GetPositionZ();
            LOG_INFO("playerbots", "Autopilotas {}: isjungiamas, HP {}/{}, z {:.1f}, zemes z {:.1f}, krenta {}, judejimo vėliavos {:#x}",
                     player->GetName(), player->GetHealth(), player->GetMaxHealth(), player->GetPositionZ(), gz,
                     player->IsFalling() ? "taip" : "ne", uint32(player->GetUnitMovementFlags()));

            player->StopMoving();
            player->GetMotionMaster()->Clear();
            // pasenusi kritimo informacija (is laiko prie autopilota) galetu duoti didele kritimo zala pirmo zaidejo zingsnio metu
            player->SetFallInformation(0, player->GetPositionZ());
            player->SetClientControl(player, true);
        }
    }

    reply = wasAutopilot ? TXT_AP_OFF : TXT_OFF;
    return was;
}

void PlayerbotRotationMgr::ApplyAutopilot(PlayerbotAI* botAI, Player* bot)
{
    bool const inBg = bot->InBattleground() || bot->InArena();

    // Be pokalbiu su kitais zaidejais, be dvikovu, be sekimo (savininko nera), be PvP uzpuldinejimo.
    botAI->ChangeStrategy("-follow,-chat,-duel,-start duel,-emote,-pvp,-stay", BOT_STATE_NON_COMBAT);
    botAI->ChangeStrategy("-chat,-duel,-formation", BOT_STATE_COMBAT);
    botAI->ChangeStrategy("-chat,-follow", BOT_STATE_DEAD);

    if (inBg)
        return;     // musio lauke strategijas parenka AiFactory (BG tactics)

    // Kaip random botai: grind + naujoji RPG (uzduotys, klajojimas, skrydziai), musio laukai, LFG pozemiai.
    std::string add = "+grind,";
    add += sPlayerbotAIConfig->enableNewRpgStrategy ? "+new rpg" : "+rpg";
    if (sPlayerbotAIConfig->randomBotJoinBG)
        add += ",+bg";
    if (sPlayerbotAIConfig->randomBotJoinLfg)
        add += ",+lfg";
    botAI->ChangeStrategy(add, BOT_STATE_NON_COMBAT);
}

void PlayerbotRotationMgr::Apply(PlayerbotAI* botAI)
{
    if (!botAI)
        return;

    Player* bot = botAI->GetBot();
    if (!bot || !botAI->IsRealPlayer())
        return;

    bool allowMove = false;
    bool autopilot = false;
    {
        std::shared_lock<std::shared_mutex> g(_lock);
        auto it = _states.find(bot->GetGUID());
        if (it == _states.end())
            return;
        allowMove = it->second.allowMove;
        autopilot = it->second.autopilot;
    }

    if (autopilot)
    {
        ApplyAutopilot(botAI, bot);
        return;
    }

    // Ne kovos ir mirties varikliai tušti: AI veikia tik kovoje, niekada nerenka grobio, nekalbasi, neatsikelia ir pan.
    botAI->ClearStrategies(BOT_STATE_NON_COMBAT);
    botAI->ClearStrategies(BOT_STATE_DEAD);

    std::string remove = "-chat,-default,-duel";
    if (!allowMove)
        remove += ",-formation,-avoid aoe,-behind,-tank face,-flee";
    botAI->ChangeStrategy(remove, BOT_STATE_COMBAT);
}

namespace
{
    bool ValidTarget(Player* bot, Unit* t)
    {
        return t && t->IsInWorld() && t->IsAlive() && bot->IsValidAttackTarget(t);
    }

    // Taikinio parinkimas: 1) ka veikejas jau kerta, 2) zaidejo pasirinktas taikinys, 3) AI „dps target“ (geriausias
    // is puolanciu), 4) artimiausias puolantis priesas, matomas ir ne toliau kaip 40 m.
    Unit* PickTarget(PlayerbotAI* botAI, Player* bot)
    {
        Unit* t = bot->GetVictim();
        if (ValidTarget(bot, t))
            return t;

        t = bot->GetSelectedUnit();
        if (ValidTarget(bot, t))
            return t;

        t = botAI->GetAiObjectContext()->GetValue<Unit*>("dps target")->Get();
        if (ValidTarget(bot, t) && bot->IsWithinLOSInMap(t))
            return t;

        Unit* best = nullptr;
        float bestDist = 40.0f;
        for (Unit* a : bot->getAttackers())
        {
            if (!ValidTarget(bot, a) || !bot->IsWithinLOSInMap(a))
                continue;
            float d = bot->GetDistance(a);
            if (d < bestDist)
            {
                bestDist = d;
                best = a;
            }
        }
        return best;
    }
}

void PlayerbotRotationMgr::PreAction(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !botAI->IsRealPlayer())
        return;

    // Autopilotas: likutine „krenta“ busena (botas/zaidejas be kliento nusileidimo) rodo „skridimo“ animacija ir blokuoja teleportus
    if (IsAutopilot(bot->GetGUID()) && bot->IsAlive() && !bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
        NovaClearStaleFalling(bot);

    // Autopilotas: diagnostika – ar veikejas neperejo kiaurai statiniu kliuciu. Kas ~1 s lyginam su ankstesne pozicija; jei tarp ju
    // nera tiesioginio matomumo (VMAP/WMO) – irasom i zurnala (ne dazniau nei kas 5 s), kad butu aisku, kur ir kaip tai nutinka.
    if (IsAutopilot(bot->GetGUID()) && bot->IsAlive() && bot->IsInWorld() && !bot->IsFalling() && !bot->isSwimming() && !bot->IsFlying() &&
        !bot->GetTransport() && !bot->GetVehicle() && !bot->IsInFlight() && !bot->IsBeingTeleported())
    {
        struct LastPos
        {
            uint32 map = 0;
            float x = 0.0f, y = 0.0f, z = 0.0f;
            uint32 t = 0;
            uint32 warnedAt = 0;
        };
        static std::mutex lockPos;
        static std::unordered_map<ObjectGuid, LastPos> lastPos;

        uint32 const nowMs = getMSTime();
        std::lock_guard<std::mutex> guard(lockPos);
        LastPos& lp = lastPos[bot->GetGUID()];
        if (!lp.t || nowMs - lp.t >= 1000)
        {
            float const cx = bot->GetPositionX(), cy = bot->GetPositionY(), cz = bot->GetPositionZ();
            if (lp.t && lp.map == bot->GetMapId())
            {
                float const d = std::hypot(cx - lp.x, cy - lp.y);
                if (d > 3.0f && d < 20.0f && nowMs - lp.warnedAt >= 5000 &&
                    !bot->GetMap()->isInLineOfSight(lp.x, lp.y, lp.z + 1.4f, cx, cy, cz + 1.4f, bot->GetPhaseMask(),
                                                    LineOfSightChecks(LINEOFSIGHT_CHECK_VMAP | LINEOFSIGHT_CHECK_GOBJECT_WMO),
                                                    VMAP::ModelIgnoreFlags::Nothing))
                {
                    lp.warnedAt = nowMs;
                    LOG_INFO("playerbots", "Autopilotas {}: KIAURAI KLIUTIES? ({:.0f}, {:.0f}, {:.1f}) -> ({:.0f}, {:.0f}, {:.1f}), zona {}",
                             bot->GetName(), lp.x, lp.y, lp.z, cx, cy, cz, bot->GetZoneId());
                }
            }

            lp.map = bot->GetMapId();
            lp.x = cx;
            lp.y = cy;
            lp.z = cz;
            lp.t = nowMs;
        }
    }

    // Autopilotas: kas ~10 s irasom busena i Playerbots.log (diagnostikai: kur yra, ka daro, kokie paskutiniai veiksmai).
    if (IsAutopilot(bot->GetGUID()))
    {
        static std::unordered_map<ObjectGuid, uint32> lastTrace;
        uint32 const now = getMSTime();
        uint32& last = lastTrace[bot->GetGUID()];
        if (now - last >= 10000)
        {
            last = now;
            static char const* const rpgNames[] = { "medziot", "stovykla", "klajoja", "NPC", "uzduotis", "skrydis", "ilsisi", "ramybe" };
            uint32 const st = uint32(botAI->rpgInfo.status);
            std::string actions = botAI->HandleRemoteCommand("action");
            if (actions.size() > 260)
                actions = actions.substr(actions.size() - 260);
            float const groundZ = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            LOG_INFO("playerbots", "Autopilotas {} @ ({:.0f}, {:.0f}, z {:.1f}, zeme {:.1f}, vel. 0x{:x}) zona {} lvl {} | variklis {} | rpg {} | laisvu vietu {} | veiksmai {}",
                     bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), groundZ,
                     bot->m_movementInfo.GetMovementFlags(), bot->GetZoneId(), bot->GetLevel(),
                     uint32(botAI->GetState()), st < 8 ? rpgNames[st] : "?", bot->GetFreeInventorySpace(), actions);
        }
    }

    if (!bot->IsAlive())
        return;

    // Tik paprasta rotacija (autopilotas kovos varikli perjungia pats, kaip botai, per „dps assist“ / „grind“).
    {
        std::shared_lock<std::shared_mutex> g(_lock);
        auto it = _states.find(bot->GetGUID());
        if (it == _states.end() || it->second.autopilot)
            return;
    }

    BotState state = botAI->GetState();
    if (bot->IsInCombat())
    {
        if (state != BOT_STATE_NON_COMBAT)
            return;

        // Perjungiam tik turint tinkama taikini (kitaip kovos variklio „invalid target“ iskart grazintu atgal ir kiekviena
        // AI cikla variklis butu perkraunamas). Mirus taikiniui automatiskai parenkamas kitas puolantis priesas.
        Unit* target = PickTarget(botAI, bot);
        if (!target)
            return;

        botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
        bot->SetSelection(target->GetGUID());
        botAI->ChangeEngine(BOT_STATE_COMBAT);
    }
    else if (state == BOT_STATE_COMBAT)
    {
        botAI->ChangeEngine(BOT_STATE_NON_COMBAT);
    }
}

std::string PlayerbotRotationMgr::Describe(Player* player)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI || !botAI->IsRealPlayer() || !IsEnabled(player->GetGUID()))
        return "";

    std::string out = "Variklis: ";
    switch (botAI->GetState())
    {
        case BOT_STATE_COMBAT:     out += "kovos"; break;
        case BOT_STATE_NON_COMBAT: out += "ne kovos"; break;
        default:                   out += "mirties"; break;
    }
    out += "; kovoje: ";
    out += player->IsInCombat() ? "taip" : "ne";

    auto list = [&](char const* title, BotState st)
    {
        out += "; ";
        out += title;
        out += ": ";
        bool first = true;
        for (std::string const& s : botAI->GetStrategies(st))
        {
            if (!first)
                out += ", ";
            out += s;
            first = false;
        }
    };

    list("kovos strategijos", BOT_STATE_COMBAT);
    if (IsAutopilot(player->GetGUID()))
    {
        list("ne kovos strategijos", BOT_STATE_NON_COMBAT);

        static char const* const rpgNames[] = { "eina medziot", "eina i stovykla", "klajoja", "kalbasi su NPC", "uzduotis",
                                                "skrenda", "ilsisi", "ramybe" };
        uint32 const st = uint32(botAI->rpgInfo.status);
        out += "; RPG: ";
        out += st < sizeof(rpgNames) / sizeof(rpgNames[0]) ? rpgNames[st] : "?";

        uint32 incomplete = 0, complete = 0;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 q = player->GetQuestSlotQuestId(slot);
            if (!q)
                continue;
            if (player->GetQuestStatus(q) == QUEST_STATUS_COMPLETE)
                ++complete;
            else
                ++incomplete;
        }
        out += "; uzduotys: vykdoma " + std::to_string(incomplete) + ", ivykdyta (reikia atiduoti) " + std::to_string(complete);
    }
    return out;
}

void PlayerbotRotationMgr::Update(uint32 diff)
{
    _timer += diff;
    if (_timer < 2000)
        return;
    _timer = 0;

    std::vector<ObjectGuid> gone;
    std::vector<ObjectGuid> banned;
    {
        std::shared_lock<std::shared_mutex> g(_lock);
        for (auto const& kv : _states)
        {
            Player* p = ObjectAccessor::FindPlayer(kv.first);
            if (!p || !p->IsInWorld() || !GET_PLAYERBOT_AI(p))
                gone.push_back(kv.first);
            else if (!kv.second.autopilot && (p->InBattleground() || p->InArena()))
                banned.push_back(kv.first);
            else if (kv.second.autopilot && p->InArena())
                banned.push_back(kv.first);
        }
    }

    for (ObjectGuid const& guid : banned)
    {
        if (Player* p = ObjectAccessor::FindPlayer(guid))
        {
            std::string reply;
            Disable(p, reply);
            ChatHandler(p->GetSession()).SendSysMessage("Mūšio laukuose ir arenose ši funkcija negalima.");
            ChatHandler(p->GetSession()).SendSysMessage(reply.c_str());
        }
    }

    if (!gone.empty())
    {
        std::unique_lock<std::shared_mutex> g(_lock);
        for (ObjectGuid const& guid : gone)
        {
            auto it = _states.find(guid);
            if (it == _states.end())
                continue;
            if (it->second.autopilot)
            {
                _autopilotCount.fetch_sub(1, std::memory_order_relaxed);
                // AI dingo (pvz. buvo perkurtas) – grazinam kliento valdyma, kad veikejas neliktu uzrakintas.
                if (Player* p = ObjectAccessor::FindPlayer(guid))
                    if (p->GetSession())
                        p->SetClientControl(p, true);
            }
            _states.erase(it);
        }
    }
}

bool NovaRotationBlocksMovement(PlayerbotAI* botAI)
{
    if (!botAI)
        return false;

    Player* bot = botAI->GetBot();
    return bot && botAI->IsRealPlayer() && sPlayerbotRotationMgr->IsStationary(bot->GetGUID());
}

bool NovaIsAutopilot(Player* bot)
{
    return bot && sPlayerbotRotationMgr->IsAutopilot(bot->GetGUID());
}

void NovaCollectTemporarySpells(Player* bot, std::unordered_set<uint32>& out)
{
    if (!bot)
        return;

    for (auto const& kv : bot->GetSpellMap())
        if (kv.second && kv.second->State == PLAYERSPELL_TEMPORARY)
            out.insert(kv.first);
}

void NovaPersistNewTemporarySpells(Player* bot, std::unordered_set<uint32> const& before)
{
    if (!bot)
        return;

    uint32 n = 0;
    for (auto& kv : bot->GetSpellMap())
    {
        if (kv.second && kv.second->State == PLAYERSPELL_TEMPORARY && before.find(kv.first) == before.end())
        {
            kv.second->State = PLAYERSPELL_NEW;     // bus issaugotas kartu su veikeju
            ++n;
        }
    }

    if (n)
        LOG_INFO("playerbots", "NovaCore autopilotas {}: issaugota {} naujai isimokytu burtu", bot->GetName(), n);
}

// ---------------------------------------------------------------- scenarijai

class PlayerbotRotationWorldScript : public WorldScript
{
public:
    PlayerbotRotationWorldScript() : WorldScript("PlayerbotRotationWorldScript", { WORLDHOOK_ON_UPDATE }) { }

    void OnUpdate(uint32 diff) override { sPlayerbotRotationMgr->Update(diff); }
};

class PlayerbotRotationCommandScript : public CommandScript
{
public:
    PlayerbotRotationCommandScript() : CommandScript("PlayerbotRotationCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable rotTable =
        {
            { "",     HandleToggle, SEC_PLAYER, Console::No },
            { "info", HandleInfo,   SEC_PLAYER, Console::No },
            { "on",   HandleOn,     SEC_PLAYER, Console::No },
            { "off",  HandleOff,    SEC_PLAYER, Console::No }
        };

        static ChatCommandTable apTable =
        {
            { "",     HandleApToggle, SEC_PLAYER, Console::No },
            { "info", HandleApInfo,   SEC_PLAYER, Console::No },
            { "on",   HandleApOn,     SEC_PLAYER, Console::No },
            { "off",  HandleOff,      SEC_PLAYER, Console::No }
        };

        static ChatCommandTable rootTable =
        {
            { "rotacija",    rotTable },
            { "autopilotas", apTable }
        };

        return rootTable;
    }

    static Player* Me(ChatHandler* handler)
    {
        return handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
    }

    static bool HandleToggle(ChatHandler* handler)
    {
        Player* player = Me(handler);
        if (!player)
            return false;

        std::string reply;
        if (sPlayerbotRotationMgr->IsEnabled(player->GetGUID()))
            sPlayerbotRotationMgr->Disable(player, reply);
        else
            sPlayerbotRotationMgr->Enable(player, false, reply);

        handler->SendSysMessage(reply.c_str());
        return true;
    }

    static bool HandleOn(ChatHandler* handler, Optional<std::string> mode)
    {
        Player* player = Me(handler);
        if (!player)
            return false;

        // Be argumento – rotacija be judėjimo; „judeti“ / „judėti“ – su judėjimu (pilnas autopilotas kovoje).
        bool allowMove = mode && (*mode == "judeti" || *mode == "judėti" || *mode == "move");

        std::string reply;
        sPlayerbotRotationMgr->Enable(player, allowMove, reply);
        handler->SendSysMessage(reply.c_str());
        return true;
    }

    static bool HandleOff(ChatHandler* handler)
    {
        Player* player = Me(handler);
        if (!player)
            return false;

        std::string reply;
        sPlayerbotRotationMgr->Disable(player, reply);
        handler->SendSysMessage(reply.c_str());
        return true;
    }

    static bool HandleApToggle(ChatHandler* handler)
    {
        Player* player = Me(handler);
        if (!player)
            return false;

        std::string reply;
        if (sPlayerbotRotationMgr->IsAutopilot(player->GetGUID()))
            sPlayerbotRotationMgr->Disable(player, reply);
        else
            sPlayerbotRotationMgr->EnableAutopilot(player, reply);

        handler->SendSysMessage(reply.c_str());
        return true;
    }

    static bool HandleApOn(ChatHandler* handler)
    {
        Player* player = Me(handler);
        if (!player)
            return false;

        std::string reply;
        sPlayerbotRotationMgr->EnableAutopilot(player, reply);
        handler->SendSysMessage(reply.c_str());
        return true;
    }

    static bool HandleApInfo(ChatHandler* handler)
    {
        Player* player = Me(handler);
        if (!player)
            return false;

        bool on = sPlayerbotRotationMgr->IsAutopilot(player->GetGUID());
        handler->SendSysMessage(on ? TXT_AP_ON : TXT_AP_OFF);
        if (on)
            handler->SendSysMessage(sPlayerbotRotationMgr->Describe(player).c_str());
        handler->SendSysMessage("Autopilotas veikėją valdo kaip botą: kelia lygį, atlieka užduotis, renka medžiagas, parduoda daiktus,");
        handler->SendSysMessage("deda į aukcioną ir perka pagerinimus, jungiasi į mūšio laukus ir pozemius. |cffffff00.autopilotas|r – įjungti / išjungti.");
        handler->SendSysMessage("Kol autopilotas veikia, veikėjo judinti negali (valdymas atimtas) – išjunk komanda |cffffff00.autopilotas off|r.");
        return true;
    }

    static bool HandleInfo(ChatHandler* handler)
    {
        Player* player = Me(handler);
        if (!player)
            return false;

        bool on = sPlayerbotRotationMgr->IsEnabled(player->GetGUID()) && !sPlayerbotRotationMgr->IsAutopilot(player->GetGUID());
        handler->SendSysMessage(on ? (sPlayerbotRotationMgr->IsStationary(player->GetGUID()) ? TXT_ON : TXT_ON_MOVE) : TXT_OFF);
        if (on)
            handler->SendSysMessage(sPlayerbotRotationMgr->Describe(player).c_str());
        handler->SendSysMessage("Kovos rotaciją (gebėjimų seką pagal klasę ir specializaciją) atlieka serveris už tave.");
        handler->SendSysMessage("Kovą pradėk pats (automatiniu smūgiu ar pirmuoju burtu), tikslą ir vietą pasirenki pats.");
        handler->SendSysMessage("|cffffff00.rotacija|r – įjungti / išjungti; |cffffff00.rotacija on judeti|r – kovoje veikėjas judės pats.");
        handler->SendSysMessage("Pakeitus talentus rotaciją išjunk ir vėl įjunk. Mūšio laukuose ir arenose ji negalima.");
        return true;
    }
};

void AddPlayerbotRotationScripts()
{
    new PlayerbotRotationWorldScript();
    new PlayerbotRotationCommandScript();
}
