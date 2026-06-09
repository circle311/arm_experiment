# 智能联网时钟系统 - 第一周基线

本仓库当前完成第一周目标所需的 MCU 业务代码骨架，核心代码位于：

- `mcu/src/main.c`

当前工作区没有课程提供的 S800/Keil 底层驱动工程，因此 `main.c` 中保留了一组弱定义硬件适配函数。将代码迁入真实 S800 工程时，只需要把这些函数替换为课程实验阶段已经验证过的驱动调用。

## 第一周已覆盖目标

- 上电开机画面：
  - 8 位数码管 + 8 位 LED 全亮/全灭。
  - 显示学号后 8 位。
  - 显示姓名拼音。
  - 显示版本号。
- 正常走时：
  - 基于 1 ms 系统时基累计秒。
  - 支持年月日进位。
  - 支持闰年。
- 日期/时间显示：
  - 默认显示 `HH.MM.SS`。
  - 串口输入 `DISP` 可在时间、短日期、长日期之间切换。
- UART 行接收：
  - 接收一行命令。
  - 回显输入内容。
  - 支持 `*PING`、`*GET:TIME`、`*GET:DATE`、`DISP`。

## 需要接入的底层函数

在真实 S800 工程中，请用驱动库实现或替换以下函数：

```c
void Board_Init(void);
uint32_t Board_Millis(void);
void Board_DelayMs(uint32_t ms);
void Board_Seg7Show(const char chars[8], uint8_t dp_mask);
void Board_LedWrite(uint8_t value);
bool Board_UartReadByte(uint8_t *byte);
void Board_UartWriteByte(uint8_t byte);
void Board_UartWriteString(const char *text);
```

建议映射方式：

| 适配函数 | 真实硬件含义 |
|---|---|
| `Board_Init` | 初始化系统时钟、SysTick、数码管、LED、UART |
| `Board_Millis` | 返回系统启动后的毫秒数 |
| `Board_DelayMs` | 毫秒延时 |
| `Board_Seg7Show` | 刷新 8 位数码管字符和小数点 |
| `Board_LedWrite` | 写 8 位 LED 状态 |
| `Board_UartReadByte` | 非阻塞读取 1 字节 UART 数据 |
| `Board_UartWriteByte` | UART 发送 1 字节 |
| `Board_UartWriteString` | UART 发送字符串 |

## 编译与烧写建议

1. 将 `mcu/src/main.c` 复制或加入课程 Keil 工程。
2. 保留课程给定的 `Inc/`、`Driverlib/` 和启动文件。
3. 在 `main.c` 中修改：

```c
#define STUDENT_ID_LAST8    "12345678"
#define STUDENT_NAME_PINYIN "ZHANGSAN"
```

4. 用真实驱动替换 `Board_*` 弱定义函数。
5. 编译生成 `obj/xxx.axf`。
6. 烧写后检查：
   - 上电动画完整。
   - 进入正常时间显示。
   - 串口助手输入 `*PING` 返回 `*PONG 0`。
   - 输入 `*GET:TIME` 返回当前时间。
   - 输入 `*GET:DATE` 返回当前日期。
   - 输入 `DISP` 可以切换显示模式。

## 下一阶段

第二周应继续完成：

- 完整按键扫描与消抖。
- 闹钟功能。
- 编辑状态机。
- 流水显示与 `FORMAT LEFT/RIGHT`。
- 完整 `*SET` / `*GET` 协议与容错规则。
- `*EVT:DISP` / `*EVT:LED` 每秒心跳上报。
