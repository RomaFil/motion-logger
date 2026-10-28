# Makefile застосунку motion_logger.
#   make                                    збірка на цьому комп'ютері (motion_logger і sensor_sim)
#   make CROSS_COMPILE=aarch64-linux-gnu-   збірка для Raspberry Pi 5
#   make STATIC=1                           статична збірка (без залежності від libc на цілі)
#   make DEBUG=1                            з відладочною інформацією
#   make test                               зібрати й запустити тести (лише на цьому комп'ютері)
#   make SANITIZE=address,undefined test    те саме з перевіркою пам'яті (ASan + UBSan; make clean до і після)
#   make test-tsan                          те саме з ThreadSanitizer (перевірка гонок даних)
#   make analyze                            зібрати інструмент аналізу запису (analyze_recording)
#   make check-samples                      звірити звіт на tests/data з еталоном і з ground_truth.txt
#   make update-sample-report               переписати еталон (після заміни даних на справжні)
#   make format-check | format              перевірити / виправити стиль коду (clang-format)
#   make install [DESTDIR=...] [PREFIX=...] встановити motion_logger (за замовчуванням в /usr/bin)
#   make -n                                 показати команди, нічого не виконуючи
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

# CFLAGS ?= $(OPT): рівень оптимізації лише за замовчуванням, його можна замінити з командного
# рядка (так робить Buildroot). А обов'язкові прапорці нижче дописуємо через override: інакше
# CFLAGS з командного рядка перекрила б наші -std/-Wall/-MMD.
CFLAGS ?= $(OPT)
override CFLAGS += -std=c11 -Wall -Wextra -MMD -MP -pthread -D_POSIX_C_SOURCE=200809L -Iinterfaces -Icore
LDFLAGS ?=
override LDFLAGS += -pthread

# make SANITIZE=address,undefined test: усі тести з перевіркою пам'яті (потрібен make clean перед і після)
ifneq ($(SANITIZE),)
  override CFLAGS += -g -fsanitize=$(SANITIZE)
  override LDFLAGS += -fsanitize=$(SANITIZE)
endif
ifeq ($(STATIC),1)
  override LDFLAGS += -static
endif

TARGET := $(BUILD)/motion_logger
SRCS := main.c core/ring_buffer.c core/sample_reader.c core/log_writer.c core/io_util.c
OBJS := $(addprefix $(BUILD)/,$(SRCS:.c=.o))

# Імітатор датчика (пише числа в stdout раз на 10 мс): окрема програма, core-файли їй не потрібні.
SIM_TARGET := $(BUILD)/sensor_sim
SIM_OBJS := $(BUILD)/tools/sensor_sim.o $(BUILD)/core/io_util.o

# Інструмент аналізу запису (крок 3.5): make analyze. У make all не входить, на плату не ставиться.
ANALYZE_TARGET := $(BUILD)/analyze_recording
ANALYZE_OBJS := $(BUILD)/tools/analyze_recording.o $(BUILD)/core/accel_csv.o \
	$(BUILD)/core/motion_report.o $(BUILD)/core/motion_events.o $(BUILD)/core/motion_filter.o \
	$(BUILD)/core/nmea.o $(BUILD)/core/nmea_parser.o

# Тести: окремі програми з тих самих core-файлів (без main.c), запускаються командою make test.
TEST_TARGETS := $(BUILD)/test_ring_buffer $(BUILD)/test_sample_reader $(BUILD)/test_log_writer \
	$(BUILD)/test_nmea $(BUILD)/test_nmea_parser $(BUILD)/test_motion_filter \
	$(BUILD)/test_accel_csv $(BUILD)/test_motion_events $(BUILD)/test_motion_report
TEST_OBJS := $(BUILD)/tests/test_ring_buffer.o $(BUILD)/tests/test_sample_reader.o \
	$(BUILD)/tests/test_log_writer.o $(BUILD)/core/ring_buffer.o $(BUILD)/core/sample_reader.o \
	$(BUILD)/core/log_writer.o $(BUILD)/core/io_util.o $(BUILD)/tests/test_nmea.o $(BUILD)/core/nmea.o \
	$(BUILD)/tests/test_nmea_parser.o $(BUILD)/core/nmea_parser.o \
	$(BUILD)/tests/test_motion_filter.o $(BUILD)/core/motion_filter.o \
	$(BUILD)/tests/test_accel_csv.o $(BUILD)/core/accel_csv.o \
	$(BUILD)/tests/test_motion_events.o $(BUILD)/core/motion_events.o \
	$(BUILD)/tests/test_motion_report.o $(BUILD)/core/motion_report.o \
	$(ANALYZE_OBJS)

DEPS := $(OBJS:.o=.d) $(SIM_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

# Форматер: версія зафіксована в .clang-format (23.1.1). Інша команда: make format-check CLANG_FORMAT=...
CLANG_FORMAT ?= clang-format
FORMAT_FILES := $(wildcard main.c core/*.c core/*.h interfaces/*.h tests/*.c tests/*.h tools/*.c)

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
		$(BUILD)/core/ring_buffer.o $(BUILD)/core/io_util.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_log_writer: $(BUILD)/tests/test_log_writer.o $(BUILD)/core/log_writer.o \
		$(BUILD)/core/io_util.o $(BUILD)/core/ring_buffer.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_nmea: $(BUILD)/tests/test_nmea.o $(BUILD)/core/nmea.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_nmea_parser: $(BUILD)/tests/test_nmea_parser.o $(BUILD)/core/nmea_parser.o \
		$(BUILD)/core/nmea.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_motion_filter: $(BUILD)/tests/test_motion_filter.o $(BUILD)/core/motion_filter.o
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(BUILD)/test_accel_csv: $(BUILD)/tests/test_accel_csv.o $(BUILD)/core/accel_csv.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_motion_events: $(BUILD)/tests/test_motion_events.o $(BUILD)/core/motion_events.o
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_motion_report: $(BUILD)/tests/test_motion_report.o $(BUILD)/core/motion_report.o \
		$(BUILD)/core/motion_events.o $(BUILD)/core/motion_filter.o
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(ANALYZE_TARGET): $(ANALYZE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

analyze: $(ANALYZE_TARGET)

# Запис у tests/data: звіт збігається з еталоном і з тим, що відбувалось (ground_truth.txt).
check-samples: $(ANALYZE_TARGET)
	sh tests/check_samples.sh $(ANALYZE_TARGET) tests/data

update-sample-report: $(ANALYZE_TARGET)
	sh tests/check_samples.sh $(ANALYZE_TARGET) tests/data --update

test: $(TEST_TARGETS) check-samples
	@for test_program in $(TEST_TARGETS); do ./$$test_program || exit 1; done

# Те саме, але з перевіркою гонок даних (ThreadSanitizer): очікуємо 0 попереджень.
TSAN_FLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g -O1 -fsanitize=thread \
	-pthread -Iinterfaces -Icore -Itests
TSAN_TARGETS := $(addsuffix _tsan,$(TEST_TARGETS))

$(BUILD)/test_ring_buffer_tsan: tests/test_ring_buffer.c tests/check.h core/ring_buffer.c \
		core/ring_buffer.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_ring_buffer.c core/ring_buffer.c

$(BUILD)/test_sample_reader_tsan: tests/test_sample_reader.c tests/check.h core/sample_reader.c \
		core/sample_reader.h core/ring_buffer.c core/ring_buffer.h core/io_util.c core/io_util.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_sample_reader.c core/sample_reader.c core/ring_buffer.c \
		core/io_util.c

$(BUILD)/test_log_writer_tsan: tests/test_log_writer.c tests/check.h core/log_writer.c \
		core/log_writer.h core/io_util.c core/io_util.h core/ring_buffer.c core/ring_buffer.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_log_writer.c core/log_writer.c core/io_util.c \
		core/ring_buffer.c

$(BUILD)/test_nmea_tsan: tests/test_nmea.c tests/check.h core/nmea.c core/nmea.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_nmea.c core/nmea.c

$(BUILD)/test_nmea_parser_tsan: tests/test_nmea_parser.c tests/check.h core/nmea_parser.c \
		core/nmea_parser.h core/nmea.c core/nmea.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_nmea_parser.c core/nmea_parser.c core/nmea.c

$(BUILD)/test_motion_filter_tsan: tests/test_motion_filter.c tests/check.h core/motion_filter.c \
		core/motion_filter.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_motion_filter.c core/motion_filter.c -lm

$(BUILD)/test_accel_csv_tsan: tests/test_accel_csv.c tests/check.h core/accel_csv.c core/accel_csv.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_accel_csv.c core/accel_csv.c

$(BUILD)/test_motion_events_tsan: tests/test_motion_events.c tests/check.h core/motion_events.c \
		core/motion_events.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_motion_events.c core/motion_events.c

$(BUILD)/test_motion_report_tsan: tests/test_motion_report.c tests/check.h core/motion_report.c \
		core/motion_report.h core/motion_events.c core/motion_events.h core/motion_filter.c \
		core/motion_filter.h core/accel_csv.h
	@mkdir -p $(@D)
	$(CC) $(TSAN_FLAGS) -o $@ tests/test_motion_report.c core/motion_report.c \
		core/motion_events.c core/motion_filter.c -lm

test-tsan: $(TSAN_TARGETS)
	@for test_program in $(TSAN_TARGETS); do ./$$test_program || exit 1; done

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

# Звичайна установка: DESTDIR це корінь чужої файлової системи (так збирають пакети й образи),
# PREFIX це місце всередині неї. Buildroot-пакет копіює бінарник сам і цієї цілі не викликає.
PREFIX ?= /usr
INSTALL ?= install

install: $(TARGET)
	$(INSTALL) -D -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/motion_logger

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
	@echo "make install [DESTDIR=...]             - встановити motion_logger"
	@echo "make -n                                - лише показати команди"
	@echo "make analyze                            - інструмент аналізу запису"
	@echo "make check-samples                     - звірити звіт на tests/data"
	@echo "make clean | help"

.PHONY: all analyze check-samples update-sample-report test test-tsan format-check format install clean help
