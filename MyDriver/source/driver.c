#include "../headers/headers.h"
#include <wdm.h>
#include <ntstrsafe.h>


/*
INFO BUILD:
OS VERSION: Windows 7
SDK VERSION: 10.0.18362.0
VISUAl STUDIO 2019 COMMUNITY
*/

#define DEVICE_SEND CTL_CODE(FILE_DEVICE_UNKNOWN,0x801,METHOD_BUFFERED,FILE_WRITE_DATA)
#define DEVICE_REC CTL_CODE(FILE_DEVICE_UNKNOWN,0x802,METHOD_BUFFERED,FILE_READ_DATA)


#define DEFAULT_NUMBEROFDEVICES 5
HANDLE DirHandle;

NTSTATUS CreateDevice(struct _DRIVER_OBJECT* DriverObject, ULONG uNumber, DEVICE_TYPE DeviceType);
PDEVICE_OBJECT DeleteDevice(IN PDEVICE_OBJECT pDeviceObject);

DRIVER_UNLOAD Unload;
//DRIVER_DISPATCH StubFunc;
DRIVER_DISPATCH CreateAndCloseDevice;
DRIVER_DISPATCH ReadAndWriteDevice;
DRIVER_DISPATCH ControlDevice;
KSTART_ROUTINE Thread;

NTSTATUS DriverEntry(struct _DRIVER_OBJECT* DriverObject, UNICODE_STRING* pRegPath)
{
	UNREFERENCED_PARAMETER(pRegPath);

	const ULONG           uDevices = DEFAULT_NUMBEROFDEVICES;
	ULONG                 n;
	USHORT                uCreatedDevice;
	UNICODE_STRING        unDeviceDirName;
	OBJECT_ATTRIBUTES     ObjectAttributes;

	RtlInitUnicodeString(&unDeviceDirName, DEVICE_DIR_NAME);

	InitializeObjectAttributes(
		&ObjectAttributes,
		&unDeviceDirName,
		OBJ_PERMANENT,
		NULL,
		NULL
	);


	NTSTATUS status = ZwCreateDirectoryObject(
		&DirHandle,
		DIRECTORY_ALL_ACCESS,
		&ObjectAttributes);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	/*
	 * A temporary object has a name only as long as its handle count is greater than zero. When the handle count reaches zero, the system deletes the object name and appropriately adjusts the object's pointer count.
	 */
	ZwMakeTemporaryObject(DirHandle);
	DbgBreakPoint();
	for (n = 0, uCreatedDevice = 0; n < uDevices; n++)
	{
		status = CreateDevice(DriverObject, n, FILE_DEVICE_DISK);

		if (NT_SUCCESS(status))
		{
			uCreatedDevice++;
		}
	}
	if (uCreatedDevice == 0)
	{
		ZwClose(DirHandle);
		return status;
	}



	/*for (int i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; ++i)
	{
		DriverObject->MajorFunction[i] = StubFunc;
	}*/

	DriverObject->MajorFunction[IRP_MJ_CREATE] = CreateAndCloseDevice;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = CreateAndCloseDevice;
	DriverObject->MajorFunction[IRP_MJ_WRITE] = ReadAndWriteDevice;
	DriverObject->MajorFunction[IRP_MJ_READ] = ReadAndWriteDevice;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ControlDevice;
	DriverObject->DriverUnload = Unload;

	DbgPrint("Success driver installation!\r\n");

	return status;
}

_Use_decl_annotations_
NTSTATUS ControlDevice(struct _DEVICE_OBJECT* pDeviceObject, struct _IRP* pIrp)
{

	PDEVICE_EXTENSION  pDeviceExtension = (PDEVICE_EXTENSION)pDeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(pIrp);
	NTSTATUS status = STATUS_SUCCESS;
	if (!pDeviceExtension->media_in_device
		&&
		IoStack->Parameters.DeviceIoControl.IoControlCode != IOCTL_FILE_DISK_OPEN_FILE
		)
	{
		pIrp->IoStatus.Status = STATUS_NO_MEDIA_IN_DEVICE;
		pIrp->IoStatus.Information = 0;
		IoCompleteRequest(pIrp, IO_NO_INCREMENT);
		return STATUS_NO_MEDIA_IN_DEVICE;
	}

	switch (IoStack->Parameters.DeviceIoControl.IoControlCode)
	{

		case IOCTL_FILE_DISK_OPEN_FILE:
		{
			if (pDeviceExtension->media_in_device)
			{
				status = STATUS_INVALID_DEVICE_REQUEST;
				pIrp->IoStatus.Information = 0;
				break;
			}

			if (IoStack->Parameters.DeviceIoControl.InputBufferLength
				<
				sizeof(OPEN_FILE_INFORMATION)
				+
				((POPEN_FILE_INFORMATION)pIrp->AssociatedIrp.SystemBuffer)->FileNameLength * sizeof(WCHAR)
				-
				sizeof(WCHAR))
			{
				status = STATUS_INVALID_PARAMETER;
				pIrp->IoStatus.Information = 0;
				break;
			}

			IoMarkIrpPending(pIrp);

			ExInterlockedInsertTailList(
				&pDeviceExtension->list_head,
				&pIrp->Tail.Overlay.ListEntry,
				&pDeviceExtension->list_lock
			);

			KeSetEvent(
				&pDeviceExtension->request_event,
				(KPRIORITY)0,
				FALSE
			);

			status = STATUS_PENDING;

		}
		default:
        {
            KdPrint((
                "FileDisk: Unknown IoControlCode %#x\n",
                IoStack->Parameters.DeviceIoControl.IoControlCode
                ));

            status = STATUS_INVALID_DEVICE_REQUEST;
            pIrp->IoStatus.Information = 0;
        }

	}

	if (status != STATUS_PENDING)
    {
        pIrp->IoStatus.Status = status;

        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    }

    return status;

}

/*
   The DRIVER_UNLOAD function type is defined in the Wdm.h header file.
   To more accurately identify errors when you run the code analysis tools, be sure to add the _Use_decl_annotations_ annotation to your function definition.
   The _Use_decl_annotations_ annotation ensures that the annotations that are applied to the DRIVER_UNLOAD function type in the header file are used.
   For more information about the requirements for function declarations, see Declaring Functions by Using Function Role Types for WDM Drivers.
   For information about _Use_decl_annotations_, see Annotating Function Behavior.
*/
_Use_decl_annotations_
VOID Unload(IN PDRIVER_OBJECT pDriverObject)
{
	PAGED_CODE();
	PDEVICE_OBJECT pDeviceObject = pDriverObject->DeviceObject;

	while (pDeviceObject)
	{
		pDeviceObject = DeleteDevice(pDeviceObject);
	}

	ZwClose(DirHandle);
#ifdef DBG
	DbgPrint("Unload success!\r\n");
#endif


}

PDEVICE_OBJECT DeleteDevice(IN PDEVICE_OBJECT pDeviceObject)
{
	PAGED_CODE();
	ASSERT(pDeviceObject != NULL);

	PDEVICE_EXTENSION pDeviceExtension = pDeviceObject->DeviceExtension;
	pDeviceExtension->terminate_thread = TRUE;

	KeSetEvent(
		&pDeviceExtension->request_event,
		(KPRIORITY)0,
		FALSE
	);

	KeWaitForSingleObject(
		pDeviceExtension->thread_pointer,
		Executive,
		KernelMode,
		FALSE,
		NULL
	);

	ObDereferenceObject(pDeviceExtension->thread_pointer);
	if (pDeviceExtension->device_name.Buffer != NULL)
	{

		ExFreePool(pDeviceExtension->device_name.Buffer);

	}

	/*
	 * A pointer to the next device object, if any, that was created by the same driver.
	 * The I/O manager updates this list at each successful call to IoCreateDevice or IoCreateDeviceSecure.
	 */
	PDEVICE_OBJECT pNextDevice = pDeviceObject->NextDevice;

	IoDeleteDevice(pDeviceObject);

	return pNextDevice;

}

_Use_decl_annotations_
NTSTATUS CreateAndCloseDevice(struct _DEVICE_OBJECT* pDeviceObject, struct _IRP* pIrp)
{
	UNREFERENCED_PARAMETER(pDeviceObject);
	pIrp->IoStatus.Status = STATUS_SUCCESS;
	pIrp->IoStatus.Information = FILE_OPENED;

	IoCompleteRequest(pIrp, IO_NO_INCREMENT);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ReadAndWriteDevice(struct _DEVICE_OBJECT* pDeviceObject, struct _IRP* pIrp)
{
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pDeviceObject->DeviceExtension;
	if (!pDeviceExtension->media_in_device)
	{
		pIrp->IoStatus.Status = STATUS_NO_MEDIA_IN_DEVICE;
		pIrp->IoStatus.Information = 0;

		IoCompleteRequest(pIrp, IO_NO_INCREMENT);

		return STATUS_NO_MEDIA_IN_DEVICE;

	}

	PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(pIrp);
	if (IoStack->Parameters.Read.Length == 0)
	{
		pIrp->IoStatus.Status = STATUS_SUCCESS;
		pIrp->IoStatus.Information = 0;

		IoCompleteRequest(pIrp, IO_NO_INCREMENT);

		return STATUS_SUCCESS;
	}


	IoMarkIrpPending(pIrp);

	ExInterlockedInsertTailList(
		&pDeviceExtension->list_head,
		&pIrp->Tail.Overlay.ListEntry,
		&pDeviceExtension->list_lock
	);


	KeSetEvent(
		&pDeviceExtension->request_event,
		(KPRIORITY)0,
		FALSE
	);

	return STATUS_PENDING;

}

NTSTATUS CreateDevice(struct _DRIVER_OBJECT* DriverObject, ULONG uNumber, DEVICE_TYPE DeviceType)
{


	UNICODE_STRING      usDeviceName;
	PDEVICE_OBJECT      pDeviceObject;
	HANDLE              hThread;
	UNICODE_STRING      usSSDDL;

	ASSERT(DriverObject != NULL);

	usDeviceName.Buffer = (PWCHAR)ExAllocatePoolWithTag(PagedPool, MAXIMUM_FILENAME_LENGTH * 2, FILE_DISK_POOL_TAG);
	if (usDeviceName.Buffer == NULL)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	usDeviceName.Length = 0;
	usDeviceName.MaximumLength = MAXIMUM_FILENAME_LENGTH * 2;
	RtlUnicodeStringPrintf(&usDeviceName, DEVICE_NAME_PREFIX L"%u", uNumber);
	RtlInitUnicodeString(&usSSDDL, _T("D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;BU)"));


	//Creates a named device
	NTSTATUS status = IoCreateDeviceSecure(
		DriverObject,
		sizeof(DEVICE_EXTENSION),
		&usDeviceName,
		DeviceType,
		0,
		FALSE,
		&usSSDDL,
		NULL,
		&pDeviceObject
	);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	//The operating system locks the application's buffer in memory.
	//It then creates a memory descriptor list (MDL) that identifies the locked memory pages, and passes the MDL to the driver stack.
	//Drivers access the locked pages through the MDL.
	pDeviceObject->Flags |= DO_DIRECT_IO;

	/*
	 * For most intermediate and lowest-level drivers, the device extension is the most important data structure associated with a device object. Its internal structure is driver-defined, and it is typically used to:

		Maintain device state information.
		Provide storage for any kernel-defined objects or other system resources, such as spin locks, used by the driver.
		Hold any data the driver must have resident and in system space to carry out its I/O operations.
	 */
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pDeviceObject->DeviceExtension;
	pDeviceExtension->media_in_device = FALSE;
	pDeviceExtension->device_name.Length = usDeviceName.Length;
	pDeviceExtension->device_name.MaximumLength = usDeviceName.MaximumLength;
	pDeviceExtension->device_name.Buffer = usDeviceName.Buffer;
	pDeviceExtension->device_number = uNumber;
	pDeviceExtension->device_type = DeviceType;


	InitializeListHead(&pDeviceExtension->list_head);
	KeInitializeSpinLock(&pDeviceExtension->list_lock);

	KeInitializeEvent(
		&pDeviceExtension->request_event,
		SynchronizationEvent,
		FALSE);

	pDeviceExtension->terminate_thread = FALSE;

	status = PsCreateSystemThread(
		&hThread,
		(ACCESS_MASK)0L,
		NULL,
		NULL,
		NULL,
		Thread,
		pDeviceObject
	);

	if (!NT_SUCCESS(status))
	{
		IoDeleteDevice(pDeviceObject);
		ExFreePool(usDeviceName.Buffer);
		return status;
	}

	/*The ObReferenceObjectByHandle routine provides access validation on the object handle, and, if access can be granted, returns the corresponding pointer to the object's body.*/

	status = ObReferenceObjectByHandle(
		hThread,
		THREAD_ALL_ACCESS,
		NULL,
		KernelMode,
		&pDeviceExtension->thread_pointer,
		NULL
	);

	if (!NT_SUCCESS(status))
	{
		ZwClose(hThread);
		pDeviceExtension->terminate_thread = TRUE;
		KeSetEvent(
			&pDeviceExtension->request_event,
			(KPRIORITY)0,
			FALSE
		);
		IoDeleteDevice(pDeviceObject);
		ExFreePool(usDeviceName.Buffer);
		return status;

	}

	ZwClose(hThread);

	return status;

}

VOID Thread(IN PVOID pContext)
{
	PLIST_ENTRY         pRequest;
	//PUCHAR              uSystemBuffer;
	//PUCHAR              uBuffer;
	//ULONGLONG			i = 0, j = 0;

	//The PAGED_CODE macro ensures that the calling thread is running at an IRQL that is low enough to permit paging.
	PAGED_CODE();

	ASSERT(pContext != NULL);

	PDEVICE_OBJECT pDeviceObject = (PDEVICE_OBJECT)pContext;
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pDeviceObject->DeviceExtension;
	KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
	while (TRUE)
	{

		KeWaitForSingleObject(
			&pDeviceExtension->request_event,
			Executive,
			KernelMode,
			FALSE,
			NULL
		);

		if (pDeviceExtension->terminate_thread)
		{
			PsTerminateSystemThread(STATUS_SUCCESS);
		}

		while ((pRequest = ExInterlockedRemoveHeadList(
			&pDeviceExtension->list_head,
			&pDeviceExtension->list_lock))
			!= NULL)
		{
			/*The CONTAINING_RECORD macro returns the base address of an instance of a structure
			 *given the type of the structure and the address of a field within the containing structure.*/
			PIRP pIrp = CONTAINING_RECORD(pRequest, IRP, Tail.Overlay.ListEntry);
			PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(pIrp);


			switch (IoStack->MajorFunction)
			{


			default:
				pIrp->IoStatus.Status = STATUS_SUCCESS;

			}

			IoCompleteRequest(
				pIrp,
				(CCHAR)(NT_SUCCESS(pIrp->IoStatus.Status) ?
					IO_DISK_INCREMENT : IO_NO_INCREMENT
					)
			);


		}

	}


}
