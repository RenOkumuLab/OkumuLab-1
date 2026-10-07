/*
 * OkumuLab 1 — the room (P5): a physical model of a stone church nave
 *
 * The impulse response from the organ to a spaced pair of microphones in the
 * nave is computed from the room itself, then convolved with the sound:
 *   - early reflections: image sources of the box-shaped nave (Allen & Berkley),
 *     every reflection with the absorption of the surfaces it met (octave bands),
 *     the share it keeps specular (scattering), the air it crossed (ISO 9613-1)
 *     and 1/distance, as a short linear-phase filter at its exact arrival time
 *   - diffuse tail: the reverberant energy density of the room, 4 pi c / V
 *     relative to the direct sound at 1 m, decaying per frequency with the Eyring
 *     reverberation time (surfaces + air); it grows in as reflections scatter
 *     (1 - (1 - s)^order) and takes over after the mixing time; the two
 *     microphones are correlated as in a diffuse field, sin(kd)/(kd)
 *   - convolution: uniform partitions in four stages (64 .. 4096 samples), the
 *     larger stages spread over the following blocks; no added latency (the
 *     first reflection arrives several milliseconds after the direct sound)
 *
 * No JUCE in here (also linked into the offline checks).
 */
#pragma once

#include <array>
#include <vector>

namespace okl
{

/* real FFT of a power-of-two size (float): n real <-> n/2 + 1 complex (split arrays) */
class RealFFT
{
public:
    void init (int n);
    int size() const { return n; }
    void forward (const float* in, float* re, float* im);
    /* scaled by 1 / n */
    void inverse (const float* re, const float* im, float* out);

private:
    void cfft (float* r, float* i, bool inv);
    int n = 0, m = 0;
    std::vector<float> cosT, sinT, wr, wi, zr, zi;
    std::vector<int> rev;
};

/* the room and where the organ and the microphones are */
struct RoomSpec
{
    static constexpr int kBands = 8;                      // octave bands 63 Hz .. 8 kHz
    double Lx = 34, Ly = 16, Lz = 16;                     // nave: length, width, height [m]
    double src[3] { 2.0, 8.0, 9.0 };                      // the organ in the west gallery
    double mic[3] { 16.0, 8.0, 1.7 };                     // the listener in the nave (centre of the pair)
    double micSpacing = 0.6;                              // spaced omni pair, left-right along y [m]
    // absorption coefficients by surface (x0 west, x1 east, y0, y1 side walls, z0 floor, z1 vault), per band
    double alpha[6][kBands] {};
    double scattering = 0.3;
    double tempC = 20.0, humidity = 50.0;                 // air in the room
    double mixingTime = 0.09;                             // image sources up to here [s after the direct sound]
};
RoomSpec defaultRoom();

struct RoomInfo
{
    double V = 0, S = 0, c = 343, meanFreePath = 0, directDist = 0, critDist = 0, firstReflection = 0;
    double t60[RoomSpec::kBands] {};
    double airDbPerKm[RoomSpec::kBands] {};
    int images = 0;
    double drRatioDb = 0;          // reverberant / direct energy at the microphones [dB]
};
constexpr double kBandHz[RoomSpec::kBands] { 63, 125, 250, 500, 1000, 2000, 4000, 8000 };

/* the room's stereo impulse response at fs (direct sound excluded, starts at its arrival); unit energy */
void roomImpulseResponse (const RoomSpec& r, double fs, std::vector<float>& L, std::vector<float>& R, RoomInfo* info);

/* air absorption [dB/m] at f (ISO 9613-1) */
double airAbsorptionDbPerM (double f, double tempC, double humidityPct, double pressureKPa = 101.325);

/* mono in -> stereo out, latency-free partitioned convolution (the impulse response must start after 64 samples) */
class Convolver
{
public:
    void prepare (const std::vector<float>& irL, const std::vector<float>& irR);
    void reset();
    void process (const float* in, float* outL, float* outR, int n);
    bool ready() const { return ! st.empty(); }

private:
    struct Stage
    {
        int B = 64, P = 0, offset = 0;
        RealFFT fft;
        std::vector<float> HLr, HLi, HRr, HRi;        // P x (B + 1) partition spectra
        std::vector<float> Xr, Xi;                    // P x (B + 1) input spectra (ring)
        std::vector<float> aLr, aLi, aRr, aRi;        // accumulators
        std::vector<float> frame, outT;
        bool busy = false;
        int step = 0;
        long long block = 0;
        double credit = 0;
    };
    void startJob (Stage& s, long long j);
    void doStep (Stage& s);
    std::vector<Stage> st;
    std::vector<float> hist, ringL, ringR;
    int hmask = 0, omask = 0;
    long long t = 0;
};

/* the room as the engine uses it: stereo in (summed), wet stereo out */
class RoomReverb
{
public:
    void prepare (double fs);
    void clear();
    void process (const double* inL, const double* inR, double* outL, double* outR, int n);
    const RoomInfo& info() const { return roomInfo; }

private:
    Convolver conv;
    RoomInfo roomInfo;
    std::vector<float> in, oL, oR;
};

} // namespace okl
