/* logl.c - Interpreter for the Log language family (Log-, Log+, Log++, LogAll), written in C.
 * Portable: Linux, macOS and Windows (MinGW-w64).
 * REAL hardware access (only with --hw, Linux only):
 *   Reg:Map/Read/Write/Set/Clear/Toggle/Unmap  memory-mapped registers (/dev/mem, /dev/gpiomem)
 *   Port:In / Port:Out                         x86 I/O ports (iopl)
 *   Pin                                        real GPIO (Raspberry Pi 0-4 via /dev/gpiomem)
 *   Mem:Load / Mem:Dump                        load/dump real files to the emulated RAM
 *   Host:Read/Write/Append/Size/Delete/List/Exists/Mkdir  real files
 *   Host:Copy/Move/Lines/WriteLines/Stat/IsDir/Rmdir/Cwd/SaveJson/LoadJson  (v2.0) more file tools
 *   G3D v2: cylinder/cone/plane/diamond, z-buffer, OBJ Load/Save, Pick, Project, Orbit, Grid ...
 *   UI v2: TextArea (code editor), Dropdown, List, Progress, Title, Separator, Break
 *   Mem v2: Find, Cmp, Hash, Swap, ReadText, bit ops;  Math: popcount, clz, ctz, wrap/sext, clamp, lerp
 * v2.1 (part 1A): permissions (--allow-disk/--allow-pins/--allow-net/--allow-all), files confined to --root unless
 *   --allow-disk, shortcuts x += 1 (-= *= /=), Switch/Case/Default.
 * Build Linux:    gcc -O2 -static -o logl logl.c -lm -lpthread
 * Build Windows:  gcc -O2 -static -o logl.exe logl.c -lm -lpthread -lws2_32   (MinGW-w64 / MSYS2)
 * Usage:     logl program.log+ [--check] [--hw] [--root DIR] [--png out.png] [--repl]
 *            logl            (no arguments opens the REPL)
 */
#define _GNU_SOURCE
#define _USE_MATH_DEFINES
#ifdef _WIN32
  #define __USE_MINGW_ANSI_STDIO 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/time.h>
/* v2.1: on Windows (MinGW) the C runtime's longjmp crashes with a segmentation fault (it tries to unwind the stack),
 * which killed every program that raised an error. Use GCC's own setjmp/longjmp there: they do not go through the runtime.
 * LG_BUILTIN_JMP lets the same code be tested on Linux (gcc -DLG_BUILTIN_JMP). */
#if (defined(_WIN32) || defined(LG_BUILTIN_JMP)) && defined(__GNUC__)
  typedef void *lg_jmp_buf[5];
  #define jmp_buf lg_jmp_buf
  #undef setjmp
  #define setjmp(b) __builtin_setjmp(b)
  #define longjmp(b, v) __builtin_longjmp((b), 1)
#endif
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #include <process.h>
  #define usleep(us) Sleep(((us) + 999) / 1000)
  #define isatty _isatty
  #define getcwd _getcwd
  #define getpid _getpid
  #define mkdir(p, m) _mkdir(p)
  #define popen _popen
  #define pclose _pclose
  #ifndef S_ISREG
  #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
  #endif
  #ifndef S_ISDIR
  #define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
  #endif
  /* getline propio para no depender de la version de MinGW */
  static ssize_t getline_w(char **lp, size_t *cap, FILE *f){
    size_t n = 0; int c;
    if(!*lp || !*cap){ *cap = 128; *lp = (char *)malloc(*cap); if(!*lp) return -1; }
    while((c = fgetc(f)) != EOF){
      if(n + 2 > *cap){ *cap *= 2; char *t = (char *)realloc(*lp, *cap); if(!t) return -1; *lp = t; }
      (*lp)[n++] = (char)c; if(c == '\n') break;
    }
    if(!n) return -1;
    (*lp)[n] = 0; return (ssize_t)n;
  }
  #define getline getline_w
#else
  #include <unistd.h>
  #include <sys/mman.h>
  #include <sys/ioctl.h>
  #include <sys/syscall.h>
  #include <sys/utsname.h>
  #include <sys/resource.h>
  #include <sys/wait.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #ifdef __APPLE__
    #include <sys/sysctl.h>
    #include <mach-o/dyld.h>
  #endif
#endif

#ifdef _WIN32
  typedef SOCKET sock_t;
  #define SOCK_BAD INVALID_SOCKET
  #define sclose closesocket
#else
  typedef int sock_t;
  #define SOCK_BAD (-1)
  #define sclose close
#endif

#if defined(__x86_64__) || defined(__i386__)
  #include <cpuid.h>
  #include <x86intrin.h>
#endif

#define LOGL_VERSION "2.1"

typedef long long ll;
typedef unsigned long long ull;

/* ---------------------------------------------------------------- utilidades */
static void *xm(size_t n){ void *p = malloc(n ? n : 1); if(!p){ fputs("out of memory\n", stderr); exit(3); } return p; }
static void *xr(void *p, size_t n){ p = realloc(p, n ? n : 1); if(!p){ fputs("out of memory\n", stderr); exit(3); } return p; }
static char *xsn(const char *s, size_t n){ char *p = xm(n + 1); memcpy(p, s, n); p[n] = 0; return p; }
static char *xs(const char *s){ return xsn(s, strlen(s)); }
static char *strip_n(const char *s, size_t n){
  while(n && isspace((unsigned char)*s)){ s++; n--; }
  while(n && isspace((unsigned char)s[n-1])) n--;
  return xsn(s, n);
}
static char *strip_dup(const char *s){ return strip_n(s, strlen(s)); }
static char *lowerdup(const char *s){ char *p = xs(s); for(char *q = p; *q; q++) *q = tolower((unsigned char)*q); return p; }

typedef struct { char *s; int n, cap; } Buf;
static void bcat(Buf *b, const char *s, int n){
  if(b->n + n + 1 > b->cap){ b->cap = (b->n + n + 1) * 2; b->s = xr(b->s, b->cap); }
  memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
static void bs(Buf *b, const char *s){ bcat(b, s, (int)strlen(s)); }
static void bc(Buf *b, char c){ bcat(b, &c, 1); }
static char *btake(Buf *b){ return b->s ? b->s : xs(""); }

typedef struct { char **v; int n, cap; } SV;
static void svp(SV *s, char *x){ if(s->n == s->cap){ s->cap = s->cap ? s->cap * 2 : 4; s->v = xr(s->v, s->cap * sizeof(char *)); } s->v[s->n++] = x; }

/* rutas estilo posixpath */
static char *normpath(const char *p){
  if(!*p) return xs(".");
  int ab = p[0] == '/';
  char *cp = xs(p), **seg = xm(sizeof(char *) * (strlen(p) + 2)); int n = 0;
  for(char *t = cp; ; ){
    char *e = strchr(t, '/'); if(e) *e = 0;
    if(*t && strcmp(t, ".")){
      if(!strcmp(t, "..")){ if(n && strcmp(seg[n-1], "..")) n--; else if(!ab) seg[n++] = t; }
      else seg[n++] = t;
    }
    if(!e) break; t = e + 1;
  }
  Buf b = {0}; if(ab) bc(&b, '/');
  for(int i = 0; i < n; i++){ if(i) bc(&b, '/'); bs(&b, seg[i]); }
  char *r = b.n ? btake(&b) : xs(ab ? "/" : ".");
  free(cp); free(seg); return r;
}
static char *pjoin(const char *a, const char *b){
  if(b[0] == '/' || !*a || (isalpha((unsigned char)b[0]) && b[1] == ':')) return xs(b);
  Buf r = {0}; bs(&r, a); if(a[strlen(a)-1] != '/') bc(&r, '/'); bs(&r, b); return btake(&r);
}
static char *pdirname(const char *p){
  const char *s = strrchr(p, '/'); if(!s) return xs("");
  const char *e = s; while(e > p && e[-1] == '/') e--;
  return e == p ? xs("/") : xsn(p, e - p);
}
static void fixslash(char *s){ for(; s && *s; s++) if(*s == '\\') *s = '/'; }

static char *readfile(const char *p, size_t *len){
  FILE *f = fopen(p, "rb"); if(!f) return NULL;
  Buf b = {0}; char t[8192]; size_t n;
  while((n = fread(t, 1, sizeof t, f)) > 0) bcat(&b, t, (int)n);
  fclose(f); if(len) *len = b.n; return btake(&b);
}
static int writefile(const char *p, const char *d, size_t n, const char *mode){
  FILE *f = fopen(p, mode); if(!f) return -1;
  fwrite(d, 1, n, f); fclose(f); return 0;
}

/* ---------------------------------------------------------------- valores */
enum { T_UND, T_BOOL, T_NUM, T_STR, T_LIST, T_DICT, T_FN };
typedef struct { int t; union { double n; void *p; } u; } Val;
typedef struct { int rc, len; char s[1]; } Str;
typedef struct { int rc, n, cap; Val *a; } List;
typedef struct { int rc, n, cap; char **k; Val *v; signed char *ty; } Dict;
#define SP(v) ((Str *)(v).u.p)
#define LP(v) ((List *)(v).u.p)
#define DP(v) ((Dict *)(v).u.p)
#define CSTR(v) (SP(v)->s)

static Val Vu(void){ Val v; v.t = T_UND; v.u.n = 0; return v; }
static Val Vn(double d){ Val v; v.t = T_NUM; v.u.n = d; return v; }
static Val Vb(int b){ Val v; v.t = T_BOOL; v.u.n = b ? 1 : 0; return v; }
static Val Vsn(const char *s, int n){ Str *p = xm(sizeof(Str) + n); p->rc = 1; p->len = n; memcpy(p->s, s, n); p->s[n] = 0; Val v; v.t = T_STR; v.u.p = p; return v; }
static Val Vs(const char *s){ return Vsn(s, (int)strlen(s)); }
static Val Vl(void){ List *l = calloc(1, sizeof *l); l->rc = 1; Val v; v.t = T_LIST; v.u.p = l; return v; }
static Val Vd(void){ Dict *d = calloc(1, sizeof *d); d->rc = 1; Val v; v.t = T_DICT; v.u.p = d; return v; }
static Val Vf(int i){ Val v; v.t = T_FN; v.u.n = i; return v; }
static int isobj(Val v){ return v.t >= T_STR && v.t <= T_DICT; }
static Val vref(Val v){ if(isobj(v)) (*(int *)v.u.p)++; return v; }
static void vdrop(Val v){
  if(!isobj(v)) return;
  if(--*(int *)v.u.p > 0) return;
  if(v.t == T_LIST){ List *l = LP(v); for(int i = 0; i < l->n; i++) vdrop(l->a[i]); free(l->a); }
  else if(v.t == T_DICT){ Dict *d = DP(v); for(int i = 0; i < d->n; i++){ free(d->k[i]); vdrop(d->v[i]); } free(d->k); free(d->v); free(d->ty); }
  free(v.u.p);
}
static void lpush(List *l, Val v){ if(l->n == l->cap){ l->cap = l->cap ? l->cap * 2 : 8; l->a = xr(l->a, l->cap * sizeof(Val)); } l->a[l->n++] = v; }
static int dfind(Dict *d, const char *k){ for(int i = 0; i < d->n; i++) if(!strcmp(d->k[i], k)) return i; return -1; }
static Val *dget(Dict *d, const char *k){ int i = dfind(d, k); return i < 0 ? NULL : &d->v[i]; }
static int dset(Dict *d, const char *k, Val v){ /* toma posesion de v */
  int i = dfind(d, k);
  if(i >= 0){ vdrop(d->v[i]); d->v[i] = v; return i; }
  if(d->n == d->cap){ d->cap = d->cap ? d->cap * 2 : 8; d->k = xr(d->k, d->cap * sizeof(char *)); d->v = xr(d->v, d->cap * sizeof(Val)); d->ty = xr(d->ty, d->cap); }
  d->k[d->n] = xs(k); d->v[d->n] = v; d->ty[d->n] = -1; return d->n++;
}
static void dremove(Dict *d, const char *k){
  int i = dfind(d, k); if(i < 0) return;
  free(d->k[i]); vdrop(d->v[i]);
  for(int j = i; j < d->n - 1; j++){ d->k[j] = d->k[j+1]; d->v[j] = d->v[j+1]; d->ty[j] = d->ty[j+1]; }
  d->n--;
}

/* ---------------------------------------------------------------- hilo / errores */
typedef struct Scope Scope;
typedef struct Task Task;
typedef struct Stmt Stmt;
typedef struct { jmp_buf *jb; char err[1024]; Scope *cur; int depth, tick; Task *task; Val retv; } Th;
static __thread Th *T;
static pthread_mutex_t GIL = PTHREAD_MUTEX_INITIALIZER;
static int nthreads = 0;

static void lerr(const char *f, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void lerr(const char *f, ...){
  va_list a; va_start(a, f); vsnprintf(T->err, sizeof T->err, f, a); va_end(a);
  longjmp(*T->jb, 1);
}

/* ---------------------------------------------------------------- conversiones */
static void fmtnum(Buf *b, double v){
  if(v != v){ bs(b, "NaN"); return; }
  if(isinf(v)){ bs(b, v > 0 ? "Infinity" : "-Infinity"); return; }
  char t[64];
  if(v == floor(v) && fabs(v) < 1e21){ if(v == 0) v = 0; snprintf(t, sizeof t, "%.0f", v); bs(b, t); return; }
  for(int p = 1; p <= 17; p++){ snprintf(t, sizeof t, "%.*g", p, v); if(strtod(t, NULL) == v) break; }
  bs(b, t);
}
static void jbuf(Buf *b, Val v){
  switch(v.t){
  case T_UND: bs(b, "undefined"); break;
  case T_BOOL: bs(b, v.u.n ? "true" : "false"); break;
  case T_NUM: fmtnum(b, v.u.n); break;
  case T_STR: bcat(b, CSTR(v), SP(v)->len); break;
  case T_LIST: { List *l = LP(v); for(int i = 0; i < l->n; i++){ if(i) bc(b, ','); jbuf(b, l->a[i]); } break; }
  case T_DICT: bs(b, "[object Object]"); break;
  default: bs(b, "function");
  }
}
static char *jstr(Val v){ Buf b = {0}; jbuf(&b, v); return btake(&b); }
static int truthy(Val v){
  switch(v.t){
  case T_UND: return 0;
  case T_BOOL: case T_NUM: return v.u.n == v.u.n && v.u.n != 0;
  case T_STR: return SP(v)->len > 0;
  default: return 1;
  }
}
static double tonumok(Val v, int *ok){
  *ok = 1;
  if(v.t == T_NUM || v.t == T_BOOL) return v.u.n;
  if(v.t == T_STR){
    const char *s = CSTR(v); char *e; while(isspace((unsigned char)*s)) s++;
    if(*s){ double d = strtod(s, &e); while(isspace((unsigned char)*e)) e++; if(!*e) return d; }
  }
  *ok = 0; return NAN;
}
static double tonum(Val v){ int ok; return tonumok(v, &ok); }
static ll dint(double d){ if(d != d || isinf(d)) return 0; if(d > 9.2e18) d = 9.2e18; if(d < -9.2e18) d = -9.2e18; return (ll)d; }
static ll toint(Val v){ return dint(tonum(v)); }
static ll i32v(ll x){ return (ll)(int32_t)(uint32_t)(ull)x; }

static const char *TYP[] = {"u8", "u16", "u32", "i32", "f32", "f64", "str"};
static int typidx(const char *s){ for(int i = 0; i < 7; i++) if(!strcasecmp(s, TYP[i])) return i; return -1; }
static Val cv(int t, Val v){ /* consume v */
  if(t < 0 || v.t == T_LIST || v.t == T_DICT) return v;   /* typed variables keep whole lists and dictionaries (their elements are typed) */
  Val r; int ok;
  switch(t){
  case 0: r = Vn((double)(toint(v) & 255)); break;
  case 1: r = Vn((double)(toint(v) & 65535)); break;
  case 2: r = Vn((double)(toint(v) & 0xFFFFFFFFLL)); break;
  case 3: r = Vn((double)i32v(toint(v))); break;
  case 4: { double d = tonumok(v, &ok); if(!ok) lerr("Cannot convert to number"); r = Vn((double)(float)d); break; }
  case 5: { double d = tonumok(v, &ok); if(!ok) lerr("Cannot convert to number"); r = Vn(d); break; }
  default: { char *s = jstr(v); int n = (int)strlen(s); if(n > 65535) n = 65535; r = Vsn(s, n); free(s); }
  }
  vdrop(v); return r;
}
static int veq(Val a, Val b){
  int an = a.t == T_NUM || a.t == T_BOOL, bn = b.t == T_NUM || b.t == T_BOOL;
  if(an && bn) return a.u.n == b.u.n;
  if(a.t != b.t) return 0;
  switch(a.t){
  case T_UND: return 1;
  case T_STR: return SP(a)->len == SP(b)->len && !memcmp(CSTR(a), CSTR(b), SP(a)->len);
  case T_LIST: { List *x = LP(a), *y = LP(b); if(x == y) return 1; if(x->n != y->n) return 0;
    for(int i = 0; i < x->n; i++) if(!veq(x->a[i], y->a[i])) return 0; return 1; }
  case T_DICT: return a.u.p == b.u.p;
  default: return a.u.n == b.u.n;
  }
}

/* ---------------------------------------------------------------- JSON minimo */
static const char *jp; static int jperr;
static void jws(void){ while(isspace((unsigned char)*jp)) jp++; }
static Val jval(int depth);
static void utf8(Buf *b, unsigned c){
  if(c < 0x80) bc(b, c);
  else if(c < 0x800){ bc(b, 0xC0 | (c >> 6)); bc(b, 0x80 | (c & 63)); }
  else { bc(b, 0xE0 | (c >> 12)); bc(b, 0x80 | ((c >> 6) & 63)); bc(b, 0x80 | (c & 63)); }
}
static char *jstring(void){
  Buf b = {0}; jp++;
  while(*jp && *jp != '"'){
    if(*jp == '\\'){ jp++;
      switch(*jp){ case 'n': bc(&b, '\n'); break; case 't': bc(&b, '\t'); break; case 'r': bc(&b, '\r'); break;
        case 'b': bc(&b, '\b'); break; case 'f': bc(&b, '\f'); break;
        case 'u': { unsigned c = 0; for(int i = 1; i <= 4; i++){ if(!isxdigit((unsigned char)jp[i])){ jperr = 1; return btake(&b); } c = c * 16 + (isdigit((unsigned char)jp[i]) ? jp[i] - '0' : (tolower(jp[i]) - 'a' + 10)); } utf8(&b, c); jp += 4; break; }
        case 0: jperr = 1; return btake(&b);
        default: bc(&b, *jp); }
      jp++;
    } else bc(&b, *jp++);
  }
  if(*jp != '"') jperr = 1; else jp++;
  return btake(&b);
}
static Val jval(int depth){
  jws();
  if(depth > 64){ jperr = 1; return Vu(); }
  if(*jp == '"'){ char *s = jstring(); Val v = Vs(s); free(s); return v; }
  if(*jp == '{'){
    Val d = Vd(); jp++; jws();
    if(*jp == '}'){ jp++; return d; }
    for(;;){
      jws(); if(*jp != '"'){ jperr = 1; return d; }
      char *k = jstring(); jws(); if(*jp != ':'){ jperr = 1; free(k); return d; } jp++;
      dset(DP(d), k, jval(depth + 1)); free(k); if(jperr) return d; jws();
      if(*jp == ','){ jp++; continue; } if(*jp == '}'){ jp++; return d; } jperr = 1; return d;
    }
  }
  if(*jp == '['){
    Val l = Vl(); jp++; jws();
    if(*jp == ']'){ jp++; return l; }
    for(;;){
      lpush(LP(l), jval(depth + 1)); if(jperr) return l; jws();
      if(*jp == ','){ jp++; continue; } if(*jp == ']'){ jp++; return l; } jperr = 1; return l;
    }
  }
  if(!strncmp(jp, "true", 4)){ jp += 4; return Vb(1); }
  if(!strncmp(jp, "false", 5)){ jp += 5; return Vb(0); }
  if(!strncmp(jp, "null", 4)){ jp += 4; return Vu(); }
  if(!strncmp(jp, "NaN", 3)){ jp += 3; return Vn(NAN); }
  if(!strncmp(jp, "Infinity", 8)){ jp += 8; return Vn(INFINITY); }
  if(!strncmp(jp, "-Infinity", 9)){ jp += 9; return Vn(-INFINITY); }
  char *e; double d = strtod(jp, &e);
  if(e == jp){ jperr = 1; return Vu(); }
  jp = e; return Vn(d);
}
static Val jparse(const char *s, int *err){ jp = s; jperr = 0; Val v = jval(0); jws(); if(*jp) jperr = 1; *err = jperr; return v; }
static void jdump(Buf *b, Val v){
  switch(v.t){
  case T_UND: bs(b, "null"); break;
  case T_BOOL: bs(b, v.u.n ? "true" : "false"); break;
  case T_NUM: fmtnum(b, v.u.n); break;
  case T_STR: { bc(b, '"'); for(int i = 0; i < SP(v)->len; i++){ unsigned char c = CSTR(v)[i];
      if(c == '"' || c == '\\'){ bc(b, '\\'); bc(b, c); } else if(c == '\n') bs(b, "\\n"); else if(c == '\t') bs(b, "\\t"); else if(c == '\r') bs(b, "\\r");
      else if(c < 32){ char t[8]; snprintf(t, sizeof t, "\\u%04x", c); bs(b, t); } else bc(b, c); }
    bc(b, '"'); break; }
  case T_LIST: { bc(b, '['); for(int i = 0; i < LP(v)->n; i++){ if(i) bc(b, ','); jdump(b, LP(v)->a[i]); } bc(b, ']'); break; }
  case T_DICT: { Dict *d = DP(v); bc(b, '{'); for(int i = 0; i < d->n; i++){ if(i) bc(b, ','); jdump(b, Vs(d->k[i])); bc(b, ':'); jdump(b, d->v[i]); } bc(b, '}'); break; }
  default: bs(b, "null");
  }
}

/* ---------------------------------------------------------------- ambitos */
typedef struct { char *name; Stmt *x; Scope *sc; } Fn;
struct Scope { int rc; Dict *v; Scope *p; Fn *fns; int nfn, cfn; int xd; };

static Scope *scope_new(Scope *parent){
  Scope *s = calloc(1, sizeof *s); s->rc = 1; Val d = Vd(); s->v = DP(d); s->p = parent;
  if(parent) parent->rc++;
  return s;
}
static void scope_ref(Scope *s){ s->rc++; }
static void scope_unref(Scope *s){
  if(--s->rc > 0) return;
  Val d; d.t = T_DICT; d.u.p = s->v; vdrop(d);
  for(int i = 0; i < s->nfn; i++) free(s->fns[i].name);
  free(s->fns);
  if(s->p) scope_unref(s->p);
  free(s);
}
static Scope *stop_(Scope *s){ while(s->p) s = s->p; return s; }
static Val *slookup(Scope *s, const char *n, Dict **dd, int *ix){
  for(; s; s = s->p){ int i = dfind(s->v, n); if(i >= 0){ if(dd) *dd = s->v; if(ix) *ix = i; return &s->v->v[i]; } }
  return NULL;
}
static int shas(Scope *s, const char *n){ return slookup(s, n, NULL, NULL) != NULL; }
static Val sget(Scope *s, const char *n){ Val *p = slookup(s, n, NULL, NULL); if(!p) lerr("Undeclared variable: %s", n); return vref(*p); }
static int sty(Scope *s, const char *n){ Dict *d; int i; if(!slookup(s, n, &d, &i)) return -1; return d->ty[i]; }
static void sdecl(Scope *s, const char *n, Val v, int ty){ int i = dset(s->v, n, v); s->v->ty[i] = ty; }
static void ssetv(Scope *s, const char *n, Val v){ /* consume v */
  Dict *d; int i; if(!slookup(s, n, &d, &i)){ vdrop(v); lerr("Undeclared variable: %s", n); }
  Val nv = cv(d->ty[i], v); vdrop(d->v[i]); d->v[i] = nv;
}
static void fn_set(Scope *top, const char *name, Stmt *x, Scope *sc){
  for(int i = 0; i < top->nfn; i++) if(!strcmp(top->fns[i].name, name)){ top->fns[i].x = x; top->fns[i].sc = sc; return; }
  if(top->nfn == top->cfn){ top->cfn = top->cfn ? top->cfn * 2 : 8; top->fns = xr(top->fns, top->cfn * sizeof(Fn)); }
  top->fns[top->nfn].name = xs(name); top->fns[top->nfn].x = x; top->fns[top->nfn].sc = sc; top->nfn++;
}
static Fn *fn_get(Scope *top, const char *name){ for(int i = 0; i < top->nfn; i++) if(!strcmp(top->fns[i].name, name)) return &top->fns[i]; return NULL; }

/* ---------------------------------------------------------------- expresiones: tokens */
typedef struct { char k; double n; char *s; } Tok;
static const char *OPN[] = {"??","||","&&","|","^","&","==","!=","===","!==","<",">","<=",">=","<<",">>",">>>","+","-","*","/","%","**"};
static const int OPP[] = {1,2,3,4,5,6,7,7,7,7,8,8,8,8,9,9,9,10,10,11,11,11,12};
enum { O_NC,O_OR,O_AND,O_BOR,O_XOR,O_BAND,O_EQ,O_NE,O_SEQ,O_SNE,O_LT,O_GT,O_LE,O_GE,O_SHL,O_SHR,O_USHR,O_ADD,O_SUB,O_MUL,O_DIV,O_MOD,O_POW, O_COUNT };
static int binidx(const char *s){ for(int i = 0; i < O_COUNT; i++) if(!strcmp(OPN[i], s)) return i; return -1; }

static Tok *tokenize(const char *src, int *cnt){
  Tok *t = NULL; int n = 0, cap = 0;
  size_t len = strlen(src); while(len && isspace((unsigned char)src[len-1])) len--;
  const char *p = src, *end = src + len;
#define PUSHT(K, N, S) do{ if(n == cap){ cap = cap ? cap * 2 : 16; t = xr(t, cap * sizeof(Tok)); } t[n].k = (K); t[n].n = (N); t[n].s = (S); n++; }while(0)
  for(;;){
    while(p < end && isspace((unsigned char)*p)) p++;
    if(p >= end) break;
    if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])){
      char *e; double d = (double)strtoull(p, &e, 16); PUSHT('n', d, NULL); p = e; continue;
    }
    if(isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))){
      const char *q = p; while(isdigit((unsigned char)*q)) q++;
      if(*q == '.'){ q++; while(isdigit((unsigned char)*q)) q++; }
      if(*q == 'e' || *q == 'E'){ const char *r = q + 1; if(*r == '+' || *r == '-') r++; if(isdigit((unsigned char)*r)){ while(isdigit((unsigned char)*r)) r++; q = r; } }
      char *tmp = xsn(p, q - p); double d = strtod(tmp, NULL); free(tmp); PUSHT('n', d, NULL); p = q; continue;
    }
    if(*p == '"' || *p == '\''){
      char qc = *p++; Buf b = {0};
      while(p < end && *p != qc){
        if(*p == '\\' && p + 1 < end){ p++; bc(&b, *p == 'n' ? '\n' : *p == 't' ? '\t' : *p); p++; }
        else bc(&b, *p++);
      }
      if(p >= end){ free(b.s); lerr("Invalid expression: %s", src); }
      p++; PUSHT('s', 0, btake(&b)); continue;
    }
    if(isalpha((unsigned char)*p) || *p == '_' || *p == '$'){
      const char *q = p; while(isalnum((unsigned char)*q) || *q == '_' || *q == '$') q++;
      PUSHT('i', 0, xsn(p, q - p)); p = q; continue;
    }
    static const char *o3[] = {">>>", "===", "!=="}, *o2[] = {"**","&&","||","??","<<",">>","==","!=","<=",">="};
    int found = 0;
    for(int i = 0; i < 3 && !found; i++) if(!strncmp(p, o3[i], 3)){ PUSHT('o', 0, xs(o3[i])); p += 3; found = 1; }
    for(int i = 0; i < 10 && !found; i++) if(!strncmp(p, o2[i], 2)){ PUSHT('o', 0, xs(o2[i])); p += 2; found = 1; }
    if(found) continue;
    if(strchr("-+*/%<>=!&|^~?:.,()[]", *p)){ PUSHT('o', 0, xsn(p, 1)); p++; continue; }
    lerr("Invalid expression: unexpected character '%c'", *p);
  }
  *cnt = n; return t;
}

/* ---------------------------------------------------------------- expresiones: parser */
typedef struct Node { char k; int op; double n; char *s; struct Node *a, *b, *c, **args; int na; } Node;
typedef struct { Tok *t; int n, i; } PS;
static Tok EOFT = {0, 0, NULL};
static Tok *pk(PS *p){ return p->i < p->n ? &p->t[p->i] : &EOFT; }
static Tok *nx(PS *p){ Tok *t = pk(p); p->i++; return t; }
static int isop(Tok *t, const char *o){ return t->k == 'o' && !strcmp(t->s, o); }
static void eat(PS *p, const char *o){ if(!isop(nx(p), o)) lerr("Invalid expression: expected %s", o); }
static Node *mk(char k){ Node *n = calloc(1, sizeof *n); n->k = k; return n; }
static Node *pexpr(PS *p);
static Node *pprim(PS *p){
  Tok *t = nx(p); Node *n;
  if(t->k == 'n'){ n = mk('n'); n->n = t->n; return n; }
  if(t->k == 's'){ n = mk('s'); n->s = t->s; return n; }
  if(t->k == 'i'){ n = mk('i'); n->s = t->s; return n; }
  if(isop(t, "(")){ n = pexpr(p); eat(p, ")"); return n; }
  if(isop(t, "[")){
    n = mk('A');
    while(!isop(pk(p), "]")){ n->args = xr(n->args, (n->na + 1) * sizeof(Node *)); n->args[n->na++] = pexpr(p); if(isop(pk(p), ",")) nx(p); }
    eat(p, "]"); return n;
  }
  lerr("Invalid expression");
}
static Node *ppost(PS *p){
  Node *e = pprim(p);
  for(;;){
    if(isop(pk(p), ".")){
      nx(p); Tok *t = nx(p); if(t->k != 'i') lerr("Invalid expression");
      Node *n = mk('.'); n->a = e; n->s = t->s; e = n;
    } else if(isop(pk(p), "[")){
      nx(p); Node *n = mk('['); n->a = e; n->b = pexpr(p); eat(p, "]"); e = n;
    } else if(isop(pk(p), "(")){
      nx(p); Node *n = mk('C'); n->a = e;
      while(!isop(pk(p), ")")){ n->args = xr(n->args, (n->na + 1) * sizeof(Node *)); n->args[n->na++] = pexpr(p); if(isop(pk(p), ",")) nx(p); }
      eat(p, ")"); e = n;
    } else return e;
  }
}
static Node *pun(PS *p){
  Tok *t = pk(p);
  if(t->k == 'o' && t->s[1] == 0 && strchr("-+!~", t->s[0])){ nx(p); Node *n = mk('u'); n->op = t->s[0]; n->a = pun(p); return n; }
  return ppost(p);
}
static Node *pbin(PS *p, int mp){
  Node *l = pun(p);
  for(;;){
    Tok *t = pk(p); int o;
    if(t->k != 'o' || (o = binidx(t->s)) < 0 || OPP[o] <= mp) return l;
    nx(p); Node *r = pbin(p, o == O_POW ? OPP[o] - 1 : OPP[o]);
    Node *n = mk('B'); n->op = o; n->a = l; n->b = r; l = n;
  }
}
static Node *pexpr(PS *p){
  Node *c = pbin(p, 0);
  if(isop(pk(p), "?")){ nx(p); Node *n = mk('?'); n->a = c; n->b = pexpr(p); eat(p, ":"); n->c = pexpr(p); return n; }
  return c;
}
typedef struct { char *key; Node *n; } CE;
static CE *cache; static int ccap, ccnt;
static unsigned hstr(const char *s){ unsigned h = 5381; while(*s) h = h * 33 + (unsigned char)*s++; return h; }
static void cput(const char *k, Node *n);
static void cgrow(void){
  CE *old = cache; int oc = ccap; ccap = ccap ? ccap * 2 : 256; cache = calloc(ccap, sizeof(CE)); ccnt = 0;
  for(int i = 0; i < oc; i++) if(old[i].key) cput(old[i].key, old[i].n);
  free(old);
}
static void cput(const char *k, Node *n){
  if((ccnt + 1) * 2 > ccap) cgrow();
  unsigned i = hstr(k) & (ccap - 1);
  while(cache[i].key) i = (i + 1) & (ccap - 1);
  cache[i].key = (char *)k; cache[i].n = n; ccnt++;
}
static Node *compile(const char *e){
  if(ccap){ unsigned i = hstr(e) & (ccap - 1); while(cache[i].key){ if(!strcmp(cache[i].key, e)) return cache[i].n; i = (i + 1) & (ccap - 1); } }
  int n; Tok *t = tokenize(e, &n); PS p = {t, n, 0};
  Node *r = pexpr(&p); cput(xs(e), r); return r;
}

/* ---------------------------------------------------------------- Math */
enum { F_abs, F_floor, F_ceil, F_round, F_trunc, F_sqrt, F_pow, F_min, F_max, F_sin, F_cos, F_tan, F_asin, F_acos, F_atan, F_atan2, F_exp, F_log, F_log2, F_log10, F_hypot, F_random, F_fround, F_sign, F_cbrt, F_rnd, F_fix, F_hex, F_bit, F_lo32, F_hi32, F_rol, F_ror, F_bswap, F_popcount, F_clz, F_ctz, F_wrap8, F_wrap16, F_wrap32, F_sext8, F_sext16, F_sext32, F_bswap16, F_clamp, F_lerp, F_pin, F_COUNT };
static const char *FNN[] = {"abs","floor","ceil","round","trunc","sqrt","pow","min","max","sin","cos","tan","asin","acos","atan","atan2","exp","log","log2","log10","hypot","random","fround","sign","cbrt","rnd","fix","hex","bit","lo32","hi32","rol","ror","bswap","popcount","clz","ctz","wrap8","wrap16","wrap32","sext8","sext16","sext32","bswap16","clamp","lerp","pin"};
static Val MATHV;
static double elapsed_ms(void);
static ull rngs = 88172645463325252ULL;
static double rnd(void){ rngs ^= rngs << 13; rngs ^= rngs >> 7; rngs ^= rngs << 17; return (double)(rngs >> 11) / 9007199254740992.0; }
static double numarg(Val v){ if(v.t == T_NUM || v.t == T_BOOL) return v.u.n; lerr("Math: expected a number"); }
static int pin_level(int n);   /* defined after the global state S */
static Val callfn(int f, Val *a, int na){
  if(f == F_random) return Vn(rnd());
  if(f == F_pin){ if(na < 1) lerr("pin(n) needs a pin number"); return Vn(pin_level((int)numarg(a[0]))); }   /* v2.1: documented in ch. 6, missing in logl 2.0 */
  if(f >= F_hex && f <= F_bswap){   /* bit tools for low-level code */
    if(!na) lerr("Math: not enough arguments");
    double d0 = numarg(a[0]); ull v = (ull)(ll)dint(d0); uint32_t v32 = (uint32_t)v; int n = na > 1 ? (int)numarg(a[1]) : 0;
    if(f == F_hex){ int w = na > 1 ? n : 1; if(w < 1) w = 1; if(w > 16) w = 16; char t[40]; snprintf(t, sizeof t, "0x%0*llX", w, v); return Vs(t); }
    if(f == F_bit){ if(na < 2) lerr("Math: not enough arguments"); return Vn((double)((v >> (n & 63)) & 1)); }
    if(f == F_lo32) return Vn((double)v32);
    if(f == F_hi32) return Vn((double)(uint32_t)(v >> 32));
    if(f == F_rol){ if(na < 2) lerr("Math: not enough arguments"); n &= 31; return Vn((double)(uint32_t)((v32 << n) | (v32 >> ((32 - n) & 31)))); }
    if(f == F_ror){ if(na < 2) lerr("Math: not enough arguments"); n &= 31; return Vn((double)(uint32_t)((v32 >> n) | (v32 << ((32 - n) & 31)))); }
    return Vn((double)__builtin_bswap32(v32));
  }
  if(f >= F_popcount && f <= F_bswap16){   /* v2.0 extra bit tools */
    if(!na) lerr("Math: not enough arguments");
    ull v = (ull)(ll)dint(numarg(a[0])); uint32_t v32 = (uint32_t)v;
    switch(f){
    case F_popcount: return Vn((double)__builtin_popcountll(v));
    case F_clz: return Vn(v32 ? (double)__builtin_clz(v32) : 32.0);
    case F_ctz: return Vn(v32 ? (double)__builtin_ctz(v32) : 32.0);
    case F_wrap8: return Vn((double)(uint8_t)v); case F_wrap16: return Vn((double)(uint16_t)v); case F_wrap32: return Vn((double)v32);
    case F_sext8: return Vn((double)(int8_t)v); case F_sext16: return Vn((double)(int16_t)v); case F_sext32: return Vn((double)(int32_t)v32);
    default: return Vn((double)(uint16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)));
    }
  }
  if(f == F_clamp){ if(na < 3) lerr("Math: clamp(x, lo, hi) needs 3 arguments"); double x_ = numarg(a[0]), lo_ = numarg(a[1]), hi_ = numarg(a[2]); return Vn(x_ < lo_ ? lo_ : x_ > hi_ ? hi_ : x_); }
  if(f == F_lerp){ if(na < 3) lerr("Math: lerp(a, b, t) needs 3 arguments"); double a_ = numarg(a[0]), b_ = numarg(a[1]), t_ = numarg(a[2]); return Vn(a_ + (b_ - a_) * t_); }
  if(f == F_fix){   /* fix(v,n): text with n decimals (default 2) */
    if(!na) lerr("Math: not enough arguments");
    int nd = na > 1 ? (int)numarg(a[1]) : 2; if(nd < 0) nd = 0; if(nd > 20) nd = 20;
    char tb[400]; snprintf(tb, sizeof tb, "%.*f", nd, numarg(a[0])); return Vs(tb);
  }
  if(f == F_rnd){   /* rnd(a,b): whole number from a to b, both included; rnd(n): 0..n; rnd(): 0..1 */
    if(!na) return Vn(rnd());
    double lo = 0, hi = numarg(a[0]); if(na > 1){ lo = hi; hi = numarg(a[1]); }
    if(hi < lo){ double tmp = lo; lo = hi; hi = tmp; }
    return Vn(floor(lo + rnd() * (hi - lo + 1)));
  }
  if(f == F_min || f == F_max){
    if(!na) lerr("Math: missing arguments");
    double r = numarg(a[0]);
    for(int i = 1; i < na; i++){ double q = numarg(a[i]); r = f == F_min ? (q < r ? q : r) : (q > r ? q : r); }
    return Vn(r);
  }
  int need = (f == F_pow || f == F_atan2 || f == F_hypot) ? 2 : 1;
  if(na < need) lerr("Math: not enough arguments");
  double x = numarg(a[0]), y = need > 1 ? numarg(a[1]) : 0;
  switch(f){
  case F_abs: return Vn(fabs(x)); case F_floor: return Vn(floor(x)); case F_ceil: return Vn(ceil(x));
  case F_round: return Vn(floor(x + 0.5)); case F_trunc: return Vn(trunc(x));
  case F_sqrt: return Vn(x >= 0 ? sqrt(x) : NAN); case F_pow: return Vn(pow(x, y));
  case F_sin: return Vn(sin(x)); case F_cos: return Vn(cos(x)); case F_tan: return Vn(tan(x));
  case F_asin: return Vn(asin(x)); case F_acos: return Vn(acos(x)); case F_atan: return Vn(atan(x)); case F_atan2: return Vn(atan2(x, y));
  case F_exp: return Vn(exp(x)); case F_log: return Vn(log(x)); case F_log2: return Vn(log2(x)); case F_log10: return Vn(log10(x));
  case F_hypot: return Vn(hypot(x, y)); case F_fround: return Vn((double)(float)x);
  case F_sign: return Vn((x > 0) - (x < 0)); default: return Vn(cbrt(x));
  }
}
static void math_init(void){
  MATHV = Vd(); dset(DP(MATHV), "PI", Vn(M_PI)); dset(DP(MATHV), "E", Vn(M_E));
  for(int i = 0; i < F_COUNT; i++) dset(DP(MATHV), FNN[i], Vf(i));
}

/* ---------------------------------------------------------------- evaluacion */
static int isnumv(Val v){ return v.t == T_NUM || v.t == T_BOOL; }
static Val binop(int op, Val a, Val b){
  switch(op){
  case O_ADD:
    if(a.t == T_STR || b.t == T_STR){ Buf r = {0}; jbuf(&r, a); jbuf(&r, b); char *s = btake(&r); Val v = Vsn(s, r.n); free(s); return v; }
    if(isnumv(a) && isnumv(b)) return Vn(a.u.n + b.u.n);
    lerr("Invalid operands for +");
  case O_SUB: case O_MUL: case O_DIV: case O_MOD: case O_POW:
    if(!isnumv(a) || !isnumv(b)) lerr("Invalid operands for %s", OPN[op]);
    switch(op){ case O_SUB: return Vn(a.u.n - b.u.n); case O_MUL: return Vn(a.u.n * b.u.n);
      case O_DIV: return Vn(a.u.n / b.u.n); case O_MOD: return Vn(b.u.n == 0 ? NAN : fmod(a.u.n, b.u.n));
      default: return Vn(pow(a.u.n, b.u.n)); }
  case O_EQ: case O_SEQ: return Vb(veq(a, b));
  case O_NE: case O_SNE: return Vb(!veq(a, b));
  case O_LT: case O_GT: case O_LE: case O_GE: {
    int c;
    if(a.t == T_STR && b.t == T_STR){ int r = strcmp(CSTR(a), CSTR(b)); c = op == O_LT ? r < 0 : op == O_GT ? r > 0 : op == O_LE ? r <= 0 : r >= 0; }
    else if(isnumv(a) && isnumv(b)){ double x = a.u.n, y = b.u.n; c = op == O_LT ? x < y : op == O_GT ? x > y : op == O_LE ? x <= y : x >= y; }
    else lerr("Cannot compare these values");
    return Vb(c);
  }
  case O_BAND: return Vn((double)i32v(toint(a) & toint(b)));
  case O_BOR: return Vn((double)i32v(toint(a) | toint(b)));
  case O_XOR: return Vn((double)i32v(toint(a) ^ toint(b)));
  case O_SHL: return Vn((double)i32v((ll)((ull)toint(a) << (toint(b) & 31))));
  case O_SHR: return Vn((double)(i32v(toint(a)) >> (toint(b) & 31)));
  case O_USHR: return Vn((double)((toint(a) & 0xFFFFFFFFLL) >> (toint(b) & 31)));
  }
  lerr("Unsupported operator: %s", OPN[op]);
}
static void slice_idx(int len, Val *a, int na, int *s, int *e){
  ll st = na > 0 ? toint(a[0]) : 0, en = na > 1 ? toint(a[1]) : len;
  if(st < 0){ st += len; if(st < 0) st = 0; } if(st > len) st = len;
  if(en < 0){ en += len; if(en < 0) en = 0; } if(en > len) en = len;
  if(en < st) en = st; *s = (int)st; *e = (int)en;
}
static Val methods(Val o, const char *name, Val *a, int na){
  if(o.t == T_STR){
    char *s = CSTR(o); int len = SP(o)->len;
    if(!strcmp(name, "toUpperCase") || !strcmp(name, "toLowerCase")){
      Val r = Vsn(s, len); for(int i = 0; i < len; i++) CSTR(r)[i] = name[2] == 'U' ? toupper((unsigned char)s[i]) : tolower((unsigned char)s[i]); return r; }
    if(!strcmp(name, "trim")){ char *t = strip_n(s, len); Val r = Vs(t); free(t); return r; }
    if(!strcmp(name, "includes")){ if(!na || a[0].t != T_STR) lerr("includes needs a string"); return Vb(strstr(s, CSTR(a[0])) != NULL); }
    if(!strcmp(name, "indexOf")){ if(!na || a[0].t != T_STR) lerr("indexOf needs a string"); char *p = strstr(s, CSTR(a[0])); return Vn(p ? p - s : -1); }
    if(!strcmp(name, "charCodeAt")){ ll i = na ? toint(a[0]) : 0; if(i < 0 || i >= len) lerr("Index out of range"); return Vn((unsigned char)s[i]); }
    if(!strcmp(name, "slice")){ int st, en; slice_idx(len, a, na, &st, &en); return Vsn(s + st, en - st); }
  } else if(o.t == T_LIST){
    List *l = LP(o);
    if(!strcmp(name, "includes") || !strcmp(name, "indexOf")){
      if(!na) lerr("%s needs an argument", name);
      int ix = -1; for(int i = 0; i < l->n; i++) if(veq(l->a[i], a[0])){ ix = i; break; }
      return name[0] == 'i' && name[1] == 'n' && name[2] == 'c' ? Vb(ix >= 0) : Vn(ix);
    }
    if(!strcmp(name, "slice")){ int st, en; slice_idx(l->n, a, na, &st, &en); Val r = Vl(); for(int i = st; i < en; i++) lpush(LP(r), vref(l->a[i])); return r; }
    if(!strcmp(name, "join")){
      char *sep = na ? jstr(a[0]) : xs(","); Buf b = {0};
      for(int i = 0; i < l->n; i++){ if(i) bs(&b, sep); jbuf(&b, l->a[i]); }
      free(sep); char *s = btake(&b); Val r = Vsn(s, b.n); free(s); return r;
    }
  }
  if(!strcmp(name, "toString")){
    /* v2.1: n.toString(radix) for whole numbers, like the simulator (JavaScript): 39.toString(16) = "27". Used by the official i2c_scan example. */
    if(o.t == T_NUM && na >= 1){
      int rx = (int)numarg(a[0]); double d = o.u.n;
      if(rx >= 2 && rx <= 36 && rx != 10 && d == floor(d) && fabs(d) < 9.2e18){
        long long v = (long long)d; int neg = v < 0; unsigned long long u = neg ? 0ULL - (unsigned long long)v : (unsigned long long)v;
        char tmp[80]; int n = 0; do { int dg = (int)(u % (unsigned)rx); tmp[n++] = (char)(dg < 10 ? '0' + dg : 'a' + dg - 10); u /= (unsigned)rx; } while(u);
        if(neg) tmp[n++] = '-';
        for(int i = 0; i < n / 2; i++){ char c = tmp[i]; tmp[i] = tmp[n - 1 - i]; tmp[n - 1 - i] = c; }
        return Vsn(tmp, n);
      }
    }
    char *s = jstr(o); Val r = Vs(s); free(s); return r;
  }
  lerr("Unknown method: %s", name);
}
static Val eval(Node *n, Scope *sc){
  switch(n->k){
  case 'n': return Vn(n->n);
  case 's': return Vs(n->s);
  case 'i': {
    Val *p = slookup(sc, n->s, NULL, NULL); if(p) return vref(*p);
    if(!strcmp(n->s, "true")) return Vb(1); if(!strcmp(n->s, "false")) return Vb(0);
    if(!strcmp(n->s, "null") || !strcmp(n->s, "undefined")) return Vu();
    if(!strcmp(n->s, "Infinity")) return Vn(INFINITY); if(!strcmp(n->s, "NaN")) return Vn(NAN);
    if(!strcmp(n->s, "Math")) return vref(MATHV);
    if(!strcmp(n->s, "t")) return Vn(elapsed_ms());
    Val *m = dget(DP(MATHV), n->s); if(m) return vref(*m);
    lerr("Undeclared variable: %s", n->s);
  }
  case 'A': { Val l = Vl(); for(int i = 0; i < n->na; i++) lpush(LP(l), eval(n->args[i], sc)); return l; }
  case '?': { Val c = eval(n->a, sc); int t = truthy(c); vdrop(c); return eval(t ? n->b : n->c, sc); }
  case 'u': {
    Val v = eval(n->a, sc), r;
    if(n->op == '!') r = Vb(!truthy(v));
    else if(n->op == '~') r = Vn((double)i32v(~toint(v)));
    else { if(!isnumv(v)){ vdrop(v); lerr("Invalid operand for %c", n->op); } r = Vn(n->op == '-' ? -v.u.n : v.u.n); }
    vdrop(v); return r;
  }
  case 'B': {
    int op = n->op;
    if(op == O_AND || op == O_OR || op == O_NC){
      Val a = eval(n->a, sc); int go = op == O_AND ? truthy(a) : op == O_OR ? !truthy(a) : a.t == T_UND;
      if(go){ vdrop(a); return eval(n->b, sc); } return a;
    }
    Val a = eval(n->a, sc), b = eval(n->b, sc);
    Val r; jmp_buf jb, *old = T->jb; T->jb = &jb;
    if(setjmp(jb)){ T->jb = old; vdrop(a); vdrop(b); longjmp(*old, 1); }
    r = binop(op, a, b); T->jb = old; vdrop(a); vdrop(b); return r;
  }
  case '.': {
    Val o = eval(n->a, sc), r = Vu();
    if(!strcmp(n->s, "length") && (o.t == T_LIST || o.t == T_STR)) r = Vn(o.t == T_LIST ? LP(o)->n : SP(o)->len);
    else if(o.t == T_DICT){ Val *p = dget(DP(o), n->s); if(p) r = vref(*p); }
    vdrop(o); return r;
  }
  case '[': {
    Val o = eval(n->a, sc), i = eval(n->b, sc), r = Vu();
    if(o.t == T_DICT){ char *k = jstr(i); Val *p = dget(DP(o), k); if(p) r = vref(*p); free(k); }
    else if(o.t == T_LIST || o.t == T_STR){
      ll ix = toint(i); int len = o.t == T_LIST ? LP(o)->n : SP(o)->len;
      if(ix >= 0 && ix < len) r = o.t == T_LIST ? vref(LP(o)->a[ix]) : Vsn(CSTR(o) + ix, 1);
    }
    vdrop(o); vdrop(i); return r;
  }
  case 'C': {
    Val av[n->na ? n->na : 1];
    for(int i = 0; i < n->na; i++) av[i] = Vu();
    jmp_buf jb, *old = T->jb; T->jb = &jb; int ne = 0; Val r = Vu();
    if(setjmp(jb)){ T->jb = old; for(int i = 0; i < n->na; i++) vdrop(av[i]); longjmp(*old, 1); }
    for(int i = 0; i < n->na; i++){ av[i] = eval(n->args[i], sc); ne++; }
    Node *f = n->a;
    if(f->k == '.'){
      Val o = eval(f->a, sc); Val *m = o.t == T_DICT ? dget(DP(o), f->s) : NULL;
      if(m && m->t == T_FN) r = callfn((int)m->u.n, av, n->na);
      else { Val *keep = NULL; (void)keep; r = methods(o, f->s, av, n->na); }
      vdrop(o);
    } else {
      Val fv = eval(f, sc); if(fv.t != T_FN){ vdrop(fv); lerr("Not a function"); }
      r = callfn((int)fv.u.n, av, n->na);
    }
    T->jb = old; for(int i = 0; i < n->na; i++) vdrop(av[i]); (void)ne; return r;
  }
  }
  lerr("Invalid expression");
}

/* ---------------------------------------------------------------- sentencias */
#define KWLIST X(use) X(print) X(var) X(set) X(repeat) X(for) X(if) X(wait) X(screen) X(display) X(reg) X(pin) X(on) X(loop) X(setup) X(expect) X(sound) X(serial) X(else) X(while) X(break) X(continue) X(input) X(ui) X(store) X(g3d) X(push) X(pop) X(remove) X(insert) X(sort) X(shuffle) X(split) X(each) X(def) X(call) X(return) X(elseif) X(console) X(mem) X(disk) X(exec) X(panic) X(stop) X(task) X(spawn) X(kill) X(join) X(import) X(try) X(lock) X(unlock) X(send) X(recv) X(yield) X(assert) X(sys) X(proc) X(surface) X(host) X(port) X(graphics3d) X(dyn2d) X(ram) X(cpu) X(irq) X(pci) X(dev) X(admin) X(catch) X(switch) X(case) X(default) X(i2c) X(spi) X(uart) X(watchdog)
enum {
#define X(n) K_##n,
KWLIST
#undef X
K_COUNT };
static const char *KWN[] = {
#define X(n) #n,
KWLIST
#undef X
};
static int kwid(const char *l){ for(int i = 0; i < K_COUNT; i++) if(!strcmp(KWN[i], l)) return i; return -1; }

typedef struct { Stmt **v; int n, cap; } SL;
struct Stmt {
  int n, lg, kw, bad, has_alt, elif, has_body;
  const char *why;
  SV h, a, q; SV *o; char **ov; int no;     /* o[i]: segmentos de q[i]; ov[i]: valor unido tras la clave */
  char *els, *cv;
  SL body, alt, cat;
};
static void slp(SL *l, Stmt *s){ if(l->n == l->cap){ l->cap = l->cap ? l->cap * 2 : 8; l->v = xr(l->v, l->cap * sizeof(Stmt *)); } l->v[l->n++] = s; }

typedef struct { int n; char *m; } Diag;
typedef struct { Diag *v; int n, cap; } DV;
static void dpush(DV *d, int n, const char *m){ if(d->n == d->cap){ d->cap = d->cap ? d->cap * 2 : 8; d->v = xr(d->v, d->cap * sizeof(Diag)); } d->v[d->n].n = n; d->v[d->n].m = xs(m); d->n++; }
static void dpushf(DV *d, int n, const char *f, ...){ char t[512]; va_list a; va_start(a, f); vsnprintf(t, sizeof t, f, a); va_end(a); dpush(d, n, t); }
static int dcmp(const void *a, const void *b){ const Diag *x = a, *y = b; return x->n != y->n ? x->n - y->n : strcmp(x->m, y->m); }

/* devuelve lista de grupos entre parentesis; 0 si esta mal formado */
static int grp(const char *s, SV *g){
  int d = 0, b = 0; size_t i = 0, len = strlen(s);
  while(i < len){
    char c = s[i];
    if(c == '"'){
      if(!d) return 0;
      const char *j = strchr(s + i + 1, '"'); if(!j) return 0; i = j - s;
    } else if(c == '('){ if(d == 0) b = (int)i + 1; d++; }
    else if(c == ')'){ d--; if(d < 0) return 0; if(d == 0) svp(g, xsn(s + b, i - b)); }
    else if(d == 0 && c != ' ') return 0;
    i++;
  }
  return d == 0;
}
static void sp_split(const char *s, SV *a){
  int d = 0, q = 0; size_t b = 0, len = strlen(s);
  for(size_t i = 0; i <= len; i++){
    int c = i < len ? s[i] : -1;
    if(c == '"') q = !q;
    if(!q && c != -1){ if(c == '(' || c == '[') d++; if(c == ')' || c == ']') d--; }
    if(i == len || (c == ',' && d == 0 && !q)){ svp(a, strip_n(s + b, i - b)); b = i + 1; }
  }
}
static Stmt *stm(const char *tin, int n, int lg){
  Stmt *x = calloc(1, sizeof *x); x->n = n; x->lg = lg; x->kw = -1;
  char *t = xs(tin); char *ei = strstr(t, " Else:");
  if(ei && ei > t){ x->els = xs(ei + 6); *ei = 0; }
  size_t hl = 0;
  if(isalpha((unsigned char)t[0]) || t[0] == '_'){ hl = 1; while(isalnum((unsigned char)t[hl]) || t[hl] == '_' || t[hl] == ':' || (unsigned char)t[hl] >= 0x80) hl++; }
  if(!hl){ x->bad = 1; x->why = "The line must start with a command name"; svp(&x->h, xs("?")); return x; }
  for(size_t i = 0, b = 0; i <= hl; i++) if(i == hl || t[i] == ':'){ svp(&x->h, xsn(t + b, i - b)); b = i + 1; }
  char *rest = strip_dup(t + hl); SV g = {0};
  if(!grp(rest, &g)){ x->bad = 1; x->why = "Unclosed parenthesis or quotes"; return x; }
  for(int i = 0; i < g.n; i++){
    char *s = g.v[i];
    if(strchr(s, '"')){
      for(char *p = s; (p = strchr(p, '"')); ){ char *e = strchr(p + 1, '"'); if(!e) break; svp(&x->q, xsn(p + 1, e - p - 1)); p = e + 1; }
    } else { x->a.n = 0; char *sc = strip_dup(s); if(*sc) sp_split(s, &x->a); free(sc); }
  }
  x->no = x->q.n; x->o = calloc(x->no ? x->no : 1, sizeof(SV)); x->ov = xm((x->no ? x->no : 1) * sizeof(char *));
  for(int i = 0; i < x->no; i++){
    const char *q = x->q.v[i]; size_t b = 0, L = strlen(q);
    for(size_t j = 0; j <= L; j++) if(j == L || q[j] == ';'){ svp(&x->o[i], strip_n(q + b, j - b)); b = j + 1; }
    Buf r = {0}; for(int j = 1; j < x->o[i].n; j++){ if(j > 1) bc(&r, ';'); bs(&r, x->o[i].v[j]); }
    x->ov[i] = btake(&r);
  }
  char *lo = lowerdup(x->h.v[0]); x->kw = kwid(lo); free(lo);
  return x;
}
/* opcion Key;valor -> valor (o NULL si no existe) */
static const char *og(Stmt *x, const char *k){
  for(int i = 0; i < x->no; i++) if(!strcasecmp(x->o[i].v[0], k)) return x->ov[i];
  return NULL;
}

typedef struct { int n; char *t; int o; } Line;
typedef struct { Line *L; int nL, pos, warned; const char *fl; DV *E, *W; } PC;
static int lgcode(const char *fl){ return !strcmp(fl, "log-") ? 0 : !strcmp(fl, "log+") ? 1 : !strcmp(fl, "log++") ? 2 : -1; }

static void blk(PC *c, int lg, int ol, SL *o){
  while(c->pos < c->nL){
    Line *x = &c->L[c->pos++];
    if(!strcmp(x->t, "}")){
      if(ol < 0){ dpush(c->E, x->n, "Closing brace } without a matching {"); continue; }
      return;
    }
    const char *t = x->t; int l = lg, tagged = 0;
    if(t[0] == '#'){
      int tg = -2; const char *r = NULL;
      if(t[1] == '+' && t[2] == '+'){ tg = 2; r = t + 3; } else if(t[1] == '+'){ tg = 1; r = t + 2; } else if(t[1] == '-'){ tg = 0; r = t + 2; }
      if(tg != -2){ l = tg; t = r; while(isspace((unsigned char)*t)) t++; tagged = 1; }
    }
    if(!tagged && l < 0){
      l = 1;
      if(!strcmp(c->fl, "logall") && !c->warned){ c->warned = 1; dpush(c->W, x->n, "Line without a #-, #+ or #++ tag: assuming #+"); }
    }
    Stmt *s = stm(t, x->n, l);
    if(x->o){ s->has_body = 1; blk(c, l, x->n, &s->body); }
    char *hn = lowerdup(s->h.v[0]);
    Stmt *q = o->n ? o->v[o->n - 1] : NULL;
    if(!strcmp(hn, "catch")){
      if(q && q->kw == K_try && !q->cv){
        q->cat = s->body; q->cv = s->h.n > 1 ? xs(s->h.v[1]) : xs("");
      } else dpush(c->E, x->n, "Catch without a Try right before it");
      free(hn); continue;
    }
    if(!strcmp(hn, "else") || !strcmp(hn, "elseif")){
      int isel = !strcmp(hn, "else");
      while(q && q->has_alt && q->alt.n == 1 && q->alt.v[0]->elif) q = q->alt.v[0];
      if(q && q->kw == K_if && !q->has_alt){
        if(isel){ q->alt = s->body; q->has_alt = 1; }
        else { s->h.v[0] = xs("If"); s->kw = K_if; s->elif = 1; slp(&q->alt, s); q->has_alt = 1; }
      } else dpush(c->E, x->n, isel ? "Else without an If right before it" : "ElseIf without an If right before it");
      free(hn); continue;
    }
    free(hn); slp(o, s);
  }
  if(ol >= 0) dpush(c->E, ol, "This brace { is never closed");
}

/* v2.1: "x += e" (also -= *= /=) -> Set:x("Value;x+(e)"). Only lines that used to be syntax errors match, so old programs are unaffected.
 * Returns a new string, or NULL if the line is not of that form (a line with a quote or ';' in the expression is left alone). */
static char *desugar_assign(const char *t){
  const char *p = t, *pre = "";
  if(p[0] == '#' && (p[1] == '+' || p[1] == '-')){ const char *q = p + 2; if(p[1] == '+' && *q == '+') q++; pre = p; p = q; while(*p == ' ' || *p == '\t') p++; }
  if(!(isalpha((unsigned char)*p) || *p == '_')) return NULL;
  const char *id = p; while(isalnum((unsigned char)*p) || *p == '_') p++;
  size_t idl = (size_t)(p - id);
  while(*p == ' ' || *p == '\t') p++;
  char op = *p; if(op != '+' && op != '-' && op != '*' && op != '/') return NULL;
  if(p[1] != '=' || p[2] == '=') return NULL;
  const char *e = p + 2; while(*e == ' ' || *e == '\t') e++;
  if(!*e || strchr(e, '"') || strchr(e, ';')) return NULL;
  size_t prel = (size_t)(id - pre); if(!*pre) prel = 0;
  Buf b = {0}; if(prel) bcat(&b, pre, (int)prel);
  bs(&b, "Set:"); bcat(&b, id, (int)idl); bs(&b, "(\"Value;"); bcat(&b, id, (int)idl); bc(&b, op); bc(&b, '('); bs(&b, e); bs(&b, ")\")");
  return btake(&b);
}

/* parse: devuelve 1 si hay cabecera valida (nodes relleno) */
static int parse(const char *src, const char *name, DV *E, DV *W, SL *nodes){
  PC c = {0}; c.E = E; c.W = W;
  /* Windows editors (Notepad, PowerShell) may add a BOM or save as UTF-16 */
  if((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) src += 3;
  if(((unsigned char)src[0] == 0xFF && (unsigned char)src[1] == 0xFE) || ((unsigned char)src[0] == 0xFE && (unsigned char)src[1] == 0xFF) || (src[0] && !src[1])){
    dpush(E, 1, "File is saved as UTF-16: save it as UTF-8 (Notepad: Save As -> Encoding: UTF-8)"); return 0;
  }
  int n = 0; const char *p = src; Line *L = NULL; int cap = 0;
  /* ignora una primera linea tipo "#!/usr/bin/env logl" */
  if(src[0] == '#' && src[1] == '!'){ const char *e0 = strchr(src, '\n'); n = 1; p = e0 ? e0 + 1 : src + strlen(src); }
  for(;;){
    const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
    n++;
    char *raw = xsn(p, len); char *cm = strstr(raw, "//"); if(cm) *cm = 0;
    char *t = strip_dup(raw); free(raw);
    { char *ds = desugar_assign(t); if(ds){ free(t); t = ds; } }
    if(!*t) free(t);
    else if(!strcmp(t, "{")){ if(c.nL) c.L[c.nL - 1].o = 1; free(t); }
    else {
      int o = 0, tl = (int)strlen(t);
      if(tl > 1 && t[tl - 1] == '{'){ t[tl - 1] = 0; char *u = strip_dup(t); free(t); t = u; o = 1; }
      if(c.nL == cap){ cap = cap ? cap * 2 : 64; c.L = xr(c.L, cap * sizeof(Line)); }
      c.L[c.nL].n = n; c.L[c.nL].t = t; c.L[c.nL].o = o; c.nL++;
    }
    if(!e) break; p = e + 1;
  }
  (void)L;
  if(!c.nL){ dpush(E, 1, "Missing language header, for example <log+>"); return 0; }
  Line h = c.L[0]; c.pos = 1;
  char *ht = lowerdup(h.t); int ok = 0; char *fl = NULL;
  if(ht[0] == '<'){
    size_t l = strlen(ht);
    if(ht[l - 1] == '>'){ char *in = strip_n(ht + 1, l - 2);
      if(!strcmp(in, "log++") || !strcmp(in, "log+-") || !strcmp(in, "log+") || !strcmp(in, "log-") || !strcmp(in, "logall")){ ok = 1; fl = in; } else free(in); }
  }
  free(ht);
  if(!ok){ dpush(E, h.n, "Missing language header, for example <log+>"); return 0; }
  c.fl = fl;
  const char *dot = strrchr(name, '.'); char *ext = lowerdup(dot ? dot + 1 : name);
  if(strcmp(ext, fl)) dpushf(W, h.n, "Header <%s> does not match the file extension .%s", fl, ext);
  free(ext);
  blk(&c, lgcode(fl), -1, nodes);
  return 1;
}

static int hasop(Stmt *x, const char **list);
static int has_show(SL *ns){ for(int i = 0; i < ns->n; i++){ Stmt *x = ns->v[i]; if((x->kw == K_screen || x->kw == K_display || x->kw == K_reg) && hasop(x, (const char *[]){"show", NULL})) return 1; if(has_show(&x->body) || has_show(&x->alt) || has_show(&x->cat)) return 1; } return 0; }
static int has_wait(SL *ns){ for(int i = 0; i < ns->n; i++) if(ns->v[i]->kw == K_wait || has_wait(&ns->v[i]->body)) return 1; return 0; }

static double lcs_ratio(const char *a, int la, const char *b, int lb){
  /* Ratcliff/Obershelp como difflib */
  if(!la || !lb) return 0;
  int best = 0, ia = 0, ib = 0;
  for(int i = 0; i < la; i++) for(int j = 0; j < lb; j++){ int k = 0; while(i + k < la && j + k < lb && a[i+k] == b[j+k]) k++; if(k > best){ best = k; ia = i; ib = j; } }
  if(!best) return 0;
  return best + lcs_ratio(a, ia, b, ib) + lcs_ratio(a + ia + best, la - ia - best, b + ib + best, lb - ib - best);
}
static const char *HWOPS[] = {"map", "unmap", "read", "write", "set", "clear", "toggle", NULL};
static const char *SCROPS[] = {"resolution", "clear", "pixel", "rect", "circle", "line", "text", "get", "show", "blit", NULL};
typedef struct { int res, ids_done; SV ids; } ChkSt;
static int hasop(Stmt *x, const char **list){
  for(int i = 0; i < x->h.n; i++) for(int j = 0; list[j]; j++) if(!strcasecmp(x->h.v[i], list[j])) return j + 1;
  return 0;
}
static void collect_ids(SL *ns, SV *ids){
  for(int i = 0; i < ns->n; i++){
    Stmt *x = ns->v[i];
    if(x->kw == K_ui && !x->bad){ const char *id = og(x, "id"); if(id && *id) svp(ids, xs(id)); }
    collect_ids(&x->body, ids); collect_ids(&x->alt, ids); collect_ids(&x->cat, ids);
  }
}
static void chk_opts(Stmt *x, DV *W, const char *name, const char **valid){
  for(int i = 0; i < x->no; i++){
    const char *a = x->o[i].v[0]; int ok = 0; for(int j = 0; valid[j]; j++) if(!strcasecmp(a, valid[j])) ok = 1;
    if(!ok){ Buf b = {0}; for(int j = 0; valid[j]; j++){ if(j) bs(&b, ", "); bs(&b, valid[j]); } dpushf(W, x->n, "Unknown command \"%s\" in %s\n  -> Valid: %s", a, name, btake(&b)); }
  }
}
static void chk(SL *ns, DV *E, DV *W, ChkSt *s){
  if(!s->ids_done){ s->ids_done = 1; collect_ids(ns, &s->ids); }
  for(int i = 0; i < ns->n; i++){
    Stmt *x = ns->v[i]; int k = x->kw, l = x->lg, n = x->n;
    if(x->bad){ dpushf(E, n, "Syntax error: %s", x->why); continue; }
    if(k < 0){
      char *lo = lowerdup(x->h.v[0]); int bi = -1; double br = 0.6;
      for(int j = 0; j < K_COUNT; j++){ double r = 2.0 * lcs_ratio(lo, (int)strlen(lo), KWN[j], (int)strlen(KWN[j])) / (strlen(lo) + strlen(KWN[j])); if(r >= br){ br = r; bi = j; } }
      if(bi >= 0) dpushf(E, n, "Unknown command: %s (did you mean %s?)", x->h.v[0], KWN[bi]);
      else dpushf(E, n, "Unknown command: %s", x->h.v[0]);
      free(lo); continue;
    }
    if(k == K_reg && l != 0) dpush(E, n, "Reg: is exclusive to Log-");
    if(k == K_port && l != 0) dpush(E, n, "Port: is exclusive to Log-");
    if((k == K_ram || k == K_cpu || k == K_irq || k == K_pci || k == K_dev || k == K_admin) && l != 0) dpushf(E, n, "%s: is exclusive to Log-", x->h.v[0]);
    if(k == K_screen || k == K_display || k == K_reg){
      if(k == K_reg && hasop(x, HWOPS)){ /* acceso a registros reales: sin chequeos de pantalla */ }
      else {
        int p = hasop(x, SCROPS);
        if(!p) dpush(E, n, "Unknown screen operation");
        else if(l == 2) dpush(E, n, "Log++ has no screen: use Log+ or Log-");
        else if(p == 1) s->res = 1;
        else if(l == 0 && !s->res) dpush(E, n, "Log- requires Resolution before drawing");
      }
    }
    if(k == K_var){
      const char *t = x->h.n > 2 ? x->h.v[1] : NULL;
      if(l == 0 && !t) dpush(E, n, "Log- requires a type: Var:U8:name(\"Value;0\")");
      if(t && typidx(t) < 0) dpushf(E, n, "Unknown type: %s", x->h.v[1]);
      if(t && l == 1) dpushf(W, n, "Log+ infers the type: you can remove %s", x->h.v[1]);
    }
    if(k == K_g3d || k == K_graphics3d){
      static const char *G3OPS[] = {"cube", "sphere", "torus", "pyramid", "cylinder", "cone", "plane", "diamond", "object", "set", "remove", "info", "list", "meshes", "load", "save", "pick", "project", "render", "clear", "scene", "camera", "orbit", "light", NULL};
      static const char *G3OPT[] = {"id", "x", "y", "z", "size", "color", "rx", "ry", "rz", "s", "sx", "sy", "sz", "visible", "mesh", "name", "file", "fit", "twosided", "into", "tx", "ty", "tz", "fov", "mode", "bg", "ambient", "grid", "yaw", "pitch", "dist", "dyaw", "dpitch", "zoom", NULL};
      const char *op = x->h.n > 1 ? x->h.v[1] : ""; int okop = 0; for(int j = 0; G3OPS[j]; j++) if(!strcasecmp(op, G3OPS[j])) okop = 1;
      if(l == 2) dpush(E, n, "Log++ has no screen: G3D cannot be used here\n  -> Use Log+ or Log-");
      else if(l == 0 && !s->res) dpush(E, n, "Log- requires Resolution before using G3D\n  -> Add Screen:Resolution(...) above");
      if(!okop) dpushf(E, n, "Unknown G3D operation: %s\n  -> Valid: cube, sphere, torus, pyramid, cylinder, cone, plane, diamond, object, set, remove, info, list, meshes, load, save, pick, project, render, clear, scene, camera, orbit, light", op);
      chk_opts(x, W, x->h.v[0], G3OPT);
    }
    if(k == K_ui){
      static const char *UIOPT[] = {"id", "label", "text", "min", "max", "value", "options", "rows", "cols", "syntax", "goto", "what", "into", NULL};
      if(l != 1) dpush(E, n, "UIVisuals only works in Log+ and in #+ lines of mixed files\n  -> Change the header to <log+> or tag the line with #+");
      chk_opts(x, W, "UI", UIOPT);
    }
    if(k == K_dyn2d && l == 2) dpush(E, n, "Log++ has no screen: Dyn2D cannot be used here\n  -> Use Log+ or Log-");
    if(k == K_on){
      const char *m = x->h.n > 1 ? x->h.v[1] : ""; int ok = !strcasecmp(m, "start_button") || !strcasecmp(m, "key") || !strcasecmp(m, "canvas") || !strcasecmp(m, "timer") || !strcasecmp(m, "mouse") || !strcasecmp(m, "pin");
      if(!strcasecmp(m, "pin")){   /* v2.1: On:Pin(N)("Rising"|"Falling"|"Change") */
        if(x->a.n < 1) dpush(E, n, "On:Pin needs a pin number: On:Pin(5)(\"Rising\")");
        else if(x->no){ const char *e0 = x->o[0].v[0]; if(strcasecmp(e0, "rising") && strcasecmp(e0, "falling") && strcasecmp(e0, "change")) dpushf(E, n, "On:Pin: use Rising, Falling or Change (not %s)", e0); }
      }
      for(int j = 0; j < s->ids.n && !ok; j++) if(!strcasecmp(s->ids.v[j], m)) ok = 1;
      if(!ok) dpushf(W, n, "On:%s: that event does not exist, nor a widget with that Id\n  -> Events: START_BUTTON or the Id of a UI widget", m);
    }
    if(k == K_case || k == K_default){ dpushf(E, n, "%s is only valid directly inside a Switch", x->h.v[0]); continue; }
    if(k == K_switch){
      if(x->a.n < 1) dpush(E, n, "Switch needs an expression: Switch(expr)");
      if(!x->has_body) dpush(E, n, "Switch needs a block { ... } with Case(...) and Default");
      int nd = 0;
      for(int j = 0; j < x->body.n; j++){
        Stmt *c = x->body.v[j];
        if(c->bad){ dpushf(E, c->n, "Syntax error: %s", c->why); continue; }
        if(c->kw == K_case){ if(c->a.n < 1) dpush(E, c->n, "Case needs at least one value: Case(1) or Case(1,2)"); }
        else if(c->kw == K_default){ if(++nd > 1) dpush(E, c->n, "Switch has more than one Default"); }
        else { dpush(E, c->n, "Only Case(...) and Default are allowed directly inside a Switch"); continue; }
        chk(&c->body, E, W, s);
      }
      continue;
    }
    if(k == K_i2c || k == K_spi || k == K_uart){   /* v2.1: unknown operation = compile error, like the simulator */
      const char *op = x->h.n > 1 ? x->h.v[1] : "", *okl = k == K_i2c ? "write read scan close" : k == K_spi ? "open transfer write read close" : "open write read close";
      int found = 0; size_t ol = strlen(op); const char *q = okl;
      while(*q){ const char *e = strchr(q, ' '); size_t l2 = e ? (size_t)(e - q) : strlen(q); if(l2 == ol && !strncasecmp(q, op, l2)) found = 1; q += l2; if(*q == ' ') q++; }
      if(!found) dpushf(E, n, "Unknown %s operation: %s (valid: %s)", x->h.v[0], op, okl);
    }
    if(k == K_expect && l != 0) dpush(E, n, "Expect is exclusive to Log-");
    if(k == K_loop && (x->h.n == 1 || !strcasecmp(x->h.v[1], "forever")) && !has_wait(&x->body))
      dpush(W, n, "Loop without Wait: the CPU will sit at 100%");
    chk(&x->body, E, W, s); chk(&x->alt, E, W, s); chk(&x->cat, E, W, s);
  }
}

/* ---------------------------------------------------------------- framebuffer */
typedef struct { unsigned char r, g, b; } Col;
typedef struct { int w, h; unsigned char *d; } FB;
static const struct { char c; const char *g; } FONT3[] = {
 {'A',".#. #.# ### #.# #.#"},{'B',"##. #.# ##. #.# ##."},{'C',".## #.. #.. #.. .##"},{'D',"##. #.# #.# #.# ##."},
 {'E',"### #.. ##. #.. ###"},{'F',"### #.. ##. #.. #.."},{'G',".## #.. #.# #.# .##"},{'H',"#.# #.# ### #.# #.#"},
 {'I',"### .#. .#. .#. ###"},{'J',"..# ..# ..# #.# .#."},{'K',"#.# #.# ##. #.# #.#"},{'L',"#.. #.. #.. #.. ###"},
 {'M',"#.# ### ### #.# #.#"},{'N',"##. #.# #.# #.# #.#"},{'O',".#. #.# #.# #.# .#."},{'P',"##. #.# ##. #.. #.."},
 {'Q',".#. #.# #.# ##. .##"},{'R',"##. #.# ##. #.# #.#"},{'S',".## #.. .#. ..# ##."},{'T',"### .#. .#. .#. .#."},
 {'U',"#.# #.# #.# #.# ###"},{'V',"#.# #.# #.# #.# .#."},{'W',"#.# #.# ### ### #.#"},{'X',"#.# #.# .#. #.# #.#"},
 {'Y',"#.# #.# .#. .#. .#."},{'Z',"### ..# .#. #.. ###"},
 {'0',"### #.# #.# #.# ###"},{'1',".#. ##. .#. .#. ###"},{'2',"##. ..# .#. #.. ###"},{'3',"##. ..# .#. ..# ##."},
 {'4',"#.# #.# ### ..# ..#"},{'5',"### #.. ##. ..# ##."},{'6',".## #.. ### #.# ###"},{'7',"### ..# .#. .#. .#."},
 {'8',"### #.# ### #.# ###"},{'9',"### #.# ### ..# ##."},
 {'.',"... ... ... ... .#."},{',',"... ... ... .#. #.."},{':',"... .#. ... .#. ..."},{';',"... .#. ... .#. #.."},
 {'!',".#. .#. .#. ... .#."},{'?',"##. ..# .#. ... .#."},{'-',"... ... ### ... ..."},{'+',"... .#. ### .#. ..."},
 {'=',"... ### ... ### ..."},{'*',"#.# .#. ### .#. #.#"},{'/',"..# ..# .#. #.. #.."},{'(',".## #.. #.. #.. .##"},
 {')',"##. ..# ..# ..# ##."},{'<',"..# .#. #.. .#. ..#"},{'>',"#.. .#. ..# .#. #.."},{'_',"... ... ... ... ###"},
 {'\'',".#. .#. ... ... ..."},{'"',"#.# #.# ... ... ..."},{'%',"#.# ..# .#. #.. #.#"},{'#',"#.# ### #.# ### #.#"},
 {'[',"##. #.. #.. #.. ##."},{']',".## ..# ..# ..# .##"},{' ',"... ... ... ... ..."},{0,NULL}};

static FB *fb_new(double w, double h){
  FB *f = xm(sizeof *f); ll W = dint(w), H = dint(h);
  f->w = W < 1 ? 1 : W > 1920 ? 1920 : (int)W; f->h = H < 1 ? 1 : H > 1080 ? 1080 : (int)H;
  f->d = calloc((size_t)f->w * f->h * 3, 1); return f;
}
static void fb_put(FB *f, ll x, ll y, Col c){ if(x >= 0 && x < f->w && y >= 0 && y < f->h){ unsigned char *p = f->d + ((size_t)y * f->w + x) * 3; p[0] = c.r; p[1] = c.g; p[2] = c.b; } }
static Col fb_get(FB *f, ll x, ll y){ Col c = {0, 0, 0}; if(x >= 0 && x < f->w && y >= 0 && y < f->h){ unsigned char *p = f->d + ((size_t)y * f->w + x) * 3; c.r = p[0]; c.g = p[1]; c.b = p[2]; } return c; }
static void fb_rect(FB *f, ll x, ll y, ll w, ll h, Col c){
  for(ll yy = y < 0 ? 0 : y; yy < y + h && yy < f->h; yy++) for(ll xx = x < 0 ? 0 : x; xx < x + w && xx < f->w; xx++) fb_put(f, xx, yy, c);
}
static void fb_line(FB *f, ll x0, ll y0, ll x1, ll y1, Col c){
  ll dx = llabs(x1 - x0), dy = -llabs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for(;;){
    fb_put(f, x0, y0, c); if(x0 == x1 && y0 == y1) break;
    ll e2 = 2 * e; if(e2 >= dy){ e += dy; x0 += sx; } if(e2 <= dx){ e += dx; y0 += sy; }
  }
}
static void fb_circle(FB *f, ll cx, ll cy, ll r, Col c, int fill){
  for(ll dy = -r; dy <= r; dy++) for(ll dx = -r; dx <= r; dx++){
    ll d = dx * dx + dy * dy;
    if(fill ? d <= r * r : (r * r - r <= d && d <= r * r + r)) fb_put(f, cx + dx, cy + dy, c);
  }
}
static void fb_text(FB *f, ll x, ll y, const char *t, Col c, ll sc){
  ll x0 = x; if(sc < 1) sc = 1;
  for(const unsigned char *p = (const unsigned char *)t; *p; p++){
    if((*p & 0xC0) == 0x80) continue;
    if(*p == '\n'){ x = x0; y += 6 * sc; continue; }
    int ch = toupper(*p);
    for(int i = 0; FONT3[i].g; i++) if(FONT3[i].c == ch){
      const char *g = FONT3[i].g;
      for(int r = 0; r < 5; r++) for(int q = 0; q < 3; q++) if(g[r * 4 + q] == '#') fb_rect(f, x + q * sc, y + r * sc, sc, sc, c);
      break;
    }
    x += 4 * sc;
  }
}
static void fb_blit(FB *d, FB *src, ll x, ll y, ll w, ll h){
  if(w < 0) w = src->w; if(h < 0) h = src->h;
  unsigned char *sd = xm((size_t)src->w * src->h * 3); memcpy(sd, src->d, (size_t)src->w * src->h * 3);
  for(ll yy = 0; yy < h; yy++) for(ll xx = 0; xx < w; xx++){
    size_t i = (((size_t)(yy * src->h / (h ? h : 1))) * src->w + (size_t)(xx * src->w / (w ? w : 1))) * 3;
    Col c = {sd[i], sd[i+1], sd[i+2]}; fb_put(d, x + xx, y + yy, c);
  }
  free(sd);
}
static int term_cols(void){
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO ci;
  if(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci)) return ci.srWindow.Right - ci.srWindow.Left + 1;
  return 80;
#else
  struct winsize ws; if(ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col; return 80;
#endif
}
static char *fb_render(FB *f, int ansi){
  int cols = term_cols() - 1; if(cols > f->w) cols = f->w; if(cols < 1) cols = 1;
  int step = (f->w + cols - 1) / cols; if(step < 1) step = 1;
  Buf b = {0};
  for(int y = 0; y < f->h; y += step * 2){
    for(int x = 0; x < f->w; x += step){
      Col t = fb_get(f, x, y), u = {0, 0, 0}; if(y + step < f->h) u = fb_get(f, x, y + step);
      if(ansi){ char s[96]; snprintf(s, sizeof s, "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xe2\x96\x80", t.r, t.g, t.b, u.r, u.g, u.b); bs(&b, s); }
      else { int v = (int)((t.r + t.g + t.b + u.r + u.g + u.b) / 6.0 * 9 / 255); bc(&b, " .:-=+*#%@"[v > 9 ? 9 : v]); }
    }
    if(ansi) bs(&b, "\x1b[0m");
    if(y + step * 2 < f->h) bc(&b, '\n');
  }
  return btake(&b);
}
/* PNG con bloques deflate "stored" (sin zlib) */
static uint32_t crc_t[256]; static int crc_ok;
static uint32_t crc32u(uint32_t c, const unsigned char *p, size_t n){
  if(!crc_ok){ for(uint32_t i = 0; i < 256; i++){ uint32_t k = i; for(int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; crc_t[i] = k; } crc_ok = 1; }
  c = ~c; for(size_t i = 0; i < n; i++) c = crc_t[(c ^ p[i]) & 255] ^ (c >> 8); return ~c;
}
static void be32(unsigned char *p, uint32_t v){ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void pchunk(FILE *f, const char *t, const unsigned char *d, uint32_t n){
  unsigned char h[8]; be32(h, n); memcpy(h + 4, t, 4); fwrite(h, 1, 8, f); if(n) fwrite(d, 1, n, f);
  uint32_t c = crc32u(0, (const unsigned char *)t, 4); c = crc32u(c, d, n); unsigned char e[4]; be32(e, c); fwrite(e, 1, 4, f);
}
static int fb_png(FB *f, const char *path){
  size_t rawn = (size_t)f->h * (f->w * 3 + 1); unsigned char *raw = xm(rawn);
  for(int y = 0; y < f->h; y++){ raw[(size_t)y * (f->w * 3 + 1)] = 0; memcpy(raw + (size_t)y * (f->w * 3 + 1) + 1, f->d + (size_t)y * f->w * 3, f->w * 3); }
  size_t nb = (rawn + 65534) / 65535, zn = 2 + rawn + nb * 5 + 4; unsigned char *z = xm(zn), *q = z;
  *q++ = 0x78; *q++ = 1; uint32_t a = 1, b = 0;
  for(size_t i = 0; i < rawn; i++){ a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
  for(size_t off = 0; off < rawn; ){ size_t n = rawn - off > 65535 ? 65535 : rawn - off; *q++ = off + n >= rawn; *q++ = n & 255; *q++ = n >> 8; *q++ = ~n & 255; *q++ = (~n >> 8) & 255; memcpy(q, raw + off, n); q += n; off += n; }
  be32(q, (b << 16) | a); q += 4;
  FILE *fp = fopen(path, "wb"); if(!fp){ free(raw); free(z); return -1; }
  fwrite("\x89PNG\r\n\x1a\n", 1, 8, fp);
  unsigned char ih[13]; be32(ih, f->w); be32(ih + 4, f->h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
  pchunk(fp, "IHDR", ih, 13); pchunk(fp, "IDAT", z, (uint32_t)(q - z)); pchunk(fp, "IEND", NULL, 0);
  fclose(fp); free(raw); free(z); return 0;
}

/* ---------------------------------------------------------------- estado global */
enum { ST_OK, ST_BRK, ST_CNT, ST_RET, ST_STOP };
struct Task { char *name; volatile int on, stop; int proc, every; SL *body; Scope *sc; };
typedef struct { char *file; Scope *sc; } Mod;
typedef struct { unsigned char *base; size_t len, maplen; char *id; int fd; void *map; } RMap;
enum { EV_KEY, EV_MOUSE, EV_CANVAS, EV_START, EV_UI, EV_PIN };
typedef struct { int kind; char sub[16]; char id[64]; SL *body; Scope *sc; } EvH;
typedef struct { int kind; char sub[16]; char id[64]; char key[64]; char val[256]; double mx, my; int btn, isnum; } Ev;
static struct {
  char *root_dir, *png, *path; char *argv0; int nowin, nobrowser, forcewin, port, nstdin; int nowait, show_end, autowin, ansi, hw, showfirst, tid, pid;
  volatile int stop; volatile sig_atomic_t gotint;
  FB *fb; Scope *root; unsigned char *MEM; size_t memsz; int allowraw, allow_disk, allow_pins, allow_net, fakegpio, npinh, evthread; int pins[64];
  Val vd; char *vdpath, *stpath; double t0;
  struct { char *id; FB *fb; } surf[64]; int nsurf;
  Val queues, locks;
  Task **tasks; int ntasks, ctasks;
  EvH evh[64]; int nevh, keythread;
  struct { int on, closed, seen, port, fw, fh; double last, bye, lastp; char token[40]; unsigned char *fd; unsigned fver, uver; char snd[256]; } win;
  int hasfg, hasbg; Col fg, bg;
  Mod mods[64]; int nmods; struct { char *alias; Mod *m; } al[64]; int nal;
  RMap rm[16]; int nrm; volatile uint32_t *gpio; int ioperm_done;
  ChkSt chkst;
} S;

static void out(const char *m){ fputs(m, stdout); fputc('\n', stdout); fflush(stdout); }
static void outn(const char *m){ fputs(m, stdout); fflush(stdout); }
static double now(void){
#ifdef _WIN32
  LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)f.QuadPart;
#else
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9;
#endif
}
static double elapsed_ms(void);
static void irq_poll(void);
static void present(void);
static void present_maybe(void);
static int win_ensure(void);
static void win_sound(double f, double ms);
static const char *esc(const char *c){ return S.ansi ? c : ""; }
/* v2.1: gil_want counts threads waiting for the GIL, so a busy loop hands the lock over when an event is waiting (low latency) */
static volatile int gil_want;
static void gil_lock(void){ __sync_fetch_and_add(&gil_want, 1); pthread_mutex_lock(&GIL); __sync_fetch_and_sub(&gil_want, 1); }
static void gil_yield(void){ pthread_mutex_unlock(&GIL); if(gil_want){ for(int i = 0; i < 400 && gil_want; i++) sched_yield(); } else sched_yield(); pthread_mutex_lock(&GIL); }
static void nap(int us){ pthread_mutex_unlock(&GIL); usleep(us); gil_lock(); }
static int chk_stop(void){ return S.stop || (T->task && T->task->stop); }
static double elapsed_ms(void){ return floor((now() - S.t0) * 1000); }
static int sleep_ms(double ms, int force){
  if(S.win.on){ if(ms > 2) present(); else present_maybe(); }
  if(S.nowait && !force) return chk_stop();
  double end = now() + ms / 1000;
  for(;;){
    if(chk_stop()) return 1;
    irq_poll();
    double r = end - now(); if(r <= 0) return 0;
    nap(r > 0.005 ? 5000 : (int)(r * 1e6) + 1);
  }
}
static char *readline_nogil(void){
  pthread_mutex_unlock(&GIL);
  char *line = NULL; size_t cap = 0; ssize_t n = getline(&line, &cap, stdin);
  pthread_mutex_lock(&GIL);
  if(n < 0){ free(line); return NULL; }
  while(n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
  return line;
}

static Val ev(const char *e){ return eval(compile(e), T->cur); }
static double evd(const char *e){ Val v = ev(e); double d = tonum(v); vdrop(v); return d; }
static ll evi(const char *e){ return dint(evd(e)); }
static char *fm(const char *t){
  Buf b = {0};
  for(const char *p = t; *p; ){
    if(*p == '{'){
      const char *e = strchr(p + 1, '}');
      if(e && e > p + 1){ char *in = xsn(p + 1, e - p - 1); Val v = ev(in); char *s = jstr(v); bs(&b, s); free(s); vdrop(v); free(in); p = e + 1; continue; }
    }
    bc(&b, *p++);
  }
  return btake(&b);
}
static const char *arg(Stmt *x, int i){ if(i >= x->a.n) lerr("Missing argument %d of %s", i + 1, x->h.v[0]); return x->a.v[i]; }
static const char *hw_(Stmt *x, int i){ return i < x->h.n ? x->h.v[i] : ""; }
static Val valof(Stmt *x){
  const char *t = og(x, "text");
  if(t){ char *s = fm(t); Val v = Vs(s); free(s); return v; }
  t = og(x, "value"); if(t) return ev(t);
  return x->a.n ? ev(x->a.v[0]) : Vu();
}
static void into(Stmt *x, Val v){
  const char *to = og(x, "into");
  if(!to){ vdrop(v); return; }
  if(!shas(T->cur, to)){ vdrop(v); lerr("Undeclared variable: %s", to); }
  ssetv(T->cur, to, v);
}
static void into_need(Stmt *x){ if(!og(x, "into")) lerr("Undeclared variable: (missing Into)"); }
static char *jstrdup_nl(Val v){ /* jstr y \\n -> salto de linea */
  char *s = jstr(v); Buf b = {0};
  for(char *p = s; *p; p++){ if(p[0] == '\\' && p[1] == 'n'){ bc(&b, '\n'); p++; } else bc(&b, *p); }
  free(s); return btake(&b);
}
static void mc(ll a){ if(a < 0 || a >= (ll)S.memsz) lerr("Address out of range: %lld", a); }

static int ex(SL *ns, Scope *sc);
static int one(Stmt *x, Scope *sc);

static SL *load(const char *f, const char *fr){
  char *text = NULL;
  if(!strcmp(fr, "disk")){ Val *p = dget(DP(S.vd), f); if(p && p->t == T_STR) text = xs(CSTR(*p)); }
  else { char *pp = pjoin(S.root_dir, f); struct stat st; if(!stat(pp, &st) && S_ISREG(st.st_mode)) text = readfile(pp, NULL); free(pp); }
  if(!text) lerr("file not found: %s (in %s)", f, fr);
  DV E = {0}, W = {0}; SL *nodes = calloc(1, sizeof(SL)); ChkSt cs = {0};
  if(parse(text, f, &E, &W, nodes)) chk(nodes, &E, &W, &cs);
  free(text);
  if(E.n) lerr("%s l.%d: %s", f, E.v[0].n, E.v[0].m);
  return nodes;
}
static void reg_defs(SL *nodes, Scope *top, Scope *root){
  for(int i = 0; i < nodes->n; i++) if(nodes->v[i]->kw == K_def && nodes->v[i]->h.n > 1) fn_set(top, nodes->v[i]->h.v[1], nodes->v[i], root);
}
static void vdsave(void){
  Buf b = {0}; jdump(&b, S.vd); char *s = btake(&b); writefile(S.vdpath, s, b.n, "wb"); free(s);
}

/* ---------------------------------------------------------------- tareas */
static Task *task_find(const char *n){ for(int i = S.ntasks - 1; i >= 0; i--) if(!strcmp(S.tasks[i]->name, n)) return S.tasks[i]; return NULL; }
static void *task_main(void *arg_){
  Task *t = arg_; pthread_mutex_lock(&GIL);
  Th th; memset(&th, 0, sizeof th); th.task = t; th.retv = Vu(); T = &th;
  jmp_buf jb; th.jb = &jb;
  if(!setjmp(jb)){
    if(t->every < 0) ex(t->body, t->sc);
    else for(;;){
      if(sleep_ms(t->every, 1)) break;
      Scope *ls = scope_new(t->sc); th.cur = ls; int st = ex(t->body, ls); scope_unref(ls);
      if(st == ST_STOP || st == ST_RET) break;
    }
  } else { char m[1100]; snprintf(m, sizeof m, "[%s] ERROR %s", t->name, th.err); out(m); }
  t->on = 0; nthreads--; pthread_mutex_unlock(&GIL); return NULL;
}
static void start_task(const char *name, SL *body, Scope *sc, int proc, int every){
  Task *old = task_find(name); if(old && old->on) lerr("A task is already running: %s", name);
  Task *t = calloc(1, sizeof *t); t->name = xs(name); t->on = 1; t->proc = proc; t->every = every; t->body = body; t->sc = sc;
  if(S.ntasks == S.ctasks){ S.ctasks = S.ctasks ? S.ctasks * 2 : 16; S.tasks = xr(S.tasks, S.ctasks * sizeof(Task *)); }
  S.tasks[S.ntasks++] = t; nthreads++;
  pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if(pthread_create(&th, &at, task_main, t)){ t->on = 0; nthreads--; lerr("Could not create thread"); }
}
static void killproc(int pid){ for(int i = 0; i < S.ntasks; i++) if(S.tasks[i]->proc == pid) S.tasks[i]->stop = 1; }
/* ---------------------------------------------------------------- acceso real al hardware */
static void need_hw(const char *w){ if(!S.hw) lerr("%s: real hardware access is disabled (run with --hw; usually needs root)", w); }
/* ---- permissions (v2.1): a program cannot touch pins, network or files outside its folder without an explicit flag ---- */
static void need_perm(int ok, const char *what, const char *flag){ if(!ok) lerr("%s: permission denied (run logl with %s)", what, flag); }
static char *abs_lex(const char *p){   /* absolute path, lexically normalized */
  if(p[0] == '/' || (isalpha((unsigned char)p[0]) && p[1] == ':')) return normpath(p);
  char cwd[4096]; if(!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
  char *j = pjoin(cwd, p), *r = normpath(j); free(j); return r;
}
#ifdef _WIN32
static char *real_prefix(const char *p){   /* Windows: _fullpath + lexical check (symlinks/junctions NOT resolved). NOT TESTED on Windows. */
  char buf[4096]; if(!_fullpath(buf, p, sizeof buf)) return abs_lex(p);
  fixslash(buf); return normpath(buf);
}
#else
/* resolves symlinks in the part of the path that exists and appends the rest (a file that does not exist yet) */
static char *real_prefix(const char *p){
  char *cur = abs_lex(p), buf[4096], tail[4096], t2[4096]; tail[0] = 0;
  for(;;){
    if(realpath(cur, buf)){ Buf r = {0}; bs(&r, buf); bs(&r, tail); free(cur); return btake(&r); }
    char *sl = strrchr(cur, '/'); if(!sl) return cur;
    snprintf(t2, sizeof t2, "%s%s", sl, tail); snprintf(tail, sizeof tail, "%s", t2);
    if(sl == cur) cur[1] = 0; else *sl = 0;
  }
}
#endif
/* real path for a file operation: confined to --root unless --allow-disk (then it is the legacy path, unchanged) */
static char *fpath(const char *f, const char *who){
  char *p = pjoin(S.root_dir, f);
  if(S.allow_disk) return p;
  char *rp = real_prefix(p), *rr = real_prefix(S.root_dir); size_t n = strlen(rr);
  int inside = (n == 1 && rr[0] == '/') || (!strncasecmp(rp, rr, n) && (rp[n] == 0 || rp[n] == '/'));
  free(p); free(rr);
  if(!inside){ free(rp); lerr("%s: access outside the program folder is not allowed (run logl with --allow-disk)", who); }
  return rp;
}
/* ---- v2.1: per-pin state shared by the virtual and the real pins ---- */
typedef struct { int pwm, safe; double freq, duty, analog; } PinSt;   /* safe: 0 none, 1 low, 2 high */
static PinSt PST[64];
static int pin_watch[64];                 /* number of On:Pin handlers per pin */
static volatile int pwm_run[64];          /* real software-PWM thread running for this pin */
static void eq_push(const Ev *e);
static void do_watchdog(Stmt *x);
static void pinpoll_start(void);
static void pinpoll_watch(int n);
static void install_crash_handlers(void);

#ifdef _WIN32
/* En Windows no hay /dev/mem, /dev/gpiomem ni iopl: se dejan stubs que avisan. */
static int gpio_rd(int n){ (void)n; return 0; }
static void gpio_wr(int n, int v){ (void)n; (void)v; }
static void pwm_hw_start(int n){ (void)n; lerr("Pin: real PWM is not available on Windows"); }
static void pwm_hw_stop(int n){ (void)n; }
static void pinpoll_start(void){ lerr("Pin: real GPIO is not available on Windows"); }
static void pinpoll_watch(int n){ (void)n; }
static double adc_hw(int ch, int dev){ (void)ch; (void)dev; lerr("Pin: real ADC is not available on Windows"); }
static void do_reg(Stmt *x, const char *w){ (void)x; (void)w; lerr("Reg: real register access is not available on Windows"); }
static void pin_hw(int n, const char *c, const char *a1, int *res){ (void)n; (void)c; (void)a1; (void)res; lerr("Pin: real GPIO is not available on Windows"); }
static void do_port(Stmt *x, const char *w){ (void)x; (void)w; lerr("Port: I/O ports are not available on Windows"); }
#else
static RMap *rm_find(const char *id){ for(int i = 0; i < S.nrm; i++) if(S.rm[i].id && !strcmp(S.rm[i].id, id)) return &S.rm[i]; lerr("Reg: window not mapped: %s", id); }
static void do_reg(Stmt *x, const char *w){
  need_hw("Reg");
  const char *id = og(x, "id"); if(!id || !*id) lerr("Reg: missing Id");
  if(!strcmp(w, "map")){
    const char *a = og(x, "addr"), *l = og(x, "len"), *d = og(x, "dev");
    ull addr = a ? (ull)evi(a) : 0; size_t len = l ? (size_t)evi(l) : 4096; if(!len || len > (1u << 28)) lerr("Reg: invalid Len");
    if(S.nrm >= 16) lerr("Reg: too many mapped windows");
    const char *dev = d && *d ? d : "/dev/mem";
    int fd = open(dev, O_RDWR | O_SYNC); if(fd < 0) lerr("Reg: could not open %s: %s", dev, strerror(errno));
    long pg = sysconf(_SC_PAGESIZE); ull off = addr & ~(ull)(pg - 1); size_t delta = addr - off, ml = len + delta;
    void *m = mmap(NULL, ml, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);
    if(m == MAP_FAILED){ int e = errno; close(fd); lerr("Reg: mmap failed at 0x%llx: %s", addr, strerror(e)); }
    for(int i = 0; i < S.nrm; i++) if(S.rm[i].id && !strcmp(S.rm[i].id, id)) lerr("Reg: window %s already exists", id);
    RMap *r = &S.rm[S.nrm++]; r->id = xs(id); r->fd = fd; r->map = m; r->maplen = ml; r->base = (unsigned char *)m + delta; r->len = len;
    return;
  }
  RMap *r = rm_find(id);
  if(!strcmp(w, "unmap")){ munmap(r->map, r->maplen); close(r->fd); free(r->id); r->id = NULL; return; }
  const char *o = og(x, "off"), *sz = og(x, "size");
  ull off = o ? (ull)evi(o) : 0; int bits = sz ? (int)evi(sz) : 32;
  if(bits != 8 && bits != 16 && bits != 32 && bits != 64) lerr("Reg: Size must be 8, 16, 32 or 64");
  int by = bits / 8; if(off + by > r->len) lerr("Reg: offset outside the window (0x%llx)", off); if(off % by) lerr("Reg: misaligned offset");
  volatile unsigned char *p = r->base + off; ull cur = 0;
  switch(bits){ case 8: cur = *(volatile uint8_t *)p; break; case 16: cur = *(volatile uint16_t *)p; break; case 32: cur = *(volatile uint32_t *)p; break; default: cur = *(volatile uint64_t *)p; }
  if(!strcmp(w, "read")){ into(x, Vn((double)cur)); return; }
  const char *vs_ = og(x, "value"); if(!vs_) lerr("Reg: missing Value");
  ull v = (ull)dint(evd(vs_)), nv;
  if(!strcmp(w, "write")) nv = v; else if(!strcmp(w, "set")) nv = cur | v; else if(!strcmp(w, "clear")) nv = cur & ~v; else if(!strcmp(w, "toggle")) nv = cur ^ v;
  else lerr("Reg: use Map, Read, Write, Set, Clear, Toggle or Unmap");
  switch(bits){ case 8: *(volatile uint8_t *)p = (uint8_t)nv; break; case 16: *(volatile uint16_t *)p = (uint16_t)nv; break; case 32: *(volatile uint32_t *)p = (uint32_t)nv; break; default: *(volatile uint64_t *)p = nv; }
}
static void do_port(Stmt *x, const char *w){
  need_hw("Port");
#if defined(__x86_64__) || defined(__i386__)
  const char *pt = og(x, "port"), *sz = og(x, "size"); if(!pt) lerr("Port: missing Port");
  ll port = evi(pt); int bits = sz ? (int)evi(sz) : 8;
  if(port < 0 || port > 0xFFFF) lerr("Port: port out of range");
  if(bits != 8 && bits != 16 && bits != 32) lerr("Port: Size must be 8, 16 or 32");
#ifdef SYS_iopl
  if(!S.ioperm_done){ if(syscall(SYS_iopl, 3) != 0) lerr("Port: iopl failed: %s (you need root)", strerror(errno)); S.ioperm_done = 1; }
#else
  lerr("Port: iopl is not available on this system");
#endif
  unsigned short p = (unsigned short)port;
  if(!strcmp(w, "in")){
    uint32_t v = 0;
    if(bits == 8){ unsigned char b; __asm__ volatile("inb %1,%0" : "=a"(b) : "Nd"(p)); v = b; }
    else if(bits == 16){ unsigned short b; __asm__ volatile("inw %1,%0" : "=a"(b) : "Nd"(p)); v = b; }
    else { uint32_t b; __asm__ volatile("inl %1,%0" : "=a"(b) : "Nd"(p)); v = b; }
    into(x, Vn(v));
  } else if(!strcmp(w, "out")){
    const char *vs_ = og(x, "value"); if(!vs_) lerr("Port: missing Value"); uint32_t v = (uint32_t)evi(vs_);
    if(bits == 8) __asm__ volatile("outb %0,%1" : : "a"((unsigned char)v), "Nd"(p));
    else if(bits == 16) __asm__ volatile("outw %0,%1" : : "a"((unsigned short)v), "Nd"(p));
    else __asm__ volatile("outl %0,%1" : : "a"(v), "Nd"(p));
  } else lerr("Port: use In or Out");
#else
  (void)x; (void)w; lerr("Port: only available on x86");
#endif
}
/* GPIO real de Raspberry Pi 0-4 (BCM283x) via /dev/gpiomem */
static volatile uint32_t *gpio_map(void){
  if(S.gpio) return S.gpio;
  int fd = open("/dev/gpiomem", O_RDWR | O_SYNC); if(fd < 0) lerr("Pin: could not open /dev/gpiomem: %s", strerror(errno));
  void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd);
  if(m == MAP_FAILED) lerr("Pin: mmap failed: %s", strerror(errno));
  return S.gpio = (volatile uint32_t *)m;
}
/* Level read/write used by pin_hw, the PWM threads, the edge poller and the watchdog. They never call lerr (safe in any thread).
 * --fake-gpio replaces the registers with an array in RAM so the whole real-hardware path can be tested without a board. */
static uint8_t fake_lv[64];
static int gpio_rd(int n){ if(S.fakegpio) return fake_lv[n]; volatile uint32_t *g = S.gpio; return g ? (int)((g[0x34 / 4 + n / 32] >> (n % 32)) & 1) : 0; }
static void gpio_wr(int n, int v){ if(S.fakegpio){ fake_lv[n] = (uint8_t)v; return; } volatile uint32_t *g = S.gpio; if(g) g[(v ? 0x1C : 0x28) / 4 + n / 32] = 1u << (n % 32); }
static void pin_hw(int n, const char *c, const char *a1, int *res){
  need_perm(S.allow_pins, "Pin", "--allow-pins");
  if(n > 53) lerr("Pin: real GPIO supports 0-53");
  volatile uint32_t *g = S.fakegpio ? NULL : gpio_map(); int sh = (n % 10) * 3;
  if(!strcmp(c, "mode")){ if(g){ uint32_t f = g[n / 10] & ~(7u << sh); if(a1 && !strcasecmp(a1, "out")) f |= 1u << sh; g[n / 10] = f; } }
  else if(!strcmp(c, "set")){ int hi = a1 && (!strcasecmp(a1, "high") || !strcmp(a1, "1")); gpio_wr(n, hi); }
  else if(!strcmp(c, "toggle")) gpio_wr(n, !gpio_rd(n));
  else if(!strcmp(c, "read")) *res = gpio_rd(n);
}
/* software PWM: one thread per pin toggles the output. Jitter is typically 50-500 us on a normal Linux kernel (not hard real time),
 * so it is fine for LEDs, servos (50 Hz) and slow motors, not for audio or precise timing. Limited to 1000 Hz. */
static pthread_t pwm_th[64];
static void pwm_sleep(int n, double sec){
  while(sec > 0 && pwm_run[n]){
    double c = sec > 0.005 ? 0.005 : sec; struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = (long)(c * 1e9); nanosleep(&ts, NULL); sec -= c;
  }
}
static void *pwm_main(void *a){
  int n = (int)(intptr_t)a;
  while(pwm_run[n]){
    double f = PST[n].freq > 0 ? PST[n].freq : 1, d = PST[n].duty, per = 1.0 / f;
    if(d <= 0){ gpio_wr(n, 0); pwm_sleep(n, per < 0.02 ? per : 0.02); }
    else if(d >= 100){ gpio_wr(n, 1); pwm_sleep(n, per < 0.02 ? per : 0.02); }
    else { gpio_wr(n, 1); pwm_sleep(n, per * d / 100.0); gpio_wr(n, 0); pwm_sleep(n, per * (100.0 - d) / 100.0); }
  }
  return NULL;
}
static void pwm_hw_start(int n){ if(pwm_run[n]) return; pwm_run[n] = 1; if(pthread_create(&pwm_th[n], NULL, pwm_main, (void *)(intptr_t)n)){ pwm_run[n] = 0; lerr("Pin: could not start the PWM thread"); } }
static void pwm_hw_stop(int n){ if(pwm_run[n]){ pwm_run[n] = 0; pthread_join(pwm_th[n], NULL); } gpio_wr(n, 0); }
/* edge detection on real pins: a thread polls the watched pins every ~50 us. Edges shorter than that are missed, and it uses a little CPU.
 * (A kernel GPIO-event interface would be better; this one needs no extra libraries and works with the same register access.) */
static volatile int poll_run; static pthread_t poll_th; static int poll_last[64];
static void push_edge(int n, int v);
static void *pinpoll_main(void *a){
  (void)a;
  while(poll_run){
    for(int n = 0; n < 54; n++) if(pin_watch[n]){ int v = gpio_rd(n); if(poll_last[n] < 0) poll_last[n] = v; else if(v != poll_last[n]){ poll_last[n] = v; push_edge(n, v); } }
    usleep(50);
  }
  return NULL;
}
static void pinpoll_watch(int n){ poll_last[n] = gpio_rd(n); }   /* reference level read synchronously, so the first edge is never lost */
static void pinpoll_start(void){ static int init; if(!init){ for(int i = 0; i < 64; i++) poll_last[i] = -1; init = 1; } if(poll_run) return; poll_run = 1; if(pthread_create(&poll_th, NULL, pinpoll_main, NULL)){ poll_run = 0; lerr("Pin: could not start edge detection"); } }
/* real ADC through the Linux IIO subsystem: /sys/bus/iio/devices/iio:deviceD/in_voltageC_raw (boards with an ADC or an ADS1115/MCP3008 driver) */
static double adc_hw(int ch, int dev){
  if(S.fakegpio) return floor(PST[ch].analog / 3.3 * 1023.0 + 0.5);
#ifdef __linux__
  char path[160]; snprintf(path, sizeof path, "/sys/bus/iio/devices/iio:device%d/in_voltage%d_raw", dev, ch);
  FILE *f = fopen(path, "r"); if(!f) lerr("Pin: ADC: could not open %s: %s (is an IIO ADC driver loaded?)", path, strerror(errno));
  long v = 0; int ok = fscanf(f, "%ld", &v) == 1; fclose(f); if(!ok) lerr("Pin: ADC: could not read %s", path);
  return (double)v;
#else
  (void)ch; (void)dev; lerr("Pin: real ADC is only available on Linux");
#endif
}
#endif

/* pin(n) in expressions: virtual state, or the real GPIO level with --hw (needs --allow-pins) */
static int pin_level(int n){
  if(n < 0 || n >= 64) lerr("Pin out of range (0-63)");
  if(S.hw){ int res = -1; pin_hw(n, "read", NULL, &res); S.pins[n] = res; return res; }
  if(PST[n].pwm){ double ph = fmod((now() - S.t0) * PST[n].freq, 1.0); return ph * 100.0 < PST[n].duty; }   /* virtual PWM: level follows the clock */
  return S.pins[n];
}

/* informacion del sistema real (para Sys) */
static void sys_host(Dict *D){
#ifdef _WIN32
  dset(D, "uid", Vn(0)); dset(D, "is_root", Vb(0));
  SYSTEM_INFO si; GetSystemInfo(&si);
  MEMORYSTATUSEX ms; memset(&ms, 0, sizeof ms); ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
  char host[256] = "windows"; DWORD hl = sizeof host; GetComputerNameA(host, &hl);
  dset(D, "real_cpus", Vn((double)si.dwNumberOfProcessors));
  dset(D, "real_ram_mb", Vn((double)(ms.ullTotalPhys / 1048576)));
  dset(D, "hostname", Vs(host)); dset(D, "os", Vs("Windows"));
  dset(D, "arch", Vs(si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x86_64" : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x86"));
#else
  struct utsname u; uname(&u);
  dset(D, "uid", Vn(getuid())); dset(D, "is_root", Vb(geteuid() == 0));
  dset(D, "real_cpus", Vn((double)sysconf(_SC_NPROCESSORS_ONLN)));
#ifdef __APPLE__
  { uint64_t mem = 0; size_t ml = sizeof mem; if(sysctlbyname("hw.memsize", &mem, &ml, NULL, 0) != 0) mem = 0; dset(D, "real_ram_mb", Vn((double)(mem / 1048576))); }
#else
  dset(D, "real_ram_mb", Vn((double)((double)sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE) / 1048576)));
#endif
  dset(D, "hostname", Vs(u.nodename)); dset(D, "os", Vs(u.sysname)); dset(D, "arch", Vs(u.machine));
#endif
}


/* ================================================================ Log- bare-metal toolkit: Ram, Cpu, Irq, Pci, Dev, Admin (all need --hw except Cpu:Id/Tick and Irq) */
static double od(Stmt *x, const char *k, double d);
static char *os_(Stmt *x, const char *k);
static void setvar(const char *name, Val v){
  if(!name || !*name){ vdrop(v); return; }
  if(!shas(T->cur, name)){ vdrop(v); lerr("Undeclared variable: %s", name); }
  ssetv(T->cur, name, v);
}
/* 64-bit helpers: Into gets a number (exact up to 2^53); Lo / Hi variables receive the exact 32-bit halves */
static void u64out(Stmt *x, ull v){
  into(x, Vn((double)v));
  const char *lo = og(x, "lo"), *hi = og(x, "hi");
  if(lo && *lo) setvar(lo, Vn((double)(uint32_t)v));
  if(hi && *hi) setvar(hi, Vn((double)(uint32_t)(v >> 32)));
}
static ull u64in(Stmt *x){
  const char *lo = og(x, "lo"), *hi = og(x, "hi"), *v = og(x, "value");
  if((lo && *lo) || (hi && *hi)) return ((ull)(uint32_t)(hi && *hi ? evi(hi) : 0) << 32) | (uint32_t)(lo && *lo ? evi(lo) : 0);
  if(!v) lerr("missing Value (or Lo and Hi)");
  double d = evd(v); return d < 0 ? (ull)(ll)d : (ull)d;
}
static void lowop(Stmt *x, char *op, size_t n){ snprintf(op, n, "%s", hw_(x, 1)); for(char *c = op; *c; c++) *c = (char)tolower((unsigned char)*c); }

/* ---- guarded access to real memory: a bad address becomes a Log error instead of a crash ---- */
#ifndef _WIN32
static sigjmp_buf SEGV_JB; static volatile int SEGV_ON; static int guard_done;
static void guard_h(int s){ if(SEGV_ON) siglongjmp(SEGV_JB, 1); signal(s, SIG_DFL); raise(s); }
static void guard_install(void){
  if(guard_done) return; guard_done = 1;
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = guard_h; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_NODEFER;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
#ifdef SIGBUS
  sigaction(SIGBUS, &sa, NULL);
#endif
}
static int real_copy(void *dst, const void *src, size_t n){
  SEGV_ON = 1; if(sigsetjmp(SEGV_JB, 1)){ SEGV_ON = 0; return -1; }
  volatile unsigned char *d = (volatile unsigned char *)dst; const volatile unsigned char *s = (const volatile unsigned char *)src;
  if(d <= s) for(size_t i = 0; i < n; i++) d[i] = s[i]; else for(size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
  SEGV_ON = 0; return 0;
}
#else
static void guard_install(void){}
static int real_copy(void *dst, const void *src, size_t n){ if(IsBadReadPtr(src, n) || IsBadWritePtr(dst, n)) return -1; memmove(dst, src, n); return 0; }
#endif
#define UA(v) ((uintptr_t)(ull)(v))
static struct { void *p; size_t n; } RAL[256]; static int nral;

static void do_ram(Stmt *x){
  need_hw("Ram"); guard_install();
  char op[16]; lowop(x, op, sizeof op);
  const char *as = og(x, "addr"); uintptr_t addr = as ? UA(evi(as)) : 0;
  if(!strcmp(op, "alloc")){
    ll n = evi(og(x, "size") ? og(x, "size") : "4096"); if(n <= 0 || n > (1LL << 32)) lerr("Ram:Alloc: invalid Size");
    if(nral >= 256) lerr("Ram:Alloc: too many blocks (max 256)");
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, (SIZE_T)n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); if(!p) lerr("Ram:Alloc: the system refused the memory");
#else
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
    void *p = mmap(NULL, (size_t)n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if(p == MAP_FAILED) lerr("Ram:Alloc: %s", strerror(errno));
#endif
    RAL[nral].p = p; RAL[nral].n = (size_t)n; nral++; into(x, Vn((double)(uintptr_t)p));
  } else if(!strcmp(op, "free")){
    int k = -1; for(int i = 0; i < nral; i++) if((uintptr_t)RAL[i].p == addr) k = i;
    if(k < 0) lerr("Ram:Free: 0x%llx is not a block returned by Ram:Alloc", (ull)addr);
#ifdef _WIN32
    VirtualFree(RAL[k].p, 0, MEM_RELEASE);
#else
    munmap(RAL[k].p, RAL[k].n);
#endif
    RAL[k] = RAL[--nral];
  } else if(!strcmp(op, "read") || !strcmp(op, "write")){
    const char *sz = og(x, "size"); int n = sz ? (int)evi(sz) : 4; if(n != 1 && n != 2 && n != 4 && n != 8) lerr("Ram: Size must be 1, 2, 4 or 8");
    unsigned char buf[8] = {0};
    if(!strcmp(op, "read")){
      if(real_copy(buf, (void *)addr, (size_t)n)) lerr("Ram:Read: invalid address 0x%llx", (ull)addr);
      ull v = 0; for(int i = n - 1; i >= 0; i--) v = (v << 8) | buf[i]; u64out(x, v);
    } else {
      ull v = u64in(x); for(int i = 0; i < n; i++) buf[i] = (unsigned char)(v >> (8 * i));
      if(real_copy((void *)addr, buf, (size_t)n)) lerr("Ram:Write: invalid address 0x%llx (or read-only memory)", (ull)addr);
    }
  } else if(!strcmp(op, "copy")){
    const char *f = og(x, "from"), *t = og(x, "to"), *l = og(x, "len"); if(!f || !t || !l) lerr("Ram:Copy needs From, To and Len");
    ll n = evi(l); uintptr_t src = UA(evi(f)), dst = UA(evi(t)); if(n < 0) lerr("Ram:Copy: invalid Len");
    if(real_copy((void *)dst, (void *)src, (size_t)n)) lerr("Ram:Copy: invalid address");
  } else if(!strcmp(op, "load") || !strcmp(op, "store")){
    const char *l = og(x, "len"); if(!as || !l) lerr("Ram:%s needs Addr and Len", hw_(x, 1));
    ll n = evi(l); int ld = !strcmp(op, "load"); const char *em = og(x, ld ? "from" : "to"); ll e = em ? evi(em) : 0;
    if(n < 0) lerr("Ram:%s: invalid Len", hw_(x, 1)); if(n){ mc(e); mc(e + n - 1); }
    int bad = ld ? real_copy((void *)addr, S.MEM + e, (size_t)n) : real_copy(S.MEM + e, (void *)addr, (size_t)n);
    if(bad) lerr("Ram:%s: invalid address 0x%llx", hw_(x, 1), (ull)addr);
  } else if(!strcmp(op, "protect")){
    const char *l = og(x, "len"), *m = og(x, "mode"); if(!as || !l || !m) lerr("Ram:Protect needs Addr, Len and Mode (r, w, x)");
    int r = strchr(m, 'r') != NULL, w = strchr(m, 'w') != NULL, xx = strchr(m, 'x') != NULL; ll n = evi(l);
#ifdef _WIN32
    DWORD np = xx ? (w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE) : (w ? PAGE_READWRITE : r ? PAGE_READONLY : PAGE_NOACCESS), old;
    if(!VirtualProtect((void *)addr, (SIZE_T)n, np, &old)) lerr("Ram:Protect failed");
#else
    long pg = sysconf(_SC_PAGESIZE); uintptr_t a0 = addr & ~(uintptr_t)(pg - 1);
    if(mprotect((void *)a0, (size_t)n + (addr - a0), (r ? PROT_READ : 0) | (w ? PROT_WRITE : 0) | (xx ? PROT_EXEC : 0))) lerr("Ram:Protect: %s", strerror(errno));
#endif
    if(xx) __builtin___clear_cache((char *)addr, (char *)addr + n);
  } else lerr("Ram: use Alloc, Free, Read, Write, Copy, Load, Store or Protect");
}

/* ---- Cpu ---- */
static void do_cpu(Stmt *x){
  char op[16]; lowop(x, op, sizeof op);
  if(!strcmp(op, "id")){
#if defined(__x86_64__) || defined(__i386__)
    unsigned leaf = (unsigned)evi(og(x, "leaf") ? og(x, "leaf") : "0"), sub = (unsigned)evi(og(x, "sub") ? og(x, "sub") : "0"), a, b, c, d;
    __cpuid_count(leaf, sub, a, b, c, d);
    char tx[17], vn[13]; unsigned w[4] = {a, b, c, d};
    for(int i = 0; i < 16; i++){ unsigned char ch = (unsigned char)(w[i / 4] >> (8 * (i % 4))); tx[i] = (ch >= 32 && ch < 127) ? (char)ch : '.'; } tx[16] = 0;
    unsigned vw[3] = {b, d, c}; for(int i = 0; i < 12; i++) vn[i] = (char)(vw[i / 4] >> (8 * (i % 4))); vn[12] = 0;
    static const char *RN[] = {"eax", "ebx", "ecx", "edx"};                 /* Eax;a  Ebx;b  Ecx;c  Edx;d  -> variable names */
    for(int i = 0; i < 4; i++){ const char *nm = og(x, RN[i]); if(nm && *nm) setvar(nm, Vn(w[i])); }
    const char *nm = og(x, "vendor"); if(nm && *nm) setvar(nm, Vs(vn));       /* leaf 0: "GenuineIntel", "AuthenticAMD"... */
    nm = og(x, "text"); if(nm && *nm) setvar(nm, Vs(tx));                      /* the 16 bytes as text (leaves 0x80000002-4: brand name) */
#else
    (void)x; lerr("Cpu:Id: only available on x86 processors");
#endif
  } else if(!strcmp(op, "tick")){
    ull t;
#if defined(__x86_64__) || defined(__i386__)
    t = (ull)__rdtsc();
#elif defined(__aarch64__)
    { ull v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); t = v; }
#else
    t = (ull)(now() * 1e9);
#endif
    u64out(x, t);
  } else if(!strcmp(op, "call")){
    need_hw("Cpu:Call"); guard_install();
    const char *as = og(x, "addr"); if(!as) lerr("Cpu:Call needs Addr"); uintptr_t addr = UA(evi(as)); ll arg = evi(og(x, "arg") ? og(x, "arg") : "0");
    typedef long long (*Fn1)(long long); Fn1 fn = (Fn1)addr; volatile long long r = 0;
#ifndef _WIN32
    SEGV_ON = 1; if(sigsetjmp(SEGV_JB, 1)){ SEGV_ON = 0; lerr("Cpu:Call: the code at 0x%llx crashed (invalid instruction or memory access)", (ull)addr); }
    r = fn(arg); SEGV_ON = 0;
#else
    if(IsBadCodePtr((FARPROC)addr)) lerr("Cpu:Call: invalid address 0x%llx", (ull)addr);
    r = fn(arg);
#endif
    into(x, Vn((double)r));
  } else if(!strcmp(op, "msr")){
    need_hw("Cpu:Msr");
#if defined(__linux__)
    int core = (int)evi(og(x, "core") ? og(x, "core") : "0"); const char *rg = og(x, "reg"); if(!rg) lerr("Cpu:Msr needs Reg");
    char path[64]; snprintf(path, sizeof path, "/dev/cpu/%d/msr", core); int wr = og(x, "value") || og(x, "lo") || og(x, "hi");
    int fd = open(path, wr ? O_RDWR : O_RDONLY); if(fd < 0) lerr("Cpu:Msr: could not open %s: %s (needs root and: modprobe msr)", path, strerror(errno));
    off_t off = (off_t)evi(rg); ull v = 0; int bad;
    if(wr){ v = u64in(x); bad = pwrite(fd, &v, 8, off) != 8; } else { bad = pread(fd, &v, 8, off) != 8; }
    int e = errno; close(fd); if(bad) lerr("Cpu:Msr: access to that register failed: %s", strerror(e));
    if(!wr) u64out(x, v);
#else
    (void)x; lerr("Cpu:Msr: only available on Linux");
#endif
  } else lerr("Cpu: use Id, Tick, Call or Msr");
}

/* ---- Irq: interrupt vector table (emulated), real signals and timers can raise vectors ---- */
static struct { char *fn[256]; volatile unsigned char pend[256]; volatile int any, on, in; volatile int tmr[256]; } IRQ = {{0}, {0}, 0, 1, 0, {0}};
static void irq_call(const char *name, int n){
  Fn *f = fn_get(stop_(S.root), name);
  if(!f){ char m[300]; snprintf(m, sizeof m, "[irq %d] ERROR function not defined: %s", n, name); out(m); return; }
  Scope *volatile loc = scope_new(f->sc); Scope *volatile cur = T->cur;
  if(f->x->a.n >= 1) sdecl(loc, f->x->a.v[0], Vn(n), -1);
  jmp_buf jb, *old = T->jb; T->jb = &jb;
  if(!setjmp(jb)){ ex(&f->x->body, loc); vdrop(T->retv); T->retv = Vu(); }
  else { char m[1100]; snprintf(m, sizeof m, "[irq %d] ERROR %s", n, T->err); out(m); T->depth = 0; }
  T->jb = old; T->cur = cur; scope_unref(loc);
}
static void irq_deliver(void);
static void irq_poll(void){ if(IRQ.any && IRQ.on) irq_deliver(); }
static void irq_deliver(void){
  if(IRQ.in) return; IRQ.in = 1; IRQ.any = 0;
  for(int n = 0; n < 256; n++) if(IRQ.pend[n]){ IRQ.pend[n] = 0; if(IRQ.fn[n]) irq_call(IRQ.fn[n], n); }
  IRQ.in = 0;
}
#ifndef _WIN32
static volatile int SIGVEC[65];
static void irq_sig(int s){ int v = (s >= 0 && s < 65) ? SIGVEC[s] : -1; if(v >= 0){ IRQ.pend[v] = 1; IRQ.any = 1; } }
#endif
static void *irq_tmr(void *a){
  int v = (int)(intptr_t)a;
  while(IRQ.tmr[v] > 0 && !S.stop){ usleep((unsigned)IRQ.tmr[v] * 1000u); if(IRQ.tmr[v] > 0){ IRQ.pend[v] = 1; IRQ.any = 1; } }
  return NULL;
}
static void do_irq(Stmt *x){
  char op[16]; lowop(x, op, sizeof op);
  if(!strcmp(op, "enable")){ IRQ.on = 1; if(IRQ.any) irq_deliver(); return; }
  if(!strcmp(op, "disable")){ IRQ.on = 0; return; }
  const char *ns = og(x, "n"); if(!ns) lerr("Irq:%s needs N (vector 0-255)", hw_(x, 1));
  ll n = evi(ns); if(n < 0 || n > 255) lerr("Irq: N must be between 0 and 255");
  if(!strcmp(op, "set")){
    char *f = os_(x, "call"); if(!f || !*f) lerr("Irq:Set needs Call (the name of a Def function)");
    free(IRQ.fn[n]); IRQ.fn[n] = f;
  } else if(!strcmp(op, "off")){
    free(IRQ.fn[n]); IRQ.fn[n] = NULL; IRQ.pend[n] = 0; IRQ.tmr[n] = 0;
  } else if(!strcmp(op, "raise")){
    IRQ.pend[n] = 1; IRQ.any = 1; if(IRQ.on) irq_deliver();
  } else if(!strcmp(op, "timer")){
    int ms = (int)od(x, "ms", 0);
    if(ms <= 0){ IRQ.tmr[n] = 0; return; }
    int was = IRQ.tmr[n]; IRQ.tmr[n] = ms;
    if(!was){ pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_create(&th, &at, irq_tmr, (void *)(intptr_t)n); }
  } else if(!strcmp(op, "signal")){
#ifndef _WIN32
    const char *sg = og(x, "sig"); if(!sg) lerr("Irq:Signal needs Sig (USR1, USR2, HUP or ALRM)");
    int s = !strcasecmp(sg, "usr1") ? SIGUSR1 : !strcasecmp(sg, "usr2") ? SIGUSR2 : !strcasecmp(sg, "hup") ? SIGHUP : !strcasecmp(sg, "alrm") ? SIGALRM : -1;
    if(s < 0) lerr("Irq:Signal: Sig must be USR1, USR2, HUP or ALRM");
    SIGVEC[s] = (int)n; signal(s, irq_sig);
#else
    lerr("Irq:Signal: not available on Windows");
#endif
  } else lerr("Irq: use Set, Off, Raise, Enable, Disable, Timer or Signal");
}

/* ---- Pci (Linux sysfs) ---- */
static void do_pci(Stmt *x){
  need_hw("Pci"); char op[16]; lowop(x, op, sizeof op);
#ifdef __linux__
  if(!strcmp(op, "list")){
    DIR *d = opendir("/sys/bus/pci/devices"); if(!d) lerr("Pci:List: this system has no /sys/bus/pci");
    Val l = Vl(); struct dirent *e;
    while((e = readdir(d))){
      if(e->d_name[0] == '.') continue;
      static const char *F[] = {"vendor", "device", "class", NULL}; Buf ln = {0}; bs(&ln, e->d_name);
      for(int i = 0; F[i]; i++){ char p[300]; snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/%s", e->d_name, F[i]); size_t n; char *t = readfile(p, &n); char *s2 = t ? strip_dup(t) : xs("?"); bc(&ln, ' '); bs(&ln, s2); free(s2); free(t); }
      char *lt = btake(&ln); lpush(LP(l), Vs(lt)); free(lt);          /* "0000:00:1f.3 0x8086 0x7a04 0x060100" */
    }
    closedir(d); into(x, l); return;
  }
  int seg = (int)od(x, "seg", 0), bus = (int)od(x, "bus", 0), dev = (int)od(x, "dev", 0), fn = (int)od(x, "fn", 0), off = (int)od(x, "off", 0), sz = (int)od(x, "size", 4);
  if(sz != 1 && sz != 2 && sz != 4) lerr("Pci: Size must be 1, 2 or 4");
  char path[128]; snprintf(path, sizeof path, "/sys/bus/pci/devices/%04x:%02x:%02x.%d/config", seg, bus, dev, fn);
  int wr = !strcmp(op, "write"); if(!wr && strcmp(op, "read")) lerr("Pci: use List, Read or Write");
  int fd = open(path, wr ? O_RDWR : O_RDONLY); if(fd < 0) lerr("Pci: could not open %s: %s", path, strerror(errno));
  uint32_t v = 0; int bad;
  if(wr){ v = (uint32_t)u64in(x); bad = pwrite(fd, &v, (size_t)sz, off) != sz; } else bad = pread(fd, &v, (size_t)sz, off) != sz;
  int e2 = errno; close(fd); if(bad) lerr("Pci: access failed: %s", strerror(e2));
  if(!wr) into(x, Vn((double)v));
#else
  (void)x; (void)op; lerr("Pci: only available on Linux");
#endif
}

/* ---- Dev: real devices and disk images (raw sectors) ---- */
#ifndef _WIN32
static struct { char *id; int fd; } DEVT[16];
static int dev_find(const char *id){ for(int i = 0; i < 16; i++) if(DEVT[i].id && !strcmp(DEVT[i].id, id)) return i; lerr("Dev: not open: %s", id); }
static void do_dev(Stmt *x){
  need_hw("Dev"); char op[16]; lowop(x, op, sizeof op);
  const char *id = og(x, "id"); if(!id || !*id) lerr("Dev: missing Id");
  if(!strcmp(op, "open")){
    char *path = os_(x, "path"); if(!path || !*path) lerr("Dev:Open needs Path");
    const char *m = og(x, "mode"); if(!m || !*m) m = "r"; int w = strchr(m, 'w') != NULL, c = strchr(m, 'c') != NULL;
    struct stat st; int ex_ = stat(path, &st) == 0;
    if(w && ex_ && (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) && !S.allowraw){ free(path); lerr("Dev:Open: writing to a device (disk, port...) is dangerous: run logl with --allow-raw to allow it"); }
    int slot = -1; for(int i = 0; i < 16; i++){ if(DEVT[i].id && !strcmp(DEVT[i].id, id)){ free(path); lerr("Dev: %s is already open", id); } if(!DEVT[i].id && slot < 0) slot = i; }
    if(slot < 0){ free(path); lerr("Dev: too many open devices (max 16)"); }
    int fd = open(path, (w ? (strchr(m, 'r') ? O_RDWR : O_WRONLY) : O_RDONLY) | (c ? O_CREAT : 0), 0644);
    if(fd < 0){ int e = errno; char *p2 = path; lerr("Dev:Open: could not open %s: %s", p2, strerror(e)); }
    free(path); DEVT[slot].id = xs(id); DEVT[slot].fd = fd; return;
  }
  int k = dev_find(id); int fd = DEVT[k].fd;
  if(!strcmp(op, "close")){ close(fd); free(DEVT[k].id); DEVT[k].id = NULL; return; }
  if(!strcmp(op, "size")){ off_t cur = lseek(fd, 0, SEEK_CUR), e = lseek(fd, 0, SEEK_END); lseek(fd, cur < 0 ? 0 : cur, SEEK_SET); if(e < 0) lerr("Dev:Size: %s", strerror(errno)); into(x, Vn((double)e)); return; }
  if(!strcmp(op, "ioctl")){
    const char *rq = og(x, "req"); if(!rq) lerr("Dev:Ioctl needs Req");
    int r = ioctl(fd, (unsigned long)evi(rq), (long)evi(og(x, "arg") ? og(x, "arg") : "0")); if(r < 0) lerr("Dev:Ioctl: %s", strerror(errno)); into(x, Vn(r)); return;
  }
  if(!strcmp(op, "read") || !strcmp(op, "write")){
    ll len = evi(og(x, "len") ? og(x, "len") : "0"), am = evi(og(x, "addr") ? og(x, "addr") : "0"); const char *of = og(x, "off");
    if(len < 0) lerr("Dev: invalid Len"); if(len){ mc(am); mc(am + len - 1); }
    ssize_t r;
    if(!strcmp(op, "read")) r = of ? pread(fd, S.MEM + am, (size_t)len, (off_t)evi(of)) : read(fd, S.MEM + am, (size_t)len);
    else r = of ? pwrite(fd, S.MEM + am, (size_t)len, (off_t)evi(of)) : write(fd, S.MEM + am, (size_t)len);
    if(r < 0) lerr("Dev:%s: %s", hw_(x, 1), strerror(errno));
    into(x, Vn((double)r)); return;
  }
  lerr("Dev: use Open, Read, Write, Size, Ioctl or Close");
}
#else
static void do_dev(Stmt *x){ (void)x; lerr("Dev: not available on Windows (use Host: for files)"); }
#endif

/* ---- Admin ---- */
static void do_admin(Stmt *x){
  need_hw("Admin"); char op[16]; lowop(x, op, sizeof op);
  if(!strcmp(op, "id")){
    double v[6];
#ifndef _WIN32
    v[0] = getuid(); v[1] = geteuid(); v[2] = getgid(); v[3] = getegid(); v[4] = geteuid() == 0;
#else
    v[0] = v[1] = v[2] = v[3] = v[4] = 0;
#endif
    v[5] = getpid(); static const char *AN[] = {"uid", "euid", "gid", "egid", "root", "pid"};   /* Uid;u  Root;r ... -> variable names */
    for(int i = 0; i < 6; i++){ const char *nm = og(x, AN[i]); if(nm && *nm) setvar(nm, Vn(v[i])); }
  } else if(!strcmp(op, "run")){
    need_perm(S.allowraw, "Admin:Run (runs system commands)", "--allow-raw");   /* v2.1 */
    char *cmd = os_(x, "cmd"); if(!cmd || !*cmd) lerr("Admin:Run needs Cmd");
    FILE *p = popen(cmd, "r"); free(cmd); if(!p) lerr("Admin:Run: could not start the command");
    Buf b = {0}; char t[4096]; size_t n; while((n = fread(t, 1, sizeof t, p)) > 0 && b.n < (1 << 20)) bcat(&b, t, (int)n);
    int st = pclose(p);
#ifndef _WIN32
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#else
    int code = st;
#endif
    char *o = btake(&b); Val ov = Vs(o); free(o); into(x, ov);
    const char *cv = og(x, "code"); if(cv && *cv) setvar(cv, Vn(code));
  } else if(!strcmp(op, "kill")){
#ifndef _WIN32
    const char *pd = og(x, "pid"); if(!pd) lerr("Admin:Kill needs Pid");
    if(kill((pid_t)evi(pd), (int)od(x, "sig", 15))) lerr("Admin:Kill: %s", strerror(errno));
#else
    lerr("Admin:Kill: not available on Windows");
#endif
  } else if(!strcmp(op, "nice")){
#ifndef _WIN32
    if(setpriority(PRIO_PROCESS, (id_t)od(x, "pid", 0), (int)od(x, "level", 0))) lerr("Admin:Nice: %s", strerror(errno));
#else
    lerr("Admin:Nice: not available on Windows");
#endif
  } else if(!strcmp(op, "chmod")){
    char *f = os_(x, "file"); const char *m = og(x, "mode"); if(!f || !m) lerr("Admin:Chmod needs File and Mode (octal, for example 0644)");
    char *p = fpath(f, "Admin:Chmod"); int r = chmod(p, (mode_t)strtol(m, NULL, 8)); int e = errno; free(p); free(f);
    if(r) lerr("Admin:Chmod: %s", strerror(e));
  } else lerr("Admin: use Id, Run, Kill, Nice or Chmod");
}

/* ---------------------------------------------------------------- variables y listas */
typedef struct { Val v; int i; } SI;
static int sicmp(const void *a, const void *b){
  const SI *x = a, *y = b; int xn = isnumv(x->v), yn = isnumv(y->v), r;
  if(xn && yn) r = x->v.u.n < y->v.u.n ? -1 : x->v.u.n > y->v.u.n;
  else if(xn != yn) r = xn ? -1 : 1;
  else { char *p = jstr(x->v), *q = jstr(y->v); r = strcmp(p, q); free(p); free(q); }
  return r ? r : x->i - y->i;
}
static void do_vs(Stmt *x, int k, Scope *sc){
  SV *h = &x->h; const char *n = h->v[h->n - 1];
  if(k == K_set && !shas(sc, n)) lerr("Undeclared variable: %s", n);
  int ty = (k == K_var && h->n > 2) ? typidx(h->v[1]) : (k == K_set ? sty(sc, n) : -1);
  const char *ix = og(x, "index"), *lst = og(x, "list"), *sz = og(x, "size"), *dc = og(x, "dict"), *ky = og(x, "key");
  Val v;
  if(k == K_var && dc){
    v = Vd(); SV p = {0}; char *t = strip_dup(dc); if(*t) sp_split(dc, &p); free(t);
    for(int i = 0; i < p.n; i++){ char *eq = strchr(p.v[i], '='); if(!eq) lerr("Dict: missing '=' in %s", p.v[i]); char *key = strip_n(p.v[i], eq - p.v[i]); dset(DP(v), key, ev(eq + 1)); free(key); }
  } else if(k == K_var && (lst || sz)){
    v = Vl();
    if(lst){ char *t = strip_dup(lst); if(*t){ SV p = {0}; sp_split(lst, &p); for(int i = 0; i < p.n; i++) lpush(LP(v), ev(p.v[i])); } free(t); }
    else { ll cnt = evi(sz); const char *fl = og(x, "fill"); for(ll i = 0; i < cnt; i++) lpush(LP(v), ev(fl && *fl ? fl : "0")); }
  } else v = valof(x);
  if(ky){
    Val o = sget(sc, n); if(o.t != T_DICT){ vdrop(o); vdrop(v); lerr("%s is not a dictionary", n); }
    char *key = fm(ky); dset(DP(o), key, v); free(key); vdrop(o); return;
  }
  if(ix){
    Val a = sget(sc, n); if(a.t != T_LIST){ vdrop(a); vdrop(v); lerr("%s is not a list", n); }
    SV p = {0}; sp_split(ix, &p); Val cur = a;
    for(int i = 0; i < p.n - 1; i++){ ll j = evi(p.v[i]); if(cur.t != T_LIST || j < 0 || j >= LP(cur)->n) lerr("Index out of range"); cur = LP(cur)->a[j]; }
    double li = evd(p.v[p.n - 1]);
    if(cur.t != T_LIST || li != floor(li) || li < 0 || li >= LP(cur)->n) lerr("Index out of range: %s", ix);
    Val nv = cv(ty, v); vdrop(LP(cur)->a[(int)li]); LP(cur)->a[(int)li] = nv; vdrop(a); return;
  }
  if(k == K_var) sdecl(sc, n, cv(ty, v), ty); else ssetv(sc, n, v);
}
/* Split:parts("Value;text","By;,")   or   Split("Text;a b c","Into;parts")
   Cuts a text into a list (the destination must be declared, e.g. Var:parts("List;")).
   By/Sep/On = separator (\\n and \\t allowed; none = any run of spaces), Limit;n = at most n pieces, Trim;1 = trim every piece, Skip;1 = drop empty pieces */
static void do_split(Stmt *x, Scope *sc){
  const char *dst = *hw_(x, 1) ? hw_(x, 1) : og(x, "into");
  if(!dst) lerr("Split needs a destination: Split:list(\"Value;text\",\"By;,\") or Split(\"Value;text\",\"Into;list\")");
  if(!shas(sc, dst)) lerr("Undeclared variable: %s (declare it first, for example Var:%s(\"List;\"))", dst, dst);
  Val v = valof(x); char *s = jstrdup_nl(v); vdrop(v);
  const char *bs0 = og(x, "by"); if(!bs0) bs0 = og(x, "sep"); if(!bs0) bs0 = og(x, "on");
  char *sep = NULL; if(bs0){ char *f = fm(bs0); Buf b = {0};
    for(char *p = f; *p; p++){ if(p[0] == '\\' && p[1] == 'n'){ bc(&b, '\n'); p++; } else if(p[0] == '\\' && p[1] == 't'){ bc(&b, '\t'); p++; } else bc(&b, *p); }
    free(f); sep = btake(&b); }
  const char *ts = og(x, "trim"), *ks = og(x, "skip"), *ls = og(x, "limit");
  int trim = ts && evi(ts), skip = ks && evi(ks); ll lim = ls ? evi(ls) : 0; int t = sty(sc, dst);
  Val l = Vl(); size_t sl = sep ? strlen(sep) : 0; const char *p = s; int n = 0;
  for(;;){
    const char *e; size_t len;
    if(sl){ e = (lim > 0 && n + 1 >= lim) ? NULL : strstr(p, sep); len = e ? (size_t)(e - p) : strlen(p); }
    else { while(*p && isspace((unsigned char)*p)) p++; if(!*p) break; e = p; if(lim > 0 && n + 1 >= lim) e = p + strlen(p); else while(*e && !isspace((unsigned char)*e)) e++; len = (size_t)(e - p); if(!*e) e = NULL; }
    char *piece = xsn(p, len);
    if(trim || !sl){ char *a = piece; while(*a && isspace((unsigned char)*a)) a++; size_t m = strlen(a); while(m && isspace((unsigned char)a[m - 1])) m--; char *q = xsn(a, m); free(piece); piece = q; }
    if(!(skip && !*piece)){ lpush(LP(l), cv(t, Vs(piece))); n++; }
    free(piece);
    if(!e) break; p = e + sl;
    if(!sl && !*p) break;
  }
  free(s); free(sep); ssetv(sc, dst, l);
}
static void do_lst(Stmt *x, int k, Scope *sc){
  const char *n = hw_(x, 1); int has = shas(sc, n); Val a = has ? sget(sc, n) : Vu();
  if(k == K_remove && a.t == T_DICT){ char *key = fm(og(x, "key") ? og(x, "key") : ""); dremove(DP(a), key); free(key); vdrop(a); return; }
  if(a.t != T_LIST){ vdrop(a); lerr("%s is not a list (declare it with Var:list(\"List;1,2,3\"))", n); }
  List *l = LP(a); int t = sty(sc, n);
  if(k == K_push) lpush(l, cv(t, valof(x)));
  else if(k == K_insert || k == K_remove){
    const char *ixs = og(x, "index"); if(!ixs) lerr("Missing Index"); ll i = evi(ixs);
    if(k == K_insert){
      if(i < 0 || i > l->n) lerr("Index out of range: %lld", i);
      Val v = cv(t, valof(x)); lpush(l, Vu()); memmove(&l->a[i + 1], &l->a[i], (l->n - 1 - i) * sizeof(Val)); l->a[i] = v;
    } else {
      if(i < 0 || i >= l->n) lerr("Index out of range: %lld", i);
      vdrop(l->a[i]); memmove(&l->a[i], &l->a[i + 1], (l->n - 1 - i) * sizeof(Val)); l->n--;
    }
  } else if(k == K_pop){ if(!l->n) lerr("The list is empty"); Val v = l->a[--l->n]; into(x, v); }
  else if(k == K_sort){
    SI *t2 = xm(l->n * sizeof(SI) + 1); for(int i = 0; i < l->n; i++){ t2[i].v = l->a[i]; t2[i].i = i; }
    qsort(t2, l->n, sizeof(SI), sicmp); for(int i = 0; i < l->n; i++) l->a[i] = t2[i].v; free(t2);
    const char *o = og(x, "order");
    if(o && !strncasecmp(o, "desc", 4)) for(int i = 0, j = l->n - 1; i < j; i++, j--){ Val tmp = l->a[i]; l->a[i] = l->a[j]; l->a[j] = tmp; }
  } else for(int i = l->n - 1; i > 0; i--){ int j = (int)(rnd() * (i + 1)); Val tmp = l->a[i]; l->a[i] = l->a[j]; l->a[j] = tmp; }
  vdrop(a);
}

/* ---------------------------------------------------------------- funciones y modulos */
static Mod *mod_alias(const char *al){ for(int i = 0; i < S.nal; i++) if(!strcmp(S.al[i].alias, al)) return S.al[i].m; return NULL; }
static int docall(Stmt *x, Scope *sc){
  SV *h = &x->h; Fn *f; const char *nm = hw_(x, 1); char full[256];
  if(h->n > 2){
    Mod *m = mod_alias(h->v[1]); if(!m) lerr("Module not imported: %s", h->v[1]);
    f = fn_get(stop_(m->sc), h->v[2]); snprintf(full, sizeof full, "%s:%s", h->v[1], h->v[2]); nm = full;
    if(!f) lerr("Function not defined in module %s: %s", h->v[1], h->v[2]);
  } else { f = fn_get(stop_(sc), h->v[1]); if(!f) lerr("Function not defined: %s", h->v[1]); }
  Stmt *fx = f->x; Scope *par = f->sc;
  if(x->a.n != fx->a.n) lerr("%s espera %d argumento(s) y recibio %d", nm, fx->a.n, x->a.n);
  Scope *loc = scope_new(par);
  Val av[x->a.n ? x->a.n : 1]; for(int i = 0; i < x->a.n; i++) av[i] = ev(x->a.v[i]);
  for(int i = 0; i < x->a.n; i++) sdecl(loc, fx->a.v[i], av[i], -1);
  if(++T->depth > 200){ T->depth = 0; lerr("Too much recursion (limit 200)"); }
  Val rv = Vu(); int st = ex(&fx->body, loc);
  if(st == ST_RET){ rv = T->retv; T->retv = Vu(); st = ST_OK; }
  T->depth = T->depth > 0 ? T->depth - 1 : 0; scope_unref(loc);
  if(st == ST_STOP || st == ST_BRK || st == ST_CNT){ vdrop(rv); return st; }
  T->cur = sc; into(x, rv); return ST_OK;
}
static void do_import(Stmt *x, Scope *sc){
  if(!x->q.n) lerr("Import needs a file: Import(\"lib/util.log+\",\"As;u\")");
  char *j = pjoin(S.path, x->q.v[0]); char *f = normpath(j); free(j);
  const char *frs = og(x, "from"); char *fr = lowerdup(frs && *frs ? frs : "host");
  const char *als = og(x, "as"); char alb[128];
  if(als && *als) snprintf(alb, sizeof alb, "%s", als);
  else { const char *b = strrchr(f, '/'); b = b ? b + 1 : f; snprintf(alb, sizeof alb, "%s", b); char *d = strrchr(alb, '.'); if(d) *d = 0; for(char *c = alb; *c; c++) if(!isalnum((unsigned char)*c) && *c != '_') *c = '_'; }
  if(!(isalpha((unsigned char)alb[0]) || alb[0] == '_')) lerr("Import: invalid alias: %s", alb);
  for(char *c = alb; *c; c++) if(!isalnum((unsigned char)*c) && *c != '_') lerr("Import: invalid alias: %s", alb);
  Mod *m = NULL; for(int i = 0; i < S.nmods; i++) if(!strcmp(S.mods[i].file, f)) m = &S.mods[i];
  if(!m){
    SL *nodes = load(f, fr); if(S.nmods >= 64) lerr("Too many modules");
    m = &S.mods[S.nmods++]; m->file = f; m->sc = scope_new(NULL);
    reg_defs(nodes, m->sc, m->sc);
    char *p0 = S.path; S.path = pdirname(f); ex(nodes, m->sc); S.path = p0;
  }
  if(S.nal < 64){ S.al[S.nal].alias = xs(alb); S.al[S.nal].m = m; S.nal++; }
  T->cur = sc; Val d; d.t = T_DICT; d.u.p = m->sc->v; dset(sc->v, alb, vref(d));
}

/* ---------------------------------------------------------------- consola, memoria, disco, host, pantalla */
static const struct { const char *n; Col c; } NAMED[] = {{"white",{255,255,255}},{"black",{0,0,0}},{"red",{255,0,0}},{"green",{0,255,0}},{"blue",{0,0,255}},{"yellow",{255,255,0}},{"cyan",{0,255,255}},{"magenta",{255,0,255}},{"gray",{128,128,128}},{"orange",{255,165,0}},{NULL,{0,0,0}}};
static double hue_v(double m1, double m2, double h){
  h = h - floor(h);
  if(h < 1.0 / 6) return m1 + (m2 - m1) * h * 6; if(h < 0.5) return m2;
  if(h < 2.0 / 3) return m1 + (m2 - m1) * (2.0 / 3 - h) * 6; return m1;
}
static Col cs_(SV *p, int from){
  Col c = {255, 255, 255}; int n = p->n - from; char **v = p->v + from;
  if(n > 0 && !strcasecmp(v[0], "hsl")){
    ll hh = n > 1 ? evi(v[1]) : 0; hh = ((hh % 360) + 360) % 360; double H = hh / 360.0, L = (n > 2 ? evi(v[2]) : 55) / 100.0, Sa = 0.9;
    double m2 = L <= 0.5 ? L * (1 + Sa) : L + Sa - L * Sa, m1 = 2 * L - m2;
    c.r = (unsigned char)(hue_v(m1, m2, H + 1.0 / 3) * 255); c.g = (unsigned char)(hue_v(m1, m2, H) * 255); c.b = (unsigned char)(hue_v(m1, m2, H - 1.0 / 3) * 255); return c;
  }
  if(n > 0 && !strcasecmp(v[0], "rgb")){ v++; n--; }
  if(n >= 3){ c.r = evi(v[0]) & 255; c.g = evi(v[1]) & 255; c.b = evi(v[2]) & 255; return c; }
  if(n > 0 && v[0][0] == '#' && strlen(v[0]) == 7){ unsigned r, g, b; if(sscanf(v[0] + 1, "%2x%2x%2x", &r, &g, &b) == 3){ c.r = r; c.g = g; c.b = b; } return c; }
  if(n > 0) for(int i = 0; NAMED[i].n; i++) if(!strcasecmp(NAMED[i].n, v[0])) return NAMED[i].c;
  return c;
}
static Col col_(Stmt *x){
  Col c = {255, 255, 255};
  for(int i = 0; i < x->no; i++) if(!strcasecmp(x->o[i].v[0], "color")) return cs_(&x->o[i], 1);
  for(int i = 0; i < x->no; i++) if(!strcasecmp(x->o[i].v[0], "write")){
    c.r = c.g = c.b = 0;
    for(int j = 1; j < x->o[i].n; j++){ char *eq = strchr(x->o[i].v[j], '='); if(!eq) continue; char k = toupper((unsigned char)x->o[i].v[j][0]); int v = (int)(evi(eq + 1) & 255); if(k == 'R') c.r = v; else if(k == 'G') c.g = v; else if(k == 'B') c.b = v; }
    return c;
  }
  return c;
}
static const char *cstr(const char *tx, Col *rgb, Buf *b){
  int hf = rgb ? 1 : S.hasfg, hb = S.hasbg; Col f = rgb ? *rgb : S.fg;
  if(!S.ansi || (!hf && !hb)){ bs(b, tx); return b->s; }
  char t[64]; if(hf){ snprintf(t, sizeof t, "\x1b[38;2;%d;%d;%dm", f.r, f.g, f.b); bs(b, t); }
  if(hb){ snprintf(t, sizeof t, "\x1b[48;2;%d;%d;%dm", S.bg.r, S.bg.g, S.bg.b); bs(b, t); }
  bs(b, tx); bs(b, "\x1b[0m"); return b->s;
}
static void do_console(Stmt *x, Scope *sc){
  const char *w = hw_(x, 1); Col rgb; int hr = 0;
  for(int i = 0; i < x->no; i++) if(!strcasecmp(x->o[i].v[0], "color")){ rgb = cs_(&x->o[i], 1); hr = 1; break; }
  const char *tq = NULL; for(int i = 0; i < x->q.n; i++) if(!strncasecmp(x->q.v[i], "text;", 5)){ tq = x->q.v[i]; break; }
  char *tx = tq ? fm(tq + 5) : xs(""); Buf b = {0};
  if(!strcasecmp(w, "init")){
    if(hr){ S.fg = rgb; S.hasfg = 1; }
    for(int i = 0; i < x->no; i++) if(!strcasecmp(x->o[i].v[0], "bg")){ S.bg = cs_(&x->o[i], 1); S.hasbg = 1; break; }
    outn(esc("\x1b[2J\x1b[H"));
  } else if(!strcasecmp(w, "clear")) outn(esc("\x1b[2J\x1b[H"));
  else if(!strcasecmp(w, "refresh")){}
  else if(!strcasecmp(w, "at")){ const char *r = og(x, "row"), *c = og(x, "col"); char t[64]; snprintf(t, sizeof t, "\x1b[%lld;%lldH", (r ? evi(r) : 0) + 1, (c ? evi(c) : 0) + 1); outn(esc(t)); }
  else if(!strcasecmp(w, "print")){ cstr(tx, hr ? &rgb : NULL, &b); out(b.s ? b.s : ""); }
  else if(!strcasecmp(w, "put")){ cstr(tx, hr ? &rgb : NULL, &b); outn(b.s ? b.s : ""); }
  else if(!strcasecmp(w, "beep")) outn("\a");
  else if(!strcasecmp(w, "read")){
    const char *n = og(x, "into"); if(!n || !shas(sc, n)) lerr("Undeclared variable: %s", n ? n : "(null)");
    const char *pm = og(x, "prompt"); if(pm && *pm){ char *p = fm(pm); Buf pb = {0}; bs(&pb, p); bc(&pb, ' '); Buf o2 = {0}; cstr(pb.s, hr ? &rgb : NULL, &o2); outn(o2.s); free(p); }
    char *r = readline_nogil(); if(!r) lerr("End of input (EOF)");
    Val cur = sget(sc, n); int numeric = (sty(sc, n) >= 0 && sty(sc, n) != 6) || isnumv(cur); vdrop(cur);
    double nv = 0; if(numeric){ char *e; nv = strpbrk(r, ".eE") ? strtod(r, &e) : (double)strtoll(r, &e, 10); if(*e) nv = 0; }
    ssetv(sc, n, numeric ? Vn(nv) : Vs(r)); free(r);
  } else lerr("Console: unknown operation: %s", w);
  free(tx);
}
static void do_mem(Stmt *x){
  const char *w = hw_(x, 1); const char *as = og(x, "addr"); ll a = as ? evi(as) : 0;
  const char *ts = og(x, "type"); char *ty = lowerdup(ts ? ts : ""); const char *vs_ = og(x, "value");
  const char *szs = og(x, "size");
  if(!strcasecmp(w, "write") && *ty){
    int f32 = !strcmp(ty, "f32"), f64 = !strcmp(ty, "f64"); if(!f32 && !f64) lerr("Mem: Type must be f32 or f64");
    double v = vs_ ? evd(vs_) : 0; int n = f32 ? 4 : 8; mc(a); mc(a + n - 1);
    if(f32){ float f = (float)v; memcpy(S.MEM + a, &f, 4); } else memcpy(S.MEM + a, &v, 8);
  } else if(!strcasecmp(w, "read") && *ty){
    int f32 = !strcmp(ty, "f32"), f64 = !strcmp(ty, "f64"); if(!f32 && !f64) lerr("Mem: Type must be f32 or f64");
    int n = f32 ? 4 : 8; mc(a); mc(a + n - 1); double v; if(f32){ float f; memcpy(&f, S.MEM + a, 4); v = f; } else memcpy(&v, S.MEM + a, 8);
    into(x, Vn(v));
  } else if(!strcasecmp(w, "write")){
    ll v = vs_ ? dint(evd(vs_)) : 0, sz = szs ? evi(szs) : 1;
    for(ll i = 0; i < sz; i++){ mc(a + i); S.MEM[a + i] = i < 8 ? (v >> (8 * i)) & 255 : (v < 0 ? 255 : 0); }
  } else if(!strcasecmp(w, "read")){
    ll sz = szs ? evi(szs) : 1; ull v = 0;
    for(ll i = sz - 1; i >= 0; i--){ mc(a + i); v = v * 256 + S.MEM[a + i]; }
    into(x, Vn((double)v));
  } else if(!strcasecmp(w, "fill")){
    const char *ls = og(x, "len"); ll n = ls ? evi(ls) : 0, v = vs_ ? dint(evd(vs_)) : 0; mc(a); mc(a + n - 1);
    for(ll i = 0; i < n; i++) S.MEM[a + i] = v & 255;
  } else if(!strcasecmp(w, "copy")){
    const char *fs = og(x, "from"), *tos = og(x, "to"), *ls = og(x, "len"); ll f = fs ? evi(fs) : 0, t = tos ? evi(tos) : 0, n = ls ? evi(ls) : 0;
    mc(f); mc(f + n - 1); mc(t); mc(t + n - 1); memmove(S.MEM + t, S.MEM + f, n);
  } else if(!strcasecmp(w, "writetext")){
    Val v = valof(x); char *tx = jstr(v); vdrop(v); size_t n = strlen(tx);
    for(size_t i = 0; i <= n; i++){ mc(a + (ll)i); S.MEM[a + i] = i < n ? (unsigned char)tx[i] : 0; } free(tx);
  } else if(!strcasecmp(w, "load")){      /* archivo real -> RAM emulada */
    const char *f = og(x, "file"); if(!f) lerr("Mem:Load needs File"); char *fe = fm(f), *p = fpath(fe, "Mem:Load");
    size_t n; char *d = readfile(p, &n); if(!d) lerr("Mem:Load: could not read %s", fe);
    const char *ls = og(x, "len"); if(ls) { ll m = evi(ls); if((ll)n > m) n = m; }
    if(n){ mc(a); mc(a + (ll)n - 1); memcpy(S.MEM + a, d, n); } free(d); free(p); free(fe);
  } else if(!strcasecmp(w, "dump")){      /* RAM emulada -> archivo real */
    const char *f = og(x, "file"), *ls = og(x, "len"); if(!f) lerr("Mem:Dump needs File"); ll n = ls ? evi(ls) : 0; mc(a); if(n > 0) mc(a + n - 1);
    char *fe = fm(f), *p = fpath(fe, "Mem:Dump"); if(writefile(p, (char *)S.MEM + a, n > 0 ? n : 0, "wb")) lerr("Mem:Dump: could not write %s", fe); free(p); free(fe);
  } else if(!strcasecmp(w, "find")){          /* Mem:Find Addr;a Len;n Text;"abc" (or Value;byte) Into;v -> address or -1 */
    Val pv = valof(x); char *pat; size_t pl;
    if(pv.t == T_NUM || pv.t == T_BOOL){ pat = xm(1); pat[0] = (char)((ll)dint(pv.u.n) & 255); pl = 1; } else { pat = jstr(pv); pl = strlen(pat); }
    vdrop(pv); const char *fl_ = og(x, "len"); ll n = fl_ ? evi(fl_) : (ll)S.memsz - a; if(n > (ll)S.memsz - a) n = (ll)S.memsz - a; mc(a); ll res = -1;
    if(pl) for(ll i = 0; i + (ll)pl <= n; i++) if(!memcmp(S.MEM + a + i, pat, pl)){ res = a + i; break; }
    free(pat); into(x, Vn((double)res));
  } else if(!strcasecmp(w, "cmp")){           /* Mem:Cmp A;x B;y Len;n Into;v -> -1, 0, 1 */
    const char *as2 = og(x, "a"), *bs2 = og(x, "b"), *ls = og(x, "len"); ll pa = as2 ? evi(as2) : 0, pb = bs2 ? evi(bs2) : 0, n = ls ? evi(ls) : 0;
    if(n > 0){ mc(pa); mc(pa + n - 1); mc(pb); mc(pb + n - 1); }
    int r = n > 0 ? memcmp(S.MEM + pa, S.MEM + pb, (size_t)n) : 0; into(x, Vn(r < 0 ? -1 : r > 0 ? 1 : 0));
  } else if(!strcasecmp(w, "hash")){          /* Mem:Hash Addr;a Len;n Into;v -> CRC-32 */
    const char *ls = og(x, "len"); ll n = ls ? evi(ls) : 0; if(n > 0){ mc(a); mc(a + n - 1); }
    into(x, Vn((double)crc32u(0, S.MEM + a, n > 0 ? (size_t)n : 0)));
  } else if(!strcasecmp(w, "swap")){          /* Mem:Swap From;a To;b Len;n */
    const char *fs = og(x, "from"), *tos = og(x, "to"), *ls = og(x, "len"); ll f = fs ? evi(fs) : 0, t = tos ? evi(tos) : 0, n = ls ? evi(ls) : 0;
    if(n > 0){ mc(f); mc(f + n - 1); mc(t); mc(t + n - 1); if(f < t + n && t < f + n) lerr("Mem:Swap: the blocks overlap");
      for(ll i = 0; i < n; i++){ unsigned char c = S.MEM[f + i]; S.MEM[f + i] = S.MEM[t + i]; S.MEM[t + i] = c; } }
  } else if(!strcasecmp(w, "readtext")){      /* Mem:ReadText Addr;a [Len;max] Into;v -> text up to the first 0 byte */
    const char *ls = og(x, "len"); ll mx = ls ? evi(ls) : (ll)S.memsz - a; if(mx > (ll)S.memsz - a) mx = (ll)S.memsz - a; mc(a); ll n = 0;
    while(n < mx && S.MEM[a + n]) n++;
    into(x, Vsn((const char *)S.MEM + a, (int)n));
  } else if(!strcasecmp(w, "setbit") || !strcasecmp(w, "clearbit") || !strcasecmp(w, "togglebit") || !strcasecmp(w, "testbit")){
    const char *bi = og(x, "bit"); ll bit = bi ? evi(bi) : 0; ll ba = a + (bit >> 3); int m = 1 << (bit & 7); mc(ba);
    if(!strcasecmp(w, "setbit")) S.MEM[ba] |= m; else if(!strcasecmp(w, "clearbit")) S.MEM[ba] &= ~m;
    else if(!strcasecmp(w, "togglebit")) S.MEM[ba] ^= m; else into(x, Vn((S.MEM[ba] & m) ? 1 : 0));
  } else if(!strcasecmp(w, "xor") || !strcasecmp(w, "and") || !strcasecmp(w, "or") || !strcasecmp(w, "not")){
    const char *ls = og(x, "len"); ll n = ls ? evi(ls) : 0, v = vs_ ? dint(evd(vs_)) : 0; if(n > 0){ mc(a); mc(a + n - 1); }
    for(ll i = 0; i < n; i++){ unsigned char *b = S.MEM + a + i; if(!strcasecmp(w, "xor")) *b ^= (unsigned char)v; else if(!strcasecmp(w, "and")) *b &= (unsigned char)v; else if(!strcasecmp(w, "or")) *b |= (unsigned char)v; else *b = (unsigned char)~*b; }
  } else lerr("Mem: use Write, Read, Fill, Copy, WriteText, Load, Dump, Find, Cmp, Hash, Swap, ReadText, SetBit, ClearBit, ToggleBit, TestBit, Xor, And, Or or Not");
  free(ty);
}
static void do_disk(Stmt *x){
  const char *w = hw_(x, 1); const char *fo = og(x, "file"); char *f = fm(fo ? fo : ""); Dict *VD = DP(S.vd);
  if(!strcasecmp(w, "list")){
    const char *po = og(x, "prefix"); char *pre = fm(po ? po : ""); Val l = Vl();
    for(int i = 0; i < VD->n; i++) if(!strncmp(VD->k[i], pre, strlen(pre))) lpush(LP(l), Vs(VD->k[i]));
    SI *t2 = xm(LP(l)->n * sizeof(SI) + 1); for(int i = 0; i < LP(l)->n; i++){ t2[i].v = LP(l)->a[i]; t2[i].i = i; } qsort(t2, LP(l)->n, sizeof(SI), sicmp);
    for(int i = 0; i < LP(l)->n; i++) LP(l)->a[i] = t2[i].v; free(t2); free(pre); into(x, l);
  } else if(!strcasecmp(w, "read")){ Val *p = dget(VD, f); if(!p) lerr("Disk: not found: %s", f); into(x, vref(*p)); }
  else if(!strcasecmp(w, "exists")) into(x, Vn(dget(VD, f) ? 1 : 0));
  else if(!strcasecmp(w, "size")){ Val *p = dget(VD, f); into(x, Vn(p ? SP(*p)->len : -1)); }
  else if(!strcasecmp(w, "usage")){ double s = 0; for(int i = 0; i < VD->n; i++) s += SP(VD->v[i])->len; into(x, Vn(s)); }
  else if(!strcasecmp(w, "write") || !strcasecmp(w, "append")){
    if(!*f) lerr("Disk: missing File"); Val v = valof(x); char *t = jstrdup_nl(v); vdrop(v);
    Val *p = dget(VD, f); Buf b = {0}; if(p && !strcasecmp(w, "append")) bs(&b, CSTR(*p)); bs(&b, t);
    char *r = btake(&b); dset(VD, f, Vs(r)); free(r); free(t); vdsave();
  } else if(!strcasecmp(w, "delete")){ dremove(VD, f); vdsave(); }
  else if(!strcasecmp(w, "rename") || !strcasecmp(w, "copy")){
    const char *to = og(x, "to"); char *t = fm(to ? to : ""); if(!*t) lerr("Disk: missing To");
    Val *p = dget(VD, f); if(!p) lerr("Disk: not found: %s", f); dset(VD, t, vref(*p)); if(!strcasecmp(w, "rename")) dremove(VD, f); free(t); vdsave();
  } else if(!strcasecmp(w, "format")){ while(VD->n) dremove(VD, VD->k[0]); vdsave(); }
  else lerr("Disk: use List, Read, Exists, Size, Usage, Write, Append, Delete, Rename, Copy or Format");
  free(f);
}
static void do_host(Stmt *x){
  const char *w = hw_(x, 1); const char *fo = og(x, "file"); char *f = fm(fo ? fo : ""); char *p = fpath(f, "Host");
  struct stat st; int ex_ = stat(p, &st) == 0;
  if(!strcasecmp(w, "list")){      /* Host:List [File;subfolder] [Mark;1: folders end in /] [Filter;.log+] [Hidden;1] */
    const char *base = *f ? p : S.root_dir; const char *mk = og(x, "mark"), *ft = og(x, "filter"), *hd = og(x, "hidden");
    int mark = mk && evi(mk), hidden = hd && evi(hd); char *flt = ft ? fm(ft) : NULL;
    DIR *d = opendir(base); Val l = Vl(); struct dirent *e;
    if(d){ while((e = readdir(d))){
      if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      if(!hidden && e->d_name[0] == '.') continue;
      char *fp = pjoin(base, e->d_name); struct stat s2; int isd = !stat(fp, &s2) && S_ISDIR(s2.st_mode); free(fp);
      if(flt && *flt && !isd){ size_t nl = strlen(e->d_name), fl = strlen(flt); if(nl < fl || strcasecmp(e->d_name + nl - fl, flt)) continue; }
      if(mark && isd){ char *nm = xm(strlen(e->d_name) + 2); sprintf(nm, "%s/", e->d_name); lpush(LP(l), Vs(nm)); free(nm); }
      else lpush(LP(l), Vs(e->d_name));
    } closedir(d); }
    free(flt);
    SI *t2 = xm(LP(l)->n * sizeof(SI) + 1); for(int i = 0; i < LP(l)->n; i++){ t2[i].v = LP(l)->a[i]; t2[i].i = i; } qsort(t2, LP(l)->n, sizeof(SI), sicmp);
    for(int i = 0; i < LP(l)->n; i++) LP(l)->a[i] = t2[i].v; free(t2); into(x, l);
  } else if(!strcasecmp(w, "read")){
    if(!ex_ || !S_ISREG(st.st_mode)) lerr("Host: not found: %s", f);
    size_t n; char *d = readfile(p, &n); if(!d) lerr("Host: could not read %s", f); Val v = Vsn(d, (int)n); free(d); into(x, v);
  } else if(!strcasecmp(w, "exists")) into(x, Vn(ex_));
  else if(!strcasecmp(w, "size")) into(x, Vn(ex_ ? (double)st.st_size : -1));
  else if(!strcasecmp(w, "write") || !strcasecmp(w, "append")){
    if(!*f) lerr("Host: missing File"); Val v = valof(x); const char *rw = og(x, "raw"); char *t = (rw && evi(rw)) ? jstr(v) : jstrdup_nl(v); vdrop(v);   /* Raw;1: keep a backslash-n as typed (needed to save source code) */
    int r = writefile(p, t, strlen(t), !strcasecmp(w, "append") ? "ab" : "wb"); free(t); if(r) lerr("Host: could not write %s: %s", f, strerror(errno));
  } else if(!strcasecmp(w, "delete")){ if(ex_ && S_ISREG(st.st_mode)) remove(p); }
  else if(!strcasecmp(w, "mkdir")){ if(mkdir(p, 0777) && errno != EEXIST) lerr("Host: mkdir failed: %s", strerror(errno)); }
  else if(!strcasecmp(w, "copy") || !strcasecmp(w, "move")){
    const char *to = og(x, "to"); if(!to) lerr("Host:%s needs To", w);
    if(!ex_ || !S_ISREG(st.st_mode)) lerr("Host: not found: %s", f);
    char *t = fm(to), *tp = fpath(t, "Host");
    if(!strcasecmp(w, "move")){
#ifdef _WIN32
      remove(tp);
#endif
      if(rename(p, tp)){ int en = errno; free(t); free(tp); lerr("Host:Move failed: %s", strerror(en)); }
    } else {
      size_t n; char *d = readfile(p, &n); if(!d){ free(t); free(tp); lerr("Host:Copy: could not read %s", f); }
      int r = writefile(tp, d, n, "wb"); free(d); if(r){ int en = errno; char tmp_[256]; snprintf(tmp_, sizeof tmp_, "%s", t); free(t); free(tp); lerr("Host:Copy: could not write %s: %s", tmp_, strerror(en)); }
    }
    free(t); free(tp);
  } else if(!strcasecmp(w, "lines")){      /* Host:Lines File;x Into;list -> list of lines (without the line ending) */
    if(!ex_ || !S_ISREG(st.st_mode)) lerr("Host: not found: %s", f);
    size_t n; char *d = readfile(p, &n); if(!d) lerr("Host: could not read %s", f);
    Val l = Vl(); size_t b = 0;
    for(size_t i = 0; i <= n; i++) if(i == n || d[i] == '\n'){
      if(i == n && b == n) break;
      size_t e2 = i; if(e2 > b && d[e2 - 1] == '\r') e2--; lpush(LP(l), Vsn(d + b, (int)(e2 - b))); b = i + 1;
    }
    free(d); into(x, l);
  } else if(!strcasecmp(w, "writelines")){  /* Host:WriteLines File;x Value;list */
    if(!*f) lerr("Host: missing File"); Val v = valof(x); if(v.t != T_LIST){ vdrop(v); lerr("Host:WriteLines needs a list"); }
    Buf b = {0}; for(int i = 0; i < LP(v)->n; i++){ char *s = jstr(LP(v)->a[i]); bs(&b, s); bc(&b, '\n'); free(s); }
    vdrop(v); int r = writefile(p, btake(&b), (size_t)b.n, "wb"); if(r) lerr("Host: could not write %s: %s", f, strerror(errno));
  } else if(!strcasecmp(w, "stat")){        /* Host:Stat File;x Into;dict -> exists, size, isdir, mtime */
    Val d = Vd(); dset(DP(d), "exists", Vn(ex_)); dset(DP(d), "size", Vn(ex_ ? (double)st.st_size : -1));
    dset(DP(d), "isdir", Vn(ex_ && S_ISDIR(st.st_mode))); dset(DP(d), "mtime", Vn(ex_ ? (double)st.st_mtime : 0)); into(x, d);
  } else if(!strcasecmp(w, "isdir")) into(x, Vn(ex_ && S_ISDIR(st.st_mode)));
  else if(!strcasecmp(w, "rmdir")){ if(ex_ && S_ISDIR(st.st_mode) && rmdir(p)) lerr("Host: rmdir failed (is the folder empty?): %s", strerror(errno)); }
  else if(!strcasecmp(w, "cwd")) into(x, Vs(S.root_dir));
  else if(!strcasecmp(w, "savejson")){      /* Host:SaveJson File;x.json Value;anything -> saves lists, dictionaries, texts and numbers */
    if(!*f) lerr("Host: missing File"); Val v = valof(x); Buf b = {0}; jdump(&b, v); vdrop(v);
    int r = writefile(p, btake(&b), (size_t)b.n, "wb"); if(r) lerr("Host: could not write %s: %s", f, strerror(errno));
  } else if(!strcasecmp(w, "loadjson")){    /* Host:LoadJson File;x.json Into;v */
    if(!ex_ || !S_ISREG(st.st_mode)) lerr("Host: not found: %s", f);
    char *tx = readfile(p, NULL); if(!tx) lerr("Host: could not read %s", f); int err = 0; Val v = jparse(tx, &err); free(tx);
    if(err){ vdrop(v); lerr("Host:LoadJson: %s is not valid JSON", f); } into(x, v);
  }
  else lerr("Host: use List, Read, Exists, Size, Write, Append, Delete, Mkdir, Copy, Move, Lines, WriteLines, Stat, IsDir, Rmdir, Cwd, SaveJson or LoadJson");
  free(f); free(p);
}

/* ======================================================================================================
 * v2.1: I2C, SPI and UART
 *   I2C:Write("Addr;0x27", "Byte;n" | "Bytes;1,2,3" | "Text;..", ["Bus;1"], ["Into;ack"])   ack = 1 if the device answered
 *   I2C:Read("Addr;0x48","Count;2",["Bus;1"],"Into;list")      error if the device does not answer
 *   I2C:Scan("Into;list",["Bus;1"])      I2C:Close
 *   SPI:Open("Bus;0","Cs;0",["Speed;1000000"],["Mode;0"])   SPI:Transfer(bytes.. ,"Into;list")   SPI:Write(bytes..)
 *   SPI:Read("Count;n","Into;list")   SPI:Close
 *   UART:Open("Port;/dev/ttyS0","Baud;115200",["Id;gps"])   UART:Write(["Id;gps"], bytes..)
 *   UART:Read(["Id;gps"],"Count;n",["Timeout;100"],["As;Text"],"Into;x")   UART:Close(["Id;gps"])
 * Without --hw they are VIRTUAL like the simulator: I2C has no devices (ack 0, empty scan), SPI and UART are loopbacks
 * (what you send comes back). With --hw --allow-pins they use the real /dev/i2c-N, /dev/spidevB.C and serial ports.
 * Real I2C/SPI: Linux only. Real UART: Linux and macOS. NOT TESTED ON REAL HARDWARE (UART was tested against a pty).
 * The kernel headers are not needed: the few ioctl numbers are written here (valid on x86, ARM, ARM64 and RISC-V).
 * ====================================================================================================== */
#ifndef _WIN32
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#endif
#define BUSMAX 4096

/* bytes from the options Byte; Bytes; Text; in the order written (same rules as the simulator) */
static int collect_bytes(Stmt *x, unsigned char *buf, int cap){
  int n = 0;
  for(int i = 0; i < x->no; i++){
    const char *k = x->o[i].v[0], *v = x->ov[i];
    if(!strcasecmp(k, "byte")){ if(n >= cap) lerr("Too many bytes (max %d)", cap); buf[n++] = (unsigned char)((long long)evd(v) & 255); }
    else if(!strcasecmp(k, "bytes")){
      Buf b = {0}; bc(&b, '['); bs(&b, v); bc(&b, ']'); char *e = btake(&b); Val l = ev(e); free(e);
      if(l.t != T_LIST){ vdrop(l); lerr("Bytes needs numbers separated by commas"); }
      for(int j = 0; j < LP(l)->n; j++){ if(n >= cap){ vdrop(l); lerr("Too many bytes (max %d)", cap); } buf[n++] = (unsigned char)((long long)numarg(LP(l)->a[j]) & 255); }
      vdrop(l);
    } else if(!strcasecmp(k, "text")){
      char *t = fm(v);
      for(char *p = t; *p; p++){
        int c = (unsigned char)*p;
        if(p[0] == '\\' && p[1]){ char q = p[1]; if(q == 'n') { c = '\n'; p++; } else if(q == 'r'){ c = '\r'; p++; } else if(q == 'f'){ c = '\f'; p++; } else if(q == 't'){ c = '\t'; p++; } else if(q == '0'){ c = 0; p++; } }
        if(n >= cap){ free(t); lerr("Too many bytes (max %d)", cap); }
        buf[n++] = (unsigned char)c;
      }
      free(t);
    }
  }
  return n;
}
static Val bytes_list(const unsigned char *b, int n){ Val l = Vl(); for(int i = 0; i < n; i++) lpush(LP(l), Vn(b[i])); return l; }
/* 1 = use the real device (needs --hw and --allow-pins), 0 = virtual */
static int bus_real(const char *who){ if(!S.hw) return 0; need_perm(S.allow_pins, who, "--allow-pins"); return 1; }
static int opt_int(Stmt *x, const char *k, int d){ const char *s = og(x, k); return s && *s ? (int)evd(s) : d; }

/* ---------------------------------------------------------------- I2C */
#ifdef __linux__
#define LG_I2C_SLAVE 0x0703
#define LG_I2C_SMBUS 0x0720
struct lg_smbus { unsigned char rw, cmd; unsigned int size; void *data; };   /* = struct i2c_smbus_ioctl_data */
static int i2c_fd[16] = {0}, i2c_have[16] = {0};
static int i2c_bus_fd(int bus){
  if(bus < 0 || bus > 15) lerr("I2C: Bus must be 0-15");
  if(!i2c_have[bus]){
    char path[64]; snprintf(path, sizeof path, "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR); if(fd < 0) lerr("I2C: could not open %s: %s (is I2C enabled, and are you root or in the i2c group?)", path, strerror(errno));
    i2c_fd[bus] = fd; i2c_have[bus] = 1;
  }
  return i2c_fd[bus];
}
static int i2c_probe(int fd, int addr){   /* same method as i2cdetect: read a byte in EEPROM ranges, "quick write" elsewhere */
  if(ioctl(fd, LG_I2C_SLAVE, addr) < 0) return 0;
  struct lg_smbus a; unsigned char d[34]; memset(&a, 0, sizeof a); memset(d, 0, sizeof d);
  if((addr >= 0x30 && addr <= 0x37) || (addr >= 0x50 && addr <= 0x5f)){ a.rw = 1; a.size = 1; a.data = d; }
  else { a.rw = 0; a.size = 0; a.data = NULL; }
  return ioctl(fd, LG_I2C_SMBUS, &a) >= 0;
}
#endif
static void do_i2c(Stmt *x){
  const char *w = hw_(x, 1); int real = bus_real("I2C"); int bus = opt_int(x, "bus", 1);
  if(!strcasecmp(w, "scan")){
    into_need(x); Val l = Vl();
#ifdef __linux__
    if(real){ int fd = i2c_bus_fd(bus); for(int a = 8; a < 120; a++) if(i2c_probe(fd, a)) lpush(LP(l), Vn(a)); }
#else
    if(real) lerr("I2C: real I2C is only available on Linux");
#endif
    into(x, l); return;
  }
  if(!strcasecmp(w, "close")){
#ifdef __linux__
    for(int b = 0; b < 16; b++) if(i2c_have[b]){ close(i2c_fd[b]); i2c_have[b] = 0; }
#endif
    return;
  }
  int isw = !strcasecmp(w, "write"), isr = !strcasecmp(w, "read");
  if(!isw && !isr) lerr("Unknown I2C operation: %s (valid: Write, Read, Scan, Close)", w);
  const char *ad = og(x, "addr"); if(!ad) lerr("I2C: Addr is missing");
  long long addr = (long long)evd(ad) & 127;
  if(isw){
    unsigned char buf[BUSMAX]; int n = collect_bytes(x, buf, BUSMAX); int ack = 0;
#ifdef __linux__
    if(real){ int fd = i2c_bus_fd(bus); if(n == 0) ack = i2c_probe(fd, (int)addr); else if(ioctl(fd, LG_I2C_SLAVE, (int)addr) >= 0) ack = write(fd, buf, n) == n; }
#else
    if(real) lerr("I2C: real I2C is only available on Linux");
#endif
    (void)n; if(og(x, "into")) into(x, Vn(ack)); return;
  }
  int cnt = opt_int(x, "count", 1); if(cnt < 1 || cnt > BUSMAX) lerr("I2C: Count must be 1-%d", BUSMAX);
  into_need(x);
#ifdef __linux__
  if(real){
    int fd = i2c_bus_fd(bus); unsigned char buf[BUSMAX];
    if(ioctl(fd, LG_I2C_SLAVE, (int)addr) < 0 || read(fd, buf, cnt) != cnt) lerr("I2C: no answer from address 0x%02X", (unsigned)addr);
    into(x, bytes_list(buf, cnt)); return;
  }
#else
  if(real) lerr("I2C: real I2C is only available on Linux");
#endif
  lerr("I2C: no answer from address 0x%02X", (unsigned)addr);   /* virtual bus: there are no devices */
}

/* ---------------------------------------------------------------- SPI */
static int spi_open_ = 0, spi_fd = -1, spi_speed = 1000000;
#ifdef __linux__
struct lg_spi_xfer { unsigned long long tx, rx; unsigned int len, speed; unsigned short delay; unsigned char bits, cs_change, tx_nbits, rx_nbits, word_delay, pad; };   /* = struct spi_ioc_transfer (32 bytes) */
typedef char lg_spi_size_check[sizeof(struct lg_spi_xfer) == 32 ? 1 : -1];
#define LG_SPI_MSG1      0x40206B00u   /* SPI_IOC_MESSAGE(1) */
#define LG_SPI_WR_MODE   0x40016B01u
#define LG_SPI_WR_BITS   0x40016B03u
#define LG_SPI_WR_SPEED  0x40046B04u
#endif
static void spi_xfer(const unsigned char *tx, unsigned char *rx, int n){
  if(!spi_open_) lerr("SPI: not open (use SPI:Open first)");
  if(spi_fd < 0){ memcpy(rx, tx, n); return; }   /* virtual: loopback (MISO wired to MOSI) */
#ifdef __linux__
  struct lg_spi_xfer t; memset(&t, 0, sizeof t);
  t.tx = (unsigned long long)(uintptr_t)tx; t.rx = (unsigned long long)(uintptr_t)rx; t.len = (unsigned)n; t.speed = (unsigned)spi_speed; t.bits = 8;
  if(ioctl(spi_fd, LG_SPI_MSG1, &t) < 0) lerr("SPI: transfer failed: %s", strerror(errno));
#endif
}
static void do_spi(Stmt *x){
  const char *w = hw_(x, 1);
  if(!strcasecmp(w, "open")){
    int real = bus_real("SPI"); if(spi_open_) lerr("SPI: already open (use SPI:Close first)");
    int bus = opt_int(x, "bus", 0), cs = opt_int(x, "cs", 0), mode = opt_int(x, "mode", 0), sp = opt_int(x, "speed", 1000000);
    if(mode < 0 || mode > 3) lerr("SPI: Mode must be 0-3");
    if(sp < 1) lerr("SPI: Speed must be positive");
    spi_speed = sp; spi_fd = -1;
    if(real){
#ifdef __linux__
      char path[64]; snprintf(path, sizeof path, "/dev/spidev%d.%d", bus, cs);
      int fd = open(path, O_RDWR); if(fd < 0) lerr("SPI: could not open %s: %s (is SPI enabled, and are you root or in the spi group?)", path, strerror(errno));
      unsigned char m = (unsigned char)mode, bits = 8; unsigned int spd = (unsigned)sp;
      if(ioctl(fd, LG_SPI_WR_MODE, &m) < 0 || ioctl(fd, LG_SPI_WR_BITS, &bits) < 0 || ioctl(fd, LG_SPI_WR_SPEED, &spd) < 0){ int e = errno; close(fd); lerr("SPI: could not configure %s: %s", path, strerror(e)); }
      spi_fd = fd;
#else
      (void)bus; (void)cs; lerr("SPI: real SPI is only available on Linux");
#endif
    }
    spi_open_ = 1; return;
  }
  if(!strcasecmp(w, "close")){
#ifndef _WIN32
    if(spi_fd >= 0) close(spi_fd);
#endif
    spi_fd = -1; spi_open_ = 0; return;
  }
  unsigned char tx[BUSMAX], rx[BUSMAX]; int n;
  if(!strcasecmp(w, "transfer") || !strcasecmp(w, "write")){
    n = collect_bytes(x, tx, BUSMAX); if(n == 0) lerr("SPI:%s needs Byte, Bytes or Text", w);
    spi_xfer(tx, rx, n);
    if(!strcasecmp(w, "transfer")){ into_need(x); into(x, bytes_list(rx, n)); }
    return;
  }
  if(!strcasecmp(w, "read")){
    n = opt_int(x, "count", 0); if(n < 1 || n > BUSMAX) lerr("SPI:Read needs Count between 1 and %d", BUSMAX);
    into_need(x); memset(tx, 0, (size_t)n); spi_xfer(tx, rx, n); into(x, bytes_list(rx, n)); return;
  }
  lerr("Unknown SPI operation: %s (valid: Open, Transfer, Write, Read, Close)", w);
}

/* ---------------------------------------------------------------- UART */
typedef struct { char id[32]; int used, fd; unsigned char q[BUSMAX]; int head, cnt; } Uart;
static Uart uarts[4];
static Uart *uart_find(const char *id, int create){
  for(int i = 0; i < 4; i++) if(uarts[i].used && !strcmp(uarts[i].id, id)) return &uarts[i];
  if(!create) return NULL;
  for(int i = 0; i < 4; i++) if(!uarts[i].used){ memset(&uarts[i], 0, sizeof uarts[i]); uarts[i].used = 1; uarts[i].fd = -1; snprintf(uarts[i].id, sizeof uarts[i].id, "%s", id); return &uarts[i]; }
  lerr("UART: at most 4 ports can be open at the same time");
}
#ifndef _WIN32
static speed_t uart_baud(int b){
  switch(b){
    case 1200: return B1200; case 2400: return B2400; case 4800: return B4800; case 9600: return B9600; case 19200: return B19200;
    case 38400: return B38400; case 57600: return B57600; case 115200: return B115200; case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
  }
  lerr("UART: unsupported Baud %d (valid: 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)", b);
}
#endif
static char *uart_id(Stmt *x){ const char *s = og(x, "id"); return s ? fm(s) : xs("uart0"); }
static void do_uart(Stmt *x){
  const char *w = hw_(x, 1); char *id = uart_id(x);
  if(!strcasecmp(w, "open")){
    int real = bus_real("UART"); if(uart_find(id, 0)) lerr("UART: '%s' is already open", id);
    int baud = opt_int(x, "baud", 9600); Uart *u = uart_find(id, 1);
    if(real){
#ifndef _WIN32
      const char *po = og(x, "port"); if(!po){ u->used = 0; lerr("UART:Open needs Port (for example Port;/dev/ttyS0)"); }
      char *port = fm(po); speed_t sp;
      { Uart *keep = u; keep->used = 0; sp = uart_baud(baud); keep->used = 1; }
      int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
      if(fd < 0){ u->used = 0; char *m = xs(strerror(errno)); lerr("UART: could not open %s: %s", port, m); }
      struct termios t; if(tcgetattr(fd, &t) < 0){ close(fd); u->used = 0; lerr("UART: %s is not a serial port", port); }
      cfmakeraw(&t); cfsetispeed(&t, sp); cfsetospeed(&t, sp); t.c_cflag |= CLOCAL | CREAD; t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
      if(tcsetattr(fd, TCSANOW, &t) < 0){ close(fd); u->used = 0; lerr("UART: could not configure %s", port); }
      u->fd = fd; free(port);
#else
      u->used = 0; lerr("UART: real serial ports are not available on Windows yet");
#endif
    }
    return;
  }
  Uart *u = uart_find(id, 0); if(!u && strcasecmp(w, "close")) lerr("UART: '%s' is not open (use UART:Open first)", id);
  if(!strcasecmp(w, "close")){
    if(u){
#ifndef _WIN32
      if(u->fd >= 0) close(u->fd);
#endif
      u->used = 0; u->fd = -1;
    }
    return;
  }
  if(!strcasecmp(w, "write")){
    unsigned char buf[BUSMAX]; int n = collect_bytes(x, buf, BUSMAX);
    if(u->fd >= 0){
#ifndef _WIN32
      int off = 0; while(off < n){ ssize_t r = write(u->fd, buf + off, (size_t)(n - off)); if(r < 0){ if(errno == EAGAIN){ fd_set wf; FD_ZERO(&wf); FD_SET(u->fd, &wf); struct timeval tv = { 0, 100000 }; select(u->fd + 1, NULL, &wf, NULL, &tv); continue; } lerr("UART: write failed: %s", strerror(errno)); } off += (int)r; }
#endif
    } else {
      if(u->cnt + n > BUSMAX) lerr("UART: virtual buffer full (%d bytes)", BUSMAX);
      for(int i = 0; i < n; i++) u->q[(u->head + u->cnt++) % BUSMAX] = buf[i];
    }
    return;
  }
  if(!strcasecmp(w, "read")){
    int cnt = opt_int(x, "count", 0), tmo = opt_int(x, "timeout", 100); unsigned char buf[BUSMAX]; int got = 0;
    if(cnt < 1 || cnt > BUSMAX) lerr("UART:Read needs Count between 1 and %d", BUSMAX);
    into_need(x);
    if(u->fd >= 0){
#ifndef _WIN32
      struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
      while(got < cnt){
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        long el = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000; long left = tmo - el; if(left < 0) left = 0;
        fd_set rf; FD_ZERO(&rf); FD_SET(u->fd, &rf); struct timeval tv = { left / 1000, (left % 1000) * 1000 };
        int pr = select(u->fd + 1, &rf, NULL, NULL, &tv);   /* select, not poll: poll() is unreliable with tty devices on macOS */
        if(pr > 0){ ssize_t r = read(u->fd, buf + got, (size_t)(cnt - got)); if(r > 0) got += (int)r; else if(r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) break; }
        else if(pr == 0) break;
      }
#endif
    } else {
      while(got < cnt && u->cnt > 0){ buf[got++] = u->q[u->head]; u->head = (u->head + 1) % BUSMAX; u->cnt--; }
    }
    const char *as = og(x, "as");
    if(as && !strcasecmp(as, "text")) into(x, Vsn((const char *)buf, got)); else into(x, bytes_list(buf, got));
    return;
  }
  lerr("Unknown UART operation: %s (valid: Open, Write, Read, Close)", w);
}
static FB *surf_get(const char *id){ for(int i = 0; i < S.nsurf; i++) if(S.surf[i].id && !strcmp(S.surf[i].id, id)) return S.surf[i].fb; return NULL; }
static void do_scr(Stmt *x){
  const char *p = NULL; static const char *OPS[] = {"pixel","rect","line","circle","clear","resolution","text","blit","get","show",NULL};
  for(int i = 0; i < x->h.n && !p; i++) for(int j = 0; OPS[j]; j++) if(!strcasecmp(x->h.v[i], OPS[j])){ p = OPS[j]; break; }
  if(!p) lerr("Unknown screen operation");
  if(S.autowin && strcmp(p, "show")) win_ensure();
  Val a[8]; int na = x->a.n < 8 ? x->a.n : 8; for(int i = 0; i < na; i++) a[i] = ev(x->a.v[i]);
#define A(i) ((i) < na ? dint(tonum(a[i])) : (lerr("Missing arguments in %s", p), 0))
  if(!strcmp(p, "resolution")){
    double w = 320, hh = 180;
    if(x->no && !strcasecmp(x->o[0].v[0], "set")){ w = x->o[0].n > 1 ? evd(x->o[0].v[1]) : 320; hh = x->o[0].n > 2 ? evd(x->o[0].v[2]) : 180; }
    else for(int i = 0; i < x->no; i++){ if(!strcasecmp(x->o[i].v[0], "w") && x->o[i].n > 1) w = evd(x->o[i].v[1]); if(!strcasecmp(x->o[i].v[0], "h") && x->o[i].n > 1) hh = evd(x->o[i].v[1]); }
    S.fb = fb_new(w, hh); return;
  }
  const char *tg = og(x, "target"); if(tg && !surf_get(tg)) lerr("Surface: not found: %s", tg);
  if(!S.fb) S.fb = fb_new(320, 180);
  if(!strcmp(p, "show")){
    if(S.win.on){ present(); return; }
    const char *m = og(x, "mode"); int ansi = m && *m ? !strcasecmp(m, "ansi") : S.ansi;
    char *r = fb_render(S.fb, ansi); Buf b = {0}; if(S.ansi) bs(&b, S.showfirst ? "\x1b[2J\x1b[H" : "\x1b[H"); bs(&b, r);
    S.showfirst = 0; out(btake(&b)); free(r); return;
  }
  FB *fb = tg ? surf_get(tg) : S.fb; Col c = col_(x); const char *st = og(x, "style"); int line = st && !strcasecmp(st, "line");
  if(!strcmp(p, "clear")) fb_rect(fb, 0, 0, fb->w, fb->h, c);
  else if(!strcmp(p, "pixel")) fb_put(fb, A(0), A(1), c);
  else if(!strcmp(p, "rect")){
    ll X = A(0), Y = A(1), W = A(2), H = A(3);
    if(line){ fb_line(fb, X, Y, X + W - 1, Y, c); fb_line(fb, X, Y + H - 1, X + W - 1, Y + H - 1, c); fb_line(fb, X, Y, X, Y + H - 1, c); fb_line(fb, X + W - 1, Y, X + W - 1, Y + H - 1, c); }
    else fb_rect(fb, X, Y, W, H, c);
  } else if(!strcmp(p, "circle")) fb_circle(fb, A(0), A(1), A(2), c, !line);
  else if(!strcmp(p, "line")) fb_line(fb, A(0), A(1), A(2), A(3), c);
  else if(!strcmp(p, "get")){
    Col g = fb_get(fb, A(0), A(1)); const char *to = og(x, "into"); if(!to || !shas(T->cur, to)) lerr("Undeclared variable: %s", to ? to : "(null)");
    Val l = Vl(); lpush(LP(l), Vn(g.r)); lpush(LP(l), Vn(g.g)); lpush(LP(l), Vn(g.b)); ssetv(T->cur, to, l);
  } else if(!strcmp(p, "text")){
    const char *tq = NULL; for(int i = 0; i < x->q.n; i++) if(!strncasecmp(x->q.v[i], "text;", 5)){ tq = x->q.v[i]; break; }
    const char *sz = og(x, "size"); ll sc = 1; if(sz && *sz){ sc = (ll)nearbyint(evd(sz) / 8); if(sc < 1) sc = 1; }
    char *tx = tq ? fm(tq + 5) : xs(""); fb_text(fb, A(0), A(1), tx, c, sc); free(tx);
  } else if(!strcmp(p, "blit")){
    const char *sn = og(x, "src"); if(!sn) lerr("Screen:Blit needs Src");
    FB *sr = !strcmp(sn, "screen") ? S.fb : surf_get(sn); if(!sr) lerr("Surface: not found: %s", sn);
    const char *bw = og(x, "w"), *bh = og(x, "h"); fb_blit(fb, sr, A(0), A(1), bw ? evi(bw) : -1, bh ? evi(bh) : -1);
  }
  for(int i = 0; i < na; i++) vdrop(a[i]);
#undef A
}


/* ================================================================ window (browser view): HTTP server + events */
static pthread_mutex_t WM = PTHREAD_MUTEX_INITIALIZER;   /* protects front buffer, UI state and the event queue */
static struct { Ev q[256]; int h, t; } EQ;
static sock_t WLS = SOCK_BAD;
static pthread_cond_t EQC = PTHREAD_COND_INITIALIZER;   /* v2.1: wakes the event thread at once */
static void eq_push(const Ev *e){
  pthread_mutex_lock(&WM);
  if(e->kind == EV_MOUSE && !strcasecmp(e->sub, "move") && EQ.h != EQ.t){
    Ev *l = &EQ.q[(EQ.t + 255) % 256]; if(l->kind == EV_MOUSE && !strcasecmp(l->sub, "move")){ *l = *e; pthread_mutex_unlock(&WM); return; }
  }
  int n = (EQ.t + 1) % 256; if(n != EQ.h){ EQ.q[EQ.t] = *e; EQ.t = n; pthread_cond_signal(&EQC); } pthread_mutex_unlock(&WM); }
/* the event thread sleeps here (without the GIL) until an event arrives or 3 ms pass */
static void ev_wait(void){
  pthread_mutex_unlock(&GIL);
  pthread_mutex_lock(&WM);
  if(EQ.h == EQ.t){ struct timeval tv; gettimeofday(&tv, NULL); struct timespec ts; ts.tv_sec = tv.tv_sec; ts.tv_nsec = tv.tv_usec * 1000L + 3000000L; if(ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; } pthread_cond_timedwait(&EQC, &WM, &ts); }
  pthread_mutex_unlock(&WM);
  gil_lock();
}
static void push_edge(int n, int v){ Ev e; memset(&e, 0, sizeof e); e.kind = EV_PIN; snprintf(e.id, sizeof e.id, "%d", n); snprintf(e.sub, sizeof e.sub, "%s", v ? "rising" : "falling"); e.mx = now(); eq_push(&e); }
static int eq_pop(Ev *e){ int r = 0; pthread_mutex_lock(&WM); if(EQ.h != EQ.t){ *e = EQ.q[EQ.h]; EQ.h = (EQ.h + 1) % 256; r = 1; } pthread_mutex_unlock(&WM); return r; }

static void present(void){
  if(!S.win.on || !S.fb) return;
  size_t n = (size_t)S.fb->w * S.fb->h * 3;
  pthread_mutex_lock(&WM);
  if(S.win.fw != S.fb->w || S.win.fh != S.fb->h || !S.win.fd){ S.win.fd = xr(S.win.fd, n); S.win.fw = S.fb->w; S.win.fh = S.fb->h; }
  memcpy(S.win.fd, S.fb->d, n); S.win.fver++; S.win.lastp = now();
  pthread_mutex_unlock(&WM);
}
static void present_maybe(void){ if(S.win.on && now() - S.win.lastp > 0.1) present(); }
static void win_sound(double f, double ms){
  if(!S.win.on) return;
  pthread_mutex_lock(&WM);
  size_t l = strlen(S.win.snd);
  if(l < 200) snprintf(S.win.snd + l, sizeof S.win.snd - l, "%s%d,%d", l ? ";" : "", (int)f, (int)ms);
  pthread_mutex_unlock(&WM);
}

static const char PAGE_HTML[] =
"<!doctype html>\n"
"<html><head><meta charset=\"utf-8\"><title>Logl</title>\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<style>\n"
":root{--bg:#10151c;--fg:#e6edf3;--mut:#8b98a5;--acc:#4ade80;--pnl:#18202a;--bd:#2a3644}\n"
":root.light{--bg:#f4f6f8;--fg:#1b232c;--mut:#5b6875;--acc:#16a34a;--pnl:#ffffff;--bd:#cfd8e0}\n"
"html,body{margin:0;height:100%}\n"
"body{display:flex;flex-direction:column;background:var(--bg);color:var(--fg);font:14px system-ui,Segoe UI,sans-serif}\n"
"#bar{display:flex;align-items:center;gap:10px;padding:6px 10px;background:var(--pnl);border-bottom:1px solid var(--bd)}\n"
"#ttl{flex:1;font-weight:600}\n"
"button{font:inherit;color:var(--fg);background:var(--pnl);border:1px solid var(--bd);border-radius:6px;padding:5px 12px;cursor:pointer}\n"
"button:hover{border-color:var(--acc)}\n"
"button:disabled{opacity:.5;cursor:default}\n"
"#start{background:var(--acc);color:#101820;border-color:var(--acc);font-weight:600}\n"
"#ui{padding:10px;display:flex;flex-wrap:wrap;gap:10px;align-items:center}\n"
"#ui:empty{display:none}\n"
".row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}\n"
"label{display:flex;gap:8px;align-items:center}\n"
"input[type=text],select{font:inherit;color:var(--fg);background:var(--pnl);border:1px solid var(--bd);border-radius:6px;padding:4px 8px}\n"
"#stage{flex:1;display:flex;align-items:center;justify-content:center;min-height:0;overflow:hidden}\n"
"canvas{image-rendering:pixelated;background:#000;display:none;touch-action:none;outline:none}\n"
"#ui{max-height:72vh;overflow:auto}\n"
"select.lst{min-width:12em}\n"
".lstl{align-items:flex-start}\n"
"progress{width:12em}\n"
".ttl2{flex-basis:100%;font-size:1.15em;font-weight:700}\n"
".sep{flex-basis:100%;height:1px;background:var(--bd)}\n"
".brk{flex-basis:100%;height:0}\n"
".ed{flex:1 1 100%;display:flex;flex-direction:column;min-width:0;max-width:100%}\n"
".edcap{font-weight:600;margin-bottom:4px;color:var(--mut)}\n"
".edbox{display:flex;height:25em;border:1px solid var(--bd);border-radius:6px;background:var(--pnl);overflow:hidden;font:13px/1.5 ui-monospace,Consolas,Menlo,monospace}\n"
".gut{margin:0;padding:8px 8px 30px 10px;text-align:right;color:var(--mut);background:rgba(127,127,127,.08);overflow:hidden;user-select:none;min-width:3ch;font:inherit}\n"
".edwr{position:relative;flex:1;min-width:0}\n"
".edwr textarea,.edwr .hl{position:absolute;inset:0;margin:0;padding:8px 8px 30px 8px;border:0;font:inherit;white-space:pre;tab-size:4;box-sizing:border-box;width:100%;height:100%}\n"
".edwr textarea{resize:none;outline:none;background:transparent;color:var(--fg);z-index:1;overflow:auto}\n"
".edwr .hl{overflow:hidden;pointer-events:none;color:var(--fg);display:none}\n"
".syn .hl{display:block}\n"
".syn textarea{color:transparent;caret-color:var(--fg)}\n"
".tc{color:#6a9955}.ts{color:#d19a66}.th{color:#c678dd}.tk{color:#4ade80;font-weight:600}.tn{color:#56b6c2}.tp{color:var(--mut)}\n"
":root.light .tk{color:#15803d}\n"
"</style></head>\n"
"<body>\n"
"<div id=\"bar\"><span id=\"ttl\">Logl</span><button id=\"start\">START</button></div>\n"
"<div id=\"ui\"></div>\n"
"<div id=\"stage\"><canvas id=\"cv\" tabindex=\"0\"></canvas></div>\n"
"<script>\n"
"const K='%%TOKEN%%';\n"
"const $=id=>document.getElementById(id);\n"
"const sleep=ms=>new Promise(r=>setTimeout(r,ms));\n"
"const cv=$('cv'),cx=cv.getContext('2d');\n"
"let img=null,fw=0,fh=0,fv=0,uv=0;\n"
"\n"
"/* ---- events to the interpreter (batched, strictly ordered) ---- */\n"
"let evq=[],busy=false;\n"
"function send(ev){\n"
"  if(ev[0]==='c'&&ev[1]==='move'){\n"
"    for(let i=evq.length-1;i>=0;i--){ if(evq[i][0]==='c'&&evq[i][1]==='move'){evq.splice(i,1);break;} }\n"
"  }\n"
"  evq.push(ev); pump();\n"
"}\n"
"async function pump(){\n"
"  if(busy||!evq.length)return;\n"
"  busy=true;\n"
"  const b=evq.splice(0).map(e=>e.map(x=>encodeURIComponent(x)).join(',')).join(';');\n"
"  try{await fetch('/ev?k='+K+'&b='+b);}catch(e){}\n"
"  busy=false; pump();\n"
"}\n"
"\n"
"/* ---- canvas ---- */\n"
"function fit(){\n"
"  if(!fw)return;\n"
"  const st=$('stage'),aw=st.clientWidth,ah=st.clientHeight;\n"
"  let s=Math.min(aw/fw,ah/fh); if(s>=1)s=Math.floor(s);\n"
"  cv.style.width=Math.floor(fw*s)+'px'; cv.style.height=Math.floor(fh*s)+'px';\n"
"}\n"
"addEventListener('resize',fit);\n"
"function draw(b,w,h){\n"
"  if(w!==fw||h!==fh){fw=w;fh=h;cv.width=w;cv.height=h;img=cx.createImageData(w,h);cv.style.display='block';fit();}\n"
"  const d=img.data;\n"
"  for(let i=0,j=0,n=w*h*3;i<n;i+=3,j+=4){d[j]=b[i];d[j+1]=b[i+1];d[j+2]=b[i+2];d[j+3]=255;}\n"
"  cx.putImageData(img,0,0);\n"
"}\n"
"function pos(e){\n"
"  const r=cv.getBoundingClientRect();\n"
"  return [Math.floor((e.clientX-r.left)*fw/r.width),Math.floor((e.clientY-r.top)*fh/r.height)];\n"
"}\n"
"cv.addEventListener('pointermove',e=>{const p=pos(e);send(['c','move',p[0],p[1],e.buttons]);});\n"
"cv.addEventListener('pointerdown',e=>{\n"
"  cv.focus(); try{cv.setPointerCapture(e.pointerId);}catch(_){}\n"
"  const p=pos(e); send(['c','down',p[0],p[1],e.button]); e.preventDefault();\n"
"});\n"
"cv.addEventListener('pointerup',e=>{\n"
"  const p=pos(e); send(['c','up',p[0],p[1],e.button]);\n"
"});\n"
"\n"
"/* ---- keyboard ---- */\n"
"function typing(){const a=document.activeElement;return a&&(a.tagName==='INPUT'||a.tagName==='SELECT'||a.tagName==='TEXTAREA');}\n"
"addEventListener('keydown',e=>{\n"
"  if(typing())return;\n"
"  send(['k','down',e.key]);\n"
"  if(e.key.indexOf('Arrow')===0||e.key===' ')e.preventDefault();\n"
"});\n"
"addEventListener('keyup',e=>{ if(typing())return; send(['k','up',e.key]); });\n"
"$('start').onclick=()=>{send(['s','press']);$('start').blur();};\n"
"addEventListener('pagehide',()=>{try{navigator.sendBeacon('/bye?k='+K);}catch(e){}});\n"
"\n"
"/* ---- sound ---- */\n"
"let ac=null;\n"
"function unlock(){\n"
"  if(!ac){try{ac=new (window.AudioContext||window.webkitAudioContext)();}catch(e){}}\n"
"  if(ac&&ac.state==='suspended')ac.resume();\n"
"}\n"
"addEventListener('pointerdown',unlock);addEventListener('keydown',unlock);\n"
"function snd(s){\n"
"  unlock(); if(!ac)return;\n"
"  s.split(';').forEach(p=>{\n"
"    const a=p.split(',').map(Number); if(!a[0]||!a[1])return;\n"
"    const o=ac.createOscillator(),g=ac.createGain();\n"
"    o.type='square';o.frequency.value=a[0];g.gain.value=0.04;\n"
"    o.connect(g);g.connect(ac.destination);o.start();o.stop(ac.currentTime+a[1]/1000);\n"
"  });\n"
"}\n"
"\n"
"/* ---- widgets (UIVisuals v2: Button, Label, Slider, Check, TextBox, TextArea, Dropdown, List, Progress, Title, Separator, Break) ---- */\n"
"let nodes={},structKey='',revs={};\n"
"const esc=s=>s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');\n"
"const el=(t,c)=>{const e=document.createElement(t);if(c)e.className=c;return e;};\n"
"function hilite(t){\n"
"  return t.split('\\n').map(l=>{\n"
"    const re=/(\\/\\/.*$)|(\"(?:[^\"\\\\]|\\\\.)*\"?)|(^\\s*<[^>]*>)|(^\\s*(?:#(?:\\+\\+|\\+|-)\\s*)?[A-Za-z_][\\w:]*)|(\\b\\d+(?:\\.\\d+)?\\b)|([{}()])/g;\n"
"    let o='',i=0,m;\n"
"    while((m=re.exec(l))){\n"
"      if(m[0]===''){re.lastIndex++;continue;}\n"
"      o+=esc(l.slice(i,m.index));\n"
"      const c=m[1]?'c':m[2]?'s':m[3]?'h':m[4]?'k':m[5]?'n':'p';\n"
"      o+='<span class=\"t'+c+'\">'+esc(m[0])+'</span>'; i=m.index+m[0].length;\n"
"    }\n"
"    return o+esc(l.slice(i));\n"
"  }).join('\\n');\n"
"}\n"
"let putq=Promise.resolve();\n"
"function putText(id,v){putq=putq.then(()=>fetch('/put?k='+K+'&id='+encodeURIComponent(id),{method:'POST',body:v}).catch(()=>{}));return putq;}\n"
"const lineStart=(v,s)=>s?v.lastIndexOf('\\n',s-1)+1:0;\n"
"function mkEditor(id){\n"
"  const root=el('div','ed'),cap=el('div','edcap'),box=el('div','edbox'),gut=el('pre','gut'),wr=el('div','edwr'),hl=el('pre','hl'),ta=el('textarea');\n"
"  ta.spellcheck=false; ta.wrap='off'; wr.append(hl,ta); box.append(gut,wr); root.append(cap,box);\n"
"  let tm=null,ct=null,syn='';\n"
"  const sync=()=>{hl.scrollTop=ta.scrollTop;hl.scrollLeft=ta.scrollLeft;gut.scrollTop=ta.scrollTop;};\n"
"  const refresh=()=>{const t=ta.value;let g='';const n=t.split('\\n').length;for(let i=1;i<=n;i++)g+=i+'\\n';gut.textContent=g;hl.innerHTML=(syn?hilite(t):esc(t))+'\\n';sync();};\n"
"  const flush=()=>{clearTimeout(tm);return putText(id,ta.value);};\n"
"  const cursor=()=>{const v=ta.value,p=ta.selectionStart;send(['u','cursor',id,(v.slice(0,p).split('\\n').length)+','+(p-lineStart(v,p)+1)+','+ta.selectionStart+','+ta.selectionEnd]);};\n"
"  const typed=()=>ta.dispatchEvent(new Event('input'));\n"
"  ta.addEventListener('input',()=>{refresh();clearTimeout(tm);tm=setTimeout(flush,250);});\n"
"  ta.addEventListener('scroll',sync);\n"
"  ta.addEventListener('keyup',()=>{clearTimeout(ct);ct=setTimeout(cursor,120);});\n"
"  ta.addEventListener('click',cursor);\n"
"  ta.addEventListener('keydown',e=>{\n"
"    const k=e.key, mod=(e.ctrlKey||e.metaKey)&&!e.altKey;\n"
"    if((mod&&/^[sonfrSONFR]$/.test(k))||k==='F5'){\n"
"      e.preventDefault(); const nm=k==='F5'?'F5':'Ctrl+'+k.toUpperCase();\n"
"      flush().then(()=>send(['k','down',nm])); return;\n"
"    }\n"
"    if(k==='Tab'){\n"
"      e.preventDefault(); const v=ta.value,s=ta.selectionStart,en=ta.selectionEnd;\n"
"      if(s===en&&!e.shiftKey){ta.setRangeText('    ',s,en,'end');}\n"
"      else{\n"
"        const ls=lineStart(v,s),out=v.slice(ls,en).split('\\n').map(l=>e.shiftKey?l.replace(/^( {1,4}|\\t)/,''):'    '+l).join('\\n');\n"
"        ta.setRangeText(out,ls,en,'select');\n"
"      }\n"
"      typed();\n"
"    }else if(k==='Enter'&&!e.shiftKey&&!e.ctrlKey&&!e.metaKey){\n"
"      const v=ta.value,s=ta.selectionStart,cur=v.slice(lineStart(v,s),s),ind=cur.match(/^\\s*/)[0],extra=/\\{\\s*$/.test(cur)?'    ':'';\n"
"      e.preventDefault(); ta.setRangeText('\\n'+ind+extra,s,ta.selectionEnd,'end'); typed();\n"
"    }\n"
"  });\n"
"  const gotoLine=n=>{\n"
"    const ls=ta.value.split('\\n'); n=Math.max(1,Math.min(n,ls.length)); let off=0; for(let i=0;i<n-1;i++)off+=ls[i].length+1;\n"
"    ta.focus(); ta.setSelectionRange(off,off+ls[n-1].length);\n"
"    const lh=parseFloat(getComputedStyle(ta).lineHeight)||19; ta.scrollTop=Math.max(0,(n-1)*lh-ta.clientHeight/2); sync(); cursor();\n"
"  };\n"
"  const set=w=>{\n"
"    cap.textContent=w.text; cap.style.display=w.text?'block':'none'; syn=w.syn||''; box.classList.toggle('syn',!!syn);\n"
"    box.style.height=((w.rows||16)*1.5+1.2)+'em';\n"
"    if(w.cols){root.style.flex='0 0 auto';root.style.width=(w.cols+6)+'ch';}\n"
"    if(ta.value!==w.val)ta.value=w.val; refresh(); if(w.goto>0)gotoLine(w.goto);\n"
"  };\n"
"  return {root,set};\n"
"}\n"
"function mk(w){\n"
"  const id=w.id; let root,set;\n"
"  if(w.t==='label'){root=el('span');set=w=>{root.textContent=w.text;};}\n"
"  else if(w.t==='title'){root=el('div','ttl2');set=w=>{root.textContent=w.text;};}\n"
"  else if(w.t==='separator'){root=el('div','sep');set=()=>{};}\n"
"  else if(w.t==='break'){root=el('div','brk');set=()=>{};}\n"
"  else if(w.t==='button'){root=el('button');set=w=>{root.textContent=w.text;};root.onclick=()=>send(['u','press',id,'']);}\n"
"  else if(w.t==='progress'){root=el('label');const s=el('span'),p=el('progress');root.append(s,p);set=w=>{s.textContent=w.text;p.max=w.max-w.min;p.value=w.val-w.min;};}\n"
"  else if(w.t==='dropdown'||w.t==='list'){\n"
"    root=el('label');const s=el('span'),sel=el('select');\n"
"    if(w.t==='list'){sel.size=Math.max(2,w.rows||8);sel.className='lst';root.className='lstl';}\n"
"    root.append(s,sel);sel.onchange=()=>send(['u','input',id,sel.value]);\n"
"    set=w=>{s.textContent=w.text;const o=w.opts?w.opts.split('|'):[];\n"
"      sel.replaceChildren(...o.map(t=>{const e=el('option');e.textContent=t;e.value=t;return e;}));sel.value=w.val;};\n"
"  }\n"
"  else if(w.t==='textarea'){return mkEditor(id);}\n"
"  else{\n"
"    root=el('label');const sp=el('span'),i=el('input');root.append(sp,i);\n"
"    if(w.t==='slider'){\n"
"      i.type='range';i.oninput=()=>send(['u','input',id,i.value]);\n"
"      set=w=>{sp.textContent=w.text;i.min=w.min;i.max=w.max;i.value=w.val;};\n"
"    }else if(w.t==='check'){\n"
"      i.type='checkbox';i.oninput=()=>send(['u','input',id,i.checked?'1':'0']);\n"
"      set=w=>{sp.textContent=w.text;i.checked=w.val==='1';};\n"
"    }else{\n"
"      i.type='text';i.oninput=()=>send(['u','input',id,i.value]);\n"
"      set=w=>{sp.textContent=w.text;i.value=w.val;};\n"
"    }\n"
"  }\n"
"  return {root,set};\n"
"}\n"
"function applyUI(j){\n"
"  const sk=j.w.map(w=>w.id+':'+w.t).join('|'),ui=$('ui');\n"
"  if(sk!==structKey){\n"
"    structKey=sk; ui.replaceChildren(); nodes={}; revs={};\n"
"    for(const w of j.w){const n=mk(w);nodes[w.id]=n;ui.appendChild(n.root);}\n"
"  }\n"
"  for(const w of j.w){\n"
"    const n=nodes[w.id]; if(!n)continue;\n"
"    if(revs[w.id]!==w.rev){revs[w.id]=w.rev;n.set(w);}\n"
"  }\n"
"}\n"
"/* ---- long-poll loops ---- */\n"
"async function frames(){\n"
"  for(;;){\n"
"    try{\n"
"      const r=await fetch('/frame?k='+K+'&v='+fv);\n"
"      if(r.status===200){\n"
"        const w=+r.headers.get('X-W'),h=+r.headers.get('X-H'); fv=+r.headers.get('X-V');\n"
"        const s=r.headers.get('X-Sound'),buf=new Uint8Array(await r.arrayBuffer());\n"
"        if(w&&h&&buf.length>=w*h*3)draw(buf,w,h);\n"
"        if(s)snd(s);\n"
"      }else if(r.status!==204){await sleep(500);}\n"
"    }catch(e){await sleep(500);}\n"
"  }\n"
"}\n"
"async function uiloop(){\n"
"  for(;;){\n"
"    try{\n"
"      const r=await fetch('/ui?k='+K+'&v='+uv);\n"
"      if(r.status===200){const j=await r.json();uv=j.v;applyUI(j);}\n"
"      else if(r.status!==204){await sleep(500);}\n"
"    }catch(e){await sleep(500);}\n"
"  }\n"
"}\n"
"frames(); uiloop();\n"
"</script>\n"
"</body></html>\n"
"\n"
;

static int sock_send(sock_t s, const char *d, size_t n){
  while(n){ int k = (int)send(s, d, (int)n, 0); if(k <= 0) return -1; d += k; n -= (size_t)k; }
  return 0;
}
static void http_reply(sock_t s, int code, const char *ctype, const char *extra, const void *body, size_t n){
  const char *st = code == 200 ? "OK" : code == 204 ? "No Content" : code == 403 ? "Forbidden" : "Not Found";
  char h[900]; int l = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nCache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                                code, st, ctype, (unsigned long)n, extra ? extra : "");
  sock_send(s, h, (size_t)l); if(n) sock_send(s, (const char *)body, n);
}
static int hexv(int c){ return isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10); }
static void pdec(char *s){
  char *o = s;
  for(; *s; s++){
    if(*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])){ *o++ = (char)(hexv((unsigned char)s[1]) * 16 + hexv((unsigned char)s[2])); s += 2; }
    else *o++ = *s;
  }
  *o = 0;
}
static int qget(const char *q, const char *k, char *out_, size_t sz){
  size_t kl = strlen(k);
  for(const char *p = q; p && *p; ){
    if(!strncmp(p, k, kl) && p[kl] == '='){ p += kl + 1; size_t n = strcspn(p, "&"); if(n >= sz) n = sz - 1; memcpy(out_, p, n); out_[n] = 0; return 1; }
    p = strchr(p, '&'); if(p) p++;
  }
  return 0;
}
static int ui_user(const char *id, const char *val, const char *sub);
static void ui_var(const char *id, const char *val);
static void ui_json(Buf *b);
static void ui_put(const char *id, const char *body, size_t len);
static void ui_cursor(const char *id, const char *val);
static long hdr_clen(const char *h){
  for(const char *p = h; *p; ){
    if(!strncasecmp(p, "content-length:", 15)) return atol(p + 15);
    const char *e = strstr(p, "\r\n"); if(!e) break; p = e + 2;
  }
  return 0;
}
static void win_events(char *b){
  while(b && *b){
    char *e = strchr(b, ';'); if(e) *e = 0;
    char *f[6]; int nf = 0; char *p = b;
    while(nf < 6){ f[nf++] = p; char *c = strchr(p, ','); if(!c) break; *c = 0; p = c + 1; }
    for(int i = 0; i < nf; i++) pdec(f[i]);
    Ev ev; memset(&ev, 0, sizeof ev);
    if(nf >= 2 && f[0][0]){
      char tp = f[0][0];
      snprintf(ev.sub, sizeof ev.sub, "%s", f[1]);
      if(tp == 'k' && nf >= 3){ ev.kind = EV_KEY; snprintf(ev.key, sizeof ev.key, "%s", f[2]); eq_push(&ev); }
      else if(tp == 'c' && nf >= 4){
        ev.mx = atof(f[2]); ev.my = atof(f[3]); ev.btn = nf >= 5 ? atoi(f[4]) : 0;
        if(!strcmp(ev.sub, "down")){ Ev cv_ = ev; cv_.kind = EV_CANVAS; eq_push(&cv_); }   /* Canvas fires on pointer down */
        ev.kind = EV_MOUSE; eq_push(&ev);
      }
      else if(tp == 's'){ ev.kind = EV_START; eq_push(&ev); }
      else if(tp == 'u' && nf >= 3 && !strcmp(ev.sub, "cursor")){ ui_cursor(f[2], nf >= 4 ? f[3] : ""); }
      else if(tp == 'u' && nf >= 3){
        ev.kind = EV_UI; snprintf(ev.id, sizeof ev.id, "%s", f[2]); if(nf >= 4) snprintf(ev.val, sizeof ev.val, "%s", f[3]);
        ev.isnum = ui_user(ev.id, ev.val, ev.sub); eq_push(&ev);
      }
    }
    b = e ? e + 1 : NULL;
  }
}
static void *srv_conn(void *a){
  sock_t s = *(sock_t *)a; free(a);
  char *rq = xm(16384); int n = 0;
  while(n < 16383){ int k = (int)recv(s, rq + n, 16383 - n, 0); if(k <= 0) break; n += k; rq[n] = 0; if(strstr(rq, "\r\n\r\n")) break; }
  rq[n] = 0;
  if(strncmp(rq, "GET ", 4) && strncmp(rq, "POST ", 5)){ http_reply(s, 404, "text/plain", NULL, "not found", 9); goto done; }
  { char *tg = strchr(rq, ' ') + 1; char *e = strchr(tg, ' '); if(e) *e = 0; char *hdr = e ? e + 1 : tg;
    char *q = strchr(tg, '?'); if(q) *q++ = 0; else q = tg + strlen(tg);
    char k[64] = "";
    if(!qget(q, "k", k, sizeof k) || strcmp(k, S.win.token)){ http_reply(s, 403, "text/plain", NULL, "forbidden", 9); goto done; }
    S.win.last = now(); S.win.seen = 1;
    if(!strcmp(tg, "/")){
      const char *m = strstr(PAGE_HTML, "%%TOKEN%%"); size_t pl = strlen(PAGE_HTML);
      Buf b = {0}; bcat(&b, PAGE_HTML, (int)(m - PAGE_HTML)); bs(&b, S.win.token); bcat(&b, m + 9, (int)(pl - (size_t)(m - PAGE_HTML) - 9));
      http_reply(s, 200, "text/html; charset=utf-8", NULL, b.s, (size_t)b.n); free(b.s);
    } else if(!strcmp(tg, "/frame")){
      char vs[32] = "0"; qget(q, "v", vs, sizeof vs); unsigned v = (unsigned)strtoul(vs, NULL, 10);
      double end = now() + 1.0;
      for(;;){
        pthread_mutex_lock(&WM);
        int fresh = S.win.fd && S.win.fver != v;
        if(fresh || S.win.snd[0]){
          char sx[300]; sx[0] = 0; if(S.win.snd[0]){ snprintf(sx, sizeof sx, "X-Sound: %s\r\n", S.win.snd); S.win.snd[0] = 0; }
          size_t bn = fresh ? (size_t)S.win.fw * S.win.fh * 3 : 0; unsigned char *cp = NULL;
          if(fresh){ cp = xm(bn); memcpy(cp, S.win.fd, bn); }
          char hx[420]; snprintf(hx, sizeof hx, "X-V: %u\r\nX-W: %d\r\nX-H: %d\r\n%s", S.win.fver, fresh ? S.win.fw : 0, fresh ? S.win.fh : 0, sx);
          pthread_mutex_unlock(&WM);
          http_reply(s, 200, "application/octet-stream", hx, cp, bn); free(cp); break;
        }
        pthread_mutex_unlock(&WM);
        if(now() >= end || S.win.closed){ http_reply(s, 204, "text/plain", NULL, NULL, 0); break; }
        usleep(4000);
      }
    } else if(!strcmp(tg, "/ui")){
      char vs[32] = "0"; qget(q, "v", vs, sizeof vs); unsigned v = (unsigned)strtoul(vs, NULL, 10);
      double end = now() + 1.0;
      for(;;){
        pthread_mutex_lock(&WM);
        if(S.win.uver != v){ Buf b = {0}; ui_json(&b); pthread_mutex_unlock(&WM); http_reply(s, 200, "application/json; charset=utf-8", NULL, b.s, (size_t)b.n); free(b.s); break; }
        pthread_mutex_unlock(&WM);
        if(now() >= end || S.win.closed){ http_reply(s, 204, "text/plain", NULL, NULL, 0); break; }
        usleep(8000);
      }
    } else if(!strcmp(tg, "/ev")){
      size_t ql = strlen(q) + 1; char *bb = xm(ql);
      if(qget(q, "b", bb, ql)) win_events(bb);
      free(bb); http_reply(s, 204, "text/plain", NULL, NULL, 0);
    } else if(!strcmp(tg, "/put")){            /* POST: big text of a TextArea (up to 8 MB) */
      char pid[64] = ""; qget(q, "id", pid, sizeof pid); pdec(pid);
      long cl = hdr_clen(hdr); if(cl < 0) cl = 0; if(cl > 8 * 1024 * 1024) cl = 8 * 1024 * 1024;
      char *body = xm((size_t)cl + 1); size_t have = 0; char *hb = strstr(hdr, "\r\n\r\n");
      if(hb){ hb += 4; size_t av = (size_t)(rq + n - hb); if(av > (size_t)cl) av = (size_t)cl; memcpy(body, hb, av); have = av; }
      while(have < (size_t)cl){ int k = (int)recv(s, body + have, (size_t)cl - have, 0); if(k <= 0) break; have += (size_t)k; }
      body[have] = 0; ui_put(pid, body, have); free(body); http_reply(s, 204, "text/plain", NULL, NULL, 0);
    } else if(!strcmp(tg, "/bye")){
      S.win.bye = now(); http_reply(s, 204, "text/plain", NULL, NULL, 0);
    } else http_reply(s, 404, "text/plain", NULL, "not found", 9);
  }
done:
  sclose(s); free(rq); return NULL;
}
static void *srv_accept(void *a_){
  (void)a_;
  for(;;){
    sock_t c = accept(WLS, NULL, NULL);
    if(c == SOCK_BAD){ if(S.win.closed) break; usleep(10000); continue; }
    sock_t *p = xm(sizeof *p); *p = c;
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if(pthread_create(&th, &at, srv_conn, p)){ sclose(c); free(p); }
  }
  return NULL;
}
/* the program stops when the browser tab is closed */
static void *win_watch(void *a_){
  (void)a_;
  while(!S.win.closed){
    usleep(300000);
    double t = now();
    if(S.win.seen && ((t - S.win.last > 10.0) || (S.win.bye > 0 && t - S.win.bye > 2.0 && S.win.last < S.win.bye))){ S.win.closed = 1; S.stop = 1; }
  }
  return NULL;
}

/* ---- events: run the On: handlers ---- */
static int evmatch(EvH *h, const Ev *e){
  if(h->kind != e->kind) return 0;
  if(e->kind == EV_KEY){ int up = !strcasecmp(h->sub, "up"); return up ? !strcasecmp(e->sub, "up") : !strcasecmp(e->sub, "down"); }
  if(e->kind == EV_MOUSE) return !strcasecmp(h->sub[0] ? h->sub : "move", e->sub);
  if(e->kind == EV_UI) return !strcasecmp(h->id, e->id);
  if(e->kind == EV_PIN) return !strcmp(h->id, e->id) && (!strcmp(h->sub, "change") || !strcmp(h->sub, e->sub));
  return 1;
}
static void setg(const char *n, Val v){ Val *p = dget(S.root->v, n); if(p){ vdrop(*p); *p = v; } else dset(S.root->v, n, v); }
static void fire_ev(const Ev *e){
#ifdef LG_LATENCY
  if(e->kind == EV_PIN) fprintf(stderr, "LAT %.1f\n", (now() - e->mx) * 1e6);   /* microseconds from the edge to the handler (measurement build only) */
#endif
  if(e->kind == EV_CANVAS || e->kind == EV_MOUSE){
    setg("mx", Vn(e->mx)); setg("my", Vn(e->my));
    if(e->kind == EV_MOUSE && strcasecmp(e->sub, "move")) setg("mb", Vn(!strcasecmp(e->sub, "down") ? (e->btn == 2 ? 2 : 1) : 0));
  }
  if(e->kind == EV_KEY){
    setg("key", Vs(e->key)); Val *kd = dget(S.root->v, "keys");
    if(kd && kd->t == T_DICT){ if(!strcasecmp(e->sub, "down")) dset(DP(*kd), e->key, Vn(1)); else dremove(DP(*kd), e->key); }
  }
  if(e->kind == EV_UI) ui_var(e->id, e->val);
  for(int i = 0; i < S.nevh; i++){
    EvH *h = &S.evh[i]; if(!evmatch(h, e)) continue;
    Scope *volatile loc = scope_new(h->sc);
    jmp_buf jb, *old = T->jb; T->jb = &jb;
    if(!setjmp(jb)){ T->cur = loc; ex(h->body, loc); }
    else { char m[1100]; snprintf(m, sizeof m, "[On] ERROR %s", T->err); out(m); T->depth = 0; }
    T->jb = old; scope_unref(loc);
  }
}
static void *ev_main(void *a_){
  (void)a_; gil_lock();   /* gil_lock (not a plain lock): announces that a thread is waiting, so a busy main program yields */
  Th th; memset(&th, 0, sizeof th); th.retv = Vu(); T = &th; jmp_buf jb; th.jb = &jb; nthreads++;
  while(!S.stop){ Ev e; if(eq_pop(&e)) fire_ev(&e); else ev_wait(); }
  nthreads--; pthread_mutex_unlock(&GIL); return NULL;
}
/* bare On:Key (no window): one line typed in the terminal = one key press */
static void *key_main(void *a_){
  (void)a_; pthread_mutex_lock(&GIL);
  Th th; memset(&th, 0, sizeof th); th.retv = Vu(); T = &th; jmp_buf jb; th.jb = &jb; nthreads++;
  while(!S.stop){
    char *line = readline_nogil(); if(!line) break;
    Val *keys = dget(S.root->v, "keys"); if(keys && keys->t == T_DICT) dset(DP(*keys), "last", Vs(line));
    Ev e; memset(&e, 0, sizeof e); e.kind = EV_KEY; snprintf(e.sub, sizeof e.sub, "down"); snprintf(e.key, sizeof e.key, "%s", line);
    fire_ev(&e); { Val *kd = dget(S.root->v, "keys"); if(kd && kd->t == T_DICT) dremove(DP(*kd), e.key); } free(line);
  }
  S.nstdin = 0; nthreads--; pthread_mutex_unlock(&GIL); return NULL;
}
static void start_keys(void){
  if(S.keythread) return; S.keythread = 1; S.nstdin = 1;
  pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_create(&th, &at, key_main, NULL);
}
static void start_evthread(void){
  if(S.evthread) return; S.evthread = 1;
  pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_create(&th, &at, ev_main, NULL);
}
static void open_url(const char *u){
  char c[700];
#ifdef _WIN32
  snprintf(c, sizeof c, "start \"\" \"%s\"", u);
#elif defined(__APPLE__)
  snprintf(c, sizeof c, "open \"%s\" >/dev/null 2>&1 &", u);
#else
  snprintf(c, sizeof c, "xdg-open \"%s\" >/dev/null 2>&1 &", u);
#endif
  if(system(c)){}
}
/* starts the local window server (once). Returns 0 if windows are disabled (--no-window) */
static int win_ensure(void){
  if(S.nowin) return 0;
  if(S.win.on) return 1;
#ifdef _WIN32
  { WSADATA wd; if(WSAStartup(MAKEWORD(2, 2), &wd)) lerr("Window: could not start the network layer"); }
#endif
  sock_t s = socket(AF_INET, SOCK_STREAM, 0); if(s == SOCK_BAD) lerr("Window: could not create a socket");
#ifndef _WIN32
  { int o = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&o, sizeof o); }
#endif
  struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((unsigned short)S.port);
  if(bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)){ sclose(s); lerr("Window: could not listen on 127.0.0.1:%d (try --port N)", S.port); }
  socklen_t sl = sizeof a; getsockname(s, (struct sockaddr *)&a, &sl); S.win.port = ntohs(a.sin_port);
  snprintf(S.win.token, sizeof S.win.token, "%08llx%08llx", (ull)(rnd() * 4294967296.0), (ull)(rnd() * 4294967296.0));
  WLS = s; S.win.uver = 1; S.win.last = now(); S.win.on = 1;
  pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_create(&th, &at, srv_accept, NULL); pthread_create(&th, &at, win_watch, NULL); start_evthread();
  char url[160]; snprintf(url, sizeof url, "http://127.0.0.1:%d/?k=%s", S.win.port, S.win.token);
  char m[220]; snprintf(m, sizeof m, "Window: %s", url); out(m);
  if(!S.nobrowser) open_url(url);
  return 1;
}

/* ================================================================ option helpers shared by the libraries */
static int oi(Stmt *x, const char *k){ for(int i = 0; i < x->no; i++) if(!strcasecmp(x->o[i].v[0], k)) return i; return -1; }
static double od(Stmt *x, const char *k, double d){ const char *s = og(x, k); return s && *s ? evd(s) : d; }
static Col oc(Stmt *x, const char *k, Col d){ int i = oi(x, k); return i >= 0 ? cs_(&x->o[i], 1) : d; }
static char *os_(Stmt *x, const char *k){ const char *s = og(x, k); return s ? fm(s) : NULL; }
static double vdot(const double *a, const double *b){ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void vcross(const double *a, const double *b, double *o){ o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static double vnorm(double *a){ double l = sqrt(vdot(a, a)); if(l > 1e-12){ a[0] /= l; a[1] /= l; a[2] /= l; } return l; }

/* ================================================================ G3D / Graphics3D v2 (z-buffer, more meshes, OBJ load/save, picking, projection) */
#define G3_MAXM 24
typedef struct { double (*V)[3]; int (*F)[3]; int nV, nF, two; char *name; } G3M;
typedef struct { char *id; int type; double size, x, y, z, rx, ry, rz, s, sx, sy, sz; int vis; Col col; } G3O;
typedef struct { double x[3], y[3], iw[3], d, sp; Col col; int obj; } G3T;
/* built-in meshes: 0 cube, 1 sphere, 2 torus, 3 pyramid, 4 cylinder, 5 cone, 6 plane, 7 diamond; loaded OBJ meshes come after */
static const char *G3BN[] = {"cube", "sphere", "torus", "pyramid", "cylinder", "cone", "plane", "diamond", NULL};
#define G3_NB 8
static struct { G3O *o; int n, cap; G3M m[G3_MAXM]; int nm, init, wire, grid; Col bg; double cam[3], tgt[3], fov, lit[3], amb; G3T *last; int nlast; } G3;
static struct { double fw[3], r[3], u[3], F; int W, H; } G3V;
static double g3n(double *a){ double l = sqrt(vdot(a, a)); if(l == 0) l = 1; a[0] /= l; a[1] /= l; a[2] /= l; return l; }
static void g3_build(int t){
  G3M *m = &G3.m[t]; double h = 0.5; m->V = xm(1024 * sizeof *m->V); m->F = xm(1024 * sizeof *m->F); m->nV = m->nF = 0; m->two = (t == 6); m->name = xs(G3BN[t]);
#define PV(a, b, c) do{ m->V[m->nV][0] = (a); m->V[m->nV][1] = (b); m->V[m->nV][2] = (c); m->nV++; }while(0)
#define PF(a, b, c) do{ m->F[m->nF][0] = (a); m->F[m->nF][1] = (b); m->F[m->nF][2] = (c); m->nF++; }while(0)
  if(t == 0){
    for(int i = 0; i < 8; i++) PV(i & 1 ? h : -h, i & 2 ? h : -h, i & 4 ? h : -h);
    static const int Q[6][4] = {{0,1,3,2},{4,5,7,6},{0,1,5,4},{2,3,7,6},{0,2,6,4},{1,3,7,5}};
    for(int q = 0; q < 6; q++){ PF(Q[q][0], Q[q][1], Q[q][2]); PF(Q[q][0], Q[q][2], Q[q][3]); }
  } else if(t == 3){
    PV(-h,-h,-h); PV(h,-h,-h); PV(h,-h,h); PV(-h,-h,h); PV(0,h,0);
    PF(0,1,4); PF(1,2,4); PF(2,3,4); PF(3,0,4); PF(0,1,2); PF(0,2,3);
  } else if(t == 1){
    int a = 10, b = 16;
    for(int i = 0; i <= a; i++) for(int j = 0; j <= b; j++){ double p = M_PI * i / a, q = 2 * M_PI * j / b; PV(h * sin(p) * cos(q), h * cos(p), h * sin(p) * sin(q)); }
    for(int i = 0; i < a; i++) for(int j = 0; j < b; j++){ int k = i * (b + 1) + j; PF(k, k + b + 1, k + 1); PF(k + 1, k + b + 1, k + b + 2); }
  } else if(t == 2){
    double R = 0.35, r = 0.15; int a = 20, b = 10;
    for(int i = 0; i <= a; i++) for(int j = 0; j <= b; j++){ double u = 2 * M_PI * i / a, v = 2 * M_PI * j / b; PV((R + r * cos(v)) * cos(u), r * sin(v), (R + r * cos(v)) * sin(u)); }
    for(int i = 0; i < a; i++) for(int j = 0; j < b; j++){ int k = i * (b + 1) + j; PF(k, k + b + 1, k + 1); PF(k + 1, k + b + 1, k + b + 2); }
  } else if(t == 4 || t == 5){
    int n = 24;
    for(int i = 0; i < n; i++){ double a = 2 * M_PI * i / n; PV(h * cos(a), -h, h * sin(a)); }
    if(t == 4){
      for(int i = 0; i < n; i++){ double a = 2 * M_PI * i / n; PV(h * cos(a), h, h * sin(a)); }
      PV(0, -h, 0); PV(0, h, 0);
      for(int i = 0; i < n; i++){ int j = (i + 1) % n; PF(i, j, n + i); PF(j, n + j, n + i); PF(2 * n, i, j); PF(2 * n + 1, n + i, n + j); }
    } else {
      PV(0, h, 0); PV(0, -h, 0);
      for(int i = 0; i < n; i++){ int j = (i + 1) % n; PF(i, j, n); PF(n + 1, i, j); }
    }
  } else if(t == 6){
    PV(-h, 0, -h); PV(h, 0, -h); PV(h, 0, h); PV(-h, 0, h); PF(0, 2, 1); PF(0, 3, 2);
  } else {
    PV(h, 0, 0); PV(-h, 0, 0); PV(0, h, 0); PV(0, -h, 0); PV(0, 0, h); PV(0, 0, -h);
    for(int a = 0; a < 2; a++) for(int b = 2; b < 4; b++) for(int c = 4; c < 6; c++) PF(a, b, c);
  }
#undef PV
#undef PF
  if(t == 6) return;                                       /* the plane is two-sided: no need to orient it */
  for(int f = 0; f < m->nF; f++){                          /* make every face point outwards */
    double *A = m->V[m->F[f][0]], *B = m->V[m->F[f][1]], *C = m->V[m->F[f][2]];
    double ab[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, ac[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]}, n[3]; vcross(ab, ac, n);
    double c[3] = {(A[0] + B[0] + C[0]) / 3, (A[1] + B[1] + C[1]) / 3, (A[2] + B[2] + C[2]) / 3}, rf[3] = {c[0], c[1], c[2]};
    if(t == 3 || t == 5) rf[1] += 0.25;
    else if(t == 2){ double l = hypot(c[0], c[2]); if(l == 0) l = 1; rf[0] = c[0] - 0.35 * c[0] / l; rf[2] = c[2] - 0.35 * c[2] / l; }
    if(vdot(n, rf) < 0){ int tmp = m->F[f][1]; m->F[f][1] = m->F[f][2]; m->F[f][2] = tmp; }
  }
}
static void g3_init(void){
  if(G3.init) return; G3.init = 1;
  G3.cam[0] = 0; G3.cam[1] = 2; G3.cam[2] = 6; G3.fov = 60; G3.lit[0] = 3; G3.lit[1] = 5; G3.lit[2] = 4; G3.amb = 0.18;
  G3.bg.r = 5; G3.bg.g = 7; G3.bg.b = 22;
  for(int t = 0; t < G3_NB; t++) g3_build(t);
  G3.nm = G3_NB;
}
static G3O *g3_find(const char *id){ for(int i = 0; i < G3.n; i++) if(!strcmp(G3.o[i].id, id)) return &G3.o[i]; return NULL; }
static int g3_mesh(const char *n){ for(int i = 0; i < G3.nm; i++) if(G3.m[i].name && !strcasecmp(G3.m[i].name, n)) return i; return -1; }
static void g3_drop_last(void){ free(G3.last); G3.last = NULL; G3.nlast = 0; }
/* Wavefront OBJ (v and f lines; quads and polygons are triangulated). Result: index of the mesh, or -1 (empty) / -2 (too many meshes) */
static int g3_load_obj(const char *name, const char *text, int fit, int two){
  size_t vc = 0, vcap = 1024, fc = 0, fcap = 2048; double (*V)[3] = xm(vcap * sizeof *V); int (*F)[3] = xm(fcap * sizeof *F);
  for(const char *p = text; *p; ){
    const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p); char *ln = xsn(p, len);
    if(ln[0] == 'v' && (ln[1] == ' ' || ln[1] == '\t')){
      double a = 0, b = 0, c = 0; sscanf(ln + 2, "%lf %lf %lf", &a, &b, &c);
      if(vc == vcap){ vcap *= 2; V = xr(V, vcap * sizeof *V); } V[vc][0] = a; V[vc][1] = b; V[vc][2] = c; vc++;
    } else if(ln[0] == 'f' && (ln[1] == ' ' || ln[1] == '\t')){
      int idx[64], ni = 0; char *q = ln + 2;
      while(*q && ni < 64){
        while(*q == ' ' || *q == '\t' || *q == '\r') q++; if(!*q) break;
        long k = strtol(q, &q, 10); if(k < 0) k = (long)vc + k + 1; if(k >= 1 && (size_t)k <= vc) idx[ni++] = (int)k - 1;
        while(*q && *q != ' ' && *q != '\t') q++;
      }
      for(int i = 1; i + 1 < ni; i++){
        if(fc == fcap){ fcap *= 2; F = xr(F, fcap * sizeof *F); } F[fc][0] = idx[0]; F[fc][1] = idx[i]; F[fc][2] = idx[i + 1]; fc++;
      }
    }
    free(ln); if(vc > 400000 || fc > 800000) break; p = e ? e + 1 : p + len;
  }
  if(!vc || !fc){ free(V); free(F); return -1; }
  if(fit){
    double mn[3] = {1e300, 1e300, 1e300}, mx[3] = {-1e300, -1e300, -1e300};
    for(size_t i = 0; i < vc; i++) for(int k = 0; k < 3; k++){ if(V[i][k] < mn[k]) mn[k] = V[i][k]; if(V[i][k] > mx[k]) mx[k] = V[i][k]; }
    double ext = fmax(mx[0] - mn[0], fmax(mx[1] - mn[1], mx[2] - mn[2])); if(ext < 1e-12) ext = 1;
    for(size_t i = 0; i < vc; i++) for(int k = 0; k < 3; k++) V[i][k] = (V[i][k] - (mn[k] + mx[k]) / 2) / ext;
  }
  int mi = g3_mesh(name);
  if(mi >= 0 && mi < G3_NB) mi = -3;
  if(mi == -3){ free(V); free(F); return -2; }
  if(mi < 0){ if(G3.nm >= G3_MAXM){ free(V); free(F); return -2; } mi = G3.nm++; G3.m[mi].name = xs(name); }
  else { free(G3.m[mi].V); free(G3.m[mi].F); }
  G3M *m = &G3.m[mi]; m->V = V; m->F = F; m->nV = (int)vc; m->nF = (int)fc; m->two = two; g3_drop_last();
  return mi;
}
static void g3_xform(G3O *o, G3M *m, double (*P)[3]){
  double sc = o->size * o->s, cxr = cos(o->rx), sxr = sin(o->rx), cyr = cos(o->ry), syr = sin(o->ry), czr = cos(o->rz), szr = sin(o->rz);
  for(int v = 0; v < m->nV; v++){
    double x = m->V[v][0] * sc * o->sx, y = m->V[v][1] * sc * o->sy, z = m->V[v][2] * sc * o->sz, t;
    t = y * cxr - z * sxr; z = y * sxr + z * cxr; y = t;
    t = x * cyr + z * syr; z = -x * syr + z * cyr; x = t;
    t = x * czr - y * szr; y = x * szr + y * czr; x = t;
    P[v][0] = x + o->x; P[v][1] = y + o->y; P[v][2] = z + o->z;
  }
}
static void g3_basis(int W, int H){
  for(int i = 0; i < 3; i++) G3V.fw[i] = G3.tgt[i] - G3.cam[i];
  if(vdot(G3V.fw, G3V.fw) < 1e-18) G3V.fw[2] = -1;
  g3n(G3V.fw); double up[3] = {0, 1, 0}; vcross(G3V.fw, up, G3V.r);
  if(vdot(G3V.r, G3V.r) < 1e-12){ G3V.r[0] = 1; G3V.r[1] = G3V.r[2] = 0; }
  g3n(G3V.r); vcross(G3V.r, G3V.fw, G3V.u);
  G3V.W = W; G3V.H = H; G3V.F = H / 2.0 / tan(G3.fov * M_PI / 360.0);
}
static int g3_proj(const double *P, double *sx, double *sy, double *z){
  double q[3] = {P[0] - G3.cam[0], P[1] - G3.cam[1], P[2] - G3.cam[2]}, zz = vdot(q, G3V.fw); if(zz < 0.1) return 0;
  *sx = G3V.W / 2.0 + G3V.F * vdot(q, G3V.r) / zz; *sy = G3V.H / 2.0 - G3V.F * vdot(q, G3V.u) / zz; *z = zz; return 1;
}
static void g3_line3(FB *f, const double *A, const double *B, Col c){
  double a[3] = {A[0], A[1], A[2]}, b[3] = {B[0], B[1], B[2]}, qa[3], qb[3];
  for(int i = 0; i < 3; i++){ qa[i] = a[i] - G3.cam[i]; qb[i] = b[i] - G3.cam[i]; }
  double za = vdot(qa, G3V.fw), zb = vdot(qb, G3V.fw); if(za < 0.1 && zb < 0.1) return;
  if(za < 0.1){ double t = (0.1 - za) / (zb - za); for(int i = 0; i < 3; i++) a[i] += (b[i] - a[i]) * t; }
  else if(zb < 0.1){ double t = (0.1 - zb) / (za - zb); for(int i = 0; i < 3; i++) b[i] += (a[i] - b[i]) * t; }
  double x0, y0, z0, x1, y1, z1; if(!g3_proj(a, &x0, &y0, &z0) || !g3_proj(b, &x1, &y1, &z1)) return;
  if(fabs(x0) > 1e6 || fabs(y0) > 1e6 || fabs(x1) > 1e6 || fabs(y1) > 1e6) return;
  fb_line(f, (ll)lround(x0), (ll)lround(y0), (ll)lround(x1), (ll)lround(y1), c);
}
static void fb_tri_z(FB *f, float *zb, const double *x, const double *y, const double *iw, Col c){
  double area = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]); if(fabs(area) < 1e-12) return;
  int x0 = (int)floor(fmin(fmin(x[0], x[1]), x[2])), x1 = (int)ceil(fmax(fmax(x[0], x[1]), x[2]));
  int y0 = (int)floor(fmin(fmin(y[0], y[1]), y[2])), y1 = (int)ceil(fmax(fmax(y[0], y[1]), y[2]));
  if(x0 < 0) x0 = 0; if(y0 < 0) y0 = 0; if(x1 > f->w - 1) x1 = f->w - 1; if(y1 > f->h - 1) y1 = f->h - 1;
  for(int j = y0; j <= y1; j++) for(int i = x0; i <= x1; i++){
    double px = i + 0.5, py = j + 0.5;
    double w0 = ((x[1] - px) * (y[2] - py) - (y[1] - py) * (x[2] - px)) / area, w1 = ((x[2] - px) * (y[0] - py) - (y[2] - py) * (x[0] - px)) / area, w2 = 1 - w0 - w1;
    if(w0 >= -1e-9 && w1 >= -1e-9 && w2 >= -1e-9){
      float iz = (float)(w0 * iw[0] + w1 * iw[1] + w2 * iw[2]); size_t k = (size_t)j * f->w + i;
      if(iz > zb[k]){ zb[k] = iz; fb_put(f, i, j, c); }
    }
  }
}
static unsigned char clamp8(double v){ return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void g3_render(void){
  if(!S.fb) S.fb = fb_new(320, 180);
  FB *f = S.fb; int W = f->w, H = f->h; g3_basis(W, H);
  double L[3] = {G3.lit[0], G3.lit[1], G3.lit[2]}; g3n(L);
  G3T *tr = NULL; int nt = 0, ct = 0;
  for(int oi_ = 0; oi_ < G3.n; oi_++){
    G3O *o = &G3.o[oi_]; if(!o->vis || o->type < 0 || o->type >= G3.nm) continue; G3M *m = &G3.m[o->type];
    double (*P)[3] = xm((size_t)(m->nV + 1) * sizeof *P); g3_xform(o, m, P);
    for(int fi = 0; fi < m->nF; fi++){
      double *A = P[m->F[fi][0]], *B = P[m->F[fi][1]], *C = P[m->F[fi][2]];
      double ab[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, ac[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]}, N[3]; vcross(ab, ac, N); g3n(N);
      double M[3] = {(A[0] + B[0] + C[0]) / 3, (A[1] + B[1] + C[1]) / 3, (A[2] + B[2] + C[2]) / 3}, E[3] = {G3.cam[0] - M[0], G3.cam[1] - M[1], G3.cam[2] - M[2]}; g3n(E);
      if(vdot(N, E) <= 0){ if(!m->two) continue; N[0] = -N[0]; N[1] = -N[1]; N[2] = -N[2]; }
      G3T t; int bad = 0; double *Q3[3] = {A, B, C};
      for(int k = 0; k < 3; k++){
        double q[3] = {Q3[k][0] - G3.cam[0], Q3[k][1] - G3.cam[1], Q3[k][2] - G3.cam[2]}, z = vdot(q, G3V.fw);
        if(z < 0.1){ bad = 1; break; }
        t.x[k] = W / 2.0 + G3V.F * vdot(q, G3V.r) / z; t.y[k] = H / 2.0 - G3V.F * vdot(q, G3V.u) / z; t.iw[k] = 1.0 / z;
      }
      if(bad) continue;
      double Hh[3] = {L[0] + E[0], L[1] + E[1], L[2] + E[2]}; g3n(Hh);
      t.d = G3.amb + (1 - G3.amb) * fmax(0, vdot(N, L)); t.sp = 0.5 * pow(fmax(0, vdot(N, Hh)), 30); t.col = o->col; t.obj = oi_;
      if(nt == ct){ ct = ct ? ct * 2 : 256; tr = xr(tr, (size_t)ct * sizeof(G3T)); }
      tr[nt++] = t;
    }
    free(P);
  }
  fb_rect(f, 0, 0, W, H, G3.bg);
  if(G3.grid){                                             /* ground grid on y = 0 with coloured X (red) and Z (blue) axes */
    Col gc = {42, 52, 74}, ax = {200, 60, 60}, az = {60, 90, 220};
    for(int i = -10; i <= 10; i++){
      double a1[3] = {i, 0, -10}, b1[3] = {i, 0, 10}, a2[3] = {-10, 0, i}, b2[3] = {10, 0, i};
      g3_line3(f, a1, b1, i == 0 ? az : gc); g3_line3(f, a2, b2, i == 0 ? ax : gc);
    }
  }
  float *zb = calloc((size_t)W * H, sizeof(float)); if(!zb){ fputs("out of memory\n", stderr); exit(3); }
  for(int i = 0; i < nt; i++){
    G3T *t = &tr[i]; Col c = {clamp8(floor(t->col.r * t->d + 255 * t->sp)), clamp8(floor(t->col.g * t->d + 255 * t->sp)), clamp8(floor(t->col.b * t->d + 255 * t->sp))};
    if(!G3.wire) fb_tri_z(f, zb, t->x, t->y, t->iw, c);
    else for(int k = 0; k < 3; k++){ int k2 = (k + 1) % 3; fb_line(f, (ll)lround(t->x[k]), (ll)lround(t->y[k]), (ll)lround(t->x[k2]), (ll)lround(t->y[k2]), c); }
  }
  free(zb); g3_drop_last(); G3.last = tr; G3.nlast = nt;      /* kept for G3D:Pick */
}
static int g3_pick(double px, double py){
  int best = -1; float bz = 0;
  for(int i = 0; i < G3.nlast; i++){
    G3T *t = &G3.last[i]; const double *x = t->x, *y = t->y; double area = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]); if(fabs(area) < 1e-12) continue;
    double w0 = ((x[1] - px) * (y[2] - py) - (y[1] - py) * (x[2] - px)) / area, w1 = ((x[2] - px) * (y[0] - py) - (y[2] - py) * (x[0] - px)) / area, w2 = 1 - w0 - w1;
    if(w0 >= 0 && w1 >= 0 && w2 >= 0){ float iz = (float)(w0 * t->iw[0] + w1 * t->iw[1] + w2 * t->iw[2]); if(iz > bz){ bz = iz; best = t->obj; } }
  }
  return best;
}
static int g3_save_obj(const char *path){
  Buf b = {0}; bs(&b, "# exported by logl\n"); int voff = 1; char t[160];
  for(int i = 0; i < G3.n; i++){
    G3O *o = &G3.o[i]; if(o->type < 0 || o->type >= G3.nm) continue; G3M *m = &G3.m[o->type];
    double (*P)[3] = xm((size_t)(m->nV + 1) * sizeof *P); g3_xform(o, m, P);
    snprintf(t, sizeof t, "o %s\n", o->id); bs(&b, t);
    for(int v = 0; v < m->nV; v++){ snprintf(t, sizeof t, "v %.6f %.6f %.6f\n", P[v][0], P[v][1], P[v][2]); bs(&b, t); }
    for(int fi = 0; fi < m->nF; fi++){ snprintf(t, sizeof t, "f %d %d %d\n", m->F[fi][0] + voff, m->F[fi][1] + voff, m->F[fi][2] + voff); bs(&b, t); }
    voff += m->nV; free(P);
  }
  int r = writefile(path, btake(&b), (size_t)b.n, "wb"); free(b.s); return r;
}
static const char *g3_id(Stmt *x){      /* Id may use {variables}: "Id;box{i}" */
  static char idb[256]; const char *r = og(x, "id"); if(!r) return NULL; char *f = fm(r); snprintf(idb, sizeof idb, "%s", f); free(f); return idb;
}
static void do_g3d(Stmt *x){
  win_ensure(); g3_init();
  char op[32]; snprintf(op, sizeof op, "%s", hw_(x, 1)); for(char *c = op; *c; c++) *c = (char)tolower((unsigned char)*c);
  if(!strcmp(op, "render")){ g3_render(); return; }
  if(!strcmp(op, "clear")){ for(int i = 0; i < G3.n; i++) free(G3.o[i].id); G3.n = 0; g3_drop_last(); return; }
  if(!strcmp(op, "scene")){
    int i = oi(x, "bg"); if(i >= 0) G3.bg = cs_(&x->o[i], 1);
    const char *m = og(x, "mode"); if(m) G3.wire = !strcasecmp(m, "wire");
    double fv = od(x, "fov", 0); if(fv) G3.fov = fv;
    G3.amb = od(x, "ambient", G3.amb); if(G3.amb < 0) G3.amb = 0; if(G3.amb > 1) G3.amb = 1;
    G3.grid = od(x, "grid", G3.grid) != 0;
    if(!S.fb) S.fb = fb_new(320, 180);
    return;
  }
  if(!strcmp(op, "camera")){
    G3.cam[0] = od(x, "x", G3.cam[0]); G3.cam[1] = od(x, "y", G3.cam[1]); G3.cam[2] = od(x, "z", G3.cam[2]);
    G3.tgt[0] = od(x, "tx", G3.tgt[0]); G3.tgt[1] = od(x, "ty", G3.tgt[1]); G3.tgt[2] = od(x, "tz", G3.tgt[2]); G3.fov = od(x, "fov", G3.fov); return;
  }
  if(!strcmp(op, "orbit")){                                /* camera around the target: Yaw/Pitch (degrees), Dist, or relative DYaw/DPitch/Zoom */
    G3.tgt[0] = od(x, "tx", G3.tgt[0]); G3.tgt[1] = od(x, "ty", G3.tgt[1]); G3.tgt[2] = od(x, "tz", G3.tgt[2]);
    double d[3] = {G3.cam[0] - G3.tgt[0], G3.cam[1] - G3.tgt[1], G3.cam[2] - G3.tgt[2]}, dist = sqrt(vdot(d, d)); if(dist < 1e-6){ dist = 6; d[2] = 6; }
    double yaw = atan2(d[0], d[2]) * 180 / M_PI, pit = asin(fmax(-1, fmin(1, d[1] / dist))) * 180 / M_PI;
    yaw = od(x, "yaw", yaw) + od(x, "dyaw", 0); pit = od(x, "pitch", pit) + od(x, "dpitch", 0); if(pit > 89) pit = 89; if(pit < -89) pit = -89;
    dist = od(x, "dist", dist) * od(x, "zoom", 1); if(dist < 0.2) dist = 0.2; if(dist > 1000) dist = 1000;
    double ya = yaw * M_PI / 180, pa = pit * M_PI / 180;
    G3.cam[0] = G3.tgt[0] + dist * cos(pa) * sin(ya); G3.cam[1] = G3.tgt[1] + dist * sin(pa); G3.cam[2] = G3.tgt[2] + dist * cos(pa) * cos(ya); return;
  }
  if(!strcmp(op, "light")){ G3.lit[0] = od(x, "x", G3.lit[0]); G3.lit[1] = od(x, "y", G3.lit[1]); G3.lit[2] = od(x, "z", G3.lit[2]); return; }
  if(!strcmp(op, "load")){                                 /* G3D:Load Name;ship File;ship.obj [Fit;0] [Twosided;0] */
    const char *nm = og(x, "name"), *fo = og(x, "file"); if(!nm || !*nm || !fo) lerr("G3D:Load needs Name and File, for example \"Name;ship\",\"File;ship.obj\"");
    char *fe = fm(fo), *p = fpath(fe, "G3D:Load"); char *tx = readfile(p, NULL); if(!tx){ char tmp[200]; snprintf(tmp, sizeof tmp, "%s", fe); free(p); free(fe); lerr("G3D:Load: could not read %s", tmp); }
    int r = g3_load_obj(nm, tx, od(x, "fit", 1) != 0, od(x, "twosided", 1) != 0); free(tx); free(p); free(fe);
    if(r == -1) lerr("G3D:Load: the file has no vertices or faces (only OBJ with v and f lines)"); if(r == -2) lerr("G3D:Load: too many meshes, or that name is a built-in shape"); return;
  }
  if(!strcmp(op, "save")){
    const char *fo = og(x, "file"); if(!fo) lerr("G3D:Save needs File, for example \"File;scene.obj\"");
    char *fe = fm(fo), *p = fpath(fe, "G3D:Save"); int r = g3_save_obj(p); free(p); if(r){ char tmp[200]; snprintf(tmp, sizeof tmp, "%s", fe); free(fe); lerr("G3D:Save: could not write %s", tmp); } free(fe); return;
  }
  if(!strcmp(op, "list")){ Val l = Vl(); for(int i = 0; i < G3.n; i++) lpush(LP(l), Vs(G3.o[i].id)); into(x, l); return; }
  if(!strcmp(op, "meshes")){ Val l = Vl(); for(int i = 0; i < G3.nm; i++) lpush(LP(l), Vs(G3.m[i].name)); into(x, l); return; }
  if(!strcmp(op, "pick")){                                 /* G3D:Pick X;mx Y;my Into;v -> id of the object under that pixel of the last Render ("" if none) */
    int b = g3_pick(od(x, "x", -1), od(x, "y", -1)); into(x, Vs(b >= 0 && b < G3.n ? G3.o[b].id : "")); return;
  }
  if(!strcmp(op, "project")){                              /* G3D:Project Id;a Into;p  or  X;1 Y;2 Z;3 -> [screen x, screen y, depth] ([] if behind the camera) */
    if(!S.fb) S.fb = fb_new(320, 180); g3_basis(S.fb->w, S.fb->h); double P[3], sx, sy, z; const char *id = g3_id(x);
    if(id && *id){ G3O *o = g3_find(id); if(!o) lerr("G3D: object does not exist %s", id); P[0] = o->x; P[1] = o->y; P[2] = o->z; }
    else { P[0] = od(x, "x", 0); P[1] = od(x, "y", 0); P[2] = od(x, "z", 0); }
    Val l = Vl(); if(g3_proj(P, &sx, &sy, &z)){ lpush(LP(l), Vn(floor(sx))); lpush(LP(l), Vn(floor(sy))); lpush(LP(l), Vn(z)); } into(x, l); return;
  }
  const char *id = g3_id(x); if(!id || !*id) lerr("G3D: Id is missing, for example \"Id;box\"");
  if(!strcmp(op, "remove")){
    for(int i = 0; i < G3.n; i++) if(!strcmp(G3.o[i].id, id)){ free(G3.o[i].id); memmove(&G3.o[i], &G3.o[i + 1], (size_t)(G3.n - i - 1) * sizeof(G3O)); G3.n--; g3_drop_last(); break; }
    return;
  }
  if(!strcmp(op, "info")){                                 /* G3D:Info Id;a Into;d -> dictionary with x, y, z, rx, ry, rz, s, sx, sy, sz, size, mesh, visible, color */
    G3O *o = g3_find(id); if(!o) lerr("G3D: object does not exist %s", id); Val d = Vd(); Dict *D = DP(d);
    dset(D, "x", Vn(o->x)); dset(D, "y", Vn(o->y)); dset(D, "z", Vn(o->z)); dset(D, "rx", Vn(o->rx)); dset(D, "ry", Vn(o->ry)); dset(D, "rz", Vn(o->rz));
    dset(D, "s", Vn(o->s)); dset(D, "sx", Vn(o->sx)); dset(D, "sy", Vn(o->sy)); dset(D, "sz", Vn(o->sz)); dset(D, "size", Vn(o->size));
    dset(D, "mesh", Vs(G3.m[o->type].name)); dset(D, "visible", Vn(o->vis));
    Val c = Vl(); lpush(LP(c), Vn(o->col.r)); lpush(LP(c), Vn(o->col.g)); lpush(LP(c), Vn(o->col.b)); dset(D, "color", c); into(x, d); return;
  }
  if(!strcmp(op, "set")){
    G3O *o = g3_find(id); if(!o) lerr("G3D: object does not exist %s", id);
    o->x = od(x, "x", o->x); o->y = od(x, "y", o->y); o->z = od(x, "z", o->z); o->rx = od(x, "rx", o->rx); o->ry = od(x, "ry", o->ry); o->rz = od(x, "rz", o->rz); o->s = od(x, "s", o->s);
    o->sx = od(x, "sx", o->sx); o->sy = od(x, "sy", o->sy); o->sz = od(x, "sz", o->sz); o->size = od(x, "size", o->size); o->vis = od(x, "visible", o->vis) != 0;
    int ci = oi(x, "color"); if(ci >= 0) o->col = cs_(&x->o[ci], 1);
    const char *mn = og(x, "mesh"); if(mn){ int mi = g3_mesh(mn); if(mi < 0) lerr("G3D: unknown mesh: %s", mn); o->type = mi; }
    return;
  }
  int ty = -1; for(int i = 0; G3BN[i]; i++) if(!strcmp(op, G3BN[i])) ty = i;
  if(!strcmp(op, "object")){ const char *mn = og(x, "mesh"); if(!mn) lerr("G3D:Object needs Mesh, for example \"Mesh;ship\" (a built-in shape or one loaded with G3D:Load)"); ty = g3_mesh(mn); if(ty < 0) lerr("G3D: unknown mesh: %s", mn); }
  if(ty < 0) lerr("Unknown G3D operation: %s\n  -> Valid: cube, sphere, torus, pyramid, cylinder, cone, plane, diamond, object, set, remove, info, list, meshes, load, save, pick, project, render, clear, scene, camera, orbit, light", hw_(x, 1));
  G3O no; memset(&no, 0, sizeof no); no.type = ty; no.size = od(x, "size", 1); no.x = od(x, "x", 0); no.y = od(x, "y", 0); no.z = od(x, "z", 0);
  no.rx = od(x, "rx", 0); no.ry = od(x, "ry", 0); no.rz = od(x, "rz", 0); no.s = od(x, "s", 1); no.sx = od(x, "sx", 1); no.sy = od(x, "sy", 1); no.sz = od(x, "sz", 1); no.vis = od(x, "visible", 1) != 0;
  no.col.r = 74; no.col.g = 163; no.col.b = 255;
  int ci = oi(x, "color"); if(ci >= 0) no.col = cs_(&x->o[ci], 1);
  G3O *o = g3_find(id);
  if(o){ no.id = o->id; *o = no; }
  else { if(G3.n == G3.cap){ G3.cap = G3.cap ? G3.cap * 2 : 16; G3.o = xr(G3.o, (size_t)G3.cap * sizeof(G3O)); } no.id = xs(id); G3.o[G3.n++] = no; }
}

/* ================================================================ Dyn2D (2DDynamics): sprites, physics, tweens, collisions */
typedef struct { int p; double from, to, t0, dur; int ease; } Tw;
typedef struct { char *id; int shape, layer, vis, bounds, seq, ntw, tsz; double x, y, w, h, r, vx, vy, ax, ay, bounce; Col col; char *text; Tw tw[4]; } Sp;
static struct { Sp *s; int n, cap, seq; } D2;
static const char *SPN[] = {"x", "y", "w", "h", "r", "vx", "vy", "ax", "ay", "bounce", NULL};
static double *sp_pi(Sp *s, int i){ double *P[] = {&s->x, &s->y, &s->w, &s->h, &s->r, &s->vx, &s->vy, &s->ax, &s->ay, &s->bounce}; return P[i]; }
static int sp_pidx(const char *p){ for(int i = 0; SPN[i]; i++) if(!strcasecmp(p, SPN[i])) return i; return -1; }
static Sp *sp_find(const char *id){ for(int i = 0; i < D2.n; i++) if(!strcmp(D2.s[i].id, id)) return &D2.s[i]; return NULL; }
static void sp_box(Sp *s, double *l, double *t, double *r, double *b){
  if(s->shape == 1){ *l = s->x - s->r; *t = s->y - s->r; *r = s->x + s->r; *b = s->y + s->r; }
  else if(s->shape == 2){ int sc = s->tsz / 8; if(sc < 1) sc = 1; *l = s->x; *t = s->y; *r = s->x + (s->text ? (double)strlen(s->text) : 0) * 4 * sc; *b = s->y + 5 * sc; }
  else { *l = s->x; *t = s->y; *r = s->x + s->w; *b = s->y + s->h; }
}
static int sp_hit(Sp *a, Sp *b){
  if(a->shape == 1 && b->shape == 1){ double dx = a->x - b->x, dy = a->y - b->y, rr = a->r + b->r; return dx * dx + dy * dy <= rr * rr; }
  if(a->shape == 1 || b->shape == 1){
    Sp *c = a->shape == 1 ? a : b, *o = c == a ? b : a; double l, t, r, bt; sp_box(o, &l, &t, &r, &bt);
    double qx = fmax(l, fmin(c->x, r)), qy = fmax(t, fmin(c->y, bt)), dx = c->x - qx, dy = c->y - qy; return dx * dx + dy * dy <= c->r * c->r;
  }
  double l1, t1, r1, b1, l2, t2, r2, b2; sp_box(a, &l1, &t1, &r1, &b1); sp_box(b, &l2, &t2, &r2, &b2);
  return l1 < r2 && l2 < r1 && t1 < b2 && t2 < b1;
}
static int sp_pt(Sp *s, double px, double py){
  if(s->shape == 1){ double dx = px - s->x, dy = py - s->y; return dx * dx + dy * dy <= s->r * s->r; }
  double l, t, r, b; sp_box(s, &l, &t, &r, &b); return px >= l && px <= r && py >= t && py <= b;
}
static void sp_apply(Stmt *x, Sp *s){
  for(int i = 0; SPN[i]; i++){ double *p = sp_pi(s, i); *p = od(x, SPN[i], *p); }
  const char *sh = og(x, "shape");
  if(sh && *sh){ s->shape = !strcasecmp(sh, "rect") ? 0 : !strcasecmp(sh, "circle") ? 1 : !strcasecmp(sh, "text") ? 2 : (lerr("Dyn2D: Shape must be rect, circle or text"), 0); }
  s->col = oc(x, "color", s->col); s->layer = (int)od(x, "layer", s->layer); s->tsz = (int)od(x, "size", s->tsz);
  const char *v = og(x, "visible"); if(v && *v) s->vis = evi(v) != 0;
  v = og(x, "bounds"); if(v && *v) s->bounds = evi(v) != 0;
  char *tx = os_(x, "text"); if(tx){ free(s->text); s->text = tx; }
}
static double ease_(int e, double p){ return e == 1 ? p * p : e == 2 ? 1 - (1 - p) * (1 - p) : e == 3 ? (p < 0.5 ? 2 * p * p : 1 - 2 * (1 - p) * (1 - p)) : p; }
static void do_dyn(Stmt *x){
  win_ensure();
  char op[32]; snprintf(op, sizeof op, "%s", hw_(x, 1)); for(char *c = op; *c; c++) *c = (char)tolower((unsigned char)*c);
  const char *id = og(x, "id");
  if(!strcmp(op, "sprite")){
    if(!id || !*id) lerr("Dyn2D:Sprite needs Id");
    Sp *s = sp_find(id);
    if(!s){
      if(D2.n == D2.cap){ D2.cap = D2.cap ? D2.cap * 2 : 16; D2.s = xr(D2.s, (size_t)D2.cap * sizeof(Sp)); }
      s = &D2.s[D2.n++]; memset(s, 0, sizeof *s); s->id = xs(id); s->w = s->h = 10; s->r = 5; s->bounce = 1; s->vis = 1; s->tsz = 8; s->seq = D2.seq++;
      s->col.r = s->col.g = s->col.b = 255;
    }
    sp_apply(x, s);
  } else if(!strcmp(op, "set")){
    Sp *s = id ? sp_find(id) : NULL; if(!s) lerr("Dyn2D: sprite not found: %s", id ? id : "(no Id)"); sp_apply(x, s);
  } else if(!strcmp(op, "remove")){
    Sp *s = id ? sp_find(id) : NULL; if(!s) lerr("Dyn2D: sprite not found: %s", id ? id : "(no Id)");
    free(s->id); free(s->text); memmove(s, s + 1, (size_t)(D2.s + D2.n - 1 - s) * sizeof(Sp)); D2.n--;
  } else if(!strcmp(op, "clear")){
    for(int i = 0; i < D2.n; i++){ free(D2.s[i].id); free(D2.s[i].text); } D2.n = 0;
  } else if(!strcmp(op, "tween")){
    Sp *s = id ? sp_find(id) : NULL; if(!s) lerr("Dyn2D: sprite not found: %s", id ? id : "(no Id)");
    const char *pr = og(x, "prop"); int pi = pr ? sp_pidx(pr) : -1; if(pi < 0) lerr("Dyn2D:Tween needs Prop (x, y, w, h, r, vx, vy, ax, ay or bounce)");
    const char *ez = og(x, "ease"); int ea = !ez ? 0 : !strcasecmp(ez, "in") ? 1 : !strcasecmp(ez, "out") ? 2 : !strcasecmp(ez, "inout") ? 3 : 0;
    double to = od(x, "to", *sp_pi(s, pi)), from = od(x, "from", *sp_pi(s, pi)), ms = od(x, "ms", 1000); if(ms < 1) ms = 1;
    int k = -1; for(int i = 0; i < s->ntw; i++) if(s->tw[i].p == pi) k = i;
    if(k < 0){ if(s->ntw >= 4) lerr("Dyn2D: too many tweens on one sprite (max 4)"); k = s->ntw++; }
    s->tw[k].p = pi; s->tw[k].from = from; s->tw[k].to = to; s->tw[k].t0 = now(); s->tw[k].dur = ms; s->tw[k].ease = ea; *sp_pi(s, pi) = from;
  } else if(!strcmp(op, "step")){
    double dt = od(x, "dt", 1), tn = now(); int W = S.fb ? S.fb->w : 320, H = S.fb ? S.fb->h : 180;
    for(int i = 0; i < D2.n; i++){
      Sp *s = &D2.s[i];
      for(int k = 0; k < s->ntw; ){
        Tw *w = &s->tw[k]; double p = (tn - w->t0) * 1000.0 / w->dur; int done = p >= 1; if(done) p = 1;
        *sp_pi(s, w->p) = w->from + (w->to - w->from) * ease_(w->ease, p);
        if(done){ memmove(w, w + 1, (size_t)(s->ntw - k - 1) * sizeof(Tw)); s->ntw--; } else k++;
      }
      s->vx += s->ax * dt; s->vy += s->ay * dt; s->x += s->vx * dt; s->y += s->vy * dt;
      if(s->bounds){
        double l, t, r, b; sp_box(s, &l, &t, &r, &b);
        if(l < 0){ s->x -= l; s->vx = -s->vx * s->bounce; } if(r > W){ s->x -= r - W; s->vx = -s->vx * s->bounce; }
        if(t < 0){ s->y -= t; s->vy = -s->vy * s->bounce; } if(b > H){ s->y -= b - H; s->vy = -s->vy * s->bounce; }
      }
    }
  } else if(!strcmp(op, "draw")){
    if(!S.fb) S.fb = fb_new(320, 180);
    int ci = oi(x, "clear"); if(ci >= 0) fb_rect(S.fb, 0, 0, S.fb->w, S.fb->h, cs_(&x->o[ci], 1));
    int *ord = xm((size_t)(D2.n + 1) * sizeof(int)); for(int i = 0; i < D2.n; i++) ord[i] = i;
    for(int i = 1; i < D2.n; i++){ int v = ord[i], j = i - 1; while(j >= 0 && (D2.s[ord[j]].layer > D2.s[v].layer || (D2.s[ord[j]].layer == D2.s[v].layer && D2.s[ord[j]].seq > D2.s[v].seq))){ ord[j + 1] = ord[j]; j--; } ord[j + 1] = v; }
    for(int i = 0; i < D2.n; i++){
      Sp *s = &D2.s[ord[i]]; if(!s->vis) continue;
      if(s->shape == 1) fb_circle(S.fb, (ll)lround(s->x), (ll)lround(s->y), (ll)lround(s->r), s->col, 1);
      else if(s->shape == 2){ int sc = s->tsz / 8; if(sc < 1) sc = 1; fb_text(S.fb, (ll)lround(s->x), (ll)lround(s->y), s->text ? s->text : "", s->col, sc); }
      else fb_rect(S.fb, (ll)lround(s->x), (ll)lround(s->y), (ll)lround(s->w), (ll)lround(s->h), s->col);
    }
    free(ord);
  } else if(!strcmp(op, "hit")){
    const char *ia = og(x, "a"), *ib = og(x, "b"); Sp *a = ia ? sp_find(ia) : NULL; if(!a) lerr("Dyn2D:Hit: sprite not found: %s", ia ? ia : "(no A)");
    int r;
    if(ib){ Sp *b = sp_find(ib); if(!b) lerr("Dyn2D:Hit: sprite not found: %s", ib); r = sp_hit(a, b); }
    else if(og(x, "x") && og(x, "y")) r = sp_pt(a, od(x, "x", 0), od(x, "y", 0));
    else lerr("Dyn2D:Hit needs B, or X and Y for a point test");
    into(x, Vn(r));
  } else if(!strcmp(op, "get")){
    Sp *s = id ? sp_find(id) : NULL; if(!s) lerr("Dyn2D: sprite not found: %s", id ? id : "(no Id)");
    const char *pr = og(x, "prop"); int pi = pr ? sp_pidx(pr) : -1; if(pi < 0) lerr("Dyn2D:Get needs Prop");
    into(x, Vn(*sp_pi(s, pi)));
  } else lerr("Dyn2D: unknown operation: %s (use Sprite, Set, Remove, Clear, Tween, Step, Draw, Hit or Get)", hw_(x, 1));
}

/* ================================================================ UI (UIVisuals): Button, Label, Slider, Check, TextBox, Set, Clear */
enum { UW_BUTTON, UW_LABEL, UW_SLIDER, UW_CHECK, UW_TEXTBOX, UW_TEXTAREA, UW_DROPDOWN, UW_LIST, UW_PROGRESS, UW_TITLE, UW_SEP, UW_BREAK };
static const char *UWN[] = {"button", "label", "slider", "check", "textbox", "textarea", "dropdown", "list", "progress", "title", "separator", "break", NULL};
typedef struct { char *id, *text, *val, *opts, *syn, *cur; int type, rows, cols, go; double mn, mx; unsigned rev; Scope *sc; } UW;
static struct { UW *w; int n, cap; } UI;
static int ui_isstr(int t){ return t == UW_TEXTBOX || t == UW_TEXTAREA || t == UW_DROPDOWN || t == UW_LIST; }
static int ui_isvar(int t){ return ui_isstr(t) || t == UW_SLIDER || t == UW_CHECK; }
static UW *ui_find(const char *id){ for(int i = 0; i < UI.n; i++) if(!strcmp(UI.w[i].id, id)) return &UI.w[i]; return NULL; }
static void ui_json(Buf *b){                                /* caller holds WM */
  Val root = Vd(); Dict *D = DP(root); dset(D, "v", Vn(S.win.uver));
  Val l = Vl();
  for(int i = 0; i < UI.n; i++){
    UW *w = &UI.w[i]; Val d = Vd(); Dict *E = DP(d);
    dset(E, "id", Vs(w->id)); dset(E, "t", Vs(UWN[w->type])); dset(E, "text", Vs(w->text)); dset(E, "val", Vs(w->val));
    dset(E, "min", Vn(w->mn)); dset(E, "max", Vn(w->mx)); dset(E, "rev", Vn(w->rev));
    dset(E, "opts", Vs(w->opts ? w->opts : "")); dset(E, "syn", Vs(w->syn ? w->syn : "")); dset(E, "rows", Vn(w->rows)); dset(E, "cols", Vn(w->cols)); dset(E, "goto", Vn(w->go));
    lpush(LP(l), d);
  }
  dset(D, "w", l); jdump(b, root); vdrop(root);
}
/* called by the server thread when the user edits a widget */
static int ui_user(const char *id, const char *val, const char *sub){
  (void)sub; int num = 0; pthread_mutex_lock(&WM);
  UW *w = ui_find(id);
  if(w){ num = w->type == UW_SLIDER || w->type == UW_CHECK; if(ui_isvar(w->type)){ free(w->val); w->val = xs(val); } }
  pthread_mutex_unlock(&WM); return num;
}
/* big text from a TextArea (POST /put): too large for the event URL, so it goes straight into the widget and a normal input event is queued */
static void ui_put(const char *id, const char *body, size_t len){
  pthread_mutex_lock(&WM); UW *w = ui_find(id); int ok = w && w->type == UW_TEXTAREA;
  if(ok){ free(w->val); w->val = xsn(body, len); }
  pthread_mutex_unlock(&WM);
  if(ok){ Ev ev; memset(&ev, 0, sizeof ev); ev.kind = EV_UI; snprintf(ev.sub, sizeof ev.sub, "input"); snprintf(ev.id, sizeof ev.id, "%s", id); eq_push(&ev); }
}
static void ui_cursor(const char *id, const char *val){      /* "line,col,selStart,selEnd" of a TextArea: stored, no event */
  pthread_mutex_lock(&WM); UW *w = ui_find(id); if(w){ free(w->cur); w->cur = xs(val); } pthread_mutex_unlock(&WM);
}
/* runs in the event thread: the widget value becomes the variable named like its Id */
static void ui_var(const char *id, const char *val){
  pthread_mutex_lock(&WM); UW *w = ui_find(id); Scope *sc = w ? w->sc : NULL; int ty = w ? w->type : -1;
  char *sv = (w && ui_isstr(ty)) ? xs(w->val) : NULL; pthread_mutex_unlock(&WM);
  if(!sc || !ui_isvar(ty)){ free(sv); return; }
  if(sv){ dset(sc->v, id, Vs(sv)); free(sv); } else dset(sc->v, id, Vn(atof(val)));
}
static int ui_ident(const char *s){ if(!(isalpha((unsigned char)*s) || *s == '_')) return 0; for(; *s; s++) if(!(isalnum((unsigned char)*s) || *s == '_')) return 0; return 1; }
static void ui_free(UW *w){ free(w->id); free(w->text); free(w->val); free(w->opts); free(w->syn); free(w->cur); }
static void do_ui(Stmt *x, Scope *sc){
  if(!win_ensure()) lerr("UI: the window is disabled (--no-window)");
  char op[32]; snprintf(op, sizeof op, "%s", hw_(x, 1)); for(char *c = op; *c; c++) *c = (char)tolower((unsigned char)*c);
  if(!strcmp(op, "clear")){
    pthread_mutex_lock(&WM); for(int i = 0; i < UI.n; i++) ui_free(&UI.w[i]); UI.n = 0; S.win.uver++; pthread_mutex_unlock(&WM); return;
  }
  int ty = -1; for(int i = 0; UWN[i]; i++) if(!strcmp(op, UWN[i])) ty = i;
  const char *id = og(x, "id"); static int auto_n; char autoid[24];
  if((!id || !*id) && (ty == UW_TITLE || ty == UW_SEP || ty == UW_BREAK)){ snprintf(autoid, sizeof autoid, "_w%d", ++auto_n); id = autoid; }
  if(!id || !ui_ident(id)) lerr("UI: a valid Id is missing, for example \"Id;button1\"");
  const char *lb0 = og(x, "label"); if(!lb0) lb0 = og(x, "text");
  if(!strcmp(op, "get")){                                  /* UI:Get Id;x [What;value|cursor|options] Into;v */
    const char *wh = og(x, "what"); Val r = Vu(); int found = 0;
    pthread_mutex_lock(&WM); UW *w = ui_find(id);
    if(w){
      found = 1;
      if(wh && !strcasecmp(wh, "cursor")){ int a[4] = {1, 1, 0, 0}; if(w->cur) sscanf(w->cur, "%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3]); r = Vl(); for(int i = 0; i < 4; i++) lpush(LP(r), Vn(a[i])); }
      else if(wh && !strcasecmp(wh, "options")){
        r = Vl(); const char *o = w->opts ? w->opts : ""; const char *b = o;
        if(*o) for(const char *q = o; ; q++) if(*q == '|' || !*q){ lpush(LP(r), Vsn(b, (int)(q - b))); b = q + 1; if(!*q) break; }
      } else if(ui_isstr(w->type)) r = Vs(w->val); else r = Vn(atof(w->val));
    }
    pthread_mutex_unlock(&WM);
    if(!found) lerr("UI: widget does not exist %s", id);
    into(x, r); return;
  }
  char *lb = fm(lb0 ? lb0 : "");
  if(!strcmp(op, "set")){
    UW *w = ui_find(id); if(!w){ free(lb); lerr("UI: widget does not exist %s", id); }
    const char *v = og(x, "value"), *tx = og(x, "text"), *oo = og(x, "options"), *sy = og(x, "syntax"), *gt = og(x, "goto"); if(!tx) tx = og(x, "label");
    char *ntx = tx ? fm(tx) : NULL, *nv = NULL, *nopt = oo ? fm(oo) : NULL, *nsy = sy ? fm(sy) : NULL; Val vv = Vu(); int hasv = 0;
    if(v && (ui_isvar(w->type) || w->type == UW_PROGRESS)){
      vv = ev(v); hasv = 1;
      if(w->type == UW_CHECK) nv = xs(truthy(vv) ? "1" : "0"); else nv = jstr(vv);
    }
    double nmn = w->mn, nmx = w->mx; if(og(x, "min")) nmn = od(x, "min", w->mn); if(og(x, "max")) nmx = od(x, "max", w->mx);
    int ngo = gt ? (int)evi(gt) : 0, nrows = og(x, "rows") ? (int)od(x, "rows", w->rows) : w->rows;
    pthread_mutex_lock(&WM);
    if(ntx){ free(w->text); w->text = ntx; } if(nv){ free(w->val); w->val = nv; }
    if(nopt){ free(w->opts); w->opts = nopt; } if(nsy){ free(w->syn); w->syn = nsy; }
    w->mn = nmn; w->mx = nmx; w->go = ngo; w->rows = nrows;
    w->rev++; S.win.uver++; pthread_mutex_unlock(&WM);
    if(hasv && ui_isvar(w->type)) dset((w->sc ? w->sc : sc)->v, id, vv); else vdrop(vv);
    free(lb); return;
  }
  if(ty < 0){ free(lb); lerr("UI: unknown widget: %s (Button, Label, Slider, Check, TextBox, TextArea, Dropdown, List, Progress, Title, Separator, Break, Set, Get, Clear)", hw_(x, 1)); }
  double mn = 0, mx = 100; char *val = xs(""), *opts = NULL, *syn = NULL; Val vv = Vu(); int rows = 0, cols = 0;
  if(ty == UW_SLIDER || ty == UW_PROGRESS){
    mn = od(x, "min", 0); mx = od(x, "max", 100); const char *v = og(x, "value");
    double d = v ? evd(v) : mn; vv = Vn(d); Buf b = {0}; fmtnum(&b, d); free(val); val = btake(&b);
  } else if(ty == UW_CHECK){
    const char *v = og(x, "value"); double d = v ? evd(v) : 0; vv = Vn(d); free(val); val = xs(d != 0 ? "1" : "0");
  } else if(ty == UW_TEXTBOX || ty == UW_TEXTAREA){
    char *v = os_(x, "value"); free(val); val = v ? v : xs(""); vv = Vs(val);
    if(ty == UW_TEXTAREA){ rows = (int)od(x, "rows", 16); if(rows < 3) rows = 3; if(rows > 80) rows = 80; cols = (int)od(x, "cols", 0); syn = os_(x, "syntax"); }
  } else if(ty == UW_DROPDOWN || ty == UW_LIST){
    opts = os_(x, "options"); if(!opts) opts = xs("");
    char *v = os_(x, "value"); if(!v || !*v){ free(v); const char *e = strchr(opts, '|'); v = e ? xsn(opts, (size_t)(e - opts)) : xs(opts); }
    free(val); val = v; vv = Vs(val); rows = (int)od(x, "rows", 8);
  }
  if(ty == UW_BUTTON && !*lb){ free(lb); lb = xs(id); }
  pthread_mutex_lock(&WM);
  UW *old = ui_find(id); if(old){ ui_free(old); memmove(old, old + 1, (size_t)(UI.w + UI.n - 1 - old) * sizeof(UW)); UI.n--; }
  if(UI.n == UI.cap){ UI.cap = UI.cap ? UI.cap * 2 : 16; UI.w = xr(UI.w, (size_t)UI.cap * sizeof(UW)); }
  UW *w = &UI.w[UI.n++]; memset(w, 0, sizeof *w); w->id = xs(id); w->text = lb; w->val = val; w->opts = opts; w->syn = syn; w->type = ty; w->mn = mn; w->mx = mx; w->rev = 1; w->sc = sc; w->rows = rows; w->cols = cols;
  S.win.uver++; pthread_mutex_unlock(&WM);
  scope_ref(sc);
  if(ui_isvar(ty)) dset(sc->v, id, vv); else vdrop(vv);
}

/* ================================================================ libraries: built-in ones + Log libraries found on disk */
static const char *LEXT[] = {".log+", ".log-", ".log++", ".logall", NULL};
static int file_ok(const char *p){ struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
static char *exe_dir(void){
  char buf[4096]; buf[0] = 0;
#ifdef _WIN32
  { DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf); if(!n || n >= sizeof buf) buf[0] = 0; }
#elif defined(__APPLE__)
  { uint32_t sz = sizeof buf; if(_NSGetExecutablePath(buf, &sz)) buf[0] = 0; }
#else
  { ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1); if(n > 0) buf[n] = 0; else buf[0] = 0; }
#endif
  if(!buf[0] && S.argv0) snprintf(buf, sizeof buf, "%s", S.argv0);
  fixslash(buf); char *d = pdirname(buf); if(!*d){ free(d); d = xs("."); } return d;
}
/* directories searched for libraries: 1 = relative to the program root, 0 = absolute */
static void lib_dirs(SV *dirs, SV *rel){
  svp(dirs, xs("libs")); svp(rel, xs("1")); svp(dirs, xs(".")); svp(rel, xs("1"));
  char *ed = exe_dir(); Buf b = {0}; bs(&b, ed); bs(&b, "/libs"); svp(dirs, btake(&b)); svp(rel, xs("0")); svp(dirs, ed); svp(rel, xs("0"));
  const char *env = getenv("LOGL_LIBS");
  if(env && *env){
#ifdef _WIN32
    const char dl = ';';
#else
    const char dl = ':';
#endif
    char *cp = xs(env); char *p = cp;
    while(p){ char *e = strchr(p, dl); if(e) *e = 0; if(*p){ char *q = xs(p); fixslash(q); svp(dirs, q); svp(rel, xs("0")); } p = e ? e + 1 : NULL; }
    free(cp);
  }
}
static char *find_lib(const char *name){
  SV dirs = {0}, rel = {0}; lib_dirs(&dirs, &rel);
  for(int i = 0; i < dirs.n; i++) for(int j = 0; LEXT[j]; j++){
    Buf b = {0}; bs(&b, dirs.v[i]); bc(&b, '/'); bs(&b, name); bs(&b, LEXT[j]); char *cand = btake(&b);
    char *probe = rel.v[i][0] == '1' ? pjoin(S.root_dir, cand) : xs(cand);
    int ok = file_ok(probe); free(probe);
    if(ok) return cand;
    free(cand);
  }
  return NULL;
}
static int is_builtin_lib(const char *name){
  char lo[128]; snprintf(lo, sizeof lo, "%s", name); for(char *c = lo; *c; c++) *c = (char)tolower((unsigned char)*c);
  return !strcmp(lo, "g3d") || !strcmp(lo, "graphics3d") || !strcmp(lo, "2ddynamics") || !strcmp(lo, "dyn2d") || !strcmp(lo, "ui") || !strcmp(lo, "uivisuals") || !strcmp(lo, "storage");
}
static int use_lib(Scope *sc, const char *name){
  char m[300];
  if(is_builtin_lib(name)){ win_ensure(); snprintf(m, sizeof m, "[lib] %s loaded from silicon", name); out(m); return ST_OK; }
  char *f = find_lib(name);
  if(!f){ snprintf(m, sizeof m, "[lib] WARNING: library '%s' not found (looked in libs/, the program folder, <logl folder>/libs and LOGL_LIBS)", name); out(m); return ST_OK; }
  SL *nodes = load(f, "host"); Scope *top = stop_(sc);
  reg_defs(nodes, top, top); char *p0 = S.path; S.path = pdirname(f);
  int st = ex(nodes, sc); S.path = p0; return st;
}
static int pkg_cmd(int argc, char **argv){
  static const struct { const char *n, *alias, *d; } CAT[] = {
    {"Graphics3D", "G3D", "Software 3D engine: cube, sphere, torus and pyramid, camera with lookAt, light with shininess, hidden faces and depth sorting. Log+ and Log- (Log++ has no screen)."},
    {"UIVisuals", "UI", "Buttons, labels, sliders, checkboxes and text boxes. Log+ and #+ lines of mixed files only."},
    {"Storage", "", "Save and load data between runs with Store:Save and Store:Load. All variants."},
    {"2DDynamics", "Dyn2D", "2D graphics: all the classic Screen:* commands plus Dyn2D: sprites, physics, tweens and collisions (Dyn2D is new: not in the web Log OS)."},
    {NULL, NULL, NULL}};
  const char *sub = argc > 2 ? argv[2] : "list", *name = argc > 3 ? argv[3] : NULL;
  if(!strcmp(sub, "list")){
    for(int i = 0; CAT[i].n; i++) printf("[built in] %s%s%s%s\n   %s\n", CAT[i].n, *CAT[i].alias ? " (" : "", CAT[i].alias, *CAT[i].alias ? ")" : "", CAT[i].d);
    S.root_dir = xs("."); SV dirs = {0}, rel = {0}; lib_dirs(&dirs, &rel); int any = 0;
    for(int i = 0; i < dirs.n; i++){
      if(!strcmp(dirs.v[i], ".")) continue;
      char *p = rel.v[i][0] == '1' ? pjoin(S.root_dir, dirs.v[i]) : xs(dirs.v[i]); DIR *d = opendir(p);
      if(d){ struct dirent *e; while((e = readdir(d))){ size_t l = strlen(e->d_name);
          for(int j = 0; LEXT[j]; j++){ size_t el = strlen(LEXT[j]); if(l > el && !strcmp(e->d_name + l - el, LEXT[j])){ if(!any){ puts("Libraries found on disk:"); any = 1; } printf("  %.*s   (%s/%s)\n", (int)(l - el), e->d_name, p, e->d_name); } } }
        closedir(d); }
      free(p);
    }
    if(!any) puts("Your own libraries: put NAME.log+ in a 'libs' folder (next to your program or next to logl) and use it with  Use:NAME");
    return 0;
  }
  if((!strcmp(sub, "install") || !strcmp(sub, "remove") || !strcmp(sub, "info")) && name){
    for(int i = 0; CAT[i].n; i++) if(!strcasecmp(name, CAT[i].n) || (*CAT[i].alias && !strcasecmp(name, CAT[i].alias))){
      if(!strcmp(sub, "info")) printf("%s: %s\n", CAT[i].n, CAT[i].d);
      else if(!strcmp(sub, "install")) printf("%s is built in: nothing to install\n", CAT[i].n);
      else printf("%s is built in and cannot be removed\n", CAT[i].n);
      return 0;
    }
    if(!strcmp(sub, "install")){ char *ed = exe_dir(); printf("To add the library '%s': copy %s.log+ into a 'libs' folder next to your program, or into %s/libs\nthen use it in your code with  Use:%s\n", name, name, ed, name); return 0; }
    printf("package not found: %s. Use: pkg list\n", name); return 1;
  }
  puts("usage: logl pkg list | install <name> | remove <name> | info <name>"); return 1;
}

/* ---------------------------------------------------------------- ejecucion */
static int lp(SL *b, Scope *sc){ /* 0 sigue, -1 break consumido, >0 propagar */
  int st = ex(b, sc);
  if(st == ST_BRK) return -1; if(st == ST_CNT || st == ST_OK) return 0; return st;
}
static int ex(SL *ns, Scope *sc){
  for(int i = 0; i < ns->n; i++){
    Stmt *x = ns->v[i];
    if(IRQ.any && IRQ.on) irq_deliver();
    if(chk_stop()) return ST_STOP;
    if((nthreads || gil_want) && ((++T->tick) & 127) == 0){ if(S.win.on) present_maybe(); gil_yield(); }
    T->cur = sc;
    jmp_buf jb, *old = T->jb; T->jb = &jb; int st;
    if(!setjmp(jb)){ st = one(x, sc); T->jb = old; }
    else {
      T->jb = old; const char *e = T->err; int pref = 0;
      if(e[0] == 'l' && e[1] == '.'){ const char *q = e + 2; while(isdigit((unsigned char)*q)) q++; pref = q > e + 2 && *q == ':'; }
      if(!pref){ char t[1024]; snprintf(t, sizeof t, "l.%d: %s", x->n, e); strncpy(T->err, t, sizeof T->err - 1); T->err[sizeof T->err - 1] = 0; }
      longjmp(*old, 1);
    }
    if(st) return st;
  }
  return ST_OK;
}
static int sleep_stmt(double ms){ return sleep_ms(ms, 0); }

static int one(Stmt *x, Scope *sc){
  int k = x->kw; SV *h = &x->h; T->cur = sc;
  switch(k){
  case K_use:
    if(x->q.n){
      char *j = pjoin(S.path, x->q.v[0]), *f = normpath(j); free(j);
      const char *fr0 = "host"; if(x->q.n > 1){ char *sc2 = strchr(x->q.v[1], ';'); if(sc2 && sc2[1]) fr0 = sc2 + 1; }
      char *fr = lowerdup(fr0); SL *nodes = load(f, fr); Scope *top = stop_(sc);
      reg_defs(nodes, top, top); char *p0 = S.path; S.path = pdirname(f);
      int st = ex(nodes, sc); S.path = p0; return st;
    } else return use_lib(sc, h->n > 1 ? h->v[1] : "");
    break;
  case K_print: { char *s = fm(x->q.n ? x->q.v[0] : ""); out(s); free(s); break; }
  case K_serial: { const char *q = x->q.n ? x->q.v[0] : ""; if(!strncasecmp(q, "write;", 6)) q += 6; char *s = fm(q); out(s); free(s); break; }
  case K_sound: {
    const char *fq = NULL, *ms = NULL;
    for(int i = 0; i < x->no; i++){ if(!strcasecmp(x->o[i].v[0], "freq") && x->o[i].n > 1) fq = x->o[i].v[1]; if(!strcasecmp(x->o[i].v[0], "ms") && x->o[i].n > 1) ms = x->o[i].v[1]; }
    Val f = ev(fq ? fq : "440"), m = ev(ms ? ms : "200"); char *a = jstr(f), *b = jstr(m);
    if(S.win.on) win_sound(tonum(f), tonum(m)); else { char t[256]; snprintf(t, sizeof t, "\a[sound %s Hz, %s ms]", a, b); out(t); }
    double d = tonum(m); free(a); free(b); vdrop(f); vdrop(m); if(sleep_stmt(d)) return ST_STOP; break;
  }
  case K_var: case K_set: do_vs(x, k, sc); break;
  case K_push: case K_pop: case K_remove: case K_insert: case K_sort: case K_shuffle: do_lst(x, k, sc); break;
  case K_split: do_split(x, sc); break;
  case K_each: {
    Val a = ev(arg(x, 0)); if(a.t != T_LIST){ vdrop(a); lerr("Each needs a list"); }
    List *l = LP(a); int n = l->n; Val *cp = xm((n + 1) * sizeof(Val)); for(int i = 0; i < n; i++) cp[i] = vref(l->a[i]);
    int r = 0;
    for(int i = 0; i < n && !r; i++){ dset(sc->v, hw_(x, 1), vref(cp[i])); r = lp(&x->body, sc); }
    for(int i = 0; i < n; i++) vdrop(cp[i]); free(cp); vdrop(a); if(r > 0) return r; break;
  }
  case K_def: fn_set(stop_(sc), h->v[1], x, stop_(sc)); break;
  case K_call: return docall(x, sc);
  case K_return: vdrop(T->retv); T->retv = x->q.n ? valof(x) : Vu(); return ST_RET;
  case K_if: {
    Val c = ev(arg(x, 0)); int t = truthy(c); vdrop(c); T->cur = sc;
    if(t) return ex(&x->body, sc); else if(x->alt.n) return ex(&x->alt, sc); break;
  }
  case K_switch: {
    Val v = ev(arg(x, 0)); Stmt *hit = NULL, *dflt = NULL; T->cur = sc;
    for(int i = 0; i < x->body.n && !hit; i++){
      Stmt *c = x->body.v[i];
      if(c->kw == K_default){ dflt = c; continue; }
      if(c->kw != K_case) lerr("Only Case(...) and Default are allowed directly inside a Switch");
      for(int j = 0; j < c->a.n && !hit; j++){ Val w = ev(c->a.v[j]); int eq = veq(v, w); vdrop(w); if(eq) hit = c; }
    }
    vdrop(v); T->cur = sc;
    if(!hit) hit = dflt;
    if(hit) return ex(&hit->body, sc);
    break;
  }
  case K_case: case K_default: lerr("%s is only valid directly inside a Switch", h->v[0]);
  case K_while:
    for(;;){ T->cur = sc; Val c = ev(arg(x, 0)); int t = truthy(c); vdrop(c); if(!t) break; int r = lp(&x->body, sc); if(r < 0) break; if(r > 0) return r; }
    break;
  case K_repeat: { ll n = evi(arg(x, 0)); for(ll i = 0; i < n; i++){ int r = lp(&x->body, sc); if(r < 0) break; if(r > 0) return r; } break; }
  case K_for: {
    double a = evd(arg(x, 0)), b = evd(arg(x, 1)), st = x->a.n > 2 ? evd(x->a.v[2]) : 1;
    if(st == 0) lerr("The For step cannot be 0");
    for(double i = a; st > 0 ? i < b : i > b; i += st){ dset(sc->v, hw_(x, 1), Vn(i)); int r = lp(&x->body, sc); if(r < 0) break; if(r > 0) return r; }
    break;
  }
  case K_break: return ST_BRK;
  case K_continue: return ST_CNT;
  case K_setup: return ex(&x->body, sc);
  case K_loop:
    while(!S.stop){ int r = lp(&x->body, sc); if(r < 0) break; if(r > 0) return r; if(sleep_stmt(1)) return ST_STOP; }
    break;
  case K_wait: {
    if(!x->no || x->o[0].n < 1) lerr("Wait: missing option");
    SV *o0 = &x->o[0]; const char *m = o0->v[0];
    if(!strcasecmp(m, "press")){ outn("[Press Enter to continue] "); char *l = readline_nogil(); if(!l) lerr("End of input (EOF)"); free(l); }
    else if(!strcasecmp(m, "until")){
      Buf c = {0}; for(int i = 1; i < o0->n; i++){ if(i > 1) bc(&c, ';'); bs(&c, o0->v[i]); }
      char *cs = btake(&c);
      for(;;){ T->cur = sc; Val v = ev(cs); int t = truthy(v); vdrop(v); if(t) break; if(sleep_stmt(10)){ free(cs); return ST_STOP; } }
      free(cs);
    } else {
      if(o0->n < 2) lerr("Wait: missing value");
      double v = evd(o0->v[1]); if(!strcasecmp(m, "s") || !strcasecmp(m, "sec")) v *= 1000; else if(!strcasecmp(m, "us")) v /= 1000;
      if(sleep_stmt(v)) return ST_STOP;
    }
    break;
  }
  case K_try: {
    jmp_buf jb, *old = T->jb; volatile int depth = T->depth; char *volatile p0 = S.path; Scope *volatile vsc = sc; T->jb = &jb;
    if(!setjmp(jb)){ int st = ex(&x->body, sc); T->jb = old; return st; }
    sc = vsc; T->jb = old; T->depth = depth; S.path = p0; T->cur = sc;
    if(x->cv && *x->cv){
      const char *e = T->err; if(e[0] == 'l' && e[1] == '.'){ const char *q = e + 2; while(isdigit((unsigned char)*q)) q++; if(q > e + 2 && q[0] == ':' && q[1] == ' ') e = q + 2; }
      dset(sc->v, x->cv, Vs(e));
    }
    return ex(&x->cat, sc);
  }
  case K_assert: {
    const char *c = og(x, "cond"); if(!c) lerr("Assert needs Cond");
    Val v = ev(c); int t = truthy(v); vdrop(v);
    if(!t){ const char *tx = og(x, "text"); char *m = fm(tx && *tx ? tx : c); char buf[600]; snprintf(buf, sizeof buf, "%s", m); free(m); lerr("Assert failed: %s", buf); }
    break;
  }
  case K_yield: gil_yield(); break;
  case K_stop: if(stop_(sc)->xd) return ST_RET; S.stop = 1; return ST_STOP;
  case K_panic: {
    const char *q = x->q.n ? x->q.v[0] : "no message"; if(!strncasecmp(q, "text;", 5)) q += 5;
    char *m = fm(q); fprintf(stderr, "*** KERNEL PANIC: %s ***\n", m); char b[600]; snprintf(b, sizeof b, "%s", m); free(m); lerr("KERNEL PANIC: %s", b);
  }
  case K_input: {
    const char *n = hw_(x, 1); if(!shas(sc, n)) lerr("Undeclared variable: %s (declare it first with Var)", n);
    const char *pq0 = x->q.n ? x->q.v[0] : ""; if(!strncasecmp(pq0, "prompt;", 7)) pq0 += 7; char *pq = fm(pq0);
    const char *dflt = og(x, "default"), *lo = og(x, "min"), *hi = og(x, "max");
    for(;;){
      if(*pq){ outn(pq); outn(" "); } else outn("> ");
      char *r = readline_nogil(); if(!r){ free(pq); lerr("End of input (EOF)"); }
      char *t = strip_dup(r); if(!*t && dflt){ Val d = ev(dflt); free(r); r = jstr(d); vdrop(d); } free(t);
      Val cur = sget(sc, n); int isstr = sty(sc, n) == 6 || cur.t == T_STR; vdrop(cur);
      if(isstr){ ssetv(sc, n, Vs(r)); free(r); break; }
      char *e; double v = strtod(r, &e); int ok = *r && !*e; free(r);
      if(!ok){ out("Enter a number"); continue; }
      if(lo && v < evd(lo)){ Val m = ev(lo); char *ms = jstr(m); char t[128]; snprintf(t, sizeof t, "The minimum is %s", ms); out(t); free(ms); vdrop(m); continue; }
      if(hi && v > evd(hi)){ Val m = ev(hi); char *ms = jstr(m); char t[128]; snprintf(t, sizeof t, "The maximum is %s", ms); out(t); free(ms); vdrop(m); continue; }
      ssetv(sc, n, Vn(v)); break;
    }
    free(pq); break;
  }
  case K_g3d: case K_graphics3d: do_g3d(x); break;
  case K_ui: do_ui(x, sc); break;
  case K_dyn2d: do_dyn(x); break;
  case K_ram: do_ram(x); break;
  case K_cpu: do_cpu(x); break;
  case K_irq: do_irq(x); break;
  case K_pci: do_pci(x); break;
  case K_dev: do_dev(x); break;
  case K_admin: do_admin(x); break;
  case K_console: do_console(x, sc); break;
  case K_sys: {
    into_need(x); Val d = Vd(); Dict *D = DP(d);
    dset(D, "ghz", Vn(3)); dset(D, "cpu", Vs("Log-SoC 3.0 GHz")); dset(D, "ram_mb", Vn(4096)); dset(D, "mem_bytes", Vn((double)S.memsz));
    dset(D, "width", Vn(S.fb ? S.fb->w : 0)); dset(D, "height", Vn(S.fb ? S.fb->h : 0)); dset(D, "kernel", Vn(0));
    dset(D, "uptime_ms", Vn((double)(long)((now() - S.t0) * 1000)));
    int nt = 0; for(int i = 0; i < S.ntasks; i++) nt += S.tasks[i]->on; dset(D, "tasks", Vn(nt)); dset(D, "version", Vs("1.0-c"));
    sys_host(D);
    dset(D, "pid", Vn(getpid())); dset(D, "hw", Vb(S.hw)); dset(D, "allow_disk", Vb(S.allow_disk)); dset(D, "allow_pins", Vb(S.allow_pins)); dset(D, "allow_net", Vb(S.allow_net));
    into(x, d); break;
  }
  case K_mem: do_mem(x); break;
  case K_disk: do_disk(x); break;
  case K_host: do_host(x); break;
  case K_i2c: do_i2c(x); break;
  case K_spi: do_spi(x); break;
  case K_uart: do_uart(x); break;
  case K_watchdog: do_watchdog(x); break;
  case K_store: {
    const char *w = hw_(x, 1); const char *ko = og(x, "key"); Buf kb = {0}; bs(&kb, "logdata:"); bs(&kb, ko ? ko : ""); char *key = btake(&kb);
    int err; size_t n; char *tx = readfile(S.stpath, &n); Val d = tx ? jparse(tx, &err) : Vu(); if(!tx || err || d.t != T_DICT){ vdrop(d); d = Vd(); } free(tx);
    if(!strcasecmp(w, "save")){
      const char *vs_ = og(x, "value"); dset(DP(d), key, ev(vs_ ? vs_ : "undefined"));
      Buf b = {0}; jdump(&b, d); char *s = btake(&b); writefile(S.stpath, s, b.n, "wb"); free(s);
    } else if(!strcasecmp(w, "load")){
      const char *to = og(x, "into"), *df = og(x, "default"); if(!to || !shas(sc, to)) lerr("Undeclared variable: %s", to ? to : "(null)");
      Val *p = dget(DP(d), key); Val v = p ? vref(*p) : (df ? ev(df) : sget(sc, to)); ssetv(sc, to, v);
    } else lerr("Store: use Save or Load");
    vdrop(d); free(key); break;
  }
  case K_import: do_import(x, sc); break;
  case K_screen: case K_display: do_scr(x); break;
  case K_reg: {
    const char *w = NULL; static const char *HW[] = {"map","unmap","read","write","set","clear","toggle",NULL};
    for(int i = 1; i < h->n && !w; i++) for(int j = 0; HW[j]; j++) if(!strcasecmp(h->v[i], HW[j])){ w = HW[j]; break; }
    if(w) do_reg(x, w); else do_scr(x); break;
  }
  case K_port: do_port(x, hw_(x, 1)[0] ? (!strcasecmp(hw_(x, 1), "in") ? "in" : !strcasecmp(hw_(x, 1), "out") ? "out" : "?") : "?"); break;
  case K_expect: {
    if(!S.fb) lerr("Screen not initialized");
    ll ax = evi(arg(x, 0)), ay = evi(arg(x, 1)); Col d = fb_get(S.fb, ax, ay); ll R = -1, G = -1, B = -1;
    if(x->no) for(int i = 0; i < x->o[0].n; i++){ char *eq = strchr(x->o[0].v[i], '='); if(!eq) continue; char kk = toupper((unsigned char)x->o[0].v[i][0]); ll v = evi(eq + 1); if(kk == 'R') R = v; else if(kk == 'G') G = v; else if(kk == 'B') B = v; }
    if(d.r != R || d.g != G || d.b != B){
      const char *m = "EXPECT"; char mm[256] = ""; if(x->els){ const char *a = strchr(x->els, '"'); if(a){ const char *b = strchr(a + 1, '"'); if(b){ snprintf(mm, sizeof mm, "%.*s", (int)(b - a - 1), a + 1); m = mm; } } }
      lerr("HALT %s: pixel (%lld,%lld) expected (%lld, %lld, %lld) but read (%d, %d, %d)", m, ax, ay, R, G, B, d.r, d.g, d.b);
    }
    char t[128]; snprintf(t, sizeof t, "Expect OK: pixel (%lld,%lld) verified", ax, ay); out(t); break;
  }
  case K_task:
    if(h->n < 2) lerr("Task needs a name: Task:enemy{ ... }");
    if(!x->has_body) lerr("Task needs a { } block");
    start_task(h->v[1], &x->body, scope_new(sc), 0, -1); break;
  case K_spawn: {
    Fn *f; const char *nm = hw_(x, 1); char full[256];
    if(h->n > 2){ Mod *m = mod_alias(h->v[1]); if(!m) lerr("Module not imported: %s", h->v[1]); f = fn_get(stop_(m->sc), h->v[2]); snprintf(full, sizeof full, "%s:%s", h->v[1], h->v[2]); nm = full; if(!f) lerr("Function not defined in module %s: %s", h->v[1], h->v[2]); }
    else { f = fn_get(stop_(sc), h->v[1]); if(!f) lerr("Function not defined: %s", h->v[1]); }
    if(x->a.n != f->x->a.n) lerr("%s espera %d argumento(s) y recibio %d", nm, f->x->a.n, x->a.n);
    Scope *loc = scope_new(f->sc); for(int i = 0; i < x->a.n; i++) sdecl(loc, f->x->a.v[i], ev(x->a.v[i]), -1);
    S.tid++; const char *io = og(x, "id"); char *id = io ? fm(io) : xs("");
    char dn[300]; if(!*id) snprintf(dn, sizeof dn, "%s#%d", h->v[1], S.tid); start_task(*id ? id : dn, &f->x->body, loc, 0, -1); free(id); break;
  }
  case K_kill: {
    char *nm = h->n > 1 ? xs(h->v[1]) : fm(og(x, "id") ? og(x, "id") : "");
    if(!strcasecmp(nm, "all")){ for(int i = 0; i < S.ntasks; i++) S.tasks[i]->stop = 1; }
    else { Task *t = task_find(nm); if(!t) lerr("Unknown task: %s", nm); t->stop = 1; if(t->proc) killproc(t->proc); }
    free(nm); break;
  }
  case K_join: {
    char *nm = h->n > 1 ? xs(h->v[1]) : fm(og(x, "id") ? og(x, "id") : ""); Task *t = task_find(nm); if(!t) lerr("Unknown task: %s", nm); free(nm);
    while(t->on){ if(chk_stop()) return ST_STOP; nap(10000); } break;
  }
  case K_lock: {
    const char *n = hw_(x, 1);
    for(;;){ Val *p = dget(DP(S.locks), n); if(!p || !p->u.n){ dset(DP(S.locks), n, Vn(1)); break; } if(chk_stop()) return ST_STOP; nap(1000); }
    break;
  }
  case K_unlock: dset(DP(S.locks), hw_(x, 1), Vn(0)); break;
  case K_send: { Val *q = dget(DP(S.queues), hw_(x, 1)); if(!q){ dset(DP(S.queues), hw_(x, 1), Vl()); q = dget(DP(S.queues), hw_(x, 1)); } lpush(LP(*q), valof(x)); break; }
  case K_recv: {
    const char *n = hw_(x, 1), *to = og(x, "into"), *okv = og(x, "ok"), *tm = og(x, "timeout");
    double dl = tm ? now() + evd(tm) / 1000 : INFINITY; int got = 0; Val v = Vu();
    for(;;){
      Val *q = dget(DP(S.queues), n);
      if(q && LP(*q)->n){ List *l = LP(*q); v = l->a[0]; memmove(&l->a[0], &l->a[1], (l->n - 1) * sizeof(Val)); l->n--; got = 1; break; }
      if(now() >= dl) break; if(chk_stop()) return ST_STOP; nap(5000);
    }
    T->cur = sc;
    if(got && to){ if(!shas(sc, to)) lerr("Undeclared variable: %s", to); ssetv(sc, to, v); } else vdrop(v);
    if(okv){ if(!shas(sc, okv)) lerr("Undeclared variable: %s", okv); ssetv(sc, okv, Vn(got)); }
    break;
  }
  case K_on: {
    const char *nm = hw_(x, 1);
    if(!strcasecmp(nm, "timer")){ const char *ev_ = og(x, "every"); ll ms = evi(ev_ && *ev_ ? ev_ : "1000"); if(ms < 10) ms = 10; S.tid++; char id[64]; snprintf(id, sizeof id, "timer#%d", S.tid); scope_ref(sc); start_task(id, &x->body, sc, 0, (int)ms); }
    else if(!strcasecmp(nm, "pin")){
      /* v2.1: On:Pin(5)("Rising") / ("Falling") / ("Change"): runs the block when the pin changes level */
      ll pn = x->a.n ? evi(x->a.v[0]) : -1; if(pn < 0 || pn >= 64) lerr("On:Pin needs a pin number 0-63, for example On:Pin(5)(\"Rising\")");
      const char *ed = x->no ? x->o[0].v[0] : "Change"; char edl[16];
      if(!strcasecmp(ed, "rising")) strcpy(edl, "rising"); else if(!strcasecmp(ed, "falling")) strcpy(edl, "falling"); else if(!strcasecmp(ed, "change")) strcpy(edl, "change");
      else lerr("On:Pin: use Rising, Falling or Change");
      if(S.nevh >= 64) lerr("Too many event handlers");
      if(S.hw){ int dm = -1; pin_hw((int)pn, "read", NULL, &dm); }   /* permission check + maps the GPIO */
      EvH *eh = &S.evh[S.nevh]; memset(eh, 0, sizeof *eh); eh->kind = EV_PIN; eh->body = &x->body; eh->sc = sc;
      snprintf(eh->id, sizeof eh->id, "%lld", pn); snprintf(eh->sub, sizeof eh->sub, "%s", edl);
      scope_ref(sc); S.nevh++; S.npinh++;
      start_evthread(); if(S.hw){ pinpoll_start(); pinpoll_watch((int)pn); }
      pin_watch[pn]++;
    }
    else {
      const char *a0 = x->no && x->o[0].n == 1 ? x->o[0].v[0] : "";
      int kind; char hid[64] = "";
      if(!strcasecmp(nm, "key")) kind = EV_KEY; else if(!strcasecmp(nm, "mouse")) kind = EV_MOUSE; else if(!strcasecmp(nm, "canvas")) kind = EV_CANVAS;
      else if(!strcasecmp(nm, "start_button")) kind = EV_START; else { kind = EV_UI; snprintf(hid, sizeof hid, "%s", nm); }
      if(S.nevh >= 64) lerr("Too many event handlers");
      EvH *eh = &S.evh[S.nevh]; memset(eh, 0, sizeof *eh); eh->kind = kind; eh->body = &x->body; eh->sc = sc;
      snprintf(eh->id, sizeof eh->id, "%s", hid);
      if(kind == EV_KEY || kind == EV_MOUSE) snprintf(eh->sub, sizeof eh->sub, "%s", a0);
      scope_ref(sc); S.nevh++;
      int w = (kind != EV_KEY || eh->sub[0] || S.win.on) ? win_ensure() : 0;
      if(!w && kind == EV_KEY) start_keys();
    }
    break;
  }
  case K_proc: {
    const char *w = hw_(x, 1); into_need(x);
    if(!strcasecmp(w, "list")){ Val l = Vl(); for(int i = 0; i < S.ntasks; i++) if(S.tasks[i]->on) lpush(LP(l), Vs(S.tasks[i]->name)); into(x, l); }
    else if(!strcasecmp(w, "alive")){ const char *io = og(x, "id"); char *id = fm(io ? io : ""); Task *t = task_find(id); free(id); into(x, Vn(t && t->on)); }
    else lerr("Proc: use List or Alive");
    break;
  }
  case K_pin: {
    ll n = h->n > 1 ? atoll(h->v[1]) : -1; if(n < 0 || n >= 64) lerr("Pin out of range (0-63)");
    if(!x->no) lerr("Pin: use Mode, Set, Toggle, Read, PWM, Duty, ADC, Analog or Safe");
    char t[96]; int res = -1;
    for(int oi_ = 0; oi_ < x->no; oi_++){   /* v2.1: several options in one statement, e.g. Pin:13("PWM;500","Duty;25") */
      SV *a = &x->o[oi_]; const char *c = a->v[0], *a1 = a->n > 1 ? a->v[1] : "";
      if(!strcasecmp(c, "set")){ int hi = !strcasecmp(a1, "high") || !strcmp(a1, "1"); int old = S.pins[n]; S.pins[n] = hi; if(S.hw) pin_hw((int)n, "set", a1, &res); else if(old != hi && pin_watch[n]) push_edge((int)n, hi); snprintf(t, sizeof t, "[pin %lld] = %d", n, S.pins[n]); out(t); }
      else if(!strcasecmp(c, "toggle")){ int old = S.pins[n]; S.pins[n] ^= 1; if(S.hw) pin_hw((int)n, "toggle", a1, &res); else if(pin_watch[n]) push_edge((int)n, S.pins[n]); (void)old; snprintf(t, sizeof t, "[pin %lld] = %d", n, S.pins[n]); out(t); }
      else if(!strcasecmp(c, "read")){ if(S.hw){ pin_hw((int)n, "read", a1, &res); S.pins[n] = res; } into(x, Vn(pin_level((int)n))); }
      else if(!strcasecmp(c, "mode")){ if(S.hw) pin_hw((int)n, "mode", a1, &res); }
      /* v2.1: Pin:N("Output") / Pin:N("Input[;PullUp|PullDown]") are in the manual (ch. 6) and the simulator accepts them; logl 2.0 rejected them.
       * Pull-up/down is accepted but NOT applied to the real GPIO yet (needs the GPPUD sequence): NOT IMPLEMENTED. */
      else if(!strcasecmp(c, "output")){ if(S.hw) pin_hw((int)n, "mode", "out", &res); }
      else if(!strcasecmp(c, "input")){ if(S.hw) pin_hw((int)n, "mode", "in", &res); }
      else if(!strcasecmp(c, "pwm")){
        double f = evd(a1);
        if(f == 0){ PST[n].pwm = 0; if(S.hw) pwm_hw_stop((int)n); snprintf(t, sizeof t, "[pin %lld] PWM off", n); out(t); }
        else {
          if(f < 1 || f > 100000) lerr("Pin: PWM frequency must be between 1 and 100000 Hz");
          if(S.hw){ need_perm(S.allow_pins, "Pin", "--allow-pins"); if(f > 1000) lerr("Pin: PWM on real pins is software PWM and goes up to 1000 Hz"); }
          PST[n].freq = f; PST[n].pwm = 1;
          if(S.hw){ pin_hw((int)n, "mode", "out", &res); pwm_hw_start((int)n); }
          snprintf(t, sizeof t, "[pin %lld] PWM %g Hz, duty %g%%", n, PST[n].freq, PST[n].duty); out(t);
        }
      }
      else if(!strcasecmp(c, "duty")){
        if(!PST[n].pwm) lerr("Pin: set PWM;frequency before Duty");
        double d = evd(a1); if(d < 0 || d > 100) lerr("Pin: Duty must be between 0 and 100");
        PST[n].duty = d; snprintf(t, sizeof t, "[pin %lld] PWM %g Hz, duty %g%%", n, PST[n].freq, PST[n].duty); out(t);
      }
      else if(!strcasecmp(c, "analog")){ double v = evd(a1); if(v < 0 || v > 3.3) lerr("Pin: Analog must be between 0 and 3.3 (volts)"); PST[n].analog = v; }
      else if(!strcasecmp(c, "adc")){
        into_need(x); int bits = *a1 ? (int)evi(a1) : 10; if(bits < 2 || bits > 16) lerr("Pin: ADC bits must be between 2 and 16");
        double v;
        if(S.hw){ need_perm(S.allow_pins, "Pin", "--allow-pins"); const char *dv = og(x, "dev"); v = adc_hw((int)n, dv ? (int)evi(dv) : 0); }   /* real: raw driver counts */
        else v = floor(PST[n].analog / 3.3 * (double)((1 << bits) - 1) + 0.5);                                                         /* virtual: Analog volts -> counts */
        into(x, Vn(v));
      }
      else if(!strcasecmp(c, "safe")){
        if(!strcasecmp(a1, "low")) PST[n].safe = 1; else if(!strcasecmp(a1, "high")) PST[n].safe = 2; else if(!strcasecmp(a1, "off")) PST[n].safe = 0;
        else lerr("Pin: Safe must be Low, High or Off");
        if(PST[n].safe) install_crash_handlers();
      }
      else if(oi_ == 0) lerr("Pin: use Mode, Set, Toggle, Read, PWM, Duty, ADC, Analog or Safe");
      /* later options (Into, Dev ...) belong to the first one */
    }
    break;
  }
  case K_surface: {
    const char *w = hw_(x, 1), *sid = og(x, "id"); if(!sid || !*sid) lerr("Surface: missing Id");
    if(!strcasecmp(w, "new")){ const char *ws = og(x, "w"), *hs = og(x, "h"); FB *f = fb_new(evd(ws ? ws : "64"), evd(hs ? hs : "64"));
      for(int i = 0; i < S.nsurf; i++) if(S.surf[i].id && !strcmp(S.surf[i].id, sid)){ S.surf[i].fb = f; goto surf_done; }
      if(S.nsurf >= 64) lerr("Too many surfaces"); S.surf[S.nsurf].id = xs(sid); S.surf[S.nsurf].fb = f; S.nsurf++; }
    else if(!strcasecmp(w, "free")){ for(int i = 0; i < S.nsurf; i++) if(S.surf[i].id && !strcmp(S.surf[i].id, sid)){ free(S.surf[i].id); S.surf[i].id = NULL; } }
    else lerr("Surface: use New or Free");
    surf_done: break;
  }
  case K_exec: {
    const char *fo = og(x, "file"); char *fe = fm(fo ? fo : ""), *j = pjoin(*S.path ? S.path : S.root_dir, fe), *f = normpath(j); free(j); free(fe);   /* relative to the program folder, like Host: */
    const char *fro = og(x, "from"); char *fr = lowerdup(fro && *fro ? fro : "host"); volatile int code = 0; SL *volatile nodes = NULL;
    { jmp_buf jb, *old = T->jb; T->jb = &jb; if(!setjmp(jb)){ nodes = load(f, fr); T->jb = old; } else { T->jb = old; code = 1; char m[1200]; snprintf(m, sizeof m, "[%s] ERROR %s", f, T->err); out(m); } }
    if(nodes){
      int pid = ++S.pid; Scope *cs = scope_new(NULL); cs->xd = 1; const char *ao = og(x, "args"); char *ar = fm(ao ? ao : ""); sdecl(cs, "args", Vs(ar), -1); free(ar);
      reg_defs(nodes, cs, cs);
      if(og(x, "bg")){ S.tid++; const char *io = og(x, "id"); char *id = io ? fm(io) : xs(""); char dn[300]; snprintf(dn, sizeof dn, "%s#%d", f, S.tid); start_task(*id ? id : dn, nodes, cs, pid, -1); free(id); }
      else {
        char *p0 = S.path; S.path = pdirname(f); jmp_buf jb, *old = T->jb; T->jb = &jb; volatile int st = 0;
        if(!setjmp(jb)){ st = ex(nodes, cs); T->jb = old; } else { T->jb = old; code = 1; char m[1200]; snprintf(m, sizeof m, "[%s] ERROR %s", f, T->err); out(m); }
        S.path = p0; killproc(pid); if(st == ST_STOP) return ST_STOP;
      }
    }
    T->cur = sc; into(x, Vn(code)); break;
  }
  default: lerr("Command not implemented: %s", h->v[0]);
  }
  return ST_OK;
}

/* ---------------------------------------------------------------- arranque */
static int alive_events(void){ if(S.win.on && !S.win.closed) return 1; if(S.nstdin) return 1; if(S.npinh) return 1; for(int i = 0; i < S.ntasks; i++) if(S.tasks[i]->on) return 1; return 0; }
static void halt_all(void){ S.stop = 1; for(int i = 0; i < S.ntasks; i++) S.tasks[i]->stop = 1; }
static void wait_events(void){
  present();
  if(alive_events() && !S.stop){ out(S.win.on ? "Window open: close the browser tab or press Ctrl+C to exit" : "Waiting for events (Ctrl+C to exit)"); while(alive_events() && !S.stop){ present_maybe(); nap(50000); } }
}
/* ---- v2.1: safe outputs + watchdog ----
 * Pin:N("Safe;Low") marks an output as "safe when something goes wrong". Those pins are driven to their safe level when the watchdog
 * expires, when the program ends (normally or with an error), on Ctrl+C / kill, and on a crash (segmentation fault). */
static void apply_safe(int from_signal){
  for(int n = 0; n < 64; n++) if(PST[n].safe){
    int v = PST[n].safe == 2, cur = S.hw ? gpio_rd(n) : S.pins[n];
    if(PST[n].pwm){ PST[n].pwm = 0; pwm_run[n] = 0; }
    S.pins[n] = v; if(S.hw) gpio_wr(n, v);
    if(!from_signal && cur != v){ char t[80]; snprintf(t, sizeof t, "[safe] pin %d = %d", n, v); out(t); }
  }
}
static void pwm_stop_all(void){ for(int n = 0; n < 64; n++){ if(pwm_run[n]) pwm_hw_stop(n); PST[n].pwm = 0; } }
static int crash_inst;
static void crash_h(int sg){ apply_safe(1); signal(sg, SIG_DFL); raise(sg); }
static void install_crash_handlers(void){
  if(crash_inst) return; crash_inst = 1;
  signal(SIGSEGV, crash_h); signal(SIGFPE, crash_h); signal(SIGABRT, crash_h);
#ifdef SIGBUS
  signal(SIGBUS, crash_h);
#endif
}
/* Watchdog("Ms;500") arms it; the program must call Watchdog("Feed") more often than that. If it does not (a hang, an endless loop
 * that forgot to feed), the safe outputs are applied once. Watchdog("Off") disarms; Watchdog("Status","Into;n") = times it fired. */
static struct { volatile int armed, fired; volatile double ms, last; int started; } WD;
static void *wd_main(void *a){
  (void)a;
  for(;;){
    usleep(1000);
    if(WD.armed && (now() - WD.last) * 1000.0 > WD.ms){
      WD.armed = 0; WD.fired++; apply_safe(0);
      char t[100]; snprintf(t, sizeof t, "[WATCHDOG] no Feed in %.0f ms: safe outputs applied", WD.ms); out(t);
    }
  }
  return NULL;
}
static void do_watchdog(Stmt *x){
  const char *k = x->no ? x->o[0].v[0] : "";
  if(!strcasecmp(k, "feed")){ WD.last = now(); }
  else if(!strcasecmp(k, "off")){ WD.armed = 0; }
  else if(!strcasecmp(k, "status")){ into_need(x); into(x, Vn(WD.fired)); }
  else if(!strcasecmp(k, "ms")){
    double ms = evd(x->ov[0]); if(ms < 10 || ms > 600000) lerr("Watchdog: Ms must be between 10 and 600000");
    WD.ms = ms; WD.last = now(); WD.armed = 1; install_crash_handlers();
    if(!WD.started){ WD.started = 1; pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_create(&th, &at, wd_main, NULL); }
  } else lerr("Watchdog: use Ms;n (arm), Feed, Off or Status");
}

static void finish_run(void){
  pwm_stop_all(); apply_safe(0);
  present(); halt_all(); nap(50000);
  if(S.show_end && S.fb){ char *r = fb_render(S.fb, S.ansi); out(r); free(r); }
  if(S.png && S.fb){ fb_png(S.fb, S.png); char m[600]; snprintf(m, sizeof m, "[screen saved to %s]", S.png); out(m); }
}
/* devuelve 0 ok, 1 stop, 2 error */
static int run_nodes(SL *nodes){
  jmp_buf jb; T->jb = &jb; int r = 0;
  reg_defs(nodes, S.root, S.root);
  if(!setjmp(jb)){ if(S.forcewin) win_ensure(); int st = ex(nodes, S.root); if(st == ST_STOP) r = 1; else wait_events(); }
  else r = 2;
  return r;
}
static void sigint(int s){ (void)s; S.gotint = 1; S.stop = 1; }
static int dprint(DV *d, const char *pre, int off){ if(d->n > 1) qsort(d->v, d->n, sizeof(Diag), dcmp); for(int i = 0; i < d->n; i++) printf("%s l.%d: %s\n", pre, d->v[i].n - off, d->v[i].m); return d->n; }
static void exec_src(const char *text, const char *name){
  DV E = {0}, W = {0}; SL *nodes = calloc(1, sizeof(SL));
  if(parse(text, name, &E, &W, nodes)) chk(nodes, &E, &W, &S.chkst);
  dprint(&W, "WARNING", 1); dprint(&E, "ERROR", 1);
  if(E.n) return;
  jmp_buf jb; T->jb = &jb; reg_defs(nodes, S.root, S.root);
  if(!setjmp(jb)){ int st = ex(nodes, S.root); if(st == ST_STOP){ S.stop = 0; puts("(stop)"); } }
  else { printf("ERROR %s\n", T->err); T->depth = 0; free(S.path); S.path = xs(""); }
  fflush(stdout);
}
static void repl(const char *lang){
  printf("Logl %s - REPL <%s>. An empty line runs the block; :quit exits.\n", LOGL_VERSION, lang);
  Buf buf = {0}; int depth = 0, nb = 0; char *last = NULL;
  for(;;){
    printf(nb ? "... " : "%s> ", lang); fflush(stdout);
    char *line = readline_nogil(); if(!line){ puts(""); break; }
    char *t = strip_dup(line);
    if(!strcmp(t, ":quit") || !strcmp(t, ":q") || !strcmp(t, "exit") || !strcmp(t, "quit")){ free(t); free(line); break; }
    int flushn = 0;
    if(nb && depth <= 0 && last && !strcmp(last, "}") && strncasecmp(t, "else", 4) && strncasecmp(t, "elseif", 6) && strncasecmp(t, "catch", 5)) flushn = 1;
    if(flushn || !*t){
      if(nb){ Buf s = {0}; char hd[64]; snprintf(hd, sizeof hd, "<%s>\n", lang); bs(&s, hd); bs(&s, buf.s); char nm[64]; snprintf(nm, sizeof nm, "repl.%s", lang); exec_src(s.s, nm); free(s.s); free(buf.s); buf.s = NULL; buf.n = buf.cap = 0; nb = 0; depth = 0; free(last); last = NULL; }
      if(!*t){ free(t); free(line); continue; }
    }
    bs(&buf, line); bc(&buf, '\n'); nb++; free(last); last = xs(t);
    int tl = (int)strlen(t); if(!strcmp(t, "}")) depth--; else if(tl && t[tl - 1] == '{') depth++;
    if(depth <= 0 && strcmp(t, "}")){
      Buf s = {0}; char hd[64]; snprintf(hd, sizeof hd, "<%s>\n", lang); bs(&s, hd); bs(&s, buf.s); char nm[64]; snprintf(nm, sizeof nm, "repl.%s", lang); exec_src(s.s, nm); free(s.s);
      free(buf.s); buf.s = NULL; buf.n = buf.cap = 0; nb = 0; depth = 0; free(last); last = NULL;
    }
    free(t); free(line);
  }
  if(nb){ Buf s = {0}; char hd[64]; snprintf(hd, sizeof hd, "<%s>\n", lang); bs(&s, hd); bs(&s, buf.s); char nm[64]; snprintf(nm, sizeof nm, "repl.%s", lang); exec_src(s.s, nm); }
  halt_all();
}

static void usage(void){
  puts("Logl " LOGL_VERSION " - Log+/Log-/Log++/LogAll interpreter\n"
       "usage: logl [file] [--png X] [--no-wait] [--check] [--root DIR] [--show] [--ansi] [--plain]\n"
       "             [--strict] [--repl] [--lang log+|log-|log++|logall] [--hw] [--version]\n"
       "             [--window] [--no-window] [--no-browser] [--port N]\n"
       "             [--ram KB] [--allow-raw] [--allow-disk] [--allow-pins] [--allow-net] [--allow-all]\n"
       "       logl pkg list | logl pkg install NAME\n"
       "  Graphics, events and UI programs open a window in your web browser (local only).\n"
       "  --no-window  never open a window (G3D/Screen still render, e.g. with --png)\n"
       "  --no-browser print the window address instead of opening the browser\n"
       "  with no file it opens the interactive REPL.\n"
       "  v2.0: Host file tools, G3D (OBJ, Pick, Orbit, z-buffer), UI TextArea/Dropdown/List, Mem/Math bit tools (see README_v2.md)\n"
       "  --hw  enables REAL hardware access (Reg, Port, Pin). Linux only; may need root.\n"
       "  Permissions (v2.1): files are confined to the program folder (--root) unless --allow-disk;\n"
       "  real Pin access needs --hw AND --allow-pins; --allow-net is reserved for the network layer;\n"
       "  --allow-all = disk + pins + net + raw (it does not imply --hw).\n"
       "  --fake-gpio runs the real-hardware pin code on a GPIO simulated in memory (for tests, no board needed).");
}
int main(int argc, char **argv){
  const char *file = NULL, *root = NULL, *lang = "log+"; int check = 0, strict = 0, repl_ = 0, ansi = -1;
  S.argv0 = argv[0]; if(getenv("LOGL_NO_BROWSER")) S.nobrowser = 1;
  if(argc > 1 && !strcmp(argv[1], "pkg")) return pkg_cmd(argc, argv);
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  { HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE); DWORD md;
    if(GetConsoleMode(ho, &md)) SetConsoleMode(ho, md | 0x0004); }   /* colores ANSI */
#endif
  for(int i = 1; i < argc; i++){
    const char *a = argv[i];
    if(!strcmp(a, "--png") && i + 1 < argc) S.png = argv[++i];
    else if(!strcmp(a, "--no-wait")) S.nowait = 1;
    else if(!strcmp(a, "--check")) check = 1;
    else if(!strcmp(a, "--root") && i + 1 < argc) root = argv[++i];
    else if(!strcmp(a, "--show")) S.show_end = 1;
    else if(!strcmp(a, "--ansi")) ansi = 1; else if(!strcmp(a, "--plain")) ansi = 0;
    else if(!strcmp(a, "--strict")) strict = 1; else if(!strcmp(a, "--repl")) repl_ = 1;
    else if(!strcmp(a, "--lang") && i + 1 < argc) lang = argv[++i];
    else if(!strcmp(a, "--hw")) S.hw = 1;
    else if(!strcmp(a, "--allow-raw")) S.allowraw = 1;
    else if(!strcmp(a, "--allow-disk")) S.allow_disk = 1;
    else if(!strcmp(a, "--allow-pins")) S.allow_pins = 1;
    else if(!strcmp(a, "--allow-net")) S.allow_net = 1;   /* reserved: used by Net:/Http: (part 1C) */
    else if(!strcmp(a, "--fake-gpio")){ S.hw = 1; S.fakegpio = 1; }   /* real-hardware code path on a GPIO simulated in RAM: for tests */
    else if(!strcmp(a, "--allow-all")) S.allow_disk = S.allow_pins = S.allow_net = S.allowraw = 1;
    else if(!strcmp(a, "--ram") && i + 1 < argc){ long kb = atol(argv[++i]); if(kb < 1) kb = 1; if(kb > 1048576) kb = 1048576; S.memsz = (size_t)kb * 1024; }
    else if(!strcmp(a, "--window")) S.forcewin = 1;
    else if(!strcmp(a, "--no-window")) S.nowin = 1;
    else if(!strcmp(a, "--no-browser")) S.nobrowser = 1;
    else if(!strcmp(a, "--port") && i + 1 < argc) S.port = atoi(argv[++i]);
    else if(!strcmp(a, "-v") || !strcmp(a, "--version")){ puts("logl " LOGL_VERSION); return 0; }
    else if(!strcmp(a, "-h") || !strcmp(a, "--help")){ usage(); return 0; }
    else if(a[0] == '-'){ fprintf(stderr, "unknown option: %s\n", a); return 2; }
    else file = a;
  }
#ifdef _WIN32
  if(file) fixslash((char *)file);
  if(root) fixslash((char *)root);
  if(S.png) fixslash(S.png);
#endif
  Th th0; memset(&th0, 0, sizeof th0); th0.retv = Vu(); T = &th0; jmp_buf jb0; th0.jb = &jb0;
  pthread_mutex_lock(&GIL); signal(SIGINT, sigint);
#ifdef SIGTERM
  signal(SIGTERM, sigint);   /* v2.1: kill / systemd stop also end the program cleanly, so safe outputs are applied */
#endif
#ifdef SIGHUP
  signal(SIGHUP, sigint);
#endif
#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);
#endif
  char cwd[4096]; if(!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
#ifdef _WIN32
  fixslash(cwd);
#endif
  if(root) S.root_dir = xs(root);
  else if(file){ char *d = pdirname(file); if(!*d){ free(d); d = xs("."); } S.root_dir = d; } else S.root_dir = xs(cwd);
  S.path = xs(""); S.ansi = ansi >= 0 ? ansi : (isatty(1) && !getenv("NO_COLOR")); S.showfirst = 1; S.t0 = now();
  rngs ^= (ull)(now() * 1e9) * 2654435761ULL + (ull)time(NULL) + (ull)getpid(); if(!rngs) rngs = 1;
  if(!S.memsz) S.memsz = 65536;
  S.MEM = calloc(S.memsz, 1); if(!S.MEM){ fputs("out of memory\n", stderr); return 3; }
  math_init(); S.queues = Vd(); S.locks = Vd(); S.root = scope_new(NULL); dset(S.root->v, "keys", Vd());
  sdecl(S.root, "mb", Vn(0), -1); sdecl(S.root, "mx", Vn(0), -1); sdecl(S.root, "my", Vn(0), -1); sdecl(S.root, "key", Vs(""), -1);
  S.vdpath = pjoin(S.root_dir, "logdisk.json"); S.stpath = pjoin(S.root_dir, "logdata.json"); S.vd = Vd();
  { char *tx = readfile(S.vdpath, NULL); if(tx){ int err; Val v = jparse(tx, &err); if(!err && v.t == T_DICT){ vdrop(S.vd); S.vd = v; } free(tx); } }
  if(repl_ || !file){ repl(lang); return 0; }
  size_t n; struct stat fst; int isdir = !stat(file, &fst) && S_ISDIR(fst.st_mode);
  char *src = isdir ? NULL : readfile(file, &n);
  if(!src){
    int en = errno;
    if(isdir) fprintf(stderr, "could not open '%s': it is a folder, not a file\n", file);
    else if(en == ENOENT){
      fprintf(stderr, "could not open '%s': file not found (looked in: %s)\n", file, getcwd(cwd, sizeof cwd) ? cwd : ".");
      fputs("tip: on Windows, make sure the file is not really named 'name.log+.txt' (enable View > File name extensions in Explorer)\n", stderr);
    } else fprintf(stderr, "could not open '%s': %s\n", file, strerror(en));
    return 2;
  }
  DV E = {0}, W = {0}; SL *nodes = calloc(1, sizeof(SL)); const char *bn = strrchr(file, '/'); bn = bn ? bn + 1 : file;
  ChkSt cs = {0};
  if(parse(src, bn, &E, &W, nodes)) chk(nodes, &E, &W, &cs);
  if(strict){ for(int i = 0; i < W.n; i++) dpush(&E, W.v[i].n, W.v[i].m); W.n = 0; }
  dprint(&W, "WARNING", 0); dprint(&E, "ERROR", 0);
  if(E.n){ printf("Compilation aborted: %d error(s)\n", E.n); return 1; }
  printf("Compiled with no errors (%d warning(s)).\n", W.n); fflush(stdout);
  if(check) return 0;
  S.autowin = !S.nowin && !S.png && !S.show_end && !has_show(nodes);
  int r = run_nodes(nodes);
  finish_run();
  if(r == 1 || S.gotint){ puts(S.gotint ? "\nSTOPPED" : "STOPPED"); return 0; }
  if(r == 2){
    const char *e = T->err; if(e[0] == 'l' && e[1] == '.'){ const char *q = e + 2; while(isdigit((unsigned char)*q)) q++; if(q > e + 2 && q[0] == ':'){ printf("ERROR %s\n", e); return 2; } }
    printf("ERROR %s\n", e); return 2;
  }
  puts("FINISHED"); return 0;
}
