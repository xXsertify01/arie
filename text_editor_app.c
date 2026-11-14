// text_editor_app_fixed_with_splash.c
// Compact text editor with file browser and compact keyboard.
// Splash screen shows provided XBM until a directional key is pressed (Up/Down/Left/Right).
// OK/select is ignored on the splash so it won't open folders.
// Cursor placement fixed: measure text pixel width for accurate cursor X using canvas_string_width.
// Added safe copy/paste in browser: short Right to copy a file, short Right again to paste into current directory.
// Paste refuses to overwrite existing files.
// Stack-safety fixes: large temporary buffers moved to App fields to avoid stack overflow.
// Restored hidden-entry behavior: entries named "assets" and dotfiles are hidden again.
// Centralized allowlist logic and streaming file load to avoid large heap allocations.
// Small transient status line added for copy/paste feedback.
// Bugfix: separate timestamp for transient status so messages time out correctly.
// Minimal in-place cleanup applied: centralized status helper, path builder, allowlist matcher, paste validator, and constants.
// This isnt the one used on my fap but it was one of the original ones.

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define MAX_LINES        256
#define MAX_LINE_LEN     128
#define MAX_PATH_LEN     256
#define MAX_NAME_LEN     128
#define MAX_ENTRIES      256
#define VISIBLE_LINES    5
#define BROWSER_VISIBLE_LINES 6
#define BROWSER_LINE_HEIGHT   8
#define TOP_MARGIN       8
#define IO_CHUNK         1024
#define BROWSER_NAME_SCROLL_TICK  320
#define BROWSER_NAME_PAUSE_TICK   1800

#define MAX_VISIBLE_COLS 20

#define KB_ROWS          4
#define KB_COLS_MAX      18

#define ROOT_DIR         "/ext"
#define TRASH_DIR        "/ext/text_editor/deleted files"

// Centralized UI constants
#define BROWSER_MAX_TEXT_W 124
#define EDITOR_LEFT_MARGIN 6
#define KEYBOARD_PREVIEW_W 112
#define STATUS_TIMEOUT_MS 1500
#define BROWSER_MENU_COUNT 5
#define CRYPTO_MENU_COUNT 3

typedef enum {
    View_Splash,   // show XBM until a directional key is pressed
    View_Browser,
    View_Editor,
    View_Keyboard,
    View_FileInfo,
} ViewMode;

typedef struct {
    char lines[MAX_LINES][MAX_LINE_LEN];
    uint16_t line_count;
    uint16_t cursor_line;
    uint16_t cursor_col;
    uint16_t scroll;   // vertical scroll (line index)
    uint16_t hscroll;  // horizontal scroll (column index)
} TextBuffer;

typedef struct {
    ViewMode view;
    bool running;

    Gui* gui;
    ViewPort* vp;
    FuriMessageQueue* input_queue;
    Storage* storage;

    char current_dir[MAX_PATH_LEN];

    char entries[MAX_ENTRIES][MAX_NAME_LEN];
    bool is_dir[MAX_ENTRIES];
    uint32_t entry_size[MAX_ENTRIES];
    uint16_t entry_count;
    uint16_t selected_entry;
    uint16_t browser_scroll;
    uint16_t browser_name_hscroll;
    uint32_t browser_name_last_tick;

    TextBuffer buf;
    char opened_path[MAX_PATH_LEN];

    // Copy/paste buffer for browser (full path of file to copy)
    char copy_buffer[MAX_PATH_LEN];
    bool copy_pending;
    char last_deleted_original[MAX_PATH_LEN];
    char last_deleted_trash[MAX_PATH_LEN];
    bool delete_pending;
    uint32_t trash_serial;

    // Reusable buffers moved out of stack to avoid overflow
    char tmp_name[MAX_NAME_LEN];
    char tmp_path[MAX_PATH_LEN];
    char browser_marquee[MAX_NAME_LEN];
    char tmp_status[64];
    uint8_t io_buffer[IO_CHUNK];
    char crypto_target[MAX_PATH_LEN];
    char crypto_temporary[MAX_PATH_LEN];

    // Keyboard state (compact)
    bool kb_active;
    uint8_t kb_row;
    uint8_t kb_col;
    uint8_t kb_view_offset;        // leftmost visible column index in keyboard view (cells)
    uint8_t kb_view_visible_cols;  // how many key-cells fit horizontally
    bool kb_lowercase;             // true => insert lowercase letters; false => uppercase

    bool menu_active;
    uint8_t menu_selected;
    bool crypto_menu_active;
    uint8_t crypto_menu_selected;
    bool filename_mode;
    bool rename_mode;
    bool crypto_mode;
    bool crypto_decrypt;
    uint16_t info_name_offset;
    uint16_t info_path_offset;
    bool info_is_dir;
    uint32_t info_size;
    uint32_t info_file_count;
    uint32_t info_folder_count;

    // Safety
    bool readonly;

    // Separate timestamp for transient status messages (fixes persistent status bug)
    uint32_t status_last_tick;
} App;

// ---------- Compact keyboard tokens ----------
static const char* kb_rows[KB_ROWS][KB_COLS_MAX] = {
    { "1","2","3","4","5","6","7","8","9","0","-","=" , NULL },
    { "Q","W","E","R","T","Y","U","I","O","P","[","]", NULL },
    { "A","S","D","F","G","H","J","K","L",";","'", "\\", NULL },
    { "Z","X","C","V","B","N","M",",",".","/","TAB","IND","DEL","SP","SAVE","NEW", NULL }
};
static const uint8_t kb_row_lens[KB_ROWS] = { 12, 12, 13, 17 };

// --- Splash XBM (128x64) ---
#define SPLASH_WIDTH 128
#define SPLASH_HEIGHT 64
static const uint8_t splash_bits[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x80, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x1F, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0xC0, 0x3F, 0x00, 0x00, 0x00, 0xC0, 0xFF, 0x07, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0xC0, 0x7F, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x78, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x7F, 0x00, 0x00, 
  0x00, 0x3C, 0x00, 0xC0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0xF0, 0x7F, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0x00, 0x80, 0x0D, 0x00, 0x00, 
  0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0x00, 0x00, 
  0xC0, 0x07, 0x1E, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0xFC, 0xFF, 0x01, 0x00, 0xE0, 0x87, 0x31, 0x00, 0x10, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0xFC, 0xFF, 0x01, 0x00, 0xB0, 0xC3, 0x7C, 0x00, 
  0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0xFD, 0x03, 0x00, 
  0xD0, 0x43, 0x7E, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0xF6, 0xFE, 0x03, 0x00, 0xE8, 0x61, 0x7E, 0x00, 0x20, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x06, 0xF8, 0x03, 0x00, 0xF8, 0x21, 0x7F, 0x40, 
  0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x80, 0x03, 0x00, 
  0xF4, 0x21, 0x7F, 0x18, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 
  0x41, 0x18, 0x02, 0x00, 0xF4, 0xE1, 0x7F, 0x0E, 0x80, 0x1F, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x80, 0xE0, 0x18, 0x00, 0x00, 0xFA, 0x01, 0x00, 0x03, 
  0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x18, 0x00, 0x00, 
  0xFA, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x0C, 0x00, 0xFB, 0x00, 0xC0, 0x00, 0x00, 0x20, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0xFD, 0x00, 0x80, 0x07, 
  0x00, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x38, 0x60, 0x18, 0x00, 
  0x7D, 0x00, 0x00, 0xFE, 0xFF, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 
  0xF0, 0x7B, 0x3C, 0x00, 0x7F, 0x00, 0x00, 0xFC, 0x7F, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x7C, 0xC0, 0x13, 0xFC, 0x00, 0x7F, 0x00, 0x00, 0x98, 
  0xDF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x06, 0xFC, 0x01, 
  0x7F, 0x00, 0x00, 0x70, 0xA4, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 
  0x00, 0x00, 0xFC, 0x03, 0x7F, 0x00, 0x00, 0xC0, 0x1F, 0x01, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0xFC, 0x07, 0xFF, 0x00, 0x00, 0x0C, 
  0x80, 0x01, 0x80, 0x03, 0x00, 0x00, 0x00, 0xFE, 0x01, 0x00, 0xF6, 0x07, 
  0xFF, 0x00, 0x00, 0x78, 0xC0, 0x00, 0x60, 0x02, 0x00, 0x00, 0x00, 0xFE, 
  0x01, 0x00, 0xF0, 0x07, 0xFF, 0x00, 0x00, 0xC0, 0x3F, 0x00, 0x18, 0x02, 
  0x00, 0x00, 0x00, 0xFF, 0x01, 0x00, 0xF8, 0x07, 0xFF, 0x01, 0x00, 0x00, 
  0x0F, 0x00, 0x04, 0x03, 0x00, 0x00, 0x00, 0xFF, 0x03, 0x00, 0xFC, 0x07, 
  0xFF, 0x07, 0x00, 0x00, 0x1E, 0x00, 0x82, 0x03, 0x00, 0x00, 0x00, 0xFF, 
  0x07, 0x00, 0xC0, 0x0F, 0xFF, 0x0F, 0x00, 0x00, 0x36, 0x80, 0x81, 0x01, 
  0x00, 0x00, 0x00, 0xFF, 0x07, 0x00, 0xC0, 0x0F, 0x7F, 0x38, 0x00, 0x00, 
  0xCC, 0x40, 0xC0, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x0F, 0x00, 0x80, 0x1C, 
  0xFF, 0x60, 0x00, 0x7C, 0x0C, 0x3F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 
  0x3F, 0x00, 0x80, 0x1C, 0xFD, 0x80, 0x00, 0x42, 0x1C, 0x00, 0x40, 0xC0, 
  0x04, 0x00, 0x00, 0xFF, 0x3F, 0x00, 0x40, 0x1C, 0xFD, 0x01, 0x81, 0x41, 
  0x38, 0x00, 0x28, 0xC0, 0x04, 0x00, 0x00, 0xFF, 0x7F, 0x00, 0x40, 0x1C, 
  0xFF, 0x01, 0x42, 0x40, 0xF8, 0x80, 0x2D, 0xC8, 0x05, 0x00, 0x00, 0xFF, 
  0xFF, 0x00, 0x00, 0x1C, 0xFF, 0x03, 0x24, 0x20, 0xF8, 0xE7, 0x17, 0xC8, 
  0x05, 0x00, 0x40, 0xDE, 0xFF, 0x00, 0x00, 0x1C, 0xFD, 0x0F, 0x18, 0x10, 
  0xF0, 0xFF, 0x0F, 0xF8, 0xC7, 0x00, 0xC0, 0xCE, 0xFF, 0x00, 0x20, 0x18, 
  0xFD, 0x1F, 0x80, 0x10, 0xF0, 0xFF, 0x07, 0xF0, 0x3F, 0x01, 0x80, 0xCC, 
  0xFF, 0x00, 0x00, 0x18, 0xFD, 0x7F, 0xC0, 0x08, 0xF0, 0xFF, 0x01, 0xE0, 
  0xFF, 0x00, 0x80, 0xCC, 0xFF, 0xD1, 0x00, 0x18, 0xFD, 0xFF, 0xE0, 0x0B, 
  0xE0, 0x7F, 0x00, 0xF0, 0xFC, 0x01, 0xC0, 0x88, 0xFF, 0xF9, 0x0E, 0x18, 
  0xFA, 0xFF, 0xFB, 0x05, 0xE0, 0x0F, 0x00, 0xC0, 0xEA, 0x03, 0xC0, 0x80, 
  0xFF, 0xF9, 0x07, 0x18, 0xFE, 0xFF, 0xFF, 0x02, 0xE0, 0x00, 0x00, 0xC0, 
  0xF0, 0x1F, 0xC0, 0x02, 0xFF, 0xF9, 0x0F, 0x18, 0xF4, 0xFF, 0xFF, 0x01, 
  0xE0, 0x00, 0x00, 0x00, 0xBA, 0x37, 0x40, 0x02, 0xFF, 0xFD, 0x07, 0x10, 
  0xF8, 0xFF, 0xFF, 0x00, 0xE0, 0x00, 0x00, 0x00, 0xD6, 0x33, 0x00, 0x01, 
  0xFE, 0xFF, 0x07, 0x10, 0xE8, 0xFF, 0x7F, 0x00, 0xC0, 0x00, 0x00, 0x00, 
  0x7F, 0x0B, 0x40, 0x04, 0xFE, 0xF9, 0x03, 0x18, 0xF0, 0xFF, 0x0F, 0x00, 
  0xC0, 0x00, 0x00, 0x80, 0xDD, 0x01, 0xC0, 0x80, 0xFF, 0xF9, 0x03, 0x10, 
  0xD0, 0xFF, 0x07, 0x00, 0xC0, 0x00, 0x00, 0x80, 0xEF, 0x01, 0x00, 0x07, 
  0xFF, 0xFB, 0x03, 0x10, 0xF0, 0xFF, 0x03, 0x00, 0xC0, 0x00, 0x00, 0x30, 
  0xF9, 0x01, 0x00, 0x00, 0xFE, 0xFB, 0x23, 0x18, 0xA0, 0xFF, 0x07, 0x00, 
  0xC0, 0x00, 0x00, 0xE0, 0xFC, 0x03, 0x80, 0x01, 0xFE, 0xFA, 0x05, 0x18, 
  0xA0, 0xFF, 0x07, 0x00, 0x40, 0x00, 0x00, 0xF0, 0x7F, 0x06, 0x00, 0x04, 
  0xFE, 0xF8, 0x01, 0x10, 0x40, 0xFF, 0x17, 0x00, 0x40, 0x00, 0x00, 0xC0, 
  0xF8, 0x0C, 0x00, 0x00, 0xFE, 0xF8, 0x00, 0x1E, 0x80, 0xFE, 0x0F, 0x00, 
  0x40, 0x00, 0x00, 0x00, 0xFE, 0x35, 0x00, 0x00, 0xFE, 0xF8, 0x83, 0x06, 
  0x80, 0xFE, 0x1F, 0x00, 0x40, 0x00, 0x00, 0xE0, 0x37, 0xF8, 0x00, 0x00, 
  0x00, 0x60, 0x20, 0x01, 0x00, 0xFD, 0x3F, 0x00, 0x60, 0x00, 0x00, 0x60, 
  0x07, 0x74, 0x01, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00, 0xFD, 0x7F, 0x00, 
  0x20, 0x00, 0x00, 0xE0, 0x03, 0xF8, 0x01, 0x00, 0xF8, 0x38, 0xF0, 0x00, 
  0x00, 0xFA, 0x7F, 0x00, 0x20, 0x00, 0x00, 0xC0, 0x01, 0xF0, 0x03, 0x00, 
  0x78, 0x78, 0x20, 0x00, 0x00, 0xFA, 0xFF, 0x00, 0x30, 0x00, 0x00, 0xE0, 
  0x01, 0xC0, 0x07, 0x00, 0x78, 0xF8, 0x1C, 0x00, 0x00, 0xF4, 0xFF, 0x00, 
  0x10, 0x00, 0x00, 0xF0, 0x01, 0x80, 0x0D, 0x00, 0x00, 0xF8, 0x00, 0x00, 
  0x00, 0xF4, 0x7F, 0x00, 0x10, 0x00, 0x00, 0xF0, 0x01, 0x80, 0x0F, 0x00, 
  0x00, 0x08, 0x00, 0x00,
};

// ---------- Helpers ----------
static void safe_copy(char* dst, const char* src, size_t max) {
    if(max == 0) return;
    size_t i = 0;
    while(i < max - 1 && src[i] != '\0') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static void safe_append(char* dst, const char* src, size_t max) {
    if(max == 0) return;
    size_t dlen = 0;
    while(dlen < max - 1 && dst[dlen] != '\0') dlen++;
    size_t i = 0;
    while(dlen + i < max - 1 && src[i] != '\0') { dst[dlen + i] = src[i]; i++; }
    dst[dlen + i] = '\0';
}
static size_t safe_strlen(const char* s, size_t max) {
    size_t i = 0;
    while(i < max && s[i] != '\0') i++;
    return i;
}
static bool safe_path_join(char* dst, size_t dst_size, const char* dir, const char* name) {
    if(dst_size == 0) return false;
    dst[0] = '\0';
    size_t dlen = safe_strlen(dir, dst_size);
    if(dlen >= dst_size) { memcpy(dst, dir, dst_size - 1); dst[dst_size - 1] = '\0'; return false; }
    memcpy(dst, dir, dlen);
    size_t pos = dlen;
    if(pos + 1 < dst_size) { if(pos == 0 || dst[pos - 1] != '/') dst[pos++] = '/'; }
    else { dst[dst_size - 1] = '\0'; return false; }
    size_t avail = dst_size - pos - 1;
    size_t nlen = safe_strlen(name, avail + 1);
    if(avail == 0) { dst[dst_size - 1] = '\0'; return false; }
    if(nlen > avail) nlen = avail;
    memcpy(dst + pos, name, nlen);
    dst[pos + nlen] = '\0';
    return (safe_strlen(name, avail + 1) <= avail);
}
static bool is_allowed_edit_path(const char* path); // forward
static bool editor_load_file(App* app, const char* path); // forward
static void browser_load_dir(App* app); // forward

// Centralized allowlist matcher and path lists
static bool allowlist_match(const char* path, const char* const allowed[], size_t count) {
    if(!path) return false;
    for(size_t i = 0; i < count; i++) {
        size_t alen = strlen(allowed[i]);
        if(strcmp(path, allowed[i]) == 0) return true;
        if(strncmp(path, allowed[i], alen) == 0 && (path[alen] == '/' || path[alen] == '\0')) return true;
    }
    return false;
}

static bool is_allowed_browser_path(const char* path) {
    static const char* allowed[] = {
        "/ext",
        "/ext/subghz",
        "/ext/badusb",
        "/ext/text_editor",
        "/ext/ibutton_fuzzer",
        "/ext/ifrfid_fuzzer",
        "/ext/mifare_fuzzer",
        "/ext/apps_data",
        "/ext/apps_data/evil_portal",
        "/ext/apps_data/evil_portal/html",
        "/ext/apps_data/ble_spam"
    };
    return allowlist_match(path, allowed, sizeof(allowed)/sizeof(allowed[0]));
}

static bool is_allowed_edit_path(const char* path) {
    static const char* allowed_dirs[] = {
        "/ext/subghz",
        "/ext/badusb",
        "/ext/text_editor",
        "/ext/ibutton_fuzzer",
        "/ext/ifrfid_fuzzer",
        "/ext/mifare_fuzzer",
        "/ext/apps_data/evil_portal/html",
        "/ext/apps_data/ble_spam"
    };
    return allowlist_match(path, allowed_dirs, sizeof(allowed_dirs)/sizeof(allowed_dirs[0]));
}
static bool is_protected_path(const char* path) {
    if(!path) return false;
    return !is_allowed_edit_path(path);
}
static bool is_under_root(const char* path) {
    if(!path) return false;
    size_t rlen = strlen(ROOT_DIR);
    if(rlen == 0) return false;
    return strncmp(path, ROOT_DIR, rlen) == 0;
}

// ---------- Small centralized helpers ----------
static void app_set_status(App* app, const char* msg) {
    if(!app) return;
    safe_copy(app->tmp_status, msg, sizeof(app->tmp_status));
    app->status_last_tick = furi_get_tick();
    if(app->vp) view_port_update(app->vp);
}

static bool build_child_path(App* app, char* dst, size_t dst_size, const char* parent, const char* name) {
    (void)app;
    return safe_path_join(dst, dst_size, parent, name);
}

// ---------- Text buffer ----------
static void textbuffer_clear(TextBuffer* buf) {
    buf->line_count = 1;
    buf->cursor_line = 0;
    buf->cursor_col = 0;
    buf->scroll = 0;
    buf->hscroll = 0;
    memset(buf->lines, 0, sizeof(buf->lines));
}

// ---------- File helpers ----------
static bool file_exists(Storage* storage, const char* path) {
    if(!storage) return false;
    File* f = storage_file_alloc(storage);
    if(!f) return false;
    bool ok = storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) storage_file_close(f);
    storage_file_free(f);
    return ok;
}

// ---------- File copy helper ----------
static bool file_copy(App* app, const char* src_path, const char* dst_path) {
    if(!app || !app->storage || !src_path || !dst_path) return false;
    Storage* storage = app->storage;
    File* src = storage_file_alloc(storage);
    if(!src) return false;
    if(!storage_file_open(src, src_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(src);
        return false;
    }
    File* dst = storage_file_alloc(storage);
    if(!dst) { storage_file_close(src); storage_file_free(src); return false; }
    if(!storage_file_open(dst, dst_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_close(src); storage_file_free(src);
        storage_file_free(dst);
        return false;
    }

    ssize_t r;
    bool ok = true;
    while((r = storage_file_read(src, app->io_buffer, sizeof(app->io_buffer))) > 0) {
        if(storage_file_write(dst, app->io_buffer, (size_t)r) != (size_t)r) { ok = false; break; }
    }

    storage_file_close(src); storage_file_free(src);
    storage_file_close(dst); storage_file_free(dst);
    if(!ok) {
        // Attempt to remove partial destination if possible
        storage_simply_remove(storage, dst_path);
    }
    return ok;
}

// Portable password-based XTEA counter-mode format used by this app.
static void crypto_key_from_password(const char* password, uint32_t key[4]) {
    static const uint32_t seeds[4] = {0x811C9DC5u, 0xA3B19535u, 0xC2B2AE35u, 0x27D4EB2Fu};
    for(uint8_t k = 0; k < 4; k++) {
        uint32_t hash = seeds[k];
        for(size_t i = 0; password[i] != '\0'; i++) {
            hash ^= (uint8_t)password[i];
            hash *= 16777619u;
            hash ^= (uint32_t)(k + 1u) * 0x9E3779B9u;
        }
        key[k] = hash;
    }
}

static uint32_t crypto_key_check(const uint32_t key[4]) {
    return key[0] ^ key[1] ^ key[2] ^ key[3] ^ 0x54455831u;
}

static void crypto_xtea(uint32_t block[2], const uint32_t key[4]) {
    uint32_t v0 = block[0], v1 = block[1], sum = 0;
    for(uint8_t round = 0; round < 32; round++) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        sum += 0x9E3779B9u;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
    }
    block[0] = v0;
    block[1] = v1;
}

static void crypto_apply_block(uint8_t data[8], const uint32_t key[4], uint32_t counter) {
    uint32_t block[2] = {counter, 0xA5C3F1D7u};
    crypto_xtea(block, key);
    for(uint8_t i = 0; i < sizeof(block); i++) data[i] ^= ((uint8_t*)block)[i];
}

static bool crypto_read_exact(File* file, uint8_t* data, size_t size) {
    return storage_file_read(file, data, size) == size;
}

static bool crypto_process_file(Storage* storage, const char* source, const char* target, const char* password, bool decrypt) {
    if(!storage || !source || !target || !password || password[0] == '\0') return false;
    File* src = storage_file_alloc(storage);
    File* dst = storage_file_alloc(storage);
    if(!src || !dst) { if(src) storage_file_free(src); if(dst) storage_file_free(dst); return false; }
    if(!storage_file_open(src, source, FSAM_READ, FSOM_OPEN_EXISTING) ||
       !storage_file_open(dst, target, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_close(src); storage_file_close(dst);
        storage_file_free(src); storage_file_free(dst);
        return false;
    }

    uint32_t key[4];
    crypto_key_from_password(password, key);
    bool ok = true;
    uint32_t counter = 0;
    uint8_t block[8];

    if(!decrypt) {
        const uint8_t header[16] = { 'T', 'X', 'E', '1', 'X', 'T', 'E', 'A',
            (uint8_t)crypto_key_check(key), (uint8_t)(crypto_key_check(key) >> 8),
            (uint8_t)(crypto_key_check(key) >> 16), (uint8_t)(crypto_key_check(key) >> 24),
            1, 0, 0, 0 };
        if(storage_file_write(dst, header, sizeof(header)) != sizeof(header)) ok = false;
        size_t used = 0;
        while(ok) {
            ssize_t count = storage_file_read(src, block + used, sizeof(block) - used);
            if(count < 0) { ok = false; break; }
            if(count == 0) {
                uint8_t pad = (uint8_t)(sizeof(block) - used);
                while(used < sizeof(block)) block[used++] = pad;
                crypto_apply_block(block, key, counter++);
                if(storage_file_write(dst, block, sizeof(block)) != sizeof(block)) ok = false;
                break;
            }
            used += (size_t)count;
            if(used == sizeof(block)) {
                crypto_apply_block(block, key, counter++);
                if(storage_file_write(dst, block, sizeof(block)) != sizeof(block)) ok = false;
                used = 0;
            }
        }
    } else {
        uint8_t header[16] = {0};
        if(!crypto_read_exact(src, header, sizeof(header)) || memcmp(header, "TXE1XTEA", 8) != 0) ok = false;
        uint32_t stored_check = (uint32_t)header[8] | ((uint32_t)header[9] << 8) | ((uint32_t)header[10] << 16) | ((uint32_t)header[11] << 24);
        if(ok && stored_check != crypto_key_check(key)) ok = false;
        uint8_t previous[8];
        bool have_previous = false;
        while(ok) {
            ssize_t count = storage_file_read(src, block, sizeof(block));
            if(count == 0) break;
            if(count != sizeof(block)) { ok = false; break; }
            crypto_apply_block(block, key, counter++);
            if(have_previous && storage_file_write(dst, previous, sizeof(previous)) != sizeof(previous)) ok = false;
            memcpy(previous, block, sizeof(previous));
            have_previous = true;
        }
        if(ok && have_previous) {
            uint8_t pad = previous[7];
            if(pad == 0 || pad > sizeof(previous)) ok = false;
            else for(uint8_t i = 0; i < pad; i++) if(previous[7 - i] != pad) ok = false;
            if(ok && storage_file_write(dst, previous, sizeof(previous) - pad) != sizeof(previous) - pad) ok = false;
        } else if(ok) ok = false;
    }

    storage_file_close(src); storage_file_free(src);
    storage_file_close(dst); storage_file_free(dst);
    if(!ok) storage_simply_remove(storage, target);
    return ok;
}

// ---------- Browser helpers ----------
static uint16_t browser_max_scroll(App* app) {
    if(app->entry_count <= BROWSER_VISIBLE_LINES) return 0;
    return app->entry_count - BROWSER_VISIBLE_LINES;
}
static void clamp_browser_scroll(App* app) {
    uint16_t max = browser_max_scroll(app);
    if(app->browser_scroll > max) app->browser_scroll = max;
}
static void browser_jump_top(App* app) {
    if(!app) return;
    if(app->entry_count == 0) return;
    app->selected_entry = 0;
    app->browser_scroll = 0;
    view_port_update(app->vp);
}
static void browser_jump_bottom(App* app) {
    if(!app) return;
    if(app->entry_count == 0) return;
    app->selected_entry = app->entry_count - 1;
    if(app->entry_count > BROWSER_VISIBLE_LINES) app->browser_scroll = app->entry_count - BROWSER_VISIBLE_LINES;
    else app->browser_scroll = 0;
    view_port_update(app->vp);
}

// ---------- Browser ----------
static void browser_load_dir(App* app) {
    if(!app || !app->storage) return;
    app->entry_count = 0; app->selected_entry = 0; app->browser_scroll = 0;
    app->browser_name_hscroll = 0;
    File* dir = storage_file_alloc(app->storage);
    if(!dir) return;
    if(!storage_dir_open(dir, app->current_dir)) { storage_file_free(dir); return; }
    FileInfo info;
    // use app->tmp_name and app->tmp_path to avoid stack allocations
    while(storage_dir_read(dir, &info, app->tmp_name, sizeof(app->tmp_name))) {
        if(app->entry_count >= MAX_ENTRIES) break;
        // hide dotfiles and "assets" entries (restore original hidden behavior)
        if(app->tmp_name[0] == '.') continue;
        if(strcmp(app->tmp_name, "assets") == 0) continue;
        if(strcmp(app->tmp_name,".")==0 || strcmp(app->tmp_name,"..")==0) continue;

        // Build full path and check allowlist centrally
        if(!safe_path_join(app->tmp_path, sizeof(app->tmp_path), app->current_dir, app->tmp_name)) continue;
        if(strcmp(app->tmp_path, TRASH_DIR) == 0) continue;
        if(!is_allowed_browser_path(app->tmp_path)) continue;

        safe_copy(app->entries[app->entry_count], app->tmp_name, MAX_NAME_LEN);
        app->is_dir[app->entry_count] = (info.flags & FSF_DIRECTORY) != 0;
        app->entry_size[app->entry_count] = info.size;
        app->entry_count++;
    }
    storage_dir_close(dir); storage_file_free(dir);
    clamp_browser_scroll(app);
    if(app->entry_count && app->selected_entry >= app->entry_count) app->selected_entry = app->entry_count - 1;
}
static void browser_go_up(App* app) {
    if(!app) return;
    if(strcmp(app->current_dir, ROOT_DIR) == 0) { app->running = false; return; }
    char* slash = strrchr(app->current_dir, '/');
    if(slash && slash != app->current_dir) *slash = '\0';
    else safe_copy(app->current_dir, ROOT_DIR, MAX_PATH_LEN);
    browser_load_dir(app);
}

// ---------- Browser copy/paste helpers ----------
static void browser_start_copy(App* app) {
    if(!app) return;
    if(app->entry_count == 0) return;
    if(app->is_dir[app->selected_entry]) return; // only files
    // build path into app->copy_buffer using tmp_path to avoid stack usage
    if(!build_child_path(app, app->tmp_path, sizeof(app->tmp_path), app->current_dir, app->entries[app->selected_entry])) return;
    safe_copy(app->copy_buffer, app->tmp_path, sizeof(app->copy_buffer));
    app->copy_pending = true;
    // set transient status (use separate status timestamp)
    app_set_status(app, "Copied");
    view_port_update(app->vp);
}

static int browser_validate_paste(App* app, const char* src, const char* dst) {
    if(!app || !src || !dst) return -1;
    if(file_exists(app->storage, dst)) return 1; // exists
    if(!is_under_root(src) || is_protected_path(src) || is_protected_path(dst)) return 2; // blocked
    if(!file_exists(app->storage, src)) return 3; // source missing
    return 0; // ok
}

static void browser_paste_copy(App* app) {
    if(!app || !app->storage) return;
    if(!app->copy_pending) return;
    const char* src = app->copy_buffer;
    const char* slash = strrchr(src, '/');
    const char* filename = slash ? slash + 1 : src;
    if(!filename || filename[0] == '\0') return;

    // build dst path into app->tmp_path
    app->tmp_path[0] = '\0';
    if(!safe_path_join(app->tmp_path, sizeof(app->tmp_path), app->current_dir, filename)) return;

    // Validate paste using helper
    int v = browser_validate_paste(app, src, app->tmp_path);
    if(v == 1) { app_set_status(app, "Paste failed: exists"); return; }
    if(v == 2) { app_set_status(app, "Paste blocked"); return; }
    if(v == 3) { app_set_status(app, "Source missing"); return; }

    // Perform copy
    if(file_copy(app, src, app->tmp_path)) {
        // success: reload directory to show new file
        browser_load_dir(app);
        // clear buffer
        app->copy_buffer[0] = '\0';
        app->copy_pending = false;
        app_set_status(app, "Paste OK");
        view_port_update(app->vp);
    } else {
        // copy failed; keep buffer and update view
        app_set_status(app, "Paste failed");
        view_port_update(app->vp);
    }
}

static bool browser_crypto_selected(App* app, const char* password, bool decrypt) {
    if(!app || !app->storage || !password || password[0] == '\0' || app->entry_count == 0 ||
       app->is_dir[app->selected_entry] || is_protected_path(app->current_dir)) return false;
    if(!build_child_path(app, app->tmp_path, sizeof(app->tmp_path), app->current_dir, app->entries[app->selected_entry])) return false;
    char* target = app->crypto_target;
    if(decrypt) {
        size_t source_len = safe_strlen(app->tmp_path, sizeof(app->tmp_path));
        if(source_len < 4 || strcmp(app->tmp_path + source_len - 4, ".enc") != 0) return false;
        safe_copy(target, app->tmp_path, sizeof(app->crypto_target));
        target[source_len - 4] = '\0';
    } else {
        if(safe_strlen(app->tmp_path, sizeof(app->tmp_path)) + 4 >= sizeof(app->crypto_target)) return false;
        safe_copy(target, app->tmp_path, sizeof(app->crypto_target));
        safe_append(target, ".enc", sizeof(app->crypto_target));
    }
    if(!decrypt && file_exists(app->storage, target)) return false;
    if(decrypt) {
        char* temporary = app->crypto_temporary;
        if(safe_strlen(target, sizeof(app->crypto_target)) + 4 >= sizeof(app->crypto_temporary)) return false;
        safe_copy(temporary, target, sizeof(app->crypto_temporary));
        safe_append(temporary, ".tmp", sizeof(app->crypto_temporary));
        if(file_exists(app->storage, temporary)) storage_simply_remove(app->storage, temporary);
        if(!crypto_process_file(app->storage, app->tmp_path, temporary, password, true)) return false;
        if(!file_copy(app, temporary, target)) {
            storage_simply_remove(app->storage, temporary);
            return false;
        }
        storage_simply_remove(app->storage, temporary);
    } else {
        if(!crypto_process_file(app->storage, app->tmp_path, target, password, false)) return false;
        if(!storage_simply_remove(app->storage, app->tmp_path)) {
            storage_simply_remove(app->storage, target);
            return false;
        }
    }
    if(!file_exists(app->storage, target)) return false;
    browser_load_dir(app);
    const char* slash = strrchr(target, '/');
    const char* target_name = slash ? slash + 1 : target;
    for(uint16_t i = 0; i < app->entry_count; i++) {
        if(strcmp(app->entries[i], target_name) == 0) {
            app->selected_entry = i;
            app->browser_scroll = (i >= BROWSER_VISIBLE_LINES) ? i - BROWSER_VISIBLE_LINES + 1 : 0;
            break;
        }
    }
    return true;
}

// ---------- Editor load/save ----------
// Streaming file load to avoid large heap allocations
static bool editor_load_file(App* app, const char* path) {
    if(!app || !app->storage || !path) return false;
    File* f = storage_file_alloc(app->storage);
    if(!f) return false;
    if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) { storage_file_free(f); return false; }

    // Initialize buffer
    textbuffer_clear(&app->buf);

    uint8_t tmp[IO_CHUNK];
    ssize_t r;
    uint16_t line = 0;
    uint16_t col = 0;

    while((r = storage_file_read(f, tmp, sizeof(tmp))) > 0) {
        for(ssize_t i = 0; i < r; i++) {
            char c = (char)tmp[i];
            if(c == '\r') continue;
            if(c == '\n') {
                app->buf.lines[line][col] = '\0';
                line++;
                col = 0;
                if(line >= MAX_LINES) { line = MAX_LINES - 1; goto done_read; }
            } else {
                if(col < MAX_LINE_LEN - 1) {
                    app->buf.lines[line][col++] = c;
                } // else drop extra chars on this line
            }
        }
    }
done_read:
    // finalize current line
    app->buf.lines[line][col] = '\0';
    app->buf.line_count = (uint16_t)(line + 1);
    if(app->buf.line_count == 0) app->buf.line_count = 1;
    if(app->buf.cursor_line >= app->buf.line_count) app->buf.cursor_line = app->buf.line_count - 1;
    if(app->buf.scroll + VISIBLE_LINES > app->buf.line_count) {
        if(app->buf.line_count > VISIBLE_LINES) app->buf.scroll = app->buf.line_count - VISIBLE_LINES;
        else app->buf.scroll = 0;
    }
    size_t cur_len = safe_strlen(app->buf.lines[app->buf.cursor_line], MAX_LINE_LEN);
    if(app->buf.cursor_col > cur_len) app->buf.cursor_col = (uint16_t)cur_len;
    app->buf.hscroll = 0;

    storage_file_close(f);
    storage_file_free(f);
    return true;
}
static bool editor_write_file(App* app, const char* path) {
    if(!app || !app->storage || !path) return false;
    if(is_protected_path(path)) return false;

    File* f = storage_file_alloc(app->storage);
    if(!f) return false;
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(f);
        return false;
    }

    for(uint16_t li = 0; li < app->buf.line_count; li++) {
        const char* line = app->buf.lines[li];
        if(line) {
            size_t len = safe_strlen(line, MAX_LINE_LEN);
            if(len) storage_file_write(f, (const uint8_t*)line, len);
        }
        if(li + 1 < app->buf.line_count) {
            const uint8_t nl = '\n';
            storage_file_write(f, &nl, 1);
        }
    }

    storage_file_close(f);
    storage_file_free(f);
    return true;
}

static bool editor_save_current(App* app) {
    if(!app || !app->storage) return false;
    if(app->opened_path[0] == '\0') return false;
    if(is_protected_path(app->opened_path)) return false;
    return editor_write_file(app, app->opened_path);
}

static bool editor_save_as_new(App* app) {
    if(!app || !app->storage) return false;
    if(app->opened_path[0] == '\0') return false;
    if(is_protected_path(app->opened_path) || is_protected_path(app->current_dir)) return false;
    char base[MAX_NAME_LEN]; const char* slash = strrchr(app->opened_path, '/');
    safe_copy(base, slash ? slash + 1 : app->opened_path, MAX_NAME_LEN);
    char name_only[MAX_NAME_LEN]; safe_copy(name_only, base, MAX_NAME_LEN);
    char* dot = strrchr(name_only, '.'); char ext[32] = {0};
    if(dot) { safe_copy(ext, dot, sizeof(ext)); *dot = '\0'; }
    char new_path[MAX_PATH_LEN];
    for(int i = 1; i < 1000; i++) {
        if(!safe_path_join(new_path, sizeof(new_path), app->current_dir, name_only)) safe_copy(new_path, app->current_dir, sizeof(new_path));
        char suffix[64]; snprintf(suffix, sizeof(suffix), "_edit%d", i);
        if(safe_strlen(new_path, sizeof(new_path)) + safe_strlen(suffix, sizeof(suffix)) + safe_strlen(ext, sizeof(ext)) + 1 >= sizeof(new_path)) continue;
        safe_append(new_path, suffix, sizeof(new_path)); safe_append(new_path, ext, sizeof(new_path));
        if(!file_exists(app->storage, new_path) && editor_write_file(app, new_path)) {
            return true;
        }
    }
    return false;
}

// ---------- Create new files in browser ----------
static bool browser_create_new_generic(App* app, const char* fmt_ext) {
    if(!app || !app->storage || !fmt_ext) return false;
    if(is_protected_path(app->current_dir)) return false;

    // Use app buffers instead of heap allocations
    char *new_name = app->tmp_name;
    char *new_path = app->tmp_path;

    bool result = false;
    for(int i = 1; i < 1000; i++) {
        snprintf(new_name, MAX_NAME_LEN, "Created_file%d.%s", i, fmt_ext);
        if(!safe_path_join(new_path, MAX_PATH_LEN, app->current_dir, new_name)) continue;
        if(!file_exists(app->storage, new_path)) {
            File* f = storage_file_alloc(app->storage);
            if(!f) { result = false; break; }
            if(!storage_file_open(f, new_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                storage_file_free(f);
                result = false;
                break;
            }
            storage_file_close(f);
            storage_file_free(f);
            // Open the new file in editor
            if(editor_load_file(app, new_path)) {
                safe_copy(app->opened_path, new_path, sizeof(app->opened_path));
                app->view = View_Editor;
                app->buf.cursor_line = 0; app->buf.cursor_col = 0; app->buf.scroll = 0; app->buf.hscroll = 0;
                app->kb_active = false; app->kb_row = 0; app->kb_col = 0; app->kb_view_offset = 0; app->kb_view_visible_cols = 12;
                app->readonly = is_protected_path(new_path);
                result = true;
            } else result = false;
            break;
        }
    }

    return result;
}

static bool browser_create_new_file(App* app) {
    return browser_create_new_generic(app, "txt");
}

static bool browser_create_named_file(App* app) {
    if(!app || !app->storage || is_protected_path(app->current_dir)) return false;

    const char* name = app->buf.lines[0];
    size_t name_len = safe_strlen(name, MAX_LINE_LEN);
    if(name_len == 0 || name_len >= MAX_NAME_LEN) return false;
    if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    if(strchr(name, '/') || strchr(name, '\\')) return false;

    if(!safe_path_join(app->tmp_path, sizeof(app->tmp_path), app->current_dir, name)) return false;
    if(file_exists(app->storage, app->tmp_path)) return false;

    File* f = storage_file_alloc(app->storage);
    if(!f) return false;
    bool created = storage_file_open(f, app->tmp_path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(created) storage_file_close(f);
    storage_file_free(f);
    if(!created || !editor_load_file(app, app->tmp_path)) return false;

    safe_copy(app->opened_path, app->tmp_path, sizeof(app->opened_path));
    app->view = View_Editor;
    app->buf.cursor_line = 0;
    app->buf.cursor_col = 0;
    app->buf.scroll = 0;
    app->buf.hscroll = 0;
    app->kb_active = false;
    app->filename_mode = false;
    app->readonly = is_protected_path(app->tmp_path);
    return true;
}

static bool browser_rename_selected(App* app) {
    if(!app || !app->storage || app->entry_count == 0) return false;
    if(is_protected_path(app->current_dir)) return false;

    const char* name = app->buf.lines[0];
    size_t name_len = safe_strlen(name, MAX_LINE_LEN);
    if(name_len == 0 || name_len >= MAX_NAME_LEN) return false;
    if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    if(strchr(name, '/') || strchr(name, '\\')) return false;
    if(strcmp(name, app->entries[app->selected_entry]) == 0) return false;

    char old_path[MAX_PATH_LEN];
    if(!safe_path_join(old_path, sizeof(old_path), app->current_dir, app->entries[app->selected_entry])) return false;
    if(!is_allowed_edit_path(old_path)) return false;
    if(!safe_path_join(app->tmp_path, sizeof(app->tmp_path), app->current_dir, name)) return false;
    if(file_exists(app->storage, app->tmp_path)) return false;

    if(!storage_common_rename(app->storage, old_path, app->tmp_path)) return false;
    browser_load_dir(app);
    return true;
}

static void browser_get_directory_stats(
    App* app,
    const char* path,
    uint32_t* file_count,
    uint32_t* folder_count,
    uint32_t* total_size) {
    if(!app || !path || !file_count || !folder_count || !total_size) return;

    File* dir = storage_file_alloc(app->storage);
    if(!dir || !storage_dir_open(dir, path)) {
        if(dir) storage_file_free(dir);
        return;
    }

    FileInfo info;
    char name[MAX_NAME_LEN];
    while(storage_dir_read(dir, &info, name, sizeof(name))) {
        if(name[0] == '.' || strcmp(name, "assets") == 0) continue;
        if(info.flags & FSF_DIRECTORY) {
            (*folder_count)++;
        } else {
            (*file_count)++;
            *total_size += info.size;
        }
    }

    storage_dir_close(dir);
    storage_file_free(dir);
}

static void browser_prepare_trash(App* app) {
    if(!app || !app->storage) return;
    storage_common_mkdir(app->storage, TRASH_DIR);
}

// Move a file into the session trash so the latest deletion can be undone.
static bool browser_delete_selected_file(App* app) {
    if(!app || !app->storage) return false;
    if(app->entry_count == 0) return false;
    if(app->is_dir[app->selected_entry]) return false; // don't delete directories
    app->tmp_path[0] = '\0';
    if(!build_child_path(app, app->tmp_path, sizeof(app->tmp_path), app->current_dir, app->entries[app->selected_entry])) return false;
    if(!is_under_root(app->tmp_path)) return false;
    if(is_protected_path(app->tmp_path)) return false;
    browser_prepare_trash(app);
    safe_copy(app->last_deleted_original, app->tmp_path, sizeof(app->last_deleted_original));
    for(uint32_t attempt = 0; attempt < 1000; attempt++) {
        app->trash_serial++;
        snprintf(app->tmp_name, sizeof(app->tmp_name), "%lu_%s", (unsigned long)app->trash_serial, app->entries[app->selected_entry]);
        if(!safe_path_join(app->last_deleted_trash, sizeof(app->last_deleted_trash), TRASH_DIR, app->tmp_name)) return false;
        if(!file_exists(app->storage, app->last_deleted_trash)) break;
    }
    if(file_exists(app->storage, app->last_deleted_trash)) return false;
    if(file_copy(app, app->last_deleted_original, app->last_deleted_trash) &&
       storage_simply_remove(app->storage, app->last_deleted_original)) {
        app->delete_pending = true;
        browser_load_dir(app);
        return true;
    }
    storage_simply_remove(app->storage, app->last_deleted_trash);
    return false;
}

static bool browser_undo_delete(App* app) {
    if(!app || !app->storage || app->last_deleted_trash[0] == '\0' ||
       app->last_deleted_original[0] == '\0') return false;
    if(!file_exists(app->storage, app->last_deleted_trash) ||
       file_exists(app->storage, app->last_deleted_original)) return false;
    if(!file_copy(app, app->last_deleted_trash, app->last_deleted_original)) return false;
    if(!storage_simply_remove(app->storage, app->last_deleted_trash)) return false;
    app->delete_pending = false;
    browser_load_dir(app);
    return true;
}

static void browser_empty_trash(App* app) {
    if(!app || !app->storage) return;
    File* dir = storage_file_alloc(app->storage);
    if(!dir) return;
    if(storage_dir_open(dir, TRASH_DIR)) {
        FileInfo info;
        char name[MAX_NAME_LEN];
        char path[MAX_PATH_LEN];
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(name[0] == '.' || (info.flags & FSF_DIRECTORY)) continue;
            if(safe_path_join(path, sizeof(path), TRASH_DIR, name)) {
                storage_simply_remove(app->storage, path);
            }
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    storage_common_remove(app->storage, TRASH_DIR);
}

// ---------- Drawing ----------
static void draw_status_message(Canvas* canvas, App* app, uint8_t y) {
    if(!canvas || !app) return;
    if(app->tmp_status[0] == '\0') return;
    canvas_set_font(canvas, FontSecondary);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 0, y - 2, 128, 10);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_str(canvas, 2, y + 4, app->tmp_status);
}

static void draw_browser_line_text(Canvas* canvas, App* app, const char* text, uint8_t x, uint8_t y) {
    const uint16_t max_w = BROWSER_MAX_TEXT_W;
    const uint16_t ellipsis_w = canvas_string_width(canvas, "...");

    safe_copy(app->tmp_path, text, sizeof(app->tmp_path));
    size_t len = safe_strlen(app->tmp_path, sizeof(app->tmp_path));

    if(canvas_string_width(canvas, app->tmp_path) > max_w) {
        while(len > 0) {
            app->tmp_path[len] = '\0';
            if(canvas_string_width(canvas, app->tmp_path) + ellipsis_w <= max_w) break;
            len--;
        }
        if(len == 0) {
            safe_copy(app->tmp_path, "...", sizeof(app->tmp_path));
        } else {
            app->tmp_path[len] = '\0';
            safe_append(app->tmp_path, "...", sizeof(app->tmp_path));
        }
    }

    canvas_draw_str(canvas, x, y, app->tmp_path);
}

static void draw_browser_marquee_text(Canvas* canvas, App* app, const char* text, uint8_t x, uint8_t y) {
    const uint16_t max_w = BROWSER_MAX_TEXT_W;

    app->browser_marquee[0] = '\0';
    size_t len = safe_strlen(text, MAX_NAME_LEN);
    if(len == 0) return;

    if(canvas_string_width(canvas, text) <= max_w) {
        canvas_draw_str(canvas, x, y, text);
        return;
    }

    size_t start = app->browser_name_hscroll;
    if(start >= len) start = len - 1;

    size_t draw_len = 0;
    uint16_t draw_w = 0;
    while(start + draw_len < len) {
        char probe[2] = { text[start + draw_len], '\0' };
        uint16_t probe_w = canvas_string_width(canvas, probe);
        if(draw_w + probe_w > max_w) break;
        draw_w += probe_w;
        draw_len++;
    }

    if(draw_len == 0) {
        safe_copy(app->browser_marquee, "...", sizeof(app->browser_marquee));
        canvas_draw_str(canvas, x, y, app->browser_marquee);
        return;
    }

    memset(app->browser_marquee, 0, sizeof(app->browser_marquee));
    memcpy(app->browser_marquee, text + start, draw_len);
    app->browser_marquee[draw_len] = '\0';
    canvas_draw_str(canvas, x, y, app->browser_marquee);
}

static void browser_advance_marquee(App* app, Canvas* canvas, const char* text) {
    if(!app || !canvas || !text) return;
    safe_copy(app->browser_marquee, text, sizeof(app->browser_marquee));
    size_t len = safe_strlen(app->browser_marquee, sizeof(app->browser_marquee));
    if(len == 0) {
        app->browser_name_hscroll = 0;
        app->browser_name_last_tick = furi_get_tick();
        return;
    }

    if(canvas_string_width(canvas, app->browser_marquee) <= BROWSER_MAX_TEXT_W) {
        app->browser_name_hscroll = 0;
        app->browser_name_last_tick = furi_get_tick();
        return;
    }

    if(app->browser_name_hscroll > len) {
        app->browser_name_hscroll = len;
    }

    uint32_t now = furi_get_tick();
    if(app->browser_name_hscroll >= len) {
        if((now - app->browser_name_last_tick) >= BROWSER_NAME_PAUSE_TICK) {
            app->browser_name_hscroll = 0;
            app->browser_name_last_tick = now;
        }
        return;
    }

    if((now - app->browser_name_last_tick) >= BROWSER_NAME_SCROLL_TICK) {
        if(app->browser_name_hscroll < len) {
            app->browser_name_hscroll++;
        }
        app->browser_name_last_tick = now;
    }
}

static void draw_browser(Canvas* canvas, App* app) {
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    canvas_set_color(canvas, ColorWhite);
    draw_browser_line_text(canvas, app, app->current_dir, 2, 6);
    canvas_set_color(canvas, ColorBlack);

    if(app->entry_count == 0) {
        canvas_draw_str(canvas, 2, 20, "No files");
        draw_status_message(canvas, app, 62);
        return;
    }

    uint8_t y = 10;
    uint16_t start = app->browser_scroll;
    uint16_t end = start + BROWSER_VISIBLE_LINES;
    if(end > app->entry_count) end = app->entry_count;

    for(uint16_t i = start; i < end; i++) {
        app->tmp_name[0] = '\0';
        if(app->is_dir[i]) {
            safe_copy(app->tmp_name, "[", sizeof(app->tmp_name));
            safe_append(app->tmp_name, app->entries[i], sizeof(app->tmp_name));
            safe_append(app->tmp_name, "]", sizeof(app->tmp_name));
        } else {
            safe_copy(app->tmp_name, app->entries[i], sizeof(app->tmp_name));
        }

        if(i == app->selected_entry) {
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_box(canvas, 0, y - 1, 128, BROWSER_LINE_HEIGHT);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_str(canvas, 2, y + 1, ">");
            browser_advance_marquee(app, canvas, app->tmp_name);
            draw_browser_marquee_text(canvas, app, app->tmp_name, 8, y + 1);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
            draw_browser_line_text(canvas, app, app->tmp_name, 2, y + 1);
        }

        y += BROWSER_LINE_HEIGHT;
    }

    // Draw transient status if present (cleared after timeout)
    draw_status_message(canvas, app, 54);
}

// Pixel-accurate editor drawing using canvas_string_width
static void draw_editor(Canvas* canvas, App* app) {
    if(!canvas || !app) return;

    // Use the same font for drawing and measuring
    canvas_set_font(canvas, FontSecondary);

    const uint8_t left_margin = EDITOR_LEFT_MARGIN; // same left offset used for drawing
    uint16_t start = app->buf.scroll;
    uint16_t end = start + VISIBLE_LINES;
    if(end > app->buf.line_count) end = app->buf.line_count;
    uint8_t y = TOP_MARGIN;

    for(uint16_t i = start; i < end; i++) {
        const char* line = app->buf.lines[i];
        const char* draw_line = line ? line : "";
        size_t len = safe_strlen(draw_line, MAX_LINE_LEN);

        // visible substring after horizontal scroll
        const char* visible;
        if(app->buf.hscroll < len) visible = draw_line + app->buf.hscroll;
        else visible = draw_line + len;

        // Draw the visible text
        canvas_draw_str_aligned(canvas, left_margin, y + 2, AlignLeft, AlignCenter, visible);

        // If this is the cursor line, compute pixel X for cursor
        if(i == app->buf.cursor_line) {
            // draw caret marker at left of line
            canvas_draw_str_aligned(canvas, 0, y + 2, AlignLeft, AlignCenter, ">");

            // compute number of characters from visible start to cursor
            uint16_t rel_col = 0;
            if(app->buf.cursor_col > app->buf.hscroll) rel_col = app->buf.cursor_col - app->buf.hscroll;
            else rel_col = 0;

            // Build substring up to rel_col characters into app->tmp_name (safe)
            if(rel_col > 0) {
                size_t copy_len = (size_t)rel_col;
                size_t vis_len = safe_strlen(visible, MAX_LINE_LEN);
                if(copy_len > vis_len) copy_len = vis_len;
                if(copy_len >= sizeof(app->tmp_name)) copy_len = sizeof(app->tmp_name) - 1;
                memcpy(app->tmp_name, visible, copy_len);
                app->tmp_name[copy_len] = '\0';
            } else {
                app->tmp_name[0] = '\0';
            }

            // Measure pixel width of tmp using canvas API
            uint16_t px = canvas_string_width(canvas, app->tmp_name);

            // Final cursor X in pixels
            uint8_t cx = (uint8_t)(left_margin + px);

            // Draw underline cursor
            canvas_draw_line(canvas, cx, y + 5, cx + 4, y + 5);
        }

        y += 10;
    }

    if(app->readonly) canvas_draw_str(canvas, 2, 62, "READ-ONLY");

    // Draw transient status if present
    draw_status_message(canvas, app, 54);
}

// Live text preview for the compact keyboard overlay.
static void draw_keyboard_preview(Canvas* canvas, App* app) {
    canvas_set_font(canvas, FontSecondary);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 0, 0, 128, 12);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_str(canvas, 2, 8, ">");

    const char* line = app->buf.lines[app->buf.cursor_line];
    if(!line) line = "";
    size_t len = safe_strlen(line, MAX_LINE_LEN);

    if(app->buf.cursor_col > len) app->buf.cursor_col = (uint16_t)len;

    const uint16_t preview_w = KEYBOARD_PREVIEW_W;
    if(app->buf.cursor_col >= app->buf.hscroll + MAX_VISIBLE_COLS) {
        app->buf.hscroll = app->buf.cursor_col - (MAX_VISIBLE_COLS - 1);
    } else if(app->buf.cursor_col < app->buf.hscroll) {
        app->buf.hscroll = app->buf.cursor_col;
    }

    size_t left = (size_t)app->buf.hscroll;
    if(left > len) left = len;
    const char* visible = line + left;
    size_t visible_len = len - left;
    if(visible_len >= sizeof(app->tmp_name)) visible_len = sizeof(app->tmp_name) - 1;
    memcpy(app->tmp_name, visible, visible_len);
    app->tmp_name[visible_len] = '\0';

    if(canvas_string_width(canvas, app->tmp_name) > preview_w) {
        size_t trim = visible_len;
        while(trim > 0) {
            app->tmp_name[trim] = '\0';
            if(canvas_string_width(canvas, app->tmp_name) + canvas_string_width(canvas, "...") <= preview_w) break;
            trim--;
        }
        if(trim == 0) safe_copy(app->tmp_name, "...", sizeof(app->tmp_name));
        else safe_append(app->tmp_name, "...", sizeof(app->tmp_name));
    }

    canvas_draw_str(canvas, 8, 8, app->tmp_name);

    uint16_t prefix_w = 0;
    if(app->buf.cursor_col > app->buf.hscroll) {
        size_t prefix_len = (size_t)(app->buf.cursor_col - app->buf.hscroll);
        if(prefix_len >= sizeof(app->tmp_path)) prefix_len = sizeof(app->tmp_path) - 1;
        memcpy(app->tmp_path, line + app->buf.hscroll, prefix_len);
        app->tmp_path[prefix_len] = '\0';
        prefix_w = canvas_string_width(canvas, app->tmp_path);
    }

    canvas_draw_line(canvas, 8 + prefix_w - 1, 4, 8 + prefix_w - 1, 10);
}

// Compact keyboard drawing with horizontal panning
static void draw_keyboard(Canvas* canvas, App* app) {
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    draw_keyboard_preview(canvas, app);

    const uint8_t base_x = 2;
    const uint8_t base_y = TOP_MARGIN + 6;
    const uint8_t cell_w = 10;
    const uint8_t cell_y_step = 12;

    for(uint8_t r = 0; r < KB_ROWS; r++) {
        uint8_t cols = kb_row_lens[r];
        uint8_t y = base_y + (r * cell_y_step);

        uint8_t start_col = app->kb_view_offset;
        if(start_col >= cols) start_col = cols;
        uint8_t end_col = app->kb_view_offset + app->kb_view_visible_cols;
        if(end_col > cols) end_col = cols;

        uint8_t draw_index = 0;
        for(uint8_t c = start_col; c < end_col; c++) {
            const char* token = kb_rows[r][c];
            if(!token) { draw_index++; continue; }

            char s[6] = {0};
            if(strcmp(token, "TAB") == 0) strcpy(s, "TB");
            else if(strcmp(token, "IND") == 0) strcpy(s, "IN");
            else if(strcmp(token, "DEL") == 0) strcpy(s, "<");
            else if(strcmp(token, "SAVE") == 0) strcpy(s, "SV");
            else if(strcmp(token, "NEW") == 0) strcpy(s, "NW");
            else if(strcmp(token, "SP") == 0) strcpy(s, "_");
            else {
                char ch = token[0];
                if(isalpha((unsigned char)ch)) {
                    if(app->kb_lowercase) ch = (char)tolower((unsigned char)ch);
                    else ch = (char)toupper((unsigned char)ch);
                }
                s[0] = ch; s[1] = '\0';
            }

            uint8_t x = base_x + (draw_index * cell_w);

            if(r == app->kb_row && c == app->kb_col) {
                canvas_draw_box(canvas, x - 1, y - 7, cell_w + 4, 12);
                canvas_set_color(canvas, ColorWhite);
                canvas_draw_str(canvas, x, y, s);
                canvas_set_color(canvas, ColorBlack);
            } else {
                canvas_draw_str(canvas, x, y, s);
            }

            draw_index++;
        }
    }

    if(app->filename_mode) {
        if(app->rename_mode) canvas_draw_str(canvas, 2, 62, "RENAME  SAVE=apply  BACK=cancel");
        else if(app->crypto_mode) canvas_draw_str(canvas, 2, 62, "PASSWORD  SAVE=apply  BACK=cancel");
        else canvas_draw_str(canvas, 2, 62, "TYPE NAME  SAVE=create  BACK=cancel");
    } else if(app->kb_lowercase) {
        canvas_draw_str(canvas, 2, 62, "OK=Ins  BACK=Close  SAVE=save  IND=lower");
    } else {
        canvas_draw_str(canvas, 2, 62, "OK=Ins  BACK=Close  SAVE=save  IND=UPPER");
    }
}

static void draw_browser_menu(Canvas* canvas, App* app) {
    static const char* items[BROWSER_MENU_COUNT] = {
        "CREATE FILE",
        "NEW TXT FILE",
        "RENAME",
        "UNDO DELETE",
        "REFRESH",
    };

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 12, 2, 104, 60);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, 12, 2, 104, 60);
    canvas_set_font(canvas, FontSecondary);

    for(uint8_t i = 0; i < BROWSER_MENU_COUNT; i++) {
        uint8_t y = (uint8_t)(5 + i * 11);
        if(i == app->menu_selected) {
            canvas_draw_box(canvas, 16, y, 96, 10);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 20, y + 8, items[i]);
        canvas_set_color(canvas, ColorBlack);
    }
}

static void draw_crypto_menu(Canvas* canvas, App* app) {
    static const char* items[CRYPTO_MENU_COUNT] = {"INFO", "ENCRYPT", "DECRYPT"};
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 24, 10, 80, 40);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_frame(canvas, 24, 10, 80, 40);
    canvas_set_font(canvas, FontSecondary);
    for(uint8_t i = 0; i < CRYPTO_MENU_COUNT; i++) {
        uint8_t y = (uint8_t)(14 + i * 12);
        if(i == app->crypto_menu_selected) {
            canvas_draw_box(canvas, 28, y, 72, 10);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 34, y + 8, items[i]);
        canvas_set_color(canvas, ColorBlack);
    }
}

static void format_size_string(char* dst, size_t dst_size, uint32_t size_bytes) {
    if(!dst || dst_size == 0) return;
    if(size_bytes >= 1024 * 1024) {
        snprintf(dst, dst_size, "%lu KB", (unsigned long)(size_bytes / 1024));
    } else {
        snprintf(dst, dst_size, "%lu B", (unsigned long)size_bytes);
    }
}

static const char* file_type_label(const char* name, bool is_dir) {
    if(is_dir) return "DIR";
    if(!name || name[0] == '\0') return "FILE";
    const char* dot = strrchr(name, '.');
    if(!dot || dot == name) return "FILE";
    const char* ext = dot + 1;
    if(strcmp(ext, "txt") == 0 || strcmp(ext, "TXT") == 0) return "TXT";
    if(strcmp(ext, "enc") == 0 || strcmp(ext, "ENC") == 0) return "ENC";
    if(strcmp(ext, "bin") == 0 || strcmp(ext, "BIN") == 0) return "BIN";
    if(strcmp(ext, "json") == 0 || strcmp(ext, "JSON") == 0) return "JSON";
    if(strcmp(ext, "csv") == 0 || strcmp(ext, "CSV") == 0) return "CSV";
    if(strcmp(ext, "md") == 0 || strcmp(ext, "MD") == 0) return "MD";
    if(strcmp(ext, "c") == 0 || strcmp(ext, "h") == 0 || strcmp(ext, "C") == 0 || strcmp(ext, "H") == 0) return "C";
    if(strcmp(ext, "py") == 0 || strcmp(ext, "PY") == 0) return "PY";
    return "FILE";
}

static void draw_file_info(Canvas* canvas, App* app) {
    char name_view[32];
    char path_view[44];
    char size_view[20];
    char contents_view[20];
    const char* type_label = file_type_label(app->tmp_name, app->info_is_dir);
    const bool read_only = app->info_is_dir ? false : is_protected_path(app->tmp_path);

    size_t name_len = safe_strlen(app->tmp_name, sizeof(app->tmp_name));
    size_t path_len = safe_strlen(app->tmp_path, sizeof(app->tmp_path));
    size_t name_start = app->info_name_offset;
    size_t start = app->info_path_offset;
    if(name_start > name_len) name_start = name_len;
    if(start > path_len) start = path_len;
    size_t name_visible_len = name_len - name_start;
    size_t visible_len = path_len - start;
    if(name_visible_len >= sizeof(name_view)) name_visible_len = sizeof(name_view) - 1;
    if(visible_len >= sizeof(path_view)) visible_len = sizeof(path_view) - 1;
    memcpy(name_view, app->tmp_name + name_start, name_visible_len);
    name_view[name_visible_len] = '\0';
    memcpy(path_view, app->tmp_path + start, visible_len);
    path_view[visible_len] = '\0';
    format_size_string(size_view, sizeof(size_view), app->info_size);
    if(app->info_is_dir) {
        snprintf(contents_view, sizeof(contents_view), "%luF %luD",
                 (unsigned long)app->info_file_count,
                 (unsigned long)app->info_folder_count);
    } else {
        safe_copy(contents_view, read_only ? "READONLY" : "READWRITE", sizeof(contents_view));
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 8, "INFO");
    canvas_draw_line(canvas, 2, 10, 124, 10);

    canvas_draw_str(canvas, 2, 20, "NAME");
    canvas_draw_str(canvas, 44, 20, name_view);

    canvas_draw_str(canvas, 2, 30, "TYPE");
    canvas_draw_str(canvas, 44, 30, type_label);

    canvas_draw_str(canvas, 2, 40, app->info_is_dir ? "CONT" : "SIZE");
    canvas_draw_str(canvas, 44, 40, app->info_is_dir ? contents_view : size_view);

    canvas_draw_str(canvas, 2, 50, app->info_is_dir ? "SIZE" : "ACC");
    canvas_draw_str(canvas, 44, 50, app->info_is_dir ? size_view : contents_view);

    canvas_draw_str(canvas, 2, 58, "PATH");
    canvas_draw_str(canvas, 44, 58, path_view);

    draw_status_message(canvas, app, 62);
}

// Draw the splash bitmap centered on the 128x64 screen.
// Interprets XBM bytes (LSB-first per byte).
static void draw_splash(Canvas* canvas, App* app) {
    (void)app;
    canvas_clear(canvas);

    const int x0 = (128 - SPLASH_WIDTH) / 2;
    const int y0 = (64 - SPLASH_HEIGHT) / 2;
    const int bytes_per_row = (SPLASH_WIDTH + 7) / 8;

    for(int row = 0; row < SPLASH_HEIGHT; row++) {
        for(int b = 0; b < bytes_per_row; b++) {
            uint8_t byte = splash_bits[row * bytes_per_row + b];
            for(int bit = 0; bit < 8; bit++) {
                int col = b * 8 + bit;
                if(col >= SPLASH_WIDTH) break;
                bool pixel_on = (byte >> bit) & 1;
                if(pixel_on) {
                    // draw a single pixel as a 1x1 box (works with your canvas API)
                    canvas_draw_box(canvas, x0 + col, y0 + row, 1, 1);
                }
            }
        }
    }
}

// ---------- Horizontal scroll helper ----------
static void ensure_hscroll(App* app) {
    if(!app) return;
    if(app->buf.cursor_col < app->buf.hscroll) app->buf.hscroll = app->buf.cursor_col;
    else if(app->buf.cursor_col >= app->buf.hscroll + MAX_VISIBLE_COLS) app->buf.hscroll = app->buf.cursor_col - (MAX_VISIBLE_COLS - 1);
}

// ---------- Editing helpers ----------
static void editor_insert_char(App* app, char ch) {
    if(!app || app->readonly) return;
    if(app->buf.cursor_line >= app->buf.line_count) return;
    char* line = app->buf.lines[app->buf.cursor_line];
    size_t len = safe_strlen(line, MAX_LINE_LEN);
    if(len >= MAX_LINE_LEN - 1) return;
    for(int i = (int)len; i >= (int)app->buf.cursor_col; i--) line[i + 1] = line[i];
    line[app->buf.cursor_col] = ch;
    app->buf.cursor_col++;
    if(app->buf.cursor_line < app->buf.scroll) app->buf.scroll = app->buf.cursor_line;
    if(app->buf.cursor_line >= app->buf.scroll + VISIBLE_LINES) app->buf.scroll = app->buf.cursor_line - (VISIBLE_LINES - 1);
    ensure_hscroll(app);
}
static void editor_backspace(App* app) {
    if(!app || app->readonly) return;
    if(app->buf.cursor_line >= app->buf.line_count) return;
    char* line = app->buf.lines[app->buf.cursor_line];
    size_t len = safe_strlen(line, MAX_LINE_LEN);
    if(app->buf.cursor_col > 0) {
        for(size_t i = app->buf.cursor_col - 1; i < len; i++) line[i] = line[i + 1];
        app->buf.cursor_col--;
    }
    ensure_hscroll(app);
}

void editor_newline(App* app) {
    if(!app || app->readonly) return;
    if(app->buf.line_count >= MAX_LINES) return;
    if(app->buf.cursor_line >= app->buf.line_count) return;
    for(int i = (int)app->buf.line_count; i > (int)app->buf.cursor_line + 1; i--) safe_copy(app->buf.lines[i], app->buf.lines[i - 1], MAX_LINE_LEN);
    char* line = app->buf.lines[app->buf.cursor_line];
    char* next = app->buf.lines[app->buf.cursor_line + 1];
    size_t col = app->buf.cursor_col;
    safe_copy(next, line + col, MAX_LINE_LEN);
    line[col] = '\0';
    app->buf.line_count++;
    app->buf.cursor_line++;
    app->buf.cursor_col = 0;
    app->buf.hscroll = 0;
    if(app->buf.cursor_line >= app->buf.scroll + VISIBLE_LINES) app->buf.scroll = app->buf.cursor_line - (VISIBLE_LINES - 1);
}

bool editor_delete_line_if_blank(App* app) {
    if(!app) return false;
    if(app->buf.cursor_line >= app->buf.line_count) return false;
    char* line = app->buf.lines[app->buf.cursor_line];
    size_t len = safe_strlen(line, MAX_LINE_LEN);
    if(len != 0) return false;
    if(app->buf.line_count == 1) { app->buf.cursor_col = 0; app->buf.scroll = 0; app->buf.hscroll = 0; return true; }
    for(uint16_t i = app->buf.cursor_line; i + 1 < app->buf.line_count; i++) safe_copy(app->buf.lines[i], app->buf.lines[i + 1], MAX_LINE_LEN);
    app->buf.lines[app->buf.line_count - 1][0] = '\0';
    app->buf.line_count--;
    if(app->buf.cursor_line >= app->buf.line_count) app->buf.cursor_line = app->buf.line_count - 1;
    size_t newlen = safe_strlen(app->buf.lines[app->buf.cursor_line], MAX_LINE_LEN);
    if(app->buf.cursor_col > newlen) app->buf.cursor_col = (uint16_t)newlen;
    if(app->buf.cursor_line < app->buf.scroll) app->buf.scroll = app->buf.cursor_line;
    if(app->buf.scroll + VISIBLE_LINES > app->buf.line_count) {
        if(app->buf.line_count > VISIBLE_LINES) app->buf.scroll = app->buf.line_count - VISIBLE_LINES;
        else app->buf.scroll = 0;
    }
    ensure_hscroll(app);
    return true;
}

static bool editor_delete_current_line(App* app) {
    if(!app) return false;
    if(app->buf.cursor_line >= app->buf.line_count) return false;
    if(app->buf.line_count == 1) {
        app->buf.lines[0][0] = '\0';
        app->buf.cursor_col = 0;
        app->buf.hscroll = 0;
        app->buf.scroll = 0;
        return true;
    }
    for(uint16_t i = app->buf.cursor_line; i + 1 < app->buf.line_count; i++) safe_copy(app->buf.lines[i], app->buf.lines[i + 1], MAX_LINE_LEN);
    app->buf.lines[app->buf.line_count - 1][0] = '\0';
    app->buf.line_count--;
    if(app->buf.cursor_line >= app->buf.line_count) app->buf.cursor_line = app->buf.line_count - 1;
    size_t newlen = safe_strlen(app->buf.lines[app->buf.cursor_line], MAX_LINE_LEN);
    if(app->buf.cursor_col > newlen) app->buf.cursor_col = (uint16_t)newlen;
    if(app->buf.cursor_line < app->buf.scroll) app->buf.scroll = app->buf.cursor_line;
    if(app->buf.scroll + VISIBLE_LINES > app->buf.line_count) {
        if(app->buf.line_count > VISIBLE_LINES) app->buf.scroll = app->buf.line_count - VISIBLE_LINES;
        else app->buf.scroll = 0;
    }
    ensure_hscroll(app);
    return true;
}

static bool editor_insert_empty_line_below(App* app) {
    if(!app || app->readonly) return false;
    if(app->buf.line_count >= MAX_LINES) return false;
    uint16_t insert_at = app->buf.cursor_line + 1;
    for(int i = (int)app->buf.line_count; i > (int)insert_at; i--) safe_copy(app->buf.lines[i], app->buf.lines[i - 1], MAX_LINE_LEN);
    app->buf.lines[insert_at][0] = '\0';
    app->buf.line_count++;
    app->buf.cursor_line = insert_at;
    app->buf.cursor_col = 0;
    if(app->buf.cursor_line >= app->buf.scroll + VISIBLE_LINES) app->buf.scroll = app->buf.cursor_line - (VISIBLE_LINES - 1);
    ensure_hscroll(app);
    return true;
}

// ---------- Input handlers ----------
// Splash input: only directional keys (Up/Down/Left/Right) will exit the splash.
// OK/select is ignored while splash is visible.
static void input_splash(App* app, InputEvent* e) {
    if(!app || !e) return;

    // Only respond to directional keys (short or long).
    if((e->key == InputKeyUp ||
        e->key == InputKeyDown ||
        e->key == InputKeyLeft ||
        e->key == InputKeyRight) &&
       (e->type == InputTypeShort || e->type == InputTypeLong || e->type == InputTypeRepeat)) {
        // Switch to browser view. browser_load_dir was already called in app_alloc,
        // so just update the view and redraw.
        app->view = View_Browser;
        view_port_update(app->vp);
    }

    // All other keys (including InputKeyOk) are ignored while splash is visible.
    // Do not forward the event to other handlers.
}

static void input_file_info(App* app, InputEvent* e) {
    if(!app || !e) return;
    if((e->type == InputTypeShort || e->type == InputTypeRepeat) && e->key == InputKeyLeft) {
        if(app->info_name_offset > 0) app->info_name_offset--;
        if(app->info_path_offset > 0) app->info_path_offset--;
        view_port_update(app->vp);
        return;
    }
    if((e->type == InputTypeShort || e->type == InputTypeRepeat) && e->key == InputKeyRight) {
        size_t name_len = safe_strlen(app->tmp_name, sizeof(app->tmp_name));
        size_t path_len = safe_strlen(app->tmp_path, sizeof(app->tmp_path));
        if((size_t)app->info_name_offset + 1U < name_len) app->info_name_offset++;
        if((size_t)app->info_path_offset + 1U < path_len) app->info_path_offset++;
        view_port_update(app->vp);
        return;
    }
    if(e->key == InputKeyBack && (e->type == InputTypeShort || e->type == InputTypeLong)) {
        app->view = View_Browser;
        browser_load_dir(app);
        view_port_update(app->vp);
    }
}

static void input_browser(App* app, InputEvent* e) {
    if(!app || !e) return;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat && e->type != InputTypeLong) return;

    if(app->crypto_menu_active) {
        if(e->type == InputTypeShort || e->type == InputTypeRepeat) {
            if(e->key == InputKeyUp || e->key == InputKeyDown) {
                app->crypto_menu_selected = (uint8_t)((app->crypto_menu_selected + 1) % CRYPTO_MENU_COUNT);
                view_port_update(app->vp);
                return;
            }
            if(e->key == InputKeyBack) {
                app->crypto_menu_active = false;
                view_port_update(app->vp);
                return;
            }
            if(e->key == InputKeyOk) {
                if(app->crypto_menu_selected == 0) {
                    if(app->entry_count == 0) {
                        app_set_status(app, "No item selected");
                    } else {
                        app->tmp_name[0] = '\0';
                        app->tmp_path[0] = '\0';
                        safe_copy(app->tmp_name, app->entries[app->selected_entry], sizeof(app->tmp_name));
                        if(!build_child_path(app, app->tmp_path, sizeof(app->tmp_path), app->current_dir, app->entries[app->selected_entry])) {
                            app_set_status(app, "Path error");
                        } else {
                            app->info_is_dir = app->is_dir[app->selected_entry];
                            app->info_size = app->entry_size[app->selected_entry];
                            app->info_file_count = 0;
                            app->info_folder_count = 0;
                            if(app->info_is_dir) {
                                browser_get_directory_stats(
                                    app,
                                    app->tmp_path,
                                    &app->info_file_count,
                                    &app->info_folder_count,
                                    &app->info_size);
                            }
                            app->info_name_offset = 0;
                            app->info_path_offset = 0;
                            app->view = View_FileInfo;
                        }
                    }
                } else if(app->entry_count == 0 || app->is_dir[app->selected_entry]) {
                    app_set_status(app, "Select a file");
                } else {
                    textbuffer_clear(&app->buf);
                    app->filename_mode = true;
                    app->rename_mode = false;
                    app->crypto_mode = true;
                    app->crypto_decrypt = (app->crypto_menu_selected == 2);
                    app->kb_active = true;
                    app->kb_row = 0;
                    app->kb_col = 0;
                    app->kb_view_offset = 0;
                    app->kb_view_visible_cols = 12;
                }
                app->crypto_menu_active = false;
                view_port_update(app->vp);
                return;
            }
        }
        return;
    }

    if(app->menu_active) {
        if(e->type == InputTypeShort || e->type == InputTypeRepeat) {
            if(e->key == InputKeyUp) {
                if(app->menu_selected == 0) app->menu_selected = BROWSER_MENU_COUNT - 1;
                else app->menu_selected--;
                view_port_update(app->vp);
                return;
            }
            if(e->key == InputKeyDown) {
                app->menu_selected = (uint8_t)((app->menu_selected + 1) % BROWSER_MENU_COUNT);
                view_port_update(app->vp);
                return;
            }
            if(e->key == InputKeyBack) {
                app->menu_active = false;
                view_port_update(app->vp);
                return;
            }
            if(e->key == InputKeyOk) {
                if(app->menu_selected == 0) {
                    textbuffer_clear(&app->buf);
                    app->filename_mode = true;
                    app->rename_mode = false;
                    app->kb_active = true;
                    app->kb_row = 0;
                    app->kb_col = 0;
                    app->kb_view_offset = 0;
                    app->kb_view_visible_cols = 12;
                    app->menu_active = false;
                } else if(app->menu_selected == 1) {
                    if(!browser_create_new_file(app)) app_set_status(app, "Create failed");
                    app->menu_active = false;
                } else if(app->menu_selected == 2) {
                    if(app->entry_count == 0) {
                        app_set_status(app, "No item selected");
                    } else {
                        textbuffer_clear(&app->buf);
                        safe_copy(app->buf.lines[0], app->entries[app->selected_entry], MAX_LINE_LEN);
                        app->buf.cursor_col = (uint16_t)safe_strlen(app->buf.lines[0], MAX_LINE_LEN);
                        app->filename_mode = true;
                        app->rename_mode = true;
                        app->kb_active = true;
                        app->kb_row = 0;
                        app->kb_col = 0;
                        app->kb_view_offset = 0;
                        app->kb_view_visible_cols = 12;
                    }
                    app->menu_active = false;
                }
                if(app->menu_selected == 3) {
                    if(browser_undo_delete(app)) app_set_status(app, "Restored");
                    else app_set_status(app, "Nothing to undo");
                } else if(app->menu_selected == 4) {
                    browser_load_dir(app);
                    app_set_status(app, "Refreshed");
                }
                view_port_update(app->vp);
                return;
            }
        }
        if(e->type == InputTypeLong && e->key == InputKeyBack) {
            app->menu_active = false;
            if(browser_delete_selected_file(app)) app_set_status(app, "Deleted");
            else app_set_status(app, "Delete failed");
            view_port_update(app->vp);
            return;
        }
        return;
    }

    uint16_t max_scroll = browser_max_scroll(app);
    if(e->key == InputKeyUp && e->type == InputTypeShort) {
        if(app->selected_entry > 0) {
            app->selected_entry--;
            if(app->selected_entry < app->browser_scroll) app->browser_scroll = app->selected_entry;
            if(app->browser_scroll > max_scroll) app->browser_scroll = max_scroll;
            app->browser_name_hscroll = 0;
            view_port_update(app->vp);
        }
    } else if(e->key == InputKeyDown && e->type == InputTypeShort) {
        if(app->selected_entry + 1 < app->entry_count) {
            app->selected_entry++;
            if(app->selected_entry >= app->browser_scroll + BROWSER_VISIBLE_LINES) {
                if(app->selected_entry >= BROWSER_VISIBLE_LINES - 1) app->browser_scroll = app->selected_entry - (BROWSER_VISIBLE_LINES - 1);
                else app->browser_scroll = 0;
            }
            if(app->browser_scroll > max_scroll) app->browser_scroll = max_scroll;
            app->browser_name_hscroll = 0;
            view_port_update(app->vp);
        }
    } else if(e->key == InputKeyLeft && e->type == InputTypeRepeat) {
        if(app->browser_name_hscroll > 0) {
            app->browser_name_hscroll--;
            view_port_update(app->vp);
        }
    } else if(e->key == InputKeyRight && e->type == InputTypeRepeat) {
        if(app->entry_count > 0) {
            size_t len = safe_strlen(app->entries[app->selected_entry], sizeof(app->entries[app->selected_entry]));
            if(app->browser_name_hscroll < len) {
                app->browser_name_hscroll++;
                view_port_update(app->vp);
            }
        }
    } else if(e->key == InputKeyOk && e->type == InputTypeShort) {
        if(app->entry_count == 0) return;
        app->tmp_path[0] = '\0';
        if(!build_child_path(app, app->tmp_path, sizeof(app->tmp_path), app->current_dir, app->entries[app->selected_entry])) return;
        if(!is_under_root(app->tmp_path)) return;
        if(app->is_dir[app->selected_entry]) {
            safe_copy(app->current_dir, app->tmp_path, sizeof(app->current_dir));
            browser_load_dir(app);
            view_port_update(app->vp);
        } else {
            if(editor_load_file(app, app->tmp_path)) {
                safe_copy(app->opened_path, app->tmp_path, sizeof(app->opened_path));
                app->view = View_Editor;
                app->buf.cursor_line = 0; app->buf.cursor_col = 0; app->buf.scroll = 0; app->buf.hscroll = 0;
                app->kb_active = false; app->kb_row = 0; app->kb_col = 0; app->kb_view_offset = 0; app->kb_view_visible_cols = 12;
                app->readonly = is_protected_path(app->tmp_path);
                view_port_update(app->vp);
            }
        }
    } else if(e->key == InputKeyBack && e->type == InputTypeShort) {
        browser_go_up(app); view_port_update(app->vp);
    } else if(e->key == InputKeyBack && e->type == InputTypeLong) {
        // Long Back: delete selected file (if allowed)
        if(browser_delete_selected_file(app)) {
            app_set_status(app, "Deleted");
            view_port_update(app->vp);
        }
    } else if(e->key == InputKeyLeft && e->type == InputTypeLong) {
        // Long Left opens the visible browser action menu.
        app->crypto_menu_active = false;
        app->menu_active = true;
        app->menu_selected = 0;
        view_port_update(app->vp);
    } else if(e->key == InputKeyRight && e->type == InputTypeLong) {
        app->menu_active = false;
        app->crypto_menu_active = true;
        app->crypto_menu_selected = 0;
        view_port_update(app->vp);
    } else if(e->key == InputKeyRight && e->type == InputTypeShort) {
        // Short Right: copy / paste behavior
        if(!app->copy_pending) {
            browser_start_copy(app);
        } else {
            browser_paste_copy(app);
        }
    } else if(e->key == InputKeyUp && e->type == InputTypeLong) {
        // Long Up in browser: jump to top
        browser_jump_top(app);
    } else if(e->key == InputKeyDown && e->type == InputTypeLong) {
        // Long Down in browser: jump to bottom
        browser_jump_bottom(app);
    }
}

static void input_editor(App* app, InputEvent* e) {
    if(!app || !e) return;
    if(e->type != InputTypeShort && e->type != InputTypePress && e->type != InputTypeRepeat && e->type != InputTypeLong) return;

    // Keyboard mode
    if(app->kb_active) {
        // If keyboard active, handle navigation and token insertion.
        if(e->type == InputTypeShort || e->type == InputTypeRepeat) {
            if(e->key == InputKeyUp) {
                if(app->kb_row > 0) {
                    app->kb_row--;
                    uint8_t len = kb_row_lens[app->kb_row];
                    if(app->kb_col >= len) app->kb_col = (uint8_t)(len ? len - 1 : 0);
                    if(app->kb_col < app->kb_view_offset) app->kb_view_offset = app->kb_col;
                    else if(app->kb_col >= app->kb_view_offset + app->kb_view_visible_cols) app->kb_view_offset = app->kb_col - app->kb_view_visible_cols + 1;
                }
            } else if(e->key == InputKeyDown) {
                if(app->kb_row + 1 < KB_ROWS) {
                    app->kb_row++;
                    uint8_t len = kb_row_lens[app->kb_row];
                    if(app->kb_col >= len) app->kb_col = (uint8_t)(len ? len - 1 : 0);
                    if(app->kb_col < app->kb_view_offset) app->kb_view_offset = app->kb_col;
                    else if(app->kb_col >= app->kb_view_offset + app->kb_view_visible_cols) app->kb_view_offset = app->kb_col - app->kb_view_visible_cols + 1;
                }
            } else if(e->key == InputKeyLeft) {
                if(app->kb_col > 0) {
                    app->kb_col--;
                    if(app->kb_col < app->kb_view_offset) app->kb_view_offset = app->kb_col;
                }
            } else if(e->key == InputKeyRight) {
                uint8_t len = kb_row_lens[app->kb_row];
                if((uint8_t)(app->kb_col + 1) < len) {
                    app->kb_col++;
                    if(app->kb_col >= app->kb_view_offset + app->kb_view_visible_cols) app->kb_view_offset = app->kb_col - app->kb_view_visible_cols + 1;
                }
            } else if(e->key == InputKeyOk && e->type == InputTypeShort) {
                const char* token = kb_rows[app->kb_row][app->kb_col];
                if(token) {
                    if(strcmp(token, "DEL") == 0) { editor_backspace(app); }
                    else if(strcmp(token, "TAB") == 0) {
                        size_t cur = app->buf.cursor_col;
                        uint8_t to_insert = (uint8_t)(4 - (cur % 4));
                        if(to_insert == 0) to_insert = 4;
                        for(uint8_t k = 0; k < to_insert; k++) editor_insert_char(app, ' ');
                    }
                    else if(strcmp(token, "IND") == 0) {
                        app->kb_lowercase = !app->kb_lowercase;
                    }
                    else if(strcmp(token, "SAVE") == 0) {
                        if(app->filename_mode) {
                            if(app->crypto_mode) {
                                if(browser_crypto_selected(app, app->buf.lines[0], app->crypto_decrypt)) {
                                    app_set_status(app, app->crypto_decrypt ? "Decrypted" : "Encrypted");
                                    app->kb_active = false;
                                    app->filename_mode = false;
                                    app->crypto_mode = false;
                                } else {
                                    app_set_status(app, app->crypto_decrypt ? "Decrypt failed" : "Encrypt failed");
                                }
                            } else if(app->rename_mode) {
                                if(browser_rename_selected(app)) {
                                    app_set_status(app, "Renamed");
                                    app->kb_active = false;
                                    app->filename_mode = false;
                                    app->rename_mode = false;
                                } else {
                                    app_set_status(app, "Rename failed");
                                }
                            } else if(browser_create_named_file(app)) {
                                app_set_status(app, "File created");
                            } else {
                                app_set_status(app, "Name unavailable");
                            }
                            view_port_update(app->vp);
                            return;
                        }
                        if(editor_save_current(app)) {
                            app->kb_active = false;
                            app->kb_row = 0; app->kb_col = 0; app->kb_view_offset = 0; app->kb_view_visible_cols = 12;
                            app_set_status(app, "Saved");
                            view_port_update(app->vp);
                            return;
                        } else {
                            app_set_status(app, "Save failed");
                        }
                    }
                    else if(strcmp(token, "NEW") == 0) {
                        if(editor_save_as_new(app)) {
                            app->kb_active = false;
                            app->kb_row = 0; app->kb_col = 0; app->kb_view_offset = 0; app->kb_view_visible_cols = 12;
                            app_set_status(app, "Saved as new");
                            view_port_update(app->vp);
                            return;
                        } else {
                            app_set_status(app, "Save as new failed");
                        }
                    }
                    else if(strcmp(token, "SP") == 0) {
                        editor_insert_char(app, ' ');
                    }
                    else {
                        char ch = token[0];
                        if(isalpha((unsigned char)ch)) {
                            if(app->kb_lowercase) ch = (char)tolower((unsigned char)ch);
                            else ch = (char)toupper((unsigned char)ch);
                        }
                        editor_insert_char(app, ch);
                    }
                }
                view_port_update(app->vp);
                return;
            } else if(e->key == InputKeyBack && e->type == InputTypeShort) {
                app->kb_active = false;
                if(app->filename_mode) {
                    app->filename_mode = false;
                    app->rename_mode = false;
                    app->crypto_mode = false;
                    textbuffer_clear(&app->buf);
                }
                view_port_update(app->vp);
                return;
            }
        }

        if(e->type == InputTypeLong && e->key == InputKeyBack) {
            if(editor_delete_current_line(app)) {
                app->kb_active = false;
                view_port_update(app->vp);
                return;
            }
        }

        if(e->key == InputKeyOk && e->type == InputTypeLong) {
            // Long OK is intentionally ignored so accidental hold doesn't save.
            view_port_update(app->vp);
            return;
        }

        view_port_update(app->vp);
        return;
    }

    // Editor navigation / editing mode (keyboard not active)
    if(e->type == InputTypeShort || e->type == InputTypeRepeat) {
        if(e->key == InputKeyUp) {
            if(app->buf.cursor_line > 0) {
                app->buf.cursor_line--;
                size_t len = safe_strlen(app->buf.lines[app->buf.cursor_line], MAX_LINE_LEN);
                if(app->buf.cursor_col > len) app->buf.cursor_col = (uint16_t)len;
                if(app->buf.cursor_line < app->buf.scroll) app->buf.scroll = app->buf.cursor_line;
                view_port_update(app->vp);
            }
        } else if(e->key == InputKeyDown) {
            if(app->buf.cursor_line + 1 < app->buf.line_count) {
                app->buf.cursor_line++;
                size_t len = safe_strlen(app->buf.lines[app->buf.cursor_line], MAX_LINE_LEN);
                if(app->buf.cursor_col > len) app->buf.cursor_col = (uint16_t)len;
                if(app->buf.cursor_line >= app->buf.scroll + VISIBLE_LINES) app->buf.scroll = app->buf.cursor_line - (VISIBLE_LINES - 1);
                view_port_update(app->vp);
            }
        } else if(e->key == InputKeyLeft) {
            if(app->buf.cursor_col > 0) {
                app->buf.cursor_col--;
            } else if(app->buf.cursor_line > 0) {
                app->buf.cursor_line--;
                app->buf.cursor_col = (uint16_t)safe_strlen(app->buf.lines[app->buf.cursor_line], MAX_LINE_LEN);
                if(app->buf.cursor_line < app->buf.scroll) app->buf.scroll = app->buf.cursor_line;
            }
            ensure_hscroll(app);
            view_port_update(app->vp);
        } else if(e->key == InputKeyRight) {
            size_t len = safe_strlen(app->buf.lines[app->buf.cursor_line], MAX_LINE_LEN);
            if(app->buf.cursor_col < len) {
                app->buf.cursor_col++;
            } else if(app->buf.cursor_line + 1 < app->buf.line_count) {
                app->buf.cursor_line++;
                app->buf.cursor_col = 0;
                if(app->buf.cursor_line >= app->buf.scroll + VISIBLE_LINES) app->buf.scroll = app->buf.cursor_line - (VISIBLE_LINES - 1);
            }
            ensure_hscroll(app);
            view_port_update(app->vp);
        } else if(e->key == InputKeyOk && e->type == InputTypeShort) {
            // Open compact keyboard
            app->kb_active = true;
            app->kb_row = 0; app->kb_col = 0; app->kb_view_offset = 0; app->kb_view_visible_cols = 12;
            view_port_update(app->vp);
        } else if(e->key == InputKeyBack && e->type == InputTypeShort) {
            // Return to the browser; browser actions are in its drop-down menu.
            app->menu_active = false;
            app->view = View_Browser;
            browser_load_dir(app);
            view_port_update(app->vp);
        } else if(e->key == InputKeyBack && e->type == InputTypeLong) {
            // Long Back in editor: delete current line
            if(editor_delete_current_line(app)) view_port_update(app->vp);
        }
    } else if(e->type == InputTypeLong) {
        // Editor long-press behaviors:
        if(e->key == InputKeyUp) {
            // Long Up in editor: delete current line if blank
            if(editor_delete_line_if_blank(app)) view_port_update(app->vp);
        } else if(e->key == InputKeyDown) {
            // Long Down in editor: insert an empty line below
            if(editor_insert_empty_line_below(app)) view_port_update(app->vp);
        } else if(e->key == InputKeyLeft) {
            // Long Left: horizontal scroll left (do not move cursor)
            if(app->buf.hscroll > 0) {
                app->buf.hscroll--;
                view_port_update(app->vp);
            }
        } else if(e->key == InputKeyRight) {
            // Long Right: horizontal scroll right
            app->buf.hscroll++;
            view_port_update(app->vp);
        } else if(e->key == InputKeyBack) {
            // Long Back (when keyboard not active): delete current line
            if(editor_delete_current_line(app)) view_port_update(app->vp);
        }
    }
}

// ---------- Viewport callbacks ----------
static void view_port_draw_callback(Canvas* canvas, void* ctx) {
    App* app = (App*)ctx;
    if(!app || !canvas) return;

    // Clear transient status after STATUS_TIMEOUT_MS ms (use status_last_tick)
    if(app->tmp_status[0] != '\0') {
        uint32_t now = furi_get_tick();
        if((now - app->status_last_tick) >= STATUS_TIMEOUT_MS) {
            app->tmp_status[0] = '\0';
        }
    }

    if(app->view == View_Splash) {
        draw_splash(canvas, app);
        return;
    }
    if(app->view == View_Browser) draw_browser(canvas, app);
    else if(app->view == View_Editor) draw_editor(canvas, app);
    else if(app->view == View_Keyboard) draw_keyboard(canvas, app);
    else if(app->view == View_FileInfo) draw_file_info(canvas, app);

    // If keyboard is active while in editor, draw keyboard overlay
    if(app->kb_active && (app->view == View_Editor || app->filename_mode)) draw_keyboard(canvas, app);
    if(app->menu_active && app->view == View_Browser) draw_browser_menu(canvas, app);
    if(app->crypto_menu_active && app->view == View_Browser) draw_crypto_menu(canvas, app);
}

static void view_port_input_callback(InputEvent* event, void* ctx) {
    App* app = (App*)ctx;
    if(!app || !event) return;
    if(app->view == View_Splash) input_splash(app, event);
    else if(app->view == View_FileInfo) input_file_info(app, event);
    else if(app->view == View_Browser && !app->filename_mode) input_browser(app, event);
    else input_editor(app, event);
}

// ---------- App lifecycle ----------
static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    if(!app) return NULL;
    memset(app, 0, sizeof(App));
    app->view = View_Splash;
    app->running = true;
    app->gui = furi_record_open("gui");
    app->storage = furi_record_open("storage");
    app->vp = view_port_alloc();
    if(!app->vp) {
        if(app->gui) furi_record_close("gui");
        if(app->storage) furi_record_close("storage");
        free(app);
        return NULL;
    }
    view_port_draw_callback_set(app->vp, view_port_draw_callback, app);
    view_port_input_callback_set(app->vp, view_port_input_callback, app);
    if(app->gui && app->vp) gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);
    app->input_queue = NULL; // not used currently
    safe_copy(app->current_dir, ROOT_DIR, sizeof(app->current_dir));
    textbuffer_clear(&app->buf);
    app->kb_active = false;
    app->kb_row = 0; app->kb_col = 0; app->kb_view_offset = 0; app->kb_view_visible_cols = 12;
    app->kb_lowercase = true; // default to lowercase insertion
    app->menu_active = false;
    app->menu_selected = 0;
    app->crypto_menu_active = false;
    app->crypto_menu_selected = 0;
    app->filename_mode = false;
    app->rename_mode = false;
    app->crypto_mode = false;
    app->crypto_decrypt = false;
    app->readonly = false;
    app->copy_buffer[0] = '\0';
    app->copy_pending = false;
    app->last_deleted_original[0] = '\0';
    app->last_deleted_trash[0] = '\0';
    app->delete_pending = false;
    app->trash_serial = 0;
    app->tmp_name[0] = '\0';
    app->tmp_path[0] = '\0';
    app->tmp_status[0] = '\0';
    app->browser_name_last_tick = furi_get_tick();
    app->status_last_tick = furi_get_tick(); // initialize status timestamp separately
    // Preload browser state so it's ready when splash is dismissed
    browser_load_dir(app);
    return app;
}

static void app_free(App* app) {
    if(!app) return;
    browser_empty_trash(app);
    if(app->gui && app->vp) gui_remove_view_port(app->gui, app->vp);
    if(app->vp) view_port_free(app->vp);
    if(app->gui) furi_record_close("gui");
    if(app->storage) furi_record_close("storage");
    free(app);
}

// ---------- Main entry ----------
int32_t text_editor_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();
    if(!app) return 0;

    // Main loop: keep running until app->running is false
    while(app->running) {
        // Keep the browser viewport redrawing so the selected row marquee can auto-scroll.
        furi_delay_ms(50);
        view_port_update(app->vp);
    }

    app_free(app);
    return 0;
}
