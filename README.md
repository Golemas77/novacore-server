# NovaCore: serverio šaltinio kodas

**NovaCore** yra nepelno entuziastų lietuviškas „World of Warcraft“ (Ličo Karaliaus rūstybė, 3.3.5a) serveris.
Svetainė: <https://novacore-site.vercel.app/> · Discord: <https://discord.gg/fsMXA7vY>

Šioje saugykloje yra **mūsų pakeitimai ir mūsų sukurti moduliai**. Serveris paremtas
[AzerothCore](https://www.azerothcore.org/) su „Playerbots“ šaka, todėl visas darbas skelbiamas pagal tą pačią
**GNU Affero General Public License v3.0** licenciją (failas [`LICENSE`](LICENSE)). Pagal AGPLv3 13 skyrių kiekvienas,
kuris naudojasi mūsų serveriu per tinklą, turi teisę gauti šį pakeistą šaltinį: štai jis.

## Pagrindas (upstream)

Šie pakeitimai taikomi tiksliai šioms versijoms:

| Dalis | Saugykla | Versija |
| --- | --- | --- |
| Šerdis | <https://github.com/liyunfan1223/azerothcore-wotlk> (šaka `Playerbot`) | `451498079` |
| mod-playerbots | <https://github.com/liyunfan1223/mod-playerbots> | `2dad8bf0` |
| mod-aoe-loot | <https://github.com/azerothcore/mod-aoe-loot> | `b5c6635` |
| mod-ah-bot | <https://github.com/azerothcore/mod-ah-bot> | `80b08a5` (nekeista) |

## Kas yra šioje saugykloje

```
patches/
  azerothcore-core-novacore.patch      šerdies pakeitimai (git apply)
  mod-playerbots-novacore.patch        playerbots modulio pakeitimai
  mod-aoe-loot-novacore.patch          aoe-loot suderinimas su šia šerdimi
new-files/
  mod-playerbots/...                   nauji playerbots failai (botų pirkimas iš aukciono)
modules/
  mod-hardcore/  mod-warmode/  mod-personal-loot/  mod-transmog/  mod-gm-island/    mūsų moduliai (pilni)
LICENSE                                AGPL-3.0
```

### Šerdies pakeitimai

* **NPC kalbų lyties žymės** (`CreatureTextMgr.*`): klientas NPC kalbose nesprendžia `$g vyr:mot;`, todėl serveris pats
  parenka formą pagal gavėjo lytį ir talpina paketus pagal kalbą ir lytį.
* **`ObjectMgr::GetModuleString`**: trūkstama modulio eilutė anksčiau grąžindavo netikrą `std::string*` ir sugriaudavo serverį;
  dabar grąžinama tikra statinė eilutė.

### Playerbots pakeitimai

* pakartotinis LFG teleportas, kad botai pasiektų požemį;
* botų pasisveikinimų / atsisveikinimų šnabždesių išjungimas (`AiPlayerbot.EnableGreetWhispers`);
* sistemos pranešimų prisijungiant išjungimas (`AiPlayerbot.LoginNotices`), numatytoji reikšmė lieka įjungta;
* botų pirkimas iš aukciono (nauji failai `PlayerbotAuctionMgr`, `AuctionShopping*`).

### Mūsų moduliai

| Modulis | Ką daro |
| --- | --- |
| `mod-hardcore` | Hardkoro režimas (viena gyvybė iki 80 lygio; apdovanojimas: pasiekimas, titulas, raitelis), pranešimas visiems pasiekus 80. |
| `mod-warmode` | Karo režimas (+20 % patirties, PvP tik tarp savanorių), NPC „Karo vadas“. |
| `mod-personal-loot` | Asmeninis grobis: kiekvienas grupės narys gauna savo grobį. |
| `mod-transmog` | Išvaizdos keitimas (NPC „Išvaizdos meistrė“), kolekcija visai paskyrai. |
| `mod-gm-island` | GM testų sala su NPC (daiktai, glifai, brangakmeniai, mokytojai) serverio tikrinimui. |

Kiekvieno modulio nustatymai yra `conf/*.conf.dist`, duomenų bazės lentelės ir turinys – `data/sql/`.

## Kaip sukompiliuoti

1. Atsisiųsk šerdį ir modulius **tiksliai tomis versijomis**, kurios nurodytos aukščiau:
   ```
   git clone -b Playerbot https://github.com/liyunfan1223/azerothcore-wotlk.git
   cd azerothcore-wotlk && git checkout 451498079
   git clone https://github.com/liyunfan1223/mod-playerbots.git modules/mod-playerbots   # git -C modules/mod-playerbots checkout 2dad8bf0
   git clone https://github.com/azerothcore/mod-aoe-loot.git modules/mod-aoe-loot         # git -C modules/mod-aoe-loot checkout b5c6635
   git clone https://github.com/azerothcore/mod-ah-bot.git modules/mod-ah-bot             # git -C modules/mod-ah-bot checkout 80b08a5
   ```
2. Pritaikyk pakeitimus (kelius pakeisk į savo):
   ```
   git apply  /kelias/iki/novacore-server/patches/azerothcore-core-novacore.patch
   git -C modules/mod-playerbots apply /kelias/iki/novacore-server/patches/mod-playerbots-novacore.patch
   git -C modules/mod-aoe-loot   apply /kelias/iki/novacore-server/patches/mod-aoe-loot-novacore.patch
   cp -r /kelias/iki/novacore-server/new-files/mod-playerbots/* modules/mod-playerbots/
   cp -r /kelias/iki/novacore-server/modules/* modules/
   ```
3. Kompiliuok pagal [AzerothCore dokumentaciją](https://www.azerothcore.org/wiki/) su statiniais moduliais (`-DMODULES=static`).

## Ko čia nėra

Kliento failų (jie priklauso „Blizzard Entertainment“ ir čia neplatinami), kliento priedų ir MPQ pataisų, duomenų bazės
vertimo duomenų, gyvų serverio nustatymų su slaptažodžiais. NovaCore nėra susijęs su „Blizzard Entertainment“.

---

## English summary

**NovaCore** is a non-profit Lithuanian-language WotLK 3.3.5a private server built on AzerothCore (Playerbots branch).
This repository publishes our modifications under **AGPL-3.0**, as required for a modified network service:
patches against the exact upstream commits listed above (`patches/`), new playerbots files (`new-files/`) and our own
modules (`modules/`: hardcore mode, war mode, personal loot, transmog, GM test island). See "Kaip sukompiliuoti" for the
build steps (clone upstream at the listed commits, `git apply` the patches, copy the modules, build with `-DMODULES=static`).
No client files, credentials or database dumps are included. We are not affiliated with Blizzard Entertainment.
