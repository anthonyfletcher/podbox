/* pbgide: the Arcade IDE served from this computer, with a cart folder it
 * can open and save straight back to.
 *
 *   pbgide [--port n] [--ide file.html] [--no-browser] [cart folder]
 *
 * It serves the IDE's page on 127.0.0.1 -- this computer only -- and opens
 * it in the default browser. With a cart folder, the page opens that cart
 * and Save writes its files back through the server, and the list it asks
 * for carries a fingerprint of each file so the page sees one changed on
 * the disk. The browser never writes a file itself and needs no leave to
 * (some managed browsers refuse pages that leave whatever the owner says).
 *
 * The page's address carries a token made at each start; every request
 * that reads or writes the folder has to carry it back, so another page in
 * the same browser cannot reach the files. Requests naming any host but
 * this one are refused too. Only a cart's own files are read or written:
 * cart.json, the code, and the files in the cart's folders.
 *
 * Plain HTTP: a page from http://127.0.0.1 is as secure, to a browser, as
 * one from HTTPS, and a certificate would be one more thing to install.
 *
 * The page is read from the disk at each request, so a rebuilt IDE is
 * served on the next reload. Ctrl+C stops it.
 *
 * Parts: sockets either side; the folder; requests; starting. */

#define _CRT_RAND_S
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#define make_dir(p) _mkdir(p)
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
#define make_dir(p) mkdir((p), 0777)
#endif

#define MAX_HEAD  16384
#define MAX_BODY  (64 * 1024 * 1024)
#define MAX_PATH_LEN 1024

static char folder[MAX_PATH_LEN];   /* the cart's folder, or "" for none */
static char ide_path[MAX_PATH_LEN];
static char token[33];
static int  port = 8484;

/* ---- the folder ---------------------------------------------------------- */

/* The folders a cart keeps files in, one level down */
static const char *const DIRS[] = { "palettes", "maps", "images", "sounds",
                                    "data", "fonts", "editors" };
#define NDIRS (sizeof(DIRS) / sizeof(DIRS[0]))

static const char *const EXTS[] = { ".js", ".json", ".png", ".gpl", ".txt",
                                    ".bdf", ".html" };

/* A cart's file, named as the IDE names it: "main.js", "images/x.png" --
 * one of the cart's folders at most, a plain name, a cart's extension */
static int cart_name(const char *rel)
{
    const char *slash = strchr(rel, '/'), *name = rel, *dot;
    size_t n = strlen(rel);
    if (n == 0 || n > 200)
        return 0;
    if (slash) {
        size_t d = (size_t)(slash - rel), i;
        int ok = 0;
        for (i = 0; i < NDIRS; i++)
            if (strlen(DIRS[i]) == d && !strncmp(rel, DIRS[i], d))
                ok = 1;
        if (!ok)
            return 0;
        name = slash + 1;
    }
    if (!*name || *name == '.' || strchr(name, '/'))
        return 0;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("_-. ", *p))
            return 0;
    dot = strrchr(name, '.');
    if (!dot)
        return 0;
    for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++)
        if (!strcmp(dot, EXTS[i]))
            return 1;
    return 0;
}

static void full_path(char *out, size_t cap, const char *rel)
{
    snprintf(out, cap, "%s/%s", folder, rel);
}

static int is_file(const char *path)
{
    struct stat s;
    return !stat(path, &s) && S_ISREG(s.st_mode);
}

/* The whole of a file, malloc'd; NULL where it cannot be read */
static char *read_all(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)*size + 1);
    if (buf && fread(buf, 1, (size_t)*size, f) != (size_t)*size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

/* Written beside itself and then moved into place, so a write that fails
 * part-way leaves the old file whole */
static int write_all(const char *rel, const char *data, long size)
{
    char path[MAX_PATH_LEN], tmp[MAX_PATH_LEN + 8];
    const char *slash = strchr(rel, '/');
    FILE *f;
    if (slash) {
        char dir[MAX_PATH_LEN];
        snprintf(dir, sizeof(dir), "%s/%.*s", folder, (int)(slash - rel), rel);
        make_dir(dir);
    }
    full_path(path, sizeof(path), rel);
    snprintf(tmp, sizeof(tmp), "%s.saving", path);
    f = fopen(tmp, "wb");
    if (!f)
        return 0;
    if (fwrite(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        remove(tmp);
        return 0;
    }
    if (fclose(f)) {
        remove(tmp);
        return 0;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp);
        return 0;
    }
#else
    if (rename(tmp, path)) {
        remove(tmp);
        return 0;
    }
#endif
    return 1;
}

/* A file's contents as FNV-1a, for the page to tell a file changed on the
 * disk from one it already has; 0 where it cannot be read */
static unsigned fingerprint(const char *path)
{
    long n;
    char *data = read_all(path, &n);
    unsigned h = 2166136261u;
    if (!data)
        return 0;
    for (long i = 0; i < n; i++)
        h = (h ^ (unsigned char)data[i]) * 16777619u;
    free(data);
    return h;
}

/* The cart's files as a JSON list into out: each its name and fingerprint */
static size_t list_files(char *out, size_t cap)
{
    size_t n = 0;
    int first = 1;
    n += (size_t)snprintf(out + n, cap - n, "[");
    for (int pass = -1; pass < (int)NDIRS; pass++) {
        char dir[MAX_PATH_LEN];
        DIR *d;
        struct dirent *e;
        snprintf(dir, sizeof(dir), "%s%s%s", folder, pass < 0 ? "" : "/",
                 pass < 0 ? "" : DIRS[pass]);
        d = opendir(dir);
        if (!d)
            continue;
        while ((e = readdir(d)) != NULL) {
            char rel[MAX_PATH_LEN], path[MAX_PATH_LEN];
            snprintf(rel, sizeof(rel), "%s%s%s", pass < 0 ? "" : DIRS[pass],
                     pass < 0 ? "" : "/", e->d_name);
            full_path(path, sizeof(path), rel);
            if (!cart_name(rel) || !is_file(path) || n + strlen(rel) + 40 > cap)
                continue;
            n += (size_t)snprintf(out + n, cap - n, "%s{\"path\":\"%s\",\"hash\":\"%08x\"}",
                                  first ? "" : ",", rel, fingerprint(path));
            first = 0;
        }
        closedir(d);
    }
    n += (size_t)snprintf(out + n, cap - n, "]");
    return n;
}

/* ---- requests ------------------------------------------------------------ */

static void send_all(sock_t s, const char *p, size_t n)
{
    while (n > 0) {
        int k = send(s, p, n > 65536 ? 65536 : (int)n, 0);
        if (k <= 0)
            return;
        p += k;
        n -= (size_t)k;
    }
}

static void reply(sock_t s, int code, const char *what, const char *type,
                  const char *body, size_t n)
{
    char head[512];
    int h = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     code, what, type, (unsigned long)n);
    send_all(s, head, (size_t)h);
    if (n)
        send_all(s, body, n);
}

static void reply_text(sock_t s, int code, const char *what, const char *text)
{
    reply(s, code, what, "text/plain; charset=utf-8", text, strlen(text));
}

/* A header's value, case-insensitively by name, into out; 0 where absent */
static int header(const char *head, const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    for (const char *p = strstr(head, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        const char *l = p + 2;
        size_t i;
        if (strncmp(l, "\r\n", 2) == 0)
            break;
        for (i = 0; i < n && tolower((unsigned char)l[i]) == tolower((unsigned char)name[i]); i++)
            ;
        if (i == n && l[n] == ':') {
            const char *v = l + n + 1, *end = strstr(v, "\r\n");
            size_t k;
            while (*v == ' ')
                v++;
            k = end ? (size_t)(end - v) : strlen(v);
            if (k >= cap)
                k = cap - 1;
            memcpy(out, v, k);
            out[k] = 0;
            return 1;
        }
    }
    return 0;
}

/* The query's path= value, %-decoded, into out; 0 where absent */
static int query_path(const char *target, char *out, size_t cap)
{
    const char *q = strstr(target, "?path=");
    size_t n = 0;
    if (!q)
        q = strstr(target, "&path=");
    if (!q)
        return 0;
    for (q += 6; *q && *q != '&' && n + 1 < cap; q++) {
        if (*q == '%' && isxdigit((unsigned char)q[1]) && isxdigit((unsigned char)q[2])) {
            char hex[3] = { q[1], q[2], 0 };
            out[n++] = (char)strtol(hex, NULL, 16);
            q += 2;
        } else
            out[n++] = *q == '+' ? ' ' : *q;
    }
    out[n] = 0;
    return 1;
}

static void serve_page(sock_t s)
{
    long n;
    char *page = read_all(ide_path, &n);
    if (!page) {
        reply_text(s, 500, "No IDE", "The IDE's page could not be read: build it with node ide/build.js, "
                   "or give pbgide --ide the path of arcade-ide.html.");
        return;
    }
    reply(s, 200, "OK", "text/html; charset=utf-8", page, (size_t)n);
    free(page);
}

static void handle(sock_t s)
{
    static char head[MAX_HEAD + 1];
    char method[16], target[2048], host[256], want[256], got[64], rel[MAX_PATH_LEN];
    size_t have = 0;
    char *end = NULL, *body = NULL;
    long length = 0;

    while (have < MAX_HEAD) {
        int k = recv(s, head + have, (int)(MAX_HEAD - have), 0);
        if (k <= 0)
            return;
        have += (size_t)k;
        head[have] = 0;
        if ((end = strstr(head, "\r\n\r\n")) != NULL)
            break;
    }
    if (!end || sscanf(head, "%15s %2047s", method, target) != 2) {
        reply_text(s, 400, "Bad Request", "bad request");
        return;
    }
    /* this computer's own name for the server, and no other: a page
     * elsewhere that names its own host here is turned away */
    snprintf(want, sizeof(want), "127.0.0.1:%d", port);
    if (!header(head, "Host", host, sizeof(host)) ||
        (strcmp(host, want) && (snprintf(want, sizeof(want), "localhost:%d", port), strcmp(host, want)))) {
        reply_text(s, 403, "Forbidden", "wrong host");
        return;
    }

    if (!strcmp(method, "GET") && (!strcmp(target, "/") || !strncmp(target, "/?", 2))) {
        serve_page(s);
        return;
    }
    if (strncmp(target, "/cart/", 6)) {
        reply_text(s, 404, "Not Found", "not found");
        return;
    }
    if (!header(head, "X-Arcade-Token", got, sizeof(got)) || strcmp(got, token)) {
        reply_text(s, 403, "Forbidden", "no token");
        return;
    }
    if (!folder[0]) {
        reply_text(s, 404, "Not Found", "pbgide was started without a cart folder");
        return;
    }

    if (!strcmp(method, "GET") && !strcmp(target, "/cart/list")) {
        static char list[262144];
        char out[262144 + MAX_PATH_LEN + 64];
        const char *base = strrchr(folder, '/');
        char name[2 * MAX_PATH_LEN], *q = name;
        /* the folder's name as a JSON string: it may hold a quote or a
         * backslash, where a cart's file names cannot */
        for (const char *p = base ? base + 1 : folder; *p; p++) {
            if (*p == '"' || *p == '\\')
                *q++ = '\\';
            if ((unsigned char)*p >= 0x20)
                *q++ = *p;
        }
        *q = 0;
        list_files(list, sizeof(list));
        snprintf(out, sizeof(out), "{\"name\":\"%s\",\"files\":%s}", name, list);
        reply(s, 200, "OK", "application/json", out, strlen(out));
        return;
    }
    if (strncmp(target, "/cart/file?", 11) || !query_path(target, rel, sizeof(rel)) || !cart_name(rel)) {
        reply_text(s, 400, "Bad Request", "not a cart's file");
        return;
    }

    if (!strcmp(method, "GET")) {
        char path[MAX_PATH_LEN];
        long n;
        char *data;
        full_path(path, sizeof(path), rel);
        data = read_all(path, &n);
        if (!data) {
            reply_text(s, 404, "Not Found", "no such file");
            return;
        }
        reply(s, 200, "OK", "application/octet-stream", data, (size_t)n);
        printf("read %s\n", rel);
        free(data);
        return;
    }
    if (!strcmp(method, "DELETE")) {
        char path[MAX_PATH_LEN];
        full_path(path, sizeof(path), rel);
        remove(path);
        printf("removed %s\n", rel);
        reply_text(s, 200, "OK", "removed");
        return;
    }
    if (strcmp(method, "PUT")) {
        reply_text(s, 405, "Method Not Allowed", "method not allowed");
        return;
    }

    {
        char len[32];
        size_t got_body;
        if (!header(head, "Content-Length", len, sizeof(len)) || (length = atol(len)) < 0 ||
            length > MAX_BODY) {
            reply_text(s, 400, "Bad Request", "needs a length, under 64 MB");
            return;
        }
        body = malloc((size_t)length + 1);
        if (!body) {
            reply_text(s, 500, "Error", "out of memory");
            return;
        }
        /* what came in with the header first, then the rest */
        got_body = have - (size_t)(end + 4 - head);
        if (got_body > (size_t)length)
            got_body = (size_t)length;
        memcpy(body, end + 4, got_body);
        while (got_body < (size_t)length) {
            int k = recv(s, body + got_body, (int)((size_t)length - got_body), 0);
            if (k <= 0) {
                free(body);
                return;
            }
            got_body += (size_t)k;
        }
    }
    if (write_all(rel, body, length)) {
        printf("saved %s\n", rel);
        reply_text(s, 200, "OK", "saved");
    } else
        reply_text(s, 500, "Error", strerror(errno));
    free(body);
}

/* ---- starting ------------------------------------------------------------ */

static void make_token(void)
{
    unsigned v[4];
#ifdef _WIN32
    for (int i = 0; i < 4; i++)
        rand_s(&v[i]);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(v, sizeof(v), 1, f) != 1) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        for (int i = 0; i < 4; i++)
            v[i] = (unsigned)rand() << 16 ^ (unsigned)rand();
    }
    if (f)
        fclose(f);
#endif
    snprintf(token, sizeof(token), "%08x%08x%08x%08x", v[0], v[1], v[2], v[3]);
}

/* This program's own folder, for finding the IDE's page beside it */
static void own_dir(char *out, size_t cap, const char *argv0)
{
    char *slash;
#ifdef _WIN32
    if (!GetModuleFileNameA(NULL, out, (DWORD)cap))
        snprintf(out, cap, "%s", argv0);
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n > 0)
        out[n] = 0;
    else
        snprintf(out, cap, "%s", argv0);
#endif
    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
    slash = strrchr(out, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(out, cap, ".");
}

static void open_browser(const char *url)
{
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "open '%s'", url);
    if (system(cmd)) { /* the address is printed: open it by hand */ }
#else
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", url);
    if (system(cmd)) { /* the address is printed: open it by hand */ }
#endif
}

int main(int argc, char **argv)
{
    int browser = 1, i;
    char here[MAX_PATH_LEN], url[256];
    sock_t ls;
    struct sockaddr_in a;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ide") && i + 1 < argc)
            snprintf(ide_path, sizeof(ide_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--no-browser"))
            browser = 0;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: pbgide [--port n] [--ide file.html] [--no-browser] [cart folder]\n");
            return 2;
        } else
            snprintf(folder, sizeof(folder), "%s", argv[i]);
    }
    for (char *p = folder; *p; p++)
        if (*p == '\\')
            *p = '/';
    while (strlen(folder) > 1 && folder[strlen(folder) - 1] == '/')
        folder[strlen(folder) - 1] = 0;
    if (folder[0]) {
        char cj[MAX_PATH_LEN];
        full_path(cj, sizeof(cj), "cart.json");
        if (!is_file(cj)) {
            fprintf(stderr, "%s has no cart.json: it is not a cart folder (pbgc new makes one)\n", folder);
            return 1;
        }
    }

    /* the IDE's page: asked for, beside this program, or where the SDK
     * builds it */
    if (!ide_path[0]) {
        own_dir(here, sizeof(here), argv[0]);
        snprintf(ide_path, sizeof(ide_path), "%s/arcade-ide.html", here);
        if (!is_file(ide_path))
            snprintf(ide_path, sizeof(ide_path), "%s/../ide/dist/arcade-ide.html", here);
    }
    if (!is_file(ide_path)) {
        fprintf(stderr, "no IDE page at %s: build it with node ide/build.js, or give --ide\n", ide_path);
        return 1;
    }

#ifdef _WIN32
    {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w)) {
            fprintf(stderr, "no sockets\n");
            return 1;
        }
    }
#endif
    make_token();
    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == BAD_SOCK) {
        fprintf(stderr, "no socket\n");
        return 1;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    /* the port asked for, or the next free one after it */
    for (i = 0; i < 20; i++, port++) {
        a.sin_port = htons((unsigned short)port);
        if (!bind(ls, (struct sockaddr *)&a, sizeof(a)))
            break;
    }
    if (i == 20 || listen(ls, 16)) {
        fprintf(stderr, "no free port from %d\n", port - i);
        return 1;
    }

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/?token=%s", port, token);
    printf("The Arcade IDE is at %s\n", url);
    if (folder[0])
        printf("Opening %s: Save writes its files back there.\n", folder);
    printf("Ctrl+C stops it.\n");
    fflush(stdout);
    if (browser)
        open_browser(url);

    for (;;) {
        sock_t s = accept(ls, NULL, NULL);
        if (s == BAD_SOCK)
            continue;
        /* one connection at a time, so one that sends nothing must not
         * hold the rest up: a read waits 2 s at most */
        {
#ifdef _WIN32
            DWORD wait = 2000;
#else
            struct timeval wait = { 2, 0 };
#endif
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&wait, sizeof(wait));
        }
        handle(s);
        fflush(stdout);
        close_sock(s);
    }
}
