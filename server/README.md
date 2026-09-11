# Server / Сервер

The co-op server is [ds3os](https://github.com/TLeonardUK/ds3os) by Tim Leonard (MIT, see
[`LICENSE-ds3os.txt`](LICENSE-ds3os.txt)) with a few changes for seamless co-op. This folder keeps those
changes as a patch against a fixed upstream commit. The built server ships in
[`release/host/SeamlessServer`](../release/host/SeamlessServer).

Сервер кооператива — [ds3os](https://github.com/TLeonardUK/ds3os) (автор Tim Leonard, лицензия MIT, см.
[`LICENSE-ds3os.txt`](LICENSE-ds3os.txt)) с несколькими правками под seamless-кооп. Здесь эти правки лежат
патчем к зафиксированному коммиту ds3os. Собранный сервер лежит в
[`release/host/SeamlessServer`](../release/host/SeamlessServer).

## What the patch changes / Что меняет патч

- **Signs across areas.** The joiner's sign is offered to the host wherever the joiner stands. A summon or a
  refusal for a sign that is cached under another area is found by its id. A summon for a sign that is gone
  (the joiner's game restarted) goes to the same player's current sign.
  **Знаки из любых локаций.** Знак подключающегося предлагается хосту, где бы тот ни стоял. Призыв или отказ
  находят знак по номеру, даже если он записан в другой области. Призыв исчезнувшего знака (игра друга
  перезапустилась) переходит на текущий знак того же игрока.
- **Restarted games.** A player who logs in again (after a crash) has their old connection and its signs
  dropped at once, and player lookups always take the newest connection.
  **Перезапуск игры.** Игрок, вошедший заново (после вылета), сразу теряет старое соединение вместе с его
  знаками; поиск игрока всегда берёт самое новое соединение.
- **Lenient messages.** A message with a missing required protobuf field is still delivered instead of
  dropping the connection.
  **Мягкий разбор сообщений.** Сообщение без обязательного поля protobuf всё равно доставляется, а не рвёт
  соединение.

Files / Файлы: `DS2_SignManager.cpp`, `DS2_BootManager.cpp`, `GameService.cpp`,
`Frpg2ReliableUdpMessageStream.cpp`.

## Build / Сборка

Visual Studio 2022 (Desktop development with C++) and CMake 3.20+.

```bat
git clone https://github.com/TLeonardUK/ds3os.git
cd ds3os
git checkout 56406879a31505a71bea8c6290da370067f43659
git apply path\to\server\ds3os-seamless.patch
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target Server
```

The result is `bin\x64_release\Server.exe`; it replaces `release\host\SeamlessServer\Server.exe`.
Keep the rest of that folder (`WebUI`, `Saved\default\config.json`, `steam_api64.dll`, the scripts).

Результат — `bin\x64_release\Server.exe`; им заменяется `release\host\SeamlessServer\Server.exe`. Остальное в
той папке (`WebUI`, `Saved\default\config.json`, `steam_api64.dll`, скрипты) оставить как есть.
