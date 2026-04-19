#ifndef ONE_MONTH_H
#define ONE_MONTH_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Embedded data from one_month.txt
 *
 * Generated at build time from the text file.
 * Total size: embedded in the array itself.
 */
static const uint8_t one_month_data[] = {
#include "one_month_txt.inc"
};

#define ONE_MONTH_DATA_SIZE (sizeof(one_month_data))

#endif /* ONE_MONTH_H */
