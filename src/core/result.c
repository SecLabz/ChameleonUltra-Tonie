#include "result.h"

const char *operation_result_name(operation_result_t r) {
    static const char *n[] = {"ok",           "invalid-data",  "not-found",
                              "storage-full", "storage-error", "busy"};
    return r <= RESULT_RF_BUSY ? n[r] : "error";
}
