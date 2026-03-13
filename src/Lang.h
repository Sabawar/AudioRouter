#pragma once
#include <string>

// ============================================================================
//  Локализация / Localization
//  Использование: UI::T(Str::FILE_MENU)  →  "Файл" или "File"
// ============================================================================

enum class Lang { RU, EN };

// ── Все строки интерфейса ────────────────────────────────────────────────────
enum class Str {
    // Меню File
    MENU_FILE,
    MENU_FILE_NEW,
    MENU_FILE_SAVE,
    MENU_FILE_LOAD,
    MENU_FILE_EXIT,
    // Меню Engine
    MENU_ENGINE,
    MENU_ENGINE_START,
    MENU_ENGINE_STOP,
    MENU_ENGINE_REFRESH,
    // Меню ASIO
    MENU_ASIO,
    MENU_ASIO_SETTINGS,
    MENU_ASIO_PANEL,
    MENU_ASIO_UNLOAD,
    // Меню Add
    MENU_ADD,
    MENU_ADD_MIXER,
    // Меню View
    MENU_VIEW_VOLUME,
    MENU_VIEW_LOG,
    // Статусы
    STATUS_ROUTING_ACTIVE,
    STATUS_ROUTING_STOPPED,
    // Панель устройств
    PANEL_DEVICES,
    PANEL_DEVICES_HINT,
    PANEL_MICROPHONES,
    PANEL_LOOPBACK,
    PANEL_SPEAKERS,
    // Граф
    GRAPH_TITLE,
    GRAPH_HINT,
    // Свойства
    PROPS_TITLE,
    PROPS_HINT,
    PROPS_ID,
    PROPS_TYPE,
    PROPS_GAIN,
    PROPS_ENABLED,
    PROPS_PEAK_L,
    PROPS_PEAK_R,
    PROPS_DELETE,
    // Volume Mixer
    MIXER_TITLE,
    MIXER_RMS,
    MIXER_HOLD,
    MIXER_MIN_DB,
    MIXER_APP_SESSIONS,
    MIXER_RESET_CLIPS,
    MIXER_SYS,
    MIXER_MUTED,
    MIXER_MUTE,
    MIXER_NO_SESSIONS,
    MIXER_REFRESH,
    MIXER_APP_TITLE,
    // Типы устройств
    DEV_MIC,
    DEV_LOOPBACK,
    DEV_SPEAKER,
    DEV_ASIO_IN,
    DEV_ASIO_OUT,
    DEV_VIRTUAL,
    DEV_MIXER,
    // ASIO панель
    ASIO_DRIVERS,
    ASIO_NO_DRIVERS,
    ASIO_LOAD,
    ASIO_LOAD_FAIL,
    ASIO_INPUTS,
    ASIO_OUTPUTS,
    ASIO_BUFFER,
    ASIO_SR,
    ASIO_CTRL_PANEL,
    ASIO_UNLOAD,
    // Лог
    LOG_TITLE,
    LOG_CLEAR,
    LOG_AUTOSCROLL,
    // Сообщения
    MSG_SAVED,
    MSG_SAVED_TO,
    MSG_ENABLED,
    // Статус-бар
    STATUS_CAPTURE,
    STATUS_RENDER,
    STATUS_VIRTUAL,
    STATUS_NODES,
    STATUS_CONNECTIONS,
    STATUS_ROUTING,
    STATUS_STOPPED,
    // Настройки
    SETTINGS_LANGUAGE,
    SETTINGS_RUSSIAN,
    SETTINGS_ENGLISH,
};

// ── Таблицы строк ────────────────────────────────────────────────────────────
namespace LangTable {

inline const char* RU[] = {
    // MENU_FILE
    "Файл", "Новый", "Сохранить патч...", "Загрузить патч...", "Выход",
    // MENU_ENGINE
    "Движок", "Запустить маршрутизацию", "Остановить маршрутизацию",
    "Обновить список устройств",
    // MENU_ASIO
    "ASIO", "Настройки ASIO", "Панель управления", "Выгрузить драйвер",
    // MENU_ADD
    "Добавить", "Узел микшера",
    // MENU_VIEW
    "  Volume Mixer", "  Лог",
    // Статусы
    "[LIVE] МАРШРУТИЗАЦИЯ", "[---] ОСТАНОВЛЕНО",
    // Панель устройств
    "АУДИО УСТРОЙСТВА", "Двойной клик = добавить в граф",
    "  Микрофоны / Входы", "  Loopback (Воспроизводимое)", "  Динамики / Выходы",
    // Граф
    "ГРАФ МАРШРУТИЗАЦИИ",
    "  [перетаскивать узлы | рисовать соединения | ПКМ = удалить]",
    // Свойства
    "СВОЙСТВА", "Выберите узел для\nредактирования свойств.",
    "ID:", "Тип:", "Усиление", "Включён",
    "Пик Л:", "Пик П:", "Удалить узел",
    // Volume Mixer
    "  Микшер громкости", "RMS", "Удержание", "Мин дБ",
    "Приложения", "Сбросить клипы", "СИС",
    "ТИХО", "ТИХО", "Нет активных аудио-сессий.",
    "Обновить", "  Громкость приложений  (устройство воспроизведения по умолчанию)",
    // Типы устройств
    "[МИК]", "[LB]", "[ДИН]", "[ASIO ВХ]", "[ASIO ВЫХ]", "[ВИРТУАЛ]", "[МИКШ]",
    // ASIO
    "Доступные ASIO драйверы:", "ASIO драйверы не найдены.",
    "Загрузить драйвер", "Не удалось загрузить драйвер.",
    "Входы:", "Выходы:", "Буфер:", "Частота:", "Панель управления", "Выгрузить",
    // Лог
    "  Лог", "Очистить", "Автопрокрутка",
    // Сообщения
    "Сохранено", "Сохранено в AudioRouter.patch", "Включён",
    // Статус-бар
    "захват", "вывод", "виртуал",
    "Узлов:", "Соединений:", ">> МАРШРУТИЗАЦИЯ", "-- ОСТАНОВЛЕНО",
    // Настройки
    "Язык", "Русский", "English",
};

inline const char* EN[] = {
    // MENU_FILE
    "File", "New", "Save patch...", "Load patch...", "Exit",
    // MENU_ENGINE
    "Engine", "Start Routing", "Stop Routing", "Refresh device list",
    // MENU_ASIO
    "ASIO", "ASIO Settings", "Control Panel", "Unload driver",
    // MENU_ADD
    "Add", "Mixer node",
    // MENU_VIEW
    "  Volume Mixer", "  Log",
    // Status
    "[LIVE] ROUTING ACTIVE", "[---] ROUTING STOPPED",
    // Device panel
    "AUDIO DEVICES", "Double-click to add to graph",
    "  Microphones / Inputs", "  Loopback (What's Playing)", "  Speakers / Outputs",
    // Graph
    "ROUTING GRAPH",
    "  [drag nodes | draw connections | right-click to delete]",
    // Properties
    "PROPERTIES", "Select a node to\nedit its properties.",
    "ID:", "Type:", "Gain", "Enabled",
    "Peak L:", "Peak R:", "Delete Node",
    // Volume Mixer
    "  Volume Mixer", "RMS", "Hold", "Min dB",
    "App Sessions", "Reset Clips", "SYS",
    "MUTED", "MUTE", "No active audio sessions found.",
    "Refresh", "  Application Volume  (default playback device)",
    // Device types
    "[MIC]", "[LB]", "[SPK]", "[ASIO IN]", "[ASIO OUT]", "[VIRT]", "[MIX]",
    // ASIO
    "Available ASIO Drivers:", "No ASIO drivers found.",
    "Load Driver", "Failed to load driver.",
    "Inputs:", "Outputs:", "Buffer:", "SR:", "Control Panel", "Unload",
    // Log
    "  Log", "Clear", "Auto-scroll",
    // Messages
    "Saved", "Saved to AudioRouter.patch", "Enabled",
    // Status bar
    "capture", "render", "virtual",
    "Nodes:", "Connections:", ">> ROUTING", "-- STOPPED",
    // Settings
    "Language", "Russian", "English",
};

static_assert(sizeof(RU)/sizeof(RU[0]) == sizeof(EN)/sizeof(EN[0]),
              "RU and EN tables must have the same number of entries");

} // namespace LangTable

// ── Global language state ─────────────────────────────────────────────────────
inline Lang g_lang = Lang::RU;

// T() = translate
inline const char* T(Str s) {
    int idx = (int)s;
    return (g_lang == Lang::RU) ? LangTable::RU[idx] : LangTable::EN[idx];
}
