#define _GNU_SOURCE

#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

#include "../de1soc_io/de1soc_io.h"

#define NB_LOOP

bool running = true;

void *toggle_gpio(void *arg __attribute_maybe_unused__) {
    int ret;
    cpu_set_t cpuset;
    int value = 0;
    struct timespec delay;

    // Set affinity
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    ret = sched_setaffinity(getpid(), sizeof(cpuset), &cpuset);
    if (ret) {
        fprintf(stderr, "[ERROR] Set affinity failed\n");
        return NULL;
    }

    delay.tv_sec = 0; // 0 seconds delay
    delay.tv_nsec = 10000000; // 10 milliseconds delay

    while (running) {
        // Toggle the value
        value = value ? 0x0 : 0x00000002;

        // Set the value of GPIO_0_D1
        write_gpio_val(0, REG_LOW, value);

        nanosleep(&delay, NULL);
    }
    return NULL;
}

// Signal handler used to end the infinite loop
void sigint_handler(int signum __attribute_maybe_unused__) {
    printf("\nCtrl+C received. Exiting...\n");
    running = false;
}


int main(int argc __attribute_maybe_unused__, char *argv[] __attribute_maybe_unused__) {
    uint32_t switch_val;
    // Setup of the ioctl
    if (init_de1soc_io() < 0) return -1;

    printf("DE1-SoC IO initialised\n");
    printf("Switches value: 0x%x\n", switch_val = read_switch());
    printf("Copying switches to leds\n");
    write_led(switch_val);


    // Enable the GPIO_0_D1. It is the FPGA pin PIN_Y17 of the 2x20 GPIO
    // Expansion Headers
    write_gpio_en(0, REG_LOW, 0x00000002);

    // Register Ctrl+C handler
    signal(SIGINT, sigint_handler);

    // Thread setup to handle GPIO value toggle
    pthread_t thread;
    if (pthread_create(&thread, NULL, toggle_gpio, NULL)) {
        fprintf(stderr, "Error creating thread\n");
        return 1;
    }

    // Wait for the thread to end
    pthread_join(thread, NULL);

    clear_de1soc_io();

    printf("Goodbye!\n");

    return 0;
}
