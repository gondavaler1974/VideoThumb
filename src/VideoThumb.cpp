// VideoThumb.wlx64 v1.4.2
// Total Commander video thumbnail WLX plugin.
//
// v1.4 adds:
//   * general uniform-frame rejection for black, white, gray, or colored backgrounds
//   * smooth fade/gradient rejection using sampled edge density and mean gradient
//   * richer frame scoring (detail + variance + color diversity) for fallback selection
//
// v1.3 added:
//   * black-title-card detection (black/dark background with sparse text)
//   * cheap luma distribution scoring so suspicious frames retry later positions
//   * best-quality fallback selection when every sampled frame is suspicious
//
// v1.2 added:
//   * near-black frame detection with cheap BGRA sampling
//   * automatic retry at later positions and a configurable percentage of duration
//
// v1.1 added:
//   * ZIP-disguised video support: first image in an archive becomes the thumbnail
//   * miniz ZIP reader with Win32 wide-path/random-access I/O (no temp extraction)
//   * WIC image decode directly from memory
//   * exception containment at the WLX DLL boundary
//   * exact-version FFmpeg DLL loading (no wildcard DLL selection)
//   * optional symlink->UNC fallback, disabled by default because direct libav works
//     on the tested path and the Win32 CreateFile fallback has no strict open timeout
//
// Normal video performance is unchanged: ZIP handling is only entered when TC's
// content buffer says the file is ZIP, or after normal libav decoding fails.

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <miniz.h>

// FFmpeg headers intentionally contain narrowing conversions in inline helper
// functions that MSVC reports as C4244. Suppress that warning only while
// parsing the third-party FFmpeg headers; warnings in VideoThumb itself remain
// fully enabled.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/version.h>
#include <libswresample/version.h>
#include <libswscale/swscale.h>
#include <libswscale/version.h>
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace {

HINSTANCE g_instance = nullptr;

// ------------------------------- settings ---------------------------------

std::wstring module_dir() {
    wchar_t buf[32768]{};
    const DWORD n = GetModuleFileNameW(g_instance, buf, static_cast<DWORD>(std::size(buf)));
    if (!n || n >= std::size(buf)) return L".";
    std::wstring p(buf, n);
    const auto pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    p.resize(pos);
    return p;
}

struct Settings {
    int cache_mb = 96;           // RAM cache ceiling
    int cache_entries = 256;     // and entry ceiling
    int max_parallel = 2;        // concurrent decode jobs
    int decoder_threads = 2;     // threads inside each FFmpeg decoder context
    int seek_ms = 1000;          // representative video frame position
    int timeout_ms = 12000;      // whole libav thumbnail decode timeout
    int packet_limit = 5000;     // safety limit after seek

    // A normal video pays only a tiny sampled-pixel test. Additional seeks happen
    // only when the candidate looks unsuitable (black/title/uniform/transition).
    bool black_frame_detection = true;
    int black_threshold = 20;        // luma <= this is considered dark (0..255)
    int black_pixel_percent = 95;    // retry if >= this percent of sampled pixels are dark

    // Black title cards often contain enough bright text to evade a pure-black test.
    // These checks are performed on the already-scaled thumbnail and are extremely cheap.
    bool title_card_detection = true;
    int title_card_dark_percent = 70;       // at least this much near-black area
    int title_card_midtone_max_percent = 20;// and at most this much midtone detail
    int title_card_mean_luma_max = 70;      // and a low overall average brightness

    // General-purpose boring/transition frame rejection. These metrics are sampled
    // on the small thumbnail, not on the full video frame.
    bool uniform_frame_detection = true;
    int max_luma_stddev = 18;               // low brightness variation => nearly flat
    int max_color_stddev = 16;              // low RGB variation => nearly one color
    int dominant_color_percent = 82;        // coarse RGB bin covers this much => one-color card

    bool transition_detection = true;
    int min_edge_percent = 2;               // too few meaningful local edges => low detail
    int edge_threshold = 20;                 // luma delta that counts as an edge
    int max_mean_gradient = 7;               // smooth fades/gradients stay below this

    std::vector<int> fallback_seek_ms{3000, 5000, 10000};
    int fallback_percent = 10;       // final retry at N% of clip duration; 0 disables

    bool zip_sequences = true;   // ZIP-disguised .mp4 etc. -> first image
    int zip_max_entry_mb = 64;   // decompression-bomb guard for one image entry
    int zip_max_candidates = 16; // try at most N naturally-first image entries
    int zip_max_source_mp = 100; // WIC source pixel guard (megapixels)

    // Direct libav already opens the tested local-symlink -> UNC path correctly.
    // Keep the older GetFinalPathNameByHandle retry as an opt-in compatibility path.
    bool resolve_symlink_fallback = false;

    Settings() {
        const std::wstring ini = module_dir() + L"\\VideoThumb.ini";
        auto ini_int = [&](const wchar_t* section, const wchar_t* key, int def) -> int {
            return static_cast<int>(GetPrivateProfileIntW(section, key, static_cast<UINT>(def), ini.c_str()));
        };

        cache_mb        = std::clamp(ini_int(L"Performance", L"CacheMB",        cache_mb),        0, 1024);
        cache_entries   = std::clamp(ini_int(L"Performance", L"CacheEntries",   cache_entries),   0, 4096);
        max_parallel    = std::clamp(ini_int(L"Performance", L"MaxParallel",    max_parallel),    1, 16);
        decoder_threads = std::clamp(ini_int(L"Performance", L"DecoderThreads", decoder_threads), 0, 32);
        seek_ms          = std::clamp(ini_int(L"Thumbnail",   L"SeekMs",         seek_ms),          0, 600000);
        timeout_ms       = std::clamp(ini_int(L"Performance", L"TimeoutMs",      timeout_ms),       1000, 120000);
        packet_limit     = std::clamp(ini_int(L"Performance", L"PacketLimit",    packet_limit),     100, 100000);

        black_frame_detection = ini_int(L"Thumbnail", L"BlackFrameDetection",
                                         black_frame_detection ? 1 : 0) != 0;
        black_threshold = std::clamp(ini_int(L"Thumbnail", L"BlackThreshold", black_threshold), 0, 255);
        black_pixel_percent = std::clamp(ini_int(L"Thumbnail", L"BlackPixelPercent",
                                                 black_pixel_percent), 50, 100);
        title_card_detection = ini_int(L"Thumbnail", L"TitleCardDetection",
                                       title_card_detection ? 1 : 0) != 0;
        title_card_dark_percent = std::clamp(ini_int(L"Thumbnail", L"TitleCardDarkPercent",
                                                     title_card_dark_percent), 40, 100);
        title_card_midtone_max_percent = std::clamp(ini_int(L"Thumbnail", L"TitleCardMidtoneMaxPercent",
                                                            title_card_midtone_max_percent), 0, 80);
        title_card_mean_luma_max = std::clamp(ini_int(L"Thumbnail", L"TitleCardMeanLumaMax",
                                                      title_card_mean_luma_max), 0, 255);

        uniform_frame_detection = ini_int(L"Thumbnail", L"UniformFrameDetection",
                                          uniform_frame_detection ? 1 : 0) != 0;
        max_luma_stddev = std::clamp(ini_int(L"Thumbnail", L"MaxLumaStdDev",
                                             max_luma_stddev), 0, 128);
        max_color_stddev = std::clamp(ini_int(L"Thumbnail", L"MaxColorStdDev",
                                              max_color_stddev), 0, 128);
        dominant_color_percent = std::clamp(ini_int(L"Thumbnail", L"DominantColorPercent",
                                                    dominant_color_percent), 40, 100);

        transition_detection = ini_int(L"Thumbnail", L"TransitionDetection",
                                       transition_detection ? 1 : 0) != 0;
        min_edge_percent = std::clamp(ini_int(L"Thumbnail", L"MinEdgePercent",
                                              min_edge_percent), 0, 100);
        edge_threshold = std::clamp(ini_int(L"Thumbnail", L"EdgeThreshold",
                                            edge_threshold), 1, 255);
        max_mean_gradient = std::clamp(ini_int(L"Thumbnail", L"MaxMeanGradient",
                                               max_mean_gradient), 0, 255);

        fallback_percent = std::clamp(ini_int(L"Thumbnail", L"FallbackPercent", fallback_percent), 0, 90);

        wchar_t fallback_buf[256]{};
        GetPrivateProfileStringW(L"Thumbnail", L"FallbackSeekMs", L"3000,5000,10000",
                                 fallback_buf, static_cast<DWORD>(std::size(fallback_buf)), ini.c_str());
        std::vector<int> parsed;
        const wchar_t* q = fallback_buf;
        while (*q && parsed.size() < 8) {
            while (*q && (*q < L'0' || *q > L'9')) ++q;
            if (!*q) break;
            int value = 0;
            while (*q >= L'0' && *q <= L'9') {
                const int digit = *q - L'0';
                if (value > 600000 / 10) { value = 600000; while (*q >= L'0' && *q <= L'9') ++q; break; }
                value = value * 10 + digit;
                ++q;
            }
            value = std::clamp(value, 0, 600000);
            if (value > 0 && std::find(parsed.begin(), parsed.end(), value) == parsed.end()) parsed.push_back(value);
        }
        fallback_seek_ms = std::move(parsed);

        zip_sequences      = ini_int(L"ZipSequence", L"Enabled",          zip_sequences ? 1 : 0) != 0;
        zip_max_entry_mb   = std::clamp(ini_int(L"ZipSequence", L"MaxEntryMB",       zip_max_entry_mb),   1, 512);
        zip_max_candidates = std::clamp(ini_int(L"ZipSequence", L"MaxCandidates",    zip_max_candidates), 1, 128);
        zip_max_source_mp  = std::clamp(ini_int(L"ZipSequence", L"MaxSourceMP",      zip_max_source_mp),  1, 1000);

        resolve_symlink_fallback = ini_int(L"Compatibility", L"ResolveSymlinkFallback",
                                            resolve_symlink_fallback ? 1 : 0) != 0;
    }
};

const Settings& settings() {
    static const Settings s;
    return s;
}

// --------------------------- Windows path helpers --------------------------

std::string utf8_from_wide(const wchar_t* s) {
    if (!s || !*s) return {};
    const int need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<size_t>(need), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, out.data(), need, nullptr, nullptr)) return {};
    out.resize(static_cast<size_t>(need - 1));
    return out;
}

std::wstring resolve_final_path(const wchar_t* input) {
    if (!input || !*input) return {};
    HANDLE h = CreateFileW(input,
                           FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    std::vector<wchar_t> buf(32768);
    const DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (!n || n >= buf.size()) return {};

    std::wstring p(buf.data(), n);
    // Convert Win32 "extended" syntax back to ordinary syntax FFmpeg accepts.
    constexpr wchar_t unc_prefix[] = L"\\\\?\\UNC\\";
    constexpr wchar_t ext_prefix[] = L"\\\\?\\";
    if (p.rfind(unc_prefix, 0) == 0) return L"\\\\" + p.substr(8);
    if (p.rfind(ext_prefix, 0) == 0) return p.substr(4);
    return p;
}

std::wstring normalized_cache_path(const wchar_t* p) {
    std::wstring s = p ? p : L"";
    std::replace(s.begin(), s.end(), L'/', L'\\');
    // Windows paths are case-insensitive in our intended use. Lower-casing avoids
    // duplicate cache entries if TC hands us the same path with different case.
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

// ------------------------------- bitmap ------------------------------------

struct ThumbData {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> bgra;

    size_t bytes() const noexcept { return bgra.size(); }
};

HBITMAP make_hbitmap(const ThumbData& t) {
    if (t.width <= 0 || t.height <= 0 || t.stride < t.width * 4 || t.bgra.empty()) return nullptr;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = t.width;
    bmi.bmiHeader.biHeight = -t.height; // top-down DIB, same orientation as swscale output
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = 0; // valid for BI_RGB; avoids an unnecessary DWORD narrowing

    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) return nullptr;

    auto* dst = static_cast<uint8_t*>(bits);
    const auto* src = t.bgra.data();
    const size_t row = static_cast<size_t>(t.width) * 4;
    for (int y = 0; y < t.height; ++y) {
        memcpy(dst + static_cast<size_t>(y) * t.width * 4,
               src + static_cast<size_t>(y) * t.stride,
               row);
    }
    return hbmp; // ownership transfers to Total Commander
}

// -------------------------------- cache ------------------------------------

struct CacheKey {
    std::wstring path;
    int max_w = 0;
    int max_h = 0;

    bool operator==(const CacheKey& o) const noexcept {
        return max_w == o.max_w && max_h == o.max_h && path == o.path;
    }
};

struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const noexcept {
        size_t h = std::hash<std::wstring>{}(k.path);
        h ^= static_cast<size_t>(k.max_w) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= static_cast<size_t>(k.max_h) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

class ThumbCache {
    struct Entry {
        std::shared_ptr<const ThumbData> data;
        std::list<CacheKey>::iterator lru_it;
        size_t bytes = 0;
    };

    std::mutex m_;
    std::list<CacheKey> lru_; // front = most recently used
    std::unordered_map<CacheKey, Entry, CacheKeyHash> map_;
    size_t total_bytes_ = 0;

public:
    std::shared_ptr<const ThumbData> get(const CacheKey& key) {
        if (settings().cache_mb == 0 || settings().cache_entries == 0) return {};
        std::lock_guard lock(m_);
        auto it = map_.find(key);
        if (it == map_.end()) return {};
        lru_.splice(lru_.begin(), lru_, it->second.lru_it);
        return it->second.data;
    }

    void put(CacheKey key, std::shared_ptr<const ThumbData> data) {
        if (!data || settings().cache_mb == 0 || settings().cache_entries == 0) return;
        const size_t limit_bytes = static_cast<size_t>(settings().cache_mb) * 1024ULL * 1024ULL;
        const size_t n = data->bytes();
        if (n == 0 || n > limit_bytes) return;

        std::lock_guard lock(m_);
        if (auto old = map_.find(key); old != map_.end()) {
            total_bytes_ -= old->second.bytes;
            lru_.erase(old->second.lru_it);
            map_.erase(old);
        }

        lru_.push_front(key);
        Entry e{std::move(data), lru_.begin(), n};
        total_bytes_ += n;
        map_.emplace(std::move(key), std::move(e));

        while (!lru_.empty() &&
               (map_.size() > static_cast<size_t>(settings().cache_entries) || total_bytes_ > limit_bytes)) {
            const CacheKey victim = lru_.back();
            auto it = map_.find(victim);
            if (it != map_.end()) {
                total_bytes_ -= it->second.bytes;
                map_.erase(it);
            }
            lru_.pop_back();
        }
    }
};

ThumbCache& cache() {
    static ThumbCache c;
    return c;
}

// -------------------------- bounded parallelism ----------------------------

HANDLE decode_semaphore() {
    static HANDLE h = CreateSemaphoreW(nullptr, settings().max_parallel, settings().max_parallel, nullptr);
    return h;
}

class DecodePermit {
    HANDLE h_ = nullptr;
    bool held_ = false;
public:
    DecodePermit() : h_(decode_semaphore()) {
        if (h_) held_ = (WaitForSingleObject(h_, static_cast<DWORD>(settings().timeout_ms)) == WAIT_OBJECT_0);
    }
    ~DecodePermit() { if (held_) ReleaseSemaphore(h_, 1, nullptr); }
    explicit operator bool() const noexcept { return held_; }
};

// ------------------------------- libav -------------------------------------

// v1.0 intentionally loads FFmpeg DLLs itself from the plugin directory.
// This avoids a common Windows plugin problem: dependent DLLs next to a WLX
// are not guaranteed to be in the host EXE's normal DLL search path.
struct FfmpegApi {
    HMODULE h_avutil = nullptr;
    HMODULE h_swresample = nullptr;
    HMODULE h_avcodec = nullptr;
    HMODULE h_avformat = nullptr;
    HMODULE h_swscale = nullptr;
    bool ready = false;

    decltype(&::av_log_set_level) p_av_log_set_level = nullptr;
    decltype(&::av_frame_alloc) p_av_frame_alloc = nullptr;
    decltype(&::av_frame_free) p_av_frame_free = nullptr;
    decltype(&::av_frame_unref) p_av_frame_unref = nullptr;

    decltype(&::av_packet_alloc) p_av_packet_alloc = nullptr;
    decltype(&::av_packet_free) p_av_packet_free = nullptr;
    decltype(&::av_packet_unref) p_av_packet_unref = nullptr;
    decltype(&::avcodec_alloc_context3) p_avcodec_alloc_context3 = nullptr;
    decltype(&::avcodec_parameters_to_context) p_avcodec_parameters_to_context = nullptr;
    decltype(&::avcodec_open2) p_avcodec_open2 = nullptr;
    decltype(&::avcodec_send_packet) p_avcodec_send_packet = nullptr;
    decltype(&::avcodec_receive_frame) p_avcodec_receive_frame = nullptr;
    decltype(&::avcodec_flush_buffers) p_avcodec_flush_buffers = nullptr;
    decltype(&::avcodec_free_context) p_avcodec_free_context = nullptr;

    decltype(&::avformat_network_init) p_avformat_network_init = nullptr;
    decltype(&::avformat_alloc_context) p_avformat_alloc_context = nullptr;
    decltype(&::avformat_open_input) p_avformat_open_input = nullptr;
    decltype(&::avformat_find_stream_info) p_avformat_find_stream_info = nullptr;
    decltype(&::av_find_best_stream) p_av_find_best_stream = nullptr;
    decltype(&::av_read_frame) p_av_read_frame = nullptr;
    decltype(&::av_seek_frame) p_av_seek_frame = nullptr;
    decltype(&::avformat_close_input) p_avformat_close_input = nullptr;

    decltype(&::sws_getContext) p_sws_getContext = nullptr;
    decltype(&::sws_scale) p_sws_scale = nullptr;
    decltype(&::sws_freeContext) p_sws_freeContext = nullptr;

    static std::wstring versioned_dll_path(const wchar_t* stem, int major) {
        return module_dir() + L"\\" + stem + std::to_wstring(major) + L".dll";
    }

    static bool module_is_expected(HMODULE h, const std::wstring& expected) {
        if (!h) return false;
        wchar_t buf[32768]{};
        const DWORD n = GetModuleFileNameW(h, buf, static_cast<DWORD>(std::size(buf)));
        if (!n || n >= std::size(buf)) return false;
        return _wcsicmp(buf, expected.c_str()) == 0;
    }

    static HMODULE load_exact(const wchar_t* stem, int major, bool optional = false) {
        const std::wstring path = versioned_dll_path(stem, major);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            return optional ? reinterpret_cast<HMODULE>(1) : nullptr;

        HMODULE h = LoadLibraryExW(path.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!h) return nullptr;

        // Defensive check: do not silently reuse a same-named FFmpeg DLL that a
        // different TC plugin may already have loaded from another directory.
        if (!module_is_expected(h, path)) {
            FreeLibrary(h);
            return nullptr;
        }
        return h;
    }

    template<class T>
    static bool load_proc(HMODULE h, const char* name, T& out) {
        if (!h || h == reinterpret_cast<HMODULE>(1)) return false;
        out = reinterpret_cast<T>(GetProcAddress(h, name));
        return out != nullptr;
    }

    FfmpegApi() {
        // Load dependency order first. swresample is not directly called, but
        // some avcodec builds depend on it.
        h_avutil = load_exact(L"avutil-", LIBAVUTIL_VERSION_MAJOR);
        if (!h_avutil) return;
        h_swresample = load_exact(L"swresample-", LIBSWRESAMPLE_VERSION_MAJOR, true);
        h_avcodec = load_exact(L"avcodec-", LIBAVCODEC_VERSION_MAJOR);
        if (!h_avcodec) return;
        h_avformat = load_exact(L"avformat-", LIBAVFORMAT_VERSION_MAJOR);
        if (!h_avformat) return;
        h_swscale = load_exact(L"swscale-", LIBSWSCALE_VERSION_MAJOR);
        if (!h_swscale) return;

        bool ok = true;
        ok &= load_proc(h_avutil, "av_log_set_level", p_av_log_set_level);
        ok &= load_proc(h_avutil, "av_frame_alloc", p_av_frame_alloc);
        ok &= load_proc(h_avutil, "av_frame_free", p_av_frame_free);
        ok &= load_proc(h_avutil, "av_frame_unref", p_av_frame_unref);

        ok &= load_proc(h_avcodec, "av_packet_alloc", p_av_packet_alloc);
        ok &= load_proc(h_avcodec, "av_packet_free", p_av_packet_free);
        ok &= load_proc(h_avcodec, "av_packet_unref", p_av_packet_unref);
        ok &= load_proc(h_avcodec, "avcodec_alloc_context3", p_avcodec_alloc_context3);
        ok &= load_proc(h_avcodec, "avcodec_parameters_to_context", p_avcodec_parameters_to_context);
        ok &= load_proc(h_avcodec, "avcodec_open2", p_avcodec_open2);
        ok &= load_proc(h_avcodec, "avcodec_send_packet", p_avcodec_send_packet);
        ok &= load_proc(h_avcodec, "avcodec_receive_frame", p_avcodec_receive_frame);
        ok &= load_proc(h_avcodec, "avcodec_flush_buffers", p_avcodec_flush_buffers);
        ok &= load_proc(h_avcodec, "avcodec_free_context", p_avcodec_free_context);

        ok &= load_proc(h_avformat, "avformat_network_init", p_avformat_network_init);
        ok &= load_proc(h_avformat, "avformat_alloc_context", p_avformat_alloc_context);
        ok &= load_proc(h_avformat, "avformat_open_input", p_avformat_open_input);
        ok &= load_proc(h_avformat, "avformat_find_stream_info", p_avformat_find_stream_info);
        ok &= load_proc(h_avformat, "av_find_best_stream", p_av_find_best_stream);
        ok &= load_proc(h_avformat, "av_read_frame", p_av_read_frame);
        ok &= load_proc(h_avformat, "av_seek_frame", p_av_seek_frame);
        ok &= load_proc(h_avformat, "avformat_close_input", p_avformat_close_input);

        ok &= load_proc(h_swscale, "sws_getContext", p_sws_getContext);
        ok &= load_proc(h_swscale, "sws_scale", p_sws_scale);
        ok &= load_proc(h_swscale, "sws_freeContext", p_sws_freeContext);

        ready = ok;
        if (ready) {
            p_av_log_set_level(AV_LOG_QUIET);
            p_avformat_network_init();
        }
    }
};

FfmpegApi& ff() {
    static FfmpegApi api;
    return api;
}

struct InterruptState {
    ULONGLONG deadline = 0;
};

int ff_interrupt_cb(void* opaque) {
    const auto* s = static_cast<const InterruptState*>(opaque);
    return s && GetTickCount64() >= s->deadline;
}

bool receive_one_frame(AVCodecContext* dec, AVFrame* frame) {
    return ff().p_avcodec_receive_frame(dec, frame) == 0;
}

bool decode_frame_from_current(AVFormatContext* fmt,
                               AVCodecContext* dec,
                               int video_index,
                               AVPacket* pkt,
                               AVFrame* frame,
                               int packet_limit) {
    int packets = 0;
    while (packets++ < packet_limit && ff().p_av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != video_index) {
            ff().p_av_packet_unref(pkt);
            continue;
        }

        int s = ff().p_avcodec_send_packet(dec, pkt);
        if (s == AVERROR(EAGAIN)) {
            if (receive_one_frame(dec, frame)) {
                ff().p_av_packet_unref(pkt);
                return true;
            }
            // Retry the same packet after draining; EAGAIN means it was not consumed.
            s = ff().p_avcodec_send_packet(dec, pkt);
        }
        ff().p_av_packet_unref(pkt);
        if (s < 0) continue;

        for (;;) {
            const int r = ff().p_avcodec_receive_frame(dec, frame);
            if (r == 0) return true;
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            break;
        }
    }

    // Flush delayed frames at EOF.
    ff().p_avcodec_send_packet(dec, nullptr);
    return receive_one_frame(dec, frame);
}

std::shared_ptr<ThumbData> scale_frame_to_thumb(const AVFrame* frame, int max_w, int max_h) {
    if (!frame || frame->width <= 0 || frame->height <= 0 || frame->format < 0 || max_w <= 0 || max_h <= 0) return {};

    const double sx = static_cast<double>(max_w) / frame->width;
    const double sy = static_cast<double>(max_h) / frame->height;
    const double scale = std::min(sx, sy);
    const int out_w = std::max(1, static_cast<int>(std::floor(frame->width * scale + 0.5)));
    const int out_h = std::max(1, static_cast<int>(std::floor(frame->height * scale + 0.5)));

    auto out = std::make_shared<ThumbData>();
    out->width = out_w;
    out->height = out_h;
    out->stride = out_w * 4;
    out->bgra.resize(static_cast<size_t>(out->stride) * out_h);

    SwsContext* sws = ff().p_sws_getContext(frame->width,
                                     frame->height,
                                     static_cast<AVPixelFormat>(frame->format),
                                     out_w,
                                     out_h,
                                     AV_PIX_FMT_BGRA,
                                     SWS_FAST_BILINEAR,
                                     nullptr,
                                     nullptr,
                                     nullptr);
    if (!sws) return {};

    uint8_t* dst_data[4] = { out->bgra.data(), nullptr, nullptr, nullptr };
    int dst_linesize[4] = { out->stride, 0, 0, 0 };
    const int rows = ff().p_sws_scale(sws,
                               frame->data,
                               frame->linesize,
                               0,
                               frame->height,
                               dst_data,
                               dst_linesize);
    ff().p_sws_freeContext(sws);
    if (rows <= 0) return {};
    return out;
}

// Cheap thumbnail statistics used to reject boring/transition frames. Sampling
// every fourth pixel in both directions examines only ~1/16 of the thumbnail.
// The extra math is tiny compared with a video seek/decode.
struct FrameStats {
    double dark_ratio = 0.0;       // luma <= black threshold
    double mid_ratio = 0.0;        // between black threshold and 180
    double bright_ratio = 0.0;     // luma >= 180
    double mean_luma = 0.0;        // 0..255
    double luma_stddev = 0.0;      // global brightness variation
    double color_stddev = 0.0;     // RMS of per-channel standard deviations
    double dominant_color_ratio = 0.0; // largest coarse RGB bin (8x8x8)
    double edge_ratio = 0.0;       // local luma deltas >= EdgeThreshold
    double mean_gradient = 0.0;    // mean local luma delta across sampled neighbors
};

inline int pixel_luma(const uint8_t* p) noexcept {
    return (29 * p[0] + 150 * p[1] + 77 * p[2] + 128) >> 8; // BGRA -> luma
}

FrameStats frame_stats(const ThumbData& t, const Settings& cfg) noexcept {
    FrameStats out{};
    if (t.width <= 0 || t.height <= 0 || t.stride < t.width * 4 || t.bgra.empty()) return out;

    uint64_t dark = 0, mid = 0, bright = 0, total = 0;
    uint64_t luma_sum = 0, luma_sq_sum = 0;
    uint64_t b_sum = 0, g_sum = 0, r_sum = 0;
    uint64_t b_sq_sum = 0, g_sq_sum = 0, r_sq_sum = 0;
    uint64_t edge_count = 0, gradient_count = 0, gradient_sum = 0;
    std::array<uint32_t, 512> color_bins{}; // 3 bits/channel: near colors share a bin
    uint32_t dominant_count = 0;

    constexpr int step = 4;
    constexpr int bright_threshold = 180;

    for (int y = 0; y < t.height; y += step) {
        const uint8_t* row = t.bgra.data() + static_cast<size_t>(y) * t.stride;
        for (int x = 0; x < t.width; x += step) {
            const uint8_t* p = row + static_cast<size_t>(x) * 4;
            const int b = p[0], g = p[1], r = p[2];
            const int luma = pixel_luma(p);

            if (luma <= cfg.black_threshold) ++dark;
            else if (luma >= bright_threshold) ++bright;
            else ++mid;

            luma_sum += static_cast<uint64_t>(luma);
            luma_sq_sum += static_cast<uint64_t>(luma) * luma;
            b_sum += static_cast<uint64_t>(b); g_sum += static_cast<uint64_t>(g); r_sum += static_cast<uint64_t>(r);
            b_sq_sum += static_cast<uint64_t>(b) * b;
            g_sq_sum += static_cast<uint64_t>(g) * g;
            r_sq_sum += static_cast<uint64_t>(r) * r;

            const unsigned bin = (static_cast<unsigned>(r) >> 5U) << 6U |
                                 (static_cast<unsigned>(g) >> 5U) << 3U |
                                 (static_cast<unsigned>(b) >> 5U);
            const uint32_t count = ++color_bins[bin];
            if (count > dominant_count) dominant_count = count;

            if (x + step < t.width) {
                const int other = pixel_luma(row + static_cast<size_t>(x + step) * 4);
                const unsigned diff = static_cast<unsigned>(std::abs(luma - other));
                gradient_sum += diff; ++gradient_count;
                if (diff >= static_cast<unsigned>(cfg.edge_threshold)) ++edge_count;
            }
            if (y + step < t.height) {
                const uint8_t* row2 = t.bgra.data() + static_cast<size_t>(y + step) * t.stride;
                const int other = pixel_luma(row2 + static_cast<size_t>(x) * 4);
                const unsigned diff = static_cast<unsigned>(std::abs(luma - other));
                gradient_sum += diff; ++gradient_count;
                if (diff >= static_cast<unsigned>(cfg.edge_threshold)) ++edge_count;
            }
            ++total;
        }
    }

    if (!total) return out;
    const double n = static_cast<double>(total);
    const double inv = 1.0 / n;
    out.dark_ratio = static_cast<double>(dark) * inv;
    out.mid_ratio = static_cast<double>(mid) * inv;
    out.bright_ratio = static_cast<double>(bright) * inv;
    out.mean_luma = static_cast<double>(luma_sum) * inv;

    auto stddev_from = [n](uint64_t sum, uint64_t sq_sum) noexcept -> double {
        const double mean = static_cast<double>(sum) / n;
        const double variance = std::max(0.0, static_cast<double>(sq_sum) / n - mean * mean);
        return std::sqrt(variance);
    };
    out.luma_stddev = stddev_from(luma_sum, luma_sq_sum);
    const double b_sd = stddev_from(b_sum, b_sq_sum);
    const double g_sd = stddev_from(g_sum, g_sq_sum);
    const double r_sd = stddev_from(r_sum, r_sq_sum);
    out.color_stddev = std::sqrt((b_sd * b_sd + g_sd * g_sd + r_sd * r_sd) / 3.0);
    out.dominant_color_ratio = static_cast<double>(dominant_count) * inv;

    if (gradient_count) {
        out.edge_ratio = static_cast<double>(edge_count) / static_cast<double>(gradient_count);
        out.mean_gradient = static_cast<double>(gradient_sum) / static_cast<double>(gradient_count);
    }
    return out;
}

bool frame_analysis_enabled(const Settings& cfg) noexcept {
    return cfg.black_frame_detection || cfg.title_card_detection ||
           cfg.uniform_frame_detection || cfg.transition_detection;
}

bool suspicious_frame(const FrameStats& st, const Settings& cfg) noexcept {
    if (cfg.black_frame_detection) {
        const double black_cutoff = static_cast<double>(cfg.black_pixel_percent) / 100.0;
        if (st.dark_ratio >= black_cutoff) return true;
    }

    if (cfg.title_card_detection) {
        const double title_dark = static_cast<double>(cfg.title_card_dark_percent) / 100.0;
        const double title_mid = static_cast<double>(cfg.title_card_midtone_max_percent) / 100.0;
        // Typical black title card: mostly near-black background, small white/colored
        // glyph area, little midtone/image texture, and low average brightness.
        if (st.dark_ratio >= title_dark &&
            st.mid_ratio <= title_mid &&
            st.mean_luma <= static_cast<double>(cfg.title_card_mean_luma_max)) return true;
    }

    if (cfg.uniform_frame_detection) {
        const double dominant_cutoff = static_cast<double>(cfg.dominant_color_percent) / 100.0;
        // Catches black/white/gray/colored cards, including sparse text on a flat field.
        if (st.dominant_color_ratio >= dominant_cutoff) return true;
        // Also catch nearly uniform colors that straddle two neighboring coarse RGB bins.
        if (st.luma_stddev <= static_cast<double>(cfg.max_luma_stddev) &&
            st.color_stddev <= static_cast<double>(cfg.max_color_stddev)) return true;
    }

    if (cfg.transition_detection) {
        const double min_edges = static_cast<double>(cfg.min_edge_percent) / 100.0;
        // Smooth fades/gradients may have substantial global brightness variation, but
        // almost no local edges and only tiny neighbor-to-neighbor changes.
        if (st.edge_ratio < min_edges &&
            st.mean_gradient <= static_cast<double>(cfg.max_mean_gradient)) return true;
    }

    return false;
}

// Higher is better. Used when every sampled position is suspicious. Reward
// useful local detail and tonal/color variation, and penalize one-color fields.
double frame_quality_score(const FrameStats& st) noexcept {
    return st.edge_ratio * 2200.0 +
           st.luma_stddev * 9.0 +
           st.color_stddev * 5.0 +
           st.mid_ratio * 180.0 +
           (1.0 - st.dominant_color_ratio) * 420.0 +
           std::min(st.mean_gradient, 40.0) * 4.0 -
           st.dark_ratio * 120.0;
}

// -------------------------- ZIP image sequences ----------------------------

bool has_zip_signature(const char* buf, int len) noexcept {
    if (!buf || len < 4) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(buf);
    return p[0] == 'P' && p[1] == 'K' &&
           ((p[2] == 3 && p[3] == 4) ||
            (p[2] == 5 && p[3] == 6) ||
            (p[2] == 7 && p[3] == 8));
}

bool is_image_entry_name(std::string_view name) noexcept {
    const size_t slash = name.find_last_of("/\\");
    const size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return false;
    std::string ext(name.substr(dot));
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" ||
           ext == ".gif" || ext == ".tif" || ext == ".tiff" || ext == ".webp" ||
           ext == ".heic" || ext == ".heif";
}

int natural_compare_ascii_ci(std::string_view a, std::string_view b) noexcept {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            const size_t za0 = i, zb0 = j;
            while (i < a.size() && a[i] == '0') ++i;
            while (j < b.size() && b[j] == '0') ++j;
            const size_t na = i, nb = j;
            while (i < a.size() && a[i] >= '0' && a[i] <= '9') ++i;
            while (j < b.size() && b[j] >= '0' && b[j] <= '9') ++j;
            const size_t la = i - na, lb = j - nb;
            if (la != lb) return la < lb ? -1 : 1;
            const int cmp = a.substr(na, la).compare(b.substr(nb, lb));
            if (cmp != 0) return cmp < 0 ? -1 : 1;
            const size_t zca = na - za0, zcb = nb - zb0;
            if (zca != zcb) return zca < zcb ? -1 : 1;
            continue;
        }

        auto lower_ascii = [](unsigned char c) noexcept -> unsigned char {
            return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
        };
        const unsigned char la = lower_ascii(ca), lb = lower_ascii(cb);
        if (la != lb) return la < lb ? -1 : 1;
        ++i; ++j;
    }
    if (i == a.size() && j == b.size()) return 0;
    return i == a.size() ? -1 : 1;
}

struct ZipWin32Reader {
    HANDLE h = INVALID_HANDLE_VALUE;
};

size_t zip_win32_read(void* opaque, mz_uint64 file_ofs, void* buf, size_t n) {
    auto* r = static_cast<ZipWin32Reader*>(opaque);
    if (!r || r->h == INVALID_HANDLE_VALUE || !buf || n == 0 ||
        file_ofs > static_cast<mz_uint64>(std::numeric_limits<LONGLONG>::max())) return 0;

    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(file_ofs);
    if (!SetFilePointerEx(r->h, pos, nullptr, FILE_BEGIN)) return 0;

    size_t total = 0;
    auto* out = static_cast<unsigned char*>(buf);
    while (total < n) {
        const size_t remain = n - total;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remain, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(r->h, out + total, chunk, &got, nullptr) || got == 0) break;
        total += got;
        if (got < chunk) break;
    }
    return total;
}

struct ZipCandidate {
    mz_uint index = 0;
    std::string name;
    mz_uint64 uncompressed_size = 0;
};

class ComScope {
    HRESULT hr_ = E_FAIL;
public:
    ComScope() noexcept : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    bool usable() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
};

std::shared_ptr<ThumbData> decode_image_wic(const std::vector<uint8_t>& bytes, int max_w, int max_h) {
    if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max()) ||
        max_w <= 0 || max_h <= 0) return {};

    ComScope com;
    if (!com.usable()) return {};

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return {};

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return {};
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())))) return {};

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return {};

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return {};

    UINT src_w = 0, src_h = 0;
    if (FAILED(frame->GetSize(&src_w, &src_h)) || src_w == 0 || src_h == 0) return {};

    const uint64_t source_pixels = static_cast<uint64_t>(src_w) * src_h;
    const uint64_t max_pixels = static_cast<uint64_t>(settings().zip_max_source_mp) * 1000000ULL;
    if (source_pixels > max_pixels) return {};

    const double sx = static_cast<double>(max_w) / src_w;
    const double sy = static_cast<double>(max_h) / src_h;
    const double scale = std::min(sx, sy);
    const UINT out_w = std::max<UINT>(1, static_cast<UINT>(std::floor(src_w * scale + 0.5)));
    const UINT out_h = std::max<UINT>(1, static_cast<UINT>(std::floor(src_h * scale + 0.5)));

    ComPtr<IWICBitmapSource> source;
    if (FAILED(frame.As(&source))) return {};
    ComPtr<IWICBitmapScaler> scaler;
    if (out_w != src_w || out_h != src_h) {
        if (FAILED(factory->CreateBitmapScaler(&scaler))) return {};
        if (FAILED(scaler->Initialize(frame.Get(), out_w, out_h, WICBitmapInterpolationModeFant))) return {};
        if (FAILED(scaler.As(&source))) return {};
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return {};
    if (FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) return {};

    const uint64_t stride64 = static_cast<uint64_t>(out_w) * 4ULL;
    const uint64_t bytes64 = stride64 * out_h;
    if (stride64 > static_cast<uint64_t>(std::numeric_limits<UINT>::max()) ||
        bytes64 > static_cast<uint64_t>(std::numeric_limits<UINT>::max())) return {};

    auto out = std::make_shared<ThumbData>();
    out->width = static_cast<int>(out_w);
    out->height = static_cast<int>(out_h);
    out->stride = static_cast<int>(stride64);
    out->bgra.resize(static_cast<size_t>(bytes64));

    if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(stride64),
                                     static_cast<UINT>(bytes64), out->bgra.data()))) return {};
    return out;
}

std::shared_ptr<ThumbData> decode_zip_first_image(const wchar_t* file, int max_w, int max_h) {
    if (!settings().zip_sequences || !file || !*file) return {};

    HANDLE h = CreateFileW(file, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 22) {
        CloseHandle(h);
        return {};
    }

    ZipWin32Reader reader{h};
    mz_zip_archive zip{};
    zip.m_pRead = zip_win32_read;
    zip.m_pIO_opaque = &reader;

    if (!mz_zip_reader_init(&zip, static_cast<mz_uint64>(size.QuadPart), 0)) {
        CloseHandle(h);
        return {};
    }

    std::shared_ptr<ThumbData> result;
    try {
        std::vector<ZipCandidate> candidates;
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        const uint64_t max_entry = static_cast<uint64_t>(settings().zip_max_entry_mb) * 1024ULL * 1024ULL;
        const size_t keep = static_cast<size_t>(settings().zip_max_candidates);

        for (mz_uint i = 0; i < count; ++i) {
            if (mz_zip_reader_is_file_a_directory(&zip, i) || mz_zip_reader_is_file_encrypted(&zip, i)) continue;
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
            if (st.m_uncomp_size == 0 || st.m_uncomp_size > max_entry || !is_image_entry_name(st.m_filename)) continue;

            ZipCandidate c{i, st.m_filename, st.m_uncomp_size};
            candidates.push_back(std::move(c));
            std::sort(candidates.begin(), candidates.end(), [](const ZipCandidate& a, const ZipCandidate& b) {
                return natural_compare_ascii_ci(a.name, b.name) < 0;
            });
            if (candidates.size() > keep) candidates.resize(keep);
        }

        for (const auto& c : candidates) {
            if (c.uncompressed_size > static_cast<mz_uint64>(std::numeric_limits<size_t>::max())) continue;
            std::vector<uint8_t> image(static_cast<size_t>(c.uncompressed_size));
            if (!mz_zip_reader_extract_to_mem(&zip, c.index, image.data(), image.size(), 0)) continue;
            result = decode_image_wic(image, max_w, max_h);
            if (result) break;
        }
    } catch (...) {
        mz_zip_reader_end(&zip);
        CloseHandle(h);
        throw;
    }

    mz_zip_reader_end(&zip);
    CloseHandle(h);
    return result;
}

std::shared_ptr<ThumbData> decode_with_libav_utf8(const std::string& input, int max_w, int max_h) {
    if (input.empty()) return {};
    if (!ff().ready) return {};

    InterruptState interrupt{GetTickCount64() + static_cast<ULONGLONG>(settings().timeout_ms)};
    AVFormatContext* fmt = ff().p_avformat_alloc_context();
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    const AVCodec* decoder = nullptr;
    int vi = -1;
    std::shared_ptr<ThumbData> result;

    if (!fmt) return {};
    fmt->interrupt_callback.callback = ff_interrupt_cb;
    fmt->interrupt_callback.opaque = &interrupt;

    if (ff().p_avformat_open_input(&fmt, input.c_str(), nullptr, nullptr) < 0 || !fmt) goto done;
    if (ff().p_avformat_find_stream_info(fmt, nullptr) < 0) goto done;

    vi = ff().p_av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (vi < 0 || !decoder || vi >= static_cast<int>(fmt->nb_streams)) goto done;

    dec = ff().p_avcodec_alloc_context3(decoder);
    if (!dec) goto done;
    if (ff().p_avcodec_parameters_to_context(dec, fmt->streams[vi]->codecpar) < 0) goto done;

    if (settings().decoder_threads > 0) dec->thread_count = settings().decoder_threads;
    if (ff().p_avcodec_open2(dec, decoder, nullptr) < 0) goto done;

    pkt = ff().p_av_packet_alloc();
    frame = ff().p_av_frame_alloc();
    if (!pkt || !frame) goto done;

    {
        const auto& cfg = settings();
        std::vector<int64_t> targets_us;
        targets_us.reserve(2 + cfg.fallback_seek_ms.size());

        auto add_target = [&](int64_t target_us) {
            if (target_us < 0) return;
            if (fmt->duration > 0 && target_us >= fmt->duration) return;
            // Avoid repeated seeks to almost the same place.
            for (const int64_t old : targets_us) {
                const int64_t delta = old > target_us ? old - target_us : target_us - old;
                if (delta < 100000) return; // within 100 ms
            }
            targets_us.push_back(target_us);
        };

        int64_t first_us = static_cast<int64_t>(cfg.seek_ms) * 1000LL;
        if (fmt->duration > 0 && first_us >= fmt->duration) first_us = fmt->duration / 3;
        add_target(std::max<int64_t>(0, first_us));

        const bool analyze_frames = frame_analysis_enabled(cfg);
        if (analyze_frames) {
            for (const int ms : cfg.fallback_seek_ms) add_target(static_cast<int64_t>(ms) * 1000LL);
            if (fmt->duration > 0 && cfg.fallback_percent > 0) {
                add_target((fmt->duration * static_cast<int64_t>(cfg.fallback_percent)) / 100);
            }
        }

        // Preserve the old short/odd-file recovery path if every requested seek fails.
        bool zero_tried = false;
        std::shared_ptr<ThumbData> best_suspicious;
        double best_quality = -1.0;

        auto try_target = [&](int64_t target_us) -> bool {
            if (GetTickCount64() >= interrupt.deadline) return false;

            ff().p_av_frame_unref(frame);
            ff().p_avcodec_flush_buffers(dec);
            if (target_us == 0) zero_tried = true;
            if (ff().p_av_seek_frame(fmt, -1, target_us, AVSEEK_FLAG_BACKWARD) < 0) return false;
            if (!decode_frame_from_current(fmt, dec, vi, pkt, frame, cfg.packet_limit)) return false;

            std::shared_ptr<ThumbData> thumb;
            try {
                thumb = scale_frame_to_thumb(frame, max_w, max_h);
            } catch (...) {
                return false;
            }
            if (!thumb) return false;

            if (!analyze_frames) {
                result = std::move(thumb);
                return true;
            }

            const FrameStats stats = frame_stats(*thumb, cfg);
            if (!suspicious_frame(stats, cfg)) {
                result = std::move(thumb);
                return true;
            }

            // If every sampled position is suspicious, keep the visually richest
            // candidate instead of returning a flat card, fade, or sparse title card.
            const double quality = frame_quality_score(stats);
            if (!best_suspicious || quality > best_quality) {
                best_suspicious = std::move(thumb);
                best_quality = quality;
            }
            return false;
        };

        for (const int64_t target_us : targets_us) {
            if (try_target(target_us)) break;
        }

        if (!result && !zero_tried && try_target(0)) {
            // result set by try_target
        }
        if (!result && best_suspicious) result = std::move(best_suspicious);
    }

done:
    if (frame) ff().p_av_frame_free(&frame);
    if (pkt) ff().p_av_packet_free(&pkt);
    if (dec) ff().p_avcodec_free_context(&dec);
    if (fmt) ff().p_avformat_close_input(&fmt);
    return result;
}

std::shared_ptr<ThumbData> decode_path(const wchar_t* file, int max_w, int max_h,
                                           const char* contentbuf, int contentbuflen) {
    const bool content_known = contentbuf && contentbuflen >= 4;
    const bool zip_hint = has_zip_signature(contentbuf, contentbuflen);

    // For ZIP-disguised .mp4 files TC often gives us the first bytes for free.
    // In that case skip FFmpeg probing entirely: this makes the new feature faster
    // without adding any work to normal videos.
    if (zip_hint) {
        if (auto t = decode_zip_first_image(file, max_w, max_h)) return t;
    }

    const std::string original = utf8_from_wide(file);
    if (!original.empty()) {
        if (auto t = decode_with_libav_utf8(original, max_w, max_h)) return t;
    }

    // If TC supplied a non-ZIP header, do not reopen a failed normal video just to
    // ask miniz whether it is a ZIP. If no content buffer was supplied, try once.
    if (!zip_hint && !content_known) {
        if (auto t = decode_zip_first_image(file, max_w, max_h)) return t;
    }

    // Compatibility-only fallback. Disabled by default because direct libav works
    // with our local-directory-symlink -> UNC path, and CreateFileW itself has no
    // strict per-call timeout on a dead SMB target.
    if (settings().resolve_symlink_fallback) {
        const std::wstring final_path = resolve_final_path(file);
        if (!final_path.empty() && final_path != file) {
            const std::string u8 = utf8_from_wide(final_path.c_str());
            if (!u8.empty()) {
                if (auto t = decode_with_libav_utf8(u8, max_w, max_h)) return t;
            }
            if (settings().zip_sequences) {
                if (auto t = decode_zip_first_image(final_path.c_str(), max_w, max_h)) return t;
            }
        }
    }
    return {};
}

HBITMAP make_thumb(const wchar_t* file, int max_w, int max_h,
                   const char* contentbuf, int contentbuflen) {
    if (!file || !*file || max_w <= 0 || max_h <= 0 || max_w > 8192 || max_h > 8192) return nullptr;

    CacheKey key{normalized_cache_path(file), max_w, max_h};
    if (auto hit = cache().get(key)) return make_hbitmap(*hit);

    DecodePermit permit;
    if (!permit) return nullptr;

    // Re-check after waiting for a decode slot. Another TC thumbnail worker may
    // have finished and populated the cache while we were queued.
    if (auto hit = cache().get(key)) return make_hbitmap(*hit);

    auto data = decode_path(file, max_w, max_h, contentbuf, contentbuflen);
    if (!data) return nullptr;

    cache().put(std::move(key), data);
    return make_hbitmap(*data);
}

} // namespace

// ------------------------------- WLX API -----------------------------------

extern "C" __declspec(dllexport) HWND __stdcall ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags) {
    (void)ParentWin; (void)FileToLoad; (void)ShowFlags;
    return nullptr;
}

extern "C" __declspec(dllexport) HWND __stdcall ListLoadW(HWND ParentWin, wchar_t* FileToLoad, int ShowFlags) {
    (void)ParentWin; (void)FileToLoad; (void)ShowFlags;
    return nullptr;
}

extern "C" __declspec(dllexport) void __stdcall ListGetDetectString(char* DetectString, int maxlen) {
    static constexpr char s[] =
        "EXT=\"MP4\" | EXT=\"MKV\" | EXT=\"AVI\" | EXT=\"MOV\" | EXT=\"WEBM\" | "
        "EXT=\"WMV\" | EXT=\"M4V\" | EXT=\"MPG\" | EXT=\"MPEG\" | EXT=\"TS\" | "
        "EXT=\"M2TS\" | EXT=\"FLV\" | EXT=\"VOB\" | EXT=\"3GP\" | EXT=\"OGV\" | "
        "EXT=\"ASF\" | EXT=\"RM\" | EXT=\"RMVB\" | EXT=\"F4V\"";
    if (!DetectString || maxlen <= 0) return;
    strncpy_s(DetectString, static_cast<size_t>(maxlen), s, _TRUNCATE);
}

extern "C" __declspec(dllexport) HBITMAP __stdcall ListGetPreviewBitmapW(
    wchar_t* FileToLoad, int width, int height, char* contentbuf, int contentbuflen) noexcept {
    try {
        return make_thumb(FileToLoad, width, height, contentbuf, contentbuflen);
    } catch (...) {
        // Never let a C++ exception escape through the WLX ABI into Total Commander.
        return nullptr;
    }
}

extern "C" __declspec(dllexport) HBITMAP __stdcall ListGetPreviewBitmap(
    char* FileToLoad, int width, int height, char* contentbuf, int contentbuflen) {
    (void)FileToLoad; (void)width; (void)height; (void)contentbuf; (void)contentbuflen;
    return nullptr; // Unicode entry point is used by current 64-bit TC.
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
