#pragma once
#include <stdint.h>
#include <stddef.h>

// Часы устройства. Интернета у ESP нет (только SoftAP) — точное время
// присылает телефон заголовком X-Time (UTC-эпоха, сек) и X-Tz (смещение
// локальной зоны, сек) на каждом запросе. Последнее сохранённое значение
// лежит в /clock и восстанавливается при загрузке (время переживает ребут,
// пока телефон не подключится).

void time_service_set_utc(int64_t utc_sec);     // применить время с телефона
void time_service_set_tz(int32_t offset_sec);   // смещение зоны, сек (МСК=10800)
void time_service_boot_restore();               // вызвать в setup() после LittleFS

// "HH:MM" и "YYYYMMDD_HHMMSS" в локальной зоне.
// ВАЖНО: без localtime()/gmtime() — они берут newlib-замок, а вызов из
// portENTER_CRITICAL приводил к abort() «recursive mutex in ISR context»
// (bэкстрейс: locks.c:139 → localtime → hist_add).
void time_hm(char* out, size_t max);            // "12:34"
void time_ymdhms(char* out, size_t max);        // "20261002_153045"
