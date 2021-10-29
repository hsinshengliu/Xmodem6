#include "sp.h"

static BOOL verbose = FALSE;

void sp_verb_clear(void)
{
	verbose = FALSE;
}

void sp_verb_set(void)
{
	verbose = TRUE;
}

static BOOL query_device_description(HDEVINFO hDevInfoSet, SP_DEVINFO_DATA* devInfo, char* buf, const size_t BUF_SZ, size_t* nReturn)
{
    DWORD dwType = 0;
    DWORD nBytes = 0;
    BOOL ret = FALSE;
    ret = SetupDiGetDeviceRegistryProperty(hDevInfoSet, devInfo, SPDRP_DEVICEDESC, &dwType, (PBYTE)buf, BUF_SZ, &nBytes);
    if(ret == FALSE)
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            return FALSE;
        }
    }
    if (dwType != REG_SZ)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    *nReturn = (size_t)nBytes;
    return TRUE;
}

static BOOL query_registry_value(HKEY hKey, LPCTSTR lpValueName, char* buf, const size_t BUF_SZ, size_t* nReturn)
{
    DWORD dwType = 0;
    DWORD nBytes = (DWORD)BUF_SZ;
    LSTATUS nStatus = RegQueryValueEx(hKey, lpValueName, NULL, &dwType, (LPBYTE)buf, &nBytes);
    if (nStatus != ERROR_SUCCESS)
    {
        SetLastError(nStatus);
        return FALSE;
    }
    if ((dwType != REG_SZ) && (dwType != REG_EXPAND_SZ))
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    if ((nBytes % sizeof(TCHAR)) != 0)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    *nReturn = (size_t)nBytes;
    if (buf[(nBytes / sizeof(TCHAR)) - 1] != _T('\0'))
    {
        buf[(nBytes / sizeof(TCHAR))] = _T('\0');
        *nReturn = (nBytes / sizeof(TCHAR)) - 1;
    }
    return TRUE;
}

static BOOL query_registry_for_port_name(HKEY hKey, int* nPort)
{
    BOOL bAdded = FALSE;
    do
    {
        char buf[8192] = {'\0'};
        size_t len = 0;
        BOOL ret = query_registry_value(hKey, _T("PortName"), buf, sizeof(buf), &len);
        if(ret == FALSE)
        {
            break;
        }
        if(len > 3)
        {
            if(_tcsnicmp(buf, _T("COM"), 3) == 0)
            {
                *nPort = _ttoi(buf + 3);
                bAdded = TRUE;
            }
        }
    } while(0);
    return bAdded;
}

int sp_query(int** port_number_list, int* port_cnt)
{
	//i.e. Device Manager manner
	int ret = -1;
	int* list = NULL;
	int cnt = 0;

    const GUID guid = GUID_DEVINTERFACE_COMPORT;
    DWORD dwFlags = DIGCF_PRESENT | DIGCF_DEVICEINTERFACE;
    do
    {
		if(port_number_list == NULL || port_cnt == NULL)
		{
			break;
		}
		list = NULL;
		cnt = 0;

        HDEVINFO hDevInfoSet = SetupDiGetClassDevs(&guid, NULL, NULL, dwFlags);
        if (hDevInfoSet == INVALID_HANDLE_VALUE)
        {
            break;
        }
        BOOL bMoreItems = TRUE;
        int nIndex = 0;
        while (bMoreItems)
        {
            SP_DEVINFO_DATA devInfo = {.cbSize = 0};
            devInfo.cbSize = sizeof(SP_DEVINFO_DATA);
            BOOL ret = SetupDiEnumDeviceInfo(hDevInfoSet, nIndex, &devInfo);
            if(ret)
            {
                BOOL bAdded = FALSE;
                HKEY hKey = NULL;
                hKey = SetupDiOpenDevRegKey(hDevInfoSet, &devInfo, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
                if(hKey != INVALID_HANDLE_VALUE)
                {
                    int nPort = 0;
                    int queried = query_registry_for_port_name(hKey, &nPort);
                    if(queried == TRUE)
                    {
						if(verbose == TRUE)
						{
							_tprintf("nPort = %d", nPort);
						}
                        bAdded = TRUE;
						list = (int*)realloc(list, sizeof(int) * (cnt + 1));
						if(list == NULL)
						{
							break;
						}
						list[cnt] = nPort;
						cnt++;
                    }
                }
                if(bAdded == TRUE)
                {
                    char buf[8192] = {'\0'};
                    size_t len = 0;
                    BOOL queried = query_device_description(hDevInfoSet, &devInfo, buf, sizeof(buf), &len);
                    if(queried == TRUE)
                    {
						if(verbose == TRUE)
						{
							_tprintf("; sFriendlyName = %s\n", buf);
						}
                    }
					else
					{
						if(verbose == TRUE)
						{
							_tprintf("\n");
						}
					}
                }
            }
            else
            {
                bMoreItems = ret;
            }
            ++nIndex;
        }

        SetupDiDestroyDeviceInfoList(hDevInfoSet);

		*port_number_list = list;
		*port_cnt = cnt;

		ret = 0;
    } while(0);
	if(port_number_list != NULL && *port_number_list != NULL)
	{
		free(*port_number_list);
	}
	return ret;
}

HANDLE sp_open(int port_number, unsigned int baud)
{
	HANDLE hComm = INVALID_HANDLE_VALUE;
	TCHAR comname[128] = {_T('\0')};
	_stprintf(comname, _T("\\\\.\\COM%d"), port_number);
	hComm = CreateFile(comname,
		GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_OVERLAPPED,
		0);

	if (hComm == INVALID_HANDLE_VALUE)
	{
		return INVALID_HANDLE_VALUE;
	}
	COMMTIMEOUTS timeouts = { 0 };
	timeouts.ReadIntervalTimeout = MAXDWORD;
	timeouts.ReadTotalTimeoutMultiplier = 0;
	timeouts.ReadTotalTimeoutConstant = 0;
	timeouts.WriteTotalTimeoutMultiplier = 0;
	timeouts.WriteTotalTimeoutConstant = 0;

	if (FALSE == SetCommTimeouts(hComm, &timeouts))
	{
		(void)CloseHandle(hComm);
		return INVALID_HANDLE_VALUE;
	}
	DWORD dwStoredFlags = EV_RXCHAR;
	if (FALSE == SetCommMask(hComm, dwStoredFlags))
	{
		(void)CloseHandle(hComm);
		return INVALID_HANDLE_VALUE;
	}
	DCB dcb = {0};
	FillMemory(&dcb, sizeof(dcb), 0);
	dcb.DCBlength = sizeof(dcb);
	if (FALSE == GetCommState(hComm, &dcb))
	{
		(void)CloseHandle(hComm);
		return INVALID_HANDLE_VALUE;
	}
	dcb.BaudRate = (baud == 0)?CBR_115200:baud;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	if (FALSE == SetCommState(hComm, &dcb))
	{
		(void)CloseHandle(hComm);
		return INVALID_HANDLE_VALUE;
	}
	//TODO: show current configuration
	return hComm;
}

void sp_close(HANDLE hComm)
{
	(void)CloseHandle(hComm);
}

int sp_read(HANDLE hComm, unsigned char* buf, const size_t BUF_SZ)
{
	OVERLAPPED osReader = {0};
	DWORD dwRead = 0;
	BOOL fRes = FALSE;
	do
	{
		osReader.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		if (osReader.hEvent == NULL)
		{
			fRes = FALSE;
			break;
		}

		BOOL fWaitingOnRead = FALSE; //Note the fWaitingOnRead flag that is defined by the code; it indicates whether or not a read operation is overlapped.
		if (fWaitingOnRead == FALSE)
		{
			BOOL rret = ReadFile(hComm, buf, BUF_SZ, &dwRead, &osReader);
			if(rret == FALSE)
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					fRes = FALSE;
					break;
				}
				else
				{
					fWaitingOnRead = TRUE;
				}
			}
			else
			{
				fRes = TRUE;
				break;
			}
		}

		if (fWaitingOnRead == TRUE)
		{
			const DWORD READ_TIMEOUT = INFINITE;
			DWORD dwRes = WaitForSingleObject(osReader.hEvent, READ_TIMEOUT);
			switch(dwRes)
			{
				case WAIT_OBJECT_0:
				{
					BOOL gret = GetOverlappedResult(hComm, &osReader, &dwRead, FALSE);
					if(gret == FALSE)
					{
						fRes = FALSE;
					}
					else
					{
						fRes = TRUE;
					}
					fWaitingOnRead = FALSE;
				}
				break;
				case WAIT_TIMEOUT:
				{
					fRes = FALSE;
				}
				break;
				default:
				{
					fRes = FALSE;
				}
				break;
			}
		}
	} while(0);
	if (osReader.hEvent != NULL)
	{
		CloseHandle(osReader.hEvent);
	}

	return (fRes == TRUE)?(dwRead):(-1);
}

int sp_write(HANDLE hComm, unsigned char* buf, const size_t BUF_SZ)
{
	OVERLAPPED osWrite = {0};
	DWORD dwWritten = 0;
	BOOL fRes = FALSE;
	do
	{
		osWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		if (osWrite.hEvent == NULL)
		{
			fRes = FALSE;
			break;
		}

		BOOL wret = WriteFile(hComm,
			buf,
			BUF_SZ,
			&dwWritten,
			&osWrite);
		if(wret == FALSE)
		{
			if (GetLastError() != ERROR_IO_PENDING)
			{
				fRes = FALSE;
			}
			else
			{
				DWORD dwRes = WaitForSingleObject(osWrite.hEvent, INFINITE);
				switch(dwRes)
				{
					case WAIT_OBJECT_0:
					{
						BOOL gret = GetOverlappedResult(hComm, &osWrite, &dwWritten, FALSE);
						if(gret == FALSE)
						{
							fRes = FALSE;
						}
						else
						{
							fRes = TRUE;
						}
					}
					break;
					default:
					{
						fRes = FALSE;
					}
					break;
				}
			}
		}
		else
		{
			fRes = TRUE;
		}
	} while(0);
	if (osWrite.hEvent != NULL)
	{
		CloseHandle(osWrite.hEvent);
	}

	return (fRes == TRUE)?(dwWritten):(-1);
}
