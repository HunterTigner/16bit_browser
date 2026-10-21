#include <stdint.h>

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 200
#define FILE_COUNT 32
#define VOLUME_COUNT 12
#define VOLUME_SIZE_LIMIT_MB 4096u
#define FILE_CAPACITY (512u * 1024u)
#define DRIVE_CAPACITY (16u * 1024u * 1024u)
#define NOTE_LIMIT 100000u
#define SCRATCH_CAPACITY (FILE_CAPACITY + 32u)
#define EDITOR_CAPACITY (NOTE_LIMIT * 4u + 8u)
#define URL_CAPACITY 256u
#define FETCH_LIMIT 8192u

typedef struct {
  uint32_t used, size, modified;
  char name[11], extension[5];
} File;

static uint8_t framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT * 4];
static File files[FILE_COUNT];
static uint8_t file_data[FILE_COUNT][FILE_CAPACITY];
static uint8_t scratch[SCRATCH_CAPACITY];
static uint8_t editor[EDITOR_CAPACITY];
static char address[URL_CAPACITY];
static char fetch_address[URL_CAPACITY];
static uint16_t ram[32768];
static uint32_t running_apps, dirty, open_url_requested, file_scroll;
static uint32_t editor_size, editor_characters, editor_cursor, editor_slot = 0xffffffffu;
static uint32_t name_length, extension_length, active_field;
static uint32_t address_length, ram_choice = 2, show_menu;
static uint32_t selected_slot = 0xffffffffu, message_ticks;
static uint32_t save_as_pending;
static uint32_t clock_minutes;
static uint32_t scene, calc_length;
static uint32_t input_mode, desktop_selection, menu_selection;
static uint32_t terminal_length, terminal_line_count;
static char terminal_command[URL_CAPACITY + 48];
static char terminal_lines[12][49];
static uint8_t terminal_line_lengths[12];
static char fetch_name[11], fetch_extension[5];
static uint32_t fetch_name_length, fetch_extension_length, fetch_address_length;
static uint32_t fetch_requested, fetch_pending;
static uint32_t volume_sizes_mb[VOLUME_COUNT] = {16};
static uint32_t current_volume, disk_dirty;
static uint32_t pending_disk_action, pending_disk_slot, pending_disk_parts;
static uint8_t calc_expression_buffer[128];
static int32_t calc_error;
static double calc_result;
static char message[48];
static uint32_t system_time;

int32_t pixel16_file_import(uint32_t name_size, uint32_t ext_size, uint32_t data_size, uint32_t modified);
int32_t pixel16_file_write(uint32_t slot, uint32_t length, uint32_t modified);
int32_t pixel16_file_delete(uint32_t slot);
double pixel16_calculate(uint32_t length);
uint32_t pixel16_calculation_error(void);
uint32_t pixel16_process_count(void);
static void evaluate_calculator(void);

enum { BOOT, DESKTOP, EXPLORER, EDITOR, NEW_FILE, WEB_BROWSER, CALCULATOR, SYSTEM_INFO, TERMINAL };
enum { APP_EXPLORER = 1, APP_NOTEPAD, APP_BROWSER, APP_CALCULATOR, APP_SYSTEM, APP_TERMINAL };
enum { KEY_BACKSPACE = 8, KEY_ENTER = 13, KEY_ESCAPE = 27, KEY_LEFT = 1000, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_DELETE, KEY_TAB };
enum { C_BLACK=0x000000, C_WHITE=0xffffff, C_GRAY=0xc0c0c0, C_DARKGRAY=0x808080, C_NAVY=0x000080, C_TEAL=0x008080, C_CYAN=0x00ffff, C_YELLOW=0xffff00, C_RED=0x800000, C_GREEN=0x008000 };

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

static uint32_t utf8_size(uint32_t code) {
  if (code < 0x80) return 1;
  if (code < 0x800) return 2;
  if (code < 0x10000) return 3;
  return 4;
}

static uint32_t utf8_decode(const uint8_t *text, uint32_t length, uint32_t *position) {
  if (*position >= length) return 0;
  uint32_t p = (*position)++;
  uint8_t a = text[p];
  if (a < 0x80) return a;
  if (a < 0xe0 && p + 1 < length) {
    *position = p + 2;
    return ((uint32_t)(a & 31) << 6) | (text[p + 1] & 63);
  }
  if (a < 0xf0 && p + 2 < length) {
    *position = p + 3;
    return ((uint32_t)(a & 15) << 12) | ((uint32_t)(text[p + 1] & 63) << 6) | (text[p + 2] & 63);
  }
  if (p + 3 < length) {
    *position = p + 4;
    return ((uint32_t)(a & 7) << 18) | ((uint32_t)(text[p + 1] & 63) << 12) |
           ((uint32_t)(text[p + 2] & 63) << 6) | (text[p + 3] & 63);
  }
  return '?';
}

static uint32_t utf16_count(const uint8_t *text, uint32_t length) {
  uint32_t count = 0, position = 0;
  while (position < length) count += utf8_decode(text, length, &position) > 0xffff ? 2 : 1;
  return count;
}

static void set_pixel(int32_t x, int32_t y, uint32_t color) {
  if ((uint32_t)x >= SCREEN_WIDTH || (uint32_t)y >= SCREEN_HEIGHT) return;
  uint32_t i = ((uint32_t)y * SCREEN_WIDTH + (uint32_t)x) * 4;
  framebuffer[i] = color >> 16;
  framebuffer[i + 1] = color >> 8;
  framebuffer[i + 2] = color;
  framebuffer[i + 3] = 255;
}

static void rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
  if (w <= 0 || h <= 0) return;
  int32_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int32_t x1 = x + w > SCREEN_WIDTH ? SCREEN_WIDTH : x + w;
  int32_t y1 = y + h > SCREEN_HEIGHT ? SCREEN_HEIGHT : y + h;
  for (int32_t yy = y0; yy < y1; yy++)
    for (int32_t xx = x0; xx < x1; xx++) set_pixel(xx, yy, color);
}

static void bevel(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t light, uint32_t dark) {
  rect(x, y, w, 1, light); rect(x, y, 1, h, light);
  rect(x + 1, y + h - 1, w - 1, 1, dark); rect(x + w - 1, y + 1, 1, h - 1, dark);
}

static uint8_t glyph_column(uint32_t input, uint32_t column) {
  if (column >= 5) return 0;
  uint32_t ch = input;
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  static const uint8_t letters[26][5] = {
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},
    {0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
  };
  static const uint8_t digits[10][5] = {
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},{0x62,0x51,0x49,0x49,0x46},
    {0x22,0x49,0x49,0x49,0x36},{0x18,0x14,0x12,0x7f,0x10},{0x2f,0x49,0x49,0x49,0x31},
    {0x3e,0x49,0x49,0x49,0x32},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},
    {0x26,0x49,0x49,0x49,0x3e}
  };
  if (ch >= 'A' && ch <= 'Z') return letters[ch - 'A'][column];
  if (ch >= '0' && ch <= '9') return digits[ch - '0'][column];
  switch (ch) {
    case ' ': return 0;
    case '!': return (uint8_t[]){0,0,0x5f,0,0}[column];
    case '"': return (uint8_t[]){0x07,0,0x07,0,0}[column];
    case '#': return (uint8_t[]){0x14,0x7f,0x14,0x7f,0x14}[column];
    case '$': return (uint8_t[]){0x24,0x2a,0x7f,0x2a,0x12}[column];
    case '%': return (uint8_t[]){0x23,0x13,0x08,0x64,0x62}[column];
    case '&': return (uint8_t[]){0x36,0x49,0x55,0x22,0x50}[column];
    case '\'': return (uint8_t[]){0,0x05,0x03,0,0}[column];
    case '(': return (uint8_t[]){0,0x1c,0x22,0x41,0}[column];
    case ')': return (uint8_t[]){0,0x41,0x22,0x1c,0}[column];
    case '*': return (uint8_t[]){0x14,0x08,0x3e,0x08,0x14}[column];
    case '+': return (uint8_t[]){0x08,0x08,0x3e,0x08,0x08}[column];
    case ',': return (uint8_t[]){0,0x50,0x30,0,0}[column];
    case '-': return (uint8_t[]){0x08,0x08,0x08,0x08,0x08}[column];
    case '.': return (uint8_t[]){0,0x60,0x60,0,0}[column];
    case '/': return (uint8_t[]){0x20,0x10,0x08,0x04,0x02}[column];
    case ':': return (uint8_t[]){0,0x36,0x36,0,0}[column];
    case ';': return (uint8_t[]){0,0x56,0x36,0,0}[column];
    case '<': return (uint8_t[]){0x08,0x14,0x22,0x41,0}[column];
    case '=': return (uint8_t[]){0x14,0x14,0x14,0x14,0x14}[column];
    case '>': return (uint8_t[]){0,0x41,0x22,0x14,0x08}[column];
    case '?': return (uint8_t[]){0x02,0x01,0x51,0x09,0x06}[column];
    case '@': return (uint8_t[]){0x32,0x49,0x79,0x41,0x3e}[column];
    case '[': return (uint8_t[]){0,0x7f,0x41,0x41,0}[column];
    case '\\': return (uint8_t[]){0x02,0x04,0x08,0x10,0x20}[column];
    case ']': return (uint8_t[]){0,0x41,0x41,0x7f,0}[column];
    case '^': return (uint8_t[]){0x04,0x02,0x01,0x02,0x04}[column];
    case '_': return (uint8_t[]){0x40,0x40,0x40,0x40,0x40}[column];
    case '`': return (uint8_t[]){0,0x01,0x02,0x04,0}[column];
    case '{': return (uint8_t[]){0,0x08,0x36,0x41,0}[column];
    case '|': return (uint8_t[]){0,0,0x7f,0,0}[column];
    case '}': return (uint8_t[]){0,0x41,0x36,0x08,0}[column];
    case '~': return (uint8_t[]){0x08,0x04,0x08,0x10,0x08}[column];
    default: return (uint8_t[]){0x02,0x01,0x51,0x09,0x06}[column];
  }
}

static void draw_char(int32_t x, int32_t y, uint32_t ch, uint32_t color) {
  if (ch < 32 || ch > 126) ch = '?';
  for (uint32_t column = 0; column < 5; column++) {
    uint8_t bits = glyph_column(ch, column);
    for (uint32_t row = 0; row < 7; row++)
      if (bits & (1u << row)) set_pixel(x + (int32_t)column, y + (int32_t)row, color);
  }
}

static void text(int32_t x, int32_t y, const char *value, uint32_t color) {
  for (uint32_t i = 0; value[i]; i++) draw_char(x + (int32_t)i * 6, y, (uint8_t)value[i], color);
}

static void text_n(int32_t x, int32_t y, const uint8_t *value, uint32_t length, uint32_t color) {
  uint32_t position = 0, column = 0;
  while (position < length && x + (int32_t)column * 6 < SCREEN_WIDTH) {
    uint32_t code = utf8_decode(value, length, &position);
    draw_char(x + (int32_t)column * 6, y, code, color);
    column++;
  }
}

static void text_center(int32_t y, const char *value, uint32_t color) {
  uint32_t length = 0;
  while (value[length]) length++;
  text((SCREEN_WIDTH - (int32_t)length * 6) / 2, y, value, color);
}

static void button(int32_t x, int32_t y, int32_t w, int32_t h, const char *label, uint8_t pressed) {
  rect(x, y, w, h, C_GRAY);
  bevel(x, y, w, h, pressed ? C_DARKGRAY : C_WHITE, pressed ? C_WHITE : C_DARKGRAY);
  uint32_t length = 0;
  while (label[length]) length++;
  text(x + max_u32(3, (uint32_t)((w - (int32_t)length * 6) / 2)),
       y + (h - 7) / 2, label, C_BLACK);
}

static void desktop_icon(int32_t x, int32_t y, const char *label, uint32_t kind) {
  uint32_t ink = C_WHITE;
  uint32_t label_length = 0;
  while (label[label_length]) label_length++;
  if (kind == 0) {
    rect(x + 4, y + 5, 18, 12, C_YELLOW); rect(x + 4, y + 3, 9, 4, C_YELLOW);
    bevel(x + 4, y + 5, 18, 12, C_WHITE, C_DARKGRAY);
  } else if (kind == 1) {
    rect(x + 6, y + 2, 14, 19, C_WHITE); bevel(x + 6, y + 2, 14, 19, C_WHITE, C_DARKGRAY);
    rect(x + 9, y + 8, 8, 1, C_NAVY); rect(x + 9, y + 11, 8, 1, C_NAVY);
    rect(x + 9, y + 14, 7, 1, C_NAVY);
  } else if (kind == 2) {
    rect(x + 4, y + 3, 20, 15, C_WHITE); bevel(x + 4, y + 3, 20, 15, C_WHITE, C_DARKGRAY);
    rect(x + 6, y + 5, 16, 2, C_NAVY); text(x + 7, y + 9, "WWW", C_NAVY);
  } else if (kind == 3) {
    rect(x + 4, y + 2, 18, 18, C_GRAY); bevel(x + 4, y + 2, 18, 18, C_WHITE, C_DARKGRAY);
    text(x + 8, y + 7, "123", C_NAVY);
  } else if (kind == 4) {
    rect(x + 7, y + 3, 13, 16, C_GRAY); bevel(x + 7, y + 3, 13, 16, C_WHITE, C_DARKGRAY);
    rect(x + 10, y + 6, 7, 2, C_NAVY); rect(x + 10, y + 11, 7, 2, C_NAVY);
  } else {
    rect(x + 4, y + 3, 20, 15, C_BLACK); bevel(x + 4, y + 3, 20, 15, C_WHITE, C_DARKGRAY);
    text(x + 7, y + 7, "C:", C_GREEN);
  }
  int32_t label_x = x + 16 - (int32_t)label_length * 3;
  if (label_x < 1) label_x = 1;
  if (label_x + (int32_t)label_length * 6 >= SCREEN_WIDTH)
    label_x = SCREEN_WIDTH - (int32_t)label_length * 6 - 1;
  text(label_x, y + 24, label, ink);
}

static void window_frame(const char *title) {
  rect(4, 4, 312, 178, C_GRAY);
  bevel(4, 4, 312, 178, C_WHITE, C_BLACK);
  rect(7, 7, 306, 12, C_NAVY);
  text(11, 9, title, C_WHITE);
  button(299, 8, 11, 10, "X", 0);
  rect(7, 21, 306, 157, C_GRAY);
}

static void set_message(const char *value) {
  uint32_t i = 0;
  while (value[i] && i < sizeof(message) - 1) { message[i] = value[i]; i++; }
  message[i] = 0;
  message_ticks = 180;
}

static uint32_t drive_used(void) {
  uint32_t total = 0;
  for (uint32_t i = 0; i < FILE_COUNT; i++) if (files[i].used) total += files[i].size;
  return total;
}

static void draw_taskbar(void) {
  rect(0, 182, SCREEN_WIDTH, 18, C_GRAY);
  rect(0, 182, SCREEN_WIDTH, 1, C_WHITE);
  button(3, 184, 42, 13, "START", show_menu);
  uint32_t hours = (clock_minutes / 60) % 24, minutes = clock_minutes % 60;
  char clock_text[6] = {
    (char)('0' + hours / 10), (char)('0' + hours % 10), ':',
    (char)('0' + minutes / 10), (char)('0' + minutes % 10), 0
  };
  text(279, 187, clock_text, C_BLACK);
}

static void draw_desktop(void) {
  rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, C_TEAL);
  const char *labels[6] = {"MY COMPUTER", "NOTEPAD", "WEB BROWSER", "CALCULATOR", "SYSTEM", "TERMINAL"};
  const uint32_t positions[6] = {3, 32, 61, 90, 119, 148};
  for (uint32_t i = 0; i < 6; i++) {
    if (input_mode && !show_menu && desktop_selection == i)
      bevel(1, (int32_t)positions[i], 31, 29, C_WHITE, C_BLACK);
    desktop_icon(5, (int32_t)positions[i], labels[i], i);
  }
  draw_taskbar();
  if (show_menu) {
    rect(3, 77, 116, 103, C_GRAY); bevel(3, 77, 116, 103, C_WHITE, C_BLACK);
    rect(7, 81, 14, 95, C_NAVY); text(10, 91, "P", C_WHITE);
    text(27, 84, "PROGRAMS", C_BLACK);
    const char *items[7] = {"FILE MANAGER", "NOTEPAD", "WEB BROWSER", "CALCULATOR", "SYSTEM", "TERMINAL", "SHUT DOWN"};
    for (uint32_t i = 0; i < 7; i++) {
      int32_t y = 97 + (int32_t)i * 11;
      if (input_mode && menu_selection == i) rect(24, y - 2, 92, 10, C_NAVY);
      text(27, y, items[i], input_mode && menu_selection == i ? C_WHITE : C_BLACK);
    }
  }
}

static void draw_boot(void) {
  rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, C_TEAL);
  rect(35, 28, 250, 142, C_GRAY); bevel(35, 28, 250, 142, C_WHITE, C_BLACK);
  rect(39, 32, 242, 14, C_NAVY);
  text(45, 36, "PIXEL16 SETUP", C_WHITE);
  text_center(55, "16-BIT WEB OPERATING SYSTEM", C_NAVY);
  text_center(73, "KERNEL: PIXEL16.WASM", C_BLACK);
  text_center(85, "64 KB 16-BIT RAM  -  32 DISK FILES", C_BLACK);
  text_center(103, "TAB SETTING - BROWSER LIMITS MEMORY", C_BLACK);
  button(61, 119, 25, 18, "<", 0);
  char ram_labels[5][8] = {"128 MB","256 MB","512 MB","1 GB","2 GB"};
  text(99, 124, ram_labels[ram_choice], C_NAVY);
  button(231, 119, 25, 18, ">", 0);
  button(102, 145, 116, 19, "START PIXEL16", 0);
}

static void draw_explorer(void) {
  window_frame("FILE MANAGER - C:\\");
  text(12, 25, "NAME          EXT     SIZE", C_NAVY);
  rect(12, 34, 286, 1, C_DARKGRAY);
  uint32_t count = 0;
  for (uint32_t i = 0; i < FILE_COUNT; i++) {
    if (!files[i].used) continue;
    if (count >= file_scroll && count < file_scroll + 9) {
      uint32_t y = 39 + (count - file_scroll) * 12;
      if (selected_slot == i) rect(11, y - 2, 289, 11, C_NAVY);
      uint32_t ink = selected_slot == i ? C_WHITE : C_BLACK;
      text(15, y, files[i].name, ink);
      text(105, y, files[i].extension, ink);
      char size[12]; uint32_t n = files[i].size, digits = 1;
      while (n >= 10) { n /= 10; digits++; }
      uint32_t value = files[i].size;
      size[digits] = 0;
      for (uint32_t d = digits; d > 0; d--) { size[d - 1] = (char)('0' + value % 10); value /= 10; }
      text(155, y, size, ink);
    }
    count++;
  }
  if (!count) text(20, 48, "DRIVE IS EMPTY. CREATE A FILE.", C_BLACK);
  text(15, 153, "DISK SPACE:", C_BLACK);
  uint32_t used = drive_used();
  char used_kb[12]; uint32_t kb = used / 1024, digits = 1, number = kb;
  while (number >= 10) { number /= 10; digits++; }
  used_kb[digits] = 0;
  for (uint32_t d = digits; d > 0; d--) { used_kb[d-1] = (char)('0' + kb % 10); kb /= 10; }
  text(81, 153, used_kb, C_BLACK); text(81 + (int32_t)digits * 6, 153, " KB / 16384 KB", C_BLACK);
  button(12, 160, 54, 14, "NEW FILE", 0);
  button(71, 160, 47, 14, "OPEN", 0);
  button(123, 160, 52, 14, "DELETE", 0);
  button(267, 160, 35, 14, "BACK", 0);
  text(181, 164, "ARROWS SELECT", C_BLACK);
}

static void draw_editor(void) {
  window_frame(editor_slot == 0xffffffffu ? "NOTEPAD - UNTITLED" : "NOTEPAD");
  rect(9, 22, 302, 13, C_GRAY);
  text(12, 25, "FILE  EDIT  SAVE", C_BLACK);
  rect(10, 38, 300, 113, C_WHITE);
  bevel(10, 38, 300, 113, C_DARKGRAY, C_WHITE);
  uint32_t position = 0, line = 0, column = 0;
  uint32_t lines[16], line_count = 1;
  lines[0] = 0;
  while (position < editor_size) {
    uint32_t before = position;
    uint32_t ch = utf8_decode(editor, editor_size, &position);
    if (ch == '\n' || column >= 47) {
      uint32_t next_start = ch == '\n' ? position : before;
      if (line_count < 16) lines[line_count++] = next_start;
      else {
        for (uint32_t i = 1; i < 16; i++) lines[i - 1] = lines[i];
        lines[15] = next_start;
      }
      line++;
      column = 0;
    } else column++;
  }
  uint32_t first_visible = line_count > 15 ? line_count - 15 : 0;
  uint32_t visible_lines = min_u32(line_count - first_visible, 15);
  for (uint32_t row = 0; row < visible_lines; row++) {
    uint32_t line_index = first_visible + row;
    uint32_t pos = lines[line_index], end = line_index + 1 < line_count ? lines[line_index + 1] : editor_size;
    if (end > pos && editor[end - 1] == '\n') end--;
    if (end - pos > 47) end = pos + 47;
    int32_t line_y = 41 + (int32_t)row * 7;
    text_n(13, line_y, editor + pos, end - pos, C_BLACK);
    if (editor_cursor >= pos && editor_cursor <= end) {
      uint32_t cursor_position = pos, cursor_column = 0;
      while (cursor_position < editor_cursor) {
        utf8_decode(editor, editor_cursor, &cursor_position);
        cursor_column++;
      }
      rect(13 + (int32_t)cursor_column * 6, line_y - 1, 1, 9, C_NAVY);
    }
  }
  button(11, 157, 53, 14, "SAVE", 0);
  button(70, 157, 57, 14, "SAVE AS", 0);
  button(267, 157, 35, 14, "BACK", 0);
  text(134, 161, "100,000 CHAR LIMIT", C_BLACK);
  if (message_ticks) text(12, 173, message, C_RED);
}

static void draw_new_file(void) {
  window_frame("NEW FILE");
  text(14, 29, "FILE NAME: 10 LETTERS MAX", C_BLACK);
  rect(14, 40, 174, 15, C_WHITE); bevel(14, 40, 174, 15, C_DARKGRAY, C_WHITE);
  text_n(18, 44, (uint8_t *)scratch, name_length, C_BLACK);
  if (active_field == 0) rect(18 + (int32_t)name_length * 6, 43, 1, 9, C_NAVY);
  text(14, 65, "EXTENSION: 4 LETTERS MAX", C_BLACK);
  rect(14, 76, 80, 15, C_WHITE); bevel(14, 76, 80, 15, C_DARKGRAY, C_WHITE);
  text_n(18, 80, scratch + 12, extension_length, C_BLACK);
  if (active_field == 1) rect(18 + (int32_t)extension_length * 6, 79, 1, 9, C_NAVY);
  text(14, 101, "EXAMPLE: NOTES.TXT", C_NAVY);
  text(14, 118, "PRESS TAB TO SWITCH FIELDS", C_BLACK);
  button(14, 146, 58, 17, "CREATE", 0);
  button(81, 146, 58, 17, "CANCEL", 0);
  if (message_ticks) text(14, 168, message, C_RED);
}

static void draw_browser(void) {
  window_frame("PIXEL NET - WEB BROWSER");
  text(12, 25, "ADDRESS:", C_BLACK);
  rect(12, 37, 244, 16, C_WHITE); bevel(12, 37, 244, 16, C_DARKGRAY, C_WHITE);
  text_n(16, 41, (uint8_t *)address, address_length, C_BLACK);
  button(261, 37, 43, 16, "GO", 0);
  rect(12, 59, 292, 89, C_WHITE); bevel(12, 59, 292, 89, C_DARKGRAY, C_WHITE);
  text_center(72, "PIXEL NET BROWSER", C_NAVY);
  text_center(88, "TYPE A WEBSITE ADDRESS ABOVE", C_BLACK);
  text_center(103, "PRESS ENTER OR CLICK GO", C_BLACK);
  text_center(119, "WEB PAGES OPEN IN A NEW TAB", C_DARKGRAY);
  text_center(133, "BROWSER SECURITY STAYS ENABLED", C_DARKGRAY);
  button(269, 157, 35, 14, "BACK", 0);
}

static void draw_calculator(void) {
  window_frame("POCKET CALCULATOR");
  rect(15, 24, 289, 25, C_WHITE); bevel(15, 24, 289, 25, C_DARKGRAY, C_WHITE);
  text_n(21, 32, calc_expression_buffer, calc_length, C_BLACK);
  button(17, 58, 55, 19, "7", 0); button(78, 58, 55, 19, "8", 0); button(139, 58, 55, 19, "9", 0); button(200, 58, 48, 19, "/", 0);
  button(17, 82, 55, 19, "4", 0); button(78, 82, 55, 19, "5", 0); button(139, 82, 55, 19, "6", 0); button(200, 82, 48, 19, "*", 0);
  button(17, 106, 55, 19, "1", 0); button(78, 106, 55, 19, "2", 0); button(139, 106, 55, 19, "3", 0); button(200, 106, 48, 19, "-", 0);
  button(17, 130, 55, 19, "0", 0); button(78, 130, 55, 19, ".", 0); button(139, 130, 48, 19, "+", 0); button(192, 130, 56, 19, "=", 0);
  button(255, 58, 48, 19, "CLEAR", 0);
  if (calc_result != 0 || calc_error) {
    char output[16]; int32_t whole = (int32_t)calc_result, pos = 0;
    if (calc_error) text(18, 154, "INVALID EXPRESSION", C_RED);
    else {
      if (whole < 0) { output[pos++] = '-'; whole = -whole; }
      char reverse[12]; uint32_t n = 0;
      do { reverse[n++] = (char)('0' + whole % 10); whole /= 10; } while (whole && n < 10);
      while (n) output[pos++] = reverse[--n];
      output[pos] = 0; text(18, 154, output, C_NAVY);
    }
  }
  button(269, 157, 35, 14, "BACK", 0);
}

static void draw_system(void) {
  window_frame("PIXEL16 SYSTEM INFORMATION");
  text(14, 28, "MACHINE: PIXEL16 WEB COMPUTER", C_NAVY);
  text(14, 43, "KERNEL: COMPILED WEBASSEMBLY", C_BLACK);
  text(14, 58, "CPU: 16-BIT PIXEL16 VIRTUAL CPU", C_BLACK);
  text(14, 73, "RAM: 64 KB SIMULATED ADDRESS SPACE", C_BLACK);
  text(14, 88, "APP PROCESSES RUNNING:", C_BLACK);
  char process_count[2] = {(char)('0' + pixel16_process_count()),0};
  text(145, 88, process_count, C_NAVY);
  text(14, 103, "DRIVE C: 16 MB, 32 FILE SLOTS", C_BLACK);
  text(14, 118, "MAX FILE NAME: 10; EXT: 4 LETTERS", C_BLACK);
  text(14, 133, "NOTEPAD: 100,000 CHARACTERS", C_BLACK);
  button(14, 146, 225, 18, input_mode ? "INPUT: KEYBOARD (ENTER OR CLICK)" : "INPUT: MOUSE (ENTER OR CLICK)", 0);
  button(269, 157, 35, 14, "BACK", 0);
}

static void draw_terminal(void) {
  window_frame("PIXEL16 TERMINAL - C:\\");
  rect(11, 24, 298, 124, C_BLACK);
  uint32_t first = terminal_line_count > 12 ? terminal_line_count - 12 : 0;
  for (uint32_t row = first; row < terminal_line_count; row++)
    text_n(15, 28 + (int32_t)(row - first) * 9,
      (const uint8_t *)terminal_lines[row], terminal_line_lengths[row], C_WHITE);
  char prompt[5] = {(char)('C' + current_volume), ':', '\\', '>', 0};
  text(15, 137, prompt, C_GREEN);
  uint32_t command_start = terminal_length > 46 ? terminal_length - 46 : 0;
  text_n(39, 137, (const uint8_t *)terminal_command + command_start,
    terminal_length - command_start, C_WHITE);
  button(269, 157, 35, 14, "BACK", 0);
}

static void redraw(void) {
  if (scene == BOOT) draw_boot();
  else if (scene == DESKTOP) draw_desktop();
  else {
    draw_desktop();
    if (scene == EXPLORER) draw_explorer();
    else if (scene == EDITOR) draw_editor();
    else if (scene == NEW_FILE) draw_new_file();
    else if (scene == WEB_BROWSER) draw_browser();
    else if (scene == CALCULATOR) draw_calculator();
    else if (scene == SYSTEM_INFO) draw_system();
    else if (scene == TERMINAL) draw_terminal();
  }
  if (message_ticks && scene != EDITOR && scene != NEW_FILE) {
    rect(30, 172, 260, 9, C_YELLOW);
    text_center(173, message, C_RED);
    message_ticks--;
  } else if (message_ticks) message_ticks--;
}

static void open_app(uint32_t app) {
  show_menu = 0;
  if (app < 1 || app > 6) return;
  running_apps |= 1u << app;
  if (app == APP_EXPLORER) { scene = EXPLORER; selected_slot = 0xffffffffu; file_scroll = 0; }
  else if (app == APP_NOTEPAD) {
    scene = EDITOR; editor_slot = 0xffffffffu; editor_size = 0; editor_characters = 0; editor_cursor = 0;
  } else if (app == APP_BROWSER) {
    scene = WEB_BROWSER;
  } else if (app == APP_CALCULATOR) {
    scene = CALCULATOR; calc_length = 0; calc_error = 0; calc_result = 0;
  } else if (app == APP_SYSTEM) scene = SYSTEM_INFO;
  else { scene = TERMINAL; terminal_length = terminal_line_count = 0; }
}

static void close_app(void) {
  if (scene == NEW_FILE) { scene = EXPLORER; return; }
  uint32_t app = scene == EXPLORER ? APP_EXPLORER :
    scene == EDITOR || scene == NEW_FILE ? APP_NOTEPAD :
    scene == WEB_BROWSER ? APP_BROWSER :
    scene == CALCULATOR ? APP_CALCULATOR :
    scene == SYSTEM_INFO ? APP_SYSTEM :
    scene == TERMINAL ? APP_TERMINAL : 0;
  if (app) running_apps &= ~(1u << app);
  scene = DESKTOP; show_menu = 0;
}

static void select_menu_item(uint32_t selection) {
  if (selection == 0) open_app(APP_EXPLORER);
  else if (selection == 1) open_app(APP_NOTEPAD);
  else if (selection == 2) open_app(APP_BROWSER);
  else if (selection == 3) open_app(APP_CALCULATOR);
  else if (selection == 4) open_app(APP_SYSTEM);
  else if (selection == 5) open_app(APP_TERMINAL);
  else { running_apps = 0; scene = BOOT; show_menu = 0; }
}

static int terminal_char_equal(char left, char right) {
  if (left >= 'a' && left <= 'z') left -= 32;
  if (right >= 'a' && right <= 'z') right -= 32;
  return left == right;
}

static int terminal_command_is(const char *command, const char *expected, uint32_t length) {
  uint32_t i = 0;
  while (i < length && expected[i] && terminal_char_equal(command[i], expected[i])) i++;
  return i == length && expected[i] == 0;
}

static void terminal_new_line(void) {
  if (terminal_line_count == 12) {
    for (uint32_t i = 1; i < 12; i++) {
      for (uint32_t j = 0; j < sizeof(terminal_lines[i]); j++)
        terminal_lines[i - 1][j] = terminal_lines[i][j];
      terminal_line_lengths[i - 1] = terminal_line_lengths[i];
    }
    terminal_line_count = 11;
  }
  terminal_lines[terminal_line_count][0] = 0;
  terminal_line_lengths[terminal_line_count] = 0;
  terminal_line_count++;
}

static void terminal_put_char(char value) {
  if (!terminal_line_count) terminal_new_line();
  uint32_t row = terminal_line_count - 1;
  if (value == '\n') { terminal_new_line(); return; }
  if (terminal_line_lengths[row] == 48) {
    terminal_new_line();
    row = terminal_line_count - 1;
  }
  uint32_t length = terminal_line_lengths[row];
  terminal_lines[row][length] = value;
  terminal_lines[row][length + 1] = 0;
  terminal_line_lengths[row]++;
}

static void terminal_put_text(const char *value) {
  for (uint32_t i = 0; value[i]; i++) terminal_put_char(value[i]);
}

static void terminal_put_number(uint32_t value) {
  char digits[10];
  uint32_t count = 0;
  do {
    digits[count++] = (char)('0' + value % 10);
    value /= 10;
  } while (value && count < sizeof(digits));
  while (count) terminal_put_char(digits[--count]);
}

static void terminal_put_prompt(void) {
  terminal_put_char((char)('C' + current_volume));
  terminal_put_char(':');
  terminal_put_char('\\');
  terminal_put_char('>');
}

static void terminal_put_file_text(const uint8_t *value, uint32_t length) {
  uint32_t position = 0;
  while (position < length) {
    uint32_t code = utf8_decode(value, length, &position);
    if (code == '\n') terminal_put_char('\n');
    else if (code == '\r') { }
    else terminal_put_char(code >= 32 && code <= 126 ? (char)code : '?');
  }
}

static int terminal_find_file(const char *filename, uint32_t length) {
  uint32_t dot = 0;
  while (dot < length && filename[dot] != '.') dot++;
  if (!dot || dot + 1 >= length || dot > 10 || length - dot - 1 > 4 || dot == length)
    return -1;
  for (uint32_t i = 0; i < FILE_COUNT; i++) {
    if (!files[i].used) continue;
    uint32_t name_length = 0, ext_length = 0;
    while (files[i].name[name_length]) name_length++;
    while (files[i].extension[ext_length]) ext_length++;
    if (name_length != dot || ext_length != length - dot - 1) continue;
    uint32_t n = 0;
    while (n < dot && terminal_char_equal(files[i].name[n], filename[n])) n++;
    if (n != dot) continue;
    n = 0;
    while (n < ext_length && terminal_char_equal(files[i].extension[n], filename[dot + 1 + n])) n++;
    if (n == ext_length) return (int)i;
  }
  return -1;
}

static int terminal_parse_drive(const char *value, uint32_t length) {
  if (length == 0 || length > 2) return -1;
  char letter = value[0];
  if (letter >= 'a' && letter <= 'z') letter -= 32;
  if (letter < 'C' || letter >= 'C' + VOLUME_COUNT) return -1;
  if (length == 2 && value[1] != ':') return -1;
  uint32_t slot = (uint32_t)(letter - 'C');
  return volume_sizes_mb[slot] ? (int)slot : -1;
}

static uint32_t terminal_parse_number(const char *value, uint32_t length) {
  if (!length) return 0;
  uint32_t number = 0;
  for (uint32_t i = 0; i < length; i++) {
    if (value[i] < '0' || value[i] > '9') return 0;
    uint32_t digit = (uint32_t)(value[i] - '0');
    if (number > (VOLUME_SIZE_LIMIT_MB - digit) / 10) return 0;
    number = number * 10 + digit;
  }
  return number;
}

static int terminal_parse_filename(const char *value, uint32_t length,
    uint32_t *name_length, uint32_t *extension_length) {
  uint32_t dot = 0;
  while (dot < length && value[dot] != '.') dot++;
  if (!dot || dot == length || dot > 10 || length - dot - 1 > 4 ||
      dot == length - 1) return 0;
  for (uint32_t i = 0; i < length; i++) {
    if (i == dot) continue;
    char c = value[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return 0;
  }
  *name_length = dot;
  *extension_length = length - dot - 1;
  return 1;
}

static void terminal_put_filename(uint32_t slot) {
  terminal_put_text(files[slot].name);
  terminal_put_char('.');
  terminal_put_text(files[slot].extension);
}

static int terminal_name_exists(const char *name, uint32_t name_length,
    const char *extension, uint32_t extension_length, int except_slot) {
  for (uint32_t i = 0; i < FILE_COUNT; i++) {
    if (!files[i].used || (int)i == except_slot) continue;
    uint32_t n = 0, e = 0;
    while (files[i].name[n]) n++;
    while (files[i].extension[e]) e++;
    if (n != name_length || e != extension_length) continue;
    uint32_t j = 0;
    while (j < n && terminal_char_equal(files[i].name[j], name[j])) j++;
    if (j != n) continue;
    j = 0;
    while (j < e && terminal_char_equal(files[i].extension[j], extension[j])) j++;
    if (j == e) return 1;
  }
  return 0;
}

static uint32_t terminal_max_parts(uint32_t slot) {
  uint32_t available = 0;
  for (uint32_t i = 1; i < VOLUME_COUNT; i++)
    if (!volume_sizes_mb[i] || i == slot) available++;
  return min_u32(volume_sizes_mb[slot], available);
}

static int terminal_parse_fetch_name(const char *name, uint32_t name_length,
    const char *url, uint32_t url_length) {
  uint32_t dot = 0;
  while (dot < name_length && name[dot] != '.') dot++;
  uint32_t base_length = dot;
  uint32_t ext_length = 0;
  const char *extension = 0;
  if (dot < name_length) {
    ext_length = name_length - dot - 1;
    extension = name + dot + 1;
  } else {
    uint32_t path = 0;
    while (path + 2 < url_length &&
        !(url[path] == ':' && url[path + 1] == '/' && url[path + 2] == '/')) path++;
    if (path + 2 >= url_length) return 0;
    path += 3;
    while (path < url_length && url[path] != '/') path++;
    uint32_t path_end = path;
    while (path_end < url_length && url[path_end] != '?' && url[path_end] != '#') path_end++;
    uint32_t filename_start = path_end;
    while (filename_start > path && url[filename_start - 1] != '/') filename_start--;
    uint32_t url_dot = path_end;
    while (url_dot > filename_start && url[url_dot - 1] != '.') url_dot--;
    if (url_dot > filename_start && url[url_dot - 1] == '.') {
      extension = url + url_dot + 1;
      ext_length = path_end - url_dot;
    }
  }
  if (!base_length || !extension || !ext_length) return 0;
  uint32_t kept_name = min_u32(base_length, 10);
  uint32_t kept_extension = min_u32(ext_length, 4);
  for (uint32_t i = 0; i < base_length; i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return 0;
    if (i < kept_name) fetch_name[i] = c;
  }
  for (uint32_t i = 0; i < ext_length; i++) {
    char c = extension[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return 0;
    if (i < kept_extension) fetch_extension[i] = c;
  }
  fetch_name[kept_name] = 0;
  fetch_extension[kept_extension] = 0;
  fetch_name_length = kept_name;
  fetch_extension_length = kept_extension;
  return 1;
}

static void terminal_fetch_command(const char *argument, uint32_t length) {
  uint32_t location_end = 0;
  while (location_end < length && argument[location_end] != ' ') location_end++;
  uint32_t name_start = location_end;
  while (name_start < length && argument[name_start] == ' ') name_start++;
  uint32_t name_end = name_start;
  while (name_end < length && argument[name_end] != ' ') name_end++;
  uint32_t url_start = name_end;
  while (url_start < length && argument[url_start] == ' ') url_start++;
  uint32_t url_end = length;
  while (url_end > url_start && argument[url_end - 1] == ' ') url_end--;
  uint32_t url_length = url_end - url_start;
  int root_location = terminal_command_is(argument, "C:", location_end) ||
    (location_end == 3 && terminal_char_equal(argument[0], 'C') &&
      argument[1] == ':' && (argument[2] == '\\' || argument[2] == '/'));

  if (!location_end || name_end == name_start || url_start == url_end) {
    terminal_put_text("USAGE: FET C:\\ NAME.EXT HTTPS://URL");
    terminal_new_line();
    return;
  }
  if (!root_location) {
    terminal_put_text("USE C:\\ - OTHER VIRTUAL DRIVES ARE EMPTY");
    terminal_new_line();
    return;
  }
  if (url_length >= URL_CAPACITY || url_length < 8 ||
      !(terminal_command_is(argument + url_start, "https://", 8) ||
        terminal_command_is(argument + url_start, "http://", 7)) ||
      !terminal_parse_fetch_name(argument + name_start, name_end - name_start,
        argument + url_start, url_length)) {
    terminal_put_text("USE HTTP(S) URL AND A VALID NAME.EXT");
    terminal_new_line();
    return;
  }
  if (fetch_pending || fetch_requested) {
    terminal_put_text("A DOWNLOAD IS ALREADY RUNNING");
    terminal_new_line();
    return;
  }
  if (terminal_name_exists(fetch_name, fetch_name_length, fetch_extension,
      fetch_extension_length, -1)) {
    terminal_put_text("FILE ALREADY EXISTS");
    terminal_new_line();
    return;
  }
  for (uint32_t i = 0; i < url_length; i++) fetch_address[i] = argument[url_start + i];
  fetch_address[url_length] = 0;
  fetch_address_length = url_length;
  fetch_pending = fetch_requested = 1;
  terminal_put_text("FETCHING ");
  terminal_put_text(fetch_name);
  terminal_put_char('.');
  terminal_put_text(fetch_extension);
  terminal_put_text(" (8 KB MAX)");
  terminal_new_line();
}

static void terminal_finish_disk_action(void) {
  if (pending_disk_action == 1) {
    volume_sizes_mb[pending_disk_slot] = 0;
    if (current_volume == pending_disk_slot) current_volume = 0;
    terminal_put_text("VIRTUAL DISC REMOVED");
    terminal_new_line();
    disk_dirty = 1;
  } else if (pending_disk_action == 2) {
    uint32_t size = volume_sizes_mb[pending_disk_slot];
    uint32_t parts = pending_disk_parts;
    uint32_t base_size = size / parts, remainder = size % parts;
    volume_sizes_mb[pending_disk_slot] = 0;
    uint32_t created = 0;
    for (uint32_t pass = 0; pass < 2 && created < parts; pass++) {
      for (uint32_t i = 1; i < VOLUME_COUNT && created < parts; i++) {
        if (pass == 0 && i != pending_disk_slot) continue;
        if (pass == 1 && (i == pending_disk_slot || volume_sizes_mb[i])) continue;
        volume_sizes_mb[i] = base_size + (created < remainder ? 1 : 0);
        created++;
      }
    }
    if (current_volume == pending_disk_slot) current_volume = 0;
    terminal_put_text("DISC SPLIT INTO ");
    terminal_put_number(parts);
    terminal_put_text(" EMPTY VOLUMES");
    terminal_new_line();
    disk_dirty = 1;
  }
  pending_disk_action = pending_disk_slot = pending_disk_parts = 0;
}

static void terminal_disk_command(const char *argument, uint32_t length) {
  if (!length) {
    terminal_put_text("DISK LIST | ADD [MB] | INFO X: | REMOVE X: | CUT X: [PARTS]");
    terminal_new_line();
    return;
  }
  uint32_t sub_end = 0;
  while (sub_end < length && argument[sub_end] != ' ') sub_end++;
  uint32_t rest = sub_end;
  while (rest < length && argument[rest] == ' ') rest++;
  uint32_t rest_end = length;
  while (rest_end > rest && argument[rest_end - 1] == ' ') rest_end--;
  uint32_t rest_length = rest_end - rest;

  if (terminal_command_is(argument, "LIST", sub_end)) {
    for (uint32_t i = 0; i < VOLUME_COUNT; i++) {
      if (!volume_sizes_mb[i]) continue;
      terminal_put_char((char)('C' + i));
      terminal_put_text(": ");
      terminal_put_number(volume_sizes_mb[i]);
      terminal_put_text(" MB");
      if (i == current_volume) terminal_put_text(" (CURRENT)");
      terminal_new_line();
    }
  } else if (terminal_command_is(argument, "ADD", sub_end)) {
    uint32_t size = rest_length ? terminal_parse_number(argument + rest, rest_length) : 16;
    uint32_t slot = 1;
    while (slot < VOLUME_COUNT && volume_sizes_mb[slot]) slot++;
    if (!size || size > VOLUME_SIZE_LIMIT_MB || slot == VOLUME_COUNT) {
      terminal_put_text(slot == VOLUME_COUNT ? "NO DRIVE LETTERS AVAILABLE" : "SIZE MUST BE 1-4096 MB");
      terminal_new_line();
    } else {
      volume_sizes_mb[slot] = size;
      current_volume = slot;
      disk_dirty = 1;
      terminal_put_text("ADDED EMPTY VIRTUAL DISC ");
      terminal_put_char((char)('C' + slot)); terminal_put_char(':');
      terminal_new_line();
    }
  } else if (terminal_command_is(argument, "REMOVE", sub_end) ||
             terminal_command_is(argument, "EJECT", sub_end)) {
    int slot = terminal_parse_drive(argument + rest, rest_length);
    if (slot <= 0) {
      terminal_put_text(slot == 0 ? "C: IS PROTECTED" : "DRIVE NOT FOUND");
      terminal_new_line();
    } else {
      pending_disk_action = 1;
      pending_disk_slot = (uint32_t)slot;
      terminal_put_text("REMOVE VIRTUAL DISC ");
      terminal_put_char((char)('C' + slot));
      terminal_put_text(":? TYPE YES OR NO");
      terminal_new_line();
    }
  } else if (terminal_command_is(argument, "INFO", sub_end)) {
    int slot = terminal_parse_drive(argument + rest, rest_length);
    if (slot < 0) {
      terminal_put_text("DRIVE NOT FOUND");
      terminal_new_line();
    } else {
      terminal_put_char((char)('C' + slot)); terminal_put_text(": ");
      terminal_put_number(volume_sizes_mb[slot]);
      terminal_put_text(slot == 0 ? " MB PIXEL16 FILE DRIVE" : " MB EMPTY VIRTUAL VOLUME");
      terminal_new_line();
    }
  } else if (terminal_command_is(argument, "CUT", sub_end) ||
             terminal_command_is(argument, "SPLIT", sub_end)) {
    uint32_t drive_end = rest;
    while (drive_end < rest_end && argument[drive_end] != ' ') drive_end++;
    int slot = terminal_parse_drive(argument + rest, drive_end - rest);
    uint32_t parts_start = drive_end;
    while (parts_start < rest_end && argument[parts_start] == ' ') parts_start++;
    uint32_t parts = parts_start < rest_end ?
      terminal_parse_number(argument + parts_start, rest_end - parts_start) : 0;
    if (slot <= 0) {
      terminal_put_text(slot == 0 ? "C: IS PROTECTED" : "DRIVE NOT FOUND");
      terminal_new_line();
    } else {
      uint32_t maximum = terminal_max_parts((uint32_t)slot);
      terminal_put_text("MAX EMPTY PARTITIONS: ");
      terminal_put_number(maximum);
      terminal_new_line();
      if (parts_start == rest_end) {
        terminal_put_text("USE DISK CUT X: PARTS");
        terminal_new_line();
      } else if (parts < 2 || parts > maximum) {
        terminal_put_text("PARTS MUST BE 2..MAX; EACH AT LEAST 1 MB");
        terminal_new_line();
      } else {
        pending_disk_action = 2;
        pending_disk_slot = (uint32_t)slot;
        pending_disk_parts = parts;
        terminal_put_text("SPLIT INTO EMPTY VOLUMES? TYPE YES OR NO");
        terminal_new_line();
      }
    }
  } else {
    terminal_put_text("USE DISK LIST, ADD, INFO, REMOVE, OR CUT");
    terminal_new_line();
  }
}

static void terminal_execute(void) {
  uint32_t command_end = 0;
  while (command_end < terminal_length && terminal_command[command_end] != ' ') command_end++;
  if (!command_end) return;
  terminal_put_prompt();
  for (uint32_t i = 0; i < terminal_length; i++) terminal_put_char(terminal_command[i]);
  terminal_new_line();

  uint32_t argument = command_end;
  while (argument < terminal_length && terminal_command[argument] == ' ') argument++;
  uint32_t argument_end = terminal_length;
  while (argument_end > argument && terminal_command[argument_end - 1] == ' ') argument_end--;
  terminal_command[terminal_length] = 0;
  const char *arg = terminal_command + argument;
  uint32_t arg_length = argument_end - argument;

  if (pending_disk_action) {
    if (terminal_command_is(terminal_command, "YES", terminal_length))
      terminal_finish_disk_action();
    else {
      terminal_put_text(terminal_command_is(terminal_command, "NO", terminal_length) ?
        "DISC OPERATION CANCELLED" : "TYPE YES TO CONFIRM OR NO TO CANCEL");
      terminal_new_line();
      if (terminal_command_is(terminal_command, "NO", terminal_length))
        pending_disk_action = pending_disk_slot = pending_disk_parts = 0;
    }
  } else if (terminal_command_is(terminal_command, "HELP", command_end)) {
    terminal_put_text("HELP DIR TYPE ECHO CLS VER MEM EXIT");
    terminal_new_line();
    terminal_put_text("CD PWD DRIVE DISK DISC COPY DEL REN FIND");
    terminal_new_line();
    terminal_put_text("DISK: LIST ADD INFO REMOVE EJECT CUT");
    terminal_new_line();
    terminal_put_text("FET C:\\ NAME.EXT HTTPS://URL (8 KB MAX)");
    terminal_new_line();
  } else if (terminal_command_is(terminal_command, "DIR", command_end)) {
    uint32_t count = 0;
    for (uint32_t i = 0; current_volume == 0 && i < FILE_COUNT; i++) {
      if (!files[i].used) continue;
      char filename[16];
      uint32_t pos = 0;
      while (files[i].name[pos]) { filename[pos] = files[i].name[pos]; pos++; }
      filename[pos++] = '.';
      uint32_t ext = 0;
      while (files[i].extension[ext]) filename[pos++] = files[i].extension[ext++];
      filename[pos] = 0;
      terminal_put_text(filename);
      terminal_new_line();
      count++;
    }
    if (!count) terminal_put_text("NO FILES ON THIS VOLUME");
    else terminal_put_text("END OF DIRECTORY");
    terminal_new_line();
  } else if (terminal_command_is(terminal_command, "TYPE", command_end)) {
    int slot = current_volume == 0 ? terminal_find_file(arg, arg_length) : -1;
    if (slot < 0) {
      terminal_put_text(arg_length ? "FILE NOT FOUND" : "USAGE: TYPE NAME.EXT");
      terminal_new_line();
    } else {
      terminal_put_file_text(file_data[slot], files[slot].size);
      terminal_new_line();
    }
  } else if (terminal_command_is(terminal_command, "ECHO", command_end)) {
    for (uint32_t i = argument; i < argument_end; i++) terminal_put_char(terminal_command[i]);
    terminal_new_line();
  } else if (terminal_command_is(terminal_command, "VER", command_end)) {
    terminal_put_text("PIXEL16 16-BIT WEB OS");
    terminal_new_line();
  } else if (terminal_command_is(terminal_command, "MEM", command_end)) {
    terminal_put_text("65536 BYTES - 64 KB SIMULATED RAM");
    terminal_new_line();
  } else if (terminal_command_is(terminal_command, "CD", command_end)) {
    if (!arg_length) {
      terminal_put_char((char)('C' + current_volume)); terminal_put_text(":\\"); terminal_new_line();
    } else {
      int slot = terminal_parse_drive(arg, arg_length);
      if (slot < 0) {
        terminal_put_text("DRIVE NOT FOUND. USE CD X:");
        terminal_new_line();
      } else {
        current_volume = (uint32_t)slot;
        terminal_put_char((char)('C' + current_volume)); terminal_put_text(":\\"); terminal_new_line();
      }
    }
  } else if (terminal_command_is(terminal_command, "PWD", command_end)) {
    terminal_put_char((char)('C' + current_volume)); terminal_put_text(":\\"); terminal_new_line();
  } else if (terminal_command_is(terminal_command, "DRIVE", command_end)) {
    terminal_put_text("CURRENT VOLUME ");
    terminal_put_char((char)('C' + current_volume)); terminal_put_text(": ");
    terminal_put_number(volume_sizes_mb[current_volume]);
    terminal_put_text(" MB"); terminal_new_line();
  } else if (terminal_command_is(terminal_command, "DISK", command_end) ||
             terminal_command_is(terminal_command, "DISC", command_end)) {
    terminal_disk_command(arg, arg_length);
  } else if (terminal_command_is(terminal_command, "COPY", command_end)) {
    uint32_t split = 0;
    while (split < arg_length && arg[split] != ' ') split++;
    uint32_t target = split;
    while (target < arg_length && arg[target] == ' ') target++;
    uint32_t target_name = 0, target_ext = 0;
    int source_slot = current_volume == 0 ? terminal_find_file(arg, split) : -1;
    if (source_slot < 0 || target == arg_length ||
        !terminal_parse_filename(arg + target, arg_length - target, &target_name, &target_ext)) {
      terminal_put_text("USAGE: COPY SOURCE.EXT TARGET.EXT");
      terminal_new_line();
    } else if (terminal_name_exists(arg + target, target_name, arg + target + target_name + 1, target_ext, -1)) {
      terminal_put_text("TARGET ALREADY EXISTS");
      terminal_new_line();
    } else {
      for (uint32_t i = 0; i < target_name; i++) scratch[i] = (uint8_t)arg[target + i];
      for (uint32_t i = 0; i < target_ext; i++) scratch[target_name + i] = (uint8_t)arg[target + target_name + 1 + i];
      for (uint32_t i = 0; i < files[source_slot].size; i++)
        scratch[target_name + target_ext + i] = file_data[source_slot][i];
      int copied = pixel16_file_import(target_name, target_ext,
        files[source_slot].size, system_time);
      if (copied < 0) { terminal_put_text("COPY FAILED"); terminal_new_line(); }
      else { dirty = 1; terminal_put_text("FILE COPIED"); terminal_new_line(); }
    }
  } else if (terminal_command_is(terminal_command, "DEL", command_end) ||
             terminal_command_is(terminal_command, "DELETE", command_end)) {
    int slot = current_volume == 0 ? terminal_find_file(arg, arg_length) : -1;
    if (slot < 0) { terminal_put_text("FILE NOT FOUND"); terminal_new_line(); }
    else { pixel16_file_delete((uint32_t)slot); dirty = 1; terminal_put_text("FILE DELETED"); terminal_new_line(); }
  } else if (terminal_command_is(terminal_command, "REN", command_end) ||
             terminal_command_is(terminal_command, "RENAME", command_end)) {
    uint32_t split = 0, name_size = 0, ext_size = 0;
    while (split < arg_length && arg[split] != ' ') split++;
    uint32_t target = split;
    while (target < arg_length && arg[target] == ' ') target++;
    int slot = current_volume == 0 ? terminal_find_file(arg, split) : -1;
    uint32_t filename_start = target;
    if (slot < 0 || target == arg_length ||
        !terminal_parse_filename(arg + target, arg_length - target, &name_size, &ext_size)) {
      terminal_put_text("USAGE: REN OLD.EXT NEW.EXT"); terminal_new_line();
    } else {
      uint32_t dot = filename_start;
      while (dot < arg_length && arg[dot] != '.') dot++;
      if (terminal_name_exists(arg + filename_start, name_size,
          arg + dot + 1, ext_size, slot)) {
        terminal_put_text("TARGET ALREADY EXISTS"); terminal_new_line();
      } else {
        for (uint32_t i = 0; i < name_size; i++) files[slot].name[i] = arg[filename_start + i];
        files[slot].name[name_size] = 0;
        for (uint32_t i = 0; i < ext_size; i++) files[slot].extension[i] = arg[dot + 1 + i];
        files[slot].extension[ext_size] = 0;
        files[slot].modified = system_time;
        dirty = 1; terminal_put_text("FILE RENAMED"); terminal_new_line();
      }
    }
  } else if (terminal_command_is(terminal_command, "FIND", command_end)) {
    if (current_volume != 0 || !arg_length) {
      terminal_put_text("USAGE: FIND TEXT"); terminal_new_line();
    } else {
      uint32_t matches = 0;
      for (uint32_t i = 0; i < FILE_COUNT; i++) {
        if (!files[i].used || !arg_length || arg_length > files[i].size) continue;
        for (uint32_t at = 0; at + arg_length <= files[i].size; at++) {
          uint32_t j = 0;
          while (j < arg_length && terminal_char_equal((char)file_data[i][at + j], arg[j])) j++;
          if (j == arg_length) { terminal_put_filename(i); terminal_new_line(); matches++; break; }
        }
      }
      if (!matches) terminal_put_text("NO MATCHES");
      else terminal_put_text("SEARCH COMPLETE");
      terminal_new_line();
    }
  } else if (terminal_command_is(terminal_command, "FET", command_end)) {
    terminal_fetch_command(arg, arg_length);
  } else if (terminal_command_is(terminal_command, "CLS", command_end)) {
    terminal_line_count = 0;
    if (pending_disk_action) pending_disk_action = pending_disk_slot = pending_disk_parts = 0;
  } else if (terminal_command_is(terminal_command, "EXIT", command_end)) {
    close_app();
  } else {
    terminal_put_text("BAD COMMAND. TYPE HELP.");
    terminal_new_line();
  }
  terminal_length = 0;
  terminal_command[0] = 0;
}

static void save_editor(void) {
  if (editor_slot == 0xffffffffu) {
    name_length = extension_length = active_field = 0;
    save_as_pending = 1;
    for (uint32_t i = 0; i < 16; i++) scratch[i] = 0;
    scene = NEW_FILE;
    return;
  }
  if (editor_size > FILE_CAPACITY || utf16_count(editor, editor_size) > NOTE_LIMIT) {
    set_message("FILE SIZE LIMIT EXCEEDED");
    return;
  }
  for (uint32_t i = 0; i < editor_size; i++) scratch[i] = editor[i];
  uint32_t result = pixel16_file_write(editor_slot, editor_size, system_time);
  if ((int32_t)result == -4) set_message("C: DRIVE IS FULL");
  else if ((int32_t)result < 0) set_message("DISK WRITE ERROR");
  else { dirty = 1; set_message("FILE SAVED TO C:"); }
}

static void create_file(void) {
  if (!name_length || !extension_length) { set_message("ENTER A NAME AND EXTENSION"); return; }
  for (uint32_t i = 0; i < extension_length; i++) scratch[name_length + i] = scratch[12 + i];
  uint32_t result = pixel16_file_import(name_length, extension_length, 0, 0);
  if ((int32_t)result == -2) { set_message("C: HAS 32 FILES MAXIMUM"); return; }
  if ((int32_t)result == -3) { set_message("FILE ALREADY EXISTS"); return; }
  if ((int32_t)result < 0) { set_message("INVALID FILE NAME"); return; }
  if (save_as_pending && editor_size) {
    for (uint32_t i = 0; i < editor_size; i++) scratch[i] = editor[i];
    int32_t saved = pixel16_file_write((uint32_t)result, editor_size, system_time);
    if (saved < 0) {
      pixel16_file_delete((uint32_t)result);
      set_message("CANNOT SAVE FILE - DISK LIMIT");
      return;
    }
  }
  editor_slot = result;
  if (!save_as_pending) editor_size = editor_characters = editor_cursor = 0;
  save_as_pending = 0;
  running_apps |= 1u << APP_NOTEPAD; scene = EDITOR; dirty = 1;
}

static void delete_character(void) {
  if (!editor_cursor || !editor_characters) return;
  uint32_t start = editor_cursor - 1;
  while (start && (editor[start] & 0xc0) == 0x80) start--;
  uint32_t removed = editor_cursor - start, following = editor_size - editor_cursor;
  for (uint32_t i = 0; i < following; i++) editor[start + i] = editor[editor_cursor + i];
  editor_size -= removed;
  editor_cursor = start;
  editor_characters--;
}

static void editor_insert(uint32_t code) {
  if (!code || code < 32 && code != '\n' && code != '\t') return;
  if (code == '\t') code = ' ';
  uint32_t bytes = utf8_size(code);
  if (editor_characters + (code > 0xffff ? 2 : 1) > NOTE_LIMIT || editor_size + bytes >= EDITOR_CAPACITY) {
    set_message("100,000 CHARACTER LIMIT");
    return;
  }
  for (uint32_t i = editor_size; i > editor_cursor; i--) editor[i + bytes - 1] = editor[i - 1];
  if (bytes == 1) editor[editor_cursor++] = code;
  else if (bytes == 2) {
    editor[editor_cursor++] = 0xc0 | (code >> 6); editor[editor_cursor++] = 0x80 | (code & 63);
  } else if (bytes == 3) {
    editor[editor_cursor++] = 0xe0 | (code >> 12); editor[editor_cursor++] = 0x80 | ((code >> 6) & 63);
    editor[editor_cursor++] = 0x80 | (code & 63);
  } else {
    editor[editor_cursor++] = 0xf0 | (code >> 18); editor[editor_cursor++] = 0x80 | ((code >> 12) & 63);
    editor[editor_cursor++] = 0x80 | ((code >> 6) & 63); editor[editor_cursor++] = 0x80 | (code & 63);
  }
  editor_size += bytes;
  editor_characters += code > 0xffff ? 2 : 1;
  dirty = 1;
}

static void browser_open(void) {
  if (!address_length) { set_message("TYPE A WEB ADDRESS FIRST"); return; }
  if (address_length >= 7 && address[0] == 'h' && address[1] == 't' && address[2] == 't' &&
      address[3] == 'p' && address[4] == 's' && address[5] == ':' && address[6] == '/') {
    open_url_requested = 1;
    set_message("OPENING WEBSITE IN NEW TAB");
  } else if (address_length >= 7 && address[0] == 'h' && address[1] == 't' && address[2] == 't' &&
             address[3] == 'p' && address[4] == ':' && address[5] == '/' && address[6] == '/') {
    open_url_requested = 1;
    set_message("OPENING WEBSITE IN NEW TAB");
  } else if (address_length + 8 < URL_CAPACITY) {
    for (uint32_t i = address_length + 1; i > 0; i--) address[i + 7] = address[i - 1];
    const char prefix[] = "https://";
    for (uint32_t i = 0; i < 8; i++) address[i] = prefix[i];
    address_length += 8;
    address[address_length] = 0;
    open_url_requested = 1;
    set_message("OPENING WEBSITE IN NEW TAB");
  }
}

static void paste_text(uint32_t length) {
  if (length > SCRATCH_CAPACITY) {
    set_message("PASTED TEXT TOO LARGE");
    return;
  }
  if (scene == EDITOR) {
    uint32_t position = 0;
    while (position < length) editor_insert(utf8_decode(scratch, length, &position));
  } else if (scene == NEW_FILE) {
    for (uint32_t i = 0; i < length; i++) {
      uint8_t c = scratch[i];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        if (!active_field && name_length < 10) scratch[name_length++] = c;
        else if (active_field && extension_length < 4) scratch[12 + extension_length++] = c;
      }
    }
  } else if (scene == WEB_BROWSER) {
    for (uint32_t i = 0; i < length && address_length < URL_CAPACITY - 1; i++) {
      uint8_t c = scratch[i];
      if (c >= 32 && c < 127) address[address_length++] = (char)c;
    }
    address[address_length] = 0;
  } else if (scene == CALCULATOR) {
    for (uint32_t i = 0; i < length && calc_length < sizeof(calc_expression_buffer) - 1; i++) {
      uint8_t c = scratch[i];
      if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '*' ||
          c == '/' || c == '.' || c == '(' || c == ')') calc_expression_buffer[calc_length++] = c;
    }
  }
  redraw();
}

static void start_boot(void) {
  scene = DESKTOP;
  ram[0] = 0x5031;
}

static void mouse_click(uint32_t x, uint32_t y) {
  if (scene == BOOT) {
    if (y >= 119 && y <= 139 && x >= 55 && x <= 92) ram_choice = ram_choice ? ram_choice - 1 : 4;
    else if (y >= 119 && y <= 139 && x >= 228 && x <= 260) ram_choice = (ram_choice + 1) % 5;
    else if (y >= 143 && y <= 166) start_boot();
    return;
  }
  if (scene == DESKTOP) {
    if (y >= 182 && x <= 48) { show_menu = !show_menu; return; }
    if (show_menu && y >= 95 && y < 174 && x <= 120) {
      uint32_t selection = min_u32((y - 95) / 11, 6);
      select_menu_item(selection);
      return;
    }
    if (x <= 55 && y >= 3 && y < 177) {
      uint32_t selection = (y - 3) / 29;
      if (selection < 6) open_app(selection + 1);
    }
    return;
  }
  if (x >= 297 && y <= 20) { close_app(); return; }
  if (scene == EXPLORER) {
    if (y >= 37 && y < 149) {
      uint32_t row = file_scroll + (y - 37) / 12, count = 0;
      selected_slot = 0xffffffffu;
      for (uint32_t i = 0; i < FILE_COUNT; i++) {
        if (!files[i].used) continue;
        if (count++ == row) { selected_slot = i; break; }
      }
    } else if (y >= 158 && y <= 177) {
      if (x < 68) { name_length = extension_length = active_field = 0; editor_size = editor_characters = 0; save_as_pending = 0; scene = NEW_FILE; }
      else if (x < 120 && selected_slot < FILE_COUNT && files[selected_slot].used) {
        editor_slot = selected_slot; editor_size = files[selected_slot].size;
        for (uint32_t i = 0; i < editor_size; i++) editor[i] = file_data[selected_slot][i];
        editor_characters = utf16_count(editor, editor_size); editor_cursor = editor_size;
        scene = EDITOR; running_apps |= 1u << APP_NOTEPAD;
      } else if (x < 180 && selected_slot < FILE_COUNT && files[selected_slot].used) {
        pixel16_file_delete(selected_slot); selected_slot = 0xffffffffu; dirty = 1;
      } else if (x > 260) close_app();
    } else if (y >= 38 && y < 149 && x > 285 && file_scroll) file_scroll--;
    return;
  }
  if (scene == EDITOR) {
    if (y >= 155 && y <= 176) {
      if (x < 67) save_editor();
      else if (x < 130) { editor_slot = 0xffffffffu; save_editor(); }
      else if (x > 260) close_app();
    }
    return;
  }
  if (scene == NEW_FILE) {
    if (y >= 39 && y <= 57) active_field = 0;
    else if (y >= 75 && y <= 94) active_field = 1;
    else if (y >= 144 && y <= 166) {
      if (x < 76) create_file();
      else { scene = EXPLORER; }
    }
    return;
  }
  if (scene == WEB_BROWSER) {
    if (y >= 36 && y <= 55 && x >= 258) browser_open();
    else if (y >= 37 && y <= 55) return;
    if (y >= 155 && x > 260) close_app();
    return;
  }
  if (scene == CALCULATOR) {
    if (y >= 56 && y < 151) {
      uint32_t col = x < 75 ? 0 : x < 136 ? 1 : x < 197 ? 2 : x < 252 ? 3 : 4;
      uint32_t row = (y - 58) / 24;
      const char *buttons[4][4] = {{"7","8","9","/"},{"4","5","6","*"},{"1","2","3","-"},{"0",".","+","="}};
      if (col == 4 && row == 0) { calc_length = 0; calc_error = 0; calc_result = 0; }
      else if (col < 4 && row < 4) {
        const char *label = buttons[row][col];
        if (label[0] == '=') {
          evaluate_calculator();
        } else if (calc_length < sizeof(calc_expression_buffer) - 1) {
          calc_expression_buffer[calc_length++] = label[0]; calc_error = 0;
        }
      }
    } else if (y >= 155 && x > 260) close_app();
    return;
  }
  if (scene == SYSTEM_INFO) {
    if (y >= 145 && y <= 164 && x >= 14 && x <= 239) input_mode = !input_mode;
    else if (y >= 155 && x > 260) close_app();
    return;
  }
  if (scene == TERMINAL) {
    if (y >= 155 && x > 260) close_app();
    return;
  }
  if ((scene == WEB_BROWSER || scene == CALCULATOR) && y >= 155 && x > 260) close_app();
}

__attribute__((visibility("default")))
uint32_t pixel16_abi_version(void) { return 2; }

__attribute__((visibility("default")))
int32_t pixel16_boot(void) { ram[0] = 0x5031; scene = BOOT; redraw(); return 1; }

__attribute__((visibility("default")))
uint32_t pixel16_screen_width(void) { return SCREEN_WIDTH; }

__attribute__((visibility("default")))
uint32_t pixel16_screen_height(void) { return SCREEN_HEIGHT; }

__attribute__((visibility("default")))
uint32_t pixel16_framebuffer_ptr(void) { return (uint32_t)(uintptr_t)framebuffer; }

__attribute__((visibility("default")))
void pixel16_redraw(void) { redraw(); }

__attribute__((visibility("default")))
void pixel16_mouse(uint32_t x, uint32_t y) {
  if (input_mode && !(scene == SYSTEM_INFO && y >= 145 && y <= 164 && x >= 14 && x <= 239)) return;
  mouse_click(x, y); redraw();
}

__attribute__((visibility("default")))
void pixel16_paste(uint32_t length) { paste_text(length); }

__attribute__((visibility("default")))
void pixel16_key(uint32_t key, uint32_t codepoint) {
  if (!input_mode && (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
      key == KEY_TAB || (key == KEY_ENTER && (scene == BOOT || scene == DESKTOP || scene == EXPLORER)))) {
    redraw();
    return;
  }
  if (scene == BOOT) {
    if (key == KEY_LEFT) ram_choice = ram_choice ? ram_choice - 1 : 4;
    else if (key == KEY_RIGHT) ram_choice = (ram_choice + 1) % 5;
    else if (key == KEY_ENTER) start_boot();
  } else if (key == KEY_ESCAPE) {
    if (scene == NEW_FILE) scene = EXPLORER;
    else if (scene != DESKTOP) close_app();
    else show_menu = 0;
  } else if (scene == DESKTOP) {
    if (show_menu) {
      if (key == KEY_UP) menu_selection = menu_selection ? menu_selection - 1 : 6;
      else if (key == KEY_DOWN) menu_selection = (menu_selection + 1) % 7;
      else if (key == KEY_ENTER) select_menu_item(menu_selection);
    } else if (key == KEY_LEFT || key == KEY_UP) desktop_selection = desktop_selection ? desktop_selection - 1 : 5;
    else if (key == KEY_RIGHT || key == KEY_DOWN) desktop_selection = (desktop_selection + 1) % 6;
    else if (key == KEY_ENTER) open_app(desktop_selection + 1);
    else if (key == KEY_TAB) { show_menu = 1; menu_selection = 0; }
  } else if (scene == EXPLORER) {
    if (key == KEY_UP || key == KEY_DOWN) {
      int32_t current = -1, total = 0;
      for (uint32_t i = 0; i < FILE_COUNT; i++) {
        if (!files[i].used) continue;
        if (i == selected_slot) current = total;
        total++;
      }
      if (total) {
        current = key == KEY_DOWN ? (current + 1) % (int32_t)total :
          current <= 0 ? (int32_t)total - 1 : current - 1;
        selected_slot = 0xffffffffu;
        int32_t position = 0;
        for (uint32_t i = 0; i < FILE_COUNT; i++) {
          if (!files[i].used) continue;
          if (position++ == current) { selected_slot = i; break; }
        }
        file_scroll = current >= 9 ? (uint32_t)current - 8 : 0;
      }
    } else if (key == KEY_ENTER && selected_slot < FILE_COUNT && files[selected_slot].used) {
      editor_slot = selected_slot; editor_size = files[selected_slot].size;
      for (uint32_t i = 0; i < editor_size; i++) editor[i] = file_data[selected_slot][i];
      editor_characters = utf16_count(editor, editor_size); editor_cursor = editor_size; scene = EDITOR;
    } else if (key == KEY_DELETE && selected_slot < FILE_COUNT && files[selected_slot].used) {
      pixel16_file_delete(selected_slot); selected_slot = 0xffffffffu; dirty = 1;
    }
  } else if (scene == EDITOR) {
    if (key == KEY_BACKSPACE) delete_character();
    else if (key == KEY_ENTER) editor_insert('\n');
    else if (key == KEY_TAB) editor_insert(' ');
    else if (key == KEY_DELETE && editor_cursor < editor_size) {
      uint32_t after = editor_cursor + 1;
      while (after < editor_size && (editor[after] & 0xc0) == 0x80) after++;
      uint32_t removed = after - editor_cursor;
      for (uint32_t i = editor_cursor; i + removed < editor_size; i++) editor[i] = editor[i + removed];
      editor_size -= removed; editor_characters--; dirty = 1;
    }
    else if (key == KEY_LEFT && editor_cursor) {
      editor_cursor--;
      while (editor_cursor && (editor[editor_cursor] & 0xc0) == 0x80) editor_cursor--;
    } else if (key == KEY_RIGHT && editor_cursor < editor_size) {
      editor_cursor++;
      while (editor_cursor < editor_size && (editor[editor_cursor] & 0xc0) == 0x80) editor_cursor++;
    }
    else if (key == KEY_UP || key == KEY_DOWN) { }
    else if (codepoint) editor_insert(codepoint);
    if (key == KEY_ESCAPE) close_app();
  } else if (scene == NEW_FILE) {
    if (key == KEY_TAB) active_field = !active_field;
    else if (key == KEY_BACKSPACE) {
      if (active_field && extension_length) scratch[12 + --extension_length] = 0;
      else if (!active_field && name_length) scratch[--name_length] = 0;
    } else if (key == KEY_ENTER) create_file();
    else if (key == KEY_ESCAPE) scene = EXPLORER;
    else if ((codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 'A' && codepoint <= 'Z')) {
      if (!active_field && name_length < 10) scratch[name_length++] = (uint8_t)codepoint;
      else if (active_field && extension_length < 4) scratch[12 + extension_length++] = (uint8_t)codepoint;
    }
  } else if (scene == WEB_BROWSER) {
    if (key == KEY_BACKSPACE && address_length) address[--address_length] = 0;
    else if (key == KEY_ENTER) browser_open();
    else if (key == KEY_ESCAPE) close_app();
    else if (codepoint >= 32 && codepoint < 127 && address_length < URL_CAPACITY - 1) {
      address[address_length++] = (char)codepoint; address[address_length] = 0;
    }
  } else if (scene == CALCULATOR) {
    if (key == KEY_ENTER || codepoint == '=') {
    evaluate_calculator();
    } else if (key == KEY_BACKSPACE && calc_length) calc_length--;
    else if ((codepoint >= '0' && codepoint <= '9') || codepoint == '+' || codepoint == '-' ||
             codepoint == '*' || codepoint == '/' || codepoint == '.' || codepoint == '(' || codepoint == ')') {
      if (calc_length < sizeof(calc_expression_buffer) - 1) calc_expression_buffer[calc_length++] = (uint8_t)codepoint;
    }
  } else if (scene == SYSTEM_INFO && key == KEY_ENTER) {
    input_mode = !input_mode;
  } else if (scene == TERMINAL) {
    if (key == KEY_BACKSPACE && terminal_length) terminal_command[--terminal_length] = 0;
    else if (key == KEY_ENTER) terminal_execute();
    else if (key == KEY_ESCAPE) close_app();
    else if (codepoint >= 32 && codepoint <= 126 &&
        terminal_length < sizeof(terminal_command) - 1) {
      terminal_command[terminal_length++] = (char)codepoint;
      terminal_command[terminal_length] = 0;
    }
  }
  redraw();
}

__attribute__((visibility("default")))
uint32_t pixel16_ram_choice(void) { return ram_choice; }

__attribute__((visibility("default")))
void pixel16_set_ram_choice(uint32_t choice) { if (choice < 5) ram_choice = choice; redraw(); }

__attribute__((visibility("default")))
uint32_t pixel16_input_mode(void) { return input_mode; }

__attribute__((visibility("default")))
void pixel16_set_input_mode(uint32_t mode) { if (mode < 2) input_mode = mode; redraw(); }

__attribute__((visibility("default")))
void pixel16_set_clock(uint32_t minutes) { clock_minutes = minutes % 1440; redraw(); }

__attribute__((visibility("default")))
void pixel16_set_time(uint32_t seconds) { system_time = seconds; }

__attribute__((visibility("default")))
uint32_t pixel16_file_limit(void) { return FILE_COUNT; }

__attribute__((visibility("default")))
uint32_t pixel16_memory_bytes(void) { return 65536; }

__attribute__((visibility("default")))
uint32_t pixel16_read16(uint32_t address_value) { return address_value < 32768 ? ram[address_value] : 0xffffffffu; }

__attribute__((visibility("default")))
int32_t pixel16_write16(uint32_t address_value, uint32_t value) {
  if (address_value >= 32768 || value > 65535) return -1;
  ram[address_value] = (uint16_t)value; return 0;
}

__attribute__((visibility("default")))
void pixel16_storage_error(void) { set_message("C: STORAGE ERROR - CHECK BROWSER"); redraw(); }

__attribute__((visibility("default")))
void pixel16_browser_error(void) { set_message("INVALID WEB ADDRESS"); redraw(); }

__attribute__((visibility("default")))
uint32_t pixel16_take_dirty(void) { uint32_t result = dirty; dirty = 0; return result; }

__attribute__((visibility("default")))
uint32_t pixel16_take_disk_dirty(void) { uint32_t result = disk_dirty; disk_dirty = 0; return result; }

__attribute__((visibility("default")))
uint32_t pixel16_take_fetch_request(void) {
  uint32_t result = fetch_requested;
  fetch_requested = 0;
  return result;
}

__attribute__((visibility("default")))
uint32_t pixel16_fetch_url_ptr(void) { return (uint32_t)(uintptr_t)fetch_address; }

__attribute__((visibility("default")))
uint32_t pixel16_fetch_url_length(void) { return fetch_address_length; }

__attribute__((visibility("default")))
uint32_t pixel16_fetch_name_ptr(void) { return (uint32_t)(uintptr_t)fetch_name; }

__attribute__((visibility("default")))
uint32_t pixel16_fetch_name_length(void) { return fetch_name_length; }

__attribute__((visibility("default")))
uint32_t pixel16_fetch_extension_ptr(void) { return (uint32_t)(uintptr_t)fetch_extension; }

__attribute__((visibility("default")))
uint32_t pixel16_fetch_extension_length(void) { return fetch_extension_length; }

__attribute__((visibility("default")))
uint32_t pixel16_fetch_scratch_ptr(void) { return (uint32_t)(uintptr_t)scratch; }

__attribute__((visibility("default")))
void pixel16_fetch_complete(uint32_t status, uint32_t length) {
  if (!fetch_pending) return;
  fetch_pending = 0;
  if (status) {
    const char *errors[] = {
      "DOWNLOAD FAILED - CHECK URL OR CORS",
      "DOWNLOAD EXCEEDS 8 KB LIMIT",
      "DOWNLOAD IS NOT VALID UTF-8 TEXT",
      "HTTP DOWNLOAD FAILED",
      "URL NOT ALLOWED - USE PUBLIC HTTP(S)"
    };
    terminal_put_text(status <= 5 ? errors[status - 1] : "DOWNLOAD FAILED");
    terminal_new_line();
    redraw();
    return;
  }
  if (length > FETCH_LIMIT) {
    terminal_put_text("DOWNLOAD EXCEEDS 8 KB LIMIT");
    terminal_new_line();
    redraw();
    return;
  }
  uint32_t prefix = fetch_name_length + fetch_extension_length;
  for (uint32_t i = length; i > 0; i--) scratch[prefix + i - 1] = scratch[i - 1];
  for (uint32_t i = 0; i < fetch_name_length; i++) scratch[i] = (uint8_t)fetch_name[i];
  for (uint32_t i = 0; i < fetch_extension_length; i++)
    scratch[fetch_name_length + i] = (uint8_t)fetch_extension[i];
  int32_t slot = pixel16_file_import(fetch_name_length, fetch_extension_length,
    length, system_time);
  if (slot < 0) {
    terminal_put_text(slot == -2 ? "C: HAS 32 FILES MAXIMUM" :
      slot == -3 ? "FILE ALREADY EXISTS" : "C: FILE SAVE FAILED");
  } else {
    dirty = 1;
    terminal_put_text("SAVED C:\\");
    terminal_put_text(fetch_name);
    terminal_put_char('.');
    terminal_put_text(fetch_extension);
  }
  terminal_new_line();
  redraw();
}

__attribute__((visibility("default")))
uint32_t pixel16_disk_count(void) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < VOLUME_COUNT; i++) count += volume_sizes_mb[i] != 0;
  return count;
}

__attribute__((visibility("default")))
uint32_t pixel16_disk_size_mb(uint32_t slot) {
  return slot < VOLUME_COUNT ? volume_sizes_mb[slot] : 0;
}

__attribute__((visibility("default")))
int32_t pixel16_disk_restore(uint32_t slot, uint32_t size_mb) {
  if (slot == 0 || slot >= VOLUME_COUNT || size_mb == 0 ||
      size_mb > VOLUME_SIZE_LIMIT_MB || volume_sizes_mb[slot]) return -1;
  volume_sizes_mb[slot] = size_mb;
  return 0;
}

__attribute__((visibility("default")))
void pixel16_disk_reset(void) {
  for (uint32_t i = 0; i < VOLUME_COUNT; i++) volume_sizes_mb[i] = 0;
  volume_sizes_mb[0] = 16;
  current_volume = disk_dirty = 0;
}

__attribute__((visibility("default")))
void pixel16_disk_storage_error(void) {
  set_message("VIRTUAL DISC SETTINGS COULD NOT BE SAVED");
  redraw();
}

__attribute__((visibility("default")))
uint32_t pixel16_file_count(void) {
  uint32_t count = 0; for (uint32_t i = 0; i < FILE_COUNT; i++) count += files[i].used != 0; return count;
}

__attribute__((visibility("default")))
uint32_t pixel16_file_slot_at(uint32_t index) {
  for (uint32_t i = 0; i < FILE_COUNT; i++) if (files[i].used && !index--) return i;
  return 0xffffffffu;
}

__attribute__((visibility("default")))
uint32_t pixel16_file_name_ptr(uint32_t slot) { return slot < FILE_COUNT && files[slot].used ? (uint32_t)(uintptr_t)files[slot].name : 0; }
__attribute__((visibility("default")))
uint32_t pixel16_file_name_length(uint32_t slot) {
  uint32_t n = 0; if (slot >= FILE_COUNT || !files[slot].used) return 0; while (files[slot].name[n]) n++; return n;
}
__attribute__((visibility("default")))
uint32_t pixel16_file_extension_ptr(uint32_t slot) { return slot < FILE_COUNT && files[slot].used ? (uint32_t)(uintptr_t)files[slot].extension : 0; }
__attribute__((visibility("default")))
uint32_t pixel16_file_extension_length(uint32_t slot) {
  uint32_t n = 0; if (slot >= FILE_COUNT || !files[slot].used) return 0; while (files[slot].extension[n]) n++; return n;
}
__attribute__((visibility("default")))
uint32_t pixel16_file_data_ptr(uint32_t slot) { return slot < FILE_COUNT && files[slot].used ? (uint32_t)(uintptr_t)file_data[slot] : 0; }
__attribute__((visibility("default")))
uint32_t pixel16_file_size(uint32_t slot) { return slot < FILE_COUNT && files[slot].used ? files[slot].size : 0; }
__attribute__((visibility("default")))
uint32_t pixel16_file_modified(uint32_t slot) { return slot < FILE_COUNT && files[slot].used ? files[slot].modified : 0; }
__attribute__((visibility("default")))
uint32_t pixel16_file_used_bytes(void) { return drive_used(); }
__attribute__((visibility("default")))
uint32_t pixel16_scratch_ptr(void) { return (uint32_t)(uintptr_t)scratch; }
__attribute__((visibility("default")))
uint32_t pixel16_scratch_capacity(void) { return SCRATCH_CAPACITY; }
__attribute__((visibility("default")))
uint32_t pixel16_file_bytes_limit(void) { return FILE_CAPACITY; }

__attribute__((visibility("default")))
int32_t pixel16_file_import(uint32_t name_size, uint32_t ext_size, uint32_t data_size, uint32_t modified) {
  if (!name_size || name_size > 10 || !ext_size || ext_size > 4 || data_size > FILE_CAPACITY ||
      utf16_count(scratch + name_size + ext_size, data_size) > NOTE_LIMIT) return -1;
  for (uint32_t i = 0; i < name_size + ext_size; i++) {
    uint8_t c = scratch[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return -1;
  }
  uint32_t slot = FILE_COUNT;
  for (uint32_t i = 0; i < FILE_COUNT; i++) if (!files[i].used) { slot = i; break; }
  if (slot == FILE_COUNT) return -2;
  if (drive_used() > DRIVE_CAPACITY - data_size) return -4;
  for (uint32_t i = 0; i < FILE_COUNT; i++) {
    uint32_t a = 0;
    while (files[i].used && a < name_size) {
      uint8_t left = files[i].name[a], right = scratch[a];
      if (left >= 'a' && left <= 'z') left -= 32;
      if (right >= 'a' && right <= 'z') right -= 32;
      if (left != right) break;
      a++;
    }
    uint32_t b = 0;
    while (files[i].used && b < ext_size) {
      uint8_t left = files[i].extension[b], right = scratch[name_size + b];
      if (left >= 'a' && left <= 'z') left -= 32;
      if (right >= 'a' && right <= 'z') right -= 32;
      if (left != right) break;
      b++;
    }
    if (files[i].used && a == name_size && files[i].name[a] == 0 &&
        b == ext_size && files[i].extension[b] == 0) return -3;
  }
  for (uint32_t i = 0; i < name_size; i++) files[slot].name[i] = (char)scratch[i];
  files[slot].name[name_size] = 0;
  for (uint32_t i = 0; i < ext_size; i++) files[slot].extension[i] = (char)scratch[name_size + i];
  files[slot].extension[ext_size] = 0;
  for (uint32_t i = 0; i < data_size; i++) file_data[slot][i] = scratch[name_size + ext_size + i];
  files[slot].size = data_size; files[slot].modified = modified; files[slot].used = 1;
  return (int32_t)slot;
}

__attribute__((visibility("default")))
int32_t pixel16_file_write(uint32_t slot, uint32_t length, uint32_t modified) {
  if (slot >= FILE_COUNT || !files[slot].used || length > FILE_CAPACITY || utf16_count(scratch, length) > NOTE_LIMIT) return -1;
  if (drive_used() - files[slot].size > DRIVE_CAPACITY - length) return -4;
  for (uint32_t i = 0; i < length; i++) file_data[slot][i] = scratch[i];
  files[slot].size = length; files[slot].modified = modified;
  return 0;
}

__attribute__((visibility("default")))
int32_t pixel16_file_delete(uint32_t slot) {
  if (slot >= FILE_COUNT || !files[slot].used) return -1;
  files[slot].used = 0; files[slot].size = files[slot].modified = 0;
  return 0;
}

__attribute__((visibility("default")))
int32_t pixel16_file_clear(void) {
  for (uint32_t i = 0; i < FILE_COUNT; i++) { files[i].used = 0; files[i].size = files[i].modified = 0; }
  dirty = 1; return 0;
}

static uint32_t calc_position, calc_end;
static void skip_spaces(void) { while (calc_position < calc_end && scratch[calc_position] == ' ') calc_position++; }
static double parse_sum(void);

static double parse_primary(void) {
  skip_spaces();
  if (calc_position >= calc_end) { calc_error = 1; return 0; }
  if (scratch[calc_position] == '(') {
    calc_position++; double value = parse_sum(); skip_spaces();
    if (calc_position >= calc_end || scratch[calc_position++] != ')') calc_error = 1;
    return value;
  }
  uint32_t start = calc_position; double value = 0;
  while (calc_position < calc_end && scratch[calc_position] >= '0' && scratch[calc_position] <= '9')
    value = value * 10 + scratch[calc_position++] - '0';
  if (calc_position < calc_end && scratch[calc_position] == '.') {
    calc_position++; double place = 0.1;
    while (calc_position < calc_end && scratch[calc_position] >= '0' && scratch[calc_position] <= '9') {
      value += (scratch[calc_position++] - '0') * place; place *= 0.1;
    }
  }
  if (calc_position == start) calc_error = 1;
  return value;
}

static double parse_unary(void) {
  skip_spaces();
  if (calc_position < calc_end && (scratch[calc_position] == '+' || scratch[calc_position] == '-')) {
    uint8_t sign = scratch[calc_position++]; double value = parse_unary(); return sign == '-' ? -value : value;
  }
  return parse_primary();
}

static double parse_product(void) {
  double value = parse_unary();
  while (!calc_error) {
    skip_spaces();
    if (calc_position >= calc_end || (scratch[calc_position] != '*' && scratch[calc_position] != '/')) return value;
    uint8_t op = scratch[calc_position++]; double right = parse_unary();
    if (op == '/' && right == 0) { calc_error = 1; return 0; }
    value = op == '*' ? value * right : value / right;
  }
  return 0;
}

static double parse_sum(void) {
  double value = parse_product();
  while (!calc_error) {
    skip_spaces();
    if (calc_position >= calc_end || (scratch[calc_position] != '+' && scratch[calc_position] != '-')) return value;
    uint8_t op = scratch[calc_position++]; double right = parse_product();
    value = op == '+' ? value + right : value - right;
  }
  return 0;
}

__attribute__((visibility("default")))
double pixel16_calculate(uint32_t length) {
  if (!length || length > 128) { calc_error = 1; return 0; }
  calc_position = 0; calc_end = length; calc_error = 0;
  double result = parse_sum(); skip_spaces();
  if (calc_position != calc_end || result != result || result > 1.0e18 || result < -1.0e18) calc_error = 1;
  return calc_error ? 0 : result;
}

static void evaluate_calculator(void) {
  for (uint32_t i = 0; i < calc_length; i++) scratch[i] = calc_expression_buffer[i];
  calc_result = pixel16_calculate(calc_length);
  calc_error = pixel16_calculation_error();
  if (calc_error) return;

  char output[32], reverse[20];
  uint32_t position = 0, reverse_length = 0;
  double value = calc_result;
  if (value < 0) { output[position++] = '-'; value = -value; }
  uint64_t whole = (uint64_t)value;
  do {
    reverse[reverse_length++] = (char)('0' + whole % 10);
    whole /= 10;
  } while (whole && reverse_length < sizeof(reverse));
  while (reverse_length) output[position++] = reverse[--reverse_length];
  double fraction = value - (uint64_t)value;
  if (fraction > 0.0000001) {
    output[position++] = '.';
    uint32_t fraction_start = position;
    for (uint32_t i = 0; i < 6 && fraction > 0.0000001; i++) {
      fraction *= 10;
      uint32_t digit = (uint32_t)fraction;
      output[position++] = (char)('0' + digit);
      fraction -= digit;
    }
    while (position > fraction_start && output[position - 1] == '0') position--;
    if (position == fraction_start) position--;
  }
  calc_length = min_u32(position, sizeof(calc_expression_buffer) - 1);
  for (uint32_t i = 0; i < calc_length; i++) calc_expression_buffer[i] = (uint8_t)output[i];
}

__attribute__((visibility("default")))
uint32_t pixel16_calculation_error(void) { return (uint32_t)calc_error; }

__attribute__((visibility("default")))
int32_t pixel16_process_start(uint32_t id) {
  if (id < 1 || id > 5) return -1;
  running_apps |= 1u << id; return 0;
}

__attribute__((visibility("default")))
int32_t pixel16_process_stop(uint32_t id) {
  if (id < 1 || id > 5) return -1;
  running_apps &= ~(1u << id); return 0;
}

__attribute__((visibility("default")))
uint32_t pixel16_process_count(void) {
  uint32_t bits = running_apps, count = 0;
  while (bits) { count += bits & 1; bits >>= 1; }
  return count;
}

__attribute__((visibility("default")))
uint32_t pixel16_take_open_url(void) {
  uint32_t result = open_url_requested; open_url_requested = 0; return result;
}

__attribute__((visibility("default")))
uint32_t pixel16_url_ptr(void) { return (uint32_t)(uintptr_t)address; }

__attribute__((visibility("default")))
uint32_t pixel16_url_length(void) { return address_length; }
