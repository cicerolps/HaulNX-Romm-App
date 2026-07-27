#ifndef HTTPSRV_H
#define HTTPSRV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A single-purpose HTTP server for receiving one file from a PC on the same
 * LAN. It serves a small upload page on GET and captures the posted file on
 * POST, then it is done. There is no routing, no keep-alive and no TLS: it
 * exists for the seconds the Import screen is open and is closed immediately
 * after.
 *
 * The received file is either a dl_sources.json collection or a HaulNX .nro
 * build to install; the server is a plain transport and the caller tells them
 * apart (an NRO carries "NRO0" at offset 0x10).
 *
 * The one exception is ROM mode (HTTPSRV_MODE_ROM): a game file can be hundreds
 * of MB to several GB, far too large to hold in RAM, so it is streamed straight
 * to a file under a caller-set folder rather than buffered into s->body. */

#define HTTPSRV_PORT     8080
/* Buffered-in-RAM modes (collection/nro): big enough for an app build, small
 * enough to refuse runaway uploads. ROM mode streams to disk and is bounded by
 * HTTPSRV_MAX_ROM (and, ultimately, free space on the card) instead. */
#define HTTPSRV_MAX_BODY (16 * 1024 * 1024)
/* A single game file. exFAT-formatted cards go past 4 GB, so this is a sanity
 * ceiling on the declared Content-Length, not a real expected size; the card
 * filling up is the true limit and surfaces as a write failure mid-stream. */
#define HTTPSRV_MAX_ROM  (64ULL * 1024 * 1024 * 1024)
/* One-time code carried in the URL path (e.g. http://ip:8080/k7m2xq9r). It only
 * ever appears on the console's screen, so anything that knows it was shown
 * the address by the user — a web page loaded on some other LAN device can
 * neither POST a file nor fetch the config export without it.
 *
 * It is also the only thing standing between a LAN peer and replacing the app
 * binary. It is deliberately a short 4-digit numeric code: it lives only on a
 * trusted home LAN, the receive screen is open for a moment at a time, and the
 * code is regenerated every time that screen opens, so the value is being read
 * off a TV and typed on a phone rather than defended against sustained guessing.
 * Brevity is the point. */
#define HTTPSRV_TOKEN_LEN 4

/* Which one-way task the open server is serving. It only picks the page shown
 * in a browser and whether the config export is offered; POST is still routed
 * by content, so a file meant for the wrong screen is forgiven. Defaults to
 * IMPORT because httpsrv_open zeroes the struct. */
typedef enum {
    HTTPSRV_MODE_IMPORT = 0, /* receive a collection from a PC (upload page) */
    HTTPSRV_MODE_EXPORT,     /* hand this console's collection to a PC (GET) */
    HTTPSRV_MODE_NRO,        /* receive an app .nro build (update page) */
    HTTPSRV_MODE_ROM,        /* receive a game file, streamed into dest_dir */
} HttpSrvMode;

typedef struct {
    int listen_fd;   /* -1 when closed */
    HttpSrvMode mode; /* set by the caller after open; see HttpSrvMode */
    char token[HTTPSRV_TOKEN_LEN + 1]; /* set by open; caller shows it in the URL */
    char ip[46];     /* our own address, for the Host-header check (may be "") */
    /* One in-flight connection, read a slice per poll so the UI thread never
     * blocks and can show receive progress. All owned/reset internally. */
    int client_fd;      /* -1 when idle */
    char *head;         /* request head while it is still arriving */
    size_t head_len;
    char *cbody;        /* POST body being filled */
    size_t cbody_len;   /* bytes received so far */
    size_t cbody_total; /* Content-Length */
    char ctype[192];    /* Content-Type (for multipart slicing at the end) */
    unsigned long long last_data_ns;  /* watchdog for a client that went quiet */
    unsigned long long conn_start_ns; /* accept time, for the head deadline */
    char *body;      /* received file, NUL-terminated, owned; NULL until then */
    size_t body_len;
    /* ROM mode only. dest_dir is set by the caller before serving; an upload is
     * streamed to <dest_dir>/<recv_name>.part (part_path) rather than into body.
     * On completion httpsrv_poll returns 1 with body == NULL, part_path holding
     * the finished temp file and recv_name the sanitized name it should take —
     * the caller confirms any overwrite and moves it into place. sink is the
     * open temp file mid-transfer (closed and removed by httpsrv_close if the
     * user backs out before it finishes). */
    char dest_dir[768];
    FILE *sink;
    char part_path[1088];
    char recv_name[256];
    char last_err[64]; /* why a ROM stream aborted, for the caller to log */
} HttpSrv;

/* The console's LAN address as a dotted quad, e.g. "192.168.1.42".
 * False if there is no network connection (nothing to advertise). */
bool httpsrv_local_ip(char *out, size_t out_sz);

/* Bind HTTPSRV_PORT on all interfaces. False if the port is unavailable. */
bool httpsrv_open(HttpSrv *s);

/* Service the connection a slice at a time and return immediately: this is
 * called once per frame from the UI thread, so it never waits for a client —
 * a large upload simply spans many polls (see httpsrv_receiving).
 *   0  nothing finished this poll — keep polling
 *   1  a file was received; s->body / s->body_len hold it
 *   2  the browser fetched the post-upload page, so it is parked somewhere a
 *      reload is harmless and the caller can stop serving
 *   3  the current config was handed to the browser (an export)
 *   4  a ROM stream aborted mid-transfer (card full, or the peer dropped);
 *      s->last_err holds a short reason and the server is left listening for a
 *      retry unless the caller stops it
 *  -1  the server is not open
 * Once this returns 1 the caller owns s->body until httpsrv_close. */
int httpsrv_poll(HttpSrv *s);

/* True while a POST body is arriving; *now / *total (either may be NULL)
 * report the bytes so far and the Content-Length, for a progress line. */
bool httpsrv_receiving(const HttpSrv *s, size_t *now, size_t *total);

/* Close the socket and free any received body. Safe on an unopened server. */
void httpsrv_close(HttpSrv *s);

#ifdef __cplusplus
}
#endif

#endif /* HTTPSRV_H */
