#ifndef NM_SEARCH_H
#define NM_SEARCH_H

// Placeholder for search functionality (HashMap + LRU)
// To be implemented by Saharsh

int nm_search_init(void);
int nm_search_add_entry(const char *filename, const char *ss_id);
int nm_search_lookup(const char *filename, char *ss_id_out, int buflen);
int nm_search_remove_entry(const char *filename);

#endif

