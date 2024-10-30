// Wp81Wiimote2.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "getopt.h"

BOOL WINAPI consoleHandler(DWORD signal)
{
	switch (signal)
	{
	case CTRL_C_EVENT:
		mainLoop_exit();
		// Signal is handled - don't pass it on to the next handler.
		return TRUE;
	default:
		// Pass signal on to the next handler.
		return FALSE;
	}
}


static void usage(char *programName)
{
	printf("%s - Wiimote demonstration\n"
		"Usage:\n", programName);
	printf("\t%s [options]\n", programName);
	printf("options:\n"
		"\t-h, --help             Show help options\n");
}

static const struct option main_options[] = {
	{ "help",      no_argument,       NULL, 'h' },
	{}
};

int main(int argc, char* argv[])
{
	int exit_status = EXIT_SUCCESS;

	SetConsoleCtrlHandler(consoleHandler, TRUE);

	for (;;) {
		int opt;

		opt = getopt_long(argc, argv,
			"h",
			main_options, NULL);

		if (opt < 0) {
			// no more option to parse
			break;
		}

		switch (opt) {
		case 'h':
			usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	exit_status = mainLoop_run();

	return exit_status;
}

