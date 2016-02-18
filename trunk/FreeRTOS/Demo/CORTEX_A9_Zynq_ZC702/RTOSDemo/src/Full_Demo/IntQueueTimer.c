/*
    FreeRTOS V9.0.0rc1 - Copyright (C) 2016 Real Time Engineers Ltd.
    All rights reserved

    VISIT http://www.FreeRTOS.org TO ENSURE YOU ARE USING THE LATEST VERSION.

    This file is part of the FreeRTOS distribution.

    FreeRTOS is free software; you can redistribute it and/or modify it under
    the terms of the GNU General Public License (version 2) as published by the
    Free Software Foundation >>>> AND MODIFIED BY <<<< the FreeRTOS exception.

    ***************************************************************************
    >>!   NOTE: The modification to the GPL is included to allow you to     !<<
    >>!   distribute a combined work that includes FreeRTOS without being   !<<
    >>!   obliged to provide the source code for proprietary components     !<<
    >>!   outside of the FreeRTOS kernel.                                   !<<
    ***************************************************************************

    FreeRTOS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE.  Full license text is available on the following
    link: http://www.freertos.org/a00114.html

    ***************************************************************************
     *                                                                       *
     *    FreeRTOS provides completely free yet professionally developed,    *
     *    robust, strictly quality controlled, supported, and cross          *
     *    platform software that is more than just the market leader, it     *
     *    is the industry's de facto standard.                               *
     *                                                                       *
     *    Help yourself get started quickly while simultaneously helping     *
     *    to support the FreeRTOS project by purchasing a FreeRTOS           *
     *    tutorial book, reference manual, or both:                          *
     *    http://www.FreeRTOS.org/Documentation                              *
     *                                                                       *
    ***************************************************************************

    http://www.FreeRTOS.org/FAQHelp.html - Having a problem?  Start by reading
    the FAQ page "My application does not run, what could be wrong?".  Have you
    defined configASSERT()?

    http://www.FreeRTOS.org/support - In return for receiving this top quality
    embedded software for free we request you assist our global community by
    participating in the support forum.

    http://www.FreeRTOS.org/training - Investing in training allows your team to
    be as productive as possible as early as possible.  Now you can receive
    FreeRTOS training directly from Richard Barry, CEO of Real Time Engineers
    Ltd, and the world's leading authority on the world's leading RTOS.

    http://www.FreeRTOS.org/plus - A selection of FreeRTOS ecosystem products,
    including FreeRTOS+Trace - an indispensable productivity tool, a DOS
    compatible FAT file system, and our tiny thread aware UDP/IP stack.

    http://www.FreeRTOS.org/labs - Where new FreeRTOS products go to incubate.
    Come and try FreeRTOS+TCP, our new open source TCP/IP stack for FreeRTOS.

    http://www.OpenRTOS.com - Real Time Engineers ltd. license FreeRTOS to High
    Integrity Systems ltd. to sell under the OpenRTOS brand.  Low cost OpenRTOS
    licenses offer ticketed support, indemnification and commercial middleware.

    http://www.SafeRTOS.com - High Integrity Systems also provide a safety
    engineered and independently SIL3 certified version for use in safety and
    mission critical applications that require provable dependability.

    1 tab == 4 spaces!
*/

/*
 * This file initialises three timers as follows:
 *
 * Timer 0 and Timer 1 provide the interrupts that are used with the IntQ
 * standard demo tasks, which test interrupt nesting and using queues from
 * interrupts.  Both these interrupts operate below the maximum syscall
 * interrupt priority.
 *
 * Timer 2 is a much higher frequency timer that tests the nesting of interrupts
 * that execute above the maximum syscall interrupt priority.
 *
 * All the timers can nest with the tick interrupt - creating a maximum
 * interrupt nesting depth of 4.
 *
 * For convenience, the high frequency timer is also used to provide the time
 * base for the run time stats.
 */

/* Scheduler includes. */
#include "FreeRTOS.h"

/* Demo includes. */
#include "IntQueueTimer.h"
#include "IntQueue.h"

/* Xilinx includes. */
#include "xttcps.h"
#include "xscugic.h"

/* The frequencies at which the first two timers expire are slightly offset to
ensure they don't remain synchronised.  The frequency of the interrupt that
operates above the max syscall interrupt priority is 10 times faster so really
hammers the interrupt entry and exit code. */
#define tmrTIMERS_USED	3
#define tmrTIMER_0_FREQUENCY	( 2000UL )
#define tmrTIMER_1_FREQUENCY	( 2001UL )
#define tmrTIMER_2_FREQUENCY	( 20000UL )

/*-----------------------------------------------------------*/

/*
 * The single interrupt service routines that is used to service all three
 * timers.
 */
static void prvTimerHandler( void *CallBackRef );

/*-----------------------------------------------------------*/

/* Hardware constants. */
static const BaseType_t xDeviceIDs[ tmrTIMERS_USED ] = { XPAR_XTTCPS_0_DEVICE_ID, XPAR_XTTCPS_1_DEVICE_ID, XPAR_XTTCPS_2_DEVICE_ID };
static const BaseType_t xInterruptIDs[ tmrTIMERS_USED ] = { XPAR_XTTCPS_0_INTR, XPAR_XTTCPS_1_INTR, XPAR_XTTCPS_2_INTR };

/* Timer configuration settings. */
typedef struct
{
	uint32_t OutputHz;	/* Output frequency. */
	uint16_t Interval;	/* Interval value. */
	uint8_t Prescaler;	/* Prescaler value. */
	uint16_t Options;	/* Option settings. */
} TmrCntrSetup;

static TmrCntrSetup xTimerSettings[ tmrTIMERS_USED ] =
{
	{ tmrTIMER_0_FREQUENCY, 0, 0, XTTCPS_OPTION_INTERVAL_MODE | XTTCPS_OPTION_WAVE_DISABLE },
	{ tmrTIMER_1_FREQUENCY, 0, 0, XTTCPS_OPTION_INTERVAL_MODE | XTTCPS_OPTION_WAVE_DISABLE },
	{ tmrTIMER_2_FREQUENCY, 0, 0, XTTCPS_OPTION_INTERVAL_MODE | XTTCPS_OPTION_WAVE_DISABLE }
};

/* Lower priority number means higher logical priority, so
configMAX_API_CALL_INTERRUPT_PRIORITY - 1 is above the maximum system call
interrupt priority. */
static const UBaseType_t uxInterruptPriorities[ tmrTIMERS_USED ] =
{
	configMAX_API_CALL_INTERRUPT_PRIORITY + 1,
	configMAX_API_CALL_INTERRUPT_PRIORITY,
	configMAX_API_CALL_INTERRUPT_PRIORITY - 1
};

static XTtcPs xTimerInstances[ tmrTIMERS_USED ];

/* Used to provide a means of ensuring the intended interrupt nesting depth is
actually being reached. */
extern uint32_t ulPortInterruptNesting;
static uint32_t ulMaxRecordedNesting = 0;

/* Used to ensure the high frequency timer is running at the expected
frequency. */
static volatile uint32_t ulHighFrequencyTimerCounts = 0;

/*-----------------------------------------------------------*/

void vInitialiseTimerForIntQueueTest( void )
{
BaseType_t xStatus;
TmrCntrSetup *pxTimerSettings;
extern XScuGic xInterruptController;
BaseType_t xTimer;
XTtcPs *pxTimerInstance;
XTtcPs_Config *pxTimerConfiguration;
const uint8_t ucRisingEdge = 3;

	for( xTimer = 0; xTimer < tmrTIMERS_USED; xTimer++ )
	{
		/* Look up the timer's configuration. */
		pxTimerInstance = &( xTimerInstances[ xTimer ] );
		pxTimerConfiguration = XTtcPs_LookupConfig( xDeviceIDs[ xTimer ] );
		configASSERT( pxTimerConfiguration );

		pxTimerSettings = &( xTimerSettings[ xTimer ] );

		/* Initialise the device. */
		xStatus = XTtcPs_CfgInitialize( pxTimerInstance, pxTimerConfiguration, pxTimerConfiguration->BaseAddress );
		if( xStatus != XST_SUCCESS )
		{
			/* Not sure how to do this before XTtcPs_CfgInitialize is called
			as pxTimerInstance is set within XTtcPs_CfgInitialize(). */
			XTtcPs_Stop( pxTimerInstance );
			xStatus = XTtcPs_CfgInitialize( pxTimerInstance, pxTimerConfiguration, pxTimerConfiguration->BaseAddress );
			configASSERT( xStatus == XST_SUCCESS );
		}

		/* Set the options. */
		XTtcPs_SetOptions( pxTimerInstance, pxTimerSettings->Options );

		/* The timer frequency is preset in the pxTimerSettings structure.
		Derive the values for the other structure members. */
		XTtcPs_CalcIntervalFromFreq( pxTimerInstance, pxTimerSettings->OutputHz, &( pxTimerSettings->Interval ), &( pxTimerSettings->Prescaler ) );

		/* Set the interval and prescale. */
		XTtcPs_SetInterval( pxTimerInstance, pxTimerSettings->Interval );
		XTtcPs_SetPrescaler( pxTimerInstance, pxTimerSettings->Prescaler );

		/* The priority must be the lowest possible. */
		XScuGic_SetPriorityTriggerType( &xInterruptController, xInterruptIDs[ xTimer ], uxInterruptPriorities[ xTimer ] << portPRIORITY_SHIFT, ucRisingEdge );

		/* Connect to the interrupt controller. */
		xStatus = XScuGic_Connect( &xInterruptController, xInterruptIDs[ xTimer ], ( Xil_InterruptHandler ) prvTimerHandler, ( void * ) pxTimerInstance );
		configASSERT( xStatus == XST_SUCCESS);

		/* Enable the interrupt in the GIC. */
		XScuGic_Enable( &xInterruptController, xInterruptIDs[ xTimer ] );

		/* Enable the interrupts in the timer. */
		XTtcPs_EnableInterrupts( pxTimerInstance, XTTCPS_IXR_INTERVAL_MASK );

		/* Start the timer. */
		XTtcPs_Start( pxTimerInstance );
	}
}
/*-----------------------------------------------------------*/

static void prvTimerHandler( void *pvCallBackRef )
{
uint32_t ulInterruptStatus;
XTtcPs *pxTimer = ( XTtcPs * ) pvCallBackRef;
BaseType_t xYieldRequired;

	/* Read the interrupt status, then write it back to clear the interrupt. */
	ulInterruptStatus = XTtcPs_GetInterruptStatus( pxTimer );
	XTtcPs_ClearInterruptStatus( pxTimer, ulInterruptStatus );

	/* Only one interrupt event type is expected. */
	configASSERT( ( XTTCPS_IXR_INTERVAL_MASK & ulInterruptStatus ) != 0 );

	/* Check the device ID to know which IntQueue demo to call. */
	if( pxTimer->Config.DeviceId == xDeviceIDs[ 0 ] )
	{
		xYieldRequired = xFirstTimerHandler();
	}
	else if( pxTimer->Config.DeviceId == xDeviceIDs[ 1 ] )
	{
		xYieldRequired = xSecondTimerHandler();
	}
	else
	{
		/* Used to check the timer is running at the expected frequency. */
		ulHighFrequencyTimerCounts++;

		/* Latch the highest interrupt nesting count detected. */
		if( ulPortInterruptNesting > ulMaxRecordedNesting )
		{
			ulMaxRecordedNesting = ulPortInterruptNesting;
		}

		xYieldRequired = pdFALSE;
	}

	/* If xYieldRequired is not pdFALSE then calling either xFirstTimerHandler()
	or xSecondTimerHandler() resulted in a task leaving the blocked state and
	the task that left the blocked state had a priority higher than the currently
	running task (the task this interrupt interrupted) - so a context switch
	should be performed so the interrupt returns directly to the higher priority
	task.  xYieldRequired is tested inside the following macro. */
	portYIELD_FROM_ISR( xYieldRequired );
}

