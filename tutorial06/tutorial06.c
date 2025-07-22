// tutorial06.c
// A pedagogical video player that really works!
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard, 
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101, SDL 1.2.15
// on GCC 4.7.2 in Debian February 2015
// Use
//
// gcc -o tutorial05 tutorial05.c -lavformat -lavcodec -lswscale -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed, 
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial06 myvideofile.mpg
//
// to play the video stream on your screen.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>

#include <SDL2/SDL.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <string.h>

#define SDL_AUDIO_BUFFER_SIZE 2048  // Increased for better batching
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (16 * 16 * 1024)  // 4x increase for better buffering
#define MAX_VIDEOQ_SIZE (16 * 256 * 1024) // 4x increase for better buffering

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 16  // Increased for better buffering
#define PACKET_QUEUE_CAPACITY 2048   // 32x increase for better throughput

#define DEFAULT_AV_SYNC_TYPE AV_SYNC_VIDEO_MASTER

typedef struct PacketQueue {
  AVPacket *packets;
  int capacity;
  int size;
  int front;
  int rear;
  SDL_mutex *mutex;
  SDL_cond *cond;
} PacketQueue;


typedef struct VideoPicture {
  SDL_Texture *texture;
  int width, height; /* source height & width */
  int allocated;
  double pts;
} VideoPicture;

typedef struct VideoState {

  AVFormatContext *pFormatCtx;
  int             videoStream, audioStream;

  int             av_sync_type;
  double          external_clock; /* external clock base */
  int64_t         external_clock_time;

  double          audio_clock;
  AVStream        *audio_st;
  AVCodecContext  *audio_ctx;
  PacketQueue     audioq;
  uint8_t         audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
  unsigned int    audio_buf_size;
  unsigned int    audio_buf_index;
  AVFrame         *audio_frame;
  AVPacket        audio_pkt;
  uint8_t         *audio_pkt_data;
  int             audio_pkt_size;
  int             audio_pkt_in_use;
  int             audio_hw_buf_size;  
  double          audio_diff_cum; /* used for AV difference average computation */
  double          audio_diff_avg_coef;
  double          audio_diff_threshold;
  int             audio_diff_avg_count;
  
  double          frame_timer;
  double          frame_last_pts;
  double          frame_last_delay;
  double          video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame
  double          video_current_pts; ///<current displayed pts (different from video_clock if frame fifos are used)
  int64_t         video_current_pts_time;  ///<time (av_gettime) at which we updated video_current_pts - used to have running video pts

  AVStream        *video_st;
  AVCodecContext  *video_ctx;
  PacketQueue     videoq;
  struct SwsContext *sws_ctx;
  struct SwrContext *swr_ctx;

  VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
  int             pictq_size, pictq_rindex, pictq_windex;
  SDL_mutex       *pictq_mutex;
  SDL_cond        *pictq_cond;
  
  SDL_Thread      *parse_tid;
  SDL_Thread      *video_tid;

  char            filename[1024];
  int             quit;
  int             eof_reached;
} VideoState;

enum {
  AV_SYNC_AUDIO_MASTER,
  AV_SYNC_VIDEO_MASTER,
  AV_SYNC_EXTERNAL_MASTER,
};

SDL_Window      *window;
SDL_Renderer    *renderer;
SDL_mutex       *screen_mutex;

/* Since we only have one decoding thread, the Big Struct
   can be global in case we need it. */
VideoState *global_video_state;

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
    if (global_video_state->quit) {
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

double get_audio_clock(VideoState *is) {
  double pts;
  int hw_buf_size, bytes_per_sec, n;
  
  pts = is->audio_clock; /* maintained in the audio thread */
  hw_buf_size = is->audio_buf_size - is->audio_buf_index;
  bytes_per_sec = 0;
  n = is->audio_ctx->ch_layout.nb_channels * 2;
  if(is->audio_st) {
    bytes_per_sec = is->audio_ctx->sample_rate * n;
  }
  if(bytes_per_sec) {
    pts -= (double)hw_buf_size / bytes_per_sec;
  }
  return pts;
}

double get_video_clock(VideoState *is) {
  double delta;

  delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
  return is->video_current_pts + delta;
}

double get_external_clock(VideoState *is) {
  return av_gettime() / 1000000.0;
}

double get_master_clock(VideoState *is) {
  if(is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
    return get_video_clock(is);
  } else if(is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
    return get_audio_clock(is);
  } else {
    return get_external_clock(is);
  }
}

/* Add or subtract samples to get a better sync, return new
   audio buffer size */
int synchronize_audio(VideoState *is, short *samples,
		      int samples_size, double pts) {
  int n;
  double ref_clock;

  n = 2 * is->audio_ctx->ch_layout.nb_channels;
  
  if(is->av_sync_type != AV_SYNC_AUDIO_MASTER) {
    double diff, avg_diff;
    int wanted_size, min_size, max_size /*, nb_samples */;
    
    ref_clock = get_master_clock(is);
    diff = get_audio_clock(is) - ref_clock;

    if(diff < AV_NOSYNC_THRESHOLD) {
      // accumulate the diffs
      is->audio_diff_cum = diff + is->audio_diff_avg_coef
	* is->audio_diff_cum;
      if(is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
	is->audio_diff_avg_count++;
      } else {
	avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
	if(fabs(avg_diff) >= is->audio_diff_threshold) {
	  wanted_size = samples_size + ((int)(diff * is->audio_ctx->sample_rate) * n);
	  min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
	  max_size = samples_size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);
	  if(wanted_size < min_size) {
	    wanted_size = min_size;
	  } else if (wanted_size > max_size) {
	    wanted_size = max_size;
	  }
	  if(wanted_size < samples_size) {
	    /* remove samples */
	    samples_size = wanted_size;
	  } else if(wanted_size > samples_size) {
	    uint8_t *samples_end, *q;
	    int nb;

	    /* add samples by copying final sample*/
	    nb = (samples_size - wanted_size);
	    samples_end = (uint8_t *)samples + samples_size - n;
	    q = samples_end + n;
	    while(nb > 0) {
	      memcpy(q, samples_end, n);
	      q += n;
	      nb -= n;
	    }
	    samples_size = wanted_size;
	  }
	}
      }
    } else {
      /* difference is TOO big; reset diff stuff */
      is->audio_diff_avg_count = 0;
      is->audio_diff_cum = 0;
    }
  }
  return samples_size;
}

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size, double *pts_ptr) {
  AVPacket *pkt = &is->audio_pkt;
  int ret, data_size = 0;

  double pts;
  int n;

  for(;;) {
    if (!is->audio_pkt_in_use) {
      if (packet_queue_get(&is->audioq, pkt, 1) < 0) {
        return -1;
      }
      is->audio_pkt_in_use = 1;
    }

    ret = avcodec_send_packet(is->audio_ctx, pkt);
    if (ret < 0) {
      is->audio_pkt_in_use = 0;
      av_packet_unref(pkt);
      continue;
    }

    while (ret >= 0) {
      ret = avcodec_receive_frame(is->audio_ctx, is->audio_frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      } else if (ret < 0) {
        is->audio_pkt_in_use = 0;
        av_packet_unref(pkt);
        return -1;
      }

      // Resample if needed
      if (is->swr_ctx) {
        int dst_nb_samples = av_rescale_rnd(
          swr_get_delay(is->swr_ctx, is->audio_ctx->sample_rate) + is->audio_frame->nb_samples,
          is->audio_ctx->sample_rate, is->audio_ctx->sample_rate, AV_ROUND_UP);
        
        uint8_t *out_buf[2] = { audio_buf, NULL };
        int samples_converted = swr_convert(
          is->swr_ctx,
          out_buf,
          dst_nb_samples,
          (const uint8_t **)is->audio_frame->data,
          is->audio_frame->nb_samples);
        
        if (samples_converted < 0) {
          is->audio_pkt_in_use = 0;
          av_packet_unref(pkt);
          return -1;
        }
        
        data_size = samples_converted * is->audio_ctx->ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
        if (data_size > buf_size) data_size = buf_size;
      } else {
        // fallback: no resample
        data_size = av_samples_get_buffer_size(NULL, is->audio_ctx->ch_layout.nb_channels, 
                                             is->audio_frame->nb_samples, is->audio_ctx->sample_fmt, 1);
        if (data_size > buf_size) data_size = buf_size;
        memcpy(audio_buf, is->audio_frame->data[0], data_size);
      }
      
      is->audio_pkt_in_use = 0;
      av_packet_unref(pkt);

      pts = is->audio_clock;
      *pts_ptr = pts;
      n = 2 * is->audio_ctx->ch_layout.nb_channels;
      is->audio_clock += (double)data_size / (double)(n * is->audio_ctx->sample_rate);
      return data_size;
    }
    
    is->audio_pkt_in_use = 0;
    av_packet_unref(pkt);

    /* if update, update the audio clock w/pts */
    if(pkt->pts != AV_NOPTS_VALUE) {
      is->audio_clock = av_q2d(is->audio_st->time_base)*pkt->pts;
    }
  }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {

  VideoState *is = (VideoState *)userdata;
  int len1, audio_size;
  double pts;

  while(len > 0) {
    if(is->audio_buf_index >= is->audio_buf_size) {
      /* We have already sent all our data; get more */
      audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf), &pts);
      if(audio_size < 0) {
	/* If error, output silence */
	is->audio_buf_size = 1024;
	memset(is->audio_buf, 0, is->audio_buf_size);
      } else {
        audio_size = synchronize_audio(is, (int16_t *)is->audio_buf, audio_size, pts);
	is->audio_buf_size = audio_size;
      }
      is->audio_buf_index = 0;
    }
    len1 = is->audio_buf_size - is->audio_buf_index;
    if(len1 > len)
      len1 = len;
    memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
    len -= len1;
    stream += len1;
    is->audio_buf_index += len1;
  }
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
  SDL_Event event;
  event.type = FF_REFRESH_EVENT;
  event.user.data1 = opaque;
  SDL_PushEvent(&event);
  return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
static void schedule_refresh(VideoState *is, int delay) {
  SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState *is) {
  SDL_Rect rect;
  VideoPicture *vp;
  float aspect_ratio;
  int w, h, x, y;
  int window_w, window_h;

  vp = &is->pictq[is->pictq_rindex];
  if(vp->texture) {
    if(is->video_ctx->sample_aspect_ratio.num == 0) {
      aspect_ratio = 0;
    } else {
      aspect_ratio = av_q2d(is->video_ctx->sample_aspect_ratio) *
        is->video_ctx->width / is->video_ctx->height;
    }
    if(aspect_ratio <= 0.0) {
      aspect_ratio = (float)is->video_ctx->width /
        (float)is->video_ctx->height;
    }
    
    SDL_GetWindowSize(window, &window_w, &window_h);
    h = window_h;
    w = ((int)rint(h * aspect_ratio)) & -3;
    if(w > window_w) {
      w = window_w;
      h = ((int)rint(w / aspect_ratio)) & -3;
    }
    x = (window_w - w) / 2;
    y = (window_h - h) / 2;
    
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    
    SDL_LockMutex(screen_mutex);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, vp->texture, NULL, &rect);
    SDL_RenderPresent(renderer);
    SDL_UnlockMutex(screen_mutex);
  }
}

void video_refresh_timer(void *userdata) {
  VideoState *is = (VideoState *)userdata;
  VideoPicture *vp;
  double actual_delay, delay, sync_threshold, ref_clock, diff;
  
  if(!is) {
    return;
  }
  
  if(is->quit) {
    return;
  }
  
  if(is->video_st) {
    if(is->pictq_size == 0) {
      schedule_refresh(is, 1);  // Increased delay to prevent tight loop
    } else {
      vp = &is->pictq[is->pictq_rindex];

      is->video_current_pts = vp->pts;
      is->video_current_pts_time = av_gettime();

      delay = vp->pts - is->frame_last_pts; /* the pts from last time */
      if(delay <= 0 || delay >= 1.0) {
        /* if incorrect delay, use previous one */
        delay = is->frame_last_delay;
      }
      /* save for next time */
      is->frame_last_delay = delay;
      is->frame_last_pts = vp->pts;



      /* update delay to sync to audio if not master source */
      if(is->av_sync_type != AV_SYNC_VIDEO_MASTER) {
        /* update delay to sync to audio */
  	    ref_clock = get_master_clock(is);
        diff = vp->pts - ref_clock;

        /* Skip or repeat the frame. Take delay into account
          FFPlay still doesn't "know if this is the best guess." */
        sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
        if(fabs(diff) < AV_NOSYNC_THRESHOLD) {
          if(diff <= -sync_threshold) {
            delay = 0;
          } else if(diff >= sync_threshold) {
            delay = 2 * delay;
          }
        }
      }
      is->frame_timer += delay;
      /* computer the REAL delay */
      actual_delay = is->frame_timer - (av_gettime_relative() / 1000000.0);
      if(actual_delay < 0.010) {
        /* Really it should skip the picture instead */
        actual_delay = 0.010;
      }
      schedule_refresh(is, (int)(actual_delay * 1000 + 0.5));      
      
      /* show the picture! */
      video_display(is);
      
      /* update queue for next picture! */
      if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
        is->pictq_rindex = 0;
      }
      SDL_LockMutex(is->pictq_mutex);
      is->pictq_size--;
      SDL_CondSignal(is->pictq_cond);
      SDL_UnlockMutex(is->pictq_mutex);
    }
  } else {
    schedule_refresh(is, 100);
  }
}
      
void alloc_picture(void *userdata) {
  VideoState *is = (VideoState *)userdata;
  VideoPicture *vp;

  vp = &is->pictq[is->pictq_windex];
  if(vp->texture) {
    // we already have one make another, bigger/smaller
    SDL_DestroyTexture(vp->texture);
  }
  
  // Allocate a place to put our YUV image
  SDL_LockMutex(screen_mutex);
  vp->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, 
                                 SDL_TEXTUREACCESS_STREAMING,
                                 is->video_ctx->width,
                                 is->video_ctx->height);
  SDL_UnlockMutex(screen_mutex);

  vp->width = is->video_ctx->width;
  vp->height = is->video_ctx->height;
  vp->allocated = 1;
}

int queue_picture(VideoState *is, AVFrame *pFrame, double pts) {
  VideoPicture *vp;
  uint8_t *yPlane, *uPlane, *vPlane;
  int yPitch, uPitch, vPitch;

  /* wait until we have space for a new pic */
  SDL_LockMutex(is->pictq_mutex);
  while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit) {
    SDL_CondWait(is->pictq_cond, is->pictq_mutex);
  }
  SDL_UnlockMutex(is->pictq_mutex);

  if(is->quit)
    return -1;

  // windex is set to 0 initially
  vp = &is->pictq[is->pictq_windex];

  /* allocate or resize the buffer! */
  if(!vp->texture ||
     vp->width != is->video_ctx->width ||
     vp->height != is->video_ctx->height) {
    vp->allocated = 0;
    alloc_picture(is);
    if(is->quit) {
      return -1;
    }
  }

  /* We have a place to put our picture on the queue */
  if(vp->texture) {
    // Create a temporary frame for YUV420P conversion
    AVFrame *pFrameYUV = av_frame_alloc();

    vp->pts = pts;

    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, is->video_ctx->width, is->video_ctx->height, 1);
    uint8_t *buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
    av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, buffer, AV_PIX_FMT_YUV420P, is->video_ctx->width, is->video_ctx->height, 1);
    
    // Convert the image into YUV format that SDL uses
    sws_scale(is->sws_ctx, (uint8_t const * const *)pFrame->data,
              pFrame->linesize, 0, is->video_ctx->height,
              pFrameYUV->data, pFrameYUV->linesize);
    
    // Update SDL texture
    SDL_UpdateYUVTexture(vp->texture, NULL,
                        pFrameYUV->data[0], pFrameYUV->linesize[0],
                        pFrameYUV->data[1], pFrameYUV->linesize[1],
                        pFrameYUV->data[2], pFrameYUV->linesize[2]);
    
    av_frame_free(&pFrameYUV);
    av_free(buffer);
    
    /* now we inform our display thread that we have a pic ready */
    if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
      is->pictq_windex = 0;
    }
    SDL_LockMutex(is->pictq_mutex);
    is->pictq_size++;
    SDL_UnlockMutex(is->pictq_mutex);
  }
  return 0;
}

double synchronize_video(VideoState *is, AVFrame *src_frame, double pts) {
  double frame_delay;

  if(pts != 0) {
    /* if we have pts, set video clock to it */
    is->video_clock = pts;
  } else {
    /* if we aren't given a pts, set it to the clock */
    pts = is->video_clock;
  }
  
  /* update the video clock */
  frame_delay = av_q2d(is->video_ctx->time_base);
  /* if we are repeating a frame, adjust clock accordingly */
  frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
  is->video_clock += frame_delay;
  
  return pts;
}

int video_thread(void *arg) {
  VideoState *is = (VideoState *)arg;
  AVPacket pkt1, *packet = &pkt1;
  AVFrame *pFrame;
  int ret;
  double pts;

  pFrame = av_frame_alloc();

  for(;;) {
    if(packet_queue_get(&is->videoq, packet, 1) < 0) {
      // means we quit getting packets
      break;
    }

    // Send packet to decoder
    ret = avcodec_send_packet(is->video_ctx, packet);
    if (ret < 0) {
      fprintf(stderr, "Error sending packet to video decoder: %d\n", ret);
      av_packet_unref(packet);
      continue;
    }
    
    // Receive frames from decoder
    while (ret >= 0) {
      ret = avcodec_receive_frame(is->video_ctx, pFrame);

      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      } else if (ret < 0) {
        fprintf(stderr, "Error receiving frame from video decoder: %d\n", ret);
        break;
      }

      // Get PTS from the frame
      pts = 0;
      if(pFrame->best_effort_timestamp != AV_NOPTS_VALUE) {
        pts = pFrame->best_effort_timestamp;
      } else if(pFrame->pts != AV_NOPTS_VALUE) {
        pts = pFrame->pts;
      } else if(packet->pts != AV_NOPTS_VALUE) {
        pts = packet->pts;
      }
      pts *= av_q2d(is->video_st->time_base);

      pts = synchronize_video(is, pFrame, pts);
      
      // We got a video frame
      if(queue_picture(is, pFrame, pts) < 0) {
        av_packet_unref(packet);
        av_frame_free(&pFrame);
        return 0;
      }
    }
    
    av_packet_unref(packet);
  }
  
  // Send flush packet to get remaining frames
  avcodec_send_packet(is->video_ctx, NULL);
  while (avcodec_receive_frame(is->video_ctx, pFrame) == 0) {
    pts = 0;
    if(pFrame->best_effort_timestamp != AV_NOPTS_VALUE) {
      pts = pFrame->best_effort_timestamp;
    } else if(pFrame->pts != AV_NOPTS_VALUE) {
      pts = pFrame->pts;
    }
    pts *= av_q2d(is->video_st->time_base);
    pts = synchronize_video(is, pFrame, pts);
    
    if(queue_picture(is, pFrame, pts) < 0) {
      break;
    }
  }
  
  av_frame_free(&pFrame);
  return 0;
}

int stream_component_open(VideoState *is, int stream_index) {
  AVFormatContext *pFormatCtx = is->pFormatCtx;
  AVCodecContext *codecCtx = NULL;
  const AVCodec *codec = NULL;
  SDL_AudioSpec wanted_spec, spec;
  AVCodecParameters *codecpar;

  if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
    return -1;
  }

  codecpar = pFormatCtx->streams[stream_index]->codecpar;
  codec = avcodec_find_decoder(codecpar->codec_id);
  if(!codec) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }

  codecCtx = avcodec_alloc_context3(codec);
  if(avcodec_parameters_to_context(codecCtx, codecpar) != 0) {
    fprintf(stderr, "Couldn't copy codec context");
    return -1;
  }

  // Set decoder options for better H.264 handling
  if(codecCtx->codec_type == AVMEDIA_TYPE_VIDEO && codecpar->codec_id == AV_CODEC_ID_H264) {
    // Enable error concealment for missing reference frames
    codecCtx->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
    // Set thread count for better decoding
    codecCtx->thread_count = 0; // Auto-detect thread count
    codecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  }

  if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
    // Setup audio resampler
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, codecCtx->ch_layout.nb_channels);
    
    is->swr_ctx = swr_alloc();
    if (!is->swr_ctx) {
      fprintf(stderr, "Could not allocate resampler!\n");
      return -1;
    }
    
    if (swr_alloc_set_opts2(
          &is->swr_ctx,
          &out_ch_layout,
          AV_SAMPLE_FMT_S16,
          codecCtx->sample_rate,
          &codecCtx->ch_layout,
          codecCtx->sample_fmt,
          codecCtx->sample_rate,
          0, NULL) < 0) {
      fprintf(stderr, "Could not set resampler options!\n");
      return -1;
    }
    
    if (swr_init(is->swr_ctx) < 0) {
      fprintf(stderr, "Could not initialize resampler!\n");
      return -1;
    }
    
    // Set audio settings from codec info
    wanted_spec.freq = codecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = codecCtx->ch_layout.nb_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = is;
    
    if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
      fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
      return -1;
    }
    is->audio_hw_buf_size = spec.size;
    
    av_channel_layout_uninit(&out_ch_layout);
  }
  
  if(avcodec_open2(codecCtx, codec, NULL) < 0) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }

  switch(codecCtx->codec_type) {
  case AVMEDIA_TYPE_AUDIO:
    is->audioStream = stream_index;
    is->audio_st = pFormatCtx->streams[stream_index];
    is->audio_ctx = codecCtx;
    is->audio_buf_size = 0;
    is->audio_buf_index = 0;
    is->audio_pkt_in_use = 0;
    is->audio_frame = av_frame_alloc();
    memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
    packet_queue_init(&is->audioq);
    SDL_PauseAudio(0);
    break;
  case AVMEDIA_TYPE_VIDEO:
    is->videoStream = stream_index;
    is->video_st = pFormatCtx->streams[stream_index];
    is->video_ctx = codecCtx;

    is->frame_timer = (double)av_gettime_relative() / 1000000.0;
    is->frame_last_delay = 40e-3;
    is->video_current_pts_time = av_gettime();

    packet_queue_init(&is->videoq);
    is->video_tid = SDL_CreateThread(video_thread, "video_thread", is);
    is->sws_ctx = sws_getContext(is->video_ctx->width, is->video_ctx->height,
                                is->video_ctx->pix_fmt, is->video_ctx->width,
                                is->video_ctx->height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, NULL, NULL, NULL);
    break;
  default:
    break;
  }
  return 0;
}

int decode_thread(void *arg) {
  VideoState *is = (VideoState *)arg;
  AVFormatContext *pFormatCtx;
  AVPacket pkt1, *packet = &pkt1;

  int video_index = -1;
  int audio_index = -1;
  int i;

  is->videoStream = -1;
  is->audioStream = -1;

  global_video_state = is;

  // Open video file
  pFormatCtx = NULL;
  if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0) {
    return -1; // Couldn't open file
  }

  is->pFormatCtx = pFormatCtx;
  
  // Retrieve stream information
  if(avformat_find_stream_info(pFormatCtx, NULL) < 0) {
    return -1; // Couldn't find stream information
  }
  
  // Dump information about file onto standard error
  av_dump_format(pFormatCtx, 0, is->filename, 0);
  
  // Find the first video and audio streams
  for(i = 0; i < pFormatCtx->nb_streams; i++) {
    AVCodecParameters *codecpar = pFormatCtx->streams[i]->codecpar;
    if(codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_index < 0) {
      video_index = i;
    }
    if(codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_index < 0) {
      audio_index = i;
    }
  }
  
  if(audio_index >= 0) {
    stream_component_open(is, audio_index);
  }
  if(video_index >= 0) {
    stream_component_open(is, video_index);
  }   

  if(is->videoStream < 0 || is->audioStream < 0) {
    fprintf(stderr, "%s: could not open codecs\n", is->filename);
    goto fail;
  }

  // main decode loop
  for(;;) {
    if(is->quit) {
      break;
    }
    // seek stuff goes here
    if(is->audioq.size > MAX_AUDIOQ_SIZE ||
       is->videoq.size > MAX_VIDEOQ_SIZE) {
      SDL_Delay(10);
      continue;
    }
    if(av_read_frame(is->pFormatCtx, packet) < 0) {
      if(is->pFormatCtx->pb->error == 0) {
        // End of file reached, set EOF flag and signal queues
        is->eof_reached = 1;
        // printf( "[DECODE] EOF reached, signaling queues\n");
        
        // SDL_LockMutex(is->audioq.mutex);
        // SDL_CondSignal(is->audioq.cond);
        // SDL_UnlockMutex(is->audioq.mutex);
        
        // SDL_LockMutex(is->videoq.mutex);
        // SDL_CondSignal(is->videoq.cond);
        // SDL_UnlockMutex(is->videoq.mutex);
        
        SDL_Delay(100);
        continue;
        break;
      } else {
        break;
      }
    }
    // Is this a packet from the video stream?
    if(packet->stream_index == is->videoStream) {
      packet_queue_put(&is->videoq, packet);
    } else if(packet->stream_index == is->audioStream) {
      packet_queue_put(&is->audioq, packet);
    } else {
      av_packet_unref(packet);
    }
  }
  
  /* all done - wait for it */
  while(!is->quit) {
    SDL_Delay(100);
  }

 fail:
  if(1){
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
  }
  return 0;
}

int main(int argc, char *argv[]) {
  SDL_Event event;
  VideoState *is;

  if(argc < 2) {
    fprintf(stderr, "Usage: %s <file>\n", argv[0]);
    exit(1);
  }
  
  is = av_mallocz(sizeof(VideoState));
  if (!is) {
    printf("[MAIN] ERROR: Failed to allocate VideoState\n");
    fflush(stdout);
    exit(1);
  }
  
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
    exit(1);
  }

  // Create window and renderer
  window = SDL_CreateWindow("FFmpeg Tutorial04", 
                           SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                           640, 480, 0);
  if(!window) {
    fprintf(stderr, "SDL: could not create window - exiting\n");
    exit(1);
  }

  renderer = SDL_CreateRenderer(window, -1, 0);
  if(!renderer) {
    fprintf(stderr, "SDL: could not create renderer - exiting\n");
    exit(1);
  }

  screen_mutex = SDL_CreateMutex();

  strncpy(is->filename, argv[1], sizeof(is->filename) - 1);
  is->filename[sizeof(is->filename) - 1] = '\0';

  is->pictq_mutex = SDL_CreateMutex();
  is->pictq_cond = SDL_CreateCond();

  schedule_refresh(is, 20);
  is->av_sync_type = DEFAULT_AV_SYNC_TYPE;

  is->parse_tid = SDL_CreateThread(decode_thread, "decode_thread", is);
  if(!is->parse_tid) {
    printf("[MAIN] ERROR: Failed to create decode thread\n");
    av_free(is);
    return -1;
  }
  
  for(;;) {
    SDL_WaitEvent(&event);
    switch(event.type) {
    case FF_QUIT_EVENT:
    case SDL_QUIT:
      is->quit = 1;
      SDL_Quit();
      return 0;
      break;
    case FF_REFRESH_EVENT:
      video_refresh_timer(event.user.data1);
      break;
    default:
      break;
    }
  }
  return 0;
}
