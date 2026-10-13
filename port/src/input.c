/* Keyboard controller (port 60h/64h, IRQ 1) and the BIOS keyboard buffer
 * in the BIOS data area (0040:001A head, 001C tail, 001E-003D ring). */
#include "host.h"

#define BDA_HEAD  0x41A
#define BDA_TAIL  0x41C
#define BDA_START 0x1E
#define BDA_END   0x3E

/* US layout, set 1 make codes 0x00-0x39 -> ASCII */
static const char ascii_map[] =
    "\0\x1b" "1234567890-=" "\b\t" "qwertyuiop[]" "\r\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0*\0 ";

static void bda_push(Host *h, uint16_t key)
{
    uint16_t head = rd16(h, BDA_HEAD), tail = rd16(h, BDA_TAIL);
    uint16_t next = tail + 2 >= BDA_END ? BDA_START : tail + 2;
    if (next == head)
        return;
    wr16(h, 0x400 + tail, key);
    wr16(h, BDA_TAIL, next);
}

/* BIOS INT 09h: the game's own handler chains here after reading port 60h. */
void bios_int9(Host *h)
{
    uint8_t sc = h->kbd.current;
    if (sc & 0x80)
        return;
    uint8_t ch = sc < sizeof ascii_map - 1 ? (uint8_t)ascii_map[sc] : 0;
    bda_push(h, sc << 8 | ch);
}

void kbd_press(Host *h, uint8_t scancode, bool release, uint16_t bios_key)
{
    Keyboard *k = &h->kbd;
    int next = (k->tail + 1) % 64;
    if (next != k->head) {
        k->queue[k->tail] = scancode | (release ? 0x80 : 0);
        k->tail = next;
    }
    (void)bios_key;
    host_raise_irq(h, 1);
}

bool kbd_in(Host *h, uint16_t port, uint8_t *v)
{
    Keyboard *k = &h->kbd;
    if (port == 0x60) {
        if (k->head != k->tail) {
            k->current = k->queue[k->head];
            if (h->trace)
                host_log("kbd read %02x", k->current);
            k->head = (k->head + 1) % 64;
            if (k->head != k->tail)
                host_raise_irq(h, 1);
        }
        *v = k->current;
        return true;
    }
    if (port == 0x64) {
        *v = 0x1C | (k->head != k->tail ? 1 : 0);
        return true;
    }
    return false;
}

int kbd_pop_bios(Host *h)
{
    uint16_t head = rd16(h, BDA_HEAD), tail = rd16(h, BDA_TAIL);
    if (head == tail)
        return -1;
    int v = rd16(h, 0x400 + head);
    wr16(h, BDA_HEAD, head + 2 >= BDA_END ? BDA_START : head + 2);
    return v;
}

int kbd_peek_bios(Host *h)
{
    uint16_t head = rd16(h, BDA_HEAD), tail = rd16(h, BDA_TAIL);
    return head == tail ? -1 : rd16(h, 0x400 + head);
}
