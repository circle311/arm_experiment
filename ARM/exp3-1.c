/*
 * Smart Network Clock - S800/TM4C1294 week-1 port
 *
 * Keil project entry file.  This file keeps the assignment business logic and
 * the S800 hardware adaptation in one source file, because the course project
 * usually expects user code to be concentrated in main.c / exp3-1.c.
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

#define SYSTICK_FREQUENCY      1000u
#define SEG_DIGITS             8u
#define UART_LINE_MAX          64u
#define BOOT_STEP_MS           700u
#define VERSION_STEP_MS        1000u
#define HEARTBEAT_MS           1000u
#define UART_PROMPT            "> "

/* TODO: replace with your real information before final submission. */
#define CLASS_NUMBER           "2405"
#define STUDENT_CODE           "00000430"
#define STUDENT_NAME_PINYIN    "ZHANGSAN"
#define FIRMWARE_VERSION       "V1.2"

/* I2C GPIO chip addresses and register definitions used by the S800 board. */
#define TCA6424_I2CADDR        0x22u
#define PCA9557_I2CADDR        0x18u

#define PCA9557_OUTPUT         0x01u
#define PCA9557_CONFIG         0x03u

#define TCA6424_CONFIG_PORT0   0x0cu
#define TCA6424_CONFIG_PORT1   0x0du
#define TCA6424_CONFIG_PORT2   0x0eu
#define TCA6424_OUTPUT_PORT1   0x05u
#define TCA6424_OUTPUT_PORT2   0x06u

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

static void Board_Init(void);
static uint32_t Board_Millis(void);
static void Board_DelayMs(uint32_t ms);
static void Board_Seg7Show(const char chars[SEG_DIGITS], uint8_t dp_mask);
static void Board_LedWrite(uint8_t value);
static bool Board_UartReadByte(uint8_t *byte);
static void Board_UartWriteByte(uint8_t byte);
static void Board_UartWriteString(const char *text);

static void S800_GPIO_Init(void);
static void S800_I2C0_Init(void);
static void S800_UART_Init(void);
static uint8_t I2C0_WriteByte(uint8_t dev_addr, uint8_t reg_addr,
                              uint8_t write_data);

void SysTick_Handler(void);
void UART0_Handler(void);

static volatile uint32_t g_ms;
static volatile uint8_t g_seg_shadow[SEG_DIGITS];
static volatile uint8_t g_seg_scan_index;
static volatile uint8_t g_rx_led_ms;

static uint32_t g_sys_clock;

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
static uint8_t g_uart_len;

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

static void uart_handle_line(char *line)
{
    Board_UartWriteString("RX: ");
    Board_UartWriteString(line);
    Board_UartWriteString("\r\n");

    if (str_equal_ignore_case(line, "*PING")) {
        Board_UartWriteString("*PONG 0\r\n");
    } else if (str_equal_ignore_case(line, "*GET:TIME") ||
               str_equal_ignore_case(line, "TIME")) {
        uart_print_time();
    } else if (str_equal_ignore_case(line, "*GET:DATE") ||
               str_equal_ignore_case(line, "DATE")) {
        uart_print_date();
    } else if (str_equal_ignore_case(line, "DISP")) {
        g_clock.display_mode =
            (DisplayMode)(((uint8_t)g_clock.display_mode + 1u) % 3u);
        display_current_time_or_date();
        Board_UartWriteString("OK DISPLAY\r\n");
    } else if (str_equal_ignore_case(line, "AT+CLASS")) {
        Board_UartWriteString("CLASS");
        Board_UartWriteString(CLASS_NUMBER);
        Board_UartWriteString("\r\n");
    } else if (str_equal_ignore_case(line, "AT+STUDENTCODE")) {
        Board_UartWriteString("CODE");
        Board_UartWriteString(STUDENT_CODE);
        Board_UartWriteString("\r\n");
    } else {
        Board_UartWriteString("OK ECHO\r\n");
    }

    Board_UartWriteString(UART_PROMPT);
}

static void uart_poll(void)
{
    uint8_t byte;

    while (Board_UartReadByte(&byte)) {
        g_rx_led_ms = 100u;
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

    Board_UartWriteString("\r\nSmart Clock week-1 S800 port ready\r\n");
    Board_UartWriteString("Commands: *PING, *GET:TIME, *GET:DATE, DISP\r\n");
    Board_UartWriteString(UART_PROMPT);

    while (1) {
        clock_poll();
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
    case 'N': return 0x54u;
    case 'O': return 0x3fu;
    case 'P': return 0x73u;
    case 'R': return 0x50u;
    case 'S': return 0x6du;
    case 'T': return 0x78u;
    case 'U': return 0x3eu;
    case 'V': return 0x3eu;
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
    g_sys_clock = SysCtlClockFreqSet((SYSCTL_XTAL_16MHZ |
                                      SYSCTL_OSC_INT |
                                      SYSCTL_USE_PLL |
                                      SYSCTL_CFG_VCO_480), 20000000u);

    S800_GPIO_Init();
    S800_I2C0_Init();
    S800_UART_Init();

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

    if (g_rx_led_ms != 0u) {
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_1, GPIO_PIN_1);
        g_rx_led_ms--;
    } else {
        GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_1, 0);
    }

    if (GPIOPinRead(GPIO_PORTJ_BASE, GPIO_PIN_0) == 0) {
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
