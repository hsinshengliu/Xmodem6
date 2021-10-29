#include <windows.h>
#include <stdbool.h>

#ifndef _XMODEM_H
#define _XMODEM_H

void xmodem_verb_clear(void);
void xmodem_verb_set(void);

typedef int (xmodem_keep_xfer_cb)(void);

int xmodem_transmit(const HANDLE hComm, const char* fnxmt, const bool xmodem_1k, xmodem_keep_xfer_cb keep_xfer_cb);
int xmodem_receive(const HANDLE hComm, const char* fnrcv, xmodem_keep_xfer_cb keep_xfer_cb);

#endif //_XMODEM_H
