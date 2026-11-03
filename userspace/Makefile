CFLAGS += -Wall -Wextra -O2 -I../driver

dmatest: dmatest.c ../driver/dmaeng_uapi.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f dmatest
