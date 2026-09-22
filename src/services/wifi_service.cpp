#include "services/wifi_service.h"
#include "ui/widgets/status_bar.h"
#include <WiFi.h>
#include <Preferences.h>

static Preferences prefs;
static bool enabled = true;
static bool running = false;

void wifi_service_init() {
    prefs.begin("wifi", true);
    enabled = prefs.getBool("on", true);
    prefs.end();
    if (enabled) wifi_service_start();
}

bool wifi_service_start() {
    if (running) return true;

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS)) {
        WiFi.mode(WIFI_OFF);
        Serial.println("wifi: softAP failed");
        return false;
    }
    running = true;
    status_bar_set_wifi(true);
    Serial.printf("wifi: AP '%s' up, IP %s\n",
                  WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
    return true;
}

void wifi_service_stop() {
    if (!running) return;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    running = false;
    status_bar_set_wifi(false);
    Serial.println("wifi: AP stopped");
}

bool wifi_service_running() {
    return running;
}

bool wifi_service_enabled() {
    return enabled;
}

void wifi_service_set_enabled(bool on) {
    if (on == enabled) return;
    enabled = on;
    prefs.begin("wifi", false);
    prefs.putBool("on", on);
    prefs.end();
    if (on) wifi_service_start();
    else    wifi_service_stop();
}
