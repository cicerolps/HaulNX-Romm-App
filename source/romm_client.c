#include "romm_client.h"
#include "net.h"
#include "jsonutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- credential setup -------------------------------------------------- */

/* Decide how to authenticate: an API token (Bearer) takes priority over
 * username+password (Basic), matching romm_creds_configured/the Settings UI's
 * "token is the advanced alternative" framing. Fills exactly one of
 * (*user,*pass) or bearer_hdr; the other is left empty/NULL. Returns false if
 * neither is set (caller should not attempt the request unauthenticated --
 * RomM has no public-item concept the way archive.org does). */
static bool romm_auth_pick(const RommCredentials *c, const char **user,
                           const char **pass, char *bearer_hdr,
                           size_t bearer_sz) {
    bearer_hdr[0] = '\0';
    *user = NULL;
    *pass = NULL;
    if (c->api_token[0]) {
        snprintf(bearer_hdr, bearer_sz, "authorization: Bearer %s",
                 c->api_token);
        return true;
    }
    if (c->username[0] && c->password[0]) {
        *user = c->username;
        *pass = c->password;
        return true;
    }
    return false;
}

/* The bare host[:port] authority of server_url, for http_get_authed's
 * auth_host (the exact host the credential may be sent to). server_url is
 * always stored without a trailing slash (see romm_creds_load/save), so this
 * is just the part between "://" and the next '/' or end of string. */
static void romm_authority(const char *server_url, char *out, size_t out_sz) {
    out[0] = '\0';
    const char *p = strstr(server_url, "://");
    if (!p) {
        return;
    }
    p += 3;
    size_t n = 0;
    while (p[n] && p[n] != '/' && n + 1 < out_sz) {
        n++;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

/* ---- JSON helpers -------------------------------------------------------- */

/* Like json_copy, but leaves out empty for anything that isn't a JSON string
 * -- notably a JSON null, which json_copy would otherwise copy in as the
 * literal 4 characters "null" (it only special-cases JSMN_STRING). Several
 * RomM fields (rom name/slug/summary, md5_hash, ...) are typed `str | None`. */
static void copy_str_field(const char *js, jsmntok_t *tok, int obj,
                           const char *key, char *out, size_t out_sz) {
    int idx = json_obj_get(js, tok, obj, key);
    if (idx >= 0 && tok[idx].type == JSMN_STRING) {
        json_copy(js, tok, idx, out, out_sz);
    } else {
        out[0] = '\0';
    }
}

static int int_field(const char *js, jsmntok_t *tok, int obj, const char *key) {
    return (int)json_u64(js, tok, json_obj_get(js, tok, obj, key));
}

/* ---- transport ------------------------------------------------------- */

/* One GET against the RomM API: builds the URL (server_url + path), attaches
 * whichever credential is configured (scoped to the RomM host only --
 * romm_authority/http_get_authed keep it from ever reaching archive.org),
 * and reports a short, plain-English reason on failure. */
static char *romm_get(const RommCredentials *c, const char *path,
                      long *http_code, char *err, size_t err_sz,
                      size_t *out_len) {
    if (err && err_sz) {
        err[0] = '\0';
    }
    long code = 0;
    if (!http_code) {
        http_code = &code;
    }
    *http_code = 0;

    if (!romm_creds_configured(c)) {
        snprintf(err, err_sz, "RomM not configured");
        return NULL;
    }

    char url[600];
    snprintf(url, sizeof(url), "%s%s", c->server_url, path);
    char authority[256];
    romm_authority(c->server_url, authority, sizeof(authority));

    const char *user = NULL, *pass = NULL;
    char bearer[160];
    if (!romm_auth_pick(c, &user, &pass, bearer, sizeof(bearer))) {
        snprintf(err, err_sz, "no credentials configured");
        return NULL;
    }

    char *body = http_get_authed(url, authority, user, pass, bearer,
                                 !c->ignore_cert_verify, http_code, out_len);
    if (!body) {
        snprintf(err, err_sz, *http_code == 0 ? "connection failed"
                                               : "request failed");
        return NULL;
    }
    if (*http_code == 401 || *http_code == 403) {
        snprintf(err, err_sz, "authentication failed (HTTP %ld)", *http_code);
        free(body);
        return NULL;
    }
    if (*http_code != 200) {
        snprintf(err, err_sz, "server returned HTTP %ld", *http_code);
        free(body);
        return NULL;
    }
    return body;
}

/* ---- platforms --------------------------------------------------------- */

static bool parse_platforms(const char *body, size_t len,
                            RommPlatformList *out) {
    int ntok = 0;
    jsmntok_t *tok = json_parse_alloc(body, len, &ntok);
    if (!tok || tok[0].type != JSMN_ARRAY) {
        free(tok);
        return false;
    }

    int count = tok[0].size;
    RommPlatform *arr =
        (RommPlatform *)calloc(count > 0 ? (size_t)count : 1, sizeof(RommPlatform));
    if (!arr) {
        free(tok);
        return false;
    }

    int child = 1;
    int added = 0;
    for (int i = 0; i < count; i++) {
        if (tok[child].type == JSMN_OBJECT) {
            RommPlatform *p = &arr[added];
            p->id = int_field(body, tok, child, "id");
            copy_str_field(body, tok, child, "slug", p->slug, sizeof(p->slug));
            copy_str_field(body, tok, child, "fs_slug", p->fs_slug,
                           sizeof(p->fs_slug));
            copy_str_field(body, tok, child, "name", p->name, sizeof(p->name));
            p->rom_count = int_field(body, tok, child, "rom_count");
            if (p->slug[0] || p->fs_slug[0]) {
                added++;
            }
        }
        child = json_tok_skip(tok, child);
    }
    out->platforms = arr;
    out->count = added;
    free(tok);
    return true;
}

bool romm_fetch_platforms(const RommCredentials *c, RommPlatformList *out,
                          long *http_code, char *err, size_t err_sz) {
    memset(out, 0, sizeof(*out));
    size_t len = 0;
    char *body = romm_get(c, "/api/platforms", http_code, err, err_sz, &len);
    if (!body) {
        return false;
    }
    bool ok = parse_platforms(body, len, out);
    if (!ok && err) {
        snprintf(err, err_sz, "unexpected response from server");
    }
    free(body);
    return ok;
}

void romm_free_platforms(RommPlatformList *list) {
    if (list && list->platforms) {
        free(list->platforms);
        list->platforms = NULL;
        list->count = 0;
    }
}

/* ---- roms --------------------------------------------------------------- */

static bool parse_roms(const char *body, size_t len, RommRomList *out) {
    int ntok = 0;
    jsmntok_t *tok = json_parse_alloc(body, len, &ntok);
    if (!tok || tok[0].type != JSMN_ARRAY) {
        free(tok);
        return false;
    }

    int count = tok[0].size;
    RommRom *arr =
        (RommRom *)calloc(count > 0 ? (size_t)count : 1, sizeof(RommRom));
    if (!arr) {
        free(tok);
        return false;
    }

    int child = 1;
    int added = 0;
    for (int i = 0; i < count; i++) {
        if (tok[child].type == JSMN_OBJECT) {
            RommRom *r = &arr[added];
            r->id = int_field(body, tok, child, "id");
            r->platform_id = int_field(body, tok, child, "platform_id");
            copy_str_field(body, tok, child, "fs_name", r->fs_name,
                           sizeof(r->fs_name));
            copy_str_field(body, tok, child, "name", r->name, sizeof(r->name));
            r->size = json_u64_size(body, tok,
                                    json_obj_get(body, tok, child, "fs_size_bytes"));
            copy_str_field(body, tok, child, "md5_hash", r->md5, sizeof(r->md5));
            if (r->fs_name[0]) {
                added++;
            }
        }
        child = json_tok_skip(tok, child);
    }
    out->roms = arr;
    out->count = added;
    free(tok);
    return true;
}

bool romm_fetch_roms(const RommCredentials *c, int platform_id,
                     RommRomList *out, long *http_code, char *err,
                     size_t err_sz) {
    memset(out, 0, sizeof(*out));
    char path[64];
    snprintf(path, sizeof(path), "/api/roms?platform_id=%d", platform_id);
    size_t len = 0;
    char *body = romm_get(c, path, http_code, err, err_sz, &len);
    if (!body) {
        return false;
    }
    bool ok = parse_roms(body, len, out);
    if (!ok && err) {
        snprintf(err, err_sz, "unexpected response from server");
    }
    free(body);
    return ok;
}

void romm_free_roms(RommRomList *list) {
    if (list && list->roms) {
        free(list->roms);
        list->roms = NULL;
        list->count = 0;
    }
}

/* ---- connection test / content URL -------------------------------------- */

bool romm_test_connection(const RommCredentials *c, long *http_code,
                          char *err, size_t err_sz) {
    RommPlatformList list = {0};
    bool ok = romm_fetch_platforms(c, &list, http_code, err, err_sz);
    romm_free_platforms(&list);
    return ok;
}

/* Percent-encode a single filename component: keep unreserved chars, encode
 * everything else (spaces, parentheses, '/', etc -- fs_name is one filename,
 * never a path, so a stray '/' is encoded rather than treated as a separator). */
static void url_encode_component(const char *in, char *out, size_t out_sz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in;
         *p && o + 4 < out_sz; p++) {
        unsigned char ch = *p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
            ch == '~') {
            out[o++] = (char)ch;
        } else {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 0xF];
        }
    }
    out[o] = '\0';
}

void romm_content_url(const RommCredentials *c, const RommRom *rom, char *out,
                      size_t out_sz) {
    char enc[1536];
    url_encode_component(rom->fs_name, enc, sizeof(enc));
    snprintf(out, out_sz, "%s/api/roms/%d/content/%s", c->server_url, rom->id,
            enc);
}
