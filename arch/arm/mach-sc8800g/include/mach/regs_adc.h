#ifndef _REGS_ADC_H_
#define _REGS_ADC_H_

#include "bits.h"
#include <linux/types.h>
#include <mach/hardware.h>
#define ADC_REG_BASE                (SPRD_MISC_BASE+0x300)
#define ADC_CTRL                    (ADC_REG_BASE + 0x0000)
#define ADC_CS                      (ADC_REG_BASE + 0x0004)
#define ADC_TPC_CH_CTRL             (ADC_REG_BASE + 0x0008)
#define ADC_DAT                     (ADC_REG_BASE + 0x00C)
#define ADC_INT_EN                  (ADC_REG_BASE + 0x0010)
#define ADC_INT_CLR                 (ADC_REG_BASE + 0x0014)
#define ADC_INT_STAT                (ADC_REG_BASE + 0x0018)
#define ADC_INT_SRC                 (ADC_REG_BASE + 0x001C)

///ADC_CTRL
#define ADC_STATUS_BIT                      BIT_4
#define ADC_TPC_CH_ON_BIT                   BIT_2
#define SW_CH_ON_BIT                        BIT_1
#define ADC_EN_BIT                          BIT_0

//ADC_CS bit map
#define ADC_SCALE_BIT                       BIT_4
#define ADC_CS_BIT_MSK                      0x0F

#define ADC_SCALE_3V       0
#define ADC_SCALE_1V2      1

//ADC_TPC_CH_CTRL bit map
#define ADC_TPC_X_CH_MSK 0x0F
#define ADC_TPC_Y_CH_OFFSET 4
#define ADC_TPC_Y_CH_MSK (0x0F << ADC_TPC_Y_CH_OFFSET)

//ADC_INT_EN
#define ADC_IRQ_EN_BIT BIT_0
#define ADC_IRQ_CLR_BIT BIT_0
#define ADC_IRQ_RAW_BIT BIT_0

#define ADC_DATA_MSK 0x3FF

#define TPC_CHANNEL_X 2
#define TPC_CHANNEL_Y 3

#define ADC_CH_MAX_NUM 8

#endif //end of _REGS_ADC_H_