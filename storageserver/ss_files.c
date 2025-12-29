#define _POSIX_C_SOURCE 200809L
#include "ss_files.h"
#include "../common/proto.h"
#include "../common/jsonl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>

#define MAX_FILES 128
#define DATA_DIR "storageserver/data/files/"
#define UNDO_DIR "storageserver/data/undo/"
#define META_DIR "storageserver/data/meta/"
#define CHECKPOINT_DIR "storageserver/data/checkpoints/"

// Global file state cache
static FileState *g_file_cache[MAX_FILES];
static int g_file_count = 0;
static pthread_mutex_t g_file_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static int ensure_lock_capacity(FileState *state)
{
  if (state->lock_count >= state->lock_capacity)
  {
    int new_cap = state->lock_capacity == 0 ? 4 : state->lock_capacity * 2;
    SentenceLock *locks = realloc(state->locks, new_cap * sizeof(SentenceLock));
    if (!locks)
      return -1;
    state->locks = locks;
    state->lock_capacity = new_cap;
  }
  return 0;
}

static SentenceLock *find_lock(FileState *state, const char *user)
{
  for (int i = 0; i < state->lock_count; i++)
  {
    if (!strcmp(state->locks[i].user, user))
      return &state->locks[i];
  }
  return NULL;
}

static int sentence_locked_by_other(FileState *state, int sentence_idx, const char *user)
{
  for (int i = 0; i < state->lock_count; i++)
  {
    if (state->locks[i].sentence_idx == sentence_idx &&
        strcmp(state->locks[i].user, user) != 0)
      return 1;
  }
  return 0;
}

static int add_lock(FileState *state, const char *user, int sentence_idx)
{
  if (ensure_lock_capacity(state) != 0)
    return -1;
  SentenceLock *lock = &state->locks[state->lock_count++];
  lock->sentence_idx = sentence_idx;
  strncpy(lock->user, user, sizeof(lock->user) - 1);
  lock->user[sizeof(lock->user) - 1] = '\0';
  return 0;
}

static int remove_lock(FileState *state, const char *user)
{
  for (int i = 0; i < state->lock_count; i++)
  {
    if (!strcmp(state->locks[i].user, user))
    {
      for (int j = i; j < state->lock_count - 1; j++)
      {
        state->locks[j] = state->locks[j + 1];
      }
      state->lock_count--;
      return 0;
    }
  }
  return -1;
}

static int any_active_locks(FileState *state)
{
  return state->lock_count > 0;
}

// Helper: ensure directory path exists (mkdir -p)
static void ensure_dirs(const char *path)
{
  if (!path || !path[0])
    return;

  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s", path);

  size_t len = strlen(tmp);
  if (!len)
    return;

  for (size_t i = 1; i < len; i++)
  {
    if (tmp[i] == '/')
    {
      char saved = tmp[i];
      tmp[i] = '\0';
      mkdir(tmp, 0755);
      tmp[i] = saved;
    }
  }

  mkdir(tmp, 0755);
}

// Helper: Check if character is sentence terminator
static int is_sentence_end(char c)
{
  return (c == '.' || c == '?' || c == '!');
}

// Helper: Add word to sentence
static void add_word_to_sentence(Sentence *sent, const char *word)
{
  if (sent->word_count >= sent->capacity)
  {
    sent->capacity = sent->capacity == 0 ? 8 : sent->capacity * 2;
    sent->words = realloc(sent->words, sent->capacity * sizeof(char *));
  }
  sent->words[sent->word_count++] = strdup(word);
}

static void insert_word_at(Sentence *sent, int idx, const char *word)
{
  if (idx < 0)
    idx = 0;
  if (idx > sent->word_count)
    idx = sent->word_count;

  if (sent->word_count >= sent->capacity)
  {
    sent->capacity = sent->capacity == 0 ? 8 : sent->capacity * 2;
    sent->words = realloc(sent->words, sent->capacity * sizeof(char *));
  }

  for (int i = sent->word_count; i > idx; i--)
  {
    sent->words[i] = sent->words[i - 1];
  }

  sent->words[idx] = strdup(word);
  sent->word_count++;
}

// Helper: Add sentence to file state
static void add_sentence_to_file(FileState *fs, Sentence sent)
{
  if (fs->sentence_count >= fs->sentence_capacity)
  {
    fs->sentence_capacity = fs->sentence_capacity == 0 ? 8 : fs->sentence_capacity * 2;
    fs->sentences = realloc(fs->sentences, fs->sentence_capacity * sizeof(Sentence));
  }
  fs->sentences[fs->sentence_count++] = sent;
}

// Tokenize file into sentences and words
int tokenize_file(const char *filepath, FileState *state)
{
  FILE *fp = fopen(filepath, "r");
  if (!fp)
    return ERR_NOT_FOUND;

  fseek(fp, 0, SEEK_END);
  long fsize = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  char *content = malloc(fsize + 1);
  if (!content)
  {
    fclose(fp);
    return ERR_INTERNAL;
  }

  size_t bytes_read = fread(content, 1, fsize, fp);
  content[bytes_read] = 0;
  fclose(fp);

  state->sentence_count = 0;
  state->sentence_capacity = 0;
  state->sentences = NULL;

  Sentence current_sent = {0};
  char word[256] = {0};
  int word_len = 0;

  for (long i = 0; i <= (long)bytes_read; i++)
  {
    char c = content[i];

    if (isalnum(c) || c == '_' || c == '-' || c == '\'')
    {
      if (word_len < 255)
        word[word_len++] = c;
    }
    else
    {
      if (word_len > 0)
      {
        word[word_len] = 0;
        add_word_to_sentence(&current_sent, word);
        word_len = 0;
      }

      if (is_sentence_end(c))
      {
        if (current_sent.word_count > 0)
        {
          current_sent.delimiter = c; // Store the delimiter
          add_sentence_to_file(state, current_sent);
          current_sent.words = NULL;
          current_sent.word_count = 0;
          current_sent.capacity = 0;
          current_sent.delimiter = 0;
        }
      }
    }
  }

  // Add last sentence if it has words (no delimiter means it's incomplete)
  if (current_sent.word_count > 0)
  {
    current_sent.delimiter = 0; // No delimiter for incomplete last sentence
    add_sentence_to_file(state, current_sent);
  }

  free(content);
  return OK;
}

// Rebuild file content from tokens
int rebuild_file(const FileState *state, char *output, int maxlen)
{
  output[0] = 0;
  int pos = 0;

  for (int i = 0; i < state->sentence_count; i++)
  {
    Sentence *sent = &state->sentences[i];

    for (int j = 0; j < sent->word_count; j++)
    {
      if ((i > 0 || j > 0) && pos < maxlen - 1)
        output[pos++] = ' ';

      const char *word = sent->words[j];
      while (*word && pos < maxlen - 1)
        output[pos++] = *word++;
    }

    // Only add delimiter if one was explicitly set by user
    if (sent->delimiter && pos < maxlen - 1)
    {
      output[pos++] = sent->delimiter;
    }
  }

  output[pos] = 0;
  return OK;
}

// Free memory for file state
void free_file_state(FileState *state)
{
  if (!state)
    return;

  for (int i = 0; i < state->sentence_count; i++)
  {
    for (int j = 0; j < state->sentences[i].word_count; j++)
    {
      free(state->sentences[i].words[j]);
    }
    free(state->sentences[i].words);
  }
  free(state->sentences);

  if (state->metadata.access_list)
  {
    free(state->metadata.access_list);
  }
  if (state->locks)
  {
    free(state->locks);
  }

  state->locks = NULL;
  state->lock_count = 0;
  state->lock_capacity = 0;

  state->sentences = NULL;
  state->sentence_count = 0;
  state->sentence_capacity = 0;
}

// Find file in cache
static FileState *find_file_in_cache(const char *filename)
{
  for (int i = 0; i < g_file_count; i++)
  {
    if (g_file_cache[i] && strcmp(g_file_cache[i]->filename, filename) == 0)
    {
      return g_file_cache[i];
    }
  }
  return NULL;
}

// Update metadata counts
static void update_metadata_counts(FileState *state)
{
  int word_count = 0, char_count = 0;

  for (int i = 0; i < state->sentence_count; i++)
  {
    word_count += state->sentences[i].word_count;
    for (int j = 0; j < state->sentences[i].word_count; j++)
    {
      char_count += strlen(state->sentences[i].words[j]);
    }
    char_count++; // Space or period
  }

  state->metadata.word_count = word_count;
  state->metadata.char_count = char_count;
}

// Save metadata to disk
static void save_metadata(const FileState *state)
{
  char metapath[512];
  snprintf(metapath, sizeof metapath, "%s%s.json", META_DIR, state->filename);

  FILE *fp = fopen(metapath, "w");
  if (!fp)
    return;

  fprintf(fp, "{\"owner\":\"%s\",\"created\":%ld,\"modified\":%ld,\"accessed\":%ld,\"last_access_user\":\"%s\",\"access_list\":[",
          state->metadata.owner,
          (long)state->metadata.created_time,
          (long)state->metadata.modified_time,
          (long)state->metadata.accessed_time,
          state->metadata.last_access_user[0] ? state->metadata.last_access_user : state->metadata.owner);

  // Save access control list
  for (int i = 0; i < state->metadata.access_count; i++)
  {
    if (i > 0)
      fprintf(fp, ",");
    fprintf(fp, "{\"user\":\"%s\",\"read\":%d,\"write\":%d}",
            state->metadata.access_list[i].username,
            state->metadata.access_list[i].can_read,
            state->metadata.access_list[i].can_write);
  }

  fprintf(fp, "]}\n");
  fclose(fp);
}

// Load metadata from disk
static int load_metadata(FileState *state)
{
  char metapath[512];
  snprintf(metapath, sizeof metapath, "%s%s.json", META_DIR, state->filename);

  FILE *fp = fopen(metapath, "r");
  if (!fp)
    return -1;

  char line[2048];
  if (fgets(line, sizeof line, fp))
  {
    // Parse owner
    char *owner_start = strstr(line, "\"owner\":\"");
    if (owner_start)
    {
      owner_start += 9;
      char *owner_end = strchr(owner_start, '"');
      if (owner_end)
      {
        int len = owner_end - owner_start;
        if (len < 64)
        {
          strncpy(state->metadata.owner, owner_start, len);
          state->metadata.owner[len] = 0;
          strncpy(state->owner, owner_start, len);
          state->owner[len] = 0;
        }
      }
    }

    int created = 0, modified = 0, accessed = 0;
    if (json_get_int(line, "created", &created) == 0)
      state->metadata.created_time = created;
    if (json_get_int(line, "modified", &modified) == 0)
      state->metadata.modified_time = modified;
    if (json_get_int(line, "accessed", &accessed) == 0)
      state->metadata.accessed_time = accessed;
    else
      state->metadata.accessed_time = state->metadata.modified_time;

    char last_access_user[64];
    if (json_get_str(line, "last_access_user", last_access_user, sizeof last_access_user) == 0)
    {
      strncpy(state->metadata.last_access_user, last_access_user, sizeof state->metadata.last_access_user - 1);
      state->metadata.last_access_user[sizeof state->metadata.last_access_user - 1] = '\0';
    }
    else if (state->metadata.owner[0])
    {
      strncpy(state->metadata.last_access_user, state->metadata.owner, sizeof state->metadata.last_access_user - 1);
      state->metadata.last_access_user[sizeof state->metadata.last_access_user - 1] = '\0';
    }

    // Parse access list
    char *acl_start = strstr(line, "\"access_list\":[");
    if (acl_start)
    {
      acl_start += 15; // Skip "access_list":[
      char *acl_end = strchr(acl_start, ']');
      if (acl_end)
      {
        char *entry = acl_start;
        while (entry < acl_end && *entry != ']')
        {
          // Find user field
          char *user_start = strstr(entry, "\"user\":\"");
          if (!user_start || user_start >= acl_end)
            break;
          user_start += 8;
          char *user_end = strchr(user_start, '"');
          if (!user_end || user_end >= acl_end)
            break;

          // Extract username
          char username[64];
          int ulen = user_end - user_start;
          if (ulen >= 64)
            ulen = 63;
          strncpy(username, user_start, ulen);
          username[ulen] = 0;

          // Find read and write flags
          int can_read = 0, can_write = 0;
          char *read_start = strstr(user_end, "\"read\":");
          if (read_start && read_start < acl_end)
          {
            can_read = (*(read_start + 7) == '1');
          }
          char *write_start = strstr(user_end, "\"write\":");
          if (write_start && write_start < acl_end)
          {
            can_write = (*(write_start + 8) == '1');
          }

          // Add to access list
          if (state->metadata.access_count >= state->metadata.access_capacity)
          {
            state->metadata.access_capacity = state->metadata.access_capacity == 0 ? 4 : state->metadata.access_capacity * 2;
            state->metadata.access_list = realloc(state->metadata.access_list,
                                                  state->metadata.access_capacity * sizeof(AccessEntry));
          }

          AccessEntry *ae = &state->metadata.access_list[state->metadata.access_count++];
          strncpy(ae->username, username, sizeof(ae->username) - 1);
          ae->username[sizeof(ae->username) - 1] = 0;
          ae->can_read = can_read;
          ae->can_write = can_write;

          // Move to next entry
          entry = strchr(write_start ? write_start : read_start, '}');
          if (!entry)
            break;
          entry++;
        }
      }
    }
  }

  fclose(fp);
  return 0;
}

// Check if user has access to file
int check_access(const char *file, const char *user, int need_write)
{
  FileState *state = find_file_in_cache(file);
  if (!state)
    return ERR_NOT_FOUND;

  // Owner always has access
  if (strcmp(state->metadata.owner, user) == 0)
    return OK;

  // Check access list
  for (int i = 0; i < state->metadata.access_count; i++)
  {
    if (strcmp(state->metadata.access_list[i].username, user) == 0)
    {
      if (need_write && !state->metadata.access_list[i].can_write)
      {
        return ERR_UNAUTHORIZED;
      }
      if (!need_write && !state->metadata.access_list[i].can_read)
      {
        return ERR_UNAUTHORIZED;
      }
      return OK;
    }
  }

  return ERR_UNAUTHORIZED;
}

// Load or get file from cache
static FileState *load_file(const char *filename)
{
  FileState *cached = find_file_in_cache(filename);
  if (cached)
    return cached;

  char filepath[512];
  snprintf(filepath, sizeof filepath, "%s%s", DATA_DIR, filename);

  FileState *state = calloc(1, sizeof(FileState));
  if (!state)
    return NULL;

  strncpy(state->filename, filename, sizeof state->filename - 1);

  if (tokenize_file(filepath, state) != OK)
  {
    free(state);
    return NULL;
  }

  // Try to load metadata first
  if (load_metadata(state) != 0)
  {
    // No metadata found, initialize from file stats
    struct stat st;
    if (stat(filepath, &st) == 0)
    {
      state->metadata.created_time = st.st_ctime;
      state->metadata.modified_time = st.st_mtime;
      state->metadata.accessed_time = st.st_atime;
    }
    // Owner unknown for legacy files
    state->metadata.owner[0] = 0;
    state->owner[0] = 0;
  }

  update_metadata_counts(state);

  if (g_file_count < MAX_FILES)
  {
    g_file_cache[g_file_count++] = state;
  }

  return state;
}

// Helper function to recursively scan directories
static void scan_directory_recursive(const char *base_dir, const char *relative_path)
{
  char scan_path[512];
  if (relative_path && relative_path[0])
  {
    snprintf(scan_path, sizeof(scan_path), "%s%s", base_dir, relative_path);
  }
  else
  {
    snprintf(scan_path, sizeof(scan_path), "%s", base_dir);
  }

  DIR *dir = opendir(scan_path);
  if (!dir)
    return;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL)
  {
    // Skip hidden files and .gitkeep
    if (entry->d_name[0] == '.' || strcmp(entry->d_name, ".gitkeep") == 0)
    {
      continue;
    }

    char filepath[512];
    char relative_file[512];

    if (relative_path && relative_path[0])
    {
      snprintf(filepath, sizeof(filepath), "%s%s/%s", base_dir, relative_path, entry->d_name);
      snprintf(relative_file, sizeof(relative_file), "%s/%s", relative_path, entry->d_name);
    }
    else
    {
      snprintf(filepath, sizeof(filepath), "%s%s", base_dir, entry->d_name);
      snprintf(relative_file, sizeof(relative_file), "%s", entry->d_name);
    }

    struct stat st;
    if (stat(filepath, &st) == 0)
    {
      if (S_ISDIR(st.st_mode))
      {
        // Recursively scan subdirectory
        scan_directory_recursive(base_dir, relative_file);
      }
      else if (S_ISREG(st.st_mode))
      {
        // Load file into cache
        if (!find_file_in_cache(relative_file))
        {
          load_file(relative_file);
        }
      }
    }
  }

  closedir(dir);
}

// Initialize file subsystem
int ss_files_init(void)
{
  g_file_count = 0;

  mkdir("storageserver", 0755);
  mkdir("storageserver/data", 0755);
  mkdir("storageserver/data/files", 0755);
  mkdir("storageserver/data/meta", 0755);
  mkdir("storageserver/data/undo", 0755);
  mkdir("storageserver/data/checkpoints", 0755);

  // Load existing files from disk recursively
  scan_directory_recursive(DATA_DIR, "");

  return OK;
}

// Read file content
int ss_files_read(const char *file, const char *user, char *content, int maxlen)
{
  FileState *state = load_file(file);
  if (!state)
    return ERR_NOT_FOUND;

  // Check read access
  int access_check = check_access(file, user, 0);
  if (access_check != OK && access_check != ERR_UNAUTHORIZED)
  {
    return access_check;
  }
  // For now, allow reads even without explicit access (can be changed)

  int rc = rebuild_file(state, content, maxlen);
  if (rc == OK)
  {
    state->metadata.accessed_time = time(NULL);
    if (user && user[0])
    {
      strncpy(state->metadata.last_access_user, user, sizeof state->metadata.last_access_user - 1);
      state->metadata.last_access_user[sizeof state->metadata.last_access_user - 1] = '\0';
    }
    save_metadata(state);
  }
  return rc;
}

// Begin write session
int ss_files_write_begin(const char *file, const char *user, int sentence_idx)
{
  pthread_mutex_lock(&g_file_cache_mutex);

  FileState *state = load_file(file);
  if (!state)
  {
    pthread_mutex_unlock(&g_file_cache_mutex);
    return ERR_NOT_FOUND;
  }

  // Check write access (legacy behavior keeps ERR_UNAUTHORIZED fallback)
  int access_check = check_access(file, user, 1);
  if (access_check != OK && access_check != ERR_UNAUTHORIZED)
  {
    pthread_mutex_unlock(&g_file_cache_mutex);
    return access_check;
  }

  // Special case: empty file and writing to sentence 0
  if (state->sentence_count == 0 && sentence_idx == 0)
  {
    // Create first empty sentence (no delimiter until user adds one)
    Sentence empty_sent = {0};
    empty_sent.delimiter = 0; // No delimiter initially
    add_sentence_to_file(state, empty_sent);
  }

  // Allow writing to one past the last sentence ONLY if previous sentence has delimiter
  if (sentence_idx == state->sentence_count)
  {
    if (sentence_idx == 0)
    {
      // Allow creating first sentence even without delimiter from previous
      Sentence empty_sent = {0};
      empty_sent.delimiter = 0;
      add_sentence_to_file(state, empty_sent);
    }
    else if (sentence_idx > 0 && state->sentences[sentence_idx - 1].delimiter != 0)
    {
      // Previous sentence has delimiter, allow creating new sentence
      Sentence empty_sent = {0};
      empty_sent.delimiter = 0;
      add_sentence_to_file(state, empty_sent);
    }
    else
    {
      // Previous sentence has no delimiter, can't create new sentence
      pthread_mutex_unlock(&g_file_cache_mutex);
      return ERR_BAD_REQUEST;
    }
  }

  if (sentence_idx < 0 || sentence_idx >= state->sentence_count)
  {
    pthread_mutex_unlock(&g_file_cache_mutex);
    return ERR_BAD_REQUEST;
  }

  SentenceLock *existing = find_lock(state, user);
  if (existing)
  {
    if (existing->sentence_idx == sentence_idx)
    {
      pthread_mutex_unlock(&g_file_cache_mutex);
      return OK;
    }
    pthread_mutex_unlock(&g_file_cache_mutex);
    return ERR_LOCKED;
  }

  if (sentence_locked_by_other(state, sentence_idx, user))
  {
    pthread_mutex_unlock(&g_file_cache_mutex);
    return ERR_LOCKED;
  }

  if (add_lock(state, user, sentence_idx) != 0)
  {
    pthread_mutex_unlock(&g_file_cache_mutex);
    return ERR_INTERNAL;
  }

  pthread_mutex_unlock(&g_file_cache_mutex);
  return OK;
}

// Edit word in sentence
int ss_files_write_edit(const char *file, const char *user, int word_index, const char *content)
{
  FileState *state = find_file_in_cache(file);
  if (!state)
    return ERR_NOT_FOUND;

  SentenceLock *lock = find_lock(state, user);
  if (!lock)
    return ERR_BAD_REQUEST;

  Sentence *sent = &state->sentences[lock->sentence_idx];

  if (word_index < 0 || word_index > sent->word_count)
  {
    return ERR_BAD_REQUEST;
  }

  // Parse content for words and delimiters
  char *content_copy = strdup(content);
  char *p = content_copy;
  int current_idx = word_index;

  // Build words from content, checking for delimiters
  char word_buf[256];
  int word_len = 0;

  while (*p)
  {
    if (*p == ' ' || *p == '\t' || *p == '\n')
    {
      if (word_len > 0)
      {
        word_buf[word_len] = 0;
        insert_word_at(sent, current_idx++, word_buf);
        word_len = 0;
      }
      p++;
    }
    else if (*p == '.' || *p == '!' || *p == '?')
    {
      if (word_len > 0)
      {
        word_buf[word_len] = 0;
        insert_word_at(sent, current_idx++, word_buf);
        word_len = 0;
      }

      sent->delimiter = *p;

      char *remaining = p + 1;
      while (*remaining == ' ' || *remaining == '\t' || *remaining == '\n')
        remaining++;

      if (*remaining)
      {
        Sentence new_sent = {0};
        new_sent.delimiter = 0;

        char *word_start = remaining;
        int in_word = 0;
        while (*remaining)
        {
          if (*remaining == ' ' || *remaining == '\t' || *remaining == '\n')
          {
            if (in_word)
            {
              *remaining = 0;
              add_word_to_sentence(&new_sent, word_start);
              in_word = 0;
            }
          }
          else if (*remaining == '.' || *remaining == '!' || *remaining == '?')
          {
            if (in_word)
            {
              *remaining = 0;
              add_word_to_sentence(&new_sent, word_start);
              in_word = 0;
            }
            new_sent.delimiter = *remaining;
            remaining++;
            break;
          }
          else
          {
            if (!in_word)
            {
              word_start = remaining;
              in_word = 1;
            }
          }
          remaining++;
        }
        if (in_word)
        {
          add_word_to_sentence(&new_sent, word_start);
        }

        if (state->sentence_count >= state->sentence_capacity)
        {
          state->sentence_capacity = state->sentence_capacity == 0 ? 8 : state->sentence_capacity * 2;
          state->sentences = realloc(state->sentences, state->sentence_capacity * sizeof(Sentence));
        }

        for (int i = state->sentence_count; i > lock->sentence_idx + 1; i--)
        {
          state->sentences[i] = state->sentences[i - 1];
        }
        state->sentences[lock->sentence_idx + 1] = new_sent;
        state->sentence_count++;
        for (int i = 0; i < state->lock_count; i++)
        {
          if (state->locks[i].sentence_idx > lock->sentence_idx)
          {
            state->locks[i].sentence_idx++;
          }
        }
      }

      free(content_copy);
      return OK;
    }
    else
    {
      if (word_len < 255)
      {
        word_buf[word_len++] = *p;
      }
      p++;
    }
  }

  if (word_len > 0)
  {
    word_buf[word_len] = 0;
    insert_word_at(sent, current_idx, word_buf);
  }

  free(content_copy);
  return OK;
}

// Commit write session
int ss_files_write_commit(const char *file, const char *user)
{
  FileState *state = find_file_in_cache(file);
  if (!state)
    return ERR_NOT_FOUND;

  SentenceLock *lock = find_lock(state, user);
  if (!lock)
  {
    return ERR_BAD_REQUEST;
  }

  char src_path[512], undo_path[512];
  snprintf(src_path, sizeof src_path, "%s%s", DATA_DIR, file);
  snprintf(undo_path, sizeof undo_path, "%s%s.bak", UNDO_DIR, file);

  FILE *src = fopen(src_path, "r");
  FILE *dst = fopen(undo_path, "w");
  if (src && dst)
  {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, src)) > 0)
    {
      fwrite(buf, 1, n, dst);
    }
    fclose(src);
    fclose(dst);
  }
  else
  {
    if (src)
      fclose(src);
    if (dst)
      fclose(dst);
  }

  char content[8192];
  rebuild_file(state, content, sizeof content);

  FILE *fp = fopen(src_path, "w");
  if (!fp)
    return ERR_INTERNAL;
  fprintf(fp, "%s", content);
  fclose(fp);

  state->metadata.modified_time = time(NULL);
  state->metadata.accessed_time = time(NULL);
  if (user && user[0])
  {
    strncpy(state->metadata.last_access_user, user, sizeof state->metadata.last_access_user - 1);
    state->metadata.last_access_user[sizeof state->metadata.last_access_user - 1] = '\0';
  }
  update_metadata_counts(state);

  remove_lock(state, user);

  // Save metadata to disk
  save_metadata(state);

  return OK;
}

// Undo last commit
int ss_files_undo(const char *file, const char *user)
{
  FileState *state = load_file(file);
  if (!state)
    return ERR_NOT_FOUND;

  int access_check = check_access(file, user, 1);
  if (access_check != OK)
  {
    return access_check;
  }

  char src_path[512], undo_path[512];
  snprintf(src_path, sizeof src_path, "%s%s", DATA_DIR, file);
  snprintf(undo_path, sizeof undo_path, "%s%s.bak", UNDO_DIR, file);

  FILE *undo = fopen(undo_path, "r");
  if (!undo)
    return ERR_NOT_FOUND;

  fseek(undo, 0, SEEK_END);
  long size = ftell(undo);
  fseek(undo, 0, SEEK_SET);

  char *content = malloc(size + 1);
  if (!content)
  {
    fclose(undo);
    return ERR_INTERNAL;
  }

  fread(content, 1, size, undo);
  content[size] = 0;
  fclose(undo);

  FILE *fp = fopen(src_path, "w");
  if (!fp)
  {
    free(content);
    return ERR_INTERNAL;
  }
  fprintf(fp, "%s", content);
  fclose(fp);
  free(content);

  free_file_state(state);
  tokenize_file(src_path, state);
  update_metadata_counts(state);
  state->metadata.modified_time = time(NULL);
  state->metadata.accessed_time = time(NULL);
  if (user && user[0])
  {
    strncpy(state->metadata.last_access_user, user, sizeof state->metadata.last_access_user - 1);
    state->metadata.last_access_user[sizeof state->metadata.last_access_user - 1] = '\0';
  }
  save_metadata(state);

  return OK;
}

// Create new file
int ss_files_create(const char *file, const char *owner)
{
  // Check if file is already in cache
  if (find_file_in_cache(file))
  {
    return ERR_CONFLICT;
  }

  char filepath[512];
  snprintf(filepath, sizeof filepath, "%s%s", DATA_DIR, file);

  FILE *fp = fopen(filepath, "r");
  if (fp)
  {
    fclose(fp);
    return ERR_CONFLICT;
  }

  fp = fopen(filepath, "w");
  if (!fp)
    return ERR_INTERNAL;
  // Create completely empty file
  fclose(fp);

  FileState *state = calloc(1, sizeof(FileState));
  if (!state)
    return ERR_INTERNAL;

  strncpy(state->filename, file, sizeof state->filename - 1);
  strncpy(state->owner, owner, sizeof state->owner - 1);
  strncpy(state->metadata.owner, owner, sizeof state->metadata.owner - 1);
  strncpy(state->metadata.filename, file, sizeof state->metadata.filename - 1);

  state->metadata.created_time = time(NULL);
  state->metadata.modified_time = time(NULL);
  state->metadata.accessed_time = time(NULL);
  strncpy(state->metadata.last_access_user, owner, sizeof state->metadata.last_access_user - 1);

  if (tokenize_file(filepath, state) != OK)
  {
    free(state);
    return ERR_INTERNAL;
  }

  update_metadata_counts(state);

  if (g_file_count < MAX_FILES)
  {
    g_file_cache[g_file_count++] = state;
  }

  // Save metadata to disk
  save_metadata(state);

  return OK;
}

// Delete file
int ss_files_delete(const char *file, const char *actor)
{
  FileState *state = load_file(file);
  if (!state)
    return ERR_NOT_FOUND;

  if (strcmp(state->metadata.owner, actor) != 0)
    return ERR_UNAUTHORIZED;

  if (any_active_locks(state))
    return ERR_LOCKED;

  char filepath[512], undopath[512], metapath[512];
  snprintf(filepath, sizeof filepath, "%s%s", DATA_DIR, file);
  snprintf(undopath, sizeof undopath, "%s%s.bak", UNDO_DIR, file);
  snprintf(metapath, sizeof metapath, "%s%s.json", META_DIR, file);

  for (int i = 0; i < g_file_count; i++)
  {
    if (g_file_cache[i] && strcmp(g_file_cache[i]->filename, file) == 0)
    {
      free_file_state(g_file_cache[i]);
      free(g_file_cache[i]);
      for (int j = i; j < g_file_count - 1; j++)
      {
        g_file_cache[j] = g_file_cache[j + 1];
      }
      g_file_count--;
      break;
    }
  }

  unlink(filepath);
  unlink(undopath);
  unlink(metapath);

  return OK;
}

// Get file info
int ss_files_get_info(const char *file, char *info, int maxlen)
{
  FileState *state = load_file(file);
  if (!state)
    return ERR_NOT_FOUND;

  struct stat st;
  char filepath[512];
  snprintf(filepath, sizeof filepath, "%s%s", DATA_DIR, file);
  if (stat(filepath, &st) != 0)
    return ERR_NOT_FOUND;

  char created[64], modified[64], accessed[64];
  strftime(created, sizeof created, "%Y-%m-%d %H:%M", localtime(&state->metadata.created_time));
  strftime(modified, sizeof modified, "%Y-%m-%d %H:%M", localtime(&state->metadata.modified_time));
  strftime(accessed, sizeof accessed, "%Y-%m-%d %H:%M", localtime(&state->metadata.accessed_time));

  char access_buf[1024] = "";
  snprintf(access_buf, sizeof access_buf, "%s (RW)", state->metadata.owner);
  for (int i = 0; i < state->metadata.access_count; i++)
  {
    strncat(access_buf, ", ", sizeof access_buf - strlen(access_buf) - 1);
    strncat(access_buf, state->metadata.access_list[i].username, sizeof access_buf - strlen(access_buf) - 1);
    strncat(access_buf, " (", sizeof access_buf - strlen(access_buf) - 1);
    if (state->metadata.access_list[i].can_read && state->metadata.access_list[i].can_write)
      strncat(access_buf, "RW", sizeof access_buf - strlen(access_buf) - 1);
    else if (state->metadata.access_list[i].can_write)
      strncat(access_buf, "W", sizeof access_buf - strlen(access_buf) - 1);
    else if (state->metadata.access_list[i].can_read)
      strncat(access_buf, "R", sizeof access_buf - strlen(access_buf) - 1);
    else
      strncat(access_buf, "-", sizeof access_buf - strlen(access_buf) - 1);
    strncat(access_buf, ")", sizeof access_buf - strlen(access_buf) - 1);
  }
  char last_user[64];
  if (state->metadata.last_access_user[0])
    strncpy(last_user, state->metadata.last_access_user, sizeof last_user - 1);
  else
    strncpy(last_user, state->metadata.owner, sizeof last_user - 1);
  last_user[sizeof last_user - 1] = '\0';

  snprintf(info, maxlen,
           "File:%s||Owner:%s||Created:%s||LastModified:%s||Size:%lld bytes||Access:%s||LastAccessed:%s||LastAccessUser:%s",
           state->filename,
           state->metadata.owner,
           created,
           modified,
           (long long)st.st_size,
           access_buf,
           accessed,
           last_user);

  return OK;
}

// List all files
int ss_files_list_all(char *list, int maxlen, int include_all, int include_details, const char *user)
{
  list[0] = 0;
  int pos = 0;

  for (int i = 0; i < g_file_count; i++)
  {
    FileState *state = g_file_cache[i];
    if (!state)
      continue;

    // Verify file still exists on disk
    char filepath[512];
    snprintf(filepath, sizeof filepath, "%s%s", DATA_DIR, state->filename);
    struct stat st;
    if (stat(filepath, &st) != 0)
    {
      // File was deleted externally, skip it
      continue;
    }

    // Check access
    int has_access = (strcmp(state->metadata.owner, user) == 0);
    if (!has_access)
    {
      for (int j = 0; j < state->metadata.access_count; j++)
      {
        if (strcmp(state->metadata.access_list[j].username, user) == 0)
        {
          has_access = 1;
          break;
        }
      }
    }

    // Skip files without access if -a flag not set
    if (!include_all && !has_access)
      continue;

    if (include_details)
    {
      char timestr[64];
      strftime(timestr, sizeof timestr, "%Y-%m-%d %H:%M:%S", localtime(&state->metadata.modified_time));

      pos += snprintf(list + pos, maxlen - pos,
                      "%s | Owner: %s | Words: %d | Chars: %d | Modified: %s;;",
                      state->filename, state->metadata.owner,
                      state->metadata.word_count, state->metadata.char_count, timestr);
    }
    else
    {
      pos += snprintf(list + pos, maxlen - pos, "%s;;", state->filename);
    }

    if (pos >= maxlen - 1)
      break;
  }

  return OK;
}

// Add access to user
int ss_files_add_access(const char *file, const char *actor, const char *target_user, const char *mode)
{
  FileState *state = find_file_in_cache(file);
  if (!state)
    state = load_file(file);
  if (!state)
    return ERR_NOT_FOUND;

  if (strcmp(state->metadata.owner, actor) != 0)
    return ERR_UNAUTHORIZED;

  // Check if user already has access
  for (int i = 0; i < state->metadata.access_count; i++)
  {
    if (strcmp(state->metadata.access_list[i].username, target_user) == 0)
    {
      // Update existing entry
      if (strcmp(mode, "W") == 0)
      {
        state->metadata.access_list[i].can_read = 1;
        state->metadata.access_list[i].can_write = 1;
      }
      else
      {
        state->metadata.access_list[i].can_read = 1;
      }
      save_metadata(state);
      return OK;
    }
  }

  // Add new entry
  if (state->metadata.access_count >= state->metadata.access_capacity)
  {
    state->metadata.access_capacity = state->metadata.access_capacity == 0 ? 4 : state->metadata.access_capacity * 2;
    state->metadata.access_list = realloc(state->metadata.access_list,
                                          state->metadata.access_capacity * sizeof(AccessEntry));
  }

  AccessEntry *entry = &state->metadata.access_list[state->metadata.access_count++];
  strncpy(entry->username, target_user, sizeof entry->username - 1);
  entry->can_read = 1;
  entry->can_write = (strcmp(mode, "W") == 0) ? 1 : 0;

  save_metadata(state);
  return OK;
}

// Remove access from user
int ss_files_remove_access(const char *file, const char *actor, const char *target_user)
{
  FileState *state = find_file_in_cache(file);
  if (!state)
    return ERR_NOT_FOUND;

  if (strcmp(state->metadata.owner, actor) != 0)
    return ERR_UNAUTHORIZED;

  for (int i = 0; i < state->metadata.access_count; i++)
  {
    if (strcmp(state->metadata.access_list[i].username, target_user) == 0)
    {
      // Shift remaining entries
      for (int j = i; j < state->metadata.access_count - 1; j++)
      {
        state->metadata.access_list[j] = state->metadata.access_list[j + 1];
      }
      state->metadata.access_count--;
      save_metadata(state);
      return OK;
    }
  }

  return ERR_NOT_FOUND;
}

// ==================== FOLDER OPERATIONS ====================

// Create a folder
int ss_files_create_folder(const char *foldername)
{
  char folderpath[512];
  snprintf(folderpath, sizeof(folderpath), "%s%s", DATA_DIR, foldername);

  // Check if folder already exists
  struct stat st;
  if (stat(folderpath, &st) == 0)
  {
    return ERR_ALREADY_EXISTS;
  }

  // Create folder
  if (mkdir(folderpath, 0755) != 0)
  {
    return ERR_INTERNAL;
  }

  return OK;
}

// Move a file to a folder
int ss_files_move_file(const char *filename, const char *foldername)
{
  char src_path[512], dest_path[512];
  char new_filename[512];

  snprintf(src_path, sizeof(src_path), "%s%s", DATA_DIR, filename);
  snprintf(dest_path, sizeof(dest_path), "%s%s/%s", DATA_DIR, foldername, filename);
  snprintf(new_filename, sizeof(new_filename), "%s/%s", foldername, filename);

  // Check if source file exists
  struct stat st;
  if (stat(src_path, &st) != 0)
  {
    return ERR_NOT_FOUND;
  }

  // Check if destination folder exists
  char folderpath[512];
  snprintf(folderpath, sizeof(folderpath), "%s%s", DATA_DIR, foldername);
  if (stat(folderpath, &st) != 0 || !S_ISDIR(st.st_mode))
  {
    return ERR_NOT_FOUND;
  }

  // Create meta and undo subdirectories if they don't exist
  char meta_subdir[512], undo_subdir[512];
  snprintf(meta_subdir, sizeof(meta_subdir), "%s%s", META_DIR, foldername);
  snprintf(undo_subdir, sizeof(undo_subdir), "%s%s", UNDO_DIR, foldername);
  mkdir(meta_subdir, 0755);
  mkdir(undo_subdir, 0755);

  // Move file
  if (rename(src_path, dest_path) != 0)
  {
    return ERR_INTERNAL;
  }

  // Also move metadata and undo files
  char src_meta[512], dest_meta[512];
  snprintf(src_meta, sizeof(src_meta), "%s%s.json", META_DIR, filename);
  snprintf(dest_meta, sizeof(dest_meta), "%s%s/%s.json", META_DIR, foldername, filename);
  rename(src_meta, dest_meta); // Ignore errors

  char src_undo[512], dest_undo[512];
  snprintf(src_undo, sizeof(src_undo), "%s%s.bak", UNDO_DIR, filename);
  snprintf(dest_undo, sizeof(dest_undo), "%s%s/%s.bak", UNDO_DIR, foldername, filename);
  rename(src_undo, dest_undo); // Ignore errors

  // Update filename in cache
  FileState *state = find_file_in_cache(filename);
  if (state)
  {
    strncpy(state->filename, new_filename, sizeof(state->filename) - 1);
    state->filename[sizeof(state->filename) - 1] = '\0';
    strncpy(state->metadata.filename, new_filename, sizeof(state->metadata.filename) - 1);
    state->metadata.filename[sizeof(state->metadata.filename) - 1] = '\0';

    // Save updated metadata
    save_metadata(state);
  }

  return OK;
}

// View files in a folder
int ss_files_view_folder(const char *foldername, char *files, int maxlen)
{
  char folderpath[512];
  snprintf(folderpath, sizeof(folderpath), "%s%s", DATA_DIR, foldername);

  DIR *dir = opendir(folderpath);
  if (!dir)
  {
    return ERR_NOT_FOUND;
  }

  files[0] = '\0';
  struct dirent *entry;
  int first = 1;

  while ((entry = readdir(dir)) != NULL)
  {
    if (entry->d_name[0] == '.')
      continue; // Skip hidden files

    if (!first && strlen(files) + 3 < (size_t)maxlen)
    {
      strcat(files, ";;");
    }

    if (strlen(files) + strlen(entry->d_name) + 1 < (size_t)maxlen)
    {
      strcat(files, entry->d_name);
      first = 0;
    }
  }

  closedir(dir);
  return OK;
}

// ==================== CHECKPOINT OPERATIONS ====================

// Create a checkpoint for a file
int ss_files_create_checkpoint(const char *file, const char *tag)
{
  char filepath[512];
  snprintf(filepath, sizeof(filepath), "%s%s", DATA_DIR, file);

  // Check if file exists
  struct stat st;
  if (stat(filepath, &st) != 0)
  {
    return ERR_NOT_FOUND;
  }

  // Create checkpoint directory (mkdir -p)
  char checkpoint_dir[512];
  snprintf(checkpoint_dir, sizeof(checkpoint_dir), "%s%s", CHECKPOINT_DIR, file);
  ensure_dirs(checkpoint_dir);

  // Copy file to checkpoint
  char checkpoint_path[512];
  snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/%s", checkpoint_dir, tag);

  FILE *src = fopen(filepath, "r");
  if (!src)
    return ERR_INTERNAL;

  FILE *dest = fopen(checkpoint_path, "w");
  if (!dest)
  {
    fclose(src);
    return ERR_INTERNAL;
  }

  char buffer[8192];
  size_t bytes;
  while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0)
  {
    fwrite(buffer, 1, bytes, dest);
  }

  fclose(src);
  fclose(dest);

  return OK;
}

// View checkpoint content
int ss_files_view_checkpoint(const char *file, const char *tag, char *content, int maxlen)
{
  char checkpoint_path[512];
  snprintf(checkpoint_path, sizeof(checkpoint_path), "%s%s/%s", CHECKPOINT_DIR, file, tag);

  FILE *fp = fopen(checkpoint_path, "r");
  if (!fp)
  {
    return ERR_NOT_FOUND;
  }

  size_t bytes = fread(content, 1, maxlen - 1, fp);
  content[bytes] = '\0';
  fclose(fp);

  return OK;
}

// Revert file to a checkpoint
int ss_files_revert_checkpoint(const char *file, const char *tag)
{
  char filepath[512];
  snprintf(filepath, sizeof(filepath), "%s%s", DATA_DIR, file);

  char checkpoint_path[512];
  snprintf(checkpoint_path, sizeof(checkpoint_path), "%s%s/%s", CHECKPOINT_DIR, file, tag);

  // Check if checkpoint exists
  struct stat st;
  if (stat(checkpoint_path, &st) != 0)
  {
    return ERR_NOT_FOUND;
  }

  // Copy checkpoint back to file
  FILE *src = fopen(checkpoint_path, "r");
  if (!src)
    return ERR_INTERNAL;

  FILE *dest = fopen(filepath, "w");
  if (!dest)
  {
    fclose(src);
    return ERR_INTERNAL;
  }

  char buffer[8192];
  size_t bytes;
  while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0)
  {
    fwrite(buffer, 1, bytes, dest);
  }

  fclose(src);
  fclose(dest);

  // Invalidate cache
  for (int i = 0; i < g_file_count; i++)
  {
    if (g_file_cache[i] && strcmp(g_file_cache[i]->filename, file) == 0)
    {
      // Free and reload
      for (int j = 0; j < g_file_cache[i]->sentence_count; j++)
      {
        for (int k = 0; k < g_file_cache[i]->sentences[j].word_count; k++)
        {
          free(g_file_cache[i]->sentences[j].words[k]);
        }
        free(g_file_cache[i]->sentences[j].words);
      }
      free(g_file_cache[i]->sentences);
      free(g_file_cache[i]->metadata.access_list);
      free(g_file_cache[i]);
      g_file_cache[i] = NULL;
      break;
    }
  }

  return OK;
}

// List all checkpoints for a file
int ss_files_list_checkpoints(const char *file, char *checkpoints, int maxlen)
{
  char checkpoint_dir[512];
  snprintf(checkpoint_dir, sizeof(checkpoint_dir), "%s%s", CHECKPOINT_DIR, file);

  DIR *dir = opendir(checkpoint_dir);
  if (!dir)
  {
    checkpoints[0] = '\0';
    return OK; // No checkpoints yet
  }

  checkpoints[0] = '\0';
  struct dirent *entry;
  int first = 1;

  while ((entry = readdir(dir)) != NULL)
  {
    if (entry->d_name[0] == '.')
      continue;

    if (!first && strlen(checkpoints) + 2 < (size_t)maxlen)
    {
      strcat(checkpoints, ",");
    }

    if (strlen(checkpoints) + strlen(entry->d_name) + 1 < (size_t)maxlen)
    {
      strcat(checkpoints, entry->d_name);
      first = 0;
    }
  }

  closedir(dir);
  return OK;
}
