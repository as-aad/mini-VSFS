// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

// below contains some basic structures you need for your project
// you are free to create more structures as you require

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;              // 0x4D565346
    uint32_t version;            // 1
    uint32_t block_size;         // 4096
    uint64_t total_blocks;       // Calculated
    uint64_t inode_count;        // From CLI
    uint64_t inode_bitmap_start; // Block 1
    uint64_t inode_bitmap_blocks; // 1
    uint64_t data_bitmap_start;  // Block 2
    uint64_t data_bitmap_blocks; // 1
    uint64_t inode_table_start;  // Block 3
    uint64_t inode_table_blocks; // Calculated
    uint64_t data_region_start;  // After inode table
    uint64_t data_region_blocks; // Remaining blocks
    uint64_t root_inode;         // 1
    uint64_t mtime_epoch;        // Build time
    uint32_t flags;              // 0
    
    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint32_t checksum;            // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;               // File type (0100000=file, 0040000=dir)
    uint16_t links;              // Link count
    uint32_t uid;                // 0
    uint32_t gid;                // 0
    uint64_t size_bytes;         // File size
    uint64_t atime;              // Access time
    uint64_t mtime;              // Modification time
    uint64_t ctime;              // Change time
    uint32_t direct[12];         // Direct block pointers
    uint32_t reserved_0;         // 0
    uint32_t reserved_1;         // 0
    uint32_t reserved_2;         // 0
    uint32_t proj_id;            // Group ID
    uint32_t uid16_gid16;        // 0
    uint64_t xattr_ptr;          // 0

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;           // Inode number (0 if free)
    uint8_t  type;               // 1=file, 2=directory
    char     name[58];           // Filename
    uint8_t  checksum; // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

// Helper function to find first free bit in bitmap
static int find_first_free_bit(const uint8_t* bitmap, int total_bits) {
    for (int i = 0; i < total_bits; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!(bitmap[byte_idx] & (1 << bit_idx))) {
            return i;
        }
    }
    return -1; // No free bit found
}

// Helper function to set bit in bitmap
static void set_bit(uint8_t* bitmap, int bit_num) {
    int byte_idx = bit_num / 8;
    int bit_idx = bit_num % 8;
    bitmap[byte_idx] |= (1 << bit_idx);
}

// Helper function to clear bit in bitmap
static void clear_bit(uint8_t* bitmap, int bit_num) {
    int byte_idx = bit_num / 8;
    int bit_idx = bit_num % 8;
    bitmap[byte_idx] &= ~(1 << bit_idx);
}

int main(int argc, char* argv[]) {
    crc32_init();
    
    // Parse command line arguments
    char* output_image = NULL;
    uint32_t size_kib = 0;
    uint32_t inode_count = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            output_image = argv[++i];
        } else if (strcmp(argv[i], "--size-kib") == 0 && i + 1 < argc) {
            size_kib = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--inodes") == 0 && i + 1 < argc) {
            inode_count = atoi(argv[++i]);
        }
    }
    
    // Validate arguments
    if (!output_image || size_kib < 180 || size_kib > 4096 || size_kib % 4 != 0 ||
        inode_count < 128 || inode_count > 512) {
        fprintf(stderr, "Usage: mkfs_builder --image <file> --size-kib <180-4096> --inodes <128-512>\n");
        fprintf(stderr, "size-kib must be multiple of 4\n");
        return 1;
    }
    
    // Calculate file system layout
    uint32_t total_blocks = (size_kib * 1024) / BS;
    uint32_t inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS;
    uint32_t data_region_start = 3 + inode_table_blocks; // After superblock, bitmaps, inode table
    uint32_t data_region_blocks = total_blocks - data_region_start;
    
    if (data_region_blocks < 1) {
        fprintf(stderr, "Error: Not enough space for data blocks\n");
        return 1;
    }
    
    // Allocate memory for the entire file system
    uint8_t* fs_image = calloc(total_blocks * BS, 1);
    if (!fs_image) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        return 1;
    }
    
    // Initialize superblock
    superblock_t* sb = (superblock_t*)fs_image;
    sb->magic = 0x4D565346;
    sb->version = 1;
    sb->block_size = BS;
    sb->total_blocks = total_blocks;
    sb->inode_count = inode_count;
    sb->inode_bitmap_start = 1;
    sb->inode_bitmap_blocks = 1;
    sb->data_bitmap_start = 2;
    sb->data_bitmap_blocks = 1;
    sb->inode_table_start = 3;
    sb->inode_table_blocks = inode_table_blocks;
    sb->data_region_start = data_region_start;
    sb->data_region_blocks = data_region_blocks;
    sb->root_inode = 1;
    sb->mtime_epoch = time(NULL);
    sb->flags = 0;
    
    // Get bitmaps
    uint8_t* inode_bitmap = fs_image + (1 * BS);
    uint8_t* data_bitmap = fs_image + (2 * BS);
    
    // Mark root inode (inode #1) as used
    set_bit(inode_bitmap, 0); // inode #1 is at index 0 (1-indexed)
    
    // Mark first data block as used for root directory
    set_bit(data_bitmap, 0); // First data block
    
    // Initialize root directory inode
    inode_t* root_inode = (inode_t*)(fs_image + (3 * BS)); // First inode in table
    root_inode->mode = 0040000; // Directory
    root_inode->links = 2; // . and ..
    root_inode->uid = 0;
    root_inode->gid = 0;
    root_inode->size_bytes = 2 * sizeof(dirent64_t); // . and .. entries
    root_inode->atime = time(NULL);
    root_inode->mtime = time(NULL);
    root_inode->ctime = time(NULL);
    root_inode->direct[0] = data_region_start; // First data block
    for (int i = 1; i < 12; i++) {
        root_inode->direct[i] = 0; // Unused
    }
    root_inode->reserved_0 = 0;
    root_inode->reserved_1 = 0;
    root_inode->reserved_2 = 0;
    root_inode->proj_id = 14; // You can set this to your group ID
    root_inode->uid16_gid16 = 0;
    root_inode->xattr_ptr = 0;
    
    // Create root directory entries (. and ..)
    dirent64_t* root_dir = (dirent64_t*)(fs_image + (data_region_start * BS));
    
    // . entry
    root_dir[0].inode_no = 1;
    root_dir[0].type = 2; // Directory
    strcpy(root_dir[0].name, ".");
    
    // .. entry
    root_dir[1].inode_no = 1;
    root_dir[1].type = 2; // Directory
    strcpy(root_dir[1].name, "..");
    
    // Finalize checksums
    superblock_crc_finalize(sb);
    inode_crc_finalize(root_inode);
    dirent_checksum_finalize(&root_dir[0]);
    dirent_checksum_finalize(&root_dir[1]);
    
    // Write the file system image
    FILE* fp = fopen(output_image, "wb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot create output file %s\n", output_image);
        free(fs_image);
        return 1;
    }
    
    if (fwrite(fs_image, BS, total_blocks, fp) != total_blocks) {
        fprintf(stderr, "Error: Failed to write file system image\n");
        fclose(fp);
        free(fs_image);
        return 1;
    }
    
    fclose(fp);
    free(fs_image);
    
    printf("File system created successfully: %s\n", output_image);
    printf("Size: %u KB, Inodes: %u, Total blocks: %u\n", size_kib, inode_count, total_blocks);
    
    return 0;
}
