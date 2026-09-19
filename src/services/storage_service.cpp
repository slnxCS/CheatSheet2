#include "services/storage_service.h"
#include <Preferences.h>

static Preferences prefs;
static int current_device = 0; // 0 = Flash
static int device_constrain = 1;

void storage_init() {
    prefs.begin("storage", true);
    int stored = prefs.getInt("device", 0);
    prefs.end();
    if (stored >= 0 && stored <= 1)
        current_device = stored;
    else 
        Serial.printf("Unknown storage device : %d\n", current_device);
}

int storage_get() {
    return current_device;
}

void storage_set_constrain(int constrain) {
    device_constrain = constrain;
}

bool storage_set(int device) {
    if (device < 0 || device > device_constrain) return false;
    current_device = device;
    prefs.begin("storage", false);
    prefs.putInt("device", device);
    prefs.end();

    return true;
}
