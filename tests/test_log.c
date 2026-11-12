#include "log.h"
#include "test.h"
#include <stdlib.h>

int main(void)
{
    CHECK(mg_log_parse_level("error") == MG_LOG_ERROR);
    CHECK(mg_log_parse_level("WARN") == MG_LOG_WARN);
    CHECK(mg_log_parse_level("Info") == MG_LOG_INFO);
    CHECK(mg_log_parse_level("debug") == MG_LOG_DEBUG);
    CHECK(mg_log_parse_level("2") == 2);
    CHECK(mg_log_parse_level("7") == -1);
    CHECK(mg_log_parse_level("") == -1);
    CHECK(mg_log_parse_level(NULL) == -1);
    CHECK(mg_log_parse_level("bogus") == -1);

    /* Precedence: CLI > $MINER_LOG > default INFO. */
    unsetenv("MINER_LOG");
    mg_log_init("t", -1);
    CHECK(mg_log_level == MG_LOG_INFO);
    setenv("MINER_LOG", "warn", 1);
    mg_log_init("t", -1);
    CHECK(mg_log_level == MG_LOG_WARN);
    mg_log_init("t", MG_LOG_ERROR);
    CHECK(mg_log_level == MG_LOG_ERROR);

    return TEST_DONE("test_log");
}
