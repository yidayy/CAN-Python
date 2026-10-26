/* ============================================================================
 * can_bsp.h   STM32F103C8T6 bxCAN 用户侧驱动（v0.1）
 *
 * 为什么单独写一个 BSP 文件、而不是在 CubeMX 里配 CAN：
 *   1. CAN 的 PA11/PA12 不需要在 CubeMX 里配 GPIO —— CAN 外设一旦使能就
 *      自动接管这两个脚（GPIOA 的时钟本来就已经开了）。
 *   2. 这个文件注册在「根 CMakeLists.txt」，CubeMX 重新生成时不会覆盖它，
 *      也不会和 CubeMX 自己生成的 can.c 撞车。
 *
 * 硬件前提（代码再对，缺一个都收不到帧）：
 *   - TJA1050 的 VCC 接 4.75~5.25V（不能接 3.3V）
 *   - CANH/CANL 两根线，总线两端各一个 120Ω
 *   - PA11 = CAN_RX（FT 脚，5V 容忍，可直接接 TJA1050 的 RXD）
 *     PA12 = CAN_TX
 *   - 所有节点共地
 * ==========================================================================*/
#ifndef CAN_BSP_H
#define CAN_BSP_H

#include "stm32f1xx_hal.h"

/* ------------------------------- 可调参数 -------------------------------*/
#define CAN_BSP_BAUD        500000U   /* 波特率 500 kbps（USB-CAN 分析仪最常见默认值）*/
/* ⚠️ 2026-10-19 设为 1：TJA1050 还没到货，先用自收自发验证 bxCAN+协议栈全链路。
 *    收发器到货并接好后改回 0 上真总线（改完必须重编译重烧）。 */
#define CAN_BSP_LOOPBACK    1         /* 1 = 自收自发：不接收发器/不接总线也能先验证外设
                                         0 = 正常模式：接 TJA1050 + 120Ω 时用这个      */

/* ------------------------------ 数据结构 -------------------------------*/
typedef struct
{
    uint32_t id;              /* 标准帧 ID（11 位，0x000~0x7FF）*/
    uint8_t  len;             /* DLC，0~8 */
    uint8_t  data[8];
} CAN_BSP_Frame_t;

extern CAN_HandleTypeDef hcan_bsp;

/* ------------------------------- 接口 ----------------------------------*/
/* 初始化：时钟 → CAN → 滤波器(全通过) → 接收通知 → NVIC → Start            */
void     CAN_BSP_Init(void);

/* 发送一个标准帧；返回 HAL_OK(0) / HAL_ERROR(1) / HAL_BUSY(2) / HAL_TIMEOUT(3) */
int8_t   CAN_BSP_SendStd(uint32_t std_id, const uint8_t *data, uint8_t len);

/* 从接收环形缓冲区取一帧；1 = 取到了，0 = 缓冲区空 */
uint8_t  CAN_BSP_ReadFrame(CAN_BSP_Frame_t *out);

/* 统计（用来判断到底发没发出去、收没收到）*/
uint32_t CAN_BSP_GetRxCount(void);
uint32_t CAN_BSP_GetTxCount(void);

#endif /* CAN_BSP_H */
