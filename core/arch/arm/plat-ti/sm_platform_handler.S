/*
 * Copyright (c) 2015, Texas Instruments
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES// LOSS OF USE, DATA, OR PROFITS// OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <asm.S>
#include <arm.h>
#include <arm32_macros.S>
#include "api_monitor_index.h"

.global sm_platform_handler

/******************************************************************************
 *  FUNCTION: sm_platform_handler
 *
 *  DESCRIPTION: This function is called when a particular Monitor function is
 *               called by the NS environment. The service doesn't need to
 *               enter the secure kernel but needs to be executed in SE. Hence
 *               Monitor is the appropriate environment
 *
 *  INPUTS: r0-r2 are the Secure Entry parameters, r12 the SMI Code Secure
 *          Service
 *          lr value is the Non-Secure return value
 *
 *  RETURN: API_HAL_RET_VALUE_SERVICE_UNKNWON if service is unknown.
 *
 *  NOTE: There are no secure information in the registers on exit, don't erase
 *        them.
 *        r3 is used to get the MMU Enable bit
 *****************************************************************************/
FUNC sm_platform_handler , :
	/* Check if this is a monitor mode API or not */
	cmp	r12, #0x200
	bxhs	lr

	/* Pop the complete stack, not going back to the calling function */
	pop	{lr}
	pop	{r0-r7}

	ldr	r3, =API_MONITOR_ACTLR_SETREGISTER_INDEX
	cmp	r12, r3
	bne	monapi_MonitorDispatcher_TIMER_SetCntFrq
	mcr	p15, 0, r0, c1, c0, 1
	isb
	b	monapi_MonitorDispatcher_Exit

monapi_MonitorDispatcher_TIMER_SetCntFrq:
	ldr	r3, =API_MONITOR_TIMER_SETCNTFRQ_INDEX
	cmp	r12, r3
	bne	monapi_MonitorDispatcher_Exit
	mcr	p15, 0, r0, c14, c0, 0
	isb

monapi_MonitorDispatcher_Exit:
	rfefd	sp!

END_FUNC sm_platform_handler
