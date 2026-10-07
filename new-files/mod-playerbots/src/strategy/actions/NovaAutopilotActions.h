/*
 * NovaCore: zaidejo autopilotas – patikimi veiksmai, kurie veikia NEPRIKLAUSOMAI nuo atsitiktines „new rpg“ busenu masinos:
 *   - atiduoti ivykdytas uzduotis pas tikra uzduoties NPC;
 *   - uzduociu objektai (knygos, sventoves, skrynios su uzduoties daiktais);
 *   - pilna kuprine -> pas pardavejus parduoti sunka.
 * Aktyvuoja tik triggeriai (NovaAutopilotTriggers.h) ir tik veikejui, kuriam ijungtas `.autopilotas` (zr. PlayerbotRotation.h).
 */

#ifndef _PLAYERBOT_NOVAAUTOPILOTACTIONS_H
#define _PLAYERBOT_NOVAAUTOPILOTACTIONS_H

#include "NewRpgBaseAction.h"

class NovaTurnInAction : public NewRpgBaseAction
{
public:
    NovaTurnInAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova turn in") {}
    bool Execute(Event event) override;
};

class NovaQuestObjectsAction : public NewRpgBaseAction
{
public:
    NovaQuestObjectsAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova quest objects") {}
    bool Execute(Event event) override;

private:
    bool UseOrApproach(GameObject* go);
    bool WorkQuest(uint32 questId);
};

class NovaVendorTripAction : public NewRpgBaseAction
{
public:
    NovaVendorTripAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova vendor trip") {}
    bool Execute(Event event) override;
};

// Uzduociu mobai: kai matomu uzduociu moku nera – einam i artimiausia TIKRA ju spawn vieta (ne i quest_poi, kurio zemelapio
// centras daznai yra ant kalno virs urvo). Pakeliui priimamos / atiduodamos uzduotys is netoliese esanciu NPC.
class NovaQuestMobsAction : public NewRpgBaseAction
{
public:
    NovaQuestMobsAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova quest mobs") {}
    bool Execute(Event event) override;
};

// Bendros salygos: autopilotas gyvas, ne kovoje, ne skrenda, ne teleportuojasi, ne musio lauke / pozemyje.
bool NovaAutopilotFree(Player* bot, PlayerbotAI* botAI);
// Pirma ivykdyta (nenutraukta) uzduotis, kuriai zinoma atidavimo vieta siame zemelapyje.
bool NovaFindCompletedQuest(Player* bot, PlayerbotAI* botAI, uint32& questId, WorldPosition& pos);
// Ar yra uzduociu su objektu tikslais.
bool NovaHasObjectQuests(Player* bot, PlayerbotAI* botAI);
// Ar yra nebaigtu uzduociu su moku tikslais (nukauti X arba surinkti daikta, krentanti is moku).
bool NovaHasKillQuests(Player* bot, PlayerbotAI* botAI);
// Laisvu kuprines vietu <= 25 % visos talpos (ne maziau kaip 3).
bool NovaLowBagSpace(Player* bot);
// Ar kuprine pilna ir jau laikas vykti pas pardavejus (isskaitant „atvesinimo“ pauze po nepavykusio pardavimo).
bool NovaNeedsVendor(Player* bot);

#endif
