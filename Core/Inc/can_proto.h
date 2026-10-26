/* ============================================================================
 * can_proto.h   应用层通信协议 v0.2（跑在 can_bsp 之上）
 * ==========================================================================*/
#ifndef CAN_PROTO_H
#define CAN_PROTO_H

#include <stdint.h>

/* ============================================================================
 * 为什么要有这一层？
 *   can_bsp 只会"收/发 8 字节裸帧"。裸帧的问题：
 *     ① 总线上可能混着别的设备的帧（或没配好时的乱帧）——无法区分是不是自己的
 *     ② 发多个帧时对端不知道有没有丢——无法确认
 *     ③ 8 字节怎么解释全靠口头约定——无法扩展
 *   所以需要：帧头 + 序号 + 命令码 + 长度 + CRC，也就是一个最小协议栈。
 *
 * ---- 帧格式（固定 8 字节）---------------------------------------------------
 *
 *   字节  0        1     2      3    4    5    6    7
 *         0xA0|L   seq   cmd    p0   p1   p2   p3   CRC8
 *         └─ 帧头兼长度 ┘
 *
 *   [0]  0xA0|L   L = 载荷有效长度 0~4，所以这个字节只可能是
 *                 0xA0 / 0xA1 / 0xA2 / 0xA3 / 0xA4 五个值之一。
 *                 ⚠️ 把长度塞进帧头高 5 位固定的魔法字节里，是无奈但正确的选择：
 *                    8 字节帧 = 4 字节头 + 1 字节 CRC，只剩 3 字节载荷，
 *                    int32 位置量（v0.3 要用）根本塞不下。
 *                    省下这一个字节，载荷才能到 4 字节，而且校验反而更严——
 *                    合法的 [0] 只有 5 个取值，比单一个 0xA5 更容易识破乱帧。
 *   [1]  seq      发送序号 0~255 循环，收端用来数丢帧
 *   [2]  cmd      命令码（见下面 CMD_ / RSP_）
 *   [3..6] p0..p3 载荷，小端序，右侧补 0；只有前 L 字节有效
 *   [7]  CRC8     CRC-8/SMBUS，覆盖 [0]~[6]
 *
 * ---- CAN ID 分配 ------------------------------------------------------------
 *   0x100 + NODE_ID   命令帧（A → B 下发控制指令）
 *   0x200 + NODE_ID   遥测帧 / 应答（B → A 上报状态）
 *   接收时不区分对端是谁，只要 ID 落在 0x1xx / 0x2xx 且帧头正确就认。
 *
 *   ⚠️ 只有 CMD_* 会被自动应答，RSP_* 不会 —— 否则两块板子互相回话会无限乒乓。
 *
 * ---- 已知限制（v0.3 再解决）-------------------------------------------------
 *   ① 单帧载荷上限 4 字节。一组 PID 参数（3×int16）就超了，到时候要上分段传输
 *      （ISO-TP 的思路：首帧带总长，后续帧带序号）。
 *   ② 序号跳号统计不按对端分桶，两块板子同时发时会互相误判成丢帧。
 * ==========================================================================*/

/* ---- 帧格式常量 ---- */
#define CAN_PROTO_SOF_BASE      0xA0U   /* 帧头高 5 位固定，低 3 位是长度 */
#define CAN_PROTO_SOF_MASK      0xF8U   /* 校验时用 base 比这个 */
#define CAN_PROTO_FRAME_LEN     8U
#define CAN_PROTO_MAX_PAYLOAD   4U

/* ---- 命令码 ---- */
#define CMD_PING         0x01U   /* 探活，无载荷        → 对端回 RSP_PONG        */
#define CMD_SET_SPEED    0x02U   /* int16 小端，速度给定                        */
#define CMD_STOP         0x03U   /* 立即停止                                        */
#define CMD_GET_STAT     0x04U   /* 请求遥测              → 对端回 RSP_STAT        */

#define RSP_PONG         0x81U   /* 载荷 [版本, 节点号, 0]                          */
#define RSP_STAT         0x82U   /* 载荷 [int16 速度, rx_ok, flags]                */

#define CAN_PROTO_VERSION        1U

/* ---- 统计 ---- */
typedef struct {
    uint32_t rx_frames;      /* 从驱动层取到的原始帧总数                  */
    uint32_t rx_ok;          /* 通过全部校验并被应用层接收                */
    uint32_t rx_bad_sof;     /* 帧头不对（不是本协议的帧 / 乱帧）          */
    uint32_t rx_bad_len;     /* 长度字段 > 4，非法                        */
    uint32_t rx_bad_crc;     /* CRC 校验失败                              */
    uint32_t rx_seq_gap;     /* 序号不连续 ⇒ 中间丢了帧                   */
    uint32_t tx_frames;      /* 本节点发出的协议帧                        */
    uint32_t tx_fail;        /* 发送失败（邮箱满 / 总线关闭）              */
} CAN_PROTO_Stats_t;

/* ---- 解出来的一个消息（给应用层用的干净结构）---- */
typedef struct {
    uint8_t cmd;
    uint8_t seq;
    uint8_t len;
    uint8_t payload[CAN_PROTO_MAX_PAYLOAD];
} CAN_PROTO_Msg_t;

/* 收到合法消息时的回调。在 CAN_PROTO_Poll() 里同步调用，不要在里面耗时 */
typedef void (*CAN_PROTO_Handler_t)(const CAN_PROTO_Msg_t *msg);

/* ---- 接口 ---- */
void     CAN_PROTO_Init(uint8_t node_id);
int8_t   CAN_PROTO_SendCmd(uint8_t cmd, const uint8_t *payload, uint8_t len); /* ID 0x100+node */
int8_t   CAN_PROTO_SendRsp(uint8_t cmd, const uint8_t *payload, uint8_t len); /* ID 0x200+node */
void     CAN_PROTO_Poll(void);
void     CAN_PROTO_SetHandler(CAN_PROTO_Handler_t cb);
const CAN_PROTO_Stats_t *CAN_PROTO_GetStats(void);

/* CRC-8/SMBUS（poly 0x07, init 0x00）。校验值：ASCII "123456789" → 0xF4 */
uint8_t  CAN_PROTO_Crc8(const uint8_t *data, uint8_t len);

/* 便捷打包：把 int16 / int32 按小端塞进 4 字节载荷 */
void     CAN_PROTO_PutI16(uint8_t *p, int16_t v);
void     CAN_PROTO_PutI32(uint8_t *p, int32_t v);
int16_t  CAN_PROTO_GetI16(const uint8_t *p);
int32_t  CAN_PROTO_GetI32(const uint8_t *p);

#endif /* CAN_PROTO_H */
