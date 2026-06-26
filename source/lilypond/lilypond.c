// A Max UI external that engraves GNU LilyPond music notation.
//
// LilyPond has no embeddable C API: it is a standalone application (Scheme on
// Guile, rasterizing through Cairo). This external shells out to an installed
// `lilypond` binary as a subprocess, has it write a cropped SVG, and draws
// it via jgraphics_draw_jsvg for crisp, retina-friendly vector output.
//
// We do not bundle LilyPond (GPL-3 redistribution, notarizing third-party
// binaries, version-pin maintenance). Instead the binary is discovered at
// render time: an explicit @lilypondpath override wins, otherwise we probe the
// common install locations and finally PATH. When nothing is found we post an
// actionable install hint.
//
// The subprocess + file I/O run on a background thread (systhread); SVG
// creation, box resizing, and redraw happen only on the main thread, marshaled
// back via a qelem. A per-request generation counter drops stale results so
// rapid edits never paint an out-of-date image.
//
// macOS and Windows are both supported. The OS-specific process and filesystem
// calls (spawn/wait/kill, executable probing, temp dir) are isolated behind a
// small platform seam (see the lp_* helpers) so the rest of the code is shared.
//
// The .ly source is held as an object-owned string. It is set via the `set`
// message or loaded from disk with `read`, and persisted in the patcher via
// the dictionary.

#include "ext.h"
#include "ext_obex.h"
#include "jpatcher_api.h"
#include "jgraphics.h"

#if defined(MAC_VERSION) || defined(WIN_VERSION)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIN_VERSION
#include <windows.h>
#include <io.h>
// POSIX names the CRT exposes only under an underscore prefix.
#define strdup _strdup
#define unlink _unlink
#else // MAC_VERSION
#include <unistd.h>
#include <spawn.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
extern char** environ;
#endif

// Default LilyPond source rendered before the user supplies any.
#define LILYPOND_DEFAULT_SOURCE "{ c' d' e' f' g' }"

// --- Platform seam -------------------------------------------------------
// Everything below isolates the OS-specific process and filesystem calls so
// the render worker stays platform-neutral. macOS uses posix_spawn/waitpid;
// Windows uses CreateProcess/WaitForSingleObject.

#ifdef WIN_VERSION
#define LP_PATH_SEP ';'
#define LP_DIR_SEP "\\"
#define LP_EXE_NAME "lilypond.exe"
#else
#define LP_PATH_SEP ':'
#define LP_DIR_SEP "/"
#define LP_EXE_NAME "lilypond"
#endif

// A running child process. `active` disambiguates a live handle/pid from a
// zeroed one and is the mutex-guarded signal that free() may still kill it.
typedef struct _lp_child {
#ifdef WIN_VERSION
    HANDLE handle;
#else
    pid_t pid;
#endif
    char active;
} lp_child;

// Is `path` an executable file we could launch?
static int lp_is_executable(const char* path)
{
#ifdef WIN_VERSION
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    return access(path, X_OK) == 0;
#endif
}

// The per-process temp directory.
static const char* lp_tmpdir(void)
{
#ifdef WIN_VERSION
    const char* t = getenv("TEMP");
    if (!t || !t[0]) {
        t = getenv("TMP");
    }
    if (!t || !t[0]) {
        t = ".";
    }
    return t;
#else
    const char* t = getenv("TMPDIR");
    if (!t || !t[0]) {
        t = "/tmp";
    }
    return t;
#endif
}

// Launch lilypond, redirecting its stdout+stderr to `logpath`. Fills `*child`
// and returns 0 on success, nonzero if the binary could not be launched.
static int lp_spawn(const char* binpath, const char* base, const char* input,
                    const char* logpath, lp_child* child)
{
#ifdef WIN_VERSION
    // CreateProcess takes one mutable command line, not an argv array. Both the
    // binary and the temp paths can contain spaces, so quote each token.
    char cmdline[4096];
    snprintf(cmdline, sizeof(cmdline),
             "\"%s\" -dbackend=svg -dcrop -dno-point-and-click -o \"%s\" \"%s\"",
             binpath, base, input);

    // The child must inherit the log handle for the redirect to take effect.
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE hlog = CreateFileA(logpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hlog == INVALID_HANDLE_VALUE) {
        hlog = NULL; // run without the redirect rather than fail outright
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (hlog) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hlog;
        si.hStdError = hlog;
    }

    BOOL ok = CreateProcessA(binpath, cmdline, NULL, NULL, hlog ? TRUE : FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (hlog) {
        CloseHandle(hlog);
    }
    if (!ok) {
        return 1;
    }

    CloseHandle(pi.hThread); // we never use the primary-thread handle
    child->handle = pi.hProcess;
    child->active = 1;
    return 0;
#else
    char* spawn_argv[] = {
        (char*)binpath, "-dbackend=svg", "-dcrop", "-dno-point-and-click",
        "-o", (char*)base, (char*)input, NULL
    };

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // Capture stdout+stderr into the log file so it never blocks on a pipe.
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);

    pid_t pid = 0;
    int rc = posix_spawn(&pid, binpath, &fa, NULL, spawn_argv, environ);
    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) {
        return rc;
    }
    child->pid = pid;
    child->active = 1;
    return 0;
#endif
}

// Block until the child exits. Leaves the handle open (on Windows) so a racing
// free() can still signal it; lp_close reclaims it afterward.
static void lp_wait(lp_child* child)
{
#ifdef WIN_VERSION
    if (child->handle) {
        WaitForSingleObject(child->handle, INFINITE);
    }
#else
    int status = 0;
    waitpid(child->pid, &status, 0);
#endif
}

// Force-terminate the child. Signal only — never closes/reaps, mirroring the
// macOS kill(): the worker owns reaping so there is exactly one close path.
static void lp_kill(lp_child* child)
{
#ifdef WIN_VERSION
    if (child->handle) {
        TerminateProcess(child->handle, 1);
    }
#else
    kill(child->pid, SIGKILL);
#endif
}

// Release the OS handle for an already-waited child (no-op on macOS).
static void lp_close(lp_child* child)
{
#ifdef WIN_VERSION
    if (child->handle) {
        CloseHandle(child->handle);
        child->handle = NULL;
    }
#else
    (void)child;
#endif
}

typedef struct _lilypond
{
    t_jbox b_jbox;

    // Attributes (main-thread owned)
    double size; // pixels per staff-space unit (from jsvg_get_size)
    double margin; // padding around the rendered image, in pixels
    t_symbol* lilypondpath; // binary override; empty => auto-find
    char autofind; // probe common locations when lilypondpath is empty

    // Source text (main-thread owned heap string)
    char* source;

    // Rendered SVG (main-thread owned)
    t_jsvg* jsvg_result;
    double jsvg_w, jsvg_h; // natural size from jsvg_get_size

    // Async rendering machinery
    t_systhread render_thread; // started + joined on the main thread only
    t_qelem* done_qelem; // marshals worker completion to main thread
    t_systhread_mutex mutex; // guards the fields below
    long generation; // bumped per request; newest wins
    char busy; // a worker is currently running (main only)
    char pending; // a request arrived mid-render (main only)
    char quit; // set in free so the worker bails

    // Worker -> main handoff (guarded by mutex)
    long result_generation;
    char* result_svg; // SVG XML bytes (malloc'd by worker)
    long result_svg_len;
    char* result_err; // captured stderr on failure (malloc'd)
    char result_binpath[MAX_PATH_CHARS]; // auto-discovered path, empty if override was used
    lp_child child; // running subprocess, for kill-on-free (guarded by mutex)
} t_lilypond;

void* lilypond_new(t_symbol* s, long argc, t_atom* argv);
void lilypond_free(t_lilypond* x);
void lilypond_assist(t_lilypond* x, void* b, long m, long a, char* s);
void lilypond_paint(t_lilypond* x, t_object* patcherview);
t_atom_long lilypond_oksize(t_lilypond* x, t_rect* r);

void lilypond_set(t_lilypond* x, t_symbol* s, long argc, t_atom* argv);
void lilypond_read(t_lilypond* x, t_symbol* s);
void lilypond_getpath(t_lilypond* x);
void lilypond_jsave(t_lilypond* x, t_dictionary* d);

t_max_err lilypond_path_set(t_lilypond* x, void* attr, long argc, t_atom* argv);
t_max_err lilypond_autofind_set(t_lilypond* x, void* attr, long argc, t_atom* argv);
t_max_err lilypond_size_set(t_lilypond* x, void* attr, long argc, t_atom* argv);
t_max_err lilypond_margin_set(t_lilypond* x, void* attr, long argc, t_atom* argv);

void lilypond_set_source(t_lilypond* x, const char* text);
void lilypond_schedule_render(t_lilypond* x);
void lilypond_start_worker(t_lilypond* x);
void lilypond_render_worker(t_lilypond* x);
void lilypond_render_done(t_lilypond* x);

static t_class* s_lilypond_class = NULL;
static t_symbol* ps_lilypond = NULL;

// Message posted when no LilyPond binary can be found.
#ifdef WIN_VERSION
#define LILYPOND_NOTFOUND_MSG                                              \
    "lilypond: no LilyPond binary found. Install it from "                 \
    "https://lilypond.org/download.html, then set the @lilypondpath "      \
    "attribute to lilypond.exe."
#else
#define LILYPOND_NOTFOUND_MSG                                               \
    "lilypond: no LilyPond binary found. Install it (e.g. \"brew install "  \
    "lilypond\" or from https://lilypond.org/download.html), then set the " \
    "@lilypondpath attribute."
#endif

// Common absolute install locations, probed in order before falling back to
// PATH.
#ifdef WIN_VERSION
// Windows LilyPond now ships as a portable zip with no installer, registry
// key, or fixed path, so there is no reliable location to probe — @lilypondpath
// and the PATH scan below are the dependable mechanisms. These two are
// best-effort guesses at where a user might have unpacked it.
static const char* const k_lilypond_candidates[] = {
    "C:\\Program Files\\LilyPond\\bin\\lilypond.exe",
    "C:\\Program Files (x86)\\LilyPond\\usr\\bin\\lilypond.exe",
    NULL
};
#else
// Homebrew (arm64, then Intel), the official relocatable app, MacPorts.
static const char* const k_lilypond_candidates[] = {
    "/opt/homebrew/bin/lilypond",
    "/usr/local/bin/lilypond",
    "/Applications/LilyPond.app/Contents/Resources/bin/lilypond",
    "/opt/local/bin/lilypond",
    NULL
};
#endif

// Discover an installed lilypond binary. Returns 1 and fills `out`
// (MAX_PATH_CHARS) on success, 0 if none found. Touches only the filesystem
// (no Max API), so it is safe to call from the render worker.
static int lilypond_discover_default(char* out)
{
    for (int i = 0; k_lilypond_candidates[i]; i++) {
        if (lp_is_executable(k_lilypond_candidates[i])) {
            strncpy(out, k_lilypond_candidates[i], MAX_PATH_CHARS - 1);
            out[MAX_PATH_CHARS - 1] = '\0';
            return 1;
        }
    }

    // Scan PATH last. Rarely useful: Max is a GUI app and usually doesn't
    // inherit the shell PATH, which is why the explicit locations come first.
    const char* path = getenv("PATH");
    for (const char* p = path; p && *p;) {
        const char* sep = strchr(p, LP_PATH_SEP);
        size_t dirlen = sep ? (size_t)(sep - p) : strlen(p);
        if (dirlen > 0 && dirlen + strlen(LP_DIR_SEP LP_EXE_NAME) < MAX_PATH_CHARS) {
            char cand[MAX_PATH_CHARS];
            memcpy(cand, p, dirlen);
            snprintf(cand + dirlen, sizeof(cand) - dirlen, LP_DIR_SEP LP_EXE_NAME);
            if (lp_is_executable(cand)) {
                strncpy(out, cand, MAX_PATH_CHARS - 1);
                out[MAX_PATH_CHARS - 1] = '\0';
                return 1;
            }
        }
        if (!sep) {
            break;
        }
        p = sep + 1;
    }

    out[0] = '\0';
    return 0;
}

// Resolve which lilypond binary to run. An explicit @lilypondpath override is
// authoritative — used verbatim so any launch error names the user's own path
// — otherwise fall back to discovery. Returns 1 if a path was produced.
static int lilypond_resolve_binary(const char* override, char* out)
{
    if (override && override[0]) {
        strncpy(out, override, MAX_PATH_CHARS - 1);
        out[MAX_PATH_CHARS - 1] = '\0';
        return 1;
    }
    return lilypond_discover_default(out);
}

void ext_main(void* r)
{
    ps_lilypond = gensym("lilypond");

    t_class* c = class_new("lilypond", (method)lilypond_new, (method)lilypond_free, sizeof(t_lilypond), NULL, A_GIMME, 0);

    c->c_flags |= CLASS_FLAG_NEWDICTIONARY;
    jbox_initclass(c, JBOX_COLOR);

    class_addmethod(c, (method)lilypond_paint, "paint", A_CANT, 0);
    class_addmethod(c, (method)lilypond_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)lilypond_oksize, "oksize", A_CANT, 0);
    class_addmethod(c, (method)lilypond_jsave, "jsave", A_CANT, 0);

    class_addmethod(c, (method)lilypond_set, "set", A_GIMME, 0);
    class_addmethod(c, (method)lilypond_read, "read", A_DEFSYM, 0);
    class_addmethod(c, (method)lilypond_getpath, "getpath", 0);

    CLASS_ATTR_DOUBLE(c, "size", 0, t_lilypond, size);
    CLASS_ATTR_LABEL(c, "size", 0, "Pixels Per Staff Space");
    CLASS_ATTR_ACCESSORS(c, "size", NULL, lilypond_size_set);
    CLASS_ATTR_FILTER_MIN(c, "size", 0.5);
    CLASS_ATTR_SAVE(c, "size", 0);

    CLASS_ATTR_DOUBLE(c, "margin", 0, t_lilypond, margin);
    CLASS_ATTR_LABEL(c, "margin", 0, "Margin (px)");
    CLASS_ATTR_ACCESSORS(c, "margin", NULL, lilypond_margin_set);
    CLASS_ATTR_FILTER_MIN(c, "margin", 0.0);
    CLASS_ATTR_SAVE(c, "margin", 0);

    CLASS_ATTR_SYM(c, "lilypondpath", 0, t_lilypond, lilypondpath);
    CLASS_ATTR_LABEL(c, "lilypondpath", 0, "LilyPond Binary Path");
    CLASS_ATTR_ACCESSORS(c, "lilypondpath", NULL, lilypond_path_set);
    CLASS_ATTR_SAVE(c, "lilypondpath", 0);

    CLASS_ATTR_CHAR(c, "autofind", 0, t_lilypond, autofind);
    CLASS_ATTR_LABEL(c, "autofind", 0, "Auto-find LilyPond Binary");
    CLASS_ATTR_STYLE(c, "autofind", 0, "onoff");
    CLASS_ATTR_ACCESSORS(c, "autofind", NULL, lilypond_autofind_set);
    CLASS_ATTR_SAVE(c, "autofind", 0);

    class_register(CLASS_BOX, c);
    s_lilypond_class = c;
}

void* lilypond_new(t_symbol* s, long argc, t_atom* argv)
{
    t_lilypond* x = NULL;
    t_dictionary* d = NULL;

    if (!(d = object_dictionaryarg(argc, argv))) {
        return NULL;
    }

    if (!(x = (t_lilypond*)object_alloc(s_lilypond_class))) {
        return NULL;
    }

    x->size = 8.0;
    x->margin = 8.0;
    x->lilypondpath = gensym("");
    x->autofind = 1;
    x->source = NULL;
    x->jsvg_result = NULL;
    x->jsvg_w = x->jsvg_h = 0.0;
    x->render_thread = NULL;
    x->done_qelem = NULL;
    x->mutex = NULL;
    x->generation = 0;
    x->busy = 0;
    x->pending = 0;
    x->quit = 0;
    x->result_generation = 0;
    x->result_svg = NULL;
    x->result_svg_len = 0;
    x->result_err = NULL;
    x->result_binpath[0] = '\0';
    memset(&x->child, 0, sizeof(x->child));

    systhread_mutex_new(&x->mutex, 0);
    x->done_qelem = qelem_new((t_object*)x, (method)lilypond_render_done);

    long flags = JBOX_DRAWFIRSTIN
                 | JBOX_NODRAWBOX
                 | JBOX_DRAWINLAST
                 | JBOX_TRANSPARENT
                 | JBOX_NOGROW;
    jbox_new(&x->b_jbox, flags, argc, argv);
    x->b_jbox.b_firstin = (t_object*)x;

    // Restore embedded source written by lilypond_jsave, else use the default.
    const char* saved = NULL;
    if (dictionary_getstring(d, gensym("ly_source"), &saved) == MAX_ERR_NONE && saved && saved[0]) {
        x->source = strdup(saved);
    }
    else {
        x->source = strdup(LILYPOND_DEFAULT_SOURCE);
    }

    attr_dictionary_process(x, d);

    jbox_ready(&x->b_jbox);

    lilypond_schedule_render(x);
    return x;
}

void lilypond_free(t_lilypond* x)
{
    // Stop any in-flight render: signal quit, kill the child so the wait returns
    // promptly, then join the worker before tearing anything down. We only
    // signal here; the worker still owns reaping and closing the child handle.
    if (x->mutex) {
        systhread_mutex_lock(x->mutex);
        x->quit = 1;
        if (x->child.active) {
            lp_kill(&x->child);
        }
        systhread_mutex_unlock(x->mutex);
    }
    if (x->render_thread) {
        unsigned int ret = 0;
        systhread_join(x->render_thread, &ret);
        x->render_thread = NULL;
    }
    if (x->done_qelem) {
        qelem_free(x->done_qelem);
    }

    if (x->jsvg_result) {
        jsvg_destroy(x->jsvg_result);
    }

    if (x->source) {
        free(x->source);
    }
    if (x->result_svg) {
        free(x->result_svg);
    }
    if (x->result_err) {
        free(x->result_err);
    }

    if (x->mutex) {
        systhread_mutex_free(x->mutex);
    }

    jbox_free(&x->b_jbox);
}

void lilypond_assist(t_lilypond* x, void* b, long m, long a, char* s)
{
    if (m == ASSIST_INLET) {
        snprintf(s, 256, "set/read LilyPond source");
    }
}

void lilypond_paint(t_lilypond* x, t_object* patcherview)
{
    t_rect r;
    jbox_get_rect_for_view((t_object*)x, patcherview, &r);

    t_jgraphics* g = jbox_start_layer((t_object*)x, patcherview, ps_lilypond, r.width, r.height);
    if (g) {
        // Paint white background
        t_jrgba bgcolor = { 1.0, 1.0, 1.0, 1.0 };
        jgraphics_set_source_jrgba(g, &bgcolor);
        jgraphics_rectangle(g, 0, 0, r.width, r.height);
        jgraphics_fill(g);

        if (x->jsvg_result) {
            t_rect dest = { x->margin, x->margin,
                            r.width - 2.0 * x->margin,
                            r.height - 2.0 * x->margin };
            jgraphics_draw_jsvg(g, x->jsvg_result, &dest, JGRAPHICS_JSVG_USE_CONTENT_AREA, 1.0);
        }

        jbox_end_layer((t_object*)x, patcherview, ps_lilypond);
    }
    jbox_paint_layer((t_object*)x, patcherview, ps_lilypond, 0.0, 0.0);
}

t_atom_long lilypond_oksize(t_lilypond* x, t_rect* r)
{
    if (r->width < 16.0) {
        r->width = 16.0;
    }
    if (r->height < 16.0) {
        r->height = 16.0;
    }
    return 0;
}

void lilypond_set_source(t_lilypond* x, const char* text)
{
    if (x->source) {
        free(x->source);
    }
    x->source = strdup(text ? text : "");
    lilypond_schedule_render(x);
}

void lilypond_set(t_lilypond* x, t_symbol* s, long argc, t_atom* argv)
{
    char buf[4096];
    size_t len = 0;
    buf[0] = '\0';

#define LILYPOND_APPEND(...)                                             \
    do {                                                                 \
        if (len < sizeof(buf) - 1) {                                     \
            int w = snprintf(buf + len, sizeof(buf) - len, __VA_ARGS__); \
            if (w > 0)                                                   \
                len = (len + (size_t)w < sizeof(buf)) ? len + (size_t)w  \
                                                      : sizeof(buf) - 1; \
        }                                                                \
    } while (0)

    for (long i = 0; i < argc; i++) {
        const char* sep = (len > 0) ? " " : "";
        switch (atom_gettype(argv + i)) {
            case A_LONG: LILYPOND_APPEND("%s%lld", sep, (long long)atom_getlong(argv + i)); break;
            case A_FLOAT: LILYPOND_APPEND("%s%g", sep, atom_getfloat(argv + i)); break;
            case A_SYM: LILYPOND_APPEND("%s%s", sep, atom_getsym(argv + i)->s_name); break;
            default: break;
        }
    }

#undef LILYPOND_APPEND

    lilypond_set_source(x, buf);
}

void lilypond_read(t_lilypond* x, t_symbol* s)
{
    if (!s || !s->s_name[0]) {
        object_error((t_object*)x, "read: a filename argument is required");
        return;
    }

    char filename[MAX_PATH_CHARS];
    short path = 0;
    t_fourcc type = 0;
    strncpy(filename, s->s_name, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';

    // locatefile_extended modifies `filename` (alias/full-path handling) and
    // returns nonzero when the file cannot be found on the search path.
    if (locatefile_extended(filename, &path, &type, NULL, 0)) {
        object_error((t_object*)x, "read: can't find file %s", s->s_name);
        return;
    }

    char native[MAX_PATH_CHARS];
    if (path_toabsolutesystempath(path, filename, native) != MAX_ERR_NONE) {
        object_error((t_object*)x, "read: can't resolve path for %s", s->s_name);
        return;
    }

    FILE* f = fopen(native, "rb");
    if (!f) {
        object_error((t_object*)x, "read: can't open %s", native);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return;
    }
    char* text = (char*)malloc((size_t)n + 1);
    if (!text) {
        fclose(f);
        return;
    }
    size_t got = fread(text, 1, (size_t)n, f);
    text[got] = '\0';
    fclose(f);

    lilypond_set_source(x, text);
    free(text);
}

// Report the binary that would be used for the next render, for diagnostics.
void lilypond_getpath(t_lilypond* x)
{
    const char* override = (x->lilypondpath && x->lilypondpath->s_name[0])
                               ? x->lilypondpath->s_name
                               : "";
    char binpath[MAX_PATH_CHARS];
    if (lilypond_resolve_binary(override, binpath)) {
        object_post((t_object*)x, "lilypond: using %s%s", binpath,
                    override[0] ? " (from @lilypondpath)" : "");
    }
    else {
        object_warn((t_object*)x, "%s", LILYPOND_NOTFOUND_MSG);
    }
}

void lilypond_jsave(t_lilypond* x, t_dictionary* d)
{
    if (x->source) {
        dictionary_appendstring(d, gensym("ly_source"), x->source);
    }
}

t_max_err lilypond_size_set(t_lilypond* x, void* attr, long argc, t_atom* argv)
{
    if (argc) {
        x->size = atom_getfloat(argv);
    }
    if (x->jsvg_result && x->jsvg_w > 0.0 && x->jsvg_h > 0.0) {
        t_rect r;
        jbox_get_patching_rect((t_object*)x, &r);
        r.width = x->jsvg_w * x->size + 2.0 * x->margin;
        r.height = x->jsvg_h * x->size + 2.0 * x->margin;
        jbox_set_patching_rect((t_object*)x, &r);
        jbox_invalidate_layer((t_object*)x, NULL, ps_lilypond);
        jbox_redraw(&x->b_jbox);
    }
    return MAX_ERR_NONE;
}

t_max_err lilypond_path_set(t_lilypond* x, void* attr, long argc, t_atom* argv)
{
    if (argc) {
        x->lilypondpath = atom_getsym(argv);
    }
    lilypond_schedule_render(x);
    return MAX_ERR_NONE;
}

t_max_err lilypond_autofind_set(t_lilypond* x, void* attr, long argc, t_atom* argv)
{
    if (argc) {
        x->autofind = (char)atom_getlong(argv);
    }
    lilypond_schedule_render(x);
    return MAX_ERR_NONE;
}

t_max_err lilypond_margin_set(t_lilypond* x, void* attr, long argc, t_atom* argv)
{
    if (argc) {
        x->margin = atom_getfloat(argv);
    }
    if (x->jsvg_result && x->jsvg_w > 0.0 && x->jsvg_h > 0.0) {
        t_rect r;
        jbox_get_patching_rect((t_object*)x, &r);
        r.width = x->jsvg_w * x->size + 2.0 * x->margin;
        r.height = x->jsvg_h * x->size + 2.0 * x->margin;
        jbox_set_patching_rect((t_object*)x, &r);
        jbox_invalidate_layer((t_object*)x, NULL, ps_lilypond);
        jbox_redraw(&x->b_jbox);
    }
    return MAX_ERR_NONE;
}

void lilypond_schedule_render(t_lilypond* x)
{
    systhread_mutex_lock(x->mutex);
    x->generation++;
    char already_busy = x->busy;
    if (already_busy) {
        x->pending = 1;
    }
    else {
        x->busy = 1;
    }
    systhread_mutex_unlock(x->mutex);

    if (!already_busy) {
        lilypond_start_worker(x);
    }
}

void lilypond_start_worker(t_lilypond* x)
{
    if (systhread_create((method)lilypond_render_worker, x, 0, 0, 0, &x->render_thread)) {
        // Failed to spawn; clear busy so a later request can retry.
        systhread_mutex_lock(x->mutex);
        x->busy = 0;
        systhread_mutex_unlock(x->mutex);
    }
}

// Read an entire file into a freshly malloc'd, NUL-terminated buffer.
// Returns NULL (and sets *out_len to 0) if the file is missing or empty.
static char* lilypond_slurp(const char* path, long* out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) {
        *out_len = (long)got;
    }
    return buf;
}

void lilypond_render_worker(t_lilypond* x)
{
    // snapshot the request under the lock
    systhread_mutex_lock(x->mutex);
    long my_gen = x->generation;
    char* src = strdup(x->source ? x->source : "");
    char overridepath[MAX_PATH_CHARS];
    if (x->lilypondpath && x->lilypondpath->s_name[0]) {
        strncpy(overridepath, x->lilypondpath->s_name, sizeof(overridepath) - 1);
        overridepath[sizeof(overridepath) - 1] = '\0';
    }
    else {
        overridepath[0] = '\0';
    }
    char autofind_local = x->autofind;
    char early_quit = x->quit;
    systhread_mutex_unlock(x->mutex);

    if (early_quit) {
        if (src) {
            free(src);
        }
        systhread_exit(0);
        return;
    }

    // Resolve the binary off-lock (discovery only touches the filesystem).
    char binpath[MAX_PATH_CHARS];
    int have_bin;
    if (overridepath[0]) {
        strncpy(binpath, overridepath, MAX_PATH_CHARS - 1);
        binpath[MAX_PATH_CHARS - 1] = '\0';
        have_bin = 1;
    }
    else if (autofind_local) {
        have_bin = lilypond_discover_default(binpath);
    }
    else {
        binpath[0] = '\0';
        have_bin = 0;
    }

    // build the temp file paths (instance + generation unique)
    const char* tmp = lp_tmpdir();

    char base[MAX_PATH_CHARS], input[MAX_PATH_CHARS];
    char svgcropped[MAX_PATH_CHARS], svgfull[MAX_PATH_CHARS], errpath[MAX_PATH_CHARS];
    snprintf(base, sizeof(base), "%s/lilypond_%p_%ld", tmp, (void*)x, my_gen);
    snprintf(input, sizeof(input), "%s.ly", base);
    snprintf(svgcropped, sizeof(svgcropped), "%s.cropped.svg", base);
    snprintf(svgfull, sizeof(svgfull), "%s.svg", base);
    snprintf(errpath, sizeof(errpath), "%s.log", base);

    char* svg_bytes = NULL;
    long svg_len = 0;
    char* errstr = NULL;

    // write the wrapped .ly source
    FILE* lyf = NULL;
    if (!have_bin) {
        errstr = strdup(LILYPOND_NOTFOUND_MSG);
    }
    else if (!(lyf = fopen(input, "wb"))) {
        errstr = strdup("lilypond: could not write temporary .ly file");
    }
    else {
        fprintf(lyf, "#(set-global-staff-size 18)\n");
        fprintf(lyf,
                "\\paper {\n"
                "  indent = 0\\mm\n"
                "  oddHeaderMarkup = ##f\n"
                "  evenHeaderMarkup = ##f\n"
                "  oddFooterMarkup = ##f\n"
                "  evenFooterMarkup = ##f\n"
                "}\n");
        fwrite(src, 1, strlen(src), lyf);
        fputc('\n', lyf);
        fclose(lyf);

        // spawn the lilypond subprocess (stdout+stderr -> errpath)
        lp_child child = { 0 };
        int spawn_rc = lp_spawn(binpath, base, input, errpath, &child);

        if (spawn_rc != 0) {
            char msg[MAX_PATH_CHARS + 64];
            snprintf(msg, sizeof(msg), "lilypond: could not launch binary at %s", binpath);
            errstr = strdup(msg);
        }
        else {
            // Register the child so free() can kill it; bail if quit raced in.
            // Ownership of the handle stays with this worker either way.
            char killed = 0;
            systhread_mutex_lock(x->mutex);
            if (x->quit) {
                lp_kill(&child);
                killed = 1;
            }
            else {
                x->child = child;
            }
            systhread_mutex_unlock(x->mutex);

            lp_wait(&child);

            // Mark inactive before closing the handle so a racing free() (which
            // only acts while active) can no longer touch it.
            systhread_mutex_lock(x->mutex);
            x->child.active = 0;
            systhread_mutex_unlock(x->mutex);
            lp_close(&child);

            if (!killed) {
                svg_bytes = lilypond_slurp(svgcropped, &svg_len);
                if (!svg_bytes) {
                    // Fall back to the full page if -dcrop didn't produce a cropped file.
                    svg_bytes = lilypond_slurp(svgfull, &svg_len);
                }
                if (!svg_bytes) {
                    errstr = lilypond_slurp(errpath, NULL);
                    if (!errstr) {
                        errstr = strdup("lilypond: rendering produced no output");
                    }
                }
            }
        }
    }

    // tidy all temp files (-dcrop produces both .svg and .cropped.svg)
    unlink(input);
    unlink(svgcropped);
    unlink(svgfull);
    unlink(errpath);

    // hand the result to the main thread
    systhread_mutex_lock(x->mutex);
    if (x->result_svg) {
        free(x->result_svg);
    }
    if (x->result_err) {
        free(x->result_err);
    }
    x->result_svg = svg_bytes;
    x->result_svg_len = svg_len;
    x->result_err = errstr;
    x->result_generation = my_gen;
    if (!overridepath[0] && have_bin) {
        strncpy(x->result_binpath, binpath, MAX_PATH_CHARS - 1);
        x->result_binpath[MAX_PATH_CHARS - 1] = '\0';
    }
    else {
        x->result_binpath[0] = '\0';
    }
    systhread_mutex_unlock(x->mutex);

    if (src) {
        free(src);
    }

    qelem_set(x->done_qelem);
    systhread_exit(0);
}

void lilypond_render_done(t_lilypond* x)
{
    // The worker has signalled completion; join it so its handle is reclaimed
    // on the main thread (where it was created).
    if (x->render_thread) {
        unsigned int ret = 0;
        systhread_join(x->render_thread, &ret);
        x->render_thread = NULL;
    }

    // Take ownership of the result buffers under the lock.
    systhread_mutex_lock(x->mutex);
    long gen = x->result_generation;
    char* svg = x->result_svg;
    long svg_len = x->result_svg_len;
    char* err = x->result_err;
    x->result_svg = NULL;
    x->result_svg_len = 0;
    x->result_err = NULL;
    char auto_binpath[MAX_PATH_CHARS];
    strncpy(auto_binpath, x->result_binpath, MAX_PATH_CHARS);
    x->result_binpath[0] = '\0';
    char current = (gen == x->generation);
    systhread_mutex_unlock(x->mutex);

    // Only act on the newest request; stale renders are discarded so rapid
    // edits never paint an out-of-date image.
    if (current) {
        if (svg && svg_len > 0) {
            t_jsvg* s = jsvg_create_from_xmlstring(svg);
            if (s) {
                if (x->jsvg_result) {
                    jsvg_destroy(x->jsvg_result);
                }
                x->jsvg_result = s;
                jsvg_get_size(s, &x->jsvg_w, &x->jsvg_h);

                t_rect r;
                jbox_get_patching_rect((t_object*)x, &r);
                r.width = x->jsvg_w * x->size + 2.0 * x->margin;
                r.height = x->jsvg_h * x->size + 2.0 * x->margin;
                jbox_set_patching_rect((t_object*)x, &r);

                jbox_invalidate_layer((t_object*)x, NULL, ps_lilypond);
                jbox_redraw(&x->b_jbox);
            }
            else {
                object_error((t_object*)x, "lilypond: could not parse SVG output");
            }
        }
        else if (err) {
            object_error((t_object*)x, "%s", err);
        }

        // Back-fill the attribute when auto-discovery found a binary and the
        // user hasn't set one manually, so the Inspector reflects the active path.
        if (auto_binpath[0] && !(x->lilypondpath && x->lilypondpath->s_name[0])) {
            x->lilypondpath = gensym(auto_binpath);
            object_attr_touch((t_object*)x, gensym("lilypondpath"));
        }
    }

    if (svg) {
        free(svg);
    }
    if (err) {
        free(err);
    }

    // Clear busy and, if a newer request arrived during the render, start again.
    systhread_mutex_lock(x->mutex);
    x->busy = 0;
    char restart = x->pending;
    x->pending = 0;
    if (restart) {
        x->busy = 1;
    }
    systhread_mutex_unlock(x->mutex);

    if (restart) {
        lilypond_start_worker(x);
    }
}

#else // unsupported platform

void ext_main(void* r)
{
    (void)r;
    object_error(NULL, "lilypond: this object is only supported on macOS and Windows");
}

#endif // MAC_VERSION || WIN_VERSION
