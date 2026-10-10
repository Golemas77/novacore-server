/*
 * NovaCore: zaidejo automatines rotacijos (`.rotacija`) ne kovos priežiūros saugiklis.
 *
 * Ne kovos variklyje paliekama klases priežiūra (buffai, auros, augintiniai ir ju gebejimai, gydymas, prikelimas), bet
 * visi veiksmai, kurie zaidejo veikejui butu netiketi (daiktu keitimas, uzduociu valymas, slepimosi nuemimas, pozos
 * keitimas, joimas ir pan.), nuliniami. Priežiūra taip pat nevykdoma, kai zaidejas joja, skrenda, sedi, plauko, slepiasi
 * ar yra pasikeitęs i gyvuno forma – kad AI netrukdytų.
 */

#ifndef _PLAYERBOT_NOVAROTATIONGUARDSTRATEGY_H
#define _PLAYERBOT_NOVAROTATIONGUARDSTRATEGY_H

#include "Multiplier.h"
#include "Strategy.h"

class PlayerbotAI;

class NovaRotationGuardMultiplier : public Multiplier
{
public:
    NovaRotationGuardMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "nova rotation guard") {}

    float GetValue(Action* action) override;
};

class NovaRotationGuardStrategy : public Strategy
{
public:
    NovaRotationGuardStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    std::string const getName() override { return "nova guard"; }
};

#endif
