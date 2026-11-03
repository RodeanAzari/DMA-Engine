CFLAGS += -Wall -Wextra -O2 -I../driver -I../include

dmatest: dmatest.c ../driver/dmaeng_uapi.h ../include/dmaeng_regs.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f dmatest
