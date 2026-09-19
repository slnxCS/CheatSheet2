#include "services/storage_service.h"
#include <Preferences.h>

static Preferences prefs;
static int current_device = 0; // 0 = Flash

void storage_init() {
    prefs.begin("storage", true);
    int stored = prefs.getInt("device", 0);
    prefs.end();
    if (stored >= 0 && stored <= 1) {
        current_device = stored;
    }
}

int storage_get() {
    return current_device;
}

void storage_set(int device) {
    if (device < 0 || device > 1) return;
    current_device = device;
    prefs.begin("storage", false);
    prefs.putInt("device", device);
    prefs.end();
}
