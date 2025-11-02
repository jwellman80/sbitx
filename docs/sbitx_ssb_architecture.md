# sBitx SSB Modulation Architecture - Comprehensive Analysis

## Executive Summary

The sBitx transceiver implements SSB (Single Sideband) modulation using an FFT-based frequency-domain approach with Kaiser-windowed FIR filtering. The current architecture is well-suited for implementing CESSB (Controlled Envelope Single Sideband) with envelope shaping enhancements.

---

## 1. Audio Processing Pipeline Overview

### 1.1 Main Processing Flow

```
Input Audio → Sound Thread → TX/RX Selection → FFT Processing → Filtering → 
IFFT → AGC → Compression/EQ → Output Audio
```

### 1.2 Key Entry Points

**Primary Audio Entry:** `/home/pi/sbitx/src/sbitx.c:sound_process()` (line 1947)
- Called continuously from sound thread at 96 kHz sample rate
- Handles both TX and RX paths
- Parameters:
  - `int32_t *input_rx` - RX audio input
  - `int32_t *input_mic` - TX microphone input  
  - `int32_t *output_speaker` - RX audio output
  - `int32_t *output_tx` - TX RF modulated output
  - `int n_samples` - Samples per buffer (1024 typical, MAX_BINS/2)

**TX Processing:** `tx_process()` (line 1589) - Main transmit signal processing
**RX Processing:** `rx_linear()` (line 1142) - Main receive signal processing

---

## 2. SSB Modulation Implementation Details

### 2.1 TX Processing Flow (tx_process)

**Location:** `/home/pi/sbitx/src/sbitx.c:1589-1945`

#### Step 1: Microphone Audio Input
```c
// Line 1717-1721: Normalize microphone samples
i_sample = (1.0 * input_mic[j]) / 2000000000.0;  // int32_t to float [-1.0, 1.0]
```

#### Step 2: Compression (Optional)
```c
// Line 1626-1663: Apply fixed compression if enabled
apply_fixed_compression(temp_input_mic, n_samples, compression_control_level);
```
- Threshold-based compression
- Ratio: 4.0:1 (configurable)
- Only active for voice modes (USB, LSB, AM)

#### Step 3: Equalization (Optional)
```c
// Line 1665-1672: Apply TX EQ if enabled
apply_eq(&tx_eq, input_mic, n_samples, 96000.0);
```

#### Step 4: Time-Domain Processing
```c
// Line 1684-1762: Fill FFT input buffer with overlap-add
// Previous samples (fft_m) copied to first half of fft_in
for (i = 0; i < MAX_BINS / 2; i++)
    fft_in[i] = fft_m[i];

// New samples added to second half
for (i = MAX_BINS / 2; i < MAX_BINS; i++) {
    i_sample = (1.0 * input_mic[j]) / 2000000000.0;
    __real__ fft_in[i] = i_sample;
    __imag__ fft_in[i] = 0.0;  // Q channel zero for SSB
    j++;
}
```

#### Step 5: FFT to Frequency Domain
```c
// Line 1770: Execute forward FFT
fftw_execute(plan_fwd);
// Result: fft_out contains frequency bins (0 to MAX_BINS-1)
```
- FFT Size: MAX_BINS = 2048
- Sample Rate: 96 kHz
- Frequency Resolution: 96000/2048 = 46.875 Hz per bin
- Bandwidth: ±48 kHz

#### Step 6: Frequency-Domain Filtering
```c
// Line 1776-1778: Apply FIR filter in frequency domain
for (i = 0; i < MAX_BINS; i++)
    fft_out[i] *= tx_filter->fir_coeff[i];
```

#### Step 7: Sideband Selection
```c
// Line 1786-1799: Zero out unwanted sideband
if (r->mode == MODE_LSB || r->mode == MODE_CWR)
    // zero out LSB (bins 0 to MAX_BINS/2-1)
    for (i = 0; i < MAX_BINS / 2; i++) {
        __real__ fft_out[i] = 0;
        __imag__ fft_out[i] = 0;
    }
else if (r->mode != MODE_AM)
    // zero out USB (bins MAX_BINS/2 to MAX_BINS-1)
    for (i = MAX_BINS / 2; i < MAX_BINS; i++) {
        __real__ fft_out[i] = 0;
        __imag__ fft_out[i] = 0;
    }
```

#### Step 8: SSB Power Factor Adjustment
```c
// Line 1801-1805: Apply SSB modulation power adjustment (W9JES)
for (i = 0; i < MAX_BINS / 2; i++) {
    __real__ fft_out[i] = __real__ fft_out[i] * ssb_val;
    __imag__ fft_out[i] = __imag__ fft_out[i] * ssb_val;
}
// ssb_val is loaded from hw_settings.ini
```

#### Step 9: Frequency Shifting
```c
// Line 1807-1820: Shift signal to TX frequency bin
int shift = tx_shift;  // Default: 512 (24 kHz offset from center)
for (i = 0; i < MAX_BINS; i++) {
    int b = i + shift;
    if (b >= MAX_BINS) b = b - MAX_BINS;
    if (b < 0) b = b + MAX_BINS;
    r->fft_freq[b] = fft_out[i];  // Rotate bins
}
```

#### Step 10: Inverse FFT (Frequency to Time Domain)
```c
// Line 1826: Convert back to time domain
fftw_execute(r->plan_rev);
// Result: r->fft_time contains time-domain I/Q samples
```

#### Step 11: Output Scaling and ALC
```c
// Line 1829-1839: Apply gain, ALC, and limit output
float scale = volume;
for (i = 0; i < MAX_BINS / 2; i++) {
    double s = creal(r->fft_time[i + (MAX_BINS / 2)]);
    output_tx[i] = s * scale * tx_amp * alc_level;
    // Track min/max for ALC adjustment
    if (min > output_tx[i]) min = output_tx[i];
    if (max < output_tx[i]) max = output_tx[i];
}
```

### 2.2 RX Processing Flow (rx_linear)

**Location:** `/home/pi/sbitx/src/sbitx.c:1142-1525`

#### Step 1-3: Time-Domain Input and FFT
Similar to TX, uses overlap-add with previous samples

#### Step 4: Bin Rotation (Frequency Shifting)
```c
// Line 1175-1189: Rotate bins to bring desired signal to baseband
int shift = r->tuned_bin;
for (i = 0; i < MAX_BINS; i++) {
    int b = i + shift;
    if (b >= MAX_BINS) b -= MAX_BINS;
    if (b < 0) b += MAX_BINS;
    r->fft_freq[i] = fft_out[b];
}
```

#### Step 5: DSP Processing (Optional)
- **Notch Filter:** Line 1227-1248
- **Spectral Subtraction:** Line 1329-1359
- **Wiener Filter (ANR):** Line 1361-1388

#### Step 6: Sideband Selection
```c
// Line 1392-1411: Zero out opposite sideband
switch (r->mode) {
    case MODE_LSB:
    case MODE_CWR:
        // Zero bins 0 to MAX_BINS/2-1
        break;
    case MODE_AM:
        // Keep both sidebands
        break;
    default:  // USB, CW
        // Zero bins MAX_BINS/2 to MAX_BINS-1
        break;
}
```

#### Step 7: Filtering
```c
// Line 1413-1417: Apply FIR filter
for (i = 0; i < MAX_BINS; i++) {
    r->fft_freq[i] *= r->filter->fir_coeff[i];
}
```

#### Step 8: Inverse FFT
```c
// Line 1447: Convert to time domain
my_fftw_execute(r->plan_rev);
```

#### Step 9: AGC (Automatic Gain Control)
```c
// Line 1450: Apply AGC
agc2(r);
```

#### Step 10: Output and Optional EQ
```c
// Line 1454-1474: Generate output
// Line 1493-1516: Apply RX EQ if enabled
```

---

## 3. Key Data Structures

### 3.1 Filter Structure
**File:** `/home/pi/sbitx/src/sdr.h:119-125`

```c
struct filter {
    complex float *fir_coeff;    // FIR coefficients in frequency domain
    complex float *overlap;      // Overlap-add buffer
    int N;                       // Total length (L + M - 1)
    int L;                       // Input length (1024)
    int M;                       // Impulse length (1025)
};
```

### 3.2 RX Structure (Applies to TX too)
**File:** `/home/pi/sbitx/src/sdr.h:159-187`

```c
struct rx {
    long tuned_bin;              // Current bin offset
    short mode;                  // MODE_USB, MODE_LSB, MODE_CW, MODE_AM
    int low_hz;                  // Passband low cutoff (Hz)
    int high_hz;                 // Passband high cutoff (Hz)
    fftw_plan plan_rev;          // IFFT plan
    fftw_complex *fft_freq;      // Frequency domain samples
    fftw_complex *fft_time;      // Time domain samples
    
    // AGC parameters
    int agc_speed;               // Hang time (samples)
    int agc_threshold;           // AGC threshold (-60 dB typical)
    int agc_loop;                // AGC counter
    double signal_strength;      // Current signal level
    double agc_gain;             // Current gain
    double signal_avg;           // Smoothed signal level
    
    struct filter *filter;       // Filtering object
    int output;                  // Output destination (-1, 0, or socket)
    struct rx *next;             // Linked list pointer
};
```

---

## 4. Filter Implementation

### 4.1 Filter Design
**File:** `/home/pi/sbitx/src/fft_filter.c`

#### Kaiser Window Filter
```c
// Line 91-181: window_filter() - Applies Kaiser window and creates FIR coefficients
// Process:
// 1. Convert frequency response to time domain (IFFT)
// 2. Apply Kaiser window: window[n] = I0(pi*beta*sqrt(1-(2n/(M-1)-1)^2)) / I0(pi*beta)
// 3. Shift and zero-pad impulse response
// 4. Convert back to frequency domain (FFT)
// 5. Store FIR coefficients for multiplication
```

#### Filter Tuning
```c
// Line 194-225: filter_tune() - Sets passband edges
void filter_tune(struct filter *f, float low, float high, float kaiser_beta) {
    // low, high are normalized frequencies (0.0 to 1.0, relative to sample rate)
    // kaiser_beta controls filter roll-off (typical: 5.0)
    
    for (int n = 0; n < f->N; n++) {
        float s = (n <= f->N/2) ? (float)n / f->N : (float)(n - f->N) / f->N;
        f->fir_coeff[n] = (s >= low && s <= high) ? gain : 0;
    }
    window_filter(f->L, f->M, f->fir_coeff, kaiser_beta);
}
```

**Typical Usage:**
```c
// Line 759: RX filter setup
filter_tune(r->filter, 
    (1.0 * (-3000)) / 96000.0,  // -3000 Hz (normalized)
    (1.0 * (-300)) / 96000.0,   // -300 Hz (normalized)
    5.0);  // Kaiser beta
// For LSB mode, negative frequencies are used
```

---

## 5. Signal Processing Utilities

### 5.1 VFO (Voltage Controlled Oscillator)
**File:** `/home/pi/sbitx/src/vfo.c`

```c
void vfo_start(struct vfo *v, int frequency_hz, int start_phase) {
    v->phase_increment = (frequency_hz * 65536) / sampling_freq;
    // For 96 kHz: phase_increment = freq * 65536 / 96000
}

int vfo_read(struct vfo *v) {
    // Returns sin(phase) * 2^30, uses lookup table for efficiency
    // Phase increments: v->phase += v->phase_increment; v->phase &= 0xffff;
}
```

Used for:
- Tone generation (1000 Hz sidetone for CW)
- AM carrier generation (24 kHz modulation)
- 2-tone test signals

### 5.2 AGC (Automatic Gain Control)
**File:** `/home/pi/sbitx/src/sbitx.c:836-896`

```c
double agc2(struct rx *r) {
    // 1. Find peak signal amplitude from time-domain samples
    // 2. Calculate smoothed signal average: signal_avg = 0.93*signal_avg + 0.07*signal
    // 3. Compute desired gain: agc_gain_should_be = 100e9 / signal_strength
    // 4. Apply fast attack, slow decay:
    //    - If signal increases: immediate gain adjustment
    //    - If signal decreases: hang for r->agc_speed blocks, then decay
    // 5. Apply gain ramp across sample block for smooth transitions
    // 6. Returns effective S-meter reading
}
```

**AGC Parameters (Configurable):**
- `agc_speed`: Hang time (5-10 blocks typical)
- `agc_threshold`: Activation threshold (-60 dB)
- `agc_decay_rate`: Slope of decay

### 5.3 Compression
**File:** `/home/pi/sbitx/src/sbitx.c:361-394`

```c
void apply_fixed_compression(float *input, int num_samples, int compression_control_value) {
    float compression_level = compression_control_value / 10.0;
    float internal_threshold = 0.1f;
    float internal_ratio = 4.0f;  // 4:1 compression
    
    for (int i = 0; i < num_samples; i++) {
        float sample = input[i];
        
        // Compress above/below threshold
        if (sample > internal_threshold)
            sample = internal_threshold + (sample - internal_threshold) / internal_ratio;
        else if (sample < -internal_threshold)
            sample = -internal_threshold + (sample + internal_threshold) / internal_ratio;
        
        // Apply makeup gain
        sample *= 1.0 + compression_level;
        
        // Clip if needed
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        
        input[i] = sample;
    }
}
```

### 5.4 Parametric EQ
**File:** `/home/pi/sbitx/src/para_eq.c`

- 10-band parametric EQ (RX and TX)
- Uses biquad filters
- Configurable via user_settings.ini
- Applied to audio before TX modulation or after RX demodulation

---

## 6. Transmit Audio Chain Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    TX AUDIO CHAIN                            │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  Microphone Input (int32_t, 96kHz)                          │
│           ↓                                                  │
│  ┌──────────────────────────────────────────┐               │
│  │  Compression (Optional)                   │               │
│  │  - Threshold: 0.1                        │               │
│  │  - Ratio: 4:1                            │               │
│  │  - Control: 1-10 (0=off)                 │               │
│  └──────────────────────────────────────────┘               │
│           ↓                                                  │
│  ┌──────────────────────────────────────────┐               │
│  │  TX Parametric EQ (Optional)              │               │
│  │  - 10 bands                              │               │
│  │  - Biquad filters                        │               │
│  └──────────────────────────────────────────┘               │
│           ↓                                                  │
│  Normalize: sample / 2e9                                    │
│           ↓                                                  │
│  ┌──────────────────────────────────────────┐               │
│  │  FFT (Overlap-Add)                        │               │
│  │  - Size: 2048                            │               │
│  │  - Window: Previous 50% overlap          │               │
│  └──────────────────────────────────────────┘               │
│           ↓                                                  │
│  ┌──────────────────────────────────────────┐               │
│  │  Frequency-Domain Filtering               │               │
│  │  - Kaiser-windowed FIR                   │               │
│  │  - Passband: configurable (-3dB to -300Hz typical)│
│  └──────────────────────────────────────────┘               │
│           ↓                                                  │
│  ┌──────────────────────────────────────────┐               │
│  │  SSB Sideband Selection                   │               │
│  │  - USB: Zero lower bins                  │               │
│  │  - LSB: Zero upper bins                  │               │
│  │  - AM: Keep both sidebands               │               │
│  └──────────────────────────────────────────┘               │
│           ↓                                                  │
│  Apply SSB Power Factor (ssb_val from hw_settings.ini)     │
│           ↓                                                  │
│  Frequency Shift: Rotate bins by tx_shift (≈24 kHz offset)  │
│           ↓                                                  │
│  ┌──────────────────────────────────────────┐               │
│  │  IFFT (Frequency to Time)                │               │
│  │  - Generate complex I/Q output           │               │
│  │  - 1024 samples at 96 kHz = 10.67 ms    │               │
│  └──────────────────────────────────────────┘               │
│           ↓                                                  │
│  ┌──────────────────────────────────────────┐               │
│  │  Output Scaling & ALC                     │               │
│  │  - Scale: volume * tx_amp * alc_level   │               │
│  │  - Track min/max for ALC adjustment     │               │
│  │  - Output: int32_t (headroom maintained) │               │
│  └──────────────────────────────────────────┘               │
│           ↓                                                  │
│  RF Output (output_tx) → ALSA Audio System → RF Hardware    │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 7. Key Files and Their Roles

| File | Lines | Purpose |
|------|-------|---------|
| `/home/pi/sbitx/src/sbitx.c` | 2587 | Main SDR processing (tx_process, rx_linear) |
| `/home/pi/sbitx/src/sbitx_sound.c` | 1167 | Audio I/O and ALSA interface |
| `/home/pi/sbitx/src/fft_filter.c` | 246 | Kaiser-windowed FIR filtering |
| `/home/pi/sbitx/src/vfo.c` | 60 | Oscillator generation |
| `/home/pi/sbitx/src/para_eq.c` | 300+ | Parametric EQ (biquad filters) |
| `/home/pi/sbitx/src/sdr.h` | 285 | Data structures and function declarations |
| `/home/pi/sbitx/src/modem_cw.c` | 800+ | CW modulation with envelope shaping |

---

## 8. Current Envelope Handling

### 8.1 Existing Envelope Shaping
**File:** `/home/pi/sbitx/src/modem_cw.c:680-720`

CW mode includes basic envelope shaping:
```c
static float cw_envelope = 1;
static int cw_envelope_pos = 0;
static int cw_envelope_len = 480;  // ~5ms at 96kHz

// Applies rise/fall time to CW keying
apply_fir_filter(samples, filtered_samples, fir_coeffs, count, 64);
```

### 8.2 AM Modulation (Envelope Technique)
**File:** `/home/pi/sbitx/src/sbitx.c:1701-1713`

```c
if (r->mode == MODE_AM) {
    double modulation = (1.0 * input_mic[j]) / 200000000.0;
    if (modulation < -1.0) modulation = -1.0;
    i_carrier = (1.0 * vfo_read(&am_carrier)) / 50000000000.0;
    i_sample = (1.0 + modulation) * i_carrier;  // AM modulation
}
```

---

## 9. CESSB Implementation Opportunities

### 9.1 Current Limitations for CESSB

1. **No envelope control:** Output envelope is determined solely by frequency-domain processing
2. **No peak detection:** No real-time envelope monitoring
3. **Limited clipping:** Basic audio clipping only, no intelligent envelope limiting
4. **No Hilbert transform:** Could improve sideband rejection

### 9.2 Recommended CESSB Implementation Points

**Location for implementation: `tx_process()` function (sbitx.c)**

#### Option 1: Post-IFFT Envelope Limiting (Simplest)
```c
// After line 1839 (output_tx generation), before line 1841:

// Detect envelope
float envelope[MAX_BINS/2];
float peak_envelope = 0;
for (i = 0; i < MAX_BINS/2; i++) {
    envelope[i] = sqrt(pow(creal(r->fft_time[i]), 2) + pow(cimag(r->fft_time[i]), 2));
    if (envelope[i] > peak_envelope) peak_envelope = envelope[i];
}

// Limit envelope while preserving phase
float envelope_limit = some_threshold;
if (peak_envelope > envelope_limit) {
    for (i = 0; i < MAX_BINS/2; i++) {
        float scale = (envelope[i] > 0) ? envelope_limit / peak_envelope : 1.0;
        output_tx[i] *= scale;
    }
}
```

#### Option 2: Frequency-Domain Envelope Control (Better)
```c
// In frequency domain, before IFFT (before line 1826)
// Apply Hilbert transform to generate analytic signal
// Then control magnitude while preserving phase
// Use complex signal for proper I/Q modulation
```

#### Option 3: Pre-IFFT Envelope Shaping (Most Elegant)
```c
// Apply Kaiser or Hann window to FFT result before IFFT
// Smooths envelope transitions naturally
// Reduces spectral splatter
```

### 9.3 Required New Functions

```c
// Hilbert transform for sideband quality
void hilbert_transform(complex float *signal, int length);

// Envelope detector
float detect_envelope(complex float *signal, int length);

// Envelope limiter with intelligent attack/release
void envelope_limit(complex float *signal, int length, float limit, float attack_ms);

// Peak-to-average power ratio (PAPR) measurement
float measure_papr(complex float *signal, int length);

// Pre-distortion for CESSB (optional)
void cessb_predistort(complex float *signal, int length, float target_papr);
```

---

## 10. Technical Parameters & Constants

| Parameter | Value | File | Notes |
|-----------|-------|------|-------|
| Sample Rate | 96000 Hz | sbitx_sound.c:144 | All processing at 96 kHz |
| FFT Size (MAX_BINS) | 2048 | sdr.h:81 | Frequency resolution: 46.875 Hz |
| Frequency Resolution | 46.875 Hz | Calculated | 96000 / 2048 |
| Block Size | 1024 samples | sbitx.c:707 | Processing block = 10.67 ms |
| TX Frequency Shift | 512 bins (~24 kHz) | sbitx.c:1809 | Adjustable via tx_shift |
| Filter Input Length (L) | 1024 | sbitx.c:727 | FFT length |
| Filter Impulse Length (M) | 1025 | sbitx.c:727 | Impulse response length |
| Kaiser Beta | 5.0 | sbitx.c:759 | Filter roll-off parameter |
| TX Audio Levels | int32_t ±2e9 | sbitx.c:1638 | Normalized to ±1.0 float |
| Output Scaling | volume * tx_amp * alc_level | sbitx.c:1833 | Dynamic range control |
| AGC Speed | 5-10 blocks | sbitx.c:267 | Fast attack, slow decay |
| Compression Ratio | 4:1 | sbitx.c:367 | Fixed ratio, configurable level |
| EQ Bands | 10 | para_eq.h | RX + TX independent |

---

## 11. Configuration Files

### 11.1 Hardware Settings
**Path:** `~/sbitx/data/hw_settings.ini`
- `bfo_freq`: BFO frequency (typically 40035000 Hz)
- `ssb_val`: SSB/CW Power Factor (W9JES)
- Band-specific power scaling

### 11.2 User Settings
**Path:** `~/sbitx/data/user_settings.ini`
- TX/RX EQ parameters (10 bands each)
- Default passband settings
- Compression control values

---

## 12. Audio Hardware Interface

### 12.1 ALSA Configuration
**File:** `/home/pi/sbitx/src/sbitx_sound.c`

- 4-channel setup:
  1. PCM Playback (RX speaker output)
  2. Loopback Capture (TX audio to FLDIGI)
  3. PCM Capture (RX input)
  4. Loopback Playback (TX input from browser)

- Buffer: 8192 bytes
- Periods: 2 per buffer
- Format: S32_LE (32-bit signed little-endian)

---

## 13. Summary of SSB Architecture

**Core Principle:** Frequency-domain single-sideband modulation with FFT-based processing

**Key Strengths:**
1. Efficient Kaiser-windowed FIR filtering
2. Clean sideband separation via frequency-domain zeroing
3. Flexible frequency shifting
4. Integrated AGC and compression
5. Parametric EQ for audio shaping
6. Real-time FFT processing at 96 kHz

**Ideal for CESSB because:**
1. Already uses complex I/Q in frequency domain
2. Can apply envelope control at multiple points
3. Flexible phase/magnitude manipulation
4. FFT allows spectral monitoring for PAPR reduction
5. Existing infrastructure for signal processing

---

## 14. Measurement & Monitoring Points

The following can be monitored for CESSB implementation:

1. **Envelope Peak:** Max of |output_tx[i]| across block
2. **Average Power:** RMS of output_tx block
3. **PAPR (Peak-to-Average Power Ratio):** Peak/Average
4. **Spectral Content:** Via FFT of output_tx
5. **Phase Continuity:** Arg(fft_time[i])
6. **Modulation Quality:** Via EVM (Error Vector Magnitude)

---

## 15. File Locations Summary

**Main Processing:**
- `/home/pi/sbitx/src/sbitx.c` - TX/RX DSP processing
- `/home/pi/sbitx/src/sbitx_sound.c` - Audio I/O

**Signal Processing:**
- `/home/pi/sbitx/src/fft_filter.c` - FFT-based filtering
- `/home/pi/sbitx/src/vfo.c` - Oscillator generation
- `/home/pi/sbitx/src/para_eq.c` - Parametric EQ
- `/home/pi/sbitx/src/modem_cw.c` - CW with envelope shaping

**Headers & Data:**
- `/home/pi/sbitx/src/sdr.h` - Main structures & defines

---

## Conclusion

The sBitx implements a sophisticated FFT-based SSB modulation system with excellent signal processing capabilities. The architecture is well-suited for CESSB implementation, with the main additions being:

1. Envelope detection from complex time-domain samples
2. Intelligent peak limiting with attack/release curves
3. Optional Hilbert transform for improved phase coherence
4. Real-time PAPR monitoring and adjustment

The existing frequency-domain processing pipeline provides multiple insertion points for envelope control without disrupting the core SSB modulation mechanism.

