/* ============================================================================
 * can_bsp.c   STM32F103C8T6 bxCAN 用户侧驱动（v0.1）
 * ==========================================================================*/
#include "can_bsp.h"
#include "main.h"
#include <string.h>

CAN_HandleTypeDef hcan_bsp;

/* 接收环形缓冲区：中断里只负责"快速搬进来"，主循环慢慢取 */
#define RX_FIFO_DEPTH   16

static CAN_BSP_Frame_t s_rx_fifo[RX_FIFO_DEPTH];
static volatile uint8_t  s_rx_head = 0;
static volatile uint8_t  s_rx_tail = 0;
static volatile uint32_t s_rx_count = 0;
static volatile uint32_t s_tx_count = 0;

/* ----------------------------------------------------------------------------
 * 时钟依据（这地方最容易拍脑袋拍错，所以写清楚）
 *
 *   STM32F103 的 bxCAN 挂在 APB1 上，CAN 时钟 = PCLK1。
 *   ⚠️ 它【不像定时器那样自动 ×2】！
 *      - 定时器：APB1 预分频 ≠1 时，TIMxCLK = 2 × PCLK1 = 72MHz
 *      - bxCAN ：最高只能跑 36MHz，所以就是 PCLK1 = 36MHz（不翻倍）
 *
 *   本工程时钟链：HSE 8MHz → PLL×9 → SYSCLK 72MHz → AHB÷1 → APB1÷2 → PCLK1 = 36MHz
 *
 *   波特率 = CAN时钟 / (Prescaler × (1 + BS1 + BS2))
 *          36MHz / (6 × 12) = 500 kbps
 *   采样点 = (1 + BS1) / (1 + BS1 + BS2) = 9 / 12 = 75%
 *
 *   ★ 选 500kbps + 75% 是因为绝大多数 USB-CAN 分析仪默认就是这组，最容易互通。
 *     要改波特率就同步改 Prescaler（换速率只需改这一个数，Tq 分配不用动）：
 *       250kbps → 12      1Mbps → 3
 * -------------------------------------------------------------------------*/
void CAN_BSP_Init(void)
{
    CAN_FilterTypeDef filter;

    __HAL_RCC_CAN1_CLK_ENABLE();

    hcan_bsp.Instance = CAN1;

    hcan_bsp.Init.Prescaler              = 6;
#if CAN_BSP_LOOPBACK
    hcan_bsp.Init.Mode                   = CAN_MODE_LOOPBACK;
#else
    hcan_bsp.Init.Mode                   = CAN_MODE_NORMAL;
#endif
    hcan_bsp.Init.SyncJumpWidth          = CAN_SJW_1TQ;
    hcan_bsp.Init.TimeSeg1               = CAN_BS1_8TQ;   /* 1 + 8 + 3 = 12 Tq */
    hcan_bsp.Init.TimeSeg2               = CAN_BS2_3TQ;
    hcan_bsp.Init.TimeTriggeredMode      = DISABLE;
    hcan_bsp.Init.AutoBusOff             = ENABLE;
    hcan_bsp.Init.AutoWakeUp             = DISABLE;
    hcan_bsp.Init.AutoRetransmission     = ENABLE;        /* 发送失败自动重发 */
    hcan_bsp.Init.ReceiveFifoLocked      = DISABLE;       /* FIFO 满了覆盖最旧的 */
    hcan_bsp.Init.TransmitFifoPriority   = DISABLE;       /* 按 mailbox 顺序发 */

    if (HAL_CAN_Init(&hcan_bsp) != HAL_OK)
    {
        Error_Handler();
    }

    /* ---- 滤波器：Mask 全 0 = 全部通过 ------------------------------------
     * ⚠️ STM32F1 的 bxCAN 【不配滤波器，一个帧都收不到】！
     *    F1 用的是 filter bank 组，跟 F4 的"简单 mask"不是一回事。
     *    这里 14 个 bank 里只用 bank 0，掩码全 0 ⇒ 所有 ID 都放进 FIFO0。
     *    SlaveStartFilterBank = 14：F103 只有一个 CAN，14 = 不留给从机。      */
    filter.FilterBank            = 0;
    filter.FilterMode            = CAN_FILTERMODE_IDMASK;
    filter.FilterScale           = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh          = 0x0000;
    filter.FilterIdLow           = 0x0000;
    filter.FilterMaskIdHigh      = 0x0000;
    filter.FilterMaskIdLow       = 0x0000;
    filter.FilterFIFOAssignment  = CAN_FILTER_FIFO0;
    filter.FilterActivation      = ENABLE;
    filter.SlaveStartFilterBank  = 14;

    if (HAL_CAN_ConfigFilter(&hcan_bsp, &filter) != HAL_OK)
    {
        Error_Handler();
    }

    /* ---- 接收通知 ---------------------------------------------------------
     * ⚠️ 顺序不能反：必须先 Start，再 ActivateNotification，最后 NVIC 才会生效 */
    if (HAL_CAN_ActivateNotification(&hcan_bsp, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK)
    {
        Error_Handler();
    }

    /* ⚠️ 中断入口名是 USB_LP_CAN1_RX0_IRQHandler，不是 CAN1_RX0_IRQHandler
     *    （CAN RX0 和 USB-LP 共用同一个向量，启动文件里只有前者）            */
    HAL_NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);

    if (HAL_CAN_Start(&hcan_bsp) != HAL_OK)
    {
        Error_Handler();
    }
}

/* ----------------------------------------------------------------------------
 * 发送一个标准帧
 * -------------------------------------------------------------------------*/
int8_t CAN_BSP_SendStd(uint32_t std_id, const uint8_t *data, uint8_t len)
{
    CAN_TxHeaderTypeDef tx_header;
    uint32_t mailbox;
    HAL_StatusTypeDef st;

    if (len > 8U)
    {
        len = 8U;
    }
    std_id &= 0x7FFU;                      /* 标准帧只有 11 位 */

    tx_header.StdId              = std_id;
    tx_header.ExtId              = 0U;
    tx_header.IDE                = CAN_ID_STD;
    tx_header.RTR                = CAN_RTR_DATA;
    tx_header.DLC                = len;
    tx_header.TransmitGlobalTime = DISABLE;

    st = HAL_CAN_AddTxMessage(&hcan_bsp, &tx_header, (uint8_t *)data, &mailbox);

    if (st == HAL_OK)
    {
        s_tx_count++;
    }
    return (int8_t)st;
}

/* ----------------------------------------------------------------------------
 * 主循环侧取帧
 * -------------------------------------------------------------------------*/
uint8_t CAN_BSP_ReadFrame(CAN_BSP_Frame_t *out)
{
    uint8_t next;

    if (s_rx_head == s_rx_tail)
    {
        return 0;                          /* 空 */
    }

    next = (uint8_t)((s_rx_tail + 1U) % RX_FIFO_DEPTH);
    *out = s_rx_fifo[s_rx_tail];
    s_rx_tail = next;
    return 1;
}

uint32_t CAN_BSP_GetRxCount(void)
{
    return s_rx_count;
}

uint32_t CAN_BSP_GetTxCount(void)
{
    return s_tx_count;
}

/* ----------------------------------------------------------------------------
 * 中断回调：只做"搬进来"，不做任何耗时操作
 * -------------------------------------------------------------------------*/
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef rx_header;
    uint8_t  data[8];
    uint8_t  next;

    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rx_header, data) != HAL_OK)
    {
        return;
    }

    next = (uint8_t)((s_rx_head + 1U) % RX_FIFO_DEPTH);
    if (next != s_rx_tail)                 /* 不满才收，满了就丢这一帧 */
    {
        s_rx_fifo[s_rx_head].id  = (rx_header.IDE == CAN_ID_STD)
                                    ? rx_header.StdId : rx_header.ExtId;
        s_rx_fifo[s_rx_head].len = rx_header.DLC;
        memcpy(s_rx_fifo[s_rx_head].data, data, rx_header.DLC);
        s_rx_head = next;
    }
    s_rx_count++;
}

/* ----------------------------------------------------------------------------
 * 中断服务函数：必须叫这个名字（向量表里 USB_LP 和 CAN1_RX0 是同一个入口）
 * -------------------------------------------------------------------------*/
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    HAL_CAN_IRQHandler(&hcan_bsp);
}
