/*
 * OkumuLab 1 — Mallet mode (see Mallet.h)
 */
#include "Mallet.h"
#include "Simd.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <emmintrin.h>

namespace okl
{

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
using cd = std::complex<double>;

/* ------------------------------------------------------------------ materials */
struct Pure { double E, nu, rho, eta, alpha, kth, cp; };
// tin and lead at room temperature; loss factors: Cremer & Heckl, Structure-Borne Sound (tin ~2e-3, lead ~2e-2)
constexpr Pure kSn { 50e9, 0.36, 7290.0, 2e-3, 22.0e-6, 66.8, 228.0 };
constexpr Pure kPb { 16e9, 0.44, 11340.0, 2e-2, 28.9e-6, 35.3, 129.0 };

/* a tin-lead pipe metal from its tin content by weight: the two phases by volume fraction
   (density and heat capacity exact, modulus Hill average, loss factor log-interpolated) */
MetalProps alloy (const char* name, double wSn)
{
    const double vs = (wSn / kSn.rho) / (wSn / kSn.rho + (1.0 - wSn) / kPb.rho), vp = 1.0 - vs;
    MetalProps m {};
    m.name = name;
    m.rho = vs * kSn.rho + vp * kPb.rho;
    const double eV = vs * kSn.E + vp * kPb.E, eR = 1.0 / (vs / kSn.E + vp / kPb.E);
    m.E = 0.5 * (eV + eR);
    m.nu = vs * kSn.nu + vp * kPb.nu;
    m.eta = std::exp (vs * std::log (kSn.eta) + vp * std::log (kPb.eta));
    m.alpha = vs * kSn.alpha + vp * kPb.alpha;
    m.kth = vs * kSn.kth + vp * kPb.kth;
    m.cp = (vs * kSn.rho * kSn.cp + vp * kPb.rho * kPb.cp) / m.rho;
    return m;
}

const MetalProps kMetals[kNumMetals] = {
    alloy ("Common metal 30% Sn", 0.30),
    alloy ("Spotted metal 50% Sn", 0.50),
    alloy ("Tin 75% Sn", 0.75),
    { "Zinc", 108e9, 0.25, 7140.0, 3e-4, 30.2e-6, 116.0, 388.0 },
    { "Copper", 120e9, 0.34, 8960.0, 2e-3, 16.5e-6, 401.0, 385.0 },
};

const HeadProps kHeads[kNumHeads] = {
    { "Hard rubber", 60e6, 0.49, 1150.0, 0.75 },
    { "Acrylic", 3.2e9, 0.37, 1190.0, 0.80 },
    { "Boxwood", 12e9, 0.30, 950.0, 0.70 },
    { "Brass", 100e9, 0.34, 8500.0, 0.90 },
};

constexpr double kStickMass = 0.003;       // effective mass of the handle at the head [kg]
constexpr double kDamperC = 6.0;           // felt damper at full strength [N s/m]
constexpr double kDamperPos = 0.85;        // ... pressed on near the top
constexpr double kCapMassPerArea = 2.0;    // cap mass = this x (rho h pi a^2): lid and skirt

/* ---------------------------------------------------------- beam functions */
/* clamped at x = 0; at x = L no moment and a translational spring K = k L^3 / EI, or a tip mass
   ratio mu = m_tip / m_beam. Characteristic function divided by cosh(l) (no overflow). */
double bcFun (double l, double mu, double K)
{
    const double th = std::tanh (l), sech = 1.0 / std::cosh (l), cs = std::cos (l), sn = std::sin (l);
    double f = sech + cs;
    if (mu > 0) f += mu * l * (cs * th - sn);
    if (K > 0) f += K / (l * l * l) * (sn - cs * th);
    return f;
}

double rootCantilever (int m)     // clamped-free
{
    static const double r[] { 1.8751040687, 4.6940911330, 7.8547574382, 10.9955407349, 14.1371683910 };
    return m <= 5 ? r[m - 1] : (2.0 * m - 1.0) * kPi / 2.0;
}
double rootClampedPinned (int m)
{
    if (m <= 0) return 0.0;
    static const double r[] { 3.9266023120, 7.0685827547, 10.2101761242, 13.3517687778, 16.4933614313 };
    return m <= 5 ? r[m - 1] : (4.0 * m + 1.0) * kPi / 4.0;
}

double beamRootImpl (int m, double mu, double K)
{
    double lo, hi;
    if (mu > 0) { lo = rootClampedPinned (m - 1) + 1e-6; hi = rootCantilever (m) + 0.01; }
    else if (K > 0) { lo = rootCantilever (m) - 0.01; hi = rootClampedPinned (m) + 0.01; }
    else return rootCantilever (m);
    double flo = bcFun (lo, mu, K), fhi = bcFun (hi, mu, K);
    if (flo * fhi > 0) return std::abs (flo) < std::abs (fhi) ? lo : hi;
    for (int it = 0; it < 60; ++it)
    {
        const double mid = 0.5 * (lo + hi), fm = bcFun (mid, mu, K);
        if ((fm < 0) == (flo < 0)) { lo = mid; flo = fm; } else hi = mid;
        if (hi - lo < 1e-12 * hi) break;
    }
    return 0.5 * (lo + hi);
}

/* X(xi) = cosh - cos - s (sinh - sin), y = l xi; written without the large exponentials */
struct BeamFn
{
    double l, s, E, D, omsE;    // omsE = (1 - s) e^l
    explicit BeamFn (double lam) : l (lam)
    {
        E = std::exp (-l);
        D = 1.0 - E * E + 2.0 * E * std::sin (l);
        s = (1.0 + E * E + 2.0 * E * std::cos (l)) / D;
        omsE = 2.0 * (std::sin (l) - std::cos (l) - E) / D;
    }
    // cosh y - s sinh y and sinh y - s cosh y at y = l xi
    void hyp (double xi, double& ch, double& sh) const
    {
        const double ep = omsE * std::exp (l * (xi - 1.0)), em = (1.0 + s) * std::exp (-l * xi);
        ch = 0.5 * (ep + em);
        sh = 0.5 * (ep - em);
    }
    double X (double xi) const
    {
        double ch, sh;
        hyp (xi, ch, sh);
        const double y = l * xi;
        return ch - std::cos (y) + s * std::sin (y);
    }
    /* integrals over [0, 1] (closed form; X'''' = l^4 X): int X^2 = [3 X X''' + l^4 X^2 - 2 X' X''']/(4 l^4) at 1 */
    void integrals (double& x2, double& x1, double& xEnd) const
    {
        double ch, sh;
        hyp (1.0, ch, sh);
        const double c = std::cos (l), sn = std::sin (l);
        const double X1 = ch - c + s * sn;
        const double Xp = l * (sh + sn + s * c);
        const double X3 = l * l * l * (sh - (sn + s * c));
        x2 = (3.0 * X1 * X3 + std::pow (l, 4) * X1 * X1 - 2.0 * Xp * X3) / (4.0 * std::pow (l, 4));
        x1 = (X3 / (l * l * l) + 2.0 * s) / l;
        xEnd = X1;
    }
};

/* ------------------------------------------------------------- Flügge shell */
/* stiffness matrix of mode (lambda = axial wavenumber x a, n); eigenvalues Delta = rho (1 - nu^2) a^2 w^2 / E */
void shellMatrix (double L, int n, double nu, double k, double A[3][3])
{
    const double nn = (double) n, L2 = L * L;
    A[0][0] = L2 + 0.5 * (1.0 - nu) * nn * nn * (1.0 + k);
    A[0][1] = -0.5 * (1.0 + nu) * L * nn;
    A[0][2] = -nu * L - k * (L2 * L - 0.5 * (1.0 - nu) * L * nn * nn);
    A[1][1] = 0.5 * (1.0 - nu) * L2 * (1.0 + 3.0 * k) + nn * nn;
    A[1][2] = nn + k * 0.5 * (3.0 - nu) * L2 * nn;
    A[2][2] = 1.0 + k * std::pow (L2 + nn * nn, 2) + k * (1.0 - 2.0 * nn * nn);
    A[1][0] = A[0][1]; A[2][0] = A[0][2]; A[2][1] = A[1][2];
}

/* symmetric 3x3: eigenvalues ascending, unit eigenvectors (rows of V) */
void eig3 (const double A[3][3], double ev[3], double V[3][3])
{
    const double p1 = A[0][1] * A[0][1] + A[0][2] * A[0][2] + A[1][2] * A[1][2];
    const double q = (A[0][0] + A[1][1] + A[2][2]) / 3.0;
    if (p1 < 1e-300)
    {
        double d[3] { A[0][0], A[1][1], A[2][2] };
        int idx[3] { 0, 1, 2 };
        std::sort (idx, idx + 3, [&] (int a, int b) { return d[a] < d[b]; });
        for (int i = 0; i < 3; ++i)
        {
            ev[i] = d[idx[i]];
            for (int j = 0; j < 3; ++j) V[i][j] = j == idx[i] ? 1.0 : 0.0;
        }
        return;
    }
    const double p2 = (A[0][0] - q) * (A[0][0] - q) + (A[1][1] - q) * (A[1][1] - q) + (A[2][2] - q) * (A[2][2] - q) + 2.0 * p1;
    const double p = std::sqrt (p2 / 6.0);
    double B[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) B[i][j] = (A[i][j] - (i == j ? q : 0.0)) / p;
    const double detB = B[0][0] * (B[1][1] * B[2][2] - B[1][2] * B[2][1]) - B[0][1] * (B[1][0] * B[2][2] - B[1][2] * B[2][0])
                        + B[0][2] * (B[1][0] * B[2][1] - B[1][1] * B[2][0]);
    const double r = std::clamp (detB / 2.0, -1.0, 1.0);
    const double phi = std::acos (r) / 3.0;
    const double e1 = q + 2.0 * p * std::cos (phi);
    const double e3 = q + 2.0 * p * std::cos (phi + 2.0 * kPi / 3.0);
    const double e2 = 3.0 * q - e1 - e3;
    double e[3] { e3, e2, e1 };
    for (int i = 0; i < 3; ++i)
    {
        // Newton polish on det(A - x I) (the smallest root is tiny next to the others for bending modes)
        double x = e[i];
        for (int it = 0; it < 2; ++it)
        {
            const double a = A[0][0] - x, b = A[1][1] - x, c = A[2][2] - x;
            const double det = a * (b * c - A[1][2] * A[1][2]) - A[0][1] * (A[0][1] * c - A[1][2] * A[0][2]) + A[0][2] * (A[0][1] * A[1][2] - b * A[0][2]);
            const double dd = -(b * c - A[1][2] * A[1][2]) - (a * c - A[0][2] * A[0][2]) - (a * b - A[0][1] * A[0][1]);
            if (dd == 0) break;
            const double xn = x - det / dd;
            if (! (std::abs (xn - x) < 0.01 * (std::abs (x) + 1e-300) + 1e-300)) break;
            x = xn;
        }
        ev[i] = x;
        // eigenvector: the longest cross product of two rows of A - x I
        double R[3][3];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) R[a][b] = A[a][b] - (a == b ? x : 0.0);
        double best = -1, v[3] { 0, 0, 1 };
        for (int a = 0; a < 3; ++a)
        {
            const int b = (a + 1) % 3;
            const double c0 = R[a][1] * R[b][2] - R[a][2] * R[b][1];
            const double c1 = R[a][2] * R[b][0] - R[a][0] * R[b][2];
            const double c2 = R[a][0] * R[b][1] - R[a][1] * R[b][0];
            const double nrm = c0 * c0 + c1 * c1 + c2 * c2;
            if (nrm > best) { best = nrm; v[0] = c0; v[1] = c1; v[2] = c2; }
        }
        const double inv = best > 0 ? 1.0 / std::sqrt (best) : 1.0;
        for (int j = 0; j < 3; ++j) V[i][j] = v[j] * inv;
    }
}

/* the lowest eigenvalue and its vector (n >= 1: the flexural, mostly radial mode). Newton on the characteristic
   polynomial from c0 / c2 (below the smallest root, where the cubic is convex and falling: monotone convergence) */
double lowestEig (const double A[3][3], double v[3])
{
    const double c1 = A[0][0] + A[1][1] + A[2][2];
    const double c2 = A[0][0] * A[1][1] - A[0][1] * A[0][1] + A[0][0] * A[2][2] - A[0][2] * A[0][2] + A[1][1] * A[2][2] - A[1][2] * A[1][2];
    const double c0 = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[1][2]) - A[0][1] * (A[0][1] * A[2][2] - A[1][2] * A[0][2])
                      + A[0][2] * (A[0][1] * A[1][2] - A[1][1] * A[0][2]);
    double x = c2 > 0 ? std::max (0.0, c0 / c2) : 0.0;
    for (int it = 0; it < 8; ++it)
    {
        const double p = ((-x + c1) * x - c2) * x + c0, dp = (-3.0 * x + 2.0 * c1) * x - c2;
        if (dp >= 0) break;
        const double xn = x - p / dp;
        const bool done = std::abs (xn - x) <= 1e-13 * std::abs (xn);
        x = xn;
        if (done) break;
    }
    double R[3][3];
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) R[a][b] = A[a][b] - (a == b ? x : 0.0);
    double best = -1;
    v[0] = 0; v[1] = 0; v[2] = 1;
    for (int a = 0; a < 3; ++a)
    {
        const int b = (a + 1) % 3;
        const double c0v = R[a][1] * R[b][2] - R[a][2] * R[b][1];
        const double c1v = R[a][2] * R[b][0] - R[a][0] * R[b][2];
        const double c2v = R[a][0] * R[b][1] - R[a][1] * R[b][0];
        const double nrm = c0v * c0v + c1v * c1v + c2v * c2v;
        if (nrm > best) { best = nrm; v[0] = c0v; v[1] = c1v; v[2] = c2v; }
    }
    const double inv = best > 0 ? 1.0 / std::sqrt (best) : 1.0;
    for (int j = 0; j < 3; ++j) v[j] *= inv;
    return x;
}

/* eigenpair "root" of a shell mode: n >= 1 the flexural one (fast path); n = 0 any of the three */
double shellEigen (double lam, int n, double nu, double k, int root, double v[3])
{
    double A[3][3];
    shellMatrix (lam, n, nu, k, A);
    if (n >= 1 && root == 0) return lowestEig (A, v);
    double ev[3], V[3][3];
    eig3 (A, ev, V);
    const int r = std::clamp (root, 0, 2);
    for (int j = 0; j < 3; ++j) v[j] = V[r][j];
    return ev[r];
}

/* ------------------------------------------------------- Bessel functions */
struct Bes { double z, zp; };       // value and derivative
bool finite (double x) { return x == x && std::abs (x) < 1e300; }

/* J_n, Y_n and their derivatives at x (Z'_n = n/x Z_n - Z_{n+1}) */
void besselJY (int n, double x, Bes& J, Bes& Y)
{
    const double jn = std::cyl_bessel_j ((double) n, x), jn1 = std::cyl_bessel_j ((double) n + 1, x);
    J = { jn, n / x * jn - jn1 };
    double yn = std::cyl_neumann ((double) n, x), yn1 = std::cyl_neumann ((double) n + 1, x);
    if (! finite (yn) || ! finite (yn1))
    {
        // tiny argument: Y_n ~ -(n-1)! (2/x)^n / pi (n >= 1)
        const double lg = std::lgamma ((double) std::max (n, 1)) + std::max (n, 1) * std::log (2.0 / x) - std::log (kPi);
        yn = -std::exp (std::min (lg, 690.0));
        yn1 = yn * 2.0 * std::max (n, 1) / x;
        if (! finite (yn1)) yn1 = -1e300;
    }
    Y = { yn, n / x * yn - yn1 };
}

/* I_n, K_n and derivatives (I'_n = n/x I_n + I_{n+1}, K'_n = n/x K_n - K_{n+1}) */
void besselIK (int n, double x, Bes& I, Bes& K)
{
    const double in = std::cyl_bessel_i ((double) n, x), in1 = std::cyl_bessel_i ((double) n + 1, x);
    I = { in, n / x * in + in1 };
    double kn = std::cyl_bessel_k ((double) n, x), kn1 = std::cyl_bessel_k ((double) n + 1, x);
    if (! finite (kn) || ! finite (kn1)) { kn = 1e300; kn1 = 1e300; }
    K = { kn, n / x * kn - kn1 };
}

/* H_n'(x) of the outgoing wave (H = J + i Y, time e^{-i w t}) */
cd hankelPrime (int n, double x)
{
    Bes J, Y;
    besselJY (n, x, J, Y);
    return { J.zp, Y.zp };
}
} // namespace

const MetalProps& metalProps (int i) { return kMetals[std::clamp (i, 0, kNumMetals - 1)]; }
const HeadProps& headProps (int i) { return kHeads[std::clamp (i, 0, kNumHeads - 1)]; }
double wallThickness (double d) { return 0.25e-3 + 0.009 * d; }

/* test hooks (labium_check) */
double malletTestShellDelta (double lam, int n, double nu, double k, int root)
{
    double A[3][3], ev[3], V[3][3];
    shellMatrix (lam, n, nu, k, A);
    eig3 (A, ev, V);
    return ev[std::clamp (root, 0, 2)];
}
double malletTestShellLowest (double lam, int n, double nu, double k)
{
    double v[3];
    return shellEigen (lam, n, nu, k, 0, v);
}
double malletTestBeamRoot (int m, double mu, double K) { return beamRootImpl (m, mu, K); }
void malletTestBeamIntegrals (double lam, double& x2, double& x1, double& xEnd)
{
    BeamFn b (lam);
    b.integrals (x2, x1, xEnd);
}
double malletTestBeamX (double lam, double xi) { return BeamFn (lam).X (xi); }

/* ------------------------------------------------------------------ model */
void MalletModel::prepare (double sampleRate)
{
    fs = sampleRate;
    T = 1.0 / fs;
    reset();
}

void MalletModel::reset()
{
    for (int i = 0; i < kMaxModes; ++i) zr[i] = zi[i] = 0.0;
    nActive = 0;
    flight = inContact = false;
    F = prevF = delta = y = yd = 0;
    damperAmt = 0;
    damperOn = false;
    forceN = 0;
}

void MalletModel::beamRoots()
{
    // index 0: bending (n = 1), the cap is a tip mass; index 1: n != 1, the cap holds the rim (spring)
    const double mu = sp.morph * kCapMassPerArea * a / Lb;
    const double K = std::pow (10.0, 4.0 * sp.morph) - 1.0;
    const bool rootsMoved = std::abs (mu - bcMu) > 1e-4 * (mu + 1e-3) || std::abs (K - bcK) > 1e-4 * (K + 1e-3);
    if (rootsMoved)
    {
        bcMu = mu; bcK = K;
        for (int m = 1; m <= kM; ++m)
        {
            lam[0][m - 1] = beamRootImpl (m, mu, 0.0);
            lam[1][m - 1] = beamRootImpl (m, 0.0, K);
            for (int b = 0; b < 2; ++b)
            {
                BeamFn bf (lam[b][m - 1]);
                sShape[b][m - 1] = bf.s;
                bf.integrals (nrm[b][m - 1], intX[b][m - 1], xTip[b][m - 1]);
                xD[b][m - 1] = bf.X (kDamperPos);
            }
        }
        bcXs = -1;
    }
    if (rootsMoved || std::abs (sp.strike - bcXs) > 1e-6)
    {
        bcXs = sp.strike;
        for (int b = 0; b < 2; ++b)
            for (int m = 1; m <= kM; ++m) xS[b][m - 1] = BeamFn (lam[b][m - 1]).X (sp.strike);
    }
}

namespace
{
struct Cand { int n, m, br, bc; double ws, radial, mass, phiS, score, compl_; double vec[3]; };
}

void MalletModel::design()
{
    const MetalProps& mt = metalProps (sp.metal);
    E = mt.E; nu = mt.nu; rhoS = mt.rho; etaInt = mt.eta;
    a = 0.5 * std::max (sp.d, 0.002);
    Lb = std::max (sp.L, 4.0 * a);
    h = std::max (1e-4, wallThickness (sp.d) * sp.wall);
    ms = rhoS * h;
    kk = h * h / (12.0 * a * a);
    mTip = sp.morph * kCapMassPerArea * rhoS * h * kPi * a * a;
    // thermoelastic loss of the wall in bending (Zener; plate: x (1 + nu)/(1 - nu))
    const double rc = rhoS * mt.cp;
    teDelta = E * mt.alpha * mt.alpha * sp.tempK / rc * (1.0 + nu) / (1.0 - nu);
    teTau = h * h / (kPi * kPi * (mt.kth / rc));
    beamRoots();

    // mallet: Hertz sphere on the outside of the cylinder
    const HeadProps& hd = headProps (sp.head);
    const double Rh = 0.5 * std::max (0.004, sp.headD);
    mh = hd.rho * 4.0 / 3.0 * kPi * Rh * Rh * Rh + kStickMass;
    const double Es = 1.0 / ((1.0 - hd.nu * hd.nu) / hd.E + (1.0 - nu * nu) / E);
    const double Rs = std::sqrt (Rh * (Rh * a / (Rh + a)));
    Kh = 4.0 / 3.0 * Es * std::sqrt (Rs);
    eRest = hd.e;
    // how long the mallet stays on the wall: Hertz on a rigid wall (1 m/s), or the thin wall giving way under it
    // (point impedance of a plate, 8 sqrt(D rho h): the mallet's momentum spreads over m / Z)
    const double tauHertz = 3.2145 * std::pow (mh * mh / (Kh * Kh * 1.0), 0.2);
    const double Dpl = E * h * h * h / (12.0 * (1.0 - nu * nu));
    const double tauEst = std::max (tauHertz, mh / (8.0 * std::sqrt (Dpl * ms)));

    const double wRef = std::sqrt (E / (rhoS * (1.0 - nu * nu))) / a;
    // modes far above what the stroke excites (its spectrum is down ~55 dB at 12 / contact time) are left out
    const double wMax = kTwoPi * std::min ({ 20000.0, 0.45 * fs, 12.0 / tauEst });
    thread_local static Cand cand[1200];       // (per audio thread: two plugin instances may render at once)
    int nc = 0;
    int overN = 0;
    for (int n = 0; n <= 60 && overN < 2 && nc < 1150; ++n)
    {
        const int bc = n == 1 ? 0 : 1;
        bool firstOver = false;
        const double lgN = std::lgamma ((double) n + 1.0) + n * std::log (2.0) - std::log (kPi);
        for (int m = 1; m <= kM && nc < 1150; ++m)
        {
            const double L = lam[bc][m - 1] * a / Lb;
            double ev[3], V[3][3];
            int nRoots = 3;
            if (n >= 1) { ev[0] = shellEigen (L, n, nu, kk, 0, V[0]); nRoots = 1; }
            else { double A[3][3]; shellMatrix (L, n, nu, kk, A); eig3 (A, ev, V); }
            bool any = false, allOver = true;
            for (int r = 0; r < nRoots; ++r)
            {
                if (ev[r] <= 0) continue;
                const double w = wRef * std::sqrt (ev[r]);
                const double C = V[r][2], rad = C * C;
                if (w < wMax) allOver = false;
                if (rad < 0.02 || w >= wMax || w < kTwoPi * 15.0) continue;
                any = true;
                Cand& c = cand[nc++];
                c.n = n; c.m = m; c.br = r; c.bc = bc; c.ws = w; c.radial = rad;
                c.vec[0] = V[r][0] / C; c.vec[1] = V[r][1] / C; c.vec[2] = 1.0;
                const double cn = n == 0 ? kTwoPi : kPi;
                c.mass = ms * a * cn * Lb * nrm[bc][m - 1] / rad + (n == 1 ? mTip * xTip[bc][m - 1] * xTip[bc][m - 1] / rad : 0.0);
                c.phiS = xS[bc][m - 1];
                // rough radiated energy for the choice: |G| ~ L |int X| / |H'_n(ka)|, force spectrum of the stroke, decay time
                const double ka = w / sp.c0 * a;
                const double lka = std::log (ka);
                const double lgH = n == 0 ? std::log (2.0 / kPi) - lka : lgN - (n + 1.0) * lka;
                const double lHp = std::max (lgH, 0.5 * (std::log (2.0 / kPi) - lka));
                const double wt = w * tauEst / kPi;
                const double eta = etaInt + (n >= 2 ? teDelta * w * teTau / (1.0 + w * w * teTau * teTau) : 0.0) + 1e-4;
                const double t60 = 2.0 * 6.91 / (eta * w);
                const double lg = std::log (std::abs (intX[bc][m - 1]) * Lb + 1e-9) - lHp + std::log (std::abs (c.phiS) + 1e-9) - std::log (c.mass)
                                  - std::log (1.0 + wt * wt);
                c.score = 2.0 * lg + std::log (t60);
                c.compl_ = c.phiS * c.phiS / (c.mass * w * w);
            }
            if (m == 1) firstOver = allOver;
            if (! any && allOver) break;      // higher m only rise
        }
        if (n >= 2 && firstOver) ++overN; else overN = 0;
    }

    // keep the strongest radiators, plus the modes that matter most to the contact itself
    std::sort (cand, cand + nc, [] (const Cand& x, const Cand& y) { return x.score > y.score; });
    const int nRad = std::min (nc, kMaxModes - 16);
    std::sort (cand + nRad, cand + nc, [] (const Cand& x, const Cand& y) { return x.compl_ > y.compl_; });
    const int nKeep = std::min (nc, kMaxModes);

    // carry the ringing of modes that stay (re-designed while sounding)
    double oldZr[kMaxModes], oldZi[kMaxModes], oldWd[kMaxModes], oldSig[kMaxModes];
    int oldKey[kMaxModes];
    const int oldN = nActive;
    for (int i = 0; i < oldN; ++i)
    {
        oldZr[i] = zr[i]; oldZi[i] = zi[i];
        oldWd[i] = std::sqrt (std::max (w0[i] * w0[i] - sigma[i] * sigma[i], 1e-6));
        oldSig[i] = sigma[i];
        oldKey[i] = mn[i] * 4096 + mm[i] * 4 + mbc[i];
    }

    // the wall's give under the mallet from the modes left out: those chosen against, statically, and those above
    // the cut-off like a plate (modal density S/(4 pi) sqrt(rho h / D): sum of 1 / (M w^2) above wc = 1 / (4 pi wc sqrt(D rho h)))
    cRes = 1.0 / (4.0 * kPi * wMax * std::sqrt (Dpl * ms));
    for (int i = nKeep; i < nc; ++i) cRes += cand[i].compl_;
    nSel = nKeep;
    for (int i = 0; i < nSel; ++i)
    {
        const Cand& c = cand[i];
        mn[i] = c.n; mm[i] = c.m; mbc[i] = c.br;
        wS[i] = c.ws; radial[i] = c.radial; mass[i] = c.mass; phiS[i] = c.phiS;
        ax[i] = c.vec[0] * c.vec[0] + c.vec[1] * c.vec[1] + 1.0;
        fluidRatio[i] = 1.0; etaRad[i] = 0.0;
        zr[i] = zi[i] = 0.0;
    }
    for (int i = nSel; i < kMaxModes; ++i)
    {
        zr[i] = zi[i] = pr[i] = pim[i] = g0r[i] = g0i[i] = g1r[i] = g1i[i] = wq[i] = oa[i] = ob[i] = 0.0;
        gAbs[i] = 0;
    }
    spDesigned = sp;
    designed = true;
    nActive = 0;
    retune (true);
    // restore the ringing of the modes that stayed
    int restored = 0;
    for (int i = 0; i < nSel && oldN > 0; ++i)
    {
        const int key = mn[i] * 4096 + mm[i] * 4 + mbc[i];
        for (int j = 0; j < oldN; ++j)
            if (oldKey[j] == key)
            {
                const double q = oldZi[j] / oldWd[j], qd = oldZr[j] - oldSig[j] * q;
                const double wd = std::sqrt (std::max (w0[i] * w0[i] - sigma[i] * sigma[i], 1e-6));
                zi[i] = wd * q; zr[i] = qd + sigma[i] * q;
                ++restored;
                break;
            }
    }
    nActive = restored > 0 ? nSel : 0;
}

/* added mass of the gas inside and outside, radiation damping, far-field gain of mode i */
void MalletModel::computeFluid (int i)
{
    const int n = mn[i];
    const double rho0 = sp.rho0, c0 = sp.c0;
    const double w = wS[i];
    const double k0 = w / c0;
    const double kz = lam[n == 1 ? 0 : 1][mm[i] - 1] / Lb;
    cd mOut (0, 0), mIn (0, 0);
    if (k0 > kz * 1.000001)
    {
        const double kap = std::sqrt (k0 * k0 - kz * kz), x = kap * a;
        Bes J, Y;
        besselJY (n, x, J, Y);
        const cd H (J.z, Y.z), Hp (J.zp, Y.zp);
        mOut = -rho0 * H / (kap * Hp);
        if (n >= 1 && std::abs (J.zp) > 1e-300) mIn = rho0 * J.z / (kap * J.zp);
    }
    else
    {
        const double gam = std::max (1e-9, std::sqrt (kz * kz - k0 * k0)), x = gam * a;
        Bes I, K;
        besselIK (n, x, I, K);
        if (std::abs (K.zp) > 1e-300 && K.z < 1e299) mOut = -rho0 * K.z / (gam * K.zp);
        if (n >= 1 && std::abs (I.zp) > 1e-300) mIn = rho0 * I.z / (gam * I.zp);
    }
    const double rw = radial[i];
    double mInR = std::clamp (mIn.real(), -0.3 * ms / rw, 3.0 * ms);   // near an interior cross-mode the single-mode correction fails
    if (! finite (mInR)) mInR = 0;
    double mOutR = finite (mOut.real()) ? mOut.real() : 0.0, mOutI = finite (mOut.imag()) ? mOut.imag() : 0.0;
    const double mAdd = (mOutR + mInR) * rw;
    fluidRatio[i] = std::clamp (1.0 / std::sqrt (std::max (0.05, 1.0 + mAdd / ms)), 0.7, 1.05);
    etaRad[i] = std::max (0.0, mOutI) * rw / std::max (0.2 * ms, ms + mAdd);
    // far field 1 m broadside, struck side towards the listener (outward velocity = -dq/dt X(x) cos n theta)
    const double wf = w * fluidRatio[i];
    const cd Hp = hankelPrime (n, std::max (1e-9, wf / c0 * a));
    cd in (1, 0);
    for (int j = 0; j < (n & 3); ++j) in *= cd (0, -1);
    const double Lx = Lb * intX[n == 1 ? 0 : 1][mm[i] - 1];
    cd G = -(rho0 * c0 / kPi) * in * Lx / Hp;
    if (! finite (G.real()) || ! finite (G.imag())) G = 0;
    gAbs[i] = std::abs (G);
    gArg[i] = std::arg (G);
}

void MalletModel::retune (bool fluid)
{
    const MetalProps& mt = metalProps (sp.metal);
    // material and geometry may have moved (not the choice of modes)
    E = mt.E; nu = mt.nu; rhoS = mt.rho; etaInt = mt.eta;
    a = 0.5 * std::max (sp.d, 0.002);
    Lb = std::max (sp.L, 4.0 * a);
    h = std::max (1e-4, wallThickness (sp.d) * sp.wall);
    ms = rhoS * h;
    kk = h * h / (12.0 * a * a);
    mTip = sp.morph * kCapMassPerArea * rhoS * h * kPi * a * a;
    const double rc = rhoS * mt.cp;
    teDelta = E * mt.alpha * mt.alpha * sp.tempK / rc * (1.0 + nu) / (1.0 - nu);
    teTau = h * h / (kPi * kPi * (mt.kth / rc));
    beamRoots();
    const HeadProps& hd = headProps (sp.head);
    const double Rh = 0.5 * std::max (0.004, sp.headD);
    mh = hd.rho * 4.0 / 3.0 * kPi * Rh * Rh * Rh + kStickMass;
    const double Es = 1.0 / ((1.0 - hd.nu * hd.nu) / hd.E + (1.0 - nu * nu) / E);
    Kh = 4.0 / 3.0 * Es * std::sqrt (std::sqrt (Rh * (Rh * a / (Rh + a))));
    eRest = hd.e;

    const double wRef = std::sqrt (E / (rhoS * (1.0 - nu * nu))) / a;
    for (int i = 0; i < nSel; ++i)
    {
        const int n = mn[i], m = mm[i], bc = n == 1 ? 0 : 1;
        double vec[3];
        const double evr = shellEigen (lam[bc][m - 1] * a / Lb, n, nu, kk, mbc[i], vec);
        const double C = vec[2];
        const double rad = std::max (1e-4, C * C);
        wS[i] = wRef * std::sqrt (std::max (evr, 1e-30));
        radial[i] = rad;
        ax[i] = 1.0 / rad;
        const double cn = n == 0 ? kTwoPi : kPi;
        mass[i] = ms * a * cn * Lb * nrm[bc][m - 1] / rad + (n == 1 ? mTip * xTip[bc][m - 1] * xTip[bc][m - 1] / rad : 0.0);
        phiS[i] = xS[bc][m - 1];
        sigD[i] = kDamperC * sp.damper * xD[bc][m - 1] * xD[bc][m - 1] / (2.0 * mass[i]);
        // wall bending share of the strain energy (Flügge bending term of the radial row); beam and breathing modes are membrane
        if (n >= 2)
        {
            const double nn = (double) n, L = lam[bc][m - 1] * a / Lb;
            const double bend = kk * (std::pow (L * L + nn * nn, 2) - 2.0 * nn * nn + 1.0);
            fBend[i] = std::clamp (bend * C * C / std::max (evr, 1e-30), 0.0, 1.0);
        }
        else fBend[i] = 0.0;
        if (fluid) computeFluid (i);
    }
    if (fluid) { spFluid = sp; framesSinceFluid = 0; }
    for (int i = 0; i < nSel; ++i) discretise (i);
    cModes = 0;
    for (int i = 0; i < nSel; ++i) cModes += wq[i] * g1i[i];
    spTuned = sp;
    framesSinceBC = 0;
}

/* pole, first-order-hold input weights and output weights of mode i (keeps q, dq/dt of a ringing mode) */
void MalletModel::discretise (int i)
{
    const double wOld = std::sqrt (std::max (w0[i] * w0[i] - sigma[i] * sigma[i], 1e-6));
    const double qOld = zi[i] / wOld, qdOld = zr[i] - sigma[i] * qOld;

    const int n = mn[i];
    const double w = wS[i] * fluidRatio[i];
    const double te = n >= 2 ? fBend[i] * teDelta * w * teTau / (1.0 + w * w * teTau * teTau) : 0.0;
    const double eta = etaInt + te + etaRad[i];
    const double sg = 0.5 * eta * w + damperAmt * sigD[i];
    const double wd = std::sqrt (std::max (w * w - sg * sg, 1e-4 * w * w));
    w0[i] = w;
    sigma[i] = sg;

    const cd lamT (-sg * T, wd * T);
    const cd p = std::exp (lamT);
    cd E0, E1;
    if (std::abs (lamT) < 1e-4)
    {
        E0 = T * (1.0 + lamT / 2.0 + lamT * lamT / 6.0);
        E1 = T * T * (0.5 + lamT / 3.0 + lamT * lamT / 8.0);
    }
    else
    {
        const cd lmb (-sg, wd);
        E0 = (p - 1.0) / lmb;
        E1 = (p * (lamT - 1.0) + 1.0) / (lmb * lmb);
    }
    const cd c0 = E1 / T, c1 = E0 - E1 / T;
    const double f = phiS[i] / mass[i];
    pr[i] = p.real(); pim[i] = p.imag();
    g0r[i] = c0.real() * f; g0i[i] = c0.imag() * f;
    g1r[i] = c1.real() * f; g1i[i] = c1.imag() * f;
    wq[i] = phiS[i] / wd;
    const double ca = std::cos (gArg[i]), sa = std::sin (gArg[i]);
    oa[i] = gAbs[i] * ca;
    ob[i] = gAbs[i] * (sa * w - ca * sg) / wd;
    // same displacement and velocity under the new pole
    zi[i] = wd * qOld;
    zr[i] = qdOld + sg * qOld;
}

void MalletModel::setParams (const StrikeParams& p)
{
    sp = p;
    sp.L = std::max (0.02, sp.L);
    sp.d = std::max (0.002, sp.d);
    if (! designed) return;
    ++framesSinceBC;
    ++framesSinceFluid;
    auto moved = [] (double x, double y, double tol) { return std::abs (x - y) > tol * (std::abs (y) + 1e-12); };
    const bool structural = moved (sp.L, spTuned.L, 1e-5) || moved (sp.d, spTuned.d, 1e-5) || moved (sp.wall, spTuned.wall, 1e-5)
                            || sp.metal != spTuned.metal || sp.head != spTuned.head || moved (sp.headD, spTuned.headD, 1e-5)
                            || std::abs (sp.strike - spTuned.strike) > 1e-5 || std::abs (sp.morph - spTuned.morph) > 1e-5
                            || std::abs (sp.damper - spTuned.damper) > 1e-5 || moved (sp.tempK, spTuned.tempK, 1e-5)
                            || moved (sp.c0, spTuned.c0, 1e-5) || moved (sp.rho0, spTuned.rho0, 1e-5);
    const bool fluidMoved = moved (sp.L, spFluid.L, 2e-3) || moved (sp.d, spFluid.d, 2e-3) || moved (sp.c0, spFluid.c0, 2e-3)
                            || moved (sp.rho0, spFluid.rho0, 2e-3) || moved (sp.wall, spFluid.wall, 5e-3) || sp.metal != spFluid.metal
                            || std::abs (sp.morph - spFluid.morph) > 2e-3;
    if (! nActive && ! flight) return;      // silent: brought up to date at the next stroke
    // retune at most every 4 blocks while a knob moves, the gas side every 16
    if (structural && framesSinceBC >= 4) retune (fluidMoved && framesSinceFluid >= 16);
    else if (fluidMoved && framesSinceFluid >= 16) retune (true);
}

void MalletModel::strike (double v0)
{
    // a new design when the pipe, the material or the stroke changed a lot since the modes were chosen
    auto far = [] (double x, double y, double tol) { return std::abs (x - y) > tol * (std::abs (y) + 1e-12); };
    const bool redesign = ! designed || sp.metal != spDesigned.metal || sp.head != spDesigned.head || far (sp.L, spDesigned.L, 0.03)
                          || far (sp.d, spDesigned.d, 0.03) || far (sp.wall, spDesigned.wall, 0.05) || far (sp.headD, spDesigned.headD, 0.1)
                          || std::abs (sp.strike - spDesigned.strike) > 0.02 || std::abs (sp.morph - spDesigned.morph) > 0.05
                          || far (sp.c0, spDesigned.c0, 0.05);
    if (redesign) design();
    else
    {
        const bool fl = std::abs (sp.L - spFluid.L) > 1e-6 * sp.L || std::abs (sp.d - spFluid.d) > 1e-6 * sp.d || sp.c0 != spFluid.c0
                        || sp.rho0 != spFluid.rho0 || sp.wall != spFluid.wall || sp.metal != spFluid.metal || sp.morph != spFluid.morph;
        retune (fl);
    }
    nActive = nSel;
    // the mallet arrives: one sample before touching the wall where it is now
    double ws = 0;
    for (int i = 0; i < nSel; ++i) ws += wq[i] * zi[i];
    y = ws + std::min (0.0, -v0 * T * 0.5);
    yd = std::max (0.01, v0);
    F = prevF = 0;
    delta = y - ws;
    flight = true;
    inContact = false;
    flightT = 0;
    lamHC = 0;
    v0Last = v0;
    fPeakLast = 0;
    tContactLast = 0;
    contactStart = -1;
    forceN = 0;
    ++strikes;
    contacts = 0;
    cModes = 0;
    for (int i = 0; i < nSel; ++i) cModes += wq[i] * g1i[i];
}

void MalletModel::render (int n, double* shell, double* q)
{
    // the felt damper comes on and off over ~10 ms
    {
        const double tgt = damperOn ? 1.0 : 0.0;
        if (damperAmt != tgt)
        {
            const double k = 1.0 - std::exp (-n * T / 0.01);
            damperAmt += (tgt - damperAmt) * k;
            if (std::abs (damperAmt - tgt) < 0.01) damperAmt = tgt;
            for (int i = 0; i < nSel; ++i) discretise (i);
            cModes = 0;
            for (int i = 0; i < nSel; ++i) cModes += wq[i] * g1i[i];
        }
    }
    const int na = (nActive + 1) & ~1;              // pairs (a silent slot rotates zeros)
    const double cV = a * a / (E * h);               // volume pushed in by the wall per newton (a long tube: a^2 / (E h))
    for (int t = 0; t < n; ++t)
    {
        if (flight)
        {
            // free prediction of this sample, then the contact force that makes mallet and wall agree
            alignas(16) double fr[kMaxModes], fi[kMaxModes];
            double wFree = 0;
            for (int k = 0; k < na; ++k)
            {
                fr[k] = pr[k] * zr[k] - pim[k] * zi[k] + g0r[k] * F;
                fi[k] = pr[k] * zi[k] + pim[k] * zr[k] + g0i[k] * F;
                wFree += wq[k] * fi[k];
            }
            const double yFree = y + T * yd - T * T * F / (3.0 * mh);
            const double dFree = yFree - wFree;
            const double C = T * T / (6.0 * mh) + cModes + cRes;
            double x = dFree, Fn = 0;
            if (dFree > 0)
            {
                if (! inContact)
                {
                    // first touch: Hunt-Crossley (Lankarani-Nikravesh) damping for the coefficient of restitution
                    vIn = std::max (0.01, (dFree - delta) * fs);
                    lamHC = 3.0 * (1.0 - eRest * eRest) / (4.0 * vIn);
                }
                double lo = 0, hi = dFree;
                x = dFree;
                for (int it = 0; it < 40; ++it)
                {
                    const double sx = std::sqrt (x), dmp = 1.0 + lamHC * (x - delta) * fs;
                    double f = Kh * x * sx * dmp, df = Kh * (1.5 * sx * dmp + x * sx * lamHC * fs);
                    if (f < 0) { f = 0; df = 0; }
                    const double g = x + C * f - dFree;
                    if (g > 0) hi = x; else lo = x;
                    double xn = x - g / (1.0 + C * df);
                    if (! (xn > lo && xn < hi)) xn = 0.5 * (lo + hi);
                    const bool done = std::abs (xn - x) <= 1e-13 * dFree;
                    x = xn;
                    if (done) break;
                }
                Fn = std::max (0.0, Kh * x * std::sqrt (x) * (1.0 + lamHC * (x - delta) * fs));
            }
            for (int k = 0; k < na; ++k)
            {
                zr[k] = fr[k] + g1r[k] * Fn;
                zi[k] = fi[k] + g1i[k] * Fn;
            }
            y = yFree - T * T * Fn / (6.0 * mh);
            yd -= T * (F + Fn) / (2.0 * mh);
            q[t] = cV * (Fn - F) * fs;
            // bookkeeping: contacts, the force record of the stroke
            if (Fn > 0)
            {
                if (! inContact) { inContact = true; ++contacts; if (contactStart < 0) contactStart = clock; }
                fPeakLast = std::max (fPeakLast, Fn);
            }
            else if (inContact)
            {
                inContact = false;
                if (contacts == 1) tContactLast = clock - contactStart;
            }
            if (contactStart >= 0 && forceN < 256) forceRec[forceN++] = (float) Fn;
            F = Fn;
            delta = x;
            flightT += T;
            // the player takes the mallet away: well clear of the wall, or 50 ms after the stroke
            if ((! inContact && flightT > 0.003 && x < -1e-3) || flightT > 0.05)
            {
                flight = false; inContact = false; F = 0;
                if (contacts == 1 && tContactLast == 0 && contactStart >= 0) tContactLast = clock - contactStart;
            }
            double out = 0;
            for (int k = 0; k < na; ++k) out += oa[k] * zr[k] + ob[k] * zi[k];
            shell[t] = out;
        }
        else
        {
            // free ringing for the rest of the block (a stroke starts only between blocks): complex rotation
            // of every mode (P5: SSE2, or AVX2 + FMA where the CPU has it, Simd.h)
            modalRing (n - t, na, zr, zi, pr, pim, oa, ob, shell + t);
            std::fill (q + t, q + n, 0.0);
            clock += (n - t) * T;
            break;
        }
        clock += T;
    }

    // modes that have died away stop costing anything (only while the mallet is off the wall)
    if (! flight)
    {
        for (int k = 0; k < nActive;)
        {
            const double amp = gAbs[k] * std::sqrt (zr[k] * zr[k] + zi[k] * zi[k]);
            const double disp = std::abs (zi[k]) / std::max (1.0, w0[k]);
            if (amp < 2e-8 && disp < 1e-12)
            {
                zr[k] = zi[k] = 0.0;
                const int last = nActive - 1;
                if (k != last)
                {
                    auto sw = [&] (auto* arr) { std::swap (arr[k], arr[last]); };
                    sw (zr); sw (zi); sw (pr); sw (pim); sw (g0r); sw (g0i); sw (g1r); sw (g1i); sw (wq); sw (oa); sw (ob);
                    sw (mn); sw (mm); sw (mbc); sw (w0); sw (wS); sw (sigma); sw (sigD); sw (mass); sw (phiS);
                    sw (radial); sw (fluidRatio); sw (etaRad); sw (fBend); sw (gAbs); sw (gArg); sw (ax);
                }
                --nActive;
            }
            else ++k;
        }
    }
}

void MalletModel::fillTelemetry (MalletTelemetry& t) const
{
    t.strikes = strikes;
    t.contacts = contacts;
    t.nModes = nSel;
    t.nActive = nActive;
    t.v0 = (float) v0Last;
    t.fPeak = (float) fPeakLast;
    t.tContact = (float) tContactLast;
    t.h = (float) h;
    t.K = (float) Kh;
    t.mass = (float) mh;
    // force record: the first forceN samples, shown over at most 2 x the contact time
    const int span = std::max (8, std::min (forceN, tContactLast > 0 ? (int) (2.0 * tContactLast * fs) + 4 : forceN));
    t.forceSpan = (float) (span * T);
    for (int j = 0; j < MalletTelemetry::kForce; ++j)
    {
        const int i = (int) ((double) j * span / MalletTelemetry::kForce);
        t.force[j] = i < forceN ? forceRec[i] : 0.0f;
    }
    for (float& r : t.ring) r = 0;
    for (float& b : t.bend) b = 0;
    for (int k = 0; k < nActive; ++k)
    {
        const double wd = std::sqrt (std::max (w0[k] * w0[k] - sigma[k] * sigma[k], 1e-6));
        const double qk = zi[k] / wd;
        if (mn[k] < MalletTelemetry::kRing) t.ring[mn[k]] += (float) (phiS[k] * qk);
        if (mn[k] == 1)
        {
            const BeamFn bf (lam[0][mm[k] - 1]);
            for (int j = 0; j < MalletTelemetry::kBend; ++j) t.bend[j] += (float) (bf.X (j / (double) (MalletTelemetry::kBend - 1)) * qk);
        }
    }
    // strongest radiating modes now
    struct L { double lv; int k; } best[MalletTelemetry::kModes];
    int nb = 0;
    for (int k = 0; k < nActive; ++k)
    {
        const double amp = gAbs[k] * std::sqrt (zr[k] * zr[k] + zi[k] * zi[k]);
        if (amp <= 0) continue;
        if (nb < MalletTelemetry::kModes) best[nb++] = { amp, k };
        else
        {
            int mi = 0;
            for (int j = 1; j < nb; ++j) if (best[j].lv < best[mi].lv) mi = j;
            if (amp > best[mi].lv) best[mi] = { amp, k };
        }
    }
    std::sort (best, best + nb, [] (const L& x, const L& y) { return x.lv > y.lv; });
    t.nShown = nb;
    for (int j = 0; j < nb; ++j)
    {
        const int k = best[j].k;
        t.modes[j] = { (float) (w0[k] / kTwoPi), (float) (20.0 * std::log10 (best[j].lv / std::sqrt (2.0) / 2e-5)),
                       (float) (sigma[k] > 0 ? 6.907755 / sigma[k] : 999.0), mn[k], mm[k] };
    }
}

} // namespace okl
