# Docs++ - Distributed File Management System

A simplified Google-Docs-like system implemented in C with distributed architecture, featuring file management, access control, and real-time collaboration capabilities.

## 📋 Table of Contents

- [Architecture](#architecture)
- [Building the Project](#building-the-project)
- [Running Docs++](#running-docs)
- [User Commands](#user-commands)
- [Advanced Features](#advanced-features)
- [Implementation Details](#implementation-details)
- [File Structure](#file-structure)

---

## 🏗️ Architecture

Docs++ consists of three main components:

### 1. Name Server (NM)

- **Port:** 5050
- **Role:** Central coordinator for routing, metadata, and access control
- **Responsibilities:**
  - Route client requests to appropriate Storage Servers
  - Manage file-to-server mappings
  - Handle user registration and tracking
  - Execute commands via EXEC operation

### 2. Storage Server (SS)

- **Port:** 6001 (client), 6000 (NM)
- **Role:** File storage and management
- **Responsibilities:**
  - Store and retrieve file content
  - Manage file metadata (owner, permissions, timestamps)
  - Handle sentence-level locking for concurrent writes
  - Maintain undo history and checkpoints
  - Enforce access control (R/W permissions)
  - Support hierarchical folder structure
  - Replicate data to paired SS for fault tolerance

### 3. Client (CLI)

- **Role:** User interface for interacting with the system
- **Features:**
  - Interactive REPL (Read-Eval-Print Loop)
  - Commands for all file operations
  - Direct communication with SS for data operations

### Communication Protocol

- **Transport:** TCP sockets
- **Format:** Line-delimited JSON (JSONL)
- **Routing:** NM provides routes, CLI connects directly to SS for data operations

---

## 🔨 Building the Project

### Prerequisites

- GCC compiler
- Make
- POSIX-compliant system (Linux/macOS)

### Build Commands

```bash
# Clean previous build
make clean

# Build all components
make all

# Build individual components
make nm    # Name Server
make ss    # Storage Server
make cli   # Client
```

### Build Output

After successful build, you'll have three executables:

- `nm` - Name Server binary
- `ss` - Storage Server binary
- `cli` - Client binary

---

## 🚀 Running Docs++

### Step 1: Start Name Server

Open a terminal and run:

```bash
./nm
```

Expected output:

```
Name Server listening on port 5050
```

### Step 2: Start Storage Server

Open a second terminal and run:

```bash
./ss
```

Expected output:

```
Storage Server started
Client port: 6001
NM port: 6000
Registered with Name Server
```

### Step 3: Start Client

Open a third terminal and run:

```bash
./cli
```

You'll be prompted for a username:

```
Username: alice
[NM] {"status":0,"msg":"hello alice"}

Available commands:
  VIEW [-a] [-l]            - List files
  READ <file>               - Read file
  WRITE <file> <sent_idx>   - Edit sentence (use ETIRW to commit)
  CREATE <file>             - Create file
  DELETE <file>             - Delete file
  INFO <file>               - Show file details
  UNDO <file>               - Undo last edit
  STREAM <file>             - Stream content
  LIST                      - List users
  ADDACCESS -R/-W <file> <user> - Grant access
  REMACCESS <file> <user>   - Remove access
  REQUESTACCESS <file> <owner> - Request access to file
  VIEWREQUESTS              - View pending access requests
  APPROVE <file> <requester> - Approve access request
  DENY <file> <requester>   - Deny access request
  EXEC <file>               - Execute file as commands
  CREATEFOLDER <name>       - Create a folder
  MOVE <file> <folder>      - Move file to folder
  VIEWFOLDER <folder>       - View files in folder
  CHECKPOINT <file> <tag>   - Create checkpoint
  VIEWCHECKPOINT <file> <tag> - View checkpoint
  REVERT <file> <tag>       - Revert to checkpoint
  LISTCHECKPOINTS <file>    - List all checkpoints
  EXIT                      - Quit

docs++>
```

---

## 📝 User Commands

### 1. CREATE - Create a New File

**Syntax:** `CREATE <filename>`

**Description:** Creates an empty file with the caller as owner.

**Example:**

```
docs++> CREATE document.txt
✅ Created: {"status":0,"msg":"file created"}
```

**Features:**

- Creates empty file
- Sets owner to current user
- Returns error if file already exists

---

### 2. VIEW - List Files

**Syntax:**

- `VIEW` - List files you have access to
- `VIEW -a` - List ALL files on the system
- `VIEW -l` - List files with details
- `VIEW -al` or `VIEW -la` - List all files with details

**Description:** Displays files based on access permissions and flags.

**Examples:**

```
docs++> VIEW
📋 Files:
document.txt
myfile.txt

docs++> VIEW -l
📋 Files:
document.txt | Owner: alice | Words: 42 | Chars: 256 | Modified: 2025-11-10 10:30:00

docs++> VIEW -a
📋 Files:
document.txt
myfile.txt
other_user_file.txt

docs++> VIEW -al
📋 Files:
document.txt | Owner: alice | Words: 42 | Chars: 256 | Modified: 2025-11-10 10:30:00
other_user_file.txt | Owner: bob | Words: 10 | Chars: 50 | Modified: 2025-11-10 09:15:00
```

**Features:**

- Default VIEW shows only accessible files
- `-a` flag shows all files regardless of access
- `-l` flag includes metadata (owner, word count, character count, timestamp)
- Flags can be combined

---

### 3. READ - Read File Content

**Syntax:** `READ <filename>`

**Description:** Displays the complete content of a file.

**Example:**

```
docs++> READ document.txt

📄 document.txt:
This is the first sentence. Here is another sentence! And a third one?
━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

**Features:**

- Shows entire file content
- Checks read access (must be owner or have R/W permission)
- Preserves original sentence delimiters (. ? !)

---

### 4. WRITE - Edit File Content

**Syntax:** `WRITE <filename> <sentence_index>`

**Description:** Opens an interactive edit session for a specific sentence.

**Example:**

```
docs++> WRITE document.txt 0
✅ Lock acquired on sentence 0

Enter edits (format: <word_idx> <content>)
Type ETIRW when done to commit

docs++> 0 Hello
✏️  Edit applied
docs++> 1 world
✏️  Edit applied
docs++> 2 from
✏️  Edit applied
docs++> 3 Alice
✏️  Edit applied
docs++> ETIRW
✅ Changes committed!

💡 Use 'READ document.txt' to see changes
```

**Features:**

- **Sentence-level locking:** Prevents concurrent edits to same sentence
- **Word-level editing:** Update specific words by index
- **Multiple edits:** Make multiple changes before committing
- **ETIRW to commit:** Special command to end session and save changes
- **Automatic delimiter handling:** Splits on `.?!` characters
- **Undo support:** Creates backup before commit

**Important Notes:**

- Sentence indices start at 0
- Every `.?!` creates a new sentence (even in middle of words like "e.g.")
- Lock is held until ETIRW or disconnection
- Other users get ERR_LOCKED if they try to edit same sentence

---

### 5. UNDO - Revert Last Change

**Syntax:** `UNDO <filename>`

**Description:** Reverts the file to its state before the last WRITE commit.

**Example:**

```
docs++> UNDO document.txt
↩️  Undo successful!
```

**Features:**

- Single-level undo (only last commit)
- Available to any user (not just owner)
- Restores both content and sentence structure

---

### 6. INFO - Show File Metadata

**Syntax:** `INFO <filename>`

**Description:** Displays detailed information about a file.

**Example:**

```
docs++> INFO document.txt

📊 Info for document.txt:
━━━━━━━━━━━━━━━━━━━━━━━━━━━
Filename: document.txt
Owner: alice
Word Count: 4
Character Count: 21
Last Modified: 2025-11-10 10:30:45
Sentences: 1
━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

**Features:**

- Shows complete file metadata
- Includes statistics (words, characters, sentences)
- Displays ownership and timestamps

---

### 7. DELETE - Remove File

**Syntax:** `DELETE <filename>`

**Description:** Permanently deletes a file (owner only).

**Example:**

```
docs++> DELETE document.txt
✅ Deleted: {"status":0,"msg":"deleted"}
```

**Features:**

- Owner-only operation
- Removes file, metadata, and undo history
- Cannot be undone
- Returns ERR_UNAUTHORIZED if not owner

---

### 8. STREAM - Stream File Content

**Syntax:** `STREAM <filename>`

**Description:** Outputs file content word-by-word with 100ms delay.

**Example:**

```
docs++> STREAM document.txt

🎬 Streaming document.txt:
━━━━━━━━━━━━━━━━━━━━━━━━━━━
Hello
world
from
Alice
━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

**Features:**

- 100ms delay between words
- Useful for real-time display
- Checks read access
- Gracefully handles server interruptions

---

### 9. LIST - Show Connected Users

**Syntax:** `LIST`

**Description:** Displays all users who have connected to the system.

**Example:**

```
docs++> LIST

👥 Users: alice,bob,charlie
```

**Features:**

- Shows all registered users
- Maintained by Name Server
- No duplicates (same user counted once)

---

### 10. ADDACCESS - Grant File Access

**Syntax:**

- `ADDACCESS -R <filename> <username>` - Grant read access
- `ADDACCESS -W <filename> <username>` - Grant write access

**Description:** Grants read or write permission to another user (owner only).

**Examples:**

```
docs++> ADDACCESS -R document.txt bob
✅ Access granted

docs++> ADDACCESS -W document.txt charlie
✅ Access granted
```

**Features:**

- Owner-only operation
- `-R` grants read access
- `-W` grants both read and write access
- Returns ERR_UNAUTHORIZED if not owner

---

### 11. REMACCESS - Revoke File Access

**Syntax:** `REMACCESS <filename> <username>`

**Description:** Removes a user's access to a file (owner only).

**Example:**

```
docs++> REMACCESS document.txt bob
✅ Access removed
```

**Features:**

- Owner-only operation
- Removes both read and write permissions
- Returns ERR_UNAUTHORIZED if not owner

---

### 12. EXEC - Execute File as Commands

**Syntax:** `EXEC <filename>`

**Description:** Executes the file content as shell commands on the Name Server.

**Example:**

```
# File content: "echo Hello from EXEC"
docs++> EXEC script.txt
Hello from EXEC
```

**Features:**

- Runs on Name Server via `/bin/sh`
- Returns command output
- Useful for automation scripts

---

### 13. REQUESTACCESS - Request File Access

**Syntax:** `REQUESTACCESS <filename> <owner>`

**Description:** Send an access request to a file owner.

**Example:**

```
docs++> REQUESTACCESS important.txt alice
✅ Access request sent
```

**Features:**

- Request access to files you don't own
- Owner receives request in VIEWREQUESTS
- Prevents duplicate requests

---

### 14. VIEWREQUESTS - View Access Requests

**Syntax:** `VIEWREQUESTS`

**Description:** View all pending access requests for your files.

**Example:**

```
docs++> VIEWREQUESTS

📬 Pending Access Requests:
━━━━━━━━━━━━━━━━━━━━━━━━━━━
  important.txt:bob
  document.txt:charlie
━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

---

### 15. APPROVE - Approve Access Request

**Syntax:** `APPROVE <filename> <requester>`

**Description:** Approve a pending access request (owner only).

**Example:**

```
docs++> APPROVE important.txt bob
✅ Request approved
```

**Features:**

- Automatically grants READ access
- Removes request from pending list
- Owner-only operation

---

### 16. DENY - Deny Access Request

**Syntax:** `DENY <filename> <requester>`

**Description:** Deny a pending access request (owner only).

**Example:**

```
docs++> DENY document.txt charlie
✅ Request denied
```

---

### 17. CREATEFOLDER - Create Folder

**Syntax:** `CREATEFOLDER <foldername>`

**Description:** Create a new folder for organizing files.

**Example:**

```
docs++> CREATEFOLDER projects
✅ Folder created
```

---

### 18. MOVE - Move File to Folder

**Syntax:** `MOVE <filename> <foldername>`

**Description:** Move a file into a folder.

**Example:**

```
docs++> MOVE document.txt projects
✅ File moved
```

**Features:**

- Updates file path (e.g., `document.txt` → `projects/document.txt`)
- Moves metadata and undo files
- Folder must exist first

---

### 19. VIEWFOLDER - View Folder Contents

**Syntax:** `VIEWFOLDER <foldername>`

**Description:** List all files in a specific folder.

**Example:**

```
docs++> VIEWFOLDER projects

📁 Files in projects:
document.txt
report.txt
```

---

### 20. CHECKPOINT - Create File Checkpoint

**Syntax:** `CHECKPOINT <filename> <tag>`

**Description:** Create a named checkpoint of the current file state.

**Example:**

```
docs++> CHECKPOINT document.txt v1
✅ Checkpoint created
```

**Features:**

- Named snapshots of file state
- Multiple checkpoints per file
- Can revert to any checkpoint later

---

### 21. VIEWCHECKPOINT - View Checkpoint Content

**Syntax:** `VIEWCHECKPOINT <filename> <tag>`

**Description:** View the content of a specific checkpoint without reverting.

**Example:**

```
docs++> VIEWCHECKPOINT document.txt v1

📄 Checkpoint 'v1' for document.txt:
This is the checkpoint content.
━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

---

### 22. REVERT - Revert to Checkpoint

**Syntax:** `REVERT <filename> <tag>`

**Description:** Restore file to a previous checkpoint state.

**Example:**

```
docs++> REVERT document.txt v1
✅ Reverted to checkpoint 'v1'
```

**Features:**

- Replaces current file with checkpoint
- Invalidates file cache
- Cannot be undone (creates new state)

---

### 23. LISTCHECKPOINTS - List All Checkpoints

**Syntax:** `LISTCHECKPOINTS <filename>`

**Description:** Show all available checkpoints for a file.

**Example:**

```
docs++> LISTCHECKPOINTS document.txt

📋 Checkpoints for document.txt:
v1
v2
final
```

---

### 24. EXIT - Quit Client

**Syntax:** `EXIT` or `QUIT`

**Description:** Disconnects from the server and exits the client.

**Example:**

```
docs++> EXIT
```

---

## 🚀 Advanced Features

### Hierarchical Folder Structure (10 marks)

- **Folders**: Organize files into directories
- **Commands**: `CREATEFOLDER`, `MOVE`, `VIEWFOLDER`
- **Implementation**:
  - Folders stored as directories in `storageserver/data/files/`
  - Files maintain full paths (e.g., `projects/doc.txt`)
  - Recursive directory scanning on startup
  - Automatic metadata/undo subdirectory creation

### Checkpoints (15 marks)

- **Versioning**: Create named snapshots of file states
- **Commands**: `CHECKPOINT`, `VIEWCHECKPOINT`, `REVERT`, `LISTCHECKPOINTS`
- **Implementation**:
  - Checkpoints stored in `storageserver/data/checkpoints/`
  - Full file copies with tag-based naming
  - Cache invalidation on revert
  - Multiple checkpoints per file supported

### Access Request System (5 marks)

- **Request/Approve Workflow**: Users can request access, owners can approve/deny
- **Commands**: `REQUESTACCESS`, `VIEWREQUESTS`, `APPROVE`, `DENY`
- **Implementation**:
  - Name Server maintains pending request queue
  - Automatic permission granting on approval
  - Duplicate request prevention
  - Owner-only approval/denial

### Fault Tolerance with Replication (15 marks)

- **Replication Strategy**: Automatic pairing of storage servers

  - SS1 ↔ SS2, SS3 ↔ SS4, etc.
  - Each file mapped to primary + replica
  - Asynchronous write replication (non-blocking)

- **Failure Detection**:

  - Heartbeat mechanism (15-second timeout)
  - Background monitoring thread (5-second checks)
  - Comprehensive failure logging
  - Thread-safe with mutex protection

- **Automatic Failover**:

  - Transparent routing to replica on primary failure
  - No client-side changes required
  - Graceful degradation if both servers fail

- **SS Recovery**:
  - Reconnection with same `ss_id`
  - Automatic state restoration
  - Seamless resumption of operations

### Comprehensive Logging (5 marks)

- **Dual Output**: Console + persistent log files
- **Timestamped**: Millisecond-precision timestamps
- **Structured**: Component, operation, IP, port, user, details
- **Files**: `nameserver/nm.log`, `storageserver/ss.log`
- **Events Logged**: All operations, failures, recoveries, failovers

### Efficient O(1) Search (5 marks)

- **Hash Table**: djb2 algorithm with 1024 buckets
- **Collision Handling**: Linked list chaining
- **Average O(1) Lookup**: Fast file routing
- **LRU Tracking**: Most recently used files tracked

### Data Persistence (10 marks)

- **File Content**: Persists in `storageserver/data/files/`
- **Metadata**: JSON format in `storageserver/data/meta/`
- **ACL Storage**: Access control lists in metadata
- **Automatic Load**: Storage server loads on startup
- **Transactional**: Atomic writes with undo backups

---

## 🔧 Implementation Details

### File Storage Structure

```
storageserver/data/
├── files/              # Actual file content
│   ├── *.txt
│   └── <folder>/       # Hierarchical folders
│       └── *.txt
├── meta/               # Metadata JSON files
│   ├── *.txt.json
│   └── <folder>/       # Folder metadata
│       └── *.txt.json
├── undo/               # Undo backups
│   ├── *.txt.bak
│   └── <folder>/       # Folder undo files
│       └── *.txt.bak
└── checkpoints/        # File checkpoints
    └── <file>_<tag>    # Tagged snapshots
```

### Metadata Format

Each file has a corresponding `.json` file storing:

- Owner username
- Access control list (ACL)
- Created timestamp
- Modified timestamp
- Word count
- Character count

### Tokenization

Files are tokenized into sentences and words:

- **Sentence delimiters:** `.` `?` `!`
- **Every delimiter creates a new sentence** (including mid-word like "e.g.")
- Sentences are arrays of words
- Delimiters are preserved for accurate reconstruction

### Access Control

- **Owner:** Full read/write access, can grant/revoke permissions
- **R permission:** Can read file content
- **W permission:** Can read and write (includes R)
- **No permission:** Cannot access file (ERR_UNAUTHORIZED)

### Concurrency

- **Sentence-level locking:** Only one user can edit a sentence at a time
- **Lock holder:** User who initiated WRITE session
- **Lock release:** Automatic on ETIRW or disconnection
- **Concurrent access:** Different sentences can be edited simultaneously

### Error Codes

- `OK (0)` - Success
- `ERR_NOT_FOUND (1)` - File not found
- `ERR_UNAUTHORIZED (2)` - Access denied
- `ERR_LOCKED (3)` - Resource locked by another user
- `ERR_BAD_REQUEST (4)` - Invalid request
- `ERR_CONFLICT (5)` - Resource conflict (e.g., file exists)
- `ERR_INTERNAL (6)` - Internal server error
- `ERR_BUSY (7)` - Server busy
- `ERR_OOSCOPE (8)` - Out of scope
- `ERR_ALREADY_EXISTS (9)` - Resource already exists

---

## 📁 File Structure

```
course-project-jogipet-boys/
├── Makefile                      # Build configuration
├── README.md                     # This file
├── SYSTEM_REQUIREMENTS_IMPLEMENTATION.md  # 40 marks documentation
├── BONUS_FEATURES.md             # 25 marks bonus documentation
├── ADDITIONAL_BONUS_FEATURES.md  # 20 marks additional bonus
│
├── common/                       # Shared utilities
│   ├── net.h/c                  # TCP socket helpers
│   ├── jsonl.h/c                # JSON line parsing
│   ├── log.h/c                  # Logging system
│   └── proto.h                  # Error codes and constants
│
├── nameserver/                   # Name Server
│   ├── nm.c                     # Main NM logic with replication
│   ├── nm_state.h/c             # State management
│   ├── nm_search.h/c            # O(1) hash table search
│   ├── nm_access_req.h/c        # Access request system
│   ├── nm_replication.h/c       # Fault tolerance & replication
│   └── nm.log                   # NM operation logs
│
├── storageserver/               # Storage Server
│   ├── ss.c                     # Main SS logic with threading
│   ├── ss_files.h/c             # File ops, folders, checkpoints
│   ├── ss_acl.h/c               # Access control
│   ├── ss.log                   # SS operation logs
│   └── data/
│       ├── files/               # File storage (hierarchical)
│       ├── meta/                # Metadata storage with ACL
│       ├── undo/                # Undo backups
│       └── checkpoints/         # File checkpoints
│
└── client/                      # Client
    ├── cli.c                    # Main client logic
    └── cli_repl.c               # REPL interface
```

---

## 🎯 Quick Start Example

```bash
# Terminal 1: Start Name Server
./nm

# Terminal 2: Start Storage Server
./ss

# Terminal 3: Start Client
./cli
Username: alice

# Create and edit a file
docs++> CREATE mydoc.txt
docs++> WRITE mydoc.txt 0
0 Hello
1 world
2 this
3 is
4 my
5 document
ETIRW

# View the content
docs++> READ mydoc.txt

# Create a checkpoint
docs++> CHECKPOINT mydoc.txt v1

# Create folder and move file
docs++> CREATEFOLDER projects
docs++> MOVE mydoc.txt projects

# View folder contents
docs++> VIEWFOLDER projects

# Grant access to another user
docs++> ADDACCESS -R projects/mydoc.txt bob

# View file list with details
docs++> VIEW -l

# Clean up
docs++> DELETE projects/mydoc.txt
docs++> EXIT
```

---

## 🔍 Troubleshooting

### "Connection refused" error

- Ensure Name Server is running on port 5050
- Ensure Storage Server is running on port 6001
- Check for port conflicts: `lsof -i :5050 -i :6001`

### "Failed to start" error

- Run `make clean && make all` to rebuild
- Check for compilation errors
- Verify system has GCC and Make installed

### Files not appearing after restart

- Files persist in `storageserver/data/files/`
- Metadata persists in `storageserver/data/meta/`
- Storage Server automatically loads on startup

### Permission denied errors

- Check file ownership with `INFO <file>`
- Use `ADDACCESS` to grant permissions
- Only owner can grant/revoke access

### Testing Fault Tolerance

**Setup Multiple Storage Servers:**

```bash
# Terminal 1: Name Server
./nm

# Terminal 2: Storage Server 1 (Primary)
./ss

# Terminal 3: Storage Server 2 (Replica)
# Modify ss.c to use different ports (6003/6002) before building
./ss
```

**Simulate Failure:**

1. Create files on primary SS
2. Kill primary SS process (Ctrl+C)
3. Wait 15 seconds for failure detection
4. Check NM logs for "FAILURE DETECTED"
5. Try reading file - should failover to replica
6. Check logs for "FAILOVER" message

**Test Recovery:**

1. Restart the killed SS
2. Check logs for "Storage Server recovered"
3. Operations should resume normally

---

## 📊 Features Summary

### Core Features (40 marks)

| Feature                                  | Status |
| ---------------------------------------- | ------ |
| File CRUD (Create, Read, Update, Delete) | ✅     |
| Access Control (Owner, R, W permissions) | ✅     |
| Sentence-level locking                   | ✅     |
| Word-level editing                       | ✅     |
| Undo functionality                       | ✅     |
| File metadata tracking                   | ✅     |
| Persistence across restarts              | ✅     |
| Multi-user support                       | ✅     |
| Real-time streaming                      | ✅     |
| Command execution                        | ✅     |
| Data Persistence with ACL (10 marks)     | ✅     |
| Comprehensive Logging (5 marks)          | ✅     |
| Efficient O(1) Search (5 marks)          | ✅     |
| Multi-threaded SS (from fork) (5 marks)  | ✅     |

### Bonus Features (25 marks)

| Feature                                  | Status |
| ---------------------------------------- | ------ |
| Hierarchical Folder Structure (10 marks) | ✅     |
| Checkpoints System (15 marks)            | ✅     |

### Additional Bonus (20 marks)

| Feature                                     | Status |
| ------------------------------------------- | ------ |
| Access Request System (5 marks)             | ✅     |
| Fault Tolerance with Replication (15 marks) | ✅     |
| - Data Replication (Async writes)           | ✅     |
| - Failure Detection (Heartbeat)             | ✅     |
| - Automatic Failover                        | ✅     |
| - SS Recovery                               | ✅     |

### Total: 85 marks implemented!

---

## 👥 Authors

- **Lohith** - Common helpers, CLI, SS file engine
- **Saharsh** - NM routing, search, logging, error codes

---

## 📄 License

This is a course project for Operating Systems and Networks (OSN) at IIIT Hyderabad.

---

**Ready to use!** Start with the [Quick Start Example](#quick-start-example) above. 🚀
