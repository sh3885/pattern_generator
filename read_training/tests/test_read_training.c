#include "read_training.h"

#include <assert.h>
#include <string.h>

static void test_trim_line_removes_outer_whitespace(void)
{
    char buffer[32];
    size_t written = 0U;
    RtStatus status;

    status = rt_trim_line("  abc\t\n", buffer, sizeof(buffer), &written);

    assert(status == RT_OK);
    assert(strcmp(buffer, "abc") == 0);
    assert(written == 4U);
}

static void test_trim_line_reports_required_size(void)
{
    size_t written = 0U;
    RtStatus status;

    status = rt_trim_line("  abc  ", NULL, 0U, &written);

    assert(status == RT_OK);
    assert(written == 4U);
}

static void test_trim_line_rejects_small_buffer(void)
{
    char buffer[2];
    size_t written = 0U;
    RtStatus status;

    status = rt_trim_line("abc", buffer, sizeof(buffer), &written);

    assert(status == RT_ERROR_BUFFER_TOO_SMALL);
    assert(written == 4U);
}

static void test_trim_line_rejects_null_input(void)
{
    assert(rt_trim_line(NULL, NULL, 0U, NULL) == RT_ERROR_INVALID_ARGUMENT);
}

int main(void)
{
    test_trim_line_removes_outer_whitespace();
    test_trim_line_reports_required_size();
    test_trim_line_rejects_small_buffer();
    test_trim_line_rejects_null_input();

    return 0;
}

