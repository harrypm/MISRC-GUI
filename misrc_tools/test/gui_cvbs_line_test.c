// Line placement stability of the Video panel's CVBS decode.
//
// An analytic composite with EXACT line timing (625/50 and 525/59.94, the
// half-line-slot VBI, raised-cosine edges, a burst and chroma on alternate
// bars) goes through the real panel path (soxr to the fixed decode rate,
// gui_cvbs decode) at 40 MSPS, and the decoded field's vertical bar edges
// are located per line at their half-level crossing. Every edge must sit
// on a straight vertical line: line-to-line rms and field peak-to-peak in
// output pixels.
//
// Measured before the sub-sample placement (2026-11-15): the whole-sample
// line wrap put each line 0..1 sample off and the pixel scale followed the
// buffer's 908/909 count, so every edge sawtoothed ~1 px p2p with 0.4-1.1
// px line-to-line on all systems. After: ~0.01 px.
#include "../misrc_gui/signal/gui_cvbs.c"
#include "../misrc_gui/signal/gui_testgen.h"
#include <assert.h>

void UnloadTexture(Texture2D texture) { (void)texture; }
void TraceLog(int logLevel, const char *text, ...) { (void)logLevel; (void)text; }

#define RATE 40000000u

static int failures;

// --- analytic composite --------------------------------------------------
typedef struct {
    int lines625;
    double L;            // line period (s): exactly 227.5 / fsc for 525
    double fsc;
    int slots, eqn, hpar;
} src_t;

static double rc_step(double x, double r) {
    if (x <= -0.5 * r) return 0.0;
    if (x >= 0.5 * r) return 1.0;
    return 0.5 + 0.5 * sin(M_PI * x / r);
}

static void src_init(src_t *s, int lines625) {
    s->lines625 = lines625;
    s->fsc = lines625 ? 4433618.75 : 315e6 / 88.0;
    s->L = lines625 ? 64e-6 : 227.5 / s->fsc;
    s->slots = lines625 ? 1250 : 1050;
    s->eqn = lines625 ? 5 : 6;
    s->hpar = lines625 ? 1 : 0;
}

// Bars: twelve 4 us bars from 12 us, levels 0.6 / 0.1 alternating; the 0.1
// bars carry chroma (U axis, amplitude 0.15) under the same envelope.
#define NBAR 12
static double bar_level(int b) { return (b < 0 || b >= NBAR) ? 0.0 : ((b & 1) ? 0.1 : 0.6); }
static double bar_chroma(int b) { return (b < 0 || b >= NBAR) ? 0.0 : ((b & 1) ? 0.15 : 0.0); }

static int g_no_chroma;   // CVBS_LINE_NOCHROMA=1: luma-only source (diagnostic)

static double src_sample(const src_t *s, double t) {
    const double S = s->L * 0.5, r = 0.2e-6, sync = -0.3, t_sync = 0.6e-6;
    const int mid = s->slots / 2;
    double ft = fmod(t, (double)s->slots * S);
    int sl = (int)(ft / S);
    double tau = ft - (double)sl * S;
    // Which H line (if any) covers this slot: an H slot or the slot after it.
    int hs = -1;
    for (int d = 0; d <= 1; d++) {
        int q = sl - d;
        if (q < 0) q += s->slots;
        int k = q < mid ? q : q - mid;
        if (k < 3 * s->eqn) continue;          // VBI train slot
        if ((q & 1) == s->hpar) { hs = q; tau += d * S; break; }
    }
    if (hs < 0) {
        // VBI train: EQ (2.35 us) / broad (27.3 us) / EQ, one pulse per slot.
        int k = sl < mid ? sl : sl - mid;
        if (k >= 3 * s->eqn) return 0.0;       // blank slot
        double w = (k >= s->eqn && k < 2 * s->eqn) ? 27.3e-6 : 2.35e-6;
        return sync * (rc_step(tau - t_sync, r) - rc_step(tau - t_sync - w, r));
    }
    double v = sync * (rc_step(tau - t_sync, r) - rc_step(tau - t_sync - 4.7e-6, r));
    const double carrier = 2.0 * M_PI * s->fsc * t;
    // Burst 5.6..8.0 us (nominal 9 cycles): NTSC 180 deg; PAL +-45 deg about
    // 180 deg alternating by line so the decoder's V-switch chain sees the
    // +-90 deg steps.
    if (!g_no_chroma) {
        double env = rc_step(tau - 5.6e-6, 0.3e-6) - rc_step(tau - 8.0e-6, 0.3e-6);
        if (env > 0.0) {
            double ph = M_PI;
            if (s->lines625) ph += ((hs >> 1) & 1) ? 0.25 * M_PI : -0.25 * M_PI;
            v += 0.143 * env * sin(carrier + ph);
        }
    }
    // Bars.
    double x = tau - 12e-6;
    int b = (int)floor(x / 4e-6);
    if (b >= -1 && b <= NBAR) {
        double left = b * 4e-6, right = left + 4e-6;
        double el = rc_step(x - left, r), er = rc_step(x - right, r);
        double lum = bar_level(b - 1) + (bar_level(b) - bar_level(b - 1)) * el
                   + (bar_level(b + 1) - bar_level(b)) * er;
        double chr = bar_chroma(b - 1) + (bar_chroma(b) - bar_chroma(b - 1)) * el
                   + (bar_chroma(b + 1) - bar_chroma(b)) * er;
        v += lum;
        if (chr != 0.0 && !g_no_chroma) v += chr * sin(carrier);
    }
    return v;
}

// --- measurement ---------------------------------------------------------
static double luma(const uint8_t *p) { return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; }

// Vertical bar edges of field `fi`, rows [H/6, H/2): mean line-to-line rms
// and mean peak-to-peak of the half-level crossing, in pixels.
static bool measure(const cvbs_decoder_t *d, int fi, double *ll_rms, double *p2p, int *nedges) {
    int W = d->frame_width, H = d->field_line_count[fi];
    const uint8_t *fb = d->field_buffer[fi];
    // A near-complete field only: the rows [H/6, H/2) must lie inside the
    // picture rows. A partial field (H ~ 100) put the window on the rows
    // above picture_start_line, which render through the hybrid VBI
    // mapping (plateaus clipped, so the half-level crossing moves ~0.2 px -
    // a contrast effect, not timing).
    if (!fb || H < 200) return false;
    double *y = malloc(sizeof(double) * (size_t)W);
    int r0 = H / 6, r1 = H / 2, ref = (r0 + r1) / 2;
    for (int x = 0; x < W; x++) y[x] = luma(fb + ((size_t)ref * W + x) * 3);
    int cand[24], nc = 0;
    for (int x = 8; x < W - 8 && nc < 24; x++) {
        double g = y[x + 1] - y[x - 1];
        if (fabs(g) > 40 && fabs(g) >= fabs(y[x] - y[x - 2]) && fabs(g) >= fabs(y[x + 2] - y[x])) {
            cand[nc++] = x;
            x += 6;
        }
    }
    double tot_ll = 0, tot_p2p = 0;
    int used = 0;
    double *pos = malloc(sizeof(double) * (size_t)(r1 - r0));
    for (int c = 0; c < nc; c++) {
        int np = 0;
        for (int r = r0; r < r1; r++) {
            const uint8_t *row = fb + (size_t)r * W * 3;
            double a = 0, b = 0;
            for (int k = 4; k < 8; k++) { a += luma(row + (cand[c] - k) * 3); b += luma(row + (cand[c] + k) * 3); }
            a /= 4; b /= 4;
            if (fabs(b - a) < 20) continue;
            double mid = 0.5 * (a + b), found = -1;
            for (int x = cand[c] - 4; x < cand[c] + 4; x++) {
                double u = luma(row + x * 3) - mid, v = luma(row + (x + 1) * 3) - mid;
                if ((u <= 0 && v > 0) || (u >= 0 && v < 0)) { found = x + u / (u - v); break; }
            }
            if (found >= 0) pos[np++] = found;
        }
        if (np < (r1 - r0) / 2) continue;
        double mn = 1e9, mx = -1e9, d2 = 0;
        for (int i = 0; i < np; i++) { if (pos[i] < mn) mn = pos[i]; if (pos[i] > mx) mx = pos[i]; }
        for (int i = 1; i < np; i++) d2 += (pos[i] - pos[i - 1]) * (pos[i] - pos[i - 1]);
        tot_ll += sqrt(d2 / (np - 1));
        tot_p2p += mx - mn;
        used++;
    }
    free(pos);
    free(y);
    if (!used) return false;
    *ll_rms = tot_ll / used;
    *p2p = tot_p2p / used;
    *nedges = used;
    return true;
}

// --- one run -------------------------------------------------------------
static void run(const char *name, int lines625, int system_select, double ppm,
                int chroma, double max_ll, double max_p2p) {
    src_t s;
    src_init(&s, lines625);
    g_no_chroma = !chroma;
    gui_cvbs_feed_t *feed = gui_cvbs_feed_create();
    gui_cvbs_feed_set_system(feed, system_select);
    cvbs_decoder_t *dec = gui_cvbs_feed_decoder(feed);

    const char *e = getenv("CVBS_LINE_SECS");
    double secs = e ? atof(e) : 1.1;
    const double settle = secs - 0.3;
    const double step = 1.0 + ppm * 1e-6;
    enum { CH = 4096 };
    static int16_t buf[CH];
    size_t total = 0, limit = (size_t)(secs * RATE);
    size_t next = (size_t)(settle * RATE);
    double worst_ll = 0, worst_p2p = 0;
    int snaps = 0, edges = 0;
    while (total < limit) {
        for (int i = 0; i < CH; i++) {
            double t = (double)(total + (size_t)i) / (double)RATE * step;
            buf[i] = (int16_t)lrint(src_sample(&s, t) * 2170.0);
        }
        gui_cvbs_feed_process(feed, buf, CH, RATE);
        total += CH;
        if (total >= next) {
            next += RATE / 10;
            for (int fi = 0; fi < 2; fi++) {
                double ll, p2p;
                int n;
                if (measure(dec, fi, &ll, &p2p, &n)) {
                    if (getenv("CVBS_LINE_VERBOSE"))
                        printf("  snap t=%.2f fi=%d H=%d edges=%d ll=%.3f p2p=%.3f\n",
                               (double)total / (double)RATE, fi, dec->field_line_count[fi], n, ll, p2p);
                    if (ll > worst_ll) worst_ll = ll;
                    if (p2p > worst_p2p) worst_p2p = p2p;
                    edges = n;
                    snaps++;
                }
            }
        }
    }
    if ((e = getenv("CVBS_LINE_RGB"))) {   // diagnostic: field 0 as P6
        FILE *o = fopen(e, "wb");
        if (o) {
            fprintf(o, "P6 %d %d 255\n", dec->frame_width, dec->field_line_count[0]);
            fwrite(dec->field_buffer[0], 3, (size_t)dec->frame_width * (size_t)dec->field_line_count[0], o);
            fclose(o);
        }
    }
    bool ok = dec->pll.locked && snaps >= 4 && edges >= 6 &&
              worst_ll <= max_ll && worst_p2p <= max_p2p;
    printf("%-28s locked=%d snaps=%d edges=%d  line-to-line %.3f px (max %.2f)  field p2p %.3f px (max %.2f)  %s\n",
           name, dec->pll.locked, snaps, edges, worst_ll, max_ll, worst_p2p, max_p2p,
           ok ? "ok" : "FAIL");
    if (!ok) failures++;
    gui_cvbs_feed_destroy(feed);
}

int main(void) {
    // Diagnostic knobs (never set in the suite).
    const char *e;
    if ((e = getenv("CVBS_LINE_COMB"))) g_cvbs_pic_comb = atoi(e) != 0;
    if ((e = getenv("CVBS_LINE_SUBLINE"))) g_cvbs_subsample_line = atoi(e) != 0;
    if ((e = getenv("CVBS_LINE_ONLY"))) {   // one case: "ntsc" / "pal" (+ "c": chroma)
        bool pal = e[0] == 'p';
        bool chroma = strchr(e, 'c') != NULL;
        run(pal ? "625 only" : "525 only", pal, pal ? 0 : 1, 0.0, chroma,
            chroma ? 0.2 : 0.05, chroma ? 0.45 : 0.15);
        return failures ? 1 : 0;
    }
    // Luma-only sources: the line timing itself. Thresholds a tenth of the
    // pre-fix sawtooth, well above the measured residual (~0.01 px
    // line-to-line, ~0.03 px p2p).
    run("525 luma exact clock", 0, 1, 0.0, 0, 0.05, 0.15);
    run("525 luma +60 ppm", 0, 1, 60.0, 0, 0.05, 0.15);
    run("625 luma exact clock", 1, 0, 0.0, 0, 0.05, 0.15);
    run("625 luma -60 ppm", 1, 0, -60.0, 0, 0.05, 0.15);
    run("625 luma as SECAM decode", 1, 2, 0.0, 0, 0.05, 0.15);
    // Burst + chroma on alternate bars: the colour path (comb, bandpass,
    // luma = composite - chroma, box windows). Measured ~0.10 px line-to-line:
    // chroma-to-luma crosstalk where a chroma edge coincides with a luma
    // edge (the bandpass passes only part of the chroma envelope's
    // sidebands; the leftover in the luma alternates with the carrier's
    // per-line phase). Before the placement fix: 0.3-0.4 px line-to-line,
    // ~1 px p2p, plus the alternating hue fringes.
    run("525 NTSC colour exact clock", 0, 1, 0.0, 1, 0.2, 0.45);
    run("625 PAL colour exact clock", 1, 0, 0.0, 1, 0.2, 0.45);
    run("625 PAL colour -60 ppm", 1, 0, -60.0, 1, 0.2, 0.45);
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
