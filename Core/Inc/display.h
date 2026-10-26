#ifndef __DISPLAY_H
#define __DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * display.h —— OLED 显示层（应用层）
 *
 * 分层的意义：main.c 只调 DISPLAY_Refresh()，不需要知道 OLED 有几行几列、
 * I2C 地址是多少。以后要换屏（比如换 0.96" 换中段、或者干脆不接屏），
 * 只改这一个文件，main.c 一行不动。
 * ==========================================================================*/

void DISPLAY_Init(void);

/**
 * @brief  刷新一次 OLED（内部自带 200ms 节流，可以每个主循环都调）
 * @note   ⭐ 节流是必须的：OLED 是软件 I2C，全屏刷一次要几毫秒，
 *         不节流的话会拖慢主循环节拍。以后上了 PID，节流就更不能少。
 */
void DISPLAY_Refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* __DISPLAY_H */
