#pragma once

// Программное открытие/закрытие приложений (используется ai_link
// для автооткрытия экрана «ИИ»; в main.cpp живёт общее состояние).
void app_host_open_index(int idx);  // закрывает текущее приложение, если есть
bool app_host_in_app();
int  app_host_current();
int  app_host_ai_index();           // индекс экрана «ИИ» в реестре (-1 если нет)
