/*
 * CESSB - Controlled Envelope Single Sideband
 * Time-domain envelope clipping implementation
 */

#include "cessb.h"
#include <math.h>
#include <string.h>

// Soft clipping function using tanh
static inline float soft_clip(float x, float threshold, float ratio) {
    if (fabsf(x) <= threshold) {
        return x;
    }

    // Soft clipping using tanh
    float excess = fabsf(x) - threshold;
    float clipped = threshold + tanhf(excess / ratio) * ratio;

    return (x > 0) ? clipped : -clipped;
}

// Initialize CESSB processor
void cessb_init(cessb_t *cessb) {
    memset(cessb, 0, sizeof(cessb_t));

    cessb->enabled = 0;              // Disabled by default
    cessb->clip_threshold = 0.8;     // 80% of maximum
    cessb->clip_ratio = 3.0;         // Moderate soft clipping
    cessb->filter_order = 4;         // 4th order filter

    // Initialize filter state
    for (int i = 0; i < 8; i++) {
        cessb->filter_state[i] = 0;
    }
}

// Low-pass filter to reduce splatter from clipping
// 4th order Butterworth, cutoff ~6 kHz at 96 kHz sample rate
static complex float lowpass_filter(cessb_t *cessb, complex float in) {
    // Butterworth coefficients for fc = 6kHz, fs = 96kHz
    // Generated using scipy: butter(4, 6000/(96000/2), 'low')
    const float b[] = {0.000103, 0.000412, 0.000618, 0.000412, 0.000103};
    const float a[] = {1.000000, -3.580497, 4.838237, -2.918969, 0.661842};

    // Shift filter state
    for (int i = 7; i > 0; i--) {
        cessb->filter_state[i] = cessb->filter_state[i-1];
    }
    cessb->filter_state[0] = in;

    // Apply filter (Direct Form II)
    complex float out = 0;
    for (int i = 0; i < 5; i++) {
        if (i < 5) out += b[i] * cessb->filter_state[i];
        if (i > 0 && i < 5) out -= a[i] * cessb->filter_state[i+3];
    }

    return out;
}

// Process I/Q samples with CESSB
void cessb_process(cessb_t *cessb,
                   const complex float *in,
                   complex float *out,
                   int n_samples) {

    if (!cessb->enabled) {
        // Bypass - just copy input to output
        memcpy(out, in, n_samples * sizeof(complex float));
        return;
    }

    float peak = 0;
    float sum = 0;
    cessb->clip_count = 0;

    for (int i = 0; i < n_samples; i++) {
        complex float sample = in[i];

        // Calculate envelope magnitude
        float envelope = cabsf(sample);

        // Track statistics
        if (envelope > peak) peak = envelope;
        sum += envelope * envelope;

        // Apply soft clipping to envelope
        if (envelope > cessb->clip_threshold) {
            float clipped_envelope = soft_clip(envelope,
                                               cessb->clip_threshold,
                                               cessb->clip_ratio);

            // Scale the I/Q sample to match clipped envelope
            if (envelope > 1e-6) {
                sample *= clipped_envelope / envelope;
            }

            cessb->clip_count++;
        }

        // Apply low-pass filter to reduce splatter
        out[i] = lowpass_filter(cessb, sample);
    }

    // Update statistics
    cessb->peak_envelope = peak;
    cessb->avg_envelope = sqrtf(sum / n_samples);

    // Calculate PAPR in dB
    if (cessb->avg_envelope > 1e-6) {
        cessb->papr = 20.0f * log10f(cessb->peak_envelope / cessb->avg_envelope);
    } else {
        cessb->papr = 0;
    }
}

// Enable/disable CESSB
void cessb_set_enabled(cessb_t *cessb, int enabled) {
    cessb->enabled = enabled;

    // Reset filter state when enabling/disabling
    if (!enabled) {
        for (int i = 0; i < 8; i++) {
            cessb->filter_state[i] = 0;
        }
    }
}

// Set clipping threshold
void cessb_set_threshold(cessb_t *cessb, float threshold) {
    // Clamp to valid range
    if (threshold < 0.5) threshold = 0.5;
    if (threshold > 1.0) threshold = 1.0;

    cessb->clip_threshold = threshold;
}

// Set clipping ratio
void cessb_set_clip_ratio(cessb_t *cessb, float ratio) {
    // Clamp to valid range
    if (ratio < 1.0) ratio = 1.0;
    if (ratio > 10.0) ratio = 10.0;

    cessb->clip_ratio = ratio;
}

// Get PAPR in dB
float cessb_get_papr(cessb_t *cessb) {
    return cessb->papr;
}

// Reset statistics
void cessb_reset_stats(cessb_t *cessb) {
    cessb->peak_envelope = 0;
    cessb->avg_envelope = 0;
    cessb->papr = 0;
    cessb->clip_count = 0;
}
