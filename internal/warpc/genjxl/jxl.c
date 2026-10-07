// Copyright 2026 The Hugo Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jxl/cms.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#include "../deps/parson/parson.h"

void handle_commands(FILE *stream);

int main()
{
    // This will read commands from stdin and write responses to stdout
    // and return 0 when stdin is closed.
    // Any errors gets reported in the RPC response messages.
    handle_commands(stdin);
    return 0;
}

typedef struct
{
    int version;
    int id;
    char command[256];
    char err[256];
} Header;

typedef struct
{
    int width;
    int height;
    int stride;
    int depth;         // Bits per sample in the image. The pixel buffer uses 16 bit samples if > 8.
    int bitsPerSample; // Encoder only: bits per sample to store, if different from depth.
    int loopCount;
    int frameCount;
    int *frameDurations;

    // CICP color properties, only set for HDR (PQ or HLG) images.
    int colorPrimaries;
    int transferCharacteristics;
    int matrixCoefficients;
} Params;

typedef struct
{
    float quality;        // between 1 and 100.
    char compression[32]; // "lossy" or "lossless"
    int effort;           // 1 (fastest) to 10 (slowest).
} Options;

typedef struct
{
    Header header;
    Options options;
    Params params;
} Message;

// CICP values, see ITU-T H.273.
enum
{
    CICP_PRIMARIES_BT709 = 1,
    CICP_PRIMARIES_BT2020 = 9,
    CICP_PRIMARIES_SMPTE431 = 11, // DCI-P3.
    CICP_PRIMARIES_SMPTE432 = 12, // Display P3.
    CICP_MATRIX_BT2020_NCL = 9,
};

#define MAX_LINE_LENGTH 65536

static void copy_string(char *dst, size_t size, const char *src)
{
    if (src != NULL)
    {
        strncpy(dst, src, size - 1);
        dst[size - 1] = '\0';
    }
}

static Message parse_input_message(const char *line)
{
    Message msg = {0};

    JSON_Value *root_value = json_parse_string(line);
    if (root_value == NULL || json_value_get_type(root_value) != JSONObject)
    {
        fprintf(stderr, "Error parsing JSON line\n");
        json_value_free(root_value);
        return msg;
    }
    JSON_Object *root = json_value_get_object(root_value);

    JSON_Object *header = json_object_get_object(root, "header");
    if (header != NULL)
    {
        msg.header.version = (int)json_object_get_number(header, "version");
        msg.header.id = (int)json_object_get_number(header, "id");
        copy_string(msg.header.command, sizeof(msg.header.command), json_object_get_string(header, "command"));
    }

    JSON_Object *params = json_object_dotget_object(root, "data.params");
    if (params != NULL)
    {
        msg.params.width = (int)json_object_get_number(params, "width");
        msg.params.height = (int)json_object_get_number(params, "height");
        msg.params.stride = (int)json_object_get_number(params, "stride");
        msg.params.depth = (int)json_object_get_number(params, "depth");
        msg.params.bitsPerSample = (int)json_object_get_number(params, "bitsPerSample");
        msg.params.loopCount = (int)json_object_get_number(params, "loopCount");
        msg.params.colorPrimaries = (int)json_object_get_number(params, "colorPrimaries");
        msg.params.transferCharacteristics = (int)json_object_get_number(params, "transferCharacteristics");
        JSON_Array *durations = json_object_get_array(params, "frameDurations");
        size_t count = json_array_get_count(durations);
        if (count > 0)
        {
            msg.params.frameDurations = malloc(sizeof(int) * count);
            if (msg.params.frameDurations != NULL)
            {
                msg.params.frameCount = (int)count;
                for (size_t i = 0; i < count; i++)
                {
                    msg.params.frameDurations[i] = (int)json_array_get_number(durations, i);
                }
            }
        }
    }

    JSON_Object *options = json_object_dotget_object(root, "data.options");
    if (options != NULL)
    {
        msg.options.quality = (float)json_object_get_number(options, "quality");
        msg.options.effort = (int)json_object_get_number(options, "effort");
        copy_string(msg.options.compression, sizeof(msg.options.compression), json_object_get_string(options, "compression"));
    }

    json_value_free(root_value);
    return msg;
}

static void write_blob(uint32_t id, const uint8_t *data, uint32_t size)
{
    uint8_t blob_header[16];
    // See https://github.com/bep/textandbinarywriter
    const char magic[] = {'T', 'A', 'K', '3', '5', 'E', 'M', '1'};
    memcpy(blob_header, magic, 8);
    memcpy(&blob_header[8], &id, sizeof(id));
    memcpy(&blob_header[12], &size, sizeof(size));

    fwrite(blob_header, 1, sizeof(blob_header), stdout);
    fwrite(data, 1, (size_t)size, stdout);
    fflush(stdout);
}

static void write_output_message(const Message *msg)
{
    JSON_Value *root_value = json_value_init_object();
    JSON_Object *root = json_value_get_object(root_value);

    json_object_dotset_number(root, "header.version", msg->header.version);
    json_object_dotset_number(root, "header.id", msg->header.id);
    json_object_dotset_string(root, "header.err", msg->header.err);

    const Params *p = &msg->params;
    if (p->width > 0)
    {
        json_object_dotset_number(root, "data.params.width", p->width);
        json_object_dotset_number(root, "data.params.height", p->height);
        json_object_dotset_number(root, "data.params.stride", p->stride);
        json_object_dotset_number(root, "data.params.depth", p->depth);
        json_object_dotset_number(root, "data.params.colorPrimaries", p->colorPrimaries);
        json_object_dotset_number(root, "data.params.transferCharacteristics", p->transferCharacteristics);
        json_object_dotset_number(root, "data.params.matrixCoefficients", p->matrixCoefficients);
        if (p->frameDurations != NULL)
        {
            JSON_Value *durations_value = json_value_init_array();
            JSON_Array *durations = json_value_get_array(durations_value);
            for (int i = 0; i < p->frameCount; i++)
            {
                json_array_append_number(durations, p->frameDurations[i]);
            }
            json_object_dotset_value(root, "data.params.frameDurations", durations_value);
            json_object_dotset_number(root, "data.params.loopCount", p->loopCount);
        }
    }

    char *serialized = json_serialize_to_string(root_value);
    fprintf(stdout, "%s\n", serialized);
    fflush(stdout);

    json_free_serialized_string(serialized);
    json_value_free(root_value);
}

// drain_bytes discards n bytes from stream. Used to keep the protocol aligned
// after an error that prevents the blob from being consumed normally.
static void drain_bytes(FILE *stream, size_t n)
{
    uint8_t buf[4096];
    while (n > 0)
    {
        size_t want = n < sizeof(buf) ? n : sizeof(buf);
        size_t got = fread(buf, 1, want, stream);
        if (got == 0)
        {
            break;
        }
        n -= got;
    }
}

static const char *encoder_error_string(JxlEncoderError err)
{
    switch (err)
    {
    case JXL_ENC_ERR_OOM:
        return "out of memory";
    case JXL_ENC_ERR_BAD_INPUT:
        return "bad input";
    case JXL_ENC_ERR_NOT_SUPPORTED:
        return "not supported";
    case JXL_ENC_ERR_API_USAGE:
        return "API usage error";
    default:
        return "generic error";
    }
}

static bool is_hdr_transfer(int tf)
{
    return tf == JXL_TRANSFER_FUNCTION_PQ || tf == JXL_TRANSFER_FUNCTION_HLG;
}

// cicp_from_color_encoding sets the CICP params for HDR images and reports whether it did.
// The JXL transfer function enum values for PQ and HLG match CICP.
static bool cicp_from_color_encoding(const JxlColorEncoding *ce, Params *p)
{
    if (ce->color_space != JXL_COLOR_SPACE_RGB || !is_hdr_transfer(ce->transfer_function))
    {
        return false;
    }
    switch (ce->primaries)
    {
    case JXL_PRIMARIES_SRGB:
        p->colorPrimaries = CICP_PRIMARIES_BT709;
        break;
    case JXL_PRIMARIES_2100:
        p->colorPrimaries = CICP_PRIMARIES_BT2020;
        break;
    case JXL_PRIMARIES_P3:
        p->colorPrimaries = ce->white_point == JXL_WHITE_POINT_DCI ? CICP_PRIMARIES_SMPTE431 : CICP_PRIMARIES_SMPTE432;
        break;
    default:
        return false;
    }
    p->transferCharacteristics = ce->transfer_function;
    p->matrixCoefficients = CICP_MATRIX_BT2020_NCL;
    return true;
}

// color_encoding_from_params sets ce to sRGB or, if HDR CICP params are set, the matching HDR encoding.
static void color_encoding_from_params(const Params *p, bool gray, JxlColorEncoding *ce)
{
    JxlColorEncodingSetToSRGB(ce, gray);
    if (gray || !is_hdr_transfer(p->transferCharacteristics))
    {
        return;
    }
    switch (p->colorPrimaries)
    {
    case CICP_PRIMARIES_BT709:
        ce->primaries = JXL_PRIMARIES_SRGB;
        break;
    case CICP_PRIMARIES_BT2020:
        ce->primaries = JXL_PRIMARIES_2100;
        break;
    case CICP_PRIMARIES_SMPTE431:
        ce->primaries = JXL_PRIMARIES_P3;
        ce->white_point = JXL_WHITE_POINT_DCI;
        break;
    case CICP_PRIMARIES_SMPTE432:
        ce->primaries = JXL_PRIMARIES_P3;
        break;
    default:
        return;
    }
    ce->transfer_function = (JxlTransferFunction)p->transferCharacteristics;
    ce->rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
}

// decode decodes the image in data. If config_only is set, it stops after reading the image dimensions.
// On success, the pixels of all frames are returned in *pixels (RGBA, 8 or 16 bit big endian samples).
static bool decode(const uint8_t *data, size_t size, bool config_only, Params *p, uint8_t **pixels, size_t *pixels_size, char *err, size_t err_size)
{
    bool ok = false;
    JxlBasicInfo info;
    JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_BIG_ENDIAN, 0};
    size_t frame_size = 0;
    size_t capacity = 0;
    int durations_capacity = 0;
    int num_frames = 0;
    uint8_t *buf = NULL;

    JxlDecoder *dec = JxlDecoderCreate(NULL);
    if (dec == NULL)
    {
        snprintf(err, err_size, "failed to create JXL decoder");
        return false;
    }

    int events = JXL_DEC_BASIC_INFO;
    if (!config_only)
    {
        events |= JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE;
    }
    if (JxlDecoderSubscribeEvents(dec, events) != JXL_DEC_SUCCESS ||
        JxlDecoderSetCms(dec, *JxlGetDefaultCms()) != JXL_DEC_SUCCESS ||
        JxlDecoderSetUnpremultiplyAlpha(dec, JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec, data, size) != JXL_DEC_SUCCESS)
    {
        snprintf(err, err_size, "failed to configure JXL decoder");
        goto done;
    }
    JxlDecoderCloseInput(dec);

    for (;;)
    {
        JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        switch (status)
        {
        case JXL_DEC_ERROR:
            snprintf(err, err_size, "failed to decode JXL image");
            goto done;
        case JXL_DEC_NEED_MORE_INPUT:
            snprintf(err, err_size, "failed to decode JXL image: unexpected end of input");
            goto done;
        case JXL_DEC_BASIC_INFO:
            if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS)
            {
                snprintf(err, err_size, "failed to get JXL basic info");
                goto done;
            }
            p->width = info.xsize;
            p->height = info.ysize;
            if (info.orientation > JXL_ORIENT_ROTATE_180)
            {
                // The decoder applies the orientation, which swaps the dimensions.
                p->width = info.ysize;
                p->height = info.xsize;
            }
            if (config_only)
            {
                ok = true;
                goto done;
            }
            p->depth = info.bits_per_sample;
            if (p->depth > 8)
            {
                format.data_type = JXL_TYPE_UINT16;
            }
            p->stride = p->width * 4 * (p->depth > 8 ? 2 : 1);
            frame_size = (size_t)p->stride * p->height;
            if (info.have_animation)
            {
                p->loopCount = info.animation.num_loops;
            }
            break;
        case JXL_DEC_COLOR_ENCODING:
        {
            JxlColorEncoding ce;
            if (JxlDecoderGetColorAsEncodedProfile(dec, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &ce) == JXL_DEC_SUCCESS && cicp_from_color_encoding(&ce, p))
            {
                // Keep HDR images in their original color space.
                JxlDecoderSetOutputColorProfile(dec, &ce, NULL, 0);
            }
            else
            {
                // Convert e.g. images with ICC profiles to sRGB. Failure to do so is not fatal.
                JxlColorEncodingSetToSRGB(&ce, info.num_color_channels == 1);
                JxlDecoderSetOutputColorProfile(dec, &ce, NULL, 0);
            }
            break;
        }
        case JXL_DEC_FRAME:
            if (info.have_animation)
            {
                JxlFrameHeader fh;
                if (JxlDecoderGetFrameHeader(dec, &fh) != JXL_DEC_SUCCESS)
                {
                    snprintf(err, err_size, "failed to get JXL frame header");
                    goto done;
                }
                if (p->frameCount == durations_capacity)
                {
                    durations_capacity = durations_capacity == 0 ? 16 : durations_capacity * 2;
                    int *d = realloc(p->frameDurations, sizeof(int) * durations_capacity);
                    if (d == NULL)
                    {
                        snprintf(err, err_size, "out of memory allocating frame durations");
                        goto done;
                    }
                    p->frameDurations = d;
                }
                p->frameDurations[p->frameCount++] = (int)lround(fh.duration * 1000.0 * info.animation.tps_denominator / info.animation.tps_numerator);
            }
            break;
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
        {
            size_t needed;
            if (JxlDecoderImageOutBufferSize(dec, &format, &needed) != JXL_DEC_SUCCESS || needed != frame_size)
            {
                snprintf(err, err_size, "unexpected JXL output buffer size");
                goto done;
            }
            if ((num_frames + 1) * frame_size > capacity)
            {
                size_t new_capacity = capacity == 0 ? frame_size : capacity * 2;
                uint8_t *b = realloc(buf, new_capacity);
                if (b == NULL)
                {
                    snprintf(err, err_size, "out of memory allocating %zu bytes for image data", new_capacity);
                    goto done;
                }
                buf = b;
                capacity = new_capacity;
            }
            if (JxlDecoderSetImageOutBuffer(dec, &format, buf + num_frames * frame_size, frame_size) != JXL_DEC_SUCCESS)
            {
                snprintf(err, err_size, "failed to set JXL output buffer");
                goto done;
            }
            break;
        }
        case JXL_DEC_FULL_IMAGE:
            num_frames++;
            break;
        case JXL_DEC_SUCCESS:
            if (num_frames == 0)
            {
                snprintf(err, err_size, "JXL image has no frames");
                goto done;
            }
            if (!info.have_animation)
            {
                // Only the last (coalesced) frame is visible.
                memmove(buf, buf + (num_frames - 1) * frame_size, frame_size);
                num_frames = 1;
            }
            *pixels = buf;
            *pixels_size = num_frames * frame_size;
            buf = NULL;
            ok = true;
            goto done;
        default:
            snprintf(err, err_size, "unexpected JXL decoder status %d", status);
            goto done;
        }
    }

done:
    free(buf);
    JxlDecoderDestroy(dec);
    return ok;
}

// encode encodes the frames in pix (gray or NRGBA, 8 or 16 bit big endian samples, frames packed with stride*height bytes each).
static bool encode(const Message *in, const uint8_t *pix, size_t pix_size, bool gray, uint8_t **out, size_t *out_size, char *err, size_t err_size)
{
    const Params *p = &in->params;
    const int bps = p->depth == 16 ? 2 : 1;
    const int in_channels = gray ? 1 : 4;
    const size_t row_size = (size_t)p->width * in_channels * bps;
    const size_t frame_size = (size_t)p->stride * p->height;
    const int num_frames = p->frameCount > 1 ? p->frameCount : 1;
    const bool animated = num_frames > 1;
    bool ok = false;
    uint8_t *tight = NULL;
    uint8_t *buf = NULL;
    JxlEncoder *enc = NULL;

    if (p->width <= 0 || p->height <= 0 || (size_t)p->stride < row_size ||
        pix_size < (num_frames - 1) * frame_size + (size_t)p->stride * (p->height - 1) + row_size)
    {
        snprintf(err, err_size, "invalid image dimensions %dx%d stride %d for %zu bytes", p->width, p->height, p->stride, pix_size);
        return false;
    }

    bool has_alpha = false;
    if (!gray)
    {
        for (int f = 0; f < num_frames && !has_alpha; f++)
        {
            for (int y = 0; y < p->height && !has_alpha; y++)
            {
                const uint8_t *row = pix + f * frame_size + (size_t)y * p->stride;
                for (size_t x = 3 * bps; x < row_size; x += 4 * bps)
                {
                    if (row[x] != 0xff || (bps == 2 && row[x + 1] != 0xff))
                    {
                        has_alpha = true;
                        break;
                    }
                }
            }
        }
    }
    const int out_channels = gray ? 1 : (has_alpha ? 4 : 3);
    const size_t out_row_size = (size_t)p->width * out_channels * bps;

    tight = malloc(out_row_size * p->height);
    if (tight == NULL)
    {
        snprintf(err, err_size, "out of memory allocating %zu bytes for image data", out_row_size * p->height);
        goto done;
    }

    enc = JxlEncoderCreate(NULL);
    if (enc == NULL)
    {
        snprintf(err, err_size, "failed to create JXL encoder");
        goto done;
    }

    const bool lossless = strcmp(in->options.compression, "lossless") == 0;

    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = p->width;
    info.ysize = p->height;
    info.bits_per_sample = p->bitsPerSample > 0 && p->bitsPerSample <= 8 * bps ? p->bitsPerSample : 8 * bps;
    info.num_color_channels = gray ? 1 : 3;
    info.alpha_bits = has_alpha ? info.bits_per_sample : 0;
    info.num_extra_channels = has_alpha ? 1 : 0;
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
    if (animated)
    {
        info.have_animation = JXL_TRUE;
        info.animation.tps_numerator = 1000; // Durations are in milliseconds.
        info.animation.tps_denominator = 1;
        info.animation.num_loops = p->loopCount;
    }

    JxlColorEncoding ce;
    color_encoding_from_params(p, gray, &ce);
    if (ce.transfer_function == JXL_TRANSFER_FUNCTION_PQ)
    {
        info.intensity_target = 10000;
    }
    else if (ce.transfer_function == JXL_TRANSFER_FUNCTION_HLG)
    {
        info.intensity_target = 1000;
    }

    JxlEncoderStatus status = JxlEncoderSetBasicInfo(enc, &info);
    if (status == JXL_ENC_SUCCESS)
    {
        status = JxlEncoderSetColorEncoding(enc, &ce);
    }
    if (status != JXL_ENC_SUCCESS)
    {
        snprintf(err, err_size, "failed to configure JXL encoder: %s", encoder_error_string(JxlEncoderGetError(enc)));
        goto done;
    }

    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    int effort = in->options.effort > 0 ? in->options.effort : 3;
    if (JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, effort) != JXL_ENC_SUCCESS)
    {
        snprintf(err, err_size, "invalid JXL effort %d", effort);
        goto done;
    }
    if (lossless)
    {
        status = JxlEncoderSetFrameLossless(fs, JXL_TRUE);
    }
    else
    {
        float quality = in->options.quality > 0 ? in->options.quality : 75;
        status = JxlEncoderSetFrameDistance(fs, JxlEncoderDistanceFromQuality(quality));
    }
    if (status != JXL_ENC_SUCCESS)
    {
        snprintf(err, err_size, "failed to set JXL compression: %s", encoder_error_string(JxlEncoderGetError(enc)));
        goto done;
    }

    JxlPixelFormat format = {out_channels, bps == 2 ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8, JXL_BIG_ENDIAN, 0};
    for (int f = 0; f < num_frames; f++)
    {
        for (int y = 0; y < p->height; y++)
        {
            const uint8_t *src = pix + f * frame_size + (size_t)y * p->stride;
            uint8_t *dst = tight + (size_t)y * out_row_size;
            if (out_channels == in_channels)
            {
                memcpy(dst, src, row_size);
                continue;
            }
            // Drop the opaque alpha channel.
            for (int x = 0; x < p->width; x++)
            {
                memcpy(dst + x * 3 * bps, src + x * 4 * bps, 3 * bps);
            }
        }
        if (animated)
        {
            JxlFrameHeader fh;
            JxlEncoderInitFrameHeader(&fh);
            fh.duration = p->frameDurations[f];
            JxlEncoderSetFrameHeader(fs, &fh);
        }
        if (JxlEncoderAddImageFrame(fs, &format, tight, out_row_size * p->height) != JXL_ENC_SUCCESS)
        {
            snprintf(err, err_size, "failed to add JXL frame: %s", encoder_error_string(JxlEncoderGetError(enc)));
            goto done;
        }
    }
    JxlEncoderCloseInput(enc);

    size_t capacity = 1 << 16;
    buf = malloc(capacity);
    if (buf == NULL)
    {
        snprintf(err, err_size, "out of memory allocating output buffer");
        goto done;
    }
    uint8_t *next_out = buf;
    size_t avail_out = capacity;
    while ((status = JxlEncoderProcessOutput(enc, &next_out, &avail_out)) == JXL_ENC_NEED_MORE_OUTPUT)
    {
        size_t offset = next_out - buf;
        uint8_t *b = realloc(buf, capacity * 2);
        if (b == NULL)
        {
            snprintf(err, err_size, "out of memory allocating output buffer");
            goto done;
        }
        buf = b;
        capacity *= 2;
        next_out = buf + offset;
        avail_out = capacity - offset;
    }
    if (status != JXL_ENC_SUCCESS)
    {
        snprintf(err, err_size, "failed to encode JXL image: %s", encoder_error_string(JxlEncoderGetError(enc)));
        goto done;
    }

    *out = buf;
    *out_size = next_out - buf;
    buf = NULL;
    ok = true;

done:
    free(buf);
    free(tight);
    JxlEncoderDestroy(enc);
    return ok;
}

void handle_commands(FILE *stream)
{
    static char line[MAX_LINE_LENGTH];

    while (fgets(line, sizeof(line), stream) != NULL)
    {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) == 0)
        {
            continue;
        }

        Message input = parse_input_message(line);
        Message output = {0};
        output.header = input.header;
        uint8_t *blob_data = NULL;
        uint8_t *result = NULL;
        size_t result_size = 0;

        // Next in stream is a blob header defined in https://github.com/bep/textandbinaryreader
        // T', 'A', 'K', '3', '5', 'E', 'M', '1' id uint32, size uint32
        uint8_t blob_header[16];
        if (fread(blob_header, 1, sizeof(blob_header), stream) != sizeof(blob_header))
        {
            fprintf(stderr, "Error reading blob header\n");
            goto cleanup;
        }
        uint32_t blob_size;
        memcpy(&blob_size, &blob_header[12], sizeof(blob_size));
        blob_data = malloc(blob_size > 0 ? blob_size : 1);
        if (blob_data == NULL)
        {
            // Out of memory. Drain the blob from the input stream so the next
            // command stays aligned, then report the error to the client.
            drain_bytes(stream, blob_size);
            snprintf(output.header.err, sizeof(output.header.err), "out of memory allocating %u bytes for blob data", blob_size);
            write_output_message(&output);
            goto cleanup;
        }
        if (fread(blob_data, 1, blob_size, stream) != blob_size)
        {
            fprintf(stderr, "Error reading blob data\n");
            goto cleanup;
        }

        const char *cmd = input.header.command;
        char *err = output.header.err;
        const size_t err_size = sizeof(output.header.err);

        if (strcmp(cmd, "config") == 0)
        {
            decode(blob_data, blob_size, true, &output.params, NULL, NULL, err, err_size);
            write_output_message(&output);
        }
        else if (strcmp(cmd, "decode") == 0)
        {
            if (decode(blob_data, blob_size, false, &output.params, &result, &result_size, err, err_size))
            {
                write_output_message(&output);
                write_blob(output.header.id, result, result_size);
            }
            else
            {
                output.params.width = 0;
                write_output_message(&output);
            }
        }
        else if (strcmp(cmd, "encodeNRGBA") == 0 || strcmp(cmd, "encodeGray") == 0)
        {
            bool gray = strcmp(cmd, "encodeGray") == 0;
            if (encode(&input, blob_data, blob_size, gray, &result, &result_size, err, err_size))
            {
                write_output_message(&output);
                write_blob(output.header.id, result, result_size);
            }
            else
            {
                write_output_message(&output);
            }
        }
        else
        {
            snprintf(err, err_size, "unknown command: %s", cmd);
            write_output_message(&output);
        }

    cleanup:
        free(blob_data);
        free(result);
        free(input.params.frameDurations);
        free(output.params.frameDurations);
    }
}
