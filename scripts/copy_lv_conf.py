Import("env")
import shutil
import os

proj_dir = env["PROJECT_DIR"]
libdeps = os.path.join(proj_dir, ".pio", "libdeps", env["PIOENV"])

# Copy lv_conf.h
lvgl_dir = os.path.join(libdeps, "lvgl")
conf_src = os.path.join(proj_dir, "include", "lv_conf.h")
conf_dst = os.path.join(lvgl_dir, "lv_conf.h")
if os.path.exists(conf_src) and os.path.isdir(lvgl_dir):
    shutil.copy2(conf_src, conf_dst)

# Copy User_Setup.h
tft_dir = os.path.join(libdeps, "TFT_eSPI")
setup_src = os.path.join(proj_dir, "include", "User_Setup.h")
setup_dst = os.path.join(tft_dir, "User_Setup.h")
if os.path.exists(setup_src) and os.path.isdir(tft_dir):
    shutil.copy2(setup_src, setup_dst)
