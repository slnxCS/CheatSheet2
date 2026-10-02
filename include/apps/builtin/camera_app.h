#pragma once

#include <lvgl.h>

void camera_app_open(lv_obj_t* parent);
void camera_app_close();
void camera_app_button(int button_id, int event);

// Фоновый снимок без открытого приложения камеры — кнопка «Сфоткать»
// в чате «ИИ». Тот же AF/сохранение, что и камера, но В ОТДЕЛЬНОЙ ЗАДАЧЕ
// (16 КБ стека, ядро 1): LVGL/кнопки не блокируются, паника по переполнению
// стека loopTask исключена. Этапы опрашиваются из LVGL-таймера.
bool camera_app_capture_start();              // false = уже идёт / камера открыта
int  camera_app_capture_poll(unsigned* kb);   // -1 = нет; 0..3 = идёт (3 = кадр снят,
                                              // идёт сохранение); 4 = успех (kb = КБ);
                                              // 5 = ошибка
// «Тёплая» камера: после снимка/закрытия приложения не гасим 60 с —
// повторный снимок/открытие без camera_init. Гаснет из loop() этим вызовом:
void camera_app_warm_poll();
