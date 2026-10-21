ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK not configured)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
CFLAGS += -Wall -Wextra -O2
all: unscan.elf
unscan.elf: unscan.c
	$(CC) $(CFLAGS) -o $@ $<
unremove.elf: unremove.c
	$(CC) $(CFLAGS) -o $@ $< -lSceIpmi -lSceAppInstUtil -lSceUserService -lSceSystemService
