/* log.c — the one piece of global mutable state the library is allowed.
 *
 * Everything is under one mutex, and the mutex is statically initialized, so
 * logging is legal from any thread the moment the process starts — including
 * from a thread that is being torn down. */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kbc/kbc.h"
#include "kbc/log.h"
#include "kbc/mem.h"

/* KBC_MAX_* ceilings: a single log line must never be able to allocate more
 * than a bounded amount, however hostile the format arguments are. */
#define KBC_LOG_MSG_MAX 8192
#define KBC_LOG_TS_MAX 32

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static kbc_log_level log_level = KBC_LOG_INFO;
static bool log_json = false;

const char *kbc_log_level_str(kbc_log_level lvl) {
  switch (lvl) {
  case KBC_LOG_ERROR:
    return "error";
  case KBC_LOG_WARN:
    return "warn";
  case KBC_LOG_INFO:
    return "info";
  case KBC_LOG_DEBUG:
    return "debug";
  }
  return "info";
}

bool kbc_log_level_parse(const char *s, kbc_log_level *out) {
  if (s == NULL || out == NULL) {
    return false;
  }
  static const struct {
    const char *name;
    kbc_log_level level;
  } table[] = {
      {"error", KBC_LOG_ERROR}, {"warn", KBC_LOG_WARN},
      {"info", KBC_LOG_INFO},   {"debug", KBC_LOG_DEBUG},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    const char *want = table[i].name;
    size_t k = 0;
    while (s[k] != '\0' && want[k] != '\0') {
      char a = s[k];
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (a != want[k]) {
        break;
      }
      k++;
    }
    if (s[k] == '\0' && want[k] == '\0') {
      *out = table[i].level;
      return true;
    }
  }
  return false;
}

void kbc_log_init(kbc_log_level level, bool json_lines) {
  pthread_mutex_lock(&log_lock);
  log_level = level;
  log_json = json_lines;
  pthread_mutex_unlock(&log_lock);
}

void kbc_log_set_level(kbc_log_level level) {
  pthread_mutex_lock(&log_lock);
  log_level = level;
  pthread_mutex_unlock(&log_lock);
}

kbc_log_level kbc_log_get_level(void) {
  pthread_mutex_lock(&log_lock);
  kbc_log_level lvl = log_level;
  pthread_mutex_unlock(&log_lock);
  return lvl;
}

/* "2026-09-26T12:34:56.123Z" — UTC, milliseconds, always 24 chars. */
static void log_timestamp(char buf[KBC_LOG_TS_MAX]) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    snprintf(buf, KBC_LOG_TS_MAX, "-");
    return;
  }
  time_t secs = (time_t)ts.tv_sec;
  struct tm tm_buf;
  struct tm *tm = gmtime_r(&secs, &tm_buf);
  if (tm == NULL) {
    snprintf(buf, KBC_LOG_TS_MAX, "-");
    return;
  }
  int n = snprintf(buf, KBC_LOG_TS_MAX, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
                   tm->tm_min, tm->tm_sec, ts.tv_nsec / 1000000L);
  if (n < 0 || (size_t)n >= KBC_LOG_TS_MAX) {
    snprintf(buf, KBC_LOG_TS_MAX, "-");
  }
}

void kbc_log(kbc_log_level lvl, const char *fmt, ...) {
  if (fmt == NULL) {
    return;
  }

  pthread_mutex_lock(&log_lock);
  if (lvl > log_level) {
    pthread_mutex_unlock(&log_lock);
    return;
  }
  bool json = log_json;
  pthread_mutex_unlock(&log_lock);

  char msg[KBC_LOG_MSG_MAX];
  va_list ap;
  va_start(ap, fmt);
  /* vsnprintf truncates and always NUL-terminates, so a runaway expansion
   * loses its tail instead of smashing the stack. */
  int n = vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (n < 0) {
    msg[0] = '\0';
  }

  char ts[KBC_LOG_TS_MAX];
  log_timestamp(ts);
  const char *name = kbc_log_level_str(lvl);

  FILE *out = stderr;
  if (json) {
    kbc_str s;
    kbc_str_init(&s);
    /* The message is attacker-influenced (paths, queries), so it goes out
     * through the JSON escaper rather than raw. */
    if (kbc_str_puts(&s, "{\"ts\":") == KBC_OK &&
        kbc_str_append_json_string(&s, ts, strlen(ts)) == KBC_OK &&
        kbc_str_puts(&s, ",\"level\":") == KBC_OK &&
        kbc_str_append_json_string(&s, name, strlen(name)) == KBC_OK &&
        kbc_str_puts(&s, ",\"msg\":") == KBC_OK &&
        kbc_str_append_json_string(&s, msg, strlen(msg)) == KBC_OK &&
        kbc_str_puts(&s, "}\n") == KBC_OK) {
      fwrite(s.ptr, 1, s.len, out);
    } else {
      fputs("{\"level\":\"error\",\"msg\":\"log encoding failed\"}\n", out);
    }
    kbc_str_free(&s);
  } else {
    fprintf(out, "%s %s %s\n", ts, name, msg);
  }
  fflush(out);
}
