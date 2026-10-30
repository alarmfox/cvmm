CC       = gcc
CPPFLAGS = -I.
CFLAGS   = -Wall -Wextra
LDFLAGS  = -lpthread
LDLIBS   =

ifeq ($(DEBUG),1)
CFLAGS        += -O0 -g
CPPFLAGS      += -DDEBUG
else
CPPFLAGS      += -D_FORTIFY_SOURCE=2
CFLAGS        += -O2
endif

ELF            = kvm_boot
OBJS           = kvm_boot.o serial.o

all: $(ELF)

$(ELF): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS)

clean:
	$(RM) $(OBJS) $(ELF)
