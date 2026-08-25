/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32l4xx_hal.h"

#include "stm32l4xx_nucleo_32.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* USER CODE BEGIN Private defines -----------------------------------------------------------*/
#define in_X_OUT_Pin GPIO_PIN_0
#define in_X_OUT_GPIO_Port GPIOA
#define in_D_IN_Pin GPIO_PIN_1
#define in_D_IN_GPIO_Port GPIOA
#define VCP_TX_Pin GPIO_PIN_2
#define VCP_TX_GPIO_Port GPIOA
#define out_D_IN_Pin GPIO_PIN_0
#define out_D_IN_GPIO_Port GPIOB
#define in_SEL_1_Pin GPIO_PIN_1
#define in_SEL_1_GPIO_Port GPIOB
#define out_SEL_2_Pin GPIO_PIN_8
#define out_SEL_2_GPIO_Port GPIOA
#define in_BUSY_Pin GPIO_PIN_9
#define in_BUSY_GPIO_Port GPIOA
#define in_D_OUT_Pin GPIO_PIN_10
#define in_D_OUT_GPIO_Port GPIOA
#define out_SEL_1_Pin GPIO_PIN_11
#define out_SEL_1_GPIO_Port GPIOA
#define out_D_OUT_Pin GPIO_PIN_12
#define out_D_OUT_GPIO_Port GPIOA
#define SWDIO_Pin GPIO_PIN_13
#define SWDIO_GPIO_Port GPIOA
#define SWCLK_Pin GPIO_PIN_14
#define SWCLK_GPIO_Port GPIOA
#define VCP_RX_Pin GPIO_PIN_15
#define VCP_RX_GPIO_Port GPIOA
#define infoLed_Pin GPIO_PIN_3
#define infoLed_GPIO_Port GPIOB
#define user_BTN_Pin GPIO_PIN_4
#define user_BTN_GPIO_Port GPIOB
#define in_SEL_2_Pin GPIO_PIN_6
#define in_SEL_2_GPIO_Port GPIOB
#define out_ACK_Pin GPIO_PIN_7
#define out_ACK_GPIO_Port GPIOB
#define infoLed_Pin GPIO_PIN_3
#define infoLed_GPIO_Port GPIOB
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
