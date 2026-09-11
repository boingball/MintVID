#include "../core/mr_http.h"
#include "../core/mr_source.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_PATH "/tmp/mr_source_buffer_check.bin"
#define FAST_BUFFER_BYTES (4u * 1024u * 1024u)

int main(int argc, char **argv)
{
    static const unsigned char expected[] = {
        0x4d, 0x69, 0x6e, 0x74, 0x56, 0x49, 0x44, 0x2d,
        0x62, 0x75, 0x66, 0x66, 0x65, 0x72, 0x0a
    };
    unsigned char got[sizeof expected];
    mr_http_options options;
    mr_source *source;
    FILE *file;

    if (argc == 2) {
        assert(mr_http_options_init(&options, NULL, NULL));
        options.source_buffer_bytes = 8u * 1024u * 1024u;
        source = mr_source_open_ex(argv[1], &options);
        assert(source);
        assert(mr_source_buffer_capacity(source) == 8u * 1024u * 1024u);
        assert(mr_source_length(source) != MR_SOURCE_LEN_UNKNOWN);
        assert(mr_source_length(source) >= sizeof got);
        assert(mr_source_read_at(source, 0, got, sizeof got));
        assert(mr_source_read_at(source,
                                 mr_source_length(source) - sizeof got,
                                 got, sizeof got));
        mr_source_close(source);
        puts("progressive HTTP Fast RAM buffer checks passed");
        return 0;
    }
    assert(argc == 1);
    file = fopen(TEST_PATH, "wb");

    assert(file);
    assert(fwrite(expected, 1, sizeof expected, file) == sizeof expected);
    assert(fclose(file) == 0);

    assert(mr_http_options_init(&options, NULL, NULL));
    options.source_buffer_bytes = FAST_BUFFER_BYTES;
    source = mr_source_open_ex(TEST_PATH, &options);
    assert(source);
    assert(mr_source_buffer_capacity(source) == FAST_BUFFER_BYTES);
    assert(mr_source_read_at(source, 0, got, sizeof got));
    assert(!memcmp(got, expected, sizeof got));
    assert(mr_source_read_at(source, 8, got, 6));
    assert(!memcmp(got, expected + 8, 6));
    mr_source_close(source);

    source = mr_source_open(TEST_PATH);
    assert(source);
    assert(mr_source_buffer_capacity(source) == 0);
    mr_source_close(source);
    remove(TEST_PATH);
    puts("source Fast RAM buffer checks passed");
    return 0;
}
