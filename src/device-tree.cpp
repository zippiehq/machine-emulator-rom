// Copyright 2019 Cartesi Pte. Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
//

#include <pma-defines.h>
#include <rtc-defines.h>

extern "C" {
#include <libfdt.h>
};

#include "rom-defines.h"
#include "device-tree.h"
#include "util.h"

#define FDT_CHECK(func_call) do { \
    int errval = (func_call); \
    if (errval != 0) \
        return errval; \
} while (0);

#define PMA_VALUE(x) ((x >> 12) << 12)
#define PMA_DID(x) ((x << 52) >> 60)

#define MISA_BIT(x) (1 << ((x) - 'A'))

struct pma {
    uint64_t istart;
    uint64_t ilength;
};

static int fdt_begin_node_num(void *fdt, const char *name, uint64_t num) {
    char name_num[256];
    int ret, len  = strnlen(name, sizeof(name_num) - 18);// hex rep + @ + \0

    memcpy(name_num, name, len);
    name_num[len] = '@';

    if ((ret = ulltoa(&name_num[len+1], num, 16)) < 0)
            return ret;

    return fdt_begin_node(fdt, name_num);
}

static int fdt_property_u64_u64(void *fdt, const char *name, uint64_t v0,
        uint64_t v1) {
    uint32_t tab[4];
    tab[0] = cpu_to_fdt32(v0 >> 32);
    tab[1] = cpu_to_fdt32(v0);
    tab[2] = cpu_to_fdt32(v1 >> 32);
    tab[3] = cpu_to_fdt32(v1);
    return fdt_property(fdt, name, tab, sizeof(tab));
}

static void parse_misa(char *str, uint64_t misa)
{
    static char prefix[] = "rv64";
    for (int j = 0; j < sizeof(prefix) - 1; j++)
        *str++ = prefix[j];

    #ifdef RISCV_SUPPORT_FD_EMULATION
    misa |= MISA_BIT('F') | MISA_BIT('D');
    #endif

    for(int i = 0; i < 26; i++) {
        if (misa & (1 << i))
            *str++ = 'a' + i;
    }
    *str = '\0';
}


int build_device_tree(struct pma *pma, const char *bootargs,
        uint64_t misa, void *buf, uint64_t buflen)
{
    int cur_phandle = 1;
    char isa_string[128];

    parse_misa(isa_string, misa);

    FDT_CHECK(fdt_create(buf, buflen));
    FDT_CHECK(fdt_add_reservemap_entry(buf, 0, 0));
    FDT_CHECK(fdt_finish_reservemap(buf));
    FDT_CHECK(fdt_begin_node(buf, ""));
     FDT_CHECK(fdt_property_u32(buf, "#address-cells", 2));
     FDT_CHECK(fdt_property_u32(buf, "#size-cells", 2));
     FDT_CHECK(fdt_property_string(buf, "compatible", "ucbbar,riscvemu-bar_dev"));
     FDT_CHECK(fdt_property_string(buf, "model", "ucbbar,riscvemu-bare"));
     FDT_CHECK(fdt_begin_node(buf, "cpus"));
      FDT_CHECK(fdt_property_u32(buf, "#address-cells", 1));
      FDT_CHECK(fdt_property_u32(buf, "#size-cells", 0));
      FDT_CHECK(fdt_property_u32(buf, "timebase-frequency", CLOCK_FREQ/RTC_FREQ_DIV_DEF));
      FDT_CHECK(fdt_begin_node_num(buf, "cpu", 0));
       FDT_CHECK(fdt_property_string(buf, "device_type", "cpu"));
       FDT_CHECK(fdt_property_u32(buf, "reg", 0));
       FDT_CHECK(fdt_property_string(buf, "status", "okay"));
       FDT_CHECK(fdt_property_string(buf, "compatible", "riscv"));
       FDT_CHECK(fdt_property_string(buf, "riscv,isa", isa_string));
       FDT_CHECK(fdt_property_string(buf, "mmu-type", "riscv,sv39"));
       FDT_CHECK(fdt_property_u32(buf, "clock-frequency", CLOCK_FREQ));
       FDT_CHECK(fdt_begin_node(buf, "interrupt-controller"));
        FDT_CHECK(fdt_property_u32(buf, "#interrupt-cells", 1));
        FDT_CHECK(fdt_property(buf, "interrupt-controller", NULL, 0));
        FDT_CHECK(fdt_property_string(buf, "compatible", "riscv,cpu-intc"));
        int intc_phandle = cur_phandle++;
        FDT_CHECK(fdt_property_u32(buf, "phandle", intc_phandle));
       FDT_CHECK(fdt_end_node(buf)); /* interrupt-controller */
      FDT_CHECK(fdt_end_node(buf)); /* cpu */
     FDT_CHECK(fdt_end_node(buf)); /* cpus */

     FDT_CHECK(fdt_begin_node_num(buf, "memory", PMA_VALUE(pma->istart)));
      FDT_CHECK(fdt_property_string(buf, "device_type", "memory"));
      FDT_CHECK(fdt_property_u64_u64(buf, "reg", PMA_VALUE(pma->istart),
                  PMA_VALUE(pma->ilength) - DEVICE_TREE_MAX_SIZE));
     FDT_CHECK(fdt_end_node(buf)); /* memory */

    /* flash */
    struct pma *pma_entry = NULL;
    uint64_t start = UINT64_MAX;
    uint64_t length = UINT64_MAX;
    uint64_t rx_buffer_start = 0, rx_buffer_length = 0;
    uint64_t tx_buffer_start = 0, tx_buffer_length = 0;
    uint64_t input_metadata_start = 0, input_metadata_length = 0;
    uint64_t voucher_hashes_start = 0, voucher_hashes_length = 0;
    uint64_t notice_hashes_start = 0, notice_hashes_length = 0;
    uint8_t pma_did;
    int ret, j = 0;
    char mtd_name[64] = "flash.";
    for (int i = 0; i < PMA_MAX_DEF; i++) {
        pma_entry = &pma[i];
        pma_did = PMA_DID(pma_entry->istart);

        if (pma_did == PMA_FLASH_DRIVE_DID_DEF) {
            start = PMA_VALUE(pma_entry->istart);
            length = PMA_VALUE(pma_entry->ilength);

            if ((ret = ulltoa(&mtd_name[6], j, 10)) < 0)
                return ret;

            FDT_CHECK(fdt_begin_node_num(buf, "flash", start));
             FDT_CHECK(fdt_property_u32(buf, "#address-cells", 2));
             FDT_CHECK(fdt_property_u32(buf, "#size-cells", 2));
             FDT_CHECK(fdt_property_string(buf, "compatible", "mtd-ram"));
             FDT_CHECK(fdt_property_u32(buf, "bank-width", 4));
             FDT_CHECK(fdt_property_u64_u64(buf, "reg", start, length));
             FDT_CHECK(fdt_property_string(buf, "linux,mtd-name", mtd_name));
            FDT_CHECK(fdt_end_node(buf)); /* flash */

            j++;
        } else if (pma_did == PMA_DHD_DID_DEF) {
            start = PMA_VALUE(pma_entry->istart);
            length = PMA_VALUE(pma_entry->ilength);

            FDT_CHECK(fdt_begin_node_num(buf, "dehash", start));
             FDT_CHECK(fdt_property_string(buf, "compatible", "ctsi-dhd"));
             FDT_CHECK(fdt_property_u64_u64(buf, "reg", start, length));
            FDT_CHECK(fdt_end_node(buf)); /* dehash */
        } else if (pma_did == PMA_ROLLUP_RX_BUFFER_DID_DEF) {
            rx_buffer_start = PMA_VALUE(pma_entry->istart);
            rx_buffer_length = PMA_VALUE(pma_entry->ilength);
        } else if (pma_did == PMA_ROLLUP_TX_BUFFER_DID_DEF) {
            tx_buffer_start = PMA_VALUE(pma_entry->istart);
            tx_buffer_length = PMA_VALUE(pma_entry->ilength);
        } else if (pma_did == PMA_ROLLUP_INPUT_METADATA_DID_DEF) {
            input_metadata_start = PMA_VALUE(pma_entry->istart);
            input_metadata_length = PMA_VALUE(pma_entry->ilength);
        } else if (pma_did == PMA_ROLLUP_VOUCHER_HASHES_DID_DEF) {
            voucher_hashes_start = PMA_VALUE(pma_entry->istart);
            voucher_hashes_length = PMA_VALUE(pma_entry->ilength);
        } else if (pma_did == PMA_ROLLUP_NOTICE_HASHES_DID_DEF) {
            notice_hashes_start = PMA_VALUE(pma_entry->istart);
            notice_hashes_length = PMA_VALUE(pma_entry->ilength);
        }
    }

    if (tx_buffer_length != 0 && rx_buffer_length != 0 && input_metadata_length != 0 &&
        voucher_hashes_length != 0 && notice_hashes_length != 0) {
        FDT_CHECK(fdt_begin_node(buf, "rollup"));
         FDT_CHECK(fdt_property_u32(buf, "#address-cells", 2));
         FDT_CHECK(fdt_property_u32(buf, "#size-cells", 2));
         FDT_CHECK(fdt_property_string(buf, "compatible", "ctsi-rollup"));
         FDT_CHECK(fdt_begin_node_num(buf, "rx_buffer", rx_buffer_start));
          FDT_CHECK(fdt_property_u64_u64(buf, "reg", rx_buffer_start, rx_buffer_length));
         FDT_CHECK(fdt_end_node(buf)); /* rollup rx buffer */
         FDT_CHECK(fdt_begin_node_num(buf, "tx_buffer", tx_buffer_start));
          FDT_CHECK(fdt_property_u64_u64(buf, "reg", tx_buffer_start, tx_buffer_length));
         FDT_CHECK(fdt_end_node(buf)); /* rollup tx buffer */
         FDT_CHECK(fdt_begin_node_num(buf, "input_metadata", input_metadata_start));
          FDT_CHECK(fdt_property_u64_u64(buf, "reg", input_metadata_start, input_metadata_length));
         FDT_CHECK(fdt_end_node(buf)); /* rollup input metadata */
         FDT_CHECK(fdt_begin_node_num(buf, "voucher_hashes", voucher_hashes_start));
          FDT_CHECK(fdt_property_u64_u64(buf, "reg", voucher_hashes_start, voucher_hashes_length));
         FDT_CHECK(fdt_end_node(buf)); /* rollup voucher hashes */
         FDT_CHECK(fdt_begin_node_num(buf, "notice_hashes", notice_hashes_start));
          FDT_CHECK(fdt_property_u64_u64(buf, "reg", notice_hashes_start, notice_hashes_length));
         FDT_CHECK(fdt_end_node(buf)); /* rollup notice hashes */
        FDT_CHECK(fdt_end_node(buf)); /* rollup */
    }

     FDT_CHECK(fdt_begin_node(buf, "soc"));
      FDT_CHECK(fdt_property_u32(buf, "#address-cells", 2));
      FDT_CHECK(fdt_property_u32(buf, "#size-cells", 2));
      const char comp[] = "ucbbar,riscvemu-bar-soc\0simple-bus";
      FDT_CHECK(fdt_property(buf, "compatible", comp, sizeof(comp)));
      FDT_CHECK(fdt_property(buf, "ranges", NULL, 0));

      FDT_CHECK(fdt_begin_node_num(buf, "clint", PMA_CLINT_START_DEF));
       FDT_CHECK(fdt_property_string(buf, "compatible", "riscv,clint0"));
       uint32_t clint[] = {
           cpu_to_fdt32(intc_phandle),
           cpu_to_fdt32(3), /* M IPI irq */
           cpu_to_fdt32(intc_phandle),
           cpu_to_fdt32(7) /* M timer irq */
       };
       FDT_CHECK(fdt_property(buf, "interrupts-extended", clint, sizeof(clint)));
       FDT_CHECK(fdt_property_u64_u64(buf, "reg", PMA_CLINT_START_DEF, PMA_CLINT_LENGTH_DEF));
      FDT_CHECK(fdt_end_node(buf)); /* clint */

      FDT_CHECK(fdt_begin_node_num(buf, "htif", PMA_HTIF_START_DEF));
       FDT_CHECK(fdt_property_string(buf, "compatible", "ucb,htif0"));
       FDT_CHECK(fdt_property_u64_u64(buf, "reg", PMA_HTIF_START_DEF, PMA_HTIF_LENGTH_DEF));
       uint32_t htif[] = {
           cpu_to_fdt32(intc_phandle),
           cpu_to_fdt32(13) // X HOST
       };
       FDT_CHECK(fdt_property(buf, "interrupts-extended", htif, sizeof(htif)));
      FDT_CHECK(fdt_end_node(buf));

     FDT_CHECK(fdt_end_node(buf)); /* soc */

     FDT_CHECK(fdt_begin_node(buf, "chosen"));
      FDT_CHECK(fdt_property_string(buf, "bootargs", bootargs));
     FDT_CHECK(fdt_end_node(buf));

    FDT_CHECK(fdt_end_node(buf)); /* root */
    FDT_CHECK(fdt_finish(buf));

    return 0;
}
