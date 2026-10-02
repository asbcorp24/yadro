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

## RPLIDAR S2E (S2M1-R2E)

В REMOTION поддерживается **SLAMTEC RPLIDAR S2E / S2M1-R2E** — 360° 2D-лидар dToF с интерфейсом **Ethernet UDP 10/100M**. По данным SLAMTEC для S2E: дальность до 30 м на хорошо отражающей цели, частота сканирования 10 Гц, частота измерений 32000 точек/с, угловое разрешение 0.1125°. Эти характеристики относятся к самому датчику; параметры шага рассчитываются ядром REMOTION поверх полученных сканов.

Модуль проекта:

~~~text
src/rplidar_s2e.hpp
src/rplidar_s2e.cpp
~~~

Используется официальный SLAMTEC RPLIDAR SDK. Ethernet-канал создаётся через SDK как UDP channel.

### Заводские сетевые параметры S2E

Согласно официальному руководству SLAMTEC S2E, штатное подключение выполняется с параметрами:

~~~text
IP лидара:  192.168.11.2
UDP-порт:   8089
Протокол:   UDP
~~~

Это заводские/типовые параметры. Если IP устройства ранее менялся, нужно использовать фактический адрес конкретного лидара.

Для прямого подключения лидара к ПК Ethernet-картой ПК нужно находиться в той же подсети. Например:

~~~text
IP ПК:      192.168.11.5
Маска:      255.255.255.0
Шлюз:       не требуется
IP лидара:  192.168.11.2
UDP-порт:   8089
~~~

IP ПК не должен совпадать с IP лидара.

Проверка сетевой карты Windows:

~~~cmd
ipconfig
~~~

Базовая проверка доступности:

~~~cmd
ping 192.168.11.2
~~~

Если ping не проходит, сначала проверьте питание, Ethernet-кабель, IP сетевой карты ПК и принадлежность обоих устройств одной подсети. В документации SLAMTEC также рекомендуется проверять IP устройства и состояние сетевого интерфейса. Для ранних S2E при проблемах связи может потребоваться обновление прошивки.

### Проверка через SLAMTEC RoboStudio

Официальное руководство S2E рекомендует для ручного подключения:

~~~text
Connection mode: UDP Server
IP:              192.168.11.2
Port:            8089
~~~

После подключения нужно запустить сканирование и убедиться, что отображается облако/карта точек.

Это полезная независимая проверка перед диагностикой REMOTION: если S2E не работает в RoboStudio, проблема находится ниже уровня нашего приложения.

### Установка SLAMTEC SDK

Клонируйте SDK рядом с проектом:

~~~cmd
cd /d C:\dev
git clone https://github.com/Slamtec/rplidar_sdk.git
~~~

Ожидаемая структура:

~~~text
C:\dev\
  yadro\
  rplidar_sdk\
    sdk\
      include\
      src\
~~~

Проверка:

~~~cmd
dir C:\dev\rplidar_sdk\sdk\include\sl_lidar.h
~~~

### Сборка REMOTION + CEF + RPLIDAR на Windows

Полную REMOTION-сборку с CEF и RPLIDAR нужно выполнять **MSVC x64 + Ninja**, а не MSYS2.

Активируйте Visual Studio Build Tools:

~~~cmd
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
~~~

Проверьте:

~~~cmd
where cl
where ninja
cmake --version
~~~

Затем:

~~~cmd
cd /d C:\dev\yadro

set "CEF_ROOT=C:\dev\cef_binary_150.0.10+g8042e43+chromium-150.0.7871.101_windows64"
set "RPLIDAR_SDK_ROOT=C:\dev\rplidar_sdk"

rmdir /s /q build-cef

cmake -S . -B build-cef -G Ninja -DCMAKE_BUILD_TYPE=Release -DYADRO_WITH_CEF=ON -DCEF_ROOT="%CEF_ROOT%" -DYADRO_WITH_RPLIDAR_S2E=ON -DRPLIDAR_SDK_ROOT="%RPLIDAR_SDK_ROOT%"

cmake --build build-cef --target remotion -j 8
~~~

Запуск:

~~~cmd
build-cef\remotion.exe
~~~

В CMake для SDK на Windows согласован MSVC runtime с CEF (/MT), а также определяются требуемые upstream SDK макросы WIN32/WIN64.

### Где находятся настройки RPLIDAR в REMOTION

Путь:

~~~text
Настройки
→ Внешние устройства
→ RPLIDAR S2E
→ Настроить
~~~

Страница:

~~~text
data/static/lidar-settings.html
~~~

Здесь задаются:

- включение/выключение RPLIDAR;
- IP-адрес;
- UDP-порт;
- таймаут скана;
- длина и ширина полотна;
- положение лидара X/Y;
- угол установки лидара;
- передняя, задняя и боковые ограничительные зоны;
- число шагов прогноза;
- связность кластера;
- минимальное количество точек кластера;
- целевая длина шага;
- шаг рекомендации изменения скорости.

Для типового S2E сначала можно поставить:

~~~text
IP:        192.168.11.2
UDP-порт:  8089
~~~

Размеры полотна и положение лидара нельзя брать из примера — их нужно измерить на конкретной установке.

Настройки сохраняются в:

~~~text
data/settings.json
~~~

в секции:

~~~json
{
  "lidar": {
    "enabled": true,
    "ip": "192.168.11.2",
    "udp_port": 8089,
    "scan_timeout_ms": 500,
    "belt_length_m": 0.0,
    "belt_width_m": 0.0,
    "sensor_x_m": 0.0,
    "sensor_y_m": 0.0,
    "sensor_yaw_deg": 0.0,
    "front_margin_m": 0.30,
    "rear_margin_m": 0.30,
    "side_margin_m": 0.10,
    "prediction_steps": 5,
    "cluster_link_m": 0.12,
    "min_cluster_points": 3,
    "target_step_length_m": 0.0,
    "speed_correction_step_kmh": 0.2,
    "actuation_enabled": false
  }
}
~~~

### Проверка подключения в REMOTION

На странице настроек отображаются отдельно:

~~~text
SDK
Модуль
Ethernet/UDP
Сканирование
Модель
Серийный номер
Прошивка
Health
Последний скан
Ошибка
~~~

Наличие Ethernet/UDP-связи и наличие реального потока сканов — разные состояния. Для нормальной работы нужны одновременно:

~~~text
Ethernet/UDP: связь с устройством есть
Сканирование: идёт — данные поступают
~~~

### Карта точек лидара

В настройках есть отдельная страница:

~~~text
Настройки
→ Внешние устройства
→ RPLIDAR S2E
→ Карта точек лидара
~~~

Файл:

~~~text
data/static/lidar-points.html
~~~

Карта показывает **реальный последний 360° скан**, полученный C++ модулем через SLAMTEC SDK:

- точки скана;
- центр лидара;
- направление 0°;
- концентрические окружности расстояния;
- номер скана;
- число точек;
- возраст последнего скана;
- состояние связи;
- состояние сканирования;
- регулируемый радиус отображения.

REST endpoint карты:

~~~text
GET /api/v1/lidar/points
~~~

Пример ответа:

~~~json
{
  "ok": true,
  "scan_sequence": 123,
  "scan_age_ms": 42.0,
  "points": [
    {
      "angle_deg": 125.4,
      "distance_m": 2.83,
      "quality": 47
    }
  ]
}
~~~

### Рабочий режим лидара

Путь:

~~~text
Процедуры
→ Режим по лидару
~~~

Страница:

~~~text
data/static/lidar-mode.html
~~~

В рабочем режиме настройки подключения не дублируются. Он предназначен для отображения:

- статуса Ethernet/UDP;
- статуса сканирования;
- номера скана;
- сырых точек и точек после фильтрации;
- возраста последнего скана;
- положения левой/правой стопы;
- зон безопасности;
- текущей и предыдущей длины шага L/R;
- цикла шага L/R;
- Ct, Cx, Cy;
- рекомендации изменения скорости.

### Что делает C++ модуль

Текущая последовательность обработки:

~~~text
RPLIDAR S2E
    ↓ Ethernet UDP
SLAMTEC SDK
    ↓
полярный скан angle + distance + quality
    ↓
преобразование в X/Y
    ↓
привязка к системе координат полотна
    ↓
фильтрация рабочей области
    ↓
кластеризация точек
    ↓
левая / правая стопа
    ↓
трек стоп во времени
    ↓
длина шага / цикл
    ↓
Ct / Cx / Cy
    ↓
контроль ограничительных зон
    ↓
рекомендация коррекции скорости
~~~

Расчёт симметрии соответствует ТЗ:

~~~text
Ct = ((t_right - t_left) / (0.5 * (t_right + t_left))) * 100%
Cx = ((Sx_right - Sx_left) / (0.5 * (Sx_right + Sx_left))) * 100%
Cy = ((Sy_right - Sy_left) / (0.5 * (Sy_right + Sy_left))) * 100%
~~~

Чем ближе значение к 0%, тем меньше различие между сторонами по соответствующему параметру.

Ограничительные зоны по текущему ТЗ:

~~~text
спереди:       30 см
сзади:         30 см
справа/слева:  10 см
~~~

Эти значения доступны в настройках и должны проверяться на реальной установке.

### REST API RPLIDAR

~~~text
GET  /api/v1/lidar/status
GET  /api/v1/lidar/points
POST /api/v1/lidar/start
POST /api/v1/lidar/stop
POST /api/v1/settings       секция lidar
~~~

### Быстрая диагностика

Если в REMOTION нет данных:

1. Проверьте питание S2E и Ethernet.
2. Проверьте IP ПК через ipconfig.
3. Убедитесь, что ПК и S2E находятся в одной подсети.
4. Для заводской конфигурации проверьте 192.168.11.2:8089/UDP.
5. Проверьте лидар отдельно в SLAMTEC RoboStudio.
6. В REMOTION откройте настройки RPLIDAR и убедитесь, что SDK доступен.
7. Проверьте, что Ethernet/UDP показывает связь.
8. Проверьте, что «Сканирование» показывает поступление данных.
9. Откройте «Карта точек лидара» и убедитесь, что счётчик сканов и точек растёт.
10. Если связь есть, но сканов нет, нажмите «Перезапустить сканирование» и проверьте поле ошибки.

### Важные ограничения текущей реализации

Карта точек отображает реальные точки последнего скана, но алгоритм распознавания стоп и событий постановки ноги пока является первой реализацией и требует калибровки на реальной дорожке.

Параметры геометрии дорожки намеренно не зашиты в код: до ввода реальных размеров контур безопасности считается ненастроенным.

Автоматическая передача рекомендации скорости в реальную САУ полотна **не включена**. API может рассчитывать recommended_speed_delta_kmh, но REMOTION пока не передаёт эту рекомендацию на частотный преобразователь/ПЛК. Программный STOP также не заменяет физическую цепь аварийной остановки.

Официальные материалы SLAMTEC:

- RPLIDAR S2E Dev Kit User Manual;
- RPLIDAR S2E Datasheet;
- RPLIDAR SDK;
- SLAMTEC RoboStudio.


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
  rplidar_s2e.*       RPLIDAR S2E Ethernet/UDP, стопы, шаг и зона безопасности
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
- `GET /api/v1/lidar/status`
- `POST /api/v1/lidar/start`, `POST /api/v1/lidar/stop`

## Следующий аппаратный этап

Нужно определить интерфейс штатного контроллера дорожки: RS-232/RS-485/CAN/Ethernet, распиновку, скорость/формат кадров и команды пуска, скорости, уклона, направления, аварии и чтения телеметрии. После этого вместо `SimulationDriver` добавляется реальный драйвер, а web/API менять не потребуется.
