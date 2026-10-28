# Makefile застосунку motion_logger.
#   make                                    збірка на цьому комп'ютері (motion_logger і sensor_sim)
#   make CROSS_COMPILE=aarch64-linux-gnu-   збірка для Raspberry Pi 5
#   make STATIC=1                           статична збірка (без залежності від libc на цілі)
#   make DEBUG=1                            з відладочною інформацією
#   make test                               зібрати й запустити тести (лише на цьому комп'ютері)
#   make test-tsan                          те саме з ThreadSanitizer (перевірка гонок даних)
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
override CFLAGS += -std=c11 -Wall -Wextra $(OPT) -MMD -MP -pthread -D_POSIX_C_SOURCE=200809L -Iinterfaces -Icore
LDFLAGS ?=
override LDFLAGS += -pthread
ifeq ($(STATIC),1)
  override LDFLAGS += -static
endif

TARGET := $(BUILD)/motion_logger
SRCS := core/main.c core/ring_buffer.c core/sample_reader.c core/log_writer.c core/io_util.c
OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o))

# Імітатор датчика (пише числа в stdout раз на 10 мс): окрема програма, core-файли їй не потрібні.
SIM_TARGET := $(BUILD)/sensor_sim
SIM_OBJS := $(BUILD)/tools/sensor_sim.o $(BUILD)/core/io_util.o

# Тести: окремі програми з тих самих core-файлів (без main.c), запускаються командою make test.
TEST_TARGETS := $(BUILD)/test_ring_buffer $(BUILD)/test_sample_reader $(BUILD)/test_log_writer
TEST_OBJS := $(BUILD)/tests/test_ring_buffer.o $(BUILD)/tests/test_sample_reader.o \
	$(BUILD)/tests/test_log_writer.o $(BUILD)/core/ring_buffer.o $(BUILD)/core/sample_reader.o \
	$(BUILD)/core/log_writer.o $(BUILD)/core/io_util.o

DEPS := $(OBJS:.o=.d) $(SIM_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

# Форматер: версія зафіксована в .clang-format (23.1.1). Інша команда: make format-check CLANG_FORMAT=...
CLANG_FORMAT ?= clang-format
FORMAT_FILES := $(wildcard core/*.c core/*.h tests/*.c tests/*.h tools/*.c)

all: $(TARGET) $(SIM_TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SIM_TARGET): $(SIM_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# залежності від .h, які записав gcc (-MMD); "-" = не помилка, якщо .d ще нема
-include $(DEPS)

$(BUILD)/test_ring_buffer: $(BUILD)/tests/test_ring_buffer.o $(BUILD)/core/ring_buffer.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_sample_reader: $(BUILD)/tests/test_sample_reader.o $(BUILD)/core/sample_reader.o \
		$(BUILD)/core/ring_buffer.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_log_writer: $(BUILD)/tests/test_log_writer.o $(BUILD)/core/log_writer.o \
		$(BUILD)/core/io_util.o $(BUILD)/core/ring_buffer.o
	$(CC) $(LDFLAGS) -o $@ $^

test: $(TEST_TARGETS)
	@for test_program in $(TEST_TARGETS); do ./$$test_program || exit 1; done

# Те саме, але з перевіркою гонок даних (ThreadSanitizer): очікуємо 0 попереджень.
TSAN_FLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g -O1 -fsanitize=thread \
	-pthread -Icore -Itests
TSAN_TARGETS := $(addsuffix _tsan,$(TEST_TARGETS))

$(BUILD)/test_ring_buffer_tsan: tests/test_ring_buffer.c tests/check.h core/ring_buffer.c \
		core/ring_buffer.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_ring_buffer.c core/ring_buffer.c

$(BUILD)/test_sample_reader_tsan: tests/test_sample_reader.c tests/check.h core/sample_reader.c \
		core/sample_reader.h core/ring_buffer.c core/ring_buffer.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_sample_reader.c core/sample_reader.c core/ring_buffer.c

$(BUILD)/test_log_writer_tsan: tests/test_log_writer.c tests/check.h core/log_writer.c \
		core/log_writer.h core/io_util.c core/io_util.h core/ring_buffer.c core/ring_buffer.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_log_writer.c core/log_writer.c core/io_util.c \
		core/ring_buffer.c

test-tsan: $(TSAN_TARGETS)
	@for test_program in $(TSAN_TARGETS); do ./$$test_program || exit 1; done

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

clean:
	rm -rf $(BUILD)

help:
	@echo "make                                   - зібрати motion_logger і sensor_sim"
	@echo "make CROSS_COMPILE=aarch64-linux-gnu-  - для Raspberry Pi 5"
	@echo "make STATIC=1                          - статична збірка"
	@echo "make DEBUG=1                           - з відладкою"
	@echo "make test                              - тести"
	@echo "make test-tsan                         - тести з ThreadSanitizer"
	@echo "make format-check | format             - стиль коду"
	@echo "make clean | help"

.PHONY: all test test-tsan format-check format clean help
