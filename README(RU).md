# AudioRouter

**Профессиональная программа маршрутизации звука для Windows 10/11**

Visual audio patch bay with WASAPI + ASIO support, virtual device routing,
and a node-based connection graph.

---

## Возможности

| Функция | Статус |
|---|---|
| Захват с любого WASAPI-устройства | ✅ |
| Loopback (захват системного звука) | ✅ |
| Маршрутизация в любое устройство вывода | ✅ |
| Несколько источников → один выход (микшер) | ✅ |
| Полная поддержка ASIO (любые ASIO-драйверы) | ✅ |
| Виртуальные устройства (VB-Audio, VB-Cable) | ✅ (через VB-Audio) |
| Визуальная схема подключений | ✅ |
| Регулировка уровней (dB) | ✅ |
| Индикаторы уровня на каждом узле | ✅ |
| Сохранение/загрузка схемы (.patch) | ✅ |

---

## Требования для сборки

- **Visual Studio 2022** (или 2019) с компонентом C++ Desktop Development
- **CMake 3.20+**
- **Git** (для FetchContent)
- Windows SDK 10.0.19041+

---

## Сборка

```bat
git clone <этот репозиторий>
cd AudioRouter

cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Готовый файл: `build/bin/Release/AudioRouter.exe`

---

## Виртуальные устройства (Virtual Microphone / Virtual Speaker)

Чтобы программа появлялась в списке устройств Windows как **микрофон** или **динамик**,
нужен виртуальный аудио-драйвер. Рекомендуем **VB-Audio Virtual Cable** (бесплатно):

1. Скачать: https://vb-audio.com/Cable/
2. Установить VB-Cable (появится устройство `CABLE Input` и `CABLE Output`)
3. В AudioRouter эти устройства будут отображены как **фиолетовые** узлы `[VIRT]`
4. Подключите источник к `CABLE Input` — другие программы смогут выбрать `CABLE Output` как микрофон

---

## ASIO

1. Установите любой ASIO-драйвер (ASIO4ALL, нативный от производителя звуковой карты и т.д.)
2. В AudioRouter: меню **ASIO → ASIO Settings**
3. Выберите драйвер → **Load Driver**
4. В граф добавятся два узла: `[ASIO IN]` и `[ASIO OUT]`
5. Соедините их с WASAPI-устройствами по своему усмотрению

---

## Использование

```
Левая панель  →  список обнаруженных устройств
                 Двойной клик = добавить в граф

Центр         →  схема подключений (node editor)
                 Тащите от выхода узла к входу другого
                 Правый клик = удалить узел

Правая панель →  свойства выбранного узла (имя, усиление, пики)

Engine → Start Routing  =  запустить маршрутизацию
Engine → Stop  Routing  =  остановить
File → Save patch        =  сохранить схему в AudioRouter.patch
```

---

## Структура проекта

```
src/
  main.cpp              Win32 + D3D11 + ImGui entry point
  AudioEngine.h/cpp     Ядро маршрутизации, mix-поток
  WasapiManager.h/cpp   WASAPI capture / render / loopback
  AsioManager.h/cpp     ASIO драйвер (COM-загрузка)
  RoutingGraph.h/cpp    Граф узлов и рёбер
  UI/
    NodeEditorUI.h/cpp  Визуальный редактор схемы (ImGui + imnodes)
thirdparty/
  asio/asio.h           ASIO SDK-совместимые типы (встроены)
```

---

## Лицензия

MIT — используйте свободно в личных и коммерческих проектах.
