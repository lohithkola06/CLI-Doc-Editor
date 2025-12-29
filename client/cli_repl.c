#include <stdio.h>
#include <string.h>

int cli_readline(char *buf, int buflen){
  printf("docs++> ");
  fflush(stdout);
  if (!fgets(buf, buflen, stdin)) return 0;
  size_t n=strlen(buf); if (n && buf[n-1]=='\n') buf[n-1]=0;
  return 1;
}

