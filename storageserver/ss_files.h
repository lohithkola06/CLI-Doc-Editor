#ifndef SS_FILES_H
#define SS_FILES_H
#include <time.h>

// Data structures for sentence/word tokenization
typedef struct
{
  char **words;
  int word_count;
  int capacity;
  char delimiter; // '.', '?', '!', or '\0' for no delimiter
} Sentence;

// Access control entry
typedef struct
{
  char username[64];
  int can_read;
  int can_write;
} AccessEntry;

// File metadata
typedef struct
{
  char filename[256];
  char owner[64];
  time_t created_time;
  time_t modified_time;
  time_t accessed_time;
  char last_access_user[64];
  int word_count;
  int char_count;
  AccessEntry *access_list;
  int access_count;
  int access_capacity;
} FileMetadata;

typedef struct
{
  int sentence_idx;
  char user[64];
} SentenceLock;

typedef struct
{
  char filename[256];
  Sentence *sentences;
  int sentence_count;
  int sentence_capacity;
  char owner[64];
  SentenceLock *locks;
  int lock_count;
  int lock_capacity;
  FileMetadata metadata;
} FileState;

// Public API
int ss_files_init(void);
int ss_files_read(const char *file, const char *user, char *content, int maxlen);
int ss_files_write_begin(const char *file, const char *user, int sentence_idx);
int ss_files_write_edit(const char *file, const char *user, int word_index, const char *content);
int ss_files_write_commit(const char *file, const char *user);
int ss_files_undo(const char *file, const char *user);
int ss_files_create(const char *file, const char *owner);
int ss_files_delete(const char *file, const char *actor);
int ss_files_get_info(const char *file, char *info, int maxlen);
int ss_files_list_all(char *list, int maxlen, int include_all, int include_details, const char *user);
int ss_files_add_access(const char *file, const char *actor, const char *target_user, const char *mode);
int ss_files_remove_access(const char *file, const char *actor, const char *target_user);

// Folder operations
int ss_files_create_folder(const char *foldername);
int ss_files_move_file(const char *filename, const char *foldername);
int ss_files_view_folder(const char *foldername, char *files, int maxlen);

// Checkpoint operations
int ss_files_create_checkpoint(const char *file, const char *tag);
int ss_files_view_checkpoint(const char *file, const char *tag, char *content, int maxlen);
int ss_files_revert_checkpoint(const char *file, const char *tag);
int ss_files_list_checkpoints(const char *file, char *checkpoints, int maxlen);

// Helper functions
int tokenize_file(const char *filepath, FileState *state);
int rebuild_file(const FileState *state, char *output, int maxlen);
void free_file_state(FileState *state);
int check_access(const char *file, const char *user, int need_write);

#endif
