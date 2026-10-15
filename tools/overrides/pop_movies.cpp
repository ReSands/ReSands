// Movie playback for Prince of Persia: The Sands of Time (SLUS_207.43), natively, behind the game's
// own player. Two sources, both decoded with FFmpeg:
//  - the PS2 disc's own movies (*.INT): IPU frames (MPEG-2 intra macroblocks) decoded by FFmpeg's
//    `ipu` decoder, with the movie's own PCM sound. No IPU emulation. Default.
//  - optionally, the Bink files of another owned version (Xbox, PC/GOG or GameCube).
// Without a usable file the old behaviour stays (the movie is reported missing and skipped).
//
// The game plays movies through VIDEO_MainCall (0x4378C0), which calls small wrappers around
// its FullMovie object. Those wrappers are replaced here; everything above them (button skip,
// looping, sound pause, centring, drawing with PS2_DisplayVideo) is the game's own code.
// PS2_DisplayVideo uploads the frame from a guest buffer of 16x16-pixel RGBA blocks.
//
// Environment: POP_MOVIES=0 turns this off. POP_MOVIE_SOURCE=ps2 (default) or bink picks the
// preferred source (the other is the fallback). POP_PS2_MOVIE_DIR is the folder with the disc's
// .INT files (default: the current directory, game/ps2/disc). POP_MOVIE_DIRS lists folders to
// search for Bink files, separated by ';' (default: the Xbox, GOG and GameCube folders relative
// to game/ps2/disc, in that order: the Xbox files have the highest bitrate and 5.1 sound).
// Debug: POP_MOVIE_FORCE=<file or archive movie name> plays it for every movie; POP_MOVIE_TEST=1 draws a
// coordinate test pattern.

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
#include <fstream>
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

    // Decodes one movie on its own thread: RGBA frames into a queue, stereo s16 into a ring
    // that the movie audio stream drains. Sources (Bink, PS2) supply the decode loop.
    class MoviePlayer
    {
    public:
        virtual ~MoviePlayer() = default;

        // Stops and joins the decode thread; derived destructors call it before freeing.
        void stop()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_cv.notify_all();
            if (m_thread.joinable())
                m_thread.join();
        }

        uint32_t frameCount() const { return m_frames; }
        double fps() const { return m_fps; }
        virtual std::string audioDescription() const = 0;

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

    protected:
        void startThread()
        {
            m_thread = std::thread([this] {
                decodeLoop();
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_eof = true;
                }
                m_cv.notify_all();
            });
        }

        virtual void decodeLoop() = 0;

        // Blocks while the frame queue is full or the sound ring has less than `room` samples
        // free; false once stopping.
        bool waitForRoom(size_t room = kSampleRate * 2)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this, room] {
                return m_stop || (m_queue.size() < kQueuedFrames && m_ringQueued + room <= kAudioRing);
            });
            return !m_stop;
        }

        // Converts a decoded picture to RGBA at the output size and queues it.
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

        // Queues interleaved stereo s16 samples (as many as fit).
        void pushPcm(const int16_t *pcm, size_t samples)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const size_t n = std::min(samples, kAudioRing - m_ringQueued);
            for (size_t i = 0; i < n; ++i)
                m_ring[(m_ringRead + m_ringQueued + i) % kAudioRing] = pcm[i];
            m_ringQueued += n;
        }

        void freeScaler()
        {
            sws_freeContext(m_sws);
            m_sws = nullptr;
        }

        int m_outWidth = 0, m_pictureHeight = 0;
        double m_fps = 29.97;
        uint32_t m_frames = 0;

    private:
        SwsContext *m_sws = nullptr;
        std::thread m_thread;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_stop = false, m_eof = false;
        std::deque<Frame> m_queue;
        std::vector<int16_t> m_ring = std::vector<int16_t>(kAudioRing);
        size_t m_ringRead = 0, m_ringQueued = 0;
    };

    // A Bink file of another version.
    class BinkPlayer : public MoviePlayer
    {
    public:
        ~BinkPlayer() override
        {
            stop();
            freeScaler();
            for (AudioTrack &t : m_tracks)
            {
                swr_free(&t.swr);
                avcodec_free_context(&t.codec);
            }
            avcodec_free_context(&m_videoCodec);
            avformat_close_input(&m_fmt);
        }

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

            startThread();
            return m_frames > 0;
        }

        std::string audioDescription() const override
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

        void decodeLoop() override
        {
            AVPacket *pkt = av_packet_alloc();
            AVFrame *frame = av_frame_alloc();
            uint32_t number = 0;
            bool flushed = false;
            while (!flushed && waitForRoom())
            {
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
            av_frame_free(&frame);
            av_packet_free(&pkt);
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
            pushPcm(mixed.data(), mixed.size());
        }

        AVFormatContext *m_fmt = nullptr;
        AVCodecContext *m_videoCodec = nullptr;
        std::vector<AudioTrack> m_tracks;
        float m_mixScale = 1.0f;
        int m_video = -1;
    };

    // The PS2 disc's own movie files (*.INT). Layout (little-endian), read from the files and the
    // game's player (FullMovie::mReadHeaders, VID_fn_vDoDecodeMovie):
    //  0x00 header: +0x04 frame rate (float), +0x08 frames per chunk, +0x0C chunk count N,
    //       +0x10 sound track count T (languages), +0x18/+0x1C/+0x20/+0x24 sizes of the video
    //       header, alpha header, each sound header and the subtitles;
    //  0x28 N + 1 chunk offsets; the video header (version 3, then 0x80 bytes of movie info:
    //       +0x60/+0x64 width/height in macroblocks, +0x70 frame count; then one offset per
    //       frame); the alpha header; T sound headers (0x14 bytes: ..., bits 16, channels 2,
    //       48000 Hz; then N + 1 offsets of each chunk's slice); the subtitles.
    //  Chunk data starts at the header's end rounded up to 64 bytes (from the movie's start, also
    //  for the movies packed in MOVIEPS2.BIG). Chunk c holds frames
    //  c * fpc ... in order, frame k at index[k] - index[c * fpc], each ending with zero padding
    //  to 16 bytes and the code 00 00 01 B0; then, 16-byte aligned, one slice of raw s16 stereo
    //  PCM per track, track after track.
    // A frame is the data of one IPU IDEC command: MPEG-2 intra macroblocks without headers.
    // FFmpeg's `ipu` decoder takes it as a flag byte (decoding settings; 0 = MPEG-2, zigzag,
    // linear quantiser, DC precision 8 bits: right for all 11 movies on the disc, every frame
    // checked against an independent IPU decoder) followed by the macroblock data and the end
    // code, with exactly the data's bytes before the end code.
    class Ps2MoviePlayer : public MoviePlayer
    {
    public:
        ~Ps2MoviePlayer() override
        {
            stop();
            freeScaler();
            av_packet_free(&m_packet);
            av_frame_free(&m_frame);
            avcodec_free_context(&m_codec);
        }

        // Reads the headers of the movie at `offset` in the file and sets up the decoder;
        // start() begins decoding.
        bool open(const std::string &path, uint64_t offset, int track)
        {
            m_file.open(path, std::ios::binary);
            m_file.seekg(static_cast<std::streamoff>(offset));
            uint8_t head[0x28];
            if (!m_file.read(reinterpret_cast<char *>(head), sizeof head))
                return false;
            float fps;
            std::memcpy(&fps, head + 4, 4);
            m_framesPerChunk = u32(head + 0x08);
            const uint32_t chunks = u32(head + 0x0C);
            m_tracks = u32(head + 0x10);
            const uint32_t videoSize = u32(head + 0x18), alphaSize = u32(head + 0x1C);
            const uint32_t soundSize = u32(head + 0x20), subtitleSize = u32(head + 0x24);
            if (!head[0x14] || !m_framesPerChunk || !chunks || chunks > 100000u || videoSize < 4u + 0x80u ||
                (m_tracks && soundSize < 0x14u + (chunks + 1u) * 4u) || m_tracks > 16u)
                return false;
            const uint64_t tables = (chunks + 1ull) * 4ull + videoSize + alphaSize + uint64_t(soundSize) * m_tracks + subtitleSize;
            std::vector<uint8_t> t(static_cast<size_t>(tables));
            if (tables > (64ull << 20) || !m_file.read(reinterpret_cast<char *>(t.data()), static_cast<std::streamsize>(t.size())))
                return false;
            m_chunks.resize(chunks + 1u);
            for (uint32_t i = 0; i <= chunks; ++i)
                m_chunks[i] = u32(t.data() + i * 4u);
            const uint8_t *video = t.data() + (chunks + 1u) * 4u;
            if (u32(video) != 3u)
                return false;
            const uint8_t *info = video + 4;
            m_width = u32(info + 0x60) * 16u;
            m_height = u32(info + 0x64) * 16u;
            const uint32_t frames = u32(info + 0x70);
            if (!m_width || !m_height || m_width > 1024u || m_height > 1024u || !frames ||
                4u + 0x80u + uint64_t(frames) * 4u > videoSize)
                return false;
            m_index.resize(frames);
            for (uint32_t i = 0; i < frames; ++i)
                m_index[i] = u32(info + 0x80 + i * 4u);
            const uint8_t *sound = video + videoSize + alphaSize;
            m_slices.assign(m_tracks, {});
            for (uint32_t k = 0; k < m_tracks; ++k)
            {
                m_slices[k].resize(chunks + 1u);
                for (uint32_t i = 0; i <= chunks; ++i)
                    m_slices[k][i] = u32(sound + uint64_t(k) * soundSize + 0x14 + i * 4u);
            }
            m_track = (track >= 0 && static_cast<uint32_t>(track) < m_tracks) ? static_cast<uint32_t>(track) : 0u;
            m_dataStart = offset + ((sizeof head + tables + 63u) & ~uint64_t{63}); // aligned within the movie

            const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_IPU);
            if (!codec || !(m_codec = avcodec_alloc_context3(codec)))
                return false;
            m_codec->width = static_cast<int>(m_width);
            m_codec->height = static_cast<int>(m_height);
            if (avcodec_open2(m_codec, codec, nullptr) < 0 || !(m_packet = av_packet_alloc()) || !(m_frame = av_frame_alloc()))
                return false;

            m_fps = fps > 1.0f && fps < 120.0f ? fps : 29.97;
            m_frames = frames;
            return true;
        }

        void start(int outWidth, int pictureHeight)
        {
            m_outWidth = outWidth;
            m_pictureHeight = pictureHeight;
            startThread();
        }

        uint32_t width() const { return m_width; }
        uint32_t height() const { return m_height; }

        std::string audioDescription() const override
        {
            return m_tracks ? "PCM track " + std::to_string(m_track) + " of " + std::to_string(m_tracks) : "no audio";
        }

    private:
        static uint32_t u32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        static size_t findEndCode(const std::vector<uint8_t> &data, size_t from)
        {
            static const uint8_t kEnd[4] = {0x00, 0x00, 0x01, 0xB0};
            const auto it = std::search(data.begin() + static_cast<std::ptrdiff_t>(from), data.end(), kEnd, kEnd + 4);
            return it == data.end() ? SIZE_MAX : static_cast<size_t>(it - data.begin());
        }

        // Decodes one frame's IDEC data [begin, end) (end = its end code) and queues the picture.
        void decodeFrame(const uint8_t *begin, const uint8_t *end, uint32_t number)
        {
            // The data's last 1 bit belongs to the final end-of-block code ("10" or "0110"), whose
            // last 0 bit follows it: if that 1 is a byte's lowest bit, the next (zero) byte is data.
            while (end > begin && end[-1] == 0)
                --end;
            size_t bytes = static_cast<size_t>(end - begin);
            if (bytes && (end[-1] & 1u))
                ++bytes;
            if (av_new_packet(m_packet, static_cast<int>(1u + bytes + 4u)) < 0)
                return;
            m_packet->data[0] = 0x00; // flags: MPEG-2, zigzag, linear quantiser, DC precision 8
            std::memcpy(m_packet->data + 1, begin, static_cast<size_t>(end - begin));
            if (bytes > static_cast<size_t>(end - begin))
                m_packet->data[bytes] = 0;
            static const uint8_t kEnd[4] = {0x00, 0x00, 0x01, 0xB0};
            std::memcpy(m_packet->data + 1 + bytes, kEnd, 4);
            const int sent = avcodec_send_packet(m_codec, m_packet);
            av_packet_unref(m_packet);
            if (sent < 0)
            {
                if (m_badFrames++ < 5)
                    std::cerr << "[pop:movie] frame " << number << " does not decode" << std::endl;
                return;
            }
            while (avcodec_receive_frame(m_codec, m_frame) >= 0)
                pushVideo(m_frame, number);
        }

        void decodeLoop() override
        {
            std::vector<uint8_t> chunk;
            std::vector<int16_t> pcm;
            const uint32_t chunks = static_cast<uint32_t>(m_chunks.size() - 1u);
            for (uint32_t c = 0; c < chunks; ++c)
            {
                const uint32_t first = c * m_framesPerChunk;
                if (first >= m_frames)
                    break;
                const uint32_t last = std::min(first + m_framesPerChunk, m_frames); // exclusive
                if (m_chunks[c + 1] < m_chunks[c])
                    break;
                chunk.resize(m_chunks[c + 1] - m_chunks[c]);
                m_file.clear();
                m_file.seekg(static_cast<std::streamoff>(m_dataStart + m_chunks[c]));
                if (!m_file.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size())))
                    break;

                size_t videoEnd = 0;
                for (uint32_t f = first; f < last; ++f)
                {
                    if (!waitForRoom())
                        return;
                    const size_t start = m_index[f] - m_index[first];
                    const size_t end = start < chunk.size() ? findEndCode(chunk, start) : SIZE_MAX;
                    if (end == SIZE_MAX)
                        break;
                    decodeFrame(chunk.data() + start, chunk.data() + end, f + 1u);
                    videoEnd = end + 4u;
                }

                if (m_tracks)
                {
                    size_t offset = (videoEnd + 15u) & ~size_t{15};
                    for (uint32_t k = 0; k < m_track; ++k)
                        offset += m_slices[k][c + 1] - m_slices[k][c];
                    const size_t bytes = m_slices[m_track][c + 1] - m_slices[m_track][c];
                    if (offset + bytes <= chunk.size())
                    {
                        pcm.resize(bytes / 2u);
                        std::memcpy(pcm.data(), chunk.data() + offset, pcm.size() * 2u);
                        if (!waitForRoom(pcm.size()))
                            return;
                        pushPcm(pcm.data(), pcm.size());
                    }
                }
            }
        }

        std::ifstream m_file;
        uint64_t m_dataStart = 0;
        uint32_t m_framesPerChunk = 0, m_tracks = 0, m_track = 0;
        uint32_t m_width = 0, m_height = 0;
        std::vector<uint32_t> m_chunks, m_index;
        std::vector<std::vector<uint32_t>> m_slices;
        AVCodecContext *m_codec = nullptr;
        AVPacket *m_packet = nullptr;
        AVFrame *m_frame = nullptr;
        int m_badFrames = 0;
    };

    // One movie at a time (the game has a single FullMovie).
    struct Movie
    {
        uint32_t object = 0;      // the game's FullMovie
        std::string path;
        bool ps2 = false;         // the disc's own movie (else a Bink file)
        uint64_t offset = 0;      // where the PS2 movie starts in its file
        int track = 0;
        std::unique_ptr<MoviePlayer> player;
        uint32_t allocation = 0;  // guest block from the game's allocator
        uint32_t output = 0;      // 64-byte aligned frame buffer in it
        uint32_t width = 0, height = 0, pictureHeight = 0; // padded to 16
        uint32_t current = 1;     // frame the game is on (1-based)
        uint32_t shown = 0;       // frame last copied to the output buffer
        bool clockRunning = false;
        std::chrono::steady_clock::time_point start;
    };

    std::mutex g_audioMutex;
    MoviePlayer *g_audioSource = nullptr; // guarded by g_audioMutex
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

    // The disc's own movie: a file of that name (the name's last path part, without a ";1"
    // version, case-insensitive) in POP_PS2_MOVIE_DIR (default: the current directory), or an
    // entry of MOVIEPS2.BIG there. MOVIEPS2.FAT has 32-byte entries: index, offset, size, 0,
    // name length, name (the vision movies vis_*.int).
    struct Ps2Movie
    {
        std::string path;
        uint64_t offset = 0;
    };

    Ps2Movie findPs2(const std::string &guestName)
    {
        std::string base = guestName.substr(guestName.find_last_of("\\/:") + 1);
        base = base.substr(0, base.find(';'));
        auto lower = [](std::string v) {
            std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
            return v;
        };
        base = lower(base);
        const char *env = std::getenv("POP_PS2_MOVIE_DIR");
        const std::filesystem::path dir = env && *env ? env : ".";
        std::error_code ec;
        std::filesystem::path fat, big;
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
        {
            const std::string name = lower(entry.path().filename().string());
            if (name == base)
                return {entry.path().string(), 0};
            if (name == "movieps2.fat")
                fat = entry.path();
            else if (name == "movieps2.big")
                big = entry.path();
        }
        if (fat.empty() || big.empty())
            return {};
        std::ifstream in(fat, std::ios::binary);
        uint8_t e[32];
        while (in.read(reinterpret_cast<char *>(e), sizeof e))
        {
            uint32_t offset, length;
            std::memcpy(&offset, e + 4, 4);
            std::memcpy(&length, e + 16, 4);
            const std::string name(reinterpret_cast<const char *>(e + 20), strnlen(reinterpret_cast<const char *>(e + 20), std::min<uint32_t>(length, 12u)));
            if (lower(name) == base)
                return {big.string(), offset};
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
        if (movie.ps2)
        {
            auto player = std::make_unique<Ps2MoviePlayer>();
            if (!player->open(movie.path, movie.offset, movie.track))
                return false;
            player->start(static_cast<int>(movie.width), static_cast<int>(movie.pictureHeight));
            movie.player = std::move(player);
        }
        else
        {
            auto player = std::make_unique<BinkPlayer>();
            if (!player->open(movie.path, movie.track, static_cast<int>(movie.width), static_cast<int>(movie.pictureHeight)))
                return false;
            movie.player = std::move(player);
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
        const char *source = std::getenv("POP_MOVIE_SOURCE");
        const bool preferBink = source && std::string(source) == "bink";
        const std::string bink = findBink(name, language >= 0 && language < 5 ? language : 0);
        // POP_MOVIE_FORCE may also name a movie of the disc's archive (e.g. vis_112.int).
        const bool forceFile = force && *force && std::filesystem::exists(force);
        const Ps2Movie ps2 = findPs2(force && *force && !forceFile ? std::string(force) : name);
        std::string path;
        bool isPs2 = false;
        if (forceFile)
        {
            path = force;
            char magic[3] = {};
            std::ifstream(path, std::ios::binary).read(magic, 3);
            isPs2 = std::memcmp(magic, "BIK", 3) != 0 && std::memcmp(magic, "KB2", 3) != 0; // Bink 1 / 2
        }
        else if (!ps2.path.empty() && (!preferBink || bink.empty()))
        {
            path = ps2.path;
            isPs2 = true;
        }
        else
            path = bink;
        if (path.empty())
        {
            std::cerr << "[pop:movie] no movie file for " << name << ", skipping" << std::endl;
            callOriginal(kMovieLoadMovie, rdram, ctx, runtime);
            return;
        }
        releaseMovie(rdram, ctx, runtime);

        auto movie = std::make_unique<Movie>();
        movie->object = object;
        movie->path = path;
        movie->ps2 = isPs2;
        movie->offset = isPs2 && !forceFile ? ps2.offset : 0u;
        movie->track = language;

        // The PS2 movies are 640x448 (full screen) or 640x352 (letterboxed) for a 640x448 4:3
        // screen; the Bink files were made from the PS2 masters: 640 wide for a 640x448 4:3 screen
        // (full-screen movies are 640x448, letterboxed cutscenes 640x346), so their pixels have
        // the PS2's shape. Scale them to this screen's width and height in the same proportion.
        const uint32_t screenW = std::max(guestU32(rdram, kScreenWidth), 16u);
        const uint32_t screenH = std::max(guestU32(rdram, kScreenHeight), 16u);
        AVFormatContext *probe = nullptr;
        int srcW = 640, srcH = 346;
        if (isPs2)
        {
            Ps2MoviePlayer header;
            if (header.open(path, movie->offset, language))
            {
                srcW = static_cast<int>(header.width());
                srcH = static_cast<int>(header.height());
            }
        }
        else if (avformat_open_input(&probe, path.c_str(), nullptr, nullptr) >= 0)
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
