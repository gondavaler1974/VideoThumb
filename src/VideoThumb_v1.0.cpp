// VideoThumb.wlx64 v1.0
// Total Commander video thumbnail WLX plugin.
//
// v1.0 goals:
//   * direct libavformat/libavcodec decoding: NO ffmpeg.exe process per thumbnail
//   * in-memory LRU cache (raw BGRA pixels; every TC call gets a fresh HBITMAP)
//   * bounded decode parallelism to avoid saturating SMB + CPU
//   * Windows local-path -> UNC symlink workaround retained as fallback
//   * interrupt callback prevents a dead SMB path from blocking forever
//
// Build against a 64-bit SHARED FFmpeg SDK. FFmpeg 9.x LGPL shared is the
// recommended/test target for this source. See README.md.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <list>
#include <iterator>
#include <memory>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace
{

	HINSTANCE g_instance = nullptr;

	// ------------------------------- settings ---------------------------------

	std::wstring module_dir()
	{
		wchar_t buf[32768]{};
		const DWORD n = GetModuleFileNameW(g_instance, buf, static_cast<DWORD>(std::size(buf)));
		if (!n || n >= std::size(buf))
			return L".";
		std::wstring p(buf, n);
		const auto pos = p.find_last_of(L"\\/");
		if (pos == std::wstring::npos)
			return L".";
		p.resize(pos);
		return p;
	}

	struct Settings
	{
		int cache_mb = 96;			 // RAM cache ceiling
		int cache_entries = 256; // and entry ceiling
		int max_parallel = 2;		 // concurrent libav decode jobs
		int decoder_threads = 2; // threads inside each decoder context
		int seek_ms = 1000;			 // representative frame position
		int timeout_ms = 12000;	 // whole thumbnail decode timeout
		int packet_limit = 5000; // safety limit after seek

		Settings()
		{
			const std::wstring ini = module_dir() + L"\\VideoThumb.ini";

			cache_mb = std::clamp(
					static_cast<int>(GetPrivateProfileIntW(
							L"Performance", L"CacheMB", cache_mb, ini.c_str())),
					0, 1024);

			cache_entries = std::clamp(
					static_cast<int>(GetPrivateProfileIntW(
							L"Performance", L"CacheEntries", cache_entries, ini.c_str())),
					0, 4096);

			max_parallel = std::clamp(
					static_cast<int>(GetPrivateProfileIntW(
							L"Performance", L"MaxParallel", max_parallel, ini.c_str())),
					1, 16);

			decoder_threads = std::clamp(
					static_cast<int>(GetPrivateProfileIntW(
							L"Performance", L"DecoderThreads", decoder_threads, ini.c_str())),
					0, 32);

			seek_ms = std::clamp(
					static_cast<int>(GetPrivateProfileIntW(
							L"Thumbnail", L"SeekMs", seek_ms, ini.c_str())),
					0, 600000);

			timeout_ms = std::clamp(
					static_cast<int>(GetPrivateProfileIntW(
							L"Performance", L"TimeoutMs", timeout_ms, ini.c_str())),
					1000, 120000);

			packet_limit = std::clamp(
					static_cast<int>(GetPrivateProfileIntW(
							L"Performance", L"PacketLimit", packet_limit, ini.c_str())),
					100, 100000);
		}
	};

	const Settings &settings()
	{
		static const Settings s;
		return s;
	}

	// --------------------------- Windows path helpers --------------------------

	std::string utf8_from_wide(const wchar_t *s)
	{
		if (!s || !*s)
			return {};
		const int need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, nullptr, 0, nullptr, nullptr);
		if (need <= 1)
			return {};
		std::string out(static_cast<size_t>(need), '\0');
		if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, out.data(), need, nullptr, nullptr))
			return {};
		out.resize(static_cast<size_t>(need - 1));
		return out;
	}

	std::wstring resolve_final_path(const wchar_t *input)
	{
		if (!input || !*input)
			return {};
		HANDLE h = CreateFileW(input,
													 FILE_READ_ATTRIBUTES,
													 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
													 nullptr,
													 OPEN_EXISTING,
													 FILE_ATTRIBUTE_NORMAL,
													 nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return {};

		std::vector<wchar_t> buf(32768);
		const DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
		CloseHandle(h);
		if (!n || n >= buf.size())
			return {};

		std::wstring p(buf.data(), n);
		// Convert Win32 "extended" syntax back to ordinary syntax FFmpeg accepts.
		constexpr wchar_t unc_prefix[] = L"\\\\?\\UNC\\";
		constexpr wchar_t ext_prefix[] = L"\\\\?\\";
		if (p.rfind(unc_prefix, 0) == 0)
			return L"\\\\" + p.substr(8);
		if (p.rfind(ext_prefix, 0) == 0)
			return p.substr(4);
		return p;
	}

	std::wstring normalized_cache_path(const wchar_t *p)
	{
		std::wstring s = p ? p : L"";
		std::replace(s.begin(), s.end(), L'/', L'\\');
		// Windows paths are case-insensitive in our intended use. Lower-casing avoids
		// duplicate cache entries if TC hands us the same path with different case.
		if (!s.empty())
			CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
		return s;
	}

	// ------------------------------- bitmap ------------------------------------

	struct ThumbData
	{
		int width = 0;
		int height = 0;
		int stride = 0;
		std::vector<uint8_t> bgra;

		size_t bytes() const noexcept { return bgra.size(); }
	};

	HBITMAP make_hbitmap(const ThumbData &t)
	{
		if (t.width <= 0 || t.height <= 0 || t.stride < t.width * 4 || t.bgra.empty())
			return nullptr;

		BITMAPINFO bmi{};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = t.width;
		bmi.bmiHeader.biHeight = -t.height; // top-down DIB, same orientation as swscale output
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		bmi.bmiHeader.biSizeImage = static_cast<DWORD>(static_cast<uint64_t>(t.stride) * t.height);

		void *bits = nullptr;
		HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!hbmp || !bits)
			return nullptr;

		auto *dst = static_cast<uint8_t *>(bits);
		const auto *src = t.bgra.data();
		const size_t row = static_cast<size_t>(t.width) * 4;
		for (int y = 0; y < t.height; ++y)
		{
			memcpy(dst + static_cast<size_t>(y) * t.width * 4,
						 src + static_cast<size_t>(y) * t.stride,
						 row);
		}
		return hbmp; // ownership transfers to Total Commander
	}

	// -------------------------------- cache ------------------------------------

	struct CacheKey
	{
		std::wstring path;
		int max_w = 0;
		int max_h = 0;

		bool operator==(const CacheKey &o) const noexcept
		{
			return max_w == o.max_w && max_h == o.max_h && path == o.path;
		}
	};

	struct CacheKeyHash
	{
		size_t operator()(const CacheKey &k) const noexcept
		{
			size_t h = std::hash<std::wstring>{}(k.path);
			h ^= static_cast<size_t>(k.max_w) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
			h ^= static_cast<size_t>(k.max_h) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
			return h;
		}
	};

	class ThumbCache
	{
		struct Entry
		{
			std::shared_ptr<const ThumbData> data;
			std::list<CacheKey>::iterator lru_it;
			size_t bytes = 0;
		};

		std::mutex m_;
		std::list<CacheKey> lru_; // front = most recently used
		std::unordered_map<CacheKey, Entry, CacheKeyHash> map_;
		size_t total_bytes_ = 0;

	public:
		std::shared_ptr<const ThumbData> get(const CacheKey &key)
		{
			if (settings().cache_mb == 0 || settings().cache_entries == 0)
				return {};
			std::lock_guard lock(m_);
			auto it = map_.find(key);
			if (it == map_.end())
				return {};
			lru_.splice(lru_.begin(), lru_, it->second.lru_it);
			return it->second.data;
		}

		void put(CacheKey key, std::shared_ptr<const ThumbData> data)
		{
			if (!data || settings().cache_mb == 0 || settings().cache_entries == 0)
				return;
			const size_t limit_bytes = static_cast<size_t>(settings().cache_mb) * 1024ULL * 1024ULL;
			const size_t n = data->bytes();
			if (n == 0 || n > limit_bytes)
				return;

			std::lock_guard lock(m_);
			if (auto old = map_.find(key); old != map_.end())
			{
				total_bytes_ -= old->second.bytes;
				lru_.erase(old->second.lru_it);
				map_.erase(old);
			}

			lru_.push_front(key);
			Entry e{std::move(data), lru_.begin(), n};
			total_bytes_ += n;
			map_.emplace(std::move(key), std::move(e));

			while (!lru_.empty() &&
						 (map_.size() > static_cast<size_t>(settings().cache_entries) || total_bytes_ > limit_bytes))
			{
				const CacheKey victim = lru_.back();
				auto it = map_.find(victim);
				if (it != map_.end())
				{
					total_bytes_ -= it->second.bytes;
					map_.erase(it);
				}
				lru_.pop_back();
			}
		}
	};

	ThumbCache &cache()
	{
		static ThumbCache c;
		return c;
	}

	// -------------------------- bounded parallelism ----------------------------

	HANDLE decode_semaphore()
	{
		static HANDLE h = CreateSemaphoreW(nullptr, settings().max_parallel, settings().max_parallel, nullptr);
		return h;
	}

	class DecodePermit
	{
		HANDLE h_ = nullptr;
		bool held_ = false;

	public:
		DecodePermit() : h_(decode_semaphore())
		{
			if (h_)
				held_ = (WaitForSingleObject(h_, static_cast<DWORD>(settings().timeout_ms)) == WAIT_OBJECT_0);
		}
		~DecodePermit()
		{
			if (held_)
				ReleaseSemaphore(h_, 1, nullptr);
		}
		explicit operator bool() const noexcept { return held_; }
	};

	// ------------------------------- libav -------------------------------------

	// v1.0 intentionally loads FFmpeg DLLs itself from the plugin directory.
	// This avoids a common Windows plugin problem: dependent DLLs next to a WLX
	// are not guaranteed to be in the host EXE's normal DLL search path.
	struct FfmpegApi
	{
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

		static std::wstring find_dll(const wchar_t *pattern)
		{
			const std::wstring dir = module_dir();
			WIN32_FIND_DATAW fd{};
			HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE)
				return {};
			std::wstring result = dir + L"\\" + fd.cFileName;
			FindClose(h);
			return result;
		}

		static HMODULE load_pattern(const wchar_t *pattern, bool optional = false)
		{
			const std::wstring path = find_dll(pattern);
			if (path.empty())
				return optional ? reinterpret_cast<HMODULE>(1) : nullptr;
			HMODULE h = LoadLibraryExW(path.c_str(), nullptr,
																 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
																		 LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
			return h;
		}

		template <class T>
		static bool load_proc(HMODULE h, const char *name, T &out)
		{
			if (!h || h == reinterpret_cast<HMODULE>(1))
				return false;
			out = reinterpret_cast<T>(GetProcAddress(h, name));
			return out != nullptr;
		}

		FfmpegApi()
		{
			// Load dependency order first. swresample is not directly called, but
			// some avcodec builds depend on it.
			h_avutil = load_pattern(L"avutil-*.dll");
			if (!h_avutil)
				return;
			h_swresample = load_pattern(L"swresample-*.dll", true);
			h_avcodec = load_pattern(L"avcodec-*.dll");
			if (!h_avcodec)
				return;
			h_avformat = load_pattern(L"avformat-*.dll");
			if (!h_avformat)
				return;
			h_swscale = load_pattern(L"swscale-*.dll");
			if (!h_swscale)
				return;

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
			if (ready)
			{
				p_av_log_set_level(AV_LOG_QUIET);
				p_avformat_network_init();
			}
		}
	};

	FfmpegApi &ff()
	{
		static FfmpegApi api;
		return api;
	}

	struct InterruptState
	{
		ULONGLONG deadline = 0;
	};

	int ff_interrupt_cb(void *opaque)
	{
		const auto *s = static_cast<const InterruptState *>(opaque);
		return s && GetTickCount64() >= s->deadline;
	}

	bool receive_one_frame(AVCodecContext *dec, AVFrame *frame)
	{
		for (;;)
		{
			const int r = ff().p_avcodec_receive_frame(dec, frame);
			if (r == 0)
				return true;
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
				return false;
			return false;
		}
	}

	bool decode_frame_from_current(AVFormatContext *fmt,
																 AVCodecContext *dec,
																 int video_index,
																 AVPacket *pkt,
																 AVFrame *frame,
																 int packet_limit)
	{
		int packets = 0;
		while (packets++ < packet_limit && ff().p_av_read_frame(fmt, pkt) >= 0)
		{
			if (pkt->stream_index != video_index)
			{
				ff().p_av_packet_unref(pkt);
				continue;
			}

			int s = ff().p_avcodec_send_packet(dec, pkt);
			if (s == AVERROR(EAGAIN))
			{
				if (receive_one_frame(dec, frame))
				{
					ff().p_av_packet_unref(pkt);
					return true;
				}
				// Retry the same packet after draining; EAGAIN means it was not consumed.
				s = ff().p_avcodec_send_packet(dec, pkt);
			}
			ff().p_av_packet_unref(pkt);
			if (s < 0)
				continue;

			for (;;)
			{
				const int r = ff().p_avcodec_receive_frame(dec, frame);
				if (r == 0)
					return true;
				if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
					break;
				break;
			}
		}

		// Flush delayed frames at EOF.
		ff().p_avcodec_send_packet(dec, nullptr);
		return receive_one_frame(dec, frame);
	}

	std::shared_ptr<ThumbData> scale_frame_to_thumb(const AVFrame *frame, int max_w, int max_h)
	{
		if (!frame || frame->width <= 0 || frame->height <= 0 || frame->format < 0 || max_w <= 0 || max_h <= 0)
			return {};

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

		SwsContext *sws = ff().p_sws_getContext(frame->width,
																						frame->height,
																						static_cast<AVPixelFormat>(frame->format),
																						out_w,
																						out_h,
																						AV_PIX_FMT_BGRA,
																						SWS_FAST_BILINEAR,
																						nullptr,
																						nullptr,
																						nullptr);
		if (!sws)
			return {};

		uint8_t *dst_data[4] = {out->bgra.data(), nullptr, nullptr, nullptr};
		int dst_linesize[4] = {out->stride, 0, 0, 0};
		const int rows = ff().p_sws_scale(sws,
																			frame->data,
																			frame->linesize,
																			0,
																			frame->height,
																			dst_data,
																			dst_linesize);
		ff().p_sws_freeContext(sws);
		if (rows <= 0)
			return {};
		return out;
	}

	std::shared_ptr<ThumbData> decode_with_libav_utf8(const std::string &input, int max_w, int max_h)
	{
		if (input.empty())
			return {};
		if (!ff().ready)
			return {};

		InterruptState interrupt{GetTickCount64() + static_cast<ULONGLONG>(settings().timeout_ms)};
		AVFormatContext *fmt = ff().p_avformat_alloc_context();
		AVCodecContext *dec = nullptr;
		AVPacket *pkt = nullptr;
		AVFrame *frame = nullptr;
		const AVCodec *decoder = nullptr;
		int vi = -1;
		std::shared_ptr<ThumbData> result;

		if (!fmt)
			return {};
		fmt->interrupt_callback.callback = ff_interrupt_cb;
		fmt->interrupt_callback.opaque = &interrupt;

		if (ff().p_avformat_open_input(&fmt, input.c_str(), nullptr, nullptr) < 0 || !fmt)
			goto done;
		if (ff().p_avformat_find_stream_info(fmt, nullptr) < 0)
			goto done;

		vi = ff().p_av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
		if (vi < 0 || !decoder || vi >= static_cast<int>(fmt->nb_streams))
			goto done;

		dec = ff().p_avcodec_alloc_context3(decoder);
		if (!dec)
			goto done;
		if (ff().p_avcodec_parameters_to_context(dec, fmt->streams[vi]->codecpar) < 0)
			goto done;

		if (settings().decoder_threads > 0)
			dec->thread_count = settings().decoder_threads;
		if (ff().p_avcodec_open2(dec, decoder, nullptr) < 0)
			goto done;

		pkt = ff().p_av_packet_alloc();
		frame = ff().p_av_frame_alloc();
		if (!pkt || !frame)
			goto done;

		// Seek using AV_TIME_BASE units by passing stream_index=-1.
		// If the clip is shorter than SeekMs, choose roughly one third of its duration.
		{
			int64_t target_us = static_cast<int64_t>(settings().seek_ms) * 1000LL;
			if (fmt->duration > 0 && target_us >= fmt->duration)
				target_us = fmt->duration / 3;
			if (target_us > 0 && ff().p_av_seek_frame(fmt, -1, target_us, AVSEEK_FLAG_BACKWARD) >= 0)
			{
				ff().p_avcodec_flush_buffers(dec);
			}
		}

		if (!decode_frame_from_current(fmt, dec, vi, pkt, frame, settings().packet_limit))
		{
			// Very short or oddly indexed clips: retry from the beginning without
			// reopening the file.
			ff().p_av_frame_unref(frame);
			ff().p_avcodec_flush_buffers(dec);
			if (ff().p_av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD) >= 0)
			{
				if (!decode_frame_from_current(fmt, dec, vi, pkt, frame, settings().packet_limit))
					goto done;
			}
			else
			{
				goto done;
			}
		}

		result = scale_frame_to_thumb(frame, max_w, max_h);

	done:
		if (frame)
			ff().p_av_frame_free(&frame);
		if (pkt)
			ff().p_av_packet_free(&pkt);
		if (dec)
			ff().p_avcodec_free_context(&dec);
		if (fmt)
			ff().p_avformat_close_input(&fmt);
		return result;
	}

	std::shared_ptr<ThumbData> decode_path(const wchar_t *file, int max_w, int max_h)
	{
		const std::string original = utf8_from_wide(file);
		if (!original.empty())
		{
			if (auto t = decode_with_libav_utf8(original, max_w, max_h))
				return t;
		}

		// The crucial VideoThumb workaround: if the local directory is a symlink whose
		// target is an UNC share, resolve the file handle to the real UNC path and
		// retry libav on that final path.
		const std::wstring final_path = resolve_final_path(file);
		if (!final_path.empty() && final_path != file)
		{
			const std::string u8 = utf8_from_wide(final_path.c_str());
			if (!u8.empty())
				return decode_with_libav_utf8(u8, max_w, max_h);
		}
		return {};
	}

	HBITMAP make_thumb(const wchar_t *file, int max_w, int max_h)
	{
		if (!file || !*file || max_w <= 0 || max_h <= 0)
			return nullptr;

		CacheKey key{normalized_cache_path(file), max_w, max_h};
		if (auto hit = cache().get(key))
			return make_hbitmap(*hit);

		DecodePermit permit;
		if (!permit)
			return nullptr;

		// Re-check after waiting for a decode slot. Another TC thumbnail worker may
		// have finished and populated the cache while we were queued.
		if (auto hit = cache().get(key))
			return make_hbitmap(*hit);

		auto data = decode_path(file, max_w, max_h);
		if (!data)
			return nullptr;

		cache().put(std::move(key), data);
		return make_hbitmap(*data);
	}

} // namespace

// ------------------------------- WLX API -----------------------------------

extern "C" __declspec(dllexport) HWND __stdcall ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags)
{
	(void)ParentWin;
	(void)FileToLoad;
	(void)ShowFlags;
	return nullptr;
}

extern "C" __declspec(dllexport) HWND __stdcall ListLoadW(HWND ParentWin, wchar_t *FileToLoad, int ShowFlags)
{
	(void)ParentWin;
	(void)FileToLoad;
	(void)ShowFlags;
	return nullptr;
}

extern "C" __declspec(dllexport) void __stdcall ListGetDetectString(char *DetectString, int maxlen)
{
	static constexpr char s[] =
			"EXT=\"MP4\" | EXT=\"MKV\" | EXT=\"AVI\" | EXT=\"MOV\" | EXT=\"WEBM\" | "
			"EXT=\"WMV\" | EXT=\"M4V\" | EXT=\"MPG\" | EXT=\"MPEG\" | EXT=\"TS\" | "
			"EXT=\"M2TS\" | EXT=\"FLV\" | EXT=\"VOB\" | EXT=\"3GP\" | EXT=\"OGV\" | "
			"EXT=\"ASF\" | EXT=\"RM\" | EXT=\"RMVB\" | EXT=\"F4V\"";
	if (!DetectString || maxlen <= 0)
		return;
	strncpy_s(DetectString, static_cast<size_t>(maxlen), s, _TRUNCATE);
}

extern "C" __declspec(dllexport) HBITMAP __stdcall ListGetPreviewBitmapW(
		wchar_t *FileToLoad, int width, int height, char *contentbuf, int contentbuflen)
{
	(void)contentbuf;
	(void)contentbuflen;
	return make_thumb(FileToLoad, width, height);
}

extern "C" __declspec(dllexport) HBITMAP __stdcall ListGetPreviewBitmap(
		char *FileToLoad, int width, int height, char *contentbuf, int contentbuflen)
{
	(void)FileToLoad;
	(void)width;
	(void)height;
	(void)contentbuf;
	(void)contentbuflen;
	return nullptr; // Unicode entry point is used by current 64-bit TC.
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_instance = hinst;
		DisableThreadLibraryCalls(hinst);
	}
	return TRUE;
}
