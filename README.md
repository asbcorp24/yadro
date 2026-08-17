# Yadro — ядро управления реабилитационной беговой дорожкой

Кроссплатформенное C++20 ядро управления дорожкой с web-интерфейсом, доступным по IP. Основная цель — отделить безопасность и управление оборудованием от интерфейса: HTML/JS можно менять независимо, а аппаратный протокол подключается отдельным драйвером.

## Что уже есть в v0.1

- C++ state machine: `stopped / running / stopping / emergency_stopped / fault`.
- Аппаратная абстракция `ITreadmillDriver` и безопасный `SimulationDriver`.
- Управление скоростью, уклоном, направлением, пуском, штатным и аварийным остановом.
- Контрольный watchdog: если web-пульт перестал присылать heartbeat, движение автоматически останавливается.
- Свободный бег, пользовательские интервальные профили и движок стандартных протоколов.
- Bruce Classic: 7 ступеней по 3 минуты, параметры перенесены с интерфейса исходной дорожки.
- Naughton/Balke/Ellestad/Cornell/Kattus/STEEP/Gardner заведены как заблокированные до верификации точных таблиц ступеней — ядро не будет придумывать параметры медицинских тестов.
- REST API, хранение пользовательских профилей и карточек пациентов в JSON.
- Все web-страницы и ресурсы находятся в `data/static/`.
- Опциональный `remotion.exe`: встроенный CEF/Chromium shell, который запускает то же C++ ядро и открывает локальный web-пульт как полноэкранное desktop-приложение.

> **Безопасность:** сейчас используется только симулятор. Не подключайте силовую часть дорожки к этому ПО, пока не будет реализован и испытан драйвер реального контроллера, физический аварийный стоп, аппаратный watchdog, концевики/датчики скорости и процедура верификации.

## Сборка Windows через MSYS2 UCRT64 — ядро без CEF

Откройте **MSYS2 UCRT64** и выполните:

```bash
pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja

git clone https://github.com/asbcorp24/yadro.git
cd yadro
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/yadro.exe
```

CMake сам скачает `cpp-httplib` и `nlohmann/json`, если они не установлены как системные пакеты.

Открыть на этом ПК: `http://127.0.0.1:8080/`. С другого устройства в локальной сети: `http://IP_КОМПЬЮТЕРА:8080/`.

## REMOTION desktop: CEF + cpp-httplib + fullscreen Chromium

CEF shell собирается отдельным target `remotion`. Обычный `yadro` остаётся доступен и продолжает собираться через MSYS2/Linux без CEF.

На Windows CEF-сборку делайте отдельным **MSVC / Visual Studio 2022** build. Скачайте официальную binary distribution CEF для Windows x64, распакуйте, например, в:

```text
C:\dev\cef_binary_windows64
```

Затем из Developer PowerShell for VS 2022:

```powershell
cd C:\dev\yadro

cmake -S . -B build-cef `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DYADRO_WITH_CEF=ON `
  -DCEF_ROOT=C:\dev\cef_binary_windows64

cmake --build build-cef --config Release --target remotion
```

CEF runtime-файлы (`libcef.dll`, ресурсы, locales и т.п.) копируются рядом с `remotion.exe` CMake-макросами из самой binary distribution.

Запуск:

```powershell
.\build-cef\Release\remotion.exe
```

По умолчанию программа:

1. создаёт `SimulationDriver` и C++ контроллер;
2. запускает встроенный `cpp-httplib` сервер на `0.0.0.0:8080`;
3. ждёт готовности `/api/v1/state`;
4. запускает CEF;
5. открывает `http://127.0.0.1:8080/account-select.html`;
6. разворачивает Chromium-окно без браузерной панели на весь основной монитор;
7. блокирует переходы CEF на внешние URL и отключает контекстное меню;
8. при закрытии Chromium останавливает HTTP-сервер и завершает приложение.

Полезные ключи:

```text
remotion.exe --windowed
remotion.exe --server-only
remotion.exe --port 8090
remotion.exe --start-page /index.html
remotion.exe --bind 127.0.0.1
```

Для рабочего монитора дорожки используется обычный запуск `remotion.exe`. Для отладки удобно `--windowed`. Для работы только как сетевого сервера — `--server-only`.

### Почему две сборки на Windows

`yadro.exe` по-прежнему можно собирать текущей цепочкой MSYS2/UCRT64. CEF shell вынесен в отдельный опциональный target, чтобы тяжёлая Chromium-зависимость не ломала существующую CI/сборку ядра. При `YADRO_WITH_CEF=ON` на Windows CMake требует MSVC.

## Linux

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/yadro
```

## Структура

```text
src/
  treadmill.*         state machine, safety, session engine, hardware abstraction
  protocols.*         verified standard protocols
  web_server.*        REST + static files
  main.cpp             core/server startup
  remotion_shell.cpp   optional CEF Chromium desktop shell

data/
  static/              HTML/CSS/JS web-пульт
  profiles.json        custom profiles
  patients.json        local patient list (prototype storage)
```

## REST API v1

- `GET /api/v1/health`, `GET /api/v1/state`
- `POST /api/v1/heartbeat`
- `POST /api/v1/control/targets`
- `POST /api/v1/control/direction`
- `POST /api/v1/control/start`
- `POST /api/v1/control/stop`
- `POST /api/v1/control/emergency-stop`
- `POST /api/v1/control/reset-emergency`
- `GET /api/v1/protocols`
- `GET/POST/DELETE /api/v1/profiles`
- `GET/POST /api/v1/patients`
- `POST /api/v1/simulation/heart-rate`

## Следующий аппаратный этап

Нужно определить интерфейс штатного контроллера дорожки: RS-232/RS-485/CAN/Ethernet, распиновку, скорость/формат кадров и команды пуска, скорости, уклона, направления, аварии и чтения телеметрии. После этого вместо `SimulationDriver` добавляется реальный драйвер, а web/API менять не потребуется.
