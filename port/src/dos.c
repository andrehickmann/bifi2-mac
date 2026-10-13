/* INT 21h: files, memory, vectors and process exit, on a host directory
 * that plays drive C:. Paths are matched case-insensitively. */
#include "host.h"

#include <ctype.h>
#include <dirent.h>
#include <fnmatch.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define FIRST_HANDLE 5

static void read_cstring(Host *h, uint32_t a, char *out, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n; i++) {
        out[i] = rd8(h, a + i);
        if (!out[i])
            break;
    }
    out[i] = 0;
}

/* Find an existing entry in dir whose name matches name ignoring case. */
static void match_entry(const char *dir, const char *name, char *out, size_t n)
{
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcasecmp(e->d_name, name)) {
                snprintf(out, n, "%s", e->d_name);
                closedir(d);
                return;
            }
        }
        closedir(d);
    }
    snprintf(out, n, "%s", name);
    for (char *p = out; *p; p++)
        *p = toupper((unsigned char)*p);
}

static void host_path(Host *h, const char *dos, char *out, size_t n)
{
    char full[260];
    const char *p = dos;
    if (p[0] && p[1] == ':')
        p += 2;
    if (*p == '\\' || *p == '/')
        snprintf(full, sizeof full, "%s", p);
    else
        snprintf(full, sizeof full, "%s\\%s", h->cwd, p);

    snprintf(out, n, "%s", h->root);
    char *save, *tok = strtok_r(full, "\\/", &save);
    while (tok) {
        char entry[256];
        match_entry(out, tok, entry, sizeof entry);
        size_t len = strlen(out);
        snprintf(out + len, n - len, "/%s", entry);
        tok = strtok_r(NULL, "\\/", &save);
    }
}

static void ret_ax(Host *h, uint16_t v, bool ok)
{
    setreg16(h, UC_X86_REG_AX, v);
    set_cf(h, !ok);
}

static int new_handle(Host *h, FILE *f, const char *path, const char *mode)
{
    for (int i = FIRST_HANDLE; i < MAX_FILES; i++)
        if (!h->files[i]) {
            h->files[i] = f;
            snprintf(h->file_paths[i], sizeof h->file_paths[i], "%s", path);
            /* reopening for a snapshot must not truncate */
            snprintf(h->file_modes[i], sizeof h->file_modes[i], "%s", mode[0] == 'w' ? "r+b" : mode);
            return i;
        }
    fclose(f);
    return -1;
}

/* ---- memory arena: simple first fit over conventional memory ---- */

static int block_index(Host *h, uint16_t seg)
{
    for (int i = 0; i < h->nblocks; i++)
        if (h->blocks[i].seg == seg)
            return i;
    return -1;
}

static void sort_blocks(Host *h)
{
    for (int i = 1; i < h->nblocks; i++)
        for (int j = i; j > 0 && h->blocks[j].seg < h->blocks[j - 1].seg; j--) {
            MemBlock t = h->blocks[j];
            h->blocks[j] = h->blocks[j - 1];
            h->blocks[j - 1] = t;
        }
}

static uint16_t dos_alloc(Host *h, uint16_t paras, uint16_t *largest)
{
    sort_blocks(h);
    uint32_t start = PSP_SEG;
    uint32_t best = 0;
    for (int i = 0; i <= h->nblocks; i++) {
        uint32_t end = i < h->nblocks ? h->blocks[i].seg - 1 : MEM_TOP;
        if (end > start && end - start >= (uint32_t)paras + 1 && h->nblocks < MAX_BLOCKS) {
            h->blocks[h->nblocks].seg = start + 1;
            h->blocks[h->nblocks].paras = paras;
            h->nblocks++;
            return start + 1;
        }
        if (end > start + 1 && end - start - 1 > best)
            best = end - start - 1;
        if (i < h->nblocks)
            start = h->blocks[i].seg + h->blocks[i].paras;
    }
    *largest = best > 0xFFFF ? 0xFFFF : best;
    return 0;
}

static bool dos_resize(Host *h, uint16_t seg, uint16_t paras, uint16_t *largest)
{
    sort_blocks(h);
    int i = block_index(h, seg);
    if (i < 0) {
        *largest = 0;
        return false;
    }
    uint32_t limit = (i + 1 < h->nblocks ? h->blocks[i + 1].seg - 1 : MEM_TOP) - seg;
    if (paras <= limit) {
        h->blocks[i].paras = paras;
        return true;
    }
    *largest = limit;
    return false;
}

static void dos_free(Host *h, uint16_t seg)
{
    int i = block_index(h, seg);
    if (i >= 0)
        h->blocks[i] = h->blocks[--h->nblocks];
}

/* ---- find first / next ---- */

static void find_prepare(Host *h, const char *pattern)
{
    char path[1024];
    host_path(h, pattern, path, sizeof path);
    char *slash = strrchr(path, '/');
    char mask[256];
    snprintf(mask, sizeof mask, "%s", slash + 1);
    *slash = 0;
    if (!strcmp(mask, "*.*"))
        strcpy(mask, "*");
    h->find_count = h->find_pos = 0;
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && h->find_count < 64) {
        if (e->d_name[0] == '.')
            continue;
        if (!fnmatch(mask, e->d_name, FNM_CASEFOLD))
            snprintf(h->find_list[h->find_count++], 260, "%s/%s", path, e->d_name);
    }
    closedir(d);
}

static void find_next(Host *h)
{
    if (h->find_pos >= h->find_count) {
        ret_ax(h, 18, false);
        return;
    }
    const char *path = h->find_list[h->find_pos++];
    struct stat st;
    stat(path, &st);
    struct tm *t = localtime(&st.st_mtime);
    uint8_t rec[43] = {0};
    rec[0x15] = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
    uint16_t ftime = t->tm_hour << 11 | t->tm_min << 5 | t->tm_sec / 2;
    uint16_t fdate = (t->tm_year - 80) << 9 | (t->tm_mon + 1) << 5 | t->tm_mday;
    memcpy(rec + 0x16, &ftime, 2);
    memcpy(rec + 0x18, &fdate, 2);
    uint32_t size = (uint32_t)st.st_size;
    memcpy(rec + 0x1A, &size, 4);
    const char *name = strrchr(path, '/') + 1;
    for (int i = 0; i < 12 && name[i]; i++)
        rec[0x1E + i] = toupper((unsigned char)name[i]);
    wrmem(h, h->dta, rec, sizeof rec);
    ret_ax(h, 0, true);
}

void int21(Host *h)
{
    uint16_t ax = reg16(h, UC_X86_REG_AX), bx = reg16(h, UC_X86_REG_BX);
    uint16_t cx = reg16(h, UC_X86_REG_CX), dx = reg16(h, UC_X86_REG_DX);
    uint16_t ds = reg16(h, UC_X86_REG_DS), es = reg16(h, UC_X86_REG_ES);
    uint8_t ah = ax >> 8, al = ax & 0xFF;
    char name[260], path[1024];

    switch (ah) {
    case 0x30:
        setreg16(h, UC_X86_REG_AX, 0x0005);
        setreg16(h, UC_X86_REG_BX, 0);
        break;
    case 0x25:
        wr16(h, al * 4, dx);
        wr16(h, al * 4 + 2, ds);
        break;
    case 0x35:
        setreg16(h, UC_X86_REG_BX, rd16(h, al * 4));
        setreg16(h, UC_X86_REG_ES, rd16(h, al * 4 + 2));
        break;
    case 0x33:
        setreg16(h, UC_X86_REG_DX, dx & 0xFF00);
        break;
    case 0x1A:
        h->dta = lin(ds, dx);
        break;
    case 0x2F:
        setreg16(h, UC_X86_REG_ES, h->dta >> 4);
        setreg16(h, UC_X86_REG_BX, h->dta & 0xF);
        break;
    case 0x34:
        setreg16(h, UC_X86_REG_ES, BIOS_SEG);          /* InDOS flag: a zero byte */
        setreg16(h, UC_X86_REG_BX, 0x300);
        break;
    case 0x2A: case 0x2C: {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        if (ah == 0x2A) {
            setreg16(h, UC_X86_REG_CX, t->tm_year + 1900);
            setreg16(h, UC_X86_REG_DX, (t->tm_mon + 1) << 8 | t->tm_mday);
            setreg16(h, UC_X86_REG_AX, (ax & 0xFF00) | t->tm_wday);
        } else {
            setreg16(h, UC_X86_REG_CX, t->tm_hour << 8 | t->tm_min);
            setreg16(h, UC_X86_REG_DX, t->tm_sec << 8);
        }
        break;
    }
    case 0x3D: case 0x3C: {
        read_cstring(h, lin(ds, dx), name, sizeof name);
        host_path(h, name, path, sizeof path);
        const char *mode = ah == 0x3C ? "w+b" : ((al & 3) ? "r+b" : "rb");
        FILE *f = fopen(path, mode);
        if (!f) {
            if (h->trace)
                host_log("open failed: %s", path);
            ret_ax(h, 2, false);
            break;
        }
        int hd = new_handle(h, f, path, mode);
        ret_ax(h, hd < 0 ? 4 : hd, hd >= 0);
        break;
    }
    case 0x3E:
        if (bx < MAX_FILES && h->files[bx]) {
            fclose(h->files[bx]);
            h->files[bx] = NULL;
        }
        ret_ax(h, 0, true);
        break;
    case 0x3F: {
        if (bx >= MAX_FILES || !h->files[bx]) {
            ret_ax(h, bx < FIRST_HANDLE ? 0 : 6, bx < FIRST_HANDLE);
            break;
        }
        uint8_t buf[0x10000];
        size_t got = fread(buf, 1, cx, h->files[bx]);
        wrmem(h, lin(ds, dx), buf, got);
        ret_ax(h, got, true);
        break;
    }
    case 0x40: {
        uint8_t buf[0x10000];
        rdmem(h, lin(ds, dx), buf, cx);
        if (bx == 1 || bx == 2) {
            fwrite(buf, 1, cx, stderr);
            ret_ax(h, cx, true);
            break;
        }
        if (bx >= MAX_FILES || !h->files[bx]) {
            ret_ax(h, 6, false);
            break;
        }
        fwrite(buf, 1, cx, h->files[bx]);
        fflush(h->files[bx]);
        ret_ax(h, cx, true);
        break;
    }
    case 0x42: {
        if (bx >= MAX_FILES || !h->files[bx]) {
            ret_ax(h, 6, false);
            break;
        }
        int32_t off = (int32_t)((uint32_t)cx << 16 | dx);
        fseek(h->files[bx], off, al == 0 ? SEEK_SET : al == 1 ? SEEK_CUR : SEEK_END);
        long pos = ftell(h->files[bx]);
        setreg16(h, UC_X86_REG_DX, pos >> 16);
        ret_ax(h, pos & 0xFFFF, true);
        break;
    }
    case 0x41: {
        read_cstring(h, lin(ds, dx), name, sizeof name);
        host_path(h, name, path, sizeof path);
        int r = remove(path);
        ret_ax(h, r ? 2 : 0, r == 0);
        break;
    }
    case 0x39:
        read_cstring(h, lin(ds, dx), name, sizeof name);
        host_path(h, name, path, sizeof path);
        mkdir(path, 0755);
        ret_ax(h, 0, true);
        break;
    case 0x3A:
        read_cstring(h, lin(ds, dx), name, sizeof name);
        host_path(h, name, path, sizeof path);
        rmdir(path);
        ret_ax(h, 0, true);
        break;
    case 0x3B:
        read_cstring(h, lin(ds, dx), name, sizeof name);
        snprintf(h->cwd, sizeof h->cwd, "%s", name[1] == ':' ? name + 2 : name);
        ret_ax(h, 0, true);
        break;
    case 0x47:
        wrmem(h, lin(ds, reg16(h, UC_X86_REG_SI)), h->cwd + 1, strlen(h->cwd));
        ret_ax(h, 0, true);
        break;
    case 0x19:
        setreg16(h, UC_X86_REG_AX, (ax & 0xFF00) | 2);
        break;
    case 0x0E:
        setreg16(h, UC_X86_REG_AX, (ax & 0xFF00) | 3);
        break;
    case 0x44:
        if (al == 0) {
            setreg16(h, UC_X86_REG_DX, bx < FIRST_HANDLE ? 0x80D3 : 0x0002);
            ret_ax(h, ax, true);
        } else {
            ret_ax(h, 0, true);
        }
        break;
    case 0x48: {
        uint16_t largest = 0, seg = dos_alloc(h, bx, &largest);
        if (seg) {
            ret_ax(h, seg, true);
        } else {
            setreg16(h, UC_X86_REG_BX, largest);
            ret_ax(h, 8, false);
        }
        break;
    }
    case 0x49:
        dos_free(h, es);
        ret_ax(h, 0, true);
        break;
    case 0x4A: {
        uint16_t largest = 0;
        if (dos_resize(h, es, bx, &largest)) {
            ret_ax(h, 0, true);
        } else {
            setreg16(h, UC_X86_REG_BX, largest);
            ret_ax(h, 8, false);
        }
        break;
    }
    case 0x4C:
        h->exit_code = al;
        h->exited = true;
        uc_emu_stop(h->uc);
        break;
    case 0x4E:
        read_cstring(h, lin(ds, dx), name, sizeof name);
        find_prepare(h, name);
        find_next(h);
        break;
    case 0x4F:
        find_next(h);
        break;
    case 0x36:
        setreg16(h, UC_X86_REG_AX, 64);
        setreg16(h, UC_X86_REG_BX, 0x4000);
        setreg16(h, UC_X86_REG_CX, 512);
        setreg16(h, UC_X86_REG_DX, 0x8000);
        break;
    case 0x0B:
        setreg16(h, UC_X86_REG_AX, ax & 0xFF00);
        break;
    default:
        host_log("unhandled INT 21 AX=%04x from %04x:%04x", ax,
                 rd16(h, lin(reg16(h, UC_X86_REG_SS), reg16(h, UC_X86_REG_SP) + 2)),
                 rd16(h, lin(reg16(h, UC_X86_REG_SS), reg16(h, UC_X86_REG_SP))));
        ret_ax(h, 1, false);
        break;
    }
}
