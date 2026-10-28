/**
 * @file csv_player.c
 * @brief Реалізація програвача CSV. Опис і правила дивись у csv_player.h.
 */
#include "csv_player.h"

#include <errno.h>
#include <math.h>
#include <time.h>

#include "accel_csv.h"
#include "io_util.h"

#define MSEC_PER_SEC 1000
#define NSEC_PER_MSEC 1000000L

/* Приватні функції описано тут, а визначено внизу: публічна функція йде першою. */
static int read_accel(struct sensor *self, struct accel_sample *out);
static void close_accel(struct sensor *self);
static int read_next_line_sample(struct csv_player *player, struct accel_sample *out);
static int wait_until_due(struct csv_player *player, const struct accel_sample *sample);

int csv_player_open(struct csv_player *player, const char *path, double speed)
{
	if (player == NULL || path == NULL || isnan(speed) || speed < 0) {
		return -EINVAL;
	}

	player->file = fopen(path, "r");
	if (player->file == NULL) {
		return -errno;
	}

	player->base.read = read_accel;
	player->base.close = close_accel;
	player->speed = speed;
	player->started = false;
	player->start_ms = 0;
	player->has_pending = false;
	player->line_number = 0;

	return 0;
}

static int read_accel(struct sensor *self, struct accel_sample *out)
{
	struct csv_player *player = (struct csv_player *)self; /* base це перше поле */
	int result;

	if (!player->has_pending) {
		result = read_next_line_sample(player, &player->pending);
		if (result < 0) {
			return result;
		}
		player->has_pending = true;
	}

	result = wait_until_due(player, &player->pending);
	if (result < 0) {
		return result; /* -EINTR: відлік лишається в pending до наступної спроби */
	}

	*out = player->pending;
	player->has_pending = false;

	return 0;
}

static void close_accel(struct sensor *self)
{
	struct csv_player *player = (struct csv_player *)self;

	fclose(player->file);
	player->file = NULL;
}

/* Читає рядки, поки не знайде відлік. Кінець файлу це -ENODATA, зіпсований рядок це -EINVAL. */
static int read_next_line_sample(struct csv_player *player, struct accel_sample *out)
{
	char line[ACCEL_CSV_MAX_LINE];

	while (fgets(line, sizeof(line), player->file) != NULL) {
		int result;

		player->line_number++;
		result = accel_csv_parse_line(line, out);
		if (result == -ENODATA) {
			continue; /* порожній рядок чи коментар */
		}
		return result;
	}

	return ferror(player->file) ? -EIO : -ENODATA;
}

/*
 * Чекає, поки настане час відліку (t_ms / speed від початку програвання). Перший відлік
 * запускає годинник програвання. Повертає 0, коли час настав, -EINTR, якщо сигнал перервав сон.
 */
static int wait_until_due(struct csv_player *player, const struct accel_sample *sample)
{
	struct timespec due;
	int64_t due_ms;
	int error;

	if (player->speed == 0) {
		return 0;
	}
	if (!player->started) {
		player->start_ms =
		    monotonic_ms() - (int64_t)((double)sample->time_ms / player->speed);
		player->started = true;
	}

	due_ms = player->start_ms + (int64_t)((double)sample->time_ms / player->speed);
	due.tv_sec = due_ms / MSEC_PER_SEC;
	due.tv_nsec = (due_ms % MSEC_PER_SEC) * NSEC_PER_MSEC;

	error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);
	if (error == EINTR) {
		return -EINTR;
	}

	return error == 0 ? 0 : -error;
}
