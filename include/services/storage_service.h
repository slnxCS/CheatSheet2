#pragma once

// Storage device: 0 = Flash (LittleFS), 1 = SD Card
void storage_init();
int storage_get();
void storage_set(int device);
