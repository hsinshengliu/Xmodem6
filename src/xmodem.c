#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <synchapi.h>

#include "xmodem.h"
#include "sp.h"

#define XMODEM_CRC_IND  'C'
#define XMODEM_CRC_HDR  0x01 //SOH
#define XMODEM_1K_HDR   0x02 //STX
#define XMODEM_EOT_HDR  0x04
#define XMODEM_CAN_HDR  0x18
#define XMODEM_ACK      0x06
#define XMODEM_NAK      0x15
#define XMODEM_PAD      0x1A
//TODO: indicate timeout shall be configurable
#define XMODEM_INDICATE_TIMEOUT        100 //unit: ms
#define XMODEM_INDICATE_MULTIPLICATION 10
#define XMODEM_INDICATE_RETRY_COUNT    6*XMODEM_INDICATE_MULTIPLICATION
//TODO: transfer timeout shall be considered with the baud rate
#define XMODEM_PKT_XFER_TIMEOUT        10 //unit: ms
#define XMODEM_PKT_XFER_RETRY_COUNT    100

#define msleep(milliseconds)           Sleep(milliseconds)

struct xmodem_crc_pkt_t
{
	uint8_t hdr;
	uint8_t pkt_num_l;
	uint8_t pkt_num_h;
	uint8_t data[128];
	uint16_t crc16;
} __attribute__((packed)); //NOTE: #pragma pack(1) would be okay too

struct xmodem_1k_pkt_t
{
	uint8_t hdr;
	uint8_t pkt_num_l;
	uint8_t pkt_num_h;
	uint8_t data[1024];
	uint16_t crc16;
} __attribute__((packed)); //NOTE: #pragma pack(1) would be okay too

enum xmodem_state_t
{
	xmodem_state_initial = 0,
	xmodem_state_indicate,
	xmodem_state_wait,
	xmodem_state_wait_term,
	xmodem_state_wait_canc,
	xmodem_state_hdr_rcv,
	xmodem_state_pkt_num_rcv,
	xmodem_state_data_rcv,
	xmodem_state_data_xmt,
	xmodem_state_ack_xmt,
	xmodem_state_nak_xmt,
	xmodem_state_can_xmt,
	xmodem_state_eot_xmt,
	xmodem_state_success,
	xmodem_state_failure,
};

static const char* xmodem_state_s[] =
{
	"init",
	"ind",
	"wait",
	"wait_term",
	"wait_canc",
	"hdr_rcv",
	"pkt_num_rcv",
	"data_rcv",
	"data_xmt",
	"ack_xmt",
	"nak_xmt",
	"can_xmt",
	"eot_xmt",
	"succ",
	"fail",
};

static bool verbose = false;

void xmodem_verb_clear(void)
{
	verbose = false;
}

void xmodem_verb_set(void)
{
	verbose = true;
}

#define xmodem_printf(fmt, ...) \
	do { if(verbose == true) printf(fmt, __VA_ARGS__); } while(0)

//NOTE: mixed size transmission is unsupported

struct data_block_t
{
	unsigned char* data;
	size_t data_sz;
	struct data_block_t* next;
};

static int data_block_release(struct data_block_t** root)
{
	if(root == NULL)
	{
		return -1;
	}
	struct data_block_t* iter = *root;
	while(iter != NULL)
	{
		struct data_block_t* temp = iter;
		//xmodem_printf("data = %p, itself = %p\n", iter->data, iter);
		free(iter->data);
		free(iter);
		iter = temp->next;
	}
	*root = NULL;
	return 0;
}

static int data_block_append(struct data_block_t** root, struct data_block_t** last, unsigned char* data, size_t data_sz)
{
	struct data_block_t* newborn = NULL;
	do
	{
		if(root == NULL || last == NULL || data == NULL || data_sz == 0)
		{
			break;
		}
		newborn = (struct data_block_t*)malloc(sizeof(struct data_block_t));
		if(newborn == NULL)
		{
			break;
		}
		newborn->data = (unsigned char*)malloc(sizeof(unsigned char) * data_sz);
		if(newborn->data == NULL)
		{
			break;
		}
		memcpy(newborn->data, data, data_sz);
		newborn->data_sz = data_sz;
		newborn->next = NULL;
		//xmodem_printf("data = %p, itself = %p\n", newborn->data, newborn);

		if(*root == NULL)
		{
			*root = newborn;
			*last = newborn;
		}
		else
		{
			(*last)->next = newborn;
			*last = newborn;
		}
		return 0;
	} while(0);
	if(newborn != NULL)
	{
		if(newborn->data != NULL)
		{
			free(newborn->data);
		}
		free(newborn);
	}
	return -1;
}

static void data_block_dump(struct data_block_t* root) __attribute__((unused));
static void data_block_dump(struct data_block_t* root)
{
	int num = 0;
	struct data_block_t* iter = root;
	while(iter != NULL)
	{
		xmodem_printf("[%u] iter[%d] = %p\n", __LINE__, num, iter);
		iter = iter->next;
		num++;
	}
}

static int data_block_load(struct data_block_t** root, const char* fn, const size_t data_sz)
{
	FILE* fp = NULL;
	do
	{
		if(root == NULL || fn == NULL)
		{
			break;
		}
		if(*root != NULL)
		{
			break;
		}
		fp = fopen(fn, "rb");
		if(fp == NULL)
		{
			break;
		}
		struct data_block_t* last = NULL;
		size_t rret = 0;
		bool err = false;
		do
		{
			struct data_block_t* newborn = (struct data_block_t*)malloc(sizeof(struct data_block_t));
			if(newborn == NULL)
			{
				err = true;
				break;
			}
			newborn->data = (unsigned char*)malloc(sizeof(unsigned char) * data_sz);
			if(newborn->data == NULL)
			{
				err = true;
				break;
			}
			rret = fread(newborn->data, sizeof(unsigned char), data_sz, fp);
			if(rret > 0)
			{
				if(rret < data_sz)
				{
					memset(&newborn->data[rret], XMODEM_PAD, data_sz - rret);
					rret = 0;
				}
				newborn->data_sz = data_sz;
				newborn->next = NULL;
				if(*root == NULL)
				{
					*root = newborn;
				}
				else
				{
					last->next = newborn;
				}
				last = newborn;
			}
			else if(rret == 0)
			{
				free(newborn->data);
				free(newborn);
			}
		} while(rret > 0); //!feof(fp) would be okay too
		fclose(fp);
		if(err == true)
		{
			(void)data_block_release(root);
			return -1;
		}
		return 0;
	} while(0);
	if(fp != NULL)
	{
		fclose(fp);
	}
	(void)data_block_release(root);
	return -1;
}

static int data_block_store(struct data_block_t** root, const char* fn)
{
	int ret = -1;

	if(root == NULL || fn == NULL)
	{
		xmodem_printf("[%s] error!\n", __FUNCTION__);
	}
	else
	{
		FILE* fp = fopen(fn, "wb+");
		if(fp != NULL)
		{
			size_t cnt = 0;
			struct data_block_t* iter = *root;
			while(iter != NULL)
			{
				if(iter->next == NULL)
				{
					//i.e. last one
					short pad_cnt = 0;
					size_t j = 0;
					for(j = iter->data_sz - 1; j > 0; j--)
					{
						if(iter->data[j] == XMODEM_PAD)
						{
							pad_cnt++;
						}
						else
						{
							break;
						}
					}
					if(pad_cnt > 0)
					{
						xmodem_printf("[%s] warning: fn = %s, pad_cnt = %d\n", __FUNCTION__, fn, pad_cnt);
					}
				}
				size_t wret = fwrite(iter->data, sizeof(unsigned char), iter->data_sz, fp);
				if(wret != iter->data_sz)
				{
					xmodem_printf("[%s] error!\n", __FUNCTION__);
					break;
				}
				iter = iter->next;
				cnt++;
			}
			fflush(fp);
			fclose(fp);
			(void)data_block_release(root);
			xmodem_printf("[%s] cnt = %u\n", __FUNCTION__, (unsigned int)cnt);
			ret = 0;
		}
		else
		{
			xmodem_printf("[%s] error!\n", __FUNCTION__);
		}
	}

	return ret;
}

static int data_block_iterate(struct data_block_t** root, struct data_block_t** iter, struct data_block_t** curr)
{
	int ret = -1;
	do
	{
		if(root == NULL || iter == NULL || curr == NULL)
		{
			break;
		}
		if(*iter == NULL)
		{
			*curr = *root;
			*iter = (*root)->next;
		}
		else
		{
			*curr = *iter;
			*iter = (*iter)->next;
		}
	} while(0);
	return ret;
}

static int data_block_has_next(struct data_block_t** root, struct data_block_t** iter)
{
	int ret = 0;
	do
	{
		if(root == NULL || iter == NULL)
		{
			break;
		}
		if(*iter != NULL)
		{
			ret = !(0);
		}
	} while(0);
	return ret;
}

static int data_block_is_empty(struct data_block_t* curr)
{
	int ret = (curr != NULL)?(0):(!(0));
	return ret;
}

static void data_block_copy(uint8_t* data, struct data_block_t* curr)
{
	memcpy(data, curr->data, curr->data_sz);
}

static uint16_t crc_calculate(uint8_t *ptr, short count)
{
	//CCITT
    uint16_t crc;
    uint8_t i;

    crc = 0;
    while (--count >= 0)
    {
        crc = crc ^ (int) *ptr++ << 8;
        i = 8;
        do
        {
            if (crc & 0x8000)
                crc = crc << 1 ^ 0x1021;
            else
                crc = crc << 1;
        } while(--i);
    }
    return (crc);
}

int xmodem_receive(const HANDLE hComm, const char* fnrcv, xmodem_keep_xfer_cb keep_xfer_cb)
{
	const char* fn = (fnrcv == NULL || strlen(fnrcv) == 0)?("default_out.txt"):(fnrcv);
	struct data_block_t* dbrcv = NULL;
	xmodem_printf("[%s] hComm = 0x%p (%s)\n", __FUNCTION__, hComm, hComm==INVALID_HANDLE_VALUE?"abnormal":"normal");
	SYSTEMTIME tsBegin = {0};
	GetSystemTime(&tsBegin);
	struct xmodem_crc_pkt_t xcp = {0x0};
	struct xmodem_1k_pkt_t x1p = {0x0};
	struct data_block_t* dblast = NULL;
	short ind_retry_count = 0;
	short pkt_xfer_retry_count = 0;
	short pkt_num_index = 0;
	short data_index = 0;
	short crc16_index = 0;
	bool is_xmodem_1k = false;
	unsigned char ch = 0x0;
	enum xmodem_state_t state_curr = xmodem_state_initial;
	enum xmodem_state_t state_prev = xmodem_state_initial;
	while(state_curr != xmodem_state_success && state_curr != xmodem_state_failure)
	{
		static enum xmodem_state_t cached_state_prev = xmodem_state_initial;
		if(cached_state_prev != state_prev)
		{
			xmodem_printf("[%s] state_prev = %s\n", __FUNCTION__, xmodem_state_s[state_prev]);
			cached_state_prev = state_prev;
		}
		static enum xmodem_state_t cached_state_curr = xmodem_state_initial;
		if(cached_state_curr != state_curr)
		{
			xmodem_printf("[%s] state_curr = %s\n", __FUNCTION__, xmodem_state_s[state_curr]);
			cached_state_curr = state_curr;
		}

		switch(state_curr)
		{
			case xmodem_state_initial:
			{
				state_curr = xmodem_state_indicate;
				ind_retry_count = XMODEM_INDICATE_RETRY_COUNT;
			}
			break;
			case xmodem_state_indicate:
			{
				ch = XMODEM_CRC_IND;
				(void)sp_write(hComm, (unsigned char*)&ch, sizeof(ch));
				state_prev = state_curr;
				state_curr = xmodem_state_wait;
			}
			break;
			case xmodem_state_wait:
			{
				if(keep_xfer_cb != NULL && !keep_xfer_cb())
				{
					state_curr = xmodem_state_can_xmt;
					continue;
				}
				int rret = sp_read(hComm, &ch, sizeof(ch));
				//xmodem_printf("[%s] rret = %d, ch = 0x%02x\n", __FUNCTION__, rret, (unsigned char)ch);
				if(rret < 0)
				{
					state_curr = xmodem_state_failure;
				}
				else if(rret == 0)
				{
					switch(state_prev)
					{
						case xmodem_state_indicate:
						{
							if(ind_retry_count == 0)
							{
								state_curr = xmodem_state_failure;
							}
							else
							{
								ind_retry_count--;
								if((ind_retry_count % XMODEM_INDICATE_MULTIPLICATION) == 0)
								{
									state_curr = xmodem_state_indicate;
								}
								else
								{
									msleep(XMODEM_INDICATE_TIMEOUT);
								}
							}
						}
						break;
						case xmodem_state_ack_xmt:
						case xmodem_state_nak_xmt:
						{
							if(pkt_xfer_retry_count == 0)
							{
								state_curr = xmodem_state_failure;
							}
							else
							{
								pkt_xfer_retry_count--;
								msleep(XMODEM_PKT_XFER_TIMEOUT);
							}
						}
						break;
						default:
						{
							//do nothing
						}
						break;
					}
				}
				else
				{
					pkt_xfer_retry_count = XMODEM_PKT_XFER_RETRY_COUNT;
					switch(ch)
					{
						case XMODEM_1K_HDR:
						{
							state_curr = xmodem_state_hdr_rcv;
							x1p.hdr = ch;
							pkt_num_index = 0;
							is_xmodem_1k = true;
						}
						break;
						case XMODEM_CRC_HDR:
						{
							state_curr = xmodem_state_hdr_rcv;
							xcp.hdr = ch;
							pkt_num_index = 0;
							is_xmodem_1k = false;
						}
						break;
						case XMODEM_EOT_HDR:
						{
							state_prev = xmodem_state_wait_term;
							state_curr = xmodem_state_ack_xmt;
						}
						break;
						case XMODEM_CAN_HDR:
						{
							state_prev = xmodem_state_wait_canc;
							state_curr = xmodem_state_ack_xmt;
						}
						default:
						{
							//do nothing
						}
						break;
					}
				}
			}
			break;
			case xmodem_state_hdr_rcv:
			{
				int rret = sp_read(hComm, &ch, sizeof(ch));
				if(rret < 0)
				{
					state_curr = xmodem_state_failure;
				}
				else if(rret == 0)
				{
					if(pkt_xfer_retry_count == 0)
					{
						state_curr = xmodem_state_failure;
					}
					else
					{
						pkt_xfer_retry_count--;
						msleep(XMODEM_PKT_XFER_TIMEOUT);
					}
				}
				else
				{
					pkt_xfer_retry_count = XMODEM_PKT_XFER_RETRY_COUNT;
					if(is_xmodem_1k == true)
					{
						if(pkt_num_index == 0)
						{
							x1p.pkt_num_l = ch;
						}
						else
						{
							x1p.pkt_num_h = ch;
						}
					}
					else
					{
						if(pkt_num_index == 0)
						{
							xcp.pkt_num_l = ch;
						}
						else
						{
							xcp.pkt_num_h = ch;
						}
					}
					pkt_num_index++;
					if(pkt_num_index >= (sizeof(uint8_t) + sizeof(uint8_t)))
					{
						data_index = 0;
						state_curr = xmodem_state_pkt_num_rcv;
					}
				}
			}
			break;
			case xmodem_state_pkt_num_rcv:
			{
				int rret = sp_read(hComm, &ch, sizeof(ch));
				if(rret < 0)
				{
					state_curr = xmodem_state_failure;
				}
				else if(rret == 0)
				{
					if(pkt_xfer_retry_count == 0)
					{
						state_curr = xmodem_state_failure;
					}
					else
					{
						pkt_xfer_retry_count--;
						msleep(XMODEM_PKT_XFER_TIMEOUT);
					}
				}
				else
				{
					pkt_xfer_retry_count = XMODEM_PKT_XFER_RETRY_COUNT;
					if(is_xmodem_1k == true)
					{
						x1p.data[data_index] = ch;
						data_index++;
						if(data_index >= sizeof(x1p.data))
						{
							state_curr = xmodem_state_data_rcv;
							crc16_index = 0;
							x1p.crc16 = 0x0;
						}
					}
					else
					{
						xcp.data[data_index] = ch;
						data_index++;
						if(data_index >= sizeof(xcp.data))
						{
							state_curr = xmodem_state_data_rcv;
							crc16_index = 0;
							xcp.crc16 = 0x0;
						}
					}
				}
			}
			break;
			case xmodem_state_data_rcv:
			{
				int rret = sp_read(hComm, &ch, sizeof(ch));
				if(rret < 0)
				{
					state_curr = xmodem_state_failure;
				}
				else if(rret == 0)
				{
					if(pkt_xfer_retry_count == 0)
					{
						state_curr = xmodem_state_failure;
					}
					else
					{
						pkt_xfer_retry_count--;
						msleep(XMODEM_PKT_XFER_TIMEOUT);
					}
				}
				else
				{
					pkt_xfer_retry_count = XMODEM_PKT_XFER_RETRY_COUNT;
					if(is_xmodem_1k == true)
					{
						x1p.crc16 |= ch << ((sizeof(x1p.crc16) - crc16_index - 1) * 8);
						crc16_index++;
						if(crc16_index >= sizeof(x1p.crc16))
						{
							uint16_t crc16 = crc_calculate(x1p.data, sizeof(x1p.data));
							xmodem_printf("[%s] pkt_num_l = %u, pkt_num_h = %u, crc16 = 0x%04x (0x%04x) within %u\n", __FUNCTION__, (unsigned char)x1p.pkt_num_l, (unsigned char)x1p.pkt_num_h, x1p.crc16, crc16, (unsigned int)sizeof(x1p.data));
							if(crc16 != x1p.crc16)
							{
								state_curr = xmodem_state_nak_xmt;
							}
							else if((x1p.pkt_num_l + x1p.pkt_num_h) != 0xff)
							{
								state_curr = xmodem_state_can_xmt;
							}
							else
							{
								state_prev = state_curr;
								state_curr = xmodem_state_ack_xmt;

								static uint8_t pkt_num_l_last = -1;
								if(pkt_num_l_last != x1p.pkt_num_l)
								{
									int aret = data_block_append(&dbrcv, &dblast, x1p.data, sizeof(x1p.data));
									if(aret != 0)
									{
										state_curr = xmodem_state_failure;
									}
									pkt_num_l_last = x1p.pkt_num_l;
								}
								else
								{
									xmodem_printf("[%s] duplicate, pkt_num_l = %u (%u)\n", __FUNCTION__, x1p.pkt_num_l, pkt_num_l_last);
								}
							}
						}
					}
					else
					{
						xcp.crc16 |= ch << ((sizeof(xcp.crc16) - crc16_index - 1) * 8);
						crc16_index++;
						if(crc16_index >= sizeof(xcp.crc16))
						{
							uint16_t crc16 = crc_calculate(xcp.data, sizeof(xcp.data));
							xmodem_printf("[%s] pkt_num_l = %u, pkt_num_h = %u, crc16 = 0x%04x (0x%04x) within %u\n", __FUNCTION__, (unsigned char)xcp.pkt_num_l, (unsigned char)xcp.pkt_num_h, xcp.crc16, crc16, (unsigned int)sizeof(xcp.data));
							if(crc16 != xcp.crc16)
							{
								state_curr = xmodem_state_nak_xmt;
							}
							else if((xcp.pkt_num_l + xcp.pkt_num_h) != 0xff)
							{
								state_curr = xmodem_state_can_xmt;
							}
							else
							{
								state_prev = state_curr;
								state_curr = xmodem_state_ack_xmt;

								static uint8_t pkt_num_l_last = -1;
								if(pkt_num_l_last != xcp.pkt_num_l)
								{
									int aret = data_block_append(&dbrcv, &dblast, xcp.data, sizeof(xcp.data));
									if(aret != 0)
									{
										state_curr = xmodem_state_failure;
									}
									pkt_num_l_last = xcp.pkt_num_l;
								}
								else
								{
									xmodem_printf("[%s] duplicate, pkt_num_l = %u (%u)\n", __FUNCTION__, xcp.pkt_num_l, pkt_num_l_last);
								}
							}
						}
					}
					//NOTE: it would be better if such similar code were merged
				}
			}
			break;
			case xmodem_state_can_xmt:
			{
				ch = XMODEM_CAN_HDR;
				(void)sp_write(hComm, (unsigned char*)&ch, sizeof(ch));
				state_prev = state_curr;
				state_curr = xmodem_state_failure;
			}
			break;
			case xmodem_state_nak_xmt:
			{
				ch = XMODEM_NAK;
				(void)sp_write(hComm, (unsigned char*)&ch, sizeof(ch));
				state_prev = state_curr;
				state_curr = xmodem_state_wait;
				
			}
			break;
			case xmodem_state_ack_xmt:
			{
				ch = XMODEM_ACK;
				(void)sp_write(hComm, (unsigned char*)&ch, sizeof(ch));
				switch(state_prev)
				{
					case xmodem_state_wait_canc:
					{
						state_prev = state_curr;
						state_curr = xmodem_state_failure;
					}
					break;
					case xmodem_state_wait_term:
					{
						state_prev = state_curr;
						state_curr = xmodem_state_success;
					}
					break;
					case xmodem_state_data_rcv:
					{
						state_prev = state_curr;
						state_curr = xmodem_state_wait;
					}
					break;
					default:
					{
						//do nothing
					}
					break;
				}
			}
			break;
			case xmodem_state_failure:
			default:
			{
				//do nothing
			}
			break;
		}
	}

	if(dbrcv != NULL)
	{
		if(state_curr == xmodem_state_success)
		{
			xmodem_printf("[%s] dbrcv = %p\n", __FUNCTION__, dbrcv);
			int dret = data_block_store(&dbrcv, fn);
			xmodem_printf("[%s] dbrcv = %p; dret = %d\n", __FUNCTION__, dbrcv, dret);
		}
		else
		{
			xmodem_printf("[%s] dbrcv = %p\n", __FUNCTION__, dbrcv);
			(void)data_block_release(&dbrcv);
			xmodem_printf("[%s] dbrcv = %p\n", __FUNCTION__, dbrcv);
		}
	}
	SYSTEMTIME tsEnd = {0};
	GetSystemTime(&tsEnd);

	xmodem_printf("[%s] state_curr = %s\n", __FUNCTION__, xmodem_state_s[state_curr]);
	return (state_curr == xmodem_state_success)?(0):(-1);
}

int xmodem_transmit(const HANDLE hComm, const char* fnxmt, const bool xmodem_1k, xmodem_keep_xfer_cb keep_xfer_cb)
{
	const char* fn = (fnxmt == NULL || strlen(fnxmt) == 0)?("default_in.txt"):(fnxmt);
	struct data_block_t* dbxmt = NULL;
	xmodem_printf("[%s] hComm = %p (%s)\n", __FUNCTION__, hComm, hComm==INVALID_HANDLE_VALUE?"abnormal":"normal");
	SYSTEMTIME tsBegin = {0};
	GetSystemTime(&tsBegin);
	struct xmodem_crc_pkt_t xcp = {0x0};
	struct xmodem_1k_pkt_t x1p = {0x0};
	short ind_retry_count = 0;
	short pkt_xfer_retry_count = 0;
	bool is_xmodem_1k = xmodem_1k;
	struct data_block_t* dbiter = NULL;
	struct data_block_t* dbcurr = NULL;
	short pkt_num_index = 0;
	unsigned char ch = 0x0;
	enum xmodem_state_t state_curr = xmodem_state_initial;
	enum xmodem_state_t state_prev = xmodem_state_initial;

	while(state_curr != xmodem_state_success && state_curr != xmodem_state_failure)
	{
		static enum xmodem_state_t cached_state_prev = xmodem_state_initial;
		if(cached_state_prev != state_prev)
		{
			xmodem_printf("[%s] state_prev = %s\n", __FUNCTION__, xmodem_state_s[state_prev]);
			cached_state_prev = state_prev;
		}
		static enum xmodem_state_t cached_state_curr = xmodem_state_initial;
		if(cached_state_curr != state_curr)
		{
			xmodem_printf("[%s] state_curr = %s\n", __FUNCTION__, xmodem_state_s[state_curr]);
			cached_state_curr = state_curr;
		}

		switch(state_curr)
		{
			case xmodem_state_initial:
			{
				int cret = data_block_load(&dbxmt, fn, is_xmodem_1k==true?sizeof(x1p.data):sizeof(xcp.data));
				if(cret == 0)
				{
					state_prev = state_curr;
					state_curr = xmodem_state_wait;
					ind_retry_count = XMODEM_INDICATE_RETRY_COUNT;
				}
				else
				{
					state_curr = xmodem_state_failure;
				}
			}
			break;
			case xmodem_state_wait:
			{
				if(keep_xfer_cb != NULL && !keep_xfer_cb())
				{
					state_curr = xmodem_state_can_xmt;
					continue;
				}
				int rret = sp_read(hComm, &ch, sizeof(ch));
				//xmodem_printf("[%s] rret = %d, ch = 0x%02x\n", __FUNCTION__, rret, (unsigned char)ch);
				if(rret < 0)
				{
					state_curr = xmodem_state_failure;
				}
				else if(rret == 0)
				{
					switch(state_prev)
					{
						case xmodem_state_initial:
						{
							if(ind_retry_count == 0)
							{
								state_curr = xmodem_state_failure;
							}
							else
							{
								ind_retry_count--;
								msleep(XMODEM_INDICATE_TIMEOUT);
							}
						}
						break;
						case xmodem_state_data_xmt:
						{
							if(pkt_xfer_retry_count == 0)
							{
								state_curr = xmodem_state_failure;
							}
							else
							{
								pkt_xfer_retry_count--;
								msleep(XMODEM_PKT_XFER_TIMEOUT);
							}
						}
						break;
						default:
						{
							//do nothing
						}
						break;
					}
				}
				else
				{
					switch(ch)
					{
						case XMODEM_CRC_IND:
						{
							switch(state_prev)
							{
								case xmodem_state_data_xmt:
								{
									state_curr = xmodem_state_data_xmt;
								}
								break;
								case xmodem_state_initial:
								default:
								{
									state_curr = xmodem_state_data_xmt;
									data_block_iterate(&dbxmt, &dbiter, &dbcurr);
									pkt_num_index = 1;
								}
								break;
							}
						}
						break;
						case XMODEM_ACK:
						{
							switch(state_prev)
							{
								case xmodem_state_data_xmt:
								{
									pkt_num_index++;
									if(!data_block_has_next(&dbxmt, &dbiter))
									{
										state_curr = xmodem_state_eot_xmt;
									}
									else
									{
										state_curr = xmodem_state_data_xmt;
										data_block_iterate(&dbxmt, &dbiter, &dbcurr);
									}
								}
								break;
								case xmodem_state_eot_xmt:
								{
									state_curr = xmodem_state_success;
								}
								break;
								default:
								{
									//do nothing
								}
								break;
							}
						}
						break;
						case XMODEM_NAK:
						{
							state_curr = xmodem_state_data_xmt;
						}
						break;
						case XMODEM_CAN_HDR:
						{
							state_curr = xmodem_state_failure;
						}
						break;
						default:
						{
							//do nothing
						}
						break;
					}
				}
			}
			break;
			case xmodem_state_data_xmt:
			{
				if(!data_block_is_empty(dbcurr))
				{
					if(is_xmodem_1k == true)
					{
						x1p.hdr = XMODEM_1K_HDR;
						if(pkt_num_index > 255)
						{
							pkt_num_index = 0;
						}
						x1p.pkt_num_l = (uint8_t)pkt_num_index;
						x1p.pkt_num_h = (uint8_t)(255 - pkt_num_index);
						data_block_copy(x1p.data, dbcurr);
						uint16_t crc16 = crc_calculate(x1p.data, sizeof(x1p.data));
						x1p.crc16 = 0x0;
						x1p.crc16 |= (crc16 >> 8) & 0x00ff;
						x1p.crc16 |= (crc16 << 8) & 0xff00;
						xmodem_printf("[%s] pkt_num_l = %u, pkt_num_h = %u, crc16 = 0x%04x (0x%04x) within %u\n", __FUNCTION__, (unsigned char)x1p.pkt_num_l, (unsigned char)x1p.pkt_num_h, x1p.crc16, crc16, (unsigned int)sizeof(x1p.data));
						int wret = sp_write(hComm, (unsigned char*)&x1p, sizeof(x1p));
						//NOTE: transmission rate will be slowed down with following manner
						//int wret = 0;
						//int k = 0;
						//unsigned char* p = (unsigned char*)&x1p;
						//for(k = 0; k < sizeof(x1p); k++)
						//{
						//	(void)sp_write(hComm, p, sizeof(*p));
						//	p++;
						//	msleep(1);
						//}
						//wret = k;

						//xmodem_printf("[%s] wret = %d\n", __FUNCTION__, wret);
						if(wret == sizeof(x1p))
						{
							state_prev = state_curr;
							state_curr = xmodem_state_wait;
							pkt_xfer_retry_count = XMODEM_PKT_XFER_RETRY_COUNT;
						}
					}
					else
					{
						xcp.hdr = XMODEM_CRC_HDR;
						if(pkt_num_index > 255)
						{
							pkt_num_index = 0;
						}
						xcp.pkt_num_l = (uint8_t)pkt_num_index;
						xcp.pkt_num_h = (uint8_t)(255 - pkt_num_index);
						data_block_copy(xcp.data, dbcurr);
						uint16_t crc16 = crc_calculate(xcp.data, sizeof(xcp.data));
						xcp.crc16 = 0x0;
						xcp.crc16 |= (crc16 >> 8) & 0x00ff;
						xcp.crc16 |= (crc16 << 8) & 0xff00;
						xmodem_printf("[%s] pkt_num_l = %u, pkt_num_h = %u, crc16 = 0x%04x (0x%04x) within %u\n", __FUNCTION__, (unsigned char)xcp.pkt_num_l, (unsigned char)xcp.pkt_num_h, xcp.crc16, crc16, (unsigned int)sizeof(xcp.data));
						int wret = sp_write(hComm, (unsigned char*)&xcp, sizeof(xcp));
						//NOTE: transmission rate will be slowed down with following manner
						//int wret = 0;
						//int k = 0;
						//unsigned char* p = (unsigned char*)&xcp;
						//for(k = 0; k < sizeof(xcp); k++)
						//{
						//	(void)sp_write(hComm, p, sizeof(*p));
						//	p++;
						//	msleep(1);
						//}
						//wret = k;

						//xmodem_printf("[%s] wret = %d\n", __FUNCTION__, wret);
						if(wret == sizeof(xcp))
						{
							state_prev = state_curr;
							state_curr = xmodem_state_wait;
							pkt_xfer_retry_count = XMODEM_PKT_XFER_RETRY_COUNT;
						}
						else
						{
							xmodem_printf("[%s] unexpected behavior!\n", __FUNCTION__);
							state_prev = state_curr;
							state_curr = xmodem_state_failure;
						}
					}
					//NOTE: it would be better if such similar code were merged
				}
				else
				{
					xmodem_printf("[%s] unexpected behavior!\n", __FUNCTION__);
					state_prev = state_curr;
					state_curr = xmodem_state_failure;
				}
			}
			break;
			case xmodem_state_can_xmt:
			{
				ch = XMODEM_CAN_HDR;
				(void)sp_write(hComm, (unsigned char*)&ch, sizeof(ch));
				state_prev = state_curr;
				state_curr = xmodem_state_failure;
			}
			break;
			case xmodem_state_eot_xmt:
			{
				ch = XMODEM_EOT_HDR;
				(void)sp_write(hComm, (unsigned char*)&ch, sizeof(ch));
				state_prev = state_curr;
				state_curr = xmodem_state_wait;
			}
			break;
			case xmodem_state_failure:
			default:
			{
				//do nothing
			}
			break;
		}
	}
	if(dbxmt != NULL)
	{
		xmodem_printf("[%s] dbxmt = %p\n", __FUNCTION__, dbxmt);
		(void)data_block_release(&dbxmt);
		xmodem_printf("[%s] dbxmt = %p\n", __FUNCTION__, dbxmt);
	}
	SYSTEMTIME tsEnd = {0};
	GetSystemTime(&tsEnd);

	xmodem_printf("[%s] state_curr = %s\n", __FUNCTION__, xmodem_state_s[state_curr]);
	return (state_curr == xmodem_state_success)?(0):(-1);
}
