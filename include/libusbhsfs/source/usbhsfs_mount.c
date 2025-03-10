/*
 * usbhsfs_mount.c
 *
 * Copyright (c) 2020-2023, DarkMatterCore <pabloacurielz@gmail.com>.
 * Copyright (c) 2020-2021, XorTroll.
 * Copyright (c) 2020-2021, Rhys Koedijk.
 *
 * This file is part of libusbhsfs (https://github.com/DarkMatterCore/libusbhsfs).
 */

#include "usbhsfs_utils.h"
#include "usbhsfs_manager.h"
#include "usbhsfs_mount.h"
#include "usbhsfs_scsi.h"
#include "fatfs/ff_dev.h"

#ifdef GPL_BUILD
#include "ntfs-3g/ntfs_dev.h"
#include "lwext4/ext_dev.h"
#endif

#define MOUNT_NAME_PREFIX      "ums"

#define BOOT_SIGNATURE          0xAA55

#define MBR_PARTITION_COUNT     4

#define DEVOPTAB_INVALID_ID     UINT32_MAX

#ifdef DEBUG
#define FS_TYPE_STR(x)          ((x) == UsbHsFsDriveLogicalUnitFileSystemType_FAT ? "FAT" : ((x) == UsbHsFsDriveLogicalUnitFileSystemType_NTFS ? "NTFS" : "EXT"))
#endif

/* Type definitions. */

/// DOS 2.0 BIOS Parameter Block. Used for FAT12 (13 bytes).
#pragma pack(push, 1)
typedef struct {
    u16 sector_size;        ///< Logical sector size in bytes. Belongs to the DOS 2.0 BPB area.
    u8 sectors_per_cluster; ///< Logical sectors per cluster. Belongs to the DOS 2.0 BPB area.
    u16 reserved_sectors;   ///< Reserved sectors. Belongs to the DOS 2.0 BPB area.
    u8 num_fats;            ///< Number of FATs. Belongs to the DOS 2.0 BPB area.
    u16 root_dir_entries;   ///< Root directory entries. Belongs to the DOS 2.0 BPB area.
    u16 total_sectors;      ///< Total logical sectors. Belongs to the DOS 2.0 BPB area.
    u8 media_desc;          ///< Media descriptor. Belongs to the DOS 2.0 BPB area.
    u16 sectors_per_fat;    ///< Logical sectors per FAT. Belongs to the DOS 2.0 BPB area.
} DOS_2_0_BPB;
#pragma pack(pop)

/// DOS 3.31 BIOS Parameter Block. Used for FAT12, FAT16 and FAT16B (25 bytes).
#pragma pack(push, 1)
typedef struct {
    DOS_2_0_BPB dos_2_0_bpb;    ///< DOS 2.0 BIOS Parameter Block.
    u16 sectors_per_track;      ///< Physical sectors per track.
    u16 num_heads;              ///< Number of heads.
    u32 hidden_sectors;         ///< Hidden sectors.
    u32 total_sectors;          ///< Large total logical sectors.
} DOS_3_31_BPB;
#pragma pack(pop)

/// DOS 7.1 Extended BIOS Parameter Block (full variant). Used for FAT32 (79 bytes).
#pragma pack(push, 1)
typedef struct {
    DOS_3_31_BPB dos_3_31_bpb;  ///< DOS 3.31 BIOS Parameter Block.
    u32 sectors_per_fat;        ///< Logical sectors per FAT.
    u16 mirroring_flags;        ///< Mirroring flags.
    u16 version;                ///< Version.
    u32 root_dir_cluster;       ///< Root directory cluster.
    u16 fsinfo_sector;          ///< Location of FS Information Sector.
    u16 backup_sector;          ///< Location of Backup Sector.
    u8 boot_filename[0xC];      ///< Boot filename.
    u8 pdrv;                    ///< Physical drive number.
    u8 flags;                   ///< Flags.
    u8 ext_boot_sig;            ///< Extended boot signature (0x29).
    u32 vol_serial_num;         ///< Volume serial number.
    u8 vol_label[0xB];          ///< Volume label.
    u8 fs_type[0x8];            ///< Filesystem type. Padded with spaces (0x20). Set to "FAT32   " if this is an FAT32 VBR.
} DOS_7_1_EBPB;
#pragma pack(pop)

/// Volume Boot Record (VBR). Represents the first sector from every FAT and NTFS filesystem. If a drive is formatted using Super Floppy Drive (SFD) configuration, this is located at LBA 0.
typedef struct {
    u8 jmp_boot[0x3];           ///< Jump boot code. First byte must match 0xEB (short jump), 0xE9 (near jump) or 0xE8 (near call). Set to "\xEB\x76\x90" is this is an exFAT VBR.
    char oem_name[0x8];         ///< OEM name. Padded with spaces (0x20). Set to "EXFAT   " if this is an exFAT VBR. Set to "NTFS    " if this is an NTFS VBR.
    DOS_7_1_EBPB dos_7_1_ebpb;  ///< DOS 7.1 Extended BIOS Parameter Block (full variant).
    u8 boot_code[0x1A3];        ///< File system and operating system specific boot code.
    u8 pdrv;                    ///< Physical drive number.
    u16 boot_sig;               ///< Matches BOOT_SIGNATURE for FAT32, exFAT and NTFS. Serves a different purpose under other FAT filesystems.
} VolumeBootRecord;

/// Master Boot Record (MBR) partition types. All these types support logical block addresses. CHS addressing only and hidden types have been excluded.
typedef enum {
    MasterBootRecordPartitionType_Empty                                 = 0x00,
    MasterBootRecordPartitionType_FAT12                                 = 0x01,
    MasterBootRecordPartitionType_FAT16                                 = 0x04,
    MasterBootRecordPartitionType_ExtendedBootRecord_CHS                = 0x05,
    MasterBootRecordPartitionType_FAT16B                                = 0x06,
    MasterBootRecordPartitionType_NTFS_exFAT                            = 0x07,
    MasterBootRecordPartitionType_FAT32_CHS                             = 0x0B,
    MasterBootRecordPartitionType_FAT32_LBA                             = 0x0C,
    MasterBootRecordPartitionType_FAT16B_LBA                            = 0x0E,
    MasterBootRecordPartitionType_ExtendedBootRecord_LBA                = 0x0F,
    MasterBootRecordPartitionType_LinuxFileSystem                       = 0x83,
    MasterBootRecordPartitionType_ExtendedBootRecord_Linux              = 0x85, ///< Corresponds to MasterBootRecordPartitionType_ExtendedBootRecord_CHS.
    MasterBootRecordPartitionType_GPT_Protective_MBR                    = 0xEE
} MasterBootRecordPartitionType;

/// Master Boot Record (MBR) partition entry.
typedef struct {
    u8 status;          ///< Partition status. We won't use this.
    u8 chs_start[0x3];  ///< Cylinder-head-sector address to the first block in the partition. Unused nowadays.
    u8 type;            ///< MasterBootRecordPartitionType.
    u8 chs_end[0x3];    ///< Cylinder-head-sector address to the last block in the partition. Unused nowadays.
    u32 lba;            ///< Logical block address to the first block in the partition.
    u32 block_count;    ///< Logical block count in the partition.
} MasterBootRecordPartitionEntry;

/// Master Boot Record (MBR). Always located at LBA 0, as long as SFD configuration isn't used (VBR at LBA 0).
#pragma pack(push, 1)
typedef struct {
    u8 code_area[0x1BE];                                            ///< Bootstrap code area. We won't use this.
    MasterBootRecordPartitionEntry partitions[MBR_PARTITION_COUNT]; ///< Primary partition entries.
    u16 boot_sig;                                                   ///< Boot signature. Must match BOOT_SIGNATURE.
} MasterBootRecord;
#pragma pack(pop)

/// Extended Boot Record (EBR). Represents a way to store more than 4 partitions in a MBR-formatted logical unit using linked lists.
typedef struct {
    u8 code_area[0x1BE];                        ///< Bootstrap code area. Normally empty.
    MasterBootRecordPartitionEntry partition;   ///< Primary partition entry.
    MasterBootRecordPartitionEntry next_ebr;    ///< Next EBR in the chain.
    u8 reserved[0x20];                          ///< Normally empty.
    u16 boot_sig;                               ///< Boot signature. Must match BOOT_SIGNATURE.
} ExtendedBootRecord;

/// Globally Unique ID Partition Table (GPT) entry. These usually start at LBA 2.
typedef struct {
    u8 type_guid[0x10];     ///< Partition type GUID.
    u8 unique_guid[0x10];   ///< Unique partition GUID.
    u64 lba_start;          ///< First LBA.
    u64 lba_end;            ///< Last LBA (inclusive).
    u64 flags;              ///< Attribute flags.
    u16 name[0x24];         ///< Partition name (36 UTF-16LE code units).
} GuidPartitionTableEntry;

/// Globally Unique ID Partition Table (GPT) header. If available, it's always located at LBA 1.
typedef struct {
    u64 signature;                  ///< Must match "EFI PART".
    u32 revision;                   ///< GUID Partition Table revision.
    u32 header_size;                ///< Header size. Must match 0x5C.
    u32 header_crc32;               ///< Little-endian CRC32 checksum calculated over this header, with this field zeroed during calculation.
    u8 reserved_1[0x4];             ///< Reserved.
    u64 cur_header_lba;             ///< LBA from this GPT header.
    u64 backup_header_lba;          ///< LBA from the backup GPT header.
    u64 partition_lba_start;        ///< First usable LBA for partitions (primary partition table last LBA + 1).
    u64 partition_lba_end;          ///< Last usable LBA (secondary partition table first LBA - 1).
    u8 disk_guid[0x10];             ///< Disk GUID.
    u64 partition_array_lba;        ///< Starting LBA of array of partition entries (always 2 in primary copy).
    u32 partition_array_count;      ///< Number of partition entries in array.
    u32 partition_array_entry_size; ///< Size of a single partition entry (usually 0x80).
    u32 partition_array_crc32;      ///< Little-endian CRC32 checksum calculated over the partition array.
    u8 reserved_2[0x1A4];           ///< Reserved; must be zeroes for the rest of the block.
} GuidPartitionTableHeader;

static_assert(sizeof(DOS_2_0_BPB) == 0xD, "Bad DOS_2_0_BPB size! Expected 0xD.");
static_assert(sizeof(DOS_3_31_BPB) == 0x19, "Bad DOS_3_31_BPB size! Expected 0x19.");
static_assert(sizeof(DOS_7_1_EBPB) == 0x4F, "Bad DOS_7_1_EBPB size! Expected 0x4F.");
static_assert(sizeof(VolumeBootRecord) == 0x200, "Bad VolumeBootRecord size! Expected 0x200.");
static_assert(sizeof(MasterBootRecord) == 0x200, "Bad MasterBootRecord size! Expected 0x200.");
static_assert(sizeof(MasterBootRecordPartitionEntry) == 0x10, "Bad MasterBootRecordPartitionEntry size! Expected 0x10.");
static_assert(sizeof(GuidPartitionTableEntry) == 0x80, "Bad GuidPartitionTableEntry size! Expected 0x80.");
static_assert(sizeof(GuidPartitionTableHeader) == 0x200, "Bad GuidPartitionTableHeader size! Expected 0x200.");

/* Global variables. */

static u32 g_devoptabDeviceCount = 0;
static u32 *g_devoptabDeviceIds = NULL;

static u32 g_devoptabDefaultDeviceId = DEVOPTAB_INVALID_ID;
static Mutex g_devoptabDefaultDeviceMutex = 0;

static bool g_fatFsVolumeTable[FF_VOLUMES] = { false };

static const u8 g_microsoftBasicDataPartitionGuid[0x10] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };   /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7. */
static const u8 g_linuxFilesystemDataGuid[0x10] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };           /* 0FC63DAF-8483-4772-8E79-3D69D8477DE4. */

static u32 g_fileSystemMountFlags = (UsbHsFsMountFlags_UpdateAccessTimes | UsbHsFsMountFlags_ShowHiddenFiles | UsbHsFsMountFlags_ReplayJournal);

__thread char __usbhsfs_dev_path_buf[MAX_PATH_LENGTH] = {0};

/* Function prototypes. */

static bool usbHsFsMountParseMasterBootRecord(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block);
static void usbHsFsMountParseMasterBootRecordPartitionEntry(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u8 type, u64 lba, u64 size, bool parse_ebr_gpt);

static u8 usbHsFsMountInspectVolumeBootRecord(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u64 block_addr);
#ifdef GPL_BUILD
static u8 usbHsFsMountInspectExtSuperBlock(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u64 block_addr);
#endif

static void usbHsFsMountParseExtendedBootRecord(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u64 ebr_lba);
static void usbHsFsMountParseGuidPartitionTable(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u64 gpt_lba);

static bool usbHsFsMountRegisterVolume(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u64 block_addr, u64 block_count, u8 fs_type);

static bool usbHsFsMountRegisterFatVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, u8 *block, u64 block_addr);
static void usbHsFsMountUnregisterFatVolume(char *name, UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx);

#ifdef GPL_BUILD
static bool usbHsFsMountRegisterNtfsVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, u8 *block, u64 block_addr);
static void usbHsFsMountUnregisterNtfsVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx);

static bool usbHsFsMountRegisterExtVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, u64 block_addr, u64 block_count);
static void usbHsFsMountUnregisterExtVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx);
#endif

static bool usbHsFsMountRegisterDevoptabDevice(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx);
static u32 usbHsFsMountGetAvailableDevoptabDeviceId(void);

static void usbHsFsMountUnsetDefaultDevoptabDevice(u32 device_id);

bool usbHsFsMountInitializeLogicalUnitFileSystemContexts(UsbHsFsDriveLogicalUnitContext *lun_ctx)
{
    if (!usbHsFsDriveIsValidLogicalUnitContext(lun_ctx))
    {
        USBHSFS_LOG_MSG("Invalid parameters!");
        return false;
    }

    u8 *block = NULL;
    u8 fs_type = 0;
    bool ret = false;

    /* Allocate memory to hold data from a single logical block. */
    block = malloc(lun_ctx->block_length);
    if (!block)
    {
        USBHSFS_LOG_MSG("Failed to allocate memory to hold logical block data! (interface %d, LUN %u).", lun_ctx->usb_if_id, lun_ctx->lun);
        goto end;
    }

    /* Check if we're dealing with a SFD-formatted logical unit with a Microsoft VBR at LBA 0. */
    fs_type = usbHsFsMountInspectVolumeBootRecord(lun_ctx, block, 0);
    if (fs_type > UsbHsFsDriveLogicalUnitFileSystemType_Unsupported)
    {
        /* Mount volume at LBA 0 right away. */
        ret = usbHsFsMountRegisterVolume(lun_ctx, block, 0, lun_ctx->block_count, fs_type);
    } else
    if (fs_type == UsbHsFsDriveLogicalUnitFileSystemType_Unsupported)
    {
        /* Parse MBR. */
        ret = usbHsFsMountParseMasterBootRecord(lun_ctx, block);
    } else {
#ifdef GPL_BUILD
        /* We may be dealing with an EXT volume at LBA 0. */
        fs_type = usbHsFsMountInspectExtSuperBlock(lun_ctx, block, 0);
        if (fs_type == UsbHsFsDriveLogicalUnitFileSystemType_EXT)
        {
            /* Mount EXT volume at LBA 0. */
            ret = usbHsFsMountRegisterVolume(lun_ctx, block, 0, lun_ctx->block_count, fs_type);
        } else {
            USBHSFS_LOG_MSG("Unable to locate a valid boot sector! (interface %d, LUN %u).", lun_ctx->usb_if_id, lun_ctx->lun);
        }
#else
        USBHSFS_LOG_MSG("Unable to locate a valid boot sector! (interface %d, LUN %u).", lun_ctx->usb_if_id, lun_ctx->lun);
#endif
    }

end:
    if (block) free(block);

    return ret;
}

void usbHsFsMountDestroyLogicalUnitFileSystemContext(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx)
{
    if (!usbHsFsDriveIsValidLogicalUnitFileSystemContext(fs_ctx)) return;

    char name[MOUNT_NAME_LENGTH] = {0};
    u32 *tmp_device_ids = NULL;

    /* Unset default devoptab device. */
    usbHsFsMountUnsetDefaultDevoptabDevice(fs_ctx->device_id);

    /* Unregister devoptab interface. */
    sprintf(name, "%s:", fs_ctx->name);
    RemoveDevice(name);

    /* Free devoptab virtual device interface. */
    free(fs_ctx->device);
    fs_ctx->device = NULL;

    /* Free current working directory. */
    free(fs_ctx->cwd);
    fs_ctx->cwd = NULL;

    /* Free mount name. */
    free(fs_ctx->name);
    fs_ctx->name = NULL;

    /* Locate device ID in devoptab device ID buffer and remove it. */
    for(u32 i = 0; i < g_devoptabDeviceCount; i++)
    {
        if (g_devoptabDeviceIds[i] != fs_ctx->device_id) continue;

        USBHSFS_LOG_MSG("Found device ID %u at index %u.", fs_ctx->device_id, i);

        if (g_devoptabDeviceCount > 1)
        {
            /* Move data within the device ID buffer, if needed. */
            if (i < (g_devoptabDeviceCount - 1)) memmove(&(g_devoptabDeviceIds[i]), &(g_devoptabDeviceIds[i + 1]), (g_devoptabDeviceCount - (i + 1)) * sizeof(u32));

            /* Reallocate devoptab device IDs buffer. */
            tmp_device_ids = realloc(g_devoptabDeviceIds, (g_devoptabDeviceCount - 1) * sizeof(u32));
            if (tmp_device_ids)
            {
                g_devoptabDeviceIds = tmp_device_ids;
                tmp_device_ids = NULL;
            }
        } else {
            /* Free devoptab device ID buffer. */
            free(g_devoptabDeviceIds);
            g_devoptabDeviceIds = NULL;
        }

        /* Decrease devoptab virtual device count. */
        g_devoptabDeviceCount--;

        break;
    }

    /* Unmount filesystem. */
    switch(fs_ctx->fs_type)
    {
        case UsbHsFsDriveLogicalUnitFileSystemType_FAT:     /* FAT12/FAT16/FAT32/exFAT. */
            usbHsFsMountUnregisterFatVolume(name, fs_ctx);
            break;
#ifdef GPL_BUILD
        case UsbHsFsDriveLogicalUnitFileSystemType_NTFS:    /* NTFS. */
            usbHsFsMountUnregisterNtfsVolume(fs_ctx);
            break;
        case UsbHsFsDriveLogicalUnitFileSystemType_EXT:     /* EXT2/3/4. */
            usbHsFsMountUnregisterExtVolume(fs_ctx);
            break;
#endif

        /* TODO: populate this after adding support for additional filesystems. */

        default:
            break;
    }
}

u32 usbHsFsMountGetDevoptabDeviceCount(void)
{
    return g_devoptabDeviceCount;
}

bool usbHsFsMountSetDefaultDevoptabDevice(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx)
{
    bool ret = false;

    SCOPED_LOCK(&g_devoptabDefaultDeviceMutex)
    {
        if (!g_devoptabDeviceCount || !g_devoptabDeviceIds || !usbHsFsDriveIsValidLogicalUnitFileSystemContext(fs_ctx))
        {
            USBHSFS_LOG_MSG("Invalid parameters!");
            break;
        }

        const devoptab_t *cur_default_devoptab = NULL;
        int new_default_device = -1;
        char name[MOUNT_NAME_LENGTH] = {0};

        /* Get current default devoptab device index. */
        cur_default_devoptab = GetDeviceOpTab("");
        if (cur_default_devoptab && cur_default_devoptab->deviceData == fs_ctx)
        {
            /* Device already set as default. */
            USBHSFS_LOG_MSG("Device \"%s\" already set as default.", fs_ctx->name);
            ret = true;
            break;
        }

        /* Get devoptab device index for our filesystem. */
        sprintf(name, "%s:", fs_ctx->name);
        new_default_device = FindDevice(name);
        if (new_default_device < 0)
        {
            USBHSFS_LOG_MSG("Failed to retrieve devoptab device index for \"%s\"!", fs_ctx->name);
            break;
        }

        /* Set default devoptab device. */
        setDefaultDevice(new_default_device);
        cur_default_devoptab = GetDeviceOpTab("");
        if (!cur_default_devoptab || cur_default_devoptab->deviceData != fs_ctx)
        {
            USBHSFS_LOG_MSG("Failed to set default devoptab device to index %d! (device \"%s\").", new_default_device, fs_ctx->name);
            break;
        }

        USBHSFS_LOG_MSG("Successfully set default devoptab device to index %d! (device \"%s\").", new_default_device, fs_ctx->name);

        /* Update default device ID. */
        g_devoptabDefaultDeviceId = fs_ctx->device_id;

        /* Update return value. */
        ret = true;
    }

    return ret;
}

u32 usbHsFsMountGetFileSystemMountFlags(void)
{
    return g_fileSystemMountFlags;
}

void usbHsFsMountSetFileSystemMountFlags(u32 flags)
{
    g_fileSystemMountFlags = flags;
}

static bool usbHsFsMountParseMasterBootRecord(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block)
{
    MasterBootRecord mbr = {0};
    bool ret = false;

    memcpy(&mbr, block, sizeof(MasterBootRecord));

    /* Parse MBR partition entries. */
    for(u8 i = 0; i < MBR_PARTITION_COUNT; i++)
    {
        MasterBootRecordPartitionEntry *partition = &(mbr.partitions[i]);
        usbHsFsMountParseMasterBootRecordPartitionEntry(lun_ctx, block, partition->type, partition->lba, partition->block_count, true);
    }

    /* Update return value. */
    ret = (lun_ctx->fs_count > 0);

    return ret;
}

static void usbHsFsMountParseMasterBootRecordPartitionEntry(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u8 type, u64 lba, u64 size, bool parse_ebr_gpt)
{
    u8 fs_type = UsbHsFsDriveLogicalUnitFileSystemType_Invalid;

    switch(type)
    {
        case MasterBootRecordPartitionType_Empty:
            USBHSFS_LOG_MSG("Found empty partition entry (interface %d, LUN %u). Skipping.", lun_ctx->usb_if_id, lun_ctx->lun);
            break;
        case MasterBootRecordPartitionType_FAT12:
        case MasterBootRecordPartitionType_FAT16:
        case MasterBootRecordPartitionType_FAT16B:
        case MasterBootRecordPartitionType_NTFS_exFAT:
        case MasterBootRecordPartitionType_FAT32_CHS:
        case MasterBootRecordPartitionType_FAT32_LBA:
        case MasterBootRecordPartitionType_FAT16B_LBA:
            USBHSFS_LOG_MSG("Found FAT/NTFS partition entry with type 0x%02X at LBA 0x%lX (interface %d, LUN %u).", type, lba, lun_ctx->usb_if_id, lun_ctx->lun);

            /* Inspect VBR. */
            fs_type = usbHsFsMountInspectVolumeBootRecord(lun_ctx, block, lba);

            break;
        case MasterBootRecordPartitionType_LinuxFileSystem:
            USBHSFS_LOG_MSG("Found Linux partition entry with type 0x%02X at LBA 0x%lX (interface %d, LUN %u).", type, lba, lun_ctx->usb_if_id, lun_ctx->lun);

#ifdef GPL_BUILD
            /* Inspect EXT superblock. */
            fs_type = usbHsFsMountInspectExtSuperBlock(lun_ctx, block, lba);
#endif

            break;
        case MasterBootRecordPartitionType_ExtendedBootRecord_CHS:
        case MasterBootRecordPartitionType_ExtendedBootRecord_LBA:
        case MasterBootRecordPartitionType_ExtendedBootRecord_Linux:
            USBHSFS_LOG_MSG("Found EBR partition entry with type 0x%02X at LBA 0x%lX (interface %d, LUN %u).", type, lba, lun_ctx->usb_if_id, lun_ctx->lun);

            /* Parse EBR. */
            if (parse_ebr_gpt) usbHsFsMountParseExtendedBootRecord(lun_ctx, block, lba);

            break;
        case MasterBootRecordPartitionType_GPT_Protective_MBR:
            USBHSFS_LOG_MSG("Found GPT partition entry at LBA 0x%lX (interface %d, LUN %u).", lba, lun_ctx->usb_if_id, lun_ctx->lun);

            /* Parse GPT. */
            if (parse_ebr_gpt) usbHsFsMountParseGuidPartitionTable(lun_ctx, block, lba);

            break;
        default:
            USBHSFS_LOG_MSG("Found unsupported partition entry with type 0x%02X (interface %d, LUN %u). Skipping.", type, lun_ctx->usb_if_id, lun_ctx->lun);
            break;
    }

    /* Register detected volume. */
    if (fs_type > UsbHsFsDriveLogicalUnitFileSystemType_Unsupported && usbHsFsMountRegisterVolume(lun_ctx, block, lba, size, fs_type))
    {
        USBHSFS_LOG_MSG("Successfully registered %s volume at LBA 0x%lX (interface %d, LUN %u).", FS_TYPE_STR(fs_type), lba, lun_ctx->usb_if_id, lun_ctx->lun);
    }
}

static u8 usbHsFsMountInspectVolumeBootRecord(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u64 block_addr)
{
    u32 block_length = lun_ctx->block_length;
    u8 ret = UsbHsFsDriveLogicalUnitFileSystemType_Invalid;

    /* Read block at the provided address from this LUN. */
    if (!usbHsFsScsiReadLogicalUnitBlocks(lun_ctx, block, block_addr, 1))
    {
        USBHSFS_LOG_MSG("Failed to read block at LBA 0x%lX! (interface %d, LUN %u).", block_addr, lun_ctx->usb_if_id, lun_ctx->lun);
        goto end;
    }

    VolumeBootRecord *vbr = (VolumeBootRecord*)block;
    u8 jmp_code = vbr->jmp_boot[0];
    u16 boot_sig = vbr->boot_sig;

    DOS_3_31_BPB *dos_3_31_bpb = &(vbr->dos_7_1_ebpb.dos_3_31_bpb);
    DOS_2_0_BPB *dos_2_0_bpb = &(dos_3_31_bpb->dos_2_0_bpb);

    u8 sectors_per_cluster = dos_2_0_bpb->sectors_per_cluster, num_fats = dos_2_0_bpb->num_fats;
    u16 sector_size = dos_2_0_bpb->sector_size, reserved_sectors = dos_2_0_bpb->reserved_sectors, root_dir_entries = dos_2_0_bpb->root_dir_entries, total_sectors_16 = dos_2_0_bpb->total_sectors;
    u16 sectors_per_fat = dos_2_0_bpb->sectors_per_fat;
    u32 total_sectors_32 = dos_3_31_bpb->total_sectors;

    /* Check if we have a valid boot sector signature. */
    if (boot_sig == BOOT_SIGNATURE)
    {
        /* Check if this is an exFAT VBR. */
        if (!memcmp(vbr->jmp_boot, "\xEB\x76\x90" "EXFAT   ", 11))
        {
            ret = UsbHsFsDriveLogicalUnitFileSystemType_FAT;
            goto end;
        }

        /* Check if this is an NTFS VBR. */
        if (!memcmp(vbr->oem_name, "NTFS    ", 8))
        {
            ret = UsbHsFsDriveLogicalUnitFileSystemType_NTFS;
            goto end;
        }
    }

    /* Check if we have a valid jump boot code. */
    if (jmp_code == 0xEB || jmp_code == 0xE9 || jmp_code == 0xE8)
    {
        /* Check if this is a FAT32 VBR. */
        if (boot_sig == BOOT_SIGNATURE && !memcmp(vbr->dos_7_1_ebpb.fs_type, "FAT32   ", 8))
        {
            ret = UsbHsFsDriveLogicalUnitFileSystemType_FAT;
            goto end;
        }

        /* FAT volumes formatted with old tools lack a boot sector signature and a filesystem type string, so we'll try to identify the FAT VBR without them. */
        if ((sector_size & (sector_size - 1)) == 0 && sector_size <= (u16)block_length && sectors_per_cluster != 0 && (sectors_per_cluster & (sectors_per_cluster - 1)) == 0 && \
            reserved_sectors != 0 && (num_fats - 1) <= 1 && root_dir_entries != 0 && (total_sectors_16 >= 128 || total_sectors_32 >= 0x10000) && sectors_per_fat != 0) \
                ret = UsbHsFsDriveLogicalUnitFileSystemType_FAT;
    }

    /* Change return value if we couldn't identify a potential VBR but there's valid boot signature. */
    /* We may be dealing with a MBR/EBR. */
    if (ret == UsbHsFsDriveLogicalUnitFileSystemType_Invalid && boot_sig == BOOT_SIGNATURE) ret = UsbHsFsDriveLogicalUnitFileSystemType_Unsupported;

end:
    if (ret > UsbHsFsDriveLogicalUnitFileSystemType_Unsupported) USBHSFS_LOG_MSG("Found %s VBR at LBA 0x%lX (interface %d, LUN %u).", FS_TYPE_STR(ret), block_addr, lun_ctx->usb_if_id, lun_ctx->lun);

    return ret;
}

#ifdef GPL_BUILD

static u8 usbHsFsMountInspectExtSuperBlock(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u64 block_addr)
{
    u32 block_length = lun_ctx->block_length;
    u32 block_read_addr = (block_addr + (EXT4_SUPERBLOCK_OFFSET / block_length));
    u32 block_read_count = (block_length >= EXT4_SUPERBLOCK_SIZE ? 1 : (EXT4_SUPERBLOCK_SIZE / block_length));
    struct ext4_sblock superblock = {0};
    u8 ret = UsbHsFsDriveLogicalUnitFileSystemType_Invalid;

    if (block_read_count == 1)
    {
        /* Read entire EXT superblock. */
        if (!usbHsFsScsiReadLogicalUnitBlocks(lun_ctx, block, block_read_addr, 1))
        {
            USBHSFS_LOG_MSG("Failed to read block at LBA 0x%X! (interface %d, LUN %u).", block_read_addr, lun_ctx->usb_if_id, lun_ctx->lun);
            goto end;
        }

        /* Copy EXT superblock data. */
        memcpy(&superblock, block + (block_read_addr == block_addr ? EXT4_SUPERBLOCK_OFFSET : 0), sizeof(struct ext4_sblock));
    } else {
        /* Read entire EXT superblock. */
        if (!usbHsFsScsiReadLogicalUnitBlocks(lun_ctx, (u8*)&superblock, block_read_addr, block_read_count))
        {
            USBHSFS_LOG_MSG("Failed to read %u blocks at LBA 0x%X! (interface %d, LUN %u).", block_read_count, block_read_addr, lun_ctx->usb_if_id, lun_ctx->lun);
            goto end;
        }
    }

    /* Check if this is a valid EXT superblock. */
    if (ext4_sb_check(&superblock)) ret = UsbHsFsDriveLogicalUnitFileSystemType_EXT;

end:
    if (ret == UsbHsFsDriveLogicalUnitFileSystemType_EXT) USBHSFS_LOG_MSG("Found EXT superblock at LBA 0x%X (interface %d, LUN %u).", block_read_addr, lun_ctx->usb_if_id, lun_ctx->lun);

    return ret;
}

#endif  /* GPL_BUILD */

static void usbHsFsMountParseExtendedBootRecord(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u64 ebr_lba)
{
    ExtendedBootRecord ebr = {0};
    u64 next_ebr_lba = 0, part_lba = 0;

    do {
        /* Read current EBR sector. */
        if (!usbHsFsScsiReadLogicalUnitBlocks(lun_ctx, block, ebr_lba + next_ebr_lba, 1))
        {
            USBHSFS_LOG_MSG("Failed to read EBR at LBA 0x%lX! (interface %d, LUN %u).", ebr_lba, lun_ctx->usb_if_id, lun_ctx->lun);
            break;
        }

        /* Copy EBR data to struct. */
        memcpy(&ebr, block, sizeof(ExtendedBootRecord));

        /* Check boot signature. */
        if (ebr.boot_sig == BOOT_SIGNATURE)
        {
            /* Calculate LBAs for the current partition and the next EBR in the chain. */
            part_lba = (ebr_lba + next_ebr_lba + ebr.partition.lba);
            next_ebr_lba = ebr.next_ebr.lba;

            /* Parse partition entry. */
            usbHsFsMountParseMasterBootRecordPartitionEntry(lun_ctx, block, ebr.partition.type, part_lba, ebr.partition.block_count, false);
        } else {
            /* Reset LBA from next EBR. */
            next_ebr_lba = 0;
        }
    } while(next_ebr_lba);
}

static void usbHsFsMountParseGuidPartitionTable(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u64 gpt_lba)
{
    GuidPartitionTableHeader gpt_header = {0};
    u32 header_crc32 = 0, header_crc32_calc = 0, part_count = 0, part_per_block = 0, part_array_block_count = 0;
    u64 part_lba = 0;

    /* Read block where the GPT header is located. */
    if (!usbHsFsScsiReadLogicalUnitBlocks(lun_ctx, block, gpt_lba, 1))
    {
        USBHSFS_LOG_MSG("Failed to read GPT header from LBA 0x%lX! (interface %d, LUN %u).", gpt_lba, lun_ctx->usb_if_id, lun_ctx->lun);
        return;
    }

    /* Copy GPT header data. */
    memcpy(&gpt_header, block, sizeof(GuidPartitionTableHeader));

    /* Verify GPT header signature, revision and header size fields. */
    if (memcmp(&(gpt_header.signature), "EFI PART" "\x00\x00\x01\x00" "\x5C\x00\x00\x00", 16) != 0)
    {
        USBHSFS_LOG_MSG("Invalid GPT header at LBA 0x%lX! (interface %d, LUN %u).", gpt_lba, lun_ctx->usb_if_id, lun_ctx->lun);
        return;
    }

    /* Verify GPT header CRC32 checksum. */
    header_crc32 = gpt_header.header_crc32;
    gpt_header.header_crc32 = 0;
    header_crc32_calc = crc32Calculate(&gpt_header, gpt_header.header_size);
    gpt_header.header_crc32 = header_crc32;

    if (header_crc32_calc != header_crc32)
    {
        USBHSFS_LOG_MSG("Invalid CRC32 checksum for GPT header at LBA 0x%lX! (%08X != %08X) (interface %d, LUN %u).", gpt_lba, header_crc32_calc, header_crc32, lun_ctx->usb_if_id, lun_ctx->lun);

        /* Check if the LBA for the backup GPT header points to a valid location. */
        gpt_lba = gpt_header.backup_header_lba;
        if (!gpt_lba || gpt_lba == gpt_header.cur_header_lba || gpt_lba >= lun_ctx->block_count) return;

        /* Read block where the backup GPT header is located. */
        if (!usbHsFsScsiReadLogicalUnitBlocks(lun_ctx, block, gpt_lba, 1))
        {
            USBHSFS_LOG_MSG("Failed to read backup GPT header from LBA 0x%lX! (interface %d, LUN %u).", gpt_lba, lun_ctx->usb_if_id, lun_ctx->lun);
            return;
        }

        /* Copy backup GPT header data. */
        memcpy(&gpt_header, block, sizeof(GuidPartitionTableHeader));

        /* Verify backup GPT header CRC32 checksum. */
        header_crc32 = gpt_header.header_crc32;
        gpt_header.header_crc32 = 0;
        header_crc32_calc = crc32Calculate(&gpt_header, gpt_header.header_size);
        gpt_header.header_crc32 = header_crc32;

        if (header_crc32_calc != header_crc32)
        {
            USBHSFS_LOG_MSG("Invalid CRC32 checksum for backup GPT header at LBA 0x%lX! (%08X != %08X) (interface %d, LUN %u).", gpt_lba, header_crc32_calc, header_crc32, lun_ctx->usb_if_id, \
                        lun_ctx->lun);
            return;
        }

        USBHSFS_LOG_MSG("Backup GPT header located at LBA 0x%lX (interface %d, LUN %u).", gpt_lba, lun_ctx->usb_if_id, lun_ctx->lun);
    }

    /* Verify GPT partition entry size. */
    if (gpt_header.partition_array_entry_size != sizeof(GuidPartitionTableEntry))
    {
        USBHSFS_LOG_MSG("Invalid GPT partition entry size in GPT header at LBA 0x%lX! (0x%X != 0x%lX) (interface %d, LUN %u).", gpt_lba, gpt_header.partition_array_entry_size, \
                    sizeof(GuidPartitionTableEntry), lun_ctx->usb_if_id, lun_ctx->lun);
        return;
    }

    /* Get GPT partition entry count. Only process the first 128 entries if there's more than that. */
    part_count = gpt_header.partition_array_count;
    if (part_count > 128) part_count = 128;

    /* Calculate number of partition entries per block and the total block count for the whole partition array. */
    part_lba = gpt_header.partition_array_lba;
    part_per_block = (lun_ctx->block_length / (u32)sizeof(GuidPartitionTableEntry));
    part_array_block_count = (part_count / part_per_block);

    /* Parse GPT partition entries. */
    for(u32 i = 0; i < part_array_block_count; i++)
    {
        /* Read current partition array block. */
        if (!usbHsFsScsiReadLogicalUnitBlocks(lun_ctx, block, part_lba + i, 1))
        {
            USBHSFS_LOG_MSG("Failed to read GPT partition array block #%u from LBA 0x%lX! (interface %d, LUN %u).", i, part_lba + i, lun_ctx->usb_if_id, lun_ctx->lun);
            break;
        }

        for(u32 j = 0; j < part_per_block; j++)
        {
            GuidPartitionTableEntry *gpt_entry = (GuidPartitionTableEntry*)(block + (j * sizeof(GuidPartitionTableEntry)));
            u64 entry_lba = gpt_entry->lba_start;
            u64 entry_size = ((gpt_entry->lba_end + 1) - gpt_entry->lba_start);
            u8 fs_type = UsbHsFsDriveLogicalUnitFileSystemType_Invalid;

            if (!memcmp(gpt_entry->type_guid, g_microsoftBasicDataPartitionGuid, sizeof(g_microsoftBasicDataPartitionGuid)))
            {
                /* We're dealing with a Microsoft Basic Data Partition entry. */
                USBHSFS_LOG_MSG("Found Microsoft Basic Data Partition entry at LBA 0x%lX (interface %d, LUN %u).", entry_lba, lun_ctx->usb_if_id, lun_ctx->lun);

                /* Inspect Microsoft VBR. Register the volume if we detect a supported VBR. */
                fs_type = usbHsFsMountInspectVolumeBootRecord(lun_ctx, block, entry_lba);
#ifdef GPL_BUILD
                if (fs_type == UsbHsFsDriveLogicalUnitFileSystemType_Invalid)
                {
                    /* We may be dealing with a EXT volume. Check if we can find a valid EXT superblock. */
                    /* Certain tools set the type GUID from EXT volumes to the one from Microsoft. */
                    fs_type = usbHsFsMountInspectExtSuperBlock(lun_ctx, block, entry_lba);
                }
#endif
            } else
            if (!memcmp(gpt_entry->type_guid, g_linuxFilesystemDataGuid, sizeof(g_linuxFilesystemDataGuid)))
            {
                /* We're dealing with a Linux Filesystem Data entry. */
                USBHSFS_LOG_MSG("Found Linux Filesystem Data entry at LBA 0x%lX (interface %d, LUN %u).", entry_lba, lun_ctx->usb_if_id, lun_ctx->lun);

#ifdef GPL_BUILD
                /* Check if this LBA points to a valid EXT superblock. Register the EXT volume if so. */
                fs_type = usbHsFsMountInspectExtSuperBlock(lun_ctx, block, entry_lba);
#endif
            }

            /* Register volume. */
            if (fs_type > UsbHsFsDriveLogicalUnitFileSystemType_Unsupported && usbHsFsMountRegisterVolume(lun_ctx, block, entry_lba, entry_size, fs_type))
            {
                USBHSFS_LOG_MSG("Successfully registered %s volume at LBA 0x%lX (interface %d, LUN %u).", FS_TYPE_STR(fs_type), entry_lba, lun_ctx->usb_if_id, lun_ctx->lun);
            }
        }
    }
}

static bool usbHsFsMountRegisterVolume(UsbHsFsDriveLogicalUnitContext *lun_ctx, u8 *block, u64 block_addr, u64 block_count, u8 fs_type)
{
#ifndef GPL_BUILD
    (void)block_count;
#endif

    UsbHsFsDriveLogicalUnitFileSystemContext **tmp_fs_ctx = NULL, *fs_ctx = NULL;
    bool ret = false, free_entry = false;

    /* Reallocate filesystem context pointer array. */
    tmp_fs_ctx = realloc(lun_ctx->fs_ctx, (lun_ctx->fs_count + 1) * sizeof(UsbHsFsDriveLogicalUnitFileSystemContext*));
    if (!tmp_fs_ctx)
    {
        USBHSFS_LOG_MSG("Failed to reallocate filesystem context pointer array! (interface %d, LUN %u).", lun_ctx->usb_if_id, lun_ctx->lun);
        goto end;
    }

    lun_ctx->fs_ctx = tmp_fs_ctx;
    tmp_fs_ctx = NULL;
    free_entry = true;

    /* Allocate memory for a new filesystem context. */
    fs_ctx = calloc(1, sizeof(UsbHsFsDriveLogicalUnitFileSystemContext));
    if (!fs_ctx)
    {
        USBHSFS_LOG_MSG("Failed to allocate memory for filesystem context entry #%u! (interface %d, LUN %u).", lun_ctx->fs_count, lun_ctx->usb_if_id, lun_ctx->lun);
        goto end;
    }

    /* Set filesystem context properties. */
    fs_ctx->lun_ctx = lun_ctx;
    fs_ctx->fs_idx = lun_ctx->fs_count;
    fs_ctx->fs_type = fs_type;
    fs_ctx->flags = g_fileSystemMountFlags;

    /* Set filesystem context entry pointer and update filesystem context count. */
    lun_ctx->fs_ctx[(lun_ctx->fs_count)++] = fs_ctx;

    /* Mount and register filesystem. */
    switch(fs_type)
    {
        case UsbHsFsDriveLogicalUnitFileSystemType_FAT:     /* FAT12/FAT16/FAT32/exFAT. */
            ret = usbHsFsMountRegisterFatVolume(fs_ctx, block, block_addr);
            break;
#ifdef GPL_BUILD
        case UsbHsFsDriveLogicalUnitFileSystemType_NTFS:    /* NTFS. */
            ret = usbHsFsMountRegisterNtfsVolume(fs_ctx, block, block_addr);
            break;
        case UsbHsFsDriveLogicalUnitFileSystemType_EXT:     /* EXT2/3/4. */
            ret = usbHsFsMountRegisterExtVolume(fs_ctx, block_addr, block_count);
            break;
#endif

        /* TODO: populate this after adding support for additional filesystems. */

        default:
            USBHSFS_LOG_MSG("Invalid FS type provided! (0x%02X) (interface %d, LUN %u, FS %u).", fs_type, lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
            break;
    }

end:
    if (!ret && free_entry)
    {
        /* Free filesystem context. */
        if (fs_ctx)
        {
            free(fs_ctx);

            /* Update filesystem context count and clear filesystem context entry pointer. */
            lun_ctx->fs_ctx[--(lun_ctx->fs_count)] = fs_ctx = NULL;
        }

        if (lun_ctx->fs_count)
        {
            /* Reallocate filesystem context buffer. */
            tmp_fs_ctx = realloc(lun_ctx->fs_ctx, lun_ctx->fs_count * sizeof(UsbHsFsDriveLogicalUnitFileSystemContext*));
            if (tmp_fs_ctx)
            {
                lun_ctx->fs_ctx = tmp_fs_ctx;
                tmp_fs_ctx = NULL;
            }
        } else {
            /* Free filesystem context buffer. */
            free(lun_ctx->fs_ctx);
            lun_ctx->fs_ctx = NULL;
        }
    }

    return ret;
}

static bool usbHsFsMountRegisterFatVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, u8 *block, u64 block_addr)
{
    UsbHsFsDriveLogicalUnitContext *lun_ctx = (UsbHsFsDriveLogicalUnitContext*)fs_ctx->lun_ctx;

    u8 pdrv = 0;
    char name[MOUNT_NAME_LENGTH] = {0};
    FRESULT ff_res = FR_DISK_ERR;
    bool ret = false;

    /* Check if there's a free FatFs volume slot. */
    for(pdrv = 0; pdrv < FF_VOLUMES; pdrv++)
    {
        if (!g_fatFsVolumeTable[pdrv])
        {
            /* Jackpot. Prepare mount name. */
            sprintf(name, "%u:", pdrv);
            break;
        }
    }

    if (pdrv == FF_VOLUMES)
    {
        USBHSFS_LOG_MSG("Failed to locate a free FatFs volume slot! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    USBHSFS_LOG_MSG("Located free FatFs volume slot: %u (interface %d, LUN %u, FS %u).", pdrv, lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);

    /* Allocate memory for the FatFs object. */
    fs_ctx->fatfs = calloc(1, sizeof(FATFS));
    if (!fs_ctx->fatfs)
    {
        USBHSFS_LOG_MSG("Failed to allocate memory for FATFS object! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Set read-only flag. */
    fs_ctx->fatfs->ro_flag = ((fs_ctx->flags & UsbHsFsMountFlags_ReadOnly) || lun_ctx->write_protect);

    /* Copy VBR data. */
    fs_ctx->fatfs->winsect = (LBA_t)block_addr;
    memcpy(fs_ctx->fatfs->win, block, sizeof(VolumeBootRecord));

    /* Try to mount FAT volume. */
    ff_res = ff_mount(fs_ctx->fatfs, name, 1);
    if (ff_res != FR_OK)
    {
        USBHSFS_LOG_MSG("Failed to mount FAT volume! (%u) (interface %d, LUN %u, FS %u).", ff_res, lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Register devoptab device. */
    if (!usbHsFsMountRegisterDevoptabDevice(fs_ctx)) goto end;

    /* Update FatFs volume slot. */
    g_fatFsVolumeTable[pdrv] = true;

    /* Update return value. */
    ret = true;

end:
    /* Free stuff if something went wrong. */
    if (!ret && fs_ctx->fatfs)
    {
        if (ff_res == FR_OK) ff_unmount(name);
        free(fs_ctx->fatfs);
        fs_ctx->fatfs = NULL;
    }

    return ret;
}

static void usbHsFsMountUnregisterFatVolume(char *name, UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx)
{
    /* Update FatFs volume slot. */
    g_fatFsVolumeTable[fs_ctx->fatfs->pdrv] = false;

    /* Prepare mount name. */
    sprintf(name, "%u:", fs_ctx->fatfs->pdrv);

    /* Unmount FAT volume. */
    ff_unmount(name);

    /* Free FATFS object. */
    free(fs_ctx->fatfs);
    fs_ctx->fatfs = NULL;
}

#ifdef GPL_BUILD

static bool usbHsFsMountRegisterNtfsVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, u8 *block, u64 block_addr)
{
    UsbHsFsDriveLogicalUnitContext *lun_ctx = (UsbHsFsDriveLogicalUnitContext*)fs_ctx->lun_ctx;
    char name[MOUNT_NAME_LENGTH] = {0};
    u32 flags = fs_ctx->flags;
    bool ret = false;

    /* Allocate memory for the NTFS volume descriptor. */
    fs_ctx->ntfs = calloc(1, sizeof(ntfs_vd));
    if (!fs_ctx->ntfs)
    {
        USBHSFS_LOG_MSG("Failed to allocate memory for NTFS volume descriptor! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Allocate memory for the NTFS device descriptor. */
    fs_ctx->ntfs->dd = calloc(1, sizeof(ntfs_dd));
    if (!fs_ctx->ntfs->dd)
    {
        USBHSFS_LOG_MSG("Failed to allocate memory for NTFS device descriptor! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Get available devoptab device ID. */
    fs_ctx->device_id = usbHsFsMountGetAvailableDevoptabDeviceId();
    sprintf(name, MOUNT_NAME_PREFIX "%u", fs_ctx->device_id);

    /* Allocate memory for the NTFS device handle. */
    fs_ctx->ntfs->dev = ntfs_device_alloc(name, 0, ntfs_disk_io_get_dops(), fs_ctx->ntfs->dd);
    if (!fs_ctx->ntfs->dev)
    {
        USBHSFS_LOG_MSG("Failed to allocate memory for NTFS device object! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Copy VBR data. */
    memcpy(&(fs_ctx->ntfs->dd->vbr), block, sizeof(NTFS_BOOT_SECTOR));

    /* Setup NTFS device descriptor. */
    fs_ctx->ntfs->dd->lun_ctx = lun_ctx;
    fs_ctx->ntfs->dd->sector_start = block_addr;

    /* Setup NTFS volume descriptor. */
    fs_ctx->ntfs->id = fs_ctx->device_id;
    fs_ctx->ntfs->update_access_times = (flags & UsbHsFsMountFlags_UpdateAccessTimes);
    fs_ctx->ntfs->ignore_read_only_attr = (flags & UsbHsFsMountFlags_IgnoreFileReadOnlyAttribute);

    if ((flags & UsbHsFsMountFlags_ReadOnly) || lun_ctx->write_protect) fs_ctx->ntfs->flags |= NTFS_MNT_RDONLY;
    if (flags & UsbHsFsMountFlags_ReplayJournal) fs_ctx->ntfs->flags |= NTFS_MNT_RECOVER;
    if (flags & UsbHsFsMountFlags_IgnoreHibernation) fs_ctx->ntfs->flags |= NTFS_MNT_IGNORE_HIBERFILE;

    /* Try to mount NTFS volume. */
    fs_ctx->ntfs->vol = ntfs_device_mount(fs_ctx->ntfs->dev, fs_ctx->ntfs->flags);
    if (!fs_ctx->ntfs->vol)
    {
        USBHSFS_LOG_MSG("Failed to mount NTFS volume! (%d) (interface %d, LUN %u, FS %u).", ntfs_volume_error(errno), lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Create all LRU caches. */
    /* No errors returned -- if this fails internally, LRU caches simply won't be available. */
    ntfs_create_lru_caches(fs_ctx->ntfs->vol);

    /* Setup volume case sensitivity. */
	if (flags & UsbHsFsMountFlags_IgnoreCaseSensitivity) ntfs_set_ignore_case(fs_ctx->ntfs->vol);

    /* Set appropriate flags for showing system/hidden files on the NTFS volume. */
    ntfs_set_shown_files(fs_ctx->ntfs->vol, (flags & UsbHsFsMountFlags_ShowSystemFiles) != 0, (flags & UsbHsFsMountFlags_ShowHiddenFiles) != 0, false);

    /* Get NTFS volume free space. */
    /* This will speed up subsequent calls to stavfs(). */
    if (ntfs_volume_get_free_space(fs_ctx->ntfs->vol) < 0)
    {
        USBHSFS_LOG_MSG("Failed to retrieve free space from NTFS volume! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Register devoptab device. */
    if (!usbHsFsMountRegisterDevoptabDevice(fs_ctx)) goto end;

    /* Update return value. */
    ret = true;

end:
    /* Free stuff if something went wrong. */
    if (!ret && fs_ctx->ntfs)
    {
        if (fs_ctx->ntfs->vol)
        {
            /* ntfs_umount() takes care of calling both ntfs_create_lru_caches() and ntfs_device_free() for us. */
            ntfs_umount(fs_ctx->ntfs->vol, true);
            fs_ctx->ntfs->vol = NULL;
            fs_ctx->ntfs->dev = NULL;
        }

        if (fs_ctx->ntfs->dev)
        {
            ntfs_device_free(fs_ctx->ntfs->dev);
            fs_ctx->ntfs->dev = NULL;
        }

        if (fs_ctx->ntfs->dd)
        {
            free(fs_ctx->ntfs->dd);
            fs_ctx->ntfs->dd = NULL;
        }

        free(fs_ctx->ntfs);
        fs_ctx->ntfs = NULL;
    }

    return ret;
}

static void usbHsFsMountUnregisterNtfsVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx)
{
    /* Unmount NTFS volume. */
    /* We don't need to manually free the NTFS device handle nor the LRU caches - ntfs_umount() does that for us. */
    ntfs_umount(fs_ctx->ntfs->vol, true);
    fs_ctx->ntfs->vol = NULL;
    fs_ctx->ntfs->dev = NULL;

    /* Free NTFS device descriptor. */
    free(fs_ctx->ntfs->dd);
    fs_ctx->ntfs->dd = NULL;

    /* Free NTFS volume descriptor. */
    free(fs_ctx->ntfs);
    fs_ctx->ntfs = NULL;
}

static bool usbHsFsMountRegisterExtVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx, u64 block_addr, u64 block_count)
{
    UsbHsFsDriveLogicalUnitContext *lun_ctx = (UsbHsFsDriveLogicalUnitContext*)fs_ctx->lun_ctx;
    bool ret = false, vol_mounted = false;

    /* Allocate memory for the EXT volume descriptor. */
    fs_ctx->ext = calloc(1, sizeof(ext_vd));
    if (!fs_ctx->ext)
    {
        USBHSFS_LOG_MSG("Failed to allocate memory for EXT volume descriptor! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Setup EXT block device handle. */
    fs_ctx->ext->bdev = ext_disk_io_alloc_blockdev(lun_ctx, block_addr, block_count);
    if (!fs_ctx->ext->bdev)
    {
        USBHSFS_LOG_MSG("Failed to setup EXT block device handle! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Get available devoptab device ID. */
    fs_ctx->device_id = usbHsFsMountGetAvailableDevoptabDeviceId();

    /* Setup EXT volume descriptor. */
    sprintf(fs_ctx->ext->dev_name, MOUNT_NAME_PREFIX "%u", fs_ctx->device_id);
    fs_ctx->ext->flags = fs_ctx->flags;
    fs_ctx->ext->id = fs_ctx->device_id;

    /* Try to mount EXT volume. */
    if (!ext_mount(fs_ctx->ext))
    {
        USBHSFS_LOG_MSG("Failed to mount EXT volume! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    vol_mounted = true;

    /* Register devoptab device. */
    if (!usbHsFsMountRegisterDevoptabDevice(fs_ctx)) goto end;

    /* Update return value. */
    ret = true;

end:
    /* Free stuff if something went wrong. */
    if (!ret && fs_ctx->ext)
    {
        if (vol_mounted) ext_umount(fs_ctx->ext);

        if (fs_ctx->ext->bdev)
        {
            ext_disk_io_free_blockdev(fs_ctx->ext->bdev);
            fs_ctx->ext->bdev = NULL;
        }

        free(fs_ctx->ext);
        fs_ctx->ext = NULL;
    }

    return ret;
}

static void usbHsFsMountUnregisterExtVolume(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx)
{
    /* Unmount EXT volume. */
    ext_umount(fs_ctx->ext);

    /* Free EXT block device handle. */
    ext_disk_io_free_blockdev(fs_ctx->ext->bdev);
    fs_ctx->ext->bdev = NULL;

    /* Free EXT volume descriptor. */
    free(fs_ctx->ext);
    fs_ctx->ext = NULL;
}

#endif  /* GPL_BUILD */

static bool usbHsFsMountRegisterDevoptabDevice(UsbHsFsDriveLogicalUnitFileSystemContext *fs_ctx)
{
#ifdef DEBUG
    UsbHsFsDriveLogicalUnitContext *lun_ctx = (UsbHsFsDriveLogicalUnitContext*)fs_ctx->lun_ctx;
#endif

    char name[MOUNT_NAME_LENGTH] = {0};
    const devoptab_t *fs_device = NULL;
    int ad_res = -1;
    u32 *tmp_device_ids = NULL;
    bool ret = false;

    /* Generate devoptab mount name. */
    fs_ctx->name = calloc(MOUNT_NAME_LENGTH, sizeof(char));
    if (!fs_ctx->name)
    {
        USBHSFS_LOG_MSG("Failed to allocate memory for the mount name! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    fs_ctx->device_id = usbHsFsMountGetAvailableDevoptabDeviceId();
    USBHSFS_LOG_MSG("Available device ID: %u (interface %d, LUN %u, FS %u).", fs_ctx->device_id, lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);

    sprintf(fs_ctx->name, MOUNT_NAME_PREFIX "%u", fs_ctx->device_id);
    sprintf(name, "%s:", fs_ctx->name); /* Will be used if something goes wrong and we end up having to remove the devoptab device. */

    /* Allocate memory for the current working directory. */
    fs_ctx->cwd = calloc(MAX_PATH_LENGTH, sizeof(char));
    if (!fs_ctx->cwd)
    {
        USBHSFS_LOG_MSG("Failed to allocate memory for the current working directory! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    fs_ctx->cwd[0] = '/';   /* Always start at the root directory. */

    /* Allocate memory for our devoptab virtual device interface. */
    fs_ctx->device = calloc(1, sizeof(devoptab_t));
    if (!fs_ctx->device)
    {
        USBHSFS_LOG_MSG("Failed to allocate memory for devoptab virtual device interface! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Retrieve pointer to the devoptab interface from our filesystem type. */
    switch(fs_ctx->fs_type)
    {
        case UsbHsFsDriveLogicalUnitFileSystemType_FAT:     /* FAT12/FAT16/FAT32/exFAT. */
            fs_device = ffdev_get_devoptab();
            break;
#ifdef GPL_BUILD
        case UsbHsFsDriveLogicalUnitFileSystemType_NTFS:    /* NTFS. */
            fs_device = ntfsdev_get_devoptab();
            break;
        case UsbHsFsDriveLogicalUnitFileSystemType_EXT:     /* EXT2/3/4. */
            fs_device = extdev_get_devoptab();
            break;
#endif
        default:
            USBHSFS_LOG_MSG("Invalid FS type provided! (0x%02X) (interface %d, LUN %u, FS %u).", fs_ctx->fs_type, lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
            break;
    }

    if (!fs_device)
    {
        USBHSFS_LOG_MSG("Failed to get pointer to devoptab interface! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Copy devoptab interface data and set mount name and device data. */
    memcpy(fs_ctx->device, fs_device, sizeof(devoptab_t));
    fs_ctx->device->name = fs_ctx->name;
    fs_ctx->device->deviceData = fs_ctx;

    /* Add devoptab device. */
    ad_res = AddDevice(fs_ctx->device);
    if (ad_res < 0)
    {
        USBHSFS_LOG_MSG("AddDevice failed! (%d) (interface %d, LUN %u, FS %u).", ad_res, lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    /* Reallocate devoptab device IDs buffer. */
    tmp_device_ids = realloc(g_devoptabDeviceIds, (g_devoptabDeviceCount + 1) * sizeof(u32));
    if (!tmp_device_ids)
    {
        USBHSFS_LOG_MSG("Failed to reallocate devoptab device IDs buffer! (interface %d, LUN %u, FS %u).", lun_ctx->usb_if_id, lun_ctx->lun, fs_ctx->fs_idx);
        goto end;
    }

    g_devoptabDeviceIds = tmp_device_ids;
    tmp_device_ids = NULL;

    /* Store devoptab device ID and increase devoptab virtual device count. */
    g_devoptabDeviceIds[g_devoptabDeviceCount++] = fs_ctx->device_id;

    /* Update return value. */
    ret = true;

end:
    /* Free stuff if something went wrong. */
    if (!ret)
    {
        if (ad_res >= 0) RemoveDevice(name);

        if (fs_ctx->device)
        {
            free(fs_ctx->device);
            fs_ctx->device = NULL;
        }

        if (fs_ctx->cwd)
        {
            free(fs_ctx->cwd);
            fs_ctx->cwd = NULL;
        }

        if (fs_ctx->name)
        {
            free(fs_ctx->name);
            fs_ctx->name = NULL;
        }
    }

    return ret;
}

static u32 usbHsFsMountGetAvailableDevoptabDeviceId(void)
{
    if (!g_devoptabDeviceCount || !g_devoptabDeviceIds) return 0;

    u32 i = 0, ret = 0;

    while(true)
    {
        if (i >= g_devoptabDeviceCount) break;

        if (ret == g_devoptabDeviceIds[i])
        {
            ret++;
            i = 0;
        } else {
            i++;
        }
    }

    return ret;
}

static void usbHsFsMountUnsetDefaultDevoptabDevice(u32 device_id)
{
    SCOPED_LOCK(&g_devoptabDefaultDeviceMutex)
    {
        /* Check if the provided device ID matches the current default devoptab device ID. */
        if (g_devoptabDefaultDeviceId == DEVOPTAB_INVALID_ID || g_devoptabDefaultDeviceId != device_id) break;

        USBHSFS_LOG_MSG("Current default devoptab device matches provided device ID! (%u).", device_id);

        u32 cur_device_id = 0;
        const devoptab_t *cur_default_devoptab = GetDeviceOpTab("");

        /* Check if the current default devoptab device is the one we previously set. */
        /* If so, set the SD card as the new default devoptab device. */
        if (cur_default_devoptab && cur_default_devoptab->name && strlen(cur_default_devoptab->name) >= 4 && sscanf(cur_default_devoptab->name, MOUNT_NAME_PREFIX "%u", &cur_device_id) == 1 && \
            cur_device_id == device_id)
        {
            USBHSFS_LOG_MSG("Setting SD card as the default devoptab device.");
            setDefaultDevice(FindDevice("sdmc:"));
        }

        /* Update default device ID. */
        g_devoptabDefaultDeviceId = DEVOPTAB_INVALID_ID;
    }
}
