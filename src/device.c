/*++
	Copyright (c) Microsoft Corporation. All Rights Reserved.
	Sample code. Dealpoint ID #843729.

	Module Name:

		device.c

	Abstract:

		Code for handling WDF device-specific requests

	Environment:

		Kernel mode

	Revision History:

--*/

#include "internal.h"
#include "controller.h"
#include "device.h"
#include "spb.h"
#include "idle.h"
#include "debug.h"
//#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, OnD0Exit)
#endif

BOOLEAN
OnInterruptIsr(
	IN WDFINTERRUPT Interrupt,
	IN ULONG MessageID
)
/*++

  Routine Description:

	This routine responds to interrupts generated by the
	controller. If one is recognized, it queues a DPC for
	processing.

	This is a PASSIVE_LEVEL ISR. ACPI should specify
	level-triggered interrupts when using Synaptics 3202.

  Arguments:

	Interrupt - a handle to a framework interrupt object
	MessageID - message number identifying the device's
		hardware interrupt message (if using MSI)

  Return Value:

	TRUE if interrupt recognized.

--*/
{
	PDEVICE_EXTENSION devContext;
	NTSTATUS status;
	BOOLEAN servicingComplete;
	PHID_INPUT_REPORT hidReportsFromDriver;
    int hidReportsCount = 0;

	UNREFERENCED_PARAMETER(MessageID);

	status = STATUS_SUCCESS;
	servicingComplete = FALSE;
	devContext = GetDeviceContext(WdfInterruptGetDevice(Interrupt));

	//
	// Service the device interrupt
	//

	//
	// Service touch interrupts. Success indicates we have a report
	// to complete to Hid. ServicingComplete indicates another report
	// is required to continue servicing this interrupt.
	//
    status = TchServiceInterrupts(
        devContext->TouchContext,
        &devContext->I2CContext,
        devContext->InputMode,
        &hidReportsFromDriver,
        &hidReportsCount
    );

	if (!NT_SUCCESS(status))
	{
		//
		// error on interupt servicing
		//
        goto exit;
	}

    SendHidReports(
        devContext->PingPongQueue,
        hidReportsFromDriver,
        hidReportsCount
    );

exit:
	return TRUE;
}

void
SendHidReports(
    WDFQUEUE PingPongQueue,
    PHID_INPUT_REPORT hidReportsFromDriver,
    int hidReportsCount
)
{
    NTSTATUS status;
    WDFREQUEST request = NULL;
    PHID_INPUT_REPORT hidReportRequestBuffer;
    size_t hidReportRequestBufferLength;

    for(int i = 0; i < hidReportsCount; i++)
    {
        //
        // Complete a HIDClass request if one is available
        //
        status = WdfIoQueueRetrieveNextRequest(
            PingPongQueue,
            &request);

        if(!NT_SUCCESS(status))
        {
            Trace(
                TRACE_LEVEL_ERROR,
                TRACE_FLAG_REPORTING,
                "No request pending from HIDClass, ignoring report - STATUS:%X",
                status);

            continue;
        }

        //
        // Validate an output buffer was provided
        //
        status = WdfRequestRetrieveOutputBuffer(
            request,
            sizeof(HID_INPUT_REPORT),
            &hidReportRequestBuffer,
            &hidReportRequestBufferLength);

        if(!NT_SUCCESS(status))
        {
            Trace(
                TRACE_LEVEL_WARNING,
                TRACE_FLAG_SAMPLES,
                "Error retrieving HID read request output buffer - STATUS:%X",
                status);
        }
        else
        {
            //
            // Validate the size of the output buffer
            //
            if(hidReportRequestBufferLength < sizeof(HID_INPUT_REPORT))
            {
                status = STATUS_BUFFER_TOO_SMALL;

                Trace(
                    TRACE_LEVEL_WARNING,
                    TRACE_FLAG_SAMPLES,
                    "Error HID read request buffer is too small (%lu bytes) - STATUS:%X",
                    hidReportRequestBufferLength,
                    status);
            }
            else
            {
                RtlCopyMemory(
                    hidReportRequestBuffer,
                    &hidReportsFromDriver[i],
                    sizeof(HID_INPUT_REPORT));

                WdfRequestSetInformation(request, sizeof(HID_INPUT_REPORT));
            }
        }

        WdfRequestComplete(request, status);
    }
}

NTSTATUS
OnD0Entry(
	IN WDFDEVICE Device,
	IN WDF_POWER_DEVICE_STATE PreviousState
)
/*++

Routine Description:

	This routine will power on the hardware

Arguments:

	Device - WDF device to power on
	PreviousState - Prior power state

Return Value:

	NTSTATUS indicating success or failure

*/
{
	NTSTATUS status;
	PDEVICE_EXTENSION devContext;

	devContext = GetDeviceContext(Device);

	UNREFERENCED_PARAMETER(PreviousState);

	status = TchWakeDevice(devContext->TouchContext, &devContext->I2CContext);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_FLAG_POWER,
			"Error setting device to D0 - STATUS:%X",
			status);
	}

	//
	// N.B. This RMI chip's IRQ is level-triggered, but cannot be enabled in
	//      ACPI until passive-level interrupt handling is added to the driver.
	//      Service chip in case we missed an edge during D3 or boot-up.
	//
	devContext->ServiceInterruptsAfterD0Entry = TRUE;

	//
	// Complete any pending Idle IRPs
	//
	TchCompleteIdleIrp(devContext);

	return status;
}

NTSTATUS
OnD0Exit(
	IN WDFDEVICE Device,
	IN WDF_POWER_DEVICE_STATE TargetState
)
/*++

Routine Description:

	This routine will power down the hardware

Arguments:

	Device - WDF device to power off

	PreviousState - Prior power state

Return Value:

	NTSTATUS indicating success or failure

*/
{
	NTSTATUS status;
	PDEVICE_EXTENSION devContext;

	PAGED_CODE();

	devContext = GetDeviceContext(Device);

	UNREFERENCED_PARAMETER(TargetState);

	status = TchStandbyDevice(devContext->TouchContext, &devContext->I2CContext);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_FLAG_POWER,
			"Error exiting D0 - STATUS:%X",
			status);
	}

	return status;
}


NTSTATUS GetGPIO(WDFIOTARGET gpio, unsigned char* value)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_MEMORY_DESCRIPTOR outputDescriptor;

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDescriptor, value, 1);

	status = WdfIoTargetSendIoctlSynchronously(gpio, NULL, IOCTL_GPIO_READ_PINS, NULL, &outputDescriptor, NULL, NULL);

	return status;
}

NTSTATUS SetGPIO(WDFIOTARGET gpio, unsigned char* value)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_MEMORY_DESCRIPTOR inputDescriptor, outputDescriptor;

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDescriptor, value, 1);
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDescriptor, value, 1);

	status = WdfIoTargetSendIoctlSynchronously(gpio, NULL, IOCTL_GPIO_WRITE_PINS, &inputDescriptor, &outputDescriptor, NULL, NULL);

	return status;
}

NTSTATUS OpenIOTarget(PDEVICE_EXTENSION ctx, LARGE_INTEGER res, ACCESS_MASK use, WDFIOTARGET* target)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_OBJECT_ATTRIBUTES ObjectAttributes;
	WDF_IO_TARGET_OPEN_PARAMS OpenParams;
	UNICODE_STRING ReadString;
	WCHAR ReadStringBuffer[260];

	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "OpenIOTarget Entry");

	RtlInitEmptyUnicodeString(&ReadString,
		ReadStringBuffer,
		sizeof(ReadStringBuffer));

	status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&ReadString,
		res.LowPart,
		res.HighPart);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "RESOURCE_HUB_CREATE_PATH_FROM_ID failed 0x%x", status);
		goto Exit;
	}

	WDF_OBJECT_ATTRIBUTES_INIT(&ObjectAttributes);
	ObjectAttributes.ParentObject = ctx->FxDevice;

	status = WdfIoTargetCreate(ctx->FxDevice, &ObjectAttributes, target);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "WdfIoTargetCreate failed 0x%x", status);
		goto Exit;
	}

	WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams, &ReadString, use);
	status = WdfIoTargetOpen(*target, &OpenParams);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "WdfIoTargetOpen failed 0x%x", status);
		goto Exit;
	}

Exit:
	Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "OpenIOTarget Exit");
	return status;
}

NTSTATUS
OnPrepareHardware(
	IN WDFDEVICE FxDevice,
	IN WDFCMRESLIST FxResourcesRaw,
	IN WDFCMRESLIST FxResourcesTranslated
)
/*++

  Routine Description:

	This routine is called by the PnP manager and supplies thie device instance
	with it's SPB resources (CmResourceTypeConnection) needed to find the I2C
	driver.

  Arguments:

	FxDevice - a handle to the framework device object
	FxResourcesRaw - list of translated hardware resources that
		the PnP manager has assigned to the device
	FxResourcesTranslated - list of raw hardware resources that
		the PnP manager has assigned to the device

  Return Value:

	NTSTATUS indicating sucess or failure

--*/
{
	NTSTATUS status;
	PCM_PARTIAL_RESOURCE_DESCRIPTOR res;
	PDEVICE_EXTENSION devContext;
	ULONG resourceCount;
	ULONG i;
	//LARGE_INTEGER delay;
	//unsigned char value;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	status = STATUS_INSUFFICIENT_RESOURCES;
	devContext = GetDeviceContext(FxDevice);

	//
	// Get the resouce hub connection ID for our I2C driver
	//
	resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (i = 0; i < resourceCount; i++)
	{
		res = WdfCmResourceListGetDescriptor(FxResourcesTranslated, i);

		if (res->Type == CmResourceTypeConnection &&
			res->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
			res->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
		{
			devContext->I2CContext.I2cResHubId.LowPart =
				res->u.Connection.IdLowPart;
			devContext->I2CContext.I2cResHubId.HighPart =
				res->u.Connection.IdHighPart;

			status = STATUS_SUCCESS;
		}

		if (res->Type == CmResourceTypeConnection &&
			res->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
			res->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO)
		{
			devContext->ResetGpioId.LowPart = 
				res->u.Connection.IdLowPart;
			devContext->ResetGpioId.HighPart = 
				res->u.Connection.IdHighPart;

			devContext->HasResetGpio = TRUE;
		}
	}

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_FLAG_INIT,
			"Error finding CmResourceTypeConnection resource - STATUS:%X",
			status);

		goto exit;
	}

	/*if (devContext->HasResetGpio)
	{
		status = OpenIOTarget(devContext, devContext->ResetGpioId, GENERIC_READ | GENERIC_WRITE, &devContext->ResetGpio);
		if (!NT_SUCCESS(status)) {
			Trace(TRACE_LEVEL_ERROR, TRACE_DRIVER, "OpenIOTarget failed for Reset GPIO 0x%x", status);
			goto exit;
		}

		Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Starting bring up sequence for the controller");

		Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Setting reset gpio pin to low");

		value = 0;
		SetGPIO(devContext->ResetGpio, &value);

		Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Waiting...");

		delay.QuadPart = -10 * TOUCH_POWER_RAIL_STABLE_TIME;
		KeDelayExecutionThread(KernelMode, TRUE, &delay);

		Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Setting reset gpio pin to high");

		value = 1;
		SetGPIO(devContext->ResetGpio, &value);

		Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Waiting...");

		delay.QuadPart = -10 * TOUCH_DELAY_TO_COMMUNICATE;
		KeDelayExecutionThread(KernelMode, TRUE, &delay);

		Trace(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Done");
	}*/

	//
	// Initialize Spb so the driver can issue reads/writes
	//
	status = SpbTargetInitialize(FxDevice, &devContext->I2CContext);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_FLAG_INIT,
			"Error in Spb initialization - STATUS:%X",
			status);

		goto exit;
	}

	//
	// Prepare the hardware for touch scanning
	//
	status = TchAllocateContext(&devContext->TouchContext, FxDevice);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_FLAG_INIT,
			"Error allocating touch context - STATUS:%X",
			status);

		goto exit;
	}

	//
	// Start the controller
	//
	status = TchStartDevice(devContext->TouchContext, &devContext->I2CContext);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_FLAG_INIT,
			"Error starting touch device - STATUS:%X",
			status);

		goto exit;
	}

exit:

	return status;
}

NTSTATUS
OnReleaseHardware(
	IN WDFDEVICE FxDevice,
	IN WDFCMRESLIST FxResourcesTranslated
)
/*++

  Routine Description:

	This routine cleans up any resources provided.

  Arguments:

	FxDevice - a handle to the framework device object
	FxResourcesRaw - list of translated hardware resources that
		the PnP manager has assigned to the device
	FxResourcesTranslated - list of raw hardware resources that
		the PnP manager has assigned to the device

  Return Value:

	NTSTATUS indicating sucesss or failure

--*/
{
	NTSTATUS status;
	PDEVICE_EXTENSION devContext;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	devContext = GetDeviceContext(FxDevice);

	status = TchStopDevice(devContext->TouchContext, &devContext->I2CContext);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_FLAG_PNP,
			"Error stopping device - STATUS:%X",
			status);
	}

	status = TchFreeContext(devContext->TouchContext);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_FLAG_PNP,
			"Error freeing touch context - STATUS:%X",
			status);
	}

	SpbTargetDeinitialize(FxDevice, &GetDeviceContext(FxDevice)->I2CContext);

	return status;
}

