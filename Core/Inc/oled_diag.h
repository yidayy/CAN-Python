/**
  ******************************************************************************
  * @file    oled_diag.h
  * @brief   OLED 硬件自检（上电跑一次，串口打印结论）
  ******************************************************************************
  */

#ifndef __OLED_DIAG_H
#define __OLED_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 自检结果。放在 RAM 里一份，不接串口也能用 ST-Link 读出来：
 *   cube programmer -c port=SWD -r <g_oled_diag 地址> 24 dump.bin
 * magic 应为 0x4F4C4544，否则说明自检没跑到。 */
typedef struct {
    uint32_t magic;        /* 0x4F4C4544 = "OLED"，有值即代表自检跑完 */
    uint8_t  pd_scl;       /* 内部下拉时 PB8 的电平：1 = 外部有上拉 */
    uint8_t  pd_sda;       /* 内部下拉时 PB9 的电平 */
    uint8_t  pu_scl;       /* 内部上拉时 PB8 的电平：0 = 被外面拽低 */
    uint8_t  pu_sda;       /* 内部上拉时 PB9 的电平 */
    uint8_t  rise_scl;     /* 充电曲线：bit i = 第 i 个时间点 SCL 是否已到高电平 */
    uint8_t  rise_sda;     /* 充电曲线：bit i = 第 i 个时间点 SDA 是否已到高电平 */
    uint8_t  shorted;      /* 1 = SCL 与 SDA 短接 */
    uint8_t  ack_count;    /* 快速扫描应答地址个数 */
    uint8_t  first_addr;   /* 第一个应答的 7 位地址 */
    uint8_t  ctl_pd;       /* 对照组 PB0 的 IDR==ODR，正常 1 */
    uint8_t  ctl_pu;       /* 对照组 PB5 的 IDR==ODR，正常 1 */
    uint8_t  slow_ack;     /* 内部上拉+慢速扫描是否应答 */
    uint8_t  slow_addr;    /* 慢速扫描命中的地址 */
    uint8_t  fast_ack;     /* 内部上拉+快速扫描是否应答 */
    uint8_t  fast_addr;    /* 快速扫描命中的地址 */
    uint8_t  raw_ack;      /* 内部上拉+零延时（OLED.c 原样时序）是否应答 */
    uint8_t  raw_addr;     /* 零延时扫描命中的地址 */
    uint8_t  pd2_scl;      /* 扫描之后再测内部下拉：被"充起来"过说明在偷电 */
    uint8_t  pd2_sda;
} OledDiagResult_t;        /* 共 24 字节 */

extern volatile OledDiagResult_t g_oled_diag;

/** 跑一次自检。verbose = 1 打印详细结论，= 0 只更新 RAM。 */
void OLED_DIAG_Run(uint8_t verbose);

#ifdef __cplusplus
}
#endif

#endif /* __OLED_DIAG_H */
