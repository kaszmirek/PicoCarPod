// A2DP sink + SBC decoding + playback. Media path adapted from BTstack's
// a2dp_sink_demo.c (BlueKitchen GmbH, BSD-3-Clause).
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_resample.h"
#include "btstack_ring_buffer.h"

#include "bt_audio.h"
#include "bt_conn.h"

#define NUM_CHANNELS        2
#define BYTES_PER_FRAME     (2 * NUM_CHANNELS)
#define MAX_SBC_FRAME_SIZE  120

// ring buffer for SBC frames: below MIN add samples, MIN..MAX fine, above MAX drop samples
#define OPTIMAL_FRAMES_MIN 60
#define OPTIMAL_FRAMES_MAX 80
#define ADDITIONAL_FRAMES  30

typedef struct {
    uint8_t  reconfigure;
    uint8_t  num_channels;
    uint16_t sampling_frequency;
    uint8_t  block_length;
    uint8_t  subbands;
    uint8_t  min_bitpool_value;
    uint8_t  max_bitpool_value;
    btstack_sbc_channel_mode_t      channel_mode;
    btstack_sbc_allocation_method_t allocation_method;
} media_codec_configuration_sbc_t;

typedef enum {
    STREAM_STATE_CLOSED,
    STREAM_STATE_OPEN,
    STREAM_STATE_PLAYING,
    STREAM_STATE_PAUSED,
} stream_state_t;

static uint8_t sdp_avdtp_sink_service_buffer[150];

// we support all SBC configurations with bitpool 2-53
static uint8_t media_sbc_codec_capabilities[] = {
    0xFF, // (AVDTP_SBC_44100 << 4) | AVDTP_SBC_STEREO,
    0xFF, // (AVDTP_SBC_BLOCK_LENGTH_16 << 4) | (AVDTP_SBC_SUBBANDS_8 << 2) | AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS,
    2, 53
};
static uint8_t media_sbc_codec_configuration[4];
static uint8_t a2dp_local_seid;

static const btstack_sbc_decoder_t *   sbc_decoder_instance;
static btstack_sbc_decoder_bluedroid_t sbc_decoder_context;

static uint8_t sbc_frame_storage[(OPTIMAL_FRAMES_MAX + ADDITIONAL_FRAMES) * MAX_SBC_FRAME_SIZE];
static btstack_ring_buffer_t sbc_frame_ring_buffer;
static unsigned int sbc_frame_size;

// overflow buffer for not fully used sbc frames, with additional frames for resampling
static uint8_t decoded_audio_storage[(128 + 16) * BYTES_PER_FRAME];
static btstack_ring_buffer_t decoded_audio_ring_buffer;

static int media_initialized = 0;
static int audio_stream_started;
static btstack_resample_t resample_instance;

// temp storage of lower-layer request for audio samples
static int16_t * request_buffer;
static int       request_frames;

static struct {
    bd_addr_t addr;
    uint16_t  a2dp_cid;
    stream_state_t stream_state;
    media_codec_configuration_sbc_t sbc_configuration;
} a2dp_conn;

// ---------------------------------------------------------------------------
// Media processing

static void playback_handler(int16_t * buffer, uint16_t num_audio_frames){
    // called from lower-layer but guaranteed to be on main thread
    if (sbc_frame_size == 0){
        memset(buffer, 0, num_audio_frames * BYTES_PER_FRAME);
        return;
    }

    // first fill from resampled audio
    uint32_t bytes_read;
    btstack_ring_buffer_read(&decoded_audio_ring_buffer, (uint8_t *) buffer, num_audio_frames * BYTES_PER_FRAME, &bytes_read);
    buffer           += bytes_read / NUM_CHANNELS;
    num_audio_frames -= bytes_read / BYTES_PER_FRAME;

    // then start decoding sbc frames using request_* globals
    request_buffer = buffer;
    request_frames = num_audio_frames;
    while (request_frames && btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer) >= sbc_frame_size){
        uint8_t sbc_frame[MAX_SBC_FRAME_SIZE];
        btstack_ring_buffer_read(&sbc_frame_ring_buffer, sbc_frame, sbc_frame_size, &bytes_read);
        sbc_decoder_instance->decode_signed_16(&sbc_decoder_context, 0, sbc_frame, sbc_frame_size);
    }
    // underrun: pad with silence
    if (request_frames){
        memset(request_buffer, 0, request_frames * BYTES_PER_FRAME);
    }
}

static void handle_pcm_data(int16_t * data, int num_audio_frames, int num_channels, int sample_rate, void * context){
    UNUSED(sample_rate);
    UNUSED(context);
    UNUSED(num_channels);   // must be stereo == 2

    // resample into request buffer - add some additional space for resampling
    int16_t  output_buffer[(128 + 16) * NUM_CHANNELS];
    uint32_t resampled_frames = btstack_resample_block(&resample_instance, data, num_audio_frames, output_buffer);

    // store data in btstack_audio buffer first
    int frames_to_copy = btstack_min(resampled_frames, request_frames);
    memcpy(request_buffer, output_buffer, frames_to_copy * BYTES_PER_FRAME);
    request_frames -= frames_to_copy;
    request_buffer += frames_to_copy * NUM_CHANNELS;

    // and rest in ring buffer
    int frames_to_store = resampled_frames - frames_to_copy;
    if (frames_to_store){
        int status = btstack_ring_buffer_write(&decoded_audio_ring_buffer, (uint8_t *)&output_buffer[frames_to_copy * NUM_CHANNELS], frames_to_store * BYTES_PER_FRAME);
        if (status){
            printf("AUDIO: PCM ring buffer overflow\n");
        }
    }
}

static int media_processing_init(media_codec_configuration_sbc_t * configuration){
    if (media_initialized) return 0;

    sbc_decoder_instance = btstack_sbc_decoder_bluedroid_init_instance(&sbc_decoder_context);
    sbc_decoder_instance->configure(&sbc_decoder_context, SBC_MODE_STANDARD, handle_pcm_data, NULL);

    btstack_ring_buffer_init(&sbc_frame_ring_buffer, sbc_frame_storage, sizeof(sbc_frame_storage));
    btstack_ring_buffer_init(&decoded_audio_ring_buffer, decoded_audio_storage, sizeof(decoded_audio_storage));
    btstack_resample_init(&resample_instance, configuration->num_channels);

    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (audio){
        audio->init(NUM_CHANNELS, configuration->sampling_frequency, &playback_handler);
    }

    audio_stream_started = 0;
    media_initialized = 1;
    return 0;
}

static void media_processing_start(void){
    if (!media_initialized) return;
    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (audio){
        audio->start_stream();
    }
    audio_stream_started = 1;
}

static void media_processing_pause(void){
    if (!media_initialized) return;
    audio_stream_started = 0;
    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (audio){
        audio->stop_stream();
    }
    btstack_ring_buffer_reset(&decoded_audio_ring_buffer);
    btstack_ring_buffer_reset(&sbc_frame_ring_buffer);
}

static void media_processing_close(void){
    if (!media_initialized) return;
    media_initialized = 0;
    audio_stream_started = 0;
    sbc_frame_size = 0;
    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (audio){
        audio->close();
    }
}

// ---------------------------------------------------------------------------
// Media packets

static int read_media_data_header(uint8_t *packet, int size, int *offset, avdtp_media_packet_header_t *media_header){
    int media_header_len = 12; // without crc
    int pos = *offset;
    if (size - pos < media_header_len){
        return 0;
    }
    media_header->version = packet[pos] & 0x03;
    media_header->padding = get_bit16(packet[pos],2);
    media_header->extension = get_bit16(packet[pos],3);
    media_header->csrc_count = (packet[pos] >> 4) & 0x0F;
    pos++;
    media_header->marker = get_bit16(packet[pos],0);
    media_header->payload_type  = (packet[pos] >> 1) & 0x7F;
    pos++;
    media_header->sequence_number = big_endian_read_16(packet, pos);
    pos+=2;
    media_header->timestamp = big_endian_read_32(packet, pos);
    pos+=4;
    media_header->synchronization_source = big_endian_read_32(packet, pos);
    pos+=4;
    *offset = pos;
    return 1;
}

static int read_sbc_header(uint8_t * packet, int size, int * offset, avdtp_sbc_codec_header_t * sbc_header){
    int sbc_header_len = 12; // without crc
    int pos = *offset;
    if (size - pos < sbc_header_len){
        return 0;
    }
    sbc_header->fragmentation = get_bit16(packet[pos], 7);
    sbc_header->starting_packet = get_bit16(packet[pos], 6);
    sbc_header->last_packet = get_bit16(packet[pos], 5);
    sbc_header->num_frames = packet[pos] & 0x0f;
    pos++;
    *offset = pos;
    return 1;
}

static void handle_l2cap_media_data_packet(uint8_t seid, uint8_t *packet, uint16_t size){
    UNUSED(seid);
    int pos = 0;

    avdtp_media_packet_header_t media_header;
    if (!read_media_data_header(packet, size, &pos, &media_header)) return;

    avdtp_sbc_codec_header_t sbc_header;
    if (!read_sbc_header(packet, size, &pos, &sbc_header)) return;

    int packet_length = size - pos;
    uint8_t *packet_begin = packet + pos;

    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (!audio || !media_initialized){
        return;
    }

    // store sbc frame size for buffer management
    sbc_frame_size = packet_length / sbc_header.num_frames;
    int status = btstack_ring_buffer_write(&sbc_frame_ring_buffer, packet_begin, packet_length);
    if (status != ERROR_CODE_SUCCESS){
        printf("AUDIO: SBC ring buffer overflow\n");
    }

    // decide on audio sync drift based on number of sbc frames in queue
    int sbc_frames_in_buffer = btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer) / sbc_frame_size;

    uint32_t resampling_factor;
    uint32_t nominal_factor = 0x10000;   // fixed-point 2^16
    uint32_t compensation   = 0x00100;
    if (sbc_frames_in_buffer < OPTIMAL_FRAMES_MIN){
        resampling_factor = nominal_factor - compensation;    // stretch samples
    } else if (sbc_frames_in_buffer <= OPTIMAL_FRAMES_MAX){
        resampling_factor = nominal_factor;                   // nothing to do
    } else {
        resampling_factor = nominal_factor + compensation;    // compress samples
    }
    btstack_resample_set_factor(&resample_instance, resampling_factor);

    // start stream if enough frames buffered
    if (!audio_stream_started && sbc_frames_in_buffer >= OPTIMAL_FRAMES_MIN){
        media_processing_start();
    }
}

// ---------------------------------------------------------------------------
// A2DP events

static void a2dp_sink_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint8_t status;
    bd_addr_t addr;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    switch (packet[2]){
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
            status = a2dp_subevent_signaling_connection_established_get_status(packet);
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet, addr);
            if (status == ERROR_CODE_SUCCESS){
                a2dp_conn.a2dp_cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
                memcpy(a2dp_conn.addr, addr, 6);
                printf("A2DP : signaling connected to %s, cid 0x%02x\n", bd_addr_to_str(addr), a2dp_conn.a2dp_cid);
            } else {
                printf("A2DP : signaling connection to %s failed, status 0x%02x\n", bd_addr_to_str(addr), status);
            }
            bt_conn_on_a2dp_signaling_established(addr, status);
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
            printf("A2DP : received non-SBC codec configuration - not supported\n");
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
            media_codec_configuration_sbc_t * c = &a2dp_conn.sbc_configuration;
            c->reconfigure        = a2dp_subevent_signaling_media_codec_sbc_configuration_get_reconfigure(packet);
            c->num_channels       = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
            c->sampling_frequency = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            c->block_length       = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
            c->subbands           = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
            c->min_bitpool_value  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
            c->max_bitpool_value  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
            // Adapt Bluetooth spec definition to SBC decoder expected input
            c->allocation_method = (btstack_sbc_allocation_method_t)(a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet) - 1);
            switch (a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet)){
                case AVDTP_CHANNEL_MODE_JOINT_STEREO: c->channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO; break;
                case AVDTP_CHANNEL_MODE_STEREO:       c->channel_mode = SBC_CHANNEL_MODE_STEREO;       break;
                case AVDTP_CHANNEL_MODE_DUAL_CHANNEL: c->channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL; break;
                case AVDTP_CHANNEL_MODE_MONO:         c->channel_mode = SBC_CHANNEL_MODE_MONO;         break;
                default: btstack_assert(false); break;
            }
            printf("A2DP : SBC config: %u Hz, %u ch, bitpool %u-%u\n",
                   c->sampling_frequency, c->num_channels, c->min_bitpool_value, c->max_bitpool_value);
            break;
        }

        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            status = a2dp_subevent_stream_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                printf("A2DP : stream establishment failed, status 0x%02x\n", status);
                break;
            }
            a2dp_subevent_stream_established_get_bd_addr(packet, a2dp_conn.addr);
            a2dp_conn.a2dp_cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);
            a2dp_conn.stream_state = STREAM_STATE_OPEN;
            printf("A2DP : stream established (cid 0x%02x, local seid %u)\n",
                   a2dp_conn.a2dp_cid, a2dp_subevent_stream_established_get_local_seid(packet));
            break;

        case A2DP_SUBEVENT_STREAM_STARTED:
            printf("A2DP : stream started\n");
            a2dp_conn.stream_state = STREAM_STATE_PLAYING;
            if (a2dp_conn.sbc_configuration.reconfigure){
                media_processing_close();
            }
            media_processing_init(&a2dp_conn.sbc_configuration);
            // audio output starts once the SBC buffer reaches its minimum level
            break;

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            printf("A2DP : stream suspended\n");
            a2dp_conn.stream_state = STREAM_STATE_PAUSED;
            media_processing_pause();
            break;

        case A2DP_SUBEVENT_STREAM_RELEASED:
            printf("A2DP : stream released\n");
            a2dp_conn.stream_state = STREAM_STATE_CLOSED;
            media_processing_close();
            break;

        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            printf("A2DP : signaling connection released\n");
            a2dp_conn.a2dp_cid = 0;
            a2dp_conn.stream_state = STREAM_STATE_CLOSED;
            media_processing_close();
            bt_conn_on_a2dp_signaling_released();
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public API

void bt_audio_init(void){
    a2dp_sink_init();
    a2dp_sink_register_packet_handler(&a2dp_sink_packet_handler);
    a2dp_sink_register_media_handler(&handle_l2cap_media_data_packet);

    avdtp_stream_endpoint_t * local_stream_endpoint = a2dp_sink_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_SBC,
        media_sbc_codec_capabilities, sizeof(media_sbc_codec_capabilities),
        media_sbc_codec_configuration, sizeof(media_sbc_codec_configuration));
    btstack_assert(local_stream_endpoint != NULL);
    a2dp_local_seid = avdtp_local_seid(local_stream_endpoint);

    memset(sdp_avdtp_sink_service_buffer, 0, sizeof(sdp_avdtp_sink_service_buffer));
    a2dp_sink_create_sdp_record(sdp_avdtp_sink_service_buffer, sdp_create_service_record_handle(),
                                AVDTP_SINK_FEATURE_MASK_SPEAKER, NULL, NULL);
    btstack_assert(de_get_len(sdp_avdtp_sink_service_buffer) <= sizeof(sdp_avdtp_sink_service_buffer));
    sdp_register_service(sdp_avdtp_sink_service_buffer);
}

bool bt_audio_stream_playing(void){
    return a2dp_conn.stream_state == STREAM_STATE_PLAYING;
}

uint16_t bt_audio_a2dp_cid(void){
    return a2dp_conn.a2dp_cid;
}
