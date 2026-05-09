#ifndef SAY_BACKEND_H
#define SAY_BACKEND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SAY_FORMAT_RAW = 0,
    SAY_FORMAT_AIFF = 1,
    SAY_FORMAT_WAV = 2
} say_format_t;

typedef enum {
    SAY_INPUT_LITERAL = 0,
    SAY_INPUT_FILE = 1
} say_input_source_t;

typedef struct {
    const char *language;
    int sample_rate;
    int frame_ms;
    int phonemes;
    say_format_t format;
    double gain;
    int phone;
    int speed;
    int pitch;
    int mouth;
    int throat;
    int sing;
} say_options_t;

typedef struct {
    unsigned char *data;
    size_t size;
    const char *language;
    const char *format;
    int sample_rate;
    int frame_ms;
    int phonemes;
    int channels;
    int bits_per_sample;
    size_t sample_count;
    double duration_seconds;
    const char *pcm_encoding;
} say_result_t;

void say_default_options(say_options_t *options);
const char *say_format_name(say_format_t format);
int say_parse_format_name(const char *name, say_format_t *format);
int say_synthesize(
    const char *input,
    const say_options_t *options,
    int dry_run,
    say_input_source_t input_source,
    const char *input_label,
    say_result_t *result,
    char **debug_report,
    char **error_message
);
void say_free_result(say_result_t *result);
void say_free_string(char *value);

#ifdef __cplusplus
}
#endif

#endif
