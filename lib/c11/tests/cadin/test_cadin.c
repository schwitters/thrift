/* SPDX-License-Identifier: Apache-2.0 */
#include "cpro_integra_common_types.h"
#include "cpro_integra_bom_types.h"
#include "cpro_integra_mdmp_types.h"
#include "cpro_integra_mdmp_bulk_types.h"
#include "cpro_integra_nodes_types.h"
#include "cpro_integra_nodes_index_types.h"
#include "cpro_integra_order_types.h"
#include "raw_record_types.h"
#include <stdlib.h>

/* All generated objects are linked, including client and server entry points. */
int main(void)
{
    struct cpro_integra_nodes_extended_attribute value = {0};
    enum thrift_status status = cpro_integra_nodes_extended_attribute_init(&value);
    if (status == THRIFT_OK)
        value.f_extended_type = CPRO_INTEGRA_NODES_EXTENDED_ATTRIBUTE_TYPE_CALCULATED;
    cpro_integra_nodes_extended_attribute_clear(&value);
    return status == THRIFT_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
