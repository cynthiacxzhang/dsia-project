// Completing the declared helper functions from the lab_png.h file
// Citation: ChatGPT for function structure and C syntax.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "lab_png.h"
#include "crc.h"

int is_png(U8 *buf, size_t n)
{
    const U8 png_sig[PNG_SIG_SIZE] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (n < PNG_SIG_SIZE)
        return 0;
    return memcmp(buf, png_sig, PNG_SIG_SIZE) == 0;
}

int get_png_data_IHDR(struct data_IHDR *out, FILE *fp, long offset, int whence)
{
    fseek(fp, offset, whence);

    U32 length;
    if (fread(&length, 1, 4, fp) != 4)
        return -1;

    char type[5] = {0};
    if (fread(type, 1, 4, fp) != 4 || strncmp(type, "IHDR", 4) != 0)
        return -1;

    if (fread(out, 1, DATA_IHDR_SIZE, fp) != DATA_IHDR_SIZE)
        return -1;

    out->width = ntohl(out->width);
    out->height = ntohl(out->height);
    return 0;
}

chunk_p get_chunk(FILE *fp)
{
    chunk_p ch = malloc(sizeof(struct chunk));
    if (!ch)
        return NULL;

    U32 length_be;
    if (fread(&length_be, 1, 4, fp) != 4)
        return NULL;
    ch->length = ntohl(length_be);

    if (fread(ch->type, 1, 4, fp) != 4)
        return NULL;

    ch->p_data = malloc(ch->length);
    if (!ch->p_data || fread(ch->p_data, 1, ch->length, fp) != ch->length)
        return NULL;

    U32 crc_be;
    if (fread(&crc_be, 1, 4, fp) != 4)
        return NULL;
    ch->crc = ntohl(crc_be);

    return ch;
}

U32 get_chunk_crc(chunk_p in)
{
    return in->crc;
}

U32 calculate_chunk_crc(chunk_p in)
{
    int total_len = CHUNK_TYPE_SIZE + in->length;
    U8 *buf = malloc(total_len);
    memcpy(buf, in->type, CHUNK_TYPE_SIZE);
    memcpy(buf + CHUNK_TYPE_SIZE, in->p_data, in->length);
    U32 crc_val = crc(buf, total_len);
    free(buf);
    return crc_val;
}

void free_chunk(chunk_p in)
{
    if (in)
    {
        if (in->p_data)
            free(in->p_data);
        free(in);
    }
}

simple_PNG_p mallocPNG()
{
    simple_PNG_p png = malloc(sizeof(struct simple_PNG));
    png->p_IHDR = NULL;
    png->p_IDAT = NULL;
    png->p_IEND = NULL;
    return png;
}

void free_png(simple_PNG_p in)
{
    if (in)
    {
        if (in->p_IHDR)
            free_chunk(in->p_IHDR);
        if (in->p_IDAT)
            free_chunk(in->p_IDAT);
        if (in->p_IEND)
            free_chunk(in->p_IEND);
        free(in);
    }
}

int get_png_chunks(simple_PNG_p out, FILE *fp, long offset, int whence)
{
    fseek(fp, offset, whence);
    chunk_p chunk;
    while ((chunk = get_chunk(fp)) != NULL)
    {
        if (memcmp(chunk->type, "IHDR", 4) == 0)
        {
            out->p_IHDR = chunk;
        }
        else if (memcmp(chunk->type, "IDAT", 4) == 0)
        {
            out->p_IDAT = chunk;
        }
        else if (memcmp(chunk->type, "IEND", 4) == 0)
        {
            out->p_IEND = chunk;
            break;
        }
        else
        {
            free_chunk(chunk); // ignore unknown chunks
        }
    }
    return (out->p_IHDR && out->p_IDAT && out->p_IEND) ? 0 : -1;
}

int write_chunk(FILE *fp, chunk_p in)
{
    if (!in || !fp)
        return -1;
    U32 length_be = htonl(in->length);
    U32 crc_be = htonl(in->crc);
    fwrite(&length_be, 1, 4, fp);
    fwrite(in->type, 1, 4, fp);
    if (in->length > 0 && in->p_data)
        fwrite(in->p_data, 1, in->length, fp);
    fwrite(&crc_be, 1, 4, fp);
    return 0;
}

int write_PNG(char *filepath, simple_PNG_p in)
{
    if (!in)
        return -1;
    FILE *fp = fopen(filepath, "wb");
    if (!fp)
        return -1;

    // Write PNG signature
    const U8 png_sig[PNG_SIG_SIZE] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(png_sig, 1, PNG_SIG_SIZE, fp);

    // Write IHDR, IDAT, IEND chunks
    write_chunk(fp, in->p_IHDR);
    write_chunk(fp, in->p_IDAT);
    write_chunk(fp, in->p_IEND);

    fclose(fp);
    return 0;
}
