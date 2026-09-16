/* Minimal PNG writer, no zlib/libpng dependency.
 *
 * PNGs are written by hand using "stored" (uncompressed) deflate blocks -
 * legal per RFC 1951 and read by every PNG decoder, just bigger on disk
 * than a compressed file would be.
 */
#include "pfemu.h"

static uint32_t crc32_table[256];
static int crc32_ready = 0;
static uint32_t crc32_feed(uint32_t raw, const uint8_t *buf, size_t len){
    size_t i;
    if(!crc32_ready){
        int c_i, k;
        for(c_i=0;c_i<256;c_i++){
            uint32_t c = (uint32_t)c_i;
            for(k=0;k<8;k++) c = (c & 1) ? (0xEDB88320u ^ (c>>1)) : (c>>1);
            crc32_table[c_i] = c;
        }
        crc32_ready = 1;
    }
    for(i=0;i<len;i++) raw = crc32_table[(raw ^ buf[i]) & 0xFF] ^ (raw>>8);
    return raw;
}

static uint32_t adler32_buf(const uint8_t *buf, size_t len){
    uint32_t a = 1, b = 0;
    size_t i;
    for(i=0;i<len;i++){ a = (a + buf[i]) % 65521u; b = (b + a) % 65521u; }
    return (b<<16) | a;
}

static void put_be32(uint8_t *p, uint32_t v){
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}

static void write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len){
    uint8_t hdr[8], crcbuf[4];
    uint32_t crc = 0xFFFFFFFFu;
    put_be32(hdr, len);
    memcpy(hdr+4, type, 4);
    fwrite(hdr, 1, 8, f);
    if(len) fwrite(data, 1, len, f);
    crc = crc32_feed(crc, (const uint8_t*)type, 4);
    if(len) crc = crc32_feed(crc, data, len);
    put_be32(crcbuf, crc ^ 0xFFFFFFFFu);
    fwrite(crcbuf, 1, 4, f);
}

/* pix is w*h pixels, row-major, each 0x00RRGGBB. */
int save_png(const char *path, const uint32_t *pix, int w, int h){
    size_t rawlen = (size_t)h * (1 + (size_t)w*3);
    size_t nblocks = rawlen ? (rawlen + 65534)/65535 : 1;
    size_t idat_len = 2 + rawlen + 5*nblocks + 4;
    uint8_t *raw, *idat, *rp, *dp;
    uint8_t ihdr[13];
    FILE *f;
    int x, y;
    size_t off;

    raw = (uint8_t*)malloc(rawlen ? rawlen : 1);
    idat = (uint8_t*)malloc(idat_len);
    if(!raw || !idat){ free(raw); free(idat); return 0; }

    rp = raw;
    for(y=0;y<h;y++){
        *rp++ = 0; /* filter: None */
        for(x=0;x<w;x++){
            uint32_t p = pix[(size_t)y*w+x];
            *rp++ = (uint8_t)(p>>16);
            *rp++ = (uint8_t)(p>>8);
            *rp++ = (uint8_t)p;
        }
    }

    dp = idat;
    *dp++ = 0x78; *dp++ = 0x01; /* zlib header: deflate, 32K window, level 0 */
    off = 0;
    do {
        size_t chunk = rawlen - off; if(chunk > 65535) chunk = 65535;
        uint16_t chunk16 = (uint16_t)chunk, nlen16 = (uint16_t)~chunk16;
        int final = (off + chunk >= rawlen);
        *dp++ = final ? 1 : 0; /* BFINAL + BTYPE=00 (stored), byte-aligned */
        *dp++ = (uint8_t)(chunk16 & 0xFF); *dp++ = (uint8_t)(chunk16 >> 8);
        *dp++ = (uint8_t)(nlen16 & 0xFF);  *dp++ = (uint8_t)(nlen16 >> 8);
        if(chunk){ memcpy(dp, raw+off, chunk); dp += chunk; }
        off += chunk;
    } while(off < rawlen);
    put_be32(dp, adler32_buf(raw, rawlen)); dp += 4;
    free(raw);

    put_be32(ihdr+0, (uint32_t)w);
    put_be32(ihdr+4, (uint32_t)h);
    ihdr[8]=8; ihdr[9]=2; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0; /* 8-bit RGB, no interlace */

    f = fopen(path, "wb");
    if(!f){ free(idat); return 0; }
    { static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
      fwrite(sig,1,8,f); }
    write_chunk(f, "IHDR", ihdr, 13);
    write_chunk(f, "IDAT", idat, (uint32_t)idat_len);
    write_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(idat);
    return 1;
}
