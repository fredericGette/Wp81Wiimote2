#include "stdafx.h"

#define NUMBER_OF_THREADS 1
#define STATE_INQUIRY 1
#define STATE_CONNECTION 2

typedef struct _RemoteDevice {
	BYTE btAddr[6];
	BYTE pageScanRepetitionMode;
	BYTE clockOffset[2];
	BYTE connectionHandle[2];
} RemoteDevice;

static HANDLE hciControlDeviceEvt = NULL;
static HANDLE hciControlDeviceCmd = NULL;
static HANDLE hThreadArray[NUMBER_OF_THREADS];
static BOOL mainLoop_continue;
static RemoteDevice* remoteDevices[10] = { NULL };
static HANDLE hEventCmdFinished;
static DWORD currentState;

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
	printBuffer2HexString((BYTE*)remoteDevices[0], sizeof(RemoteDevice));
}

void storeConnectionHandle(BYTE* connectionComplete)
{
	if (memcmp(remoteDevices[0]->btAddr, connectionComplete + 10, 6) == 0)
	{
		memcpy(remoteDevices[0]->connectionHandle, connectionComplete + 8, 2);
		printBuffer2HexString((BYTE*)remoteDevices[0], sizeof(RemoteDevice));
	}
}

DWORD WINAPI readEvents(void* data) 
{
	DWORD returned;
	BYTE* readHci_inputBuffer;
	BYTE* readHci_outputBuffer;
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

	readHci_inputBuffer = (BYTE*)malloc(1);
	readHci_inputBuffer[0] = 0x04;
	readHci_inputBuffer[1] = 0x00;
	readHci_inputBuffer[2] = 0x00;
	readHci_inputBuffer[3] = 0x00;

	readHci_outputBuffer = (BYTE*)malloc(262);

	while (mainLoop_continue)
	{
		success = DeviceIoControl(hciControlDeviceEvt, IOCTL_CONTROL_READ_HCI, readHci_inputBuffer, 4, readHci_outputBuffer, 262, &returned, NULL);
		if (success)
		{
			printBuffer2HexString(readHci_outputBuffer, returned);
			if (returned == 11 && memcmp(readHci_outputBuffer, headerCommandComplete, 7) == 0)
			{
				printf("Command complete\n");
				SetEvent(hEventCmdFinished);
			}
			else if (returned == 22 && memcmp(readHci_outputBuffer, headerInquiryResult, 8) == 0)
			{
				printf("Detected %02X:%02X:%02X:%02X:%02X:%02X\n", readHci_outputBuffer[13], readHci_outputBuffer[12], readHci_outputBuffer[11], readHci_outputBuffer[10], readHci_outputBuffer[9], readHci_outputBuffer[8], readHci_outputBuffer[7]);
				if (!isRemoteDeviceAlreadyKnown(readHci_outputBuffer))
				{
					storeRemoteDevice(readHci_outputBuffer);
					printf("Stored %02X:%02X:%02X:%02X:%02X:%02X\n", readHci_outputBuffer[13], readHci_outputBuffer[12], readHci_outputBuffer[11], readHci_outputBuffer[10], readHci_outputBuffer[9], readHci_outputBuffer[8], readHci_outputBuffer[7]);
					currentState = STATE_CONNECTION;
				}
			}
			if (returned == 18 && memcmp(readHci_outputBuffer, headerConnectionComplete, 8) == 0)
			{
				storeConnectionHandle(readHci_outputBuffer);
				printf("Connection OK\n");
				SetEvent(hEventCmdFinished);
			}
		}
		else
		{
			printf("Failed to send IOCTL_CONTROL_READ_HCI! 0x%X\n", GetLastError());
		}
	}

	free(readHci_inputBuffer);
	free(readHci_outputBuffer);
	CloseHandle(hciControlDeviceEvt);

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
	cmd_inputBuffer[4] = 0x01;
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

	// First state: detect a nearby remote bluetooth device. 
	currentState = STATE_INQUIRY;
	// Inquiry command
	cmd_inputBuffer = (BYTE*)malloc(13);
	cmd_inputBuffer[0] = 0x08;
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x01;
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

	// Second state: connect to the remote device. 
	currentState = STATE_CONNECTION;
	// Connection command
	cmd_inputBuffer = (BYTE*)malloc(21);
	//10 00 00 00 01 05 04 0D 60 32 33 51 E7 E0 18 CC 01 00 04 BF 01
	cmd_inputBuffer[0] = 0x10;
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x01;
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
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
		exit_status = EXIT_FAILURE;
	}
	else
	{
		printf("Create connection\n");
		ResetEvent(hEventCmdFinished);
	}

	// Wait for the end of the Connection command
	WaitForSingleObject(hEventCmdFinished, 1000);

	// Wait for the end of the "read events" thread.
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