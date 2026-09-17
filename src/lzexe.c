/* LZEXE 0.91 unpacking, done in pfemu's DOS loader.
 *
 * The 1993 five-minute demo ships both of its programs compressed with LZEXE
 * 0.91 (DEMO.PRG, PLAND.PRG - "LZ91" at offset 1Ch).  A self-extracting EXE
 * is not in itself a problem for an emulator: the decompressor is ordinary
 * 8086 code and would run perfectly well as the guest's first instructions.
 * The problem is what pfemu does *between* reading a program off disk and
 * running it.  Every Fantasies fix in src/fantasies.c - the pause-race fix,
 * the flipper/spring state fixes, the ball-gap measurement, the trainer's
 * two hotkeys - locates its target by scanning the freshly loaded image for
 * a byte signature.  Against a packed image those scans match nothing, so a
 * demo left to unpack itself would run with every one of those fixes
 * silently off, and the same six signatures that are unique and correct in
 * TABLE1.PRG are unique and correct in PLAND.PRG once it is unpacked.
 *
 * So the loader unpacks it, and the code below is a transcription of the
 * stub in the file rather than a general-purpose LZEXE tool: the bit pump,
 * the two match encodings and the relocation walker are read straight out of
 * DEMO.PRG's own decompressor (entry CS:000Eh; bit pump at 0069h, short
 * match at 0078h, long match at 00A8h, relocation table at stub offset
 * 0158h, walked by the loop at 0109h).
 *
 * Only LZEXE is unpacked here.  The demo's sound drivers are PKLITE-packed
 * and are left strictly alone: pfemu has nothing to match inside a .SDR
 * (fantasies_patch_sdr is disabled - see src/fantasies.c), so those stubs
 * run as the guest's own code, which is the more faithful thing to do
 * whenever there is no reason not to.
 *
 * What the guest sees afterwards is what it would have seen anyway.  The
 * stub's whole job is to leave the original image at the load segment with
 * its relocations applied, SS:SP and CS:IP set from the seven-word block it
 * carries, and the process's memory block unchanged - so the reconstructed
 * header below asks for exactly the paragraph count the packed header asked
 * for (see minalloc, at the end of lzexe_load), and the program starts with
 * the same register set src/dos.c gives every other EXE.
 *
 * Every failure path here returns non-zero and nothing else: src/dos.c then
 * loads the packed file exactly as before and the stub runs after all.  That
 * is deliberate - this file can only cost the image patches, never the boot.
 * `-nolzexe` forces that path for A/B testing.
 */
#include "pfemu.h"

int dos_no_lzexe = 0;            /* -nolzexe : never unpack, let the stub run */

/* A packed program is a real-mode image, so both of these are far above
 * anything a legitimate one can reach; they exist to bound a malformed file,
 * not to express a real limit. */
#define LZ_MAX_FILE   0x100000u  /* 1 MB of packed file */
#define LZ_MAX_RELOC  8192u

/* 1 when this 32-byte EXE header is LZEXE 0.91's.  Every field checked is one
 * the packer fixes: no relocations of its own, a bare two-paragraph header,
 * and the signature where the relocation table would otherwise start. */
int lzexe_detect(const uint8_t *hdr){
    uint16_t crlc    = (uint16_t)(hdr[6]  | (hdr[7]  << 8));
    uint16_t cparhdr = (uint16_t)(hdr[8]  | (hdr[9]  << 8));
    uint16_t lfarlc  = (uint16_t)(hdr[24] | (hdr[25] << 8));
    return hdr[0] == 'M' && hdr[1] == 'Z' && crlc == 0 && cparhdr == 2 &&
           lfarlc == 0x1C && !memcmp(hdr + 0x1C, "LZ91", 4);
}

void lzexe_free(LzexeImage *im){
    if(!im) return;
    free(im->image);
    free(im->rel);
    im->image = NULL;
    im->rel = NULL;
}

/* The decompressor proper.  One flat output buffer stands in for the guest's
 * ES:DI, which is why the stub's periodic segment-normalisation record (the
 * `01` escape) is a no-op here: it only ever re-expressed the same linear
 * address as a different segment:offset pair.  Back-references reach at most
 * 8192 bytes, so a flat window is exact, not an approximation. */
static int lz_decompress(const uint8_t *src, uint32_t srclen,
                         uint8_t *dst, uint32_t dstmax, uint32_t *outlen){
    uint32_t si = 0, di = 0;
    uint16_t bitbuf;
    int nbits;

    if(srclen < 2) return -1;
    bitbuf = (uint16_t)(src[0] | (src[1] << 8));
    si = 2;
    nbits = 16;

    /* The stub takes a bit, then refills - so the bit just taken is still the
     * old word's, and a refill that runs off the end is a truncated file. */
#define LZ_BIT(out) do {                                         \
        (out) = bitbuf & 1u;                                     \
        bitbuf = (uint16_t)(bitbuf >> 1);                        \
        if(--nbits == 0){                                        \
            if(si + 2 > srclen) return -1;                       \
            bitbuf = (uint16_t)(src[si] | (src[si + 1] << 8));   \
            si += 2; nbits = 16;                                 \
        }                                                        \
    } while(0)
#define LZ_BYTE(out) do {                                        \
        if(si >= srclen) return -1;                              \
        (out) = src[si++];                                       \
    } while(0)

    for(;;){
        uint32_t bit, len, k;
        int32_t off;
        LZ_BIT(bit);
        if(bit){                                  /* 1: one literal byte */
            uint32_t v;
            LZ_BYTE(v);
            if(di >= dstmax) return -1;
            dst[di++] = (uint8_t)v;
            continue;
        }
        LZ_BIT(bit);
        if(!bit){                                 /* 00: 2-bit len, 8-bit dist */
            uint32_t hi, lo, d;
            LZ_BIT(hi); LZ_BIT(lo);
            len = ((hi << 1) | lo) + 2;
            LZ_BYTE(d);
            off = (int32_t)(d | 0xFF00u) - 0x10000;          /* -256..-1 */
        } else {                                  /* 01: one packed word */
            uint32_t w, lo, hi;
            LZ_BYTE(lo); LZ_BYTE(hi);
            w = lo | (hi << 8);
            off = (int32_t)((((0xE0u | (w >> 11)) & 0xFFu) << 8) | (w & 0xFFu))
                  - 0x10000;                                   /* -8192..-1 */
            len = (w >> 8) & 7u;
            if(len){
                len += 2;
            } else {                              /* ...plus a length byte */
                uint32_t e;
                LZ_BYTE(e);
                if(e == 0) break;                 /* end of stream */
                if(e == 1) continue;              /* segment normalisation */
                len = e + 1;
            }
        }
        if((int32_t)di + off < 0) return -1;
        if(di + len > dstmax) return -1;
        /* Byte at a time and forwards on purpose: len may exceed -off, and
         * the overlap is what encodes a run. */
        for(k = 0; k < len; k++, di++)
            dst[di] = dst[(uint32_t)((int32_t)di + off)];
    }
#undef LZ_BIT
#undef LZ_BYTE
    *outlen = di;
    return 0;
}

/* The packed relocation table, from `at` to its terminator.  A span of 0
 * escapes to a word: 0 advances the segment by 0FFFh paragraphs, 1 ends the
 * table, anything else is a 16-bit span.  Offsets are re-normalised into
 * segment:offset the same way the stub does it, so the pairs handed back are
 * already in the shape an ordinary EXE relocation table would have held. */
static int lz_relocs(const uint8_t *f, uint32_t flen, uint32_t at,
                     uint32_t *rel, uint32_t maxrel, uint32_t *nrel){
    uint32_t n = 0, seg = 0, off = 0;
    for(;;){
        uint32_t span;
        if(at >= flen) return -1;
        span = f[at++];
        if(span == 0){
            uint32_t w;
            if(at + 2 > flen) return -1;
            w = (uint32_t)(f[at] | (f[at + 1] << 8));
            at += 2;
            if(w == 0){ seg = (seg + 0x0FFFu) & 0xFFFFu; continue; }
            if(w == 1) break;
            span = w;
        }
        off = (off + span) & 0xFFFFu;
        seg = (seg + (off >> 4)) & 0xFFFFu;
        off &= 0x000Fu;
        if(n >= maxrel) return -1;
        rel[n++] = (seg << 16) | off;
    }
    *nrel = n;
    return 0;
}

/* Unpack `f` (already known to be LZEXE 0.91 by lzexe_detect).  On success
 * *out owns two allocations; the caller frees them with lzexe_free. */
int lzexe_load(FILE *f, long fsize, LzexeImage *out){
    uint8_t *file = NULL;
    uint16_t cblp, cp, cparhdr, minalloc, maxalloc, cs;
    uint32_t hdrsize, imgend, packlen, packpara, outpara, total, cap, stub;
    uint16_t info[7];
    int i;

    memset(out, 0, sizeof(*out));
    if(fsize < 64 || (unsigned long)fsize > LZ_MAX_FILE) return -1;
    file = (uint8_t*)malloc((size_t)fsize);
    if(!file) return -1;
    if(fseek(f, 0, SEEK_SET) != 0 ||
       fread(file, 1, (size_t)fsize, f) != (size_t)fsize) goto fail;

    cblp     = (uint16_t)(file[2]  | (file[3]  << 8));
    cp       = (uint16_t)(file[4]  | (file[5]  << 8));
    cparhdr  = (uint16_t)(file[8]  | (file[9]  << 8));
    minalloc = (uint16_t)(file[10] | (file[11] << 8));
    maxalloc = (uint16_t)(file[12] | (file[13] << 8));
    cs       = (uint16_t)(file[22] | (file[23] << 8));

    hdrsize = (uint32_t)cparhdr * 16;
    imgend  = cblp ? ((uint32_t)(cp - 1) * 512 + cblp) : ((uint32_t)cp * 512);
    if(imgend > (uint32_t)fsize || imgend <= hdrsize) goto fail;
    packlen  = imgend - hdrsize;
    packpara = (packlen + 15) / 16;

    /* The stub sits at the end of the packed image and CS is how far in, so
     * the compressed stream is exactly the CS*16 bytes before it.  The stub
     * carries that same figure in its own seven-word block; disagreeing with
     * itself means this is not the layout being decoded here. */
    stub = hdrsize + (uint32_t)cs * 16;
    if(stub + 14 > (uint32_t)fsize || (uint32_t)cs * 16 > packlen) goto fail;
    for(i = 0; i < 7; i++)
        info[i] = (uint16_t)(file[stub + i*2] | (file[stub + i*2 + 1] << 8));
    if(info[4] != cs) goto fail;

    /* The packed header's own memory demand is the ceiling on the unpacked
     * image: the stub has to decompress inside that block, so an image that
     * would not fit in it cannot be the one this file encodes. */
    total = packpara + minalloc;
    cap = total * 16;
    out->image = (uint8_t*)malloc(cap ? cap : 1);
    if(!out->image) goto fail;
    if(lz_decompress(file + hdrsize, (uint32_t)cs * 16,
                     out->image, cap, &out->imglen) != 0) goto fail;

    out->ip = info[0]; out->cs = info[1];
    out->sp = info[2]; out->ss = info[3];
    if((uint32_t)out->cs * 16 + out->ip >= out->imglen) goto fail;

    out->rel = (uint32_t*)malloc(LZ_MAX_RELOC * sizeof(uint32_t));
    if(!out->rel) goto fail;
    if(lz_relocs(file, (uint32_t)fsize, stub + 0x158,
                 out->rel, LZ_MAX_RELOC, &out->nrel) != 0) goto fail;
    for(i = 0; (uint32_t)i < out->nrel; i++){
        uint32_t a = (out->rel[i] >> 16) * 16 + (out->rel[i] & 0xFFFFu);
        if(a + 2 > out->imglen) goto fail;
    }

    /* Ask DOS for the same total the packed file asked for, so the process
     * gets the identical memory block either way.  maxalloc is carried over
     * rather than reconstructed because LZEXE overwrites it with FFFFh and
     * the original value is simply not in the file - and FFFFh is what the
     * packed load would have used, which is the behaviour being preserved. */
    outpara = (out->imglen + 15) / 16;
    if(total < outpara || total - outpara > 0xFFFFu) goto fail;
    out->minalloc = (uint16_t)(total - outpara);
    out->maxalloc = maxalloc;

    free(file);
    trc("[lzexe] unpacked %u -> %u bytes, %u relocations, "
        "entry %04X:%04X ss:sp %04X:%04X minalloc %04X\n",
        packlen, out->imglen, out->nrel, out->cs, out->ip,
        out->ss, out->sp, out->minalloc);
    return 0;

fail:
    free(file);
    lzexe_free(out);
    return -1;
}
