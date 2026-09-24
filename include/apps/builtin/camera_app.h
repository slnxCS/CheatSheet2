#pragma once

#include <lvgl.h>

void camera_app_open(lv_obj_t* parent);
void camera_app_close();
void camera_app_button(int button_id, int event);

// Снять и сохранить фото без открытого приложения камеры — кнопка
// «Сфоткать» в чате «ИИ». Использует тот же AF/сохранение, что и камера.
// stage_cb: 0=инициализация, 1=автофокус, 2=снимок, 3=сохранено (arg=КБ);
// вызывается между этапами — можно показать плашку и lv_refr_now().
// Блокирует вызывающего на ~3-7 с. false = ошибка.
bool camera_app_capture_headless(void (*stage_cb)(int stage, unsigned arg));
