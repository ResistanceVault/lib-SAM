#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "say_backend.h"

#ifdef _WIN32
#define STAT_STRUCT struct _stat
#define STAT_FUNC _stat
#else
#define STAT_STRUCT struct stat
#define STAT_FUNC stat
#endif

typedef struct {
    say_options_t options;
    const char *output_path;
    const char *debug_report_path;
    int dry_run;
    const char **positionals;
    int positional_count;
} cli_config_t;

static void print_usage(FILE *stream)
{
    fprintf(stream, "usage: tts <text-or-input-file> -o <output.{raw|aiff|wav}> [--lang en] [--rate 44100]\n");
    fprintf(stream, "       tts --phonemes \"<sam-phoneme-string>\" -o out.wav\n");
    fprintf(stream, "       tts \"Debug me\" --debug-report report.txt --dry-run\n");
    fprintf(stream, "\n");
    fprintf(stream, "flags:\n");
    fprintf(stream, "  -o, --output <path>        output path (.raw, .wav, .aiff)\n");
    fprintf(stream, "  --lang <en>                compatibility language flag\n");
    fprintf(stream, "  --rate <44100>             compatibility sample-rate flag\n");
    fprintf(stream, "  --frame-ms <5-10>          compatibility frame metadata\n");
    fprintf(stream, "  --phonemes                 interpret input as SAM phonemes\n");
    fprintf(stream, "  --debug-report <path|->    write a human-readable debug report\n");
    fprintf(stream, "  --dry-run                  validate input and skip audio output\n");
    fprintf(stream, "  --gain <number>            apply post-synthesis linear gain\n");
    fprintf(stream, "  --phone                    apply the telephone effect\n");
    fprintf(stream, "  --speed <0-255>            SAM speed parameter\n");
    fprintf(stream, "  --pitch <0-255>            SAM pitch parameter\n");
    fprintf(stream, "  --mouth <0-255>            SAM mouth parameter\n");
    fprintf(stream, "  --throat <0-255>           SAM throat parameter\n");
    fprintf(stream, "  --sing                     enable SAM sing mode\n");
    fprintf(stream, "  -h, --help                 show this help\n");
}

static int parse_int_value(const char *name, const char *value, int *out)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "error: invalid numeric value for %s\n", name);
        return 0;
    }

    *out = (int)parsed;
    return 1;
}

static int parse_double_value(const char *name, const char *value, double *out)
{
    char *end = NULL;
    double parsed;

    errno = 0;
    parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "error: invalid numeric value for %s\n", name);
        return 0;
    }

    *out = parsed;
    return 1;
}

static int is_unsupported_legacy_flag(const char *flag)
{
    return strcmp(flag, "--centralize") == 0 ||
           strcmp(flag, "--articulate") == 0 ||
           strcmp(flag, "--voice-formants") == 0 ||
           strcmp(flag, "--voice-pitch") == 0 ||
           strcmp(flag, "--amiga") == 0;
}

static int infer_format_from_output(const char *path, say_format_t *format)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return 0;
    }
    return say_parse_format_name(dot + 1, format);
}

static int path_exists(const char *path)
{
    STAT_STRUCT info;
    return path != NULL && STAT_FUNC(path, &info) == 0;
}

static char *read_text_file(const char *path)
{
    FILE *file = NULL;
    long size;
    size_t read_size;
    char *buffer;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    read_size = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read_size != (size_t)size) {
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';
    return buffer;
}

static int write_binary_file(const char *path, const unsigned char *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }

    if (size > 0 && fwrite(data, 1, size, file) != size) {
        fclose(file);
        return 0;
    }

    fclose(file);
    return 1;
}

static int write_text_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    size_t size = strlen(text);
    if (file == NULL) {
        return 0;
    }

    if (size > 0 && fwrite(text, 1, size, file) != size) {
        fclose(file);
        return 0;
    }

    fclose(file);
    return 1;
}

static char *join_positionals(const cli_config_t *config)
{
    int i;
    size_t total = 0;
    char *joined;
    char *cursor;

    for (i = 0; i < config->positional_count; ++i) {
        total += strlen(config->positionals[i]) + 1;
    }

    joined = malloc(total > 0 ? total : 1);
    if (joined == NULL) {
        return NULL;
    }

    cursor = joined;
    for (i = 0; i < config->positional_count; ++i) {
        size_t length = strlen(config->positionals[i]);
        memcpy(cursor, config->positionals[i], length);
        cursor += length;
        if (i + 1 < config->positional_count) {
            *cursor++ = ' ';
        }
    }
    *cursor = '\0';
    return joined;
}

static int parse_cli(int argc, const char **argv, cli_config_t *config)
{
    int i;

    say_default_options(&config->options);
    config->output_path = NULL;
    config->debug_report_path = NULL;
    config->dry_run = 0;
    config->positionals = argv;
    config->positional_count = 0;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(stdout);
            return -1;
        }

        if (is_unsupported_legacy_flag(arg)) {
            fprintf(stderr, "error: %s is unsupported by the SAM backend\n", arg);
            return 0;
        }

        if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: missing value for %s\n", arg);
                return 0;
            }
            config->output_path = argv[++i];
        } else if (strcmp(arg, "--debug-report") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: missing value for %s\n", arg);
                return 0;
            }
            config->debug_report_path = argv[++i];
        } else if (strcmp(arg, "--dry-run") == 0) {
            config->dry_run = 1;
        } else if (strcmp(arg, "--phonemes") == 0) {
            config->options.phonemes = 1;
        } else if (strcmp(arg, "--phone") == 0) {
            config->options.phone = 1;
        } else if (strcmp(arg, "--sing") == 0) {
            config->options.sing = 1;
        } else if (strcmp(arg, "--lang") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: missing value for %s\n", arg);
                return 0;
            }
            config->options.language = argv[++i];
        } else if (strcmp(arg, "--rate") == 0) {
            if (i + 1 >= argc || !parse_int_value(arg, argv[i + 1], &config->options.sample_rate)) {
                return 0;
            }
            ++i;
        } else if (strcmp(arg, "--frame-ms") == 0) {
            if (i + 1 >= argc || !parse_int_value(arg, argv[i + 1], &config->options.frame_ms)) {
                return 0;
            }
            ++i;
        } else if (strcmp(arg, "--gain") == 0) {
            if (i + 1 >= argc || !parse_double_value(arg, argv[i + 1], &config->options.gain)) {
                return 0;
            }
            ++i;
        } else if (strcmp(arg, "--speed") == 0) {
            if (i + 1 >= argc || !parse_int_value(arg, argv[i + 1], &config->options.speed)) {
                return 0;
            }
            ++i;
        } else if (strcmp(arg, "--pitch") == 0) {
            if (i + 1 >= argc || !parse_int_value(arg, argv[i + 1], &config->options.pitch)) {
                return 0;
            }
            ++i;
        } else if (strcmp(arg, "--mouth") == 0) {
            if (i + 1 >= argc || !parse_int_value(arg, argv[i + 1], &config->options.mouth)) {
                return 0;
            }
            ++i;
        } else if (strcmp(arg, "--throat") == 0) {
            if (i + 1 >= argc || !parse_int_value(arg, argv[i + 1], &config->options.throat)) {
                return 0;
            }
            ++i;
        } else if (arg[0] == '-') {
            fprintf(stderr, "error: unknown flag %s\n", arg);
            return 0;
        } else {
            config->positionals[config->positional_count++] = arg;
        }
    }

    if (config->positional_count == 0) {
        fprintf(stderr, "error: missing input\n");
        return 0;
    }

    if (!config->dry_run && config->output_path == NULL) {
        fprintf(stderr, "error: missing output path\n");
        return 0;
    }

    if (config->output_path != NULL) {
        if (!infer_format_from_output(config->output_path, &config->options.format)) {
            fprintf(stderr, "error: unsupported output extension\n");
            return 0;
        }
    }

    return 1;
}

int main(int argc, const char **argv)
{
    cli_config_t config;
    say_result_t result;
    char *input_text = NULL;
    char *report = NULL;
    char *error_message = NULL;
    say_input_source_t input_source = SAY_INPUT_LITERAL;
    const char *input_label = NULL;
    int ok;
    int parse_status;

    if (argc <= 1) {
        print_usage(stderr);
        return 1;
    }

    memset(&config, 0, sizeof(config));
    memset(&result, 0, sizeof(result));

    parse_status = parse_cli(argc, argv, &config);
    if (parse_status == 0) {
        return 1;
    }

    if (parse_status < 0) {
        return 0;
    }

    input_label = config.positional_count == 1 ? config.positionals[0] : NULL;

    if (!config.options.phonemes && config.positional_count == 1 && path_exists(config.positionals[0])) {
        input_text = read_text_file(config.positionals[0]);
        if (input_text == NULL) {
            fprintf(stderr, "error: failed to read input file\n");
            return 1;
        }
        input_source = SAY_INPUT_FILE;
        input_label = config.positionals[0];
    } else {
        input_text = join_positionals(&config);
        if (input_text == NULL) {
            fprintf(stderr, "error: out of memory\n");
            return 1;
        }
        input_source = SAY_INPUT_LITERAL;
        input_label = input_label != NULL ? input_label : input_text;
    }

    ok = say_synthesize(
        input_text,
        &config.options,
        config.dry_run,
        input_source,
        input_label,
        &result,
        config.debug_report_path != NULL ? &report : NULL,
        &error_message
    );

    if (!ok) {
        fprintf(stderr, "error: %s\n", error_message != NULL ? error_message : "operation failed");
        say_free_string(error_message);
        free(input_text);
        return 1;
    }

    if (config.debug_report_path != NULL) {
        if (strcmp(config.debug_report_path, "-") == 0) {
            fputs(report != NULL ? report : "", stdout);
        } else if (!write_text_file(config.debug_report_path, report != NULL ? report : "")) {
            fprintf(stderr, "error: failed to write debug report\n");
            say_free_string(report);
            say_free_result(&result);
            free(input_text);
            return 1;
        }
    }

    if (!config.dry_run) {
        if (!write_binary_file(config.output_path, result.data, result.size)) {
            fprintf(stderr, "error: failed to write output file\n");
            say_free_string(report);
            say_free_result(&result);
            free(input_text);
            return 1;
        }
    }

    say_free_string(report);
    say_free_result(&result);
    free(input_text);
    return 0;
}
