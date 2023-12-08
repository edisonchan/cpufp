#include "table.hpp"
#include "smtl.hpp"

#include <unistd.h>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <cstdint>
#include <vector>
#include <sstream>
#include <iomanip>

using namespace std;

extern "C"
{
#ifdef _ASIMD_
    void asimd_fmla_vs_f32f32f32(int64_t);
    void asimd_fmla_vv_f32f32f32(int64_t);
    void asimd_fmla_vs_f64f64f64(int64_t);
    void asimd_fmla_vv_f64f64f64(int64_t);
#endif

#ifdef _ASIMDHP_
    void asimd_fmla_vs_fp16fp16fp16(int64_t);
    void asimd_fmla_vv_fp16fp16fp16(int64_t);
#endif

#ifdef _ASIMDDP_
#endif
}

typedef struct
{
    std::string isa;
    std::string type;
    std::string dim;
    int64_t loop_time;
    int64_t comp_pl;
    void (*bench)(int64_t);
} cpubm_t;
static vector<cpubm_t> bm_list;

static double get_time(struct timespec *start,
	struct timespec *end)
{
	return end->tv_sec - start->tv_sec +
		(end->tv_nsec - start->tv_nsec) * 1e-9;
}

static void reg_new_isa(std::string isa,
    std::string type,
    std::string dim,
    int64_t loop_time,
    int64_t comp_pl,
    void (*bench)(int64_t))
{
    cpubm_t new_one;
    new_one.isa = isa;
    new_one.type = type;
    new_one.dim = dim;
    new_one.loop_time = loop_time;
    new_one.comp_pl = comp_pl;
    new_one.bench = bench;

    bm_list.push_back(new_one);
}

static void thread_func(void *params)
{
    cpubm_t *bm = (cpubm_t*)params;
    bm->bench(bm->loop_time);
}

static void cpubm_x86_one(smtl_handle sh,
    cpubm_t &item,
    Table &table)
{
    struct timespec start, end;
    double time_used, perf;

    int i;
    int num_threads = smtl_num_threads(sh);

	// warm up
	for (i = 0; i < num_threads; i++)
	{
		smtl_add_task(sh, thread_func, (void*)&item);
	}
	smtl_begin_tasks(sh);
	smtl_wait_tasks_finished(sh);

	clock_gettime(CLOCK_MONOTONIC_RAW, &start);
	for (i = 0; i < num_threads; i++)
	{
		smtl_add_task(sh, thread_func, (void*)&item);
	}
	smtl_begin_tasks(sh);
	smtl_wait_tasks_finished(sh);
	clock_gettime(CLOCK_MONOTONIC_RAW, &end);

	time_used = get_time(&start, &end);
	perf = item.loop_time * item.comp_pl * num_threads /
        time_used * 1e-9;
    
    stringstream ss;
    ss << std::setprecision(5) << perf << " " << item.dim;

    vector<string> cont;
    cont.resize(3);
    cont[0] = item.isa;
    cont[1] = item.type;
    cont[2] = ss.str();
    table.addOneItem(cont);
}

static void cpubm_do_bench(std::vector<int> &set_of_threads,
    uint32_t idle_time)
{
    int i;

    int num_threads = set_of_threads.size();

    printf("Number Threads: %d\n", num_threads);
    printf("Thread Pool Binding:");
    for (i = 0; i < num_threads; i++)
    {
        printf(" %d", set_of_threads[i]);
    }
    printf("\n");

    // set table head
    vector<string> ti;
    ti.resize(3);
    ti[0] = "Instruction Set";
    ti[1] = "Core Computation";
    ti[2] = "Peak Performance";
    
    Table table;
    table.setColumnNum(3);
    table.addOneItem(ti);

    // set thread pool
    smtl_handle sh;
	smtl_init(&sh, set_of_threads);

    // traverse task list
    for (i = 0; i < bm_list.size() - 1; i++)
    {
        cpubm_x86_one(sh, bm_list[i], table);
        sleep(idle_time);
    }
    if (bm_list.size() > 0)
    {
        cpubm_x86_one(sh, bm_list[i], table);
    }

    table.print();

    smtl_fini(sh);
}

static void parse_thread_pool(char *sets,
    vector<int> &set_of_threads)
{
    if (sets[0] != '[')
    {
        return;
    }
    int pos = 1;
    int left = 0, right = 0;
    int state = 0;
    while (sets[pos] != ']' && sets[pos] != '\0')
    {
        if (state == 0)
        {
            if (sets[pos] >= '0' && sets[pos] <= '9')
            {
                left *= 10;
                left += (int)(sets[pos] - '0');
            }
            else if (sets[pos] == ',')
            {
                set_of_threads.push_back(left);
                left = 0;
            }
            else if (sets[pos] == '-')
            {
                right = 0;
                state = 1;
            }
        }
        else if (state == 1)
        {
            if (sets[pos] >= '0' && sets[pos] <= '9')
            {
                right *= 10;
                right += (int)(sets[pos] - '0');
            }
            else if (sets[pos] == ',')
            {
                int i;
                for (i = left; i <= right; i++)
                {
                    set_of_threads.push_back(i);
                }
                left = 0;
                state = 0;
            }
        }
        pos++;
    }
    if (sets[pos] != ']')
    {
        return;
    }
    if (state == 0)
    {
        set_of_threads.push_back(left);
    }
    else if (state == 1)
    {
        int i;
        for (i = left; i <= right; i++)
        {
            set_of_threads.push_back(i);
        }
    }
}

static void cpufp_register_isa()
{
#ifdef _ASIMDHP_
    reg_new_isa("asimdhp", "fmla.vs(fp16,fp16,fp16)", "GFLOPS",
        0x10000000LL, 384LL, asimd_fmla_vs_fp16fp16fp16);
    reg_new_isa("asimdhp", "fmla.vv(fp16,fp16,fp16)", "GFLOPS",
        0x10000000LL, 384LL, asimd_fmla_vv_fp16fp16fp16);
#endif

#ifdef _ASIMDHP_
    reg_new_isa("asimd", "fmla.vs(f32,f32,f32)", "GFLOPS",
        0x10000000LL, 192LL, asimd_fmla_vs_f32f32f32);
    reg_new_isa("asimd", "fmla.vv(f32,f32,f32)", "GFLOPS",
        0x10000000LL, 192LL, asimd_fmla_vv_f32f32f32);
    reg_new_isa("asimd", "fmla.vs(fp16,fp16,fp16)", "GFLOPS",
        0x10000000LL, 96LL, asimd_fmla_vs_f64f64f64);
    reg_new_isa("asimd", "fmla.vv(fp16,fp16,fp16)", "GFLOPS",
        0x10000000LL, 96LL, asimd_fmla_vv_f64f64f64);
#endif
}

int main(int argc, char *argv[])
{
    vector<int> set_of_threads;
    uint32_t idle_time = 0;

    bool params_enough = false;

    int i;
    for (i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "--thread_pool=", 14) == 0)
        {
            parse_thread_pool(argv[i] + 14, set_of_threads);
            params_enough = true;
        }
        else if (strncmp(argv[i], "--idle_time=", 12) == 0)
        {
            idle_time = (uint32_t)atoi(argv[i] + 12);
        }
    }
    if (!params_enough)
    {
        fprintf(stderr, "Error: You must set --thread_pool parameter.\n");
        fprintf(stderr, "You may also set --idle_time parameter.\n");
        fprintf(stderr, "Usage: %s --thread_pool=[xxx] --idle_time=yyy\n", argv[0]);
        fprintf(stderr, "[xxx] indicates all cores to benchmark.\n");
        fprintf(stderr, "Example: [0,3,5-8,13-15].\n");
        fprintf(stderr, "idle_time is the interval time(s) between every two benchmarks.\n");
        fprintf(stderr, "idle_time parameter can be ignored, the default value is 0s.\n");
        fprintf(stderr, "Notice: there must NOT be any spaces.\n");
        exit(0);
    }

    cpufp_register_isa();
    cpubm_do_bench(set_of_threads, idle_time);

    return 0;
}
