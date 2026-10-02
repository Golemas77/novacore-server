-- NovaCore: mod-personal-loot tekstai (lietuviškai). Taisomi čia arba `module_string` lentelėje, perkrovus serverį.

DELETE FROM `module_string` WHERE `module` = 'mod-personal-loot';
INSERT INTO `module_string` (`module`, `id`, `string`) VALUES
('mod-personal-loot', 1, 'Asmeninis grobis: kiekvienas grupės narys gauna savo grobį tiesiai į kuprinę, auksas dalijamas po lygiai. Išjungti grupei: .grobis isjungti'),
('mod-personal-loot', 4, 'Asmeninis grobis: kiekvienas mato ir gali paimti tik savo daiktus iš lavono. Paėmus daiktą, visa grupė mato, ką gavai, – galima susitarti dėl apsikeitimo (2 val.). Išjungti grupei: .grobis isjungti'),
('mod-personal-loot', 2, 'Asmeninis grobis: kuprinė pilna, daiktas {} išsiųstas paštu.'),
('mod-personal-loot', 3, 'Asmeninis grobis: kuprinė pilna, pilki daiktai parduoti už {}.'),
('mod-personal-loot', 10, 'Asmeninis grobis: serveryje {} ({}). Jūsų grupei: {}.'),
('mod-personal-loot', 11, 'įjungtas'),
('mod-personal-loot', 12, 'išjungtas'),
('mod-personal-loot', 13, 'tik požemiuose ir reiduose'),
('mod-personal-loot', 14, 'visur'),
('mod-personal-loot', 15, 'naudojamas'),
('mod-personal-loot', 16, 'išjungtas grupės vadovo'),
('mod-personal-loot', 17, 'nesate grupėje (grobis standartinis)'),
('mod-personal-loot', 18, 'Master Looter režimas (grobį dalija vadovas)'),
('mod-personal-loot', 19, 'Free-For-All režimas'),
('mod-personal-loot', 20, 'Grupės vadovas išjungė asmeninį grobį. Grobis dalijamas pagal grupės grobio režimą.'),
('mod-personal-loot', 21, 'Grupės vadovas įjungė asmeninį grobį: kiekvienas gauna savo grobį.'),
('mod-personal-loot', 22, 'Asmeninį grobį grupei gali keisti tik grupės vadovas.'),
('mod-personal-loot', 23, 'Jūs nesate grupėje.'),
('mod-personal-loot', 24, 'Komandos: .grobis isjungti | .grobis ijungti (tik grupės vadovas).');
