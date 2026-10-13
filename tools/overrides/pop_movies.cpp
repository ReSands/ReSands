// Movie playback for Prince of Persia: The Sands of Time (SLUS_207.43) from the Bink files of
// another owned version (Xbox, PC/GOG or GameCube), decoded with FFmpeg. The PS2's own movies need
// the IPU, which the runtime does not emulate; without Bink files the old behaviour stays
// (the movie is reported missing and skipped).
//
// The game plays movies through VIDEO_MainCall (0x4378C0), which calls small wrappers around
// its FullMovie object. Those wrappers are replaced here; everything above them (button skip,
// looping, sound pause, centring, drawing with PS2_DisplayVideo) is the game's own code.
// PS2_DisplayVideo uploads the frame from a guest buffer of 16x16-pixel RGBA blocks.
//
// Environment: POP_MOVIES=0 turns this off. POP_MOVIE_DIRS lists folders to search, separated
// by ';' (default: the Xbox, GOG and GameCube folders relative to game/ps2/disc, in that order:
// the Xbox files have the highest bitrate and 5.1 sound). Debug: POP_MOVIE_FORCE=<file> plays
// that file for every movie; POP_MOVIE_TEST=1 draws a coordinate test pattern.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include "raylib.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    // Wrappers around FullMovie (prototype names; retail addresses).
    constexpr uint32_t kMovieLoadMovie = 0x0019F440u;      // bool (FullMovie*, char *name, uint)
    constexpr uint32_t kMovieUnloadMovie = 0x0019F4C0u;    // (FullMovie*)
    constexpr uint32_t kMovieDoFrame = 0x0019F500u;        // (FullMovie*)
    constexpr uint32_t kMovieGotoNextFrame = 0x0019F540u;  // (FullMovie*)
    constexpr uint32_t kMovieGotoFrame = 0x0019F580u;      // (FullMovie*, uint frame), 1-based
    constexpr uint32_t kMovieIsMovieWaiting = 0x0019F5C0u; // bool (FullMovie*)
    constexpr uint32_t kMovieGetNbFrames = 0x0019F600u;    // uint (FullMovie*)
    constexpr uint32_t kMovieGetWidth = 0x0019F640u;       // uint (FullMovie*)
    constexpr uint32_t kMovieGetHeight = 0x0019F680u;      // uint (FullMovie*)
    constexpr uint32_t kMovieGetLastFrame = 0x0019F6C0u;   // uint (FullMovie*)
    constexpr uint32_t kMovieGetVideoOutput = 0x0019F700u; // void *(FullMovie*)

    constexpr uint32_t kSoundTrack = 0x006A5008u;   // MovieSetSoundTrack's global (language)
    constexpr uint32_t kScreenWidth = 0x0066CED0u;  // set by VIDEO_Load from the draw env
    constexpr uint32_t kScreenHeight = 0x0066CED4u;
    constexpr uint32_t kAllocator = 0x006803B0u;    // type-4 allocator: alloc vtable+0xc, free +0x10

    constexpr int kSampleRate = 48000;
    constexpr size_t kQueuedFrames = 30;              // decoded video frames kept ahead
    constexpr size_t kAudioRing = kSampleRate * 2 * 4; // 4 s of stereo s16

    uint32_t guestU32(uint8_t *rdram, uint32_t addr)
    {
        return *reinterpret_cast<const uint32_t *>(getConstMemPtr(rdram, addr));
    }

    std::string guestString(uint8_t *rdram, uint32_t addr)
    {
        std::string s;
        for (uint32_t i = 0; addr && i < 256u; ++i)
        {
            const char c = static_cast<char>(*getConstMemPtr(rdram, addr + i));
            if (!c)
                break;
            s += c;
        }
        return s;
    }

    // Calls a guest function with two arguments and returns v0; the caller's registers survive.
    uint32_t callGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t fn, uint32_t a0, uint32_t a1)
    {
        const R5900Context saved = *ctx;
        SET_GPR_U32(ctx, 4, a0);
        SET_GPR_U32(ctx, 5, a1);
        SET_GPR_U32(ctx, 31, 0x00100008u);
        ctx->pc = fn;
        runtime->lookupFunction(fn)(rdram, ctx, runtime);
        const uint32_t v0 = getRegU32(ctx, 2);
        *ctx = saved;
        return v0;
    }

    struct Frame
    {
        uint32_t number = 0; // 1-based
        std::vector<uint8_t> rgba;
    };

    // Decodes one Bink file on its own thread: RGBA frames into a queue, stereo s16 into a ring
    // that the movie audio stream drains.
    class BinkPlayer
    {
    public:
        ~BinkPlayer() { close(); }

        bool open(const std::string &path, int track, int outWidth, int pictureHeight)
        {
            if (avformat_open_input(&m_fmt, path.c_str(), nullptr, nullptr) < 0)
                return false;
            if (avformat_find_stream_info(m_fmt, nullptr) < 0)
                return false;
            m_video = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (m_video < 0)
                return false;
            selectAudio(track);
            for (unsigned i = 0; i < m_fmt->nb_streams; ++i)
            {
                const bool used = static_cast<int>(i) == m_video ||
                                  std::any_of(m_tracks.begin(), m_tracks.end(), [i](const AudioTrack &t) { return t.stream == static_cast<int>(i); });
                if (!used)
                    m_fmt->streams[i]->discard = AVDISCARD_ALL;
            }

            if (!openCodec(m_video, m_videoCodec))
                return false;
            for (AudioTrack &t : m_tracks)
            {
                if (!openCodec(t.stream, t.codec))
                {
                    m_tracks.clear();
                    break;
                }
                AVChannelLayout layout;
                av_channel_layout_default(&layout, t.channels);
                if (swr_alloc_set_opts2(&t.swr, &layout, AV_SAMPLE_FMT_FLT, kSampleRate,
                                        &t.codec->ch_layout, t.codec->sample_fmt, t.codec->sample_rate, 0, nullptr) < 0 ||
                    swr_init(t.swr) < 0)
                {
                    m_tracks.clear();
                    break;
                }
            }

            const AVStream *vs = m_fmt->streams[m_video];
            AVRational rate = vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate;
            m_fps = rate.num ? av_q2d(rate) : 29.97;
            m_frames = static_cast<uint32_t>(vs->nb_frames);
            if (!m_frames && m_fmt->duration > 0)
                m_frames = static_cast<uint32_t>(m_fmt->duration * m_fps / AV_TIME_BASE);
            m_outWidth = outWidth;
            m_pictureHeight = pictureHeight;

            m_thread = std::thread([this] { decodeLoop(); });
            return m_frames > 0;
        }

        void close()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_cv.notify_all();
            if (m_thread.joinable())
                m_thread.join();
            sws_freeContext(m_sws);
            m_sws = nullptr;
            for (AudioTrack &t : m_tracks)
            {
                swr_free(&t.swr);
                avcodec_free_context(&t.codec);
            }
            m_tracks.clear();
            avcodec_free_context(&m_videoCodec);
            avformat_close_input(&m_fmt);
        }

        uint32_t frameCount() const { return m_frames; }
        double fps() const { return m_fps; }

        // Waits (briefly) for frame `number` and returns it, dropping older ones.
        bool takeFrame(uint32_t number, Frame &out)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            for (;;)
            {
                while (!m_queue.empty() && m_queue.front().number < number)
                    m_queue.pop_front();
                m_cv.notify_all();
                if (!m_queue.empty())
                {
                    out = std::move(m_queue.front());
                    m_queue.pop_front();
                    m_cv.notify_all();
                    return true;
                }
                if (m_eof || m_cv.wait_until(lock, deadline) == std::cv_status::timeout)
                    return false;
            }
        }

        // Called on the audio thread.
        void readAudio(int16_t *out, size_t samples)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const size_t n = std::min(samples, m_ringQueued);
            for (size_t i = 0; i < n; ++i)
                out[i] = m_ring[(m_ringRead + i) % kAudioRing];
            m_ringRead = (m_ringRead + n) % kAudioRing;
            m_ringQueued -= n;
            std::fill(out + n, out + samples, int16_t{0});
            m_cv.notify_all();
        }

        std::string audioDescription() const
        {
            if (m_tracks.empty())
                return "no audio";
            return m_tracks.size() == 1 ? "stereo track " + std::to_string(m_tracks[0].stream)
                                        : "5.1 downmix of tracks from " + std::to_string(m_tracks[0].stream);
        }

    private:
        // One audio stream feeding the stereo mix: its channels go to left/right with a gain
        // (mono to both sides).
        struct AudioTrack
        {
            int stream = -1;
            int channels = 2;
            float gain = 1.0f;
            AVCodecContext *codec = nullptr;
            SwrContext *swr = nullptr;
            std::deque<float> fifo; // interleaved, `channels` per sample
        };

        // PC and GameCube files carry one stereo track per language. Xbox files carry 5.1 as
        // four tracks per language: front L/R (stereo), centre (mono, the dialogue), LFE (mono)
        // and surround L/R (stereo). Those are mixed down like the usual 5.1 -> stereo downmix:
        // centre and surrounds at -3 dB, LFE left out.
        void selectAudio(int track)
        {
            std::vector<int> audio;
            for (unsigned i = 0; i < m_fmt->nb_streams; ++i)
                if (m_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                    audio.push_back(static_cast<int>(i));
            auto channels = [this](int stream) { return m_fmt->streams[stream]->codecpar->ch_layout.nb_channels; };
            bool surround = audio.size() >= 4 && audio.size() % 4 == 0;
            for (size_t i = 0; surround && i < audio.size(); i += 4)
                surround = channels(audio[i]) == 2 && channels(audio[i + 1]) == 1 &&
                           channels(audio[i + 2]) == 1 && channels(audio[i + 3]) == 2;
            const int groups = static_cast<int>(surround ? audio.size() / 4 : audio.size());
            if (groups == 0)
                return;
            const int group = (track >= 0 && track < groups) ? track : 0;
            constexpr float kMinus3dB = 0.7071f;
            if (surround)
            {
                const size_t base = static_cast<size_t>(group) * 4;
                m_tracks.push_back({audio[base], 2, 1.0f});
                m_tracks.push_back({audio[base + 1], 1, kMinus3dB});
                m_tracks.push_back({audio[base + 3], 2, kMinus3dB});
                m_mixScale = 1.0f / (1.0f + 2.0f * kMinus3dB);
            }
            else
            {
                m_tracks.push_back({audio[group], channels(audio[group]), 1.0f});
                m_mixScale = 1.0f;
            }
        }

        bool openCodec(int stream, AVCodecContext *&ctx)
        {
            const AVCodecParameters *par = m_fmt->streams[stream]->codecpar;
            const AVCodec *codec = avcodec_find_decoder(par->codec_id);
            if (!codec)
                return false;
            ctx = avcodec_alloc_context3(codec);
            return ctx && avcodec_parameters_to_context(ctx, par) >= 0 && avcodec_open2(ctx, codec, nullptr) >= 0;
        }

        void decodeLoop()
        {
            AVPacket *pkt = av_packet_alloc();
            AVFrame *frame = av_frame_alloc();
            uint32_t number = 0;
            bool flushed = false;
            while (!flushed)
            {
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this] {
                        return m_stop || (m_queue.size() < kQueuedFrames && m_ringQueued < kAudioRing - kSampleRate * 2);
                    });
                    if (m_stop)
                        break;
                }
                const bool eof = av_read_frame(m_fmt, pkt) < 0;
                if (eof)
                {
                    avcodec_send_packet(m_videoCodec, nullptr);
                    flushed = true;
                }
                else if (AudioTrack *t = trackFor(pkt->stream_index))
                {
                    if (avcodec_send_packet(t->codec, pkt) >= 0)
                        while (avcodec_receive_frame(t->codec, frame) >= 0)
                            pushAudio(*t, frame);
                    av_packet_unref(pkt);
                    continue;
                }
                else if (pkt->stream_index == m_video)
                {
                    avcodec_send_packet(m_videoCodec, pkt);
                }
                av_packet_unref(pkt);
                while (avcodec_receive_frame(m_videoCodec, frame) >= 0)
                    pushVideo(frame, ++number);
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_eof = true;
            }
            m_cv.notify_all();
            av_frame_free(&frame);
            av_packet_free(&pkt);
        }

        void pushVideo(const AVFrame *frame, uint32_t number)
        {
            m_sws = sws_getCachedContext(m_sws, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                         m_outWidth, m_pictureHeight, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                         nullptr, nullptr, nullptr);
            if (!m_sws)
                return;
            Frame out;
            out.number = number;
            out.rgba.resize(static_cast<size_t>(m_outWidth) * m_pictureHeight * 4);
            uint8_t *dst[1] = {out.rgba.data()};
            const int stride[1] = {m_outWidth * 4};
            sws_scale(m_sws, frame->data, frame->linesize, 0, frame->height, dst, stride);
            for (size_t i = 3; i < out.rgba.size(); i += 4)
                out.rgba[i] = 0x80; // GS alpha 1.0
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(out));
            m_cv.notify_all();
        }

        AudioTrack *trackFor(int stream)
        {
            for (AudioTrack &t : m_tracks)
                if (t.stream == stream)
                    return &t;
            return nullptr;
        }

        // Converts one decoded frame of a track to 48 kHz float, then mixes whatever all
        // tracks have in common into the stereo ring.
        void pushAudio(AudioTrack &track, const AVFrame *frame)
        {
            const int maxOut = swr_get_out_samples(track.swr, frame->nb_samples);
            if (maxOut <= 0)
                return;
            std::vector<float> pcm(static_cast<size_t>(maxOut) * track.channels);
            uint8_t *out[1] = {reinterpret_cast<uint8_t *>(pcm.data())};
            const int got = swr_convert(track.swr, out, maxOut, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
            if (got <= 0)
                return;
            track.fifo.insert(track.fifo.end(), pcm.begin(), pcm.begin() + static_cast<size_t>(got) * track.channels);

            size_t samples = SIZE_MAX;
            for (const AudioTrack &t : m_tracks)
                samples = std::min(samples, t.fifo.size() / static_cast<size_t>(t.channels));
            if (samples == 0 || samples == SIZE_MAX)
                return;
            std::vector<int16_t> mixed(samples * 2);
            for (size_t i = 0; i < samples; ++i)
            {
                float left = 0.0f, right = 0.0f;
                for (const AudioTrack &t : m_tracks)
                {
                    const float l = t.fifo[i * t.channels];
                    const float r = t.channels == 2 ? t.fifo[i * t.channels + 1] : l;
                    left += l * t.gain;
                    right += r * t.gain;
                }
                mixed[i * 2] = static_cast<int16_t>(std::clamp(left * m_mixScale, -1.0f, 1.0f) * 32767.0f);
                mixed[i * 2 + 1] = static_cast<int16_t>(std::clamp(right * m_mixScale, -1.0f, 1.0f) * 32767.0f);
            }
            for (AudioTrack &t : m_tracks)
                t.fifo.erase(t.fifo.begin(), t.fifo.begin() + static_cast<std::ptrdiff_t>(samples * t.channels));

            std::lock_guard<std::mutex> lock(m_mutex);
            const size_t n = std::min(mixed.size(), kAudioRing - m_ringQueued);
            for (size_t i = 0; i < n; ++i)
                m_ring[(m_ringRead + m_ringQueued + i) % kAudioRing] = mixed[i];
            m_ringQueued += n;
        }

        AVFormatContext *m_fmt = nullptr;
        AVCodecContext *m_videoCodec = nullptr;
        SwsContext *m_sws = nullptr;
        std::vector<AudioTrack> m_tracks;
        float m_mixScale = 1.0f;
        int m_video = -1;
        int m_outWidth = 0, m_pictureHeight = 0;
        double m_fps = 29.97;
        uint32_t m_frames = 0;

        std::thread m_thread;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_stop = false, m_eof = false;
        std::deque<Frame> m_queue;
        std::vector<int16_t> m_ring = std::vector<int16_t>(kAudioRing);
        size_t m_ringRead = 0, m_ringQueued = 0;
    };

    // One movie at a time (the game has a single FullMovie).
    struct Movie
    {
        uint32_t object = 0;      // the game's FullMovie
        std::string path;
        int track = 0;
        std::unique_ptr<BinkPlayer> player;
        uint32_t allocation = 0;  // guest block from the game's allocator
        uint32_t output = 0;      // 64-byte aligned frame buffer in it
        uint32_t width = 0, height = 0, pictureHeight = 0; // padded to 16
        uint32_t current = 1;     // frame the game is on (1-based)
        uint32_t shown = 0;       // frame last copied to the output buffer
        bool clockRunning = false;
        std::chrono::steady_clock::time_point start;
    };

    std::mutex g_audioMutex;
    BinkPlayer *g_audioSource = nullptr; // guarded by g_audioMutex
    std::atomic<bool> g_audioStarted{false};
    AudioStream g_stream{};
    bool g_streamReady = false;

    void movieAudioCallback(void *buffer, unsigned int frames)
    {
        auto *out = static_cast<int16_t *>(buffer);
        std::lock_guard<std::mutex> lock(g_audioMutex);
        if (g_audioSource && g_audioStarted)
            g_audioSource->readAudio(out, static_cast<size_t>(frames) * 2);
        else
            std::fill_n(out, static_cast<size_t>(frames) * 2, int16_t{0});
    }

    std::unique_ptr<Movie> g_movie;
    std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction> g_originals;

    bool isOurs(R5900Context *ctx)
    {
        return g_movie && getRegU32(ctx, 4) == g_movie->object;
    }

    void callOriginal(uint32_t addr, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_originals[addr](rdram, ctx, runtime);
    }

    // Searches the folders in order for the movie's file name, or the same name with the
    // language digit before the extension (Xbox: POP_IN0.INT ... POP_IN4.INT).
    std::string findBink(const std::string &guestName, int language)
    {
        std::string base = guestName.substr(guestName.find_last_of("\\/") + 1);
        std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return std::tolower(c); });
        const size_t dot = base.rfind('.');
        const std::string localized = dot == std::string::npos
            ? base + std::to_string(language)
            : base.substr(0, dot) + std::to_string(language) + base.substr(dot);
        const char *env = std::getenv("POP_MOVIE_DIRS");
        const std::string dirs = env && *env ? env : "../../xbox/disc/VIDEO;../../gog/install/Video;../../gc/disc";
        size_t pos = 0;
        while (pos <= dirs.size())
        {
            const size_t end = std::min(dirs.find(';', pos), dirs.size());
            const std::filesystem::path dir = dirs.substr(pos, end - pos);
            pos = end + 1;
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                std::string name = entry.path().filename().string();
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
                if (name == base || name == localized)
                    return entry.path().string();
            }
        }
        return {};
    }

    void releaseMovie(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_movie)
            return;
        {
            std::lock_guard<std::mutex> lock(g_audioMutex);
            g_audioSource = nullptr;
            g_audioStarted = false;
        }
        g_movie->player.reset();
        if (g_movie->allocation)
        {
            const uint32_t allocator = guestU32(rdram, kAllocator);
            callGuest(rdram, ctx, runtime, guestU32(rdram, guestU32(rdram, allocator) + 0x10u), allocator, g_movie->allocation);
        }
        g_movie.reset();
    }

    bool startPlayer(Movie &movie)
    {
        movie.player = std::make_unique<BinkPlayer>();
        if (!movie.player->open(movie.path, movie.track, static_cast<int>(movie.width), static_cast<int>(movie.pictureHeight)))
        {
            movie.player.reset();
            return false;
        }
        movie.current = 1;
        movie.shown = 0;
        movie.clockRunning = false;
        std::lock_guard<std::mutex> lock(g_audioMutex);
        g_audioSource = movie.player.get();
        g_audioStarted = false;
        return true;
    }

    void movieLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t object = getRegU32(ctx, 4);
        const std::string name = guestString(rdram, getRegU32(ctx, 5));
        const int language = static_cast<int>(guestU32(rdram, kSoundTrack));
        // Debug: POP_MOVIE_FORCE=<file> plays that file in place of every movie.
        const char *force = std::getenv("POP_MOVIE_FORCE");
        const std::string path = force && *force ? std::string(force) : findBink(name, language >= 0 && language < 5 ? language : 0);
        if (path.empty())
        {
            std::cerr << "[pop:movie] no Bink file for " << name << ", skipping" << std::endl;
            callOriginal(kMovieLoadMovie, rdram, ctx, runtime);
            return;
        }
        releaseMovie(rdram, ctx, runtime);

        auto movie = std::make_unique<Movie>();
        movie->object = object;
        movie->path = path;
        movie->track = language;

        // The Bink files were made from the PS2 masters: 640 wide for a 640x448 4:3 screen
        // (full-screen movies are 640x448, letterboxed cutscenes 640x346), so their pixels have
        // the PS2's shape. Scale them to this screen's width and height in the same proportion.
        const uint32_t screenW = std::max(guestU32(rdram, kScreenWidth), 16u);
        const uint32_t screenH = std::max(guestU32(rdram, kScreenHeight), 16u);
        AVFormatContext *probe = nullptr;
        int srcW = 640, srcH = 346;
        if (avformat_open_input(&probe, path.c_str(), nullptr, nullptr) >= 0)
        {
            if (avformat_find_stream_info(probe, nullptr) >= 0)
            {
                const int v = av_find_best_stream(probe, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                if (v >= 0)
                {
                    srcW = probe->streams[v]->codecpar->width;
                    srcH = probe->streams[v]->codecpar->height;
                }
            }
            avformat_close_input(&probe);
        }
        movie->width = std::min(screenW, 640u) & ~15u;
        const double rows = static_cast<double>(srcH) * movie->width / srcW * screenH / (screenW * 0.7);
        movie->pictureHeight = std::min(static_cast<uint32_t>(std::lround(rows)), screenH) & ~1u;
        movie->height = (movie->pictureHeight + 15u) & ~15u;

        if (!startPlayer(*movie))
        {
            std::cerr << "[pop:movie] could not decode " << path << ", skipping" << std::endl;
            callOriginal(kMovieLoadMovie, rdram, ctx, runtime);
            return;
        }

        const uint32_t bytes = movie->width * movie->height * 4u;
        const uint32_t allocator = guestU32(rdram, kAllocator);
        movie->allocation = allocator
            ? callGuest(rdram, ctx, runtime, guestU32(rdram, guestU32(rdram, allocator) + 0xCu), allocator, bytes + 64u)
            : 0u;
        if (!movie->allocation)
        {
            std::cerr << "[pop:movie] no guest memory for " << bytes << " bytes, skipping " << name << std::endl;
            movie->player.reset();
            {
                std::lock_guard<std::mutex> lock(g_audioMutex);
                g_audioSource = nullptr;
            }
            callOriginal(kMovieLoadMovie, rdram, ctx, runtime);
            return;
        }
        movie->output = (movie->allocation + 63u) & ~63u;
        std::memset(getMemPtr(rdram, movie->output), 0, bytes);

        if (!g_streamReady && IsAudioDeviceReady())
        {
            g_stream = LoadAudioStream(kSampleRate, 16, 2);
            g_streamReady = g_stream.buffer != nullptr;
            if (g_streamReady)
            {
                SetAudioStreamCallback(g_stream, movieAudioCallback);
                PlayAudioStream(g_stream);
            }
        }
        std::cerr << "[pop:movie] " << name << " -> " << path << ": " << movie->player->frameCount() << " frames at "
                  << movie->player->fps() << " fps, " << movie->width << "x" << movie->pictureHeight << " in "
                  << movie->width << "x" << movie->height << ", language " << movie->track << ", " << movie->player->audioDescription() << std::endl;
        g_movie = std::move(movie);
        SET_GPR_U32(ctx, 2, 1u);
    }

    void movieUnload(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!isOurs(ctx))
            return callOriginal(kMovieUnloadMovie, rdram, ctx, runtime);
        releaseMovie(rdram, ctx, runtime);
    }

    double elapsedFrames(const Movie &movie)
    {
        if (!movie.clockRunning)
            return 0.0;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - movie.start).count();
        return seconds * movie.player->fps();
    }

    void movieIsWaiting(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!isOurs(ctx))
            return callOriginal(kMovieIsMovieWaiting, rdram, ctx, runtime);
        const bool waiting = g_movie->clockRunning && elapsedFrames(*g_movie) < g_movie->current - 1;
        SET_GPR_U32(ctx, 2, waiting ? 1u : 0u);
    }

    // Copies frame `current` into the guest buffer as 16x16 RGBA blocks, row by row of blocks.
    void movieDoFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!isOurs(ctx))
            return callOriginal(kMovieDoFrame, rdram, ctx, runtime);
        Movie &m = *g_movie;
        if (!m.clockRunning)
        {
            m.clockRunning = true;
            m.start = std::chrono::steady_clock::now() -
                      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                          std::chrono::duration<double>((m.current - 1) / m.player->fps()));
            g_audioStarted = true;
        }
        Frame frame;
        if (m.shown == m.current || !m.player->takeFrame(m.current, frame))
            return;
        m.shown = frame.number;
        static const bool testPattern = [] { const char *v = std::getenv("POP_MOVIE_TEST"); return v && *v == '1'; }();
        if (testPattern)
        {
            // Debug: each pixel holds its own coordinates (R = x & 255, G = y & 255,
            // B = 64 * (x >> 8) + 16 * (y >> 8)), to check the block layout on screen.
            for (uint32_t y = 0; y < m.pictureHeight; ++y)
                for (uint32_t x = 0; x < m.width; ++x)
                {
                    uint8_t *px = frame.rgba.data() + (static_cast<size_t>(y) * m.width + x) * 4u;
                    px[0] = static_cast<uint8_t>(x);
                    px[1] = static_cast<uint8_t>(y);
                    px[2] = static_cast<uint8_t>(64u * (x >> 8) + 16u * (y >> 8));
                }
        }
        uint8_t *dst = getMemPtr(rdram, m.output);
        const uint32_t top = (m.height - m.pictureHeight) / 2u;
        const uint32_t blocksX = m.width / 16u;
        for (uint32_t y = 0; y < m.pictureHeight; ++y)
        {
            const uint32_t row = top + y;
            const uint8_t *src = frame.rgba.data() + static_cast<size_t>(y) * m.width * 4u;
            uint8_t *blockRow = dst + (row / 16u) * blocksX * 1024u + (row % 16u) * 64u;
            for (uint32_t bx = 0; bx < blocksX; ++bx)
                std::memcpy(blockRow + bx * 1024u, src + bx * 64u, 64u);
        }
    }

    void movieGotoNextFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!isOurs(ctx))
            return callOriginal(kMovieGotoNextFrame, rdram, ctx, runtime);
        Movie &m = *g_movie;
        // Follow the clock when the game runs slower than the movie, so sound stays in sync.
        const uint32_t due = static_cast<uint32_t>(elapsedFrames(m)) + 1u;
        m.current = std::min(std::max(m.current + 1u, due), m.player->frameCount());
    }

    void movieGotoFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!isOurs(ctx))
            return callOriginal(kMovieGotoFrame, rdram, ctx, runtime);
        Movie &m = *g_movie;
        const uint32_t frame = std::max(getRegU32(ctx, 5), 1u);
        if (frame <= 1u && m.shown > 0u)
        {
            // Restart (looping movies): decode again from the start.
            {
                std::lock_guard<std::mutex> lock(g_audioMutex);
                g_audioSource = nullptr;
                g_audioStarted = false;
            }
            m.player.reset();
            if (!startPlayer(m))
            {
                std::cerr << "[pop:movie] restart failed for " << m.path << std::endl;
                return;
            }
        }
        m.current = std::min(frame, m.player->frameCount());
        m.clockRunning = false;
    }

    void movieGetter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, uint32_t value)
    {
        if (!isOurs(ctx))
            return callOriginal(addr, rdram, ctx, runtime);
        SET_GPR_U32(ctx, 2, value);
    }

    void movieGetNbFrames(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        movieGetter(rdram, ctx, runtime, kMovieGetNbFrames, g_movie && g_movie->player ? g_movie->player->frameCount() : 0u);
    }
    void movieGetWidth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        movieGetter(rdram, ctx, runtime, kMovieGetWidth, g_movie ? g_movie->width : 0u);
    }
    void movieGetHeight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        movieGetter(rdram, ctx, runtime, kMovieGetHeight, g_movie ? g_movie->height : 0u);
    }
    void movieGetLastFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        movieGetter(rdram, ctx, runtime, kMovieGetLastFrame, g_movie ? g_movie->current : 0u);
    }
    void movieGetVideoOutput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        movieGetter(rdram, ctx, runtime, kMovieGetVideoOutput, g_movie ? g_movie->output : 0u);
    }
}


void installPopMovies(PS2Runtime &runtime)
{
    const char *env = std::getenv("POP_MOVIES");
    if (env && std::atoi(env) == 0)
        return;
    const std::pair<uint32_t, PS2Runtime::RecompiledFunction> hooks[] = {
        {kMovieLoadMovie, movieLoad},
        {kMovieUnloadMovie, movieUnload},
        {kMovieDoFrame, movieDoFrame},
        {kMovieGotoNextFrame, movieGotoNextFrame},
        {kMovieGotoFrame, movieGotoFrame},
        {kMovieIsMovieWaiting, movieIsWaiting},
        {kMovieGetNbFrames, movieGetNbFrames},
        {kMovieGetWidth, movieGetWidth},
        {kMovieGetHeight, movieGetHeight},
        {kMovieGetLastFrame, movieGetLastFrame},
        {kMovieGetVideoOutput, movieGetVideoOutput},
    };
    for (const auto &[addr, fn] : hooks)
    {
        PS2Runtime::RecompiledFunction original = runtime.lookupFunction(addr);
        if (!original)
        {
            std::cerr << "[pop:movie] no function at 0x" << std::hex << addr << std::dec << ", movies stay skipped" << std::endl;
            return;
        }
        g_originals[addr] = original;
    }
    for (const auto &[addr, fn] : hooks)
        runtime.replaceFunction(addr, fn);
}
