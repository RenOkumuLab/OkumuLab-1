/*
 * labium_core.c — Prinzipal flue-pipe physical model (Phase 1 prototype core)
 *
 * Jet-drive model (Verge / Fabre / Hirschberg) coupled to a bidirectional
 * digital waveguide resonator, with a pipe-foot pressure model (toe hole
 * throttling + foot cavity filling) and a dispersive mouth end correction.
 *
 * All parameters are given at control rate (every `ctrl` samples) and are
 * linearly interpolated per sample, so every one of them can be modulated.
 * This file is written so that it can be ported almost 1:1 to the C++
 * plugin core in Phase 3.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum {
    P_GATE = 0,   /* pallet open (0..1)                                   */
    P_PCHEST,     /* windchest pressure [Pa]                              */
    P_PALLET_TAU, /* pallet time constant [s]                             */
    P_D,          /* pipe inner diameter [m]                              */
    P_LPHYS,      /* physical resonator length (mouth to top) [m]         */
    P_MOUTHW,     /* mouth width H [m]                                    */
    P_CUTUP,      /* cut-up W [m]                                         */
    P_FLUE,       /* flueway depth h [m]                                  */
    P_TOE,        /* toe hole diameter [m]                                */
    P_FOOTVOL,    /* foot cavity volume [m^3]                             */
    P_Y0,         /* labium offset relative to jet centre [m]             */
    P_NOISE,      /* jet turbulence level (x h)                           */
    P_KAPPA,      /* jet convection velocity / jet velocity               */
    P_JETGAIN,    /* jet receptivity gain multiplier                      */
    P_MORPH,      /* top end: 0 = open, 1 = stopped                       */
    P_C,          /* speed of sound [m/s]                                 */
    P_RHO,        /* gas density [kg/m^3]                                 */
    P_GAMMA,      /* ratio of specific heats                              */
    P_MU,         /* dynamic viscosity [Pa s]                             */
    P_LOSS,       /* wall-loss multiplier                                 */
    P_FMF,        /* wind FM frequency [Hz]                               */
    P_FMD,        /* wind FM depth (fraction of pressure)                 */
    P_PLOCK,      /* pitch lock amount 0..1                               */
    P_FTARGET,    /* pitch-lock target frequency [Hz]                     */
    P_KCAL,       /* pitch-lock calibration factor                        */
    P_AMPCAP,     /* cap on jet spatial amplification exponent            */
    P_EDGE,       /* edge-tone (hydrodynamic jet-labium feedback) gain     */
    P_JETBW,      /* jet receptivity bandwidth: f_cut = jetbw * Uj / h     */
    P_ONSET_B,    /* laminar-onset boost of jet amplification exponent     */
    P_ONSET_T,    /* decay of the onset boost [periods of fres]            */
    P_KICK,       /* starting-vortex pressure pulse (x foot pressure)      */
    P_VFOLLOW,    /* voicing follow: cut-up tracks the resonance (0..1)    */
    P_FDESIGN,    /* frequency the voicing was designed for [Hz]           */
    NPAR
};

typedef struct { double *buf; int mask; int w; } DL;

static int dl_init(DL *d, int minlen) {
    int n = 1;
    while (n < minlen) n <<= 1;
    d->buf = (double *)calloc((size_t)n, sizeof(double));
    d->mask = n - 1;
    d->w = 0;
    return d->buf != NULL;
}
static void dl_free(DL *d) { free(d->buf); d->buf = NULL; }
static inline void dl_write(DL *d, double x) { d->buf[d->w] = x; d->w = (d->w + 1) & d->mask; }

/* read a value written D samples ago (call before this sample's write), D >= 3 */
static inline double dl_read(const DL *d, double D) {
    double pos = (double)d->w - D;
    double fl = floor(pos);
    int i = (int)fl;
    double f = pos - fl;
    double ym1 = d->buf[(i - 1) & d->mask];
    double y0 = d->buf[i & d->mask];
    double y1 = d->buf[(i + 1) & d->mask];
    double y2 = d->buf[(i + 2) & d->mask];
    /* 3rd-order Lagrange interpolation */
    double cm1 = -f * (f - 1.0) * (f - 2.0) / 6.0;
    double c0 = (f + 1.0) * (f - 1.0) * (f - 2.0) / 2.0;
    double c1 = -(f + 1.0) * f * (f - 2.0) / 2.0;
    double c2 = (f + 1.0) * f * (f - 1.0) / 6.0;
    return cm1 * ym1 + c0 * y0 + c1 * y1 + c2 * y2;
}

/* linear read relative to the most recent write (used only for snapshots) */
static inline double dl_read_after(const DL *d, double D) {
    double pos = (double)(d->w - 1) - D;
    double fl = floor(pos);
    int i = (int)fl;
    double f = pos - fl;
    return d->buf[i & d->mask] * (1.0 - f) + d->buf[(i + 1) & d->mask] * f;
}

static inline double sgn(double x) { return (x > 0) - (x < 0); }

static uint64_t rng_state;
static inline double rnd_uniform(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (double)(rng_state >> 11) * (1.0 / 9007199254740992.0);
}

/* derived quantities recomputed once per control frame */
typedef struct {
    double dline;      /* one-way waveguide delay [samples]  */
    double lb0, lb1, la1; /* round-trip loss: first-order shelf (b0 + b1 z^-1)/(1 + a1 z^-1) */
    double lp_delay;   /* its phase delay at f1 [samples] */
    double taum;       /* mouth inertance time constant M/c [s] */
    double a_morph;    /* top-end open/stopped allpass       */
    double M;          /* mouth end correction [m]           */
    double Leff;       /* acoustic length [m]                */
    double fres;       /* nominal fundamental resonance [Hz] */
    double W;          /* effective cut-up (after voicing follow) [m] */
} Derived;

static double loss_gain(double f, double a, double am, double Leff, double c, double rho,
                        double gam, double mu, double loss, double morph) {
    const double Pr = 0.71;
    double w = 2.0 * M_PI * f;
    double nu = mu / rho;
    double alpha = sqrt(w * nu / 2.0) / (a * c) * (1.0 + (gam - 1.0) / sqrt(Pr));
    double k = w / c;
    double rad_top = 0.5 * (k * a) * (k * a) * (1.0 - morph);
    double rad_mouth = 0.5 * (k * am) * (k * am);
    return exp(-2.0 * alpha * Leff * loss - rad_top - rad_mouth);
}

static void derive(const double *p, double fs, Derived *dv) {
    double c = p[P_C], rho = p[P_RHO];
    double d = p[P_D], a = 0.5 * d;
    double H = p[P_MOUTHW], W = p[P_CUTUP];
    double morph = p[P_MORPH];
    if (morph < 0) morph = 0;
    if (morph > 1) morph = 1;

    double M = 0, Leff = 0, fres = 0;
    for (int pass = 0; pass < 2; ++pass) {
        /* mouth end correction: M ~ 0.3 d^2 / W  (Fletcher: M ~ 1.2 d^2 / H for W = H/4) */
        M = 0.3 * d * d / W;
        double Lphys_eff = p[P_LPHYS] + M + 0.3 * d * (1.0 - morph);
        double Llock = c / (2.0 * p[P_FTARGET]) * p[P_KCAL];
        Leff = (1.0 - p[P_PLOCK]) * Lphys_eff + p[P_PLOCK] * Llock;
        if (Leff < 0.005) Leff = 0.005;
        fres = c / (2.0 * Leff) * (1.0 - 0.5 * morph);
        /* voicing follow: keep the jet transit / period ratio (Ising number) as the
           resonance moves: W ~ f^(-2/3) for the dispersive jet */
        if (pass == 0 && p[P_VFOLLOW] > 0 && p[P_FDESIGN] > 0)
            W = W * pow(fres / p[P_FDESIGN], -2.0 / 3.0 * p[P_VFOLLOW]);
        else
            break;
    }
    dv->W = W;

    /* loss filter: first-order shelf fitted exactly to the physical round-trip loss
       (visco-thermal ~ sqrt(f), radiation ~ f^2) at f1, 3 f1 and 9 f1 */
    double am = sqrt(H * W / M_PI);
    double f1 = c / (2.0 * Leff);
    double fa[3] = {f1, 3.0 * f1, 9.0 * f1};
    for (int i = 0; i < 3; ++i) if (fa[i] > 0.4 * fs) fa[i] = 0.4 * fs * (0.6 + 0.2 * i);
    double Gt[3], cw[3];
    for (int i = 0; i < 3; ++i) {
        Gt[i] = loss_gain(fa[i], a, am, Leff, c, rho, p[P_GAMMA], p[P_MU], p[P_LOSS], morph);
        cw[i] = cos(2.0 * M_PI * fa[i] / fs);
    }
    /* for a given a1: |H|^2 (1 + a1^2 + 2 a1 cw) = u + v cw  (linear in u, v) */
    double best_a1 = 0.0, best_u = Gt[0] * Gt[0], best_v = 0.0, best_r = 1e30;
    double lo = -0.9995, hi = 0.9995;
    double rlo = 0, rhi = 0;
    for (int pass = 0; pass < 2; ++pass) {
        /* coarse scan then bisection on the residual of the third point */
        double prev_a = lo, prev_r = 0;
        int found = 0;
        for (int k = 0; k <= 200 && !found; ++k) {
            double a1 = lo + (hi - lo) * k / 200.0;
            double y0 = Gt[0] * Gt[0] * (1 + a1 * a1 + 2 * a1 * cw[0]);
            double y1 = Gt[1] * Gt[1] * (1 + a1 * a1 + 2 * a1 * cw[1]);
            double v = (y0 - y1) / (cw[0] - cw[1]);
            double u = y0 - v * cw[0];
            double y2 = Gt[2] * Gt[2] * (1 + a1 * a1 + 2 * a1 * cw[2]);
            double r = (u + v * cw[2]) - y2;
            if (u >= fabs(v) && fabs(r) < best_r) { best_r = fabs(r); best_a1 = a1; best_u = u; best_v = v; }
            if (k > 0 && prev_r * r < 0) { rlo = prev_a; rhi = a1; found = 1; }
            prev_a = a1; prev_r = r;
        }
        if (!found) break;
        for (int it = 0; it < 50; ++it) {
            double a1 = 0.5 * (rlo + rhi);
            double y0 = Gt[0] * Gt[0] * (1 + a1 * a1 + 2 * a1 * cw[0]);
            double y1 = Gt[1] * Gt[1] * (1 + a1 * a1 + 2 * a1 * cw[1]);
            double v = (y0 - y1) / (cw[0] - cw[1]);
            double u = y0 - v * cw[0];
            double y2 = Gt[2] * Gt[2] * (1 + a1 * a1 + 2 * a1 * cw[2]);
            double r = (u + v * cw[2]) - y2;
            double ylo = Gt[2] * Gt[2] * (1 + rlo * rlo + 2 * rlo * cw[2]);
            double y0l = Gt[0] * Gt[0] * (1 + rlo * rlo + 2 * rlo * cw[0]);
            double y1l = Gt[1] * Gt[1] * (1 + rlo * rlo + 2 * rlo * cw[1]);
            double vl = (y0l - y1l) / (cw[0] - cw[1]);
            double ul = y0l - vl * cw[0];
            double rl = (ul + vl * cw[2]) - ylo;
            if (rl * r <= 0) rhi = a1; else rlo = a1;
            if (u >= fabs(v)) { best_a1 = a1; best_u = u; best_v = v; best_r = fabs(r); }
        }
        break;
    }
    double sp = sqrt(fmax(best_u + best_v, 0.0)), sm = sqrt(fmax(best_u - best_v, 0.0));
    dv->lb0 = 0.5 * (sp + sm);
    dv->lb1 = 0.5 * (sp - sm);
    dv->la1 = best_a1;
    /* phase delay at f1 (samples) */
    {
        double w = 2.0 * M_PI * f1 / fs;
        double nr = dv->lb0 + dv->lb1 * cos(w), ni = -dv->lb1 * sin(w);
        double dr = 1.0 + dv->la1 * cos(w), di = -dv->la1 * sin(w);
        double ph = atan2(ni, nr) - atan2(di, dr);
        dv->lp_delay = -ph / w;
    }
    /* mouth window = lumped inertance (time constant M/c). Its reflection
       -(1 - jw tau)/(1 + jw tau) adds 2M of delay at low frequency and less at
       high frequency (Fletcher: the mouth correction falls with frequency). */
    dv->taum = M / c;

    /* open <-> stopped morph: crossover frequency from Nyquist down to 5 Hz */
    double fx = 0.5 * fs * pow(5.0 / (0.5 * fs), morph);
    double t = tan(fmin(M_PI * fx / fs, M_PI / 2.0 - 1e-9));
    dv->a_morph = (t - 1.0) / (t + 1.0);

    /* one-way delay: round trip = 2*Leff, minus mouth inertance (2M) and lowpass delay */
    double D = ((2.0 * Leff - 2.0 * M) * fs / c - dv->lp_delay) / 2.0;
    if (D < 3.0) D = 3.0;
    dv->dline = D;
    dv->M = M;
    dv->Leff = Leff;
    dv->fres = fres;
}

/*
 * Render one pipe voice.
 *   params   : nc x NPAR control frames (row-major), frame i applies at sample i*ctrl
 *   outputs  : per-sample arrays (any may be NULL)
 *   snapshots: at sample indices snap_idx[k], pressure / particle velocity along the
 *              bore at nx points (mouth -> top) are stored in snap_p / snap_u [k*nx + i]
 * returns the number of instability resets (0 = clean)
 */
int labium_render(int n, double fs, int ctrl, int nc, const double *params, unsigned int seed,
                  double *out_mouth, double *out_top, double *out_eta, double *out_vm,
                  double *out_pf, double *out_uj, double *out_ps,
                  int nsnap, const int *snap_idx, int nx, double *snap_p, double *snap_u,
                  double *snap_info)
{
    rng_state = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)seed * 0xBF58476D1CE4E5B9ULL);
    if (rng_state == 0) rng_state = 1;

    /* find the longest delays needed */
    double maxD = 16.0, maxTau = 16.0;
    for (int i = 0; i < nc; ++i) {
        Derived dv;
        derive(params + (size_t)i * NPAR, fs, &dv);
        if (dv.dline > maxD) maxD = dv.dline;
        double c = params[(size_t)i * NPAR + P_C];
        (void)c;
        double W = params[(size_t)i * NPAR + P_CUTUP];
        double kap = params[(size_t)i * NPAR + P_KAPPA];
        double tau = 4.0 * W / (kap * 0.2) * fs; /* worst case: very slow jet, follow up to x4 */
        if (tau > maxTau) maxTau = tau;
    }
    if (maxTau > fs * 0.25) maxTau = fs * 0.25;

    DL up = {0}, lo = {0}, vml = {0};
    if (!dl_init(&up, (int)maxD + 16) || !dl_init(&lo, (int)maxD + 16) || !dl_init(&vml, (int)maxTau + 16)) {
        dl_free(&up); dl_free(&lo); dl_free(&vml);
        return -1;
    }

    double cur[NPAR];
    Derived dv0, dv1;
    derive(params, fs, &dv0);
    dv1 = dv0;

    double pg = 0.0, pf = 0.0;
    double qin_prev = 0.0, dq_lp = 0.0;
    double vm = 0.0;
    double lp_y = 0.0;
    double lsh_x1 = 0.0, lsh_y1 = 0.0, dc_x1 = 0.0, dc_y1 = 0.0;
    double apm_x1 = 0.0, apm_y1 = 0.0;
    double mq_prev = 0.0, mu_prev = 0.0;  /* mouth inertance state */
    double vj = 0.0, vj2 = 0.0;           /* jet receptivity low-pass state */
    int gate_on = 0;
    double onset_t = 0.0;
    double kick_t = -1.0;
    double um_prev = 0.0, ut_prev = 0.0;
    double nz = 0.0;
    double phi = 0.0;
    int resets = 0;
    int snap_k = 0;

    const double k_dq = 1.0 - exp(-2.0 * M_PI * 12000.0 / fs);
    const double k_nz = 1.0 - exp(-2.0 * M_PI * 4000.0 / fs);

    for (int t = 0; t < n; ++t) {
        int fi = t / ctrl;
        double fr = (double)(t - fi * ctrl) / (double)ctrl;
        const double *pa = params + (size_t)(fi < nc ? fi : nc - 1) * NPAR;
        const double *pb = params + (size_t)(fi + 1 < nc ? fi + 1 : nc - 1) * NPAR;
        for (int j = 0; j < NPAR; ++j) cur[j] = pa[j] + (pb[j] - pa[j]) * fr;

        if (t % ctrl == 0) {
            dv0 = dv1;
            derive(pb, fs, &dv1);
            if (t == 0) derive(pa, fs, &dv0);
        }
        double dline = dv0.dline + (dv1.dline - dv0.dline) * fr;

        const double c = cur[P_C], rho = cur[P_RHO];
        const double d = cur[P_D];
        const double H = cur[P_MOUTHW], h = cur[P_FLUE];
        const double W = dv0.W + (dv1.W - dv0.W) * fr;
        const double Spipe = M_PI * d * d / 4.0;
        const double Smouth = H * W;
        const double Sflue = h * H;
        const double Stoe = M_PI * cur[P_TOE] * cur[P_TOE] / 4.0;
        const double Vf = cur[P_FOOTVOL];

        /* ---- wind: pallet + foot ---- */
        double tau_p = cur[P_PALLET_TAU] > 1e-4 ? cur[P_PALLET_TAU] : 1e-4;
        double target = cur[P_GATE] * cur[P_PCHEST];
        pg += (target - pg) * (1.0 - exp(-1.0 / (tau_p * fs)));
        double pge = pg * (1.0 + cur[P_FMD] * sin(phi));
        phi += 2.0 * M_PI * cur[P_FMF] / fs;
        if (phi > 2.0 * M_PI) phi -= 2.0 * M_PI;

        const int nsub = 4;
        double dt = 1.0 / (fs * nsub);
        for (int s = 0; s < nsub; ++s) {
            double dp = pge - pf;
            double qt = Stoe * sgn(dp) * sqrt(2.0 * fabs(dp) / rho);
            double qf = Sflue * sqrt(2.0 * (pf > 0 ? pf : 0) / rho);
            pf += dt * rho * c * c / Vf * (qt - qf);
        }
        double Uj = sqrt(2.0 * (pf > 0 ? pf : 0) / rho) + 1e-3;

        /* ---- jet ---- */
        /* dispersive jet: disturbances travel at u_c = kappa * Uj^(2/3) * (w h)^(1/3)
           evaluated at the pipe's nominal resonance (consistent with Ising's I) */
        double fres = dv0.fres + (dv1.fres - dv0.fres) * fr;
        double uc = cur[P_KAPPA] * pow(Uj, 2.0 / 3.0) * cbrt(2.0 * M_PI * fres * h);
        double tau = W / uc * fs;
        if (tau < 3.0) tau = 3.0;
        if (tau > maxTau) tau = maxTau;
        double vdel_raw = dl_read(&vml, tau);
        /* jet receptivity: instability waves only grow for k b < ~1, i.e. below
           f ~ 0.16 Uj / h  -> 2nd-order low-pass at that limit */
        double fjet = cur[P_JETBW] * Uj / h;
        if (fjet > 0.4 * fs) fjet = 0.4 * fs;
        double kj = 1.0 - exp(-2.0 * M_PI * fjet / fs);
        vj += (vdel_raw - vj) * kj;
        vj2 += (vj - vj2) * kj;
        double vdel = vj2;
        double ampexp = 0.4 * W / h;
        if (ampexp > cur[P_AMPCAP]) ampexp = cur[P_AMPCAP];
        /* laminar onset: right after the pallet opens the young jet amplifies more;
           the boost decays as the jet turns turbulent (time measured in periods) */
        if (cur[P_GATE] > 0.5) {
            if (!gate_on) { gate_on = 1; onset_t = 0.0; }
            onset_t += fres / fs;
        } else {
            gate_on = 0;
        }
        if (gate_on && cur[P_ONSET_T] > 0)
            ampexp += cur[P_ONSET_B] * exp(-onset_t / cur[P_ONSET_T]);
        double G = cur[P_JETGAIN] * exp(ampexp);
        double b = 0.4 * h;
        nz += ((2.0 * rnd_uniform() - 1.0) - nz) * k_nz;
        /* sign: positive mouth velocity (into the pipe) deflects the jet outwards */
        double eta = -G * (h / Uj) * vdel + cur[P_NOISE] * h * nz * 3.0;
        double qin = b * H * Uj * (1.0 + tanh((eta - cur[P_Y0]) / b));
        double dq = (qin - qin_prev) * fs;
        qin_prev = qin;
        dq_lp += (dq - dq_lp) * k_dq;
        double delta_d = 4.0 / M_PI * sqrt(2.0 * h * W);
        double pj = -rho * delta_d / (W * H) * dq_lp;
        double pv = -0.5 * rho * (vm / 0.6) * (vm / 0.6) * sgn(vm);
        double ps = pj + pv;
        /* starting vortex: when the jet first reaches the labium it delivers a
           pressure pulse (half-sine, a quarter period of the resonance long) */
        if (gate_on && cur[P_KICK] != 0.0) {
            if (kick_t < 0.0 && pf > 0.5 * cur[P_PCHEST] * 0.3) kick_t = 0.0;  /* jet has formed */
            if (kick_t >= 0.0 && kick_t < 0.25) {
                ps += cur[P_KICK] * pf * sin(M_PI * kick_t / 0.25);
                kick_t += fres / fs;
            }
        }
        if (!gate_on) kick_t = -1.0;

        /* ---- waveguide ---- */
        double pplus_end = dl_read(&up, dline);
        double pminus_m = dl_read(&lo, dline);

        /* round-trip loss (first-order shelf) + in-loop DC blocker (~0.8 Hz) */
        double lsh = dv0.lb0 * pplus_end + dv0.lb1 * lsh_x1 - dv0.la1 * lsh_y1;
        lsh_x1 = pplus_end; lsh_y1 = lsh;
        double dcb = lsh - dc_x1 + 0.99995 * dc_y1;
        dc_x1 = lsh; dc_y1 = dcb;
        lp_y = dcb;
        double am = dv0.a_morph;
        double apm = am * lp_y + apm_x1 - am * apm_y1;
        apm_x1 = lp_y;
        apm_y1 = apm;
        double r_top = -apm;

        /* mouth boundary: p(0) = ps - tau_m dq/dt, q = p+ - p-  (trapezoidal) */
        double Km = 2.0 * dv0.taum * fs;
        double uin = ps - 2.0 * pminus_m;
        double mq = ((Km - 1.0) * mq_prev + uin + mu_prev) / (Km + 1.0);
        mq_prev = mq;
        mu_prev = uin;
        double pplus_new = mq + pminus_m;

        dl_write(&lo, r_top);
        dl_write(&up, pplus_new);

        double um = (pplus_new - pminus_m) / (rho * c);
        vm = um * Spipe / Smouth;
        /* hydrodynamic feedback: the jet-labium dipole induces a velocity at the
           flue exit (instantaneous, incompressible) -> edge tone during the attack */
        double qfl = qin - b * H * Uj;
        double vh = cur[P_EDGE] * qfl / (H * W);
        dl_write(&vml, vm + vh);
        double ut = (pplus_end - r_top) / (rho * c);

        /* ---- radiation (1 m, monopole) ---- */
        double Um = um * Spipe, Ut = ut * Spipe;
        double om = rho / (4.0 * M_PI) * (-(Um - um_prev) * fs);
        double ot = rho / (4.0 * M_PI) * ((Ut - ut_prev) * fs);
        um_prev = Um;
        ut_prev = Ut;

        /* ---- stability guard ---- */
        if (!isfinite(pplus_new) || fabs(pplus_new) > 1e6 || !isfinite(pf)) {
            memset(up.buf, 0, sizeof(double) * (size_t)(up.mask + 1));
            memset(lo.buf, 0, sizeof(double) * (size_t)(lo.mask + 1));
            memset(vml.buf, 0, sizeof(double) * (size_t)(vml.mask + 1));
            lp_y = apm_x1 = apm_y1 = mq_prev = mu_prev = vj = vj2 = 0.0;
            lsh_x1 = lsh_y1 = dc_x1 = dc_y1 = 0.0;
            vm = dq_lp = 0.0;
            qin_prev = 0.0;
            if (!isfinite(pf)) pf = 0.0;
            om = ot = 0.0;
            resets++;
        }

        if (out_mouth) out_mouth[t] = om;
        if (out_top) out_top[t] = ot;
        if (out_eta) out_eta[t] = (eta - cur[P_Y0]) / b;
        if (out_vm) out_vm[t] = vm;
        if (out_pf) out_pf[t] = pf;
        if (out_uj) out_uj[t] = Uj;
        if (out_ps) out_ps[t] = ps;

        /* ---- snapshots along the bore ---- */
        while (snap_k < nsnap && snap_idx[snap_k] == t) {
            for (int i = 0; i < nx; ++i) {
                double xf = (nx > 1) ? (double)i / (double)(nx - 1) : 0.0;
                double pp = dl_read_after(&up, xf * dline);
                double pm = dl_read_after(&lo, (1.0 - xf) * dline);
                snap_p[(size_t)snap_k * nx + i] = pp + pm;
                snap_u[(size_t)snap_k * nx + i] = (pp - pm) / (rho * c);
            }
            if (snap_info) {
                double *si = snap_info + (size_t)snap_k * 6;
                si[0] = (eta - cur[P_Y0]) / b;
                si[1] = qin / (2.0 * b * H * Uj); /* fraction of jet entering pipe */
                si[2] = Uj;
                si[3] = pf;
                si[4] = vm;
                si[5] = dv0.Leff;
            }
            snap_k++;
        }
    }

    dl_free(&up);
    dl_free(&lo);
    dl_free(&vml);
    return resets;
}

int labium_npar(void) { return NPAR; }

/* debug: derived quantities for one parameter frame */
void labium_debug_derive(const double *p, double fs, double *out) {
    Derived dv;
    derive(p, fs, &dv);
    out[0] = dv.lb0; out[1] = dv.lb1; out[2] = dv.la1; out[3] = dv.dline;
    out[4] = dv.M; out[5] = dv.Leff; out[6] = dv.fres; out[7] = dv.lp_delay;
}
