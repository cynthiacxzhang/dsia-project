#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <curl/curl.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <arpa/inet.h>

#include "lab_png.h"
#include "crc.h"
#include "zutil.h"

#define TOTAL_STRIPS 50
#define MAX_IDAT_SIZE (1024 * 1024)
#define NUM_SERVERS 3

const char *servers[NUM_SERVERS] = {
    "http://ece252-1.uwaterloo.ca:2520/image?img=%d",
    "http://ece252-2.uwaterloo.ca:2520/image?img=%d",
    "http://ece252-3.uwaterloo.ca:2520/image?img=%d"};

typedef struct
{
    char *data;
    size_t size;
} recv_buf_t;

typedef struct
{
    recv_buf_t strip;
    int valid;
} strip_buf_t;

strip_buf_t strip_buf[TOTAL_STRIPS];
int in_progress[TOTAL_STRIPS] = {0};
atomic_int strip_count = 0;

pthread_mutex_t strip_lock = PTHREAD_MUTEX_INITIALIZER;

int image_number = 1;
int thread_count = 1;

size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    recv_buf_t *buf = (recv_buf_t *)userdata;
    char *tmp = realloc(buf->data, buf->size + total);
    if (!tmp)
        return 0;
    buf->data = tmp;
    memcpy(&(buf->data[buf->size]), ptr, total);
    buf->size += total;
    return total;
}

size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    int *strip_index = (int *)userdata;
    size_t total = size * nitems;
    if (sscanf(buffer, "X-Ece252-Fragment: %d", strip_index) == 1)
    {
        return total;
    }
    return total;
}

void *fetch_thread(void *arg)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;

    while (atomic_load(&strip_count) < TOTAL_STRIPS)
    {
        char url[256];
        int server = rand() % NUM_SERVERS;
        snprintf(url, sizeof(url), servers[server], image_number);

        recv_buf_t buf = {0};
        int strip_index = -1;

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &strip_index);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");

        struct timeval t1, t2;
        gettimeofday(&t1, NULL);
        CURLcode res = curl_easy_perform(curl);
        gettimeofday(&t2, NULL);
        double duration = (t2.tv_sec - t1.tv_sec) + (t2.tv_usec - t1.tv_usec) / 1e6;

        if (res != CURLE_OK || strip_index < 0 || strip_index >= TOTAL_STRIPS)
        {
            free(buf.data);
            continue;
        }

        pthread_mutex_lock(&strip_lock);
        if (strip_buf[strip_index].valid || in_progress[strip_index])
        {
            pthread_mutex_unlock(&strip_lock);
            free(buf.data);
            continue;
        }
        in_progress[strip_index] = 1;
        pthread_mutex_unlock(&strip_lock);

        strip_buf[strip_index].strip = buf;
        strip_buf[strip_index].valid = 1;
        atomic_fetch_add(&strip_count, 1);

        printf("Thread %ld fetched strip %d in %.2f sec\n",
               (long)pthread_self(), strip_index, duration);
    }

    curl_easy_cleanup(curl);
    return NULL;
}

int main(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "t:n:")) != -1)
    {
        switch (opt)
        {
        case 't':
            thread_count = atoi(optarg);
            break;
        case 'n':
            image_number = atoi(optarg);
            break;
        default:
            fprintf(stderr, "Usage: %s -t <threads> -n <image_number>\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (image_number < 1 || image_number > 3 || thread_count < 1)
    {
        fprintf(stderr, "Invalid arguments.\n");
        exit(EXIT_FAILURE);
    }

    curl_global_init(CURL_GLOBAL_ALL);

    pthread_t tids[thread_count];
    for (int i = 0; i < thread_count; i++)
    {
        pthread_create(&tids[i], NULL, fetch_thread, NULL);
    }

    for (int i = 0; i < thread_count; i++)
    {
        pthread_join(tids[i], NULL);
    }

    curl_global_cleanup();

    // PNG assembly
    U8 *inflated[TOTAL_STRIPS];
    U64 lengths[TOTAL_STRIPS];
    U32 width = 0, height = 0;

    for (int i = 0; i < TOTAL_STRIPS; ++i)
    {
        FILE *mem = fmemopen(strip_buf[i].strip.data, strip_buf[i].strip.size, "rb");
        if (!mem)
            exit(EXIT_FAILURE);

        U8 sig[PNG_SIG_SIZE];
        fread(sig, 1, PNG_SIG_SIZE, mem);
        struct data_IHDR ihdr;
        get_png_data_IHDR(&ihdr, mem, 0, SEEK_CUR);
        if (i == 0)
            width = ihdr.width;
        height += ihdr.height;

        fseek(mem, PNG_SIG_SIZE, SEEK_SET);
        simple_PNG_p img = mallocPNG();
        get_png_chunks(img, mem, 0, SEEK_CUR);

        inflated[i] = malloc(MAX_IDAT_SIZE);
        mem_inf(inflated[i], &lengths[i], img->p_IDAT->p_data, img->p_IDAT->length);

        free_png(img);
        fclose(mem);
        free(strip_buf[i].strip.data);
    }

    U32 row_bytes = width * 4 + 1;
    U64 full_size = row_bytes * height;
    U8 *merged = malloc(full_size);
    U8 *p = merged;
    for (int i = 0; i < TOTAL_STRIPS; i++)
    {
        memcpy(p, inflated[i], lengths[i]);
        p += lengths[i];
        free(inflated[i]);
    }

    U8 *comp_buf = malloc(MAX_IDAT_SIZE);
    U64 comp_len = 0;
    mem_def(comp_buf, &comp_len, merged, full_size, Z_DEFAULT_COMPRESSION);

    simple_PNG_p final = mallocPNG();

    final->p_IHDR = malloc(sizeof(struct chunk));
    final->p_IHDR->length = DATA_IHDR_SIZE;
    final->p_IHDR->p_data = malloc(DATA_IHDR_SIZE);
    memcpy(final->p_IHDR->type, "IHDR", 4);
    data_IHDR_p ihdr_data = (data_IHDR_p) final->p_IHDR->p_data;
    ihdr_data->width = htonl(width);
    ihdr_data->height = htonl(height);
    ihdr_data->bit_depth = 8;
    ihdr_data->color_type = 6;
    ihdr_data->compression = 0;
    ihdr_data->filter = 0;
    ihdr_data->interlace = 0;
    final->p_IHDR->crc = calculate_chunk_crc(final->p_IHDR);

    final->p_IDAT = malloc(sizeof(struct chunk));
    final->p_IDAT->length = comp_len;
    final->p_IDAT->p_data = malloc(comp_len);
    memcpy(final->p_IDAT->p_data, comp_buf, comp_len);
    memcpy(final->p_IDAT->type, "IDAT", 4);
    final->p_IDAT->crc = calculate_chunk_crc(final->p_IDAT);

    final->p_IEND = malloc(sizeof(struct chunk));
    final->p_IEND->length = 0;
    final->p_IEND->p_data = NULL;
    memcpy(final->p_IEND->type, "IEND", 4);
    final->p_IEND->crc = calculate_chunk_crc(final->p_IEND);

    write_PNG("all.png", final);
    free(comp_buf);
    free(merged);
    free_png(final);
    return 0;
}
