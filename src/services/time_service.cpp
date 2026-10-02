#include "services/time_service.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <sys/time.h>
#include <ctime>
#include <cstdio>
#include <cstdlib>

#define CLOCK_FILE        "/clock"
#define CLOCK_VALID_FROM  1600000000LL   // 2020-09-13 — раньше невалидно
#define CLOCK_VALID_TO    4102444800LL   // 2100-01-01

static int32_t s_tz = 3 * 3600;   // МСК, пока телефон не сообщил свою зону
static int64_t s_stored = 0;      // что сейчас лежит в /clock

void time_service_set_tz(int32_t offset_sec) {
    if (offset_sec >= -14 * 3600 && offset_sec <= 14 * 3600)
        s_tz = offset_sec;
}

void time_service_set_utc(int64_t utc) {
    if (utc < CLOCK_VALID_FROM || utc > CLOCK_VALID_TO) return;
    struct timeval tv = { (time_t)utc, 0 };
    settimeofday(&tv, nullptr);

    // /clock переписываем нечасто (раз в час): значение должно пережить
    // ребут даже без телефона, но флэш не мучаем
    if (s_stored != 0 && utc - s_stored < 3600) return;
    File f = LittleFS.open(CLOCK_FILE, FILE_WRITE);
    if (f) {
        f.printf("%lld\n", (long long)utc);
        f.close();
        s_stored = utc;
    }
}

void time_service_boot_restore() {
    if (!LittleFS.exists(CLOCK_FILE)) return;
    File f = LittleFS.open(CLOCK_FILE, FILE_READ);
    if (!f) return;
    char buf[24] = {0};
    f.readBytes(buf, sizeof(buf) - 1);
    f.close();
    long long v = atoll(buf);
    if (v < CLOCK_VALID_FROM || v > CLOCK_VALID_TO) return;
    struct timeval tv = { (time_t)v, 0 };
    settimeofday(&tv, nullptr);
    s_stored = v;
    Serial.printf("clock: restored %lld\n", v);
}

// ---------- Разбор времени без localtime() ----------

// Дни с 1970-01-01 → календарная дата (алгоритм Hinnant),
// без newlib-замков и без getenv("TZ")
static void civil_from_days(long z, int* y, int* m, int* d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned long doe = (unsigned long)(z - era * 146097);            // [0, 146096]
    const unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const long y2 = (long)yoe + era * 400;
    const unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);      // [0, 365]
    const unsigned long mp = (5 * doy + 2) / 153;                           // [0, 11]
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);                               // [1, 31]
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);                                  // [1, 12]
    *y = (int)(*m <= 2 ? y2 + 1 : y2);
}

void time_hm(char* out, size_t max) {
    long sec = (long)time(nullptr) + s_tz;
    if (sec < 0) sec = 0;
    long mins = sec / 60;
    snprintf(out, max, "%02ld:%02ld", (mins / 60) % 24, mins % 60);
}

void time_ymdhms(char* out, size_t max) {
    long sec = (long)time(nullptr) + s_tz;
    if (sec < 0) sec = 0;
    int y = 1970, mo = 1, d = 1;
    civil_from_days((long)(sec / 86400), &y, &mo, &d);
    long rem = sec % 86400;
    snprintf(out, max, "%04d%02d_%02ld%02ld%02ld", y, mo, d,
             rem / 3600, (rem / 60) % 60, rem % 60);
}
