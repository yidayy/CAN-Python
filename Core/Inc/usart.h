#ifndef __USART_H
#define __USART_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * usart.h —— USART1 串口输出（v0.2）
 *
 * 引脚：PA9 = USART1_TX（AF_PP，接 USB-TTL 的 RX）
 *       PA10 = USART1_RX（本阶段只用 TX 打印，RX 预留在命令入口）
 * 参数：115200 8N1
 *
 * ⚠️ PA9/PA10 的 GPIO 配置归本文件的 HAL_UART_MspInit()，
 *    不归 gpio.c —— gpio.c 只管"板级"引脚（AIN/STBY/LED/OLED）。
 *
 * ⚠️ PB8/PB9（OLED）已被占用，不能改作 PB10/PB11 重映射。
 * --------------------------------------------------------------------------- */

#include "stm32f1xx_hal.h"

/* 波特率改这里，Baud 115200 下 BRR 计算值 = 72000000/115200 = 625 = 0x271 */
#define USART1_BAUDRATE           115200U
#define USART1_TX_TIMEOUT_MS      1000U

extern UART_HandleTypeDef huart1;

/* 初始化 USART1（含 PA9/PA10 引脚） */
void      USART_Init(void);

/* 阻塞发送 */
void      USART_SendString(const char *s);
void      USART_SendByte(uint8_t b);

/* 非阻塞接收：查到数据返回 1，*out 为字节；无数据返回 0 */
uint8_t   USART_RxAvailable(void);
uint8_t   USART_ReadByte(void);

/* 主循环每 tick 调用：清 ORE 标志 + 回显收到的字节。
 * ⚠️ ORE 不清则 RXNE 永不再置位，串口"假死"（SimpleUARTTx 踩过） */
void      USART_PollTask(void);

#ifdef __cplusplus
}
#endif

#endif /* __USART_H */
