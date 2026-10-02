-- NovaCore: mod-hardcore NPC „Hardkoras“ (entry 9000001) ir jo tekstai bei sukūrimo taškai visose 1 lygio pradžios zonose.
DELETE FROM `creature_template` WHERE `entry` = 9000001;
INSERT INTO `creature_template` (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `scale`, `rank`, `dmgschool`, `DamageModifier`, `BaseAttackTime`, `RangeAttackTime`, `unit_class`, `unit_flags`, `unit_flags2`, `type`, `type_flags`, `AIName`, `MovementType`, `HealthModifier`, `ManaModifier`, `ArmorModifier`, `ExperienceModifier`, `RegenHealth`, `flags_extra`, `ScriptName`) VALUES
(9000001, 'Hardkoras', 'Vienos gyvybės režimas', 80, 80, 2, 35, 1, 1, 1.14286, 1, 0, 0, 1, 2000, 2000, 1, 768, 2048, 7, 0, '', 0, 1, 1, 1, 1, 1, 2, 'npc_hardcore');

DELETE FROM `creature_template_model` WHERE `CreatureID` = 9000001;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(9000001, 0, 25444, 1, 1, 0);

-- NPC tekstai (gossip langas). $B = nauja eilutė.
DELETE FROM `npc_text` WHERE `ID` BETWEEN 9000001 AND 9000006;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `BroadcastTextID0`, `lang0`, `Probability0`) VALUES
(9000001, 'Sveikas, drąsuoli! Aš esu Hardkoras.$B$BPasirinkęs hardkoro režimą turėsi tik vieną gyvybę. Sąlygos:$B- negalėsi būti grupėje su kitais žaidėjais;$B- negalėsi įsijungti PvP ir pulti kitų žaidėjų;$B- negalėsi eiti į mūšio laukus;$B- negalėsi eiti į požemius;$B- negalėsi naudotis aukcionu;$B- patirties taškų reitas bus x1;$B- visi priešai turės 1,5 karto daugiau gyvybių ir darys 1,5 karto daugiau žalos.$B$BJei žūsi, hardkoro režimas bus panaikintas ir toliau žaisi įprastu režimu.$B$BJei pasieksi aukščiausią lygį gyvas, gausi pasiekimą, titulą ir jojamąjį protodrakoną, o hardkoro apribojimai bus nuimti.$B$BAr sutinki su šiomis sąlygomis?', 'Sveika, drąsuolė! Aš esu Hardkoras.$B$BPasirinkusi hardkoro režimą turėsi tik vieną gyvybę. Sąlygos:$B- negalėsi būti grupėje su kitais žaidėjais;$B- negalėsi įsijungti PvP ir pulti kitų žaidėjų;$B- negalėsi eiti į mūšio laukus;$B- negalėsi eiti į požemius;$B- negalėsi naudotis aukcionu;$B- patirties taškų reitas bus x1;$B- visi priešai turės 1,5 karto daugiau gyvybių ir darys 1,5 karto daugiau žalos.$B$BJei žūsi, hardkoro režimas bus panaikintas ir toliau žaisi įprastu režimu.$B$BJei pasieksi aukščiausią lygį gyvas, gausi pasiekimą, titulą ir jojamąjį protodrakoną, o hardkoro apribojimai bus nuimti.$B$BAr sutinki su šiomis sąlygomis?', 0, 0, 1),
(9000002, 'Tu jau esi hardkoro režime. Saugok savo vienintelę gyvybę!', 'Tu jau esi hardkoro režime. Saugok savo vienintelę gyvybę!', 0, 0, 1),
(9000003, 'Tu jau išbandei hardkorą ir žuvai. Dabar žaidi įprastu režimu – sėkmės tolesniuose nuotykiuose!', 'Tu jau išbandei hardkorą ir žuvai. Dabar žaidi įprastu režimu – sėkmės tolesniuose nuotykiuose!', 0, 0, 1),
(9000004, 'Hardkoro režimą gali pasirinkti tik ką sukurtas personažas, kuris dar negavo nė lašo patirties. Tu jau per daug pažengęs.', 'Hardkoro režimą gali pasirinkti tik ką sukurtas personažas, kuris dar negavo nė lašo patirties. Tu jau per daug pažengusi.', 0, 0, 1),
(9000005, 'Hardkoro režimas šiame serveryje šiuo metu išjungtas.', 'Hardkoro režimas šiame serveryje šiuo metu išjungtas.', 0, 0, 1),
(9000006, 'Tu įveikei hardkorą! Aukščiausią lygį pasiekei išsaugojęs vienintelę gyvybę – tai tikras žygdarbis. Titulas ir protodrakonas – tavo nuopelnas. Hardkoro apribojimai nuimti, sėkmės tolesniuose nuotykiuose!', 'Tu įveikei hardkorą! Aukščiausią lygį pasiekei išsaugojusi vienintelę gyvybę – tai tikras žygdarbis. Titulas ir protodrakonas – tavo nuopelnas. Hardkoro apribojimai nuimti, sėkmės tolesniuose nuotykiuose!', 0, 0, 1);

DELETE FROM `creature` WHERE `id1` = 9000001;
INSERT INTO `creature` (`guid`, `id1`, `id2`, `id3`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `equipment_id`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`, `MovementType`, `npcflag`, `unit_flags`, `dynamicflags`, `ScriptName`, `VerifiedBuild`, `CreateObject`, `Comment`) VALUES
(9000001, 9000001, 0, 0, 0, 12, 0, 1, 1, 0, -8955.95, -132.493, 83.8312, 0.0, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Hardkoras: Nortšyras (žmonės)'),
(9000002, 9000001, 0, 0, 1, 14, 0, 1, 1, 0, -618.518, -4245.67, 39.018, 4.7124, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Hardkoras: Išbandymų slėnis (orkai, troliai)'),
(9000003, 9000001, 0, 0, 0, 1, 0, 1, 1, 0, -6246.2863, 331.668, 383.058, 6.1772, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Hardkoras: Coldridge slėnis (nykštukai, gnomai)'),
(9000004, 9000001, 0, 0, 1, 141, 0, 1, 1, 0, 10308.6345, 837.8384, 1326.71, 5.1727, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Hardkoras: Šešėlių giria (naktiniai elfai)'),
(9000005, 9000001, 0, 0, 0, 85, 0, 1, 1, 0, 1679.2457, 1683.7478, 121.97, 4.2761, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Hardkoras: Deathknell (nemirėliai)'),
(9000006, 9000001, 0, 0, 1, 215, 0, 1, 1, 0, -2920.58, -252.7838, 53.2968, 5.236, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Hardkoras: Narache stovykla (taurenai)'),
(9000007, 9000001, 0, 0, 530, 3431, 0, 1, 1, 0, 10344.1806, -6354.7152, 33.7026, 5.8396, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Hardkoras: Sunstrider sala (kraujo elfai)'),
(9000008, 9000001, 0, 0, 530, 3526, 0, 1, 1, 0, -3958.6961, -13936.4281, 100.915, 2.0836, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Hardkoras: Ammen slėnis (draeneiai)');
