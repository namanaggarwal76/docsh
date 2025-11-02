#ifndef SS_TOKENIZE_H
#define SS_TOKENIZE_H

#include <stddef.h>

// A very simple sentence/word tokenization helper.
// Sentences end with one of '.', '!', '?'. The delimiter is attached to the last word.
// Words are separated by spaces. We do not handle quotes/escapes beyond this minimal spec.

typedef struct {
    char ***sent_words;   // [num_sentences][num_words_in_sentence] -> malloc'd C-strings
    int  *word_counts;    // per-sentence word counts
    int   num_sentences;
} ss_doc_tokens_t;

// Parse plain text into (sentences x words). Returns 0 on success.
int ss_tokenize(const char *text, ss_doc_tokens_t *out);

// Replace word at (sidx, widx) with new_word; if widx == word_counts[sidx], appends.
// Returns 0 on success, -1 on bad indices or allocation failure.
int ss_tokens_replace_or_append(ss_doc_tokens_t *doc, int sidx, int widx, const char *new_word);

// Compose back into a single newly-allocated string (caller must free).
// Words are joined with single spaces; sentence boundaries preserved as they were by attaching
// the original delimiter to the last word during tokenization. We do not insert extra newlines.
char *ss_tokens_compose(const ss_doc_tokens_t *doc);

// Free all allocations inside doc
void ss_tokens_free(ss_doc_tokens_t *doc);

#endif // SS_TOKENIZE_H
