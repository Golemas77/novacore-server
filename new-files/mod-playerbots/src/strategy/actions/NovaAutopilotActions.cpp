#define _ALLOW_KEYWORD_MACROS 1
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
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "RandomItemMgr.h"
#include "PlayerbotRotation.h"
#include "Timer.h"
#include "WorldSession.h"

// SA kovos lauko busenos laukai (Attackers, Status, GateStatus) yra private; skaitom juos tik cia.
#define private public
#include "BattlegroundSA.h"
#undef private

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

    // „Opening“ burtas pagal spynos tipa (zaidejas jį naudoja spaudziant ant uzrakinto objekto). Burto EffectMiscValue turi
    // sutapti su spynos Index (Lock.dbc): 5/6 = 3365, 10 = 6247, 12 = 6477, 13 (klupint, pvz. „Sugadinta gele“) = 6478,
    // 17 = 21651; 2 = zoleliavimas (2366), 3 = kasyba (2575) – tik jei botas turi tuos gebejimus. 0 = spynos nera (zr. UseObject).
    uint32 OpeningSpellFor(Player* bot, GameObject* go)
    {
        LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
        if (!lock)
            return 0;

        uint32 skillSpell = 0;
        for (uint8 j = 0; j < 8; ++j)
        {
            if (lock->Type[j] != LOCK_KEY_SKILL)
                continue;

            switch (lock->Index[j])
            {
                case 5:
                case 6:
                    return 3365;
                case 10:
                    return 6247;
                case 12:
                    return 6477;
                case 13:
                    return 6478;
                case 17:
                    return 21651;
                case 2:
                    if (!skillSpell && bot->HasSpell(2366))
                        skillSpell = 2366;
                    break;
                case 3:
                    if (!skillSpell && bot->HasSpell(2575))
                        skillSpell = 2575;
                    break;
                default:
                    break;
            }
        }
        return skillSpell;
    }

    // Objektu grobio sablonai (gameobject_loot_template.Entry), kuriuose yra butent sis daiktas. Kesuojama.
    std::unordered_map<uint32, std::unordered_set<uint32>> g_goLootByItem;

    std::unordered_set<uint32> const& GoLootEntriesWithItem(uint32 itemId)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_goLootByItem.find(itemId);
            if (it != g_goLootByItem.end())
                return it->second;
        }

        std::unordered_set<uint32> found;
        if (QueryResult res = WorldDatabase.Query("SELECT DISTINCT Entry FROM gameobject_loot_template WHERE Item = {}", itemId))
        {
            do
            {
                found.insert((*res)[0].Get<uint32>());
            } while (res->NextRow());
        }

        std::lock_guard<std::mutex> guard(g_lock);
        return g_goLootByItem.emplace(itemId, std::move(found)).first->second;
    }

    // Objektu (skrinios, knygos, zoles) tikros spawn vietos: pagal daikta, kuri jie meta, arba pagal objekto entry. Kesuojama.
    struct GoSpawn
    {
        uint32 id;
        uint32 entry;
        uint16 map;
        float x, y, z;
    };

    std::unordered_map<uint32, std::vector<GoSpawn>> g_goSpawnsByItem;
    std::unordered_map<uint32, std::vector<GoSpawn>> g_goSpawnsByEntry;
    std::unordered_map<ObjectGuid, std::unordered_map<uint32, uint32>> g_goVisited;   // botas -> spawn id -> iki kada praleidziam

    struct GoTripState
    {
        uint32 spawnId = 0;
        float best = FLT_MAX;
        uint32 lastImprove = 0;
    };
    std::unordered_map<ObjectGuid, GoTripState> g_goTrip;

    std::vector<GoSpawn> const& GoSpawnsForEntries(std::unordered_set<uint32> const& entries,
                                                     std::unordered_map<uint32, std::vector<GoSpawn>>& cache, uint32 key)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = cache.find(key);
            if (it != cache.end())
                return it->second;
        }

        std::vector<GoSpawn> found;
        if (!entries.empty())
            for (auto const& kv : sObjectMgr->GetAllGOData())
                if (entries.count(kv.second.id))
                    found.push_back({ uint32(kv.first), kv.second.id, kv.second.mapid, kv.second.posX, kv.second.posY, kv.second.posZ });

        std::lock_guard<std::mutex> guard(g_lock);
        return cache.emplace(key, std::move(found)).first->second;
    }

    std::vector<GoSpawn> const& GoSpawnsForItem(uint32 itemId)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_goSpawnsByItem.find(itemId);
            if (it != g_goSpawnsByItem.end())
                return it->second;
        }

        std::unordered_set<uint32> entries;
        std::unordered_set<uint32> const& lootIds = GoLootEntriesWithItem(itemId);
        if (!lootIds.empty())
            for (auto const& kv : *sObjectMgr->GetGameObjectTemplates())
                if (uint32 const lootId = kv.second.GetLootId())
                    if (lootIds.count(lootId))
                        entries.insert(kv.first);

        return GoSpawnsForEntries(entries, g_goSpawnsByItem, itemId);
    }

    std::vector<GoSpawn> const& GoSpawnsForEntry(uint32 entry)
    {
        std::unordered_set<uint32> one{ entry };
        return GoSpawnsForEntries(one, g_goSpawnsByEntry, entry);
    }

    // Eina prie artimiausios dar nelankytos objektu spawn vietos (kai pats objektas nematomas). Grazina false, jei nera kur eiti.
    template <typename MoveFn>
    bool ApproachGoSpawns(Player* bot, std::vector<GoSpawn> const& spawns, MoveFn&& move)
    {
        uint32 const now = getMSTime();
        GoSpawn best{};
        float bestDist = FLT_MAX;
        bool found = false;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto& visited = g_goVisited[bot->GetGUID()];
            for (GoSpawn const& s : spawns)
            {
                if (s.map != bot->GetMapId())
                    continue;
                auto vit = visited.find(s.id);
                if (vit != visited.end() && now < vit->second)
                    continue;

                float const d = bot->GetDistance2d(s.x, s.y);
                if (d < bestDist && d < 1500.0f)
                {
                    bestDist = d;
                    best = s;
                    found = true;
                }
            }
        }

        if (!found)
            return false;

        std::lock_guard<std::mutex> guard(g_lock);
        auto& visited = g_goVisited[bot->GetGUID()];

        // atvykom (<20 yd) ir objekto nematome – jis dar neatsirado / paimtas: sia vieta (ir artimas) praleidziam 90 s
        if (bestDist < 20.0f)
        {
            for (GoSpawn const& s : spawns)
                if (s.map == bot->GetMapId() && bot->GetDistance2d(s.x, s.y) < 40.0f)
                    visited[s.id] = now + 90 * 1000;
            g_goTrip.erase(bot->GetGUID());
            return true;
        }

        // pazanga: 30 s be artejimo (nepasiekiama) – vieta praleidziama 5 min.
        GoTripState& trip = g_goTrip[bot->GetGUID()];
        if (trip.spawnId != best.id)
        {
            trip = GoTripState();
            trip.spawnId = best.id;
            trip.lastImprove = now;
            LOG_INFO("playerbots", "Autopilotas {}: eina i uzduociu objekto {} vieta ({:.0f}, {:.0f}), atstumas {:.0f}", bot->GetName(),
                     best.entry, best.x, best.y, bestDist);
        }

        if (bestDist + 3.0f < trip.best)
        {
            trip.best = bestDist;
            trip.lastImprove = now;
        }
        else if (now - trip.lastImprove > 30000)
        {
            visited[best.id] = now + 5 * 60 * 1000;
            g_goTrip.erase(bot->GetGUID());
            LOG_INFO("playerbots", "Autopilotas {}: nepavyksta pasiekti objekto vietos ({:.0f}, {:.0f}), praleidziam 5 min.", bot->GetName(),
                     best.x, best.y);
            return true;
        }

        move(WorldPosition(best.map, best.x, best.y, best.z));
        return true;
    }

    void CollectQuestUseItems(Player* bot, std::vector<Item*>& out)
    {
        // uzduociu pradzios daiktai (StartItem) dažnai yra klases 15 (ivairus), ne 12 – juos irgi laikom uzduociu daiktais
        std::unordered_set<uint32> srcItems;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (uint32 q = bot->GetQuestSlotQuestId(slot))
                if (Quest const* qt = sObjectMgr->GetQuestTemplate(q))
                    if (qt->GetSrcItemId())
                        srcItems.insert(qt->GetSrcItemId());

        auto consider = [&out, &srcItems](Item* it)
        {
            if (!it)
                return;
            ItemTemplate const* p = it->GetTemplate();
            if (!p || (p->Class != ITEM_CLASS_QUEST && srcItems.find(p->ItemId) == srcItems.end()))
                return;
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                if (p->Spells[i].SpellId > 0 && p->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                {
                    out.push_back(it);
                    return;
                }
        };

        for (uint8 s = INVENTORY_SLOT_ITEM_START; s < INVENTORY_SLOT_ITEM_END; ++s)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* pBag = bot->GetBagByPos(b))
                for (uint32 i = 0; i < pBag->GetBagSize(); ++i)
                    consider(pBag->GetItemByPos(i));
    }

    // Burto taikinio apribojimai is `conditions` (SourceType 13 / 17, ConditionType 31 = objekto entry): pvz. „Skiepijantis kristalas“
    // veikia tik ant Gūžtamiškių pelėdžmogio. Grazina (tipas: 3 = mobas, 5 = objektas; entry). Kesuojama.
    std::unordered_map<uint32, std::vector<std::pair<uint32, uint32>>> g_spellTargets;

    std::vector<std::pair<uint32, uint32>> const& SpellTargetEntries(uint32 spellId)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_spellTargets.find(spellId);
            if (it != g_spellTargets.end())
                return it->second;
        }

        std::vector<std::pair<uint32, uint32>> found;
        if (QueryResult res = WorldDatabase.Query(
                "SELECT ConditionValue1, ConditionValue2 FROM conditions WHERE SourceTypeOrReferenceId IN (13, 17) AND "
                "SourceEntry = {} AND ConditionTypeOrReference = 31 AND NegativeCondition = 0", spellId))
        {
            do
            {
                Field* f = res->Fetch();
                uint32 const type = f[0].Get<uint32>();
                uint32 const entry = f[1].Get<uint32>();
                if (entry && (type == 3 || type == 5))
                    found.push_back({ type, entry });
            } while (res->NextRow());
        }

        std::lock_guard<std::mutex> guard(g_lock);
        return g_spellTargets.emplace(spellId, std::move(found)).first->second;
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

namespace
{
    struct TurnInSession
    {
        bool active = false;
        uint32 progressSum = UINT32_MAX;
        uint32 since = 0;
    };
    std::unordered_map<ObjectGuid, TurnInSession> g_session;
}

bool NovaTurnInGateOpen(Player* bot, PlayerbotAI* botAI)
{
    // uzduociu zurnalo suvestine: pazanga (tikslu skaiciai), ar yra darbo, ar zurnalas pilnas
    uint32 sum = 0, logCount = 0;
    bool hasWork = false;
    auto const& statusMap = bot->getQuestStatusMap();
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const q = bot->GetQuestSlotQuestId(slot);
        if (!q)
            continue;
        ++logCount;

        QuestStatus const st = bot->GetQuestStatus(q);
        if (st == QUEST_STATUS_COMPLETE)
        {
            sum += 1000;
            continue;
        }
        if (st != QUEST_STATUS_INCOMPLETE)
            continue;

        if (botAI->lowPriorityQuest.find(q) == botAI->lowPriorityQuest.end())
            hasWork = true;

        auto sit = statusMap.find(q);
        if (sit != statusMap.end())
        {
            for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                sum += sit->second.CreatureOrGOCount[i];
            for (int j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
                sum += sit->second.ItemCount[j];
        }
    }

    uint32 pendingQuest = 0;
    WorldPosition pendingPos;
    bool const pending = NovaFindCompletedQuest(bot, botAI, pendingQuest, pendingPos);

    uint32 const now = getMSTime();
    std::lock_guard<std::mutex> guard(g_lock);
    TurnInSession& s = g_session[bot->GetGUID()];
    if (s.progressSum != sum)
    {
        s.progressSum = sum;
        s.since = now;
    }

    if (!pending)
    {
        s.active = false;
        return false;
    }

    if (s.active)
        return true;

    bool const nearEnder = bot->GetDistance2d(pendingPos.GetPositionX(), pendingPos.GetPositionY()) < 20.0f;
    if (!hasWork || nearEnder || logCount >= MAX_QUEST_LOG_SIZE - 2 || now - s.since > 240000)
    {
        s.active = true;
        LOG_INFO("playerbots", "Autopilotas {}: pradeda uzduociu atidavima (darbo liko: {}, salia atidavejo: {}, zurnale {} uzduociu)",
                 bot->GetName(), hasWork ? "taip" : "ne", nearEnder ? "taip" : "ne", logCount);
        return true;
    }

    return false;
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

        // objekto nematyti – einam i jo spawn vietas
        if (ApproachGoSpawns(bot, GoSpawnsForEntry(uint32(-req)), [this](WorldPosition const& p) { return MoveFarTo(p); }))
            return true;
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

            // objekto grobyje turi buti BUTENT sio tikslo daiktas (kitaip botas kala kito uzduoties objekta veltui)
            uint32 const lootId = go->GetGOInfo()->GetLootId();
            if (!lootId)
                continue;
            std::unordered_set<uint32> const& sources = GoLootEntriesWithItem(quest->RequiredItemId[i]);
            if (!sources.empty() ? !sources.count(lootId) : !LootTemplates_Gameobject.HaveQuestLootForPlayer(lootId, bot))
                continue;

            float const d = bot->GetDistance(go);
            if (d < bestDist)
            {
                bestDist = d;
                best = go;
            }
        }

        if (!best)
        {
            // objekto-saltinio nematyti – einam i tikras ju spawn vietas (ne i quest_poi)
            if (ApproachGoSpawns(bot, GoSpawnsForItem(quest->RequiredItemId[i]),
                                 [this](WorldPosition const& p) { return MoveFarTo(p); }))
                return true;
            continue;
        }

        if (bestDist < INTERACTION_DISTANCE - 0.5f)
        {
            // atidarom skrynia / knyga pats (CMSG_GAMEOBJ_USE); gautas SMSG_LOOT_RESPONSE iskart suvaldomas „store loot“
            // 1, 3, 5 bandymas – „Opening“ burtu (taip atidaro zaidejas uzrakinta objekta), 2, 4, 6 – CMSG_GAMEOBJ_USE
            uint32 const attempt = NoteGoAttempt(bot, best);
            if (bot->IsMounted())
                bot->Dismount();
            bot->StopMoving();
            // Budai keiciami kas bandyma: 0 = „Opening“ burtas pagal spynos tipa (jei yra), 1 = tiesioginis grobio atidarymas
            // (kaip po sekmingo burto), 2 = CMSG_GAMEOBJ_USE. Be burto – tik 1 ir 2.
            uint32 const openSpell = OpeningSpellFor(bot, best);
            uint32 method = (attempt - 1) % 3;
            if (!openSpell)
                method = 1 + (attempt - 1) % 2;

            uint32 castResult = 0;
            if (method == 0)
                castResult = uint32(bot->CastSpell(best, openSpell, true));
            else if (method == 1)
                bot->SendLoot(best->GetGUID(), LOOT_CORPSE);
            else
            {
                WorldPacket p(CMSG_GAMEOBJ_USE);
                p << best->GetGUID();
                bot->GetSession()->HandleGameObjectUseOpcode(p);
            }
            ForceToWait(3000);
            LOG_INFO("playerbots", "Autopilotas {}: atidaro objekta {} ({}. bandymas, budas {}, burtas {} -> {}, busena {}, vez. {:.1f}, uzduotis {}), daiktu {}/{}",
                     bot->GetName(), best->GetEntry(), attempt, method, openSpell, castResult, uint32(best->getLootState()), bestDist, questId,
                     qs.ItemCount[i], quest->RequiredItemCount[i]);
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

    // matomo uzduociu mobo vejimas (kai „grind“ jo neima) ir priverstinis taikinys
    struct ChaseState
    {
        ObjectGuid guid;
        float bestDist = FLT_MAX;
        uint32 lastImprove = 0;
        uint32 okSince = 0;
    };
    std::unordered_map<ObjectGuid, ChaseState> g_chase;
    std::unordered_map<ObjectGuid, std::unordered_map<uint64, uint32>> g_chaseBlock;
    struct ForcedTarget
    {
        ObjectGuid guid;
        uint32 until = 0;
    };
    std::unordered_map<ObjectGuid, ForcedTarget> g_forced;

    // Nepasiekiami taikiniai: jei 3 kartus nepavyko prieiti prie to paties mobo (navmesh neturi kelio, pvz. urvas), uzduotis paliekama
    // ramybeje (lowPriorityQuest), kad botas nestovetu prie jos amzinai. KVIESTI TURINT g_lock.
    std::unordered_map<ObjectGuid, std::unordered_map<uint32, uint32>> g_unreachable;   // botas -> mobo entry -> nesekmes

    void NoteUnreachableLocked(Player* bot, PlayerbotAI* botAI, uint32 entry)
    {
        uint32& n = g_unreachable[bot->GetGUID()][entry];
        if (++n < 5)
            return;
        g_unreachable[bot->GetGUID()].erase(entry);

        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const q = bot->GetQuestSlotQuestId(slot);
            Quest const* quest = q ? sObjectMgr->GetQuestTemplate(q) : nullptr;
            if (!quest || bot->GetQuestStatus(q) != QUEST_STATUS_INCOMPLETE)
                continue;
            for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredNpcOrGo[i] == int32(entry))
                {
                    botAI->lowPriorityQuest.insert(q);
                    LOG_INFO("playerbots", "Autopilotas {}: taikinys {} nepasiekiamas (navmesh) – uzduotis {} paliekama ramybeje.", bot->GetName(),
                             entry, q);
                    break;
                }
        }
    }

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
        bool unmetCreatureObjective = false;
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
                {
                    out.insert(uint32(entry));
                    unmetCreatureObjective = true;
                }
            }

            for (int j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
            {
                uint32 const item = quest->RequiredItemId[j];
                if (item && sit->second.ItemCount[j] < quest->RequiredItemCount[j])
                    for (uint32 e : ItemDroppers(item))
                        out.insert(e);
            }
        }

        // uzduotis „panaudok daikta ant X“ (kredito NPC tikro spawn neturi): einam i mobu, kuriems skirtas turimu daiktu burtas, vietas
        if (unmetCreatureObjective)
        {
            std::vector<Item*> items;
            CollectQuestUseItems(bot, items);
            for (Item* it : items)
            {
                ItemTemplate const* p = it->GetTemplate();
                for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                    if (p->Spells[i].SpellId > 0 && p->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                        for (auto const& te : SpellTargetEntries(p->Spells[i].SpellId))
                            if (te.first == 3)
                                out.insert(te.second);
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

    // kol yra ivykdytu, bet dar neatiduotu uzduociu, NPC neaplankom (nei atiduoti, nei priimti) – viskas darysis vienu metu
    uint32 waitingQuest = 0;
    WorldPosition waitingPos;
    if (NovaFindCompletedQuest(bot, botAI, waitingQuest, waitingPos))
        giverBlocked = true;

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

    // 3a) kvestinis mobas jau matomas, bet „grind“ jo neima (kitas aukstis / nera LOS) – einam tiesiai prie jo, o ne i spawn vieta
    for (uint32 entry : entries)
    {
        Creature* c = bot->FindNearestCreature(entry, 60.0f, true);
        if (!c || bot->IsFriendlyTo(c))
            continue;

        CreatureTemplate const* cct = c->GetCreatureTemplate();
        if (!cct || (cct->rank > CREATURE_ELITE_NORMAL && !canElite) || int32(cct->maxlevel) > int32(bot->GetLevel()) + 4)
            continue;

        uint64 const gk = c->GetGUID().GetRawValue();
        float const d = bot->GetExactDist(c);
        float const dz = std::fabs(bot->GetPositionZ() - c->GetPositionZ());
        bool const los = bot->IsWithinLOSInMap(c);

        std::lock_guard<std::mutex> guard(g_lock);
        auto& blocked = g_chaseBlock[bot->GetGUID()];
        auto bit = blocked.find(gk);
        if (bit != blocked.end() && now < bit->second)
            continue;

        ChaseState& st = g_chase[bot->GetGUID()];
        if (st.guid != c->GetGUID())
        {
            st = ChaseState();
            st.guid = c->GetGUID();
            st.bestDist = d;
            st.lastImprove = now;
            LOG_INFO("playerbots", "Autopilotas {}: uzduociu mobas {} ({}) matomas (atstumas {:.0f}, aukscio skirtumas {:.0f}, LOS {}), einam prie jo",
                     bot->GetName(), cct->Name, entry, d, dz, los ? "taip" : "ne");
        }

        if (d <= 20.0f && dz <= 4.0f && los)
        {
            // viskas tinka, o „grind“ jo vis tiek neima – po 6 s priverstinai taikomes i si moba
            if (!st.okSince)
                st.okSince = now;
            else if (now - st.okSince > 6000)
            {
                g_forced[bot->GetGUID()] = { c->GetGUID(), now + 60000 };
                LOG_INFO("playerbots", "Autopilotas {}: priverstinai puola {} ({})", bot->GetName(), cct->Name, entry);
            }
            return true;
        }

        st.okSince = 0;
        if (d + 3.0f < st.bestDist)
        {
            st.bestDist = d;
            st.lastImprove = now;
        }
        else if (now - st.lastImprove > 30000)
        {
            blocked[gk] = now + 5 * 60 * 1000;
            st = ChaseState();
            LOG_INFO("playerbots", "Autopilotas {}: nepavyksta prieiti prie {} ({}), praleidziam 5 min.", bot->GetName(), cct->Name, entry);
            NoteUnreachableLocked(bot, botAI, entry);
            continue;
        }

        // MoveFarTo naudoja g_lock-neturincia logika; atlaisviname rakta prieš judant
        WorldPosition const dest(c->GetMapId(), c->GetPositionX(), c->GetPositionY(), c->GetPositionZ());
        // (guard islieka iki funkcijos pabaigos – MoveFarTo g_lock nenaudoja)
        MoveFarTo(dest);
        return true;
    }

    if (!found)
        return false;   // visos vietos neseniai lankytos – leidziam kitiems veiksmams (klajojimas, nauju uzduociu paieska)

    // 4) atvykom ir moku nematyti (trigeris aktyvus tik kai „grind target“ tuscias) – laikom vieta aplankyta.
    //    Aukstis tikrinamas: jei mobas urve po kalnu (aukscio skirtumas didelis), virs urvo atvykimu nelaikoma.
    if (bestDist < 20.0f && std::fabs(bot->GetPositionZ() - best.z) < 12.0f)
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
            NoteUnreachableLocked(bot, botAI, best.entry);
            return true;
        }
    }

    MoveFarTo(WorldPosition(best.map, best.x, best.y, best.z));
    return true;
}

// ---------------------------------------------------------------- uzduociu daiktu / burtu naudojimas

namespace
{
    struct UseOption
    {
        enum Kind { Heal, ItemOnTarget, Talk, UseGo } kind;
        uint32 spellId;
        Item* item;
    };

    // Gift of the Naaru (drenejo rasinis gydymas; skirtingos klases – skirtingi ID). Draenei pradzios „Isgelbek isgyvenusius“.
    uint32 const kHealSpells[] = { 28880, 59542, 59543, 59544, 59545, 59547, 59548 };

    std::unordered_map<uint64, uint32> g_useCooldown;    // taikinys -> iki kada nenaudojam
    std::unordered_map<uint64, uint32> g_useApproach;    // taikinys -> kada pradejom artetis

    bool KeyBlocked(uint64 key)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        uint32 const now = getMSTime();
        auto it = g_goTries.find(key);
        if (it != g_goTries.end() && it->second.blockedUntil && now < it->second.blockedUntil)
            return true;
        auto cd = g_useCooldown.find(key);
        return cd != g_useCooldown.end() && now < cd->second;
    }

    uint32 PeekAttempts(uint64 key)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_goTries.find(key);
        return it != g_goTries.end() ? it->second.count : 0;
    }

    // Uzfiksuoja bandyma (6 per 3 min. -> taikinys 5 min. praleidziamas); grazina bandymo nr.
    uint32 NoteKeyAttempt(Player* bot, uint64 key, uint32 entry)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        GoTries& g = g_goTries[key];
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
            LOG_INFO("playerbots", "Autopilotas {}: uzduociu taikinys {} nepasiduoda po 6 bandymu, praleidziam 5 min.", bot->GetName(),
                     entry);
        }
        g_useCooldown[key] = now + 12000;     // gyvas NPC „dekoja“ ~5 s ir dingsta; nespaudziam is naujo kas tika
        g_useApproach.erase(key);
        return n;
    }

    // Draugiski uzduociu mobai (belaisviai ir pan.) neturi „gossip“: juos islaisvina salia esantis objektas (narvas, svirtis).
    // Objektu sablonai randami pagal tikras spawn vietas (≤ 6 m nuo mobo), kesuojama.
    std::unordered_map<uint32, std::vector<uint32>> g_helperObjects;   // mobo entry -> objektu entry

    std::vector<uint32> const& HelperObjectsFor(uint32 creatureEntry)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_helperObjects.find(creatureEntry);
            if (it != g_helperObjects.end())
                return it->second;
        }

        std::vector<uint32> found;
        std::vector<Spawn> const& spawns = SpawnsOf(creatureEntry);
        for (auto const& kv : sObjectMgr->GetAllGOData())
        {
            GameObjectData const& d = kv.second;
            bool isNear = false;
            for (Spawn const& s : spawns)
                if (s.map == d.mapid && std::fabs(s.x - d.posX) < 6.0f && std::fabs(s.y - d.posY) < 6.0f && std::fabs(s.z - d.posZ) < 6.0f)
                {
                    isNear = true;
                    break;
                }
            if (!isNear)
                continue;

            GameObjectTemplate const* gt = sObjectMgr->GetGameObjectTemplate(d.id);
            if (!gt || (gt->type != GAMEOBJECT_TYPE_DOOR && gt->type != GAMEOBJECT_TYPE_BUTTON && gt->type != GAMEOBJECT_TYPE_GOOBER))
                continue;
            if (std::find(found.begin(), found.end(), d.id) == found.end())
                found.push_back(d.id);
        }

        std::lock_guard<std::mutex> guard(g_lock);
        return g_helperObjects.emplace(creatureEntry, std::move(found)).first->second;
    }

    // Nebaigtu uzduociu tiksliniai mobai / objektai.
    void QuestObjectiveEntries(Player* bot, PlayerbotAI* botAI, std::unordered_set<uint32>& creatures,
                               std::unordered_set<uint32>& objects)
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

            for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                int32 const entry = quest->RequiredNpcOrGo[i];
                if (!entry || sit->second.CreatureOrGOCount[i] >= quest->RequiredNpcOrGoCount[i])
                    continue;
                if (entry > 0)
                    creatures.insert(uint32(entry));
                else
                    objects.insert(uint32(-entry));
            }
        }
    }

    // Randa artimiausia taikini, kuriam yra ka daryti, ir parinktis (jos keiciamos kas bandyma).
    bool FindUseTarget(Player* bot, PlayerbotAI* botAI, WorldObject*& target, std::vector<UseOption>& options)
    {
        std::unordered_set<uint32> creatures, objects;
        QuestObjectiveEntries(bot, botAI, creatures, objects);
        if (creatures.empty() && objects.empty())
            return false;

        std::vector<Item*> items;
        CollectQuestUseItems(bot, items);

        uint32 healSpell = 0;
        for (uint32 id : kHealSpells)
            if (bot->HasSpell(id))
            {
                healSpell = id;
                break;
            }

        // daiktu burtai pagal taikinio tipa
        auto itemOptions = [&](uint32 wantMask, std::vector<UseOption>& out)
        {
            for (Item* it : items)
            {
                ItemTemplate const* p = it->GetTemplate();
                for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                {
                    if (p->Spells[i].SpellId <= 0 || p->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                        continue;
                    SpellInfo const* si = sSpellMgr->GetSpellInfo(p->Spells[i].SpellId);
                    if (si && (si->GetExplicitTargetMask() & wantMask))
                        out.push_back({ UseOption::ItemOnTarget, uint32(p->Spells[i].SpellId), it });
                }
            }
        };

        float bestDist = FLT_MAX;
        auto consider = [&](WorldObject* obj, std::vector<UseOption>&& opts)
        {
            if (!obj || opts.empty() || KeyBlocked(obj->GetGUID().GetRawValue()))
                return;
            float const d = bot->GetDistance(obj);
            if (d < bestDist)
            {
                bestDist = d;
                target = obj;
                options = std::move(opts);
            }
        };

        for (uint32 entry : creatures)
        {
            if (Creature* c = bot->FindNearestCreature(entry, 70.0f, true))
            {
                std::vector<UseOption> o;
                if (bot->IsFriendlyTo(c))
                {
                    if (healSpell)
                        o.push_back({ UseOption::Heal, healSpell, nullptr });
                    if (c->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP))
                        o.push_back({ UseOption::Talk, 0, nullptr });

                    // belaisvis narve ir pan.: spaudziam salia esanti narva / svirti
                    for (uint32 goEntry : HelperObjectsFor(entry))
                        if (GameObject* helper = c->FindNearestGameObject(goEntry, 8.0f, true))
                            consider(helper, std::vector<UseOption>{ { UseOption::UseGo, 0, nullptr } });
                }
                itemOptions(TARGET_FLAG_UNIT_MASK & ~(TARGET_FLAG_UNIT_DEAD | TARGET_FLAG_CORPSE_MASK), o);
                consider(c, std::move(o));
            }

            if (!items.empty())
                if (Creature* dead = bot->FindNearestCreature(entry, 40.0f, false))
                {
                    std::vector<UseOption> o;
                    itemOptions(TARGET_FLAG_UNIT_DEAD | TARGET_FLAG_CORPSE_MASK, o);
                    consider(dead, std::move(o));
                }
        }

        if (!items.empty())
            for (uint32 entry : objects)
                if (GameObject* go = bot->FindNearestGameObject(entry, 70.0f, true))
                {
                    std::vector<UseOption> o;
                    itemOptions(TARGET_FLAG_GAMEOBJECT_MASK, o);
                    consider(go, std::move(o));
                }

        // daiktu burtai su konkreciu taikiniu (conditions): pvz. „Skiepijantis kristalas“ ant Gūžtamiškių pelėdžmogio. Naudojam tik
        // kai yra bent viena nebaigta uzduotis su mobu tikslu (kitaip daiktas nenaudojamas veltui).
        if (!creatures.empty())
            for (Item* it : items)
            {
                ItemTemplate const* p = it->GetTemplate();
                for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                {
                    if (p->Spells[i].SpellId <= 0 || p->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                        continue;
                    for (auto const& te : SpellTargetEntries(p->Spells[i].SpellId))
                    {
                        WorldObject* obj = nullptr;
                        if (te.first == 3)
                            obj = bot->FindNearestCreature(te.second, 70.0f, true);
                        else if (te.first == 5)
                            obj = bot->FindNearestGameObject(te.second, 70.0f, true);
                        if (!obj)
                            continue;

                        std::vector<UseOption> o;
                        o.push_back({ UseOption::ItemOnTarget, uint32(p->Spells[i].SpellId), it });
                        consider(obj, std::move(o));
                    }
                }
            }

        return target != nullptr;
    }
}

bool NovaHasQuestUseTarget(Player* bot, PlayerbotAI* botAI)
{
    WorldObject* target = nullptr;
    std::vector<UseOption> options;
    return FindUseTarget(bot, botAI, target, options);
}

bool NovaQuestUseAction::Execute(Event /*event*/)
{
    WorldObject* target = nullptr;
    std::vector<UseOption> options;
    if (!FindUseTarget(bot, botAI, target, options))
        return false;

    uint64 const key = target->GetGUID().GetRawValue();
    UseOption const op = options[PeekAttempts(key) % options.size()];

    // reikiamas atstumas pagal parinkti
    float need = 18.0f;
    if (op.kind == UseOption::Talk || op.kind == UseOption::UseGo)
        need = INTERACTION_DISTANCE - 1.0f;
    else if (op.kind == UseOption::ItemOnTarget)
        if (SpellInfo const* si = sSpellMgr->GetSpellInfo(op.spellId))
            need = std::max(3.0f, std::min(si->GetMaxRange(!(target->ToUnit() && bot->IsHostileTo(target->ToUnit()))), 25.0f) - 2.0f);

    if (bot->GetDistance(target) > need || !bot->IsWithinLOSInMap(target))
    {
        // jei arteti per ilgai (nepasiekiama) – taikini praleidziam 5 min.
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> guard(g_lock);
            uint32& first = g_useApproach[key];
            if (!first)
                first = now;
            else if (now - first > 40000)
            {
                GoTries& g = g_goTries[key];
                g.blockedUntil = now + 5 * 60 * 1000;
                g_useApproach.erase(key);
                LOG_INFO("playerbots", "Autopilotas {}: nepavyksta prieiti prie uzduociu taikinio {}, praleidziam 5 min.", bot->GetName(),
                         target->GetEntry());
                return false;
            }
        }
        MoveWorldObjectTo(target->GetGUID(), std::max(2.0f, need - 4.0f));
        return true;
    }

    if (bot->IsMounted())
        bot->Dismount();
    bot->StopMoving();
    bot->SetFacingToObject(target);

    uint32 const attempt = NoteKeyAttempt(bot, key, target->GetEntry());
    Unit* unit = target->ToUnit();
    GameObject* go = target->ToGameObject();

    switch (op.kind)
    {
        case UseOption::Heal:
            if (unit)
                bot->CastSpell(unit, op.spellId, true);
            break;
        case UseOption::ItemOnTarget:
            if (unit)
                bot->CastSpell(unit, op.spellId, true, op.item);
            else if (go)
                bot->CastSpell(go, op.spellId, true, op.item);
            break;
        case UseOption::Talk:
        {
            WorldPacket p(CMSG_GOSSIP_HELLO);
            p << target->GetGUID();
            bot->GetSession()->HandleGossipHelloOpcode(p);
            break;
        }
        case UseOption::UseGo:
        {
            if (go)
            {
                WorldPacket p(CMSG_GAMEOBJ_USE);
                p << go->GetGUID();
                bot->GetSession()->HandleGameObjectUseOpcode(p);
            }
            break;
        }
    }

    ForceToWait(3500);
    LOG_INFO("playerbots", "Autopilotas {}: naudoja uzduociai ({}. bandymas, veiksmas {}, burtas {}) taikini {} ({})", bot->GetName(),
             attempt, uint32(op.kind), op.spellId, target->GetEntry(), target->GetName());
    return true;
}

// Priverstinis „grind“ taikinys (zr. NovaQuestMobsAction): tuscias, jei nera arba pasibaigo.
ObjectGuid NovaForcedTarget(Player* bot)
{
    std::lock_guard<std::mutex> guard(g_lock);
    auto it = g_forced.find(bot->GetGUID());
    if (it == g_forced.end())
        return ObjectGuid::Empty;
    if (getMSTime() >= it->second.until)
    {
        g_forced.erase(it);
        return ObjectGuid::Empty;
    }
    return it->second.guid;
}

// ---------------------------------------------------------------- Strand of the Ancients

namespace
{
    BattlegroundSA* SaOf(Player* bot)
    {
        Battleground* bg = bot->GetBattleground();
        if (!bg)
            return nullptr;

        BattlegroundTypeId type = bg->GetBgTypeID();
        if (type == BATTLEGROUND_RB)
            type = bg->GetBgTypeID(true);

        return type == BATTLEGROUND_SA ? static_cast<BattlegroundSA*>(bg) : nullptr;
    }

    std::unordered_map<ObjectGuid, uint32> g_saLastHit;
}

bool NovaSaActive(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->InBattleground() || bot->InArena() || !bot->IsAlive() || bot->IsInCombat() ||
        bot->IsBeingTeleported())
        return false;

    BattlegroundSA* sa = SaOf(bot);
    if (!sa || sa->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    return sa->Status == BG_SA_ROUND_ONE || sa->Status == BG_SA_ROUND_TWO || sa->Status == BG_SA_BONUS_ROUND;
}

bool NovaSaAction::Execute(Event /*event*/)
{
    BattlegroundSA* sa = SaOf(bot);
    if (!sa)
        return false;

    bool const attacker = bot->GetTeamId() == sa->Attackers;
    auto destroyed = [&](uint32 id) { return sa->GateStatus[id] == BG_SA_GATE_DESTROYED; };

    // sekantis tikslas: vartu grandine Zalia/Melyna (1 lygis) -> Violetine/Raudona (2) -> Geltona (3) -> Senovinis -> relikvija
    uint32 const counter = uint32(bot->GetGUID().GetCounter());
    uint32 objectId;
    bool relic = false;
    if (destroyed(BG_SA_ANCIENT_GATE))
    {
        relic = true;
        objectId = BG_SA_TITAN_RELIC;
    }
    else if (destroyed(BG_SA_YELLOW_GATE))
        objectId = BG_SA_ANCIENT_GATE;
    else if (destroyed(BG_SA_PURPLE_GATE) || destroyed(BG_SA_RED_GATE))
        objectId = BG_SA_YELLOW_GATE;
    else
    {
        std::vector<uint32> candidates;
        if (destroyed(BG_SA_GREEN_GATE))
            candidates.push_back(BG_SA_PURPLE_GATE);
        if (destroyed(BG_SA_BLUE_GATE))
            candidates.push_back(BG_SA_RED_GATE);
        if (candidates.empty())
        {
            candidates.push_back(BG_SA_GREEN_GATE);
            candidates.push_back(BG_SA_BLUE_GATE);
        }
        objectId = candidates[counter % candidates.size()];
    }

    GameObject* target = sa->GetBGObject(objectId);
    if (!target)
        return false;

    // atakuojantys stoja is rytu (x didesnis), ginantys – is vakaru (uz vartu)
    float const side = attacker ? 9.0f : -9.0f;
    float const px = target->GetPositionX() + side;
    float const py = target->GetPositionY() + float(int32(counter % 9) - 4);
    float const pz = target->GetPositionZ();

    float const dist = bot->GetDistance2d(target);
    float const reach = relic ? 5.0f : 16.0f;

    if (!attacker)
    {
        // ginantys: jei jau netoli savo pozicijos – nieko nedarom (kovos botai pagal pvp strategija)
        if (bot->GetDistance2d(px, py) > 14.0f)
            return MoveTo(bot->GetMapId(), px, py, pz);
        return false;
    }

    if (dist > reach)
        return MoveTo(bot->GetMapId(), px, py, pz);

    if (relic)
    {
        // relikvija: panaudojam objekta (taip pabaigiamas raundas)
        WorldPacket p(CMSG_GAMEOBJ_USE);
        p << target->GetGUID();
        bot->GetSession()->HandleGameObjectUseOpcode(p);
        return true;
    }

    // „ardom“ vartus: kas 3 s ~0,25 % visos vartu gyvybes (be vezimu – botai neturi pilnos vezimu logikos)
    uint32 const now = getMSTime();
    uint32& last = g_saLastHit[bot->GetGUID()];
    if (now - last >= 3000)
    {
        last = now;
        bot->StopMoving();
        bot->SetFacingToObject(target);
        GameObjectValue const* value = target->GetGOValue();
        int32 const damage = std::max<int32>(1, int32(value->Building.MaxHealth / 400));
        target->ModifyHealth(-damage, bot, 0);
    }

    return true;
}

// ---------------------------------------------------------------- augalai ir ruda

namespace
{
    struct GatherState
    {
        ObjectGuid node;
        float best = FLT_MAX;
        uint32 lastImprove = 0;
        uint32 attempts = 0;
    };

    std::unordered_map<ObjectGuid, GatherState> g_gather;
    std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint32>> g_gatherBlock;   // botas -> mazgas -> iki kada praleidziam
    std::unordered_set<ObjectGuid> g_pickGiven;

    bool HasMiningPick(Player* bot)
    {
        static uint32 const picks[] = { 756, 778, 1819, 1893, 1959, 2901, 9465, 20723, 40772, 40892, 40893 };
        for (uint32 id : picks)
            if (bot->HasItemCount(id, 1))
                return true;
        return false;
    }

    // Randa artimiausia renkama augala / gysla, kuria veikejas gali surinkti (igudis, lygis, kirtis, neuzblokuota).
    GameObject* FindGatherNode(Player* bot, PlayerbotAI* botAI, LootObject& lo, float maxDist)
    {
        if (!bot->HasSkill(SKILL_HERBALISM) && !bot->HasSkill(SKILL_MINING))
            return nullptr;

        // kasyba reikalauja kirto – autopilotui duodame vieną kartą (zaidejas ji paprastai nusiperka pas pardavejus)
        if (bot->HasSkill(SKILL_MINING) && !HasMiningPick(bot))
        {
            bool give;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                give = g_pickGiven.insert(bot->GetGUID()).second;
            }
            if (give && bot->AddItem(2901, 1))
                LOG_INFO("playerbots", "Autopilotas {}: gavo kasimo kirti (kasybai)", bot->GetName());
        }

        GuidVector nodes = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest game objects no los")->Get();
        uint32 const now = getMSTime();
        GameObject* best = nullptr;
        float bestDist = maxDist;
        LootObject bestLo;
        for (ObjectGuid const& guid : nodes)
        {
            {
                std::lock_guard<std::mutex> guard(g_lock);
                auto bit = g_gatherBlock.find(bot->GetGUID());
                if (bit != g_gatherBlock.end())
                {
                    auto it = bit->second.find(guid);
                    if (it != bit->second.end() && now < it->second)
                        continue;
                }
            }

            GameObject* go = botAI->GetGameObject(guid);
            if (!go || !go->isSpawned() || go->GetGoType() != GAMEOBJECT_TYPE_CHEST)
                continue;

            float const d = bot->GetDistance(go);
            if (d >= bestDist)
                continue;

            LootObject cand(bot, guid);
            if (cand.IsEmpty() || (cand.skillId != SKILL_HERBALISM && cand.skillId != SKILL_MINING) || !cand.IsLootPossible(bot))
                continue;

            best = go;
            bestDist = d;
            bestLo = cand;
        }

        if (best)
            lo = bestLo;
        return best;
    }
}

bool NovaHasGatherNode(Player* bot, PlayerbotAI* botAI)
{
    if (!bot->HasSkill(SKILL_HERBALISM) && !bot->HasSkill(SKILL_MINING))
        return false;

    LootObject lo;
    return FindGatherNode(bot, botAI, lo, 45.0f) != nullptr;
}

bool NovaGatherAction::Execute(Event /*event*/)
{
    LootObject lo;
    GameObject* go = FindGatherNode(bot, botAI, lo, 45.0f);
    if (!go)
        return false;

    if (bot->IsNonMeleeSpellCast(false))
        return true;   // jau renka – laukiam

    uint32 const now = getMSTime();
    float const dist = bot->GetDistance(go);

    if (dist > INTERACTION_DISTANCE - 3.0f)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            GatherState& st = g_gather[bot->GetGUID()];
            if (st.node != go->GetGUID())
            {
                st = GatherState();
                st.node = go->GetGUID();
                st.lastImprove = now;
                LOG_INFO("playerbots", "Autopilotas {}: eina rinkti {} ({}), atstumas {:.0f}", bot->GetName(), go->GetEntry(),
                         lo.skillId == SKILL_MINING ? "ruda" : "augalas", dist);
            }

            if (dist + 2.0f < st.best)
            {
                st.best = dist;
                st.lastImprove = now;
            }
            else if (now - st.lastImprove > 20000)
            {
                g_gatherBlock[bot->GetGUID()][go->GetGUID()] = now + 5 * 60 * 1000;
                g_gather.erase(bot->GetGUID());
                LOG_INFO("playerbots", "Autopilotas {}: nepavyksta prieiti prie {}, praleidziam 5 min.", bot->GetName(), go->GetEntry());
                return false;
            }
        }

        return MoveWorldObjectTo(go->GetGUID(), 2.0f);
    }

    {
        std::lock_guard<std::mutex> guard(g_lock);
        GatherState& st = g_gather[bot->GetGUID()];
        if (st.node != go->GetGUID())
        {
            st = GatherState();
            st.node = go->GetGUID();
        }

        if (++st.attempts > 6)
        {
            g_gatherBlock[bot->GetGUID()][go->GetGUID()] = now + 5 * 60 * 1000;
            g_gather.erase(bot->GetGUID());
            LOG_INFO("playerbots", "Autopilotas {}: nepavyksta surinkti {}, praleidziam 5 min.", bot->GetName(), go->GetEntry());
            return false;
        }
    }

    context->GetValue<LootObject>("loot target")->Set(lo);
    bool const ok = botAI->DoSpecificAction("open loot", Event(), true);
    if (ok)
    {
        LOG_INFO("playerbots", "Autopilotas {}: renka {} ({})", bot->GetName(), go->GetEntry(), lo.skillId == SKILL_MINING ? "ruda" : "augalas");
        ForceToWait(3500);
    }

    return ok;
}

// ---------------------------------------------------------------- daiktai su „spell focus“ objektu (Medzio draugija ir pan.)

namespace
{
    struct FocusJob
    {
        uint32 questId = 0;
        Item* item = nullptr;
        uint32 spellId = 0;
        uint32 focusId = 0;
    };

    struct FocusState
    {
        uint32 castAt = 0;
        uint32 approachSince = 0;
        uint32 casts = 0;
        uint32 spellId = 0;
        uint32 questId = 0;
    };

    std::unordered_map<ObjectGuid, FocusState> g_focus;
    std::unordered_map<uint32, std::unordered_set<uint32>> g_focusEntries;     // focusId -> objektu sablonai
    std::unordered_map<uint32, std::vector<GoSpawn>> g_focusSpawns;            // focusId -> tikros spawn vietos

    std::unordered_set<uint32> const& FocusEntries(uint32 focusId)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_focusEntries.find(focusId);
            if (it != g_focusEntries.end())
                return it->second;
        }

        std::unordered_set<uint32> found;
        for (auto const& kv : *sObjectMgr->GetGameObjectTemplates())
            if (kv.second.type == GAMEOBJECT_TYPE_SPELL_FOCUS && kv.second.spellFocus.focusId == focusId)
                found.insert(kv.first);

        std::lock_guard<std::mutex> guard(g_lock);
        return g_focusEntries.emplace(focusId, std::move(found)).first->second;
    }

    // Nebaigta uzduotis, kuriai reikia daikto, kurio burtui reikia „spell focus“ objekto (pagal StartItem / RequiredItemId).
    bool FindFocusJob(Player* bot, PlayerbotAI* botAI, FocusJob& job)
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

            std::vector<uint32> itemIds;
            if (quest->GetSrcItemId())
                itemIds.push_back(quest->GetSrcItemId());
            for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i])
                    itemIds.push_back(quest->RequiredItemId[i]);

            for (uint32 id : itemIds)
            {
                Item* it = bot->GetItemByEntry(id);
                if (!it)
                    continue;
                // daiktas imamas tik is paties uzduoties lauku (StartItem / RequiredItemId), todel klases netikrinam:
                // pvz. „Medzio persirengimo rinkinys“ (23792) yra klases 15 (ivairus), ne 12 (uzduociu)
                ItemTemplate const* p = it->GetTemplate();
                if (!p)
                    continue;

                for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                {
                    if (p->Spells[i].SpellId <= 0 || p->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                        continue;
                    SpellInfo const* si = sSpellMgr->GetSpellInfo(p->Spells[i].SpellId);
                    if (!si || !si->RequiresSpellFocus)
                        continue;

                    job.questId = q;
                    job.item = it;
                    job.spellId = uint32(p->Spells[i].SpellId);
                    job.focusId = si->RequiresSpellFocus;
                    return true;
                }
            }
        }
        return false;
    }
}

bool NovaHasQuestFocus(Player* bot, PlayerbotAI* botAI)
{
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_focus.find(bot->GetGUID()) != g_focus.end())
            return true;   // jau pradeta – reikia baigti (laukti / nuimti aura)
    }

    FocusJob job;
    return FindFocusJob(bot, botAI, job);
}

bool NovaQuestFocusAction::Execute(Event /*event*/)
{
    uint32 const now = getMSTime();

    // pradeta anksciau: laukiam kredito, o jam ikritus (arba per ilgai) – nuimam aura
    {
        FocusState st;
        bool have = false;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto it = g_focus.find(bot->GetGUID());
            if (it != g_focus.end())
            {
                st = it->second;
                have = true;
            }
        }

        if (have && st.castAt)
        {
            bool const done = bot->GetQuestStatus(st.questId) != QUEST_STATUS_INCOMPLETE;
            bool const tooLong = now - st.castAt > 180000;
            if (done || tooLong || !bot->HasAura(st.spellId))
            {
                if (bot->HasAura(st.spellId))
                    bot->RemoveAurasDueToSpell(st.spellId);

                if (tooLong && !done)
                    botAI->lowPriorityQuest.insert(st.questId);

                {
                    std::lock_guard<std::mutex> guard(g_lock);
                    if (!done && !tooLong)
                    {
                        // aura baigesi nepasiekus kredito – dar kartą (iki 3 kartu)
                        FocusState& ref = g_focus[bot->GetGUID()];
                        ref.castAt = 0;
                        if (ref.casts >= 3)
                        {
                            g_focus.erase(bot->GetGUID());
                            botAI->lowPriorityQuest.insert(st.questId);
                        }
                    }
                    else
                        g_focus.erase(bot->GetGUID());
                }
                LOG_INFO("playerbots", "Autopilotas {}: medziagos veiksmas baigtas (uzduotis {}, kreditas {})", bot->GetName(), st.questId,
                         done ? "gautas" : "negautas");
                return true;
            }

            ForceToWait(2000);   // stovim ir laukiam ivykio
            return true;
        }
    }

    FocusJob job;
    if (!FindFocusJob(bot, botAI, job))
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_focus.erase(bot->GetGUID());
        return false;
    }

    std::unordered_set<uint32> const& entries = FocusEntries(job.focusId);
    GameObject* go = nullptr;
    float best = FLT_MAX;
    for (uint32 entry : entries)
        if (GameObject* cand = bot->FindNearestGameObject(entry, 120.0f, true))
        {
            float const d = bot->GetDistance(cand);
            if (d < best)
            {
                best = d;
                go = cand;
            }
        }

    if (!go)
    {
        // objekto nematyti – einam i jo tikras spawn vietas
        return ApproachGoSpawns(bot, GoSpawnsForEntries(entries, g_focusSpawns, job.focusId),
                                [this](WorldPosition const& p) { return MoveFarTo(p); });
    }

    uint32 const focusDist = std::max<uint32>(go->GetGOInfo()->spellFocus.dist, 6u);
    float const need = std::max(3.0f, std::min<float>(float(focusDist), 25.0f) - 3.0f);

    if (best > need)
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            FocusState& st = g_focus[bot->GetGUID()];
            if (!st.approachSince)
                st.approachSince = now;
            else if (now - st.approachSince > 60000)
            {
                g_focus.erase(bot->GetGUID());
                botAI->lowPriorityQuest.insert(job.questId);
                LOG_INFO("playerbots", "Autopilotas {}: nepavyksta prieiti prie objekto {} (uzduotis {}), praleidziam", bot->GetName(),
                         go->GetEntry(), job.questId);
                return false;
            }
        }
        return MoveWorldObjectTo(go->GetGUID(), std::max(2.0f, need - 2.0f));
    }

    if (bot->IsMounted())
        bot->Dismount();
    bot->StopMoving();

    {
        std::lock_guard<std::mutex> guard(g_lock);
        FocusState& st = g_focus[bot->GetGUID()];
        st.castAt = now;
        st.spellId = job.spellId;
        st.questId = job.questId;
        ++st.casts;
    }

    bot->CastSpell(bot, job.spellId, true, job.item);
    ForceToWait(3500);
    LOG_INFO("playerbots", "Autopilotas {}: panaudojo daikta {} prie objekto {} (uzduotis {}), laukia ivykio", bot->GetName(),
             job.item->GetEntry(), go->GetEntry(), job.questId);
    return true;
}

// ---------------------------------------------------------------- amunicija

namespace
{
    // Amunicijos potipis pagal ekipuota nuotoline ginkla (0 – amunicijos nereikia: nera ginklo, metamasis, lazdele).
    uint32 AmmoSubClassFor(Player* bot)
    {
        Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!weapon || !weapon->GetTemplate())
            return 0;

        switch (weapon->GetTemplate()->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_GUN:
                return ITEM_SUBCLASS_BULLET;
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                return ITEM_SUBCLASS_ARROW;
            default:
                return 0;
        }
    }

    // Geriausia veikejui tinkama amunicija (kaip PlayerbotFactory::InitAmmo).
    uint32 BestAmmoFor(Player* bot, uint32 subClass)
    {
        for (uint32 entry : sRandomItemMgr->GetAmmo(bot->GetLevel(), subClass))
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto || proto->RequiredLevel > bot->GetLevel())
                continue;

            if (sPlayerbotAIConfig->limitGearExpansion && bot->GetLevel() <= 60 && entry >= 23728)
                continue;
            if (sPlayerbotAIConfig->limitGearExpansion && bot->GetLevel() <= 70 && entry >= 35570)
                continue;

            return entry;
        }

        // atsarginis variantas (pradiniai lygiai)
        return subClass == ITEM_SUBCLASS_ARROW ? 2512 : 2516;   // Rough Arrow / Light Shot
    }
}

bool NovaNeedsAmmo(Player* bot, PlayerbotAI* /*botAI*/)
{
    if (!bot || !NovaIsAutopilot(bot) || !bot->IsAlive() || !bot->IsInWorld())
        return false;

    uint32 const sub = AmmoSubClassFor(bot);
    if (!sub)
        return false;

    uint32 const best = BestAmmoFor(bot, sub);
    if (!best)
        return false;

    uint32 const cur = bot->GetUInt32Value(PLAYER_AMMO_ID);
    if (cur != best)
        return true;

    return bot->GetItemCount(best) < 150;
}

bool NovaAmmoAction::Execute(Event /*event*/)
{
    uint32 const sub = AmmoSubClassFor(bot);
    if (!sub)
        return false;

    uint32 const entry = BestAmmoFor(bot, sub);
    if (!entry)
        return false;

    uint32 const have = bot->GetItemCount(entry);
    if (have < 600)
        bot->AddItem(entry, 600 - have);

    bot->SetAmmo(entry);
    LOG_INFO("playerbots", "Autopilotas {}: amunicija {} – turi {}, ekipuota {}", bot->GetName(), entry, bot->GetItemCount(entry),
             bot->GetUInt32Value(PLAYER_AMMO_ID) == entry ? "taip" : "ne");
    return true;
}

// ---------------------------------------------------------------- kuprines daiktai: uzduociu pradzia ir stiprinimai

namespace
{
    std::unordered_map<ObjectGuid, std::unordered_map<uint32, uint32>> g_buffCooldown;   // botas -> daikto entry -> iki kada

    template <typename Fn>
    void ForEachBagItem(Player* bot, Fn&& fn)
    {
        for (uint8 s = INVENTORY_SLOT_ITEM_START; s < INVENTORY_SLOT_ITEM_END; ++s)
            if (Item* it = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s))
                fn(it);
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* pBag = bot->GetBagByPos(b))
                for (uint32 i = 0; i < pBag->GetBagSize(); ++i)
                    if (Item* it = pBag->GetItemByPos(i))
                        fn(it);
    }

    // Daiktas, kuris pradeda uzduoti, kuria veikejas DABAR gali priimti.
    Item* FindStarterItem(Player* bot, PlayerbotAI* botAI, Quest const*& questOut)
    {
        Item* found = nullptr;
        ForEachBagItem(bot, [&](Item* it)
        {
            if (found)
                return;
            ItemTemplate const* p = it->GetTemplate();
            if (!p || !p->StartQuest)
                return;
            if (botAI->lowPriorityQuest.find(p->StartQuest) != botAI->lowPriorityQuest.end())
                return;

            Quest const* q = sObjectMgr->GetQuestTemplate(p->StartQuest);
            if (!q || bot->GetQuestStatus(p->StartQuest) != QUEST_STATUS_NONE)
                return;
            if (!bot->CanTakeQuest(q, false) || !bot->CanAddQuest(q, false))
                return;

            found = it;
            questOut = q;
        });
        return found;
    }

    // Uzduotis be jokiu tikslu, kuriai uzteka „perskaityti / panaudoti“ pradzios daikta (pvz. kalbos vadovelis, pergamentas).
    struct ReadTries
    {
        uint32 tries = 0;
        uint32 nextAt = 0;
    };
    std::unordered_map<ObjectGuid, std::unordered_map<uint32, ReadTries>> g_readTries;   // botas -> uzduotis

    Item* FindReadItem(Player* bot, PlayerbotAI* botAI, uint32& spellOut, uint32& questOut)
    {
        uint32 const now = getMSTime();
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const q = bot->GetQuestSlotQuestId(slot);
            if (!q || bot->GetQuestStatus(q) != QUEST_STATUS_INCOMPLETE)
                continue;
            if (botAI->lowPriorityQuest.find(q) != botAI->lowPriorityQuest.end())
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(q);
            if (!quest || !quest->GetSrcItemId())
                continue;

            bool hasObjective = false;
            for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredNpcOrGo[i] != 0 && quest->RequiredNpcOrGoCount[i] > 0)
                    hasObjective = true;
            for (int j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
                if (quest->RequiredItemId[j] && quest->RequiredItemCount[j])
                    hasObjective = true;
            if (hasObjective)
                continue;

            Item* it = bot->GetItemByEntry(quest->GetSrcItemId());
            ItemTemplate const* p = it ? it->GetTemplate() : nullptr;
            if (!p)
                continue;

            {
                std::lock_guard<std::mutex> guard(g_lock);
                ReadTries const& rt = g_readTries[bot->GetGUID()][q];
                if (rt.tries >= 4 || now < rt.nextAt)
                    continue;
            }

            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                if (p->Spells[i].SpellId <= 0 || p->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                    continue;
                SpellInfo const* si = sSpellMgr->GetSpellInfo(p->Spells[i].SpellId);
                if (!si || si->RequiresSpellFocus)
                    continue;          // „spell focus“ daiktus tvarko NovaQuestFocusAction
                if (bot->HasSpellCooldown(uint32(p->Spells[i].SpellId)) || bot->CanUseItem(it) != EQUIP_ERR_OK)
                    continue;

                spellOut = uint32(p->Spells[i].SpellId);
                questOut = q;
                return it;
            }
        }
        return nullptr;
    }

    // Burtas – ilgai trunkanti nauda sau (visi efektai: aura ant naudotojo).
    bool IsSelfBuffSpell(SpellInfo const* si)
    {
        if (!si || si->GetDuration() < 5 * 60 * 1000)
            return false;

        bool aura = false;
        for (SpellEffectInfo const& eff : si->Effects)
        {
            if (!eff.Effect)
                continue;
            if (eff.Effect != SPELL_EFFECT_APPLY_AURA || eff.TargetA.GetTarget() != TARGET_UNIT_CASTER)
                return false;
            aura = true;
        }
        return aura;
    }

    Item* FindBuffItem(Player* bot, uint32& spellOut)
    {
        uint32 const now = getMSTime();
        Item* found = nullptr;
        std::lock_guard<std::mutex> guard(g_lock);
        auto& cds = g_buffCooldown[bot->GetGUID()];
        ForEachBagItem(bot, [&](Item* it)
        {
            if (found)
                return;
            ItemTemplate const* p = it->GetTemplate();
            if (!p || p->Class != ITEM_CLASS_CONSUMABLE || p->RequiredLevel > bot->GetLevel())
                return;
            if (p->SubClass != ITEM_SUBCLASS_ELIXIR && p->SubClass != ITEM_SUBCLASS_FLASK && p->SubClass != ITEM_SUBCLASS_SCROLL &&
                p->SubClass != ITEM_SUBCLASS_CONSUMABLE && p->SubClass != ITEM_SUBCLASS_CONSUMABLE_OTHER)
                return;

            auto cd = cds.find(p->ItemId);
            if (cd != cds.end() && now < cd->second)
                return;

            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                if (p->Spells[i].SpellId <= 0 || p->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                    continue;

                uint32 const sid = uint32(p->Spells[i].SpellId);
                if (bot->HasAura(sid) || bot->HasSpellCooldown(sid))
                    continue;
                if (!IsSelfBuffSpell(sSpellMgr->GetSpellInfo(sid)))
                    continue;
                if (bot->CanUseItem(it) != EQUIP_ERR_OK)
                    continue;

                found = it;
                spellOut = sid;
                return;
            }
        });
        return found;
    }
}

bool NovaHasUsefulItems(Player* bot, PlayerbotAI* botAI)
{
    Quest const* q = nullptr;
    if (FindStarterItem(bot, botAI, q))
        return true;

    uint32 rs = 0, rq = 0;
    if (FindReadItem(bot, botAI, rs, rq))
        return true;

    uint32 sid = 0;
    return FindBuffItem(bot, sid) != nullptr;
}

bool NovaItemsAction::Execute(Event /*event*/)
{
    // 1) uzduociu pradzios daiktai (iskrite is mobu): priimam kaip is NPC
    Quest const* quest = nullptr;
    if (Item* starter = FindStarterItem(bot, botAI, quest))
    {
        bot->AddQuestAndCheckCompletion(quest, starter);
        if (quest->GetSrcSpell() > 0)
            bot->CastSpell(bot, quest->GetSrcSpell(), true);

        LOG_INFO("playerbots", "Autopilotas {}: priemė užduotį {} iš daikto {}", bot->GetName(), quest->GetQuestId(), starter->GetEntry());
        ForceToWait(1000);
        return true;
    }

    // 1b) uzduotis „perskaityk / panaudok daikta“ (be kitu tikslu): naudojam pradzios daikta
    {
        uint32 readSpell = 0, readQuest = 0;
        if (Item* readItem = FindReadItem(bot, botAI, readSpell, readQuest))
        {
            {
                std::lock_guard<std::mutex> guard(g_lock);
                ReadTries& rt = g_readTries[bot->GetGUID()][readQuest];
                ++rt.tries;
                rt.nextAt = getMSTime() + 45 * 1000;
            }

            if (bot->IsMounted())
                bot->Dismount();
            bot->StopMoving();

            SpellCastResult const res = bot->CastSpell(bot, readSpell, false, readItem);
            LOG_INFO("playerbots", "Autopilotas {}: perskaito / panaudoja uzduociu daikta {} (uzduotis {}, burtas {}), rezultatas {}", bot->GetName(),
                     readItem->GetEntry(), readQuest, readSpell, uint32(res));
            ForceToWait(2500);
            return true;
        }
    }

    // 2) stiprinamieji daiktai
    uint32 spellId = 0;
    if (Item* buff = FindBuffItem(bot, spellId))
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_buffCooldown[bot->GetGUID()][buff->GetEntry()] = getMSTime() + 10 * 60 * 1000;
        }

        if (bot->IsMounted())
            bot->Dismount();

        SpellCastResult const res = bot->CastSpell(bot, spellId, false, buff);
        LOG_INFO("playerbots", "Autopilotas {}: naudoja stiprinima {} (burtas {}), rezultatas {}", bot->GetName(), buff->GetEntry(), spellId,
                 uint32(res));
        ForceToWait(2500);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------- zonu kaita pagal lygi

namespace
{
    struct ZoneState
    {
        uint32 lastCheck = 0;
        uint32 lastMove = 0;
        uint8 lastMoveLevel = 0;
        uint32 movesThisLevel = 0;
    };
    std::unordered_map<ObjectGuid, ZoneState> g_zone;

    // Vidutinis priesiskos kilmes mobu lygis aplink veikeja (pagal tikras spawn vietas, 250 m spinduliu).
    float LocalHostileLevel(Player* bot, uint32& count)
    {
        count = 0;
        float sum = 0.0f;
        FactionTemplateEntry const* mine = bot->GetFactionTemplateEntry();
        if (!mine)
            return 0.0f;

        float const px = bot->GetPositionX();
        float const py = bot->GetPositionY();
        uint32 const mapId = bot->GetMapId();

        for (auto const& kv : sObjectMgr->GetAllCreatureData())
        {
            CreatureData const& d = kv.second;
            if (d.mapid != mapId)
                continue;
            float const dx = d.posX - px;
            float const dy = d.posY - py;
            if (dx * dx + dy * dy > 250.0f * 250.0f)
                continue;

            CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(d.id1);
            if (!ct || ct->rank == CREATURE_ELITE_WORLDBOSS || !ct->maxlevel)
                continue;
            FactionTemplateEntry const* f = sFactionTemplateStore.LookupEntry(ct->faction);
            if (!f || !mine->IsHostileTo(*f))
                continue;

            sum += 0.5f * float(ct->minlevel + ct->maxlevel);
            ++count;
        }
        return count ? sum / float(count) : 0.0f;
    }
}

bool NovaZoneNeedsMove(Player* bot, PlayerbotAI* botAI)
{
    if (!NovaAutopilotFree(bot, botAI) || bot->GetGroup())
        return false;

    uint32 const level = bot->GetLevel();
    if (level < 8)
        return false;

    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> guard(g_lock);
        ZoneState& zs = g_zone[bot->GetGUID()];
        if (zs.lastMoveLevel != level)
        {
            zs.lastMoveLevel = uint8(level);
            zs.movesThisLevel = 0;
        }
        if (zs.movesThisLevel >= 2 || (zs.lastMove && now - zs.lastMove < 15 * 60 * 1000))
            return false;
        if (zs.lastCheck && now - zs.lastCheck < 90 * 1000)
            return false;
        zs.lastCheck = now;
    }

    // pirma atiduodam ivykdytas uzduotis (jos gali buti seno krasto, bet atlygis vis tiek naudingas)
    uint32 q = 0;
    WorldPosition pos;
    if (NovaFindCompletedQuest(bot, botAI, q, pos))
        return false;

    uint32 count = 0;
    float const avg = LocalHostileLevel(bot, count);
    if (count < 15)
        return false;     // miestas / tuscia vieta: nesprendziam

    bool const tooLow = avg + 3.5f <= float(level);
    if (tooLow)
        LOG_INFO("playerbots", "Autopilotas {}: aplinkiniu priesu vid. lygis {:.1f} ({} mobu), veikejas {} lygio – keliamasi i tinkama vieta",
                 bot->GetName(), avg, count, level);
    return tooLow;
}

bool NovaZoneAction::Execute(Event /*event*/)
{
    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> guard(g_lock);
        ZoneState& zs = g_zone[bot->GetGUID()];
        zs.lastMove = now;
        zs.lastMoveLevel = uint8(bot->GetLevel());
        ++zs.movesThisLevel;
    }

    if (bot->IsMounted())
        bot->Dismount();

    ChatHandler(bot->GetSession()).SendSysMessage("|cff00ff00Autopilotas:|r čia jau per lengva tavo lygiui – keliamasi į tinkamesnę vietą.");
    sRandomPlayerbotMgr->RandomTeleportGrindForLevel(bot);
    ForceToWait(5000);
    return true;
}
