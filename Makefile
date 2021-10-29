#name: Xmodem6
#description: an XMODEM implementation for Win-i386/amd64 command-prompt
#platform: it is tested under MinGW-w64 (e.g. mingw32-make.exe)

TARGET := xmodem6

.PHONY: all
all: $(TARGET)

INCS = inc
SRCS = src
SOURCES = $(SRCS)/sp.c
SOURCES += $(SRCS)/xmodem.c
SOURCES += $(SRCS)/glue.c
OBJS = $(SOURCES:.c=.o)
CFLAGS = -Wall
CFLAGS += -DINITGUID
LDLIBS = -lsetupapi

%.o: %.c
	gcc -o $@ $(CFLAGS) -I$(INCS) -c $<

$(TARGET): $(OBJS)
	gcc -o $@.exe $(CFLAGS) $(OBJS) $(LDLIBS)

.PHONY: clean 
clean:
	del $(SRCS)\\*.o
	del .\\$(TARGET).exe
