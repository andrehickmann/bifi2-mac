/*
 * Bi-Fi 2 native host.
 *
 * The original real-mode game code runs under Unicorn (dynamic binary
 * translation x86 -> host). Everything around it - DOS, BIOS, VGA, timer,
 * keyboard, mouse and sound - is implemented natively here.
 */
#ifndef HOST_H
#define HOST_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <unicorn/unicorn.h>

#define MEM_SIZE     0x110000
#define BIOS_SEG     0xF000
#define BIOS_LIN     (BIOS_SEG * 16)
#define ENV_SEG      0x0800
#define PSP_SEG      0x0900
#define MEM_TOP      0x9FFF
#define MAX_FILES    32
#define MAX_BLOCKS   64

#define FLAG_CF 0x0001
#define FLAG_ZF 0x0040
#define FLAG_IF 0x0200

typedef struct {
    uint16_t seg, paras;
} MemBlock;

typedef struct {
    /* VGA */
    uint8_t mode;
    uint8_t dac[768];
    int dac_write, dac_read;
    uint8_t seq_index, seq[8];
    uint8_t gc_index, gc[9];
    uint8_t crtc_index, crtc[0x19];
} VGA;

typedef struct {
    uint32_t reload;
    int write_lo;          /* -1 when the next write is the low byte */
    int latch_hi;          /* -1 when no latched high byte is pending */
    bool latched;          /* a counter latch command froze latch_value */
    uint16_t latch_value;
    uint16_t last_read;
    /* channel 2 (speaker timer, gated by port 61h bit 0) */
    uint16_t ch2_reload, ch2_latch;
    bool ch2_gate, ch2_counting, ch2_latched, ch2_read_hi, ch2_write_hi;
    double ch2_start;
    double acc;
    double last;
    uint8_t imr;
    uint8_t port61;
} PIT;

typedef struct {
    uint8_t queue[64];
    int head, tail;
    uint8_t current;
    uint16_t bios_buf[32];
    int bios_head, bios_tail;
} Keyboard;

typedef struct {
    int x, y;              /* driver position, virtual 640x200 */
    int target_x, target_y;/* absolute cursor target, 320x200 */
    bool has_target;
    int buttons;
    int press_count[3], release_count[3];
} Mouse;

typedef struct SB SB;

typedef struct Host {
    uc_engine *uc;
    char root[1024];       /* host directory that acts as C:\ */
    char cwd[128];         /* DOS current directory, e.g. "\\BIFI" */
    char program[16];      /* running executable, upper case */
    uint16_t load_seg;
    FILE *files[MAX_FILES];
    char file_paths[MAX_FILES][1024];
    char file_modes[MAX_FILES][4];
    uint32_t dta;
    MemBlock blocks[MAX_BLOCKS];
    int nblocks;
    int pending_irq[8];
    uint32_t irq_delivered[8], irq_lost[8], irq_merged, pit_reprograms;
    bool irq_waiting;      /* an unmasked IRQ is pending while IF is clear */
    int exit_code;
    bool exited;
    bool quit;             /* user closed the window */
    char find_list[64][260];
    int find_count, find_pos;
    VGA vga;
    PIT pit;
    Keyboard kbd;
    Mouse mouse;
    SB *sb;
    bool trace;
    /* Emulated time (see machine.c). All devices run on vtime: it advances
     * with executed code (bytes / speed) and with every port access
     * (io_cost, like an ISA bus cycle), and the frontend keeps it in step
     * with the host clock. */
    double vtime;
    double speed;          /* guest code bytes per emulated second */
    double io_cost;        /* emulated seconds per port access */
    /* CPU slicing (see machine.c) */
    int slice_us;
    double slice_end;
    uint32_t block_count;
    uint64_t exec_bytes, raw_bytes;   /* guest code bytes executed, a proxy for instructions */
    int pending_swint;
} Host;

/* machine.c */
void host_init(Host *h, const char *root);
void host_free(Host *h);
int host_load(Host *h, const char *exe, const char *args);
void host_slice(Host *h);
void host_set_slice(Host *h, int slice_us);
void host_raise_irq(Host *h, int irq);
double now_seconds(void);
void host_log(const char *fmt, ...);
static inline double host_time(Host *h) { return h->vtime; }

uint16_t reg16(Host *h, int r);
void setreg16(Host *h, int r, uint16_t v);
uint8_t rd8(Host *h, uint32_t a);
uint16_t rd16(Host *h, uint32_t a);
void wr8(Host *h, uint32_t a, uint8_t v);
void wr16(Host *h, uint32_t a, uint16_t v);
void rdmem(Host *h, uint32_t a, void *buf, size_t n);
void wrmem(Host *h, uint32_t a, const void *buf, size_t n);
static inline uint32_t lin(uint16_t seg, uint16_t off) { return ((uint32_t)seg << 4) + off; }
void set_cf(Host *h, bool on);
void set_zf(Host *h, bool on);

/* snapshot.c */
bool snapshot_save(Host *h, const char *path);
bool snapshot_load(Host *h, const char *path);

/* dos.c */
void int21(Host *h);

/* bios.c */
void int10(Host *h);
void int16(Host *h);
void int1a(Host *h);
void int2f(Host *h);
void int33(Host *h);

/* vga.c */
void vga_init(Host *h);
bool vga_in(Host *h, uint16_t port, uint8_t *v);
bool vga_out(Host *h, uint16_t port, uint8_t v);
void vga_set_mode(Host *h, uint8_t mode);
void vga_render(Host *h, uint32_t *rgba);

/* pit.c */
void pit_init(Host *h);
void pit_tick(Host *h);
bool pit_in(Host *h, uint16_t port, uint8_t *v);
bool pit_out(Host *h, uint16_t port, uint8_t v);

/* input.c */
void kbd_press(Host *h, uint8_t scancode, bool release, uint16_t bios_key);
bool kbd_in(Host *h, uint16_t port, uint8_t *v);
int kbd_pop_bios(Host *h);
void bios_int9(Host *h);
int kbd_peek_bios(Host *h);
void mouse_set_buttons(Host *h, int buttons);

/* sb.c */
SB *sb_create(Host *h);
bool sb_in(Host *h, uint16_t port, uint8_t *v);
bool sb_out(Host *h, uint16_t port, uint8_t v);
int sb_render(Host *h, int16_t *out, int frames, int rate);
void sb_update(Host *h);
void sb_stats(Host *h, uint32_t *late, uint32_t *total, uint32_t *underruns);

#endif
