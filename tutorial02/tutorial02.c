// tutorial02.c
// A pedagogical video player that will stream through every video frame as fast as it can.
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
// gcc -o tutorial02 tutorial02.c -lavformat -lavcodec -lswscale -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed, 
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial02 myvideofile.mpg
//
// to play the video stream on your screen.


#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#include <SDL2/SDL.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif


int main(int argc, char *argv[]) {
    AVFormatContext *pFormatCtx = NULL;
    int videoStream = -1;
    AVCodecContext *pCodecCtx = NULL;
    AVCodecParameters *pCodecPar = NULL;
    const AVCodec *pCodec = NULL;
    AVFrame *pFrame = NULL;
    AVFrame *pFrameYUV = NULL;
    AVPacket *packet = NULL;
    struct SwsContext *sws_ctx = NULL;

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    SDL_Event event;
    int quit = 0;


    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        exit(1);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0) {
        fprintf(stderr, "Couldn't open file\n");
        return -1;
    }

    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        fprintf(stderr, "Couldn't find stream information\n");
        return -1;
    }

    av_dump_format(pFormatCtx, 0, argv[1], 0);

    // Find the first video stream
    for (unsigned int i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            break;
        }
    }
    if (videoStream == -1) {
        fprintf(stderr, "Didn't find a video stream\n");
        return -1;
    }

    pCodecPar = pFormatCtx->streams[videoStream]->codecpar;
    pCodec = avcodec_find_decoder(pCodecPar->codec_id);
    if (!pCodec) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx) {
        fprintf(stderr, "Could not allocate codec context\n");
        return -1;
    }
    if (avcodec_parameters_to_context(pCodecCtx, pCodecPar) < 0) {
        fprintf(stderr, "Couldn't copy codec parameters to context\n");
        return -1;
    }
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return -1;
    }

    pFrame = av_frame_alloc();
    pFrameYUV = av_frame_alloc();
    if (!pFrame || !pFrameYUV) {
        fprintf(stderr, "Could not allocate video frame\n");
        return -1;
    }
    packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        return -1;
    }

    int width = pCodecCtx->width;
    int height = pCodecCtx->height;

    // Allocate buffer for YUV frame
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, width, height, 1);
    uint8_t *buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
    av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, buffer, AV_PIX_FMT_YUV420P, width, height, 1);

    // Set up SWS context for conversion
    sws_ctx = sws_getContext(width, height, pCodecCtx->pix_fmt,
                             width, height, AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR, NULL, NULL, NULL);

    // SDL2: Create window, renderer, and texture
    window = SDL_CreateWindow("FFmpeg Tutorial02", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, 0);
    if (!window) {
        fprintf(stderr, "SDL: could not create window - exiting\n");
        exit(1);
    }
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        fprintf(stderr, "SDL: could not create renderer - exiting\n");
        exit(1);
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!texture) {
        fprintf(stderr, "SDL: could not create texture - exiting\n");
        exit(1);
    }

    // Read frames and display
    while (!quit && av_read_frame(pFormatCtx, packet) >= 0) {
        if (packet->stream_index == videoStream) {
            if (avcodec_send_packet(pCodecCtx, packet) == 0) {
                while (avcodec_receive_frame(pCodecCtx, pFrame) == 0) {
                    // Convert to YUV420P for SDL2
                    sws_scale(sws_ctx, (const uint8_t * const *)pFrame->data, pFrame->linesize, 0, height, pFrameYUV->data, pFrameYUV->linesize);
                    SDL_UpdateYUVTexture(texture, NULL,
                        pFrameYUV->data[0], pFrameYUV->linesize[0],
                        pFrameYUV->data[1], pFrameYUV->linesize[1],
                        pFrameYUV->data[2], pFrameYUV->linesize[2]);
                    SDL_RenderClear(renderer);
                    SDL_RenderCopy(renderer, texture, NULL, NULL);
                    SDL_RenderPresent(renderer);
                }
            }
        }
        av_packet_unref(packet);
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = 1;
                break;
            }
        }
    }

    // Cleanup
    av_frame_free(&pFrame);
    av_frame_free(&pFrameYUV);
    av_packet_free(&packet);
    avcodec_free_context(&pCodecCtx);
    avformat_close_input(&pFormatCtx);
    av_free(buffer);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
