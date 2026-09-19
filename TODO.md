# CheatSheet2 — TODO

## Hardware
- [x] ESP32-S3-N16R8 CAM + OV5640
- [x] TFT 2.0" 240x320 ST7789 (SPI)
- [x] 5 кнопок (крестовина + OK)
- [x] WiFi связь с телефоном

## Распиновка
- [x] TFT: MOSI=42, SCLK=41, CS=40, DC=39, RST=47, BL=46
- [x] Кнопки: UP=1, DOWN=2, LEFT=38, RIGHT=21, OK=43
- [x] Камера: XCLK=15, SDA=4, SCL=5, VSYNC=6, HREF=7, PCLK=13, D0-D7=11,9,8,10,12,18,17,16

## Задачи

### Phase 1: Основа (MVP)
- [x] platformio.ini
- [x] pins.h
- [x] lv_conf.h (LVGL 9.x, шрифты 10-24, все виджеты)
- [x] display driver (TFT_eSPI + LVGL flush, DMA, PSRAM буферы)
- [x] input driver (кнопки + debounce + long press)
- [x] splash screen (анимация загрузки)
- [x] status bar (WiFi, время)
- [x] home screen (PS Vita style, круглые иконки, сетка 3x2)
- [x] theme (тёмная тема, палитра PS Vita)
- [x] ui manager (переключение экранов)
- [x] main.cpp (init + loop + button navigation)

### Phase 2: Приложения
- [x] app base class + registry (макс 8 приложений)
- [x] camera driver (OV5640, JPEG, PSRAM)
- [x] camera app (просмотр + авто-обновление)
- [x] settings app (яркость слайдер)
- [ ] WiFi service (HTTP server для связи с телефоном)
- [ ] WiFi transfer app (отправка JPEG на телефон)

### Phase 3: Polish
- [ ] OTA service
- [ ] Кастомные шрифты
- [ ] Иконки (PNG → C массивы)
- [ ] Анимации переходов
- [ ] Энергосбережение (auto-sleep)

## Архитектура
```
src/
├── main.cpp
├── drivers/         (display, camera, input)
├── ui/              (screens, widgets, theme)
├── apps/            (builtin apps + registry)
└── services/        (wifi, ota)
```

## Библиотеки
- LVGL 9.6.0
- TFT_eSPI 2.5.43
- esp32-camera (встроенная)
- WiFi (встроенная)

## Статус сборки
- RAM:   [===       ]  27.8% (90956 / 327680 bytes)
- Flash: [==        ]  21.4% (673809 / 3145728 bytes)
