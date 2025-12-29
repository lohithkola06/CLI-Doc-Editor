CC=gcc
CFLAGS=-O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS=

COMMON_OBJS=common/net.o common/jsonl.o common/log.o
NM_OBJS=nameserver/nm.o nameserver/nm_state.o nameserver/nm_search.o nameserver/nm_access_req.o nameserver/nm_replication.o
SS_OBJS=storageserver/ss.o storageserver/ss_files.o storageserver/ss_acl.o
CLI_OBJS=client/cli.o client/cli_repl.o

all: nm ss cli

nm: $(COMMON_OBJS) $(NM_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread

ss: $(COMMON_OBJS) $(SS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lpthread

cli: $(COMMON_OBJS) $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f nm ss cli $(COMMON_OBJS) $(NM_OBJS) $(SS_OBJS) $(CLI_OBJS)

