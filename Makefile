OSCAR64   ?= oscar64
INCDIR    ?= ../oscar64/include

SRCS      = main.c sid.c seq.c ui.c presets.c diskio.c
TARGET    = drumbox.prg

EMULATOR  ?= x64sc

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRCS) drumbox.h
	$(OSCAR64) -o=$(TARGET) -O2 -g $(SRCS) -ii=$(INCDIR)

run: $(TARGET)
	$(EMULATOR) -autostart $(TARGET)

clean:
	rm -f $(TARGET)