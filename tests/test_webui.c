/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of the web UI state: equalizer commands, game mode (save a game's
 * sound, apply it when the game starts, put the usual sound back when it
 * ends), the status JSON. webui.c is included as it is (payload build, the
 * system calls are stubbed, files go to /tmp/hbt). Run with: make test */
#define HEARBRIDGE_PAYLOAD 1
#include "../src/webui.c"
#include <math.h>
#include <sys/socket.h>

int32_t scePthreadCreate(void **thr, const void *attr, void *(*fn)(void *), void *arg, const char *name)
{ (void)thr; (void)attr; (void)fn; (void)arg; (void)name; return -1; }
int32_t scePthreadJoin(void *thr, void **ret) { (void)thr; (void)ret; return 0; }
int32_t sceKernelUsleep(uint32_t usec) { (void)usec; return 0; }
static uint64_t fake_step;
uint64_t sceKernelGetProcessTime(void) { static uint64_t t = 1000000; return t += fake_step; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void run(const char *q)
{
    int sv[2];
    char buf[256];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
    handle_cmd(sv[0], q);
    close(sv[0]);
    while (read(sv[1], buf, sizeof buf) > 0) ;
    close(sv[1]);
}

static char json[16384];
static void status(void)
{
    struct sb b = { json, 0, sizeof json, 0 };
    build_status(&b);
    json[b.n] = 0;
}

static int balanced(const char *s)
{
    int depth = 0, in = 0;
    for (; *s; s++) {
        if (in) { if (*s == '\\' && s[1]) s++; else if (*s == '"') in = 0; continue; }
        if (*s == '"') in = 1;
        else if (*s == '{' || *s == '[') depth++;
        else if (*s == '}' || *s == ']') { if (--depth < 0) return 0; }
    }
    return depth == 0 && !in;
}

static int16_t pcm[480 * 2];
static double rms(void)
{
    double a = 0;
    int i;
    for (i = 0; i < 480 * 2; i++) a += (double)pcm[i] * pcm[i];
    return sqrt(a / (480 * 2));
}
static void feed(int32_t gain)
{
    int i;
    for (i = 0; i < 480 * 2; i++) pcm[i] = (int16_t)(5000.0 * sin(2 * M_PI * 40.0 * (i / 2) / 48000.0));
    ui_eq_apply(pcm, 480, gain);
}

/* The page is bigger than a default socket buffer and handle() writes it all
 * before we read, so give the server side room for the whole answer. */
static void roomy(int fd)
{
    int sz = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
}

/* One GET through handle(); the whole answer (headers and body) in out. */
static long fetch(const char *path, unsigned char *out, size_t cap)
{
    int sv[2];
    char rq[128];
    long n = 0, r;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    snprintf(rq, sizeof rq, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
    if (write(sv[0], rq, strlen(rq)) < 0) return -1;
    roomy(sv[1]);
    handle(sv[1], 0);
    close(sv[1]);
    while ((size_t)n < cap && (r = read(sv[0], out + n, cap - (size_t)n)) > 0) n += r;
    close(sv[0]);
    return n;
}

static unsigned char resp[400000];
static const unsigned char *body_of(long n)
{
    long i;
    for (i = 0; i + 3 < n; i++)
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') return resp + i + 4;
    return NULL;
}

static void test_pwa(void)
{
    long n;
    const unsigned char *b;
    char *m;
    int len, w, h, i;
    static const struct { const char *path; int size; } icons[] = {
        { "/icon-192.png", 192 }, { "/icon-512.png", 512 }, { "/apple-touch-icon.png", 180 } };

    n = fetch("/manifest.webmanifest", resp, sizeof resp - 1);
    CHECK(n > 0 && strncmp((char *)resp, "HTTP/1.1 200 OK", 15) == 0);
    resp[n > 0 ? n : 0] = 0;
    CHECK(strstr((char *)resp, "Content-Type: application/manifest+json") != NULL);
    b = body_of(n);
    CHECK(b != NULL);
    m = (char *)(b ? b : (const unsigned char *)"");
    CHECK(balanced(m));
    CHECK(strstr(m, "\"name\":\"HearBridge\"") && strstr(m, "\"short_name\":\"HearBridge\""));
    CHECK(strstr(m, "\"display\":\"standalone\"") && strstr(m, "\"start_url\":\"/\""));
    CHECK(strstr(m, "\"theme_color\":\"#0e0f14\"") && strstr(m, "\"background_color\":\"#0e0f14\""));
    CHECK(strstr(m, "\"src\":\"/icon-192.png\"") && strstr(m, "\"sizes\":\"192x192\""));
    CHECK(strstr(m, "\"src\":\"/icon-512.png\"") && strstr(m, "\"sizes\":\"512x512\""));
    len = 0;
    { const char *cl = strstr((char *)resp, "Content-Length: "); if (cl) len = atoi(cl + 16); }
    CHECK(len > 0 && b && (long)(b - resp) + len == n);                     /* announced length is the body */

    for (i = 0; i < 3; i++) {                                               /* real PNGs of the right size */
        n = fetch(icons[i].path, resp, sizeof resp);
        CHECK(n > 100 && strncmp((char *)resp, "HTTP/1.1 200 OK", 15) == 0);
        resp[n > 0 && n < (long)sizeof resp ? n : 0] = 0;
        b = body_of(n);
        CHECK(strstr((char *)resp, "Content-Type: image/png") != NULL);
        CHECK(b && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0 && memcmp(b + 12, "IHDR", 4) == 0);
        w = b ? (b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19]) : 0;
        h = b ? (b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23]) : 0;
        CHECK(w == icons[i].size && h == icons[i].size);
        len = 0;
        { const char *cl = strstr((char *)resp, "Content-Length: "); if (cl) len = atoi(cl + 16); }
        CHECK(b && (long)(b - resp) + len == n);
    }

    /* the page links all of it, and the old routes still work */
    CHECK(strstr(webui_html, "<link rel=\"manifest\" href=\"/manifest.webmanifest\">") != NULL);
    CHECK(strstr(webui_html, "<link rel=\"apple-touch-icon\" sizes=\"180x180\" href=\"/app-icon-180.png\">") != NULL);
    CHECK(strstr(webui_html, "<meta name=\"theme-color\" content=\"#121117\">") != NULL);
    CHECK(strstr(webui_html, "apple-mobile-web-app-capable") && strstr(webui_html, "mobile-web-app-capable"));
    CHECK(strstr(webui_html, "X-HB-Token") != NULL);
    CHECK(strstr(webui_html, "beforeinstallprompt") == NULL && strstr(webui_html, "Install app") == NULL && strstr(webui_html, "id=\"inst\"") == NULL); /* no install button */
    CHECK(strstr(webui_html, "g=S.game||{}") != NULL);                                 /* the page reads the game block */
    n = fetch("/", resp, sizeof resp - 1);
    CHECK(n > 1000 && strncmp((char *)resp, "HTTP/1.1 200 OK", 15) == 0);
    n = fetch("/api/status", resp, sizeof resp - 1);
    CHECK(n > 100 && strncmp((char *)resp, "HTTP/1.1 200 OK", 15) == 0);
    n = fetch("/sw.js", resp, sizeof resp - 1);                              /* no service worker */
    CHECK(n > 0 && strncmp((char *)resp, "HTTP/1.1 404", 12) == 0);
}


/* One raw request through handle(); the answer goes to resp. */
static long raw(const char *rq, int loopback)
{
    int sv[2];
    long n = 0, r;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    if (write(sv[0], rq, strlen(rq)) < 0) return -1;
    roomy(sv[1]);
    handle(sv[1], loopback);
    close(sv[1]);
    while ((size_t)n < sizeof resp - 1 && (r = read(sv[0], resp + n, sizeof resp - 1 - (size_t)n)) > 0) n += r;
    close(sv[0]);
    resp[n > 0 ? n : 0] = 0;
    return n;
}

static int code_is(int c)
{
    char want[16];
    snprintf(want, sizeof want, "HTTP/1.1 %d", c);
    return strncmp((char *)resp, want, strlen(want)) == 0;
}

static void test_security(void)
{
    char rq[512];
    struct ui_cmd c;
    long n;
    const char *tk;

    token_init();
    CHECK(strlen(g_ui_tok) == 32 && strspn(g_ui_tok, "0123456789abcdef") == 32);

    /* the status carries the token */
    n = raw("GET /api/status HTTP/1.1\r\nHost: 192.0.2.20:6767\r\n\r\n", 0);
    CHECK(n > 0 && code_is(200));
    tk = strstr((char *)resp, "\"token\":\"");
    CHECK(tk && strncmp(tk + 9, g_ui_tok, 32) == 0);

    /* state changes need the token (header or k=), from the page's own origin */
    while (ui_take_cmd(&c)) ;
    raw("GET /api/cmd?c=scan HTTP/1.1\r\nHost: 192.0.2.20:6767\r\n\r\n", 0);
    CHECK(code_is(403) && !ui_take_cmd(&c));
    raw("POST /api/cmd?c=scan HTTP/1.1\r\nHost: 192.0.2.20:6767\r\nX-HB-Token: 00000000000000000000000000000000\r\n\r\n", 0);
    CHECK(code_is(403) && !ui_take_cmd(&c));
    snprintf(rq, sizeof rq, "POST /api/cmd?c=scan HTTP/1.1\r\nHost: 192.0.2.20:6767\r\nx-hb-token: %s\r\n\r\n", g_ui_tok);
    raw(rq, 0);
    CHECK(code_is(200) && ui_take_cmd(&c) && c.type == UI_CMD_SCAN);
    snprintf(rq, sizeof rq, "GET /api/cmd?c=scan&k=%s&back=1 HTTP/1.1\r\nHost: 192.0.2.20:6767\r\n\r\n", g_ui_tok);
    raw(rq, 0);
    CHECK(code_is(303) && ui_take_cmd(&c));
    snprintf(rq, sizeof rq, "POST /api/cmd?c=scan HTTP/1.1\r\nHost: 192.0.2.20:6767\r\nOrigin: http://192.0.2.20:6767\r\nX-HB-Token: %s\r\n\r\n", g_ui_tok);
    raw(rq, 0);
    CHECK(code_is(200) && ui_take_cmd(&c));
    snprintf(rq, sizeof rq, "POST /api/cmd?c=scan HTTP/1.1\r\nHost: 192.0.2.20:6767\r\nOrigin: http://evil.example\r\nX-HB-Token: %s\r\n\r\n", g_ui_tok);
    raw(rq, 0);
    CHECK(code_is(403) && !ui_take_cmd(&c));

    /* Host: addresses and private names pass, a public name (DNS rebinding) does not */
    CHECK(host_ok("") && host_ok("192.0.2.7") && host_ok("192.0.2.7:6767") && host_ok("127.0.0.1:6768"));
    CHECK(host_ok("ps4") && host_ok("ps4.local:6767") && host_ok("Console.LAN") && host_ok("localhost"));
    CHECK(!host_ok("evil.example.com") && !host_ok("1.2.3.4.evil.com") && !host_ok("evil.example.com:6767"));
    raw("GET /api/status HTTP/1.1\r\nHost: rebind.example.com\r\n\r\n", 0);
    CHECK(code_is(403));
    raw("GET / HTTP/1.1\r\nHost: rebind.example.com\r\n\r\n", 0);
    CHECK(code_is(403));

    /* /api/quit: loopback (the new daemon taking over) or the token */
    raw("GET /api/quit HTTP/1.0\r\nHost: 192.0.2.20:6767\r\n\r\n", 0);
    CHECK(code_is(403) && !ui_take_cmd(&c));
    raw("GET /api/quit HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", 1);
    CHECK(code_is(200) && ui_take_cmd(&c) && c.type == UI_CMD_STOP);

    /* query decoding: malformed escapes and %00 are refused */
    { char v[16]; CHECK(qget("a=%41%42", "a", v, sizeof v) && !strcmp(v, "AB"));
      CHECK(!qget("a=%zz", "a", v, sizeof v) && !qget("a=%00x", "a", v, sizeof v) && !qget("a=%4", "a", v, sizeof v)); }

    /* a request that takes longer than the deadline gets no answer */
    fake_step = 5000000;
    n = raw("GET /api/status HTTP/1.1\r\nHost: 192.0.2.7\r\n\r\n", 0);
    fake_step = 0;
    CHECK(n == 0);

    /* status JSON: cut output is reported, control characters in names stay small */
    { struct sb b; char tiny[64]; memset(&b, 0, sizeof b); b.p = tiny; b.cap = sizeof tiny;
      sb_str(&b, "a\x01\x02\"b\\\x7f"); CHECK(!b.ov && strstr(tiny, "a  \\\"b\\\\ ") != NULL);
      memset(&b, 0, sizeof b); b.p = tiny; b.cap = 8; sb_add(&b, "%s", "0123456789abcdef"); CHECK(b.ov && b.n == 7); }
}

int main(void)
{
    double flat, bass;
    char *p;
    mkdir("/tmp/hbt", 0777);
    unlink("/tmp/hbt/settings.txt"); unlink("/tmp/hbt/games.txt");
    snprintf(S.version, sizeof S.version, "1.0.0");
    settings_load();
    CHECK(S.gmode == 1 && !S.eq_on && S.volume == HB_LOUD_DEFAULT && S.gps.n == 0);

    /* equalizer: off passes through, on with bass boost is louder at 40 Hz, off again is back */
    feed(256); flat = rms();
    run("c=eq&on=1&b0=6&b1=3&b2=0&b3=0&b4=0");
    CHECK(S.eq_on == 1 && S.eq_db[0] == 6 && S.eq_db[1] == 3);
    feed(256); bass = rms();
    CHECK(bass / flat > 1.7 && bass / flat < 2.2);
    run("c=eq&b0=99&b4=-99");                                      /* clamped, on kept */
    CHECK(S.eq_on == 1 && S.eq_db[0] == 12 && S.eq_db[4] == -12);
    run("c=eq&on=0");
    feed(256);
    CHECK(fabs(rms() / flat - 1.0) < 0.001);
    status();
    CHECK(balanced(json) && strstr(json, "\"eq\":{\"on\":0,\"db\":[12,3,0,0,-12]}") != NULL);
    CHECK(strstr(json, "\"game\":{\"avail\":1,\"mode\":1,\"id\":\"\",\"run\":\"\",\"name\":\"\",\"has\":0,\"profile\":0,\"active\":0,\"dirty\":0,\"saved\":[]}") != NULL);

    /* slider requests do not write at once; the daemon's poll writes them when it has been quiet */
    unlink("/tmp/hbt/settings.txt");
    run("c=eq&on=0");
    CHECK(access("/tmp/hbt/settings.txt", F_OK) != 0);
    { struct ui_cmd dummy; ui_take_cmd(&dummy); }                  /* not due yet */
    CHECK(access("/tmp/hbt/settings.txt", F_OK) != 0);
    settings_flush_due(1);
    CHECK(access("/tmp/hbt/settings.txt", F_OK) == 0 && access("/tmp/hbt/settings.txt.tmp", F_OK) != 0);

    /* settings file keeps eq and game mode */
    {
        char line[128];
        int got = 0;
        FILE *f = fopen("/tmp/hbt/settings.txt", "r");
        CHECK(f != NULL);
        while (f && fgets(line, sizeof line, f)) {
            if (!strcmp(line, "eq=0\n")) got |= 1;
            if (!strcmp(line, "eqdb=12,3,0,0,-12\n")) got |= 2;
            if (!strcmp(line, "gamemode=1\n")) got |= 4;
            if (!strcmp(line, "loudness=100\n")) got |= 8;
        }
        if (f) fclose(f);
        CHECK(got == 15);
    }
    run("c=eq&on=0&b0=0&b1=0&b2=0&b3=0&b4=0");

    /* a game starts: nothing saved yet, usual sound stays, name from the tap */
    ui_set_streaming(1);
    ui_set_pcm(10, 0, "CUSA55885", 0);
    CHECK(!strcmp(S.run, "CUSA55885") && !S.applied[0]);
    status();
    CHECK(strstr(json, "\"run\":\"CUSA55885\"") && strstr(json, "\"has\":0"));
    run("c=gsave");                                                /* saves the live sound: boost 100, EQ off */
    CHECK(S.gps.n == 1 && S.gps.g[0].boost == 100 && !S.gps.g[0].eq_on && !strcmp(S.applied, "CUSA55885"));
    /* set the sound for this game and save again */
    run("c=volume&v=250");
    run("c=eq&on=1&b0=6&b1=3&b2=0&b3=2&b4=4");
    status();
    CHECK(strstr(json, "\"has\":1,\"profile\":1,\"active\":1,\"dirty\":1"));     /* live differs from what was saved */
    run("c=gsave");
    status();
    CHECK(strstr(json, "\"has\":1,\"profile\":1,\"active\":1,\"dirty\":0") && S.gps.n == 1 && S.gps.g[0].boost == 250 && S.gps.g[0].eq_on);
    CHECK(balanced(json) && strstr(json, "\"saved\":[{\"id\":\"CUSA55885\"") && strstr(json, "\"boost\":250"));
    /* the usual sound on disk is the one from before the game, not the profile */
    {
        char line[128];
        int got = 0;
        FILE *f = fopen("/tmp/hbt/settings.txt", "r");
        while (f && fgets(line, sizeof line, f)) {
            if (!strcmp(line, "loudness=100\n")) got |= 1;
            if (!strcmp(line, "eq=0\n")) got |= 2;
        }
        if (f) fclose(f);
        CHECK(got == 3);
    }
    /* tweak while the profile is on, then the game ends: everything back to the usual sound */
    run("c=volume&v=400");
    ui_set_pcm(99, 40, "CUSA55885", 0);                            /* silent for 40 s: closed */
    CHECK(!S.run[0] && !S.applied[0] && S.volume == 100 && !S.eq_on && S.eq_db[0] == 0);
    feed(256);
    CHECK(fabs(rms() / flat - 1.0) < 0.001);                        /* the audio path follows */

    /* same game again: applied by itself, restored when the stream stops */
    ui_set_pcm(120, 0, "CUSA55885", 0);
    CHECK(!strcmp(S.applied, "CUSA55885") && S.volume == 250 && S.eq_on && S.eq_db[0] == 6);
    feed(256);
    CHECK(rms() / flat > 1.7);
    ui_set_streaming(0);
    CHECK(!S.run[0] && S.volume == 100 && !S.eq_on);
    ui_set_streaming(1);

    /* a user change while no game runs is the usual sound and sticks */
    run("c=volume&v=210");
    ui_set_pcm(1, 0, "CUSA00001", 0);                               /* a game without a profile */
    CHECK(!S.applied[0] && S.volume == 210);
    run("c=volume&v=300");
    ui_set_pcm(2, 40, "CUSA00001", 0);                              /* it closes: no profile was on, nothing is undone */
    CHECK(S.volume == 300 && !S.run[0]);
    run("c=volume&v=100");

    /* switching from a game with a profile to one without */
    ui_set_pcm(3, 0, "CUSA55885", 0);
    CHECK(S.volume == 250);
    ui_set_pcm(4, 0, "CUSA00001", 0);
    CHECK(!S.applied[0] && S.volume == 100 && !S.eq_on && !strcmp(S.run, "CUSA00001"));
    ui_set_pcm(5, 0, "CUSA55885", 0);
    CHECK(S.volume == 250 && !strcmp(S.applied, "CUSA55885"));

    /* Game mode off: the profile goes away at once and does not come back at the next start */
    run("c=gamemode&on=0");
    CHECK(S.gmode == 0 && !S.applied[0] && S.volume == 100 && !S.eq_on);
    status();
    CHECK(strstr(json, "\"mode\":0") && strstr(json, "\"has\":1,\"profile\":1,\"active\":0"));
    ui_set_pcm(6, 40, "CUSA55885", 0);
    ui_set_pcm(7, 0, "CUSA55885", 0);
    CHECK(S.volume == 100 && !S.applied[0]);
    run("c=gamemode&on=1");                                         /* back on while the game runs: applied */
    CHECK(S.gmode == 1 && S.volume == 250 && !strcmp(S.applied, "CUSA55885"));

    /* remove: the running game's profile goes, the usual sound returns */
    run("c=gdel");
    CHECK(S.gps.n == 0 && !S.applied[0] && S.volume == 100);
    run("c=gdel&id=CUSA55885");                                     /* nothing left to remove */
    status();
    CHECK(strstr(json, "\"has\":0") && strstr(json, "\"saved\":[]"));
    run("c=gsave&back=1");
    CHECK(S.gps.n == 1);                                            /* the running game again */
    run("c=gdel&id=CUSA99999");
    CHECK(S.gps.n == 1);

    /* games.txt survives a restart; bad saves are refused */
    memset(&S.gps, 0, sizeof S.gps);
    games_load();
    CHECK(S.gps.n == 1 && !strcmp(S.gps.g[0].id, "CUSA55885"));
    ui_set_streaming(0);
    {
        char before = S.gps.n;
        run("c=gsave");                                             /* no game running */
        CHECK(S.gps.n == before);
    }
    p = strstr(json, "\"ip\"");
    CHECK(p != NULL);
    printf("  web ui state test done\n");
    test_pwa();
    test_security();
    printf(fails ? "WEBUI FAILED: %d\n" : "WEBUI OK\n", fails);
    return fails != 0;
}
