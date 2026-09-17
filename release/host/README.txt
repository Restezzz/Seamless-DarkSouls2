DS2 Seamless Co-op 0.2.0 - for the HOST (the player friends join)
==================================================================
(Русский текст - ниже)

INSTALL (once)
  1. Copy everything from this folder into the game folder:
       ...\steamapps\common\Dark Souls II Scholar of the First Sin\Game\
     (the folder with DarkSoulsII.exe). Replace dinput8.dll if Windows asks.
  2. Install Radmin VPN (radmin-vpn.com), create a network and give its name
     and password to your friends.

EVERY TIME
  1. Radmin VPN is on.
  2. Run StartServer.bat. The first time, Windows may ask about the firewall -
     allow it. The window shows your address for friends and where the key
     file is (ds2_server_public.key in the game folder). Send your friends that
     file and the address once.
  3. Start Dark Souls II from Steam and load your character.
  4. Press F1 (or Insert) -> "Host a lobby" -> any password -> "Create lobby".
     Tell the friend the password. Once they connect, the summon happens by
     itself.
  5. When you are done: StopServer.bat.

IF SOMETHING IS WRONG
  - The server window says it did not start: look at
    SeamlessServer\logs\server_out.txt.
  - The game says it is offline: start StartServer.bat before the game.
  - A friend is thrown out a minute or two after joining, or never summoned:
    F1 -> Lobby -> "Check the connection" while the friend is in the lobby.
    A VPN or proxy in TUN mode on either computer is the usual suspect.
  - A crash or a bug: keep ds2_seamless_coop.log and any
    ds2_seamless_crash_*.dmp from the game folder for the report.
  - To remove the mod, delete dinput8.dll from the game folder.

==================================================================
DS2 Seamless Co-op 0.2.0 - для ХОСТА (к нему подключаются друзья)
==================================================================

УСТАНОВКА (один раз)
  1. Скопируй всё из этой папки в папку игры:
       ...\steamapps\common\Dark Souls II Scholar of the First Sin\Game\
     (там, где лежит DarkSoulsII.exe). Если Windows спросит - заменить
     dinput8.dll.
  2. Поставь Radmin VPN (radmin-vpn.com), создай сеть и дай друзьям её
     название и пароль.

КАЖДЫЙ РАЗ
  1. Radmin VPN включён.
  2. Запусти StartServer.bat. В первый раз Windows может спросить про
     брандмауэр - разреши. Окно покажет адрес для друзей и где лежит файл-ключ
     (ds2_server_public.key в папке игры). Один раз отправь друзьям этот файл
     и адрес.
  3. Запусти Dark Souls II через Steam и загрузи персонажа.
  4. Нажми F1 (или Insert) -> «Создать лобби» -> любой пароль -> «Создать
     лобби». Скажи пароль другу. Когда он подключится, призыв произойдёт сам.
  5. Наигрались - StopServer.bat.

ЕСЛИ ЧТО-ТО НЕ ТАК
  - Окно сервера пишет, что он не запустился: смотри
    SeamlessServer\logs\server_out.txt.
  - Игра пишет «не в сети»: StartServer.bat надо запускать до игры.
  - Друга выкидывает через минуту-две после входа или не призывает: F1 ->
    Лобби -> «Проверить связь», пока друг в лобби. Обычный подозреваемый - VPN
    или прокси в режиме TUN на одном из компьютеров.
  - Вылет или ошибка: сохрани ds2_seamless_coop.log и файлы
    ds2_seamless_crash_*.dmp из папки игры - они нужны для разбора.
  - Удалить мод - удалить dinput8.dll из папки игры.
