#define _POSIX_C_SOURCE 200809L
/* Other sources in this target rename only their allocation calls. */
#undef calloc
#undef realloc
#include "search_progress.h"
#include "search_sampling_internal.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static atomic_int remaining = ATOMIC_VAR_INIT(-1);
static atomic_uint failures = ATOMIC_VAR_INIT(0);

static bool fail_allocation(void)
{
    int value = atomic_load(&remaining);
    while (value >= 0)
        if (atomic_compare_exchange_weak(&remaining, &value, value - 1))
        {
            if (value == 0)
            {
                atomic_fetch_add(&failures, 1);
                return true;
            }
            return false;
        }
    return false;
}

void *xq_test_calloc(size_t count, size_t size)
{
    return fail_allocation() ? NULL : calloc(count, size);
}

void *xq_test_realloc(void *pointer, size_t size)
{
    return fail_allocation() ? NULL : realloc(pointer, size);
}

int main(int argc, char **argv)
{
    if (argc != 2) return EXIT_FAILURE;
    XqPosition position;
    xq_position_startpos(&position);
    for (int failure = 0; failure < 22; ++failure)
    {
        atomic_store(&remaining, failure);
        CliSearchProgress *display = cli_search_progress_start_sampling(
            1, 0, false, 1, 1, argv[1], &position, "allocation-test");
        XqSearchSampler *sampler = cli_search_progress_sampler(display);
        if (sampler != NULL)
        {
            XqSamplingThread *thread = xq_sampling_thread(sampler, 0);
            xq_sampling_begin(thread, 1, cli_search_progress_update, display);
            XqMove move = {0, 1, 0, -1};
            for (unsigned i = 0; i < XQ_SEARCH_SAMPLE_MAX_MOVES; ++i)
                xq_sampling_push(thread, move, true);
            xq_search_sampler_request(sampler);
            xq_sampling_checkpoint(thread, true);
            struct timespec delay = {0, 5000000};
            nanosleep(&delay, NULL);
            for (unsigned i = 0; i < XQ_SEARCH_SAMPLE_MAX_MOVES; ++i)
                xq_sampling_pop(thread, true);
            xq_sampling_state(thread, XQ_SEARCH_THREAD_IDLE);
        }
        cli_search_progress_finish(display, "allocation test", NULL);
    }
    unsigned count = atomic_load(&failures);
    if (count < 12)
    {
        fprintf(stderr, "too few allocation failures exercised: %u\n", count);
        return EXIT_FAILURE;
    }
    printf("sampling allocation failure tests passed (%u failures)\n", count);
    return EXIT_SUCCESS;
}
