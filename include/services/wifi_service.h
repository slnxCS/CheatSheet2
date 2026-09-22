#pragma once

// SoftAP-точка доступа для связи с телефоном (статус = иконка WiFi,
// IP всегда 192.168.4.1). Интернет не раздаётся и не требуется.
#define WIFI_AP_SSID   "CSCAM"
#define WIFI_AP_PASS   "cscam1234"   // WPA2, минимум 8 символов

void wifi_service_init();                 // читает NVS и запускает, если включён
bool wifi_service_start();
void wifi_service_stop();
bool wifi_service_running();
bool wifi_service_enabled();
void wifi_service_set_enabled(bool on);   // запомнить в NVS + старт/стоп
