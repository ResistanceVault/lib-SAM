#include "say_backend.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reciter.h"
#include "sam.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAM_OUTPUT_RATE 22050
#define SAY_OUTPUT_RATE 44100
#define SAY_TERMINATOR 155
#define SAY_CHUNK_GAP_MS 30
#define SAY_PHONE_HZ_LOW 500.0
#define SAY_PHONE_HZ_HIGH 2800.0
#define SAY_PHONE_PRESENCE_HZ 1700.0

typedef struct {
    char *text;
    char *phonemes;
    char *split_kind;
} say_chunk_t;

typedef struct {
    say_chunk_t *items;
    size_t count;
    size_t capacity;
} say_chunk_list_t;

typedef struct {
    char *normalized_input;
    say_chunk_t *chunks;
    size_t chunk_count;
} say_plan_t;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} say_string_builder_t;

typedef struct {
    int16_t *samples;
    size_t count;
    size_t capacity;
} say_pcm_buffer_t;

typedef struct {
    double b0;
    double b1;
    double b2;
    double a1;
    double a2;
    double z1;
    double z2;
} say_biquad_t;

static char *say_strdup_local(const char *value)
{
    size_t length;
    char *copy;

    if (value == NULL) {
        return NULL;
    }

    length = strlen(value);
    copy = malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, length + 1);
    return copy;
}

static char *say_strndup_local(const char *value, size_t length)
{
    char *copy = malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, length);
    copy[length] = '\0';
    return copy;
}

static int say_ascii_casecmp(const char *left, const char *right)
{
    unsigned char lch;
    unsigned char rch;

    if (left == NULL || right == NULL) {
        return left == right ? 0 : (left == NULL ? -1 : 1);
    }

    while (*left != '\0' && *right != '\0') {
        lch = (unsigned char)tolower((unsigned char)*left++);
        rch = (unsigned char)tolower((unsigned char)*right++);
        if (lch != rch) {
            return (int)lch - (int)rch;
        }
    }

    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

static int say_set_errorf(char **error_message, const char *format, ...)
{
    va_list args;
    va_list copy;
    int needed;
    char *buffer;

    if (error_message == NULL) {
        return 0;
    }

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(args);
        *error_message = say_strdup_local("internal formatting failure");
        return 0;
    }

    buffer = malloc((size_t)needed + 1);
    if (buffer == NULL) {
        va_end(args);
        *error_message = say_strdup_local("out of memory");
        return 0;
    }

    vsnprintf(buffer, (size_t)needed + 1, format, args);
    va_end(args);
    *error_message = buffer;
    return 0;
}

static void say_sb_init(say_string_builder_t *builder)
{
    builder->data = NULL;
    builder->length = 0;
    builder->capacity = 0;
}

static void say_sb_free(say_string_builder_t *builder)
{
    free(builder->data);
    builder->data = NULL;
    builder->length = 0;
    builder->capacity = 0;
}

static int say_sb_reserve(say_string_builder_t *builder, size_t extra)
{
    size_t needed = builder->length + extra + 1;
    size_t new_capacity;
    char *new_data;

    if (needed <= builder->capacity) {
        return 1;
    }

    new_capacity = builder->capacity > 0 ? builder->capacity : 128;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    new_data = realloc(builder->data, new_capacity);
    if (new_data == NULL) {
        return 0;
    }

    builder->data = new_data;
    builder->capacity = new_capacity;
    return 1;
}

static int say_sb_append_n(say_string_builder_t *builder, const char *text, size_t length)
{
    if (!say_sb_reserve(builder, length)) {
        return 0;
    }

    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return 1;
}

static int say_sb_append(say_string_builder_t *builder, const char *text)
{
    return say_sb_append_n(builder, text, strlen(text));
}

static int say_sb_appendf(say_string_builder_t *builder, const char *format, ...)
{
    va_list args;
    va_list copy;
    int needed;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(args);
        return 0;
    }

    if (!say_sb_reserve(builder, (size_t)needed)) {
        va_end(args);
        return 0;
    }

    vsnprintf(builder->data + builder->length, builder->capacity - builder->length, format, args);
    va_end(args);
    builder->length += (size_t)needed;
    return 1;
}

static char *say_sb_take(say_string_builder_t *builder)
{
    char *data = builder->data;
    builder->data = NULL;
    builder->length = 0;
    builder->capacity = 0;
    return data;
}

static void say_pcm_init(say_pcm_buffer_t *buffer)
{
    buffer->samples = NULL;
    buffer->count = 0;
    buffer->capacity = 0;
}

static void say_pcm_free(say_pcm_buffer_t *buffer)
{
    free(buffer->samples);
    buffer->samples = NULL;
    buffer->count = 0;
    buffer->capacity = 0;
}

static int say_pcm_reserve(say_pcm_buffer_t *buffer, size_t extra)
{
    size_t needed = buffer->count + extra;
    size_t new_capacity;
    int16_t *new_samples;

    if (needed <= buffer->capacity) {
        return 1;
    }

    new_capacity = buffer->capacity > 0 ? buffer->capacity : 2048;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    new_samples = realloc(buffer->samples, new_capacity * sizeof(*new_samples));
    if (new_samples == NULL) {
        return 0;
    }

    buffer->samples = new_samples;
    buffer->capacity = new_capacity;
    return 1;
}

static int say_pcm_append_zeros(say_pcm_buffer_t *buffer, size_t count)
{
    if (!say_pcm_reserve(buffer, count)) {
        return 0;
    }

    memset(buffer->samples + buffer->count, 0, count * sizeof(*buffer->samples));
    buffer->count += count;
    return 1;
}

static int16_t say_u8_to_s16(unsigned char sample)
{
    return (int16_t)(((int)sample - 128) << 8);
}

static int say_pcm_append_upsampled(say_pcm_buffer_t *buffer, const unsigned char *samples, size_t sample_count)
{
    size_t i;

    if (!say_pcm_reserve(buffer, sample_count * 2)) {
        return 0;
    }

    for (i = 0; i < sample_count; ++i) {
        int16_t current = say_u8_to_s16(samples[i]);
        int16_t next = (i + 1 < sample_count) ? say_u8_to_s16(samples[i + 1]) : current;
        buffer->samples[buffer->count++] = current;
        buffer->samples[buffer->count++] = (int16_t)(((int)current + (int)next) / 2);
    }

    return 1;
}

static int16_t say_double_to_s16(double value)
{
    if (value > 0.999969482421875) {
        value = 0.999969482421875;
    } else if (value < -1.0) {
        value = -1.0;
    }

    if (value >= 0.0) {
        return (int16_t)(value * 32767.0 + 0.5);
    }

    return (int16_t)(value * 32768.0 - 0.5);
}

static void say_biquad_normalize(
    say_biquad_t *biquad,
    double b0,
    double b1,
    double b2,
    double a0,
    double a1,
    double a2
)
{
    biquad->b0 = b0 / a0;
    biquad->b1 = b1 / a0;
    biquad->b2 = b2 / a0;
    biquad->a1 = a1 / a0;
    biquad->a2 = a2 / a0;
    biquad->z1 = 0.0;
    biquad->z2 = 0.0;
}

static void say_biquad_init_lowpass(say_biquad_t *biquad, double sample_rate, double cutoff_hz, double q)
{
    double omega = 2.0 * M_PI * cutoff_hz / sample_rate;
    double cos_omega = cos(omega);
    double sin_omega = sin(omega);
    double alpha = sin_omega / (2.0 * q);
    double b0 = (1.0 - cos_omega) * 0.5;
    double b1 = 1.0 - cos_omega;
    double b2 = (1.0 - cos_omega) * 0.5;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cos_omega;
    double a2 = 1.0 - alpha;

    say_biquad_normalize(biquad, b0, b1, b2, a0, a1, a2);
}

static void say_biquad_init_highpass(say_biquad_t *biquad, double sample_rate, double cutoff_hz, double q)
{
    double omega = 2.0 * M_PI * cutoff_hz / sample_rate;
    double cos_omega = cos(omega);
    double sin_omega = sin(omega);
    double alpha = sin_omega / (2.0 * q);
    double b0 = (1.0 + cos_omega) * 0.5;
    double b1 = -(1.0 + cos_omega);
    double b2 = (1.0 + cos_omega) * 0.5;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cos_omega;
    double a2 = 1.0 - alpha;

    say_biquad_normalize(biquad, b0, b1, b2, a0, a1, a2);
}

static void say_biquad_init_peaking(
    say_biquad_t *biquad,
    double sample_rate,
    double center_hz,
    double q,
    double gain_db
)
{
    double omega = 2.0 * M_PI * center_hz / sample_rate;
    double cos_omega = cos(omega);
    double sin_omega = sin(omega);
    double alpha = sin_omega / (2.0 * q);
    double gain = pow(10.0, gain_db / 40.0);
    double b0 = 1.0 + alpha * gain;
    double b1 = -2.0 * cos_omega;
    double b2 = 1.0 - alpha * gain;
    double a0 = 1.0 + alpha / gain;
    double a1 = -2.0 * cos_omega;
    double a2 = 1.0 - alpha / gain;

    say_biquad_normalize(biquad, b0, b1, b2, a0, a1, a2);
}

static double say_biquad_process(say_biquad_t *biquad, double sample)
{
    double output = biquad->b0 * sample + biquad->z1;
    biquad->z1 = biquad->b1 * sample - biquad->a1 * output + biquad->z2;
    biquad->z2 = biquad->b2 * sample - biquad->a2 * output;
    return output;
}

static void say_apply_phone_filter(int16_t *samples, size_t sample_count)
{
    say_biquad_t hp1;
    say_biquad_t hp2;
    say_biquad_t lp1;
    say_biquad_t lp2;
    say_biquad_t presence;
    size_t i;

    say_biquad_init_highpass(&hp1, SAY_OUTPUT_RATE, SAY_PHONE_HZ_LOW, 0.7071067811865476);
    say_biquad_init_highpass(&hp2, SAY_OUTPUT_RATE, SAY_PHONE_HZ_LOW, 0.7071067811865476);
    say_biquad_init_lowpass(&lp1, SAY_OUTPUT_RATE, SAY_PHONE_HZ_HIGH, 0.7071067811865476);
    say_biquad_init_lowpass(&lp2, SAY_OUTPUT_RATE, SAY_PHONE_HZ_HIGH, 0.7071067811865476);
    say_biquad_init_peaking(&presence, SAY_OUTPUT_RATE, SAY_PHONE_PRESENCE_HZ, 1.1, 5.5);

    for (i = 0; i < sample_count; ++i) {
        double sample = (double)samples[i] / 32768.0;
        sample = say_biquad_process(&hp1, sample);
        sample = say_biquad_process(&hp2, sample);
        sample = say_biquad_process(&lp1, sample);
        sample = say_biquad_process(&lp2, sample);
        sample = say_biquad_process(&presence, sample);
        sample = tanh(sample * 1.35) * 0.92;
        samples[i] = say_double_to_s16(sample);
    }
}

static void say_apply_gain(int16_t *samples, size_t sample_count, double gain)
{
    const double knee = 0.7079457843841379;
    size_t i;

    for (i = 0; i < sample_count; ++i) {
        double sample = ((double)samples[i] / 32768.0) * gain;
        double sign = sample < 0.0 ? -1.0 : 1.0;
        double magnitude = fabs(sample);

        if (magnitude > knee) {
            double over = (magnitude - knee) / (1.0 - knee);
            magnitude = knee + (1.0 - knee) * tanh(over);
        }

        samples[i] = say_double_to_s16(sign * magnitude);
    }
}

static void say_chunk_list_init(say_chunk_list_t *list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void say_chunk_free(say_chunk_t *chunk)
{
    free(chunk->text);
    free(chunk->phonemes);
    free(chunk->split_kind);
    chunk->text = NULL;
    chunk->phonemes = NULL;
    chunk->split_kind = NULL;
}

static void say_chunk_list_free(say_chunk_list_t *list)
{
    size_t i;

    for (i = 0; i < list->count; ++i) {
        say_chunk_free(&list->items[i]);
    }

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int say_chunk_list_push(say_chunk_list_t *list, say_chunk_t *chunk)
{
    say_chunk_t *new_items;
    size_t new_capacity;

    if (list->count == list->capacity) {
        new_capacity = list->capacity > 0 ? list->capacity * 2 : 8;
        new_items = realloc(list->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return 0;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count++] = *chunk;
    chunk->text = NULL;
    chunk->phonemes = NULL;
    chunk->split_kind = NULL;
    return 1;
}

static void say_plan_free(say_plan_t *plan)
{
    say_chunk_list_t list;
    list.items = plan->chunks;
    list.count = plan->chunk_count;
    list.capacity = plan->chunk_count;
    say_chunk_list_free(&list);
    free(plan->normalized_input);
    plan->normalized_input = NULL;
    plan->chunks = NULL;
    plan->chunk_count = 0;
}

static int say_is_sentence_delimiter(char ch)
{
    return ch == '.' || ch == '!' || ch == '?' || ch == ';' || ch == ':';
}

static int say_is_ascii_alpha_numeric(unsigned char ch)
{
    return isalpha(ch) || isdigit(ch);
}

static char say_ascii_upper(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return (char)(ch - 'a' + 'A');
    }
    return ch;
}

static int say_append_small_number_words(say_string_builder_t *builder, unsigned int value)
{
    static const char *const units[] = {
        "zero", "one", "two", "three", "four",
        "five", "six", "seven", "eight", "nine"
    };
    static const char *const teens[] = {
        "ten", "eleven", "twelve", "thirteen", "fourteen",
        "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"
    };
    static const char *const tens[] = {
        "", "", "twenty", "thirty", "forty",
        "fifty", "sixty", "seventy", "eighty", "ninety"
    };

    if (value < 10) {
        return say_sb_append(builder, units[value]);
    }

    if (value < 20) {
        return say_sb_append(builder, teens[value - 10]);
    }

    if (!say_sb_append(builder, tens[value / 10])) {
        return 0;
    }

    if ((value % 10) != 0) {
        if (!say_sb_append(builder, " ")) {
            return 0;
        }
        if (!say_sb_append(builder, units[value % 10])) {
            return 0;
        }
    }

    return 1;
}

static int say_append_number_words(say_string_builder_t *builder, unsigned int value)
{
    if (value >= 1000) {
        if (!say_append_small_number_words(builder, value / 1000)) {
            return 0;
        }
        if (!say_sb_append(builder, " thousand")) {
            return 0;
        }
        value %= 1000;
        if (value != 0) {
            if (!say_sb_append(builder, " ")) {
                return 0;
            }
        }
    }

    if (value >= 100) {
        if (!say_append_small_number_words(builder, value / 100)) {
            return 0;
        }
        if (!say_sb_append(builder, " hundred")) {
            return 0;
        }
        value %= 100;
        if (value != 0) {
            if (!say_sb_append(builder, " ")) {
                return 0;
            }
        }
    }

    if (value != 0) {
        return say_append_small_number_words(builder, value);
    }

    return 1;
}

static int say_append_expanded_number(
    say_string_builder_t *builder,
    const unsigned char *digits,
    size_t length,
    int *in_space
)
{
    unsigned int value = 0;
    size_t i;
    say_string_builder_t number_builder;
    char *expanded;

    if (length < 2 || length > 4) {
        if (!say_sb_append_n(builder, (const char *)digits, length)) {
            return 0;
        }
        *in_space = 0;
        return 1;
    }

    for (i = 0; i < length; ++i) {
        value = value * 10u + (unsigned int)(digits[i] - '0');
    }

    if (value < 10 || value > 9999) {
        if (!say_sb_append_n(builder, (const char *)digits, length)) {
            return 0;
        }
        *in_space = 0;
        return 1;
    }

    say_sb_init(&number_builder);
    if (!say_append_number_words(&number_builder, value)) {
        say_sb_free(&number_builder);
        return 0;
    }

    expanded = say_sb_take(&number_builder);
    if (expanded == NULL) {
        return 0;
    }

    if (!say_sb_append(builder, expanded)) {
        free(expanded);
        return 0;
    }

    free(expanded);
    *in_space = 0;
    return 1;
}

static char *say_normalize_text(const char *input)
{
    say_string_builder_t builder;
    int in_space = 1;
    const unsigned char *cursor = (const unsigned char *)input;

    say_sb_init(&builder);

    while (*cursor != '\0') {
        unsigned char ch = *cursor++;
        if (isdigit(ch)) {
            const unsigned char *digit_start = cursor - 1;
            const unsigned char *digit_end = digit_start;
            unsigned char prev = digit_start > (const unsigned char *)input ? digit_start[-1] : '\0';
            unsigned char next;

            while (isdigit(*digit_end)) {
                ++digit_end;
            }
            next = *digit_end;

            if (!say_is_ascii_alpha_numeric(prev) && !say_is_ascii_alpha_numeric(next)) {
                if (!say_append_expanded_number(&builder, digit_start, (size_t)(digit_end - digit_start), &in_space)) {
                    say_sb_free(&builder);
                    return NULL;
                }
                cursor = digit_end;
                continue;
            }
        }

        if (isspace(ch)) {
            if (!in_space) {
                if (!say_sb_append_n(&builder, " ", 1)) {
                    say_sb_free(&builder);
                    return NULL;
                }
                in_space = 1;
            }
            continue;
        }

        if (!say_sb_append_n(&builder, (const char *)&ch, 1)) {
            say_sb_free(&builder);
            return NULL;
        }
        in_space = 0;
    }

    while (builder.length > 0 && builder.data[builder.length - 1] == ' ') {
        builder.data[--builder.length] = '\0';
    }

    if (builder.data == NULL) {
        builder.data = malloc(1);
        if (builder.data == NULL) {
            return NULL;
        }
        builder.data[0] = '\0';
    }

    return say_sb_take(&builder);
}

static char *say_normalize_phonemes(const char *input)
{
    char *normalized = say_normalize_text(input);
    size_t i;

    if (normalized == NULL) {
        return NULL;
    }

    for (i = 0; normalized[i] != '\0'; ++i) {
        normalized[i] = say_ascii_upper(normalized[i]);
    }

    return normalized;
}

static size_t say_find_split_point(const char *text)
{
    size_t length = strlen(text);
    size_t midpoint = length / 2;
    size_t left = midpoint;
    size_t right = midpoint;

    while (left > 0 || right < length) {
        if (left > 0 && isspace((unsigned char)text[left])) {
            return left;
        }
        if (right < length && isspace((unsigned char)text[right])) {
            return right;
        }
        if (left > 0) {
            --left;
        }
        if (right < length) {
            ++right;
        }
    }

    return length / 2;
}

static int say_recite_text(const char *text, char **phonemes, int *truncated, char **error_message)
{
    unsigned char buffer[256];
    size_t length = strlen(text);
    size_t phoneme_length = 0;

    *phonemes = NULL;
    *truncated = 0;

    if (length > 252) {
        *truncated = 1;
        return 1;
    }

    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, text, length);
    buffer[length] = '[';

    if (!TextToPhonemes(buffer)) {
        return say_set_errorf(error_message, "text reciter failed on segment: %s", text);
    }

    *truncated = ReciterWasTruncated();
    while (phoneme_length < sizeof(buffer) && buffer[phoneme_length] != SAY_TERMINATOR) {
        ++phoneme_length;
    }

    if (phoneme_length == sizeof(buffer)) {
        return say_set_errorf(error_message, "reciter did not terminate a phoneme segment");
    }

    *phonemes = say_strndup_local((const char *)buffer, phoneme_length);
    if (*phonemes == NULL) {
        return say_set_errorf(error_message, "out of memory");
    }

    return 1;
}

static int say_prepare_text_segment(
    const char *segment,
    const char *split_kind,
    say_chunk_list_t *chunks,
    char **error_message
)
{
    char *normalized = NULL;
    char *left = NULL;
    char *right = NULL;
    char *phonemes = NULL;
    say_chunk_t chunk;
    int truncated = 0;
    size_t split_at;
    size_t length;
    const char *right_start;

    memset(&chunk, 0, sizeof(chunk));
    normalized = say_normalize_text(segment);
    if (normalized == NULL) {
        return say_set_errorf(error_message, "out of memory");
    }

    if (normalized[0] == '\0') {
        free(normalized);
        return 1;
    }

    if (!say_recite_text(normalized, &phonemes, &truncated, error_message)) {
        free(normalized);
        return 0;
    }

    if (!truncated) {
        chunk.text = normalized;
        chunk.phonemes = phonemes;
        chunk.split_kind = say_strdup_local(split_kind);
        if (chunk.split_kind == NULL) {
            say_chunk_free(&chunk);
            return say_set_errorf(error_message, "out of memory");
        }
        if (!say_chunk_list_push(chunks, &chunk)) {
            say_chunk_free(&chunk);
            return say_set_errorf(error_message, "out of memory");
        }
        return 1;
    }

    free(phonemes);
    phonemes = NULL;
    length = strlen(normalized);
    split_at = say_find_split_point(normalized);
    if (split_at == 0 || split_at >= length) {
        split_at = length / 2;
        if (split_at == 0 || split_at >= length) {
            free(normalized);
            return say_set_errorf(error_message, "unable to split long text segment safely");
        }
    }

    left = say_strndup_local(normalized, split_at);
    if (left == NULL) {
        free(normalized);
        return say_set_errorf(error_message, "out of memory");
    }

    right_start = normalized + split_at;
    while (*right_start != '\0' && isspace((unsigned char)*right_start)) {
        ++right_start;
    }
    right = say_strdup_local(right_start);
    if (right == NULL) {
        free(left);
        free(normalized);
        return say_set_errorf(error_message, "out of memory");
    }

    free(normalized);

    if (!say_prepare_text_segment(left, "whitespace fallback", chunks, error_message)) {
        free(left);
        free(right);
        return 0;
    }

    if (!say_prepare_text_segment(right, "whitespace fallback", chunks, error_message)) {
        free(left);
        free(right);
        return 0;
    }

    free(left);
    free(right);
    return 1;
}

static int say_prepare_text_plan(const char *input, say_plan_t *plan, char **error_message)
{
    char *normalized = NULL;
    const char *start;
    const char *cursor;
    say_chunk_list_t chunks;

    say_chunk_list_init(&chunks);
    normalized = say_normalize_text(input);
    if (normalized == NULL) {
        return say_set_errorf(error_message, "out of memory");
    }

    if (normalized[0] == '\0') {
        free(normalized);
        return say_set_errorf(error_message, "input is empty");
    }

    start = normalized;
    cursor = normalized;
    while (*cursor != '\0') {
        if (say_is_sentence_delimiter(*cursor)) {
            const char *end = cursor + 1;
            while (*end != '\0' && isspace((unsigned char)*end)) {
                ++end;
            }
            if (end > start) {
                char *sentence = say_strndup_local(start, (size_t)(end - start));
                if (sentence == NULL) {
                    say_chunk_list_free(&chunks);
                    free(normalized);
                    return say_set_errorf(error_message, "out of memory");
                }
                if (!say_prepare_text_segment(sentence, "sentence boundary", &chunks, error_message)) {
                    free(sentence);
                    say_chunk_list_free(&chunks);
                    free(normalized);
                    return 0;
                }
                free(sentence);
            }
            start = end;
            cursor = end;
            continue;
        }
        ++cursor;
    }

    if (start < cursor) {
        char *tail = say_strndup_local(start, (size_t)(cursor - start));
        if (tail == NULL) {
            say_chunk_list_free(&chunks);
            free(normalized);
            return say_set_errorf(error_message, "out of memory");
        }
        if (!say_prepare_text_segment(tail, "single segment", &chunks, error_message)) {
            free(tail);
            say_chunk_list_free(&chunks);
            free(normalized);
            return 0;
        }
        free(tail);
    }

    plan->normalized_input = normalized;
    plan->chunks = chunks.items;
    plan->chunk_count = chunks.count;
    return 1;
}

static int say_validate_phoneme_text(const char *phonemes, char **error_message)
{
    unsigned char buffer[256];
    size_t length = strlen(phonemes);

    if (length == 0) {
        return say_set_errorf(error_message, "phoneme input is empty");
    }

    if (length > 250) {
        return say_set_errorf(error_message, "phoneme input is too long for a single SAM-safe chunk");
    }

    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, phonemes, length);
    buffer[length] = SAY_TERMINATOR;
    SetInput(buffer);
    if (!SAMMain()) {
        return say_set_errorf(error_message, "invalid phoneme input");
    }
    return 1;
}

static int say_prepare_phoneme_plan(const char *input, say_plan_t *plan, char **error_message)
{
    char *normalized = say_normalize_phonemes(input);
    say_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));

    if (normalized == NULL) {
        return say_set_errorf(error_message, "out of memory");
    }

    if (!say_validate_phoneme_text(normalized, error_message)) {
        free(normalized);
        return 0;
    }

    chunk.text = say_strdup_local(normalized);
    chunk.phonemes = say_strdup_local(normalized);
    chunk.split_kind = say_strdup_local("single segment");
    if (chunk.text == NULL || chunk.phonemes == NULL || chunk.split_kind == NULL) {
        say_chunk_free(&chunk);
        free(normalized);
        return say_set_errorf(error_message, "out of memory");
    }

    plan->normalized_input = normalized;
    plan->chunks = malloc(sizeof(*plan->chunks));
    if (plan->chunks == NULL) {
        say_chunk_free(&chunk);
        free(normalized);
        return say_set_errorf(error_message, "out of memory");
    }

    plan->chunks[0] = chunk;
    plan->chunk_count = 1;
    return 1;
}

static int say_validate_options(const say_options_t *input, say_options_t *validated, char **error_message)
{
    *validated = *input;

    if (validated->language == NULL) {
        validated->language = "en";
    }

    if (say_ascii_casecmp(validated->language, "en") != 0) {
        return say_set_errorf(error_message, "unsupported language: %s", validated->language);
    }

    if (validated->sample_rate != SAY_OUTPUT_RATE) {
        return say_set_errorf(error_message, "unsupported sample rate: %d", validated->sample_rate);
    }

    if (validated->frame_ms < 5 || validated->frame_ms > 10) {
        return say_set_errorf(error_message, "frame_ms must be between 5 and 10");
    }

    if (validated->gain <= 0.0) {
        return say_set_errorf(error_message, "gain must be greater than 0");
    }

    if (validated->speed < 0 || validated->speed > 255) {
        return say_set_errorf(error_message, "speed must be between 0 and 255");
    }

    if (validated->pitch < 0 || validated->pitch > 255) {
        return say_set_errorf(error_message, "pitch must be between 0 and 255");
    }

    if (validated->mouth < 0 || validated->mouth > 255) {
        return say_set_errorf(error_message, "mouth must be between 0 and 255");
    }

    if (validated->throat < 0 || validated->throat > 255) {
        return say_set_errorf(error_message, "throat must be between 0 and 255");
    }

    if (validated->format != SAY_FORMAT_RAW &&
        validated->format != SAY_FORMAT_AIFF &&
        validated->format != SAY_FORMAT_WAV) {
        return say_set_errorf(error_message, "unsupported output format");
    }

    validated->language = "en";
    return 1;
}

static int say_render_chunk(
    const say_options_t *options,
    const char *phonemes,
    say_pcm_buffer_t *pcm,
    char **error_message
)
{
    unsigned char buffer[256];
    const unsigned char *sam_buffer;
    size_t phoneme_length = strlen(phonemes);
    int sample_count;

    if (phoneme_length > 250) {
        return say_set_errorf(error_message, "phoneme segment exceeds the SAM-safe limit");
    }

    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, phonemes, phoneme_length);
    buffer[phoneme_length] = SAY_TERMINATOR;

    ResetSamParameters();
    SetSpeed((unsigned char)options->speed);
    SetPitch((unsigned char)options->pitch);
    SetMouth((unsigned char)options->mouth);
    SetThroat((unsigned char)options->throat);
    SetSingMode(options->sing);
    SetInput(buffer);

    if (!SAMMain()) {
        return say_set_errorf(error_message, "synthesis failure");
    }

    sample_count = GetBufferLength();
    sam_buffer = (const unsigned char *)GetBuffer();
    if (sample_count <= 0 || sam_buffer == NULL) {
        return say_set_errorf(error_message, "synthesis produced no audio");
    }

    if (pcm != NULL && !say_pcm_append_upsampled(pcm, sam_buffer, (size_t)sample_count)) {
        return say_set_errorf(error_message, "out of memory");
    }

    return 1;
}

static unsigned char *say_encode_raw(const int16_t *samples, size_t sample_count, size_t *size)
{
    size_t i;
    unsigned char *data = malloc(sample_count * 2);
    if (data == NULL) {
        return NULL;
    }

    for (i = 0; i < sample_count; ++i) {
        uint16_t value = (uint16_t)samples[i];
        data[i * 2] = (unsigned char)(value & 0xff);
        data[i * 2 + 1] = (unsigned char)((value >> 8) & 0xff);
    }

    *size = sample_count * 2;
    return data;
}

static unsigned char *say_encode_wav(const int16_t *samples, size_t sample_count, size_t *size)
{
    size_t data_size = sample_count * 2;
    size_t total_size = 44 + data_size;
    unsigned char *data = calloc(total_size, 1);
    size_t i;
    uint32_t riff_size;
    uint32_t byte_rate = SAY_OUTPUT_RATE * 2;

    if (data == NULL) {
        return NULL;
    }

    riff_size = (uint32_t)(36 + data_size);
    memcpy(data, "RIFF", 4);
    data[4] = (unsigned char)(riff_size & 0xff);
    data[5] = (unsigned char)((riff_size >> 8) & 0xff);
    data[6] = (unsigned char)((riff_size >> 16) & 0xff);
    data[7] = (unsigned char)((riff_size >> 24) & 0xff);
    memcpy(data + 8, "WAVEfmt ", 8);
    data[16] = 16;
    data[20] = 1;
    data[22] = 1;
    data[24] = (unsigned char)(SAY_OUTPUT_RATE & 0xff);
    data[25] = (unsigned char)((SAY_OUTPUT_RATE >> 8) & 0xff);
    data[26] = (unsigned char)((SAY_OUTPUT_RATE >> 16) & 0xff);
    data[27] = (unsigned char)((SAY_OUTPUT_RATE >> 24) & 0xff);
    data[28] = (unsigned char)(byte_rate & 0xff);
    data[29] = (unsigned char)((byte_rate >> 8) & 0xff);
    data[30] = (unsigned char)((byte_rate >> 16) & 0xff);
    data[31] = (unsigned char)((byte_rate >> 24) & 0xff);
    data[32] = 2;
    data[34] = 16;
    memcpy(data + 36, "data", 4);
    data[40] = (unsigned char)(data_size & 0xff);
    data[41] = (unsigned char)((data_size >> 8) & 0xff);
    data[42] = (unsigned char)((data_size >> 16) & 0xff);
    data[43] = (unsigned char)((data_size >> 24) & 0xff);

    for (i = 0; i < sample_count; ++i) {
        uint16_t value = (uint16_t)samples[i];
        data[44 + i * 2] = (unsigned char)(value & 0xff);
        data[45 + i * 2] = (unsigned char)((value >> 8) & 0xff);
    }

    *size = total_size;
    return data;
}

static void say_write_be16(unsigned char *data, size_t offset, uint16_t value)
{
    data[offset] = (unsigned char)((value >> 8) & 0xff);
    data[offset + 1] = (unsigned char)(value & 0xff);
}

static void say_write_be32(unsigned char *data, size_t offset, uint32_t value)
{
    data[offset] = (unsigned char)((value >> 24) & 0xff);
    data[offset + 1] = (unsigned char)((value >> 16) & 0xff);
    data[offset + 2] = (unsigned char)((value >> 8) & 0xff);
    data[offset + 3] = (unsigned char)(value & 0xff);
}

static unsigned char *say_encode_aiff(const int16_t *samples, size_t sample_count, size_t *size)
{
    static const unsigned char extended_44100[10] = {
        0x40, 0x0e, 0xac, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    size_t data_size = sample_count * 2;
    size_t total_size = 54 + data_size;
    unsigned char *data = calloc(total_size, 1);
    size_t i;

    if (data == NULL) {
        return NULL;
    }

    memcpy(data, "FORM", 4);
    say_write_be32(data, 4, (uint32_t)(46 + data_size));
    memcpy(data + 8, "AIFF", 4);
    memcpy(data + 12, "COMM", 4);
    say_write_be32(data, 16, 18);
    say_write_be16(data, 20, 1);
    say_write_be32(data, 22, (uint32_t)sample_count);
    say_write_be16(data, 26, 16);
    memcpy(data + 28, extended_44100, sizeof(extended_44100));
    memcpy(data + 38, "SSND", 4);
    say_write_be32(data, 42, (uint32_t)(8 + data_size));
    say_write_be32(data, 46, 0);
    say_write_be32(data, 50, 0);

    for (i = 0; i < sample_count; ++i) {
        uint16_t value = (uint16_t)samples[i];
        data[54 + i * 2] = (unsigned char)((value >> 8) & 0xff);
        data[55 + i * 2] = (unsigned char)(value & 0xff);
    }

    *size = total_size;
    return data;
}

static unsigned char *say_encode_audio(const say_options_t *options, const say_pcm_buffer_t *pcm, size_t *size)
{
    switch (options->format) {
    case SAY_FORMAT_RAW:
        return say_encode_raw(pcm->samples, pcm->count, size);
    case SAY_FORMAT_WAV:
        return say_encode_wav(pcm->samples, pcm->count, size);
    case SAY_FORMAT_AIFF:
        return say_encode_aiff(pcm->samples, pcm->count, size);
    default:
        *size = 0;
        return NULL;
    }
}

static int say_build_report(
    const char *original_input,
    say_input_source_t input_source,
    const say_options_t *options,
    const say_plan_t *plan,
    char **debug_report,
    char **error_message
)
{
    say_string_builder_t builder;
    size_t i;

    say_sb_init(&builder);

    if (!say_sb_appendf(&builder, "Original input: %s\n", original_input != NULL ? original_input : "") ||
        !say_sb_appendf(&builder, "Input source: %s\n", input_source == SAY_INPUT_FILE ? "file" : "literal text") ||
        !say_sb_appendf(&builder, "Mode: %s\n", options->phonemes ? "phoneme" : "text") ||
        !say_sb_appendf(&builder, "Selected output format: %s\n", say_format_name(options->format)) ||
        !say_sb_appendf(&builder, "Effective sample rate: %d\n", SAY_OUTPUT_RATE) ||
        !say_sb_appendf(&builder, "Frame metadata: %d ms\n", options->frame_ms) ||
        !say_sb_appendf(
            &builder,
            "SAM parameters: speed=%d pitch=%d mouth=%d throat=%d sing=%s\n",
            options->speed,
            options->pitch,
            options->mouth,
            options->throat,
            options->sing ? "true" : "false"
        ) ||
        !say_sb_appendf(
            &builder,
            "Post-processing: phone=%s gain=%.3f\n",
            options->phone ? "true" : "false",
            options->gain
        ) ||
        !say_sb_appendf(&builder, "Chunking summary: %zu chunk(s), gap=%d ms\n", plan->chunk_count, SAY_CHUNK_GAP_MS)) {
        say_sb_free(&builder);
        return say_set_errorf(error_message, "out of memory");
    }

    if (!options->phonemes) {
        if (!say_sb_appendf(&builder, "Normalized text: %s\n", plan->normalized_input)) {
            say_sb_free(&builder);
            return say_set_errorf(error_message, "out of memory");
        }
    } else {
        if (!say_sb_appendf(&builder, "Final SAM phoneme string: %s\n", plan->normalized_input)) {
            say_sb_free(&builder);
            return say_set_errorf(error_message, "out of memory");
        }
    }

    for (i = 0; i < plan->chunk_count; ++i) {
        say_chunk_t *chunk = &plan->chunks[i];
        if (!say_sb_appendf(&builder, "\nChunk %zu\n", i + 1) ||
            !say_sb_appendf(&builder, "  split: %s\n", chunk->split_kind)) {
            say_sb_free(&builder);
            return say_set_errorf(error_message, "out of memory");
        }

        if (!options->phonemes) {
            if (!say_sb_appendf(&builder, "  normalized text: %s\n", chunk->text) ||
                !say_sb_appendf(&builder, "  reciter output: %s\n", chunk->phonemes) ||
                !say_sb_appendf(&builder, "  final SAM phoneme string: %s\n", chunk->phonemes)) {
                say_sb_free(&builder);
                return say_set_errorf(error_message, "out of memory");
            }
        } else {
            if (!say_sb_appendf(&builder, "  phoneme input: %s\n", chunk->phonemes)) {
                say_sb_free(&builder);
                return say_set_errorf(error_message, "out of memory");
            }
        }
    }

    *debug_report = say_sb_take(&builder);
    return 1;
}

void say_default_options(say_options_t *options)
{
    if (options == NULL) {
        return;
    }

    options->language = "en";
    options->sample_rate = SAY_OUTPUT_RATE;
    options->frame_ms = 5;
    options->phonemes = 0;
    options->format = SAY_FORMAT_RAW;
    options->gain = 1.0;
    options->phone = 0;
    options->speed = 72;
    options->pitch = 64;
    options->mouth = 128;
    options->throat = 128;
    options->sing = 0;
}

const char *say_format_name(say_format_t format)
{
    switch (format) {
    case SAY_FORMAT_RAW:
        return "raw";
    case SAY_FORMAT_AIFF:
        return "aiff";
    case SAY_FORMAT_WAV:
        return "wav";
    default:
        return "unknown";
    }
}

int say_parse_format_name(const char *name, say_format_t *format)
{
    if (say_ascii_casecmp(name, "raw") == 0) {
        *format = SAY_FORMAT_RAW;
        return 1;
    }
    if (say_ascii_casecmp(name, "aiff") == 0) {
        *format = SAY_FORMAT_AIFF;
        return 1;
    }
    if (say_ascii_casecmp(name, "wav") == 0) {
        *format = SAY_FORMAT_WAV;
        return 1;
    }
    return 0;
}

void say_free_result(say_result_t *result)
{
    if (result == NULL) {
        return;
    }

    free(result->data);
    memset(result, 0, sizeof(*result));
}

void say_free_string(char *value)
{
    free(value);
}

int say_synthesize(
    const char *input,
    const say_options_t *options,
    int dry_run,
    say_input_source_t input_source,
    const char *input_label,
    say_result_t *result,
    char **debug_report,
    char **error_message
)
{
    say_options_t defaults;
    say_options_t validated;
    say_plan_t plan;
    say_pcm_buffer_t pcm;
    unsigned char *encoded = NULL;
    size_t encoded_size = 0;
    size_t i;
    int ok = 0;

    memset(&plan, 0, sizeof(plan));
    say_pcm_init(&pcm);
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    if (debug_report != NULL) {
        *debug_report = NULL;
    }
    if (error_message != NULL) {
        *error_message = NULL;
    }

    if (input == NULL || input[0] == '\0') {
        return say_set_errorf(error_message, "missing input");
    }

    say_default_options(&defaults);
    if (options != NULL) {
        validated = *options;
    } else {
        validated = defaults;
    }

    if (!say_validate_options(&validated, &validated, error_message)) {
        return 0;
    }

    if (validated.phonemes) {
        if (!say_prepare_phoneme_plan(input, &plan, error_message)) {
            goto cleanup;
        }
    } else {
        if (!say_prepare_text_plan(input, &plan, error_message)) {
            goto cleanup;
        }
    }

    if (debug_report != NULL &&
        !say_build_report(input_label != NULL ? input_label : input, input_source, &validated, &plan, debug_report, error_message)) {
        goto cleanup;
    }

    for (i = 0; i < plan.chunk_count; ++i) {
        if (!say_render_chunk(&validated, plan.chunks[i].phonemes, dry_run ? NULL : &pcm, error_message)) {
            goto cleanup;
        }
        if (!dry_run && i + 1 < plan.chunk_count) {
            size_t gap_samples = (size_t)((SAY_OUTPUT_RATE * SAY_CHUNK_GAP_MS) / 1000);
            if (!say_pcm_append_zeros(&pcm, gap_samples)) {
                say_set_errorf(error_message, "out of memory");
                goto cleanup;
            }
        }
    }

    if (dry_run) {
        ok = 1;
        goto cleanup;
    }

    if (validated.phone) {
        say_apply_phone_filter(pcm.samples, pcm.count);
    }

    if (validated.gain != 1.0) {
        say_apply_gain(pcm.samples, pcm.count, validated.gain);
    }

    encoded = say_encode_audio(&validated, &pcm, &encoded_size);
    if (encoded == NULL) {
        say_set_errorf(error_message, "encoding failure");
        goto cleanup;
    }

    if (result != NULL) {
        result->data = encoded;
        result->size = encoded_size;
        result->language = "en";
        result->format = say_format_name(validated.format);
        result->sample_rate = SAY_OUTPUT_RATE;
        result->frame_ms = validated.frame_ms;
        result->phonemes = validated.phonemes;
        result->channels = 1;
        result->bits_per_sample = 16;
        result->sample_count = pcm.count;
        result->duration_seconds = (double)pcm.count / (double)SAY_OUTPUT_RATE;
        result->pcm_encoding = validated.format == SAY_FORMAT_AIFF ? "s16be" : "s16le";
        encoded = NULL;
    }

    ok = 1;

cleanup:
    free(encoded);
    ReleaseBuffer();
    say_pcm_free(&pcm);
    say_plan_free(&plan);
    if (!ok && debug_report != NULL) {
        say_free_string(*debug_report);
        *debug_report = NULL;
    }
    return ok;
}
