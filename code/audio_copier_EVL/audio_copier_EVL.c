#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sched.h>

#include <evl/evl.h>
#include <evl/thread.h>
#include <evl/timer.h>
#include <evl/clock.h>
#include <evl/proxy.h>

#include "de1soc.h"

#define AUD_CTRLS_SIZE  0xB0

#define AUD_CORE_CTRL   0x00
#define AUD_CORE_FIFO   0x04
#define AUD_CORE_LEFT   0x08
#define AUD_CORE_RIGHT  0x0C

#define AUD_CORE_WR_LEFT_BITS   24
#define AUD_CORE_WR_RIGHT_BITS  16
#define AUD_CORE_RD_LEFT_BITS   8
#define AUD_CORE_RD_RIGHT_BITS  0

#define FIFO_RD_LEFT(fifo)      (((fifo) >> AUD_CORE_RD_LEFT_BITS) & 0xFF)
#define FIFO_RD_RIGHT(fifo)     (((fifo) >> AUD_CORE_RD_RIGHT_BITS) & 0xFF)
#define FIFO_WR_LEFT(fifo)      (((fifo) >> AUD_CORE_WR_LEFT_BITS) & 0xFF)
#define FIFO_WR_RIGHT(fifo)     (((fifo) >> AUD_CORE_WR_RIGHT_BITS) & 0xFF)

#define AUDIO_POLL_NS 10000L

static volatile sig_atomic_t running = 1;

static int audio_fd = -1;
static void *audio_map = NULL;
static volatile uint8_t *audio = NULL;

static inline uint32_t audio_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(audio + offset);
}

static inline void audio_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(audio + offset) = value;
}

static void sigint_handler(int signum __attribute__((unused)))
{
    running = 0;
}

static int init_audio(void)
{
    audio_fd = open(AUDIO_FILE, O_RDWR);
    if (audio_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", AUDIO_FILE, strerror(errno));
        return -1;
    }

    audio_map = mmap(NULL, AUD_CTRLS_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     audio_fd,
                     0);

    if (audio_map == MAP_FAILED) {
        fprintf(stderr, "mmap %s failed: %s\n", AUDIO_FILE, strerror(errno));
        close(audio_fd);
        audio_fd = -1;
        audio_map = NULL;
        return -1;
    }

    audio = (volatile uint8_t *)audio_map;

    /*
     * Clear FIFOs.
     * Same idea as before:
     * bit 2 = clear write FIFO
     * bit 3 = clear read FIFO
     */
    audio_write32(AUD_CORE_CTRL, 0xC);
    audio_write32(AUD_CORE_CTRL, 0x0);

    uint32_t fifo = audio_read32(AUD_CORE_FIFO);

    printf("Audio mmap OK\n");
    printf("Initial FIFO: 0x%08x\n", fifo);
    printf("RD_L=%u RD_R=%u WR_L=%u WR_R=%u\n",
           FIFO_RD_LEFT(fifo),
           FIFO_RD_RIGHT(fifo),
           FIFO_WR_LEFT(fifo),
           FIFO_WR_RIGHT(fifo));
    fflush(stdout);

    return 0;
}

static void clear_audio(void)
{
    if (audio_map != NULL && audio_map != MAP_FAILED) {
        munmap(audio_map, AUD_CTRLS_SIZE);
        audio_map = NULL;
        audio = NULL;
    }

    if (audio_fd >= 0) {
        close(audio_fd);
        audio_fd = -1;
    }
}

static void *copy_audio(void *arg __attribute__((unused)))
{
    int ret;
    int evl_fd;
    int timer_fd;
    cpu_set_t cpuset;
    struct itimerspec timer;
    __u64 ticks;

    // Run the EVL task on CPU 1
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    ret = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (ret != 0) {
        fprintf(stderr, "pthread_setaffinity_np() failed: %s\n", strerror(ret));
        return NULL;
    }

    // Make thread an EVL thread
    evl_fd = evl_attach_self("copy_audio");
    if (evl_fd < 0) {
        fprintf(stderr, "evl_attach_self() failed\n");
        return NULL;
    }

    // Create EVL timer
    timer_fd = evl_new_timer(EVL_CLOCK_MONOTONIC);
    if (timer_fd < 0) {
        evl_printf("evl_new_timer() failed\n");
        close(evl_fd);
        return NULL;
    }

    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_nsec = AUDIO_POLL_NS;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_nsec = AUDIO_POLL_NS;

    ret = evl_set_timer(timer_fd, &timer, NULL);
    if (ret != 0) {
        evl_printf("evl_set_timer() failed\n");
        close(timer_fd);
        close(evl_fd);
        return NULL;
    }

    evl_printf("Audio EVL thread started\n");

    while (running) {
        ret = oob_read(timer_fd, &ticks, sizeof(ticks));
        if (ret < 0) {
            evl_printf("oob_read() failed\n");
            break;
        }

        uint32_t fifo = audio_read32(AUD_CORE_FIFO);

        /*
         * Need:
         * - at least one input sample available on left and right
         * - at least one output slot available on left and right
         */
        if (FIFO_RD_LEFT(fifo) > 0 &&
            FIFO_RD_RIGHT(fifo) > 0 &&
            FIFO_WR_LEFT(fifo) > 0 &&
            FIFO_WR_RIGHT(fifo) > 0) {

            /*
             * Driver stores samples as uint16_t, but the hardware register is 32-bit.
             * We keep the lower 16 bits, like the driver does.
             */
            uint32_t left_sample  = audio_read32(AUD_CORE_LEFT)  & 0xFFFF;
            uint32_t right_sample = audio_read32(AUD_CORE_RIGHT) & 0xFFFF;

            audio_write32(AUD_CORE_LEFT, left_sample);
            audio_write32(AUD_CORE_RIGHT, right_sample);
        }
    }

    memset(&timer, 0, sizeof(timer));
    evl_set_timer(timer_fd, &timer, NULL);

    close(timer_fd);
    close(evl_fd);

    return NULL;
}

int main(void)
{
    pthread_t thread;

    signal(SIGINT, sigint_handler);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "mlockall() failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("Opening audio copier\n");
    printf("Device: %s\n", AUDIO_FILE);
    printf("Press Ctrl+C to stop\n");
    fflush(stdout);

    if (init_audio() < 0) {
        return EXIT_FAILURE;
    }

    if (pthread_create(&thread, NULL, copy_audio, NULL) != 0) {
        fprintf(stderr, "Error creating audio thread\n");
        clear_audio();
        return EXIT_FAILURE;
    }

    pthread_join(thread, NULL);

    clear_audio();

    printf("\nGoodbye!\n");
    return EXIT_SUCCESS;
}