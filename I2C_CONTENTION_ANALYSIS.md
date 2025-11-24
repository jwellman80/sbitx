# I2C Bus Contention Analysis - sBitx Codebase

## Executive Summary

The "double!" warnings in `i2cbb.c:198` indicate **reentrancy of I2C operations from multiple concurrent execution contexts** without proper synchronization. The I2C bus-bit-banging implementation only has a trivial, non-functional mutex that is a local static counter, not a real lock. Multiple threads can simultaneously attempt I2C transactions, corrupting the bus state.

---

## 1. CRITICAL FINDING: The "Mutex" is Not a Real Mutex

**Location**: `/mnt/z/dev/sbitx/src/i2cbb.c:196-211`

```c
static int i2c_write_byte(int send_start, int send_stop, uint8_t byte) {
    unsigned bit;
    int nack = 0;
    
    static int mutex = 0;  // <-- THIS IS NOT A REAL MUTEX
    if (mutex)
        printf("double!\n");  // <-- ONLY PRINTS A WARNING
    mutex++;
    // ... I2C transaction code (lines 200-210) ...
    mutex--;
    return nack;
}
```

**Why This Fails**:
- No atomic increment/check (not thread-safe)
- Does NOT prevent concurrent access
- Allows reentrancy to proceed - just prints "double!"
- Race condition: multiple threads can increment `mutex` before any check happens
- Should use `pthread_mutex_t` with lock/unlock

---

## 2. THREADING ARCHITECTURE

### 2.1 Main Threads in sBitx

1. **Main/GTK UI Thread** (sbitx_gtk.c)
   - GTK event loop (event handlers, ui_tick via g_timeout_add)
   - Timeout callback: `ui_tick()` fires every 1ms (line 6493)
   - Can trigger I2C operations from GUI interactions

2. **Sound/DSP Thread** (sbitx_sound.c:1041-1132)
   - SCHED_FIFO real-time priority thread
   - Runs continuously processing audio samples
   - **Can call frequency-setting functions**
   - Created in `sound_thread_start()`

3. **FT8 Decoding Thread** (modem_ft8.c:594-895)
   - Separate thread for FT8 mode processing
   - Can potentially trigger frequency changes

4. **Calibration Thread** (sbitx.c:2176-2202)
   - Created on-demand for power band calibration
   - Calls `set_rx1()` which triggers I2C

5. **Hamlib Network Thread** (hamlib.c:520, 543)
   - Remote control server thread
   - Can issue frequency commands via network

6. **Telnet/WebServer Threads** (telnet.c:250, webserver.c:1418)
   - Network control interfaces
   - Can trigger I2C operations

### 2.2 Only One Real Mutex Found

Location: `sbitx.c:163`
```c
static pthread_mutex_t jitter_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
```
- Protects **jitter audio buffer** only (lines 470, 493, 499, 517)
- **NOT protecting I2C bus**

---

## 3. I2C DEVICE ACCESS PATTERNS

### 3.1 Devices on I2C Bus

| Device | I2C Address | Function | Access From |
|--------|------------|----------|------------|
| **Si5351 Clock Gen** | 0x60 | RX/TX LO freq | `radio_tune_to()` |
| **Si570 Oscillator** | 0x55 | Alternative freq source | `si570_freq()` (wiringPiI2C) |
| **DS3231 RTC** | 0x68 | Time syncing | `rtc_read()`, `rtc_write()` |
| **INA260 Power Monitor** | 0x40 | Voltage/current | `read_voltage_current()` |
| **ZBitx Remote Control** | Various | Remote display | `zbitx_*` functions |

### 3.2 Si5351 Call Path (Most Timing-Critical)

**Call Stack - Frequency Change**:
```
UI/Network Thread               Sound/DSP Thread
─────────────────              ─────────────────
edit_field()                    modem_process()
  ↓                               ↓
do_control_action()             [audio processing]
  ↓                               ↓
sdr_request()                   [samples to output]
  ↓                               ↓
cmd_exec()                      [timing dependent]
  ↓                               ↓
set_operating_freq()
  ↓
sdr_request("r1:freq=...")
  ↓
set_field("r1:freq", ...)
  ↓
update_field()
  ↓
set_rx1(freq)  ◄─────────────────┬──────────────────┐
  ↓                              │                  │
radio_tune_to(freq)              │                  │
  ↓                              │                  │
si5351bx_setfreq()    ◄──────────┴──────────────────┘
  ↓                   (Can be called from DSP thread!)
i2cSendRegister()
  ↓
i2cbb_write_byte_data()
  ↓
i2c_write_byte()  ◄── REENTRANCY HAPPENS HERE
  ↓
i2c_write_bit()  ◄── Interleaved with other thread's i2c_write_bit()!
```

### 3.3 All I2C Access Functions

**Public API** (from i2cbb.h):
- `i2cbb_write_byte_data()` - Single register write
- `i2cbb_read_byte_data()` - Single register read
- `i2cbb_write_i2c_block_data()` - Multi-byte write (has `i2c_busy` spin-wait, but not proper mutex)
- `i2cbb_read_i2c_block_data()` - Multi-byte read
- `i2cbb_read_rll()` - Variable-length read

**Call Sites** (grep results):
1. `si5351v2.c:61` - `si5351bx_setfreq()` → multiple `i2cSendRegister()` calls
2. `sbitx.c:202-206` - `radio_tune_to()` calls `si5351bx_setfreq()`
3. `sbitx_gtk.c:6158-6315` - RTC/Power monitor I2C
4. `sbitx_gtk.c:6811-6931` - ZBitx remote I2C
5. `sbitx.c:2015, 2025` - BFO frequency setting

---

## 4. ROOT CAUSES OF REENTRANCY

### 4.1 Primary Cause: No I2C Transaction Atomicity

**Problem**: The I2C transaction consists of multiple bit-banging operations that must be atomic:
- Each `i2c_write_byte()` contains 8 bits
- Each byte is a sequence of GPIO operations (set/read SCL, SDA)
- If another thread interrupts between bit operations, bus state becomes corrupted

**Example Race Condition**:
```
Thread A: Writing address byte to Si5351
  - Bit 7: SCL=0, SDA=1, SCL=1 ✓
  - Bit 6: SCL=0, SDA=1, SCL=1 ✓
  - [CONTEXT SWITCH TO THREAD B]
  
Thread B: RTC read from different I2C address
  - Bit 7: SCL=0, SDA=0, SCL=1 (conflicts with Thread A's state!)
  - Bus corruption
  
Thread A (resumed): Bit 5: expects SCL/SDA in specific state
  - Gets garbage data, NACK, or timeout
```

### 4.2 Secondary Causes: Multiple Access Patterns

1. **UI Thread Tuning** (every 1ms from `ui_tick()`)
   - `do_tuning()` → `do_control_action()` → `sdr_request()` → `set_rx1()`
   - Frequency changes trigger Si5351 I2C writes
   - Called from GTK main loop

2. **Sound/DSP Thread** (continuous real-time thread)
   - Could theoretically access I2C if frequency changes needed
   - SCHED_FIFO priority - preempts UI thread
   - `sound_thread_continue` loop runs at audio sample rate

3. **Calibration Thread** (on-demand)
   - `pthread_create()` spawns separate thread
   - Calls `set_rx1()` which does I2C writes
   - No synchronization with UI thread

4. **Network Threads** (hamlib, webserver, telnet)
   - Accept frequency change commands
   - Execute `sdr_request()` with frequency changes
   - No I2C locking

5. **RTC/Power Monitor Access** (non-timing-critical)
   - Called from UI handlers
   - But can collide with timing-critical Si5351 access

---

## 5. TIMING-CRITICAL VS NON-TIMING-CRITICAL

### 5.1 Timing-Critical I2C Operations (Must Complete Atomically)

| Operation | Criticality | Reason | Jitter Tolerance |
|-----------|-----------|--------|------------------|
| Si5351 frequency set | **CRITICAL** | RF phase coherence, keying | <1ms |
| Si5351 clock enable/disable | **HIGH** | Affects TX/RX switching | <10ms |
| Si570 frequency set | **HIGH** | Alternate LO source | <10ms |

These affect:
- CW/SSB modulation quality
- Frequency accuracy
- Power amplifier timing

### 5.2 Non-Timing-Critical I2C Operations

| Operation | Timing | Reason |
|-----------|--------|--------|
| RTC read/write | <1s | Time is slowly varying |
| Power monitor (voltage/current) | <100ms | Power levels change slowly |
| ZBitx remote display | <100ms | Status updates can be delayed |

---

## 6. DETAILED ANALYSIS OF i2cbb.c

### 6.1 Hardware Bit-Banging Implementation

```c
// i2cbb.c bit-banging sequence (NOT ATOMIC):
static int i2c_write_byte(int send_start, int send_stop, uint8_t byte) {
    // Line 196-199: BROKEN MUTEX
    static int mutex = 0;  // Non-atomic counter!
    if (mutex)
        printf("double!\n");  // Just a warning
    mutex++;
    
    if (send_start) {
        i2c_start_cond();      // GPIO sequence: SCL, SDA transitions
    }
    for (bit = 0; bit < 8; bit++) {
        i2c_write_bit(...);    // 8 GPIO operations per bit!
        //  ├─ clear_SDA() or read_SDA()
        //  ├─ i2c_delay()
        //  ├─ while(read_SCL()==0) {...}  // Clock stretching loop
        //  ├─ i2c_delay()
        //  └─ clear_SCL()
    }
    nack = i2c_read_bit();     // Read ACK bit (more GPIO ops)
    
    if (send_stop) {
        i2c_stop_cond();       // GPIO sequence: SCL, SDA transitions
    }
    mutex--;                   // Decrement (not protected!)
    
    return nack;
}
```

**Vulnerability**: Between line 202 and 211, GPIO pins are being driven. If another thread enters, it will write conflicting GPIO values.

### 6.2 The "Busy Wait" Attempt

```c
// i2cbb.c line 278-294: INADEQUATE SYNCHRONIZATION
int i2c_busy = 0;

int32_t i2cbb_write_i2c_block_data(...) {
    for (int i = 0; i < 100; i++){
        if (!i2c_busy)
            break;
        printf("i2c busy\n");
        delay(2);
    }
    i2c_busy++;  // Not atomic! Race condition: two threads can both see i2c_busy=0
    
    // ... I2C transaction ...
    
    i2c_busy--;
    return -1;
}
```

**Problems**:
- Spin-wait wastes CPU and adds jitter
- No atomic operations (need `__atomic_compare_exchange` or `pthread_mutex`)
- Print statements inside critical section (very slow I/O)
- Doesn't protect sub-functions like `i2cbb_read_byte_data()`

---

## 7. CALL PATH ANALYSIS - WHERE REENTRANCY OCCURS

### 7.1 Si5351 Frequency Setting Path (Most Common)

```
[GTK Main Thread]
├─ g_timeout_add(1ms) → ui_tick()  (line 6493)
│  ├─ modem_poll()
│  └─ [User tunes dial]
│     └─ do_tuning()
│        └─ do_control_action()
│           └─ sdr_request("r1:freq=...")
│              └─ [Eventually calls] set_rx1()
│                 └─ radio_tune_to()
│                    └─ si5351bx_setfreq()
│                       └─ setup_multisynth()
│                          └─ i2cSendRegister() [8 times!]
│                             └─ i2cbb_write_byte_data()
│                                └─ i2c_write_byte()  ◄── REENTRANCY POINT
│                                   └─ i2c_write_bit() [8x]
│                                      └─ digitalWrite(PIN_SDA/PIN_SCL)
│
[Sound/DSP Thread - SCHED_FIFO]
├─ sound_thread_function()
│  └─ [Continuous audio processing]
│     [Could call modem processing]
│        └─ [If frequency update needed]
│           └─ set_rx1()
│              └─ [COLLIDES WITH GTK THREAD'S I2C]

[Network/Hamlib Thread]
├─ hamlib_slice() or telnet_thread_function()
│  └─ [Remote frequency command]
│     └─ sdr_request("r1:freq=...")
│        └─ set_rx1()
│           └─ [COLLIDES WITH GTK AND/OR DSP THREADS]

[Calibration Thread]
├─ calibration_thread_function()
│  └─ calibrate_band_power()
│     └─ set_rx1()
│        └─ [COLLIDES WITH UI AND/OR NETWORK THREADS]
```

### 7.2 RTC Access Path (Secondary)

```
[GTK Main Thread - ui_tick()]
└─ [Every few seconds?]
   └─ rtc_read()  (line 6158)
      ├─ i2cbb_write_i2c_block_data()
      └─ i2cbb_read_i2c_block_data()  ◄── COLLIDES WITH Si5351 writes
```

---

## 8. IMPACT ASSESSMENT

### 8.1 Symptoms When Reentrancy Occurs

1. **I2C Transaction Corruption**
   - One thread writes SCL/SDA while another reads them
   - Bus goes into undefined state
   - Slave device (Si5351) sees nonsense

2. **Frequency Glitches**
   - Partial frequency writes received by Si5351
   - RF output frequency wrong
   - CW keying distorted
   - SSB modulation degraded

3. **ACK/NACK Detection Fails**
   - Multiple threads writing ACK bits
   - Slave acknowledgment lost
   - I2C timeout
   - "Repeating I2C" messages in log (si5351v2.c:63)

4. **Audio Artifacts**
   - Phase discontinuities if DSP thread hit mid-frequency-change
   - Clicks/pops in audio
   - Audible frequency shifts

### 8.2 Why "Double!" Warnings Indicate Critical Bug

- Appears when `i2c_write_byte()` reentered
- If it prints "double!", **data corruption is occurring**
- Not a preventive measure, just a diagnostic
- By the time "double!" prints, I2C bus is already corrupted

---

## 9. SPECIFIC VULNERABLE SEQUENCES

### 9.1 Sequence 1: Fast Tuning Hits RTC Read

```
Time    GTK Thread              Sound Thread           RTC Thread
────────────────────────────────────────────────────────────────────
0ms     ui_tick()
        do_tuning()
        set_rx1(freq)
        si5351bx_setfreq()
        i2cbb_write_byte_data(0x60, reg0, val)
        i2c_write_byte()
        i2c_write_bit(bit7)
        digitalWrite(SDA, 1)
        digitalWrite(SCL, 1)
                                [CONTEXT SWITCH - SOUND THREAD]
                                modem_process()
                                frequency update check
                                set_rx1(freq) ◄── REENTRANT!
                                si5351bx_setfreq()
                                i2cbb_write_byte_data(0x60, ...)
                                i2c_write_byte()
                                mutex == 1, print "double!"
                                mutex++ → 2
                                i2c_write_bit()
                                digitalWrite(SDA, 0)  ◄── WRONG STATE
                                digitalWrite(SCL, 0)
                                
                                [SDA/SCL conflict!]
                                [Both threads manipulating GPIO]
                                [I2C bus corrupted]

        [GTK resumes, reads SCL/SDA in wrong state]
        i2c_read_bit()
        bit = read_SDA()  ← Gets garbage from Sound thread
        clear_SCL()
```

### 9.2 Sequence 2: UI Tuning Collides With RTC

```
0ms     ui_tick()
        read_voltage_current()
        i2cbb_read_i2c_block_data(0x40, ...)
        i2c_write_byte(1, 0, addr)  ← Sets up read addr on INA260
        i2c_start_cond()
        clear_SDA(), clear_SCL()
        [GPIO registers written]
        
        [PREEMPTION BY HAMLIB THREAD]
        sdr_request("r1:freq=14200000")
        set_rx1(14200000)
        si5351bx_setfreq()
        i2cSendRegister(0, val)
        i2cbb_write_byte_data(0x60, ...)
        i2c_write_byte(1, 0, address)  ← Tries to START new transaction
        i2c_start_cond()
        read_SDA()  ← SDA is in undefined state from INA260 read!
        
        [BUS CORRUPTION]
```

---

## 10. ROOT CAUSE SUMMARY

| Root Cause | Location | Severity | Evidence |
|-----------|----------|----------|----------|
| No atomic mutex | i2cbb.c:196-199 | CRITICAL | Static int `mutex` is not atomic |
| No transaction atomicity | i2cbb.c:192-213 | CRITICAL | GPIO operations not atomic as group |
| Multiple threads access I2C | sbitx.c, sbitx_gtk.c, hamlib.c | CRITICAL | 5+ threads can call I2C functions |
| No synchronization in sub-functions | i2cbb.c | HIGH | `i2cbb_read_byte_data()` unprotected |
| Spin-wait inadequate | i2cbb.c:288-293 | HIGH | `i2c_busy` is not atomic |
| Real-time thread priority inversion | sbitx_sound.c | HIGH | DSP thread can interrupt UI during I2C |
| No I2C transaction lock | i2cbb.h | CRITICAL | No global mutex in public API |

---

## 11. RECOMMENDATIONS FOR FIX

### 11.1 Immediate Short-Term Fix (Band-Aid)

**Replace the fake mutex with a real `pthread_mutex_t`** in i2cbb.c:

```c
#include <pthread.h>

static pthread_mutex_t i2c_bus_mutex = PTHREAD_MUTEX_INITIALIZER;

int32_t i2cbb_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value) {
    pthread_mutex_lock(&i2c_bus_mutex);
    
    uint8_t address = (i2c_address << 1) | 0;
    int result = 0;
    
    if (!i2c_write_byte(1, 0, address)) {
        if (!i2c_write_byte(0, 0, command)) {
            if (!i2c_write_byte(0, 1, value)) {
                result = 0;
            }
        } else {
            i2c_stop_cond();
            result = -1;
        }
    } else {
        i2c_stop_cond();
        result = -1;
    }
    
    pthread_mutex_unlock(&i2c_bus_mutex);
    return result;
}
```

### 11.2 Medium-Term Fix (Better)

1. Protect **ALL** I2C entry points with mutex:
   - `i2cbb_write_byte_data()`
   - `i2cbb_read_byte_data()`
   - `i2cbb_write_i2c_block_data()`
   - `i2cbb_read_i2c_block_data()`
   - `i2cbb_read_rll()`

2. Create wrapper macros:
```c
#define I2C_TRANSACTION_START() pthread_mutex_lock(&i2c_bus_mutex)
#define I2C_TRANSACTION_END()   pthread_mutex_unlock(&i2c_bus_mutex)
```

3. Replace spin-wait `i2c_busy` with conditional variables:
```c
static pthread_mutex_t i2c_bus_lock = PTHREAD_MUTEX_INITIALIZER;
static int i2c_transaction_in_progress = 0;

int32_t i2cbb_write_i2c_block_data(...) {
    pthread_mutex_lock(&i2c_bus_lock);
    // ... I2C operations ...
    pthread_mutex_unlock(&i2c_bus_lock);
}
```

### 11.3 Long-Term Fix (Architectural)

1. **Use Linux kernel I2C driver** instead of bit-banging
   - `/dev/i2c-*` device
   - Built-in locking
   - Hardware-accelerated if available

2. **Or: Hardware I2C controller**
   - Raspberry Pi I2C pins have hardware controller
   - No GPIO bit-banging needed
   - Atomic transactions guaranteed by hardware

3. **Create I2C transaction serialization layer**:
   - All I2C access through central queue
   - Single I2C thread processes queue
   - Timestamps and priority for real-time frequency changes

### 11.4 Testing Recommendations

1. **Stress test**: Rapidly tune frequency while logging:
```bash
while true; do
  echo "r1:freq=14200000" | nc localhost 4532
  sleep 0.01
done
```
   - Should see zero "double!" messages
   - Frequency should not glitch

2. **Concurrent access test**:
   - Frequency tuning + RTC read simultaneously
   - Network control + local tuning
   - Calibration thread + UI tuning

3. **Real-time thread testing**:
   - Monitor I2C transaction timing
   - Check for >10ms jitter (I2C timeout)
   - Verify no "double!" in logs

4. **CW keying test**:
   - Tune while sending CW
   - Check output waveform for frequency jumps
   - Should see zero clicks/pops from I2C collisions

---

## 12. FILES REQUIRING CHANGES

| File | Function | Change |
|------|----------|--------|
| i2cbb.c | i2cbb_write_byte_data | Add mutex lock/unlock |
| i2cbb.c | i2cbb_read_byte_data | Add mutex lock/unlock |
| i2cbb.c | i2cbb_write_i2c_block_data | Replace `i2c_busy` with mutex |
| i2cbb.c | i2cbb_read_i2c_block_data | Add mutex lock/unlock |
| i2cbb.c | i2cbb_read_rll | Add mutex lock/unlock |
| i2cbb.c | Top of file | Add `#include <pthread.h>` and global mutex |
| i2cbb.h | (optional) | Export mutex or create lock/unlock functions |

---

## 13. SUMMARY TABLE: THREADING HAZARD MATRIX

```
                    GTK UI      Sound/DSP   Calibration  Hamlib   WebServer  Telnet
                    ───────     ─────────   ───────────  ──────   ─────────  ──────
Si5351 (0x60)        ✗✗✗         ✗✗✗         ✗✗✗         ✗✗      ✗✗         ✗✗
RTC (0x68)           ✗✗          -           -           -        -          -
INA260 (0x40)        ✗           -           -           -        -          -
ZBitx (0x0a)         ✗           -           -           -        -          -
Si570 (0x55)         ✗           -           -           -        -          -

Legend:
✗✗✗ = Concurrent access highly likely (ms scale)
✗✗  = Concurrent access possible
✗   = Rare, but possible
-   = No access path

All cells with ✗ or more need mutex protection!
```

---

## CONCLUSION

The I2C bus contention is caused by **lack of proper synchronization primitives** (pthread_mutex) protecting I2C transaction atomicity. The "double!" warning indicates data corruption is occurring. This requires immediate fix with a proper mutex protecting all I2C bus access, especially the Si5351 frequency setting which is accessed from 5+ concurrent threads.

