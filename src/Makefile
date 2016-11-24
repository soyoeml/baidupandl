LIBS	:= -L/opt/lib
INC_PATH:= -I/opt/include/
PROGS	:= bdpandl
TOOLS	:= bcmp bsearch repair
OBJS	:= dlinfo.o dlpart.o dlcommon.o dlscrolling.o dlbuffer.o \
	   dllist.o dlssl.o err_handler.o
CFLAGS	:= -Wall -lpthread -lcurl -lssl -lcrypto -lz
CC	:= gcc


ALL: $(PROGS) $(TOOLS)

debug: ALL
debug: CFLAGS+=-g


%.o: %.c
	$(CC) -c $^   ${INC_PATH}

bdpandl: bdpandl.c $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS) ${INC_PATH} $(LIBS)

bcmp: bcmp.c $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)
bsearch: bsearch.c $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS) ${INC_PATH} $(LIBS)
repair: repair.c $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS) ${INC_PATH} $(LIBS)

.PHONY: ALL debug clean install uninstall
clean:
	$(RM) $(OBJS) $(PROGS) $(TOOLS) $(wildcard *.h.gch)

install:
	cp $(PROGS) /opt/bin

uninstall:
	$(RM) /opt/bin/$(PROGS)
