# CheatSheet2

Портативное устройство на **ESP32-S3-N16R8 CAM** с цветным дисплеем, камерой и 5 кнопками управления. Изначально задумано как «электронная шпаргалка» — для быстрого доступа к заметкам и фото документов.

## Возможности

- 📷 **Камера** — live-превью ~10 FPS, снимки JPEG с поворотом -90° и зеркалированием (сенсор физически развёрнут на плате)
- 🗂 **Проводник** — просмотр файлов на Flash (LittleFS) и SD-карте, открытие текстовых файлов и JPEG
- ⚙️ **Настройки** — яркость, язык (RU/EN), выбор накопителя
- 🔁 **Повтор нажатий** — удержание кнопок навигации для плавной прокрутки
- 💾 **Сохранение фото** — на Flash или SD-карту с меткой времени

## Железо

| Компонент | Модель |
|-----------|--------|
| MCU | ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM) |
| Камера | OV5640 (DVP, припаяна) |
| Дисплей | ST7789 2.0" 240×320 (SPI, landscape) |
| Управление | 5 кнопок (крестовина + OK) |
| SD-карта | MicroSD (распаяна, GPIO 38-40) |

### Распиновка

```
TFT ST7789:          Кнопки:
  MOSI → GPIO 42       UP    → GPIO 1
  SCLK → GPIO 41       DOWN  → GPIO 2
  CS   → GPIO 47       LEFT  → GPIO 14
  DC   → GPIO 46       RIGHT → GPIO 3
  BL   → GPIO 45       OK    → GPIO 21
```

Подробная схема подключения: [HARDWARE.md](HARDWARE.md)

## Структура проекта

```
src/
├── main.cpp                    # Инициализация + главный цикл
├── drivers/
│   ├── camera_driver.cpp       # OV5640 через esp32-camera
│   ├── display.cpp             # TFT_eSPI + LVGL flush
│   ├── input.cpp               # Кнопки + debounce + repeat-fire
│   └── jpeg_decoder.cpp        # JPEG → RGB565 (JPEGDEC)
├── apps/
│   ├── app_registry.cpp        # Регистрация и управление приложениями
│   └── builtin/
│       ├── camera_app.cpp      # Камера: превью + снимок + сохранение
│       ├── file_explorer_app.cpp # Проводник: файлы, текст, JPEG
│       └── settings_app.cpp    # Настройки: яркость, язык, накопитель
├── services/
│   ├── lang_service.cpp        # Локализация (RU/EN) через NVS
│   └── storage_service.cpp     # Выбор накопителя (Flash/SD) через NVS
├── ui/
│   ├── ui_manager.cpp          # Переключение экранов
│   ├── theme.cpp               # Тёмная тема
│   ├── screens/                # Home, Splash
│   └── widgets/                # Plasma фон, Status bar
└── fonts/                      # Кастомные шрифты (10-28px, кириллица)
```

## Сборка

### Требования

- [PlatformIO CLI](https://platformio.org/install/cli) или плагин для VS Code
- ESP32-S3 плата подключена по USB (UART)

### Прошивка

```bash
# Полная прошивка (firmware + partition table)
pio run -t upload

# Монитор SERIAL для отладки
pio device monitor
```

> ⚠️ При первом запуске LittleFS автоматически форматируется.

### Прошивка только firmware (без partitions)

```bash
pio run -t upload
```

## Технические детали

### Камера

- Разрешение: SVGA (800×600), JPEG quality 12
- Превью: QUARTER scale (200×150 декодируется, растягивается на 320×204)
- Снимок: HALF scale (400×300 декодируется → 320×204)
- Поворот -90° + горизонтальное зеркалирование (фикс-пойнт 16.16 арифметика)
- JPEGDEC библиотека (bitbank2/JPEGDEC)

### Дисплей

- LVGL 9.x с кастомными шрифтами (кириллица 10-28px)
- Canvas widget для превью камеры (без кэширования LVGL)
- PSRAM буферы для framebuffer

### Разделы памяти

| Раздел | Размер | Назначение |
|--------|--------|------------|
| app0 | 2.5 MB | Прошивка |
| spiffs | 1.35 MB | LittleFS (фото, файлы) |
| coredump | 64 KB | Отладка крашей |

### Кнопки

- Дебаунс: 20 мс
- Long press: 800 мс (выход из приложений)
- Repeat-fire: 300 мс задержка, 80 мс интервал (навигационные кнопки)

## Используемые библиотеки

| Библиотека | Версия | Назначение |
|------------|--------|------------|
| [LVGL](https://lvgl.io) | ^9.2 | GUI фреймворк |
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | ^2.5.43 | Драйвер дисплея |
| [JPEGDEC](https://github.com/bitbank2/JPEGDEC) | ^1.4.0 | Декодирование JPEG |

## Лицензия

MIT
