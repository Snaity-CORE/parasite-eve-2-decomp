/*
 * C versions of the handwritten MIPS routines in src/main/hasm/, for native
 * builds only: the PSX build keeps assembling the .s files, and this file is
 * not part of it (the port compiles it as an extra unit of the main image).
 * Each routine does what its .s does, quirks included; the port checks them
 * call by call against the original code (PE2_SHADOW).
 */
#ifdef PC_PORT

#include "main/fs.h"
#include "main/tmd.h"

#include <psyq/sys/types.h>

#include "common.h"

extern u8* Fs_ChunkReadPtr;
extern u8* Fs_ChunkWritePtr;
extern u16 D5B498_8006D748;
extern u8* D_8006C4D4;
extern void* D5B498_8006D850;
extern u16 D5B498_8006D858;
extern u16 D5B498_8006D85A;
extern s16 D_8006EA08;
extern s16 D_8006EA0A;
extern s32 D_8006EA0C;
extern u16 D5B498_8006EA1A;
extern u16 D5B498_8006EBB0;

/* ---- LZ decompression ----------------------------------------------------- */

/* The LZ ring: 256 bytes at the start of the scratchpad. */
#define FS_LZ_RING ((volatile u8*)0x1F800000)

/* Bit reader state shared by the decompressors: mask of the bits still
 * unread in the current byte (high bits first), the byte, and how many of
 * its bits remain. */
typedef struct {
    const u8* src;
    u32 mask;
    u32 byte;
    u32 bits;
} FsBits;

static inline void fs_bits_refill(FsBits* b)
{
    b->mask = 0xFF;
    b->bits = 8;
    b->byte = *b->src++;
}

/* An n-bit field (n = 8 or 4), high bits first: the bits left in the
 * current byte, then the rest from the next one. For n = 8 the next byte
 * is always loaded, even when no bits are needed from it. */
static inline u32 fs_bits_read8(FsBits* b)
{
    u32 value, take = 8;
    if (b->mask == 0) {
        fs_bits_refill(b);
    }
    value = b->mask & b->byte;
    take -= b->bits;
    fs_bits_refill(b);
    if (take != 0) {
        u32 low;
        b->mask >>= take;
        b->bits -= take;
        low = (b->mask ^ 0xFF) & b->byte;
        value = (value << take) | (low >> b->bits);
    }
    return value;
}

static inline u32 fs_bits_read4(FsBits* b)
{
    u32 value = 0, take = 4, low;
    s32 more;
    if (b->mask == 0) {
        fs_bits_refill(b);
    }
    more = (s32)(take - b->bits);
    if (more > 0) {
        take  = (u32)more;
        value = b->mask & b->byte;
        fs_bits_refill(b);
    }
    low = b->mask & b->byte;
    b->mask >>= take;
    b->bits -= take;
    low &= b->mask ^ 0xFF;
    return (value << take) | (low >> b->bits);
}

/// See src/main/hasm/Fs_DecompressImage.s: unpacks the image payload at
/// Fs_ChunkReadPtr into Fs_ChunkWritePtr (neither is advanced). Each flag
/// bit selects a literal byte or a copy of (4-bit length + 2) bytes from an
/// 8-bit offset into the 256-byte ring; offset 0 ends the data. Sets
/// D5B498_8006D748 to 1, or to 0xFFFF without unpacking when the scratchpad
/// word at 0x1F8003FC is 256 or less.
void Fs_DecompressImage(void)
{
    FsBits b;
    u8* dst;
    u32 ring;

    if (*(volatile s16*)0x1F8003FC - 0x100 <= 0) {
        D5B498_8006D748 = 0xFFFF;
        return;
    }
    b.src  = Fs_ChunkReadPtr;
    b.mask = 0;
    b.byte = 0;
    b.bits = 0;
    dst    = Fs_ChunkWritePtr;
    ring   = 1;
    for (;;) {
        u32 flag;
        if (b.mask == 0) {
            fs_bits_refill(&b);
        }
        flag = b.mask & b.byte;
        b.mask >>= 1;
        b.bits--;
        flag &= b.mask ^ 0xFF;
        if ((s32)flag > 0) {
            u8 v = (u8)fs_bits_read8(&b);
            FS_LZ_RING[ring] = v;
            *dst++ = v;
            ring = (ring + 1) & 0xFF;
        } else {
            u32 offset = fs_bits_read8(&b);
            s32 count;
            if (offset == 0) {
                D5B498_8006D748 = 1;
                return;
            }
            count = (s32)fs_bits_read4(&b) + 1;
            do {
                u8 v = FS_LZ_RING[offset & 0xFF];
                FS_LZ_RING[ring] = v;
                *dst++ = v;
                ring = (ring + 1) & 0xFF;
                offset++;
            } while (--count >= 0);
        }
    }
}

/* Resume points of Fs_DecompressChunk: the PSX addresses of the places in
 * the original routine where it continues after running out of input.
 * They are stored in D5B498_8006D850 as the original stores them. */
#define FS_RESUME_FLAG        0x800100F4u /* after the flag-bit refill */
#define FS_RESUME_LITERAL     0x80010148u /* after the literal's refill */
#define FS_RESUME_LITERAL2    0x80010180u /* after the literal's second byte */
#define FS_RESUME_OFFSET      0x80010200u /* after the match offset's refill */
#define FS_RESUME_OFFSET2     0x80010238u /* after the match offset's second byte */
#define FS_RESUME_LENGTH      0x800102A0u /* after the match length's refill */
#define FS_RESUME_LENGTH2     0x800102E0u /* after the match length's second byte */

/// See src/main/hasm/Fs_DecompressChunk.s: Fs_DecompressImage's format, read
/// from CD sectors as they arrive. The input ends at D_8006C4D4; when a byte
/// read reaches it, the routine saves its whole state (cursors, bit reader,
/// ring index, the field being read) in globals and the place to resume in
/// D5B498_8006D850, and returns. The next call continues from there.
void Fs_DecompressChunk(void)
{
    const u8* src;
    const u8* end;
    u8* dst;
    u32 mask, byte, ring, bits, value, take, offset, resume;
    s32 count, more;

    if (*(volatile s16*)0x1F8003FC - 0x100 <= 0) {
        D5B498_8006D748 = 0xFFFF;
        return;
    }
    src    = Fs_ChunkReadPtr;
    end    = D_8006C4D4;
    dst    = Fs_ChunkWritePtr;
    mask   = D5B498_8006EBB0;
    byte   = D5B498_8006EA1A;
    ring   = D5B498_8006D858;
    bits   = D5B498_8006D85A;
    value  = (u32)(s32)D_8006EA08;
    take   = (u32)(s32)D_8006EA0A;
    offset = (u32)D_8006EA0C;
    resume = (u32)D5B498_8006D850;
    D5B498_8006D850 = NULL;

#define FS_REFILL()                                                                                   \
    do {                                                                                              \
        mask = 0xFF;                                                                                  \
        bits = 8;                                                                                     \
        byte = *src++;                                                                                \
    } while (0)
#define FS_SUSPEND_AT_END(where)                                                                      \
    do {                                                                                              \
        if (src == end) {                                                                             \
            D5B498_8006D850 = (void*)(where);                                                         \
            goto save;                                                                                \
        }                                                                                             \
    } while (0)

    switch (resume) {
        case 0: goto next;
        case FS_RESUME_FLAG: goto flag;
        case FS_RESUME_LITERAL: goto literal;
        case FS_RESUME_LITERAL2: goto literal2;
        case FS_RESUME_OFFSET: goto match_offset;
        case FS_RESUME_OFFSET2: goto match_offset2;
        case FS_RESUME_LENGTH: goto match_length;
        case FS_RESUME_LENGTH2: goto match_length2;
        default: goto save; /* not a resume point of the original */
    }

next:
    if (mask == 0) {
        FS_REFILL();
        if (src == end) {
            /* The original's jump out has the flag's first step in its
             * delay slot, so that is done before saving. */
            value = mask & byte;
            D5B498_8006D850 = (void*)FS_RESUME_FLAG;
            goto save;
        }
    }
flag:
    value = mask & byte;
    mask >>= 1;
    bits--;
    value &= mask ^ 0xFF;
    take = 8;
    if ((s32)value <= 0) {
        goto match;
    }
    /* A literal byte. */
    value = 0;
    if (mask == 0) {
        FS_REFILL();
        FS_SUSPEND_AT_END(FS_RESUME_LITERAL);
    }
literal:
    value = mask & byte;
    take -= bits;
    FS_REFILL();
    FS_SUSPEND_AT_END(FS_RESUME_LITERAL2);
literal2:
    if (take != 0) {
        u32 low;
        mask >>= take;
        bits -= take;
        low = (mask ^ 0xFF) & byte;
        value = (value << take) | (low >> bits);
    }
    FS_LZ_RING[ring] = (u8)value;
    *dst++ = (u8)value;
    ring = (ring + 1) & 0xFF;
    take = 8; /* the original's jump back has this in its delay slot */
    goto next;

match:
    /* A copy: 8-bit offset into the ring (0 ends the data), 4-bit length. */
    take   = 8;
    offset = 0;
    if (mask == 0) {
        FS_REFILL();
        FS_SUSPEND_AT_END(FS_RESUME_OFFSET);
    }
match_offset:
    offset = mask & byte;
    take -= bits;
    FS_REFILL();
    FS_SUSPEND_AT_END(FS_RESUME_OFFSET2);
match_offset2:
    if (take != 0) {
        u32 low;
        mask >>= take;
        bits -= take;
        low = (mask ^ 0xFF) & byte;
        offset = (offset << take) | (low >> bits);
    }
    take = 4;
    if (offset == 0) {
        D5B498_8006D748 = 1;
        goto save;
    }
    value = 0;
    if (mask == 0) {
        FS_REFILL();
        FS_SUSPEND_AT_END(FS_RESUME_LENGTH);
    }
match_length:
    more = (s32)(take - bits);
    if (more > 0) {
        take  = (u32)more;
        value = mask & byte;
        FS_REFILL();
        FS_SUSPEND_AT_END(FS_RESUME_LENGTH2);
    }
match_length2:
    {
        u32 low = mask & byte;
        mask >>= take;
        bits -= take;
        low &= mask ^ 0xFF;
        value = (value << take) | (low >> bits);
    }
    count = (s32)value + 1;
    do {
        value = FS_LZ_RING[offset & 0xFF];
        FS_LZ_RING[ring] = (u8)value;
        *dst++ = (u8)value;
        ring = (ring + 1) & 0xFF;
        offset++;
    } while (--count >= 0);
    goto next;

#undef FS_REFILL
#undef FS_SUSPEND_AT_END

save:
    Fs_ChunkReadPtr  = (u8*)src;
    Fs_ChunkWritePtr = dst;
    D5B498_8006EBB0  = (u16)mask;
    D5B498_8006EA1A  = (u16)byte;
    D5B498_8006D858  = (u16)ring;
    D5B498_8006D85A  = (u16)bits;
    D_8006EA08       = (s16)value;
    D_8006EA0A       = (s16)take;
    D_8006EA0C       = (s32)offset;
}

/* ---- TMD stream ----------------------------------------------------------- */

/// See src/main/hasm/tmdSkipStreamRecord.s: the cursor advanced past
/// elemCount elements of elemStride words.
u32* tmdSkipStreamRecord(TmdStreamWorkspace* ws, s32 flags, u32* stream)
{
    return (u32*)((u8*)stream + (u32)(ws->elemStride << 2) * (u32)ws->elemCount);
}

#endif
