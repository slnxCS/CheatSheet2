#pragma once

#include <stdint.h>
#include <stdbool.h>

// Battery monitor — USB detection only (без дополнительной схемы)
// Показывает: USB подключён / от батареи
// Точный % заряда требует делитель напряжения на ADC

void battery_init();
int  battery_get_percent();           // 100=USB, -1=battery (без %)
float battery_get_voltage();          // 5.0=USB, 0=battery
bool battery_is_usb_connected();      // true/false
