#include "stm32f0xx_hal.h"
#include "usbd_def.h"
#include "usbd_desc.h"
#include "usbd_core.h"
#include "usbd_gs_can.h"
#include <queue.h>
#include <gs_usb.h>
#include <can.h>

#define CAN_QUEUE_SIZE 32

void SystemClock_Config(void);
static void MX_GPIO_Init(void);


CAN_HandleTypeDef hCAN;
USBD_HandleTypeDef hUSB;

int main(void)
{

	HAL_Init();
	SystemClock_Config();
	MX_GPIO_Init();
	can_init(&hCAN, CAN);

	queue_t *q_frame_pool = queue_create(CAN_QUEUE_SIZE);
	queue_t *q_from_host  = queue_create(CAN_QUEUE_SIZE);
	queue_t *q_to_host    = queue_create(CAN_QUEUE_SIZE);

	struct gs_host_frame *msgbuf = calloc(CAN_QUEUE_SIZE, sizeof(struct gs_host_frame));
	for (unsigned i=0; i<CAN_QUEUE_SIZE; i++) {
		queue_push_back(q_frame_pool, &msgbuf[i]);
	}

	USBD_Init(&hUSB, &FS_Desc, DEVICE_FS);
	USBD_RegisterClass(&hUSB, &USBD_GS_CAN);
	USBD_GS_CAN_Init(&hUSB, q_frame_pool, q_from_host);
	USBD_GS_CAN_SetChannel(&hUSB, 0, &hCAN);
	USBD_Start(&hUSB);


	HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(CAN_S_GPIO_Port, CAN_S_Pin, GPIO_PIN_RESET);

	uint32_t t_next_send = 100;

	while (1) {


		if (queue_size(q_from_host)>0) { // send can message from host
			CanTxMsgTypeDef tx_msg;
			struct gs_host_frame *frame = queue_pop_front(q_from_host);

			if (frame != 0) {
				if ((frame->can_id & CAN_EFF_FLAG) != 0) {
					tx_msg.IDE = CAN_ID_EXT;
					tx_msg.ExtId = frame->can_id & 0x1FFFFFFF;
				} else {
					tx_msg.IDE = CAN_ID_STD;
					tx_msg.StdId = frame->can_id & 0x7FF;
				}

				if ((frame->can_id & CAN_RTR_FLAG) != 0) {
					tx_msg.RTR = CAN_RTR_REMOTE;
				}

				tx_msg.DLC = MIN(8,frame->can_dlc);
				memcpy(tx_msg.Data, frame->data, tx_msg.DLC);

				if (can_send(&hCAN, &tx_msg, 10)) {
					queue_push_back(q_to_host, frame); // send echo frame back to host
				} else {
					queue_push_front(q_from_host, frame); // retry later
				}
			}
		}

		if (USBD_GS_CAN_TxReady(&hUSB)) {
			if (queue_size(q_to_host)>0) { // send received message or echo message to host
				struct gs_host_frame *frame = queue_pop_front(q_to_host);

				if (USBD_GS_CAN_Transmit(&hUSB, (uint8_t*)frame, sizeof(struct gs_host_frame))==USBD_OK) {
					queue_push_back(q_frame_pool, frame);
				} else {
					queue_push_front(q_to_host, frame);
				}
			}
		}

		if (can_is_rx_pending(&hCAN)) {
			struct gs_host_frame *frame = queue_pop_front(q_frame_pool);
			if (frame) {
				CanRxMsgTypeDef rx_msg;
				if (can_receive(&hCAN, &rx_msg, 0)) {

					frame->echo_id = 0xFFFFFFFF; // not a echo frame

					frame->can_dlc = MIN(8, rx_msg.DLC);
					frame->channel = 0;
					frame->flags = 0;
					frame->reserved = 0;

					if (rx_msg.IDE) {
						frame->can_id = rx_msg.ExtId | CAN_EFF_FLAG;
					} else {
						frame->can_id = rx_msg.StdId;
					}

					if (rx_msg.RTR) {
						frame->can_id |= CAN_RTR_FLAG;
					}

					memcpy(frame->data, rx_msg.Data, frame->can_dlc);

					queue_push_back(q_to_host, frame);


				} else {
					queue_push_back(q_frame_pool, frame);
				}
			}
		}

		if (HAL_GetTick() >= t_next_send) {
			t_next_send = HAL_GetTick() + 500;
			//USBD_GS_CAN_SendFrameToHost(&hUSB, -1, 0x100, 0, 0, 0, 0);
			HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
			HAL_GPIO_TogglePin(LED1_GPIO_Port, LED2_Pin);
		}

	}

}

void SystemClock_Config(void)
{

  RCC_OscInitTypeDef RCC_OscInitStruct;
  RCC_ClkInitTypeDef RCC_ClkInitStruct;
  RCC_PeriphCLKInitTypeDef PeriphClkInit;
  RCC_CRSInitTypeDef RCC_CRSInitStruct;

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI48;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1);

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USB;
  PeriphClkInit.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
  HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit);

  __HAL_RCC_CRS_CLK_ENABLE();

  RCC_CRSInitStruct.Prescaler = RCC_CRS_SYNC_DIV1;
  RCC_CRSInitStruct.Source = RCC_CRS_SYNC_SOURCE_USB;
  RCC_CRSInitStruct.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
  RCC_CRSInitStruct.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000,1000);
  RCC_CRSInitStruct.ErrorLimitValue = 34;
  RCC_CRSInitStruct.HSI48CalibrationValue = 32;
  HAL_RCCEx_CRSConfig(&RCC_CRSInitStruct);

  HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq()/1000);

  HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);

  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}

void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct;

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(CAN_S_GPIO_Port, CAN_S_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, LED1_Pin|LED2_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin : CAN_S_Pin */
  GPIO_InitStruct.Pin = CAN_S_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(CAN_S_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LED1_Pin LED2_Pin */
  GPIO_InitStruct.Pin = LED1_Pin|LED2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

}