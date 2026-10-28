/*==================================================================================================
*   Project              : RTD AUTOSAR 4.9
*   Platform             : CORTEXM
*   Peripheral           : C40
*   Dependencies         : None
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

/**
 * @file       C40_Ip_Cfg.c
 *
 * @addtogroup C40_IP
 * implement   C40_Ip_Cfg.c_Artifact
 *
 * @{
 */

#ifdef __cplusplus
extern "C"{
#endif

/*==================================================================================================
*                                        INCLUDE FILES
* 1) system and project includes
* 2) needed interfaces from external units
* 3) internal and external interfaces from this unit
==================================================================================================*/
#include "C40_Ip_Cfg.h"

/*==================================================================================================
*                              SOURCE FILE VERSION INFORMATION
==================================================================================================*/
#define C40_IP_VENDOR_ID_CFG_C                          43
#define C40_IP_AR_RELEASE_MAJOR_VERSION_CFG_C           4
#define C40_IP_AR_RELEASE_MINOR_VERSION_CFG_C           9
#define C40_IP_AR_RELEASE_REVISION_VERSION_CFG_C        0
#define C40_IP_SW_MAJOR_VERSION_CFG_C                   7
#define C40_IP_SW_MINOR_VERSION_CFG_C                   0
#define C40_IP_SW_PATCH_VERSION_CFG_C                   1

/*==================================================================================================
*                                     FILE VERSION CHECKS
==================================================================================================*/
/* Check if current file and C40_Ip_Cfg.h header file are of the same vendor */
#if (C40_IP_VENDOR_ID_CFG_C != C40_IP_VENDOR_ID_CFG)
    #error "C40_Ip_Cfg.c and C40_Ip_Cfg.h have different vendor ids"
#endif
/* Check if current file and C40_Ip_Cfg.h header file are of the same Autosar version */
#if ((C40_IP_AR_RELEASE_MAJOR_VERSION_CFG_C    != C40_IP_AR_RELEASE_MAJOR_VERSION_CFG) || \
     (C40_IP_AR_RELEASE_MINOR_VERSION_CFG_C    != C40_IP_AR_RELEASE_MINOR_VERSION_CFG) || \
     (C40_IP_AR_RELEASE_REVISION_VERSION_CFG_C != C40_IP_AR_RELEASE_REVISION_VERSION_CFG) \
    )
    #error "AutoSar Version Numbers of C40_Ip_Cfg.c and C40_Ip_Cfg.h are different"
#endif
/* Check if current file and C40_Ip_Cfg.h header file are of the same Software version */
#if ((C40_IP_SW_MAJOR_VERSION_CFG_C != C40_IP_SW_MAJOR_VERSION_CFG) || \
     (C40_IP_SW_MINOR_VERSION_CFG_C != C40_IP_SW_MINOR_VERSION_CFG) || \
     (C40_IP_SW_PATCH_VERSION_CFG_C != C40_IP_SW_PATCH_VERSION_CFG) \
    )
    #error "Software Version Numbers of C40_Ip_Cfg.c and C40_Ip_Cfg.h are different"
#endif

/*==================================================================================================
                                     FUNCTION PROTOTYPES
==================================================================================================*/


/*==================================================================================================
                                           CONSTANTS
==================================================================================================*/
#define MEM_43_INFLS_START_SEC_CONFIG_DATA_UNSPECIFIED
#include "Mem_43_INFLS_MemMap.h"


const C40_Ip_ConfigType C40_Ip_InitCfg =
{
    NULL_PTR,                                               /* MemStartFlashAccessNotif    */
    NULL_PTR                                                /* MemFinishedFlashAccessNotif */
};


#define MEM_43_INFLS_STOP_SEC_CONFIG_DATA_UNSPECIFIED
#include "Mem_43_INFLS_MemMap.h"


#ifdef __cplusplus
}
#endif

/** @} */

