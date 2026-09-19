#pragma once

#include "apps/app_base.h"

#define MAX_APPS 8

void app_registry_init();
int  app_registry_add(const char* name, const char* icon, AppInitFunc init, AppDeinitFunc deinit, AppButtonFunc on_button);
int  app_registry_count();
AppContext* app_registry_get(int index);
AppContext* app_registry_get_running();
void app_registry_open(int index, lv_obj_t* parent);
void app_registry_close(int index);
