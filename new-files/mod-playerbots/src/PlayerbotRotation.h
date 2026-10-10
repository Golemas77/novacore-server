/*
 * NovaCore: zaidejo paties veikejo AI rezimai (komandos `.rotacija` ir `.autopilotas`).
 *
 * 1) `.rotacija` – kaip Retail „vieno mygtuko asistentas“: zaidejas pats pradeda kova (automatinis smugis ar pirmas burtas), o
 *    gebejimu seka (rotacija) toliau vykdoma automatiskai. Naudojamas jau esantis playerbots „self“ AI (ta pati logika,
 *    kuria juda botai pagal klase ir specializacija), bet apribotas:
 *      - ne kovos variklyje paliekama tik klases priežiūra (buffai, auros, augintiniai, gydymas; zr. NovaRotationGuardStrategy),
 *        mirties variklis ISVALYTAS (nesikalba, nerenka grobio, nepriima kvietimu, nesėda ant mounto ir pan.);
 *      - kovos variklyje pasalinti „chat“, „default“ (paketu reakcijos) ir „duel“;
 *      - pagal nutylėjima veikejas NEJUDA pats (judėjimo funkcijos MovementActions.cpp grazina false) – judeti ir tikslintis
 *        zaidejas turi pats. `.rotacija on judeti` leidzia ir judeti (pilnas autopilotas kovoje);
 *      - kovos varikli perjungia PreAction pagal tikra kovos busena, o taikini parenka automatiskai.
 *    Veikia ir musio laukuose bei arenose (BG / arenos strategijos pasalinamos, AI niekur pats nebėga); kovos varikli AI
 *    ijungia is karto, kai zaidejas pradeda kova (automatinis smugis ar burtas i prieso taikini), nelaukdamas kovos zymos.
 *
 * 2) `.autopilotas` – VISAS „playerbot“ veikejui (kaip random botas): kelia lygi (kovos + uzduotys + grind), renka profesiju
 *    medziagas, parduoda daiktus pas pardavejus, deda brangesnius i aukciona ir perka is aukciono pagerinimus, jungiasi i musio
 *    laukus ir pozemius. Veikejas lieka prisijunges per klienta; klientui atimama valdymo teise (SMSG_CLIENT_CONTROL_UPDATE),
 *    kad serverio varomas judejimas nesikirstu su kliento. `.autopilotas off` grazina valdyma.
 *    Nenaudojamas globalus „IsRandomBot“ – tik atskiros vietos (zr. NovaIsAutopilot), kad botu valdytojas (Randomize ir pan.)
 *    niekada neliestu zaidejo veikejo.
 *
 * Konfigas: AiPlayerbot.NovaRotationLevel, AiPlayerbot.NovaAutopilotLevel (0 = isjungta, 1 = tik GM, 2 = visi; numatyta 1).
 * Zaidime NEISBANDYTA (tik sukompiliuota).
 */

#ifndef _PLAYERBOT_ROTATION_H
#define _PLAYERBOT_ROTATION_H

#include "ObjectGuid.h"

#include <atomic>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class PlayerbotAI;

class PlayerbotRotationMgr
{
public:
    static PlayerbotRotationMgr* instance();

    // Jungia / isjungia rotacija. `reply` – zinute zaidejui (lietuviskai). Grazina true, jei busena pasikeite ar patvirtinta.
    bool Enable(Player* player, bool allowMove, std::string& reply);
    bool Disable(Player* player, std::string& reply);

    // Pilnas autopilotas (visas playerbot). Disable() isjungia bet kuri rezima.
    bool EnableAutopilot(Player* player, std::string& reply);

    bool IsEnabled(ObjectGuid guid) const;
    // Rotacija ijungta ir judeti pati AI neleidziama.
    bool IsStationary(ObjectGuid guid) const;
    // Pilnas autopilotas ijungtas.
    bool IsAutopilot(ObjectGuid guid) const;
    std::vector<ObjectGuid> AutopilotPlayers() const;

    // Pritaiko apribojimus AI strategijoms (kvieciama po Enable ir po kiekvieno PlayerbotAI::ResetStrategies).
    void Apply(PlayerbotAI* botAI);

    void Update(uint32 diff);

    // Kvieciama PRIES kiekviena AI veiksma: zaidejo veikejo AI niekada pats neperjungia variklio i kovos (tai darydavo
    // ne kovos variklio „dps assist“, kuri mes isvalem), todel perjungiam cia pagal tikra kovos busena.
    void PreAction(PlayerbotAI* botAI);

    // Diagnostika zaidejui (`.rotacija info`, `.autopilotas info`).
    std::string Describe(Player* player);

private:
    struct State
    {
        bool allowMove = false;
        bool autopilot = false;
        bool inBg = false;          // paskutinė žinoma būsena: mūšio lauke / arenoje (pasikeitus perkraunamos strategijos)
        ObjectGuid lastChoice;      // paskutinis žaidėjo pasirinktas taikinys (auto smūgis / pažymėtas), kad AI jį sektų
    };

    void ApplyAutopilot(PlayerbotAI* botAI, Player* bot);
    void SetupAutopilotCharacter(Player* player, PlayerbotAI* botAI);
    bool AttachAI(Player* player, std::string& reply, PlayerbotAI*& botAI);

    mutable std::shared_mutex _lock;
    std::unordered_map<ObjectGuid, State> _states;
    std::atomic<uint32> _autopilotCount{0};
    uint32 _timer = 0;
};

#define sPlayerbotRotationMgr PlayerbotRotationMgr::instance()

// MovementActions.cpp: ar sitam AI draudziama pačiam judeti (zaidejo rotacijos rezimas be judejimo).
bool NovaRotationBlocksMovement(PlayerbotAI* botAI);

// Ar sis veikejas yra zaidejo pilnas autopilotas (greita patikra: kol niekas nejungė – tik vienas atominis skaitiklis).
bool NovaIsAutopilot(Player* bot);

// Kritimo zalos apsauga autopiloto veikejui (kvieciama kiekviena zaidejo atnaujinima – Playerbots.cpp OnPlayerAfterUpdate).
void NovaFallGuard(Player* player);

// PlayerbotFactory mokina klases burtus kaip „laikinus“ (neissaugomus DB – botai juos is naujo issimoko kiekviena karta).
// Zaidejo autopilotui juos reikia palikti: NovaCollectTemporarySpells paima dabartini sarasa, o
// NovaPersistNewTemporarySpells po maintenance paverčia naujus laikinus burtus tikrais (issaugomais).
void NovaCollectTemporarySpells(Player* bot, std::unordered_set<uint32>& out);
void NovaPersistNewTemporarySpells(Player* bot, std::unordered_set<uint32> const& before);

void AddPlayerbotRotationScripts();

#endif
