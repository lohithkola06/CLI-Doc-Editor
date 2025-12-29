#include "nm_search.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define HASH_TABLE_SIZE 1024
#define LRU_CACHE_SIZE 256

// Hash table entry
typedef struct HashEntry
{
  char filename[256];
  char ss_id[64];
  struct HashEntry *next;
  unsigned long last_access; // For LRU
} HashEntry;

// Hash table with chaining
static HashEntry *hash_table[HASH_TABLE_SIZE];
static unsigned long access_counter = 0;

// Simple hash function (djb2)
static unsigned long hash_string(const char *str)
{
  unsigned long hash = 5381;
  int c;
  while ((c = *str++))
  {
    hash = ((hash << 5) + hash) + c; // hash * 33 + c
  }
  return hash % HASH_TABLE_SIZE;
}

// Initialize search system
int nm_search_init(void)
{
  for (int i = 0; i < HASH_TABLE_SIZE; i++)
  {
    hash_table[i] = NULL;
  }
  access_counter = 0;
  return 0;
}

// Add entry to hash table - O(1) average
int nm_search_add_entry(const char *filename, const char *ss_id)
{
  if (!filename || !ss_id)
    return -1;

  unsigned long index = hash_string(filename);

  // Check if entry already exists
  HashEntry *current = hash_table[index];
  while (current)
  {
    if (strcmp(current->filename, filename) == 0)
    {
      // Update existing entry
      strncpy(current->ss_id, ss_id, sizeof(current->ss_id) - 1);
      current->ss_id[sizeof(current->ss_id) - 1] = '\0';
      current->last_access = ++access_counter;
      return 0;
    }
    current = current->next;
  }

  // Create new entry
  HashEntry *new_entry = (HashEntry *)malloc(sizeof(HashEntry));
  if (!new_entry)
    return -1;

  strncpy(new_entry->filename, filename, sizeof(new_entry->filename) - 1);
  new_entry->filename[sizeof(new_entry->filename) - 1] = '\0';
  strncpy(new_entry->ss_id, ss_id, sizeof(new_entry->ss_id) - 1);
  new_entry->ss_id[sizeof(new_entry->ss_id) - 1] = '\0';
  new_entry->last_access = ++access_counter;

  // Insert at head of chain
  new_entry->next = hash_table[index];
  hash_table[index] = new_entry;

  return 0;
}

// Lookup entry in hash table - O(1) average
int nm_search_lookup(const char *filename, char *ss_id_out, int buflen)
{
  if (!filename || !ss_id_out)
    return -1;

  unsigned long index = hash_string(filename);

  HashEntry *current = hash_table[index];
  while (current)
  {
    if (strcmp(current->filename, filename) == 0)
    {
      // Found! Update LRU counter
      current->last_access = ++access_counter;
      strncpy(ss_id_out, current->ss_id, buflen - 1);
      ss_id_out[buflen - 1] = '\0';
      return 0;
    }
    current = current->next;
  }

  return -1; // Not found
}

// Remove entry from hash table
int nm_search_remove_entry(const char *filename)
{
  if (!filename)
    return -1;

  unsigned long index = hash_string(filename);
  HashEntry *current = hash_table[index];
  HashEntry *prev = NULL;

  while (current)
  {
    if (strcmp(current->filename, filename) == 0)
    {
      if (prev)
      {
        prev->next = current->next;
      }
      else
      {
        hash_table[index] = current->next;
      }
      free(current);
      return 0;
    }
    prev = current;
    current = current->next;
  }

  return -1;
}
