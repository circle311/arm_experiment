/*
 * Smart Network Clock - S800/TM4C1294 week-2 build
 *
 * Keil project entry file.  User logic is intentionally kept in this file to
 * match the course packaging requirement.
 */

#include <stdint.h>
#include <stdbool.h>

#include "hw_ints.h"
#include "hw_memmap.h"
#include "gpio.h"
#include "hw_i2c.h"
#include "hw_types.h"
#include "i2c.h"
#include "interrupt.h"
#include "pin_map.h"
#include "sysctl.h"
#include "systick.h"
#include "uart.h"

#define SYSTICK_FREQUENCY       1000u
#define SEG_DIGITS              8u
#define UART_LINE_MAX           96u
#define MESSAGE_MAX             32u
#define BOOT_STEP_MS            700u
#define VERSION_STEP_MS         1000u
#define KEY_SCAN_MS             10u
#define KEY_DEBOUNCE_TICKS      3u
#define EDIT_TIMEOUT_MS         5000u
#define EVENT_INTERVAL_MS       1000u
#define UART_PROMPT             "> "

#define CLASS_NUMBER            "2445"
#define STUDENT_CODE            "1910430"
#define STUDENT_NAME_PINYIN     "YUANSHANGZHI"
#define FIRMWARE_VERSION        "V1.2"

#define TCA6424_I2CADDR         0x22u
#define PCA9557_I2CADDR         0x18u

#define PCA9557_OUTPUT          0x01u
#define PCA9557_CONFIG          0x03u

#define TCA6424_CONFIG_PORT0    0x0cu
#define TCA6424_CONFIG_PORT1    0x0du
#define TCA6424_CONFIG_PORT2    0x0eu
#define TCA6424_INPUT_PORT0     0x00u
#define TCA6424_OUTPUT_PORT1    0x05u
#define TCA6424_OUTPUT_PORT2    0x06u

#define LED_HEARTBEAT           0x01u
#define LED_ALARM_ENABLED       0x02u
#define LED_ALARM_RINGING       0x04u
#define LED_EDITING             0x08u
#define LED_UART_ACTIVITY       0x10u
#define LED_KEY_ACTIVITY        0x20u
#define LED_MESSAGE             0x40u
#define LED_FORMAT_RIGHT        0x80u

typedef enum {
    DISPLAY_TIME = 0,
    DISPLAY_DATE_SHORT,
    DISPLAY_DATE_LONG,
    DISPLAY_MESSAGE
} DisplayMode;

typedef enum {
    EDIT_NONE = 0,
    EDIT_TIME,
    EDIT_DATE,
    EDIT_ALARM
} EditMode;

typedef enum {
    FIELD_0 = 0,
    FIELD_1,
    FIELD_2
} EditField;

typedef enum {
    SCROLL_LEFT = 0,
    SCROLL_RIGHT
} ScrollDirection;

typedef enum {
    SPEED_SLOW = 0,
    SPEED_FAST
} ScrollSpeed;

typedef enum {
    KEY_FUNC = 0,
    KEY_SHIFT,
    KEY_ADD,
    KEY_SAVE,
    KEY_DISP,
    KEY_SPEED,
    KEY_FORMAT,
    KEY_EXT,
    KEY_USER1,
    KEY_USER2,
    KEY_COUNT
} KeyId;

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint16_t millisecond;

    uint8_t alarm_hour;
    uint8_t alarm_minute;
    uint8_t alarm_second;
    bool alarm_enabled;
    bool alarm_ringing;
    uint32_t alarm_started_ms;
    uint32_t last_alarm_toggle_ms;
    bool buzzer_on;

    DisplayMode display_mode;
    EditMode edit_mode;
    EditField edit_field;
    ScrollDirection scroll_dir;
    ScrollSpeed scroll_speed;
    bool format_right;
    bool events_enabled;
    bool heartbeat_on;

    uint8_t led_state;
    uint32_t last_tick_ms;
    uint32_t last_event_ms;
    uint32_t last_key_scan_ms;
    uint32_t last_edit_ms;
    uint32_t last_scroll_ms;
    uint8_t scroll_index;
    char message[MESSAGE_MAX + 1u];
} ClockState;

static void Board_Init(void);
static uint32_t Board_Millis(void);
static void Board_DelayMs(uint32_t ms);
static void Board_Seg7Show(const char chars[SEG_DIGITS], uint8_t dp_mask);
static void Board_LedWrite(uint8_t value);
static uint16_t Board_KeyRawMask(void);
static bool Board_UartReadByte(uint8_t *byte);
static void Board_UartWriteByte(uint8_t byte);
static void Board_UartWriteString(const char *text);
static void Board_BuzzerWrite(bool on);
static void display_text_8(const char *text, uint8_t dp_mask);

static void S800_GPIO_Init(void);
static void S800_I2C0_Init(void);
static void S800_UART_Init(void);
static uint8_t I2C0_WriteByte(uint8_t dev_addr, uint8_t reg_addr,
                              uint8_t write_data);
static uint8_t I2C0_ReadByte(uint8_t dev_addr, uint8_t reg_addr);

void SysTick_Handler(void);
void UART0_Handler(void);

static volatile uint32_t g_ms;
static volatile uint8_t g_seg_shadow[SEG_DIGITS];
static volatile uint8_t g_seg_scan_index;
static volatile uint8_t g_uart_activity_ms;
static volatile uint8_t g_key_activity_ms;

static uint32_t g_sys_clock;

static ClockState g_clock;

static char g_uart_line[UART_LINE_MAX + 1u];
static uint8_t g_uart_len;
static uint16_t g_key_last_raw;
static uint16_t g_key_stable_mask;
static uint8_t g_key_debounce[KEY_COUNT];

static bool is_leap_year(uint16_t year)
{
    if ((year % 400u) == 0u) {
        return true;
    }
    if ((year % 100u) == 0u) {
        return false;
    }
    return (year % 4u) == 0u;
}

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {
        31u, 28u, 31u, 30u, 31u, 30u,
        31u, 31u, 30u, 31u, 30u, 31u
    };

    if ((month == 2u) && is_leap_year(year)) {
        return 29u;
    }
    if ((month < 1u) || (month > 12u)) {
        return 31u;
    }
    return days[month - 1u];
}

static void clamp_day(void)
{
    uint8_t max_day = days_in_month(g_clock.year, g_clock.month);

    if (g_clock.day < 1u) {
        g_clock.day = 1u;
    } else if (g_clock.day > max_day) {
        g_clock.day = max_day;
    }
}

static void clock_add_one_second(void)
{
    g_clock.second++;
    if (g_clock.second < 60u) {
        return;
    }
    g_clock.second = 0u;
    g_clock.minute++;

    if (g_clock.minute < 60u) {
        return;
    }
    g_clock.minute = 0u;
    g_clock.hour++;

    if (g_clock.hour < 24u) {
        return;
    }
    g_clock.hour = 0u;
    g_clock.day++;

    if (g_clock.day <= days_in_month(g_clock.year, g_clock.month)) {
        return;
    }
    g_clock.day = 1u;
    g_clock.month++;

    if (g_clock.month <= 12u) {
        return;
    }
    g_clock.month = 1u;
    g_clock.year++;
}

static char digit_u8(uint8_t value, uint8_t tens)
{
    if (tens != 0u) {
        return (char)('0' + ((value / 10u) % 10u));
    }
    return (char)('0' + (value % 10u));
}

static void fill_blank(char chars[SEG_DIGITS])
{
    uint8_t i;

    for (i = 0u; i < SEG_DIGITS; ++i) {
        chars[i] = ' ';
    }
}

static char ascii_upper(char ch)
{
    if ((ch >= 'a') && (ch <= 'z')) {
        return (char)(ch - 'a' + 'A');
    }
    return ch;
}

static bool str_equal_ignore_case(const char *a, const char *b)
{
    while ((*a != '\0') && (*b != '\0')) {
        if (ascii_upper(*a) != ascii_upper(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

static bool str_starts_ignore_case(const char *text, const char *prefix,
                                   const char **rest)
{
    while (*prefix != '\0') {
        if (ascii_upper(*text) != ascii_upper(*prefix)) {
            return false;
        }
        ++text;
        ++prefix;
    }

    *rest = text;
    return true;
}

static const char *skip_spaces(const char *text)
{
    while ((*text == ' ') || (*text == '\t')) {
        ++text;
    }
    return text;
}

static void copy_text(char *dst, uint8_t dst_size, const char *src)
{
    uint8_t i = 0u;

    if (dst_size == 0u) {
        return;
    }

    while ((i + 1u < dst_size) && (src[i] != '\0')) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static uint8_t text_len(const char *text)
{
    uint8_t len = 0u;

    while ((text[len] != '\0') && (len < MESSAGE_MAX)) {
        ++len;
    }
    return len;
}

static bool is_token_left(const char *text)
{
    return str_equal_ignore_case(text, "LEFT") || str_equal_ignore_case(text, "L");
}

static bool is_token_right(const char *text)
{
    return str_equal_ignore_case(text, "RIGHT") || str_equal_ignore_case(text, "R");
}

static bool is_token_fast(const char *text)
{
    return str_equal_ignore_case(text, "FAST") || str_equal_ignore_case(text, "F");
}

static bool is_token_slow(const char *text)
{
    return str_equal_ignore_case(text, "SLOW") || str_equal_ignore_case(text, "S");
}

static bool is_token_on(const char *text)
{
    return str_equal_ignore_case(text, "ON") || str_equal_ignore_case(text, "1");
}

static bool is_token_off(const char *text)
{
    return str_equal_ignore_case(text, "OFF") || str_equal_ignore_case(text, "0");
}

static bool consume_set_separator(const char **text)
{
    const char *p = *text;

    if ((*p != '=') && (*p != ' ') && (*p != '\t') && (*p != ':')) {
        return false;
    }

    do {
        ++p;
    } while ((*p == ' ') || (*p == '\t'));

    *text = skip_spaces(p);
    return true;
}

static bool parse_uint_range(const char **text, uint8_t min_digits,
                             uint8_t max_digits, uint16_t max_value,
                             uint16_t *value)
{
    const char *p = skip_spaces(*text);
    uint8_t digits = 0u;
    uint16_t result = 0u;

    while ((*p >= '0') && (*p <= '9') && (digits < max_digits)) {
        result = (uint16_t)((result * 10u) + (uint16_t)(*p - '0'));
        ++p;
        ++digits;
    }

    if ((digits < min_digits) || (result > max_value)) {
        return false;
    }

    *value = result;
    *text = p;
    return true;
}

static bool consume_char(const char **text, char a, char b, char c)
{
    const char *p = skip_spaces(*text);

    if ((*p != a) && (*p != b) && (*p != c)) {
        return false;
    }

    ++p;
    *text = p;
    return true;
}

static bool parse_time_value(const char *text, uint8_t *hour,
                             uint8_t *minute, uint8_t *second)
{
    const char *p = text;
    uint16_t hh;
    uint16_t mm;
    uint16_t ss;

    if (!parse_uint_range(&p, 1u, 2u, 23u, &hh)) {
        return false;
    }
    if (!consume_char(&p, ':', '.', ':')) {
        return false;
    }
    if (!parse_uint_range(&p, 1u, 2u, 59u, &mm)) {
        return false;
    }
    if (!consume_char(&p, ':', '.', ':')) {
        return false;
    }
    if (!parse_uint_range(&p, 1u, 2u, 59u, &ss)) {
        return false;
    }

    p = skip_spaces(p);
    if (*p != '\0') {
        return false;
    }

    *hour = (uint8_t)hh;
    *minute = (uint8_t)mm;
    *second = (uint8_t)ss;
    return true;
}

static bool parse_date_value(const char *text, uint16_t *year,
                             uint8_t *month, uint8_t *day)
{
    const char *p = text;
    uint16_t yy;
    uint16_t mm;
    uint16_t dd;

    if (!parse_uint_range(&p, 2u, 4u, 9999u, &yy)) {
        return false;
    }
    if (yy < 100u) {
        yy = (uint16_t)(2000u + yy);
    }
    if (!consume_char(&p, '-', '.', '/')) {
        return false;
    }
    if (!parse_uint_range(&p, 1u, 2u, 12u, &mm)) {
        return false;
    }
    if (!consume_char(&p, '-', '.', '/')) {
        return false;
    }
    if (!parse_uint_range(&p, 1u, 2u, 31u, &dd)) {
        return false;
    }

    p = skip_spaces(p);
    if (*p != '\0') {
        return false;
    }
    if ((mm < 1u) || (dd < 1u) ||
        (dd > days_in_month(yy, (uint8_t)mm))) {
        return false;
    }

    *year = yy;
    *month = (uint8_t)mm;
    *day = (uint8_t)dd;
    return true;
}

static bool line_payload_after_prefix(const char *line, const char *prefix,
                                      const char **payload)
{
    const char *rest;

    if (!str_starts_ignore_case(line, prefix, &rest)) {
        return false;
    }
    if (!consume_set_separator(&rest)) {
        return false;
    }

    *payload = rest;
    return true;
}

static const char *key_name(KeyId key)
{
    static const char *names[] = {
        "FUNC", "SHIFT", "ADD", "SAVE", "DISP",
        "SPEED", "FORMAT", "EXT", "USER1", "USER2"
    };

    if ((uint8_t)key >= (uint8_t)KEY_COUNT) {
        return "UNKNOWN";
    }
    return names[(uint8_t)key];
}

static void make_time_display(char chars[SEG_DIGITS], uint8_t *dp_mask)
{
    chars[0] = digit_u8(g_clock.hour, 1u);
    chars[1] = digit_u8(g_clock.hour, 0u);
    chars[2] = digit_u8(g_clock.minute, 1u);
    chars[3] = digit_u8(g_clock.minute, 0u);
    chars[4] = digit_u8(g_clock.second, 1u);
    chars[5] = digit_u8(g_clock.second, 0u);
    chars[6] = ' ';
    chars[7] = ' ';
    *dp_mask = (uint8_t)((1u << 1u) | (1u << 3u));
}

static void make_short_date_display(char chars[SEG_DIGITS], uint8_t *dp_mask)
{
    uint8_t yy = (uint8_t)(g_clock.year % 100u);

    chars[0] = digit_u8(yy, 1u);
    chars[1] = digit_u8(yy, 0u);
    chars[2] = digit_u8(g_clock.month, 1u);
    chars[3] = digit_u8(g_clock.month, 0u);
    chars[4] = digit_u8(g_clock.day, 1u);
    chars[5] = digit_u8(g_clock.day, 0u);
    chars[6] = ' ';
    chars[7] = ' ';
    *dp_mask = (uint8_t)((1u << 1u) | (1u << 3u));
}

static void make_long_date_display(char chars[SEG_DIGITS], uint8_t *dp_mask)
{
    chars[0] = (char)('0' + ((g_clock.year / 1000u) % 10u));
    chars[1] = (char)('0' + ((g_clock.year / 100u) % 10u));
    chars[2] = (char)('0' + ((g_clock.year / 10u) % 10u));
    chars[3] = (char)('0' + (g_clock.year % 10u));
    chars[4] = digit_u8(g_clock.month, 1u);
    chars[5] = digit_u8(g_clock.month, 0u);
    chars[6] = digit_u8(g_clock.day, 1u);
    chars[7] = digit_u8(g_clock.day, 0u);
    *dp_mask = (uint8_t)(1u << 3u);
}

static void make_message_display(char chars[SEG_DIGITS], uint8_t *dp_mask)
{
    uint8_t len = text_len(g_clock.message);
    uint8_t i;

    fill_blank(chars);
    *dp_mask = 0u;
    if (len == 0u) {
        return;
    }

    if (len <= SEG_DIGITS) {
        for (i = 0u; i < len; ++i) {
            chars[i] = g_clock.message[i];
        }
        return;
    }

    for (i = 0u; i < SEG_DIGITS; ++i) {
        uint8_t pos;
        if (g_clock.scroll_dir == SCROLL_LEFT) {
            pos = (uint8_t)((g_clock.scroll_index + i) % len);
        } else {
            pos = (uint8_t)((g_clock.scroll_index + len - i) % len);
        }
        chars[i] = g_clock.message[pos];
    }
}

static void apply_format(char chars[SEG_DIGITS], uint8_t *dp_mask)
{
    char tmp[SEG_DIGITS];
    uint8_t new_dp = 0u;
    uint8_t i;

    if (!g_clock.format_right) {
        return;
    }

    for (i = 0u; i < SEG_DIGITS; ++i) {
        uint8_t src = (uint8_t)(SEG_DIGITS - 1u - i);
        tmp[i] = chars[src];
        if ((*dp_mask & (uint8_t)(1u << src)) != 0u) {
            new_dp |= (uint8_t)(1u << i);
        }
    }

    for (i = 0u; i < SEG_DIGITS; ++i) {
        chars[i] = tmp[i];
    }
    *dp_mask = new_dp;
}

static void display_render(void)
{
    char chars[SEG_DIGITS];
    uint8_t dp_mask = 0u;

    fill_blank(chars);
    if (g_clock.alarm_ringing && ((Board_Millis() / 250u) & 1u)) {
        display_text_8("ALARM   ", 0u);
        return;
    }

    if (g_clock.edit_mode == EDIT_TIME) {
        make_time_display(chars, &dp_mask);
    } else if (g_clock.edit_mode == EDIT_DATE) {
        make_short_date_display(chars, &dp_mask);
    } else if (g_clock.edit_mode == EDIT_ALARM) {
        chars[0] = digit_u8(g_clock.alarm_hour, 1u);
        chars[1] = digit_u8(g_clock.alarm_hour, 0u);
        chars[2] = digit_u8(g_clock.alarm_minute, 1u);
        chars[3] = digit_u8(g_clock.alarm_minute, 0u);
        chars[4] = digit_u8(g_clock.alarm_second, 1u);
        chars[5] = digit_u8(g_clock.alarm_second, 0u);
        dp_mask = (uint8_t)((1u << 1u) | (1u << 3u));
    } else if (g_clock.display_mode == DISPLAY_DATE_SHORT) {
        make_short_date_display(chars, &dp_mask);
    } else if (g_clock.display_mode == DISPLAY_DATE_LONG) {
        make_long_date_display(chars, &dp_mask);
    } else if (g_clock.display_mode == DISPLAY_MESSAGE) {
        make_message_display(chars, &dp_mask);
    } else {
        make_time_display(chars, &dp_mask);
    }

    if ((g_clock.edit_mode != EDIT_NONE) && ((Board_Millis() / 350u) & 1u)) {
        uint8_t offset = 0u;
        if ((g_clock.edit_mode == EDIT_TIME) ||
            (g_clock.edit_mode == EDIT_ALARM)) {
            offset = (uint8_t)g_clock.edit_field * 2u;
        } else if (g_clock.edit_mode == EDIT_DATE) {
            offset = (uint8_t)g_clock.edit_field * 2u;
        }
        chars[offset] = ' ';
        chars[offset + 1u] = ' ';
    }

    apply_format(chars, &dp_mask);
    Board_Seg7Show(chars, dp_mask);
}

static void display_text_8(const char *text, uint8_t dp_mask)
{
    char chars[SEG_DIGITS];
    uint8_t i;

    fill_blank(chars);
    for (i = 0u; (i < SEG_DIGITS) && (text[i] != '\0'); ++i) {
        chars[i] = text[i];
    }
    Board_Seg7Show(chars, dp_mask);
}

static void boot_animation(void)
{
    display_text_8("88888888", 0xffu);
    Board_LedWrite(0xffu);
    Board_DelayMs(BOOT_STEP_MS);

    display_text_8("        ", 0x00u);
    Board_LedWrite(0x00u);
    Board_DelayMs(BOOT_STEP_MS);

    display_text_8(STUDENT_CODE, 0x00u);
    Board_LedWrite(0xffu);
    Board_DelayMs(BOOT_STEP_MS);
    Board_LedWrite(0x00u);
    Board_DelayMs(200u);

    display_text_8(STUDENT_NAME_PINYIN, 0x00u);
    Board_LedWrite(0xffu);
    Board_DelayMs(BOOT_STEP_MS);
    Board_LedWrite(0x00u);
    Board_DelayMs(200u);

    display_text_8(FIRMWARE_VERSION, 0x00u);
    Board_DelayMs(VERSION_STEP_MS);
}

static void uart_write_u8_2(uint8_t value)
{
    Board_UartWriteByte((uint8_t)digit_u8(value, 1u));
    Board_UartWriteByte((uint8_t)digit_u8(value, 0u));
}

static void uart_write_u16_4(uint16_t value)
{
    Board_UartWriteByte((uint8_t)('0' + ((value / 1000u) % 10u)));
    Board_UartWriteByte((uint8_t)('0' + ((value / 100u) % 10u)));
    Board_UartWriteByte((uint8_t)('0' + ((value / 10u) % 10u)));
    Board_UartWriteByte((uint8_t)('0' + (value % 10u)));
}

static void uart_print_time(void)
{
    Board_UartWriteString("TIME ");
    uart_write_u8_2(g_clock.hour);
    Board_UartWriteByte(':');
    uart_write_u8_2(g_clock.minute);
    Board_UartWriteByte(':');
    uart_write_u8_2(g_clock.second);
    Board_UartWriteString("\r\n");
}

static void uart_print_date(void)
{
    Board_UartWriteString("DATE ");
    uart_write_u16_4(g_clock.year);
    Board_UartWriteByte('-');
    uart_write_u8_2(g_clock.month);
    Board_UartWriteByte('-');
    uart_write_u8_2(g_clock.day);
    Board_UartWriteString("\r\n");
}

static void uart_print_alarm(void)
{
    Board_UartWriteString("ALARM ");
    uart_write_u8_2(g_clock.alarm_hour);
    Board_UartWriteByte(':');
    uart_write_u8_2(g_clock.alarm_minute);
    Board_UartWriteByte(':');
    uart_write_u8_2(g_clock.alarm_second);
    Board_UartWriteByte(' ');
    Board_UartWriteString(g_clock.alarm_enabled ? "ON\r\n" : "OFF\r\n");
}

static void uart_print_led(void)
{
    static const char hex[] = "0123456789ABCDEF";
    Board_UartWriteString("LED 0x");
    Board_UartWriteByte((uint8_t)hex[(g_clock.led_state >> 4) & 0x0fu]);
    Board_UartWriteByte((uint8_t)hex[g_clock.led_state & 0x0fu]);
    Board_UartWriteString("\r\n");
}

static void uart_print_disp_status(void)
{
    Board_UartWriteString("DISP ");
    if (g_clock.display_mode == DISPLAY_DATE_SHORT) {
        Board_UartWriteString("DATE ");
    } else if (g_clock.display_mode == DISPLAY_DATE_LONG) {
        Board_UartWriteString("YEAR ");
    } else if (g_clock.display_mode == DISPLAY_MESSAGE) {
        Board_UartWriteString("MSG ");
    } else {
        Board_UartWriteString("TIME ");
    }
    Board_UartWriteString(g_clock.format_right ? "RIGHT\r\n" : "LEFT\r\n");
}

static void uart_print_disp_event(void)
{
    if (!g_clock.events_enabled) {
        return;
    }

    Board_UartWriteString("*EVT:");
    uart_print_disp_status();
}

static void uart_print_led_event(void)
{
    static const char hex[] = "0123456789ABCDEF";

    if (!g_clock.events_enabled) {
        return;
    }

    Board_UartWriteString("*EVT:LED 0x");
    Board_UartWriteByte((uint8_t)hex[(g_clock.led_state >> 4) & 0x0fu]);
    Board_UartWriteByte((uint8_t)hex[g_clock.led_state & 0x0fu]);
    Board_UartWriteString("\r\n");
}

static void uart_print_mode_event(void)
{
    if (!g_clock.events_enabled) {
        return;
    }

    Board_UartWriteString("*EVT:MODE ");
    if (g_clock.edit_mode == EDIT_TIME) {
        Board_UartWriteString("EDIT_TIME\r\n");
    } else if (g_clock.edit_mode == EDIT_DATE) {
        Board_UartWriteString("EDIT_DATE\r\n");
    } else if (g_clock.edit_mode == EDIT_ALARM) {
        Board_UartWriteString("EDIT_ALARM\r\n");
    } else {
        Board_UartWriteString("NORMAL\r\n");
    }
}

static void update_leds(void)
{
    uint8_t led = 0u;

    if (g_clock.heartbeat_on) {
        led |= LED_HEARTBEAT;
    }
    if (g_clock.alarm_enabled) {
        led |= LED_ALARM_ENABLED;
    }
    if (g_clock.alarm_ringing) {
        led |= LED_ALARM_RINGING;
    }
    if (g_clock.edit_mode != EDIT_NONE) {
        led |= LED_EDITING;
    }
    if (g_uart_activity_ms != 0u) {
        led |= LED_UART_ACTIVITY;
    }
    if (g_key_activity_ms != 0u) {
        led |= LED_KEY_ACTIVITY;
    }
    if (g_clock.display_mode == DISPLAY_MESSAGE) {
        led |= LED_MESSAGE;
    }
    if (g_clock.format_right) {
        led |= LED_FORMAT_RIGHT;
    }

    if (led != g_clock.led_state) {
        g_clock.led_state = led;
        Board_LedWrite(led);
    }
}

static void set_edit_mode(EditMode mode)
{
    g_clock.edit_mode = mode;
    g_clock.edit_field = FIELD_0;
    g_clock.last_edit_ms = Board_Millis();
    uart_print_mode_event();
}

static void stop_alarm(void)
{
    g_clock.alarm_ringing = false;
    g_clock.buzzer_on = false;
    Board_BuzzerWrite(false);
}

static void next_display_mode(void)
{
    if (g_clock.display_mode == DISPLAY_MESSAGE) {
        g_clock.display_mode = DISPLAY_TIME;
    } else {
        g_clock.display_mode =
            (DisplayMode)(((uint8_t)g_clock.display_mode + 1u) % 4u);
    }
    display_render();
    uart_print_disp_event();
}

static void edit_add_current_field(void)
{
    if (g_clock.edit_mode == EDIT_TIME) {
        if (g_clock.edit_field == FIELD_0) {
            g_clock.hour = (uint8_t)((g_clock.hour + 1u) % 24u);
        } else if (g_clock.edit_field == FIELD_1) {
            g_clock.minute = (uint8_t)((g_clock.minute + 1u) % 60u);
        } else {
            g_clock.second = (uint8_t)((g_clock.second + 1u) % 60u);
        }
        g_clock.millisecond = 0u;
    } else if (g_clock.edit_mode == EDIT_DATE) {
        if (g_clock.edit_field == FIELD_0) {
            g_clock.year++;
            if (g_clock.year > 2099u) {
                g_clock.year = 2000u;
            }
        } else if (g_clock.edit_field == FIELD_1) {
            g_clock.month++;
            if (g_clock.month > 12u) {
                g_clock.month = 1u;
            }
        } else {
            g_clock.day++;
            if (g_clock.day > days_in_month(g_clock.year, g_clock.month)) {
                g_clock.day = 1u;
            }
        }
        clamp_day();
    } else if (g_clock.edit_mode == EDIT_ALARM) {
        if (g_clock.edit_field == FIELD_0) {
            g_clock.alarm_hour = (uint8_t)((g_clock.alarm_hour + 1u) % 24u);
        } else if (g_clock.edit_field == FIELD_1) {
            g_clock.alarm_minute =
                (uint8_t)((g_clock.alarm_minute + 1u) % 60u);
        } else {
            g_clock.alarm_second =
                (uint8_t)((g_clock.alarm_second + 1u) % 60u);
        }
        g_clock.alarm_enabled = true;
    }

    g_clock.last_edit_ms = Board_Millis();
    display_render();
}

static void handle_key_press(KeyId key)
{
    g_key_activity_ms = 120u;
    if (g_clock.events_enabled) {
        Board_UartWriteString("*EVT:KEY ");
        Board_UartWriteString(key_name(key));
        Board_UartWriteString(" DOWN\r\n");
    }

    if (g_clock.alarm_ringing && key == KEY_FUNC) {
        stop_alarm();
        return;
    }

    if (key == KEY_FUNC) {
        if (g_clock.edit_mode == EDIT_NONE) {
            set_edit_mode(EDIT_TIME);
        } else if (g_clock.edit_mode == EDIT_TIME) {
            set_edit_mode(EDIT_DATE);
        } else if (g_clock.edit_mode == EDIT_DATE) {
            set_edit_mode(EDIT_ALARM);
        } else {
            set_edit_mode(EDIT_NONE);
        }
    } else if (key == KEY_SHIFT) {
        if (g_clock.edit_mode != EDIT_NONE) {
            g_clock.edit_field =
                (EditField)(((uint8_t)g_clock.edit_field + 1u) % 3u);
            g_clock.last_edit_ms = Board_Millis();
        }
    } else if (key == KEY_ADD) {
        edit_add_current_field();
    } else if (key == KEY_SAVE) {
        if (g_clock.edit_mode != EDIT_NONE) {
            set_edit_mode(EDIT_NONE);
            Board_UartWriteString("OK SAVE\r\n");
        }
    } else if (key == KEY_DISP) {
        next_display_mode();
    } else if (key == KEY_SPEED) {
        g_clock.scroll_speed =
            (g_clock.scroll_speed == SPEED_SLOW) ? SPEED_FAST : SPEED_SLOW;
        Board_UartWriteString("OK SPEED\r\n");
    } else if (key == KEY_FORMAT) {
        g_clock.format_right = !g_clock.format_right;
        display_render();
        Board_UartWriteString("OK FORMAT\r\n");
    } else if (key == KEY_EXT) {
        Board_UartWriteString("*EVT:KEY EXT ACTION\r\n");
    } else if (key == KEY_USER1) {
        Board_UartWriteString("*EVT:KEY USER1 SYNC_REQUEST\r\n");
    } else if (key == KEY_USER2) {
        g_clock.display_mode = DISPLAY_MESSAGE;
        copy_text(g_clock.message, sizeof(g_clock.message), "WEATHER WAIT");
        Board_UartWriteString("*EVT:KEY USER2 WEATHER\r\n");
    }

    display_render();
}

static void handle_key_release(KeyId key)
{
    if (g_clock.events_enabled) {
        Board_UartWriteString("*EVT:KEY ");
        Board_UartWriteString(key_name(key));
        Board_UartWriteString(" UP\r\n");
    }
}

static void keys_poll(void)
{
    uint32_t now = Board_Millis();
    uint16_t raw;
    uint16_t changed;
    uint8_t i;

    if ((uint32_t)(now - g_clock.last_key_scan_ms) < KEY_SCAN_MS) {
        return;
    }
    g_clock.last_key_scan_ms = now;

    raw = Board_KeyRawMask();
    changed = (uint16_t)(raw ^ g_key_last_raw);
    if (changed != 0u) {
        g_key_last_raw = raw;
        for (i = 0u; i < (uint8_t)KEY_COUNT; ++i) {
            if ((changed & (uint16_t)(1u << i)) != 0u) {
                g_key_debounce[i] = 0u;
            }
        }
    }

    for (i = 0u; i < (uint8_t)KEY_COUNT; ++i) {
        uint16_t bit = (uint16_t)(1u << i);
        bool raw_on = (raw & bit) != 0u;
        bool stable_on = (g_key_stable_mask & bit) != 0u;

        if (raw_on == stable_on) {
            g_key_debounce[i] = KEY_DEBOUNCE_TICKS;
            continue;
        }

        if (g_key_debounce[i] < KEY_DEBOUNCE_TICKS) {
            g_key_debounce[i]++;
            continue;
        }

        if (raw_on) {
            g_key_stable_mask |= bit;
            handle_key_press((KeyId)i);
        } else {
            g_key_stable_mask &= (uint16_t)(~bit);
            handle_key_release((KeyId)i);
        }
    }
}

static void alarm_poll(void)
{
    uint32_t now = Board_Millis();

    if (g_clock.alarm_enabled && !g_clock.alarm_ringing &&
        (g_clock.hour == g_clock.alarm_hour) &&
        (g_clock.minute == g_clock.alarm_minute) &&
        (g_clock.second == g_clock.alarm_second) &&
        (g_clock.millisecond == 0u)) {
        g_clock.alarm_ringing = true;
        g_clock.alarm_started_ms = now;
        g_clock.last_alarm_toggle_ms = now;
        g_clock.buzzer_on = true;
        Board_BuzzerWrite(true);
        if (g_clock.events_enabled) {
            Board_UartWriteString("*EVT:MODE ALARM_RING\r\n");
        }
    }

    if (!g_clock.alarm_ringing) {
        return;
    }

    if ((uint32_t)(now - g_clock.alarm_started_ms) >= 10000u) {
        stop_alarm();
        return;
    }

    if ((uint32_t)(now - g_clock.last_alarm_toggle_ms) >= 250u) {
        g_clock.last_alarm_toggle_ms = now;
        g_clock.buzzer_on = !g_clock.buzzer_on;
        Board_BuzzerWrite(g_clock.buzzer_on);
    }
}

static void scroll_poll(void)
{
    uint32_t now = Board_Millis();
    uint32_t interval = (g_clock.scroll_speed == SPEED_FAST) ? 180u : 420u;
    uint8_t len = text_len(g_clock.message);

    if (g_clock.display_mode != DISPLAY_MESSAGE || len <= SEG_DIGITS) {
        return;
    }

    if ((uint32_t)(now - g_clock.last_scroll_ms) >= interval) {
        g_clock.last_scroll_ms = now;
        g_clock.scroll_index++;
        if (g_clock.scroll_index >= len) {
            g_clock.scroll_index = 0u;
        }
        display_render();
    }
}

static void event_poll(void)
{
    uint32_t now = Board_Millis();

    if ((uint32_t)(now - g_clock.last_event_ms) >= EVENT_INTERVAL_MS) {
        g_clock.last_event_ms = now;
        g_clock.heartbeat_on = !g_clock.heartbeat_on;
        update_leds();
        uart_print_disp_event();
        uart_print_led_event();
    }
}

static void edit_timeout_poll(void)
{
    if ((g_clock.edit_mode != EDIT_NONE) &&
        ((uint32_t)(Board_Millis() - g_clock.last_edit_ms) >=
         EDIT_TIMEOUT_MS)) {
        set_edit_mode(EDIT_NONE);
    }
}

static void clock_poll(void)
{
    uint32_t now = Board_Millis();

    while ((uint32_t)(now - g_clock.last_tick_ms) >= 1u) {
        g_clock.last_tick_ms++;
        g_clock.millisecond++;
        if (g_clock.millisecond >= 1000u) {
            g_clock.millisecond = 0u;
            clock_add_one_second();
            display_render();
        }
    }
}

static void state_poll(void)
{
    clock_poll();
    keys_poll();
    edit_timeout_poll();
    alarm_poll();
    scroll_poll();
    event_poll();
    update_leds();
}

static void reset_state(void)
{
    g_clock.year = 2026u;
    g_clock.month = 6u;
    g_clock.day = 1u;
    g_clock.hour = 12u;
    g_clock.minute = 0u;
    g_clock.second = 0u;
    g_clock.millisecond = 0u;
    g_clock.alarm_hour = 7u;
    g_clock.alarm_minute = 0u;
    g_clock.alarm_second = 0u;
    g_clock.alarm_enabled = false;
    stop_alarm();
    g_clock.display_mode = DISPLAY_TIME;
    g_clock.edit_mode = EDIT_NONE;
    g_clock.edit_field = FIELD_0;
    g_clock.scroll_dir = SCROLL_LEFT;
    g_clock.scroll_speed = SPEED_SLOW;
    g_clock.format_right = false;
    g_clock.events_enabled = false;
    g_clock.scroll_index = 0u;
    g_clock.last_tick_ms = Board_Millis();
    g_clock.last_event_ms = g_clock.last_tick_ms;
    copy_text(g_clock.message, sizeof(g_clock.message), "HELLO S800 CLOCK");
    display_render();
}

static void handle_set_command(const char *payload)
{
    const char *value;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint16_t year;
    uint8_t month;
    uint8_t day;

    payload = skip_spaces(payload);

    if (line_payload_after_prefix(payload, "TIME", &value)) {
        if (!parse_time_value(value, &hour, &minute, &second)) {
            Board_UartWriteString("ERROR RANGE\r\n");
            return;
        }
        g_clock.hour = hour;
        g_clock.minute = minute;
        g_clock.second = second;
        g_clock.millisecond = 0u;
        g_clock.last_tick_ms = Board_Millis();
        display_render();
        Board_UartWriteString("OK TIME\r\n");
    } else if (line_payload_after_prefix(payload, "DATE", &value)) {
        if (!parse_date_value(value, &year, &month, &day)) {
            Board_UartWriteString("ERROR RANGE\r\n");
            return;
        }
        g_clock.year = year;
        g_clock.month = month;
        g_clock.day = day;
        display_render();
        Board_UartWriteString("OK DATE\r\n");
    } else if (line_payload_after_prefix(payload, "ALARM", &value)) {
        if (is_token_on(value)) {
            g_clock.alarm_enabled = true;
            Board_UartWriteString("OK ALARM ON\r\n");
        } else if (is_token_off(value)) {
            g_clock.alarm_enabled = false;
            stop_alarm();
            Board_UartWriteString("OK ALARM OFF\r\n");
        } else if (parse_time_value(value, &hour, &minute, &second)) {
            g_clock.alarm_hour = hour;
            g_clock.alarm_minute = minute;
            g_clock.alarm_second = second;
            g_clock.alarm_enabled = true;
            Board_UartWriteString("OK ALARM\r\n");
        } else {
            Board_UartWriteString("ERROR PARAM\r\n");
        }
    } else if (line_payload_after_prefix(payload, "BEEP", &value) ||
               line_payload_after_prefix(payload, "BUZZ", &value)) {
        if (is_token_on(value)) {
            g_clock.buzzer_on = true;
            Board_BuzzerWrite(true);
            Board_UartWriteString("OK BEEP ON\r\n");
        } else if (is_token_off(value)) {
            g_clock.buzzer_on = false;
            Board_BuzzerWrite(false);
            Board_UartWriteString("OK BEEP OFF\r\n");
        } else {
            Board_UartWriteString("ERROR PARAM\r\n");
        }
    } else if (line_payload_after_prefix(payload, "DISP", &value)) {
        if (str_equal_ignore_case(value, "TIME")) {
            g_clock.display_mode = DISPLAY_TIME;
        } else if (str_equal_ignore_case(value, "DATE")) {
            g_clock.display_mode = DISPLAY_DATE_SHORT;
        } else if (str_equal_ignore_case(value, "YEAR")) {
            g_clock.display_mode = DISPLAY_DATE_LONG;
        } else if (str_equal_ignore_case(value, "MSG") ||
                   str_equal_ignore_case(value, "MESSAGE")) {
            g_clock.display_mode = DISPLAY_MESSAGE;
        } else {
            Board_UartWriteString("ERROR PARAM\r\n");
            return;
        }
        display_render();
        Board_UartWriteString("OK DISP\r\n");
    } else if (line_payload_after_prefix(payload, "FORMAT", &value)) {
        if (is_token_left(value)) {
            g_clock.format_right = false;
        } else if (is_token_right(value)) {
            g_clock.format_right = true;
        } else {
            Board_UartWriteString("ERROR PARAM\r\n");
            return;
        }
        display_render();
        Board_UartWriteString("OK FORMAT\r\n");
    } else if (line_payload_after_prefix(payload, "SPEED", &value)) {
        if (is_token_slow(value)) {
            g_clock.scroll_speed = SPEED_SLOW;
        } else if (is_token_fast(value)) {
            g_clock.scroll_speed = SPEED_FAST;
        } else {
            Board_UartWriteString("ERROR PARAM\r\n");
            return;
        }
        Board_UartWriteString("OK SPEED\r\n");
    } else if (line_payload_after_prefix(payload, "EVT", &value) ||
               line_payload_after_prefix(payload, "EVENT", &value)) {
        if (is_token_on(value)) {
            g_clock.events_enabled = true;
            Board_UartWriteString("OK EVT ON\r\n");
        } else if (is_token_off(value)) {
            g_clock.events_enabled = false;
            Board_UartWriteString("OK EVT OFF\r\n");
        } else {
            Board_UartWriteString("ERROR PARAM\r\n");
        }
    } else if (line_payload_after_prefix(payload, "SCROLL", &value)) {
        if (is_token_left(value)) {
            g_clock.scroll_dir = SCROLL_LEFT;
        } else if (is_token_right(value)) {
            g_clock.scroll_dir = SCROLL_RIGHT;
        } else {
            Board_UartWriteString("ERROR PARAM\r\n");
            return;
        }
        Board_UartWriteString("OK SCROLL\r\n");
    } else if (line_payload_after_prefix(payload, "MSG", &value) ||
               line_payload_after_prefix(payload, "MESSAGE", &value)) {
        copy_text(g_clock.message, sizeof(g_clock.message), value);
        g_clock.display_mode = DISPLAY_MESSAGE;
        g_clock.scroll_index = 0u;
        display_render();
        Board_UartWriteString("OK MSG\r\n");
    } else if (line_payload_after_prefix(payload, "KEY", &value)) {
        if (str_equal_ignore_case(value, "FUNC")) {
            handle_key_press(KEY_FUNC);
        } else if (str_equal_ignore_case(value, "SHIFT")) {
            handle_key_press(KEY_SHIFT);
        } else if (str_equal_ignore_case(value, "ADD")) {
            handle_key_press(KEY_ADD);
        } else if (str_equal_ignore_case(value, "SAVE")) {
            handle_key_press(KEY_SAVE);
        } else if (str_equal_ignore_case(value, "DISP")) {
            handle_key_press(KEY_DISP);
        } else if (str_equal_ignore_case(value, "SPEED")) {
            handle_key_press(KEY_SPEED);
        } else if (str_equal_ignore_case(value, "FORMAT")) {
            handle_key_press(KEY_FORMAT);
        } else if (str_equal_ignore_case(value, "EXT")) {
            handle_key_press(KEY_EXT);
        } else if (str_equal_ignore_case(value, "USER1")) {
            handle_key_press(KEY_USER1);
        } else if (str_equal_ignore_case(value, "USER2")) {
            handle_key_press(KEY_USER2);
        } else {
            Board_UartWriteString("ERROR PARAM\r\n");
            return;
        }
        Board_UartWriteString("OK KEY\r\n");
    } else {
        Board_UartWriteString("ERROR SYNTAX\r\n");
    }
}

static void handle_get_command(const char *payload)
{
    payload = skip_spaces(payload);

    if (str_equal_ignore_case(payload, "TIME")) {
        uart_print_time();
    } else if (str_equal_ignore_case(payload, "DATE")) {
        uart_print_date();
    } else if (str_equal_ignore_case(payload, "ALARM")) {
        uart_print_alarm();
    } else if (str_equal_ignore_case(payload, "LED")) {
        uart_print_led();
    } else if (str_equal_ignore_case(payload, "DISP")) {
        uart_print_disp_status();
    } else if (str_equal_ignore_case(payload, "MSG") ||
               str_equal_ignore_case(payload, "MESSAGE")) {
        Board_UartWriteString("MSG ");
        Board_UartWriteString(g_clock.message);
        Board_UartWriteString("\r\n");
    } else {
        Board_UartWriteString("ERROR PARAM\r\n");
    }
}

static void uart_handle_line(char *line)
{
    const char *payload;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint16_t year;
    uint8_t month;
    uint8_t day;

    g_uart_activity_ms = 120u;
    Board_UartWriteString("RX: ");
    Board_UartWriteString(line);
    Board_UartWriteString("\r\n");

    if (str_equal_ignore_case(line, "*PING")) {
        Board_UartWriteString("*PONG 0\r\n");
    } else if (str_equal_ignore_case(line, "*RST") ||
               str_equal_ignore_case(line, "RST")) {
        reset_state();
        Board_UartWriteString("OK RST\r\n");
    } else if (line_payload_after_prefix(line, "*SET", &payload) ||
               line_payload_after_prefix(line, "SET", &payload)) {
        handle_set_command(payload);
    } else if (line_payload_after_prefix(line, "*GET", &payload) ||
               line_payload_after_prefix(line, "GET", &payload)) {
        handle_get_command(payload);
    } else if (line_payload_after_prefix(line, "*SET:TIME", &payload) ||
               line_payload_after_prefix(line, "TIME", &payload)) {
        if (parse_time_value(payload, &hour, &minute, &second)) {
            g_clock.hour = hour;
            g_clock.minute = minute;
            g_clock.second = second;
            g_clock.millisecond = 0u;
            g_clock.last_tick_ms = Board_Millis();
            display_render();
            Board_UartWriteString("OK TIME\r\n");
        } else {
            Board_UartWriteString("ERROR RANGE\r\n");
        }
    } else if (line_payload_after_prefix(line, "*SET:DATE", &payload) ||
               line_payload_after_prefix(line, "DATE", &payload)) {
        if (parse_date_value(payload, &year, &month, &day)) {
            g_clock.year = year;
            g_clock.month = month;
            g_clock.day = day;
            display_render();
            Board_UartWriteString("OK DATE\r\n");
        } else {
            Board_UartWriteString("ERROR RANGE\r\n");
        }
    } else if (str_equal_ignore_case(line, "*GET:TIME") ||
               str_equal_ignore_case(line, "TIME")) {
        uart_print_time();
    } else if (str_equal_ignore_case(line, "*GET:DATE") ||
               str_equal_ignore_case(line, "DATE")) {
        uart_print_date();
    } else if (str_equal_ignore_case(line, "DISP")) {
        next_display_mode();
        Board_UartWriteString("OK DISPLAY\r\n");
    } else if (str_equal_ignore_case(line, "AT+CLASS")) {
        Board_UartWriteString("CLASS");
        Board_UartWriteString(CLASS_NUMBER);
        Board_UartWriteString("\r\n");
    } else if (str_equal_ignore_case(line, "AT+STUDENTCODE")) {
        Board_UartWriteString("CODE");
        Board_UartWriteString(STUDENT_CODE);
        Board_UartWriteString("\r\n");
    } else if (line_payload_after_prefix(line, "MSG", &payload)) {
        handle_set_command(line);
    } else {
        Board_UartWriteString("ERROR SYNTAX\r\n");
    }

    Board_UartWriteString(UART_PROMPT);
}

static void uart_poll(void)
{
    uint8_t byte;

    while (Board_UartReadByte(&byte)) {
        g_uart_activity_ms = 120u;
        if ((byte == '\r') || (byte == '\n')) {
            if (g_uart_len > 0u) {
                g_uart_line[g_uart_len] = '\0';
                uart_handle_line(g_uart_line);
                g_uart_len = 0u;
            }
            continue;
        }

        if (g_uart_len < UART_LINE_MAX) {
            g_uart_line[g_uart_len++] = (char)byte;
        } else {
            g_uart_len = 0u;
            Board_UartWriteString("\r\nERROR LEN\r\n");
            Board_UartWriteString(UART_PROMPT);
        }
    }
}

int main(void)
{
    Board_Init();
    reset_state();
    boot_animation();

    g_clock.last_tick_ms = Board_Millis();
    g_clock.last_event_ms = g_clock.last_tick_ms;
    g_clock.last_key_scan_ms = g_clock.last_tick_ms;
    g_clock.last_scroll_ms = g_clock.last_tick_ms;
    display_render();

    Board_UartWriteString("\r\nSmart Clock week-2 S800 build ready\r\n");
    Board_UartWriteString("Commands: *PING, *GET TIME, *SET TIME=HH:MM:SS\r\n");
    Board_UartWriteString(UART_PROMPT);

    while (1) {
        state_poll();
        uart_poll();
    }
}

static uint8_t char_to_seg(char ch)
{
    switch (ascii_upper(ch)) {
    case '0': return 0x3fu;
    case '1': return 0x06u;
    case '2': return 0x5bu;
    case '3': return 0x4fu;
    case '4': return 0x66u;
    case '5': return 0x6du;
    case '6': return 0x7du;
    case '7': return 0x07u;
    case '8': return 0x7fu;
    case '9': return 0x6fu;
    case 'A': return 0x77u;
    case 'B': return 0x7cu;
    case 'C': return 0x39u;
    case 'D': return 0x5eu;
    case 'E': return 0x79u;
    case 'F': return 0x71u;
    case 'G': return 0x3du;
    case 'H': return 0x76u;
    case 'I': return 0x06u;
    case 'J': return 0x1eu;
    case 'L': return 0x38u;
    case 'M': return 0x37u;
    case 'N': return 0x54u;
    case 'O': return 0x3fu;
    case 'P': return 0x73u;
    case 'R': return 0x50u;
    case 'S': return 0x6du;
    case 'T': return 0x78u;
    case 'U': return 0x3eu;
    case 'V': return 0x3eu;
    case 'W': return 0x2au;
    case 'Y': return 0x6eu;
    case 'Z': return 0x5bu;
    case '-': return 0x40u;
    case '_': return 0x08u;
    case ' ': return 0x00u;
    default:  return 0x00u;
    }
}

static void Board_Init(void)
{
    uint8_t i;

    g_sys_clock = SysCtlClockFreqSet((SYSCTL_XTAL_16MHZ |
                                      SYSCTL_OSC_INT |
                                      SYSCTL_USE_PLL |
                                      SYSCTL_CFG_VCO_480), 20000000u);

    S800_GPIO_Init();
    S800_I2C0_Init();
    S800_UART_Init();

    for (i = 0u; i < (uint8_t)KEY_COUNT; ++i) {
        g_key_debounce[i] = KEY_DEBOUNCE_TICKS;
    }

    SysTickPeriodSet(g_sys_clock / SYSTICK_FREQUENCY);
    IntPriorityGroupingSet(3);
    IntPrioritySet(FAULT_SYSTICK, 0x00);
    SysTickEnable();
    SysTickIntEnable();
    IntMasterEnable();
}

static uint32_t Board_Millis(void)
{
    return g_ms;
}

static void Board_DelayMs(uint32_t ms)
{
    uint32_t start = Board_Millis();

    while ((uint32_t)(Board_Millis() - start) < ms) {
    }
}

static void Board_Seg7Show(const char chars[SEG_DIGITS], uint8_t dp_mask)
{
    uint8_t i;

    for (i = 0u; i < SEG_DIGITS; ++i) {
        uint8_t value = char_to_seg(chars[i]);
        if ((dp_mask & (uint8_t)(1u << i)) != 0u) {
            value |= 0x80u;
        }
        g_seg_shadow[i] = value;
    }
}

static void Board_LedWrite(uint8_t value)
{
    (void)I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_OUTPUT, (uint8_t)(~value));
}

static uint16_t Board_KeyRawMask(void)
{
    uint8_t i2c_keys = (uint8_t)(~I2C0_ReadByte(TCA6424_I2CADDR,
                                                TCA6424_INPUT_PORT0));
    uint16_t mask = i2c_keys;

    if (GPIOPinRead(GPIO_PORTJ_BASE, GPIO_PIN_0) == 0u) {
        mask |= (uint16_t)(1u << KEY_USER1);
    }
    if (GPIOPinRead(GPIO_PORTJ_BASE, GPIO_PIN_1) == 0u) {
        mask |= (uint16_t)(1u << KEY_USER2);
    }

    return mask;
}

static bool Board_UartReadByte(uint8_t *byte)
{
    int32_t ch;

    if (!UARTCharsAvail(UART0_BASE)) {
        return false;
    }

    ch = UARTCharGetNonBlocking(UART0_BASE);
    if (ch < 0) {
        return false;
    }

    *byte = (uint8_t)ch;
    return true;
}

static void Board_UartWriteByte(uint8_t byte)
{
    UARTCharPut(UART0_BASE, byte);
}

static void Board_UartWriteString(const char *text)
{
    while (*text != '\0') {
        Board_UartWriteByte((uint8_t)*text++);
    }
}

static void Board_BuzzerWrite(bool on)
{
    GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_0, on ? GPIO_PIN_0 : 0u);
}

static void S800_GPIO_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOF)) {
    }

    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOJ);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOJ)) {
    }

    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPION);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPION)) {
    }

    GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_0);
    GPIOPinTypeGPIOOutput(GPIO_PORTN_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_0, 0);
    GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0 | GPIO_PIN_1, 0);

    GPIOPinTypeGPIOInput(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    GPIOPadConfigSet(GPIO_PORTJ_BASE, GPIO_PIN_0 | GPIO_PIN_1,
                     GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);
}

static void S800_I2C0_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_I2C0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);

    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOB)) {
    }

    GPIOPinConfigure(GPIO_PB2_I2C0SCL);
    GPIOPinConfigure(GPIO_PB3_I2C0SDA);
    GPIOPinTypeI2CSCL(GPIO_PORTB_BASE, GPIO_PIN_2);
    GPIOPinTypeI2C(GPIO_PORTB_BASE, GPIO_PIN_3);

    I2CMasterInitExpClk(I2C0_BASE, g_sys_clock, true);
    I2CMasterEnable(I2C0_BASE);

    (void)I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT0, 0xffu);
    (void)I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT1, 0x00u);
    (void)I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_CONFIG_PORT2, 0x00u);

    (void)I2C0_WriteByte(PCA9557_I2CADDR, PCA9557_CONFIG, 0x00u);
    Board_LedWrite(0x00u);
}

static void S800_UART_Init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA)) {
    }

    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    UARTConfigSetExpClk(UART0_BASE, g_sys_clock, 115200u,
                        (UART_CONFIG_WLEN_8 |
                         UART_CONFIG_STOP_ONE |
                         UART_CONFIG_PAR_NONE));
    UARTFIFOLevelSet(UART0_BASE, UART_FIFO_TX2_8, UART_FIFO_RX1_8);
    UARTEnable(UART0_BASE);
}

static uint8_t I2C0_WriteByte(uint8_t dev_addr, uint8_t reg_addr,
                              uint8_t write_data)
{
    uint8_t result;

    while (I2CMasterBusy(I2C0_BASE)) {
    }

    I2CMasterSlaveAddrSet(I2C0_BASE, dev_addr, false);
    I2CMasterDataPut(I2C0_BASE, reg_addr);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_START);

    while (I2CMasterBusy(I2C0_BASE)) {
    }

    result = (uint8_t)I2CMasterErr(I2C0_BASE);

    I2CMasterDataPut(I2C0_BASE, write_data);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_BURST_SEND_FINISH);

    while (I2CMasterBusy(I2C0_BASE)) {
    }

    result = (uint8_t)I2CMasterErr(I2C0_BASE);
    return result;
}

static uint8_t I2C0_ReadByte(uint8_t dev_addr, uint8_t reg_addr)
{
    uint8_t value;

    while (I2CMasterBusy(I2C0_BASE)) {
    }

    I2CMasterSlaveAddrSet(I2C0_BASE, dev_addr, false);
    I2CMasterDataPut(I2C0_BASE, reg_addr);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_SINGLE_SEND);

    while (I2CMasterBusy(I2C0_BASE)) {
    }

    I2CMasterSlaveAddrSet(I2C0_BASE, dev_addr, true);
    I2CMasterControl(I2C0_BASE, I2C_MASTER_CMD_SINGLE_RECEIVE);

    while (I2CMasterBusy(I2C0_BASE)) {
    }

    value = (uint8_t)I2CMasterDataGet(I2C0_BASE);
    return value;
}

void SysTick_Handler(void)
{
    uint8_t digit_select;

    g_ms++;

    digit_select = (uint8_t)(1u << g_seg_scan_index);
    (void)I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT1,
                         g_seg_shadow[g_seg_scan_index]);
    (void)I2C0_WriteByte(TCA6424_I2CADDR, TCA6424_OUTPUT_PORT2, digit_select);

    g_seg_scan_index++;
    if (g_seg_scan_index >= SEG_DIGITS) {
        g_seg_scan_index = 0u;
    }

    if (g_uart_activity_ms != 0u) {
        g_uart_activity_ms--;
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_1, GPIO_PIN_1);
    } else {
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_1, 0);
    }

    if (g_key_activity_ms != 0u) {
        g_key_activity_ms--;
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, GPIO_PIN_0);
    } else {
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0, 0);
    }
}

void UART0_Handler(void)
{
    uint32_t status = UARTIntStatus(UART0_BASE, true);
    UARTIntClear(UART0_BASE, status);
}
