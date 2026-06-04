#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <stdatomic.h>

#include <evl/evl.h>
#include <evl/thread.h>
#include <evl/timer.h>
#include <evl/clock.h>
#include <evl/flags.h>

#include "de1soc_io.h"

// Bit macro
#define BIT(n) (1U << (n))

// Path to audio driver
#define AUDIO_FILE "/dev/snd"

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
 * 64 / 48000 = 1.333 ms
 */
#define AUDIO_PERIOD_NS     1333333L

// Other defines
#define NB_THREADS  3
#define KEY0_BIT    BIT(0)
#define KEY1_BIT    BIT(1)
#define KEY2_BIT    BIT(2)
#define KEY3_BIT    BIT(3)
#define BUTTON_SLEEP_TIME 50000 // 50'000us = 50ms

// Watchdog parameters
#define WATCHDOG_MARGIN_NS  700000L
#define WATCHDOG_TIMEOUT_NS (AUDIO_PERIOD_NS + WATCHDOG_MARGIN_NS)
#define AUDIO_DONE_FLAG     BIT(0)

// Stopping Condition
static volatile sig_atomic_t running = 1;

// Audio file descriptor
static int audio_fd = -1;

// Watchdog flags
static struct evl_flags watchdog_flags;
static int watchdog_flags_fd = -1;

static const int mode_size = 4;
enum Mode {
    NORMAL,
    AMPLITUDE,
    LOW_PASS,
    HIGH_PASS
};
static _Atomic enum Mode mode = NORMAL;

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
 * @brief Close watchdog flags
 */
static void clear_watchdog(void)
{
    if (watchdog_flags_fd >= 0) {
        evl_close_flags(&watchdog_flags);
        close(watchdog_flags_fd);
        watchdog_flags_fd = -1;
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
 * @brief Handles overload detection
 */
static void handle_overload(void)
{
    /*
     * For now, nothing is done here.
     * Later, this function can reduce filter complexity,
     * switch to bypass mode, or notify the user.
     */
}

/**
 * @brief Adds nanoseconds to a timespec
 * @param r Result timespec
 * @param t Initial timespec
 * @param ns Nanoseconds to add
 */
static void timespec_add_ns(struct timespec *r, const struct timespec *t, long long ns)
{
    long long s;
    long long rem;

    s = ns / 1000000000LL;
    rem = ns - s * 1000000000LL;

    r->tv_sec = t->tv_sec + s;
    r->tv_nsec = t->tv_nsec + rem;

    if (r->tv_nsec >= 1000000000L) {
        r->tv_sec++;
        r->tv_nsec -= 1000000000L;
    }
}

/**
 * Watches audio thread execution time
 * @param arg arguments received as parameters
 * @return NULL in any case, check terminal for more information
 */
static void *watchdog_audio(void *arg __attribute__((unused)))
{
    int ret;
    int evl_fd;
    int state;
    cpu_set_t cpuset;
    struct timespec now;
    struct timespec timeout;

    // Set CPU to 1 (RT)
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);

    // Set affinity to cpuset
    ret = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (ret != 0) {
        fprintf(stderr, "watchdog pthread_setaffinity_np() failed: %s\n", strerror(ret));
        return NULL;
    }

    // Attach to evl
    evl_fd = evl_attach_self("watchdog_audio");
    if (evl_fd < 0) {
        fprintf(stderr, "watchdog evl_attach_self() failed\n");
        return NULL;
    }

    evl_printf("Watchdog EVL thread started\n");

    // Run until stopped
    while (running) {

        // Read current time
        ret = evl_read_clock(EVL_CLOCK_MONOTONIC, &now);
        if (ret != 0) {
            evl_printf("watchdog evl_read_clock() failed\n");
            break;
        }

        // Set watchdog timeout
        timespec_add_ns(&timeout, &now, WATCHDOG_TIMEOUT_NS);

        // Wait for audio thread end of cycle
        ret = evl_timedwait_exact_flags(&watchdog_flags, AUDIO_DONE_FLAG, &timeout);

        if (ret == 0) {
            continue;
        }

        // Timeout means overload detection
        evl_peek_flags(&watchdog_flags, &state);

        if (!(state & AUDIO_DONE_FLAG)) {
            evl_printf("Watchdog: overload detected\n");
            handle_overload();

            // Clear pending flags for next cycle
            evl_trywait_flags(&watchdog_flags, &state);
        }
    }

    close(evl_fd);

    evl_printf("Watchdog EVL thread stopped\n");

    return NULL;
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
            evl_post_flags(&watchdog_flags, AUDIO_DONE_FLAG);
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

        // Notify watchdog that audio cycle is completed
        evl_post_flags(&watchdog_flags, AUDIO_DONE_FLAG);
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

static void *wait_button(void *arg __attribute__((unused)))
{
    uint32_t previous_keys = 0;
    uint32_t keys;
    enum Mode new_mode;

    // Initialize the de1soc io file descriptor
    init_de1soc_io();

    while (running) {

        // Read keys and mode
        keys = read_key();
        new_mode = atomic_load(&mode);

        if (keys != previous_keys && keys == (uint32_t) KEY0_BIT) { // Move right (up)
            new_mode = (new_mode + 1) % mode_size;
            atomic_store(&mode, new_mode);
            printf("New mode : %d\n", new_mode);
            fflush(stdout);
        } else if (keys != previous_keys && keys == (uint32_t) KEY1_BIT) { // Move left (down)
            new_mode = (new_mode - 1 + mode_size) % mode_size;
            atomic_store(&mode, new_mode);
            printf("New mode : %d\n", new_mode);
            fflush(stdout);
        }

        // Remember keys for future test
        previous_keys = keys;

        // Can sleep for some time since not needed to always run
        usleep(BUTTON_SLEEP_TIME);
    }

    // Clear file descriptor for io
    clear_de1soc_io();

    return NULL;
}

int main(void)
{
    pthread_t thread[NB_THREADS];
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

    // Create watchdog flags
    watchdog_flags_fd = evl_new_flags(&watchdog_flags, "audio_watchdog_flags");
    if (watchdog_flags_fd < 0) {
        fprintf(stderr, "evl_new_flags() failed\n");
        goto clear_audio;
    }

    // Create button thread
    ret = pthread_create(&thread[0], NULL, wait_button, NULL);
    if (ret != 0) {
        fprintf(stderr, "Error creating button thread\n");
        goto close_flags;
    }

    // Create EVL thread
    ret = pthread_create(&thread[1], NULL, copy_audio, NULL);
    if (ret != 0) {
        fprintf(stderr, "Error creating audio thread\n");
        goto close_flags;
    }

    // Create watchdog thread
    ret = pthread_create(&thread[2], NULL, watchdog_audio, NULL);
    if (ret != 0) {
        fprintf(stderr, "Error creating watchdog thread\n");
        goto close_flags;
    }

    // Wake watchdog before joining threads
    evl_post_flags(&watchdog_flags, AUDIO_DONE_FLAG);

    // Wait for threads
    for (int i = 0 ; i < NB_THREADS ; ++i) {
        ret = pthread_join(thread[i], NULL);
        if (ret != 0) {
            fprintf(stderr, "Error joining audio thread\n");
            goto close_flags;
        }
    }

    // Close watchdog flags
    clear_watchdog();

    // Close audio_fd
    clear_audio();

    printf("\nGoodbye!\n");

    return EXIT_SUCCESS;

    // Error path
close_flags:
    clear_watchdog();

clear_audio:
    clear_audio();
    return ret;
}