#ifndef TIMING_DEBUG_H
#define TIMING_DEBUG_H

#include <time.h>
#include <stdio.h>

// Enable timing debug (set to 1 to enable, 0 to disable)
#define TIMING_DEBUG_ENABLED 1

#if TIMING_DEBUG_ENABLED

static struct timespec timing_start, timing_end;
static long timing_samples[10];
static int timing_index = 0;
static const char* timing_labels[10];
static int timing_count = 0;

#define TIMING_START() \
    do { \
        clock_gettime(CLOCK_MONOTONIC, &timing_start); \
        timing_count = 0; \
    } while(0)

#define TIMING_CHECKPOINT(label) \
    do { \
        clock_gettime(CLOCK_MONOTONIC, &timing_end); \
        long delta_us = (timing_end.tv_sec - timing_start.tv_sec) * 1000000L + \
                        (timing_end.tv_nsec - timing_start.tv_nsec) / 1000L; \
        if (timing_count < 10) { \
            timing_samples[timing_count] = delta_us; \
            timing_labels[timing_count] = label; \
            timing_count++; \
        } \
        timing_start = timing_end; \
    } while(0)

#define TIMING_PRINT() \
    do { \
        static int print_counter = 0; \
        if (++print_counter >= 100) { \
            print_counter = 0; \
            printf("\n=== TIMING PROFILE ===\n"); \
            for (int i = 0; i < timing_count; i++) { \
                printf("%s: %ld us\n", timing_labels[i], timing_samples[i]); \
            } \
            printf("======================\n"); \
        } \
    } while(0)

#else
#define TIMING_START()
#define TIMING_CHECKPOINT(label)
#define TIMING_PRINT()
#endif

#endif // TIMING_DEBUG_H
