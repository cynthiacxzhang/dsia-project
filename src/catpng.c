// Concatenate multiple PNG files into a single PNG file by stacking them vertically.
// Citation: ChatGPT for code structure and C syntax.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "lab_png.h"
#include "crc.h"
#include "zutil.h"

#define MAX_IMAGES 128
#define MAX_IDAT_SIZE (1024 * 1024) // 1MB buffer for compressed/decompressed data

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <png1> <png2> ...\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Arrays to hold extracted image metadata and data buffers
    U8 *inflated_buffers[MAX_IMAGES];
    U64 inflated_lengths[MAX_IMAGES];
    U32 widths[MAX_IMAGES];
    U32 heights[MAX_IMAGES];
    int valid_png_count = 0;
    U32 common_width = 0;
    U32 total_height = 0;

    // Iterate through each input file path
    for (int i = 1; i < argc && valid_png_count < MAX_IMAGES; i++)
    {
        FILE *fp = fopen(argv[i], "rb");
        if (!fp)
        {
            fprintf(stderr, "Could not open file: %s\n", argv[i]);
            continue;
        }

        // Validate PNG signature using ispng from pnginfo
        U8 sig[PNG_SIG_SIZE];
        if (fread(sig, 1, PNG_SIG_SIZE, fp) != PNG_SIG_SIZE || !is_png(sig, PNG_SIG_SIZE))
        {
            fprintf(stderr, "%s: not a valid PNG\n", argv[i]);
            fclose(fp);
            continue;
        }

        // Extract IHDR info: width and height using get_png_data_IHDR from pnginfo
        struct data_IHDR ihdr;
        if (get_png_data_IHDR(&ihdr, fp, 0, SEEK_CUR) != 0)
        {
            fprintf(stderr, "%s: failed to read IHDR\n", argv[i]);
            fclose(fp);
            continue;
        }

        // Ensure width matches across all images
        if (valid_png_count == 0)
        {
            common_width = ihdr.width;
        }
        else if (ihdr.width != common_width)
        {
            fprintf(stderr, "%s: width mismatch\n", argv[i]);
            fclose(fp);
            continue;
        }

        heights[valid_png_count] = ihdr.height;
        widths[valid_png_count] = ihdr.width;

        // Extract PNG chunks and find IDAT for decompression
        fseek(fp, PNG_SIG_SIZE, SEEK_SET);
        simple_PNG_p img = mallocPNG();
        if (get_png_chunks(img, fp, 0, SEEK_CUR) != 0 || !img->p_IDAT)
        {
            fprintf(stderr, "%s: failed to extract IDAT\n", argv[i]);
            free_png(img);
            fclose(fp);
            continue;
        }

        // Decompress IDAT data
        U8 *inf_buf = malloc(MAX_IDAT_SIZE);
        if (!inf_buf)
        {
            fprintf(stderr, "%s: failed to allocate inflation buffer\n", argv[i]);
            free_png(img);
            fclose(fp);
            continue;
        }

        U64 inf_len = 0;
        if (mem_inf(inf_buf, &inf_len, img->p_IDAT->p_data, img->p_IDAT->length) != 0)
        {
            fprintf(stderr, "%s: inflation failed\n", argv[i]);
            free(inf_buf);
            free_png(img);
            fclose(fp);
            continue;
        }

        // Store for later merging of all images (all.png)
        inflated_buffers[valid_png_count] = inf_buf;
        inflated_lengths[valid_png_count] = inf_len;
        total_height += ihdr.height;

        free_png(img);
        fclose(fp);
        valid_png_count++;
    }

    if (valid_png_count == 0)
    {
        fprintf(stderr, "No valid PNGs given.\n");
        return EXIT_FAILURE;
    }

    // Calculate buffer size for all stacked image pixel data (each row has width*4 + 1 bytes)
    U32 row_bytes = common_width * 4 + 1;
    U64 full_size = row_bytes * total_height;
    U8 *merged = malloc(full_size);
    U8 *p = merged;

    // Merge all inflated data into that one large uncompressed buffer
    for (int i = 0; i < valid_png_count; i++)
    {
        memcpy(p, inflated_buffers[i], inflated_lengths[i]);
        p += inflated_lengths[i];
        free(inflated_buffers[i]);
    }

    // Compress the combined buffer using zutil
    U8 *comp_buf = malloc(MAX_IDAT_SIZE);
    U64 comp_len = 0;
    if (mem_def(comp_buf, &comp_len, merged, full_size, Z_DEFAULT_COMPRESSION) != 0)
    {
        fprintf(stderr, "Compression failed.\n");
        free(comp_buf);
        free(merged);
        return EXIT_FAILURE;
    }

    // Build final PNG all.png
    simple_PNG_p final = mallocPNG();

    // Allocate IHDR chunk and fill with information
    final->p_IHDR = malloc(sizeof(struct chunk));
    final->p_IHDR->length = DATA_IHDR_SIZE;
    final->p_IHDR->p_data = malloc(DATA_IHDR_SIZE);
    memcpy(final->p_IHDR->type, "IHDR", 4);
    data_IHDR_p ihdr_data = (data_IHDR_p) final->p_IHDR->p_data;
    ihdr_data->width = htonl(common_width);
    ihdr_data->height = htonl(total_height);
    ihdr_data->bit_depth = 8;
    ihdr_data->color_type = 6; // RGBA
    ihdr_data->compression = 0;
    ihdr_data->filter = 0;
    ihdr_data->interlace = 0;
    final->p_IHDR->crc = calculate_chunk_crc(final->p_IHDR);

    // Allocate and populate IDAT chunk
    final->p_IDAT = malloc(sizeof(struct chunk));
    final->p_IDAT->length = comp_len;
    final->p_IDAT->p_data = malloc(comp_len);
    memcpy(final->p_IDAT->p_data, comp_buf, comp_len);
    memcpy(final->p_IDAT->type, "IDAT", 4);
    final->p_IDAT->crc = calculate_chunk_crc(final->p_IDAT);

    // Allocate and populate IEND chunk
    final->p_IEND = malloc(sizeof(struct chunk));
    final->p_IEND->length = 0;
    final->p_IEND->p_data = NULL;
    memcpy(final->p_IEND->type, "IEND", 4);
    final->p_IEND->crc = calculate_chunk_crc(final->p_IEND);

    // Write final PNG file
    write_PNG("all.png", final);

    // Clean up memory
    free(merged);
    free(comp_buf);
    free_png(final);
    return EXIT_SUCCESS;
}
