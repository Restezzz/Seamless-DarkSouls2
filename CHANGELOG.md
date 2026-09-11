# Changelog / История версий

Versions are tagged in git (`v0.1.0`, ...). Each tag can be downloaded as an archive from the
[Tags](https://github.com/Restezzz/Seamless-DarkSouls2/tags) page.

Каждая версия — тег в git (`v0.1.0`, ...). Архив любой версии скачивается со страницы
[Tags](https://github.com/Restezzz/Seamless-DarkSouls2/tags).

## 0.1.0 — 2026-09-12

First version. Tested by two players over Radmin VPN.
Первая версия. Проверена вдвоём через Radmin VPN.

**Added / Добавлено**

- Co-op through your own server (a patched ds3os) on the host's PC: no official servers, no strangers.
  Кооп через свой сервер (доработанный ds3os) на компьютере хоста: без официальных серверов и чужих людей.
- In-game menu (F1, Insert always works): host a lobby with a password or join a friend; English and Russian.
  Меню в игре (F1, Insert работает всегда): создать лобби с паролем или подключиться к другу; английский и русский.
- Automatic summon: the joiner's sign is placed by itself and the host summons it by itself; the sign appears
  right under the host's feet. No effigies, no soapstone ritual, the phantom timer never runs out.
  Автопризыв: знак подключающегося ставится сам, хост призывает сам, знак появляется у хоста под ногами. Без
  куколок и ритуала с мелками, таймер фантома не кончается.
- The session survives boss kills, deaths, bonfires and area changes.
  Сессия переживает убийство боссов, смерти, костры и смену локаций.
- No co-op fog walls between areas: the guest goes anywhere in the host's world.
  Нет кооп-тумана между локациями: гость ходит по миру хоста куда угодно.
- The guest can rest at bonfires in the host's world; resting respawns enemies for both.
  Гость может сидеть у костров в мире хоста; отдых воскрешает врагов у обоих.
- The guest loots items and chests in the host's world; the loot is theirs only and is gone from their own
  world afterwards (no duplicates).
  Гость собирает предметы и сундуки в мире хоста; добыча только его, и в его мире её потом нет (без дублей).
- Deaths: a guest who dies comes straight back to the host's world, at the bonfire they last rested at there;
  the host dying does not end the co-op; in a boss fight nobody comes back until the fight is decided.
  Смерть: погибший гость сразу возвращается в мир хоста, к костру, где отдыхал там последним; смерть хоста не
  рвёт кооп; на боссе никто не возвращается, пока бой не решится.
- The guest goes through a boss fog after the host.
  Гость проходит в туман босса вслед за хостом.
- A player whose game crashed can rejoin at once; the lobby replaces their old entry.
  Игрок, у которого вылетела игра, сразу может зайти снова; лобби заменяет его старую запись.
- Crash log: a crash leaves the place and the call stack in `ds2_seamless_coop.log` and a
  `ds2_seamless_crash_*.dmp` next to the game.
  Журнал вылетов: при вылете в `ds2_seamless_coop.log` остаются место и цепочка вызовов, а рядом с игрой —
  `ds2_seamless_crash_*.dmp`.
- Host package: `StartServer.bat` finds the Radmin VPN address, starts the server and puts the key file for
  friends into the game folder.
  Пакет хоста: `StartServer.bat` сам находит адрес Radmin VPN, запускает сервер и кладёт файл-ключ для друзей в
  папку игры.

**Known issues / Известные проблемы**

- Joining while standing where signs cannot be placed (Majula) does not work yet.
  Подключение, стоя там, где нельзя ставить знаки (Маджула), пока не работает.
- If both players die in a boss fight, the guest may come back at the boss instead of the bonfire.
  Если на боссе погибли оба, гость может вернуться к боссу, а не к костру.
- If the guest walks into a boss fog before the host, the boss stays idle.
  Если гость заходит в туман босса раньше хоста, босс стоит неактивным.
- The guest cannot talk to NPCs.
  Гость не может разговаривать с NPC.
- The host sees the guest as a white phantom.
  Хост видит гостя белым фантомом.
