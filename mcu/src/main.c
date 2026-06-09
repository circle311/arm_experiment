/*
 * Smart Network Clock - week 1 MCU baseline
 *
 * This file keeps the week-1 business logic in main.c as required by the
 * assignment. Replace the weak Board_* functions at the bottom with calls to
 * the S800 course driver library when integrating into the real Keil project.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#if defined(__CC_ARM) || defined(__ARMCC_VERSION)
#define WEAK __weak
#else
#define WEAK __attribute__((weak))
#endif

#define SEG_DIGITS          8u
#define UART_LINE_MAX       64u
#define BOOT_STEP_MS        700u
#define VERSION_STEP_MS     1000u
#define HEARTBEAT_MS        1000u
#define UART_PROMPT         "> "

/* TODO: change these two constants before final submission. */
#define STUDENT_ID_LAST8    "12345678"
#define STUDENT_NAME_PINYIN "ZHANGSAN"
#define FIRMWARE_VERSION    "V1.2"

typedef enum {
    DISPLAY_TIME = 0,
    DISPLAY_DATE_SHORT,
    DISPLAY_DATE_LONG
} DisplayMode;

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint16_t millisecond;
    DisplayMode display_mode;
    uint32_t last_tick_ms;
    uint32_t last_heartbeat_ms;
    bool led_heartbeat;
} ClockState;

static ClockState g_clock = {
    2026u, 6u, 1u,
    12u, 0u, 0u,
    0u,
    DISPLAY_TIME,
    0u,
    0u,
    false
};

static char g_uart_line[UART_LINE_MAX + 1u];
static uint8_t g_uart_len = 0u;

void Board_Init(void);
uint32_t Board_Millis(void);
void Board_DelayMs(uint32_t ms);
void Board_Seg7Show(const char chars[SEG_DIGITS], uint8_t dp_mask);
void Board_LedWrite(uint8_t value);
bool Board_UartReadByte(uint8_t *byte);
void Board_UartWriteByte(uint8_t byte);
void Board_UartWriteString(const char *text);

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

    if (month == 2u && is_leap_year(year)) {
        return 29u;
    }
    if (month < 1u || month > 12u) {
        return 31u;
    }
    return days[month - 1u];
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

    /* Dots after HH and MM, matching HH.MM.SS on six useful digits. */
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

    /* YYYY.MMDD */
    *dp_mask = (uint8_t)(1u << 3u);
}

static void display_current_time_or_date(void)
{
    char chars[SEG_DIGITS];
    uint8_t dp_mask = 0u;

    fill_blank(chars);
    if (g_clock.display_mode == DISPLAY_DATE_SHORT) {
        make_short_date_display(chars, &dp_mask);
    } else if (g_clock.display_mode == DISPLAY_DATE_LONG) {
        make_long_date_display(chars, &dp_mask);
    } else {
        make_time_display(chars, &dp_mask);
    }

    Board_Seg7Show(chars, dp_mask);
}

static void display_text_8(const char *text, uint8_t dp_mask)
{
    char chars[SEG_DIGITS];
    uint8_t i;

    fill_blank(chars);
    for (i = 0u; i < SEG_DIGITS && text[i] != '\0'; ++i) {
        chars[i] = text[i];
    }
    Board_Seg7Show(chars, dp_mask);
}

static void boot_animation(void)
{
    display_text_8("88888888", 0xFFu);
    Board_LedWrite(0xFFu);
    Board_DelayMs(BOOT_STEP_MS);

    display_text_8("        ", 0x00u);
    Board_LedWrite(0x00u);
    Board_DelayMs(BOOT_STEP_MS);

    display_text_8(STUDENT_ID_LAST8, 0x00u);
    Board_LedWrite(0xFFu);
    Board_DelayMs(BOOT_STEP_MS);
    Board_LedWrite(0x00u);
    Board_DelayMs(200u);

    display_text_8(STUDENT_NAME_PINYIN, 0x00u);
    Board_LedWrite(0xFFu);
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

static bool str_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') {
            ca = (char)(ca - 'a' + 'A');
        }
        if (cb >= 'a' && cb <= 'z') {
            cb = (char)(cb - 'a' + 'A');
        }
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void uart_handle_line(char *line)
{
    Board_UartWriteString("RX: ");
    Board_UartWriteString(line);
    Board_UartWriteString("\r\n");

    if (str_equal(line, "*PING")) {
        Board_UartWriteString("*PONG ");
        Board_UartWriteString("0\r\n");
    } else if (str_equal(line, "*GET:TIME") || str_equal(line, "TIME")) {
        uart_print_time();
    } else if (str_equal(line, "*GET:DATE") || str_equal(line, "DATE")) {
        uart_print_date();
    } else if (str_equal(line, "DISP")) {
        g_clock.display_mode = (DisplayMode)(((uint8_t)g_clock.display_mode + 1u) % 3u);
        display_current_time_or_date();
        Board_UartWriteString("OK DISPLAY\r\n");
    } else {
        Board_UartWriteString("OK ECHO\r\n");
    }

    Board_UartWriteString(UART_PROMPT);
}

static void uart_poll(void)
{
    uint8_t byte;

    while (Board_UartReadByte(&byte)) {
        if (byte == '\r' || byte == '\n') {
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
            Board_UartWriteString("\r\nERROR LINE_TOO_LONG\r\n");
            Board_UartWriteString(UART_PROMPT);
        }
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
            display_current_time_or_date();
        }
    }

    if ((uint32_t)(now - g_clock.last_heartbeat_ms) >= HEARTBEAT_MS) {
        g_clock.last_heartbeat_ms = now;
        g_clock.led_heartbeat = !g_clock.led_heartbeat;
        Board_LedWrite(g_clock.led_heartbeat ? 0x01u : 0x00u);
    }
}

int main(void)
{
    Board_Init();
    boot_animation();

    g_clock.last_tick_ms = Board_Millis();
    g_clock.last_heartbeat_ms = g_clock.last_tick_ms;
    display_current_time_or_date();

    Board_UartWriteString("\r\nSmart Clock week-1 baseline ready\r\n");
    Board_UartWriteString("Commands: *PING, *GET:TIME, *GET:DATE, DISP\r\n");
    Board_UartWriteString(UART_PROMPT);

    while (1) {
        clock_poll();
        uart_poll();
    }
}

/*
 * Hardware adaptation layer.
 * Replace these weak functions with real S800 driver calls. Keeping them weak
 * lets this file compile in host-side syntax checks while staying portable.
 */

WEAK void Board_Init(void)
{
}

WEAK uint32_t Board_Millis(void)
{
    static uint32_t fake_ms = 0u;
    return fake_ms++;
}

WEAK void Board_DelayMs(uint32_t ms)
{
    uint32_t start = Board_Millis();
    while ((uint32_t)(Board_Millis() - start) < ms) {
    }
}

WEAK void Board_Seg7Show(const char chars[SEG_DIGITS], uint8_t dp_mask)
{
    (void)chars;
    (void)dp_mask;
}

WEAK void Board_LedWrite(uint8_t value)
{
    (void)value;
}

WEAK bool Board_UartReadByte(uint8_t *byte)
{
    (void)byte;
    return false;
}

WEAK void Board_UartWriteByte(uint8_t byte)
{
    (void)byte;
}

WEAK void Board_UartWriteString(const char *text)
{
    while (*text != '\0') {
        Board_UartWriteByte((uint8_t)*text++);
    }
}
