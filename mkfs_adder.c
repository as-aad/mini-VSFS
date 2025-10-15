#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
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

// Helper function to find first free directory entry
static int find_free_dirent(dirent64_t* dir_block, int max_entries) {
    for (int i = 0; i < max_entries; i++) {
        if (dir_block[i].inode_no == 0) {
            return i;
        }
    }
    return -1; // No free entry found
}

int main(int argc, char* argv[]) {
    crc32_init();
    
    // Parse command line arguments
    char* input_image = NULL;
    char* output_image = NULL;
    char* filename = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_image = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_image = argv[++i];
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            filename = argv[++i];
        }
    }
    
    // Validate arguments
    if (!input_image || !output_image || !filename) {
        fprintf(stderr, "Usage: mkfs_adder --input <input.img> --output <output.img> --file <filename>\n");
        return 1;
    }
    
    // Open and read the input file system image
    FILE* fp = fopen(input_image, "rb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open input file %s\n", input_image);
        return 1;
    }
    
    // Get file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    // Read the entire file system
    uint8_t* fs_image = malloc(file_size);
    if (!fs_image) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        fclose(fp);
        return 1;
    }
    
    if (fread(fs_image, 1, file_size, fp) != file_size) {
        fprintf(stderr, "Error: Failed to read input file system\n");
        fclose(fp);
        free(fs_image);
        return 1;
    }
    fclose(fp);
    
    // Parse superblock
    superblock_t* sb = (superblock_t*)fs_image;
    if (sb->magic != 0x4D565346) {
        fprintf(stderr, "Error: Invalid file system magic number\n");
        free(fs_image);
        return 1;
    }
    
    // Get bitmaps and inode table
    uint8_t* inode_bitmap = fs_image + (sb->inode_bitmap_start * BS);
    uint8_t* data_bitmap = fs_image + (sb->data_bitmap_start * BS);
    inode_t* inode_table = (inode_t*)(fs_image + (sb->inode_table_start * BS));
    
    // Find free inode
    int free_inode_idx = find_first_free_bit(inode_bitmap, sb->inode_count);
    if (free_inode_idx == -1) {
        fprintf(stderr, "Error: No free inodes available\n");
        free(fs_image);
        return 1;
    }
    
    // Open the file to be added
    FILE* file_fp = fopen(filename, "rb");
    if (!file_fp) {
        fprintf(stderr, "Error: Cannot open file %s\n", filename);
        free(fs_image);
        return 1;
    }
    
    // Get file size
    fseek(file_fp, 0, SEEK_END);
    long file_size_bytes = ftell(file_fp);
    fseek(file_fp, 0, SEEK_SET);
    
    // Check if file fits in 12 direct blocks
    if (file_size_bytes > 12 * BS) {
        fprintf(stderr, "Warning: File %s is too large to fit in 12 direct blocks\n", filename);
        fclose(file_fp);
        free(fs_image);
        return 1;
    }
    
    // Calculate number of blocks needed
    int blocks_needed = (file_size_bytes + BS - 1) / BS;
    
    // Find free data blocks
    int* data_blocks = malloc(blocks_needed * sizeof(int));
    if (!data_blocks) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        fclose(file_fp);
        free(fs_image);
        return 1;
    }
    
    for (int i = 0; i < blocks_needed; i++) {
        int free_data_idx = find_first_free_bit(data_bitmap, sb->data_region_blocks);
        if (free_data_idx == -1) {
            fprintf(stderr, "Error: Not enough free data blocks\n");
            fclose(file_fp);
            free(fs_image);
            free(data_blocks);
            return 1;
        }
        data_blocks[i] = sb->data_region_start + free_data_idx;
        set_bit(data_bitmap, free_data_idx);
    }
    
    // Create new inode for the file
    inode_t* new_inode = &inode_table[free_inode_idx];
    new_inode->mode = 0100000; // Regular file
    new_inode->links = 1;
    new_inode->uid = 0;
    new_inode->gid = 0;
    new_inode->size_bytes = file_size_bytes;
    new_inode->atime = time(NULL);
    new_inode->mtime = time(NULL);
    new_inode->ctime = time(NULL);
    
    // Set direct block pointers
    for (int i = 0; i < 12; i++) {
        if (i < blocks_needed) {
            new_inode->direct[i] = data_blocks[i];
        } else {
            new_inode->direct[i] = 0;
        }
    }
    
    new_inode->reserved_0 = 0;
    new_inode->reserved_1 = 0;
    new_inode->reserved_2 = 0;
    new_inode->proj_id = 0;
    new_inode->uid16_gid16 = 0;
    new_inode->xattr_ptr = 0;
    
    // Mark inode as used
    set_bit(inode_bitmap, free_inode_idx);
    
    // Read file data and write to data blocks
    for (int i = 0; i < blocks_needed; i++) {
        uint8_t* block_data = fs_image + (data_blocks[i] * BS);
        size_t bytes_to_read = (i == blocks_needed - 1) ? 
            (file_size_bytes - i * BS) : BS;
        
        if (fread(block_data, 1, bytes_to_read, file_fp) != bytes_to_read) {
            fprintf(stderr, "Error: Failed to read file data\n");
            fclose(file_fp);
            free(fs_image);
            free(data_blocks);
            return 1;
        }
    }
    fclose(file_fp);
    
    // Add directory entry to root directory
    inode_t* root_inode = &inode_table[0]; // Root inode is at index 0 (inode #1)
    dirent64_t* root_dir = (dirent64_t*)(fs_image + (root_inode->direct[0] * BS));
    
    int free_dirent_idx = find_free_dirent(root_dir, BS / sizeof(dirent64_t));
    if (free_dirent_idx == -1) {
        fprintf(stderr, "Error: Root directory is full\n");
        free(fs_image);
        free(data_blocks);
        return 1;
    }
    
    // Create directory entry
    root_dir[free_dirent_idx].inode_no = free_inode_idx + 1; // 1-indexed
    root_dir[free_dirent_idx].type = 1; // File
    strncpy(root_dir[free_dirent_idx].name, filename, 57);
    root_dir[free_dirent_idx].name[57] = '\0'; // Ensure null termination
    
    // Update root directory size
    root_inode->size_bytes += sizeof(dirent64_t);
    root_inode->mtime = time(NULL);
    root_inode->ctime = time(NULL);
    
    // Update superblock timestamp
    sb->mtime_epoch = time(NULL);
    
    // Finalize checksums
    superblock_crc_finalize(sb);
    inode_crc_finalize(new_inode);
    inode_crc_finalize(root_inode);
    dirent_checksum_finalize(&root_dir[free_dirent_idx]);
    
    // Write the updated file system
    FILE* out_fp = fopen(output_image, "wb");
    if (!out_fp) {
        fprintf(stderr, "Error: Cannot create output file %s\n", output_image);
        free(fs_image);
        free(data_blocks);
        return 1;
    }
    
    if (fwrite(fs_image, 1, file_size, out_fp) != file_size) {
        fprintf(stderr, "Error: Failed to write output file system\n");
        fclose(out_fp);
        free(fs_image);
        free(data_blocks);
        return 1;
    }
    
    fclose(out_fp);
    free(fs_image);
    free(data_blocks);
    
    printf("File %s added successfully to %s\n", filename, output_image);
    printf("Inode: %d, Size: %ld bytes, Blocks: %d\n", 
           free_inode_idx + 1, file_size_bytes, blocks_needed);
    
    return 0;
}
