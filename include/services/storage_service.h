#pragma once

// Storage device: 0 = Flash (LittleFS), 1 = SD Card
void storage_init();
int storage_get();
bool storage_set(int device);
void storage_set_constrain(int constrain);