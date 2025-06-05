#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/audio_fifo.h>

#define OUTPUT_WIDTH 1920
#define OUTPUT_HEIGHT 1080
#define OUTPUT_FPS 25
#define OUTPUT_SAMPLE_RATE 48000
#define PREBUFFER_SECONDS 5

typedef struct InputContext {
    AVFormatContext *fmt_ctx;
    AVCodecContext *vdec_ctx;
    AVCodecContext *adec_ctx;
    int vstream;
    int astream;
    struct SwsContext *sws;
    struct SwrContext *swr;
    AVFrame *frame;
} InputContext;

typedef struct OutputContext {
    AVFormatContext *fmt_ctx;
    AVCodecContext *venc_ctx;
    AVCodecContext *aenc_ctx;
    AVStream *vstream;
    AVStream *astream;
} OutputContext;

typedef struct PrebufferThread {
    pthread_t thread;
    char filename[1024];
    InputContext ictx;
    atomic_int ready;
} PrebufferThread;

static int open_input(const char *fname, InputContext *ictx)
{
    memset(ictx, 0, sizeof(*ictx));
    if (avformat_open_input(&ictx->fmt_ctx, fname, NULL, NULL) < 0)
        return -1;
    if (avformat_find_stream_info(ictx->fmt_ctx, NULL) < 0)
        return -1;
    ictx->vstream = av_find_best_stream(ictx->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    ictx->astream = av_find_best_stream(ictx->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ictx->vstream >= 0) {
        const AVCodec *dec = avcodec_find_decoder(ictx->fmt_ctx->streams[ictx->vstream]->codecpar->codec_id);
        ictx->vdec_ctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(ictx->vdec_ctx, ictx->fmt_ctx->streams[ictx->vstream]->codecpar);
        avcodec_open2(ictx->vdec_ctx, dec, NULL);
        ictx->sws = sws_getContext(ictx->vdec_ctx->width, ictx->vdec_ctx->height, ictx->vdec_ctx->pix_fmt,
                                   OUTPUT_WIDTH, OUTPUT_HEIGHT, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    }
    if (ictx->astream >= 0) {
        const AVCodec *dec = avcodec_find_decoder(ictx->fmt_ctx->streams[ictx->astream]->codecpar->codec_id);
        ictx->adec_ctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(ictx->adec_ctx, ictx->fmt_ctx->streams[ictx->astream]->codecpar);
        avcodec_open2(ictx->adec_ctx, dec, NULL);
        ictx->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLTP, OUTPUT_SAMPLE_RATE,
                                       ictx->adec_ctx->channel_layout ? ictx->adec_ctx->channel_layout : av_get_default_channel_layout(ictx->adec_ctx->channels),
                                       ictx->adec_ctx->sample_fmt, ictx->adec_ctx->sample_rate, 0, NULL);
        swr_init(ictx->swr);
    }
    ictx->frame = av_frame_alloc();
    return 0;
}

static void close_input(InputContext *ictx)
{
    if (ictx->vdec_ctx) avcodec_free_context(&ictx->vdec_ctx);
    if (ictx->adec_ctx) avcodec_free_context(&ictx->adec_ctx);
    if (ictx->fmt_ctx) avformat_close_input(&ictx->fmt_ctx);
    if (ictx->sws) sws_freeContext(ictx->sws);
    if (ictx->swr) swr_free(&ictx->swr);
    if (ictx->frame) av_frame_free(&ictx->frame);
}

static int init_output(const char *fname, OutputContext *octx)
{
    memset(octx, 0, sizeof(*octx));
    avformat_alloc_output_context2(&octx->fmt_ctx, NULL, NULL, fname);
    if (!octx->fmt_ctx) return -1;
    // video encoder
    const AVCodec *venc = avcodec_find_encoder_by_name("libx264");
    if (!venc) venc = avcodec_find_encoder(AV_CODEC_ID_H264);
    octx->vstream = avformat_new_stream(octx->fmt_ctx, venc);
    octx->venc_ctx = avcodec_alloc_context3(venc);
    octx->venc_ctx->width = OUTPUT_WIDTH;
    octx->venc_ctx->height = OUTPUT_HEIGHT;
    octx->venc_ctx->time_base = (AVRational){1, OUTPUT_FPS};
    octx->venc_ctx->framerate = (AVRational){OUTPUT_FPS,1};
    octx->venc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    avcodec_open2(octx->venc_ctx, venc, NULL);
    avcodec_parameters_from_context(octx->vstream->codecpar, octx->venc_ctx);
    // audio encoder
    const AVCodec *aenc = avcodec_find_encoder_by_name("aac");
    if (!aenc) aenc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    octx->astream = avformat_new_stream(octx->fmt_ctx, aenc);
    octx->aenc_ctx = avcodec_alloc_context3(aenc);
    octx->aenc_ctx->sample_rate = OUTPUT_SAMPLE_RATE;
    octx->aenc_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
    octx->aenc_ctx->channels = 2;
    octx->aenc_ctx->sample_fmt = aenc->sample_fmts ? aenc->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    octx->aenc_ctx->time_base = (AVRational){1, OUTPUT_SAMPLE_RATE};
    avcodec_open2(octx->aenc_ctx, aenc, NULL);
    avcodec_parameters_from_context(octx->astream->codecpar, octx->aenc_ctx);
    if (!(octx->fmt_ctx->oformat->flags & AVFMT_NOFILE))
        if (avio_open(&octx->fmt_ctx->pb, fname, AVIO_FLAG_WRITE) < 0) return -1;
    return avformat_write_header(octx->fmt_ctx, NULL);
}

static void close_output(OutputContext *octx)
{
    av_write_trailer(octx->fmt_ctx);
    if (!(octx->fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&octx->fmt_ctx->pb);
    if (octx->venc_ctx) avcodec_free_context(&octx->venc_ctx);
    if (octx->aenc_ctx) avcodec_free_context(&octx->aenc_ctx);
    if (octx->fmt_ctx) avformat_free_context(octx->fmt_ctx);
}

static void *prebuffer_func(void *arg)
{
    PrebufferThread *pb = arg;
    open_input(pb->filename, &pb->ictx);
    pb->ready = 1;
    return NULL;
}

static int read_next_path(FILE *fp, char *buf, size_t size)
{
    while (1) {
        if (fgets(buf, size, fp)) {
            size_t len = strlen(buf);
            while (len && (buf[len-1]=='\n' || buf[len-1]=='\r')) buf[--len]='\0';
            if (len) return 0;
        } else {
            clearerr(fp);
            sleep(1);
        }
    }
}

static int encode_write(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt, AVFormatContext *fmt, int stream_index)
{
    int ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0) return ret;
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
        else if (ret < 0) return ret;
        pkt->stream_index = stream_index;
        av_interleaved_write_frame(fmt, pkt);
        av_packet_unref(pkt);
    }
    return 0;
}

static int process(InputContext *ictx, OutputContext *octx, int64_t *vpts_off, int64_t *apts_off)
{
    AVPacket pkt;
    av_init_packet(&pkt);
    while (av_read_frame(ictx->fmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index == ictx->vstream) {
            avcodec_send_packet(ictx->vdec_ctx, &pkt);
            while (avcodec_receive_frame(ictx->vdec_ctx, ictx->frame) >= 0) {
                AVFrame *f = ictx->frame;
                AVFrame *out = av_frame_alloc();
                out->format = AV_PIX_FMT_YUV420P;
                out->width = OUTPUT_WIDTH;
                out->height = OUTPUT_HEIGHT;
                av_frame_get_buffer(out, 0);
                sws_scale(ictx->sws, (const uint8_t * const*)f->data, f->linesize, 0, ictx->vdec_ctx->height, out->data, out->linesize);
                out->pts = av_rescale_q(f->pts, ictx->fmt_ctx->streams[ictx->vstream]->time_base, octx->venc_ctx->time_base) + *vpts_off;
                encode_write(octx->venc_ctx, out, &pkt, octx->fmt_ctx, octx->vstream->index);
                *vpts_off = out->pts + 1;
                av_frame_free(&out);
            }
        } else if (pkt.stream_index == ictx->astream) {
            avcodec_send_packet(ictx->adec_ctx, &pkt);
            while (avcodec_receive_frame(ictx->adec_ctx, ictx->frame) >= 0) {
                AVFrame *f = ictx->frame;
                AVFrame *out = av_frame_alloc();
                out->format = octx->aenc_ctx->sample_fmt;
                out->channel_layout = AV_CH_LAYOUT_STEREO;
                out->sample_rate = OUTPUT_SAMPLE_RATE;
                out->nb_samples = av_rescale_rnd(swr_get_delay(ictx->swr, ictx->adec_ctx->sample_rate) + f->nb_samples, OUTPUT_SAMPLE_RATE, ictx->adec_ctx->sample_rate, AV_ROUND_UP);
                av_frame_get_buffer(out, 0);
                swr_convert(ictx->swr, out->data, out->nb_samples, (const uint8_t **)f->data, f->nb_samples);
                out->pts = av_rescale_q(f->pts, ictx->fmt_ctx->streams[ictx->astream]->time_base, octx->aenc_ctx->time_base) + *apts_off;
                encode_write(octx->aenc_ctx, out, &pkt, octx->fmt_ctx, octx->astream->index);
                *apts_off = out->pts + out->nb_samples;
                av_frame_free(&out);
            }
        }
        av_packet_unref(&pkt);
    }
    // flush decoders
    avcodec_send_packet(ictx->vdec_ctx, NULL);
    while (avcodec_receive_frame(ictx->vdec_ctx, ictx->frame) >= 0) {
        AVFrame *out = av_frame_alloc();
        out->format = AV_PIX_FMT_YUV420P;
        out->width = OUTPUT_WIDTH;
        out->height = OUTPUT_HEIGHT;
        av_frame_get_buffer(out, 0);
        sws_scale(ictx->sws, (const uint8_t * const*)ictx->frame->data, ictx->frame->linesize, 0, ictx->vdec_ctx->height, out->data, out->linesize);
        out->pts = av_rescale_q(ictx->frame->pts, ictx->fmt_ctx->streams[ictx->vstream]->time_base, octx->venc_ctx->time_base) + *vpts_off;
        encode_write(octx->venc_ctx, out, &pkt, octx->fmt_ctx, octx->vstream->index);
        *vpts_off = out->pts + 1;
        av_frame_free(&out);
    }
    avcodec_send_packet(ictx->adec_ctx, NULL);
    while (avcodec_receive_frame(ictx->adec_ctx, ictx->frame) >= 0) {
        AVFrame *out = av_frame_alloc();
        out->format = octx->aenc_ctx->sample_fmt;
        out->channel_layout = AV_CH_LAYOUT_STEREO;
        out->sample_rate = OUTPUT_SAMPLE_RATE;
        out->nb_samples = av_rescale_rnd(swr_get_delay(ictx->swr, ictx->adec_ctx->sample_rate) + ictx->frame->nb_samples, OUTPUT_SAMPLE_RATE, ictx->adec_ctx->sample_rate, AV_ROUND_UP);
        av_frame_get_buffer(out, 0);
        swr_convert(ictx->swr, out->data, out->nb_samples, (const uint8_t **)ictx->frame->data, ictx->frame->nb_samples);
        out->pts = av_rescale_q(ictx->frame->pts, ictx->fmt_ctx->streams[ictx->astream]->time_base, octx->aenc_ctx->time_base) + *apts_off;
        encode_write(octx->aenc_ctx, out, &pkt, octx->fmt_ctx, octx->astream->index);
        *apts_off = out->pts + out->nb_samples;
        av_frame_free(&out);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s playlist.txt output.mp4\n", argv[0]);
        return 1;
    }
    const char *playlist = argv[1];
    const char *output = argv[2];
    FILE *pl = fopen(playlist, "r");
    if (!pl) {
        fprintf(stderr, "Could not open playlist %s\n", playlist);
        return 1;
    }
    av_log_set_level(AV_LOG_ERROR);
    OutputContext octx;
    if (init_output(output, &octx) < 0) {
        fprintf(stderr, "Could not init output\n");
        return 1;
    }

    char current[1024];
    if (read_next_path(pl, current, sizeof(current)) < 0) {
        fprintf(stderr, "Playlist empty\n");
        return 1;
    }

    InputContext ictx;
    if (open_input(current, &ictx) < 0) {
        fprintf(stderr, "Could not open input %s\n", current);
        return 1;
    }

    int64_t vpts_off = 0, apts_off = 0;
    PrebufferThread prebuf = {0};

    while (1) {
        // start prebuffer for next file
        if (read_next_path(pl, prebuf.filename, sizeof(prebuf.filename)) == 0) {
            prebuf.ready = 0;
            pthread_create(&prebuf.thread, NULL, prebuffer_func, &prebuf);
        } else {
            prebuf.filename[0] = '\0';
        }

        process(&ictx, &octx, &vpts_off, &apts_off);
        close_input(&ictx);
        if (prebuf.filename[0]) {
            pthread_join(prebuf.thread, NULL);
            ictx = prebuf.ictx;
        } else {
            break;
        }
    }

    close_output(&octx);
    fclose(pl);
    return 0;
}

