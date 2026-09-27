#include "player.h"

#include <stdio.h>
#include <unistd.h>

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include "util.h"

double video_duration(const std::string &path)
{
    AVFormatContext *fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0)
        return 0;
    double d = 0;
    if (avformat_find_stream_info(fc, nullptr) >= 0 && fc->duration > 0)
        d = fc->duration / (double)AV_TIME_BASE;
    avformat_close_input(&fc);
    return d;
}

bool VideoPlayer::start(const std::string &path, cv::Size box)
{
    stop();
    path_ = path;
    box_ = box;
    done_ = false;
    run_ = true;
    th_ = std::thread(&VideoPlayer::loop, this);
    return true;
}

void VideoPlayer::stop()
{
    run_ = false;
    if (th_.joinable())
        th_.join();
}

bool VideoPlayer::frame(cv::Mat &bgra)
{
    std::lock_guard<std::mutex> lk(m_);
    if (last_.empty())
        return false;
    last_.copyTo(bgra);
    return true;
}

void VideoPlayer::loop()
{
    AVFormatContext *fc = nullptr;
    AVCodecContext *cc = nullptr;
    SwsContext *sws = nullptr;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int vs = -1;
    int64_t t0 = -1;  // wall clock of the first frame
    double pts0 = 0;

    if (avformat_open_input(&fc, path_.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(fc, nullptr) < 0) {
        fprintf(stderr, "player: cannot open %s\n", path_.c_str());
        goto out;
    }
    duration_ = fc->duration > 0 ? fc->duration / (double)AV_TIME_BASE : 0;
    vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0)
        goto out;
    {
        const AVCodec *dec = avcodec_find_decoder(fc->streams[vs]->codecpar->codec_id);
        cc = dec ? avcodec_alloc_context3(dec) : nullptr;
        if (!cc || avcodec_parameters_to_context(cc, fc->streams[vs]->codecpar) < 0)
            goto out;
        cc->thread_count = 2;
        if (avcodec_open2(cc, dec, nullptr) < 0)
            goto out;
    }
    while (run_.load() && av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index != vs || avcodec_send_packet(cc, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);
        while (run_.load() && avcodec_receive_frame(cc, fr) == 0) {
            // fit into the box, keep the aspect
            double s = std::min((double)box_.width / fr->width, (double)box_.height / fr->height);
            int w = std::max(2, (int)(fr->width * s)) & ~1, h = std::max(2, (int)(fr->height * s)) & ~1;
            sws = sws_getCachedContext(sws, fr->width, fr->height, (AVPixelFormat)fr->format, w, h, AV_PIX_FMT_BGRA,
                                       SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            cv::Mat img(h, w, CV_8UC4);
            uint8_t *dst[1] = {img.data};
            int dst_stride[1] = {(int)img.step};
            sws_scale(sws, fr->data, fr->linesize, 0, fr->height, dst, dst_stride);
            // pace by the stream timestamps (drop nothing: the decoder is the slow part)
            double pts = fr->best_effort_timestamp == AV_NOPTS_VALUE
                             ? 0
                             : fr->best_effort_timestamp * av_q2d(fc->streams[vs]->time_base);
            if (t0 < 0) {
                t0 = util::mono_ms();
                pts0 = pts;
            }
            int64_t due = t0 + (int64_t)((pts - pts0) * 1000);
            int64_t wait = due - util::mono_ms();
            if (wait > 0 && wait < 2000)
                usleep(wait * 1000);
            {
                std::lock_guard<std::mutex> lk(m_);
                last_ = img;
            }
            seq_++;
        }
    }
out:
    if (sws)
        sws_freeContext(sws);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    if (cc)
        avcodec_free_context(&cc);
    if (fc)
        avformat_close_input(&fc);
    done_ = true;
}
