/* ---------------------------------------------------------------------------
 * usart.c —— USART1 驱动层（v0.2）
 *
 * 设计来源：E:\Demo\SimpleUARTTx（2026-08 本机实测跑通的写法）
 * 本文件只改了三处：
 *   ① 引脚配置从 MX_GPIO_Init 拆进 HAL_UART_MspInit（HAL 惯例，usart 引脚归 usart 管）
 *   ② 补 USART_RxAvailable/ReadByte 的非阻塞接口
 *   ③ 补 USART_PollTask 统一清 ORE
 *
 * ⚠️ 文件末尾的 __io_putchar 故意定义为"非 weak"，用来覆盖
 *    syscalls.c 第 35 行的 weak 声明。链接器选强的那个 ⇒ printf 才会走到串口。
 *    如果写成 weak，_write() 里那个 weak 版会赢，printf 直接消失（静默失败）。
 * --------------------------------------------------------------------------- */

#include <string.h>

#include "main.h"
#include "config.h"
#include "usart.h"

UART_HandleTypeDef huart1;

/* ------------------------------------------------------------------ */
/* 引脚配置                                                            */
/* ------------------------------------------------------------------ */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (huart->Instance == USART1)
    {
        /* USART1 时钟 = APB2 = 72MHz（USART2/3 才是 APB1 36MHz） */
        __HAL_RCC_USART1_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();

        /* PA9 = TX，复用推挽输出 */
        GPIO_InitStruct.Pin   = GPIO_PIN_9;
        GPIO_InitStruct.Mode  = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull  = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        /* PA10 = RX，上拉输入（悬空会收到乱码） */
        GPIO_InitStruct.Pin   = GPIO_PIN_10;
        GPIO_InitStruct.Mode  = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull  = GPIO_PULLUP;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    }
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */
void USART_Init(void)
{
    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = USART1_BAUDRATE;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart1) != HAL_OK)
    {
        /* 与 motor.c 同一套约定：失败就闪 LED 告诉你，绝不死等 */
        while (1)
        {
            HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
            HAL_Delay(80U);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 发送                                                                */
/* ------------------------------------------------------------------ */
void USART_SendByte(uint8_t b)
{
    (void)HAL_UART_Transmit(&huart1, &b, 1U, USART1_TX_TIMEOUT_MS);
}

void USART_SendString(const char *s)
{
    if (s == 0) return;
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)strlen(s),
                            USART1_TX_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ */
/* 接收（非阻塞）                                                      */
/* ------------------------------------------------------------------ */
uint8_t USART_RxAvailable(void)
{
    return (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) != RESET) ? 1U : 0U;
}

uint8_t USART_ReadByte(void)
{
    return (uint8_t)(huart1.Instance->DR & 0xFFU);
}

/* ------------------------------------------------------------------ */
/* 主循环任务：清 ORE + 回显                                            */
/* ------------------------------------------------------------------ */
void USART_PollTask(void)
{
    uint8_t rx;

    /* ⭐ 溢出错误必须先清。
     * 现象：没清 ORE 时，RXNE 在第一次溢出后永不再置位，串口看起来"假死"，
     * 但 TX 一切正常——你会以为是线的问题，其实是这个标志卡住了。 */
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE) != RESET)
    {
        __HAL_UART_CLEAR_OREFLAG(&huart1);
    }

    if (USART_RxAvailable())
    {
        rx = USART_ReadByte();
        USART_SendByte(rx);   /* 回显，验证链路（v0.2 阶段够用） */
    }
}

/* ------------------------------------------------------------------ */
/* printf 重定向                                                       */
/* ------------------------------------------------------------------ */
/* ⚠️ 非 weak，故意覆盖 syscalls.c 里的 weak 声明 */
int __io_putchar(int ch)
{
    uint8_t c = (uint8_t)ch;

    if (c == '\n')
    {
        /* 终端里 '\n' 只换行不回列头 ⇒ 转成 CRLF，Excel/串口助手才不会叠字 */
        uint8_t cr = '\r';
        (void)HAL_UART_Transmit(&huart1, &cr, 1U, USART1_TX_TIMEOUT_MS);
    }

    (void)HAL_UART_Transmit(&huart1, &c, 1U, USART1_TX_TIMEOUT_MS);
    return ch;
}
