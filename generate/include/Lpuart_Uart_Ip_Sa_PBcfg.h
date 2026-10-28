/*==================================================================================================
*   Project              : RTD AUTOSAR 4.9
*   Platform             : CORTEXM
*   Peripheral           : FLEXIO_UART
*   Dependencies         : 
*
*   Autosar Version      : 4.9.0
*   Autosar Revision     : ASR_REL_4_9_REV_0000
*   Autosar Conf.Variant :
*   SW Version           : 7.0.1
*   Build Version        : S32K3_RTD_7_0_1_D2602_ASR_REL_4_9_REV_0000_20260206
*
*   Copyright 2020 - 2026 NXP
*
*   NXP Confidential and Proprietary. This software is owned or controlled by NXP and may only be 
*   used strictly in accordance with the applicable license terms. By expressly 
*   accepting such terms or by downloading, installing, activating and/or otherwise 
*   using the software, you are agreeing that you have read, and that you agree to 
*   comply with and are bound by, such license terms. If you do not agree to be 
*   bound by the applicable license terms, then you may not retain, install,
*   activate or otherwise use the software.
==================================================================================================*/


#ifndef LPUART_UART_IP_SA_PBCFG_H
#define LPUART_UART_IP_SA_PBCFG_H


/**
*   @file Lpuart_Uart_Ip_PBcfg.h
*   @defgroup lpuart_uart_ip Lpuart UART IPL
*   @addtogroup  lpuart_uart_ip Lpuart UART IPL
*   @{
*/


#ifdef __cplusplus
extern "C"{
#endif


/*==================================================================================================
                                         INCLUDE FILES
 1) system and project includes
 2) needed interfaces from external units
 3) internal and external interfaces from this unit
==================================================================================================*/
#include "Lpuart_Uart_Ip_Types.h"

/*==================================================================================================
*                              SOURCE FILE VERSION INFORMATION
==================================================================================================*/
#define LPUART_UART_IP_SA_PBCFG_VENDOR_ID                     43
#define LPUART_UART_IP_SA_PBCFG_AR_RELEASE_MAJOR_VERSION      4
#define LPUART_UART_IP_SA_PBCFG_AR_RELEASE_MINOR_VERSION      9
#define LPUART_UART_IP_SA_PBCFG_AR_RELEASE_REVISION_VERSION   0
#define LPUART_UART_IP_SA_PBCFG_SW_MAJOR_VERSION              7
#define LPUART_UART_IP_SA_PBCFG_SW_MINOR_VERSION              0
#define LPUART_UART_IP_SA_PBCFG_SW_PATCH_VERSION              1


/*==================================================================================================
                                      FILE VERSION CHECKS
==================================================================================================*/
/* Checks against Lpuart_Uart_Ip_Types.h */
#if (LPUART_UART_IP_SA_PBCFG_VENDOR_ID != LPUART_UART_IP_TYPES_VENDOR_ID)
    #error "Lpuart_Uart_Ip_Sa_PBCfg.h and Lpuart_Uart_Ip_Types.h have different vendor ids"
#endif
#if ((LPUART_UART_IP_SA_PBCFG_AR_RELEASE_MAJOR_VERSION   != LPUART_UART_IP_TYPES_AR_RELEASE_MAJOR_VERSION)|| \
     (LPUART_UART_IP_SA_PBCFG_AR_RELEASE_MINOR_VERSION   != LPUART_UART_IP_TYPES_AR_RELEASE_MINOR_VERSION)|| \
     (LPUART_UART_IP_SA_PBCFG_AR_RELEASE_REVISION_VERSION!= LPUART_UART_IP_TYPES_AR_RELEASE_REVISION_VERSION) \
    )
    #error "AUTOSAR Version Numbers of Lpuart_Uart_Ip_Sa_PBCfg.h and Lpuart_Uart_Ip_Types.h are different"
#endif
#if ((LPUART_UART_IP_SA_PBCFG_SW_MAJOR_VERSION!= LPUART_UART_IP_TYPES_SW_MAJOR_VERSION)|| \
     (LPUART_UART_IP_SA_PBCFG_SW_MINOR_VERSION!= LPUART_UART_IP_TYPES_SW_MINOR_VERSION)|| \
     (LPUART_UART_IP_SA_PBCFG_SW_PATCH_VERSION!= LPUART_UART_IP_TYPES_SW_PATCH_VERSION) \
    )
    #error "Software Version Numbers of Lpuart_Uart_Ip_Sa_PBCfg.h and Lpuart_Uart_Ip_Types.h are different"
#endif

/*==================================================================================================
*                                            CONSTANTS
==================================================================================================*/

/*==================================================================================================
*                                       DEFINES AND MACROS
==================================================================================================*/

/**
* @brief          Link Uart channels symbolic names with Uart hardware channel IDs.
* @details        Link Uart channels symbolic names with Uart hardware channel IDs.
*
* @api
*/

#ifndef LPUART_UART_IP_INSTANCE_USING_6
    #define LPUART_UART_IP_INSTANCE_USING_6   6U
#endif

/*==================================================================================================
*                                              ENUMS
==================================================================================================*/

/*==================================================================================================
*                                  STRUCTURES AND OTHER TYPEDEFS
==================================================================================================*/

/*==================================================================================================
*                                  GLOBAL VARIABLE DECLARATIONS
==================================================================================================*/

/*==================================================================================================
*                                       FUNCTION PROTOTYPES
==================================================================================================*/

/**
* @brief    Declare callback functions if any .
*/
#define LPUART_UART_IP_SA_CONFIG_PB \
extern const Lpuart_Uart_Ip_UserConfigType Lpuart_Uart_Ip_xHwConfigPB_6;

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* LPUART_UART_IP_SA_PBCFG_H */

