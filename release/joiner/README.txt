DS2 Seamless Co-op 0.1.0 - for the player who JOINS a friend
=============================================================
(Русский текст - ниже)

INSTALL (once)
  1. Copy everything from this folder into the game folder:
       ...\steamapps\common\Dark Souls II Scholar of the First Sin\Game\
     (the folder with DarkSoulsII.exe). Replace dinput8.dll if Windows asks.
  2. Get two things from the host:
       - the file ds2_server_public.key - put it into the same game folder;
       - the host's Radmin VPN address (looks like 26.12.34.56).
  3. Open ds2_seamless_coop.ini in Notepad, write that address after
     server_ip= and save.

EVERY TIME
  1. Radmin VPN is on and you are in the host's network; the host has already
     run StartServer.bat.
  2. Start Dark Souls II from Steam and load your character.
  3. Press F1 (or Insert) -> "Join a friend" -> the host's address and the
     lobby password -> "Connect". Don't sit at a bonfire: your sign is placed
     and the host summons you by itself.

IF SOMETHING IS WRONG
  - The game says it is offline: the key file or server_ip does not match the
    host, or the host's server is not running.
  - A crash or a bug: send the host ds2_seamless_coop.log and any
    ds2_seamless_crash_*.dmp from the game folder.
  - To remove the mod, delete dinput8.dll from the game folder.

=============================================================
DS2 Seamless Co-op 0.1.0 - для того, кто ПОДКЛЮЧАЕТСЯ к другу
=============================================================

УСТАНОВКА (один раз)
  1. Скопируй всё из этой папки в папку игры:
       ...\steamapps\common\Dark Souls II Scholar of the First Sin\Game\
     (там, где лежит DarkSoulsII.exe). Если Windows спросит - заменить
     dinput8.dll.
  2. Возьми у хоста две вещи:
       - файл ds2_server_public.key - положи его в ту же папку игры;
       - адрес хоста в Radmin VPN (вида 26.12.34.56).
  3. Открой ds2_seamless_coop.ini Блокнотом, впиши этот адрес после
     server_ip= и сохрани.

КАЖДЫЙ РАЗ
  1. Radmin VPN включён, ты в сети хоста; хост уже запустил StartServer.bat.
  2. Запусти Dark Souls II через Steam и загрузи персонажа.
  3. Нажми F1 (или Insert) -> «Подключиться к другу» -> адрес хоста и пароль
     лобби -> «Подключиться». Не сиди у костра: знак поставится сам, и хост
     сам тебя призовёт.

ЕСЛИ ЧТО-ТО НЕ ТАК
  - Игра пишет «не в сети»: ключ или server_ip не совпадают с хостом, или у
    хоста не запущен сервер.
  - Вылет или ошибка: пришли хосту ds2_seamless_coop.log и файлы
    ds2_seamless_crash_*.dmp из папки игры.
  - Удалить мод - удалить dinput8.dll из папки игры.
