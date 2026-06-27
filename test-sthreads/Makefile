PROGRAMS = test-create test-mutex \
	01mutex01 02mutex02 03mutex03\
	04mutex04 05monitor01 06monitor02\
	07monitor03 08monitor04 09monitor05\
	supermercado


INCLUDES = -I. -I$(srcdir) -I../include -I ../include
COMPILE = $(CC) $(DEFS) $(INCLUDES) $(CFLAGS)
CC = gcc
CFLAGS = -g -O0 -Wall -m32
DEFS = -DHAVE_CONFIG_H
LIBSTHREAD = ../sthread_lib/libsthread.a

all: $(PROGRAMS)

.SUFFIXES: .c .o

test-create: test-create.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o test-create test-create.o $(LIBSTHREAD)

test-mutex: test-mutex.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o test-mutex test-mutex.o $(LIBSTHREAD)

01mutex01: 01mutex01.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o 01mutex01 01mutex01.o $(LIBSTHREAD)

02mutex02: 02mutex02.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o 02mutex02 02mutex02.o $(LIBSTHREAD)

03mutex03: 03mutex03.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o 03mutex03 03mutex03.o $(LIBSTHREAD)

04mutex04: 04mutex04.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o 04mutex04 04mutex04.o $(LIBSTHREAD)

05monitor01: 05monitor01.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o 05monitor01 05monitor01.o $(LIBSTHREAD)

06monitor02: 06monitor02.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o 06monitor02 06monitor02.o $(LIBSTHREAD)

07monitor03: 07monitor03.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o 07monitor03 07monitor03.o $(LIBSTHREAD)

08monitor04: 08monitor04.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o 08monitor04 08monitor04.o $(LIBSTHREAD)

09monitor05: 09monitor05.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o 09monitor05 09monitor05.o $(LIBSTHREAD)

supermercado: supermercado.o
	$(CC) $(CFLAGS) ../sthread_lib/sthread_start.o -o supermercado supermercado.o $(LIBSTHREAD)

.c.o:
	$(COMPILE) -c -o $@ $<


clean: clean-PROGRAMS
	rm -f *.o

clean-PROGRAMS:
	@list='$(PROGRAMS)'; for p in $$list; do \
	  f=`echo $$p|sed 's/$$//'`; \
	  echo " rm -f $$p $$f"; \
	  rm -f $$p $$f ; \
	done


# Tell versions [3.59,3.63) of GNU make to not export all variables.
# Otherwise a system limit (for SysV at least) may be exceeded.
.NOEXPORT:
