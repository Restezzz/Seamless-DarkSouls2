# Changelog / История версий

Every version is on the [Releases](https://github.com/Restezzz/Seamless-DarkSouls2/releases) page with two
archives: `Seamless-DS2-<version>-host.zip` for the host and `Seamless-DS2-<version>-joiner.zip` for friends.

Каждая версия лежит на странице [Releases](https://github.com/Restezzz/Seamless-DarkSouls2/releases) двумя
архивами: `Seamless-DS2-<версия>-host.zip` для хоста и `Seamless-DS2-<версия>-joiner.zip` для друзей.

## 0.1.3 — 2026-09-12

**Added / Добавлено**

- Shared progress, on by default (`flag_sync=on`). Bonfires, fog gates, bosses and picked-up items are all event
  flags, and they travel both ways now; on top of that the host hands a friend who joins everything it has
  already done, which the once-a-second diff by itself can never do — a guest joining after five bonfires were
  lit hears about none of them. This writes into the receiving player's save. That is the point of it, and it is
  why the whole flag table is written to `ds2_flags_backup.bin` before the first flag is ever applied, why only
  0→1 is ever sent or accepted, and why `flag_sync=off` still means nobody's save is touched.
  Общий прогресс, по умолчанию включён (`flag_sync=on`). Костры, туманы, боссы и подобранные предметы — это
  флаги событий, и теперь они идут в обе стороны; вдобавок хост отдаёт зашедшему другу всё, что уже сделал, —
  сам дифф раз в секунду этого не умеет в принципе: гость, зашедший после пяти зажжённых костров, не узнает ни
  об одном. Это запись в сейв второго игрока. В этом и смысл, и поэтому перед первым применением вся таблица
  флагов сохраняется в `ds2_flags_backup.bin`, поэтому шлётся и принимается только `0→1`, и поэтому
  `flag_sync=off` по-прежнему означает, что сейвы никто не трогает.

**Fixed / Исправлено**

- Majula, third attempt — and this time with the reason the first two changed nothing. The check that
  decides whether the game's own map origin can be trusted demanded two hand-measured maps at once. The
  query only answers for a map whose data is loaded, the first of those two almost never is, and the loop
  returned on it: trust stayed at "not checked yet" for every session, the game's origin was never used
  once, and the value stored on disk always won. Both players' logs from 12.09 contain not a single line
  from that check — that is the proof. It is checked per map now. On top of that, a stored origin now loses
  to one the other player measured just now: the host measured Majula correctly (its real origin is
  practically zero) and sent it every ten seconds, while the guest threw it away in favour of the wrong
  number in its own file. The host's stray summon sign in Majula goes with it — it was placed only because
  "this map has no origin".
  Маджула, попытка третья — и на этот раз с причиной, по которой первые две ничего не изменили. Проверка
  «можно ли доверять началу карты от самой игры» требовала сразу две карты, измеренные руками. Запрос
  отвечает только для карты, чьи данные загружены, первая из этих двух почти никогда не загружена, и цикл
  на ней выходил: доверие в каждой сессии оставалось «не проверено», ответ игры не использовался ни разу,
  и всегда побеждало значение с диска. В логах обеих сторон за 12.09 нет ни одной строки из этой
  проверки — это и есть доказательство. Теперь карты проверяются по одной. Вдобавок сохранённое начало
  карты уступает тому, которое партнёр измерил только что: хост измерял Маджулу правильно (её настоящее
  начало — практически ноль) и отправлял каждые десять секунд, а гость выбрасывал это в пользу неверного
  числа в своём файле. Заодно исчезает и лишний знак призыва под хостом в Маджуле — он ставился только
  потому, что «у этой карты нет начала».
- Tested the same day, and worth naming which half of that fix did the work: a guest summoned into Majula
  arrived 4 cm from the host — (4.34, 5.49, -17.75) against the host's (4.38, 5.49, -17.77) — and summoning
  there works. The game's own origin query still answers for neither hand-measured map, so it is not yet
  what supplies the number; what made the difference is that a stored origin no longer beats one the other
  player measured. The per-map check is in and now says out loud when it cannot check.
  Проверено в тот же день, и стоит назвать, какая именно половина правки сработала: гость, призванный в
  Маджулу, появился в 4 см от хоста — (4.34, 5.49, -17.75) против (4.38, 5.49, -17.77) — и призыв туда
  работает. Запрос начала карты у самой игры по-прежнему не отвечает ни про одну из карт, измеренных
  руками, то есть источником числа она так и не стала; разницу дало то, что сохранённое начало больше не
  побеждает измеренное вторым игроком. Проверка по одной карте на месте и теперь честно сообщает, когда
  проверить не удалось.
- The other player's bonfires are lit in this player's own set as well, when progress sharing is on. The
  session byte alone was not enough: all five of the host's bonfires reached the guest, 10670 sat there
  with its session byte set, and the travel menu still did not list it. The menu reads more than that
  byte; owning the bonfire is what it cannot argue with. Only the lit bit is set, never the kindle level.
  Костры второго игрока зажигаются и в собственном наборе, если общий прогресс включён. Одного
  сессионного байта не хватило: все пять костров хоста дошли до гостя, у 10670 сессионный байт стоял, а в
  меню перемещения костра всё равно не было. Меню читает не только этот байт; владение костром оспорить
  нечем. Ставится только бит «зажжён», уровень разжигания не трогается.
- Flag syncing had never sent a single flag, and two of its own guards were the reason. A group that reads empty
  while the game rebuilds the table used to abort the whole pass **before** the baseline was replaced, so after
  the first occurrence nothing was ever compared again — 994 such lines in the guest's log, 541 in the host's.
  And any pass with more than eight changed flags was discarded as implausible, which is one bonfire, one fog
  gate or one area load. Now only the empty group drops out of the diff, keeping the bytes it had, and the
  ceiling is 256 — safe, because the only thing ever sent is a bit this game's own table reads as set.
  Синхронизация флагов не отправила ни одного флага, и виноваты были две её же защиты. Группа, читающаяся
  пустой в момент пересборки таблицы, прерывала весь пасс **до** замены базовой линии, поэтому после первого
  раза сравнивать было уже не с чем — 994 такие строки в логе гостя, 541 в логе хоста. А любой пасс, где
  изменилось больше восьми флагов, выбрасывался как невозможный — это один костёр, один туман или загрузка
  области. Теперь из диффа выпадает только пустая группа, сохраняя свои прежние байты, а предел равен 256 — это
  безопасно, потому что отправляется только тот бит, который в нашей же таблице стоит единицей.

- Majula, and this time with the cause named. The origin that a sign's coordinates are measured against was
  taken from the mod's own file, where Majula sat as (10.53, 5.92, -16.25) — not an origin at all, but the
  position of whoever had placed a sign there. In Majula a sign only goes down through the mod's fallback spot,
  that spot carries zeroes, and "my position minus zero" is my position. The wrong number was then kept on disk
  and handed to the other player, so the guest kept being summoned off the map and dying on arrival. The game's
  own answer comes first now, and a stored value that disagrees with it is thrown away.
  Маджула, и на этот раз с названной причиной. Начало карты, относительно которого считаются координаты знака,
  брали из собственного файла мода, а там для Маджулы лежало (10.53, 5.92, -16.25) — это вообще не начало, а
  позиция того, кто ставил там знак. В Маджуле знак ставится только через подменённое модом место, координаты в
  нём нулевые, и «моя позиция минус ноль» даёт мою позицию. Неверное число сохранилось на диск и уехало второму
  игроку, поэтому гостя и продолжало выкидывать за карту со смертью при появлении. Теперь первым спрашивается
  сама игра, а расходящееся сохранённое значение выбрасывается.
- The host's bonfires go into the guest's travel list. The game syncs a session's bonfires by itself, but only
  for the map the players are in and at most sixteen of them: measured on 12.09, the host had five lit and the
  set the guest's list reads held two. All of them are sent now. The byte this writes is never saved, so nobody's
  own progress is touched.
  Костры хоста попадают в список перемещения гостя. Игра синхронизирует костры сессии сама, но только для той
  карты, где стоят игроки, и не больше шестнадцати: по замеру 12.09 у хоста было зажжено пять, а в наборе, из
  которого читает список гостя, лежало два. Теперь отправляются все. Байт, в который это пишется, никогда не
  сохраняется, поэтому чужой прогресс не затрагивается.

**Changed / Изменено**

- `npc_spawn` is off by default. The call site fires — 24 000 times in one session — and the NPCs stay exactly as
  missing and as see-through, so this is not what hides them, and it changes what the game does for nothing.
  `npc_spawn` по умолчанию выключен. Вызов срабатывает — 24 000 раз за сессию — и NPC остаются ровно так же
  пропавшими и прозрачными, значит скрывает их не это, а игра меняется впустую.
- `npc_talk` (forcing the "Talk" prompt open for a guest) is off by default. It never produced a single prompt,
  and the game crashed reading address 0 at exe+0x18B10E, called from exe+0x4534A6 — the destructor path of the
  prompt object itself, the code this reaches into. Not proven; that is precisely why it is off.
  `npc_talk` (принудительная подсказка «Поговорить» у гостя) по умолчанию выключен. Подсказка не появилась ни
  разу, а игра вылетела с чтением по адресу 0 в exe+0x18B10E из exe+0x4534A6 — это путь деструктора самого
  объекта подсказки, того кода, в который влезает эта правка. Не доказано — именно поэтому и выключено.

**Also in 0.1.3 / Также в 0.1.3**

- The boss fog lets a guest walk in again, the way it did in 0.1.0. Holding the guest back until the host was
  inside was my own change in 0.1.1, and it was the wrong call: a host standing at the fog waiting for the guest
  locked them both out. Waiting is now an option instead of the rule (`boss_fog_wait`, off by default).
  Туман босса снова пускает гостя, как в 0.1.0. Ожидание хоста я добавил в 0.1.1 сам, и это было неверное решение:
  если хост стоял у тумана и ждал гостя, запертыми оказывались оба. Теперь ожидание — необязательная настройка
  (`boss_fog_wait`, по умолчанию выключена).

- Where a map's sign origin comes from. It used to be measured by placing a sign in that map, which nobody had ever
  done in Majula, so joining there could not be aimed at all. The game keeps that origin in the map data itself
  (`exe+0x2A9E70`), and the mod reads it from there -- after checking, in the game, that it reproduces the two
  origins measured by hand.
  Откуда берётся начало карты для знака. Раньше его измеряли, поставив в этой карте знак, а в Маджуле этого никто
  никогда не делал, поэтому и прицелиться было нечем. У игры это начало лежит в данных самой карты
  (`exe+0x2A9E70`), и мод читает его оттуда — предварительно проверив в игре, что оно совпадает с двумя началами,
  измеренными руками.

**Being tested / Проверяется**

- NPCs for a guest. A world entered by a multiplayer warp never finishes putting its characters in, which is what
  left NPCs missing or see-through with nobody to talk to -- the talk prompt was never the problem. That step now
  runs for a guest too. It ships on, and **Delete** turns it off and back on in the game, so both states can be
  seen in one session; the log also reports every character the game takes back off the map.
  NPC у гостя. Мир, в который вошли мультиплеерным варпом, не досоздаёт своих персонажей — отсюда пропавшие или
  прозрачные NPC и то, что говорить не с кем; подсказка «Поговорить» была тут не при чём. Теперь этот шаг
  выполняется и у гостя. По умолчанию включено, а **Delete** выключает и включает это прямо в игре, чтобы увидеть
  оба состояния за один заход; ещё лог сообщает о каждом персонаже, которого игра снимает с карты.

## 0.1.2 — 2026-09-12

What the two players found in 0.1.1.
Что нашли в 0.1.1 два игрока.

**Fixed / Исправлено**

- Being summoned to the host in Majula killed the guest on arrival. A sign's coordinates are kept against the
  map's own origin, and the guest's sign was left in the frame of its own map, so the guest appeared off the map
  and died within seconds. A sign that cannot be aimed is no longer placed at all, and the origin of the map a
  player stands in is measured and sent to the other player, so any area works.
  Призыв к хосту в Маджуле убивал гостя сразу после появления. Координаты знака хранятся относительно начала
  карты, а знак гостя оставался в системе координат его собственной карты, поэтому гость появлялся вне карты и
  погибал за считаные секунды. Знак, который невозможно прицелить, больше не ставится вовсе, а начало карты, в
  которой стоит игрок, измеряется и передаётся второму игроку — теперь работает любая локация.
- The guest had to wait at the boss fog until the host had already started the fight, so a host waiting at the fog
  locked both players out. Whoever goes through now tells the other player, and that fog lets them follow at once.
  Гость ждал у тумана босса, пока хост не начнёт бой, поэтому хост, ждущий у тумана, запирал обоих. Теперь
  прошедший сквозь туман сообщает об этом второму игроку, и тот же туман сразу пропускает его следом.

**Changed / Изменено**

- The log no longer fills up with door states: two doors shared one slot of the "last state" table and wrote a
  line every frame — 35 000 lines in 13 minutes.
  Лог больше не забивается состояниями дверей: две двери делили одну ячейку таблицы «последнее состояние» и писали
  строку каждый кадр — 35 000 строк за 13 минут.
- `dinput8.dll` now carries its name and version in the file's properties, and no longer the local path of the
  machine it was built on. Anti-virus heuristics flag a file without version info more readily.
  У `dinput8.dll` теперь есть название и версия в свойствах файла, а локального пути сборочной машины внутри
  больше нет. Файлы без сведений о версии эвристики антивирусов помечают чаще.

**Still open / Ещё не сделано**

- NPCs are invisible or transparent for the guest, so there is nothing to talk to.
  У гостя NPC невидимы или прозрачны, поэтому говорить не с кем.
- The guest's fast-travel list does not include the host's bonfires.
  В списке перемещения у гостя нет костров хоста.
- The guest still cannot start a boss fight by itself; it can now follow the host in at once.
  Гость по-прежнему не может сам запустить бой с боссом; зато теперь сразу проходит за хостом.

## 0.1.1 — 2026-09-12

What two players found in 0.1.0.
Что нашли в 0.1.0 два игрока.

**Fixed / Исправлено**

- Joining from Majula and other areas without multiplayer. The game held multiplayer "busy" there and turned
  every summon down; after a save loaded straight into Majula it also found no spot for the sign at all. Both are
  worked around while the co-op lobby is up.
  Вход из Маджулы и других областей без мультиплеера. Игра держала там мультиплеер «занятым» и отклоняла любой
  призыв, а после загрузки сохранения прямо в Маджуле ещё и не находила места для знака. Пока открыто лобби, и то
  и другое обходится.
- The guest can talk to NPCs in the host's world (the "Talk" prompt turned every phantom down).
  Гость может разговаривать с NPC в мире хоста (подсказка «Поговорить» отказывала любому фантому).
- Both players dead in a boss fight: the guest waits until the host is up at the bonfire and comes back there,
  not at the boss.
  Если на боссе погибли оба, гость ждёт, пока хост встанет у костра, и возвращается туда, а не к боссу.
- "Nobody comes back during a boss fight" works for the guest too: the host tells the guest that a fight is on.
  «Никто не возвращается, пока идёт бой» работает и у гостя: хост сообщает гостю, что бой идёт.
- The guest's return spots (last rest, arrival) are stamped with the right map, so they are actually used.
  Точки возврата гостя (последний отдых, место прибытия) помечаются правильной картой и теперь используются.
- A leftover "disconnect" from the host's previous lobby no longer turns the next join down as "wrong password".
  Оставшийся «отключиться» от прошлого лобби хоста больше не отбивает новый вход как «неверный пароль».

**Changed / Изменено**

- The boss fog stays closed for the guest until the host is inside: for now only the host starts a boss fight,
  and a guest who went in first found the boss idle.
  Туман босса закрыт для гостя, пока не зайдёт хост: бой пока запускает только хост, а гость, зашедший первым,
  находил босса неактивным.
- The "busy" message no longer blames the bonfire when something else holds the game busy.
  Сообщение «занято» больше не винит костёр, если игру держит что-то другое.

**Known issues / Известные проблемы**

- The guest can't start a boss fight: the fog lets the guest in once the host is inside.
  Гость не может сам запустить бой с боссом: туман пропускает его, когда хост уже внутри.
- NPC dialogue and shop progress may not stick for the guest.
  Прогресс диалогов и магазинов у NPC может не сохраняться у гостя.
- The host sees the guest as a white phantom.
  Хост видит гостя белым фантомом.

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
