/*
 * CESSB - Controlled Envelope Single Sideband
 *
 * This module implements time-domain envelope clipping for CESSB.
 * It provides:
 * - Envelope detection from I/Q samples
 * - Soft clipping with adjustable threshold
 * - Low-pass filtering to reduce splatter
 * - Peak-to-average power ratio (PAPR) reduction
 */

#ifndef CESSB_H
#define CESSB_H

#include <complex.h>
#include <stdint.h>

// CESSB processor state
typedef struct {
    int enabled;                    // CESSB on/off
    float clip_threshold;           // Clipping threshold (0.5 - 1.0)
    float clip_ratio;               // Soft clipping ratio (1.0 - 10.0)

    // Filter state for splatter reduction
    complex float filter_state[8];  // IIR filter history
    int filter_order;               // Filter order

    // Statistics
    float peak_envelope;            // Peak envelope level
    float avg_envelope;             // Average envelope level
    float papr;                     // Peak-to-average power ratio (dB)
    int clip_count;                 // Number of clipped samples
} cessb_t;

// Initialize CESSB processor
void cessb_init(cessb_t *cessb);

// Process I/Q samples with CESSB
// in_i, in_q: input I/Q samples
// out_i, out_q: output I/Q samples
// n_samples: number of samples to process
void cessb_process(cessb_t *cessb,
                   const complex float *in,
                   complex float *out,
                   int n_samples);

// Enable/disable CESSB
void cessb_set_enabled(cessb_t *cessb, int enabled);

// Set clipping threshold (0.5 - 1.0, default 0.8)
// Lower values = more compression, higher PAPR reduction
void cessb_set_threshold(cessb_t *cessb, float threshold);

// Set clipping ratio (1.0 - 10.0, default 3.0)
// Higher values = softer clipping, less distortion
void cessb_set_clip_ratio(cessb_t *cessb, float ratio);

// Get PAPR in dB
float cessb_get_papr(cessb_t *cessb);

// Reset statistics
void cessb_reset_stats(cessb_t *cessb);

#endif // CESSB_H
