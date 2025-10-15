# MiniVSFS Builder & Adder

This repository contains two C programs for creating and modifying a simple virtual file system image called **MiniVSFS**.

---

## 📦 Overview

### 🧱 `mkfs_builder.c`
Builds a new empty file system image with:
- A valid **superblock**
- **Bitmaps** for inodes and data blocks
- An initialized **root directory** with entries `.` and `..`

### ➕ `mkfs_adder.c`
Adds a regular file into an existing MiniVSFS image by:
- Allocating a new inode and data blocks
- Updating the bitmaps
- Inserting a directory entry under the root
- Recomputing checksums (CRC32, directory checksum, etc.)

---

## 🧰 Build Instructions

```bash
gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c -o mkfs_adder
```

---

## 🚀 Usage

### 1️⃣ Create a new file system image

```bash
./mkfs_builder --image fs.img --size-kib 512 --inodes 256
```

**Arguments:**
- `--image` : Output image file name (e.g., `fs.img`)
- `--size-kib` : Total size in KiB (must be between 180–4096, multiple of 4)
- `--inodes` : Number of inodes (128–512)

**Example Output:**
```
File system created successfully: fs.img
Size: 512 KB, Inodes: 256, Total blocks: 128
```

---

### 2️⃣ Add a file into the file system

```bash
./mkfs_adder --input fs.img --output new_fs.img --file hello.txt
```

**Arguments:**
- `--input` : Input image file (existing MiniVSFS)
- `--output` : Output image file after modification
- `--file` : File to insert into the image

**Example Output:**
```
File hello.txt added successfully to new_fs.img
Inode: 5, Size: 1024 bytes, Blocks: 1
```

---

## 🧪 Example Workflow

```bash
# Step 1: Create filesystem
./mkfs_builder --image fs.img --size-kib 512 --inodes 128

# Step 2: Add file into filesystem
echo "Hello MiniFS!" > hello.txt
./mkfs_adder --input fs.img --output fs_updated.img --file hello.txt
```

---

## ⚙️ Internal Structure

| Region | Description | Starts At Block |
|---------|--------------|----------------|
| Superblock | FS metadata (magic, version, layout, etc.) | 0 |
| Inode Bitmap | Tracks used/free inodes | 1 |
| Data Bitmap | Tracks used/free data blocks | 2 |
| Inode Table | Stores inodes | 3 |
| Data Region | File & directory data | after inode table |

---

## 🧑‍💻 Developers
- **Author:** (Your Name)
- **Language:** C17
- **Version:** 1.0
- **License:** MIT or your preferred license

---

## 📁 Repository Structure
```
.
├── mkfs_builder.c    # Creates the file system image
├── mkfs_adder.c      # Adds files into the image
├── README.md         # Documentation (this file)
└── .gitignore        # (optional) Build and temporary file ignores
```

---

## 🧹 Optional `.gitignore`

```gitignore
# Ignore compiled binaries and temporary files
*.o
*.out
*.exe
*.img
*.bin
*.log
*.tmp
```

---

## 🧩 Notes

- The file system uses **CRC32 checksums** for data integrity.
- Supports **up to 12 direct blocks per file** (no indirect addressing).
- Root inode number is **1** (index 0 in table).
- File names limited to **58 characters**.

---
