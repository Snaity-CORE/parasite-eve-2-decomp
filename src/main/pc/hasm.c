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
#include <psyq/libgte.h>
#include <psyq/inline_c.h>

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

/* ---- TMD drawing --------------------------------------------------------- */

/* Where the original's jal to Tmd_DispatchStream returns, which the
 * dispatcher keeps in the workspace. */
#define TMD_DISPATCH_RETURN_ADDRESS 0x800109F0u

/* A stream record handler, called through the PSX address the stream holds
 * (a native body or the original code, whichever is installed there). */
typedef u32* (*TmdRecordHandler)(TmdStreamWorkspace* ws, s32 flags, u32* stream, s32 count);

/// See src/main/hasm/Tmd_DispatchStream.s: runs the handler of each record
/// of one group, a record being its opcode, its handler's address and its
/// element count (high half) and stride (low half), then the elements.
/// Returns the TMD_STREAM_GROUP_END word, not consumed.
u32* Tmd_DispatchStream(TmdStreamWorkspace* ws, s32 flags, u32* stream)
{
    ws->dispatchObjectFlags   = flags;
    ws->dispatchReturnAddress = TMD_DISPATCH_RETURN_ADDRESS;
    for (;;) {
        u32 opcode = stream[0];
        u32 shape;
        TmdRecordHandler handler;
        s32 count;
        if (opcode == (u32)-2) {
            return stream;
        }
        flags   = ws->dispatchObjectFlags;
        shape   = stream[2];
        handler = (TmdRecordHandler)(uintptr_t)stream[1];
        count   = (s32)(shape >> 16);
        ws->opcode     = opcode;
        ws->elemCount  = count;
        ws->elemStride = (s32)(shape & 0xFFFF);
        stream = handler(ws, flags, stream + 3, count);
    }
}

/// See src/main/hasm/Tmd_SetupGteMatrices.s: for each part of the object
/// with a stream group, loads the GTE with the part's rotation (RT), the
/// light matrix rotated into the part's frame (LLM = viewLightRotation *
/// rotation, computed on the GTE itself from the transposed rotation) and
/// the part's translation (TR), then runs the group. Groups past the parts,
/// and a group that starts with its end word, run with the GTE as it is.
/// Stops at TMD_STREAM_END.
void Tmd_SetupGteMatrices(TmdStreamWorkspace* ws, u32 flags, void* streamArg, TmdObject* node)
{
    u32* stream = (u32*)streamArg;
    s32 parts   = node->partCount;
    u8* coord   = (u8*)node->coords;
    const u32* light = (const u32*)((u8*)ws + 0x50); /* viewLightRotation, as words */

    for (;; stream++, parts--, coord += 0x50) {
        u32 word = stream[0];
        if (word == (u32)-1) {
            return;
        }
        if (parts > 0 && word != (u32)-2) {
            /* The part's world matrix (workm): rotation words, then m[2][2]. */
            const u32* m = (const u32*)(coord + 0x24);
            u32 r0 = m[0], r1 = m[1], r2 = m[2], r3 = m[3], r4;
            u32 l0, l1, l2, l3, ir1, ir2, ir3, t;
            pcgte_write_ctrl(0, r0);
            pcgte_write_ctrl(1, r1);
            pcgte_write_ctrl(2, r2);
            pcgte_write_ctrl(3, r3);
            /* LLM = the rotation transposed, packed the same way. */
            l0 = (r0 & 0xFFFF) | (r1 & 0xFFFF0000);
            l1 = (r3 & 0xFFFF) | (r0 & 0xFFFF0000);
            l2 = (r2 & 0xFFFF) | (r3 & 0xFFFF0000);
            l3 = (r1 & 0xFFFF) | (r2 & 0xFFFF0000);
            r4 = m[4];
            pcgte_write_ctrl(8, l0);
            pcgte_write_ctrl(9, l1);
            pcgte_write_ctrl(10, l2);
            pcgte_write_ctrl(11, l3);
            pcgte_write_ctrl(4, r4);
            pcgte_write_ctrl(12, r4);
            /* Each row of viewLightRotation times that, through V0. */
            t = light[1];
            pcgte_write_data(0, light[0]);
            pcgte_write_data(1, t);
            {
                u32 w = light[2];
                u32 xy = ((w << 16) | (t >> 16));
                pcgte_op(0x4A0A6012u); /* MVMVA sf=1, LLM, V0, no add */
                pcgte_write_data(0, xy);
                pcgte_write_data(1, w >> 16);
            }
            ir1 = pcgte_read_data(9);
            ir2 = pcgte_read_data(10);
            ir3 = pcgte_read_data(11);
            l0  = (ir1 & 0xFFFF) | (ir2 << 16);
            pcgte_op(0x4A0A6012u);
            pcgte_write_data(0, light[3]);
            pcgte_write_data(1, light[4]);
            {
                u32 a = pcgte_read_data(9), b = pcgte_read_data(10), c = pcgte_read_data(11);
                l1 = (ir3 & 0xFFFF) | (a << 16);
                l2 = (b & 0xFFFF) | (c << 16);
            }
            pcgte_op(0x4A0A6012u);
            {
                u32 a = pcgte_read_data(9), b = pcgte_read_data(10), c = pcgte_read_data(11);
                l3 = (a & 0xFFFF) | (b << 16);
                pcgte_write_ctrl(8, l0);
                pcgte_write_ctrl(9, l1);
                pcgte_write_ctrl(10, l2);
                pcgte_write_ctrl(11, l3);
                pcgte_write_ctrl(12, c);
            }
            /* The part's translation. */
            pcgte_write_ctrl(5, m[5]);
            pcgte_write_ctrl(6, m[6]);
            pcgte_write_ctrl(7, m[7]);
        }
        stream = Tmd_DispatchStream(ws, (s32)flags, stream);
    }
}

/* GTE commands, as the handlers issue them. */
#define TMD_GTE_RTPS  0x4A180001u
#define TMD_GTE_RTPT  0x4A280030u
#define TMD_GTE_NCLIP 0x4B400006u
#define TMD_GTE_AVSZ3 0x4B58002Du
#define TMD_GTE_AVSZ4 0x4B68002Eu
#define TMD_GTE_NCCS  0x4B08041Bu
#define TMD_GTE_NCCT  0x4B18043Fu

/* Object flags the handlers test (main/tmd_types.h). */
#define TMD_OBJECT_SEMI_TRANS      0x02
#define TMD_OBJECT_REVERSE_CULLING 0x10

/* GPU command words with neutral RGB, and their command bytes. */
#define TMD_GT3_COLOR            0x34808080u
#define TMD_GT3_SEMI_TRANS_COLOR 0x36808080u
#define TMD_GT4_COLOR            0x3C808080u
#define TMD_GT4_SEMI_TRANS_COLOR 0x3E808080u
#define TMD_CODE(color)          ((u8)((color) >> 24))

/* Packet sizes in bytes; the DMA tag's length excludes the tag word. */
#define TMD_G3_BYTES  28
#define TMD_G4_BYTES  36
#define TMD_GT3_BYTES 40
#define TMD_GT4_BYTES 52
#define TMD_WORDS(bytes) ((bytes) / 4 - 1)

/* Outside the 16-bit offset domain: no previous vertex to reuse. */
#define TMD_NO_PREVIOUS_VERTEX     0x0FFF0000u
#define TMD_XFORM_NO_PREVIOUS      0x0FFE0000u
#define TMD_VERTEX_DEPTH_INVALID   0x80000000u

/* Loads GTE vector n (V0..V2) from the SVECTOR at addr. */
static inline void tmdLoadVector(u32 n, const u8* addr)
{
    pcgte_write_data(n * 2, *(const u32*)addr);
    pcgte_write_data(n * 2 + 1, *(const u32*)(addr + 4));
}

/* Prepends a packet (by its 24-bit DMA address) to the ordering-table
 * bucket of depth otz; returns the packet's new tag. */
static u32 tmdLinkPacket(TmdStreamWorkspace* ws, u8* packet, u32 otz, u32 bytes)
{
    u32* slot = (u32*)((u8*)ws->ot + ((((otz << (ws->otDepthShift & 31)) & 0x3FFF) >> 4) << 2));
    u32  next = *slot;
    *slot = (u32)(uintptr_t)packet & 0xFFFFFF;
    return ((u32)TMD_WORDS(bytes) << 24) | (next & 0xFFFFFF);
}

/* Corner k's screen XY and colour in a packet whose corners are xyStride
 * bytes apart (8 untextured, 12 textured). */
#define TMD_XY(packet, stride, k)    (*(u32*)((packet) + 0x08 + (k) * (stride)))
#define TMD_COLOR(packet, stride, k) (*(u32*)((packet) + 0x04 + (k) * (stride)))

/* Projects a triangle (vertices: word 0's halves, word 1's low half) and
 * stores its screen XY when the projection succeeds and the triangle faces
 * the camera: positive NCLIP area, or negative when reversed. */
static s32 tmdProjectTriangle(const u32* element, const u8* verts, u8* packet, u32 xyStride, s32 reverse)
{
    u32 w0 = element[0], w1 = element[1];
    s32 area;
    tmdLoadVector(0, verts + (w0 & 0xFFFF));
    tmdLoadVector(1, verts + (w0 >> 16));
    tmdLoadVector(2, verts + (w1 & 0xFFFF));
    pcgte_op(TMD_GTE_RTPT);
    if ((s32)pcgte_read_ctrl(31) < 0) {
        return 0;
    }
    pcgte_op(TMD_GTE_NCLIP);
    area = (s32)pcgte_read_data(24);
    if (reverse ? area >= 0 : area <= 0) {
        return 0;
    }
    TMD_XY(packet, xyStride, 0) = pcgte_read_data(12);
    TMD_XY(packet, xyStride, 1) = pcgte_read_data(13);
    TMD_XY(packet, xyStride, 2) = pcgte_read_data(14);
    return 1;
}

/* Vertex reuse between consecutive quads: the previous quad's vertex 0
 * (offset and screen XY) can be this one's vertex 3. */
typedef struct {
    u32 key;
    u32 xy;
} TmdQuadReuse;

static void tmdQuadReuseInit(TmdQuadReuse* reuse, const u32* elements)
{
    reuse->key = TMD_NO_PREVIOUS_VERTEX;
    /* The original keeps the XY in v0, which on entry holds the dispatcher's
     * previous handler result: this record's address. Only the reversed GT4
     * walk, which keeps the key after a failed projection, can read it. */
    reuse->xy = (u32)(uintptr_t)(elements - 3);
}

/* tmdProjectQuad modes. */
enum {
    TMD_QUAD_REVERSE   = 1, /* reversed culling; the key survives failed projections */
    TMD_QUAD_RELOAD_XY = 2, /* SXY2 takes vertex 3 even when the first triangle is kept */
};

/* Projects a quad (vertices: word 0's halves, word 1's halves): vertex 3
 * alone, unless it is the previous quad's vertex 0, whose depth is still in
 * the GTE FIFO, then vertices 2, 1, 0. Stores the screen XY (also on a
 * facing rejection) and returns whether it faces the camera: triangle
 * (2,1,0) negative, else triangle (2,1,3) positive (signs swapped when
 * reversed). */
static s32 tmdProjectQuad(const u32* element, const u8* verts, u8* packet, u32 xyStride, TmdQuadReuse* reuse, s32 mode)
{
    u32 w0 = element[0], w1 = element[1];
    s32 reverse = mode & TMD_QUAD_REVERSE;
    s32 area;
    if ((w1 >> 16) != reuse->key) {
        tmdLoadVector(0, verts + (w1 >> 16));
        pcgte_op(TMD_GTE_RTPS);
        reuse->key = w0 & 0xFFFF;
        if ((s32)pcgte_read_ctrl(31) < 0) {
            goto failed;
        }
        TMD_XY(packet, xyStride, 3) = pcgte_read_data(14);
    } else {
        reuse->key = w0 & 0xFFFF;
        TMD_XY(packet, xyStride, 3) = reuse->xy;
    }
    tmdLoadVector(0, verts + (w1 & 0xFFFF));
    tmdLoadVector(1, verts + (w0 >> 16));
    tmdLoadVector(2, verts + (w0 & 0xFFFF));
    pcgte_op(TMD_GTE_RTPT);
    if ((s32)pcgte_read_ctrl(31) < 0) {
        goto failed;
    }
    pcgte_op(TMD_GTE_NCLIP);
    reuse->xy = pcgte_read_data(14);
    TMD_XY(packet, xyStride, 2) = pcgte_read_data(12);
    TMD_XY(packet, xyStride, 1) = pcgte_read_data(13);
    TMD_XY(packet, xyStride, 0) = reuse->xy;
    area = (s32)pcgte_read_data(24);
    if (mode & TMD_QUAD_RELOAD_XY) {
        pcgte_write_data(14, TMD_XY(packet, xyStride, 3));
    }
    if (reverse ? area > 0 : area < 0) {
        return 1;
    }
    if (!(mode & TMD_QUAD_RELOAD_XY)) {
        pcgte_write_data(14, TMD_XY(packet, xyStride, 3));
    }
    pcgte_op(TMD_GTE_NCLIP);
    area = (s32)pcgte_read_data(24);
    return reverse ? area < 0 : area > 0;

failed:
    if (!reverse) {
        reuse->key = TMD_NO_PREVIOUS_VERTEX;
    }
    return 0;
}

/// See tmdXformStreamVerts in src/main/hasm/Tmd_StreamHandlers_Ops.s: per
/// element (vertex offset low half, normal offset high half; then the two
/// packet destinations), projects the vertex unless it repeats the previous
/// element's, caching its depth (with TMD_VERTEX_DEPTH_INVALID on failure),
/// lights the normal with RGB (128,128,128) and stores SXY2 and RGB2 at the
/// destinations, as byte offsets from preXformWrite.
u32* tmdXformStreamVerts(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    u32 stride   = (u32)ws->elemStride << 2;
    s32 count    = ws->elemCount;
    u8* packets  = ws->preXformWrite;
    u8* verts    = (u8*)ws->verts;
    u8* normals  = (u8*)ws->normals;
    u8* depths   = (u8*)ws->szTable;
    u32 previous = TMD_XFORM_NO_PREVIOUS;
    u8* element  = (u8*)elements;

    pcgte_write_data(6, 0x00808080u);
    while (count != 0) {
        u32 word   = *(u32*)element;
        u32 vertex = word & 0xFFFF;
        u8* normal = normals + (word >> 16);
        u32 dests;
        count--;
        if (vertex != previous) {
            u32 depth;
            previous = vertex;
            tmdLoadVector(0, verts + vertex);
            pcgte_op(TMD_GTE_RTPS);
            tmdLoadVector(0, normal);
            depth = pcgte_read_data(19);
            if ((s32)pcgte_read_ctrl(31) < 0) {
                depth |= TMD_VERTEX_DEPTH_INVALID;
            }
            *(u32*)(depths + (vertex >> 1)) = depth;
        } else {
            /* SXY2 and the cached depth survive lighting. */
            tmdLoadVector(0, normal);
        }
        dests = *(u32*)(element + 4);
        element += stride;
        pcgte_op(TMD_GTE_NCCS);
        *(u32*)(packets + (dests & 0xFFFF)) = pcgte_read_data(14);
        *(u32*)(packets + (dests >> 16))    = pcgte_read_data(22);
    }
    return (u32*)element;
}

/// See tmdXformStreamVertsElemColor in src/main/hasm/Tmd_StreamHandlers_Ops.s:
/// tmdXformStreamVerts with each element's own material colour (word 1);
/// the destinations move to word 2.
u32* tmdXformStreamVertsElemColor(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    u32 stride   = (u32)ws->elemStride << 2;
    s32 count    = ws->elemCount;
    u8* packets  = ws->preXformWrite;
    u8* verts    = (u8*)ws->verts;
    u8* normals  = (u8*)ws->normals;
    u8* depths   = (u8*)ws->szTable;
    u32 previous = TMD_XFORM_NO_PREVIOUS;
    u8* element  = (u8*)elements;

    while (count != 0) {
        u32 word   = *(u32*)element;
        u32 vertex = word & 0xFFFF;
        u8* normal = normals + (word >> 16);
        u32 dests  = *(u32*)(element + 8);
        count--;
        if (vertex != previous) {
            u32 depth;
            previous = vertex;
            tmdLoadVector(0, verts + vertex);
            pcgte_op(TMD_GTE_RTPS);
            pcgte_write_data(6, *(u32*)(element + 4));
            tmdLoadVector(0, normal);
            depth = pcgte_read_data(19);
            if ((s32)pcgte_read_ctrl(31) < 0) {
                depth |= TMD_VERTEX_DEPTH_INVALID;
            }
            *(u32*)(depths + (vertex >> 1)) = depth;
        } else {
            /* SXY2 and the cached depth survive lighting. */
            pcgte_write_data(6, *(u32*)(element + 4));
            tmdLoadVector(0, normal);
        }
        element += stride;
        pcgte_op(TMD_GTE_NCCS);
        *(u32*)(packets + (dests & 0xFFFF)) = pcgte_read_data(14);
        *(u32*)(packets + (dests >> 16))    = pcgte_read_data(22);
    }
    return (u32*)element;
}

/* ---- Triangles ---- */

/// See tmdDrawStreamPrimG3CornerNormals in src/main/hasm/Tmd_StreamHandlers_Ops.s:
/// an untextured triangle lit with one normal per corner (word 1's high
/// half, word 2's halves) and the element's material colour (word 3).
/// Object flags are ignored.
u32* tmdDrawStreamPrimG3CornerNormals(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->primWrite;
    u8* verts   = (u8*)ws->verts;
    u8* normals = (u8*)ws->normals;
    u8* element = (u8*)elements;

    for (; count != 0; count--, packet += TMD_G3_BYTES, element += stride) {
        const u32* e = (const u32*)element;
        u32 otz;
        if (!tmdProjectTriangle(e, verts, packet, 8, 0)) {
            continue;
        }
        pcgte_op(TMD_GTE_AVSZ3);
        pcgte_write_data(6, e[3]);
        tmdLoadVector(0, normals + (e[1] >> 16));
        tmdLoadVector(1, normals + (e[2] & 0xFFFF));
        tmdLoadVector(2, normals + (e[2] >> 16));
        otz = pcgte_read_data(7);
        pcgte_op(TMD_GTE_NCCT);
        *(u32*)packet          = tmdLinkPacket(ws, packet, otz, TMD_G3_BYTES);
        TMD_COLOR(packet, 8, 0) = pcgte_read_data(20);
        TMD_COLOR(packet, 8, 1) = pcgte_read_data(21);
        TMD_COLOR(packet, 8, 2) = pcgte_read_data(22);
    }
    ws->primWrite = packet;
    return (u32*)element;
}

/// See tmdDrawStreamPrimG3 in src/main/hasm/Tmd_StreamHandlers_Ops.s: the
/// triangle with one face normal (word 1's high half) and material colour
/// (word 2), the lit colour repeated at each corner.
u32* tmdDrawStreamPrimG3(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->primWrite;
    u8* verts   = (u8*)ws->verts;
    u8* normals = (u8*)ws->normals;
    u8* element = (u8*)elements;

    for (; count != 0; count--, packet += TMD_G3_BYTES, element += stride) {
        const u32* e = (const u32*)element;
        u32 otz, color;
        if (!tmdProjectTriangle(e, verts, packet, 8, 0)) {
            continue;
        }
        pcgte_op(TMD_GTE_AVSZ3);
        pcgte_write_data(6, e[2]);
        tmdLoadVector(0, normals + (e[1] >> 16));
        otz = pcgte_read_data(7);
        pcgte_op(TMD_GTE_NCCS);
        *(u32*)packet = tmdLinkPacket(ws, packet, otz, TMD_G3_BYTES);
        color = pcgte_read_data(22);
        TMD_COLOR(packet, 8, 0) = color;
        TMD_COLOR(packet, 8, 1) = color;
        TMD_COLOR(packet, 8, 2) = color;
    }
    ws->primWrite = packet;
    return (u32*)element;
}

/* GT3 with one normal per corner and a fixed command word and colour. */
static u32* tmdDrawStreamGt3Walk(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements, u32 color)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->primWrite;
    u8* verts   = (u8*)ws->verts;
    u8* normals = (u8*)ws->normals;
    s32 reverse = objectFlags & TMD_OBJECT_REVERSE_CULLING;
    u8* element = (u8*)elements;

    pcgte_write_data(6, color);
    for (; count != 0; count--, packet += TMD_GT3_BYTES, element += stride) {
        const u32* e = (const u32*)element;
        u32 otz;
        if (!tmdProjectTriangle(e, verts, packet, 12, reverse)) {
            continue;
        }
        pcgte_op(TMD_GTE_AVSZ3);
        tmdLoadVector(0, normals + (e[1] >> 16));
        tmdLoadVector(1, normals + (e[2] & 0xFFFF));
        tmdLoadVector(2, normals + (e[2] >> 16));
        otz = pcgte_read_data(7);
        pcgte_op(TMD_GTE_NCCT);
        *(u32*)packet           = tmdLinkPacket(ws, packet, otz, TMD_GT3_BYTES);
        TMD_COLOR(packet, 12, 0) = pcgte_read_data(20);
        TMD_COLOR(packet, 12, 1) = pcgte_read_data(21);
        TMD_COLOR(packet, 12, 2) = pcgte_read_data(22);
    }
    ws->primWrite = packet;
    return (u32*)element;
}

/// See tmdDrawStreamGt3 in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamGt3(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawStreamGt3Walk(ws, objectFlags, elements,
                                (objectFlags & TMD_OBJECT_SEMI_TRANS) ? TMD_GT3_SEMI_TRANS_COLOR : TMD_GT3_COLOR);
}

/// See tmdDrawStreamGt3SemiTrans in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamGt3SemiTrans(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawStreamGt3Walk(ws, objectFlags, elements, TMD_GT3_SEMI_TRANS_COLOR);
}

/* GT3 with one face normal (word 1's high half), a fixed command word and
 * colour; object flags are ignored. */
static u32* tmdDrawStreamPrimGt3OneNormalWalk(TmdStreamWorkspace* ws, u32* elements, u32 color)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->primWrite;
    u8* verts   = (u8*)ws->verts;
    u8* normals = (u8*)ws->normals;
    u8* element = (u8*)elements;

    pcgte_write_data(6, color);
    for (; count != 0; count--, packet += TMD_GT3_BYTES, element += stride) {
        const u32* e = (const u32*)element;
        u32 otz, lit;
        if (!tmdProjectTriangle(e, verts, packet, 12, 0)) {
            continue;
        }
        pcgte_op(TMD_GTE_AVSZ3);
        tmdLoadVector(0, normals + (e[1] >> 16));
        otz = pcgte_read_data(7);
        pcgte_op(TMD_GTE_NCCS);
        *(u32*)packet = tmdLinkPacket(ws, packet, otz, TMD_GT3_BYTES);
        lit = pcgte_read_data(22);
        TMD_COLOR(packet, 12, 0) = lit;
        TMD_COLOR(packet, 12, 1) = lit;
        TMD_COLOR(packet, 12, 2) = lit;
    }
    ws->primWrite = packet;
    return (u32*)element;
}

/// See tmdDrawStreamPrimGt3OneNormal in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamPrimGt3OneNormal(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawStreamPrimGt3OneNormalWalk(ws, elements, TMD_GT3_COLOR);
}

/// See tmdDrawStreamPrimGt3OneNormalSemiTrans in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamPrimGt3OneNormalSemiTrans(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawStreamPrimGt3OneNormalWalk(ws, elements, TMD_GT3_SEMI_TRANS_COLOR);
}

/// See tmdDrawStreamPrimGt3CornerColors in src/main/hasm/Tmd_StreamHandlers_Ops.s:
/// GT3 with a normal (word 1's high half, word 2's halves) and material
/// colour (words 3..5) per corner, lit one corner at a time.
u32* tmdDrawStreamPrimGt3CornerColors(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->primWrite;
    u8* verts   = (u8*)ws->verts;
    u8* normals = (u8*)ws->normals;
    u8  code    = TMD_CODE((objectFlags & TMD_OBJECT_SEMI_TRANS) ? TMD_GT3_SEMI_TRANS_COLOR : TMD_GT3_COLOR);
    u8* element = (u8*)elements;

    for (; count != 0; count--, packet += TMD_GT3_BYTES, element += stride) {
        const u32* e = (const u32*)element;
        u32 otz;
        if (!tmdProjectTriangle(e, verts, packet, 12, 0)) {
            continue;
        }
        pcgte_op(TMD_GTE_AVSZ3);
        pcgte_write_data(6, e[3]);
        tmdLoadVector(0, normals + (e[1] >> 16));
        pcgte_op(TMD_GTE_NCCS);
        TMD_COLOR(packet, 12, 0) = pcgte_read_data(22);
        pcgte_write_data(6, e[4]);
        tmdLoadVector(0, normals + (e[2] & 0xFFFF));
        packet[7] = code;
        pcgte_op(TMD_GTE_NCCS);
        TMD_COLOR(packet, 12, 1) = pcgte_read_data(22);
        pcgte_write_data(6, e[5]);
        tmdLoadVector(0, normals + (e[2] >> 16));
        otz = pcgte_read_data(7);
        pcgte_op(TMD_GTE_NCCS);
        TMD_COLOR(packet, 12, 2) = pcgte_read_data(22);
        *(u32*)packet = tmdLinkPacket(ws, packet, otz, TMD_GT3_BYTES);
    }
    ws->primWrite = packet;
    return (u32*)element;
}

/* ---- Quads ---- */

/// See tmdDrawStreamPrimG4CornerNormals in src/main/hasm/Tmd_StreamHandlers_Ops.s:
/// an untextured quad lit with one normal per corner (words 2 and 3) and the
/// element's material colour (word 4). Object flags are ignored.
u32* tmdDrawStreamPrimG4CornerNormals(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->primWrite;
    u8* verts   = (u8*)ws->verts;
    u8* normals = (u8*)ws->normals;
    u8* element = (u8*)elements;
    TmdQuadReuse reuse;

    tmdQuadReuseInit(&reuse, elements);
    for (; count != 0; count--, packet += TMD_G4_BYTES, element += stride) {
        const u32* e = (const u32*)element;
        u32 otz;
        if (!tmdProjectQuad(e, verts, packet, 8, &reuse, 0)) {
            continue;
        }
        pcgte_op(TMD_GTE_AVSZ4);
        pcgte_write_data(6, e[4]);
        tmdLoadVector(0, normals + (e[2] & 0xFFFF));
        tmdLoadVector(1, normals + (e[2] >> 16));
        tmdLoadVector(2, normals + (e[3] & 0xFFFF));
        pcgte_op(TMD_GTE_NCCT);
        tmdLoadVector(0, normals + (e[3] >> 16));
        otz = pcgte_read_data(7);
        TMD_COLOR(packet, 8, 0) = pcgte_read_data(20);
        TMD_COLOR(packet, 8, 1) = pcgte_read_data(21);
        TMD_COLOR(packet, 8, 2) = pcgte_read_data(22);
        pcgte_op(TMD_GTE_NCCS);
        *(u32*)packet          = tmdLinkPacket(ws, packet, otz, TMD_G4_BYTES);
        TMD_COLOR(packet, 8, 3) = pcgte_read_data(22);
    }
    ws->primWrite = packet;
    return (u32*)element;
}

/// See tmdDrawStreamPrimG4 in src/main/hasm/Tmd_StreamHandlers_Ops.s: the
/// quad with one face normal (word 2, a full-word offset) and material
/// colour (word 3), the lit colour repeated at each corner.
u32* tmdDrawStreamPrimG4(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->primWrite;
    u8* verts   = (u8*)ws->verts;
    u8* normals = (u8*)ws->normals;
    u8* element = (u8*)elements;
    TmdQuadReuse reuse;

    tmdQuadReuseInit(&reuse, elements);
    for (; count != 0; count--, packet += TMD_G4_BYTES, element += stride) {
        const u32* e = (const u32*)element;
        u32 lit;
        if (!tmdProjectQuad(e, verts, packet, 8, &reuse, 0)) {
            continue;
        }
        pcgte_op(TMD_GTE_AVSZ4);
        pcgte_write_data(6, e[3]);
        tmdLoadVector(0, normals + e[2]);
        pcgte_op(TMD_GTE_NCCS);
        *(u32*)packet = tmdLinkPacket(ws, packet, pcgte_read_data(7), TMD_G4_BYTES);
        lit = pcgte_read_data(22);
        TMD_COLOR(packet, 8, 0) = lit;
        TMD_COLOR(packet, 8, 1) = lit;
        TMD_COLOR(packet, 8, 2) = lit;
        TMD_COLOR(packet, 8, 3) = lit;
    }
    ws->primWrite = packet;
    return (u32*)element;
}

/* GT4 with one normal per corner (words 2 and 3) and a fixed command word
 * and colour. */
static u32* tmdDrawStreamGt4Walk(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements, u32 color)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->primWrite;
    u8* verts   = (u8*)ws->verts;
    u8* normals = (u8*)ws->normals;
    s32 mode    = TMD_QUAD_RELOAD_XY | ((objectFlags & TMD_OBJECT_REVERSE_CULLING) ? TMD_QUAD_REVERSE : 0);
    u8* element = (u8*)elements;
    TmdQuadReuse reuse;

    tmdQuadReuseInit(&reuse, elements);
    pcgte_write_data(6, color);
    for (; count != 0; count--, packet += TMD_GT4_BYTES, element += stride) {
        const u32* e = (const u32*)element;
        u32 otz;
        if (!tmdProjectQuad(e, verts, packet, 12, &reuse, mode)) {
            continue;
        }
        pcgte_op(TMD_GTE_AVSZ4);
        tmdLoadVector(0, normals + (e[2] & 0xFFFF));
        tmdLoadVector(1, normals + (e[2] >> 16));
        tmdLoadVector(2, normals + (e[3] & 0xFFFF));
        pcgte_op(TMD_GTE_NCCT);
        tmdLoadVector(0, normals + (e[3] >> 16));
        otz = pcgte_read_data(7);
        TMD_COLOR(packet, 12, 0) = pcgte_read_data(20);
        TMD_COLOR(packet, 12, 1) = pcgte_read_data(21);
        TMD_COLOR(packet, 12, 2) = pcgte_read_data(22);
        pcgte_op(TMD_GTE_NCCS);
        *(u32*)packet           = tmdLinkPacket(ws, packet, otz, TMD_GT4_BYTES);
        TMD_COLOR(packet, 12, 3) = pcgte_read_data(22);
    }
    ws->primWrite = packet;
    return (u32*)element;
}

/// See tmdDrawStreamGt4 in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamGt4(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawStreamGt4Walk(ws, objectFlags, elements,
                                (objectFlags & TMD_OBJECT_SEMI_TRANS) ? TMD_GT4_SEMI_TRANS_COLOR : TMD_GT4_COLOR);
}

/// See tmdDrawStreamGt4SemiTrans in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamGt4SemiTrans(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawStreamGt4Walk(ws, objectFlags, elements, TMD_GT4_SEMI_TRANS_COLOR);
}

/* GT4 with one face normal (word 2, a full-word offset), a fixed command
 * word and colour; object flags are ignored. */
static u32* tmdDrawStreamPrimGt4OneNormalWalk(TmdStreamWorkspace* ws, u32* elements, u32 color)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->primWrite;
    u8* verts   = (u8*)ws->verts;
    u8* normals = (u8*)ws->normals;
    u8* element = (u8*)elements;
    TmdQuadReuse reuse;

    tmdQuadReuseInit(&reuse, elements);
    pcgte_write_data(6, color);
    for (; count != 0; count--, packet += TMD_GT4_BYTES, element += stride) {
        const u32* e = (const u32*)element;
        u32 lit;
        if (!tmdProjectQuad(e, verts, packet, 12, &reuse, 0)) {
            continue;
        }
        pcgte_op(TMD_GTE_AVSZ4);
        tmdLoadVector(0, normals + e[2]);
        pcgte_op(TMD_GTE_NCCS);
        *(u32*)packet = tmdLinkPacket(ws, packet, pcgte_read_data(7), TMD_GT4_BYTES);
        lit = pcgte_read_data(22);
        TMD_COLOR(packet, 12, 0) = lit;
        TMD_COLOR(packet, 12, 1) = lit;
        TMD_COLOR(packet, 12, 2) = lit;
        TMD_COLOR(packet, 12, 3) = lit;
    }
    ws->primWrite = packet;
    return (u32*)element;
}

/// See tmdDrawStreamPrimGt4OneNormal in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamPrimGt4OneNormal(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawStreamPrimGt4OneNormalWalk(ws, elements, TMD_GT4_COLOR);
}

/// See tmdDrawStreamPrimGt4OneNormalSemiTrans in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamPrimGt4OneNormalSemiTrans(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawStreamPrimGt4OneNormalWalk(ws, elements, TMD_GT4_SEMI_TRANS_COLOR);
}

/// See tmdDrawStreamPrimGt4CornerColors in src/main/hasm/Tmd_StreamHandlers_Ops.s:
/// GT4 with a normal (words 2 and 3) and material colour (words 4..7) per
/// corner, lit one corner at a time.
u32* tmdDrawStreamPrimGt4CornerColors(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->primWrite;
    u8* verts   = (u8*)ws->verts;
    u8* normals = (u8*)ws->normals;
    u8  code    = TMD_CODE((objectFlags & TMD_OBJECT_SEMI_TRANS) ? TMD_GT4_SEMI_TRANS_COLOR : TMD_GT4_COLOR);
    u8* element = (u8*)elements;
    TmdQuadReuse reuse;

    tmdQuadReuseInit(&reuse, elements);
    for (; count != 0; count--, packet += TMD_GT4_BYTES, element += stride) {
        const u32* e = (const u32*)element;
        u32 otz;
        if (!tmdProjectQuad(e, verts, packet, 12, &reuse, TMD_QUAD_RELOAD_XY)) {
            continue;
        }
        pcgte_op(TMD_GTE_AVSZ4);
        pcgte_write_data(6, e[4]);
        tmdLoadVector(0, normals + (e[2] & 0xFFFF));
        pcgte_op(TMD_GTE_NCCS);
        TMD_COLOR(packet, 12, 0) = pcgte_read_data(22);
        pcgte_write_data(6, e[5]);
        tmdLoadVector(0, normals + (e[2] >> 16));
        otz = pcgte_read_data(7);
        pcgte_op(TMD_GTE_NCCS);
        TMD_COLOR(packet, 12, 1) = pcgte_read_data(22);
        pcgte_write_data(6, e[6]);
        tmdLoadVector(0, normals + (e[3] & 0xFFFF));
        pcgte_op(TMD_GTE_NCCS);
        TMD_COLOR(packet, 12, 2) = pcgte_read_data(22);
        pcgte_write_data(6, e[7]);
        tmdLoadVector(0, normals + (e[3] >> 16));
        pcgte_op(TMD_GTE_NCCS);
        *(u32*)packet           = tmdLinkPacket(ws, packet, otz, TMD_GT4_BYTES);
        TMD_COLOR(packet, 12, 3) = pcgte_read_data(22);
        packet[7] = code;
    }
    ws->primWrite = packet;
    return (u32*)element;
}

/* ---- Pre-transformed primitives ---- */

/* Depth-cache entry at a byte offset; negative marks a failed projection. */
#define TMD_CACHED_DEPTH(cache, offset) (*(s32*)((cache) + (offset)))

/* Pre-transformed triangles: the packets already hold XY and RGB from
 * tmdXformStreamVerts; the elements hold depth-cache offsets. Stamps the
 * command byte and links the packets that face the camera and whose
 * corners all projected. */
static u32* tmdDrawPreXformTriangles(TmdStreamWorkspace* ws, u32* elements, u8 code, u32 xyStride, u32 bytes,
                                     s32 reverse)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->preXformWrite;
    u8* depths  = (u8*)ws->szTable;
    u8* element = (u8*)elements;

    for (; count != 0; count--, packet += bytes, element += stride) {
        u32 w0, w1;
        s32 area, d0, d1, d2;
        pcgte_write_data(15, TMD_XY(packet, xyStride, 0));
        pcgte_write_data(15, TMD_XY(packet, xyStride, 1));
        pcgte_write_data(15, TMD_XY(packet, xyStride, 2));
        pcgte_op(TMD_GTE_NCLIP);
        w0   = ((u32*)element)[0];
        w1   = ((u32*)element)[1];
        area = (s32)pcgte_read_data(24);
        if (reverse ? area >= 0 : area <= 0) {
            continue;
        }
        d1 = TMD_CACHED_DEPTH(depths, w0 >> 16);
        if (d1 < 0) {
            continue;
        }
        d0 = TMD_CACHED_DEPTH(depths, w0 & 0xFFFF);
        if (d0 < 0) {
            continue;
        }
        d2 = TMD_CACHED_DEPTH(depths, w1 & 0xFFFF);
        /* The original's delay slots load SZ1 and SZ2 before testing d2. */
        pcgte_write_data(17, (u32)d0);
        pcgte_write_data(18, (u32)d1);
        if (d2 < 0) {
            continue;
        }
        pcgte_write_data(19, (u32)d2);
        pcgte_op(TMD_GTE_AVSZ3);
        packet[7]     = code;
        *(u32*)packet = tmdLinkPacket(ws, packet, pcgte_read_data(7), bytes);
    }
    ws->preXformWrite = packet;
    return (u32*)element;
}

/* Pre-transformed quads, as the triangles; facing is tested on corners
 * 0, 1, 2, then 1, 2, 3. */
static u32* tmdDrawPreXformQuads(TmdStreamWorkspace* ws, u32* elements, u8 code, u32 xyStride, u32 bytes, s32 reverse)
{
    u32 stride  = (u32)ws->elemStride << 2;
    s32 count   = ws->elemCount;
    u8* packet  = ws->preXformWrite;
    u8* depths  = (u8*)ws->szTable;
    u8* element = (u8*)elements;

    for (; count != 0; count--, packet += bytes, element += stride) {
        u32 w0, w1;
        s32 area, d0, d1, d2, d3;
        pcgte_write_data(15, TMD_XY(packet, xyStride, 0));
        pcgte_write_data(15, TMD_XY(packet, xyStride, 1));
        pcgte_write_data(15, TMD_XY(packet, xyStride, 2));
        pcgte_op(TMD_GTE_NCLIP);
        w0   = ((u32*)element)[0];
        w1   = ((u32*)element)[1];
        area = (s32)pcgte_read_data(24);
        if (reverse ? area >= 0 : area <= 0) {
            pcgte_write_data(15, TMD_XY(packet, xyStride, 3));
            pcgte_op(TMD_GTE_NCLIP);
            area = (s32)pcgte_read_data(24);
            if (reverse ? area <= 0 : area >= 0) {
                continue;
            }
        }
        d1 = TMD_CACHED_DEPTH(depths, w0 >> 16);
        if (d1 < 0) {
            continue;
        }
        d0 = TMD_CACHED_DEPTH(depths, w0 & 0xFFFF);
        if (d0 < 0) {
            continue;
        }
        d3 = TMD_CACHED_DEPTH(depths, w1 >> 16);
        if (d3 < 0) {
            continue;
        }
        d2 = TMD_CACHED_DEPTH(depths, w1 & 0xFFFF);
        /* The original's delay slots load SZ0 and SZ1 before testing d2. */
        pcgte_write_data(16, (u32)d0);
        pcgte_write_data(17, (u32)d1);
        if (d2 < 0) {
            continue;
        }
        pcgte_write_data(18, (u32)d2);
        pcgte_write_data(19, (u32)d3);
        pcgte_op(TMD_GTE_AVSZ4);
        packet[7]     = code;
        *(u32*)packet = tmdLinkPacket(ws, packet, pcgte_read_data(7), bytes);
    }
    ws->preXformWrite = packet;
    return (u32*)element;
}

/// See src/main/hasm/Tmd_StreamHandler_Prim32.s: pre-transformed untextured
/// triangles, opaque (code 0x30). Object flags are ignored.
u32* tmdDrawStreamPrimG3PreXform(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawPreXformTriangles(ws, elements, 0x30, 8, TMD_G3_BYTES, 0);
}

/// See src/main/hasm/Tmd_StreamHandler_Prim32.s: the blended entry (code 0x32).
u32* Tmd_StreamHandler_Prim32(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawPreXformTriangles(ws, elements, 0x32, 8, TMD_G3_BYTES, 0);
}

/// See src/main/hasm/Tmd_StreamHandler_Prim3A.s: pre-transformed untextured
/// quads, opaque (code 0x38). Object flags are ignored.
u32* tmdDrawStreamPrimG4PreXform(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawPreXformQuads(ws, elements, 0x38, 8, TMD_G4_BYTES, 0);
}

/// See src/main/hasm/Tmd_StreamHandler_Prim3A.s: the blended entry (code 0x3A).
u32* Tmd_StreamHandler_Prim3A(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawPreXformQuads(ws, elements, 0x3A, 8, TMD_G4_BYTES, 0);
}

/// See tmdDrawStreamPrimGt3PreXform in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamPrimGt3PreXform(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    u32 color = (objectFlags & TMD_OBJECT_SEMI_TRANS) ? TMD_GT3_SEMI_TRANS_COLOR : TMD_GT3_COLOR;
    return tmdDrawPreXformTriangles(ws, elements, TMD_CODE(color), 12, TMD_GT3_BYTES,
                                    objectFlags & TMD_OBJECT_REVERSE_CULLING);
}

/// See tmdDrawStreamPrimGt3PreXformSemiTrans in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamPrimGt3PreXformSemiTrans(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawPreXformTriangles(ws, elements, TMD_CODE(TMD_GT3_SEMI_TRANS_COLOR), 12, TMD_GT3_BYTES,
                                    objectFlags & TMD_OBJECT_REVERSE_CULLING);
}

/// See tmdDrawStreamPrimGt4PreXform in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamPrimGt4PreXform(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    u32 color = (objectFlags & TMD_OBJECT_SEMI_TRANS) ? TMD_GT4_SEMI_TRANS_COLOR : TMD_GT4_COLOR;
    return tmdDrawPreXformQuads(ws, elements, TMD_CODE(color), 12, TMD_GT4_BYTES,
                                objectFlags & TMD_OBJECT_REVERSE_CULLING);
}

/// See tmdDrawStreamPrimGt4PreXformSemiTrans in src/main/hasm/Tmd_StreamHandlers_Ops.s.
u32* tmdDrawStreamPrimGt4PreXformSemiTrans(TmdStreamWorkspace* ws, s32 objectFlags, u32* elements)
{
    return tmdDrawPreXformQuads(ws, elements, TMD_CODE(TMD_GT4_SEMI_TRANS_COLOR), 12, TMD_GT4_BYTES,
                                objectFlags & TMD_OBJECT_REVERSE_CULLING);
}

#endif

