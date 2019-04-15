/**************************************************************************
 * Copyright (C) 2017-2017  Unisound
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 **************************************************************************
 *
 * Description : uni_mp3_player.c
 * Author      : yzs@unisound.com
 * Date        : 2018.06.19
 *
 **************************************************************************/
#include "uni_mp3_player.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include "uni_log.h"
#include <pthread.h>
#include <unistd.h>

#define MP3_PLAYER_TAG               "mp3_player"
#define AUDIO_OUT_SIZE               (8 * 1024)
#define OPEN_INPUT_TIMEOUT_S         (30)
#define READ_HEADER_TIMEOUT_S        (4)
#define READ_FRAME_TIMEOUT_S         (5)
#define AUDIO_RETRIEVE_DATA_FINISHED (-1)

typedef enum {
  MP3_IDLE_STATE = 0,
  MP3_PREPARING_STATE,
  MP3_PREPARED_STATE,
  MP3_PAUSED_STATE,
  MP3_PLAYING_STATE
} Mp3State;

typedef enum {
  MP3_PLAY_EVENT,
  MP3_PREPARE_EVENT,
  MP3_START_EVENT,
  MP3_PAUSE_EVENT,
  MP3_RESUME_EVENT,
  MP3_STOP_EVENT
} Mp3Event;

typedef enum {
  BLOCK_NULL = 0,
  BLOCK_OPEN_INPUT,
  BLOCK_READ_HEADER,
  BLOCK_READ_FRAME,
} BlockState;

typedef struct _ConvertCtxNode{
  int                    channel_layout;
  enum AVSampleFormat    sample_fmt;
  int                    sample_rate;
  struct SwrContext      *au_convert_ctx;
  struct _ConvertCtxNode *next;
} ConvertCtxNode;

static struct {
  AVFormatContext     *fmt_ctx;
  AVCodecContext      *audio_dec_ctx;
  struct SwrContext   *au_convert_ctx;
  ConvertCtxNode      *convert_ctx_list;
  AVPacket            pkt;
  AVPacket            orig_pkt;
  AVFrame             *frame;
  int                 audio_stream_idx;
  long long           out_channel_layout;
  int                 out_sample_rate;
  enum AVSampleFormat out_sample_fmt;
  int                 out_channels;
  uint8_t             *out_buffer;
  Mp3State            state;
  pthread_t           prepare_thread;
  int                 last_timestamp;
  int                 block_state;
} g_mp3_player;

static const char* _block_state_2_string(BlockState state) {
  static const char* block_state[] = {
    [BLOCK_NULL]        = "BLOCK_NULL",
    [BLOCK_OPEN_INPUT]  = "BLOCK_OPEN_INPUT",
    [BLOCK_READ_HEADER] = "BLOCK_READ_HEADER",
    [BLOCK_READ_FRAME]  = "BLOCK_READ_FRAME",
  };
  return block_state[state];
}

static int interrupt_cb(void *ctx) {
  int seconds;
  int timeout;
  seconds = time((time_t *)NULL);
  LOGD(MP3_PLAYER_TAG, "%s", _block_state_2_string(g_mp3_player.block_state));
  switch (g_mp3_player.block_state) {
    case BLOCK_NULL:
      return 0;
    case BLOCK_OPEN_INPUT:
      timeout = OPEN_INPUT_TIMEOUT_S;
      break;
    case BLOCK_READ_HEADER:
      timeout = READ_HEADER_TIMEOUT_S;
      break;
    case BLOCK_READ_FRAME:
      timeout = READ_FRAME_TIMEOUT_S;
      break;
    default:
      LOGW(MP3_PLAYER_TAG, "invalid block state [%d]!!!!!!",
           g_mp3_player.block_state);
      return 0;
  }
  if (seconds - g_mp3_player.last_timestamp >= timeout) {
    LOGE(MP3_PLAYER_TAG, "ffmpeg hit timeout at state [%d, %s]!!!",
         g_mp3_player.block_state,
         _block_state_2_string(g_mp3_player.block_state));
    return 1;
  }
  return 0;
}

static const AVIOInterruptCB int_cb = {interrupt_cb, NULL};

static char* _event2string(Mp3Event event) {
  switch (event) {
  case MP3_PLAY_EVENT:
    return "MP3_PLAY_EVENT";
  case MP3_PREPARE_EVENT:
    return "MP3_PREPARE_EVENT";
  case MP3_START_EVENT:
    return "MP3_START_EVENT";
  case MP3_PAUSE_EVENT:
    return "MP3_PAUSE_EVENT";
  case MP3_RESUME_EVENT:
    return "MP3_RESUME_EVENT";
  case MP3_STOP_EVENT:
    return "MP3_STOP_EVENT";
  default:
    break;
  }
  return "N/A";
}

static char* _state2string(Mp3State state) {
  switch (state) {
    case MP3_IDLE_STATE:
      return "MP3_IDLE_STATE";
    case MP3_PREPARING_STATE:
      return "MP3_PREPARING_STATE";
    case MP3_PREPARED_STATE:
      return "MP3_PREPARED_STATE";
    case MP3_PAUSED_STATE:
      return "MP3_PAUSED_STATE";
    case MP3_PLAYING_STATE:
      return "MP3_PLAYING_STATE";
    default:
      break;
  }
  return "N/A";
}

static void _mp3_set_state(Mp3State state) {
  g_mp3_player.state = state;
  LOGT(MP3_PLAYER_TAG, "mp3 state is set to %d", state);
}

static ConvertCtxNode* _create_convert_ctx_node(int channel_layout,
                                                enum AVSampleFormat sample_fmt,
                                                int sample_rate) {
  ConvertCtxNode *node, *head = g_mp3_player.convert_ctx_list;
  node = malloc(sizeof(ConvertCtxNode));
  node->channel_layout = channel_layout;
  node->sample_fmt = sample_fmt;
  node->sample_rate = sample_rate;
  node->au_convert_ctx = swr_alloc_set_opts(NULL,
                                            g_mp3_player.out_channel_layout,
                                            g_mp3_player.out_sample_fmt,
                                            g_mp3_player.out_sample_rate,
                                            node->channel_layout,
                                            node->sample_fmt,
                                            node->sample_rate,
                                            0,
                                            NULL);
  node->next = NULL;
  swr_init(node->au_convert_ctx);
  if (NULL == head) {
    g_mp3_player.convert_ctx_list = node;
    return node;
  }
  while (NULL != head->next) {
    head = head->next;
  }
  head->next = node;
  return node;
}

static void _destroy_convert_ctx_node(ConvertCtxNode *node) {
  if (NULL != node) {
    swr_free(&node->au_convert_ctx);
    free(node);
  }
}

static void _choose_au_convert_ctx(int channel_layout,
                                   enum AVSampleFormat sample_fmt,
                                   int sample_rate) {
  ConvertCtxNode *node = g_mp3_player.convert_ctx_list;
  while (NULL != node) {
    if (node->channel_layout == channel_layout &&
        node->sample_fmt == sample_fmt &&
        node->sample_rate == sample_rate) {
      g_mp3_player.au_convert_ctx = node->au_convert_ctx;
      LOGW(MP3_PLAYER_TAG, "channel_layout=%d, sample_fmt=%d,"
           "sample_rate=%d", node->channel_layout, node->sample_fmt,
           node->sample_rate);
      return;
    }
    node = node->next;
  }
  node = _create_convert_ctx_node(channel_layout, sample_fmt, sample_rate);
  g_mp3_player.au_convert_ctx = node->au_convert_ctx;
  return;
}

static int _open_codec_context(int *stream_idx,
                               AVCodecContext **dec_ctx,
                               AVFormatContext *fmt_ctx,
                               enum AVMediaType type) {
  int ret, stream_index;
  AVStream *st;
  AVCodec *dec = NULL;
  AVDictionary *opts = NULL;
  ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
  if (ret < 0) {
    LOGE(MP3_PLAYER_TAG, "Could not find %s stream in input file",
         av_get_media_type_string(type));
    return ret;
  }
  stream_index = ret;
  st = fmt_ctx->streams[stream_index];
  dec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!dec) {
    LOGE(MP3_PLAYER_TAG, "Failed to find %s codec",
         av_get_media_type_string(type));
    return AVERROR(EINVAL);
  }
  *dec_ctx = avcodec_alloc_context3(dec);
  if (!*dec_ctx) {
    LOGE(MP3_PLAYER_TAG, "Failed to allocate the %s codec context",
         av_get_media_type_string(type));
    return AVERROR(ENOMEM);
  }
  if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
    LOGE(MP3_PLAYER_TAG, "Failed to copy %s codec parameter to decoder context",
         av_get_media_type_string(type));
    return ret;
  }
  LOGT(MP3_PLAYER_TAG, "before avcodec_open2");
  if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
    LOGE(MP3_PLAYER_TAG, "Failed to open %s codec",
         av_get_media_type_string(type));
    return ret;
  }
  *stream_idx = stream_index;
  return 0;
}

static void _set_block_state(BlockState state) {
  g_mp3_player.last_timestamp = time(NULL);
  g_mp3_player.block_state = state;
}

static int _mp3_prepare_internal(const char *url) {
  av_register_all();
  avformat_network_init();
  g_mp3_player.fmt_ctx = avformat_alloc_context();
  if (NULL == g_mp3_player.fmt_ctx) {
    LOGE(MP3_PLAYER_TAG, "Could not alloc context");
    return -1;
  }
  g_mp3_player.fmt_ctx->interrupt_callback = int_cb;
  _set_block_state(BLOCK_OPEN_INPUT);
  LOGT(MP3_PLAYER_TAG, "before avformat_open_input");
  if (avformat_open_input(&g_mp3_player.fmt_ctx, url, NULL, NULL) < 0) {
    LOGE(MP3_PLAYER_TAG, "Could not open source file %s", url);
    return -1;
  }
  _set_block_state(BLOCK_READ_HEADER);
  LOGT(MP3_PLAYER_TAG, "before avformat_find_stream_info");
  if (avformat_find_stream_info(g_mp3_player.fmt_ctx, NULL) < 0) {
    LOGE(MP3_PLAYER_TAG, "Could not find stream information");
    return -1;
  }
  LOGT(MP3_PLAYER_TAG, "before _open_codec_context");
  if (_open_codec_context(&g_mp3_player.audio_stream_idx,
                         &g_mp3_player.audio_dec_ctx,
                         g_mp3_player.fmt_ctx, AVMEDIA_TYPE_AUDIO) < 0) {
    LOGE(MP3_PLAYER_TAG, "Open codec context failed");
    return -1;
  }
  if (NULL == g_mp3_player.fmt_ctx->streams[g_mp3_player.audio_stream_idx]) {
    LOGE(MP3_PLAYER_TAG, "Could not find audio stream");
    return -1;
  }
  _set_block_state(BLOCK_NULL);
  LOGT(MP3_PLAYER_TAG, "before av_dump_format");
  av_dump_format(g_mp3_player.fmt_ctx, 0, url, 0);
  if (NULL == (g_mp3_player.frame = av_frame_alloc())) {
    LOGE(MP3_PLAYER_TAG, "Could not allocate frame");
    return -1;
  }
  av_init_packet(&g_mp3_player.pkt);
  g_mp3_player.pkt.data = NULL;
  g_mp3_player.pkt.size = 0;
  g_mp3_player.out_buffer = (uint8_t *)av_malloc(AUDIO_OUT_SIZE);
  LOGT(MP3_PLAYER_TAG, "before _choose_au_convert_ctx");
  _choose_au_convert_ctx(av_get_default_channel_layout( \
                         g_mp3_player.audio_dec_ctx->channels),
                         g_mp3_player.audio_dec_ctx->sample_fmt,
                         g_mp3_player.audio_dec_ctx->sample_rate);
  LOGT(MP3_PLAYER_TAG, "prepare internal success");
  return 0;
}

static void _write_databuffer(char *buf, int len, int *actual_write_size) {
  static int total_len = 0;
  total_len += len;
  LOGT(MP3_PLAYER_TAG, "len=%d, total_len=%d, total=%ds",
       len, total_len, total_len / 32000);
  //usleep(1000 * len / 32);
  *actual_write_size = len;
}

static int _swr_context_cache_check_and_process(int *decode_byte_len) {
  int cache_size = 0;
  int data_len;
  cache_size = swr_get_out_samples(g_mp3_player.au_convert_ctx, 0);
  if (g_mp3_player.frame->nb_samples > 0 &&
      cache_size >= g_mp3_player.frame->nb_samples) {
    data_len = swr_convert(g_mp3_player.au_convert_ctx,
                           &g_mp3_player.out_buffer,
                           AUDIO_OUT_SIZE / 2, NULL, 0);
    data_len *= (av_get_bytes_per_sample(g_mp3_player.out_sample_fmt) *
                g_mp3_player.out_channels);
    if (0 == data_len) {
      LOGW(MP3_PLAYER_TAG, "try read swr_cache, cannot get data, datasize=0");
      goto L_END;
    }
    _write_databuffer((char *)g_mp3_player.out_buffer, data_len,
                      decode_byte_len);
    return 0;
  }
L_END:
  return -1;
}

static int _decode_packet(int *decode_byte_len) {
  int ret = 0;
  int decoded = g_mp3_player.pkt.size;
  int got_frame = 0;
  int data_len;
  if (g_mp3_player.pkt.stream_index == g_mp3_player.audio_stream_idx) {
    if (0 == _swr_context_cache_check_and_process(decode_byte_len)) {
      return 0;
    }
    ret = avcodec_decode_audio4(g_mp3_player.audio_dec_ctx, g_mp3_player.frame,
                                &got_frame, &g_mp3_player.pkt);
    if (ret < 0) {
      *decode_byte_len = AUDIO_RETRIEVE_DATA_FINISHED;
      LOGE(MP3_PLAYER_TAG, "Error decoding audio frame (%s)", av_err2str(ret));
      return -1;
    }
    decoded = FFMIN(ret, g_mp3_player.pkt.size);
    if (got_frame) {
      if ((data_len = swr_convert(g_mp3_player.au_convert_ctx,
                                  &g_mp3_player.out_buffer, AUDIO_OUT_SIZE / 2,
                                  g_mp3_player.frame->data,
                                  g_mp3_player.frame->nb_samples)) < 0) {
        LOGE(MP3_PLAYER_TAG, "Could not convert input samples (error '%s')",
             av_err2str(data_len));
        return data_len;
      }
      data_len *= (av_get_bytes_per_sample(g_mp3_player.out_sample_fmt) *
                   g_mp3_player.out_channels);
      _write_databuffer((char *)g_mp3_player.out_buffer, data_len,
                        decode_byte_len);
    }
  }
  return decoded;
}

static int _audio_player_callback() {
  int ret, decode_byte_len = 0;
  if (MP3_PLAYING_STATE != g_mp3_player.state) {
    return 0;
  }
  _set_block_state(BLOCK_READ_FRAME);
  if (0 == g_mp3_player.pkt.size) {
    if ((ret = av_read_frame(g_mp3_player.fmt_ctx, &g_mp3_player.pkt)) < 0) {
      LOGE(MP3_PLAYER_TAG, "Demuxing succeeded[%d-->%s]", ret, av_err2str(ret));
      return AUDIO_RETRIEVE_DATA_FINISHED;
    }
    g_mp3_player.orig_pkt = g_mp3_player.pkt;
  }
  do {
    if (g_mp3_player.pkt.size <= 0) {
      break;
    }
    ret = _decode_packet(&decode_byte_len);
    if (ret < 0) {
      break;
    }
    g_mp3_player.pkt.data += ret;
    g_mp3_player.pkt.size -= ret;
  } while (0);
  if (0 == g_mp3_player.pkt.size) {
    av_packet_unref(&g_mp3_player.orig_pkt);
    memset(&g_mp3_player.orig_pkt, 0, sizeof(g_mp3_player.orig_pkt));
  }
  return decode_byte_len;
}

static void* __retrieve_tsk(void *args) {
  while (1) {
    if (AUDIO_RETRIEVE_DATA_FINISHED == _audio_player_callback()) break;
  }
  return NULL;
}

static void _mp3_start_internal(void) {
  pthread_t pid;
  pthread_create(&pid, NULL, __retrieve_tsk, NULL);
  pthread_detach(pid);
}

static int _mp3_release_internal(void) {
  if (0 != g_mp3_player.orig_pkt.size) {
    av_packet_unref(&g_mp3_player.orig_pkt);
    memset(&g_mp3_player.orig_pkt, 0, sizeof(g_mp3_player.orig_pkt));
  }
  if (g_mp3_player.audio_dec_ctx) {
    avcodec_free_context(&g_mp3_player.audio_dec_ctx);
    g_mp3_player.audio_dec_ctx = NULL;
  }
  if (g_mp3_player.fmt_ctx) {
    avformat_close_input(&g_mp3_player.fmt_ctx);
    g_mp3_player.fmt_ctx = NULL;
  }
  if (g_mp3_player.frame) {
    av_frame_free(&g_mp3_player.frame);
    g_mp3_player.frame = NULL;
  }
  if (g_mp3_player.out_buffer) {
    av_free(g_mp3_player.out_buffer);
    g_mp3_player.out_buffer = NULL;
  }
  avformat_network_deinit();
  g_mp3_player.au_convert_ctx = NULL;
  return 0;
}

static void _mp3_stop_internal(void) {
}

static int _mp3_fsm(Mp3Event event, void *param) {
  int rc = -1;
  switch (g_mp3_player.state) {
    case MP3_IDLE_STATE:
      if (MP3_PLAY_EVENT == event) {
        if (0 == _mp3_prepare_internal((char *)param)) {
          _mp3_start_internal();
          _mp3_set_state(MP3_PLAYING_STATE);
          rc = 0;
          break;
        }
        _mp3_release_internal();
        break;
      }
      if (MP3_PREPARE_EVENT == event) {
      }
      break;
    case MP3_PREPARING_STATE:
      if (MP3_START_EVENT == event || MP3_RESUME_EVENT == event) {
        while (MP3_PREPARING_STATE == g_mp3_player.state) {
          sleep(0);
          LOGT(MP3_PLAYER_TAG, "waiting while mp3 is preparing");
        }
        if (MP3_PREPARED_STATE == g_mp3_player.state) {
          _mp3_start_internal();
          _mp3_set_state(MP3_PLAYING_STATE);
          rc = 0;
        }
      }
      break;
    case MP3_PREPARED_STATE:
      if (MP3_START_EVENT == event || MP3_RESUME_EVENT == event) {
        _mp3_start_internal();
        _mp3_set_state(MP3_PLAYING_STATE);
        rc = 0;
      } else if (MP3_STOP_EVENT == event) {
        _mp3_release_internal();
        _mp3_set_state(MP3_IDLE_STATE);
        rc = 0;
      }
      break;
    case MP3_PLAYING_STATE:
      if (MP3_PAUSE_EVENT == event) {
        _mp3_stop_internal();
        _mp3_set_state(MP3_PAUSED_STATE);
        rc = 0;
      } else if (MP3_STOP_EVENT == event) {
        _mp3_stop_internal();
        _mp3_release_internal();
        _mp3_set_state(MP3_IDLE_STATE);
        rc = 0;
      }
      break;
    case MP3_PAUSED_STATE:
      if (MP3_RESUME_EVENT == event) {
        _mp3_start_internal();
        _mp3_set_state(MP3_PLAYING_STATE);
        rc = 0;
      } else if (MP3_STOP_EVENT == event) {
        _mp3_release_internal();
        _mp3_set_state(MP3_IDLE_STATE);
        rc = 0;
      }
      break;
    default:
      break;
  }
  LOGT(MP3_PLAYER_TAG, "event %s, state %s, result %s",
       _event2string(event), _state2string(g_mp3_player.state),
       rc == 0 ? "OK" : "FAILED");
  return rc;
}

int Mp3Play(char *filename) {
  return _mp3_fsm(MP3_PLAY_EVENT, (void *)filename);
}

int Mp3Prepare(char *filename) {
  LOGT(MP3_PLAYER_TAG, "playing %s", filename);
  return _mp3_fsm(MP3_PREPARE_EVENT, (void *)filename);
}

int Mp3Start(void) {
  return _mp3_fsm(MP3_START_EVENT, NULL);
}

int Mp3Pause(void) {
  return _mp3_fsm(MP3_PAUSE_EVENT, NULL);
}

int Mp3Resume(void) {
  return _mp3_fsm(MP3_RESUME_EVENT, NULL);
}

int Mp3Stop(void) {
  return _mp3_fsm(MP3_STOP_EVENT, NULL);
}

int Mp3Init(AudioParam *param) {
  g_mp3_player.out_channels = param->channels;
  g_mp3_player.out_sample_rate = param->rate;
  if (param->channels == 1) {
    g_mp3_player.out_channel_layout = AV_CH_LAYOUT_MONO;
  } else {
    g_mp3_player.out_channel_layout = AV_CH_LAYOUT_STEREO;
  }
  if (param->bit == 16) {
    g_mp3_player.out_sample_fmt = AV_SAMPLE_FMT_S16;
  } else {
    g_mp3_player.out_sample_fmt = AV_SAMPLE_FMT_S32;
  }
  return 0;
}

int Mp3Final(void) {
  ConvertCtxNode *head = g_mp3_player.convert_ctx_list;
  ConvertCtxNode *node = head;
  while (NULL != node) {
    head = node->next;
    _destroy_convert_ctx_node(node);
    node = head;
  }
  return 0;
}

int Mp3CheckIsPlaying(void) {
  return (g_mp3_player.state == MP3_PLAYING_STATE);
}

int Mp3CheckIsPause(void) {
  return (g_mp3_player.state == MP3_PAUSED_STATE);
}
