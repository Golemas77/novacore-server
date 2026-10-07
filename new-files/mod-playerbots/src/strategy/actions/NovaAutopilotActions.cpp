/*
 * NovaCore: zaidejo autopilotas – patikimi uzduociu / pardavimo veiksmai (zr. NovaAutopilotActions.h).
 */

#include "NovaAutopilotActions.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Event.h"
#include "GameObject.h"
#include "Log.h"
#include "LootMgr.h"
#include "LootObjectStack.h"
#include "SpellMgr.h"
#include "DBCStores.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "PlayerbotRotation.h"
#include "Timer.h"
#include "WorldSession.h"

namespace
{
    std::mutex g_lock;

    // ------------------------------------------------------------ atidavimo vietos (is spawn duomenu, kesuojama)
    struct Pos
    {
        uint16 map;
        float x, y, z;
    };

    std::unordered_map<uint32, std::vector<Pos>> g_enders;          // questId -> vietos
    std::unordered_map<uint32, std::vector<Pos>> g_vendors;         // map * 2 + team -> vietos

    std::vector<Pos> const& EnderPositions(uint32 questId)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_enders.find(questId);
        if (it != g_enders.end())
            return it->second;

        std::vector<Pos>& v = g_enders[questId];
        std::unordered_set<uint32> creatures, objects;
        for (auto const& kv : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
            if (kv.second == questId)
                creatures.insert(kv.first);
        for (auto const& kv : *sObjectMgr->GetGOQuestInvolvedRelationMap())
            if (kv.second == questId)
                objects.insert(kv.first);

        if (!creatures.empty())
            for (auto const& kv : sObjectMgr->GetAllCreatureData())
            {
                CreatureData const& d = kv.second;
                if (creatures.count(d.id1) || creatures.count(d.id2) || creatures.count(d.id3))
                    v.push_back({ d.mapid, d.posX, d.posY, d.posZ });
            }

        if (!objects.empty())
            for (auto const& kv : sObjectMgr->GetAllGOData())
                if (objects.count(kv.second.id))
                    v.push_back({ kv.second.mapid, kv.second.posX, kv.second.posY, kv.second.posZ });

        return v;
    }

    bool NearestEnder(Player* bot, uint32 questId, WorldPosition& out)
    {
        float best = FLT_MAX;
        bool found = false;
        for (Pos const& p : EnderPositions(questId))
        {
            if (p.map != bot->GetMapId())
                continue;
            float const d = bot->GetDistance2d(p.x, p.y);
            if (d < best)
            {
                best = d;
                out = WorldPosition(p.map, p.x, p.y, p.z);
                found = true;
            }
        }
        return found;
    }

    std::vector<Pos> const& VendorPositions(Player* bot)
    {
        uint32 const key = uint32(bot->GetMapId()) * 2 + bot->GetTeamId();
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_vendors.find(key);
        if (it != g_vendors.end())
            return it->second;

        std::vector<Pos>& v = g_vendors[key];
        FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
        std::unordered_map<uint32, bool> templateOk;       // entry -> tinka
        for (auto const& kv : sObjectMgr->GetAllCreatureData())
        {
            CreatureData const& d = kv.second;
            if (d.mapid != bot->GetMapId())
                continue;

            auto tit = templateOk.find(d.id1);
            if (tit == templateOk.end())
            {
                bool ok = false;
                if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(d.id1))
                {
                    if ((ct->npcflag & UNIT_NPC_FLAG_VENDOR) && !(ct->npcflag & (UNIT_NPC_FLAG_SPIRITHEALER | UNIT_NPC_FLAG_SPIRITGUIDE)))
                    {
                        FactionTemplateEntry const* f = sFactionTemplateStore.LookupEntry(ct->faction);
                        ok = botFaction && f && f->IsFriendlyTo(*botFaction);
                    }
                }
                tit = templateOk.emplace(d.id1, ok).first;
            }

            if (tit->second)
                v.push_back({ d.mapid, d.posX, d.posY, d.posZ });
        }
        return v;
    }

    bool NearestVendor(Player* bot, WorldPosition& out)
    {
        float best = FLT_MAX;
        bool found = false;
        for (Pos const& p : VendorPositions(bot))
        {
            float const d = bot->GetDistance2d(p.x, p.y);
            if (d < best)
            {
                best = d;
                out = WorldPosition(p.map, p.x, p.y, p.z);
                found = true;
            }
        }
        return found;
    }

    // ------------------------------------------------------------ busenos sekimas (kad neužstrigtu amžinai)
    struct TurnInProgress
    {
        uint32 questId = 0;
        uint32 since = 0;
        float bestDist = FLT_MAX;
        uint32 lastImprove = 0;
    };

    std::unordered_map<ObjectGuid, TurnInProgress> g_turnIn;
    std::unordered_map<ObjectGuid, uint32> g_vendorCooldown;        // iki kada (getMSTime) nesiusti i pardavejus
    std::unordered_map<ObjectGuid, uint32> g_vendorAttempts;

    // objektai (knygos, skrynios, sventoves), su kuriais per daug kartu bandyta be rezultato – kuri laika praleidziami
    struct GoTries
    {
        uint32 count = 0;
        uint32 first = 0;
        uint32 blockedUntil = 0;
    };
    std::unordered_map<uint64, GoTries> g_goTries;

    bool GoBlocked(GameObject* go)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_goTries.find(go->GetGUID().GetRawValue());
        return it != g_goTries.end() && it->second.blockedUntil && getMSTime() < it->second.blockedUntil;
    }

    // Uzfiksuoja bandyma; po 6 bandymu per 3 min. objektas 5 min. praleidziamas. Grazina bandymo nr. (1..6).
    uint32 NoteGoAttempt(Player* bot, GameObject* go)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        GoTries& g = g_goTries[go->GetGUID().GetRawValue()];
        uint32 const now = getMSTime();
        if (!g.first || now - g.first > 180000)
        {
            g.first = now;
            g.count = 0;
        }
        uint32 const n = ++g.count;
        if (n >= 6)
        {
            g.blockedUntil = now + 5 * 60 * 1000;
            g.count = 0;
            g.first = 0;
            LOG_INFO("playerbots", "Autopilotas {}: objektas {} nepasiduoda po 6 bandymu, praleidziam 5 min.", bot->GetName(),
                     go->GetEntry());
        }
        return n;
    }

    // „Opening“ burtas pagal spynos tipa (zaidejas jį naudoja spaudziant ant uzrakinto objekto): 3365 = LOCKTYPE_OPEN.
    uint32 OpeningSpellFor(GameObject* go)
    {
        if (LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId()))
            for (uint8 j = 0; j < 8; ++j)
                if (lock->Type[j] == LOCK_KEY_SKILL && lock->Index[j] == LOCKTYPE_OPEN)
                    return 3365;
        return 3365;
    }
}

bool NovaAutopilotFree(Player* bot, PlayerbotAI* botAI)
{
    if (!bot || !botAI || !NovaIsAutopilot(bot) || !bot->IsAlive() || !bot->IsInWorld())
        return false;

    if (bot->IsInCombat() || bot->IsInFlight() || bot->IsBeingTeleported() || bot->InBattleground() || bot->InArena())
        return false;

    Map* map = bot->GetMap();
    if (!map || map->Instanceable())
        return false;

    return botAI->GetState() != BOT_STATE_COMBAT;
}

bool NovaFindCompletedQuest(Player* bot, PlayerbotAI* botAI, uint32& questId, WorldPosition& pos)
{
    float best = FLT_MAX;
    bool found = false;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const q = bot->GetQuestSlotQuestId(slot);
        if (!q || bot->GetQuestStatus(q) != QUEST_STATUS_COMPLETE)
            continue;
        if (botAI->lowPriorityQuest.find(q) != botAI->lowPriorityQuest.end())
            continue;

        WorldPosition p;
        if (!NearestEnder(bot, q, p))
            continue;

        float const d = bot->GetDistance2d(p.GetPositionX(), p.GetPositionY());
        if (d < best)
        {
            best = d;
            questId = q;
            pos = p;
            found = true;
        }
    }
    return found;
}

bool NovaHasObjectQuests(Player* bot, PlayerbotAI* botAI)
{
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const q = bot->GetQuestSlotQuestId(slot);
        if (!q || bot->GetQuestStatus(q) != QUEST_STATUS_INCOMPLETE)
            continue;
        if (botAI->lowPriorityQuest.find(q) != botAI->lowPriorityQuest.end())
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(q);
        if (!quest)
            continue;

        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i] < 0)
                return true;
        for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i])
                return true;
    }
    return false;
}

// Laisvu kuprines vietu <= 25 % visos talpos (ne maziau kaip 3).
bool NovaLowBagSpace(Player* bot)
{
    uint32 totalSlots = 16;
    for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        if (Bag* pBag = bot->GetBagByPos(b))
            totalSlots += pBag->GetBagSize();
    return bot->GetFreeInventorySpace() <= std::max<uint32>(3u, totalSlots / 4);
}

bool NovaNeedsVendor(Player* bot)
{
    // NovaCore: i pardavejus vykstam anksciau nei botu bendras „kuprine >80 % pilna“ slenkstis (kitaip lootas jau praleistu daiktus):
    // kai laisvu vietu <= 25 % visos talpos (ne maziau kaip 3).
    if (!NovaLowBagSpace(bot))
        return false;

    std::lock_guard<std::mutex> guard(g_lock);
    auto it = g_vendorCooldown.find(bot->GetGUID());
    return it == g_vendorCooldown.end() || getMSTime() >= it->second;
}

// ---------------------------------------------------------------- atidavimas

bool NovaTurnInAction::Execute(Event /*event*/)
{
    uint32 questId = 0;
    WorldPosition pos;
    if (!NovaFindCompletedQuest(bot, botAI, questId, pos))
        return false;

    float const dist = bot->GetDistance2d(pos.GetPositionX(), pos.GetPositionY());
    uint32 const now = getMSTime();

    {
        std::lock_guard<std::mutex> guard(g_lock);
        TurnInProgress& pr = g_turnIn[bot->GetGUID()];
        if (pr.questId != questId)
        {
            pr = TurnInProgress();
            pr.questId = questId;
            pr.since = now;
            pr.lastImprove = now;
            LOG_INFO("playerbots", "Autopilotas {}: eina atiduoti uzduoti {} pas ({:.0f}, {:.0f}), atstumas {:.0f}", bot->GetName(),
                     questId, pos.GetPositionX(), pos.GetPositionY(), dist);
        }

        if (dist + 3.0f < pr.bestDist)
        {
            pr.bestDist = dist;
            pr.lastImprove = now;
        }
        else if (now - pr.lastImprove > 120000)
        {
            // 2 min be pazangos – neimanoma pasiekti / atiduoti, atsisakom sios uzduoties
            botAI->lowPriorityQuest.insert(questId);
            g_turnIn.erase(bot->GetGUID());
            LOG_INFO("playerbots", "Autopilotas {}: nepavyko atiduoti uzduoties {} (be pazangos 2 min.), praleidziam", bot->GetName(),
                     questId);
            return false;
        }
    }

    if (dist > 6.0f)
        return MoveFarTo(pos) || true;

    // salia: pokalbis su NPC (atlygis ir naujos uzduotys)
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return true;
}

// ---------------------------------------------------------------- uzduociu objektai

bool NovaQuestObjectsAction::UseOrApproach(GameObject* go)
{
    if (go->IsWithinDistInMap(bot, INTERACTION_DISTANCE - 1.0f))
    {
        WorldPacket p(CMSG_GAMEOBJ_USE);
        p << go->GetGUID();
        bot->GetSession()->HandleGameObjectUseOpcode(p);
        ForceToWait(4000);
        NoteGoAttempt(bot, go);
        LOG_INFO("playerbots", "Autopilotas {}: naudoja objekta {} uzduociai", bot->GetName(), go->GetEntry());
        return true;
    }

    MoveWorldObjectTo(go->GetGUID(), 2.0f);
    return true;
}

bool NovaQuestObjectsAction::WorkQuest(uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    auto const& statusMap = bot->getQuestStatusMap();
    auto sit = statusMap.find(questId);
    if (!quest || sit == statusMap.end())
        return false;

    QuestStatusData const& qs = sit->second;

    // 1) tikslai „naudoti objekta“
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 const req = quest->RequiredNpcOrGo[i];
        if (req >= 0 || qs.CreatureOrGOCount[i] >= quest->RequiredNpcOrGoCount[i])
            continue;

        if (GameObject* go = bot->FindNearestGameObject(uint32(-req), 150.0f, true))
        {
            if (GoBlocked(go))
                continue;
            return UseOrApproach(go);
        }
    }

    // 2) uzduoties daiktai is objektu su grobiu (knygos, skrynios)
    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        if (!quest->RequiredItemId[i] || qs.ItemCount[i] >= quest->RequiredItemCount[i])
            continue;

        GuidVector objects = AI_VALUE(GuidVector, "nearest game objects no los");
        GameObject* best = nullptr;
        float bestDist = FLT_MAX;
        for (ObjectGuid const& guid : objects)
        {
            GameObject* go = ObjectAccessor::GetGameObject(*bot, guid);
            if (!go || !go->isSpawned() || !go->GetGOInfo() || GoBlocked(go))
                continue;

            uint32 const lootId = go->GetGOInfo()->GetLootId();
            if (!lootId || !LootTemplates_Gameobject.HaveQuestLootForPlayer(lootId, bot))
                continue;

            float const d = bot->GetDistance(go);
            if (d < bestDist)
            {
                bestDist = d;
                best = go;
            }
        }

        if (!best)
            continue;

        if (bestDist < INTERACTION_DISTANCE - 0.5f)
        {
            // atidarom skrynia / knyga pats (CMSG_GAMEOBJ_USE); gautas SMSG_LOOT_RESPONSE iskart suvaldomas „store loot“
            // 1, 3, 5 bandymas – „Opening“ burtu (taip atidaro zaidejas uzrakinta objekta), 2, 4, 6 – CMSG_GAMEOBJ_USE
            uint32 const attempt = NoteGoAttempt(bot, best);
            if (bot->IsMounted())
                bot->Dismount();
            bot->StopMoving();
            if (attempt % 2 == 1)
                bot->CastSpell(best, OpeningSpellFor(best), true);
            else
            {
                WorldPacket p(CMSG_GAMEOBJ_USE);
                p << best->GetGUID();
                bot->GetSession()->HandleGameObjectUseOpcode(p);
            }
            ForceToWait(3000);
            LOG_INFO("playerbots", "Autopilotas {}: atidaro objekta {} ({}. bandymas, uzduotis {}), daiktu {}/{}", bot->GetName(),
                     best->GetEntry(), attempt, questId, qs.ItemCount[i], quest->RequiredItemCount[i]);
            return true;
        }

        MoveWorldObjectTo(best->GetGUID(), 2.0f);
        return true;
    }

    return false;
}

bool NovaQuestObjectsAction::Execute(Event /*event*/)
{
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const q = bot->GetQuestSlotQuestId(slot);
        if (!q || bot->GetQuestStatus(q) != QUEST_STATUS_INCOMPLETE)
            continue;
        if (botAI->lowPriorityQuest.find(q) != botAI->lowPriorityQuest.end())
            continue;

        if (WorkQuest(q))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- pardavimas

bool NovaVendorTripAction::Execute(Event /*event*/)
{
    // salia esantis draugiskas pardavejas
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    Creature* closeVendor = nullptr;
    Creature* visible = nullptr;
    for (ObjectGuid const& guid : npcs)
    {
        Creature* c = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
        if (!c || !c->IsAlive() || !c->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) || c->IsHostileTo(bot))
            continue;

        if (c->IsWithinDistInMap(bot, INTERACTION_DISTANCE - 0.5f))
        {
            closeVendor = c;
            break;
        }
        if (!visible || bot->GetDistance(c) < bot->GetDistance(visible))
            visible = c;
    }

    if (closeVendor)
    {
        uint32 const before = bot->GetFreeInventorySpace();
        botAI->DoSpecificAction("sell", Event("nova vendor", "gray"), true);
        if (NovaLowBagSpace(bot))
            botAI->DoSpecificAction("sell", Event("nova vendor", "vendor"), true);

        uint32 const after = bot->GetFreeInventorySpace();
        LOG_INFO("playerbots", "Autopilotas {}: pardavejui parduota, laisvu vietu {} -> {}", bot->GetName(), before, after);

        std::lock_guard<std::mutex> guard(g_lock);
        if (after > before)
            g_vendorAttempts.erase(bot->GetGUID());
        else if (++g_vendorAttempts[bot->GetGUID()] >= 3)
        {
            // nepavyksta nieko parduoti – 10 min. neviliojam botu pas pardavejus (kad neužstrigtu)
            g_vendorCooldown[bot->GetGUID()] = getMSTime() + 10 * 60 * 1000;
            g_vendorAttempts.erase(bot->GetGUID());
            LOG_INFO("playerbots", "Autopilotas {}: pardavimas nepavyko 3 kartus, pauze 10 min.", bot->GetName());
        }
        return true;
    }

    if (visible)
    {
        MoveWorldObjectTo(visible->GetGUID(), 2.0f);
        return true;
    }

    WorldPosition pos;
    if (!NearestVendor(bot, pos))
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_vendorCooldown[bot->GetGUID()] = getMSTime() + 10 * 60 * 1000;
        LOG_INFO("playerbots", "Autopilotas {}: pilna kuprine, bet siame zemelapyje nerastas pardavejas", bot->GetName());
        return false;
    }

    return MoveFarTo(pos) || true;
}

// ---------------------------------------------------------------- uzduociu mobai (pagal tikras spawn vietas)

namespace
{
    struct Spawn
    {
        uint32 id;      // spawn id (CreatureData raktas)
        uint32 entry;   // mobo entry
        uint16 map;
        float x, y, z;
    };

    std::unordered_map<uint32, std::vector<uint32>> g_itemDroppers;                       // daiktas -> mobu entry
    std::unordered_map<uint32, std::vector<Spawn>> g_spawns;                              // mobo entry -> spawn vietos
    std::unordered_map<ObjectGuid, std::unordered_map<uint32, uint32>> g_visitedSpawns;   // botas -> spawn id -> iki kada
    struct MobTrip
    {
        uint32 spawnId = 0;
        float bestDist = FLT_MAX;
        uint32 lastImprove = 0;
    };
    std::unordered_map<ObjectGuid, MobTrip> g_mobTrip;
    struct GiverState
    {
        uint32 since = 0;
        uint32 blockedUntil = 0;
    };
    std::unordered_map<ObjectGuid, GiverState> g_giver;

    // Mobai, metantys daikta (tiesiogiai creature_loot_template). Skaiciuojama vienkart, kesuojama.
    std::vector<uint32> const& ItemDroppers(uint32 itemId)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_itemDroppers.find(itemId);
            if (it != g_itemDroppers.end())
                return it->second;
        }

        std::vector<uint32> found;
        if (QueryResult res = WorldDatabase.Query(
                "SELECT ct.entry FROM creature_template ct INNER JOIN creature_loot_template l ON l.Entry = ct.lootid "
                "WHERE l.Item = {}", itemId))
        {
            do
            {
                found.push_back((*res)[0].Get<uint32>());
            } while (res->NextRow());
        }

        std::lock_guard<std::mutex> guard(g_lock);
        return g_itemDroppers.emplace(itemId, std::move(found)).first->second;
    }

    std::vector<Spawn> const& SpawnsOf(uint32 entry)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_spawns.find(entry);
            if (it != g_spawns.end())
                return it->second;
        }

        std::vector<Spawn> found;
        for (auto const& kv : sObjectMgr->GetAllCreatureData())
        {
            CreatureData const& d = kv.second;
            if (d.id1 == entry || d.id2 == entry || d.id3 == entry)
                found.push_back({ uint32(kv.first), entry, d.mapid, d.posX, d.posY, d.posZ });
        }

        std::lock_guard<std::mutex> guard(g_lock);
        return g_spawns.emplace(entry, std::move(found)).first->second;
    }

    // Mobu entry, kuriu reikia nebaigtoms uzduotims (nukauti X / daiktas, krentantis is moku).
    void QuestKillEntries(Player* bot, PlayerbotAI* botAI, std::unordered_set<uint32>& out)
    {
        auto const& statusMap = bot->getQuestStatusMap();
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const q = bot->GetQuestSlotQuestId(slot);
            if (!q || bot->GetQuestStatus(q) != QUEST_STATUS_INCOMPLETE)
                continue;
            if (botAI->lowPriorityQuest.find(q) != botAI->lowPriorityQuest.end())
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(q);
            auto sit = statusMap.find(q);
            if (!quest || sit == statusMap.end())
                continue;
            if (quest->GetQuestLevel() > int32(bot->GetLevel()) + 5)
                continue;

            for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                int32 const entry = quest->RequiredNpcOrGo[i];
                if (entry > 0 && sit->second.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
                    out.insert(uint32(entry));
            }

            for (int j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
            {
                uint32 const item = quest->RequiredItemId[j];
                if (item && sit->second.ItemCount[j] < quest->RequiredItemCount[j])
                    for (uint32 e : ItemDroppers(item))
                        out.insert(e);
            }
        }
    }
}

bool NovaHasKillQuests(Player* bot, PlayerbotAI* botAI)
{
    std::unordered_set<uint32> entries;
    QuestKillEntries(bot, botAI, entries);
    return !entries.empty();
}

bool NovaQuestMobsAction::Execute(Event /*event*/)
{
    uint32 const now = getMSTime();

    // 1) pakeliui: priimam / atiduodam uzduotis pas netoliese esancius NPC (kaip daro DO_QUEST). Apsauga nuo amzino ciklo.
    bool giverBlocked;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        giverBlocked = now < g_giver[bot->GetGUID()].blockedUntil;
    }
    if (!giverBlocked)
    {
        bool const busy = SearchQuestGiverAndAcceptOrReward();
        std::lock_guard<std::mutex> guard(g_lock);
        GiverState& gs = g_giver[bot->GetGUID()];
        if (!busy)
            gs.since = 0;
        else
        {
            if (!gs.since)
                gs.since = now;
            if (now - gs.since > 60000)
            {
                gs.blockedUntil = now + 5 * 60 * 1000;
                gs.since = 0;
                LOG_INFO("playerbots", "Autopilotas {}: NPC uzduociu paieska užsitesė, pauze 5 min.", bot->GetName());
            }
        }
        if (busy)
            return true;
    }

    // 2) kokiu moku reikia
    std::unordered_set<uint32> entries;
    QuestKillEntries(bot, botAI, entries);
    if (entries.empty())
        return false;

    bool const canElite = AI_VALUE(bool, "can fight elite");
    std::vector<std::vector<Spawn> const*> lists;
    for (uint32 entry : entries)
    {
        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(entry);
        if (!ct || (ct->rank > CREATURE_ELITE_NORMAL && !canElite) || int32(ct->maxlevel) > int32(bot->GetLevel()) + 4)
            continue;
        lists.push_back(&SpawnsOf(entry));
    }

    // 3) artimiausia dar nelankyta spawn vieta
    Spawn best{};
    float bestDist = FLT_MAX;
    bool found = false;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto& visited = g_visitedSpawns[bot->GetGUID()];
        for (auto const* list : lists)
            for (Spawn const& s : *list)
            {
                if (s.map != bot->GetMapId())
                    continue;
                auto vit = visited.find(s.id);
                if (vit != visited.end() && now < vit->second)
                    continue;

                float const d = bot->GetDistance2d(s.x, s.y);
                if (d < bestDist && d < 700.0f)
                {
                    bestDist = d;
                    best = s;
                    found = true;
                }
            }
    }

    if (!found)
        return false;   // visos vietos neseniai lankytos – leidziam kitiems veiksmams (klajojimas, nauju uzduociu paieska)

    // 4) atvykom ir moku nematyti (trigeris aktyvus tik kai „grind target“ tuscias) – laikom vieta aplankyta
    if (bestDist < 20.0f)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto& visited = g_visitedSpawns[bot->GetGUID()];
        for (auto const* list : lists)
            for (Spawn const& s : *list)
                if (s.map == bot->GetMapId() && bot->GetDistance2d(s.x, s.y) < 45.0f)
                    visited[s.id] = now + 90 * 1000;
        g_mobTrip.erase(bot->GetGUID());
        return true;
    }

    // 5) pazanga: jei 25 s neartejam (nepasiekiama) – vieta praleidziama 5 min.
    {
        std::lock_guard<std::mutex> guard(g_lock);
        MobTrip& trip = g_mobTrip[bot->GetGUID()];
        if (trip.spawnId != best.id)
        {
            trip = MobTrip();
            trip.spawnId = best.id;
            trip.lastImprove = now;
            if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(best.entry))
                LOG_INFO("playerbots", "Autopilotas {}: eina i uzduociu moku {} (id {}) vieta ({:.0f}, {:.0f}), atstumas {:.0f}",
                         bot->GetName(), ct->Name, best.entry, best.x, best.y, bestDist);
        }

        if (bestDist + 3.0f < trip.bestDist)
        {
            trip.bestDist = bestDist;
            trip.lastImprove = now;
        }
        else if (now - trip.lastImprove > 25000)
        {
            g_visitedSpawns[bot->GetGUID()][best.id] = now + 5 * 60 * 1000;
            g_mobTrip.erase(bot->GetGUID());
            LOG_INFO("playerbots", "Autopilotas {}: nepavyksta pasiekti mobu vietos ({:.0f}, {:.0f}), praleidziam 5 min.",
                     bot->GetName(), best.x, best.y);
            return true;
        }
    }

    MoveFarTo(WorldPosition(best.map, best.x, best.y, best.z));
    return true;
}
