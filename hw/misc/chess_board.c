#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "qemu/main-loop.h" /* iothread mutex */
#include "qemu/module.h"
#include "qapi/visitor.h"

#define TYPE_CHESS_BOARD "chess-board"
#define CHESS_BAR1_NAME "chess-board-BAR1"

#define CHESS_REG_COMMAND 0x0
#define CHESS_REG_DMA_SRC_L 0x4
#define CHESS_REG_DMA_SRC_H 0x8
#define CHESS_REG_DMA_DST_L 0xc
#define CHESS_REG_DMA_DST_H 0x10
#define CHESS_REG_DMA_SIZE 0x14
#define CHESS_REG_INT_STATUS 0x18
#define CHESS_REG_GENERAL 0x1c

#define CHESS_CMD_DMA_READ 1
#define CHESS_CMD_DMA_WRITE 2

#define CHESS_DMA_INFO_SIZE 5 

#define CHESS_DMA_SRC_L 0
#define CHESS_DMA_SRC_H 1
#define CHESS_DMA_DST_L 2
#define CHESS_DMA_DST_H 3
#define CHESS_DMA_SIZE 4

#define CHESS_MMIO_SIZE 0x1000
#define CHESS_RAM_SIZE 0x1000

#define CHESS_INTERRUPT_STATUS_DMA_END 1


DECLARE_INSTANCE_CHECKER(struct chess_board_state,CHESS_BOARD,TYPE_CHESS_BOARD)

struct chess_board_state {
        PCIDevice parent_pci;
        /*
        MemoryRegion struct only defines a memory region size and callbacks. It
        will not have an address until the OS assigns one. When I call
        pci_register_bar(), I am registering the memory region size in one BAR.
        So, before OS assigns an address, BAR have only a size. The OS will 
        overwrite the BAR with an address (in physical address space) that the
        memory controller (northbridge) will intercept and redirect to PCI 
        device.
        */
        MemoryRegion mmio_region; 

        /*
        This is the device RAM. There are no ops for this MemoryRegion. It still
        is mapped on the host physical memory address space, and its address is
        set in the same way as the mmio region
        */
        MemoryRegion dev_ram_region;


        // registers
        uint32_t command;
        uint32_t reg;
        uint32_t dma_info[CHESS_DMA_INFO_SIZE];

        uint32_t interrupt_status;

        uint8_t *buff_gmalloc;
};

        
/*
@rw: 1 - read | 2 - write
*/
static void chess_do_dma (struct chess_board_state *cbs, int rw)
{
        MemTxResult ret = MEMTX_OK;

        /*
        volatile uint8_t* dev_ram_ptr =
                memory_region_get_ram_ptr (&cbs->dev_ram_region);
        */


        /*
        if read, the dst is an offset inside dev RAM region and src is DMA
                address. Write is the opposite

        DMA address, in simpler old systems, was the same as physical address.
        However, with IOMMU, this address need to be translated do physical 
        address.
        In qemu, looks like there is no IOMMU. Even if I put the kernel
        command line option "intel_iommu=on", pass "-cpu host" on qemu command 
        line, looks like there is no address translation
        */
        dma_addr_t src =
                (dma_addr_t)((((uint64_t)cbs->dma_info[CHESS_DMA_SRC_H]) << 32)
                                | cbs->dma_info[CHESS_DMA_SRC_L]);

        dma_addr_t dst =
                (dma_addr_t)(((uint64_t)(cbs->dma_info[CHESS_DMA_DST_H]) << 32)
                                | cbs->dma_info[CHESS_DMA_DST_L]);

        dma_addr_t size = cbs->dma_info[CHESS_DMA_SIZE];


        switch (rw) {
                case 1: // read
                        printf("Performing DMA host -> device\n");
                        /*
                        ret = pci_dma_read (&cbs->parent_pci, src,
                                        (void*)(dev_ram_ptr + dst), size);
                        */

                        ret = pci_dma_read (&cbs->parent_pci, src,
                                        (void*)(cbs->buff_gmalloc + dst), size);
                        if (ret != MEMTX_OK) {
                                printf ("[CHESS-BOARD] error on DMA read\n");
                                return;
                        }
                        break;

                case 2: // write
                        printf("Performing DMA device -> host\n");
                        /*
                        ret = pci_dma_write (&cbs->parent_pci, dst,
                                        (void*)(dev_ram_ptr + src), size);
                        */
                        ret = pci_dma_write (&cbs->parent_pci, dst,
                                        (void*)(cbs->buff_gmalloc + src), size);
                        if (ret != MEMTX_OK) {
                                printf ("[CHESS-BOARD] error on DMA write\n");
                                return;
                        }
                        break;
                default:
                        return;
        }

        // assert interrupt to signal the end of DMA
        cbs->interrupt_status = rw;
        
        if (msi_enabled (&cbs->parent_pci))
        {
                printf ("[CHESS-BOARD] sending MSI\n");
                msi_notify (&cbs->parent_pci, 0);
        }
        else {
                // fallback to INTx
                printf ("[CHESS-BOARD] asserting IRQ\n");
                // assert electrical signal
                pci_set_irq (&cbs->parent_pci, 1);
        }
        
}


static uint64_t chess_board_mmio_read (void *opaque, hwaddr addr,
                unsigned int size)
{
        struct chess_board_state *cbs = opaque;
        uint64_t ret = ~0ULL;
        
        printf ("[CHESS-BOARD] reading %d bytes from addr %lx\n", size, addr);
        if (size > 4)
                return ret;


        switch (addr)
        {
                case CHESS_REG_COMMAND:
                        ret = cbs->command;
                        break;

                case CHESS_REG_DMA_SRC_L:
                        ret = cbs->dma_info[CHESS_DMA_SRC_L];
                        break;

                case CHESS_REG_DMA_SRC_H:
                        ret = cbs->dma_info[CHESS_DMA_SRC_H];
                        break;

                case CHESS_REG_DMA_DST_L:
                        ret = cbs->dma_info[CHESS_DMA_DST_L];
                        break;

                case CHESS_REG_DMA_DST_H:
                        ret = cbs->dma_info[CHESS_DMA_DST_H];
                        break;

                case CHESS_REG_DMA_SIZE:
                        ret = cbs->dma_info[CHESS_DMA_SIZE];
                        break;

                case CHESS_REG_INT_STATUS:
                        // reading interrupt_status
                        ret = cbs->interrupt_status;
                        cbs->interrupt_status = 0;

                        if (!msi_enabled (&cbs->parent_pci)) {
                                // deasserting IRQ signal
                                printf ("[CHESS-BOARD] deasserting IRQ\n");
                                pci_set_irq (&cbs->parent_pci, 0);
                        }
                        break;

                case CHESS_REG_GENERAL:
                        // reading reg
                        ret = cbs->reg;
                        break;

                default:
                        break;
        }

        return ret;

}

static void chess_board_mmio_write (void *opaque, hwaddr addr, uint64_t val,
                unsigned int size)
{
        struct chess_board_state *cbs = opaque;

        printf ("[CHESS-BOARD] writing value %ld. %d bytes to addr %lx\n",
                        val, size, addr);

        if (size > 4)
                return;

        switch (addr)
        {
                case CHESS_REG_COMMAND:
                        cbs->command = (uint32_t)val;
                        if (cbs->command & (CHESS_CMD_DMA_READ |
                                                CHESS_CMD_DMA_WRITE))

                                chess_do_dma (cbs, cbs->command);
                        cbs->command = 0;
                        break;

                case CHESS_REG_DMA_SRC_L:
                        cbs->dma_info[CHESS_DMA_SRC_L] = (uint32_t)val;
                        break;

                case CHESS_REG_DMA_SRC_H:
                        cbs->dma_info[CHESS_DMA_SRC_H] = (uint32_t)val;
                        break;

                case CHESS_REG_DMA_DST_L:
                        cbs->dma_info[CHESS_DMA_DST_L] = (uint32_t)val; 
                        break;

                case CHESS_REG_DMA_DST_H:
                        cbs->dma_info[CHESS_DMA_DST_H] = (uint32_t)val; 
                        break;

                case CHESS_REG_DMA_SIZE:
                        cbs->dma_info[CHESS_DMA_SIZE] = (uint32_t)val; 
                        break;

                case CHESS_REG_INT_STATUS:
                        cbs->interrupt_status = (uint32_t)val;
                        break;

                case CHESS_REG_GENERAL:
                        cbs->reg = (uint32_t)val;
                        break;

                default:
                        break;
        }
}

static const MemoryRegionOps chess_board_mmio_ops = {
        .read = chess_board_mmio_read,
        .write = chess_board_mmio_write,
        .endianness = DEVICE_NATIVE_ENDIAN,
        .valid ={
                .min_access_size = 4,
                .max_access_size = 8,
        },
        // used as a hint to optimization
        .impl = {
                .min_access_size = 4,
                .max_access_size = 8,
        },
};

// executed after instance init
static void chess_board_realize (PCIDevice *pdev, Error **errp)
{
        struct chess_board_state *cbs = CHESS_BOARD(pdev);
        uint8_t *pci_conf = pdev->config;


        // enabling interrupts through physical interrupt pins
        pci_config_set_interrupt_pin(pci_conf, 1);

        // enabling interrupts through MSI
        /*
        if (msi_init(pdev, 0, 1, true, false, errp)) {
                return;
        }
        */
        
        

        cbs->reg = 11;

        /* 0x1000 bytes long, but only two register is handled in read/write
        mmio callbacks
        */
        memory_region_init_io (&cbs->mmio_region, OBJECT (cbs),
                        &chess_board_mmio_ops, cbs, TYPE_CHESS_BOARD,
                        CHESS_MMIO_SIZE);

        pci_register_bar (pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY,
                        &cbs->mmio_region);

        memory_region_init_ram (&cbs->dev_ram_region, OBJECT(cbs),
                        CHESS_BAR1_NAME, CHESS_RAM_SIZE, errp);

        pci_register_bar (pdev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY,
                        &cbs->dev_ram_region);

        volatile uint8_t* dev_ram_ptr =
                memory_region_get_ram_ptr (&cbs->dev_ram_region);

        for (int i = 0 ; i < 0x1000 ; i++)
                *(dev_ram_ptr + i) = 'j';


        cbs->buff_gmalloc = g_malloc0 (0x1000);
        if (!cbs->buff_gmalloc){
                printf ("error on gmalloc\n");
                sleep (5);
        }

        for (int i = 0 ; i < 0x1000 ; i++)
                *(cbs->buff_gmalloc + i) = 'Y';
}


static void chess_board_uninit(PCIDevice *pdev)
{
        return;
}

static void chess_board_instance_init (Object *obj)
{
        return;
}


static void chess_board_class_init (ObjectClass *class, void *data)
{
        DeviceClass *dc = DEVICE_CLASS(class);
        PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

        k->realize = chess_board_realize;
        k->exit = chess_board_uninit;
        k->vendor_id = PCI_VENDOR_ID_QEMU;
        k->device_id = 0xdead; 
        // I dont know how important is revision ID. 0x10 is random
        k->revision = 0x10;
        k->class_id = PCI_CLASS_OTHERS;

        set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}


static void chess_board_register_types(void)
{
        static InterfaceInfo interfaces[] = {
                { INTERFACE_CONVENTIONAL_PCI_DEVICE },
                { },
        };
        static const TypeInfo chess_board_info = {
                .name          = TYPE_CHESS_BOARD,
                .parent        = TYPE_PCI_DEVICE,
                .instance_size = sizeof(struct chess_board_state),
                .instance_init = chess_board_instance_init,
                .class_init    = chess_board_class_init,
                .interfaces = interfaces,
        };

        type_register_static(&chess_board_info);
}

type_init(chess_board_register_types)
