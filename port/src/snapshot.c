/* Whole-machine snapshots: guest memory, CPU registers, device state and
 * open files. Used for quick save/restore and to skip ahead in tests. */
#include "host.h"

#include <stdlib.h>
#include <string.h>

#define SNAP_MAGIC 0x42494632u   /* "BIF2" */
#define SNAP_VERSION 2

static const int regs[] = {
    UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
    UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP,
    UC_X86_REG_EIP, UC_X86_REG_EFLAGS, UC_X86_REG_CS, UC_X86_REG_DS,
    UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_FS, UC_X86_REG_GS,
};
#define NREGS (sizeof regs / sizeof *regs)

typedef struct {
    uint32_t magic, version;
    uint64_t regs[NREGS];
    char cwd[128];
    char program[16];
    uint16_t load_seg;
    uint32_t dta;
    MemBlock blocks[MAX_BLOCKS];
    int nblocks;
    int pending_irq[8];
    VGA vga;
    PIT pit;
    Keyboard kbd;
    Mouse mouse;
    char file_paths[MAX_FILES][1024];
    char file_modes[MAX_FILES][4];
    long file_pos[MAX_FILES];
    uint32_t sb_size;
    double vtime;
} SnapHeader;

size_t sb_state_size(void);
void sb_state_get(Host *h, void *buf);
void sb_state_set(Host *h, const void *buf);

bool snapshot_save(Host *h, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    SnapHeader *s = calloc(1, sizeof *s);
    s->magic = SNAP_MAGIC;
    s->version = SNAP_VERSION;
    for (size_t i = 0; i < NREGS; i++)
        uc_reg_read(h->uc, regs[i], &s->regs[i]);
    memcpy(s->cwd, h->cwd, sizeof s->cwd);
    memcpy(s->program, h->program, sizeof s->program);
    s->load_seg = h->load_seg;
    s->dta = h->dta;
    memcpy(s->blocks, h->blocks, sizeof s->blocks);
    s->nblocks = h->nblocks;
    memcpy(s->pending_irq, h->pending_irq, sizeof s->pending_irq);
    s->vga = h->vga;
    s->pit = h->pit;
    s->kbd = h->kbd;
    s->mouse = h->mouse;
    for (int i = 0; i < MAX_FILES; i++) {
        if (h->files[i]) {
            memcpy(s->file_paths[i], h->file_paths[i], sizeof s->file_paths[i]);
            memcpy(s->file_modes[i], h->file_modes[i], sizeof s->file_modes[i]);
            s->file_pos[i] = ftell(h->files[i]);
        }
    }
    s->sb_size = (uint32_t)sb_state_size();
    s->vtime = h->vtime;
    fwrite(s, sizeof *s, 1, f);
    void *sb = malloc(s->sb_size);
    sb_state_get(h, sb);
    fwrite(sb, s->sb_size, 1, f);
    free(sb);
    uint8_t *mem = malloc(MEM_SIZE);
    rdmem(h, 0, mem, MEM_SIZE);
    fwrite(mem, MEM_SIZE, 1, f);
    free(mem);
    free(s);
    fclose(f);
    return true;
}

bool snapshot_load(Host *h, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    SnapHeader *s = calloc(1, sizeof *s);
    bool ok = fread(s, sizeof *s, 1, f) == 1 && s->magic == SNAP_MAGIC &&
              s->version == SNAP_VERSION && s->sb_size == sb_state_size();
    if (ok) {
        void *sb = malloc(s->sb_size);
        uint8_t *mem = malloc(MEM_SIZE);
        ok = fread(sb, s->sb_size, 1, f) == 1 && fread(mem, MEM_SIZE, 1, f) == 1;
        if (ok) {
            wrmem(h, 0, mem, MEM_SIZE);
            for (size_t i = 0; i < NREGS; i++)
                uc_reg_write(h->uc, regs[i], &s->regs[i]);
            memcpy(h->cwd, s->cwd, sizeof h->cwd);
            memcpy(h->program, s->program, sizeof h->program);
            h->load_seg = s->load_seg;
            h->dta = s->dta;
            memcpy(h->blocks, s->blocks, sizeof h->blocks);
            h->nblocks = s->nblocks;
            memcpy(h->pending_irq, s->pending_irq, sizeof h->pending_irq);
            h->vga = s->vga;
            h->pit = s->pit;
            h->kbd = s->kbd;
            h->mouse = s->mouse;
            h->vtime = s->vtime;
            sb_state_set(h, sb);
            if (!rd16(h, 0x480)) {             /* snapshots from before the BDA key buffer */
                wr16(h, 0x41A, 0x1E);
                wr16(h, 0x41C, 0x1E);
                wr16(h, 0x480, 0x1E);
                wr16(h, 0x482, 0x3E);
            }
            for (int i = 0; i < MAX_FILES; i++) {
                if (h->files[i]) {
                    fclose(h->files[i]);
                    h->files[i] = NULL;
                }
                if (s->file_paths[i][0]) {
                    h->files[i] = fopen(s->file_paths[i], s->file_modes[i]);
                    if (h->files[i])
                        fseek(h->files[i], s->file_pos[i], SEEK_SET);
                    memcpy(h->file_paths[i], s->file_paths[i], sizeof h->file_paths[i]);
                    memcpy(h->file_modes[i], s->file_modes[i], sizeof h->file_modes[i]);
                }
            }
        }
        free(sb);
        free(mem);
    }
    free(s);
    fclose(f);
    return ok;
}
