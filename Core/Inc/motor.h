#ifndef __MOTOR_H
#define __MOTOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f1xx_hal.h"
#include <stdint.h>

/* ============================================================================
 * motor.h —— 直流电机驱动抽象层（TB6612 + TIM1_CH1 / PA8）
 *
 * 对上层只暴露"速度"这一样东西（单位：千分比）：
 *
 *     MOTOR_SetDuty(600)   正转 60.0%
 *     MOTOR_SetDuty(-730)  反转 73.0%
 *     MOTOR_SetDuty(0)     自由停止
 *
 * 上层不需要知道 AIN1/AIN2 谁高谁低、也不需要知道 CCR 是 0~3599。
 * 引脚和定时器全封在 motor.c 里，只从 config.h 取参数。
 *
 * ⭐ 本文件不依赖 main.c，只 extern 一个 CubeMX 生成的 htim1 句柄
 *    ⇒ 可以整份搬到任何 F103 + TB6612 工程里直接编译。
 * ==========================================================================*/

/* ----------------------------------------------------------------------------
 * 电机运行状态（上层可读，调试时打印用）
 * -------------------------------------------------------------------------*/
typedef struct
{
    uint8_t  stby_ok;        /* 1 = STBY 已拉高，驱动器工作；0 = 休眠，写什么都没用 */
    uint8_t  ready;          /* 1 = MOTOR_Init() 跑完，可对外输出                    */
    int16_t  duty_permille;  /* 当前占空比，千分比：600 = 60.0%，-730 = -73.0%      */
    uint32_t init_tick;      /* MOTOR_Init() 被调用的时刻（HAL_GetTick 的值）        */
    uint32_t set_count;      /* MOTOR_SetDuty 被成功调用次数，看它在动没动           */
} MOTOR_Status_t;

/* ----------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------*/
void      MOTOR_Init(void);

/**
 * @brief  设置电机速度（带符号）
 * @param  duty_permille  千分比，范围 [-1000, +1000] = [-100%, +100%]
 *                        正数正转、负数反转、0 = 自由停止
 * @note   ⭐ 内部三重保护：
 *         ① 越界值被夹到 [-1000, +1000]，不会出现 300% 这种蠢事
 *         ② CCR 是 uint32_t，负数先取绝对值再算，不会溢出成 42 亿
 *         ③ STBY 没拉高时直接 return，不会"看起来设了其实没动"
 */
void      MOTOR_SetDuty(int16_t duty_permille);

/**
 * @brief  停止 —— AIN1=AIN2=0, PWM=0, 自由滑行
 * @note   ⭐ 和 MOTOR_SetDuty(0) 的关键区别：这个函数不受 stby_ok 影响，
 *         任何时候都能无条件停机。急停、故障保护用的必须是它，不能用 SetDuty(0)。
 */
void      MOTOR_Stop(void);

/**
 * @brief  短路制动 —— AIN1=AIN2=1, PWM=0, 电机被强行钉住
 * @note   ⚠️ JGA25-370 是粉末冶金减速齿轮，很脆。制动时间别太长、
 *         也别高频反复用，齿轮齿容易崩角。
 */
void      MOTOR_Brake(void);

int16_t   MOTOR_GetDuty(void);   /* 取当前值，千分比                          */
uint8_t   MOTOR_IsReady(void);   /* 1 = 已初始化且能正常输出                    */
void      MOTOR_GetStatus(MOTOR_Status_t *out);   /* 查看内部状态（调试用）     */

#ifdef __cplusplus
}
#endif

#endif /* __MOTOR_H */
