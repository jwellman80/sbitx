# I2C Contention Fix - Code Examples

## Problem Code (Current - UNSAFE)

```c
// i2cbb.c:196-211 - BROKEN IMPLEMENTATION
static int i2c_write_byte(int send_start, int send_stop, uint8_t byte) {
    unsigned bit;
    int nack = 0;

    static int mutex = 0;  // ❌ NOT THREAD-SAFE
    if (mutex)
        printf("double!\n");  // ❌ JUST A WARNING
    mutex++;  // ❌ RACE CONDITION HERE
    
    if (send_start) {
        i2c_start_cond();
    }
    for (bit = 0; bit < 8; bit++) {
        i2c_write_bit((byte & 0x80) != 0);  // ❌ GPIO CORRUPTION POSSIBLE
        byte <<= 1;
    }
    nack = i2c_read_bit();
    if (send_stop) {
        i2c_stop_cond();
    }
    mutex--;  // ❌ RACE CONDITION HERE
    return nack;
}
```

---

## Solution 1: Simple Mutex Protection (Recommended)

Add this to the top of `i2cbb.c` after includes:

```c
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include <wiringPi.h>
#include <linux/types.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>
#include "i2cbb.h"

// ========== NEW: MUTEX FOR I2C BUS PROTECTION ==========
static pthread_mutex_t i2c_bus_mutex = PTHREAD_MUTEX_INITIALIZER;

// Optional: Add statistics
static int i2c_contention_count = 0;
static int i2c_transaction_count = 0;

// Helper function to safely attempt I2C operation
static int i2c_acquire_bus(int timeout_ms) {
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += timeout_ms / 1000;
    timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
    
    int result = pthread_mutex_timedlock(&i2c_bus_mutex, &timeout);
    if (result == ETIMEDOUT) {
        fprintf(stderr, "I2C bus timeout - contention detected\n");
        i2c_contention_count++;
        return -1;
    }
    i2c_transaction_count++;
    return 0;
}

static void i2c_release_bus(void) {
    pthread_mutex_unlock(&i2c_bus_mutex);
}

// ======================================================
```

Now modify each I2C entry point:

### Fixed: i2cbb_write_byte_data()

```c
// This executes the SMBus "write byte" protocol, returning negative errno else zero on success.
int32_t i2cbb_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value) {
    // 7 bit address + 1 bit read/write
    // read = 1, write = 0
    
    // ✅ ACQUIRE MUTEX BEFORE I2C TRANSACTION
    if (i2c_acquire_bus(100) != 0) {
        fprintf(stderr, "Failed to acquire I2C bus for write_byte_data\n");
        return -1;
    }
    
    uint8_t address = (i2c_address << 1) | 0;
    int result = -1;

    if (!i2c_write_byte(1, 0, address)) {
        if (!i2c_write_byte(0, 0, command)) {
            if (!i2c_write_byte(0, 1, value)) {
                result = 0;  // SUCCESS
            }
        }
        else
            i2c_stop_cond();
    }
    else
        i2c_stop_cond();

    // ✅ RELEASE MUTEX AFTER I2C TRANSACTION
    i2c_release_bus();
    
    return result;
}
```

### Fixed: i2cbb_read_byte_data()

```c
// This executes the SMBus "read byte" protocol, returning negative errno else a data byte.
int32_t i2cbb_read_byte_data(uint8_t i2c_address, uint8_t command) {
    // ✅ ACQUIRE MUTEX
    if (i2c_acquire_bus(100) != 0) {
        fprintf(stderr, "Failed to acquire I2C bus for read_byte_data\n");
        return -1;
    }

    uint8_t address = (i2c_address << 1) | 0;
    int result = -1;

    if (!i2c_write_byte(1, 0, address)) {
        if (!i2c_write_byte(0, 0, command)) {
            address = (i2c_address << 1) | 1;
            if (!i2c_write_byte(1, 0, address)) {
                result = i2c_read_byte(1, 1);
            }
            else
                i2c_stop_cond();
        }
        else
            i2c_stop_cond();
    }
    else
        i2c_stop_cond();

    // ✅ RELEASE MUTEX
    i2c_release_bus();
    
    return result;
}
```

### Fixed: i2cbb_write_i2c_block_data()

```c
int32_t i2cbb_write_i2c_block_data(uint8_t i2c_address, uint8_t command, 
    uint8_t length, const uint8_t * values) {

    // ✅ ACQUIRE MUTEX (replaces spin-wait)
    if (i2c_acquire_bus(100) != 0) {
        fprintf(stderr, "Failed to acquire I2C bus for write_i2c_block_data\n");
        return -1;
    }

    i2c_error = 0;
    uint8_t address = (i2c_address << 1) | 0;

    if (!i2c_write_byte(1, 0, address)) {
        if (!i2c_write_byte(0, 0, command)) {
            int errors = 0;
            size_t i;
            for (i = 0; i < length; i++) {
                if (!errors) {
                    errors = i2c_write_byte(0, 0, values[i]);
                }
            }

            i2c_stop_cond();

            if (!errors) {
                // ✅ RELEASE MUTEX BEFORE RETURN
                i2c_release_bus();
                return i2c_error;
            }
            i2c_error = -1;
            printf("i2cbb: write byte failed at index %d\n", (int)i);
        }
        else {
            i2c_stop_cond();
            printf("i2cbb: command failed\n");
        }
    }
    else {
        i2c_stop_cond();
        printf("i2cbb: address failed %x, cmd %x, length %d\n",
            address, command, length);
    }
    
    // ✅ RELEASE MUTEX
    i2c_release_bus();
    
    return -1;
}
```

### Fixed: i2cbb_read_i2c_block_data()

```c
int32_t i2cbb_read_i2c_block_data(uint8_t i2c_address, uint8_t command, 
        uint8_t length, uint8_t* values) {
    
    // ✅ ACQUIRE MUTEX
    if (i2c_acquire_bus(100) != 0) {
        fprintf(stderr, "Failed to acquire I2C bus for read_i2c_block_data\n");
        return -1;
    }

    uint8_t address = (i2c_address << 1) | 1;
    
    if (i2c_write_byte(1, 0, address)) { 
        i2c_stop_cond();
        i2c_release_bus();
        return -1;
    }

    uint8_t i = 0;
    for (i = 0; i < length - 1; i++) 
        values[i] = i2c_read_byte(0, 0);
    values[i] = i2c_read_byte(1, 1);

    i2c_stop_cond();
    
    // ✅ RELEASE MUTEX
    i2c_release_bus();
    
    return length;
}
```

### Fixed: i2cbb_read_rll()

```c
int32_t i2cbb_read_rll(uint8_t i2c_address, uint8_t* values) {
    
    // ✅ ACQUIRE MUTEX
    if (i2c_acquire_bus(100) != 0) {
        fprintf(stderr, "Failed to acquire I2C bus for read_rll\n");
        return -1;
    }

    uint8_t address = (i2c_address << 1) | 1;
    
    if (i2c_write_byte(1, 0, address)) { 
        i2c_stop_cond();
        i2c_release_bus();
        return -1;
    }

    uint8_t i = 0;
    uint8_t length = i2c_read_byte(0, 0);    
    for (i = 0; i < length - 1; i++) 
        values[i] = i2c_read_byte(0, 0);
    values[i] = i2c_read_byte(1, 1);

    i2c_stop_cond();
    
    // ✅ RELEASE MUTEX
    i2c_release_bus();
    
    return length;
}
```

---

## Solution 2: Macro-Based Protection (Alternative)

If you prefer a cleaner approach with macros:

```c
// At top of i2cbb.c:

#define I2C_TRANSACTION_BEGIN() \
    do { \
        if (pthread_mutex_lock(&i2c_bus_mutex) != 0) { \
            fprintf(stderr, "Failed to lock I2C bus\n"); \
            return -1; \
        } \
    } while (0)

#define I2C_TRANSACTION_END() \
    do { \
        pthread_mutex_unlock(&i2c_bus_mutex); \
    } while (0)

#define I2C_TRANSACTION_RETURN(val) \
    do { \
        int _ret = (val); \
        pthread_mutex_unlock(&i2c_bus_mutex); \
        return _ret; \
    } while (0)

// Usage in i2cbb_write_byte_data():
int32_t i2cbb_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value) {
    I2C_TRANSACTION_BEGIN();
    
    uint8_t address = (i2c_address << 1) | 0;

    if (!i2c_write_byte(1, 0, address)) {
        if (!i2c_write_byte(0, 0, command)) {
            if (!i2c_write_byte(0, 1, value)) {
                I2C_TRANSACTION_RETURN(0);
            }
        }
        else
            i2c_stop_cond();
    }
    else
        i2c_stop_cond();

    I2C_TRANSACTION_RETURN(-1);
}
```

---

## Solution 3: Advanced - Priority-Aware Locking

For real-time systems with priority inversion concerns:

```c
#include <pthread.h>

// Use a recursive mutex to handle re-entrant calls from same thread
static pthread_mutexattr_t i2c_mutex_attr;
static pthread_mutex_t i2c_bus_mutex;

void i2cbb_init_mutex(void) {
    pthread_mutexattr_init(&i2c_mutex_attr);
    pthread_mutexattr_settype(&i2c_mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutexattr_setprotocol(&i2c_mutex_attr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&i2c_bus_mutex, &i2c_mutex_attr);
}

// Call this once at startup:
// i2cbb_init_mutex();

// Or, simpler version using timed lock with deadlock detection:
static int i2c_acquire_bus_with_timeout(void) {
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    
    // 100ms timeout
    timeout.tv_nsec += 100 * 1000 * 1000;
    if (timeout.tv_nsec >= 1000000000) {
        timeout.tv_sec += 1;
        timeout.tv_nsec -= 1000000000;
    }
    
    int ret = pthread_mutex_timedlock(&i2c_bus_mutex, &timeout);
    if (ret == ETIMEDOUT) {
        fprintf(stderr, "I2C bus locked by another thread (possible deadlock)\n");
        return -1;
    } else if (ret != 0) {
        fprintf(stderr, "I2C mutex lock error: %d\n", ret);
        return -1;
    }
    return 0;
}
```

---

## Validation: Testing the Fix

### Test 1: Concurrent Access Test

```c
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

void* thread_func_freq_update(void *arg) {
    for (int i = 0; i < 100; i++) {
        int freq = 14000000 + (i * 1000);
        si5351bx_setfreq(2, freq);
        usleep(1000);  // 1ms between updates
    }
    return NULL;
}

void* thread_func_rtc_read(void *arg) {
    for (int i = 0; i < 50; i++) {
        uint8_t rtc_time[10];
        i2cbb_read_i2c_block_data(0x68, 0, 8, rtc_time);
        usleep(10000);  // 10ms between reads
    }
    return NULL;
}

void test_concurrent_access(void) {
    pthread_t t1, t2, t3;
    
    printf("Starting concurrent I2C access test...\n");
    
    pthread_create(&t1, NULL, thread_func_freq_update, NULL);
    pthread_create(&t2, NULL, thread_func_freq_update, NULL);
    pthread_create(&t3, NULL, thread_func_rtc_read, NULL);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    
    printf("Test complete. Check for 'double!' messages in logs.\n");
}
```

### Test 2: Check for Contention Stats

```c
// Add this function to i2cbb.c:
void i2c_print_stats(void) {
    printf("=== I2C Bus Statistics ===\n");
    printf("Total transactions: %d\n", i2c_transaction_count);
    printf("Contention events: %d\n", i2c_contention_count);
    printf("Contention rate: %.2f%%\n", 
        (float)i2c_contention_count / i2c_transaction_count * 100);
    
    if (i2c_contention_count == 0) {
        printf("Status: OK - No contention detected\n");
    } else {
        printf("Status: WARNING - Contention detected!\n");
    }
}
```

### Test 3: Stress Test Script

```bash
#!/bin/bash
# test_i2c_contention.sh

echo "Starting I2C contention stress test..."
echo "Sending rapid frequency changes..."

# Change frequency rapidly while other operations happen
for i in {1..1000}; do
    echo "r1:freq=$((14000000 + $RANDOM % 100000))" | nc localhost 4532 &
done

wait
echo "Stress test complete"
```

---

## Migration Checklist

- [ ] Add `#include <pthread.h>` to i2cbb.c
- [ ] Add global `i2c_bus_mutex` variable
- [ ] Add helper functions `i2c_acquire_bus()` and `i2c_release_bus()`
- [ ] Modify `i2cbb_write_byte_data()` - add lock/unlock
- [ ] Modify `i2cbb_read_byte_data()` - add lock/unlock
- [ ] Modify `i2cbb_write_i2c_block_data()` - replace spin-wait with mutex
- [ ] Modify `i2cbb_read_i2c_block_data()` - add lock/unlock
- [ ] Modify `i2cbb_read_rll()` - add lock/unlock
- [ ] Remove old static int `i2c_busy` variable
- [ ] Compile and test
- [ ] Run concurrent access tests
- [ ] Monitor for "double!" messages in logs
- [ ] Test frequency tuning during RTC/power monitor reads
- [ ] Verify CW keying audio quality
- [ ] Test network control while tuning

---

## Deployment Notes

1. **Backward compatibility**: The changes are internal only - no API changes needed
2. **Performance**: Mutex lock/unlock is <1us typically, negligible for I2C (bit-banging is ~100us per byte)
3. **Real-time impact**: SCHED_FIFO threads may need `PTHREAD_PRIO_INHERIT` mutex type
4. **Debugging**: Keep the contention counter for field diagnostics

---

## Related Issues This Fixes

- "double!" warnings in logs
- Frequency glitches during RTC read
- CW keying audio artifacts
- Occasional NACK errors from Si5351
- "Repeating I2C" messages from si5351v2.c

