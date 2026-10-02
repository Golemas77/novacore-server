-- NovaCore: mod-warmode NPC „Karo vadas“ (entry 9000002 - Aljanso išvaizda, 9000003 - Ordos), jo tekstai ir sukūrimo taškai pagrindiniuose miestuose.
DELETE FROM `creature_template` WHERE `entry` IN (9000002, 9000003);
INSERT INTO `creature_template` (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `scale`, `rank`, `dmgschool`, `DamageModifier`, `BaseAttackTime`, `RangeAttackTime`, `unit_class`, `unit_flags`, `unit_flags2`, `type`, `type_flags`, `AIName`, `MovementType`, `HealthModifier`, `ManaModifier`, `ArmorModifier`, `ExperienceModifier`, `RegenHealth`, `flags_extra`, `ScriptName`) VALUES
(9000002, 'Karo vadas', 'Karo režimas', 80, 80, 2, 35, 1, 1, 1.14286, 1, 0, 0, 1, 2000, 2000, 1, 768, 2048, 7, 0, '', 0, 1, 1, 1, 1, 1, 2, 'npc_warmode'),
(9000003, 'Karo vadas', 'Karo režimas', 80, 80, 2, 35, 1, 1, 1.14286, 1, 0, 0, 1, 2000, 2000, 1, 768, 2048, 7, 0, '', 0, 1, 1, 1, 1, 1, 2, 'npc_warmode');

DELETE FROM `creature_template_model` WHERE `CreatureID` IN (9000002, 9000003);
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(9000002, 0, 3167, 1, 1, 0),
(9000003, 0, 4259, 1, 1, 0);

-- NPC tekstai (gossip langas). $B = nauja eilutė.
DELETE FROM `npc_text` WHERE `ID` BETWEEN 9000010 AND 9000013;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `BroadcastTextID0`, `lang0`, `Probability0`) VALUES
(9000010, 'Sveikas, kovotojau! Aš esu Karo vadas.$B$BKol karo režimas įjungtas:$B- gausi 20% daugiau patirties už priešus ir užduotis;$B- būsi visada PvP žymėtas ir galėsi kovoti su kitais karo režimo žaidėjais;$B- žaidėjų, kurie karo režimo neįjungę (taip pat hardkoro žaidėjų), pulti negalima.$B$BKaro režimą gali įjungti ir išjungti tik pagrindiniuose miestuose.$B$BAr nori jį įjungti?', 'Sveika, kovotoja! Aš esu Karo vadas.$B$BKol karo režimas įjungtas:$B- gausi 20% daugiau patirties už priešus ir užduotis;$B- būsi visada PvP žymėta ir galėsi kovoti su kitais karo režimo žaidėjais;$B- žaidėjų, kurie karo režimo neįjungę (taip pat hardkoro žaidėjų), pulti negalima.$B$BKaro režimą gali įjungti ir išjungti tik pagrindiniuose miestuose.$B$BAr nori jį įjungti?', 0, 0, 1),
(9000011, 'Tavo karo režimas įjungtas. Kovok drąsiai! Jei nori jį išjungti, pasirink toliau.', 'Tavo karo režimas įjungtas. Kovok drąsiai! Jei nori jį išjungti, pasirink toliau.', 0, 0, 1),
(9000012, 'Tu esi hardkoro režime, todėl karo režimo įjungti negali. Hardkoras ir karo režimas nesuderinami.', 'Tu esi hardkoro režime, todėl karo režimo įjungti negali. Hardkoras ir karo režimas nesuderinami.', 0, 0, 1),
(9000013, 'Karo režimas šiame serveryje šiuo metu išjungtas.', 'Karo režimas šiame serveryje šiuo metu išjungtas.', 0, 0, 1);

DELETE FROM `creature` WHERE `id1` IN (9000002, 9000003);
INSERT INTO `creature` (`guid`, `id1`, `id2`, `id3`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `equipment_id`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`, `MovementType`, `npcflag`, `unit_flags`, `dynamicflags`, `ScriptName`, `VerifiedBuild`, `CreateObject`, `Comment`) VALUES
(9000101, 9000002, 0, 0, 0, 1519, 0, 1, 1, 0, -8834.4000, 631.6000, 94.3900, 5.0432, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Karo vadas: Stormwindas'),
(9000102, 9000002, 0, 0, 0, 1537, 0, 1, 1, 0, -4917.8000, -944.6000, 501.5200, 1.8231, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Karo vadas: Ironforgas'),
(9000103, 9000002, 0, 0, 1, 1657, 0, 1, 1, 0, 9949.6000, 2287.2000, 1341.4800, 4.6990, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Karo vadas: Darnasas'),
(9000104, 9000002, 0, 0, 530, 3557, 0, 1, 1, 0, -3965.2000, -11657.0000, -138.8400, 1.7168, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Karo vadas: Egzodaras'),
(9000105, 9000003, 0, 0, 1, 1637, 0, 1, 1, 0, 1629.8000, -4370.6000, 12.0600, 4.7288, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Karo vadas: Orgrimaras'),
(9000106, 9000003, 0, 0, 1, 1638, 0, 1, 1, 0, -1280.4000, 122.8000, 131.2600, 0.5834, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Karo vadas: Griausmo Uola'),
(9000107, 9000003, 0, 0, 0, 1497, 0, 1, 1, 0, 1588.0000, 240.3000, -52.1200, 3.1390, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Karo vadas: Požemių miestas'),
(9000108, 9000003, 0, 0, 530, 3487, 0, 1, 1, 0, 9490.7000, -7285.2000, 14.3900, 2.0358, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Karo vadas: Sidabrinis Mėnulis'),
(9000109, 9000002, 0, 0, 530, 3703, 0, 1, 1, 0, -1834.8465, 5297.7358, -12.3000, 1.7600, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Karo vadas: Šatratas'),
(9000110, 9000002, 0, 0, 571, 4395, 0, 1, 1, 0, 5809.5000, 633.3000, 647.6000, 2.1622, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0, 0, 'Karo vadas: Dalaranas');
