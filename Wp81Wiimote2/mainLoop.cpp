#include "stdafx.h"

#define NUMBER_OF_THREADS 2
#define STATE_INQUIRY 1
#define STATE_BT_CONNECTION 2
#define STATE_HID_CONTROL_CONNECTION 3
#define STATE_HID_CONTROL_CONFIGURATION 4
#define STATE_HID_CONTROL_CONFIGURATION_RESPONSE 5
#define STATE_HID_INTERRUPT_CONNECTION 6
#define STATE_HID_INTERRUPT_CONFIGURATION 7
#define STATE_HID_INTERRUPT_CONFIGURATION_RESPONSE 8
#define STATE_SET_LEDS 9

typedef struct _RemoteDevice {
	BYTE btAddr[6];
	BYTE pageScanRepetitionMode;
	BYTE clockOffset[2];
	BYTE connectionHandle[2];
	BYTE hidControlChannel[2];
	BYTE hidInterruptChannel[2];
	BYTE l2capMessageId;
} RemoteDevice;

static HANDLE hciControlDeviceEvt = NULL;
static HANDLE hciControlDeviceCmd = NULL;
static HANDLE hciControlDeviceAcl = NULL;
static HANDLE hThreadArray[NUMBER_OF_THREADS];
static BOOL mainLoop_continue;
static RemoteDevice* remoteDevices[10] = { NULL };
static HANDLE hEventCmdFinished;
static DWORD currentState;

// Debug helper
void printBuffer2HexString(BYTE* buffer, size_t bufSize)
{
	FILETIME SystemFileTime;
	BYTE *p = buffer;
	UINT i = 0;

	if (bufSize < 1)
	{
		return;
	}

	GetSystemTimeAsFileTime(&SystemFileTime);
	printf("%010u.%010u ", SystemFileTime.dwHighDateTime, SystemFileTime.dwLowDateTime);
	for (; i<bufSize; i++)
	{
		printf("%02X ", p[i]);
	}
	printf("\n");
}

BOOL isRemoteDeviceAlreadyKnown(BYTE* inquiryResult)
{
	if (remoteDevices[0] != NULL && memcmp(remoteDevices[0]->btAddr, inquiryResult + 8, 6) == 0)
	{
		return TRUE;
	}
	return FALSE;
}

void storeRemoteDevice(BYTE* inquiryResult)
{
	remoteDevices[0] = (RemoteDevice*)malloc(sizeof(RemoteDevice));
	memcpy(remoteDevices[0]->btAddr, inquiryResult+8, 6);
	remoteDevices[0]->pageScanRepetitionMode = inquiryResult[14];
	memcpy(remoteDevices[0]->clockOffset, inquiryResult+20, 2);
}

void storeConnectionHandle(BYTE* connectionComplete)
{
	if (memcmp(remoteDevices[0]->btAddr, connectionComplete + 10, 6) == 0)
	{
		memcpy(remoteDevices[0]->connectionHandle, connectionComplete + 8, 2);
	}
}

DWORD WINAPI readEvents(void* data)
{
	DWORD returned;
	BYTE* readEvent_inputBuffer;
	BYTE* readEvent_outputBuffer;
	BOOL success;
	BYTE headerCommandComplete[7] = { 0x06, 0x00, 0x00, 0x00, 0x04, 0x0E, 0x04 };
	BYTE headerInquiryResult[8] = { 0x11, 0x00, 0x00, 0x00, 0x04, 0x02, 0x0F, 0x01 };
	BYTE headerConnectionComplete[8] = { 0x0D, 0x00, 0x00, 0x00, 0x04, 0x03, 0x0B, 0x00 };

	hciControlDeviceEvt = CreateFileA("\\\\.\\wp81controldevice", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hciControlDeviceEvt == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open wp81controldevice device! 0x%08X\n", GetLastError());
		return EXIT_FAILURE;
	}

	readEvent_inputBuffer = (BYTE*)malloc(4);
	readEvent_inputBuffer[0] = 0x04;
	readEvent_inputBuffer[1] = 0x00;
	readEvent_inputBuffer[2] = 0x00;
	readEvent_inputBuffer[3] = 0x00;

	readEvent_outputBuffer = (BYTE*)malloc(262);

	printf("Start listening to Events...\n");
	while (mainLoop_continue)
	{
		success = DeviceIoControl(hciControlDeviceEvt, IOCTL_CONTROL_READ_HCI, readEvent_inputBuffer, 4, readEvent_outputBuffer, 262, &returned, NULL);
		if (success)
		{
			if (returned == 11 && memcmp(readEvent_outputBuffer, headerCommandComplete, 7) == 0)
			{
				printf("Received: Command complete\n");
				SetEvent(hEventCmdFinished);
			}
			else if (returned == 22 && memcmp(readEvent_outputBuffer, headerInquiryResult, 8) == 0)
			{
				printf("Detected %02X:%02X:%02X:%02X:%02X:%02X\n", readEvent_outputBuffer[13], readEvent_outputBuffer[12], readEvent_outputBuffer[11], readEvent_outputBuffer[10], readEvent_outputBuffer[9], readEvent_outputBuffer[8], readEvent_outputBuffer[7]);
				if (!isRemoteDeviceAlreadyKnown(readEvent_outputBuffer))
				{
					storeRemoteDevice(readEvent_outputBuffer);
					printf("Stored %02X:%02X:%02X:%02X:%02X:%02X\n", readEvent_outputBuffer[13], readEvent_outputBuffer[12], readEvent_outputBuffer[11], readEvent_outputBuffer[10], readEvent_outputBuffer[9], readEvent_outputBuffer[8], readEvent_outputBuffer[7]);
					currentState = STATE_BT_CONNECTION;
				}
			}
			if (returned == 18 && memcmp(readEvent_outputBuffer, headerConnectionComplete, 8) == 0)
			{
				storeConnectionHandle(readEvent_outputBuffer);
				printf("Received: BT Connection OK\n");
				currentState = STATE_HID_CONTROL_CONNECTION;
				SetEvent(hEventCmdFinished);
			}
		}
		else
		{
			printf("Failed to send IOCTL_CONTROL_READ_HCI! 0x%X\n", GetLastError());
		}
	}

	free(readEvent_inputBuffer);
	free(readEvent_outputBuffer);
	CloseHandle(hciControlDeviceEvt);

	return EXIT_SUCCESS;
}

DWORD WINAPI readAclData(void* data)
{
	DWORD returned;
	BYTE* readAcl_inputBuffer;
	BYTE* readAcl_outputBuffer;
	BOOL success;
	BYTE resultSuccess[4] = { 0x00, 0x00, 0x00, 0x00 };
	BYTE cidLocalHidControl[2] = { 0x40, 0x00 };
	BYTE cidLocalHidInterrupt[2] = { 0x41, 0x00 };

	hciControlDeviceAcl = CreateFileA("\\\\.\\wp81controldevice", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hciControlDeviceAcl == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open wp81controldevice device! 0x%08X\n", GetLastError());
		return EXIT_FAILURE;
	}

	readAcl_inputBuffer = (BYTE*)malloc(4);
	readAcl_inputBuffer[0] = 0x02;
	readAcl_inputBuffer[1] = 0x00;
	readAcl_inputBuffer[2] = 0x00;
	readAcl_inputBuffer[3] = 0x00;

	readAcl_outputBuffer = (BYTE*)malloc(1030);

	printf("Start listening to ACL Data...\n");
	while (mainLoop_continue)
	{
		success = DeviceIoControl(hciControlDeviceAcl, IOCTL_CONTROL_READ_HCI, readAcl_inputBuffer, 4, readAcl_outputBuffer, 1030, &returned, NULL);
		if (success)
		{
			if (returned == 25 && readAcl_outputBuffer[5] == remoteDevices[0]->connectionHandle[0] && readAcl_outputBuffer[13] == 0x03 && memcmp(readAcl_outputBuffer + 21, resultSuccess, 4) == 0)
			{
				// CONNECTION_RESPONSE (0x03)
				printf("Received: L2CAP Connection OK\n");
				switch (currentState)
				{
				case STATE_HID_CONTROL_CONNECTION:
					memcpy(remoteDevices[0]->hidControlChannel, readAcl_outputBuffer+17, 2);
					currentState = STATE_HID_CONTROL_CONFIGURATION;
					break;
				case STATE_HID_INTERRUPT_CONNECTION:
					memcpy(remoteDevices[0]->hidInterruptChannel, readAcl_outputBuffer + 17, 2);
					currentState = STATE_HID_INTERRUPT_CONFIGURATION;
					break;
				}
				SetEvent(hEventCmdFinished);
			}
			else if (returned == 25 && readAcl_outputBuffer[5] == remoteDevices[0]->connectionHandle[0] && readAcl_outputBuffer[13] == 0x04 && memcmp(readAcl_outputBuffer + 17, cidLocalHidControl, 2) == 0)
			{
				// We ignore the CONFIGURATION_RESPONSE to our CONFIGURATION_REQUEST, but we must respond to the CONFIGURATION_REQUEST (0x04) of the remote device.
				printf("Received: L2CAP Configuration request HID_control\n");
				remoteDevices[0]->l2capMessageId = readAcl_outputBuffer[14];
				currentState = STATE_HID_CONTROL_CONFIGURATION_RESPONSE;
				SetEvent(hEventCmdFinished);
			}
			else if (returned == 25 && readAcl_outputBuffer[5] == remoteDevices[0]->connectionHandle[0] && readAcl_outputBuffer[13] == 0x04 && memcmp(readAcl_outputBuffer + 17, cidLocalHidInterrupt, 2) == 0)
			{
				// We ignore the CONFIGURATION_RESPONSE to our CONFIGURATION_REQUEST, but we must respond to the CONFIGURATION_REQUEST (0x04) of the remote device.
				printf("Received: L2CAP Configuration request HID_interrupt\n");
				remoteDevices[0]->l2capMessageId = readAcl_outputBuffer[14];
				currentState = STATE_HID_INTERRUPT_CONFIGURATION_RESPONSE;
				SetEvent(hEventCmdFinished);
			}
		}
		else
		{
			printf("Failed to send IOCTL_CONTROL_READ_HCI! 0x%X\n", GetLastError());
		}
	}

	free(readAcl_inputBuffer);
	free(readAcl_outputBuffer);
	CloseHandle(hciControlDeviceAcl);

	return EXIT_SUCCESS;
}

void mainLoop_exit()
{
	printf("Terminating...\n");
	mainLoop_continue = FALSE;
}

int mainLoop_run()
{
	DWORD returned;
	BYTE* cmd_inputBuffer;
	BYTE* cmd_outputBuffer;
	BOOL success;
	int exit_status = EXIT_SUCCESS;

	mainLoop_continue = TRUE;

	hciControlDeviceCmd = CreateFileA("\\\\.\\wp81controldevice", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hciControlDeviceCmd == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open wp81controldevice device! 0x%08X\n", GetLastError());
		return EXIT_FAILURE;
	}

	cmd_inputBuffer = (BYTE*)malloc(1);
	cmd_inputBuffer[0] = 1; // Block IOCTL_BTHX_WRITE_HCI and IOCTL_BTHX_READ_HCI coming from the Windows Bluetooth stack.
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_CMD, cmd_inputBuffer, 1, NULL, 0, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
		CloseHandle(hciControlDeviceCmd);
		free(cmd_inputBuffer);
		return EXIT_FAILURE;
	}
	free(cmd_inputBuffer);

	// Want to execute only one command at a time
	hEventCmdFinished = CreateEventW(
		NULL,
		TRUE,	// manually reset
		FALSE,	// initial state: nonsignaled
		L"WP81_CMD_IN_PROGRESS"
	);

	// Start "read events" thread
	hThreadArray[0] = CreateThread(NULL, 0, readEvents, NULL, 0, NULL);

	// Reset command
	cmd_inputBuffer = (BYTE*)malloc(8);
	cmd_inputBuffer[0] = 0x03;
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x01; // Command
	cmd_inputBuffer[5] = 0x03;
	cmd_inputBuffer[6] = 0x0C;
	cmd_inputBuffer[7] = 0x00;
	cmd_outputBuffer = (BYTE*)malloc(4);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 8, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
		exit_status = EXIT_FAILURE;
	}
	else
	{
		printf("Reset\n");
		ResetEvent(hEventCmdFinished);
	}
	free(cmd_inputBuffer);
	free(cmd_outputBuffer);

	// Wait for the end of the Reset command
	WaitForSingleObject(hEventCmdFinished, 1000);

	// State: detect a nearby remote bluetooth device. 
	currentState = STATE_INQUIRY;
	// Inquiry command
	cmd_inputBuffer = (BYTE*)malloc(13);
	cmd_inputBuffer[0] = 0x08;
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x01; // Command
	cmd_inputBuffer[5] = 0x01;
	cmd_inputBuffer[6] = 0x04;
	cmd_inputBuffer[7] = 0x05;
	cmd_inputBuffer[8] = 0x00;  // Limited Inquiry Access Code (LIAC) 0x9E8B00
	cmd_inputBuffer[9] = 0x8B;  // -
	cmd_inputBuffer[10] = 0x9E; // -
	cmd_inputBuffer[11] = 0x02; // Length x 1.28s
	cmd_inputBuffer[12] = 0x00; // Inquiry completed by 1 response
	cmd_outputBuffer = (BYTE*)malloc(4);
	while (mainLoop_continue && currentState == STATE_INQUIRY)
	{
		success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 13, cmd_outputBuffer, 4, &returned, NULL);
		if (!success)
		{
			printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
			exit_status = EXIT_FAILURE;
		}
		else
		{
			printf("Start inquiry\n");
			ResetEvent(hEventCmdFinished);
		}
		Sleep(2560); // Length x 1280ms
	}
	free(cmd_inputBuffer);
	free(cmd_outputBuffer);

	// State: connect to the remote device. 
	// Connection command
	cmd_inputBuffer = (BYTE*)malloc(21);
	cmd_inputBuffer[0] = 0x10;
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x01; // Command
	cmd_inputBuffer[5] = 0x05;
	cmd_inputBuffer[6] = 0x04;
	cmd_inputBuffer[7] = 0x0D;
	memcpy(cmd_inputBuffer + 8, remoteDevices[0]->btAddr, 6);
	cmd_inputBuffer[14] = 0x18; // packetType =  DH3,DM5,DH1,DH5,DM3,DM1 
	cmd_inputBuffer[15] = 0xCC; // -
	cmd_inputBuffer[16] = remoteDevices[0]->pageScanRepetitionMode;
	cmd_inputBuffer[17] = 0x00; // Reserved
	memcpy(cmd_inputBuffer + 18, remoteDevices[0]->clockOffset, 2);
	cmd_inputBuffer[19] |= 0x80; // set bit 15: clockOffset is valid
 	cmd_inputBuffer[20] = 0x01; // allowRoleSwitch:ALLOWED 
	cmd_outputBuffer = (BYTE*)malloc(4);
	while (mainLoop_continue && currentState == STATE_BT_CONNECTION)
	{
		success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
		if (!success)
		{
			printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
			exit_status = EXIT_FAILURE;
		}
		else
		{
			printf("Create BT connection\n");
			ResetEvent(hEventCmdFinished);
		}

		// Wait for the end of the Connection command
		WaitForSingleObject(hEventCmdFinished, 2000);
	}

	// Start "read ACL data" thread
	hThreadArray[1] = CreateThread(NULL, 0, readAclData, NULL, 0, NULL);

	// State: Open a "HID Control" channel with the remote device. 
	// Connection request command
	cmd_inputBuffer = (BYTE*)malloc(21);
	cmd_inputBuffer[0] = 0x10; // Length of the IOCTL message
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x02; // ACL data
	cmd_inputBuffer[5] = remoteDevices[0]->connectionHandle[0];
	cmd_inputBuffer[6] = remoteDevices[0]->connectionHandle[1];
	cmd_inputBuffer[7] = 0x0C; // Length of the ACL message
	cmd_inputBuffer[8] = 0x00;
	cmd_inputBuffer[9] = 0x08;  // Length of the L2CAP message (CMD+msgId+Length+PSM+CID)
	cmd_inputBuffer[10] = 0x00; // -
	cmd_inputBuffer[11] = 0x01; // Signaling channel
	cmd_inputBuffer[12] = 0x00; // -
	cmd_inputBuffer[13] = 0x02; // CONNECTION_REQUEST
	cmd_inputBuffer[14] = 0x01; // message id
	cmd_inputBuffer[15] = 0x04; // Length of the command parameters (PSM + CID)
	cmd_inputBuffer[16] = 0x00; // -
	cmd_inputBuffer[17] = 0x11; // PSM HID_control
	cmd_inputBuffer[18] = 0x00; // -
	cmd_inputBuffer[19] = 0x40; // local ID of the new channel requested
	cmd_inputBuffer[20] = 0x00; // -
	cmd_outputBuffer = (BYTE*)malloc(4);
	while (mainLoop_continue && currentState == STATE_HID_CONTROL_CONNECTION)
	{
		success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
		if (!success)
		{
			printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
			exit_status = EXIT_FAILURE;
		}
		else
		{
			printf("Create L2CAP HID_control connection\n");
			ResetEvent(hEventCmdFinished);
		}

		// Wait for the end of the Connection command
		WaitForSingleObject(hEventCmdFinished, 2000);
	}

	// State: Configure the "HID Control" channel with the remote device. 
	// Configuration request command (No options)
	cmd_inputBuffer = (BYTE*)malloc(21);
	cmd_inputBuffer[0] = 0x10; // Length of the IOCTL message
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x02; // ACL data
	cmd_inputBuffer[5] = remoteDevices[0]->connectionHandle[0];
	cmd_inputBuffer[6] = remoteDevices[0]->connectionHandle[1];
	cmd_inputBuffer[7] = 0x0C; // Length of the ACL message
	cmd_inputBuffer[8] = 0x00;
	cmd_inputBuffer[9] = 0x08;  // Length of the L2CAP message (CMD+msgId+Length+CID+Flags)
	cmd_inputBuffer[10] = 0x00; // -
	cmd_inputBuffer[11] = 0x01; // Signaling channel
	cmd_inputBuffer[12] = 0x00; // -
	cmd_inputBuffer[13] = 0x04; // CONFIGURATION_REQUEST
	cmd_inputBuffer[14] = 0x02; // message id
	cmd_inputBuffer[15] = 0x04; // Length of the command parameters (CID+Flags)
	cmd_inputBuffer[16] = 0x00; // -
	cmd_inputBuffer[17] = remoteDevices[0]->hidControlChannel[0];
	cmd_inputBuffer[18] = remoteDevices[0]->hidControlChannel[1];
	cmd_inputBuffer[19] = 0x00; // Flags
	cmd_inputBuffer[20] = 0x00; // -
	cmd_outputBuffer = (BYTE*)malloc(4);
	while (mainLoop_continue && currentState == STATE_HID_CONTROL_CONFIGURATION)
	{
		success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
		if (!success)
		{
			printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
			exit_status = EXIT_FAILURE;
		}
		else
		{
			printf("Configure L2CAP HID_control connection\n");
			ResetEvent(hEventCmdFinished);
		}

		// Wait for the end of the Configuration request command
		WaitForSingleObject(hEventCmdFinished, 2000);
	}

	// State: Repond to finish the configuration of the "HID Control" channel with the remote device. 
	// Configuration response command (success)
	cmd_inputBuffer = (BYTE*)malloc(23);
	cmd_inputBuffer[0] = 0x12; // Length of the IOCTL message
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x02; // ACL data
	cmd_inputBuffer[5] = remoteDevices[0]->connectionHandle[0];
	cmd_inputBuffer[6] = remoteDevices[0]->connectionHandle[1];
	cmd_inputBuffer[7] = 0x0E; // Length of the ACL message
	cmd_inputBuffer[8] = 0x00;
	cmd_inputBuffer[9] = 0x0A;  // Length of the L2CAP message
	cmd_inputBuffer[10] = 0x00; // -
	cmd_inputBuffer[11] = 0x01; // Signaling channel
	cmd_inputBuffer[12] = 0x00; // -
	cmd_inputBuffer[13] = 0x05; // CONFIGURATION_REPONSE
	cmd_inputBuffer[14] = remoteDevices[0]->l2capMessageId;
	cmd_inputBuffer[15] = 0x06; // Length of the command parameters (CID+Flags)
	cmd_inputBuffer[16] = 0x00; // -
	cmd_inputBuffer[17] = remoteDevices[0]->hidControlChannel[0];
	cmd_inputBuffer[18] = remoteDevices[0]->hidControlChannel[1];
	cmd_inputBuffer[19] = 0x00; // Flags
	cmd_inputBuffer[20] = 0x00; // -
	cmd_inputBuffer[21] = 0x00; // Result = success
	cmd_inputBuffer[22] = 0x00; // -
	cmd_outputBuffer = (BYTE*)malloc(4);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 23, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
		exit_status = EXIT_FAILURE;
	}
	else
	{
		printf("Finish configuration L2CAP HID_control connection\n");
		ResetEvent(hEventCmdFinished);
		currentState = STATE_HID_INTERRUPT_CONNECTION;
	}

	Sleep(1000);

	// State: Open a "HID Interrupt" channel with the remote device. 
	// Connection request command
	cmd_inputBuffer = (BYTE*)malloc(21);
	cmd_inputBuffer[0] = 0x10; // Length of the IOCTL message
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x02; // ACL data
	cmd_inputBuffer[5] = remoteDevices[0]->connectionHandle[0];
	cmd_inputBuffer[6] = remoteDevices[0]->connectionHandle[1];
	cmd_inputBuffer[7] = 0x0C; // Length of the ACL message
	cmd_inputBuffer[8] = 0x00;
	cmd_inputBuffer[9] = 0x08;  // Length of the L2CAP message (CMD+msgId+Length+PSM+CID)
	cmd_inputBuffer[10] = 0x00; // -
	cmd_inputBuffer[11] = 0x01; // Signaling channel
	cmd_inputBuffer[12] = 0x00; // -
	cmd_inputBuffer[13] = 0x02; // CONNECTION_REQUEST
	cmd_inputBuffer[14] = 0x03; // message id
	cmd_inputBuffer[15] = 0x04; // Length of the command parameters (PSM + CID)
	cmd_inputBuffer[16] = 0x00; // -
	cmd_inputBuffer[17] = 0x13; // PSM HID_interrupt
	cmd_inputBuffer[18] = 0x00; // -
	cmd_inputBuffer[19] = 0x41; // local ID of the new channel requested
	cmd_inputBuffer[20] = 0x00; // -
	cmd_outputBuffer = (BYTE*)malloc(4);
	while (mainLoop_continue && currentState == STATE_HID_INTERRUPT_CONNECTION)
	{
		success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
		if (!success)
		{
			printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
			exit_status = EXIT_FAILURE;
		}
		else
		{
			printf("Create L2CAP HID_interrupt connection\n");
			ResetEvent(hEventCmdFinished);
		}

		// Wait for the end of the Connection command
		WaitForSingleObject(hEventCmdFinished, 2000);
	}

	// State: Configure the "HID Interrupt" channel with the remote device. 
	// Configuration request command (No options)
	cmd_inputBuffer = (BYTE*)malloc(21);
	cmd_inputBuffer[0] = 0x10; // Length of the IOCTL message
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x02; // ACL data
	cmd_inputBuffer[5] = remoteDevices[0]->connectionHandle[0];
	cmd_inputBuffer[6] = remoteDevices[0]->connectionHandle[1];
	cmd_inputBuffer[7] = 0x0C; // Length of the ACL message
	cmd_inputBuffer[8] = 0x00;
	cmd_inputBuffer[9] = 0x08;  // Length of the L2CAP message (CMD+msgId+Length+CID+Flags)
	cmd_inputBuffer[10] = 0x00; // -
	cmd_inputBuffer[11] = 0x01; // Signaling channel
	cmd_inputBuffer[12] = 0x00; // -
	cmd_inputBuffer[13] = 0x04; // CONFIGURATION_REQUEST
	cmd_inputBuffer[14] = 0x02; // message id
	cmd_inputBuffer[15] = 0x04; // Length of the command parameters (CID+Flags)
	cmd_inputBuffer[16] = 0x00; // -
	cmd_inputBuffer[17] = remoteDevices[0]->hidInterruptChannel[0];
	cmd_inputBuffer[18] = remoteDevices[0]->hidInterruptChannel[1];
	cmd_inputBuffer[19] = 0x00; // Flags
	cmd_inputBuffer[20] = 0x00; // -
	cmd_outputBuffer = (BYTE*)malloc(4);
	while (mainLoop_continue && currentState == STATE_HID_INTERRUPT_CONFIGURATION)
	{
		success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
		if (!success)
		{
			printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
			exit_status = EXIT_FAILURE;
		}
		else
		{
			printf("Configure L2CAP HID_interrupt connection\n");
			ResetEvent(hEventCmdFinished);
		}

		// Wait for the end of the Configuration request command
		WaitForSingleObject(hEventCmdFinished, 2000);
	}

	// State: Repond to finish the configuration of the "HID Interrupt" channel with the remote device. 
	// Configuration response command (success)
	cmd_inputBuffer = (BYTE*)malloc(23);
	cmd_inputBuffer[0] = 0x12; // Length of the IOCTL message
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x02; // ACL data
	cmd_inputBuffer[5] = remoteDevices[0]->connectionHandle[0];
	cmd_inputBuffer[6] = remoteDevices[0]->connectionHandle[1];
	cmd_inputBuffer[7] = 0x0E; // Length of the ACL message
	cmd_inputBuffer[8] = 0x00;
	cmd_inputBuffer[9] = 0x0A;  // Length of the L2CAP message
	cmd_inputBuffer[10] = 0x00; // -
	cmd_inputBuffer[11] = 0x01; // Signaling channel
	cmd_inputBuffer[12] = 0x00; // -
	cmd_inputBuffer[13] = 0x05; // CONFIGURATION_REPONSE
	cmd_inputBuffer[14] = remoteDevices[0]->l2capMessageId;
	cmd_inputBuffer[15] = 0x06; // Length of the command parameters (CID+Flags)
	cmd_inputBuffer[16] = 0x00; // -
	cmd_inputBuffer[17] = remoteDevices[0]->hidInterruptChannel[0];
	cmd_inputBuffer[18] = remoteDevices[0]->hidInterruptChannel[1];
	cmd_inputBuffer[19] = 0x00; // Flags
	cmd_inputBuffer[20] = 0x00; // -
	cmd_inputBuffer[21] = 0x00; // Result = success
	cmd_inputBuffer[22] = 0x00; // -
	cmd_outputBuffer = (BYTE*)malloc(4);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 23, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
		exit_status = EXIT_FAILURE;
	}
	else
	{
		printf("Finish configuration L2CAP HID_interrupt connection\n");
		ResetEvent(hEventCmdFinished);
		currentState = STATE_HID_INTERRUPT_CONNECTION;
	}

	Sleep(1000);

	// State: Set the LEDs of the remote device. 
	cmd_inputBuffer = (BYTE*)malloc(16);
	cmd_inputBuffer[0] = 0x0B; // Length of the IOCTL message
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x02; // ACL data
	cmd_inputBuffer[5] = remoteDevices[0]->connectionHandle[0];
	cmd_inputBuffer[6] = remoteDevices[0]->connectionHandle[1];
	cmd_inputBuffer[7] = 0x07; // Length of the ACL message
	cmd_inputBuffer[8] = 0x00;
	cmd_inputBuffer[9] = 0x03;  // Length of the L2CAP message
	cmd_inputBuffer[10] = 0x00; // -
	cmd_inputBuffer[11] = remoteDevices[0]->hidInterruptChannel[0];
	cmd_inputBuffer[12] = remoteDevices[0]->hidInterruptChannel[1];
	cmd_inputBuffer[13] = 0xA2; // Output report
	cmd_inputBuffer[14] = 0x11; // Player LEDs
	cmd_inputBuffer[15] = 0x10; // Set LED 1
	cmd_outputBuffer = (BYTE*)malloc(4);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 16, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
		exit_status = EXIT_FAILURE;
	}
	else
	{
		printf("Set LEDs\n");
		ResetEvent(hEventCmdFinished);
	}

	// Wait for the end of the "read events" and "read ACL data" threads.
	WaitForMultipleObjectsEx(NUMBER_OF_THREADS, hThreadArray, TRUE, INFINITE, TRUE);
	for (int i = 0; i < NUMBER_OF_THREADS; i++)
	{
		CloseHandle(hThreadArray[i]);
	}
	
	cmd_inputBuffer = (BYTE*)malloc(1);

	cmd_inputBuffer[0] = 0; // Unblock IOCTL_BTHX_WRITE_HCI and IOCTL_BTHX_READ_HCI coming from the Windows Bluetooth stack.
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_CMD, cmd_inputBuffer, 1, NULL, 0, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
		exit_status = EXIT_FAILURE;
	}
	
	CloseHandle(hciControlDeviceCmd);
	free(cmd_inputBuffer);
	return exit_status;
}