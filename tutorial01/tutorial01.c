// tutorial01.c
// Code based on a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101 
// on GCC 4.7.2 in Debian February 2015

// A small sample program that shows how to use libavformat and libavcodec to
// read video from a file.
//
// Use
//
// gcc -o tutorial01 tutorial01.c -lavformat -lavcodec -lswscale -lz
//
// to build (assuming libavformat and libavcodec are correctly installed
// your system).
//
// Run using
//
// tutorial01 myvideofile.mpg
//
// to write the first five frames from "myvideofile.mpg" to disk in PPM
// format.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#include <stdio.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame) {
  FILE *pFile;
  char szFilename[32];
  int  y;
  
  // Open file
  sprintf(szFilename, "frame%d.ppm", iFrame);
  pFile=fopen(szFilename, "wb");
  if(pFile==NULL)
    return;
  
  // Write header
  fprintf(pFile, "P6\n%d %d\n255\n", width, height);
  
  // Write pixel data
  for(y=0; y<height; y++)
    fwrite(pFrame->data[0]+y*pFrame->linesize[0], 1, width*3, pFile);
  
  // Close file
  fclose(pFile);
}

int main(int argc, char *argv[]) {
  // Initalizing these to NULL prevents segfaults!
  AVFormatContext   *pFormatCtx = NULL;
  int               i, videoStream;
  AVCodecContext    *pCodecCtxOrig = NULL;
  AVCodecContext    *pCodecCtx = NULL;
  AVCodec           *pCodec = NULL;
  AVFrame           *pFrame = NULL;
  AVFrame           *pFrameRGB = NULL;
  AVPacket          packet;
  int               frameFinished;
  int               numBytes;
  uint8_t           *buffer = NULL;
  struct SwsContext *sws_ctx = NULL;

  if(argc < 2) {
    printf("Please provide a movie file\n");
    return -1;
  }
  // Register all formats and codecs (not needed in FFmpeg >= 4.0)
  
  // Open video file
  if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL)!=0)
    return -1; // Couldn't open file
  
  // Retrieve stream information
  if(avformat_find_stream_info(pFormatCtx, NULL)<0)
    return -1; // Couldn't find stream information
  
  // Dump information about file onto standard error
  av_dump_format(pFormatCtx, 0, argv[1], 0);
  
  // Find the first video stream
  videoStream = -1;
  for (i = 0; i < pFormatCtx->nb_streams; i++) {
    if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      videoStream = i;
      break;
    }
  }
  if (videoStream == -1)
    return -1; // Didn't find a video stream

  // Find the decoder for the video stream
  AVCodecParameters *codecpar = pFormatCtx->streams[videoStream]->codecpar;
  pCodec = avcodec_find_decoder(codecpar->codec_id);
  if (pCodec == NULL) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1; // Codec not found
  }
  // Allocate a codec context for the decoder
  pCodecCtx = avcodec_alloc_context3(pCodec);
  if (!pCodecCtx) {
    fprintf(stderr, "Could not allocate video codec context\n");
    return -1;
  }
  // Copy codec parameters from input stream to output codec context
  if (avcodec_parameters_to_context(pCodecCtx, codecpar) < 0) {
    fprintf(stderr, "Failed to copy codec parameters to decoder context\n");
    return -1;
  }

  // Open codec
  if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
    return -1; // Could not open codec

  // Allocate video frame
  pFrame = av_frame_alloc();

  // Allocate an AVFrame structure for RGB
  pFrameRGB = av_frame_alloc();
  if (pFrameRGB == NULL)
    return -1;

  // Determine required buffer size and allocate buffer
  numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width,
                                      pCodecCtx->height, 1);
  buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));

  // Assign appropriate parts of buffer to image planes in pFrameRGB
  av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_RGB24,
                      pCodecCtx->width, pCodecCtx->height, 1);

  // initialize SWS context for software scaling
  sws_ctx = sws_getContext(pCodecCtx->width,
                           pCodecCtx->height,
                           pCodecCtx->pix_fmt,
                           pCodecCtx->width,
                           pCodecCtx->height,
                           AV_PIX_FMT_RGB24,
                           SWS_BILINEAR,
                           NULL,
                           NULL,
                           NULL);

  // Read frames and save first five frames to disk
  i = 0;
  while (av_read_frame(pFormatCtx, &packet) >= 0) {
    // Is this a packet from the video stream?
    if (packet.stream_index == videoStream) {
      // Send the packet to the decoder
      if (avcodec_send_packet(pCodecCtx, &packet) == 0) {
        // Read all the output frames (in general there may be more than one)
        while (avcodec_receive_frame(pCodecCtx, pFrame) == 0) {
          // Convert the image from its native format to RGB
          sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data,
                    pFrame->linesize, 0, pCodecCtx->height,
                    pFrameRGB->data, pFrameRGB->linesize);

          // Save the frame to disk
          if (++i <= 5)
            SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i);
        }
      }
    }
    // Free the packet that was allocated by av_read_frame
    av_packet_unref(&packet);
  }
  
  // Free the RGB image
  av_free(buffer);
  av_frame_free(&pFrameRGB);

  // Free the YUV frame
  av_frame_free(&pFrame);

  // Close the codec context
  avcodec_free_context(&pCodecCtx);

  // Close the video file
  avformat_close_input(&pFormatCtx);

  return 0;
}
