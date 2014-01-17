#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clir.h"


void completion(const char *buf, clirCompletions *lc) {
    if (buf[0] == 'h') {
        clirAddCompletion(lc,"hello");
        clirAddCompletion(lc,"hi");
        clirAddCompletion(lc,"hey");
        clirAddCompletion(lc,"howzit");
    }
}

int main(int argc, char **argv) {
    char *line;
    char *prgname = argv[0];

    /* Parse options, with --multiline we enable multi line editing. */
    while(argc > 1) {
        argc--;
        argv++;
        if (!strcmp(*argv,"--multiline")) {
            clirSetMultiLine(1);
            printf("Multi-line mode enabled.\n");
        } else {
            fprintf(stderr, "Usage: %s [--multiline]\n", prgname);
            exit(1);
        }
    }

    /* Set the completion callback. This will be called every time the
     * user uses the <tab> key. */
    clirSetCompletionCallback(completion);

    /* Load history from file. The history file is just a plain text file
     * where entries are separated by newlines. */
    clirHistoryLoad("history.txt"); /* Load the history at startup */

    /* Now this is the main loop of the typical clir-based application.
     * The call to clir() will block as long as the user types something
     * and presses enter.
     *
     * The typed string is returned as a malloc() allocated string by
     * clir, so the user needs to free() it. */
    while((line = clir("hello> ")) != NULL) {
        /* Do something with the string. */
        if (line[0] != '\0' && line[0] != '/') {
            //printf("echo: '%s'\n", line);
            clirHistoryAdd(line); /* Add to the history. */
            clirHistorySave("history.txt"); /* Save the history on disk. */
        } else if (!strncmp(line,"/historylen",11)) {
            /* The "/historylen" command will change the history len. */
            int len = atoi(line+11);
            clirHistorySetMaxLen(len);
        } else if (line[0] == '/') {
            printf("Unreconized command: %s\n", line);
        }
        free(line);
    }
    return 0;
}
