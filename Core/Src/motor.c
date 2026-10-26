/* ============================================================================
 * motor.c —— 直流电机驱动实现（TB6612 + TIM1_CH1 / PA8）
 *
 * 本文件刻意不依赖 main.c 的任何内容，只依赖 CubeMX 生成的 htim1 句柄
 * （extern 进来，CubeMX 生成什么名字这里就跟什么名字对上）。
 * 换来一个好处：可以整份搬到任何 F103 + TB6612 工程里直接编译。
 *
 * ⚠️ 全文件 4 个真实坑，都在注释里标了 ⭐，改代码前先读完：
 *    ① MOTOR_Init() 里必须先写方向脚再动 PWM —— 上电瞬间电机不能抖动
 *    ② STBY 必须拉高 —— 忘了它，电机不转且量不出来
 *    ③ TIM1 是高级定时器，必须额外置 BDTR.MOE —— HAL_TIM_PWM_Start 不代劳
 *    ④ CCR 是 uint32_t —— 负数直写会溢出成 42 亿，占空比恒 100%
 * ==========================================================================*/
#include "motor.h"
#include "config.h"

/* CubeMX 在 Core/Src/tim.c 里生成 */
extern TIM_HandleTypeDef htim1;

static MOTOR_Status_t s_motor =
{
    .stby_ok       = 0U,
    .ready         = 0U,
    .duty_permille = 0,
    .init_tick     = 0UL,
    .set_count     = 0UL,
};

/* ---------------------------------------------------------------------------
 * @brief  千分比占空比 → CCR 计数值
 *
 * ⭐ 坑④：CCR 寄存器是 uint32_t 无符号。如果把负的 duty 直接算进去再写入，
 *   会溢出成一个接近 42 亿的数，效果是"占空比 100%、电机全速、且完全不受控"。
 *   曾经的现场现象：反转代码明明写了，电机一直正转全速。
 *
 * 所以：① 先取绝对值 → ② 再夹到 0~1000 → ③ 最后才乘法。
 * -------------------------------------------------------------------------*/
static inline uint32_t duty_to_ccr(int16_t duty_permille)
{
    uint32_t magnitude;

    /* 1) 取绝对值 */
    magnitude = (duty_permille >= 0) ? (uint32_t)duty_permille
                                     : (uint32_t)(-duty_permille);

    /* 2) 夹到 0~1000 */
    if (magnitude > MOTOR_DUTY_MAX_PERMILLE)
    {
        magnitude = MOTOR_DUTY_MAX_PERMILLE;
    }

    /* 3) 0~1000 → 0~(ARR+1)，全程 uint32，中间不会溢出 */
    return (uint32_t)((magnitude * (uint32_t)(MOTOR_TIM1_ARR + 1U)) / 1000U);
}

/* ---------------------------------------------------------------------------
 * @brief  写方向 + 写占空比
 * -------------------------------------------------------------------------*/
static void motor_write(int16_t duty_permille)
{
    uint32_t ccr = duty_to_ccr(duty_permille);

    /* 方向由 AIN1/AIN2 两个 GPIO 决定，和 PWM 占空比完全解耦 */
    if (duty_permille > 0)
    {
        HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN1, GPIO_PIN_SET);
        HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN2, GPIO_PIN_RESET);
    }
    else if (duty_permille < 0)
    {
        HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN1, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN2, GPIO_PIN_SET);
    }
    else
    {
        /* 两个都拉低 = 自由停止(Hi-Z)，电机自己滑行停下 */
        HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN1, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN2, GPIO_PIN_RESET);
    }

    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, ccr);   /* ⭐ 双下划线 */
}

/* ---------------------------------------------------------------------------
 * @brief  初始化电机子系统
 *
 * 顺序很重要：
 *   ① 先把方向脚压到"停止"  → 上电一瞬间电机绝对不能乱动
 *   ② 再拉高 STBY            → 驱动器才出得了力
 *   ③ 最后启动 PWM + MOE     → 通道才有波形
 *
 * ⭐③ 里的 MOE：TIM1 是"高级定时器"，它的输出除了受通道使能位(CCxE)控制，
 *   还受 BDTR 寄存器里的 MOE(Main Output Enable) 一票否决。
 *   HAL_TIM_PWM_Start() 只会帮 CCxE，不会帮 MOE ——
 *   第一次用 TIM1 的人十有八九栽在这儿：代码跑通、寄存器看着都对、
 *   逻辑分析仪接到 PA8 上，一片寂静。
 * -------------------------------------------------------------------------*/
void MOTOR_Init(void)
{
    /* ① 方向脚先归零（这一步必须最前）
     *    gpio.c 的 MX_GPIO_Init() 已经置过低，这里再写一次是双保险 ——
     *    万一以后有人在 CubeMX 里把 GPIO output level 改成 High，这里有兜底。 */
    HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN1, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN2, GPIO_PIN_RESET);

    /* ② STBY 拉高 —— TB6612 的低电平休眠脚 */
    HAL_GPIO_WritePin(MOTOR_STBY_PORT, MOTOR_STBY_PIN, GPIO_PIN_SET);
    s_motor.stby_ok = 1U;

    /* ③ 启动 TIM1_CH1 的 PWM 输出 */
    if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1) != HAL_OK)
    {
        /* PWM 起不来是"显式失败"：让 LED 狂闪，不要静默苟着 */
        while (1)
        {
            HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
            HAL_Delay(60U);
        }
    }
    __HAL_TIM_MOE_ENABLE(&htim1);          /* ⭐ TIM1 专属，别漏 */

    /* 上电就是 0 占空比，保证安全 */
    motor_write(0);

    s_motor.ready         = 1U;
    s_motor.duty_permille = 0;
    s_motor.init_tick     = HAL_GetTick();
}

/* ---------------------------------------------------------------------------
 * @brief  设置电机速度（带符号，千分比）
 * -------------------------------------------------------------------------*/
void MOTOR_SetDuty(int16_t duty_permille)
{
    /* --- 三重检查，谁调进来都不许越界 --- */
    if (!s_motor.stby_ok)          return;   /* STBY 没拉高 = 写了也白写，不如不动 */

    if (duty_permille >  (int16_t)MOTOR_DUTY_MAX_PERMILLE)
        duty_permille =  (int16_t)MOTOR_DUTY_MAX_PERMILLE;
    if (duty_permille < -(int16_t)MOTOR_DUTY_MAX_PERMILLE)
        duty_permille = -(int16_t)MOTOR_DUTY_MAX_PERMILLE;

    motor_write(duty_permille);

    s_motor.duty_permille = duty_permille;
    s_motor.set_count++;
}

/* ---------------------------------------------------------------------------
 * @brief  自由停止 —— AIN1=AIN2=0, PWM=0, 电机靠惯性滑行
 *         ⭐ 不受 stby_ok 影响，急停/故障保护专用
 * -------------------------------------------------------------------------*/
void MOTOR_Stop(void)
{
    HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN1, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN2, GPIO_PIN_RESET);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0U);

    s_motor.duty_permille = 0;
}

/* ---------------------------------------------------------------------------
 * @brief  短路制动 —— AIN1=AIN2=1, PWM=0, 电机被强行钉住
 * -------------------------------------------------------------------------*/
void MOTOR_Brake(void)
{
    HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN1, GPIO_PIN_SET);
    HAL_GPIO_WritePin(MOTOR_DIR_PORT, MOTOR_DIR_PIN_AIN2, GPIO_PIN_SET);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0U);

    s_motor.duty_permille = 0;
}

/* ---------------------------------------------------------------------------
 * 状态查询 —— 调试期当"仪表盘"用
 * -------------------------------------------------------------------------*/
int16_t MOTOR_GetDuty(void)      { return s_motor.duty_permille; }

uint8_t MOTOR_IsReady(void)      { return (s_motor.ready && s_motor.stby_ok) ? 1U : 0U; }

void MOTOR_GetStatus(MOTOR_Status_t *out)
{
    if (out != NULL)
    {
        *out = s_motor;
    }
}
