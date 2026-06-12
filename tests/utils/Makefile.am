AM_CFLAGS= $(CC_OPT) @CMOCKA_CFLAGS@
AM_LDFLAGS=@CMOCKA_LIBS@

TO_SRC=../../src

STORE_LIB=$(TO_SRC)/store/libphobos_store.la
LRS_CFG=$(TO_SRC)/lrs/lrs_cfg.o

noinst_PROGRAMS = phobosd-client

phobosd_client_SOURCES = phobosd-client.c \
                         requests.c
phobosd_client_LDADD   = $(STORE_LIB) $(LRS_CFG)
phobosd_client_CFLAGS  = $(AM_CFLAGS) -I$(TO_SRC)/lrs
