#include <windows.h>
#include <windef.h>
#include <setupapi.h>
#include <synchapi.h>
#include <tchar.h>

#ifndef _SP_H
#define _SP_H

void sp_verb_clear(void);
void sp_verb_set(void);
int sp_query(int** port_number_list, int* port_cnt);
HANDLE sp_open(int port_number, unsigned int baud);
void sp_close(HANDLE hComm);
int sp_read(HANDLE hComm, unsigned char* buf, const size_t BUF_SZ);
int sp_write(HANDLE hComm, unsigned char* buf, const size_t BUF_SZ);

#endif //#ifndef _SP_H
