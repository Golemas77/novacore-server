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

// Uzduociu daiktu / burtu naudojimas ant uzduociu tiksliniu NPC ir objektu: pagydyti isgyvenusiuosius (Gift of the Naaru),
// panaudoti uzduociu daikta ant mobo / objekto, pasikalbeti su NPC. Parinktys keiciamos kas bandyma.
class NovaQuestUseAction : public NewRpgBaseAction
{
public:
    NovaQuestUseAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova quest use") {}
    bool Execute(Event event) override;
};

// Strand of the Ancients (SA): playerbots neturi sios kovos lauko taktikos – botai stovi vietoje. Atakuojantys eina prie esamu
// vartu ir juos „griauna“ (be vezimu, serveriu pusėje – ModifyHealth), po to ima relikvija; ginantys stoja uz tuo vartu.
class NovaSaAction : public NewRpgBaseAction
{
public:
    NovaSaAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova sa") {}
    bool Execute(Event event) override;
};

// Ar botas SA kovos lauke vyksta raundas (po pasiruosimo), jis gyvas ir ne kovoje.
bool NovaSaActive(Player* bot);

// Kuprines daiktai: (1) daiktai, kurie pradeda uzduoti (iskrite is mobu) – priimama iskart; (2) stiprinamieji daiktai (eliksyrai,
// flakonai, svitkai su ilgai trunkancia aura sau) – panaudojami, kai aura nenuimta.
class NovaItemsAction : public NewRpgBaseAction
{
public:
    NovaItemsAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova items") {}
    bool Execute(Event event) override;
};

bool NovaHasUsefulItems(Player* bot, PlayerbotAI* botAI);

// Amunicija: veikejas su lanku / arbaletu / ginklu (medziotojas, plesikas, karys) be strielu nemoka naudoti Auto Shot, Arcane
// Shot ir kitu nuotoliniu burtu (jie „IMPOSSIBLE“). Autopilotui visada palaikoma tinkama amunicija (600 vnt.) ir ekipuojama.
class NovaAmmoAction : public NewRpgBaseAction
{
public:
    NovaAmmoAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova ammo") {}
    bool Execute(Event event) override;
};

bool NovaNeedsAmmo(Player* bot, PlayerbotAI* botAI);

// Uzduociu daiktai, kuriu burtui reikia „spell focus“ objekto salia (pvz. „Medzio persirengimo rinkinys“ prie Nagu veliavos –
// uzduotis „Medzio draugija“): nueina prie objekto, panaudoja daikta ir laukia, kol ivyks ivykis ir bus ikeltas kreditas.
class NovaQuestFocusAction : public NewRpgBaseAction
{
public:
    NovaQuestFocusAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova quest focus") {}
    bool Execute(Event event) override;
};

bool NovaHasQuestFocus(Player* bot, PlayerbotAI* botAI);

// Zonu kaita pagal lygi: jei aplinkiniai priesai gerokai zemesnio lygio nei veikejas (pvz. 12 lygio veikejas starto zonoje), autopilotas
// persikelia i jo lygiui tinkama vieta (kaip atsitiktiniai botai, is „starter per level“ vietu). Kaupia kas ~90 s, ne daugiau 2 kartu per lygi.
class NovaZoneAction : public NewRpgBaseAction
{
public:
    NovaZoneAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova zone") {}
    bool Execute(Event event) override;
};

bool NovaZoneNeedsMove(Player* bot, PlayerbotAI* botAI);

// Uzstrigimas: jei autopilotas ~100 s be pertraukos bando eiti, bet pasislenka <30 m (urvas, siena, duobe) – naudoja Namu akmeni
// (arba, jei jo nera / jis ant atsigavimo, persikelia i lygiui tinkama vieta), o uzduotis, kurios taikinys buvo ten, palieka ramybeje.
class NovaUnstuckAction : public NewRpgBaseAction
{
public:
    NovaUnstuckAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova unstuck") {}
    bool Execute(Event event) override;
};

bool NovaIsStuck(Player* bot, PlayerbotAI* botAI);
void NovaNoteMoveAttempt(Player* bot, float x, float y, float z);

// Namai: atvykus i nauja zemyna (kita zemelapio dalis nei „namu“ vieta) nueina iki artimiausios smukles ir nusistato namus,
// kad Namu akmuo grazintu cia, o ne i kita zemyna.
class NovaHomeAction : public NewRpgBaseAction
{
public:
    NovaHomeAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova home") {}
    bool Execute(Event event) override;
};

bool NovaNeedsHome(Player* bot, PlayerbotAI* botAI);

// Aklavietė: jei ~6 min. nepasikeitė nei patirtis, nei pinigai, nei uzduociu eiga (veikejas vaiksto vienoje vietoje, uzduotys nebeivykdomos),
// autopilotas ISMETA nebaigtas uzduotis ir persikelia i lygiui tinkama vieta (kaip atsitiktiniai botai).
class NovaGridlockAction : public NewRpgBaseAction
{
public:
    NovaGridlockAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova gridlock") {}
    bool Execute(Event event) override;
};

bool NovaIsGridlocked(Player* bot, PlayerbotAI* botAI);

// Skrydziai: kai tikslas toli (>700 jardu tame paciame zemelapyje) ir skrydzio tinklas priartina, autopilotas nueina pas artima skrydzio
// meistra, skrenda (gali buti keli persedimai) ir toliau eina pesciomis, o ne begioja per visa zemelapi.
class NovaFlightAction : public NewRpgBaseAction
{
public:
    NovaFlightAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova flight") {}
    bool Execute(Event event) override;
};

bool NovaWantsFlight(Player* bot, PlayerbotAI* botAI);

// Augalai ir ruda: veikejas su zolininkyste / kasyba, pamates netoli (iki 45 jardu) renkama augala ar gysla, nueina ir surenka
// (standartinis „gather“ veiksmas su relevance 5 dingsta po autopilotu veiksmais, todel niekada nesuveikdavo).
class NovaGatherAction : public NewRpgBaseAction
{
public:
    NovaGatherAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "nova gather") {}
    bool Execute(Event event) override;
};

// Ar netoli yra renkamas augalas / ruda (pagal veikejo igudzius, igudzio lygi ir kasimo kirti).
bool NovaHasGatherNode(Player* bot, PlayerbotAI* botAI);

// Bendros salygos: autopilotas gyvas, ne kovoje, ne skrenda, ne teleportuojasi, ne musio lauke / pozemyje.
bool NovaAutopilotFree(Player* bot, PlayerbotAI* botAI);
// Pirma ivykdyta (nenutraukta) uzduotis, kuriai zinoma atidavimo vieta siame zemelapyje.
bool NovaFindCompletedQuest(Player* bot, PlayerbotAI* botAI, uint32& questId, WorldPosition& pos);
// Ar yra uzduociu su objektu tikslais.
bool NovaHasObjectQuests(Player* bot, PlayerbotAI* botAI);
// Ar netoli yra uzduoties tikslinis NPC / objektas, su kuriuo galima ka nors padaryti (zr. NovaQuestUseAction).
bool NovaHasQuestUseTarget(Player* bot, PlayerbotAI* botAI);
// Atidavimas „viskas is karto“: grazina true, kai laikas eiti atiduoti ivykdytas uzduotis – t. y. yra ka atiduoti IR (nebeliko
// darbo nebaigtoms uzduotims ARBA bot'as jau prie atidavejo ARBA 4 min. nera jokios pazangos ARBA zurnalas beveik pilnas). Prasidejus
// atidavimui jis tesiasi, kol nebeliks atiduotinu uzduociu (naujai priimtos uzduotys jo nenutraukia).
bool NovaTurnInGateOpen(Player* bot, PlayerbotAI* botAI);
// Ar yra nebaigtu uzduociu su moku tikslais (nukauti X arba surinkti daikta, krentanti is moku).
bool NovaHasKillQuests(Player* bot, PlayerbotAI* botAI);
// Laisvu kuprines vietu <= 25 % visos talpos (ne maziau kaip 3).
bool NovaLowBagSpace(Player* bot);
// Ar kuprine pilna ir jau laikas vykti pas pardavejus (isskaitant „atvesinimo“ pauze po nepavykusio pardavimo).
bool NovaNeedsVendor(Player* bot);

#endif
