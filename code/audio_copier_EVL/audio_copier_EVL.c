#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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

#include "../de1soc.h"

// In case it's not defined from de1soc.h
#ifndef AUDIO_FILE
#define AUDIO_FILE "/dev/snd"
#endif

// General audio parameters
#define AUDIO_SAMPLE_RATE_HZ  48000
#define AUDIO_FRAMES             64 // we take 64 because with 128 the last doesn't write itself
#define AUDIO_CHANNELS            2

/*
 * One stereo frame:
 * - left  sample: uint16_t
 * - right sample: uint16_t
 *
 * Buffer format:
 * Left0, Right0, Left1, Right1, ...
 */
#define AUDIO_SAMPLES       (AUDIO_FRAMES * AUDIO_CHANNELS)
#define AUDIO_BUFFER_BYTES  (AUDIO_SAMPLES * sizeof(uint16_t))

/*
 * Time for 64 stereo frames at 48 kHz:
 * 64 / 48000 = 2.666 ms
 */
#define AUDIO_PERIOD_NS     1333333L

// Stopping Condition
static volatile sig_atomic_t running = 1;

// Audio file descriptor
static int audio_fd = -1;

/**
 * @brief Stops running after receiving SIGINT
 * @param signum number for interruption
 */
static void sigint_handler(int signum __attribute__((unused)))
{
    running = 0;
}

/**
 * @brief Initializes audio_fd by opening audio file
 * @return 0 if it went well, -1 if an error occurred
 */
static int init_audio(void)
{
    // Open audio file
    audio_fd = open(AUDIO_FILE, O_RDWR);
    if (audio_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", AUDIO_FILE, strerror(errno));
        return -1;
    }

    // Print information about audio file (debugging)
    printf("Audio device opened: %s\n", AUDIO_FILE);
    printf("Buffer: %d stereo frames, %d samples, %zu bytes\n",
           AUDIO_FRAMES,
           AUDIO_SAMPLES,
           (size_t)AUDIO_BUFFER_BYTES);
    fflush(stdout);

    return 0;
}

/**
 * @brief Closes audio_fd if it's open
 */
static void clear_audio(void)
{
    if (audio_fd >= 0) {
        close(audio_fd);
        audio_fd = -1;
    }
}

/**
 * @brief Transforms audio depending on filter
 * @param buffer Buffer of samples
 * @param samples Size of the buffer
 */
static void process_audio(uint16_t *buffer, size_t samples)
{
    /*
     * For now, this is a simple copy/bypass.
     *
     * Buffer format:
     * buffer[0] = Left0
     * buffer[1] = Right0
     * buffer[2] = Left1
     * buffer[3] = Right1
     * ...
     */

    for (size_t i = 0; i < samples; i++) {
        buffer[i] = buffer[i];
    }
}

/**
 * Copies audio from FIFO-in to FIFO-out while processing the audio
 * @param arg arguments received as parameters
 * @return NULL in any case, check terminal for more information
 */
static void *copy_audio(void *arg __attribute__((unused)))
{
    int ret;
    int evl_fd;
    int timer_fd;
    cpu_set_t cpuset;
    struct itimerspec timer;
    __u64 ticks;

    uint16_t audio_buffer[AUDIO_SAMPLES];

    // Set CPU to 1 (RT)
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);

    // Set affinity to cpuset
    ret = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (ret != 0) {
        fprintf(stderr, "pthread_setaffinity_np() failed: %s\n", strerror(ret));
        return NULL;
    }

    // Attach to evl
    evl_fd = evl_attach_self("copy_audio");
    if (evl_fd < 0) {
        fprintf(stderr, "evl_attach_self() failed: %s\n", strerror(errno));
        return NULL;
    }

    // Create timer
    timer_fd = evl_new_timer(EVL_CLOCK_MONOTONIC);
    if (timer_fd < 0) {
        evl_printf("evl_new_timer() failed\n");
        close(evl_fd);
        return NULL;
    }

    // Set timer to 0 and initialize parameters
    memset(&timer, 0, sizeof(timer));

    timer.it_value.tv_sec = 0;
    timer.it_value.tv_nsec = AUDIO_PERIOD_NS;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_nsec = AUDIO_PERIOD_NS;

    // Set timer_fd with timer
    ret = evl_set_timer(timer_fd, &timer, NULL);
    if (ret != 0) {
        evl_printf("evl_set_timer() failed\n");
        close(timer_fd);
        close(evl_fd);
        return NULL;
    }

    evl_printf("Audio EVL thread started\n");

    // Run until stopped
    while (running) {
        ssize_t bytes_read;
        ssize_t bytes_written;
        size_t samples_read;

        // Wait for next period
        ret = oob_read(timer_fd, &ticks, sizeof(ticks));
        if (ret < 0) {
            evl_printf("timer oob_read() failed\n");
            break;
        }

        // Read audio samples from audio_fd
        // Left0, Right0, Left1, Right1, ...
        bytes_read = oob_read(audio_fd, audio_buffer, AUDIO_BUFFER_BYTES);
        if (bytes_read < 0) {
            evl_printf("audio oob_read() failed\n");
            break;
        }

        // Nothing available
        if (bytes_read == 0) {
            continue;
        }

        // Check it returned a multiple of 4 bytes : 2 bytes left + 2 bytes right
        if ((bytes_read % (sizeof(uint16_t) * AUDIO_CHANNELS)) != 0) {
            evl_printf("Invalid audio read size: %ld bytes\n", bytes_read);
            continue;
        }

        // Divided by size of samples and process audio
        samples_read = bytes_read / sizeof(uint16_t);
        process_audio(audio_buffer, samples_read);

        // Write samples back
        bytes_written = oob_write(audio_fd, audio_buffer, bytes_read);
        if (bytes_written < 0) {
            evl_printf("audio oob_write() failed\n");
            break;
        }

        // Check for half-write
        if (bytes_written != bytes_read) {
            evl_printf("Partial audio write: read=%ld written=%ld\n",
                       bytes_read,
                       bytes_written);
        }
    }

    // Stop timer
    memset(&timer, 0, sizeof(timer));
    evl_set_timer(timer_fd, &timer, NULL);

    // Close timer and evl_thread
    close(timer_fd);
    close(evl_fd);

    evl_printf("Audio EVL thread stopped\n");

    return NULL;
}

int main(void)
{
    pthread_t thread;
    int ret;

    // Link SIGINT to stop program
    signal(SIGINT, sigint_handler);

    // Avoid page faults during real-time execution.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "mlockall() failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("Opening audio copier\n");
    printf("Device: %s\n", AUDIO_FILE);
    printf("Press Ctrl+C to stop\n");
    fflush(stdout);

    // Initialize audio_fd
    ret = init_audio();
    if (ret < 0) {
        return ret;
    }

    // Create EVL thread
    ret = pthread_create(&thread, NULL, copy_audio, NULL);
    if (ret != 0) {
        fprintf(stderr, "Error creating audio thread\n");
        goto clear_audio;
    }

    // Wait for thread
    ret = pthread_join(thread, NULL);
    if (ret != 0) {
        fprintf(stderr, "Error joining audio thread\n");
        goto clear_audio;
    }

    // Close audio_fd
    clear_audio();

    printf("\nGoodbye!\n");

    return EXIT_SUCCESS;

    // Error path
clear_audio:
    clear_audio();
    return ret;
}