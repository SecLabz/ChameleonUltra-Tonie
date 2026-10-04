#pragma once

typedef enum {
    RESULT_OK,
    RESULT_INVALID_DATA,
    RESULT_NOT_FOUND,
    RESULT_LIBRARY_FULL,
    RESULT_STORAGE_ERROR,
    RESULT_RF_BUSY
} operation_result_t;
const char *operation_result_name(operation_result_t result);
