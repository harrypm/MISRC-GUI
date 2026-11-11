/*
 * Private VHS monitor filter design. GPL-3.0, as the containing project.
 *
 * VHS/S-VHS parameter/profile and inverse high-shelf construction ported from
 * machcnz/vhs-decode-dotnet, VHSDecode.Core/Dsp/IirFilterDesign.cs:873-933
 * and Formats/format-params.snapshot.json (NTSC VHS and PAL/NTSC S-VHS
 * deemph_gain/mid/q).
 * The upstream Shelf is the parameterized RBJ second-order high shelf;
 * VideoDeEmphasisShelf swaps numerator and denominator before normalization.
 *
 * This helper designs that transfer at the supplied working rate. At 5MHz
 * it preserves DC, the physical midpoint and both shelf plateaus; bilinear
 * warping and phase differ from a design evaluated at 20MHz or 40MHz. There
 * is no extra rate multiplier, brightness gain, or zero-phase processing.
 * Ordinary PAL VHS keeps its existing filter. PAL S-VHS uses its matched
 * display shelf while retaining the legacy PAL sync filter.
 */
#ifndef GUI_RF_PREVIEW_FILTERS_H
#define GUI_RF_PREVIEW_FILTERS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Bitwise finite check remains valid with the production -ffast-math flags. */
static inline bool gui_rf_deemph_finite(double value)
{
    uint64_t bits;
    _Static_assert(sizeof(bits) == sizeof(value), "IEEE binary64 required");
    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}

/* ED's discriminator runs at 5 MSPS. Fold finite quadrants to [0,1]
 * with one division, then use an odd degree-17 minimax atan polynomial.
 * P(1)=pi/4 keeps the octant boundary continuous. Approximation error is
 * below 7e-9 rad (0.006 Hz at 5 MSPS), within the existing 2e-8 contract.
 * Keep the previous atan2f behavior for nonfinite or zero-vector input. */
static inline double gui_rf_preview_phase(double x, double z)
{
    if (!gui_rf_deemph_finite(x) || !gui_rf_deemph_finite(z) || (x == 0.0 && z == 0.0))
        return atan2f((float)x, (float)z);
    const double half_pi = 1.57079632679489661923;
    const double pi = 3.14159265358979323846;
    double a = fabs(x), b = fabs(z);
    double r = fmin(a,b)/fmax(a,b);
    double r2 = r*r;
    double r4=r2*r2, r8=r4*r4, r16=r8*r8;
    double p0=0.9999998810943069305+r2*(-0.3333256796434149738);
    double p1=0.1998544876555760907+r2*(-0.141580314737084545);
    double p2=0.1048723202627780543+r2*(-0.07210727034448327633);
    double p3=0.03950019139406944768+r2*(-0.01422876183184047967);
    double phase=r*((p0+r4*p1)+r8*(p2+r4*p3)+r16*(0.002413309547541135462));
    if (a > b) phase = half_pi-phase;
    if (z < 0.0) phase = pi-phase;
    return copysign(phase,x);
}

/* H(z) = (b0+b1*z^-1+b2*z^-2)/(1+a1*z^-1+a2*z^-2).
 * Outputs are untouched on failure. The monitor uses fs=5000000 Hz.
 * The finite upper bound avoids badly conditioned float-coefficient designs.
 * The unquantized transfer has exactly unity DC gain; float quantization at
 * 5MHz changes the VHS coefficients by about -1.40ppm.
 */
static inline bool gui_rf_deemph_coefficients(
    double fs, double gain_db, double midpoint, double q,
    float *b0, float *b1, float *b2, float *a1, float *a2)
{
    const double tau = 6.283185307179586476925286766559;
    if (!b0 || !b1 || !b2 || !a1 || !a2 || !gui_rf_deemph_finite(fs)
        || !gui_rf_deemph_finite(gain_db) || gain_db < 0.0 || gain_db > 40.0
        || !gui_rf_deemph_finite(midpoint) || midpoint <= 0.0
        || !gui_rf_deemph_finite(q) || q <= 0.0
        || fs <= 2.0*midpoint || fs > 80000000.0)
        return false;

    const double gain = pow(10.0, gain_db/40.0);
    const double omega = tau*midpoint/fs;
    const double cosine = cos(omega);
    const double alpha = sin(omega)/(2.0*q);
    const double root_gain = sqrt(gain);
    /* Original high-shelf numerator and denominator. */
    const double pre_b0 = gain*((gain+1.0)+(gain-1.0)*cosine+2.0*root_gain*alpha);
    const double pre_b1 = -2.0*gain*((gain-1.0)+(gain+1.0)*cosine);
    const double pre_b2 = gain*((gain+1.0)+(gain-1.0)*cosine-2.0*root_gain*alpha);
    const double pre_a0 = (gain+1.0)-(gain-1.0)*cosine+2.0*root_gain*alpha;
    const double pre_a1 = 2.0*((gain-1.0)-(gain+1.0)*cosine);
    const double pre_a2 = (gain+1.0)-(gain-1.0)*cosine-2.0*root_gain*alpha;
    /* Invert, then normalize the NEW denominator to a0=1. */
    const float cb0 = (float)(pre_a0/pre_b0);
    const float cb1 = (float)(pre_a1/pre_b0);
    const float cb2 = (float)(pre_a2/pre_b0);
    const float ca1 = (float)(pre_b1/pre_b0);
    const float ca2 = (float)(pre_b2/pre_b0);
    /* Jury conditions for the quantized real second-order denominator. */
    if (!gui_rf_deemph_finite(cb0) || !gui_rf_deemph_finite(cb1)
        || !gui_rf_deemph_finite(cb2) || !gui_rf_deemph_finite(ca1)
        || !gui_rf_deemph_finite(ca2)
        || fabs((double)ca2) >= 1.0 || 1.0+(double)ca1+ca2 <= 0.0
        || 1.0-(double)ca1+ca2 <= 0.0)
        return false;
    *b0=cb0; *b1=cb1; *b2=cb2; *a1=ca1; *a2=ca2;
    return true;
}

/* Preserve the NTSC VHS transfer exactly; Beta uses its own 8dB/100kHz/Q=.5
 * profile from the current .NET snapshot and DecodeFilterSet's default Q. */
static inline bool gui_rf_ntsc_deemph_coefficients(
    double fs, float *b0, float *b1, float *b2, float *a1, float *a2)
{
    return gui_rf_deemph_coefficients(fs,13.9794,273755.82,0.462088186,
                                      b0,b1,b2,a1,a2);
}

/* DC group delay of the actual float coefficients, in working-rate samples.
 * At 5MHz it is approximately 5.1486 samples (1.02972us). This is a
 * mathematical diagnostic, not a portable TBC/display lookahead. The shelf
 * is not linear phase; recording emphasis and the RF/FM/sync paths determine
 * effective edge alignment. Validate that full path before changing timing.
 * Caller must first obtain valid coefficients; invalid fs returns zero.
 */
static inline double gui_rf_ntsc_deemph_dc_delay_samples(double fs)
{
    float b0,b1,b2,a1,a2;
    if (!gui_rf_ntsc_deemph_coefficients(fs,&b0,&b1,&b2,&a1,&a2))
        return 0.0;
    return ((double)b1+2.0*b2)/((double)b0+b1+b2)
         - ((double)a1+2.0*a2)/(1.0+(double)a1+a2);
}

/* Steady TDF2 initialization for the existing rf_filter_sample recurrence:
 * input x0 is residual Hz = blank_hz-fm_center (not absolute carrier Hz).
 * y0 = x0*(b0+b1+b2)/(1+a1+a2);
 * z1 = y0-b0*x0; z2 = b2*x0-a2*y0.
 * VHS keeps the legacy first-order filter + 500kHz LP on the sync branch;
 * Beta sync uses its matched inverse shelf before that same low-pass.
 * Legacy sync delay retains its old first-order + low-pass contributions.
 * Do not add this isolated shelf DC delay to display/TBC sample positions.
 * Recording pre-emphasis, RF/FM processing and the selected sync reference
 * jointly determine edge timing; validate their complete path. In particular,
 * replacing a display filter does not prove its DC-delay difference is a
 * corresponding change in effective luma/chroma registration.
 */
#endif
