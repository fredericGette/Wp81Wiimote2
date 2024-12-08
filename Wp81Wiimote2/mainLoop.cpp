#include "stdafx.h"

#define NUMBER_OF_THREADS 2
#define NUMBER_OF_WIIMOTES 7

#define STATE_BT_RESET 0
#define STATE_BT_INQUIRY 1
#define STATE_BT_CONNECTION 2
#define STATE_HID_CONTROL_CONNECTION 3
#define STATE_HID_CONTROL_CONFIGURATION 4
#define STATE_HID_CONTROL_CONFIGURATION_RESPONSE 5
#define STATE_HID_INTERRUPT_CONNECTION 6
#define STATE_HID_INTERRUPT_CONFIGURATION 7
#define STATE_HID_INTERRUPT_CONFIGURATION_RESPONSE 8
#define STATE_WIIMOTE_SET_LEDS 9
#define STATE_WIIMOTE_SET_DATA_REPORTING_MODE_WITHOUT_CAMERA 10
#define STATE_WIIMOTE_READ_INPUTS 11
#define STATE_WIIMOTE_ACTIVATE_CAMERA_1 12
#define STATE_WIIMOTE_ACTIVATE_CAMERA_2 13
#define STATE_WIIMOTE_START_CAMERA_CONFIGURATION 14
#define STATE_WIIMOTE_CONFIGURE_CAMERA_SENSIBILITY_1 15
#define STATE_WIIMOTE_CONFIGURE_CAMERA_SENSIBILITY_2 16
#define STATE_WIIMOTE_CONFIGURE_CAMERA_MODE 17
#define STATE_WIIMOTE_END_CAMERA_CONFIGURATION 18
#define STATE_WIIMOTE_SET_DATA_REPORTING_MODE_WITH_CAMERA 19
#define STATE_HID_CONTROL_DISCONNECTION 20
#define STATE_HID_INTERRUPT_DISCONNECTION 21
#define STATE_FINISHED 22

typedef struct _Wiimote {
	BYTE btAddr[6];
	BYTE pageScanRepetitionMode;
	BYTE clockOffset[2];
	BYTE connectionHandle[2];
	BYTE hidControlChannel[2];
	BYTE hidInterruptChannel[2];
	BYTE hidControlConfRequestMessageId;
	BYTE hidInterruptConfRequestMessageId;
	BYTE buttons[2];
	DWORD state;
	BYTE id;
	BYTE status[4];
} Wiimote;

static HANDLE hciControlDeviceEvt = NULL;
static HANDLE hciControlDeviceCmd = NULL;
static HANDLE hciControlDeviceAcl = NULL;
static HANDLE hEventCmdFinished = NULL;
static HANDLE hThreadArray[NUMBER_OF_THREADS] = { NULL };
static BOOL mainLoop_continue;
static BOOL readLoop_continue;
static Wiimote* wiimotes[NUMBER_OF_WIIMOTES] = { NULL };
static DWORD currentState;
static DWORD previousTickCount;
static DWORD previousMsgCount;
static DWORD msgCount;
static DWORD tickCount;
static BOOL verbose;

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

BOOL isWiimoteAlreadyKnown(BYTE* inquiryResult)
{
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && memcmp(wiimotes[i]->btAddr, inquiryResult + 8, 6) == 0)
		{
			return TRUE;
		}
	}
	return FALSE;
}

void storeRemoteDevice(BYTE* inquiryResult)
{
	int newId = 0;
	// Find ID of the new wiimote
	for (int candidateID = 1; candidateID <= NUMBER_OF_WIIMOTES; candidateID++)
	{
		BOOL inUse = FALSE;
		for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
		{
			if (wiimotes[i] != NULL && wiimotes[i]->id == candidateID)
			{
				inUse = TRUE;
				break;
			}
		}
		if (!inUse)
		{
			newId = candidateID;
			break;
		}
	}

	if (newId == 0)
	{
		printf("Max number of wiimotes reached : %d\n", NUMBER_OF_WIIMOTES);
		return;
	}

	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] == NULL)
		{
			wiimotes[i] = (Wiimote*)malloc(sizeof(Wiimote));
			memcpy(wiimotes[i]->btAddr, inquiryResult + 8, 6);
			wiimotes[i]->pageScanRepetitionMode = inquiryResult[14];
			memcpy(wiimotes[i]->clockOffset, inquiryResult + 20, 2);
			wiimotes[i]->state = STATE_BT_CONNECTION;
			wiimotes[i]->id = newId;
			wiimotes[i]->hidControlConfRequestMessageId = 0;
			wiimotes[i]->hidInterruptConfRequestMessageId = 0;
			wiimotes[i]->status[3] = 0x00; // Initialize battery level
			return;
		}
	}
}

void storeConnectionHandle(BYTE* connectionComplete)
{
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && memcmp(wiimotes[i]->btAddr, connectionComplete + 10, 6) == 0)
		{
			if (verbose) printf("%d: Received BT Connection OK\n", wiimotes[i]->id);
			memcpy(wiimotes[i]->connectionHandle, connectionComplete + 8, 2);
			wiimotes[i]->state = STATE_HID_CONTROL_CONNECTION;
			return;
		}
	}
}


void removeConnectionHandle(BYTE* disconnectionComplete)
{
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (memcmp(disconnectionComplete + 8, wiimotes[i]->connectionHandle, 2) == 0)
		{
			if (verbose) printf("%d: Received Disconnection complete\n", wiimotes[i]->id);
			memset(wiimotes[i]->connectionHandle, 0, 2);
			wiimotes[i]->state = STATE_FINISHED;
			return;
		}
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
	BYTE headerDisconnectionComplete[7] = { 0x06, 0x00, 0x00, 0x00, 0x04, 0x05, 0x04 };

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

	if (verbose) printf("Start listening to Events...\n");
	while (readLoop_continue)
	{
		success = DeviceIoControl(hciControlDeviceEvt, IOCTL_CONTROL_READ_HCI, readEvent_inputBuffer, 4, readEvent_outputBuffer, 262, &returned, NULL);
		if (success)
		{
			if (returned == 11 && memcmp(readEvent_outputBuffer, headerCommandComplete, 7) == 0)
			{
				if (verbose) printf("Received: Command complete\n");
				SetEvent(hEventCmdFinished);
			}
			else if (returned == 22 && memcmp(readEvent_outputBuffer, headerInquiryResult, 8) == 0)
			{
				if (verbose) printf("Detected %02X:%02X:%02X:%02X:%02X:%02X\n", readEvent_outputBuffer[13], readEvent_outputBuffer[12], readEvent_outputBuffer[11], readEvent_outputBuffer[10], readEvent_outputBuffer[9], readEvent_outputBuffer[8], readEvent_outputBuffer[7]);
				if (!isWiimoteAlreadyKnown(readEvent_outputBuffer))
				{
					storeRemoteDevice(readEvent_outputBuffer);
					if (verbose) printf("Stored %02X:%02X:%02X:%02X:%02X:%02X\n", readEvent_outputBuffer[13], readEvent_outputBuffer[12], readEvent_outputBuffer[11], readEvent_outputBuffer[10], readEvent_outputBuffer[9], readEvent_outputBuffer[8], readEvent_outputBuffer[7]);
				}
			}
			else if (returned == 18 && memcmp(readEvent_outputBuffer, headerConnectionComplete, 8) == 0)
			{
				storeConnectionHandle(readEvent_outputBuffer);
				SetEvent(hEventCmdFinished);
			}
			else if (returned == 11 && memcmp(readEvent_outputBuffer, headerDisconnectionComplete, 7) == 0)
			{
				removeConnectionHandle(readEvent_outputBuffer);
			}
			else
			{
				if (verbose) printf("Received: unkown Event\n");
				if (verbose) printBuffer2HexString(readEvent_outputBuffer, returned);
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

void storeL2CapChannel(BYTE* connectionResponse)
{
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->connectionHandle[0] == connectionResponse[5])
		{
			switch (wiimotes[i]->state)
			{
			case STATE_HID_CONTROL_CONNECTION:
				if (verbose) printf("%d: Received L2CAP HID-Control Connection OK\n", wiimotes[i]->id);
				memcpy(wiimotes[i]->hidControlChannel, connectionResponse + 17, 2);
				wiimotes[i]->state = STATE_HID_CONTROL_CONFIGURATION;
				break;
			case STATE_HID_INTERRUPT_CONNECTION:
				if (verbose) printf("%d: Received L2CAP HID-Interrupt Connection OK\n", wiimotes[i]->id);
				memcpy(wiimotes[i]->hidInterruptChannel, connectionResponse + 17, 2);
				wiimotes[i]->state = STATE_HID_INTERRUPT_CONFIGURATION;
				break;
			}
			return;
		}
	}
}

void storeL2CapMessageIdOfHidControlConfigurationRequest(BYTE* configurationRequest)
{
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->connectionHandle[0] == configurationRequest[5])
		{
			wiimotes[i]->hidControlConfRequestMessageId = configurationRequest[14];
			if (verbose) printf("%d: Store HID-Control Configuration request message id=%d\n", wiimotes[i]->id, wiimotes[i]->hidControlConfRequestMessageId);
			return;
		}
	}
}

void storeL2CapMessageIdOfHidInterruptConfigurationRequest(BYTE* configurationRequest)
{
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->connectionHandle[0] == configurationRequest[5])
		{
			wiimotes[i]->hidInterruptConfRequestMessageId = configurationRequest[14];
			if (verbose) printf("%d: Store HID-Interrupt Configuration request message id=%d\n", wiimotes[i]->id, wiimotes[i]->hidInterruptConfRequestMessageId);
			return;
		}
	}
}

void removeL2CapChannel(BYTE* disconnectionResponse)
{
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->connectionHandle[0] == disconnectionResponse[5])
		{
			switch (wiimotes[i]->state)
			{
			case STATE_HID_CONTROL_DISCONNECTION:
				if (verbose) printf("%d: Received L2CAP HID-Control Disconnection OK\n", wiimotes[i]->id);
				memset(wiimotes[i]->hidControlChannel, 0, 2);
				wiimotes[i]->state = STATE_HID_INTERRUPT_DISCONNECTION;
				break;
			case STATE_HID_INTERRUPT_DISCONNECTION:
				if (verbose) printf("%d: Received L2CAP HID-Interrupt Disconnection OK\n", wiimotes[i]->id);
				memset(wiimotes[i]->hidInterruptChannel, 0, 2);
				wiimotes[i]->state = STATE_FINISHED;
				break;
			}
			return;
		}
	}
}

void processL2CapConfigurationResponse(BYTE* configurationResponse)
{
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->connectionHandle[0] == configurationResponse[5])
		{
			switch (wiimotes[i]->state)
			{
			case STATE_HID_CONTROL_CONFIGURATION:
				if (verbose) printf("%d: Received HID-Control configuration response\n", wiimotes[i]->id);
				wiimotes[i]->state = STATE_HID_INTERRUPT_CONNECTION;
				break;
			case STATE_HID_INTERRUPT_CONFIGURATION:
				if (verbose) printf("%d: Received HID-Interrupt configuration response\n", wiimotes[i]->id);
				wiimotes[i]->state = STATE_WIIMOTE_SET_LEDS;
				break;
			}
			return;
		}
	}
}

void printInputReport(int i, BYTE* inputReport)
{

	// Print a line only when the state of a button change
	if (wiimotes[i]->buttons[0] != (inputReport[15] & 0x9F)
		|| wiimotes[i]->buttons[1] != (inputReport[16] & 0x9F))
	{
		// message rate
		tickCount = GetTickCount();
		printf("%4d msg/s", (msgCount - previousMsgCount) * 1000 / (tickCount - previousTickCount));
		previousTickCount = tickCount;
		previousMsgCount = msgCount;

		printf(" %d:", wiimotes[i]->id);
		if ((inputReport[15] & 0x01) > 0) printf("<");
		else printf(" ");
		if ((inputReport[15] & 0x02) > 0) printf(">");
		else printf(" ");
		if ((inputReport[15] & 0x04) > 0) printf("v");
		else printf(" ");
		if ((inputReport[15] & 0x08) > 0) printf("^");
		else printf(" ");
		if ((inputReport[15] & 0x10) > 0) printf("+");
		else printf(" ");
		if ((inputReport[16] & 0x01) > 0) printf("2");
		else printf(" ");
		if ((inputReport[16] & 0x02) > 0) printf("1");
		else printf(" ");
		if ((inputReport[16] & 0x04) > 0) printf("B");
		else printf(" ");
		if ((inputReport[16] & 0x08) > 0) printf("A");
		else printf(" ");
		if ((inputReport[16] & 0x10) > 0) printf("-");
		else printf(" ");
		if ((inputReport[16] & 0x80) > 0) printf("H");
		else printf(" ");

		DWORD X = (inputReport[17] << 2) + ((inputReport[15] & 0x60) >> 5) - 512;
		DWORD Y = (inputReport[18] << 2) + ((inputReport[16] & 0x20) >> 4) - 512;
		DWORD Z = (inputReport[19] << 2) + ((inputReport[16] & 0x40) >> 5) - 512;
		printf("%+04d %+04d %+04d", X, Y, Z);

		if (inputReport[14] == 0x37)
		{
			// Print IR camera data
			DWORD X1 = inputReport[20] + ((inputReport[22] & 0x30) << 4);
			DWORD Y1 = inputReport[21] + ((inputReport[22] & 0xC0) << 2);
			DWORD X2 = inputReport[23] + ((inputReport[22] & 0x03) << 8);
			DWORD Y2 = inputReport[24] + ((inputReport[22] & 0x0C) << 6);

			DWORD X3 = inputReport[25] + ((inputReport[27] & 0x30) << 4);
			DWORD Y3 = inputReport[26] + ((inputReport[27] & 0xC0) << 2);
			DWORD X4 = inputReport[28] + ((inputReport[27] & 0x03) << 8);
			DWORD Y4 = inputReport[29] + ((inputReport[27] & 0x0C) << 6);

			if (X1 == 1023 || Y1 == 1023)
				printf(" --empty--");
			else
				printf(" %04d %04d", X1, Y1);

			if (X2 == 1023 || Y2 == 1023)
				printf(" --empty--");
			else
				printf(" %04d %04d", X2, Y2);

			if (X3 == 1023 || Y3 == 1023)
				printf(" --empty--");
			else
				printf(" %04d %04d", X3, Y3);

			if (X4 == 1023 || Y4 == 1023)
				printf(" --empty--");
			else
				printf(" %04d %04d", X4, Y4);

		}

		printf("\n");
	}

	wiimotes[i]->buttons[0] = (inputReport[15] & 0x9F);
	wiimotes[i]->buttons[1] = (inputReport[16] & 0x9F);

}

void printInputReport(BYTE* inputReport)
{
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->connectionHandle[0] == inputReport[5])
		{
			if (inputReport[14] == 0x31 || inputReport[14] == 0x37)
			{
				printInputReport(i, inputReport);
			}

			return;
		}
	}
}

void updateCameraState(BYTE* acknowledgeOutputReport)
{
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->connectionHandle[0] == acknowledgeOutputReport[5])
		{
			if (verbose) printf("%d: Received Acknowledge output report 0x%02X, result=0x%02X\n", wiimotes[i]->id, acknowledgeOutputReport[17], acknowledgeOutputReport[18]);

			switch (wiimotes[i]->state)
			{
			case STATE_WIIMOTE_ACTIVATE_CAMERA_1:
				if (acknowledgeOutputReport[17] == 0x13 && acknowledgeOutputReport[18] == 0x00)
				{
					// 0x00 = success
					wiimotes[i]->state = STATE_WIIMOTE_ACTIVATE_CAMERA_2;
				}
				break;
			case STATE_WIIMOTE_ACTIVATE_CAMERA_2:
				if (acknowledgeOutputReport[17] == 0x1A && acknowledgeOutputReport[18] == 0x00)
				{
					// 0x00 = success
					wiimotes[i]->state = STATE_WIIMOTE_START_CAMERA_CONFIGURATION;
				}
				break;
			case STATE_WIIMOTE_START_CAMERA_CONFIGURATION:
				if (acknowledgeOutputReport[17] == 0x16 && acknowledgeOutputReport[18] == 0x00)
				{
					// 0x00 = success
					wiimotes[i]->state = STATE_WIIMOTE_CONFIGURE_CAMERA_SENSIBILITY_1;
				}
				break;
			case STATE_WIIMOTE_CONFIGURE_CAMERA_SENSIBILITY_1:
				if (acknowledgeOutputReport[17] == 0x16 && acknowledgeOutputReport[18] == 0x00)
				{
					// 0x00 = success
					wiimotes[i]->state = STATE_WIIMOTE_CONFIGURE_CAMERA_SENSIBILITY_2;
				}
				break;
			case STATE_WIIMOTE_CONFIGURE_CAMERA_SENSIBILITY_2:
				if (acknowledgeOutputReport[17] == 0x16 && acknowledgeOutputReport[18] == 0x00)
				{
					// 0x00 = success
					wiimotes[i]->state = STATE_WIIMOTE_CONFIGURE_CAMERA_MODE;
				}
				break;
			case STATE_WIIMOTE_CONFIGURE_CAMERA_MODE:
				if (acknowledgeOutputReport[17] == 0x16 && acknowledgeOutputReport[18] == 0x00)
				{
					// 0x00 = success
					wiimotes[i]->state = STATE_WIIMOTE_END_CAMERA_CONFIGURATION;
				}
				break;
			case STATE_WIIMOTE_END_CAMERA_CONFIGURATION:
				if (acknowledgeOutputReport[17] == 0x16 && acknowledgeOutputReport[18] == 0x00)
				{
					// 0x00 = success
					wiimotes[i]->state = STATE_WIIMOTE_SET_DATA_REPORTING_MODE_WITH_CAMERA;
				}
				break;
			}
			return;
		}
	}
}

void updateStatus(BYTE* statusReport)
{
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->connectionHandle[0] == statusReport[5])
		{
			if (verbose) printf("%d: Received status report.\n", wiimotes[i]->id);
			memcpy(wiimotes[i]->status, statusReport + 17, 4);
			return;
		}
	}
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

	if (verbose) printf("Start listening to ACL Data...\n");
	while (readLoop_continue)
	{
		success = DeviceIoControl(hciControlDeviceAcl, IOCTL_CONTROL_READ_HCI, readAcl_inputBuffer, 4, readAcl_outputBuffer, 1030, &returned, NULL);
		if (success)
		{
			if (returned == 25 && readAcl_outputBuffer[13] == 0x03 && memcmp(readAcl_outputBuffer + 21, resultSuccess, 4) == 0)
			{
				// CONNECTION_RESPONSE (0x03)
				storeL2CapChannel(readAcl_outputBuffer);
				SetEvent(hEventCmdFinished);
			}
			else if (returned >= 23 && readAcl_outputBuffer[13] == 0x05)
			{
				// CONFIGURATION_RESPONSE (0x05) to our CONFIGURATION_REQUEST, 
				processL2CapConfigurationResponse(readAcl_outputBuffer);
				SetEvent(hEventCmdFinished);
			}
			else if ((returned == 25 || returned == 21) && readAcl_outputBuffer[13] == 0x04 && memcmp(readAcl_outputBuffer + 17, cidLocalHidControl, 2) == 0)
			{
				// We must respond to the CONFIGURATION_REQUEST (0x04) of the remote device
				//with the same message id.
				storeL2CapMessageIdOfHidControlConfigurationRequest(readAcl_outputBuffer);
			}
			else if ((returned == 25 || returned == 21) && readAcl_outputBuffer[13] == 0x04 && memcmp(readAcl_outputBuffer + 17, cidLocalHidInterrupt, 2) == 0)
			{
				// We must respond to the CONFIGURATION_REQUEST (0x04) of the remote device
				// with the same message id
				storeL2CapMessageIdOfHidInterruptConfigurationRequest(readAcl_outputBuffer);
			}
			else if (returned == 21 && readAcl_outputBuffer[13] == 0x07)
			{
				// DISCONNECTION_RESPONSE (0x07)
				removeL2CapChannel(readAcl_outputBuffer);
				SetEvent(hEventCmdFinished);
			}
			else if (returned > 14 && readAcl_outputBuffer[13] == 0xA1 && (readAcl_outputBuffer[14] == 0x31 || readAcl_outputBuffer[14] == 0x37) && memcmp(readAcl_outputBuffer + 11, cidLocalHidInterrupt, 2) == 0)
			{
				// INPUT REPORT (0xA1 0x31/0x37)
				printInputReport(readAcl_outputBuffer);
			}
			else if (returned == 19 && readAcl_outputBuffer[13] == 0xA1 && readAcl_outputBuffer[14] == 0x22 && memcmp(readAcl_outputBuffer + 11, cidLocalHidInterrupt, 2) == 0)
			{
				// Acknowledge output report, return function result (0xA1 0x22)
				updateCameraState(readAcl_outputBuffer);
				SetEvent(hEventCmdFinished);
			}
			else if (returned == 21 && readAcl_outputBuffer[13] == 0xA1 && readAcl_outputBuffer[14] == 0x20 && memcmp(readAcl_outputBuffer + 11, cidLocalHidInterrupt, 2) == 0)
			{
				// Status report (0xA1 0x20)
				updateStatus(readAcl_outputBuffer);
				SetEvent(hEventCmdFinished);
			}
			else
			{
				if (verbose) printf("Received: unkown ACL message\n");
				if (verbose) printBuffer2HexString(readAcl_outputBuffer, returned);
			}
		}
		else
		{
			printf("Failed to send IOCTL_CONTROL_READ_HCI! 0x%X\n", GetLastError());
		}
		msgCount++;
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

void disconnectWiimote(int i)
{
	DWORD returned;
	BYTE* cmd_inputBuffer;
	BYTE* cmd_outputBuffer;
	BOOL success;

	// Disconnect command
	cmd_inputBuffer = (BYTE*)malloc(11);
	cmd_inputBuffer[0] = 0x06; // Length of the IOCTL message
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x01; // Command
	cmd_inputBuffer[5] = 0x06; // DISCONNECT
	cmd_inputBuffer[6] = 0x04;
	cmd_inputBuffer[7] = 0x03;
	cmd_inputBuffer[8] = wiimotes[i]->connectionHandle[0];
	cmd_inputBuffer[9] = wiimotes[i]->connectionHandle[1];
	cmd_inputBuffer[10] = 0x13; // CONNECTION TERMINATED BY USER
	cmd_outputBuffer = (BYTE*)malloc(4);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 11, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
	}
	else
	{
		if (verbose) printf("%d: BT disconnection\n", wiimotes[i]->id);
	}
	free(cmd_inputBuffer);
	free(cmd_outputBuffer);

	// Wait for disconnection complete event
	int maxWait = 4;
	while (wiimotes[i]->state != STATE_FINISHED && maxWait > 0)
	{
		Sleep(500);
		maxWait--;
	}
}

int connectWiimotes()
{
	DWORD returned;
	BYTE* cmd_inputBuffer;
	BYTE* cmd_outputBuffer;
	BOOL success;
	int nbConnected = 0;

	// Execute the connection and configuration steps for all the newly discovered Wiimotes.
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->state == STATE_BT_CONNECTION)
		{

			// State: connect to the remote device. 
			// Connection command
			cmd_inputBuffer = (BYTE*)malloc(21);
			cmd_inputBuffer[0] = 0x10; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x01; // Command
			cmd_inputBuffer[5] = 0x05; // CONNECT
			cmd_inputBuffer[6] = 0x04;
			cmd_inputBuffer[7] = 0x0D;
			memcpy(cmd_inputBuffer + 8, wiimotes[i]->btAddr, 6);
			cmd_inputBuffer[14] = 0x18; // packetType =  DH3,DM5,DH1,DH5,DM3,DM1 
			cmd_inputBuffer[15] = 0xCC; // -
			cmd_inputBuffer[16] = wiimotes[i]->pageScanRepetitionMode;
			cmd_inputBuffer[17] = 0x00; // Reserved
			memcpy(cmd_inputBuffer + 18, wiimotes[i]->clockOffset, 2);
			cmd_inputBuffer[19] |= 0x80; // set bit 15: clockOffset is valid
			cmd_inputBuffer[20] = 0x01; // allowRoleSwitch:ALLOWED 
			cmd_outputBuffer = (BYTE*)malloc(4);
			wiimotes[i]->state = STATE_BT_CONNECTION;
			while (mainLoop_continue && wiimotes[i]->state == STATE_BT_CONNECTION)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: Create BT connection\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}

				// Wait for the end of the Connection command
				WaitForSingleObject(hEventCmdFinished, 1000);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			// State: Open a "HID Control" channel with the remote device. 
			// Connection request command
			cmd_inputBuffer = (BYTE*)malloc(21);
			cmd_inputBuffer[0] = 0x10; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
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
			wiimotes[i]->state = STATE_HID_CONTROL_CONNECTION;
			int maxRetries = 4;
			while (mainLoop_continue && wiimotes[i]->state == STATE_HID_CONTROL_CONNECTION && maxRetries > 0)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: Create L2CAP HID_control connection\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}

				// Wait for the end of the Connection command
				WaitForSingleObject(hEventCmdFinished, 1000);

				maxRetries--;
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);
			if (maxRetries == 0 && wiimotes[i]->state == STATE_HID_CONTROL_CONNECTION)
			{
				printf("Something wrong is happening. Perhaps the Bluetooth controller is in an incorrect state. Please reboot the phone and try again.\n");
				mainLoop_continue = FALSE;
				break;
			}

			// State: Configure the "HID Control" channel with the remote device. 
			// Configuration request command (No options)
			cmd_inputBuffer = (BYTE*)malloc(21);
			cmd_inputBuffer[0] = 0x10; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
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
			cmd_inputBuffer[17] = wiimotes[i]->hidControlChannel[0];
			cmd_inputBuffer[18] = wiimotes[i]->hidControlChannel[1];
			cmd_inputBuffer[19] = 0x00; // Flags
			cmd_inputBuffer[20] = 0x00; // -
			cmd_outputBuffer = (BYTE*)malloc(4);
			wiimotes[i]->state = STATE_HID_CONTROL_CONFIGURATION;
			while (mainLoop_continue && wiimotes[i]->state == STATE_HID_CONTROL_CONFIGURATION)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: Configure L2CAP HID_control connection\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}

				// Wait for the end of the Configuration request command
				WaitForSingleObject(hEventCmdFinished, 1000);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			// Wait for the Configuration request
			// Notes: This request can arrive just after the connection response,
			// but we first configure the channel on our side before responding to this request.
			while (mainLoop_continue && wiimotes[i]->hidControlConfRequestMessageId == 0)
			{
				Sleep(100);
			}
			if (wiimotes[i]->hidControlConfRequestMessageId != 0)
			{
				// We have received the configuration request.
				// State: Respond to finish the configuration of the "HID Control" channel with the remote device. 
				// Configuration response command (success)
				cmd_inputBuffer = (BYTE*)malloc(27);
				cmd_inputBuffer[0] = 0x16; // Length of the IOCTL message
				cmd_inputBuffer[1] = 0x00;
				cmd_inputBuffer[2] = 0x00;
				cmd_inputBuffer[3] = 0x00;
				cmd_inputBuffer[4] = 0x02; // ACL data
				cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
				cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
				cmd_inputBuffer[7] = 0x12; // Length of the ACL message
				cmd_inputBuffer[8] = 0x00;
				cmd_inputBuffer[9] = 0x0E;  // Length of the L2CAP message
				cmd_inputBuffer[10] = 0x00; // -
				cmd_inputBuffer[11] = 0x01; // Signaling channel
				cmd_inputBuffer[12] = 0x00; // -
				cmd_inputBuffer[13] = 0x05; // CONFIGURATION_REPONSE
				cmd_inputBuffer[14] = wiimotes[i]->hidControlConfRequestMessageId;
				cmd_inputBuffer[15] = 0x0A; // Length of the command parameters (CID+Flags)
				cmd_inputBuffer[16] = 0x00; // -
				cmd_inputBuffer[17] = wiimotes[i]->hidControlChannel[0];
				cmd_inputBuffer[18] = wiimotes[i]->hidControlChannel[1];
				cmd_inputBuffer[19] = 0x00; // Flags
				cmd_inputBuffer[20] = 0x00; // -
				cmd_inputBuffer[21] = 0x00; // Result = success
				cmd_inputBuffer[22] = 0x00; // -
				cmd_inputBuffer[23] = 0x01;
				cmd_inputBuffer[24] = 0x02;
				cmd_inputBuffer[25] = 0xA0;
				cmd_inputBuffer[26] = 0x02;
				cmd_outputBuffer = (BYTE*)malloc(4);
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 27, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: Finish configuration L2CAP HID-control connection, message id=%d\n", wiimotes[i]->id, wiimotes[i]->hidControlConfRequestMessageId);
				}
				free(cmd_inputBuffer);
				free(cmd_outputBuffer);

				wiimotes[i]->hidControlConfRequestMessageId = 0;
			}

			// State: Open a "HID Interrupt" channel with the remote device. 
			// Connection request command
			cmd_inputBuffer = (BYTE*)malloc(21);
			cmd_inputBuffer[0] = 0x10; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
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
			wiimotes[i]->state = STATE_HID_INTERRUPT_CONNECTION;
			while (mainLoop_continue && wiimotes[i]->state == STATE_HID_INTERRUPT_CONNECTION)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: Create L2CAP HID_interrupt connection\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}

				// Wait for the end of the Connection command
				WaitForSingleObject(hEventCmdFinished, 1000);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			// State: Configure the "HID Interrupt" channel with the remote device. 
			// Configuration request command (No options)
			cmd_inputBuffer = (BYTE*)malloc(21);
			cmd_inputBuffer[0] = 0x10; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
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
			cmd_inputBuffer[17] = wiimotes[i]->hidInterruptChannel[0];
			cmd_inputBuffer[18] = wiimotes[i]->hidInterruptChannel[1];
			cmd_inputBuffer[19] = 0x00; // Flags
			cmd_inputBuffer[20] = 0x00; // -
			cmd_outputBuffer = (BYTE*)malloc(4);
			wiimotes[i]->state = STATE_HID_INTERRUPT_CONFIGURATION;
			while (mainLoop_continue && wiimotes[i]->state == STATE_HID_INTERRUPT_CONFIGURATION)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: Configure L2CAP HID_interrupt connection\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}

				// Wait for the end of the Configuration request command
				WaitForSingleObject(hEventCmdFinished, 1000);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			// Wait for the Configuration request
			// Notes: This request can arrive just after the connection response,
			// but we first configure the channel on our side before responding to this request.
			while (mainLoop_continue && wiimotes[i]->hidInterruptConfRequestMessageId == 0)
			{
				Sleep(100);
			}
			if (mainLoop_continue && wiimotes[i]->hidInterruptConfRequestMessageId != 0)
			{
				// We have received the configuration request.

				// State: Respond to finish the configuration of the "HID Interrupt" channel with the remote device. 
				// Configuration response command (success)
				cmd_inputBuffer = (BYTE*)malloc(27);
				cmd_inputBuffer[0] = 0x16; // Length of the IOCTL message
				cmd_inputBuffer[1] = 0x00;
				cmd_inputBuffer[2] = 0x00;
				cmd_inputBuffer[3] = 0x00;
				cmd_inputBuffer[4] = 0x02; // ACL data
				cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
				cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
				cmd_inputBuffer[7] = 0x12; // Length of the ACL message
				cmd_inputBuffer[8] = 0x00;
				cmd_inputBuffer[9] = 0x0E;  // Length of the L2CAP message
				cmd_inputBuffer[10] = 0x00; // -
				cmd_inputBuffer[11] = 0x01; // Signaling channel
				cmd_inputBuffer[12] = 0x00; // -
				cmd_inputBuffer[13] = 0x05; // CONFIGURATION_REPONSE
				cmd_inputBuffer[14] = wiimotes[i]->hidInterruptConfRequestMessageId;
				cmd_inputBuffer[15] = 0x0A; // Length of the command parameters (CID+Flags)
				cmd_inputBuffer[16] = 0x00; // -
				cmd_inputBuffer[17] = wiimotes[i]->hidInterruptChannel[0];
				cmd_inputBuffer[18] = wiimotes[i]->hidInterruptChannel[1];
				cmd_inputBuffer[19] = 0x00; // Flags
				cmd_inputBuffer[20] = 0x00; // -
				cmd_inputBuffer[21] = 0x00; // Result = success
				cmd_inputBuffer[22] = 0x00; // -
				cmd_inputBuffer[23] = 0x01;
				cmd_inputBuffer[24] = 0x02;
				cmd_inputBuffer[25] = 0xA0;
				cmd_inputBuffer[26] = 0x02;
				cmd_outputBuffer = (BYTE*)malloc(4);
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 27, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: Finish configuration L2CAP HID-interrupt connection, message id=%d\n", wiimotes[i]->id, wiimotes[i]->hidInterruptConfRequestMessageId);
				}
				free(cmd_inputBuffer);
				free(cmd_outputBuffer);

				wiimotes[i]->hidInterruptConfRequestMessageId = 0;
			}

			if (mainLoop_continue)
			{
				// State: Query Wiimote status. 
				cmd_inputBuffer = (BYTE*)malloc(16);
				cmd_inputBuffer[0] = 0x0B; // Length of the IOCTL message
				cmd_inputBuffer[1] = 0x00;
				cmd_inputBuffer[2] = 0x00;
				cmd_inputBuffer[3] = 0x00;
				cmd_inputBuffer[4] = 0x02; // ACL data
				cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
				cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
				cmd_inputBuffer[7] = 0x07; // Length of the ACL message
				cmd_inputBuffer[8] = 0x00; // -
				cmd_inputBuffer[9] = 0x03;  // Length of the L2CAP message
				cmd_inputBuffer[10] = 0x00; // -
				cmd_inputBuffer[11] = wiimotes[i]->hidInterruptChannel[0];
				cmd_inputBuffer[12] = wiimotes[i]->hidInterruptChannel[1];
				cmd_inputBuffer[13] = 0xA2; // Output report
				cmd_inputBuffer[14] = 0x15; // Status Information Request
				cmd_inputBuffer[15] = 0x00; // Turn off rumble
				cmd_outputBuffer = (BYTE*)malloc(4);

				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 16, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: Query Wiimote status\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}

				// Wait for Status reporting
				WaitForSingleObject(hEventCmdFinished, 1000);

				free(cmd_inputBuffer);
				free(cmd_outputBuffer);

				if (wiimotes[i]->status[3] == 0x00)
				{
					// Battery level was not updated -> It means the Status reporting was not received.
					printf("%d: L2CAP connection couldn't be established. Perhaps it's a new RVL-CNT-01-TR model. Try again with the 'sync button'.\n", wiimotes[i]->id);
					
					disconnectWiimote(i);

					free(wiimotes[i]);
					wiimotes[i] = NULL;
					break;
				}
			}

			if (mainLoop_continue)
			{
				// State: Set the LEDs of the Wiimote. 
				cmd_inputBuffer = (BYTE*)malloc(16);
				cmd_inputBuffer[0] = 0x0B; // Length of the IOCTL message
				cmd_inputBuffer[1] = 0x00;
				cmd_inputBuffer[2] = 0x00;
				cmd_inputBuffer[3] = 0x00;
				cmd_inputBuffer[4] = 0x02; // ACL data
				cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
				cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
				cmd_inputBuffer[7] = 0x07; // Length of the ACL message
				cmd_inputBuffer[8] = 0x00; // -
				cmd_inputBuffer[9] = 0x03;  // Length of the L2CAP message
				cmd_inputBuffer[10] = 0x00; // -
				cmd_inputBuffer[11] = wiimotes[i]->hidInterruptChannel[0];
				cmd_inputBuffer[12] = wiimotes[i]->hidInterruptChannel[1];
				cmd_inputBuffer[13] = 0xA2; // Output report
				cmd_inputBuffer[14] = 0x11; // Player LEDs
				// Light the LEDs corresponding to the order of detection of the Wiimote
				switch (wiimotes[i]->id)
				{
				case 1:
					cmd_inputBuffer[15] = 0x10; // Set LED 1 
					break;
				case 2:
					cmd_inputBuffer[15] = 0x20; // Set LED 2 
					break;
				case 3:
					cmd_inputBuffer[15] = 0x40; // Set LED 3 
					break;
				case 4:
					cmd_inputBuffer[15] = 0x80; // Set LED 4 
					break;
				case 5:
					cmd_inputBuffer[15] = 0x90; // Set LED 1+4 
					break;
				case 6:
					cmd_inputBuffer[15] = 0xA0; // Set LED 2+4 
					break;
				case 7:
					cmd_inputBuffer[15] = 0xC0; // Set LED 3+4 
					break;
				}
				cmd_outputBuffer = (BYTE*)malloc(4);
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 16, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: Set LEDs\n", wiimotes[i]->id);
					printf("Wiimote #%d connected\n", wiimotes[i]->id);
					nbConnected++;
					ResetEvent(hEventCmdFinished);
					wiimotes[i]->state = STATE_WIIMOTE_SET_DATA_REPORTING_MODE_WITHOUT_CAMERA;
				}
				free(cmd_inputBuffer);
				free(cmd_outputBuffer);
			}
		}
	}

	return nbConnected;
}

char askChoice()
{
	char choice = getchar();
	// Flush input buffer
	char unwantedChar;
	while ((unwantedChar = getchar()) != EOF && unwantedChar != '\n');
	if (verbose) printf("choice=%c\n", choice);
	return choice;
}

void activateAndConfigureCamera()
{
	DWORD returned;
	BYTE* cmd_inputBuffer;
	BYTE* cmd_outputBuffer;
	BOOL success;

	// Execute the activation and configuration steps for all the newly discovered Wiimotes.
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->state != STATE_WIIMOTE_READ_INPUTS)
		{
			// State: set data reporting mode without camera
			cmd_inputBuffer = (BYTE*)malloc(17);
			cmd_inputBuffer[0] = 0x0C; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
			cmd_inputBuffer[7] = 0x08; // Length of the ACL message
			cmd_inputBuffer[8] = 0x00; // -
			cmd_inputBuffer[9] = 0x04;  // Length of the L2CAP message
			cmd_inputBuffer[10] = 0x00; // -
			cmd_inputBuffer[11] = wiimotes[i]->hidInterruptChannel[0];
			cmd_inputBuffer[12] = wiimotes[i]->hidInterruptChannel[1];
			cmd_inputBuffer[13] = 0xA2; // Output report
			cmd_inputBuffer[14] = 0x12; // Set data reporting mode
			cmd_inputBuffer[15] = 0x00; // Report only when data has changed
			cmd_inputBuffer[16] = 0x31; // Core Buttons and Accelerometer
			cmd_outputBuffer = (BYTE*)malloc(4);
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 17, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
			}
			else
			{
				if (verbose) printf("%d: Mode 0x31\n", wiimotes[i]->id);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			// State: first activation of the IR camera
			cmd_inputBuffer = (BYTE*)malloc(16);
			cmd_inputBuffer[0] = 0x0B; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
			cmd_inputBuffer[7] = 0x07; // Length of the ACL message
			cmd_inputBuffer[8] = 0x00; // -
			cmd_inputBuffer[9] = 0x03;  // Length of the L2CAP message
			cmd_inputBuffer[10] = 0x00; // -
			cmd_inputBuffer[11] = wiimotes[i]->hidInterruptChannel[0];
			cmd_inputBuffer[12] = wiimotes[i]->hidInterruptChannel[1];
			cmd_inputBuffer[13] = 0xA2; // Output report
			cmd_inputBuffer[14] = 0x13; // IR Camera Enable
			cmd_inputBuffer[15] = 0x06; // Activate + request for acknowledgement
			cmd_outputBuffer = (BYTE*)malloc(4);
			wiimotes[i]->state = STATE_WIIMOTE_ACTIVATE_CAMERA_1;
			while (mainLoop_continue && wiimotes[i]->state == STATE_WIIMOTE_ACTIVATE_CAMERA_1)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 16, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					ResetEvent(hEventCmdFinished);
					if (verbose) printf("%d: Enable IR Pixel Clock\n", wiimotes[i]->id);
				}
				// Wait for the end of the command
				WaitForSingleObject(hEventCmdFinished, 1000);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			// State: second activation of the IR camera
			cmd_inputBuffer = (BYTE*)malloc(16);
			cmd_inputBuffer[0] = 0x0B; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
			cmd_inputBuffer[7] = 0x07; // Length of the ACL message
			cmd_inputBuffer[8] = 0x00; // -
			cmd_inputBuffer[9] = 0x03;  // Length of the L2CAP message
			cmd_inputBuffer[10] = 0x00; // -
			cmd_inputBuffer[11] = wiimotes[i]->hidInterruptChannel[0];
			cmd_inputBuffer[12] = wiimotes[i]->hidInterruptChannel[1];
			cmd_inputBuffer[13] = 0xA2; // Output report
			cmd_inputBuffer[14] = 0x1A; // IR Camera Enable 2
			cmd_inputBuffer[15] = 0x06; // Activate + request for acknowledgement
			cmd_outputBuffer = (BYTE*)malloc(4);
			wiimotes[i]->state = STATE_WIIMOTE_ACTIVATE_CAMERA_2;
			while (mainLoop_continue && wiimotes[i]->state == STATE_WIIMOTE_ACTIVATE_CAMERA_2)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 16, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					ResetEvent(hEventCmdFinished);
					if (verbose) printf("%d: Enable IR Logic\n", wiimotes[i]->id);
				}
				// Wait for the end of the command
				WaitForSingleObject(hEventCmdFinished, 1000);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			// State: Camera configuration, write 0x08 to register 0xb00030 (Start configuration)
			cmd_inputBuffer = (BYTE*)malloc(36);
			cmd_inputBuffer[0] = 0x1F; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
			cmd_inputBuffer[7] = 0x1B; // Length of the ACL message
			cmd_inputBuffer[8] = 0x00; // -
			cmd_inputBuffer[9] = 0x17;  // Length of the L2CAP message
			cmd_inputBuffer[10] = 0x00; // -
			cmd_inputBuffer[11] = wiimotes[i]->hidInterruptChannel[0];
			cmd_inputBuffer[12] = wiimotes[i]->hidInterruptChannel[1];
			cmd_inputBuffer[13] = 0xA2; // Output report
			cmd_inputBuffer[14] = 0x16; // Write memory or registers
			cmd_inputBuffer[15] = 0x04; // Write to registers
			cmd_inputBuffer[16] = 0xB0; // Offset
			cmd_inputBuffer[17] = 0x00; // -
			cmd_inputBuffer[18] = 0x30; // -
			cmd_inputBuffer[19] = 0x01; // Size of the value
			cmd_inputBuffer[20] = 0x01;	// Value padded to 16 bytes (Start configuration)
			cmd_inputBuffer[21] = 0x00; // -
			cmd_inputBuffer[22] = 0x00; // -
			cmd_inputBuffer[23] = 0x00; // -
			cmd_inputBuffer[24] = 0x00; // -
			cmd_inputBuffer[25] = 0x00; // -
			cmd_inputBuffer[26] = 0x00; // -
			cmd_inputBuffer[27] = 0x00; // -
			cmd_inputBuffer[28] = 0x00; // -
			cmd_inputBuffer[29] = 0x00; // -
			cmd_inputBuffer[30] = 0x00; // -
			cmd_inputBuffer[31] = 0x00; // -
			cmd_inputBuffer[32] = 0x00; // -
			cmd_inputBuffer[33] = 0x00; // -
			cmd_inputBuffer[34] = 0x00; // -
			cmd_inputBuffer[35] = 0x00; // -
			cmd_outputBuffer = (BYTE*)malloc(4);
			wiimotes[i]->state = STATE_WIIMOTE_START_CAMERA_CONFIGURATION;
			while (mainLoop_continue && wiimotes[i]->state == STATE_WIIMOTE_START_CAMERA_CONFIGURATION)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 36, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: IR Camera Start configuration\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}
				// Wait for the end of the command
				WaitForSingleObject(hEventCmdFinished, 1000);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			// State: Camera configuration, write Sensitivity Block 1 to registers at 0xb00000
			cmd_inputBuffer = (BYTE*)malloc(36);
			cmd_inputBuffer[0] = 0x1F; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
			cmd_inputBuffer[7] = 0x1B; // Length of the ACL message
			cmd_inputBuffer[8] = 0x00; // -
			cmd_inputBuffer[9] = 0x17;  // Length of the L2CAP message
			cmd_inputBuffer[10] = 0x00; // -
			cmd_inputBuffer[11] = wiimotes[i]->hidInterruptChannel[0];
			cmd_inputBuffer[12] = wiimotes[i]->hidInterruptChannel[1];
			cmd_inputBuffer[13] = 0xA2; // Output report
			cmd_inputBuffer[14] = 0x16; // Write memory or registers
			cmd_inputBuffer[15] = 0x04; // Write to registers
			cmd_inputBuffer[16] = 0xB0; // Offset
			cmd_inputBuffer[17] = 0x00; // -
			cmd_inputBuffer[18] = 0x00; // -
			cmd_inputBuffer[19] = 0x09; // Size of the value
			cmd_inputBuffer[20] = 0x02;	// Value padded to 16 bytes (Sensitivity Wii level 3)
			cmd_inputBuffer[21] = 0x00; // -
			cmd_inputBuffer[22] = 0x00; // -
			cmd_inputBuffer[23] = 0x71; // -
			cmd_inputBuffer[24] = 0x01; // -
			cmd_inputBuffer[25] = 0x00; // -
			cmd_inputBuffer[26] = 0xAA; // -
			cmd_inputBuffer[27] = 0x00; // -
			cmd_inputBuffer[28] = 0x64; // -
			cmd_inputBuffer[29] = 0x00; // -
			cmd_inputBuffer[30] = 0x00; // -
			cmd_inputBuffer[31] = 0x00; // -
			cmd_inputBuffer[32] = 0x00; // -
			cmd_inputBuffer[33] = 0x00; // -
			cmd_inputBuffer[34] = 0x00; // -
			cmd_inputBuffer[35] = 0x00; // -
			cmd_outputBuffer = (BYTE*)malloc(4);
			wiimotes[i]->state = STATE_WIIMOTE_CONFIGURE_CAMERA_SENSIBILITY_1;
			while (mainLoop_continue && wiimotes[i]->state == STATE_WIIMOTE_CONFIGURE_CAMERA_SENSIBILITY_1)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 36, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: IR Camera Sensitivity Block 1\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}
				// Wait for the end of the command
				WaitForSingleObject(hEventCmdFinished, 1000);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			// State: Camera configuration, write Sensitivity Block 2 to registers at 0xb0001a
			cmd_inputBuffer = (BYTE*)malloc(36);
			cmd_inputBuffer[0] = 0x1F; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
			cmd_inputBuffer[7] = 0x1B; // Length of the ACL message
			cmd_inputBuffer[8] = 0x00; // -
			cmd_inputBuffer[9] = 0x17;  // Length of the L2CAP message
			cmd_inputBuffer[10] = 0x00; // -
			cmd_inputBuffer[11] = wiimotes[i]->hidInterruptChannel[0];
			cmd_inputBuffer[12] = wiimotes[i]->hidInterruptChannel[1];
			cmd_inputBuffer[13] = 0xA2; // Output report
			cmd_inputBuffer[14] = 0x16; // Write memory or registers
			cmd_inputBuffer[15] = 0x04; // Write to registers
			cmd_inputBuffer[16] = 0xB0; // Offset
			cmd_inputBuffer[17] = 0x00; // -
			cmd_inputBuffer[18] = 0x1A; // -
			cmd_inputBuffer[19] = 0x02; // Size of the value
			cmd_inputBuffer[20] = 0x63;	// Value padded to 16 bytes (Sensitivity Wii level 3)
			cmd_inputBuffer[21] = 0x03; // -
			cmd_inputBuffer[22] = 0x00; // -
			cmd_inputBuffer[23] = 0x00; // -
			cmd_inputBuffer[24] = 0x00; // -
			cmd_inputBuffer[25] = 0x00; // -
			cmd_inputBuffer[26] = 0x00; // -
			cmd_inputBuffer[27] = 0x00; // -
			cmd_inputBuffer[28] = 0x00; // -
			cmd_inputBuffer[29] = 0x00; // -
			cmd_inputBuffer[30] = 0x00; // -
			cmd_inputBuffer[31] = 0x00; // -
			cmd_inputBuffer[32] = 0x00; // -
			cmd_inputBuffer[33] = 0x00; // -
			cmd_inputBuffer[34] = 0x00; // -
			cmd_inputBuffer[35] = 0x00; // -

			cmd_outputBuffer = (BYTE*)malloc(4);
			wiimotes[i]->state = STATE_WIIMOTE_CONFIGURE_CAMERA_SENSIBILITY_2;
			while (mainLoop_continue && wiimotes[i]->state == STATE_WIIMOTE_CONFIGURE_CAMERA_SENSIBILITY_2)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 36, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: IR Camera Sensitivity Block 2\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}
				// Wait for the end of the command
				WaitForSingleObject(hEventCmdFinished, 1000);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			// State: Camera configuration, write Mode Number to register 0xb00033
			cmd_inputBuffer = (BYTE*)malloc(36);
			cmd_inputBuffer[0] = 0x1F; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
			cmd_inputBuffer[7] = 0x1B; // Length of the ACL message
			cmd_inputBuffer[8] = 0x00; // -
			cmd_inputBuffer[9] = 0x17;  // Length of the L2CAP message
			cmd_inputBuffer[10] = 0x00; // -
			cmd_inputBuffer[11] = wiimotes[i]->hidInterruptChannel[0];
			cmd_inputBuffer[12] = wiimotes[i]->hidInterruptChannel[1];
			cmd_inputBuffer[13] = 0xA2; // Output report
			cmd_inputBuffer[14] = 0x16; // Write memory or registers
			cmd_inputBuffer[15] = 0x04; // Write to registers
			cmd_inputBuffer[16] = 0xB0; // Offset
			cmd_inputBuffer[17] = 0x00; // -
			cmd_inputBuffer[18] = 0x33; // -
			cmd_inputBuffer[19] = 0x01; // Size of the value
			cmd_inputBuffer[20] = 0x01;	// Value padded to 16 bytes (Basic Mode)
			cmd_inputBuffer[21] = 0x00; // -
			cmd_inputBuffer[22] = 0x00; // -
			cmd_inputBuffer[23] = 0x00; // -
			cmd_inputBuffer[24] = 0x00; // -
			cmd_inputBuffer[25] = 0x00; // -
			cmd_inputBuffer[26] = 0x00; // -
			cmd_inputBuffer[27] = 0x00; // -
			cmd_inputBuffer[28] = 0x00; // -
			cmd_inputBuffer[29] = 0x00; // -
			cmd_inputBuffer[30] = 0x00; // -
			cmd_inputBuffer[31] = 0x00; // -
			cmd_inputBuffer[32] = 0x00; // -
			cmd_inputBuffer[33] = 0x00; // -
			cmd_inputBuffer[34] = 0x00; // -
			cmd_inputBuffer[35] = 0x00; // -
			cmd_outputBuffer = (BYTE*)malloc(4);
			wiimotes[i]->state = STATE_WIIMOTE_CONFIGURE_CAMERA_MODE;
			while (mainLoop_continue && wiimotes[i]->state == STATE_WIIMOTE_CONFIGURE_CAMERA_MODE)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 36, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: IR Camera Mode number\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}
				// Wait for the end of the command
				WaitForSingleObject(hEventCmdFinished, 1000);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			// State: Camera configuration, write 0x08 to register 0xb00030 (End configuration)
			cmd_inputBuffer = (BYTE*)malloc(36);
			cmd_inputBuffer[0] = 0x1F; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
			cmd_inputBuffer[7] = 0x1B; // Length of the ACL message
			cmd_inputBuffer[8] = 0x00; // -
			cmd_inputBuffer[9] = 0x17;  // Length of the L2CAP message
			cmd_inputBuffer[10] = 0x00; // -
			cmd_inputBuffer[11] = wiimotes[i]->hidInterruptChannel[0];
			cmd_inputBuffer[12] = wiimotes[i]->hidInterruptChannel[1];
			cmd_inputBuffer[13] = 0xA2; // Output report
			cmd_inputBuffer[14] = 0x16; // Write memory or registers
			cmd_inputBuffer[15] = 0x04; // Write to registers
			cmd_inputBuffer[16] = 0xB0; // Offset
			cmd_inputBuffer[17] = 0x00; // -
			cmd_inputBuffer[18] = 0x30; // -
			cmd_inputBuffer[19] = 0x01; // Size of the value
			cmd_inputBuffer[20] = 0x08;	// Value padded to 16 bytes (End configuration)
			cmd_inputBuffer[21] = 0x00; // -
			cmd_inputBuffer[22] = 0x00; // -
			cmd_inputBuffer[23] = 0x00; // -
			cmd_inputBuffer[24] = 0x00; // -
			cmd_inputBuffer[25] = 0x00; // -
			cmd_inputBuffer[26] = 0x00; // -
			cmd_inputBuffer[27] = 0x00; // -
			cmd_inputBuffer[28] = 0x00; // -
			cmd_inputBuffer[29] = 0x00; // -
			cmd_inputBuffer[30] = 0x00; // -
			cmd_inputBuffer[31] = 0x00; // -
			cmd_inputBuffer[32] = 0x00; // -
			cmd_inputBuffer[33] = 0x00; // -
			cmd_inputBuffer[34] = 0x00; // -
			cmd_inputBuffer[35] = 0x00; // -
			cmd_outputBuffer = (BYTE*)malloc(4);
			wiimotes[i]->state = STATE_WIIMOTE_END_CAMERA_CONFIGURATION;
			while (mainLoop_continue && wiimotes[i]->state == STATE_WIIMOTE_END_CAMERA_CONFIGURATION)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 36, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: IR Camera End configuration\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}
				// Wait for the end of the command
				WaitForSingleObject(hEventCmdFinished, 1000);
			}
			free(cmd_inputBuffer);
			free(cmd_outputBuffer);

			if (mainLoop_continue)
			{
				// Change report mode: add camera reading
				cmd_inputBuffer = (BYTE*)malloc(17);
				cmd_inputBuffer[0] = 0x0C; // Length of the IOCTL message
				cmd_inputBuffer[1] = 0x00;
				cmd_inputBuffer[2] = 0x00;
				cmd_inputBuffer[3] = 0x00;
				cmd_inputBuffer[4] = 0x02; // ACL data
				cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
				cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
				cmd_inputBuffer[7] = 0x08; // Length of the ACL message
				cmd_inputBuffer[8] = 0x00; // -
				cmd_inputBuffer[9] = 0x04;  // Length of the L2CAP message
				cmd_inputBuffer[10] = 0x00; // -
				cmd_inputBuffer[11] = wiimotes[i]->hidInterruptChannel[0];
				cmd_inputBuffer[12] = wiimotes[i]->hidInterruptChannel[1];
				cmd_inputBuffer[13] = 0xA2; // Output report
				cmd_inputBuffer[14] = 0x12; // Set data reporting mode
				cmd_inputBuffer[15] = 0x00; // Report only when data has changed
				cmd_inputBuffer[16] = 0x37; // Core Buttons and Accelerometer with 10 IR bytes and 6 Extension Bytes
				cmd_outputBuffer = (BYTE*)malloc(4);
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 17, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
				}
				else
				{
					if (verbose) printf("%d: Set Data Reporting Mode with camera\n", wiimotes[i]->id);
					if (verbose) printf("Wiimote #%d IR camera active\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
					wiimotes[i]->state = STATE_WIIMOTE_READ_INPUTS;
				}
				free(cmd_inputBuffer);
				free(cmd_outputBuffer);
			}
		}
	}
}

int mainLoop_run(BOOL _verbose)
{
	DWORD returned;
	BYTE* cmd_inputBuffer;
	BYTE* cmd_outputBuffer;
	BOOL success;
	int exit_status = EXIT_SUCCESS;
	int maxRetries;
	verbose = _verbose;

	mainLoop_continue = TRUE;
	readLoop_continue = TRUE;
	msgCount = 0;
	previousMsgCount = 0;
	previousTickCount = GetTickCount();
	tickCount = GetTickCount();

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
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
		printf("Check Bluetooth is enabled.\n");
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

	// Start "read ACL data" thread
	hThreadArray[1] = CreateThread(NULL, 0, readAclData, NULL, 0, NULL);

	// Reset command
	cmd_inputBuffer = (BYTE*)malloc(8);
	cmd_inputBuffer[0] = 0x03; // Length of the IOCTL message
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x01; // Command
	cmd_inputBuffer[5] = 0x03; // RESET
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
		if (verbose) printf("Reset\n");
		ResetEvent(hEventCmdFinished);
	}
	free(cmd_inputBuffer);
	free(cmd_outputBuffer);

	// Wait for the end of the Reset command
	WaitForSingleObject(hEventCmdFinished, 1000);

	printf("Press buttons 1 and 2 of the Wiimotes to put them in discoverable mode : the LEDs are blinking\n");

	// State: detect a nearby remote bluetooth device. 
	currentState = STATE_BT_INQUIRY;
	// Inquiry command
	cmd_inputBuffer = (BYTE*)malloc(13);
	cmd_inputBuffer[0] = 0x08; // Length of the IOCTL message
	cmd_inputBuffer[1] = 0x00;
	cmd_inputBuffer[2] = 0x00;
	cmd_inputBuffer[3] = 0x00;
	cmd_inputBuffer[4] = 0x01; // Command
	cmd_inputBuffer[5] = 0x01; // INQUIRY
	cmd_inputBuffer[6] = 0x04;
	cmd_inputBuffer[7] = 0x05;
	cmd_inputBuffer[8] = 0x00;  // Limited Inquiry Access Code (LIAC) 0x9E8B00 - Required to detect 3rd party Wiimote
	cmd_inputBuffer[9] = 0x8B;  // -
	cmd_inputBuffer[10] = 0x9E; // -
	cmd_inputBuffer[11] = 0x02; // Length x 1.28s
	cmd_inputBuffer[12] = 0x00; // No limit to the number of responses
	cmd_outputBuffer = (BYTE*)malloc(4);
	maxRetries = 4;
	while (mainLoop_continue)
	{
		success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 13, cmd_outputBuffer, 4, &returned, NULL);
		if (!success)
		{
			printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
			exit_status = EXIT_FAILURE;
		}
		else
		{
			if (verbose) printf("Start inquiry\n");
			ResetEvent(hEventCmdFinished);
		}

		Sleep(2560); // Length x 1280ms

		// Connect the newly discovered Wiimotes
		if (connectWiimotes() == 0 && maxRetries > 0)
		{
			// Retries several times when no newly Wiimotes are detected/connected.
			maxRetries--;
			continue;
		}
		maxRetries = 4;

		// Ask the user to continue Inquiry
		// "no" means the user doesn't want to connect additional Wiimotes
		printf("Continue Inquiry ? (y/n): ");
		char choice = askChoice();
		if (choice != 'y' && choice != 'Y') break;
	}
	free(cmd_inputBuffer);
	free(cmd_outputBuffer);

	// Count number of connecter wiimotes
	int nbOfWiimotes = 0;
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->state != STATE_FINISHED)
		{
			nbOfWiimotes++;
		}
	}
	if (nbOfWiimotes == 0)
	{
		printf("No Wiimote connected. Exiting...\n");
		mainLoop_continue = FALSE;
	}

	if (mainLoop_continue)
		activateAndConfigureCamera();

	// Read inputs
	while (mainLoop_continue)
	{
		Sleep(100);
	}

	// Execute the disconnections steps for all the connected Wiimotes.
	for (int i = 0; i < NUMBER_OF_WIIMOTES; i++)
	{
		if (wiimotes[i] != NULL && wiimotes[i]->state != STATE_FINISHED)
		{
			wiimotes[i]->state = STATE_HID_CONTROL_DISCONNECTION;

			// State: Disconnect "HID Control" channel
			cmd_inputBuffer = (BYTE*)malloc(21);
			cmd_inputBuffer[0] = 0x10; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
			cmd_inputBuffer[7] = 0x0C; // Length of the ACL message
			cmd_inputBuffer[8] = 0x00;
			cmd_inputBuffer[9] = 0x08;  // Length of the L2CAP message
			cmd_inputBuffer[10] = 0x00; // -
			cmd_inputBuffer[11] = 0x01; // Signaling channel
			cmd_inputBuffer[12] = 0x00; // -
			cmd_inputBuffer[13] = 0x06; // DISCONNECTION_REQUEST
			cmd_inputBuffer[14] = 0x04; // message id
			cmd_inputBuffer[15] = 0x04; // Length of the command parameters (CID+Flags)
			cmd_inputBuffer[16] = 0x00; // -
			cmd_inputBuffer[17] = wiimotes[i]->hidControlChannel[0];
			cmd_inputBuffer[18] = wiimotes[i]->hidControlChannel[1];
			cmd_inputBuffer[19] = 0x40; // local ID of the "HID Control" channel
			cmd_inputBuffer[20] = 0x00; // -
			cmd_outputBuffer = (BYTE*)malloc(4);
			maxRetries = 2;
			while (wiimotes[i]->state == STATE_HID_CONTROL_DISCONNECTION && maxRetries-- > 0)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
					exit_status = EXIT_FAILURE;
				}
				else
				{
					if (verbose) printf("%d: Request L2CAP HID_control disconnection\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}

				// Wait for the end of the Disconnection request command
				WaitForSingleObject(hEventCmdFinished, 2000);
			}

			// State: Disconnect "HID Interrupt" channel
			cmd_inputBuffer = (BYTE*)malloc(21);
			cmd_inputBuffer[0] = 0x10; // Length of the IOCTL message
			cmd_inputBuffer[1] = 0x00;
			cmd_inputBuffer[2] = 0x00;
			cmd_inputBuffer[3] = 0x00;
			cmd_inputBuffer[4] = 0x02; // ACL data
			cmd_inputBuffer[5] = wiimotes[i]->connectionHandle[0];
			cmd_inputBuffer[6] = wiimotes[i]->connectionHandle[1];
			cmd_inputBuffer[7] = 0x0C; // Length of the ACL message
			cmd_inputBuffer[8] = 0x00;
			cmd_inputBuffer[9] = 0x08;  // Length of the L2CAP message
			cmd_inputBuffer[10] = 0x00; // -
			cmd_inputBuffer[11] = 0x01; // Signaling channel
			cmd_inputBuffer[12] = 0x00; // -
			cmd_inputBuffer[13] = 0x06; // DISCONNECTION_REQUEST
			cmd_inputBuffer[14] = 0x03; // message id
			cmd_inputBuffer[15] = 0x04; // Length of the command parameters (CID+Flags)
			cmd_inputBuffer[16] = 0x00; // -
			cmd_inputBuffer[17] = wiimotes[i]->hidInterruptChannel[0];
			cmd_inputBuffer[18] = wiimotes[i]->hidInterruptChannel[1];
			cmd_inputBuffer[19] = 0x41; // local ID of the "HID Interrupt" channel
			cmd_inputBuffer[20] = 0x00; // -
			cmd_outputBuffer = (BYTE*)malloc(4);
			maxRetries = 2;
			while (wiimotes[i]->state == STATE_HID_INTERRUPT_DISCONNECTION && maxRetries-- > 0)
			{
				success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, 21, cmd_outputBuffer, 4, &returned, NULL);
				if (!success)
				{
					printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
					exit_status = EXIT_FAILURE;
				}
				else
				{
					if (verbose) printf("%d: Request L2CAP HID_interrupt disconnection\n", wiimotes[i]->id);
					ResetEvent(hEventCmdFinished);
				}

				// Wait for the end of the Disconnection request command
				WaitForSingleObject(hEventCmdFinished, 2000);
			}

			disconnectWiimote(i);

			if (wiimotes[i]->state != STATE_FINISHED)
			{
				// We are still waiting for the "disconnect event"
				// This means the Wiimote is not switched off yet
				printf("Wiimote #%d: Press any boutton, then press the button \"power\" of the Wiimote until the LEDs are switched off.\n", wiimotes[i]->id);
			}
		}
	}

	// Wait for the optional "disconnection events"
	Sleep(2000);

	readLoop_continue = FALSE;
	printf("Stop then start the Bluetooth of the phone to exit this program.\n");

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