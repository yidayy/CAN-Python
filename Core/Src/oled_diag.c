/**
  ******************************************************************************
  * @file    oled_diag.c
  * @brief   OLED 硬件自检（连续运行，结果存 RAM 供 SWD 直读）
  ******************************************************************************
  *
  * 为什么需要这个文件
  * ------------------
  * Core/Src/OLED.c 的 OLED_I2C_SendByte() 只发 9 个时钟、**从不读第 9 个时钟的
  * 应答位**（该文件第 61 行原话："额外的一个时钟，不处理应答信号"）。
  *
  * 后果：屏幕没插、杜邦线断芯、SCL/SDA 接反、模块根本没供电 —— 程序一律
  * 认为"写入成功"。主循环照转、LED 照闪、串口照刷 CSV，屏全黑，一个错都不报。
  * 这就是典型的**静默失败**，也是这块屏反复查不出原因的根源。
  *
  * 四项判据
  * --------
  * [0] 对照组：看 PB0/PB5 的 IDR 与 ODR 是否一致。
  *     这两个脚是推挽输出，IDR 必须等于 ODR。若不等，说明"读引脚电平"
  *     这条路径本身有问题，后面所有结论作废，免得把方向带偏。
  *
  * [1] 内部下拉后读电平（核心判据）
  *     PB8/PB9 内部下拉约 40k，把线往 0V 拽；
  *     OLED 板载上拉（典型 4.7k）把线往 3.3V 拽，而它的电源来自 OLED 的 VCC 脚。
  *     读到 1 ⇒ VCC 通 + 这根信号线通；读到 0 ⇒ 线上没有上拉源。
  *
  * [2] 内部上拉后读电平
  *     区分"线悬空"和"线被钳在低电平"：
  *     悬空的线能被内部上拉拉到 1；被拽住的拉不起来。
  *
  * [3] I2C 应答扫描，两轮
  *     ① 快速（220kHz）：只有 [1] 通过时才做，否则 SDA 恒低，
  *        每个地址都会被误判成 ACK（曾扫出"112 个地址全部应答"这种鬼结果）。
  *     ② 慢速（~10kHz）+ 开 MCU 内部上拉：扫 0x3C / 0x3D。
  *        ⭐ 这一轮是关键判别：
  *           应答了 ⇒ 屏是**有电**的、走线通的，只是它的板载上拉没起作用
  *                    ⇒ 把 gpio.c 的 GPIO_NOPULL 改成 GPIO_PULLUP 就能用
  *           不应答 ⇒ 屏真的没通电（或线断）
  *
  * 结果存进 RAM 的 g_oled_diag，不接串口也能用 ST-Link 读：
  *     cube programmer -c port=SWD -r <g_oled_diag 地址> 16 dump.bin
  ******************************************************************************
  */

#include "main.h"
#include "oled_diag.h"
#include <stdio.h>

/* ---- 引脚定义必须和 Core/Src/OLED.c 第 6-7 行、Core/Src/gpio.c 第 98 行一致 ---- */
#define DIAG_PORT   GPIOB
#define SCL_PIN     GPIO_PIN_8
#define SDA_PIN     GPIO_PIN_9

/* ⭐ 必须和 Core/Src/gpio.c 里 PB8/PB9 的 Pull 配置保持一致。
 *    自检跑完要还原成业务配置，还原错了等于每秒把上拉关一次，屏又会闪断。 */
#define OLED_BUS_PULL   GPIO_PULLUP

volatile OledDiagResult_t g_oled_diag = { 0 };

/* 位延时。72MHz 下粗约：40 次 ≈ 3us（~180kHz），900 次 ≈ 60us（~10kHz）
 * 另有一档 mode=2 完全不延时，用来复现 OLED.c 的原始时序（它一个延时都没加）。 */
static void delay_fast(void)
{
    for (volatile uint32_t i = 0U; i < 40U; i++) { __NOP(); }
}

static void delay_slow(void)
{
    for (volatile uint32_t i = 0U; i < 900U; i++) { __NOP(); }
}

static void delay_by_mode(uint8_t mode)
{
    if (mode == 1U)      { delay_slow(); }
    else if (mode == 0U) { delay_fast(); }
    else                 { /* mode 2：零延时，就是 OLED.c 的时序 */ }
}

static void scl_write(uint8_t level)
{
    HAL_GPIO_WritePin(DIAG_PORT, SCL_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void sda_write(uint8_t level)
{
    HAL_GPIO_WritePin(DIAG_PORT, SDA_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* 开漏输出模式下照样能读 IDR，读到的就是线上真实电平 */
static uint8_t scl_read(void) { return (HAL_GPIO_ReadPin(DIAG_PORT, SCL_PIN) == GPIO_PIN_SET) ? 1U : 0U; }
static uint8_t sda_read(void) { return (HAL_GPIO_ReadPin(DIAG_PORT, SDA_PIN) == GPIO_PIN_SET) ? 1U : 0U; }

static void pins_mode(uint32_t mode, uint32_t pull)
{
    GPIO_InitTypeDef g;
    g.Pin   = SCL_PIN | SDA_PIN;
    g.Mode  = mode;
    g.Pull  = pull;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DIAG_PORT, &g);
}

/* 还原成 gpio.c 里配置的开漏输出（OLED.c 依赖这个状态）。
 * pull 参数决定要不要挂 MCU 内部上拉当"救兵"。 */
static void pins_restore_od(uint32_t pull)
{
    scl_write(1);
    sda_write(1);
    pins_mode(GPIO_MODE_OUTPUT_OD, pull);
    scl_write(1);
    sda_write(1);
}

/* ---------------------------------------------------------------------------
 * [0] 对照组：推挽输出脚的 IDR 必须等于 ODR
 * ------------------------------------------------------------------------- */
static uint8_t test_control(uint8_t *b0_ok, uint8_t *b5_ok)
{
    uint8_t i;

    *b0_ok = 1U;
    *b5_ok = 1U;

    for (i = 0U; i < 3U; i++) {
        uint32_t idr = DIAG_PORT->IDR;
        uint32_t odr = DIAG_PORT->ODR;

        if (((idr ^ odr) & GPIO_PIN_0) != 0U) { *b0_ok = 0U; }
        if (((idr ^ odr) & GPIO_PIN_5) != 0U) { *b5_ok = 0U; }
        delay_fast();
    }
    return ((*b0_ok == 1U) && (*b5_ok == 1U)) ? 1U : 0U;
}

/* ---------------------------------------------------------------------------
 * [1] 内部下拉 + [2] 内部上拉 时的线上电平
 * ------------------------------------------------------------------------- */
static void test_line_levels(uint8_t *pd_scl, uint8_t *pd_sda,
                             uint8_t *pu_scl, uint8_t *pu_sda)
{
    pins_mode(GPIO_MODE_INPUT, GPIO_PULLDOWN);
    HAL_Delay(2);
    *pd_scl = scl_read();
    *pd_sda = sda_read();

    pins_mode(GPIO_MODE_INPUT, GPIO_PULLUP);
    HAL_Delay(2);
    *pu_scl = scl_read();
    *pu_sda = sda_read();
}

/* ---------------------------------------------------------------------------
 * [3] 互短：SCL 用开漏拉低，看 SDA 是否被一起拽低（安全，只灌电流）
 * ------------------------------------------------------------------------- */
static uint8_t test_short(void)
{
    uint8_t shorted;

    pins_restore_od(OLED_BUS_PULL);
    scl_write(1); sda_write(1); delay_fast();
    scl_write(0);
    delay_fast(); delay_fast();
    shorted = (sda_read() == 0U) ? 1U : 0U;
    scl_write(1); delay_fast();

    return shorted;
}

/* ---------------------------------------------------------------------------
 * [4] I2C 应答扫描
 * ------------------------------------------------------------------------- */

/* 发一个字节，返回 1 = 收到 ACK（从机把 SDA 拉低） */
static uint8_t i2c_send_byte_ack(uint8_t byte, uint8_t mode)
{
    uint8_t i;
    uint8_t ack;

    for (i = 0U; i < 8U; i++) {
        sda_write((byte & 0x80U) ? 1U : 0U);
        delay_by_mode(mode);
        scl_write(1);
        delay_by_mode(mode);
        scl_write(0);
        delay_by_mode(mode);
        byte = (uint8_t)(byte << 1);
    }

    /* 第 9 个时钟：主机释放 SDA，从机拉低表示应答 */
    sda_write(1);
    delay_by_mode(mode);
    scl_write(1);
    delay_by_mode(mode);
    ack = (sda_read() == 0U) ? 1U : 0U;
    scl_write(0);
    delay_by_mode(mode);

    return ack;
}

/* 探测一个 7 位地址。
 * ⭐ rd=1 用"读地址"（最低位 1）去探，这是唯一可靠的判据：
 *    地址字节最后一位是 1，发送时 SDA 被释放为高；此后主机再释放 SDA 等应答，
 *    若读到 0 就只能是屏把它拉低了。用写地址（最低位 0）探会有假阳性 ——
 *    上拉太弱、线来不及回高，也会读成 0，长得跟 ACK 一模一样。 */
static uint8_t i2c_probe(uint8_t addr7, uint8_t mode, uint8_t rd)
{
    uint8_t ack;

    /* START：SCL 高电平期间 SDA 由高变低 */
    sda_write(1); scl_write(1); delay_by_mode(mode);
    sda_write(0); delay_by_mode(mode);
    scl_write(0); delay_by_mode(mode);

    ack = i2c_send_byte_ack((uint8_t)((addr7 << 1) | (rd ? 1U : 0U)), mode);

    /* STOP：SCL 高电平期间 SDA 由低变高 */
    sda_write(0);
    delay_by_mode(mode);
    scl_write(1);
    delay_by_mode(mode);
    sda_write(1);
    delay_by_mode(mode);

    return ack;
}

/* 快速全扫 0x08~0x77（依赖外部上拉） */
static void scan_fast(uint8_t *found_addr, uint8_t *found_count)
{
    uint8_t addr;
    uint8_t count = 0U;
    uint8_t first = 0U;

    pins_restore_od(OLED_BUS_PULL);

    for (addr = 0x08U; addr <= 0x77U; addr++) {
        if (i2c_probe(addr, 0U, 1U)) {
            if (count == 0U) { first = addr; }
            if (count < 3U) {
                printf("# [3a] 应答: 0x%02X(7位) / 0x%02X(8位)\r\n",
                       (unsigned)addr, (unsigned)((addr << 1) & 0xFEU));
            }
            count++;
        }
    }

    if (count == 0U) { printf("# [3a] 快速扫描 0x08~0x77：无器件应答\r\n"); }

    *found_addr  = first;
    *found_count = count;
}

/* 内部上拉 + 指定速率，只扫 0x3C / 0x3D（约 5ms 或 1ms） */
static uint8_t scan_internal_pullup(uint8_t mode, uint8_t *which)
{
    uint8_t addr;
    uint8_t hit = 0U;

    pins_restore_od(GPIO_PULLUP);      /* ⭐ 让 MCU 自己的 ~40k 上拉顶上 */

    for (addr = 0x3CU; addr <= 0x3DU; addr++) {
        if (i2c_probe(addr, mode, 1U)) {
            *which = addr;
            hit = 1U;
            break;
        }
    }

    pins_restore_od(OLED_BUS_PULL);      /* 还原成工程原本的配置 */
    return hit;
}

/* ---------------------------------------------------------------------------
 * I2C 总线复位。
 *
 * 为什么需要：自检会强行改引脚模式、把时序打断，从机可能卡在"等第 9 个时钟"
 * 的状态，一直把 SDA 拉低不撒手（实测出现过 pu_sda=0 / pd2_sda=0）。
 * 标准救法：主机连给 9 个 SCL 时钟，让从机把没发完的位吐干净，再补一个 STOP。
 * 不加这一步，被卡住的从机会把后面的显示数据全带偏。
 * ------------------------------------------------------------------------- */
static void i2c_bus_recover(void)
{
    uint8_t i;

    pins_restore_od(OLED_BUS_PULL);
    sda_write(1);                    /* 释放 SDA，只由主机打时钟 */

    for (i = 0U; i < 9U; i++) {
        scl_write(1); delay_fast();
        scl_write(0); delay_fast();
    }

    /* 补一个 STOP：SCL 高电平期间 SDA 由低变高 */
    sda_write(0); delay_fast();
    scl_write(1); delay_fast();
    sda_write(1); delay_fast();
}

/* ---------------------------------------------------------------------------
 * [4] 充电曲线：把两根线放开（内部上拉），在 8 个时间点采样。
 *
 *    为什么要这个：内部 40k 上拉对 100pF 线路的时间常数只有 4us，
 *    2ms 内必定到高电平。若 2ms 还读 0，只有两种可能：
 *      ① 线被硬短到 GND            → 8 个采样点全是 0
 *      ② 线经过了 ESD 二极管去充一颗大电容（屏没供电）→ 采样点会由 0 逐渐变 1
 *    时间常数 40k x 10uF = 400ms 量级，所以采样点铺到 1.5 秒就足够分辨。
 * ------------------------------------------------------------------------- */
static const uint16_t k_rise_ms[8] = { 0U, 1U, 5U, 20U, 50U, 200U, 500U, 1500U };

static void test_rise(uint8_t *mask_scl, uint8_t *mask_sda)
{
    uint16_t elapsed = 0U;
    uint8_t  i;

    *mask_scl = 0U;
    *mask_sda = 0U;

    pins_mode(GPIO_MODE_INPUT, GPIO_PULLUP);

    for (i = 0U; i < 8U; i++) {
        if (k_rise_ms[i] > elapsed) {
            HAL_Delay((uint32_t)(k_rise_ms[i] - elapsed));
            elapsed = k_rise_ms[i];
        }
        if (scl_read()) { *mask_scl |= (uint8_t)(1U << i); }
        if (sda_read()) { *mask_sda |= (uint8_t)(1U << i); }
    }
}

static void print_rise(const char *name, uint8_t mask)
{
    uint8_t i;
    printf("#      %s:", name);
    for (i = 0U; i < 8U; i++) {
        printf(" %u", (unsigned)((mask >> i) & 1U));
    }
    printf("\r\n");
}

/* ---------------------------------------------------------------------------
 * 对外入口。verbose = 1 打印详细，= 0 只更新 RAM（供主循环每秒调用）
 * ------------------------------------------------------------------------- */
void OLED_DIAG_Run(uint8_t verbose)
{
    uint8_t pd_scl = 0U, pd_sda = 0U, pu_scl = 0U, pu_sda = 0U;
    uint8_t b0_ok = 0U, b5_ok = 0U;
    uint8_t shorted = 0U;
    uint8_t addr = 0U, count = 0U;
    uint8_t slow_addr = 0U, slow_ack = 0U;
    uint8_t fast_addr = 0U, fast_ack = 0U;
    uint8_t raw_addr = 0U, raw_ack = 0U;
    uint8_t pd2_scl = 0U, pd2_sda = 0U;
    uint8_t rise_scl = g_oled_diag.rise_scl;   /* 连续模式下沿用开机那次的结果 */
    uint8_t rise_sda = g_oled_diag.rise_sda;
    uint8_t method_ok;
    uint8_t lines_ok;

    if (verbose) {
        printf("\r\n# ========== OLED 硬件自检 ==========\r\n");
        printf("# 引脚: PB8=SCL  PB9=SDA  屏地址应为 0x78(8位) / 0x3C(7位)\r\n");
    }

    method_ok = test_control(&b0_ok, &b5_ok);

    /* 充电曲线要 1.5 秒，只在开机详细模式下测 */
    if (verbose) {
        test_rise(&rise_scl, &rise_sda);
    }

    test_line_levels(&pd_scl, &pd_sda, &pu_scl, &pu_sda);

    /* 互短测试只在"线上真有上拉"且读数可信时才做：没有上拉时 SDA 浮空，
       读数没意义，会把悬空误判成短路。 */
    if (pd_scl && pd_sda && method_ok) {
        shorted = test_short();
    }

    lines_ok = (pd_scl && pd_sda && !shorted) ? 1U : 0U;

    if (verbose) {
        printf("# [0] 对照组 PB0=%s PB5=%s (IDR 与 ODR 一致) => 读数路径%s\r\n",
               b0_ok ? "一致" : "不一致", b5_ok ? "一致" : "不一致",
               method_ok ? "可靠" : "不可靠");
        printf("# [1] 内部下拉读: SCL=%u SDA=%u   (1=外部有上拉源)\r\n",
               (unsigned)pd_scl, (unsigned)pd_sda);
        printf("# [2] 内部上拉读: SCL=%u SDA=%u   (0=被拽在低电平)\r\n",
               (unsigned)pu_scl, (unsigned)pu_sda);
        printf("# [4] 充电曲线(0/1/5/20/50/200/500/1500 ms):\r\n");
        print_rise("SCL", rise_scl);
        print_rise("SDA", rise_sda);
        if (method_ok) {
            printf("# [3] SCL/SDA 互短: %s\r\n", shorted ? "是" : "否");
        } else {
            printf("# [3] 对照组不过 => 读电平这条路径有问题，本轮结论不可信\r\n");
        }
    }

    if (lines_ok) {
        scan_fast(&addr, &count);
    } else if (verbose) {
        printf("# [3a] 跳过快速扫描：线上没有上拉源，SDA 恒低，扫什么都会误报应答\r\n");
    }

    /* ⭐ 无论上一轮结果如何都做：开内部上拉，快速和慢速各扫一次。
     *    快速那轮用接近 OLED.c 的真实时序，用来判断"只改配置够不够"。 */
    fast_ack = scan_internal_pullup(0U, &fast_addr);
    slow_ack = scan_internal_pullup(1U, &slow_addr);
    raw_ack  = scan_internal_pullup(2U, &raw_addr);   /* OLED.c 原样时序：零延时 */

    if (verbose) {
        printf("# [3b] 内部上拉 零延时/快速/慢速: %u / %u / %u\r\n",
               (unsigned)raw_ack, (unsigned)fast_ack, (unsigned)slow_ack);
        if (raw_ack) {
            printf("#      => 零延时都能应答：只把 GPIO_NOPULL 改成 GPIO_PULLUP 就够，不用降速\r\n");
        } else if (fast_ack) {
            printf("#      => 零延时不行、快速行：改 GPIO_PULLUP + OLED.c 每位加 3us 延时\r\n");
        } else if (slow_ack) {
            printf("#      => 只有慢速行：改 GPIO_PULLUP + OLED.c 每位加 20us 延时\r\n");
        } else {
            printf("#      => 三档都不应答\r\n");
        }
    }

    /* ⭐ 扫描可能给屏的 VDD 电容"充了一点电"。这时再用内部下拉测一次：
     *    若读数从 0 变成 1，说明屏的板载上拉刚才被充活了 —— 也就是屏在靠总线偷电，
     *    铁证是"VCC 没接"。 */
    test_line_levels(&pd2_scl, &pd2_sda, &pu_scl, &pu_sda);

    if (verbose && (pd2_scl != pd_scl || pd2_sda != pd_sda)) {
        printf("# [5] 扫描后再测内部下拉: SCL=%u SDA=%u  ← 变了！说明屏在靠信号线偷电\r\n",
               (unsigned)pd2_scl, (unsigned)pd2_sda);
    }

    i2c_bus_recover();                   /* ⭐ 把可能被卡住的从机放出来 */
    pins_restore_od(OLED_BUS_PULL);      /* 交还给 OLED.c */

    /* ---- 存一份到 RAM ---- */
    g_oled_diag.pd_scl     = pd_scl;
    g_oled_diag.pd_sda     = pd_sda;
    g_oled_diag.pu_scl     = pu_scl;
    g_oled_diag.pu_sda     = pu_sda;
    g_oled_diag.rise_scl   = rise_scl;
    g_oled_diag.rise_sda   = rise_sda;
    g_oled_diag.shorted    = shorted;
    g_oled_diag.ack_count  = count;
    g_oled_diag.first_addr = addr;
    g_oled_diag.ctl_pd     = b0_ok;
    g_oled_diag.ctl_pu     = b5_ok;
    g_oled_diag.slow_ack   = slow_ack;
    g_oled_diag.slow_addr  = slow_addr;
    g_oled_diag.fast_ack   = fast_ack;
    g_oled_diag.fast_addr  = fast_addr;
    g_oled_diag.raw_ack    = raw_ack;
    g_oled_diag.raw_addr   = raw_addr;
    g_oled_diag.pd2_scl    = pd2_scl;
    g_oled_diag.pd2_sda    = pd2_sda;
    g_oled_diag.magic      = 0x4F4C4544U;   /* 最后写，有值即代表前面全跑完 */

    if (!verbose) { return; }

    /* ---- 结论 ---- */
    printf("# ---------- 结论 ----------\r\n");
    if (!method_ok) {
        printf("# 对照组不过，本轮不可信。先查固件，别去改接线\r\n");
    } else if (rise_scl == 0x00U && rise_sda == 0x00U) {
        printf("# 1.5 秒都升不到高电平 => 两根线被硬短到 GND（或 40k 根本带不动）\r\n");
        printf("# 先拔掉 OLED 的 SCL/SDA 两根线再上电看读数，就能分清是板子短还是屏内短\r\n");
    } else if ((rise_scl != 0xFFU) || (rise_sda != 0xFFU)) {
        printf("# 充电曲线是逐渐上升 => 信号线在给一颗大电容充电。\r\n");
        printf("# 这一般是屏没供电、电流经 ESD 二极管倒灌进 VDD 电容的典型现象：\r\n");
        printf("# 此时屏可能仍能应答地址（靠偷来的电），但电荷泵起不来，屏永远是黑的。\r\n");
        printf("# 结论：优先查 OLED 的 VCC 到底有没有接到 3.3V\r\n");
    } else if (raw_ack) {
        printf("# 总线 1ms 内就到高电平，且 0x3C 应答 => 电气全通。问题在初始化或显示内容\r\n");
    } else if (fast_ack) {
        printf("# 零延时不行、快速行。修法：OLED.c 每位加约 3us 延时\r\n");
    } else if (slow_ack) {
        printf("# 只有慢速行。修法：OLED.c 每位加约 20us 延时\r\n");
    } else if (count > 0U) {
        if (addr == 0x3CU) {
            printf("# 屏在线且应答正常(0x3C)。若仍黑屏，问题在初始化或显示内容，不在接线\r\n");
        } else {
            printf("# 总线有器件应答，但地址是 0x%02X 不是 0x3C —— OLED.c 写死 0x78，需改\r\n",
                   (unsigned)addr);
        }
    } else if (shorted) {
        printf("# SCL 和 SDA 短在一起 => 两根线插进了面包板同一行，错开一行\r\n");
    } else if (pd_scl && pd_sda) {
        printf("# 供电和走线全通，但屏一个字都不回 => 屏已损坏，或这块是 SH1106 不是 SSD1306\r\n");
    } else if (!pd_scl && !pd_sda && !pu_scl && !pu_sda) {
        printf("# 两根线被钳在低电平，内部上拉也拉不起来 => 屏没通电，或两根线短到 GND\r\n");
        printf("# 优先查 OLED 的 VCC 有没有真正到 3.3V（VDD 为 0 时 ESD 管会把总线钳死在 0.6V）\r\n");
    } else if (!pd_scl && !pd_sda) {
        printf("# 线是通的（内部上拉能拉高），但屏那边没有上拉源 => 屏没供电到上拉电阻\r\n");
    } else {
        printf("# 只有一根不通 => 断的是 %s 那一根\r\n",
               (!pd_scl) ? "SCL -> PB8" : "SDA -> PB9");
    }
    printf("# ========== 自检结束 ==========\r\n\r\n");
}
