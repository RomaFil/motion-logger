/**
 * @file test_sample_reader.c
 * @brief Перевірки читача відліків sample_reader на справжньому каналі (pipe).
 *
 * Запуск: make test (звичайний) або make test-tsan (з перевіркою гонок даних).
 * Код виходу 0 означає, що всі перевірки пройшли. Заліза не потрібно.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "ring_buffer.h"
#include "sample_reader.h"

#define MANY_SAMPLES 1000
#define DRIBBLE_SAMPLES 200
#define SMALL_CAPACITY 3
#define OVERFLOW_SAMPLES 10

/* Скільки перевірок не пройшло. main повертає це як код виходу. */
static int failures;

/* Що потік-писар має записати в канал і яким кроком (скільки байтів за один write). */
struct writer_job {
	int fd;
	const uint8_t *data;
	size_t length;
	size_t step;
};

/* Пише дані в канал порціями по step байтів, потім закриває канал: читач побачить кінець. */
static void *write_and_close(void *argument)
{
	struct writer_job *job = argument;
	size_t offset = 0;

	while (offset < job->length) {
		size_t piece = job->length - offset;

		if (piece > job->step) {
			piece = job->step;
		}
		if (write(job->fd, job->data + offset, piece) < 0) {
			break;
		}
		offset += piece;
	}
	close(job->fd);

	return NULL;
}

/*
 * Створює канал, запускає писаря з num_samples числами 0, 1, 2, ... (по step байтів за write),
 * запускає читача й чекає його завершення. Підсумок читача лягає в stats.
 */
static int run_reader_against_writer(struct ring_buffer *rb, size_t num_samples, size_t step,
				     struct sample_reader_stats *stats)
{
	static volatile sig_atomic_t never_stop;
	int32_t values[MANY_SAMPLES];
	int pipe_fds[2];
	struct writer_job job;
	pthread_t writer;
	int result;
	size_t i;

	for (i = 0; i < num_samples; i++) {
		values[i] = (int32_t)i;
	}
	CHECK(pipe(pipe_fds) == 0);

	job.fd = pipe_fds[1];
	job.data = (const uint8_t *)values;
	job.length = num_samples * sizeof(values[0]);
	job.step = step;
	CHECK(pthread_create(&writer, NULL, write_and_close, &job) == 0);

	result = sample_reader_run(pipe_fds[0], rb, &never_stop, stats);

	pthread_join(writer, NULL);
	close(pipe_fds[0]);

	return result;
}

/* Забирає з буфера num_samples чисел і перевіряє, що це 0, 1, 2, ... без пропусків. */
static void check_buffer_holds_sequence(struct ring_buffer *rb, size_t num_samples)
{
	size_t i;

	for (i = 0; i < num_samples; i++) {
		int32_t value = -1;

		CHECK(ring_buffer_pop(rb, &value) == 0);
		CHECK(value == (int32_t)i);
	}
	CHECK(ring_buffer_is_empty(rb));
}

static void test_run_rejects_bad_arguments(void)
{
	static volatile sig_atomic_t never_stop;
	int32_t storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct sample_reader_stats stats;

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);

	CHECK(sample_reader_run(-1, &rb, &never_stop, &stats) == -EINVAL);
	CHECK(sample_reader_run(0, NULL, &never_stop, &stats) == -EINVAL);
	CHECK(sample_reader_run(0, &rb, NULL, &stats) == -EINVAL);
	CHECK(sample_reader_run(0, &rb, &never_stop, NULL) == -EINVAL);

	ring_buffer_destroy(&rb);
}

static void test_all_samples_arrive_in_order(void)
{
	int32_t storage[MANY_SAMPLES];
	struct ring_buffer rb;
	struct sample_reader_stats stats;

	CHECK(ring_buffer_init(&rb, storage, MANY_SAMPLES) == 0);

	CHECK(run_reader_against_writer(&rb, MANY_SAMPLES, sizeof(int32_t), &stats) == 0);
	CHECK(stats.received == MANY_SAMPLES);
	CHECK(stats.dropped == 0);
	check_buffer_holds_sequence(&rb, MANY_SAMPLES);

	ring_buffer_destroy(&rb);
}

/* Писар віддає по 1 байту: кожне число приходить розірваним на чотири частини. */
static void test_samples_split_across_reads_are_reassembled(void)
{
	int32_t storage[DRIBBLE_SAMPLES];
	struct ring_buffer rb;
	struct sample_reader_stats stats;

	CHECK(ring_buffer_init(&rb, storage, DRIBBLE_SAMPLES) == 0);

	CHECK(run_reader_against_writer(&rb, DRIBBLE_SAMPLES, 1, &stats) == 0);
	CHECK(stats.received == DRIBBLE_SAMPLES);
	CHECK(stats.dropped == 0);
	check_buffer_holds_sequence(&rb, DRIBBLE_SAMPLES);

	ring_buffer_destroy(&rb);
}

static void test_full_buffer_counts_dropped_and_keeps_oldest(void)
{
	int32_t storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct sample_reader_stats stats;

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);

	CHECK(run_reader_against_writer(&rb, OVERFLOW_SAMPLES, sizeof(int32_t), &stats) == 0);
	CHECK(stats.received == OVERFLOW_SAMPLES);
	CHECK(stats.dropped == OVERFLOW_SAMPLES - SMALL_CAPACITY);
	check_buffer_holds_sequence(&rb, SMALL_CAPACITY);

	ring_buffer_destroy(&rb);
}

/* Прапорець зупинки вже піднято, а канал ще відкритий: читач має одразу повернутись. */
static void test_stop_flag_ends_reader_while_source_is_open(void)
{
	static volatile sig_atomic_t already_stopped = 1;
	int32_t storage[SMALL_CAPACITY];
	struct ring_buffer rb;
	struct sample_reader_stats stats;
	int pipe_fds[2];

	CHECK(ring_buffer_init(&rb, storage, SMALL_CAPACITY) == 0);
	CHECK(pipe(pipe_fds) == 0);

	CHECK(sample_reader_run(pipe_fds[0], &rb, &already_stopped, &stats) == 0);
	CHECK(stats.received == 0);

	close(pipe_fds[0]);
	close(pipe_fds[1]);
	ring_buffer_destroy(&rb);
}

int main(void)
{
	test_run_rejects_bad_arguments();
	test_all_samples_arrive_in_order();
	test_samples_split_across_reads_are_reassembled();
	test_full_buffer_counts_dropped_and_keeps_oldest();
	test_stop_flag_ends_reader_while_source_is_open();

	if (failures != 0) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("sample_reader: all checks passed\n");

	return 0;
}
