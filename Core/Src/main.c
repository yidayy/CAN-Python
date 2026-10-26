/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"
#include "tim.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "config.h"
#include "motor.h"
#include "display.h"
#include "usart.h"
#include "oled_diag.h"
#include "can_bsp.h"
#include "can_proto.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* 上电自检演示的档位序列：0 → +30 → +60 → +90 → -30 → -60 → -90 → 0 */
static const int16_t DemoSeq[] =
{
       0,
       MOTOR_DEMO_P30,
       MOTOR_DEMO_P60,
       MOTOR_DEMO_P90,
     - MOTOR_DEMO_P30,
     - MOTOR_DEMO_P60,
     - MOTOR_DEMO_P90,
       0
};
#define DEMO_SEQ_LEN   ((uint8_t)(sizeof(DemoSeq) / sizeof(DemoSeq[0])))

/* ---- CAN 接管开关 ---------------------------------------------------------
 * 1 = 本地演示序列在跑（上电默认，电机自己走 0→+30→...→-90→0）
 * 0 = CAN 总线接管（收到 CMD_SET_SPEED / CMD_STOP 后置 0）
 * 为什么必须互斥：演示序列每 1.2s 会无条件改占空比，如果 CAN 刚下发的
 * 速度 1.2s 后就被 demo 覆盖，遥控就"时灵时不灵"，查起来像接触不良。 */
static uint8_t demo_enabled = 1U;

/* CAN 命令处理器：在 CAN_PROTO_Poll() 里被同步调用，必须快进快出 */
static void can_cmd_handler(const CAN_PROTO_Msg_t *msg)
{
    switch (msg->cmd)
    {
    case CMD_PING:                       /* 探活 → 回 RSP_PONG [版本, 节点号, 0] */
    {
        uint8_t p[3] = { CAN_PROTO_VERSION, (uint8_t)NODE_ID, 0U };
        (void)CAN_PROTO_SendRsp(RSP_PONG, p, 3U);
        break;
    }

    case CMD_SET_SPEED:                  /* int16 小端，千分比 -1000~+1000 */
    {
        int16_t duty = CAN_PROTO_GetI16(msg->payload);
        MOTOR_SetDuty(duty);
        demo_enabled = 0U;               /* CAN 接管，本地演示序列让位 */
        break;
    }

    case CMD_STOP:                       /* 急停（不受 stby 保护限制） */
        MOTOR_Stop();
        demo_enabled = 0U;
        break;

    case CMD_GET_STAT:                   /* 请求遥测 → 回 RSP_STAT */
    {
        uint8_t p[4];
        CAN_PROTO_PutI16(p, MOTOR_GetDuty());
        p[2] = (uint8_t)(CAN_PROTO_GetStats()->rx_ok & 0xFFU);
        p[3] = 0U;
        (void)CAN_PROTO_SendRsp(RSP_STAT, p, 4U);
        break;
    }

    default:                             /* 未知命令：静默丢弃（协议允许扩展） */
        break;
    }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM1_Init();
  /* USER CODE BEGIN 2 */

  /* 电机子系统初始化：先压方向脚 → 再拉高 STBY → 最后起 PWM + MOE → 占空比归零。
   * 顺序不能反，细节见 motor.c 的四条 ⭐ 注释。 */
  MOTOR_Init();

  /* OLED 显示层初始化（画出版本号和节点号）。
   * 必须在 MX_GPIO_Init() 之后 —— PB8/PB9 的开漏配置在那儿。 */
  DISPLAY_Init();

  /* ---- USART1：115200 8N1，PA9(TX) ---- */
  USART_Init();

  /* ⚠️ 必须：newlib 默认给 stdout 的是行缓冲，只有遇到 '\n' 才往 _write 里灌。
   * 而单片机的 '\n' 是 '\r''\n' 两个字节，行缓冲会把 '\r' 之后的半行憋住，
   * 表现为"串口助手隔一会儿才冒出一行"。设成无缓冲，printf 一个字节就发一个。 */
  (void)setvbuf(stdout, NULL, _IONBF, 0);

  /* ---- OLED 硬件自检：必须在 USART_Init() 和 setvbuf() 之后 ----
   * OLED.c 的软件 I2C 不读应答位，屏没接也当成功（静默失败）。
   * 这里补上"线上电平 + 互短 + I2C 应答扫描"三项判据，
   * 结果直接打到串口，用来看"屏到底有没有通电/回话"。 */
  OLED_DIAG_Run(1);

  /* banner + CSV 表头。
   * # 开头的行是给人看的，下面 "t_ms,duty_permille,dir" 那行开始才是给 Excel 的数据。
   * 导表方法：串口助手保存成 .txt，Excel 的"数据→自文本"里把分隔符选成逗号，
   * 起点选在 t_ms 那一行，上面的 # 行会作为说明文字被跳过。 */
  printf("# dianji v0.2 | NODE %u | SYSCLK 72M APB2 72M | PWM 20kHz ARR %u\r\n",
         NODE_ID, MOTOR_TIM1_ARR);
  printf("# USART1 PA9(TX) 115200 8N1  RX=PA10  接线: STM32 PA9 -> USB-TTL 的 RX\r\n");
  printf("# duty_permille: 1000 = 100%%占空比;  dir: P=正转 R=反转 S=停止\r\n");
  printf("t_ms,duty_permille,dir\r\n");

  /* ---- CAN：PA11=RX / PA12=TX，500kbps，滤波器全通 ----
   * LOOPBACK=1 时自收自发（不需要 TJA1050），用来验证控制器+协议栈；
   * TJA1050 到货后把 can_bsp.h 里 CAN_BSP_LOOPBACK 改 0 上真总线。 */
  CAN_BSP_Init();
  CAN_PROTO_Init((uint8_t)NODE_ID);
  CAN_PROTO_SetHandler(can_cmd_handler);
  printf("# CAN ready: LOOPBACK=%u, 500kbps, ID cmd=0x%X rsp=0x%X\r\n",
         CAN_BSP_LOOPBACK, (unsigned)(0x100U + NODE_ID), (unsigned)(0x200U + NODE_ID));

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    /* ---- 上电自检演示：每个 MOTOR_DEMO_STEP_MS 走一档 ----
     * demo_enabled=0（CAN 接管）后此序列冻结，duty 完全由 CAN 命令决定 */
    static int16_t  demo_duty = 0;
    static uint8_t  demo_idx  = 0;
    static uint32_t demo_last = 0;

    if (demo_enabled && (HAL_GetTick() - demo_last >= MOTOR_DEMO_STEP_MS))
    {
      demo_last = HAL_GetTick();

      demo_duty = DemoSeq[demo_idx];
      MOTOR_SetDuty(demo_duty);

      demo_idx = (uint8_t)((demo_idx + 1U) % DEMO_SEQ_LEN);

      /* LED 每档翻一次 = 心跳，证明主循环还在转、没有跑飞 */
      HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
    }

    /* ---- CAN 收包：取帧 → 校验 → 分发到 can_cmd_handler ----
     * LOOPBACK 模式下自己发的 PING 会绕回自己，handler 回 PONG 也绕回自己，
     * 但 RSP_* 不会被再次应答（协议设计），所以不会乒乓。 */
    CAN_PROTO_Poll();

#if CAN_BSP_LOOPBACK
    /* ---- LOOPBACK 自测：每 1s 发一次 PING ----
     * 绕回自己 → handler 回 PONG → PONG 也绕回自己被统计。
     * 预期串口统计：每秒 tx+1、rx/ok+2、bad_* 全 0 = bxCAN+协议栈全链路通。
     * （gap 非零是协议已知限制②：PING 和 PONG 两条 seq 流共用一个序号桶，
     *   不是丢帧。真机上 Vera 不影响判据。） */
    static uint32_t can_selftest_last = 0;
    if (HAL_GetTick() - can_selftest_last >= 1000U)
    {
      can_selftest_last = HAL_GetTick();
      (void)CAN_PROTO_SendCmd(CMD_PING, 0, 0);
    }
#endif

    /* OLED 硬件自检已不再放进主循环。
     * 2026-10-24 屏修好（根因是面包板供电轨接触不良），自检只在开机跑一次。
     * 它每秒切一次引脚模式会把从机卡在半个字节里，长期跑会把屏刷花。 */

    /* 刷新 OLED（内部 200ms 节流，每次主循环都调没关系）。
     * 电机没到的时候，这块屏就是唯一的"仪表盘"。 */
    DISPLAY_Refresh();

    /* ---- 串口遥测（v0.2）----
     * 为什么不在定时器里发：主循环一次 tick 只做"采样 + 计算 + 下发"，
     * 往串口里塞 30 字节（约 2.6ms @115200）会挤掉控制周期。所以分两路：
     *   ① 事件驱动：duty 变了才打 —— Excel 里能看到干净的阶跃点
     *   ② 1 秒心跳：duty 不变也打一行，证明设备没死 + 带 CAN 统计
     * 打的是 MOTOR_GetDuty()（真实下发值），不是 demo_duty ——
     * CAN 接管后 duty 的变化不经过演示序列，打 demo_duty 会说谎。 */
    static int16_t  log_prev_duty = 0x7FFF;   /* 0x7FFF = 不可能的值，保证第一行必打 */
    static uint32_t log_beat_last = 0;
    uint32_t now_ms = HAL_GetTick();
    int16_t  real_duty = MOTOR_GetDuty();

    if (real_duty != log_prev_duty)
    {
      log_prev_duty = real_duty;
      printf("%lu,%d,%c\r\n", now_ms, (int)real_duty,
             (real_duty > 0) ? 'P' : ((real_duty < 0) ? 'R' : 'S'));
    }
    else if (now_ms - log_beat_last >= 1000U)
    {
      log_beat_last = now_ms;
      printf("%lu,%d,%c\r\n", now_ms, (int)real_duty,
             (real_duty > 0) ? 'P' : ((real_duty < 0) ? 'R' : 'S'));

      /* CAN 统计（# 开头，Excel 导入时当注释跳过）。
       * LOOPBACK 验收看这行：tx 涨、rx_ok 涨、bad 全 0 = 链路全通。 */
      {
        const CAN_PROTO_Stats_t *st = CAN_PROTO_GetStats();
        printf("# CAN tx=%lu rx=%lu ok=%lu sof=%lu len=%lu crc=%lu gap=%lu fail=%lu\r\n",
               (unsigned long)st->tx_frames, (unsigned long)st->rx_frames,
               (unsigned long)st->rx_ok, (unsigned long)st->rx_bad_sof,
               (unsigned long)st->rx_bad_len, (unsigned long)st->rx_bad_crc,
               (unsigned long)st->rx_seq_gap, (unsigned long)st->tx_fail);
      }
    }

    /* 收命令 + 清 ORE。RX 还没接东西，这里只是保证标志位不卡住 */
    USART_PollTask();

    /* USER CODE END 3 */
  }
  /* USER CODE END WHILE */
}

/**
  * @brief System Clock Configuration
  * @retval None
*/
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_AFIO_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();

  /** Initializes the RCC Oscillators according to the specified parameters
  * RCC_OscInitStruct.OscillatorType.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;   /* 36 MHz */
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;   /* 72 MHz → TIM1 */

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
