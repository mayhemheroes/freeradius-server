TARGETNAME	:= rlm_pap

TARGET		:= $(TARGETNAME)$(L)
SOURCES		:= $(TARGETNAME).c

TGT_LDFLAGS	:= $(LCRYPT)
LOG_ID_LIB	= 35
