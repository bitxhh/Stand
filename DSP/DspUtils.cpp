#include "DspUtils.h"

namespace dsp {

std::vector<double> designLowpassFir(int numTaps, double cutoffNorm) {
    std::vector<double> h(numTaps);
    const int M   = numTaps - 1;
    const int mid = M / 2;

    for (int n = 0; n < numTaps; ++n) {
        double sinc;
        if (n == mid) {
            sinc = 2.0 * cutoffNorm;
        } else {
            const double x = 2.0 * kPi * cutoffNorm * (n - mid);
            sinc = std::sin(x) / (kPi * (n - mid));
        }
        const double win = 0.42
                         - 0.50 * std::cos(2.0 * kPi * n / M)
                         + 0.08 * std::cos(4.0 * kPi * n / M);
        h[n] = sinc * win;
    }

    double sum = 0.0;
    for (double v : h) sum += v;
    if (sum > 0.0)
        for (double& v : h) v /= sum;

    return h;
}

} // namespace dsp
