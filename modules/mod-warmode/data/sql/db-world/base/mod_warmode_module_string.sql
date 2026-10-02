-- NovaCore: mod-warmode tekstai (lietuviškai). Taisomi čia arba `module_string` lentelėje, perkrovus serverį.
SET @MODULE_STRING := 'mod-warmode';

DELETE FROM `module_string` WHERE `module` = @MODULE_STRING;
INSERT INTO `module_string` (`module`, `id`, `string`) VALUES
(@MODULE_STRING, 1, '|cffff8000=== Karo režimas ===|r'),
(@MODULE_STRING, 2, 'Karo režimas veikia panašiai kaip Retail. Kol jis įjungtas:'),
(@MODULE_STRING, 3, '- gauni {}% daugiau patirties už priešus ir užduotis;'),
(@MODULE_STRING, 4, '- esi visada PvP žymėtas ir gali kovoti su kitais karo režimo žaidėjais; žaidėjų, neįjungusių karo režimo (taip pat hardkoro), pulti negalima;'),
(@MODULE_STRING, 5, '- karo režimą įjungti ar išjungti galima tik pagrindiniuose miestuose ir ne kovoje (|cffffff00.warmode on|r / |cffffff00.warmode off|r).'),
(@MODULE_STRING, 6, 'Tavo karo režimas: |cff00ff00ĮJUNGTAS|r.'),
(@MODULE_STRING, 7, 'Tavo karo režimas: |cffaaaaaaišjungtas|r. Norėdamas įjungti, rašyk |cffffff00.warmode on|r arba pasikalbėk su NPC |cffffff00Karo vadas|r pagrindiniame mieste.'),
(@MODULE_STRING, 8, '|cffff8000[Karo režimas]|r Karo režimas |cff00ff00ĮJUNGTAS|r: +{}% patirties, o PvP galimas su kitais karo režimo žaidėjais.'),
(@MODULE_STRING, 9, '|cffff8000[Karo režimas]|r Karo režimas |cffaaaaaaIŠJUNGTAS|r.'),
(@MODULE_STRING, 10, 'Karo režimas jau įjungtas.'),
(@MODULE_STRING, 11, 'Karo režimas jau išjungtas.'),
(@MODULE_STRING, 12, 'Karo režimą galima įjungti ar išjungti tik pagrindiniuose miestuose.'),
(@MODULE_STRING, 13, 'Kovos metu karo režimo keisti negalima.'),
(@MODULE_STRING, 14, 'Hardkoro režime karo režimo įjungti negalima.'),
(@MODULE_STRING, 15, 'Karo režimas šiame serveryje išjungtas.'),
(@MODULE_STRING, 16, 'Botai karo režimo naudoti negali.'),
(@MODULE_STRING, 17, 'Karo režimą galima keisti tik būnant gyvam.'),
(@MODULE_STRING, 18, 'Karo režimo negalima keisti požemyje ar mūšio lauke.'),
(@MODULE_STRING, 19, '|cffff8000[Karo režimas]|r Įjungus karo režimą PvP žymės nuimti negalima – išjunk karo režimą pagrindiniame mieste.'),
(@MODULE_STRING, 20, '|cffff8000[Karo režimas]|r Tavo karo režimas įjungtas (+{}% patirties, PvP su kitais karo režimo žaidėjais).'),
(@MODULE_STRING, 21, 'Įjungti karo režimą'),
(@MODULE_STRING, 22, 'Išjungti karo režimą'),
(@MODULE_STRING, 23, 'Ne, ačiū'),
(@MODULE_STRING, 24, 'Gerai'),
(@MODULE_STRING, 25, 'Žaidėjui {} karo režimas {}.'),
(@MODULE_STRING, 26, 'Žaidėjas nerastas arba neprisijungęs.'),
(@MODULE_STRING, 27, 'įjungtas'),
(@MODULE_STRING, 28, 'išjungtas');
