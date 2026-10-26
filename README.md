# dianji —— 我的 CAN 直流电机伺服节点（STM32F103C8T6）

> 我是张培正，机器人工程专业。这是我"CAN 总线双轴电机伺服"项目里的**单轴节点工程**：
> 一块 STM32F103C8T6 蓝丸板，驱动 TB6612 带直流电机，跑 CAN 总线和 PC 上位机对话。
> 建于 2026-10-17，当前 ✅ **v0.2 已编译 + 反汇编验证，CAN 链路真机跑通（1390 帧 0 错误）**。
> 芯片：STM32F103C8T6 ｜ 时钟：HSE 8MHz → 72MHz（APB1 36MHz / APB2 72MHz）｜ CMake + GCC
> 占用：FLASH 18968 / 65536 B（28.9%）｜ RAM 2184 / 20480 B

**这个工程里每一项能力我都用真机或反汇编验证过，不是"应该能行"：**

| 能力 | 我怎么验证的 |
|---|---|
| TIM1_CH1 / PA8 输出 20.000 kHz PWM | 反汇编确认 `Period=3599 / Prescaler=0`；万用表量 PA8 平均值 0→0.99→1.98→2.97V |
| PB0/PB1 方向切换 | 反汇编确认掩码 0x3，AIN 上电解 0 |
| PB5 = STBY 拉高 | 实测恒 3.3V |
| PC13 LED 心跳 | 每 1.2s 翻一次 |
| **PB8/PB9 软件 I2C + SSD1306 实时显示** | 反汇编确认 `Pin=0x300 / Mode=0x11(开漏)`；**屏幕 4 行显示正常** |
| **USART1 115200 8N1（v0.2）** | 反汇编确认 `__io_putchar` 为强符号 `T`、`_write` 为 weak `W`；PA9 `Pin=0x200 / Mode=2(AF_PP)`；115200 立即数已入二进制 |
| **CAN 500kbps + PC 上位机（can v0.2）** | 真机收发 1390 帧，crc=0 gap=0 fail=0，遥测曲线见第九节 |

---

## 一、我为什么做这个工程

我的主线项目是 **CAN 总线双轴电机伺服**：PC 通过 CAN 总线给两个电机节点发速度指令，节点闭环执行。这个 dianji 工程就是其中一个节点——把"PWM 驱动电机 → 串口调试 → CAN 通信"一步一步做扎实。

我的做法是**每一步都用证据说话**：代码写了不算数，编译过了不算数，我会用 `arm-none-eabi-objdump` 反汇编，确认寄存器配置真的进了二进制；电机没到货时，先用万用表把 PWM 平均电压、方向脚、STBY 全部量一遍。这样电机到货那天，我只需要插线，而不是从头 debug。

## 二、现在就能烧（构建 / 烧录命令）

```bash
export PATH="/c/Users/19780/.local/stm32cube/bin:/c/Users/19780/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin:$PATH"
cd /e/Demo/dianji
cube cmake --build build/Debug
```

产物：`build/Debug/dianji.elf`

烧录（ST-Link 只接 SWDIO / SWCLK / GND 三根）：

```bash
export PATH="/c/Users/19780/.local/stm32cube/bin:$PATH"
cube programmer -c port=SWD --download build/Debug/dianji.elf --reset
```

> ⚠️ **绝对不要接 ST-Link 的 3.3V 脚**——它会和板上的 AMS1117 打架。

## 三、我固化下来的 6 项配置

| 项 | 值 | 二进制里已验证 |
|---|---|---|
| HSE | 8MHz 晶振 | — |
| SYSCLK | 72 MHz（HSE ×9 / PLL） | `PLL.PLLMUL = RCC_PLL_MUL9` |
| APB1 | 36 MHz（÷2） | — |
| APB2 | 72 MHz（÷1） | — |
| **TIM1_CH1 / PA8** | Prescaler=**0**, Period=**3599** → **20.000 kHz** | ✅ `movw r2, #3599` + `[r3,#4]=0` |
| GPIO | PB0/PB1=AIN1/AIN2，PB5=STBY，PC13=LED | ✅ 掩码 `#3`(PB0\|PB1)、`#32`(PB5)、AIN 置 0、STBY 置 1 |

我用的反汇编验证命令：

```bash
arm-none-eabi-objdump -d build/Debug/dianji.elf | awk '/<MX_TIM1_Init>:/,/^$/' | grep movw
```

## 四、电机没到时我做的三项静态验证

当时电机还没到货，我先用万用表把三件事全部确认：

| # | 测什么 | 怎么做 | 预期 |
|---|---|---|---|
| 1 | **PWM 波形真出来了** | 万用表 DC 档量 **PA8** 对 GND 平均电压 | +90% 时 ≈ **2.97V**；0% 时 **0.00V** |
| 2 | **AIN 极性对** | 按节奏量 PB0 / PB1 | `+` 档 `PB0=3.3V, PB1=0V`；`-` 档反过来 |
| 3 | **STBY 真被拉高** | 量 **PB5** | 恒 **3.3V** |

> 第 3 项是我最省时间的一刀：**如果当时不查，电机到货那天我会以为是 TB6612 坏了、电机坏了或线断了。**

上电 1.2 秒后电机会走第一档（+30%），LED 每档翻一次，看 LED 节奏就知道在第几档。

## 五、文件结构 —— 我的分层设计

```
E:\Demo\dianji\
├── Core/
│   ├── Inc\
│   │   ├── config.h  ← ⭐ 参数总闸（改 PWM 频率/限幅/引脚全在这里）
│   │   ├── motor.h   ← 电机 API（往外只露"千分比速度"）
│   │   ├── usart.h   ← 串口 API（v0.2）
│   │   ├── can_proto.h / can_bsp.h  ← CAN 协议层 / 硬件层（can v0.2）
│   │   ├── main.h / gpio.h / tim.h / stm32f1xx_it.h / stm32f1xx_hal_conf.h
│   └── Src\
│       ├── motor.c     ← 驱动实现，含 4 个坑的对策
│       ├── usart.c     ← USART1 驱动 + __io_putchar（v0.2）
│       ├── can_proto.c ← 应用层协议：组包·解包·序号丢帧统计
│       ├── can_bsp.c   ← bxCAN：位时序、滤波器、FIFO0 接收中断
│       ├── main.c      ← 业务代码全在 USER CODE 区
│       ├── tim.c / gpio.c / stm32f1xx_hal_msp.c / stm32f1xx_it.c
│       └── syscalls.c / sysmem.c / system_stm32f1xx.c
├── tools/can_host.py ← PC 上位机（python-can，约 780 行）
├── Drivers/          ← STM32Cube_FW_F1 V1.8.7 的 HAL + CMSIS（含手动补回的 hal_uart.c/.h）
├── cmake/            ← STM32CubeMX 的 CMake 支持
├── CMakeLists.txt    ← ⭐ 根 CMakeLists（CubeMX 不会覆盖）
├── dianji.ioc        ← 工程配置
└── STM32F103xx_FLASH.ld / startup_stm32f103xb.s
```

**我为什么这样分层**：调参数只改 `config.h`；电机驱动收敛成"千分比速度"这一个概念（1000=100% 占空比）；加 CAN、编码器、PID 都在应用层加文件，**永远不碰 `motor.c`**。

## 六、关于 dianji.ioc —— 我踩过的 CubeMX 坑

这个 `.ioc` 是我**照着已经编译通过的源码反写**的，里面每一项都对应这份代码的实际行为。

**`.ioc` 里没勾的 IP，CubeMX 生成时会连带裁掉驱动**：因为 `.ioc` 里没勾 USART1，直接 Generate Code 会把 `stm32f1xx_hal_uart.c` 从 `STM32_Drivers_Src` 删掉、把 `hal_conf.h` 的 `HAL_UART_MODULE_ENABLED` 注释回去——业务代码不丢，但**串口会静默失效**。

我的防法：宏和 `hal_uart.c` 都写在**根** `CMakeLists.txt`（CubeMX 覆盖不到），所以 USART1 照样能编进去。反过来，如果哪天在 CubeMX 里勾了 USART1 再生成，两边都会列 `hal_uart.c` ⇒ 链接报 `multiple definition of HAL_UART_Init`，把根 CMakeLists 里手动那行删掉即可。

**在 CubeMX 里点 Generate Code 之前我会先做两件事**：
1. 看芯片图上 PA8 / PB0 / PB1 / PB5 / PC13 / PB8 / PB9 / PA9 / PA10 / PA13 / PA14 是否都在
2. 确认配置表与 USART 数值一致

## 七、我在 motor.c 里踩过的 4 个坑

| # | 坑 | 当时的现场表现 |
|---|---|---|
| ① | CCR 是 uint32_t，负值直写溢出 | 电机全速、正转、完全不受控——我把 -300 写成了 42 亿 |
| ② | TIM1 是高级定时器，`HAL_TIM_PWM_Start` 不置 BDTR.MOE | 代码通了、寄存器对了、PA8 一片寂静 |
| ③ | TB6612 STBY 低电平休眠 | 电机不转、VM 有 12V、AO1/AO2 无输出 |
| ④ | Init 顺序：方向脚 → STBY → PWM | 上电瞬间电机"咯噔"抖一下 |

对策全部写进了 `motor.c` 的注释里。

## 八、USART1 串口（v0.2）—— 我为此设计的东西

### 1. 接线（3 根，只接 3 根）

| STM32F103 | 接 | 说明 |
|---|---|---|
| **PA9**（TX） | USB-TTL 的 **RX** | 只看输出就接这根 + GND |
| **PA10**（RX） | USB-TTL 的 **TX** | 要发命令回显才接 |
| **GND** | USB-TTL 的 **GND** | ⭐ **必须共地**，不共地一个字节都收不到 |

> ⚠️ PA9/PA10 不是 5V 容忍脚。USB-TTL 模块我统一跳到 **3.3V 供电**（CH340 模块上有 3V3/5V 跳线），避免 TX 输出 5V 时在 PA10 上倒灌。

### 2. 串口助手设置

**波特率 115200 ｜ 数据位 8 ｜ 停止位 1 ｜ 校验 None ｜ 流控 None**

### 3. 输出长什么样

```
# dianji v0.2 | NODE 1 | SYSCLK 72M APB2 72M | PWM 20kHz ARR 3599
# USART1 PA9(TX) 115200 8N1  RX=PA10  接线: STM32 PA9 -> USB-TTL 的 RX
# duty_permille: 1000 = 100%占空比;  dir: P=正转 R=反转 S=停止
t_ms,duty_permille,dir
0,0,S
1200,300,P
2400,600,P
3600,900,P
4800,-300,R
...
10004,0,S        ← 1 秒心跳（duty 没变也打，证明设备还活着）
```

前 3 行 `#` 是给人看的，**第 4 行开始才是给 Excel 的数据**：串口助手另存 `.txt` → Excel「数据 → 自文本」→ 逗号分隔 → 从 `t_ms` 那行导入。

### 4. 打印节奏是我设计过的

| 触发 | 频率 | 为什么 |
|---|---|---|
| duty 变化 | 每 1.2s（= `MOTOR_DEMO_STEP_MS`） | Excel 里能看到干净的阶跃点 |
| 1 秒心跳 | 每 1000ms | 万一主循环跑飞，时间戳停了我立刻能看出来 |

一行约 12 字节 ≈ **1.0ms @115200**，占主循环比例可忽略，所以我**没有**用定时器中断发串口——那会挤掉将来的 PWM 控制周期。

### 5. 这里也有 3 个静默失败点（都是我预研过并规避的）

| # | 坑 | 现场表现 |
|---|---|---|
| ① | **没清 ORE** | TX 正常，RX 第一次溢出后永不再置位，串口"假死"，看起来像线断了 |
| ② | **`__io_putchar` 写成 weak** | `syscalls.c` 里的 weak 版赢过你的实现，printf 全丢，编译链接都不报错 |
| ③ | **没设 `setvbuf(stdout, NULL, _IONBF, 0)`** | newlib 行缓冲把 `'\r'` 之后的半行憋住，"隔一会儿才冒出一行" |

### 6. `strlen` 的 `#include <string.h>`

`usart.c` 里用了 `strlen`，必须有 `#include <string.h>`。缺了只给 warning，链接报 undefined——而在 `--specs=nano.specs` 下符号被改名成 `_strlen`，报错信息很难认。

## 九、CAN 总线 + PC 上位机（can v0.2，真机已通 ✅）

这是这个工程的核心目标，已经跑通了。

### 1. 固件侧：我把协议拆成两层

| 文件 | 职责 |
|---|---|
| `Core/Inc/can_proto.h` + `Core/Src/can_proto.c` | 应用层协议：8 字节帧（0xA0\|L 帧头兼长度 / seq / cmd / 小端载荷 / CRC-8/SMBUS），组包·解包·序号丢帧统计 |
| `Core/Inc/can_bsp.h` + `Core/Src/can_bsp.c` | bxCAN 硬件层：500 kbps 位时序（PCLK1 36MHz：BRP=6 / BS1=8TQ / BS2=3TQ，采样点 75%）、ID 滤波器、FIFO0 接收中断 |

真机链路统计（串口周期打印）：`tx=1390 rx=1390 ok=1390 crc=0 gap=0 fail=0` —— **收发零错误**。

### 2. PC 上位机：tools/can_host.py（约 780 行）

只依赖 python-can，我自己写的，包含：

- 协议常量与 `can_proto.h` **逐项对齐**（两边一起改，注释里写死了这条纪律）
- 命令：PING / SET_SPEED（千分比）/ STOP / GET_STAT；fire-and-forget 命令配合 GET_STAT 回读确认
- `selftest` 子命令：**没有硬件也能在 python-can 虚拟总线上完成全流程自测**
- 遥测数据落盘 CSV，画响应曲线

![遥测曲线](docs/p1_curve.png)

*140 秒真机遥测：换向/启停状态变化已标注；同期链路统计 1390 帧 0 错误*

### 3. 协议速览

```
字节   0        1     2      3..6            7
      0xA0|L   seq   cmd    payload(小端)    CRC-8/SMBUS(poly 0x07)
CAN ID：0x100+node = 命令帧（PC→板）；0x200+node = 应答帧（板→PC）
```

我把长度塞进帧头字节（`[0]=0xA0|len`，合法值仅 0xA0-0xA4），因为 4 字节头 + 1 字节 CRC 只剩 3 字节载荷空间，独立长度字段会挤掉最后一个载荷字节——这个 bug 是我的回归测试（44 条断言的 `test_can_proto.py`）抓出来的，不是看出来的。

## 十、我的路线图（按依赖排序）

| 阶段 | 加什么 | 依赖 | 状态 |
|---|---|---|---|
| **✅ v0.1** | TIM1 PWM + 电机方向 + STBY + LED 心跳 | 无 | **已完成，真机跑通** |
| **✅ v0.2** | USART1 + printf | 一块 C8T6 + USB-TTL | **已完成，反汇编验证 + 真机确认** |
| **✅ can v0.2** | CAN 500kbps + PC 上位机 | TJA1050 + USB-CAN | **已完成，1390 帧 0 错误** |
| **v0.3** | TIM3 编码器 4 倍频测速 | 电机到货 | 进行中 |
| **v0.4** | PID 速度环（10ms） | v0.2 + v0.3 | 排队 |
| **v0.5** | CANopen 风格对象字典 / 双轴协同 | v0.4 | 规划 |

之后接 FreeRTOS（任务化控制环）和 FOC（换 G431/F405，HRTIM + 内置运放）。

---

*配套上位机仓库：[CAN-Python](https://github.com/yidayy/CAN-Python) ｜ 协议回归测试：44 条断言全通过*
