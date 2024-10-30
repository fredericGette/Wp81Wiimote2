#include "stdafx.h"

#define NUMBER_OF_THREADS 1

static HANDLE hciControlDevice = NULL;
static HANDLE hThreadArray[NUMBER_OF_THREADS];
static BOOL mainLoop_continue;

void printBuffer2HexString(UCHAR* buffer, size_t bufSize)
{
	FILETIME SystemFileTime;
	UCHAR *p = buffer;
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

DWORD WINAPI readEvents(void* data) 
{
	DWORD returned;
	UCHAR* readHci_inputBuffer;
	UCHAR* readHci_outputBuffer;
	BOOL success;

	readHci_inputBuffer = (UCHAR*)malloc(1);
	readHci_inputBuffer[0] = 0x04;
	readHci_inputBuffer[1] = 0x00;
	readHci_inputBuffer[2] = 0x00;
	readHci_inputBuffer[3] = 0x00;

	readHci_outputBuffer = (UCHAR*)malloc(262);

	while (mainLoop_continue)
	{
		success = DeviceIoControl(hciControlDevice, IOCTL_CONTROL_READ_HCI, readHci_inputBuffer, 4, readHci_outputBuffer, 262, &returned, NULL);
		if (success)
		{
			printBuffer2HexString(readHci_outputBuffer, returned);
		}
		else
		{
			printf("Failed to send IOCTL_CONTROL_READ_HCI! 0x%X\n", GetLastError());
		}
	}

	free(readHci_inputBuffer);
	free(readHci_outputBuffer);

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
	UCHAR* cmd_inputBuffer;
	BOOL success;

	mainLoop_continue = TRUE;

	hciControlDevice = CreateFileA("\\\\.\\wp81controldevice", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hciControlDevice == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open wp81controldevice device! 0x%08X\n", GetLastError());
		return EXIT_FAILURE;
	}

	cmd_inputBuffer = (UCHAR*)malloc(1);
	cmd_inputBuffer[0] = 1; // Block IOCTL_BTHX_WRITE_HCI and IOCTL_BTHX_READ_HCI coming from the Windows Bluetooth stack.
	success = DeviceIoControl(hciControlDevice, IOCTL_CONTROL_CMD, cmd_inputBuffer, 1, NULL, 0, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
		CloseHandle(hciControlDevice);
		free(cmd_inputBuffer);
		return EXIT_FAILURE;
	}
	free(cmd_inputBuffer);

	// Start read event thread
	hThreadArray[0] = CreateThread(NULL, 0, readEvents, NULL, 0, NULL);

	WaitForMultipleObjectsEx(NUMBER_OF_THREADS, hThreadArray, TRUE, INFINITE, TRUE);
	for (int i = 0; i < NUMBER_OF_THREADS; i++)
	{
		CloseHandle(hThreadArray[i]);
	}
	
	cmd_inputBuffer = (UCHAR*)malloc(1);
	cmd_inputBuffer[0] = 0; // Unblock IOCTL_BTHX_WRITE_HCI and IOCTL_BTHX_READ_HCI coming from the Windows Bluetooth stack.
	success = DeviceIoControl(hciControlDevice, IOCTL_CONTROL_CMD, cmd_inputBuffer, 1, NULL, 0, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
		CloseHandle(hciControlDevice);
		free(cmd_inputBuffer);
		return EXIT_FAILURE;
	}
	
	CloseHandle(hciControlDevice);
	free(cmd_inputBuffer);
	return EXIT_SUCCESS;
}