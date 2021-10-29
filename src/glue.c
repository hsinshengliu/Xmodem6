#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <windows.h>

#include "sp.h"
#include "xmodem.h"

#define	LOG_LEVEL_ERR  0
#define	LOG_LEVEL_WARN 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_DBG  3

static int log_level = LOG_LEVEL_ERR;

#define log_level_set(ll) (log_level = ll)

#define log_err(fmt, ...) \
	do { if(log_level >= LOG_LEVEL_ERR) printf(fmt, __VA_ARGS__); } while(0)

#define log_warn(fmt, ...) \
	do { if(log_level >= LOG_LEVEL_WARN) printf(fmt, __VA_ARGS__); } while(0)

#define log_info(fmt, ...) \
	do { if(log_level >= LOG_LEVEL_INFO) printf(fmt, __VA_ARGS__); } while(0)

#define log_dbg(fmt, ...) \
	do { if(log_level >= LOG_LEVEL_DBG) printf(fmt, __VA_ARGS__); } while(0)

static bool keep = true;
static int is_xfer_keep(void)
{
	return (keep == true)?1:0;
}

static BOOL WINAPI console_handler(DWORD dwType)
{
	switch(dwType)
	{
		case CTRL_BREAK_EVENT:
		{
			log_warn("halt (%s).\n", "ctrl-BREAK");
			keep = false;
		}
		break;
		case CTRL_C_EVENT:
		{
			log_warn("halt (%s).\n", "ctrl-C");
			keep = false;
		}
		break;
		default:
		{
			log_dbg("some other event (%ld)\n", dwType);
		}
		break;
	}

	return TRUE;
}

int main(int argc, char* argv[])
{
	bool usage = (argc >= 2)?false:true;
	unsigned long baud = 0;
	int port_number = 0;
	bool is_query_only = false;
	char fn[260] = {'\0'};
	bool is_receiver = false;
	bool is_xmodem_1k = false;
	bool verbose = false;
	const char* fmt = "b:f:p:rxvqkh";
	bool has_error = false;
	int opt = '\0';
	while((opt = getopt(argc, argv, fmt)) != -1)
	{
		switch(opt)
		{
			case 'b':
			{
				baud = strtoul(optarg, NULL, 0);
			}
			break;
			case 'p':
			{
				port_number = (int)strtoul(optarg, NULL, 0);
			}
			break;
			case 'f':
			{
				memset(fn, '\0', sizeof(fn));
				strncpy(fn, optarg, sizeof(fn)-sizeof(char));
			}
			break;
			case 'r':
			{
				is_receiver = true; //i.e. receiver
			}
			break;
			case 'x':
			{
				is_receiver = false; //i.e. transmitter
			}
			break;
			case 'q':
			{
				is_query_only = true;
			}
			break;
			case 'k':
			{
				is_xmodem_1k = true;
			}
			break;
			case 'v':
			{
				verbose = true;
			}
			break;
			case 'h':
			{
				usage = true;
			}
			break;
			case '?':
			default:
			{
				has_error = true;
			}
			break;
		}
	}

	if(usage == true || has_error == true)
	{
		printf("xmodem6 [-h]\n");
		printf("        [-q]\n");
		printf("        [-v] [-p port_number] [-b baud_rate] [-r|-x] [-k]\n");
		printf("\n");
		printf("        -h             : show usage\n");
		printf("        -q             : query existing serial port\n");
		printf("        -p port_number : specify serial port number, such as 6 (i.e. \\\\.\\COM6)\n");
		printf("        -b baud_rate   : specify baud rate, such as 115200\n");
		printf("        -r             : lauch xmodem receiver\n");
		printf("        -x             : lauch xmodem transmitter\n");
		printf("        -k             : lauch xmodem transmitter using XMODEM-1K, otherwise, XMODEM-CRC (by default)\n");
		return EXIT_SUCCESS;
	}

	if(verbose == true)
	{
		sp_verb_set();
		xmodem_verb_set();
		log_level_set(LOG_LEVEL_DBG);
	}

	if(is_query_only == true || port_number == 0)
	{
		int* port_number_list = NULL;
		int port_cnt = 0;
		int qret = sp_query(&port_number_list, &port_cnt);
		if(qret == 0)
		{
			if(is_query_only == true)
			{
				int k = 0;
				for(k = 0; k < port_cnt; k++)
				{
					log_dbg("port_number_list[%d] = %d\n", k, port_number_list[k]);
				}
			}
			if(port_cnt > 0)
			{
				if(port_number == 0)
				{
					if(is_receiver == true)
					{
						port_number = port_number_list[0];
					}
					else
					{
						port_number = port_number_list[port_cnt-1];
					}
				}
			}
			else
			{
				log_err("no available serial port (%d)!\n", port_cnt);
			}
			if(port_number_list != NULL)
			{
				free(port_number_list);
			}
		}
	}

	log_info("is_query_only = %s; baud = %ld; port_number = %d; is_receiver = %s; is_xmodem_1k = %s, fn = %s\n",
		is_query_only==true?"true":"false",
		baud,
		port_number,
		is_receiver==true?"true":"false",
		is_xmodem_1k==true?"true":"false",
		fn);

	if(is_query_only == true)
	{
		return EXIT_SUCCESS;
	}

	if (!SetConsoleCtrlHandler((PHANDLER_ROUTINE)console_handler,TRUE))
	{
		log_err("fail to set signal handler (%p)!\n", console_handler);
		return EXIT_FAILURE;
	}

	int xret = -1;
	HANDLE hComm = sp_open(port_number, baud);

	if(is_receiver == true)
	{
		xret = xmodem_receive(hComm, fn, is_xfer_keep);
	}
	else
	{
		xret = xmodem_transmit(hComm, fn, is_xmodem_1k, is_xfer_keep);
	}

	sp_close(hComm);

	log_dbg("xret = %d\n", xret);

	return (xret == 0)?EXIT_SUCCESS:EXIT_FAILURE;
}
