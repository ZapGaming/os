#include <gui/shell.h>
#include <fs/fat32.h>
#include <kernel/elf.h>
#include <kernel/kheap.h>
#include <kernel/scheduler.h>
#include <js/js.h>
#include <js/lexer.h>
#include <js/dom_binding.h>
#include <py/py.h>
#include <string.h>

#define SHELL_MAX_ENTRIES 32
#define SHELL_LINE_MAX 256
#define SHELL_MAX_ARGS 8
#define SHELL_READ_CAP (64u * 1024)

static char to_upper_ch(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

static int str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (to_upper_ch(*a) != to_upper_ch(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int has_ext_ieq(const char *name, const char *ext) {
    int nlen = (int)strlen(name), elen = (int)strlen(ext);
    if (nlen < elen + 1 || name[nlen - elen - 1] != '.') return 0;
    return str_ieq(name + nlen - elen, ext);
}

/* Splits `line` in place on whitespace (no quoting support) into up to
 * `max_args` tokens. Returns the token count. */
static int split_args(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = 0; p++; }
    }
    return argc;
}

/* Finds `name` (case-insensitive) among `cwd`'s entries. Returns 1 and
 * fills `out` on a match -- `out->name` then holds the entry's real,
 * on-disk case, which callers should pass on to fat32_* functions
 * instead of the user's typed case (an 8.3 raw-byte comparison inside
 * fat32.c is exact-case, so "cat readme.txt" has to become "README.TXT"
 * to actually match what's on disk). */
static int find_entry(uint32_t cwd, const char *name, struct fat_dirent_info *out) {
    struct fat_dirent_info entries[SHELL_MAX_ENTRIES];
    int count = fat32_list_dir(cwd, entries, SHELL_MAX_ENTRIES);
    for (int i = 0; i < count; i++) {
        if (str_ieq(entries[i].name, name)) { *out = entries[i]; return 1; }
    }
    return 0;
}

static void print_uint(void (*out)(const char *), unsigned int v) {
    char buf[12];
    int i = 10;
    buf[11] = 0;
    if (v == 0) buf[i--] = '0';
    while (v) { buf[i--] = (char)('0' + v % 10); v /= 10; }
    out(&buf[i + 1]);
}

static void cmd_help(void (*out)(const char *)) {
    out("Built-in commands:\n");
    out("  ls                 list the current directory\n");
    out("  cd <dir>           change directory (.. goes up, / goes to root)\n");
    out("  pwd                print the current directory\n");
    out("  cat <file>         print a file's contents\n");
    out("  echo <text>        print text\n");
    out("  mkdir <dir>        create a directory\n");
    out("  rm <name>          delete a file or empty directory\n");
    out("  mv <old> <new>     rename a file or directory\n");
    out("  run <name.ELF>     launch a program (or just type its name)\n");
    out("  js <name.js>       run a script with the JS engine\n");
    out("  python <name.py>   run a script with the Python interpreter\n");
    out("  ps                 list tasks\n");
    out("  clear              clear the screen\n");
    out("  help               this text\n");
}

static void cmd_ls(uint32_t cwd, void (*out)(const char *)) {
    struct fat_dirent_info entries[SHELL_MAX_ENTRIES];
    int count = fat32_list_dir(cwd, entries, SHELL_MAX_ENTRIES);
    if (count == 0) { out("(empty)\n"); return; }
    for (int i = 0; i < count; i++) {
        out(entries[i].name);
        out(entries[i].is_dir ? "/\n" : "\n");
    }
}

static void cmd_pwd(uint32_t cwd, void (*out)(const char *)) {
    uint32_t root = fat32_root_cluster();
    if (cwd == root) { out("/\n"); return; }

    char parts[8][FAT32_MAX_NAME];
    int n = 0;
    uint32_t cur = cwd;
    while (cur != root && n < 8) {
        uint32_t parent = fat32_parent_cluster(cur);
        struct fat_dirent_info entries[SHELL_MAX_ENTRIES];
        int count = fat32_list_dir(parent, entries, SHELL_MAX_ENTRIES);
        const char *name = "?";
        for (int i = 0; i < count; i++) {
            if (entries[i].is_dir && entries[i].cluster == cur) { name = entries[i].name; break; }
        }
        strncpy(parts[n], name, FAT32_MAX_NAME - 1);
        parts[n][FAT32_MAX_NAME - 1] = 0;
        n++;
        cur = parent;
    }
    for (int i = n - 1; i >= 0; i--) {
        out("/");
        out(parts[i]);
    }
    out("\n");
}

static void cmd_cd(uint32_t *cwd, const char *arg, void (*out)(const char *)) {
    if (!arg || !*arg || strcmp(arg, "/") == 0) { *cwd = fat32_root_cluster(); return; }
    if (strcmp(arg, ".") == 0) return;
    if (strcmp(arg, "..") == 0) { *cwd = fat32_parent_cluster(*cwd); return; }

    struct fat_dirent_info e;
    if (!find_entry(*cwd, arg, &e) || !e.is_dir) {
        out("cd: no such directory: ");
        out(arg);
        out("\n");
        return;
    }
    *cwd = e.cluster;
}

static void cmd_cat(uint32_t cwd, const char *arg, void (*out)(const char *)) {
    struct fat_dirent_info e;
    if (!find_entry(cwd, arg, &e) || e.is_dir) {
        out("cat: no such file: ");
        out(arg);
        out("\n");
        return;
    }
    uint32_t cap = e.size < SHELL_READ_CAP ? e.size : SHELL_READ_CAP;
    char *buf = (char *)kmalloc(cap + 1);
    if (!buf) { out("cat: out of memory\n"); return; }
    uint32_t got = fat32_read_file(e.cluster, e.size, buf, cap);
    buf[got] = 0;
    out(buf);
    if (got == 0 || buf[got - 1] != '\n') out("\n");
    kfree(buf);
}

static void cmd_mkdir(uint32_t cwd, const char *arg, void (*out)(const char *)) {
    char name[FAT32_MAX_NAME];
    strncpy(name, arg, FAT32_MAX_NAME - 1);
    name[FAT32_MAX_NAME - 1] = 0;
    for (char *p = name; *p; p++) *p = to_upper_ch(*p);
    if (!fat32_mkdir(cwd, name)) out("mkdir: failed (already exists, or disk full)\n");
}

static void cmd_rm(uint32_t cwd, const char *arg, void (*out)(const char *)) {
    struct fat_dirent_info e;
    if (!find_entry(cwd, arg, &e)) { out("rm: no such file or directory: "); out(arg); out("\n"); return; }
    if (!fat32_delete_file(cwd, e.name)) out("rm: failed (directory not empty?)\n");
}

static void cmd_mv(uint32_t cwd, const char *old_name, const char *new_name, void (*out)(const char *)) {
    struct fat_dirent_info e;
    if (!find_entry(cwd, old_name, &e)) { out("mv: no such file or directory: "); out(old_name); out("\n"); return; }
    char upper_new[FAT32_MAX_NAME];
    strncpy(upper_new, new_name, FAT32_MAX_NAME - 1);
    upper_new[FAT32_MAX_NAME - 1] = 0;
    for (char *p = upper_new; *p; p++) *p = to_upper_ch(*p);
    if (!fat32_rename_file(cwd, e.name, cwd, upper_new)) out("mv: failed (destination name taken?)\n");
}

static void cmd_ps(void (*out)(const char *)) {
    int count = scheduler_task_count();
    for (int pid = 0; pid < count; pid++) {
        out("pid ");
        print_uint(out, (unsigned int)pid);
        enum task_state st = scheduler_task_state(pid);
        out(st == TASK_RUNNING ? ": running\n" : st == TASK_READY ? ": ready\n" : ": terminated\n");
    }
}

/* Reads `name` (must already have been resolved via find_entry, so
 * `name` holds the on-disk case) fully into a fresh NUL-terminated
 * kmalloc'd buffer -- the shape js_run_program/py_run's source-string
 * parameter needs. Caller frees. NULL on failure. */
static char *read_whole_file(uint32_t cwd, const char *name, uint32_t *out_len) {
    struct fat_dirent_info e;
    if (!find_entry(cwd, name, &e) || e.is_dir) return NULL;
    char *buf = (char *)kmalloc(e.size + 1);
    if (!buf) return NULL;
    uint32_t got = fat32_read_file(e.cluster, e.size, buf, e.size);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

static void cmd_run(uint32_t cwd, const char *arg, void (*out)(const char *), int *launched_pid) {
    struct fat_dirent_info e;
    if (!find_entry(cwd, arg, &e) || e.is_dir) {
        out("run: no such file: ");
        out(arg);
        out("\n");
        return;
    }
    uint8_t *buf = (uint8_t *)kmalloc(e.size > 0 ? e.size : 1);
    uint32_t got = buf ? fat32_read_file(e.cluster, e.size, buf, e.size) : 0;
    int pid = buf ? elf_load_and_run(buf, got) : -1;
    if (buf) kfree(buf);
    if (pid >= 0) {
        out("launched as pid ");
        print_uint(out, (unsigned int)pid);
        out("\n");
        *launched_pid = pid;
    } else {
        out("run: failed to launch (see serial log)\n");
    }
}

static void cmd_js(uint32_t cwd, const char *arg, void (*out)(const char *)) {
    uint32_t len;
    char *source = read_whole_file(cwd, arg, &len);
    if (!source) { out("js: no such file: "); out(arg); out("\n"); return; }

    js_arena_reset();
    js_dom_reset();
    struct js_lexer lx;
    js_lexer_init(&lx, source, len);
    struct js_node *program = js_parse_program(&lx);
    struct js_env *env = js_make_global_env(NULL);
    js_set_console_sink(out);
    js_run_program(program, env);
    js_set_console_sink(NULL);
    js_dom_clear_relayout_flag();

    kfree(source);
}

static void cmd_python(uint32_t cwd, const char *arg, void (*out)(const char *)) {
    uint32_t len;
    char *source = read_whole_file(cwd, arg, &len);
    if (!source) { out("python: no such file: "); out(arg); out("\n"); return; }
    py_run(source, out);
    kfree(source);
}

int shell_execute(const char *cmdline, uint32_t *cwd, void (*out)(const char *)) {
    int launched_pid = -1;

    char line[SHELL_LINE_MAX];
    strncpy(line, cmdline, SHELL_LINE_MAX - 1);
    line[SHELL_LINE_MAX - 1] = 0;

    /* `echo` is the one command that wants its argument un-split (with
     * original spacing) -- grab it before tokenizing destroys that. */
    if (strncmp(line, "echo ", 5) == 0 || strcmp(line, "echo") == 0) {
        out(line[4] == ' ' ? line + 5 : "");
        out("\n");
        return -1;
    }

    char *argv[SHELL_MAX_ARGS];
    int argc = split_args(line, argv, SHELL_MAX_ARGS);
    if (argc == 0) return -1;

    const char *cmd = argv[0];
    if (strcmp(cmd, "help") == 0) cmd_help(out);
    else if (strcmp(cmd, "ls") == 0) cmd_ls(*cwd, out);
    else if (strcmp(cmd, "pwd") == 0) cmd_pwd(*cwd, out);
    else if (strcmp(cmd, "cd") == 0) cmd_cd(cwd, argc > 1 ? argv[1] : NULL, out);
    else if (strcmp(cmd, "cat") == 0 && argc > 1) cmd_cat(*cwd, argv[1], out);
    else if (strcmp(cmd, "mkdir") == 0 && argc > 1) cmd_mkdir(*cwd, argv[1], out);
    else if (strcmp(cmd, "rm") == 0 && argc > 1) cmd_rm(*cwd, argv[1], out);
    else if (strcmp(cmd, "mv") == 0 && argc > 2) cmd_mv(*cwd, argv[1], argv[2], out);
    else if (strcmp(cmd, "ps") == 0) cmd_ps(out);
    else if (strcmp(cmd, "run") == 0 && argc > 1) cmd_run(*cwd, argv[1], out, &launched_pid);
    else if (strcmp(cmd, "js") == 0 && argc > 1) cmd_js(*cwd, argv[1], out);
    else if (strcmp(cmd, "python") == 0 && argc > 1) cmd_python(*cwd, argv[1], out);
    else if (has_ext_ieq(cmd, "ELF") && argc == 1) cmd_run(*cwd, cmd, out, &launched_pid);
    else {
        out(cmd);
        out(": command not found (try 'help')\n");
    }

    return launched_pid;
}
