#include "drivers/input.h"
#include "fonts/fonts.h"
#include "apps/builtin/file_explorer_app.h"
#include "services/lang_service.h"
#include "ui/theme.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <cstring>
#include <vector>

#define MAX_PATH 128
#define MAX_ENTRIES 32
#define MAX_NAME 64
#define VISIBLE_ITEMS 8
#define TEXT_BUF_SIZE 4096

struct FileEntry {
    char name[MAX_NAME];
    bool is_dir;
    uint32_t size;
};

enum DataSource { SRC_INTERNAL, SRC_SD };
enum ViewerMode { VIEW_NONE, VIEW_TEXT, VIEW_IMAGE };

static lv_obj_t* parent_ref = nullptr;
static lv_obj_t* path_label = nullptr;
static lv_obj_t* file_list = nullptr;
static lv_obj_t* hint_label = nullptr;
static lv_obj_t* src_label = nullptr;
static lv_obj_t* viewer_obj = nullptr;
static lv_obj_t* viewer_content = nullptr;
static lv_obj_t* viewer_title = nullptr;

static lv_obj_t* item_icons[VISIBLE_ITEMS] = {nullptr};
static lv_obj_t* item_names[VISIBLE_ITEMS] = {nullptr};
static lv_obj_t* item_info[VISIBLE_ITEMS] = {nullptr};

static String current_path = String("/");
static DataSource current_src = SRC_INTERNAL;
static FileEntry entries[MAX_ENTRIES];
static int entry_count = 0;
static int selected_idx = 0;
static int scroll_offset = 0;
static bool sd_mounted = false;
static bool littlefs_mounted = false;
static bool picking_source = false;
static ViewerMode viewer_mode = VIEW_NONE;
static char* text_buf = nullptr;
static uint8_t* img_data = nullptr;
static size_t img_data_size = 0;
static lv_fs_path_ex_t img_mempath;

// --- Хелперы для путей ---

// Корень FS: LittleFS = "/", SD_MMC = "/sdcard"
static const char* get_root_path() {
    //return (current_src == SRC_SD) ? "/sdcard" : "/";
    return "/";
}

static FS* get_current_FS() {
    switch (current_src)
    {
        case SRC_SD :
            return &SD_MMC;
        case SRC_INTERNAL :
            return &LittleFS;
        default:
            Serial.printf("Unknown file system: %d\n", (uint8_t)current_src);
            return &LittleFS;
    }
}


#define is_at_root() (current_path == get_root_path())

// Проверка: текущий путь — корень?
//static bool is_at_root() {
//    //const char* root = get_root_path();
//    //size_t root_len = strlen(root);
//    //if (strlen(current_path) < root_len) return false;
//    //return (strncmp(current_path, root, root_len) == 0 &&
//    //        (current_path[root_len] == '\0' || current_path[root_len] == '/'));
//
//    return current_path == get_root_path();
//}

// Установить путь в корень
static void reset_to_root() {
    current_path = "/";
}

// Построить полный путь из current_path + имя entry
static String build_entry_path(const char* entry_name) {
    return current_path + "/" + entry_name;
}

// --- Файловые функции ---

static bool is_text_file(const char* name) {
    const char* ext = strrchr(name, '.');
    if (!ext) return false;
    ext++;
    return (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "log") == 0 ||
            strcasecmp(ext, "csv") == 0 || strcasecmp(ext, "json") == 0 ||
            strcasecmp(ext, "xml") == 0 || strcasecmp(ext, "ini") == 0 ||
            strcasecmp(ext, "cfg") == 0 || strcasecmp(ext, "h") == 0 ||
            strcasecmp(ext, "c") == 0 || strcasecmp(ext, "cpp") == 0 ||
            strcasecmp(ext, "md") == 0 || strcasecmp(ext, "py") == 0 ||
            strcasecmp(ext, "js") == 0 || strcasecmp(ext, "html") == 0 ||
            strcasecmp(ext, "css") == 0) || strcasecmp(ext, "bin") == 0;
}

static bool is_image_file(const char* name) {
    const char* ext = strrchr(name, '.');
    if (!ext) return false;
    ext++;
    return (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
            strcasecmp(ext, "png") == 0 || strcasecmp(ext, "bmp") == 0);
}

static const char* get_file_icon(const char* name, bool is_dir) {
    if (is_dir) return LV_SYMBOL_DIRECTORY;
    if (is_image_file(name)) return LV_SYMBOL_IMAGE;
    if (is_text_file(name)) return LV_SYMBOL_FILE;
    const char* ext = strrchr(name, '.');
    if (ext) {
        ext++;
        if (strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "wav") == 0)
            return LV_SYMBOL_AUDIO;
        if (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "avi") == 0)
            return LV_SYMBOL_VIDEO;
    }
    return LV_SYMBOL_SETTINGS;
}

static void format_size(uint32_t size, char* buf, size_t len) {
    if (size < 1024) snprintf(buf, len, "%u B", (unsigned)size);
    else if (size < 1024 * 1024) snprintf(buf, len, "%.1f KB", size / 1024.0f);
    else snprintf(buf, len, "%.1f MB", size / (1024.0f * 1024.0f));
}

// Подсчёт файлов в папке (принимает ПОЛНЫЙ путь)
static void count_dir_items(String fullpath, DataSource src, int* out_count) {
    int count = 0;
    File dir;
    if (src == SRC_INTERNAL) dir = LittleFS.open(fullpath, "r");
    else if (src == SRC_SD && sd_mounted) dir = SD_MMC.open(fullpath, "r");
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f && count < 1000) { count++; f = dir.openNextFile(); }
    }
    if (dir) dir.close();
    *out_count = count;
}

static void scan_directory() {
    entry_count = 0;
    selected_idx = 0;
    scroll_offset = 0;
    
    FS* selected_fs;

    switch (current_src)
    {
        case SRC_INTERNAL : 
            selected_fs = &LittleFS;
        break;

        case SRC_SD :
            selected_fs = &SD_MMC;
        break;

        default:
            Serial.printf("[Explorer] : Unknown src %d\n", (uint16_t)current_src);
        break;
    }

    File dir = selected_fs->open(current_path, FILE_READ);

    for (File f = dir.openNextFile(); f && entry_count < MAX_ENTRIES; f = dir.openNextFile(), entry_count++) {
        entries[entry_count].is_dir = f.isDirectory();
        entries[entry_count].size = f.size();
        strncpy(entries[entry_count].name, f.name(), MAX_NAME);
    }

    dir.close();
}

static void cleanup_items() {
    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        if (item_icons[i]) { lv_obj_delete(item_icons[i]); item_icons[i] = nullptr; }
        if (item_names[i]) { lv_obj_delete(item_names[i]); item_names[i] = nullptr; }
        if (item_info[i]) { lv_obj_delete(item_info[i]); item_info[i] = nullptr; }
    }
}

static void update_display() {
    if (!file_list || viewer_mode != VIEW_NONE) return;

    for (int i = 0; i < VISIBLE_ITEMS; i++) {
        if (item_icons[i]) lv_obj_set_hidden(item_icons[i], true);
        if (item_names[i]) lv_obj_set_hidden(item_names[i], true);
        if (item_info[i]) lv_obj_set_hidden(item_info[i], true);
    }

    for (int i = scroll_offset; i < entry_count && i < scroll_offset + VISIBLE_ITEMS; i++) {
        int slot = i - scroll_offset;
        int y = slot * 22;

        if (!item_icons[slot]) {
            item_icons[slot] = lv_label_create(file_list);
            lv_obj_set_style_text_font(item_icons[slot], &lv_font_cyr_14, 0);
        }
        if (!item_names[slot]) {
            item_names[slot] = lv_label_create(file_list);
            lv_obj_set_style_text_font(item_names[slot], &lv_font_cyr_12, 0);
        }
        if (!item_info[slot]) {
            item_info[slot] = lv_label_create(file_list);
            lv_obj_set_style_text_font(item_info[slot], &lv_font_cyr_10, 0);
        }

        lv_label_set_text(item_icons[slot], get_file_icon(entries[i].name, entries[i].is_dir));
        lv_obj_set_pos(item_icons[slot], 4, y);
        lv_obj_set_hidden(item_icons[slot], false);

        lv_label_set_text(item_names[slot], entries[i].name);
        lv_obj_set_pos(item_names[slot], 22, y);
        lv_obj_set_hidden(item_names[slot], false);

        // Подсчёт файлов в папке — нужен ПОЛНЫЙ путь
        char info[20] = "";
        if (entries[i].is_dir && strcmp(entries[i].name, "..") != 0) {
            String dirpath = build_entry_path(entries[i].name);
            int cnt = 0;
            count_dir_items(dirpath, current_src, &cnt);
            snprintf(info, sizeof(info), "%d", cnt);
        } else if (!entries[i].is_dir) {
            format_size(entries[i].size, info, sizeof(info));
        }
        lv_label_set_text(item_info[slot], info);
        lv_obj_set_pos(item_info[slot], 260, y);
        lv_obj_set_hidden(item_info[slot], false);

        bool is_sel = (i == selected_idx);
        lv_obj_set_style_text_color(item_icons[slot], is_sel ? theme_color_accent() : theme_color_text_muted(), 0);
        lv_obj_set_style_text_color(item_names[slot], is_sel ? lv_color_white() : theme_color_text_muted(), 0);
        lv_obj_set_style_text_color(item_info[slot], is_sel ? theme_color_accent() : theme_color_text_muted(), 0);
        lv_obj_set_style_text_font(item_names[slot], is_sel ? &lv_font_cyr_14 : &lv_font_cyr_12, 0);
    }

    if (path_label) {
        const char* src_name = (current_src == SRC_INTERNAL) ? lang_str_explorer_flash() : "SD";
        lv_label_set_text_fmt(path_label, "%s  %s", src_name, current_path);
    }

    if (src_label) {
        if (picking_source) {
            const char* s = (current_src == SRC_INTERNAL) ? lang_str_explorer_flash() : "SD";
            lv_label_set_text_fmt(src_label, "%s %s", LV_SYMBOL_LOOP, s);
            lv_obj_set_style_text_color(src_label, theme_color_accent(), 0);
            lv_obj_set_hidden(src_label, false);
        } else {
            lv_obj_set_hidden(src_label, true);
        }
    }

    if (hint_label) {
        if (picking_source) {
            lv_label_set_text(hint_label, LV_SYMBOL_LEFT " " LV_SYMBOL_RIGHT " " LV_SYMBOL_OK);
        } else {
            lv_label_set_text_fmt(hint_label, LV_SYMBOL_UP " " LV_SYMBOL_DOWN " %s   " LV_SYMBOL_LEFT " %s   " LV_SYMBOL_RIGHT " %s",
                lang_str_explorer_select(), lang_str_explorer_back(), lang_str_explorer_open());
        }
    }

    lv_obj_invalidate(file_list);
}

// --- Просмотр ---

static void close_viewer() {
    if (viewer_mode == VIEW_TEXT && text_buf) {
        free(text_buf);
        text_buf = nullptr;
    }
    if (viewer_mode == VIEW_IMAGE && img_data) {
        free(img_data);
        img_data = nullptr;
        img_data_size = 0;
    }
    if (viewer_obj) {
        lv_obj_delete(viewer_obj);
        viewer_obj = nullptr;
    }
    viewer_content = nullptr;
    viewer_title = nullptr;
    viewer_mode = VIEW_NONE;
}

static void open_text_viewer(String filepath, const char* filename) {
    File f;
    if (current_src == SRC_INTERNAL) f = LittleFS.open(filepath, "r");
    else f = SD_MMC.open(filepath, "r");

    if (!f) return;

    size_t fsize = f.size();
    if (fsize > TEXT_BUF_SIZE - 1) fsize = TEXT_BUF_SIZE - 1;

    text_buf = (char*)malloc(fsize + 1);
    if (!text_buf) { f.close(); return; }

    f.read((uint8_t*)text_buf, fsize);
    text_buf[fsize] = '\0';
    f.close();

    viewer_mode = VIEW_TEXT;

    viewer_obj = lv_obj_create(parent_ref);
    lv_obj_set_size(viewer_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(viewer_obj, lv_color_hex(0x0A1628), 0);
    lv_obj_set_style_bg_opa(viewer_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(viewer_obj, 0, 0);
    lv_obj_set_style_pad_all(viewer_obj, 0, 0);

    lv_obj_t* vheader = lv_obj_create(viewer_obj);
    lv_obj_set_size(vheader, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(vheader, theme_color_panel(), 0);
    lv_obj_set_style_bg_opa(vheader, LV_OPA_80, 0);
    lv_obj_set_style_border_width(vheader, 0, 0);
    lv_obj_set_style_radius(vheader, 0, 0);
    lv_obj_align(vheader, LV_ALIGN_TOP_MID, 0, 0);

    viewer_title = lv_label_create(vheader);
    lv_label_set_text_fmt(viewer_title, "%s %s", LV_SYMBOL_FILE, filename);
    lv_obj_set_style_text_color(viewer_title, theme_color_text(), 0);
    lv_obj_set_style_text_font(viewer_title, &lv_font_cyr_14, 0);
    lv_obj_align(viewer_title, LV_ALIGN_CENTER, 0, 0);

    viewer_content = lv_label_create(viewer_obj);
    lv_label_set_text(viewer_content, text_buf);
    lv_obj_set_style_text_color(viewer_content, theme_color_text(), 0);
    lv_obj_set_style_text_font(viewer_content, &lv_font_cyr_12, 0);
    lv_obj_set_style_pad_all(viewer_content, 8, 0);
    lv_obj_set_width(viewer_content, LV_PCT(100));
    lv_obj_set_height(viewer_content, LV_PCT(100));
    lv_obj_align(viewer_content, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_long_mode(viewer_content, LV_LABEL_LONG_WRAP);
    lv_obj_set_scroll_dir(viewer_content, LV_DIR_VER);

    if (hint_label) {
        lv_label_set_text_fmt(hint_label, LV_SYMBOL_UP " " LV_SYMBOL_DOWN " %s   " LV_SYMBOL_LEFT " %s",
            lang_str_explorer_select(), lang_str_explorer_back());
    }
}

static void open_image_viewer(String filepath, const char* filename) {
    File f = get_current_FS()->open(filepath, FILE_READ);

    if (!f) return;

    size_t fsize = f.size();
    img_data = (uint8_t*)ps_malloc(fsize);
    if (!img_data) { f.close(); return; }

    f.read(img_data, fsize);
    img_data_size = fsize;
    f.close();

    viewer_mode = VIEW_IMAGE;

    viewer_obj = lv_obj_create(parent_ref);
    lv_obj_set_size(viewer_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(viewer_obj, lv_color_hex(0x0A1628), 0);
    lv_obj_set_style_bg_opa(viewer_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(viewer_obj, 0, 0);
    lv_obj_set_style_pad_all(viewer_obj, 0, 0);

    lv_obj_t* vheader = lv_obj_create(viewer_obj);
    lv_obj_set_size(vheader, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(vheader, theme_color_panel(), 0);
    lv_obj_set_style_bg_opa(vheader, LV_OPA_80, 0);
    lv_obj_set_style_border_width(vheader, 0, 0);
    lv_obj_set_style_radius(vheader, 0, 0);
    lv_obj_align(vheader, LV_ALIGN_TOP_MID, 0, 0);

    viewer_title = lv_label_create(vheader);
    lv_label_set_text_fmt(viewer_title, "%s %s", LV_SYMBOL_IMAGE, filename);
    lv_obj_set_style_text_color(viewer_title, theme_color_text(), 0);
    lv_obj_set_style_text_font(viewer_title, &lv_font_cyr_14, 0);
    lv_obj_align(viewer_title, LV_ALIGN_CENTER, 0, 0);

    // Регистрируем JPEG в MEMFS для декодирования LVGL TJPGD
    lv_fs_make_path_from_buffer(&img_mempath, LV_FS_MEMFS_LETTER, img_data, img_data_size, "jpg");
    viewer_content = lv_img_create(viewer_obj);
    lv_img_set_src(viewer_content, &img_mempath);
    lv_image_set_rotation(viewer_content, 900);  // 90° CW — как на предпросмотре камеры
    lv_obj_align(viewer_content, LV_ALIGN_CENTER, 0, 14);

    if (hint_label) {
        lv_label_set_text_fmt(hint_label, LV_SYMBOL_LEFT " %s", lang_str_explorer_back());
    }
}

static void open_file(String filepath, const char* filename) {
    if (is_text_file(filename)) {
        open_text_viewer(filepath, filename);
    } else if (is_image_file(filename)) {
        open_image_viewer(filepath, filename);
    }
}

static bool is_dir_fp(const char* full_path, FS* fs) {
    File f = fs->open(full_path, FILE_READ);
    bool is_dir = f.isDirectory();
    f.close();

    return is_dir;
}

static void delete_file(String file_name, FS* fs = nullptr) {
    if (fs == nullptr) fs = get_current_FS();

    File root = fs->open(file_name, FILE_READ);
    bool is_dir = root.isDirectory();
    if (root.isDirectory()) {
        std::vector<String> paths;

        File f = root.openNextFile();
        while (f) {
            paths.push_back(f.path());
            f.close();
            f = root.openNextFile();
        }
        root.close();
        
        for (auto path : paths) {
            delete_file(path, fs);
        }

        fs->rmdir(file_name);
    }
    else {
        root.close();
        fs->remove(file_name);
    }
}

// --- Навигация ---

// Подъём на уровень вверх
static void go_up() {
    current_path = current_path.substring(0, current_path.lastIndexOf('/'));
    if (current_path.length() == 0) current_path = "/";
}

void print_all_entries(const char* dir_name, FS& fs) {
    File dir = fs.open(String(current_path) + String(dir_name));

    int index = 0;

    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        Serial.printf("%d. %s\n", index, f.name());
        index++;
    }

    dir.close();
}

// Вход в папку
static void enter_dir(const char* dir_name) {
    if (!is_at_root()) current_path += "/";
    current_path += dir_name;
}

// --- Открытие/закрытие ---

void file_explorer_open(lv_obj_t* parent) {
    parent_ref = parent;
    selected_idx = 0;
    scroll_offset = 0;
    entry_count = 0;
    picking_source = false;
    viewer_mode = VIEW_NONE;
    text_buf = nullptr;
    img_data = nullptr;
    img_data_size = 0;
    viewer_obj = nullptr;
    memset(item_icons, 0, sizeof(item_icons));
    memset(item_names, 0, sizeof(item_names));
    memset(item_info, 0, sizeof(item_info));

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0A1628), 0);

    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(header, theme_color_panel(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_80, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 6, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_scroll_dir(header, LV_DIR_NONE);

    lv_obj_t* lbl_title = lv_label_create(header);
    lv_label_set_text_fmt(lbl_title, "%s %s", LV_SYMBOL_DIRECTORY, lang_str_explorer_title());
    lv_obj_set_style_text_color(lbl_title, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_cyr_14, 0);
    lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 0, 0);

    src_label = lv_label_create(header);
    lv_label_set_text(src_label, "");
    lv_obj_set_style_text_font(src_label, &lv_font_cyr_12, 0);
    lv_obj_align(src_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_hidden(src_label, true);

    path_label = lv_label_create(parent);
    lv_obj_set_style_text_color(path_label, theme_color_accent(), 0);
    lv_obj_set_style_text_font(path_label, &lv_font_cyr_10, 0);
    lv_obj_align(path_label, LV_ALIGN_TOP_MID, 0, 32);

    file_list = lv_obj_create(parent);
    lv_obj_set_size(file_list, LV_PCT(95), VISIBLE_ITEMS * 22 + 4);
    lv_obj_set_style_bg_opa(file_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(file_list, 0, 0);
    lv_obj_set_style_pad_all(file_list, 0, 0);
    lv_obj_align(file_list, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_scroll_dir(file_list, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(file_list, LV_SCROLLBAR_MODE_OFF);

    hint_label = lv_label_create(parent);
    lv_obj_set_style_text_color(hint_label, theme_color_text_muted(), 0);
    lv_obj_set_style_text_font(hint_label, &lv_font_cyr_10, 0);
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -2);

    reset_to_root();
    scan_directory();
    update_display();
}

void file_explorer_button(int button_id, int event) {
    if (viewer_mode != VIEW_NONE) {
        if (button_id == BTN_ID_LEFT || button_id == BTN_ID_OK) {
            close_viewer();
            if (hint_label) {
                lv_label_set_text_fmt(hint_label, LV_SYMBOL_UP " " LV_SYMBOL_DOWN " %s   " LV_SYMBOL_LEFT " %s   " LV_SYMBOL_RIGHT " %s",
                    lang_str_explorer_select(), lang_str_explorer_back(), lang_str_explorer_open());
            }
            update_display();
        } else if (button_id == BTN_ID_UP && viewer_mode == VIEW_TEXT && viewer_content) {
            lv_obj_scroll_by(viewer_content, 0, -40, LV_ANIM_ON);
        } else if (button_id == BTN_ID_DOWN && viewer_mode == VIEW_TEXT && viewer_content) {
            lv_obj_scroll_by(viewer_content, 0, 40, LV_ANIM_ON);
        }
        return;
    }

    if (picking_source) {
        if (button_id == BTN_ID_LEFT || button_id == BTN_ID_RIGHT) {
            current_src = (current_src == SRC_INTERNAL) ? SRC_SD : SRC_INTERNAL;
            picking_source = false;
            reset_to_root();
            cleanup_items();
            scan_directory();
            update_display();
        } else if (button_id == BTN_ID_OK) {
            bool available = (current_src == SRC_INTERNAL && littlefs_mounted) ||
                             (current_src == SRC_SD && sd_mounted);
            if (available) {
                picking_source = false;
                reset_to_root();
                cleanup_items();
                scan_directory();
                update_display();
            }
        }
        return;
    }

    if (button_id == BTN_ID_UP) {
        if (selected_idx > 0) {
            selected_idx--;
            if (selected_idx < scroll_offset) scroll_offset = selected_idx;
            update_display();
        }
    } else if (button_id == BTN_ID_DOWN) {
        if (selected_idx < entry_count - 1) {
            selected_idx++;
            if (selected_idx - scroll_offset >= VISIBLE_ITEMS) scroll_offset = selected_idx - VISIBLE_ITEMS + 1;
            update_display();
        }
    } else if (button_id == BTN_ID_LEFT) {
        if (is_at_root()) {
            picking_source = true;
            update_display();
        } else {
            go_up();
            cleanup_items();
            scan_directory();
            update_display();
        }
    } 
    else if (button_id == BTN_ID_RIGHT) {
        delete_file(build_entry_path(entries[selected_idx].name));
        cleanup_items();
        scan_directory();
        update_display();
    }
    else if (button_id == BTN_ID_OK) {
        if (entry_count == 0) return;
        FileEntry* e = &entries[selected_idx];

        if (strcmp(e->name, "..") == 0) {
            go_up();
            cleanup_items();
            scan_directory();
            update_display();
        } else if (e->is_dir) {
            enter_dir(e->name);
            cleanup_items();
            scan_directory();
            update_display();
        } else {
            // Открытие файла — строим полный путь
            String fullpath = build_entry_path(e->name);
            open_file(fullpath, e->name);
        }
    }
}

void file_explorer_close() {
    close_viewer();
    cleanup_items();
    parent_ref = nullptr;
    path_label = nullptr;
    file_list = nullptr;
    hint_label = nullptr;
    src_label = nullptr;
    entry_count = 0;
    selected_idx = 0;
    scroll_offset = 0;
    picking_source = false;
}
