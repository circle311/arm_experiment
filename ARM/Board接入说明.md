# S800 Keil 工程接入说明

## 当前结论

不要重新创建空 Keil 工程。直接打开本目录下的 `exp3.uvprojx`。

原因是这份工程已经配好：

- 芯片型号：`TM4C1294NCPDT`
- 编译宏：`PART_TM4C1294NCPDT`
- 头文件路径：`.\inc;.\driverlib`
- 启动文件：`RTE\Device\TM4C1294NCPDT\startup_TM4C129.s`
- 系统文件：`RTE\Device\TM4C1294NCPDT\system_TM4C129.c`
- 库文件：`driverlib\rvmdk\driverlib.lib`
- 用户源文件：`exp3-1.c`

我已经把第一周通用 `Board_*` 版本接入到 `exp3-1.c`，Keil 里不需要再额外添加 `mcu/src/main.c`，否则会出现两个 `main()`。

## Board_* 接入关系

`Board_Init()`

- 配置系统时钟为 20 MHz。
- 调用 `S800_GPIO_Init()` 初始化板载 GPIO、USER1/USER2、PN/PF 指示灯。
- 调用 `S800_I2C0_Init()` 初始化 I2C0、TCA6424、PCA9557。
- 调用 `S800_UART_Init()` 初始化 UART0，参数为 `115200 8N1`。
- 开启 1 ms SysTick。

`Board_Millis()`

- 返回 `SysTick_Handler()` 里累加的 `g_ms`。

`Board_DelayMs(ms)`

- 基于 `Board_Millis()` 忙等延时。
- 开机动画期间 SysTick 仍会扫描数码管。

`Board_Seg7Show(chars, dp_mask)`

- 把 8 个 ASCII 字符转换为七段码，写入 `g_seg_shadow[8]`。
- 小数点由 `dp_mask` 控制：第 0 位对应第 1 个数码管，第 7 位对应第 8 个数码管。
- 真正刷新在 `SysTick_Handler()` 中完成。

`Board_LedWrite(value)`

- 写 `PCA9557_OUTPUT`。
- S800 LED 为低有效，因此实际写入 `~value`。

`Board_UartReadByte(byte)`

- 轮询 `UART0_BASE`，使用 `UARTCharsAvail()` 和 `UARTCharGetNonBlocking()`。

`Board_UartWriteByte(byte)` / `Board_UartWriteString(text)`

- 使用 `UARTCharPut()` 阻塞发送到 UART0。

## Keil 中怎么替换

如果你现在打开的是这份 `ARM\exp3.uvprojx`：

1. 不要添加 `mcu/src/main.c`。
2. 保留工程里现有的 `exp3-1.c`。
3. 直接 Build。
4. 如果 Keil 提示源文件路径失效，确认 Target 中的用户源文件仍指向 `.\exp3-1.c`。

如果你在另一个 Keil 工程里做：

1. 删除或排除原来的主程序文件，避免两个 `main()`。
2. 把本目录的 `exp3-1.c` 加入 Source Group。
3. 确认 Include Path 包含 `.\inc;.\driverlib`。
4. 确认 C/C++ Define 包含 `PART_TM4C1294NCPDT`。
5. 确认工程链接了 `driverlib\rvmdk\driverlib.lib`。

## SSCOM 验证

串口参数：

```text
115200
8 data bits
1 stop bit
no parity
no flow control
```

建议先勾选“发送新行”或手动在命令后发送回车。

依次发送：

```text
*PING
*GET TIME
*GET DATE
*SET:TIME=19:56:00
*SET:DATE=2026-06-09
*SET ALARM=20:00:00
*SET ALARM=ON
*SET MSG=HELLO S800 CLOCK
*SET DISP=MSG
*SET FORMAT=RIGHT
*SET SPEED=FAST
DISP
AT+CLASS
AT+STUDENTCODE
```

期望现象：

- 上电后数码管和 LED 有开机动画。
- 动画后数码管显示 `HH.MM.SS`。
- `DISP` 每发送一次，在时间、短日期、长日期之间切换。
- SSCOM 收到 `Smart Clock week-1 S800 port ready`。
- `*PING` 返回 `*PONG 0`。
- `*GET:TIME` 返回当前板端时间。
- `*GET:DATE` 返回当前板端日期。
- `*SET:TIME=19:56:00` 返回 `OK TIME`，并把板端时间设置为 19:56:00。
- `*SET:DATE=2026-06-09` 返回 `OK DATE`，并把板端日期设置为 2026-06-09。
- `*SET ALARM=20:00:00` 返回 `OK ALARM`，并自动启用闹钟。
- `*SET ALARM=ON/OFF` 启用或关闭闹钟。
- `*SET MSG=...` 进入消息流水显示。
- `*SET FORMAT=LEFT/RIGHT` 切换正常/反向显示。
- `*SET SPEED=SLOW/FAST` 切换流水速度。

简写也可用：

```text
TIME=19:56:00
DATE=2026-06-09
MSG=HELLO
```

如果返回 `ERR TIME FORMAT` 或 `ERR DATE FORMAT`，通常是格式不对或日期非法，例如 `2026-02-30`。

第二周版本会每秒主动输出：

```text
*EVT:DISP ...
*EVT:LED 0x..
```

按键按下/松开会输出：

```text
*EVT:KEY FUNC DOWN
*EVT:KEY FUNC UP
```

当前按键映射：

| 键位 | 功能 |
|---|---|
| K0 | FUNC |
| K1 | SHIFT |
| K2 | ADD |
| K3 | SAVE |
| K4 | DISP |
| K5 | SPEED |
| K6 | FORMAT |
| K7 | EXT |
| USER1 | 对时请求事件 |
| USER2 | 天气占位消息 |
 

## 需要你确认/修改

提交前请在 `exp3-1.c` 顶部替换真实个人信息：

```c
#define CLASS_NUMBER           "2405"
#define STUDENT_CODE           "00000430"
#define STUDENT_NAME_PINYIN    "ZHANGSAN"
```

其中 `STUDENT_CODE` 目前沿用了你原例程里的 `0430` 并补成 8 位占位，`STUDENT_NAME_PINYIN` 仍是占位。
