// tutorial03.c
// A pedagogical video player that will stream through every video frame as fast as it can
// and play audio (out of sync).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard, 
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101, SDL 1.2.15
// on GCC 4.7.2 in Debian February 2015
//
// Use
//
// gcc -o tutorial03 tutorial03.c -lavformat -lavcodec -lswscale -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed, 
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial03 myvideofile.mpg
//
// to play the stream on your screen.


#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

#include <SDL2/SDL.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>
#include <assert.h>



#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000


typedef struct PacketQueue {
    AVPacket *packets;
    int capacity;
    int size;
    int front;
    int rear;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

PacketQueue audioq;

int quit = 0;


#define PACKET_QUEUE_CAPACITY 64
void packet_queue_init(PacketQueue *q) {
    q->packets = av_malloc_array(PACKET_QUEUE_CAPACITY, sizeof(AVPacket));
    q->capacity = PACKET_QUEUE_CAPACITY;
    q->size = 0;
    q->front = 0;
    q->rear = 0;
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    SDL_LockMutex(q->mutex);
    if (q->size >= q->capacity) {
        SDL_UnlockMutex(q->mutex);
        return -1;
    }
    av_packet_ref(&q->packets[q->rear], pkt);
    q->rear = (q->rear + 1) % q->capacity;
    q->size++;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    int ret = 0;
    SDL_LockMutex(q->mutex);
    for (;;) {
        if (quit) {
            ret = -1;
            break;
        }
        if (q->size > 0) {
            av_packet_move_ref(pkt, &q->packets[q->front]);
            q->front = (q->front + 1) % q->capacity;
            q->size--;
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {
    static AVPacket pkt;
    static int pkt_in_use = 0;
    static AVFrame *frame = NULL;
    int data_size = 0;
    int ret;
    if (!frame) frame = av_frame_alloc();
    while (1) {
        if (!pkt_in_use) {
            if (packet_queue_get(&audioq, &pkt, 1) < 0) {
                return -1;
            }
            pkt_in_use = 1;
        }
        ret = avcodec_send_packet(aCodecCtx, &pkt);
        if (ret < 0) {
            pkt_in_use = 0;
            av_packet_unref(&pkt);
            continue;
        }
        while (ret >= 0) {
            ret = avcodec_receive_frame(aCodecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                pkt_in_use = 0;
                av_packet_unref(&pkt);
                return -1;
            }
            data_size = av_samples_get_buffer_size(NULL, aCodecCtx->ch_layout.nb_channels, frame->nb_samples, aCodecCtx->sample_fmt, 1);
            if (data_size > buf_size) data_size = buf_size;
            memcpy(audio_buf, frame->data[0], data_size);
            pkt_in_use = 0;
            av_packet_unref(&pkt);
            return data_size;
        }
        pkt_in_use = 0;
        av_packet_unref(&pkt);
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int len1, audio_size;
    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;
    while (len > 0) {
        if (audio_buf_index >= audio_buf_size) {
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if (audio_size < 0) {
                audio_buf_size = 1024;
                memset(audio_buf, 0, audio_buf_size);
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}


int main(int argc, char *argv[]) {
    AVFormatContext *pFormatCtx = NULL;
    int i, videoStream = -1, audioStream = -1;
    AVCodecContext *pCodecCtx = NULL;
    AVCodec *pCodec = NULL;
    AVFrame *pFrame = NULL;
    AVPacket packet;
    struct SwsContext *sws_ctx = NULL;
    AVCodecContext *aCodecCtx = NULL;
    AVCodec *aCodec = NULL;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    SDL_Event event;
    SDL_AudioSpec wanted_spec, spec;
    int width, height;
    uint8_t *yPlane = NULL, *uPlane = NULL, *vPlane = NULL;
    int yPitch, uPitch, vPitch;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        exit(1);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0)
        return -1;
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1;
    av_dump_format(pFormatCtx, 0, argv[1], 0);

    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        AVCodecParameters *codecpar = pFormatCtx->streams[i]->codecpar;
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
            videoStream = i;
        }
        if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0) {
            audioStream = i;
        }
    }
    if (videoStream == -1 || audioStream == -1)
        return -1;

    // Audio
    AVCodecParameters *aCodecPar = pFormatCtx->streams[audioStream]->codecpar;
    aCodec = avcodec_find_decoder(aCodecPar->codec_id);
    if (!aCodec) {
        fprintf(stderr, "Unsupported audio codec!\n");
        return -1;
    }
    aCodecCtx = avcodec_alloc_context3(aCodec);
    avcodec_parameters_to_context(aCodecCtx, aCodecPar);
    if (avcodec_open2(aCodecCtx, aCodec, NULL) < 0) {
        fprintf(stderr, "Could not open audio codec!\n");
        return -1;
    }
    wanted_spec.freq = aCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = aCodecCtx->ch_layout.nb_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = aCodecCtx;
    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }
    packet_queue_init(&audioq);
    SDL_PauseAudio(0);

    // Video
    AVCodecParameters *vCodecPar = pFormatCtx->streams[videoStream]->codecpar;
    pCodec = avcodec_find_decoder(vCodecPar->codec_id);
    if (!pCodec) {
        fprintf(stderr, "Unsupported video codec!\n");
        return -1;
    }
    pCodecCtx = avcodec_alloc_context3(pCodec);
    avcodec_parameters_to_context(pCodecCtx, vCodecPar);
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        fprintf(stderr, "Could not open video codec!\n");
        return -1;
    }
    pFrame = av_frame_alloc();
    width = pCodecCtx->width;
    height = pCodecCtx->height;

    window = SDL_CreateWindow("tutorial03", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, 0);
    if (!window) {
        fprintf(stderr, "SDL: could not create window - exiting\n");
        exit(1);
    }
    renderer = SDL_CreateRenderer(window, -1, 0);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);

    sws_ctx = sws_getContext(width, height, pCodecCtx->pix_fmt, width, height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);

    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, width, height, 1);
    uint8_t *buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
    AVFrame *pFrameYUV = av_frame_alloc();
    av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, buffer, AV_PIX_FMT_YUV420P, width, height, 1);

    while (av_read_frame(pFormatCtx, &packet) >= 0) {
        if (packet.stream_index == videoStream) {
            if (avcodec_send_packet(pCodecCtx, &packet) == 0) {
                while (avcodec_receive_frame(pCodecCtx, pFrame) == 0) {
                    sws_scale(sws_ctx, (const uint8_t * const *)pFrame->data, pFrame->linesize, 0, height, pFrameYUV->data, pFrameYUV->linesize);
                    SDL_UpdateYUVTexture(texture, NULL, pFrameYUV->data[0], pFrameYUV->linesize[0], pFrameYUV->data[1], pFrameYUV->linesize[1], pFrameYUV->data[2], pFrameYUV->linesize[2]);
                    SDL_RenderClear(renderer);
                    SDL_RenderCopy(renderer, texture, NULL, NULL);
                    SDL_RenderPresent(renderer);
                }
            }
            av_packet_unref(&packet);
        } else if (packet.stream_index == audioStream) {
            packet_queue_put(&audioq, &packet);
            av_packet_unref(&packet);
        } else {
            av_packet_unref(&packet);
        }
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = 1;
                SDL_Quit();
                exit(0);
            }
        }
    }

    av_frame_free(&pFrame);
    av_frame_free(&pFrameYUV);
    av_free(buffer);
    avcodec_free_context(&pCodecCtx);
    avcodec_free_context(&aCodecCtx);
    avformat_close_input(&pFormatCtx);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
