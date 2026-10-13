# Makefile застосунку motion_logger.
#   make                                    збірка на цьому комп'ютері
#   make CROSS_COMPILE=aarch64-linux-gnu-   збірка для Raspberry Pi 5
#   make STATIC=1                           статична збірка (без залежності від libc на цілі)
#   make DEBUG=1                            з відладочною інформацією
#   make clean | test | help
# Buildroot викликає: make CC=... CFLAGS=... BUILD=...  (див. package/motion_logger/motion_logger.mk)

CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc
DEBUG ?= 0
STATIC ?= 0
BUILD ?= build

ifeq ($(DEBUG),1)
  OPT := -g -O0
else
  OPT := -O2
endif

# Обов'язкові прапорці дописуємо через override: змінна CFLAGS з командного рядка
# (так її передає Buildroot) інакше перекрила б наші -std/-Wall/-MMD.
CFLAGS ?=
override CFLAGS += -std=c11 -Wall -Wextra $(OPT) -MMD -MP -Iinterfaces
LDFLAGS ?=
ifeq ($(STATIC),1)
  override LDFLAGS += -static
endif

TARGET := $(BUILD)/motion_logger
SRCS := core/main.c
OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o))
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# залежності від .h, які записав gcc (-MMD); "-" = не помилка, якщо .d ще нема
-include $(DEPS)

test: all
	@echo "тестів ще нема (кроки 2.2 і далі)"

clean:
	rm -rf $(BUILD)

help:
	@echo "make                                   - зібрати"
	@echo "make CROSS_COMPILE=aarch64-linux-gnu-  - для Raspberry Pi 5"
	@echo "make STATIC=1                          - статична збірка"
	@echo "make DEBUG=1                           - з відладкою"
	@echo "make clean | test | help"

.PHONY: all test clean help
