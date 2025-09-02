#include "engine.h"
#include <sndfile.h>
#include <fftw3.h>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <unordered_map>

using namespace std;

namespace {
    static const int    WINDOW_SIZE       = 1024;
    static const int    HOP_SIZE          = 512;
    static const int    PEAKS_PER_FRAME   = 5;
    static const int    MIN_FREQ_BIN      = 10;
    static const int    FAN_MIN_DT        = 1;
    static const int    FAN_MAX_DT        = 45;
    static const int    FAN_MAX_TARGETS   = 5;
}

struct Fingerprint {
    uint64_t hash;
    int songId;
    int offset;
};

struct Peak {
    int t;
    int f;
    float mag;
};

static inline uint64_t make_hash(uint16_t f1, uint16_t f2, uint16_t dt) {
    f1 &= 0x3FF; f2 &= 0x3FF; dt &= 0x0FFF;
    uint64_t h = (static_cast<uint64_t>(f1) << 22)
               | (static_cast<uint64_t>(f2) << 12)
               | (static_cast<uint64_t>(dt));
    return h;
}

static unordered_map<uint64_t, vector<pair<int,int>>> FP_DB;
static vector<Song> SONGS;
static std::mutex DB_MTX;
static std::string DATA_DIR = ".";

static bool load_audio_mono(const string& path, vector<double>& mono, int& rate) {
    SF_INFO info{};
    SNDFILE* snd = sf_open(path.c_str(), SFM_READ, &info);
    if (!snd) {
        cerr << "sf_open failed for " << path << "\n";
        return false;
    }
    rate = info.samplerate;
    vector<double> buf(static_cast<size_t>(info.frames) * info.channels);
    sf_count_t got = sf_readf_double(snd, buf.data(), info.frames);
    sf_close(snd);
    if (got <= 0) return false;
    mono.resize(static_cast<size_t>(got));
    if (info.channels == 1) {
        for (sf_count_t i = 0; i < got; ++i) mono[i] = buf[i];
    } else {
        for (sf_count_t i = 0; i < got; ++i) {
            double s = 0.0;
            for (int c = 0; c < info.channels; ++c)
                s += buf[static_cast<size_t>(i)*info.channels + c];
            mono[i] = s / info.channels;
        }
    }
    return true;
}

static void hann_window(vector<double>& win) {
    int N = static_cast<int>(win.size());
    for (int i = 0; i < N; ++i) {
        win[i] = 0.5 - 0.5 * cos(2.0 * M_PI * i / (N - 1));
    }
}

static void compute_spectrogram(const vector<double>& mono, vector<vector<float>>& spec) {
    if (mono.size() < static_cast<size_t>(WINDOW_SIZE)) { spec.clear(); return; }
    int numFrames = 1 + static_cast<int>((mono.size() - WINDOW_SIZE) / HOP_SIZE);
    spec.assign(numFrames, vector<float>(WINDOW_SIZE/2, 0.0f));

    vector<double> window(WINDOW_SIZE, 0.0);
    hann_window(window);

    double* in = (double*) fftw_malloc(sizeof(double) * WINDOW_SIZE);
    fftw_complex* out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * (WINDOW_SIZE/2 + 1));
    fftw_plan plan = fftw_plan_dft_r2c_1d(WINDOW_SIZE, in, out, FFTW_ESTIMATE);

    for (int t = 0; t < numFrames; ++t) {
        size_t start = static_cast<size_t>(t) * HOP_SIZE;
        for (int i = 0; i < WINDOW_SIZE; ++i) in[i] = mono[start + i] * window[i];
        fftw_execute(plan);
        for (int k = 0; k < WINDOW_SIZE/2; ++k) {
            double re = out[k][0], im = out[k][1];
            double mag = sqrt(re*re + im*im);
            float db = static_cast<float>(20.0 * log10(mag + 1e-9));
            spec[t][k] = db;
        }
    }
    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);
}

static void pick_peaks(const vector<vector<float>>& spec, vector<Peak>& peaks) {
    peaks.clear();
    if (spec.empty()) return;
    const int T = static_cast<int>(spec.size());
    const int F = static_cast<int>(spec[0].size());
    for (int t = 0; t < T; ++t) {
        vector<pair<float,int>> mags; mags.reserve(F);
        for (int f = MIN_FREQ_BIN; f < F; ++f) mags.emplace_back(spec[t][f], f);
        if ((int)mags.size() > PEAKS_PER_FRAME) {
            nth_element(mags.begin(), mags.begin()+PEAKS_PER_FRAME, mags.end(),
                        [](const auto& a, const auto& b){ return a.first > b.first; });
            mags.resize(PEAKS_PER_FRAME);
        }
        for (const auto& mf : mags) peaks.push_back(Peak{ t, mf.second, mf.first });
    }
    sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b){
        if (a.t != b.t) return a.t < b.t;
        return a.f < b.f;
    });
}

static void make_fingerprints(const vector<Peak>& peaks, vector<Fingerprint>& fps, int songId) {
    fps.clear();
    if (peaks.empty()) return;
    int maxT = 0; for (const auto& p : peaks) if (p.t > maxT) maxT = p.t;
    vector<vector<int>> byT(maxT + 1);
    for (int i = 0; i < (int)peaks.size(); ++i) byT[peaks[i].t].push_back(i);

    for (int i = 0; i < (int)peaks.size(); ++i) {
        const Peak& anchor = peaks[i];
        int fanCount = 0;
        for (int dt = FAN_MIN_DT; dt <= FAN_MAX_DT && fanCount < FAN_MAX_TARGETS; ++dt) {
            int tt = anchor.t + dt;
            if (tt > maxT) break;
            for (int idx : byT[tt]) {
                const Peak& target = peaks[idx];
                uint16_t f1 = static_cast<uint16_t>(anchor.f);
                uint16_t f2 = static_cast<uint16_t>(target.f);
                uint16_t d  = static_cast<uint16_t>(dt);
                uint64_t h = make_hash(f1, f2, d);
                fps.push_back(Fingerprint{ h, songId, anchor.t });
                ++fanCount;
                if (fanCount >= FAN_MAX_TARGETS) break;
            }
        }
    }
}

void engine_init(const std::string& data_dir) {
    DATA_DIR = data_dir;
    std::filesystem::create_directories(DATA_DIR);
    std::filesystem::create_directories(DATA_DIR + "/uploads");
    std::filesystem::create_directories(DATA_DIR + "/queries");
}

int add_song_to_db(const string& path, const string& displayName, const string& youtube_url) {
    vector<double> mono; int rate = 0;
    if (!load_audio_mono(path, mono, rate)) return -1;
    if (mono.size() < static_cast<size_t>(1024)) return -1;

    vector<vector<float>> spec; compute_spectrogram(mono, spec);
    if (spec.empty()) return -1;

    vector<Peak> peaks; pick_peaks(spec, peaks);
    if (peaks.empty()) return -1;

    vector<Fingerprint> fps; int songId;
    {
        std::lock_guard<std::mutex> lock(DB_MTX);
        songId = static_cast<int>(SONGS.size());
        make_fingerprints(peaks, fps, songId);
        if (fps.empty()) return -1;
        for (const auto& fp : fps) FP_DB[fp.hash].emplace_back(fp.songId, fp.offset);
        SONGS.push_back(Song{ songId, displayName, fps.size(), youtube_url });
    }
    cerr << "Added: [" << songId << "] " << displayName
         << " peaks=" << peaks.size()
         << " fps=" << fps.size() << "\n";
    return songId;
}

std::vector<Song> get_song_list() {
    std::lock_guard<std::mutex> lock(DB_MTX);
    return SONGS;
}

static string identify_from_samples(const vector<double>& mono) {
    if (mono.size() < static_cast<size_t>(1024)) return R"({"error":"too_short"})";
    {
        std::lock_guard<std::mutex> lock(DB_MTX);
        if (SONGS.empty()) return R"({"error":"db_empty"})";
    }
    vector<vector<float>> spec; compute_spectrogram(mono, spec);
    vector<Peak> peaks; pick_peaks(spec, peaks);
    vector<Fingerprint> qfps; make_fingerprints(peaks, qfps, -1);
    if (qfps.empty()) return R"({"error":"no_query_fps"})";

    unordered_map<int, unordered_map<int,int>> votes;
    int bestSong=-1, bestCount=0, bestOffset=0;

    {
        std::lock_guard<std::mutex> lock(DB_MTX);
        for (const auto& q : qfps) {
            auto it = FP_DB.find(q.hash);
            if (it == FP_DB.end()) continue;
            for (const auto& m : it->second) {
                int songId = m.first;
                int dbOffset = m.second;
                int delta = dbOffset - q.offset;
                int c = ++votes[songId][delta];
                if (c > bestCount) { bestCount = c; bestSong = songId; bestOffset = delta; }
            }
        }
    }

    if (bestSong < 0) return R"({"match":null,"score":0})";

    vector<pair<int,int>> perSong;
    for (auto& kv : votes) {
        int sId = kv.first;
        int top = 0;
        for (auto& dv : kv.second) top = max(top, dv.second);
        perSong.emplace_back(top, sId);
    }
    sort(perSong.begin(), perSong.end(), greater<>());

    std::ostringstream oss;
    oss << R"({"match":)" << bestSong
        << R"(,"name":")" << SONGS[bestSong].name << R"(")"
        // Add the URL for the main match
        << R"(,"url":")" << SONGS[bestSong].youtube_url << R"(")"
        << R"(,"score":)" << bestCount
        << R"(,"offset_frames":)" << bestOffset
        << R"(,"top":[)";
    for (size_t i=0;i<perSong.size() && i<5;++i) {
        if (i) oss << ",";
        oss << R"({"songId":)" << perSong[i].second
            << R"(,"name":")" << SONGS[perSong[i].second].name
            // Add the URL for each top candidate
            << R"(","url":")" << SONGS[perSong[i].second].youtube_url
            << R"(", "score":)" << perSong[i].first << "}";
    }
    oss << "]}";
    return oss.str();
}

std::string identify_from_file(const std::string& path) {
    vector<double> mono; int rate=0;
    if (!load_audio_mono(path, mono, rate)) return R"({"error":"load_failed"})";
    return identify_from_samples(mono);
}