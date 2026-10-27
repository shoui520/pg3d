/*
 * com.unity3d.player.WWW: Unity's HTTP on Android is a Java thread per
 * request that reports through the class's natives (headers, data, progress,
 * error, done). Here that thread speaks plain HTTP/1.1 over sceNet, calling
 * the natives in the order the Java code does.
 *
 * Requests for the game's own hosts go to the private server: https becomes
 * http on its HTTP port, path and Host unchanged. Other hosts fail as on a
 * phone without a connection. Offline (no server set up), and for file: and
 * jar: URLs, requests are left pending as before.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>

#include "bionic/bthread.h"
#include "bionic/net.h"
#include "jni/java.h"
#include "util/log.h"

#define WWW "com/unity3d/player/WWW"
#define CONNECT_TIMEOUT_US (5 * 1000000)
#define IO_TIMEOUT_US (20 * 1000000)
#define CHUNK 16384
#define HEAD_MAX 16384

typedef jboolean (*header_fn)(JNIEnv *, jclass, jint, jstring);
typedef jboolean (*read_fn)(JNIEnv *, jclass, jint, jbyteArray, jint);
typedef void (*progress_fn)(JNIEnv *, jclass, jint, jfloat, jfloat, jdouble, jint);
typedef void (*error_fn)(JNIEnv *, jclass, jint, jstring);
typedef void (*done_fn)(JNIEnv *, jclass, jint);

typedef struct {
  int id;
  char *url;
  uint8_t *body;
  int body_len, has_body;
  char *headers; /* "Key: value\r\n" lines */
  volatile int alive, joining, sid;
} www;

static header_fn cb_header;
static read_fn cb_read;
static progress_fn cb_progress;
static error_fn cb_error;
static done_fn cb_done;
static volatile int requests;

static jclass cls(void) {
  return (jclass)jvm_class(WWW)->obj;
}

static void on_error(www *w, const char *fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  LOG("[www] %s: %s", w->url, msg);
  if (w->joining)
    return;
  jobj *s = jvm_string(msg);
  jvm_frame_push();
  cb_error(jvm_env, cls(), w->id, (jstring)s);
  jvm_frame_pop(NULL);
  jvm_release(s);
}

static int on_header(www *w, const char *text) {
  jobj *s = jvm_string(text);
  jvm_frame_push();
  int abort = cb_header(jvm_env, cls(), w->id, (jstring)s);
  jvm_frame_pop(NULL);
  jvm_release(s);
  return abort;
}

static int on_read(www *w, jobj *arr, const void *data, int n) {
  if (n)
    memcpy(arr->arr.data, data, n);
  jvm_frame_push();
  int abort = cb_read(jvm_env, cls(), w->id, (jbyteArray)arr, n);
  jvm_frame_pop(NULL);
  return abort;
}

static void on_progress(www *w, float up, float down, int total) {
  jvm_frame_push();
  cb_progress(jvm_env, cls(), w->id, up, down, 0.0, total);
  jvm_frame_pop(NULL);
}

static void on_done(www *w) {
  jvm_frame_push();
  cb_done(jvm_env, cls(), w->id);
  jvm_frame_pop(NULL);
}

/* scheme://host[:port]/path -> 0 http, 1 https, -1 other */
static int parse_url(const char *url, char *host, size_t hl, const char **path) {
  int scheme;
  const char *p;
  if (strncasecmp(url, "http://", 7) == 0)
    scheme = 0, p = url + 7;
  else if (strncasecmp(url, "https://", 8) == 0)
    scheme = 1, p = url + 8;
  else
    return -1;
  size_t n = strcspn(p, ":/?#");
  if (n == 0 || n >= hl)
    return -1;
  memcpy(host, p, n);
  host[n] = 0;
  p += n;
  if (*p == ':')
    p += strcspn(p, "/?#");
  *path = *p ? p : "/";
  return scheme;
}

static int connect_to(www *w, uint32_t addr, int port) {
  int sid = sceNetSocket("www", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
  if (sid < 0)
    return sid;
  w->sid = sid;
  int us = IO_TIMEOUT_US, on = 1, off = 0;
  sceNetSetsockopt(sid, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &us, sizeof(us));
  sceNetSetsockopt(sid, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &us, sizeof(us));
  sceNetSetsockopt(sid, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));
  SceNetSockaddrIn sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_len = sizeof(sa);
  sa.sin_family = SCE_NET_AF_INET;
  sa.sin_port = (port >> 8 & 0xff) | (port & 0xff) << 8;
  sa.sin_addr.s_addr = addr;
  int r = sceNetConnect(sid, (SceNetSockaddr *)&sa, sizeof(sa));
  if (r < 0 && r != (int)SCE_NET_ERROR_EINPROGRESS)
    return r;
  if (r < 0) {
    int eid = sceNetEpollCreate("www", 0);
    SceNetEpollEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = SCE_NET_EPOLLOUT;
    sceNetEpollControl(eid, SCE_NET_EPOLL_CTL_ADD, sid, &ev);
    int got = sceNetEpollWait(eid, &ev, 1, CONNECT_TIMEOUT_US);
    sceNetEpollDestroy(eid);
    if (got <= 0)
      return got < 0 ? got : (int)SCE_NET_ERROR_ETIMEDOUT;
    int so = 0;
    unsigned int sl = sizeof(so);
    sceNetGetsockopt(sid, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, &so, &sl);
    if (so)
      return 0x80410100 | so;
  }
  sceNetSetsockopt(sid, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &off, sizeof(off));
  return sid;
}

static int send_all(int sid, const void *p, int n) {
  for (int done = 0; done < n;) {
    int r = sceNetSend(sid, (const char *)p + done, n - done, 0);
    if (r < 0)
      return r;
    done += r;
  }
  return 0;
}

static void request(www *w) {
  char host[256];
  const char *path;
  int scheme = parse_url(w->url, host, sizeof(host), &path);
  if (scheme < 0 || !net_is_game_host(host)) {
    if (scheme < 0)
      on_error(w, "java.net.MalformedURLException: unknown protocol");
    else
      on_error(w, "java.net.UnknownHostException: Unable to resolve host \"%s\": No address associated with hostname",
               host);
    return;
  }
  uint32_t addr;
  b_inet_aton(net_server_ip(), &addr);
  int port = net_http_port();
  int sid = connect_to(w, addr, port);
  if (sid < 0) {
    if (!w->joining)
      on_error(w, "java.net.ConnectException: failed to connect to %s/%s (port %d): 0x%08X", host, net_server_ip(),
               port, sid);
    return;
  }

  /* request */
  size_t hl = strlen(path) + strlen(host) + (w->headers ? strlen(w->headers) : 0) + 256;
  char *head = malloc(hl);
  int n = snprintf(head, hl, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n%s",
                   w->has_body ? "POST" : "GET", path, host, w->headers ? w->headers : "");
  if (w->has_body)
    n += snprintf(head + n, hl - n, "Content-Length: %d\r\n", w->body_len);
  n += snprintf(head + n, hl - n, "\r\n");
  int r = send_all(sid, head, n);
  free(head);
  if (r >= 0 && w->body_len)
    r = send_all(sid, w->body, w->body_len);
  if (r < 0) {
    on_error(w, "java.net.SocketException: send failed: 0x%08X", r);
    return;
  }
  if (w->has_body && w->body_len && !w->joining)
    on_progress(w, 1.0f, 0.0f, 0);

  /* status line and headers */
  char *buf = malloc(HEAD_MAX + CHUNK);
  int got = 0;
  char *end = NULL;
  while (!end) {
    if (got == HEAD_MAX) {
      on_error(w, "java.io.IOException: response headers too long");
      goto out;
    }
    r = sceNetRecv(sid, buf + got, HEAD_MAX - got, 0);
    if (r <= 0) {
      if (!w->joining)
        on_error(w, r < 0 ? "java.net.SocketTimeoutException: read: 0x%08X" : "java.io.EOFException", r);
      goto out;
    }
    got += r;
    buf[got] = 0;
    end = strstr(buf, "\r\n\r\n");
  }
  *end = 0;
  char *body = end + 4;
  int have = got - (body - buf);
  int code = 0;
  char reason[64] = "";
  sscanf(buf, "HTTP/%*d.%*d %d %63[^\r\n]", &code, reason);
  char *line = strstr(buf, "\r\n");
  long content_length = -1;
  /* Java hands Unity "Status: <status line>" and the fields as "Key: value" */
  size_t tl = strlen(buf) + 64;
  char *text = malloc(tl);
  int tn = snprintf(text, tl, "Status: %.*s\r\n", (int)strcspn(buf, "\r\n"), buf);
  while (line) {
    line += 2;
    char *next = strstr(line, "\r\n");
    int len = next ? next - line : (int)strlen(line);
    if (len > 0) {
      tn += snprintf(text + tn, tl - tn, "%.*s\r\n", len, line);
      if (strncasecmp(line, "content-length:", 15) == 0)
        content_length = strtol(line + 15, NULL, 10);
    }
    line = next;
  }
  if (w->joining) {
    free(text);
    goto out;
  }
  int abort = on_header(w, text);
  free(text);
  if (abort) {
    on_error(w, "%s aborted", w->url);
    goto out;
  }

  /* body: Java reads through the error stream for codes from 400 on */
  int failed = code >= 400;
  jobj *arr = jvm_array('B', CHUNK);
  long total = 0;
  if (on_read(w, arr, NULL, 0)) {
    jvm_release(arr);
    on_error(w, "%s aborted", w->url);
    goto out;
  }
  while (!w->joining) {
    if (have > 0) {
      if (on_read(w, arr, body, have)) {
        jvm_release(arr);
        on_error(w, "%s aborted", w->url);
        goto out;
      }
      total += have;
      if (!failed && content_length > 0)
        on_progress(w, 1.0f, (float)(total / content_length), (int)content_length);
    }
    if (content_length >= 0 && total >= content_length)
      break;
    r = sceNetRecv(sid, buf, CHUNK, 0);
    if (r < 0) {
      jvm_release(arr);
      if (!w->joining)
        on_error(w, "java.net.SocketTimeoutException: read: 0x%08X", r);
      goto out;
    }
    if (r == 0)
      break;
    body = buf;
    have = r;
  }
  jvm_release(arr);
  if (w->joining)
    goto out;
  if (failed)
    on_error(w, "%d: %s", code, reason);
  if (total > 0)
    on_progress(w, 1.0f, 1.0f, (int)total);
  on_done(w);
  DLOG("[www] %s -> %d, %ld bytes", w->url, code, total);
out:
  free(buf);
}

static void *www_main(void *arg) {
  jobj *o = arg;
  www *w = o->native;
  request(w);
  if (w->sid >= 0) {
    sceNetSocketClose(w->sid);
    w->sid = -1;
  }
  __atomic_sub_fetch(&requests, 1, __ATOMIC_SEQ_CST);
  w->alive = 0;
  jvm_release(o);
  return NULL;
}

static void www_free(void *p) {
  www *w = p;
  free(w->url);
  free(w->body);
  free(w->headers);
  free(w);
}

/* WWW(int id, String url, byte[] postData, Map<String, String> headers) */
static jvalue www_init(jobj *thiz, const jvalue *args, jmeth *m) {
  www *w = calloc(1, sizeof(*w));
  w->id = args[0].i;
  w->url = strdup(ARG_STR(1));
  w->sid = -1;
  jobj *data = ARG_OBJ(2);
  if (data && data->kind == OBJ_ARRAY) {
    w->has_body = 1;
    w->body_len = data->arr.len;
    w->body = malloc(w->body_len ? w->body_len : 1);
    memcpy(w->body, data->arr.data, w->body_len);
  }
  jobj *map = ARG_OBJ(3);
  int n = java_map_size(map);
  size_t len = 1;
  for (int i = 0; i < n; i++)
    len += strlen(jvm_str(java_map_key(map, i))) + strlen(jvm_str(java_map_value(map, i))) + 4;
  w->headers = calloc(1, len);
  for (int i = 0, at = 0; i < n; i++) {
    const char *k = jvm_str(java_map_key(map, i));
    /* we set these ourselves */
    if (!strcasecmp(k, "Host") || !strcasecmp(k, "Content-Length") || !strcasecmp(k, "Connection"))
      continue;
    at += snprintf(w->headers + at, len - at, "%s: %s\r\n", k, jvm_str(java_map_value(map, i)));
  }
  thiz->native = w;
  thiz->native_free = www_free;
  return jv_void();
}

static jvalue www_start(jobj *thiz, const jvalue *args, jmeth *m) {
  www *w = thiz->native;
  if (!w || w->alive)
    return jv_void();
  if (!net_enabled() || strncasecmp(w->url, "http", 4) != 0) {
    LOG("[www] %s left pending (%s)", w->url, net_enabled() ? "not http" : "offline");
    return jv_void();
  }
  if (!cb_header) {
    cb_header = jvm_native(WWW, "headerCallback", "(ILjava/lang/String;)Z");
    cb_read = jvm_native(WWW, "readCallback", "(I[BI)Z");
    cb_progress = jvm_native(WWW, "progressCallback", "(IFFDI)V");
    cb_error = jvm_native(WWW, "errorCallback", "(ILjava/lang/String;)V");
    cb_done = jvm_native(WWW, "doneCallback", "(I)V");
  }
  if (!cb_header || !cb_read || !cb_progress || !cb_error || !cb_done) {
    LOG("[www] natives not registered; %s left pending", w->url);
    return jv_void();
  }
  w->alive = 1;
  __atomic_add_fetch(&requests, 1, __ATOMIC_SEQ_CST);
  bthread *t;
  if (bthread_create(&t, "www", www_main, jvm_retain(thiz), 64 * 1024, 1, 0) != 0) {
    __atomic_sub_fetch(&requests, 1, __ATOMIC_SEQ_CST);
    w->alive = 0;
    jvm_release(thiz);
    LOG("[www] no thread for %s", w->url);
  }
  return jv_void();
}

static jvalue www_isAlive(jobj *thiz, const jvalue *args, jmeth *m) {
  www *w = thiz->native;
  return jv_bool(w && w->alive);
}

/* Unity joins when it drops a request: stop the transfer, no more callbacks */
static jvalue www_join(jobj *thiz, const jvalue *args, jmeth *m) {
  www *w = thiz->native;
  if (!w || !w->alive)
    return jv_void();
  w->joining = 1;
  if (w->sid >= 0)
    sceNetSocketAbort(w->sid, 0);
  while (w->alive)
    sceKernelDelayThread(1000);
  return jv_void();
}

void java_www_init(void) {
  jvm_method(WWW, "<init>", "(ILjava/lang/String;[BLjava/util/Map;)V", www_init);
  jvm_method(WWW, "start", "()V", www_start);
  jvm_method(WWW, "isAlive", "()Z", www_isAlive);
  jvm_method(WWW, "join", "()V", www_join);
  jvm_method(WWW, "join", "(J)V", www_join);
}
