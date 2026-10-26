/* ============================================================================
 * display.c —— OLED 显示实现（128x64 / SSD1306 / 软件 I2C / 4 行 16 列）
 *
 * 显示内容设计（等器件的空档期，先把手上的信息显示完）：
 *
 *     ┌────────────────┐
 *     │ MOTOR v0.1 N1  │  ← 工程版本 + 节点号（NODE_ID）
 *     │ DUTY +060% F   │  ← 当前占空比（带符号）+ 方向 F/R/S
 *     │ RUN  012.3s    │  ← 从上电开始的运行时间，0.1s 精度
 *     │ SET 042    RDY │  ← SetDuty 调用次数 + 子系统就绪标志
 *     └────────────────┘
 *
 * 为什么选这四行：这四样全都是**不依赖外部器件**就能验证的量。
 * 特别是第 4 行的 SET 计数 —— 它在涨，说明主循环真的在跑、MOTOR_SetDuty()
 * 真的在被调用。电机没到的时候，这一行就是"程序还活着"的仪表盘。
 *
 * 等编码器/串口/CAN 到位，再往上加：目标 vs 实际、CAN 收发计数、故障码。
 * ==========================================================================*/
#include "display.h"
#include "config.h"
#include "motor.h"
#include "OLED.h"

/* 刷新周期。200ms = 5Hz，人眼看着不累，I2C 也来得及。
 * 千万别调到 20ms 以下 —— 那时候屏幕会抢主循环的时间。*/
#define DISP_PERIOD_MS        200U

static uint32_t s_last_refresh = 0;

/* ---------------------------------------------------------------------------
 * @brief  初始化 OLED 并画出静态文字
 * -------------------------------------------------------------------------*/
void DISPLAY_Init(void)
{
    OLED_Init();      /* SSD1306 初始化 + 清屏 */
    OLED_Clear();

    /* 第一行的静态部分（行号 1，列号 1 起，江科大 OLED 驱动的语法）*/
    OLED_ShowString(1, 1, "MOTOR v0.1 N");
    OLED_ShowNum(1, 13, NODE_ID, 2);          /* 节点号，2 位 */
}

/* ---------------------------------------------------------------------------
 * @brief  刷新动态内容
 * -------------------------------------------------------------------------*/
void DISPLAY_Refresh(void)
{
    uint32_t now;
    MOTOR_Status_t st;
    int16_t  pct;         /* 百分数，带符号：-100 ~ +100 */
    uint16_t sec;
    uint8_t  dec;

    /* ---- 节流：没到 200ms 就直接返回 ---- */
    now = HAL_GetTick();
    if ((now - s_last_refresh) < DISP_PERIOD_MS)
    {
        return;
    }
    s_last_refresh = now;

    /* 从驱动层取"权威状态"，不自己猜 */
    MOTOR_GetStatus(&st);

    /* ================= 第 2 行：占空比 + 方向 ================= */
    OLED_ShowString(2, 1, "DUTY");

    pct = (int16_t)(st.duty_permille / 10);   /* 600 千分比 → 60 百分数 */

    if (pct >= 0)
    {
        OLED_ShowChar(2, 7, '+');
        OLED_ShowNum(2, 8, (uint32_t)pct, 3);   /* 补零到 3 位，如 060 */
    }
    else
    {
        OLED_ShowChar(2, 7, '-');
        OLED_ShowNum(2, 8, (uint32_t)(-pct), 3); /* 先取正再显示 */
    }
    OLED_ShowChar(2, 11, '%');

    /* 方向指示：F = 正转，R = 反转，S = 停止 */
    if (st.duty_permille > 0)
    {
        OLED_ShowString(2, 14, "F");
    }
    else if (st.duty_permille < 0)
    {
        OLED_ShowString(2, 14, "R");
    }
    else
    {
        OLED_ShowString(2, 14, "S");
    }

    /* ================= 第 3 行：运行时间 ================= */
    OLED_ShowString(3, 1, "RUN");

    sec = (uint16_t)((now / 100U) / 10U);     /* 秒 */
    dec = (uint8_t)((now / 100U) % 10U);      /* 0.1 秒那一位 */

    OLED_ShowNum(3, 5, (uint32_t)(sec % 1000U), 3);   /* 只占 3 位，到 999.9s 绕回 */
    OLED_ShowChar(3, 8, '.');
    OLED_ShowNum(3, 9, (uint32_t)dec, 1);
    OLED_ShowChar(3, 10, 's');

    /* ================= 第 4 行：调用计数 + 就绪 ================= */
    OLED_ShowString(4, 1, "SET");
    OLED_ShowNum(4, 5, (uint32_t)(st.set_count % 1000U), 3);

    if (MOTOR_IsReady())
    {
        OLED_ShowString(4, 11, "RDY");
    }
    else
    {
        OLED_ShowString(4, 11, "---");   /* 不该出现；出现 = STBY 没拉高 */
    }
}
