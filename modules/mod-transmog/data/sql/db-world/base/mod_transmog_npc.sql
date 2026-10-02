-- NovaCore: mod-transmog NPC „Išvaizdos meistrė“ (entry 9000004, modelis - Kirin Tor burtininkė 25947), tekstai ir sukūrimo taškai miestų centrinėse aikštėse.
DELETE FROM `creature_template` WHERE `entry` = 9000004;
INSERT INTO `creature_template` (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `scale`, `rank`, `dmgschool`, `DamageModifier`, `BaseAttackTime`, `RangeAttackTime`, `unit_class`, `unit_flags`, `unit_flags2`, `type`, `type_flags`, `AIName`, `MovementType`, `HealthModifier`, `ManaModifier`, `ArmorModifier`, `ExperienceModifier`, `RegenHealth`, `flags_extra`, `ScriptName`) VALUES
(9000004, 'Išvaizdos meistrė', 'Transmogrifikacija', 80, 80, 2, 35, 1, 1, 1.14286, 1, 0, 0, 1, 2000, 2000, 1, 768, 2048, 7, 0, '', 0, 1, 1, 1, 1, 1, 2, 'npc_transmog');

DELETE FROM `creature_template_model` WHERE `CreatureID` = 9000004;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(9000004, 0, 25947, 1, 1, 0);

-- NPC tekstai (gossip langas). $B = nauja eilutė.
DELETE FROM `npc_text` WHERE `ID` BETWEEN 9000020 AND 9000022;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `BroadcastTextID0`, `lang0`, `Probability0`) VALUES
(9000020, 'Sveikas! Aš galiu pakeisti tavo daiktų išvaizdą – jų savybės nepasikeis, pasikeis tik tai, kaip jie atrodo.$B$BPasirink lizdą. Išvaizdas atrakini užsidėdamas daiktus, taip pat gaudamas daiktus, kurie susiejami paėmus. Visos išvaizdos saugomos visai tavo paskyrai.$B$BUž kiekvieno lizdo pakeitimą imu mokestį – jo dydį matysi meniu ir prieš patvirtindamas; pašalinti transmogrifikaciją galima nemokamai.', 'Sveika! Aš galiu pakeisti tavo daiktų išvaizdą – jų savybės nepasikeis, pasikeis tik tai, kaip jie atrodo.$B$BPasirink lizdą. Išvaizdas atrakini užsidėdama daiktus, taip pat gaudama daiktus, kurie susiejami paėmus. Visos išvaizdos saugomos visai tavo paskyrai.$B$BUž kiekvieno lizdo pakeitimą imu mokestį – jo dydį matysi meniu ir prieš patvirtindama; pašalinti transmogrifikaciją galima nemokamai.', 0, 0, 1),
(9000021, 'Pasirink išvaizdą, kurią nori matyti ant šio daikto. Rodomos tik tos pačios rūšies daiktų išvaizdos, kurias tu ar tavo paskyra jau atrakino.$B$BGali ieškoti pagal pavadinimą.', 'Pasirink išvaizdą, kurią nori matyti ant šio daikto. Rodomos tik tos pačios rūšies daiktų išvaizdos, kurias tu ar tavo paskyra jau atrakino.$B$BGali ieškoti pagal pavadinimą.', 0, 0, 1),
(9000022, 'Transmogrifikacija šiame serveryje šiuo metu išjungta.', 'Transmogrifikacija šiame serveryje šiuo metu išjungta.', 0, 0, 1);

DELETE FROM `creature` WHERE `id1` = 9000004;
INSERT INTO `creature` (`guid`, `id1`, `id2`, `id3`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `equipment_id`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`, `MovementType`, `npcflag`, `unit_flags`, `dynamicflags`, `ScriptName`, `VerifiedBuild`, `CreateObject`, `Comment`) VALUES
(9000111, 9000004, 0, 0, 0, 1519, 0, 1, 1, 0, -8830.4000, 627.6000, 94.3100, 2.8088, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Išvaizdos meistrė: Stormwindas'),
(9000112, 9000004, 0, 0, 0, 1537, 0, 1, 1, 0, -4922.5000, -941.2000, 501.5700, 0.2149, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Išvaizdos meistrė: Ironforgas'),
(9000113, 9000004, 0, 0, 1, 1657, 0, 1, 1, 0, 9945.6000, 2286.2000, 1341.4800, 5.8175, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Išvaizdos meistrė: Darnasas'),
(9000114, 9000004, 0, 0, 530, 3557, 0, 1, 1, 0, -3964.1500, -11661.2500, -138.8200, 1.7707, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Išvaizdos meistrė: Egzodaras'),
(9000115, 9000004, 0, 0, 1, 1637, 0, 1, 1, 0, 1632.8000, -4374.6000, 12.0600, 2.8270, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Išvaizdos meistrė: Orgrimaras'),
(9000116, 9000004, 0, 0, 1, 1638, 0, 1, 1, 0, -1277.4000, 127.8000, 131.3200, 4.7224, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Išvaizdos meistrė: Griausmo Uola'),
(9000117, 9000004, 0, 0, 0, 1497, 0, 1, 1, 0, 1587.5000, 243.9000, -52.1000, 3.9601, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Išvaizdos meistrė: Požemių miestas'),
(9000118, 9000004, 0, 0, 530, 3487, 0, 1, 1, 0, 9487.7000, -7282.2000, 14.3800, 1.5741, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Išvaizdos meistrė: Sidabrinis Mėnulis'),
(9000119, 9000004, 0, 0, 530, 3703, 0, 1, 1, 0, -1835.2000, 5301.8000, -12.3200, 3.1450, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Išvaizdos meistrė: Šatratas'),
(9000120, 9000004, 0, 0, 571, 4395, 0, 1, 1, 0, 5815.5000, 641.0000, 647.8500, 3.2365, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Išvaizdos meistrė: Dalaranas');
