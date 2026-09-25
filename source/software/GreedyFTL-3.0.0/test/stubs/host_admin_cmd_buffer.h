/*
 * HOST_TEST replacement for ADMIN_CMD_DRAM_DATA_BUFFER (nvme/nvme.h).
 * On the board this is a fixed DRAM address; on the host it is a 32-bit-addressable
 * buffer carved out of the host memory arena (see host_mem.h).
 */
#ifndef HOST_ADMIN_CMD_BUFFER_H
#define HOST_ADMIN_CMD_BUFFER_H

extern unsigned int hostAdminCmdDataBufferAddr;

#define ADMIN_CMD_DRAM_DATA_BUFFER		(hostAdminCmdDataBufferAddr)

#endif
