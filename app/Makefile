# Makefile застосунку motion_logger.
#   make                                    збірка на цьому комп'ютері
#   make CROSS_COMPILE=aarch64-linux-gnu-   збірка для Raspberry Pi 5
#   make STATIC=1                           статична збірка (без залежності від libc на цілі)
#   make DEBUG=1                            з відладочною інформацією
#   make test                               зібрати й запустити тести (лише на цьому комп'ютері)
#   make format-check | format              перевірити / виправити стиль коду (clang-format)
#   make clean | help
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
override CFLAGS += -std=c11 -Wall -Wextra $(OPT) -MMD -MP -Iinterfaces -Icore
LDFLAGS ?=
ifeq ($(STATIC),1)
  override LDFLAGS += -static
endif

TARGET := $(BUILD)/motion_logger
SRCS := core/main.c core/ring_buffer.c
OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o))

# Тести: окрема програма з тих самих core-файлів (без main.c), запускається командою make test.
TEST_TARGET := $(BUILD)/test_ring_buffer
TEST_OBJS := $(BUILD)/tests/test_ring_buffer.o $(BUILD)/core/ring_buffer.o

DEPS := $(OBJS:.o=.d) $(TEST_OBJS:.o=.d)

# Форматер: версія зафіксована в .clang-format (23.1.1). Інша команда: make format-check CLANG_FORMAT=...
CLANG_FORMAT ?= clang-format
FORMAT_FILES := $(wildcard core/*.c core/*.h tests/*.c)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# залежності від .h, які записав gcc (-MMD); "-" = не помилка, якщо .d ще нема
-include $(DEPS)

$(TEST_TARGET): $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

test: $(TEST_TARGET)
	./$(TEST_TARGET)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

clean:
	rm -rf $(BUILD)

help:
	@echo "make                                   - зібрати"
	@echo "make CROSS_COMPILE=aarch64-linux-gnu-  - для Raspberry Pi 5"
	@echo "make STATIC=1                          - статична збірка"
	@echo "make DEBUG=1                           - з відладкою"
	@echo "make test                              - тести"
	@echo "make format-check | format             - стиль коду"
	@echo "make clean | help"

.PHONY: all test format-check format clean help
