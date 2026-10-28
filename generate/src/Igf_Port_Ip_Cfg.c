/*==================================================================================================
* Project : RTD AUTOSAR 4.9
* Platform : CORTEXM
* Peripheral : S32K3XX
* Dependencies : none
*
* Autosar Version : 4.9.0
* Autosar Revision : ASR_REL_4_9_REV_0000
* Autosar Conf.Variant :
* SW Version : 7.0.1
* Build Version : S32K3_RTD_7_0_1_D2602_ASR_REL_4_9_REV_0000_20260206
*
* Copyright 2020 - 2026 NXP
*
* NXP Confidential and Proprietary. This software is owned or controlled by NXP and may only be
*   used strictly in accordance with the applicable license terms. By expressly
*   accepting such terms or by downloading, installing, activating and/or otherwise
*   using the software, you are agreeing that you have read, and that you agree to
*   comply with and are bound by, such license terms. If you do not agree to be
*   bound by the applicable license terms, then you may not retain, install,
*   activate or otherwise use the software.
==================================================================================================*/

/**
*   @file    Igf_Port_Ip_Cfg.c
*
*   @addtogroup Port_CFG
*   @{
*/

#ifdef __cplusplus
extern "C"{
#endif

/*==================================================================================================
*                                          INCLUDE FILES
* 1) system and project includes
* 2) needed interfaces from external units
* 3) internal and external interfaces from this unit
==================================================================================================*/
#include "Igf_Port_Ip.h"

/*==================================================================================================
*                                 SOURCE FILE VERSION INFORMATION
==================================================================================================*/
#define IGF_PORT_IP_VENDOR_ID_CFG_C                       43
#define IGF_PORT_IP_AR_RELEASE_MAJOR_VERSION_CFG_C        4
#define IGF_PORT_IP_AR_RELEASE_MINOR_VERSION_CFG_C        9
#define IGF_PORT_IP_AR_RELEASE_REVISION_VERSION_CFG_C     0
#define IGF_PORT_IP_SW_MAJOR_VERSION_CFG_C                7
#define IGF_PORT_IP_SW_MINOR_VERSION_CFG_C                0
#define IGF_PORT_IP_SW_PATCH_VERSION_CFG_C                1

/*==================================================================================================
*                                       FILE VERSION CHECKS
==================================================================================================*/
/* Check if the files Igf_Port_Ip_Cfg.c and Igf_Port_Ip.h are of the same version */
#if (IGF_PORT_IP_VENDOR_ID_CFG_C != IGF_PORT_IP_VENDOR_ID_H)
    #error "Igf_Port_Ip_Cfg.c and Igf_Port_Ip.h have different vendor ids"
#endif
/* Check if Igf_Port_Ip_Cfg.c and Igf_Port_Ip.h are of the same Autosar version */
#if ((IGF_PORT_IP_AR_RELEASE_MAJOR_VERSION_CFG_C    != IGF_PORT_IP_AR_RELEASE_MAJOR_VERSION_H) || \
     (IGF_PORT_IP_AR_RELEASE_MINOR_VERSION_CFG_C    != IGF_PORT_IP_AR_RELEASE_MINOR_VERSION_H) || \
     (IGF_PORT_IP_AR_RELEASE_REVISION_VERSION_CFG_C != IGF_PORT_IP_AR_RELEASE_REVISION_VERSION_CFG_H) \
    )
    #error "AutoSar Version Numbers of Igf_Port_Ip_Cfg.c and Igf_Port_Ip.h are different"
#endif
/* Check if Igf_Port_Ip_Cfg.c and Igf_Port_Ip.h are of the same Software version */
#if ((IGF_PORT_IP_SW_MAJOR_VERSION_CFG_C != IGF_PORT_IP_SW_MAJOR_VERSION_H) || \
     (IGF_PORT_IP_SW_MINOR_VERSION_CFG_C != IGF_PORT_IP_SW_MINOR_VERSION_H) || \
     (IGF_PORT_IP_SW_PATCH_VERSION_CFG_C != IGF_PORT_IP_SW_PATCH_VERSION_H)    \
    )
    #error "Software Version Numbers of Igf_Port_Ip_Cfg.c and Igf_Port_Ip.h are different"
#endif

/*==================================================================================================
*                           LOCAL TYPEDEFS (STRUCTURES, UNIONS, ENUMS)
==================================================================================================*/

/*==================================================================================================
*                                          LOCAL MACROS
==================================================================================================*/

/*==================================================================================================
*                                         LOCAL CONSTANTS
==================================================================================================*/

/*==================================================================================================
*                                         LOCAL VARIABLES
==================================================================================================*/

/*==================================================================================================
*                                        GLOBAL CONSTANTS
==================================================================================================*/

/*==================================================================================================
*                                        GLOBAL VARIABLES
==================================================================================================*/

/*==================================================================================================
*                                        GLOBAL CONSTANTS
==================================================================================================*/

/*==================================================================================================
*                                        GLOBAL VARIABLES
==================================================================================================*/

/*==================================================================================================
*                                    LOCAL FUNCTION PROTOTYPES
==================================================================================================*/

/*==================================================================================================
*                                         LOCAL FUNCTIONS
==================================================================================================*/

/*==================================================================================================
*                                        GLOBAL FUNCTIONS
==================================================================================================*/

#ifdef __cplusplus
}
#endif

/** @} */
