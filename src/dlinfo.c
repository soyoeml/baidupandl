/* This is a multi-thread download tool, for pan.baidu.com
 *   		Copyright (C) 2015  Yang Zhang <yzfedora@gmail.com>
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *************************************************************************/
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>		/* strtol() */
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <errno.h>
#include <curl/curl.h>

#if (defined(__APPLE__) && defined(__MACH__))
# include <malloc/malloc.h>
#endif

#include "err_handler.h"
#include "dlinfo.h"
#include "dlpart.h"
#include "dlcommon.h"
#include "dllist.h"
#include "dlscrolling.h"
#include "dlbuffer.h"


#define PACKET_ARGS(pkt, dl, dp, start, end, no) 	\
	do {						\
		if ((pkt = malloc(sizeof(*(pkt))))) {	\
			(pkt)->arg_dl = dl;		\
			(pkt)->arg_dp = dp;		\
			(pkt)->arg_start = start;	\
			(pkt)->arg_end = end;		\
			(pkt)->arg_no = no;		\
		}					\
	} while (0)

#define UNPACKET_ARGS(pkt, dl, dp, start, end, no)	\
	do {						\
		dl = (pkt)->arg_dl;			\
		dp = (pkt)->arg_dp;			\
		start = (pkt)->arg_start;		\
		end = (pkt)->arg_end;			\
		no = (pkt)->arg_no;			\
	} while (0)

#define PACKET_ARGS_FREE(pkt)	do { free(pkt); } while (0)


static void *dlinfo_download(void *arg);

static struct args *dlinfo_args_pack(struct dlinfo *dl,
				     struct dlpart **dp,
				     size_t start,
				     size_t end,
				     int no)
{
	struct args *args = malloc(sizeof(*args));

	if (!args)
		return args;

	args->arg_dl = dl;
	args->arg_dp = dp;
	args->arg_start = start;
	args->arg_end = end;
	args->arg_no = no;
	return args;
}

static void dlinfo_args_unpack(struct args *args,
			       struct dlinfo **dl,
			       struct dlpart ***dp,
			       size_t *start,
			       size_t *end,
			       int *no)
{
	*dl = args->arg_dl;
	*dp = args->arg_dp;
	*start = args->arg_start;
	*end = args->arg_end;
	*no = args->arg_no;
	free(args);
}

/*
 * Receive the response from the server, which include the HEAD info. And
 * store result to dl->di_length, dl->di_filename.
 */
static int dlinfo_header_parsing(struct dlinfo *dl, char *header_buf)
{
	int n;
	int code;
	char tmp[DI_ENC_NAME_MAX];
	char *p;

	err_dbg(2, "--------------Received Meta info---------------\n"
						"%s\n", header_buf);

	if (strncmp(dl->di_url, "ftp://", 6)) {
		code = getrcode(header_buf);
		if (code < 200 || code >= 300) {
			return -1;
		}
	}

#define HEADER_CONTENT_LENGTH "Content-Length: "
	if ((p = dlstrcasestr(header_buf, HEADER_CONTENT_LENGTH))) {
		dl->di_length = strtol(p + sizeof(HEADER_CONTENT_LENGTH) - 1,
				       NULL, 10);
	}

	/* User specified filename */
	if (*dl->di_filename)
		return 0;

#define HEADER_FILENAME "filename="
	if ((p = dlstrcasestr(header_buf, HEADER_FILENAME))) {
		p = memccpy(tmp, p + sizeof(HEADER_FILENAME) - 1, '\n',
				DI_ENC_NAME_MAX);
		if (!p) {
			err_sys("memccpy");
			return -1;
		}
		strncpy(dl->di_filename, string_decode(tmp),
			sizeof(dl->di_filename) - 1);
		return 0;
	}

	/* if filename parsing failed, then parsing filename from url. */
	if ((p = strrchr(dl->di_url, '/'))) {
		strcpy(tmp, p + 1);
		strncpy(dl->di_filename, string_decode(tmp),
			sizeof(dl->di_filename) - 1);
		return 0;
	}

	return -1;
}

static int dlinfo_header_parsing_all(struct dlinfo *dl, char *header_buf)
{
	char *sep = header_buf;

	if (!header_buf)
		return -1;

	do {
		if (!dlinfo_header_parsing(dl, sep))
			return 0;

		if ((sep = strstr(sep, "\r\n\r\n")))
			sep += 4;
	} while (sep);

	return -1;
}

static size_t dlinfo_curl_header_callback(char *buf,
					  size_t size,
					  size_t nmemb,
					  void *userdata)
{
	size_t len = size * nmemb;
	struct dlbuffer *db = (struct dlbuffer *)userdata;

	if (dlbuffer_write(db, buf, len) < 0)
		return 0;

	/*
	 * force to terminate libcurl to call this write function, because
	 * we have get entire header.
	 */
	if (!strcmp(buf, "\r\n"))
		return 0;

	return len;
}

static int dlinfo_init_without_head(struct dlinfo *dl)
{
	int ret = -1;
	CURL *curl;
	CURLcode rc;
	struct dlbuffer *db;

	if (!(db = dlbuffer_new())) {
		err_sys("dlbuffer_new");
		goto out;
	}

	if (!(curl = curl_easy_init())) {
		err_msg("curl_easy_init");
		goto out;
	}

	curl_easy_setopt(curl, CURLOPT_URL, dl->di_url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_HEADER, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
			 dlinfo_curl_header_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, db);

	if ((rc = curl_easy_perform(curl)) && rc != CURLE_WRITE_ERROR) {
		err_msg("curl_easy_perform: %s", curl_easy_strerror(rc));
		goto out;;
	}

	if (dlinfo_header_parsing_all(dl, db->buf))
		goto out;

	err_dbg(1, "filename=%s, length=%ld\n", dl->di_filename, dl->di_length);
	ret = 0;
out:
	if (db)
		dlbuffer_free(db);
	if (curl)
		curl_easy_cleanup(curl);
	return ret;
}

static int dlinfo_init(struct dlinfo *dl)
{
	int ret = -1;
	CURL *curl;
	CURLcode rc;
	struct dlbuffer *db;

	if (!(db = dlbuffer_new())) {
		err_sys("dlbuffer_new");
		goto out;
	}

	if (!(curl = curl_easy_init())) {
		err_msg("curl_easy_init");
		goto out;
	}

	curl_easy_setopt(curl, CURLOPT_URL, dl->di_url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
			 dlinfo_curl_header_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, db);

	if ((rc = curl_easy_perform(curl)) && rc != CURLE_WRITE_ERROR) {
		err_msg("curl_easy_perform: %s", curl_easy_strerror(rc));
		goto out;;
	}

	if (dlinfo_header_parsing_all(dl, db->buf))
		goto out;

	err_dbg(1, "filename=%s, length=%ld\n", dl->di_filename, dl->di_length);
	ret = 0;
out:
	if (db)
		dlbuffer_free(db);
	if (curl)
		curl_easy_cleanup(curl);
	return ret;
}

/*
 * dlinfo_records_* functions is NOT Multi-Threads Safe, because it using
 * file's offset to read data. any more than one threads read from it at
 * same time will cause unknown results.
 */
static void dlinfo_records_recovery(struct dlinfo *dl, void *buf,
				    ssize_t len)
{
	if (read(dl->di_local, buf, len) != len)
		err_exit("records recovery occur errors");
}

static int dlinfo_download_is_finished(struct dlinfo *dl)
{
	ssize_t real = lseek(dl->di_local, 0, SEEK_END);
	return (real == dl->di_length) ? 1 : 0;
}

static int dlinfo_records_recovery_nthreads(struct dlinfo *dl)
{
	ssize_t filelen = lseek(dl->di_local, 0, SEEK_END);
	ssize_t recordlen;
	int save_nthreads = dl->di_nthreads;

	/* seek to the start of the records. and retriving number of threads. */
	lseek(dl->di_local, dl->di_length, SEEK_SET);
	dlinfo_records_recovery(dl, &dl->di_nthreads,
				sizeof(dl->di_nthreads));

	/* calculate the records length. it should be equal the ('filelen' -
	 * dl->di_length) */
	recordlen = dl->di_nthreads * 2 * sizeof(dl->di_threads->dp->dp_start)
		+ sizeof(dl->di_nthreads);


	/* unrecognized record format, setting try_ignore_records flags */
	if (dl->di_length + recordlen != filelen) {
		dl->di_nthreads = save_nthreads;
		dl->di_try_ignore_records = 1;
		return -1;
	}
	return 0;
}

/*
 * Recovery the 'total_read' field, and the range of per threads should
 * download where start from.
 */
static ssize_t dlinfo_records_recovery_all(struct dlinfo *dl)
{
	int i, s;
	ssize_t start;
	ssize_t end;
	ssize_t nedl = 0;
	struct dlthreads **dt = &dl->di_threads;
	struct args *args = NULL;

	if (dlinfo_records_recovery_nthreads(dl) == -1)
		return -1;

	/* this isn't necessary, but for a non-dependencies impl. */
	lseek(dl->di_local, dl->di_length + sizeof(dl->di_nthreads), SEEK_SET);
	for (i = 0; i < dl->di_nthreads; i++) {
		struct packet_args *pkt;
		/*
		 * if dt is second pointer in the linked list, dt = dt->next.
		 * we need to malloc a block memory for it.
		 */
		if (!*dt) {
			if (!(*dt = malloc(sizeof(**dt))))
				err_exit("malloc");
			memset(*dt, 0, sizeof(**dt));
		}

		dlinfo_records_recovery(dl, &start, sizeof(start));
		dlinfo_records_recovery(dl, &end, sizeof(end));
		if (start > end) {
			/* 
			 * if this range is finished, set 'dt->thread' and
			 * 'dt->dp both to 0 or NULL properly'.
			 */
			if (start != end + 1)
				err_exit("recovery error range: %ld-%ld\n",
					 start, end);
			/* 
			 * Set members of *dt to 0 or NULL. subtract 1 for
			 * finished part.
			 */
			(*dt)->thread = 0;
			(*dt)->dp = NULL;
			goto next_range;
		}

		if (!(args = dlinfo_args_pack(dl, &(*dt)->dp, start, end, i)))
			err_exit("packet arguments");

		if ((s = pthread_create(&(*dt)->thread, NULL,
					dlinfo_download, args)) != 0) {
			errno = s;
			err_exit("pthread_create");
		}

		nedl += (end - start) + 1;
next_range:
		dt = &((*dt)->next);
	}

	dl->total_read_update(dl, dl->di_total - nedl);
	return dl->di_total_read;
}


static void dlinfo_records_removing(struct dlinfo *dl)
{
	if (ftruncate(dl->di_local, dl->di_length) == -1)
		err_exit("ftruncate");
}

/*
 * Open a file descriptor for store the download data, and store the
 * file descriptor to variable dl->di_local.
 */
static int dlinfo_open_local_file(struct dlinfo *dl)
{
#define PERMS (S_IRUSR | S_IWUSR)
	int fd;
	int ret;
	int flags = O_RDWR | O_CREAT | O_EXCL;

	if ((fd = open(dl->di_filename, flags, PERMS)) == -1) {
		if (errno == EEXIST) {
			/*
			 * same file already exists. try recovery records
			 */
			flags &= ~O_EXCL;
			if ((fd = open(dl->di_filename, flags, PERMS)) == -1)
				err_exit("open");

			dl->di_recovery = 1;
			goto dlinfo_open_local_file_return;
		}
		err_exit("open");
	}

	/* If no file exists, append number of threads records to the file. */
try_pwrite_nthreads_again:
	ret = pwrite(fd, &dl->di_nthreads,
		     sizeof(dl->di_nthreads), dl->di_length);
	if (ret != sizeof(dl->di_nthreads))
		goto try_pwrite_nthreads_again;

dlinfo_open_local_file_return:
	dl->di_local = fd;
	return fd;
}

/*
 * Construct a fixed prompt, like: "download foo.zip, size 128MB    16%".
 * and return a pointer to the prompt string.
 */
static char *dlinfo_prompt_set(struct dlinfo *dl)
{
	short flags = 0;
	ssize_t size;
	ssize_t orig_size;

	orig_size = size = dl->di_total;
	while (size > 1024) {
		size >>= 10;
		flags++;
	}

	*dl->di_prompt = 0;
	snprintf(dl->di_file_size_string, sizeof(dl->di_file_size_string),
		 "%6.1f%-5s",
		 orig_size / ((double) (1 << (10 * flags))),
		 (flags == 0) ? "Bytes" :
		 (flags == 1) ? "KiB" :
		 (flags == 2) ? "MiB" :
		 (flags == 3) ? "GiB" : "TiB");

	dl->di_sig_cnt = 0;

	dl->di_wincsz = getwcol();	/* initialize window column. */

	/*
	 * Initial roll displayed string, and expect maximum length.
	 */
	if (dlscrolling_init(dl->di_filename, dl->di_wincsz -
				DI_PROMPT_RESERVED) == -1)
		err_exit("Setting roll display function error");
	return dl->di_prompt;
}

/* if dl->di_filename is more than 30 bytes, then dynamically print the
 * full name of this file. */
static void dlinfo_prompt_update(struct dlinfo *dl)
{
	unsigned int len, padding;
	char *ptr = dlscrolling_ptr(&len, &padding);

	snprintf(dl->di_prompt, sizeof(dl->di_prompt), "%.*s  %*s%s", len, ptr,
		 padding, "", dl->di_file_size_string);
	dl->di_sig_cnt++;
}

/*
 * To prevent the round up of snprintf().
 * example:
 * 	99.97 may be round up to 100.0
 */
#define DI_PERCENTAGE_STR_MAX	32
static char *dlinfo_get_percentage(struct dlinfo *dl)
{
	int len;
	static char percentage_str[DI_PERCENTAGE_STR_MAX];

	len = snprintf(percentage_str, sizeof(percentage_str), "%6.2f",
			(double)dl->di_total_read * 100 / dl->di_total);

	percentage_str[len - 1] = 0;        /* prevent snprintf round up */
	return percentage_str;
}

#define DI_ESTIMATE_STR_MAX 16
static char *dlinfo_get_estimate(struct dlinfo *dl)
{
	int hours, mins, secs;
	static char estimate_str[DI_ESTIMATE_STR_MAX];

	secs = (dl->di_total - dl->di_total_read) /
	       ((dl->di_bps > 0) ? dl->di_bps : 1024);

	hours = secs / 3600;
	secs = secs % 3600;
	mins = secs / 60;
	secs = secs % 60;

	if (hours < 100)
		snprintf(estimate_str, sizeof(estimate_str), "%02d:%02d:%02d",
			 hours, mins, secs);
	else
		snprintf(estimate_str, sizeof(estimate_str), "%3d days", hours / 24);

	return estimate_str;
}

static int dlinfo_get_strnum(int curr)
{
	int nstr = 0;

	if (curr < 10)
		nstr += 1;
	else if (curr < 100)
		nstr += 2;
	else if (curr < 1000)
		nstr += 3;
	else
		nstr += 4;

	return nstr;
}

static void dlinfo_sigalrm_handler(int signo)
{
	struct dlinfo *dl = dllist_get();
	ssize_t speed = dl->di_bps;
	int curr = dl->di_nthreads_curr;


	dlscrolling_setsize(dl->di_wincsz - DI_PROMPT_RESERVED - dlinfo_get_strnum(curr));
	dlinfo_prompt_update(dl);
	printf("\r" "%*s", dl->di_wincsz, "");

	speed >>= 10;
	if (speed < 1000) {
		printf("\r\e[48;5;161m\e[30m%s %s%%  %4ld%s/s  %8s  [%d]\e[0m",
		       dl->di_prompt, dlinfo_get_percentage(dl), (long)speed, "KiB",
		       dlinfo_get_estimate(dl), curr);
	} else if (speed > 1000) {
		printf("\r\e[48;5;161m\e[30m%s %s%%  %4.3g%s/s  %8s  [%d]\e[0m",
		       dl->di_prompt, dlinfo_get_percentage(dl),
		       (long)speed / 1000.0, "MiB", dlinfo_get_estimate(dl),
		       curr);
	}

	fflush(stdout);
	dl->bps_reset(dl);
}

static void dlinfo_sigwinch_handler(int signo)
{
	struct dlinfo *dl = dllist_get();
	dl->di_wincsz = getwcol();
}

static void dlinfo_register_signal_handler(void)
{
	struct sigaction act, old;

	memset(&act, 0, sizeof(act));
	act.sa_flags |= SA_RESTART;
	act.sa_handler = dlinfo_sigalrm_handler;

	/* 
	 * Register the SIGALRM handler, for print the progress of download.
	 */
	if (sigaction(SIGALRM, &act, &old) == -1)
		err_exit("sigaction - SIGALRM");

	/*
	 * Register the SIGWINCH signal, for adjust the output length of
	 * progress when user change the window size.
	 */
	act.sa_handler = dlinfo_sigwinch_handler;
	if (sigaction(SIGWINCH, &act, &old) == -1)
		err_exit("sigaction - SIGWINCH");
}


static void dlinfo_sigalrm_detach(void)
{
	if (setitimer(ITIMER_REAL, NULL, NULL) == -1)
		err_sys("setitimer");
}

static void dlinfo_alarm_launch(void)
{
	struct itimerval new;

	new.it_interval.tv_sec = 1;
	new.it_interval.tv_usec = 0;
	new.it_value.tv_sec = 1;
	new.it_value.tv_usec = 0;

	if (setitimer(ITIMER_REAL, &new, NULL) == -1)
		err_sys("setitimer");
}

/*
 * Use this thread to download partial data.
 */
static void *dlinfo_download(void *arg)
{
	int try_times = 0;
	int orig_no;
	size_t orig_start, orig_end;
	struct dlpart **dp = NULL;
	struct dlinfo *dl = NULL;

	/* Unpacket the struct packet_args. */
	dlinfo_args_unpack((struct args *)arg, &dl, &dp,
			   &orig_start, &orig_end, &orig_no);

	if (!(*dp = dlpart_new(dl, orig_start, orig_end, orig_no))) {
		err_msg("error, try download range: %ld - %ld again",
						orig_start, orig_end);
		return NULL;
	}

	err_dbg(1, "\nthread %d starting to download range: %ld-%ld\n",
			(*dp)->dp_no, (*dp)->dp_start, (*dp)->dp_end);
	dl->nthreads_inc(dl);
	while ((*dp)->dp_start < (*dp)->dp_end) {
		if (try_times++ > DI_TRY_TIMES_MAX) {
			err_msg("thread %d failed to download range: %ld-%ld",
				(*dp)->dp_no, (*dp)->dp_start, (*dp)->dp_end);
			break;
		}

		(*dp)->launch(*dp);
		orig_start = (*dp)->dp_start;
		orig_end = (*dp)->dp_end;
		(*dp)->delete(*dp);

		if (!(*dp = dlpart_new(dl, orig_start, orig_end, orig_no))) {
			err_msg("error, try download range: %ld - %ld again",
				orig_start, orig_end);
			return NULL;
		}
		sleep(1);
	}
	dl->nthreads_dec(dl);

	return NULL;
}

static int dlinfo_range_generator(struct dlinfo *dl)
{
	int i, s;
	ssize_t pos = 0;
	ssize_t size = dl->di_length / dl->di_nthreads;
	ssize_t start;
	ssize_t end;
	struct dlthreads **dt = &dl->di_threads;
	struct args *args = NULL;

	for (i = 0; i < dl->di_nthreads; i++) {

		/* if dt is second pointer in the linked, dt = dt->next.
		 * we need to malloc a block memory for it.
		 */
		if (!*dt) {
			if (!(*dt = malloc(sizeof(**dt))))
				return -1;
		}

		(*dt)->next = NULL;
		start = pos;
		pos += size;
		if (i != dl->di_nthreads - 1)
			end = pos - 1;
		else
			end = dl->di_length - 1;

		if (!(args = dlinfo_args_pack(dl, &(*dt)->dp, start, end, i)))
			err_exit("packet arguments");

		if ((s = pthread_create(&(*dt)->thread, NULL,
					dlinfo_download, args)) != 0) {
			errno = s;
			err_exit("pthread_create");
		}

		dt = &((*dt)->next);
	}
	return 0;
}

void dlinfo_launch(struct dlinfo *dl)
{
	int s;
	struct dlthreads *dt;

	/* 
	 * Before we create threads to start download, we set the download
	 * prompt first. and set the alarm too.
	 */
	dlinfo_prompt_set(dl);
	dlinfo_register_signal_handler();
	dlinfo_alarm_launch();

launch:
	/* 
	 * Set offset of the file to the start of records, and recovery
	 * number of threads to dl->di_nthreads.
	 */
	if (dl->di_recovery) {
		/* 
		 * if file has exist, and it's length is equal to bytes
		 * which need download bytes. so it has download finished.
		 */
		if (dlinfo_download_is_finished(dl))
			return;

		/* can't recovery records normally, try NOT use records again */
		if (dlinfo_records_recovery_all(dl) == -1)
			goto try_ignore_records_again;
	} else {
		if (dlinfo_range_generator(dl) == -1)
			return;
	}

	/* Waiting the threads that we are created above. */
	dt = dl->di_threads;
	while (dt) {
		/*
		 * Since we put all dlpart_new() into the download(). when
		 * all pthread_create() done, only ensure the dt->thread is
		 * set by pthread_create().
		 */
		if (dt->thread) {
			if ((s = pthread_join(dt->thread, NULL)) != 0) {
				errno = s;
				err_sys("pthread_join");
			}
		}
		dt = dt->next;
	}

	/* 
	 * Force the flush the output prompt, and clear the timer, and
	 * removing the records which we written in the end of file.
	 */
	dlinfo_sigalrm_handler(SIGALRM);
	dlinfo_sigalrm_detach();
	if (dl->di_total_read == dl->di_total)
		dlinfo_records_removing(dl);
	printf("\n");
	return;

try_ignore_records_again:
	if (dl->di_try_ignore_records) {
		dl->di_try_ignore_records = 0;
		dl->di_recovery = 0;

		if (close(dl->di_local) == -1)
			err_sys("close");
		if (remove(dl->di_filename) == -1)
			err_sys("remove");

		dlinfo_open_local_file(dl);
		goto launch;
	}
}

void dlinfo_delete(struct dlinfo *dl)
{
	struct dlthreads *dt = dl->di_threads;

	pthread_mutex_destroy(&dl->di_mutex);

	/* close the local file descriptor */
	if (dl->di_local >= 0 && close(dl->di_local) == -1)
		err_sys("close");

	while (dt) {
		if (dt->dp) {
			dt->dp->delete(dt->dp);
		}
		dt = dt->next;
	}
	free(dl);
	curl_global_cleanup();
}

static void dlinfo_nthreads_running_inc(struct dlinfo *dl)
{
	pthread_mutex_lock(&dl->di_mutex);
	dl->di_nthreads_curr++;
	pthread_mutex_unlock(&dl->di_mutex);
}

static void dlinfo_nthreads_running_dec(struct dlinfo *dl)
{
	pthread_mutex_lock(&dl->di_mutex);
	dl->di_nthreads_curr--;
	pthread_mutex_unlock(&dl->di_mutex);
}

static void dlinfo_total_read_update(struct dlinfo *dl, size_t n)
{
	pthread_mutex_lock(&dl->di_mutex);
	dl->di_total_read += n;
	pthread_mutex_unlock(&dl->di_mutex);
}

static void dlinfo_bps_update(struct dlinfo *dl, size_t bps)
{
	pthread_mutex_lock(&dl->di_mutex);
	dl->di_bps += bps;
	pthread_mutex_unlock(&dl->di_mutex);
}

static void dlinfo_bps_reset(struct dlinfo *dl)
{
	pthread_mutex_lock(&dl->di_mutex);
	dl->di_bps = 0;
	pthread_mutex_unlock(&dl->di_mutex);
}

struct dlinfo *dlinfo_new(char *url, char *filename, int nthreads)
{
	int try_times = 0;
	struct dlinfo *dl;

	if (!(dl = calloc(1, sizeof(*dl))))
		return NULL;

	pthread_mutex_init(&dl->di_mutex, NULL);
	curl_global_init(CURL_GLOBAL_ALL);

	memset(dl, 0, sizeof(*dl));
	strcpy(dl->di_url, url);
	if (filename)
		strcpy(dl->di_filename, filename);

	dl->di_nthreads = nthreads;

	dl->nthreads_inc = dlinfo_nthreads_running_inc;
	dl->nthreads_dec = dlinfo_nthreads_running_dec;
	dl->total_read_update = dlinfo_total_read_update;
	dl->bps_update = dlinfo_bps_update;
	dl->bps_reset = dlinfo_bps_reset;
	dl->launch = dlinfo_launch;
	dl->delete = dlinfo_delete;

	/* 
	 * parsing the url, for remote server's service and hostname.
	 * and establish a temporary connection used to send HTTP HEAD
	 * request, that we can retriving the length and filename.
	 */
	if (dlinfo_init(dl) && dlinfo_init_without_head(dl)) {
		dl->delete(dl);
		return NULL;
	}

	dlinfo_open_local_file(dl);

	dl->di_total = dl->di_length;	/* Set global variable 'total' */

	/*
	 * Put this dl pointer into a global list, the signal handler will
	 * retrieve it later. this will allow the program to launch multiple
	 * download task.
	 */
	dllist_put(dl);
	return dl;
}
