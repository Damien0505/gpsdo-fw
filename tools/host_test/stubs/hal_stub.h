// Minimal fake of the STM32 HAL and LCD library, so that the firmware sources can run on a PC.
#ifndef _HAL_STUB_H_
#define _HAL_STUB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET, GPIO_PIN_SET } GPIO_PinState;
typedef struct { int unused; } GPIO_TypeDef;
extern GPIO_TypeDef stub_gpio;

#define ROTARY_PRESS_Pin            1
#define ROTARY_PRESS_GPIO_Port      (&stub_gpio)
#define PPS_OUTPUT_Pin              2
#define PPS_OUTPUT_GPIO_Port        (&stub_gpio)
#define LED1_Pin                    3
#define LED1_GPIO_Port              (&stub_gpio)
#define GPS_LOCK_OUTPUT_Pin         4
#define GPS_LOCK_OUTPUT_GPIO_Port   (&stub_gpio)
#define PPB_LOCK_OUTPUT_Pin         5
#define PPB_LOCK_OUTPUT_GPIO_Port   (&stub_gpio)

typedef struct { volatile uint32_t CNT, ARR, CCR1, CCR2, CCR3; } TIM_TypeDef;
extern TIM_TypeDef stub_tim1, stub_tim2, stub_tim3;
#define TIM1 (&stub_tim1)
#define TIM2 (&stub_tim2)
#define TIM3 (&stub_tim3)
typedef struct { int Channel; } TIM_HandleTypeDef;
extern TIM_HandleTypeDef htim1, htim2, htim3;
#define HAL_TIM_ACTIVE_CHANNEL_1 1
#define TIM_CHANNEL_1   0
#define TIM_CHANNEL_2   4
#define TIM_CHANNEL_3   8
#define TIM_CHANNEL_ALL 60
uint32_t HAL_TIM_ReadCapturedValue(TIM_HandleTypeDef* htim, uint32_t channel);
HAL_StatusTypeDef HAL_TIM_Base_Start_IT(TIM_HandleTypeDef* htim);
HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* htim, uint32_t channel);
HAL_StatusTypeDef HAL_TIM_IC_Start_IT(TIM_HandleTypeDef* htim, uint32_t channel);

typedef enum { HAL_UART_STATE_RESET, HAL_UART_STATE_READY, HAL_UART_STATE_BUSY } HAL_UART_StateTypeDef;
#define UART_WORDLENGTH_8B      0
#define UART_STOPBITS_1         0
#define UART_PARITY_NONE        0
#define UART_MODE_TX_RX         0
#define UART_HWCONTROL_NONE     0
#define UART_OVERSAMPLING_16    0
typedef struct { int unused; } USART_TypeDef;
extern USART_TypeDef stub_usart2, stub_usart3;
#define USART2 (&stub_usart2)
#define USART3 (&stub_usart3)
typedef struct { uint32_t BaudRate, WordLength, StopBits, Parity, Mode, HwFlowCtl, OverSampling; } UART_InitTypeDef;
typedef struct { volatile uint32_t counter; } DMA_HandleTypeDef;
#define __HAL_DMA_GET_COUNTER(h) ((h)->counter)
typedef struct {
    USART_TypeDef*                 Instance;
    UART_InitTypeDef               Init;
    DMA_HandleTypeDef*             hdmarx;
    volatile HAL_UART_StateTypeDef gState;
    volatile HAL_UART_StateTypeDef RxState;
} UART_HandleTypeDef;
extern UART_HandleTypeDef huart2, huart3;
HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef* huart);
HAL_StatusTypeDef HAL_UART_DeInit(UART_HandleTypeDef* huart);
HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef* huart, const uint8_t* data, uint16_t size);
HAL_StatusTypeDef HAL_UART_Receive_DMA(UART_HandleTypeDef* huart, uint8_t* data, uint16_t size);
void HAL_UART_RxCpltCallback(UART_HandleTypeDef* huart);

uint32_t HAL_GetTick(void);
void     HAL_Delay(uint32_t ms);
uint32_t HAL_RCC_GetHCLKFreq(void);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef* port, uint16_t pin);
void     HAL_GPIO_WritePin(GPIO_TypeDef* port, uint16_t pin, GPIO_PinState state);
void     Error_Handler(void);
#define __disable_irq()
#define __enable_irq()

// LCD
void LCD_Init(void);
void LCD_Clear(void);
void LCD_Puts(uint8_t x, uint8_t y, const char* text);
void LCD_PutCustom(uint8_t x, uint8_t y, uint8_t code);
void LCD_CreateChar(uint8_t code, uint8_t* data);

// EEPROM library
bool EE_Init(void* data, uint32_t size);
void EE_Read(void);
bool EE_Write(void);

#endif
