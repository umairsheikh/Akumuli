#include <iostream>
#include <cassert>
#include <random>
#include <tuple>
#include <map>
#include <vector>
#include <algorithm>
#include <memory>

#include <boost/timer.hpp>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <apr_mmap.h>
#include <apr_general.h>

#include "akumuli.h"
#include "page.h"
#include "storage.h"
#include "sequencer.h"

using namespace Akumuli;
using namespace std;

const int DB_SIZE = 6;
const int NUM_ITERATIONS = 100*1000*1000;
const int CHUNK_SIZE = 5000;

const char* DB_NAME = "test";
const char* DB_PATH = "./test";
const char* DB_META_FILE = "./test/test.akumuli";

void delete_storage() {
    boost::filesystem::remove_all(DB_PATH);
}

void query_database(aku_Database* db, aku_TimeStamp begin, aku_TimeStamp end, uint64_t& counter, boost::timer& timer, uint64_t mod) {
    aku_ParamId params[] = {1};
    aku_SelectQuery* query = aku_make_select_query( begin
                                                  , end
                                                  , 1, params);
    aku_Cursor* cursor = aku_select(db, query);
    aku_TimeStamp current_time = begin;
    while(!aku_cursor_is_done(cursor)) {
        int err = AKU_SUCCESS;
        if (aku_cursor_is_error(cursor, &err)) {
            std::cout << aku_error_message(err) << std::endl;
            return;
        }
        aku_Entry const* entries[1000];
        int n_entries = aku_cursor_read(cursor, entries, 1000);
        for (int i = 0; i < n_entries; i++) {
            aku_Entry const* p = entries[i];
            if (p->time != current_time) {
                std::cout << "Error at " << current_time << " expected " << current_time << " acutal " << p->time  << std::endl;
                return;
            }
            current_time++;
            counter++;
            if (counter % mod == 0) {
                std::cout << counter << " " << timer.elapsed() << "s" << std::endl;
                timer.restart();
            }
        }
    }
    aku_close_cursor(cursor);
}

void print_storage_stats(aku_StorageStats& ss) {
    std::cout << ss.n_entries << " elenents in" << std::endl
              << ss.n_volumes << " volumes with" << std::endl
              << ss.used_space << " bytes used and" << std::endl
              << ss.free_space << " bytes free" << std::endl;
}

void print_search_stats(aku_SearchStats& ss) {
    std::cout << "Interpolation search" << std::endl;
    std::cout << ss.istats.n_matches << " matches" << std::endl
              << ss.istats.n_times << " times" << std::endl
              << ss.istats.n_steps << " steps" << std::endl
              << ss.istats.n_overshoots << " overshoots" << std::endl
              << ss.istats.n_undershoots << " undershoots" << std::endl
              << ss.istats.n_reduced_to_one_page << "  reduced to page" << std::endl

              << ss.istats.n_page_in_core_checks << "  page_in_core checks" << std::endl
              << ss.istats.n_page_in_core_errors << "  page_in_core errors" << std::endl
              << ss.istats.n_pages_in_core_found  << "  page_in_core success" << std::endl
              << ss.istats.n_pages_in_core_miss  << "  page_in_core miss" << std::endl;

    std::cout << "Binary search" << std::endl;
    std::cout << ss.bstats.n_steps << " steps" << std::endl
              << ss.bstats.n_times << " times" << std::endl;

    std::cout << "Scan" << std::endl;
    std::cout << ss.scan.bwd_bytes << " bytes read in backward direction" << std::endl
              << ss.scan.fwd_bytes << " bytes read in forward direction" << std::endl;
}

enum Mode {
    NONE,
    CREATE,
    DELETE,
    READ
};

Mode read_cmd(int cnt, const char** args) {
    if (cnt < 2) {
        return NONE;
    }
    if (std::string(args[1]) == "create") {
        return CREATE;
    }
    if (std::string(args[1]) == "read") {
        return READ;
    }
    if (std::string(args[1]) == "delete") {
        return DELETE;
    }
    std::cout << "Invalid command line" << std::endl;
    std::terminate();
}

int main(int cnt, const char** args)
{
    Mode mode = read_cmd(cnt, args);

    aku_initialize();

    if (mode == DELETE) {
        delete_storage();
        std::cout << "storage deleted" << std::endl;
        return 0;
    }

    if (mode != READ) {
        // Cleanup
        delete_storage();

        // Create database
        apr_status_t result = aku_create_database(DB_NAME, DB_PATH, DB_PATH, DB_SIZE, nullptr);
        if (result != APR_SUCCESS) {
            std::cout << "Error in new_storage" << std::endl;
            return (int)result;
        }
    }

    aku_Config config;
    config.debug_mode = 0;
    config.max_late_write = 10000;
    auto db = aku_open_database(DB_META_FILE, config);
    boost::timer timer;

    if (mode != READ) {
        // Fill in data
        for(uint64_t i = 0; i < NUM_ITERATIONS; i++) {
            aku_MemRange memr;
            memr.address = (void*)&i;
            memr.length = sizeof(i);
            aku_add_sample(db, 1, i, memr);
            if (i % 1000000 == 0) {
                std::cout << i << " " << timer.elapsed() << "s" << std::endl;
                timer.restart();
            }
        }
    }

    aku_StorageStats storage_stats;
    aku_global_storage_stats(db, &storage_stats);
    print_storage_stats(storage_stats);

    if (mode != CREATE) {
        // Search
        std::cout << "Sequential access" << std::endl;
        aku_SearchStats search_stats;
        uint64_t counter = 0;
        /*
        timer.restart();
        query_database( db
                      , std::numeric_limits<aku_TimeStamp>::min()
                      , std::numeric_limits<aku_TimeStamp>::max()
                      , counter
                      , timer
                      , 1000000);

        aku_global_search_stats(&search_stats, true);
        print_search_stats(search_stats);
        */

        // Random access
        std::cout << "Prepare test data" << std::endl;
        std::vector<std::pair<aku_TimeStamp, aku_TimeStamp>> ranges;
        for (aku_TimeStamp i = 1u; i < (aku_TimeStamp)NUM_ITERATIONS/CHUNK_SIZE; i++) {
            std::vector<aku_TimeStamp> range;
            aku_TimeStamp j = (i - 1)*CHUNK_SIZE;
            //aku_TimeStamp j = CHUNK_SIZE*10;  // Fixed position
            std::generate_n(std::back_inserter(range), CHUNK_SIZE, [&j]() {return j++;});
            std::random_shuffle(range.begin(), range.end());
            int count = 5;
            for (auto k: range) {
                ranges.push_back(std::make_pair(k, k+1));
                if (!count--) {
                    break;
                }
            }
        }

        std::random_shuffle(ranges.begin(), ranges.end());

        std::cout << "Random access" << std::endl;
        counter = 0;
        timer.restart();
        for(auto range: ranges) {
            query_database(db, range.first, range.second, counter, timer, 10000);
        }
        aku_global_search_stats(&search_stats, true);
        print_search_stats(search_stats);
    }

    aku_close_database(db);

    if (mode == NONE) {
        delete_storage();
    }
    return 0;
}

