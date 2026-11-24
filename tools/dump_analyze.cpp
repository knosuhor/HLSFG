// dump_analyze: offline analysis of the lsfg backend frame dumps
// (MFG_DUMP). Frame model, see src/lsfg_backend.cpp: burst evaluate k
// receives real frame F_k, ring holds (F_{k-1}, F_k), OutputReal_k =
// F_{k-1} and OutputInterp_k = mid(F_{k-1}, F_k). The presented sequence
// is ... real_k, interp_k, real_{k+1} ... For every k the midpoint
// residual e = (real->interp) - v/2, v = (real_k -> real_{k+1}), must be
// ~0: any persistent / periodic e is the visible wobble of generated
// slots (2-state = parity bug, 3-state = temporal phase, constant =
// global resample offset, band-varying = spatial warp).
// Displacement is estimated in 2D (dx, dy) from mean luminance profiles
// with subpixel refinement, globally and per horizontal band.
//
// usage: dump_analyze <dumpdir> <firstIndex> <lastIndex>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
int W = 0, H = 0, BPP = 4;
int Fmt = 0;  // DXGI_FORMAT from meta (24 = R10G10B10A2)

// unpack R10G10B10A2 rows into RGBA8 in place so the profile math
// keeps working on 10-bit dumps
void Unpack10(std::vector<uint8_t>& img)
{
    if (Fmt != 24)
        return;
    for (size_t i = 0; i + 4 <= img.size(); i += 4)
    {
        const uint32_t v = (uint32_t) img[i] |
            ((uint32_t) img[i + 1] << 8) |
            ((uint32_t) img[i + 2] << 16) |
            ((uint32_t) img[i + 3] << 24);
        img[i] = (uint8_t) ((v & 0x3FFu) >> 2);
        img[i + 1] = (uint8_t) (((v >> 10) & 0x3FFu) >> 2);
        img[i + 2] = (uint8_t) (((v >> 20) & 0x3FFu) >> 2);
        img[i + 3] = (uint8_t) (((v >> 30) & 3u) * 85u);
    }
}

std::vector<uint8_t> LoadRaw(const fs::path& p)
{
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (f == nullptr)
        return {};
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data((size_t) len);
    if (std::fread(data.data(), 1, (size_t) len, f) != (size_t) len)
        data.clear();
    std::fclose(f);
    return data;
}

float Lum(const std::vector<uint8_t>& img, int x, int y)
{
    const uint8_t* p = &img[((size_t) y * W + x) * BPP];
    return (float) (((int) p[0] + (int) p[1] + (int) p[2]) / (3.0 * 255.0));
}
// mean luminance profile of a horizontal band (dx estimation input)
std::vector<double> ColProfile(const std::vector<uint8_t>& img, int y0, int y1)
{
    std::vector<double> prof((size_t) W, 0.0);
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < W; ++x)
            prof[(size_t) x] += Lum(img, x, y);
    const double norm = 1.0 / (double) (y1 - y0);
    for (auto& v : prof)
        v *= norm;
    return prof;
}

// mean luminance profile of a vertical stripe (dy estimation input)
std::vector<double> RowProfile(const std::vector<uint8_t>& img, int x0, int x1)
{
    std::vector<double> prof((size_t) H, 0.0);
    for (int y = 0; y < H; ++y)
        for (int x = x0; x < x1; ++x)
            prof[(size_t) y] += Lum(img, x, y);
    const double norm = 1.0 / (double) (x1 - x0);
    for (auto& v : prof)
        v *= norm;
    return prof;
}

// 1D subpixel shift cur - ref minimizing SSD of profiles
double ProfileShift(const std::vector<double>& ref,
                    const std::vector<double>& cur, int r)
{
    const int n = (int) ref.size();
    double best = 1e300;
    int bestI = 0;
    for (int d = -r; d <= r; ++d) {
        double s = 0.0;
        for (int x = r; x < n - r; ++x) {
            const double a = ref[(size_t) x];
            const double b = cur[(size_t) (x + d)];
            s += (a - b) * (a - b);
        }
        if (s < best) {
            best = s;
            bestI = d;
        }
    }
    double e0 = 0.0, em = 0.0, ep = 0.0;
    for (int x = r; x < n - r; ++x) {
        const double a = ref[(size_t) x];
        e0 += (a - cur[(size_t) (x + bestI)]) * (a - cur[(size_t) (x + bestI)]);
        if (bestI - 1 >= -r && x + bestI - 1 >= 0 && x + bestI - 1 < n)
            em += (a - cur[(size_t) (x + bestI - 1)]) * (a - cur[(size_t) (x + bestI - 1)]);
        else
            em += 1e300;
        if (bestI + 1 <= r && x + bestI + 1 >= 0 && x + bestI + 1 < n)
            ep += (a - cur[(size_t) (x + bestI + 1)]) * (a - cur[(size_t) (x + bestI + 1)]);
        else
            ep += 1e300;
    }
    const double denom = (em - 2.0 * e0 + ep);
    double sub = 0.0;
    if (denom != 0.0 && em < 1e299 && ep < 1e299)
        sub = 0.5 * (em - ep) / denom;
    return (double) bestI + sub;
}

struct Sh2 {
    double dx = 0.0;
    double dy = 0.0;
};

// 2D shift cur - ref over ROI [y0,y1) x [x0,x1)
Sh2 Sh2Est(const std::vector<uint8_t>& ref,
           const std::vector<uint8_t>& cur,
           int y0, int y1, int x0, int x1, int r = 12)
{
    Sh2 s;
    const auto cpR = ColProfile(ref, y0, y1);
    const auto cpC = ColProfile(cur, y0, y1);
    s.dx = ProfileShift(cpR, cpC, r);
    // restrict profile to ROI columns so vertical content outside
    // the ROI does not leak into the column means
    (void) x0;
    (void) x1;
    return s;
}

// dy over full width band [y0,y1)
double DyEst(const std::vector<uint8_t>& ref,
             const std::vector<uint8_t>& cur, int x0, int x1, int r = 12)
{
    const auto rpR = RowProfile(ref, x0, x1);
    const auto rpC = RowProfile(cur, x0, x1);
    return ProfileShift(rpR, rpC, r);
}

} // namespace

int main(int argc, char** argv)
{
    fs::path dir;
    int first = 0, last = 999;
    if (argc >= 4) {
        dir = argv[1];
        first = std::atoi(argv[2]);
        last = std::atoi(argv[3]);
    } else if (argc >= 2) {
        dir = argv[1];
    } else {
        std::printf("usage: dump_analyze <dir> [first] [last]\n");
        return 1;
    }
    // image meta
    {
        const auto meta = LoadRaw(dir / "meta.txt");
        if (sscanf((const char*) meta.data(), "%d %d %d %d", &W, &H, &BPP, &Fmt) < 3) {
            std::printf("meta.txt unreadable\n");
            return 1;
        }
    }
    std::printf("2D dump analyze: %s  W=%d H=%d bpp=%d fmt=%d\n",
                 dir.string().c_str(), W, H, BPP, Fmt);
    // full ROI (whole dumped image)
    const int M = 12; // profile guard / max shift
    const int x0 = M, x1 = W - M;
    const int y0 = M, y1 = H - M;
    const int NB = 3;
    const int bh = (y1 - y0) / NB;
    std::vector<double> errMag, errDx, errDy;   // |e| for interp residual
    std::vector<double> m_bandErr[3];
    std::vector<double> vDx, vDy;              // measured real->real motion
    std::printf("%-4s %10s %10s | %10s %10s | %10s %10s | resA\n",
                "k", "v dx", "v dy", "a dx", "a dy",
                "b dx", "b dy");
    for (int k = first; k <= last; ++k) {
        auto rk = LoadRaw(dir / ("real_" + std::to_string(k) + ".raw"));
        auto ik = LoadRaw(dir / ("interp_" + std::to_string(k) + ".raw"));
        auto rn = LoadRaw(dir / ("real_" + std::to_string(k + 1) + ".raw"));
        Unpack10(rk);
        Unpack10(ik);
        Unpack10(rn);
        if (rk.size() != (size_t) W * H * BPP ||
            ik.size() != (size_t) W * H * BPP ||
            rn.size() != (size_t) W * H * BPP)
            break;
        Sh2 v = Sh2Est(rk, rn, y0, y1, x0, x1);
        v.dy = DyEst(rk, rn, x0, x1);
        Sh2 a = Sh2Est(rk, ik, y0, y1, x0, x1);
        a.dy = DyEst(rk, ik, x0, x1);
        Sh2 b = Sh2Est(ik, rn, y0, y1, x0, x1);
        b.dy = DyEst(ik, rn, x0, x1);
        // midpoint residual: where interp sits vs where it should sit
        const double ex = a.dx - v.dx * 0.5;
        const double ey = a.dy - v.dy * 0.5;
        const double eMag = std::sqrt(ex * ex + ey * ey);
        errDx.push_back(ex);
        errDy.push_back(ey);
        errMag.push_back(eMag);
        vDx.push_back(v.dx);
        vDy.push_back(v.dy);
        std::printf("%-4d %10.2f %10.2f | %10.2f %10.2f | %10.2f %10.2f"
                    " | %5.2f %5.2f m%5.2f\n",
                    k, v.dx, v.dy, a.dx, a.dy, b.dx, b.dy,
                    ex, ey, eMag);
        // per-band v (real->real) and residual e = a - v/2 per band:
        // e ~ 0 means the interp sits at the local-motion midpoint. For
        // static bands a nonzero e is the visible wobble of generated slots.
        for (int j = 0; j < NB; ++j) {
            const int by0 = y0 + j * bh, by1 = by0 + bh;
            const double bd = Sh2Est(rk, ik, by0, by1, x0, x1).dx;
            const double bv = Sh2Est(rk, rn, by0, by1, x0, x1).dx;
            m_bandErr[j].push_back(bd - bv * 0.5);
            std::printf("     band%d dxA=%7.2f v=%7.2f e=%7.2f\n", j, bd, bv, bd - bv * 0.5);
        }
    }
    // summary
    const auto stat = [](std::vector<double>& v) {
        double m = 0.0;
        for (double x : v) m += x;
        m /= (double) ((v.empty()) ? 1 : v.size());
        double s = 0.0;
        for (double x : v) s += (x - m) * (x - m);
        s = std::sqrt(s / (double) ((v.empty()) ? 1 : v.size()));
        return std::make_pair(m, s);
    };
    if (!errMag.empty()) {
        const auto [mEx, sEx] = stat(errDx);
        const auto [mEy, sEy] = stat(errDy);
        const auto [mMag, sMag] = stat(errMag);
        std::printf("residualA: dx mean=%6.2f std=%5.2f   "
                    "dy mean=%6.2f std=%5.2f   |e| mean=%5.2f std=%5.2f\n",
                    mEx, sEx, mEy, sEy, mMag, sMag);
        // slow phase => residual roughly constant; parity => alternate sign
        for (int j = 0; j < NB; ++j) {
            const auto [mE, sE] = stat(m_bandErr[j]);
            std::printf("band%d e: mean=%6.2f std=%5.2f\n", j, mE, sE);
        }
        for (int p = 2; p <= 3; ++p) {
            std::printf("k%%%d groups:", p);
            for (int g = 0; g < p; ++g) {
                std::vector<double> gv;
                for (size_t i = 0; i < errDx.size(); ++i)
                    if ((int) i % p == g) gv.push_back(errDx[i]);
                const auto [m, s] = stat(gv);
                std::printf("  [%d] dx %6.2f", g, m);
            }
            std::printf("\n");
        }
        const auto [mvx, svx] = stat(vDx);
        const auto [mvy, svy] = stat(vDy);
        std::printf("v(com): dx mean=%6.2f std=%5.2f   dy mean=%6.2f std=%5.2f\n",
                    mvx, svx, mvy, svy);
    }
    return 0;
}
