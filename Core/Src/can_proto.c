/* ============================================================================
 * can_proto.c   应用层通信协议 v0.2
 * ==========================================================================*/
#include "can_proto.h"
#include "can_bsp.h"
#include <string.h>

static uint8_t  s_node_id      = 1;
static uint8_t  s_tx_seq       = 0;
static uint8_t  s_last_rx_seq  = 0;
static uint8_t  s_has_rx       = 0;   /* 是否已经收到过第一帧（用来跳过首次序号比对）*/

static CAN_PROTO_Stats_t       s_stats;
static CAN_PROTO_Handler_t     s_handler = 0;

/* ----------------------------------------------------------------------------
 * CRC-8/SMBUS：poly 0x07，init 0x00，无输入/输出反转，无异或输出
 *   逐位实现，不查表 —— 8 字节一帧，性能完全够，而且逻辑一眼看得懂。
 *   自检：对 ASCII "123456789" 得 0xF4（CRC-8/SMBUS 标准校验值）。
 *
 *   ⚠️ 这里有个 C 语言的坑：crc << 1 在 uint8_t 上会先提升成 int，
 *      不截断的话高位永远丢不掉，必须显式 & 0xFF。
 * -------------------------------------------------------------------------*/
uint8_t CAN_PROTO_Crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0x00;
    uint8_t i;
    uint8_t bit;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++)
        {
            if (crc & 0x80U)
            {
                crc = (uint8_t)((crc << 1) ^ 0x07U);
            }
            else
            {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* ----------------------------------------------------------------------------
 * 小端序打包 / 解包
 *   CAN 载荷里必须固定字节序，否则两块板子对不上就是"玄学bug"。
 *   选小端是因为跟 STM32 的内存布局一致，union 强转不会错。
 * -------------------------------------------------------------------------*/
void CAN_PROTO_PutI16(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

void CAN_PROTO_PutI32(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int16_t CAN_PROTO_GetI16(const uint8_t *p)
{
    uint16_t u = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return (int16_t)u;
}

int32_t CAN_PROTO_GetI32(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0]
               | ((uint32_t)p[1] << 8)
               | ((uint32_t)p[2] << 16)
               | ((uint32_t)p[3] << 24);
    return (int32_t)u;
}

/* ----------------------------------------------------------------------------
 * 初始化
 * -------------------------------------------------------------------------*/
void CAN_PROTO_Init(uint8_t node_id)
{
    s_node_id = (uint8_t)(node_id & 0x0FU);
    memset(&s_stats, 0, sizeof(s_stats));
    s_tx_seq      = 0;
    s_last_rx_seq = 0;
    s_has_rx      = 0;
    s_handler     = 0;
}

void CAN_PROTO_SetHandler(CAN_PROTO_Handler_t cb)
{
    s_handler = cb;
}

const CAN_PROTO_Stats_t *CAN_PROTO_GetStats(void)
{
    return &s_stats;
}

/* ----------------------------------------------------------------------------
 * 组包并发送
 *
 *   返回 HAL 状态码（0 = HAL_OK）。
 *   SendCmd 走 0x100+node（命令），SendRsp 走 0x200+node（遥测/应答）。
 *
 *   ⚠️ 载荷从 [3] 开始放，CRC 固定占 [7] 且覆盖 [0]~[6]。
 *      老版本把长度单独放在 [3]、载荷从 [4] 开始，结果 len=4 时 memcpy 会写到 [7]，
 *      紧接着的 f[7]=CRC 把第 4 个载荷字节冲掉 —— 回归测试抓出来的真 bug。
 * -------------------------------------------------------------------------*/
static int8_t proto_send(uint32_t id, uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t f[CAN_PROTO_FRAME_LEN];
    int8_t  st;

    if (len > CAN_PROTO_MAX_PAYLOAD)
    {
        len = CAN_PROTO_MAX_PAYLOAD;
    }

    memset(f, 0, sizeof(f));
    f[0] = (uint8_t)(CAN_PROTO_SOF_BASE | len);   /* 帧头兼长度 */
    f[1] = s_tx_seq;
    f[2] = cmd;
    if (len > 0U && payload != 0)
    {
        memcpy(&f[3], payload, len);
    }
    f[7] = CAN_PROTO_Crc8(f, 7U);       /* 覆盖 [0]~[6] */

    st = CAN_BSP_SendStd(id, f, CAN_PROTO_FRAME_LEN);

    if (st == 0)                        /* HAL_OK */
    {
        s_stats.tx_frames++;
        s_tx_seq++;
    }
    else
    {
        s_stats.tx_fail++;
    }
    return st;
}

int8_t CAN_PROTO_SendCmd(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    return proto_send(0x100U + s_node_id, cmd, payload, len);
}

int8_t CAN_PROTO_SendRsp(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    return proto_send(0x200U + s_node_id, cmd, payload, len);
}

/* ----------------------------------------------------------------------------
 * 收包 + 校验 + 分发。主循环里反复调用。
 *
 *   校验顺序是按"最可能失败且最便宜"的排前面：
 *     帧头 → 长度 → CRC。任何一步不过，计数 +1 然后丢掉这一帧，
 *     绝不让坏帧污染上层。
 * -------------------------------------------------------------------------*/
void CAN_PROTO_Poll(void)
{
    CAN_BSP_Frame_t  raw;
    CAN_PROTO_Msg_t  msg;
    uint8_t          plen;

    while (CAN_BSP_ReadFrame(&raw))
    {
        s_stats.rx_frames++;

        /* 只认 0x1xx / 0x2xx 这两个段的 ID */
        if ((raw.id & 0xF00U) != 0x100U && (raw.id & 0xF00U) != 0x200U)
        {
            continue;
        }

        /* CAN 帧的 DLC 可能 <8，本协议要求恒定 8 字节 */
        if (raw.len != CAN_PROTO_FRAME_LEN)
        {
            s_stats.rx_bad_sof++;
            continue;
        }

        /* ① 帧头：高 5 位必须是 0xA0 */
        if ((raw.data[0] & CAN_PROTO_SOF_MASK) != CAN_PROTO_SOF_BASE)
        {
            s_stats.rx_bad_sof++;
            continue;
        }

        plen = (uint8_t)(raw.data[0] & 0x07U);

        /* ② 载荷长度：低 3 位必须 ≤ 4 */
        if (plen > CAN_PROTO_MAX_PAYLOAD)
        {
            s_stats.rx_bad_len++;
            continue;
        }

        /* ③ CRC（覆盖前 7 字节，第 8 字节是校验值本身）*/
        if (CAN_PROTO_Crc8(raw.data, 7U) != raw.data[7])
        {
            s_stats.rx_bad_crc++;
            continue;
        }

        /* ④ 序号连续性：不等于"上一个+1"就说明中间丢了帧 */
        if (s_has_rx && raw.data[1] != (uint8_t)(s_last_rx_seq + 1U))
        {
            s_stats.rx_seq_gap++;
        }
        s_last_rx_seq = raw.data[1];
        s_has_rx = 1;

        /* ⑤ 通过：拷成干净结构交给应用层 */
        msg.cmd  = raw.data[2];
        msg.seq  = raw.data[1];
        msg.len  = plen;
        memset(msg.payload, 0, sizeof(msg.payload));
        if (plen > 0U)
        {
            memcpy(msg.payload, &raw.data[3], plen);
        }

        s_stats.rx_ok++;

        if (s_handler != 0)
        {
            s_handler(&msg);
        }
    }
}
