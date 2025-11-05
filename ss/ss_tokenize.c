#include "ss_tokenize.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_sentence_end(char c) {
    return c == '.' || c == '!' || c == '?';
}

static char *str_dup_range(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *str_dup_c(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

int ss_tokenize(const char *text, ss_doc_tokens_t *out) {
    if (!text || !out) return -1;
    memset(out, 0, sizeof(*out));

    // We'll accumulate per-sentence arrays in dynamic buffers
    int cap_sent = 8;
    int num_sent = 0;
    char ***sent_words = (char ***)calloc((size_t)cap_sent, sizeof(char **));
    int *word_counts = (int *)calloc((size_t)cap_sent, sizeof(int));
    int *word_caps = (int *)calloc((size_t)cap_sent, sizeof(int));
    if (!sent_words || !word_counts || !word_caps) goto oom;

    // Start first sentence
    num_sent = 1;
    word_caps[0] = 8;
    sent_words[0] = (char **)calloc((size_t)word_caps[0], sizeof(char *));
    if (!sent_words[0]) goto oom;

    const char *p = text;
    const char *tok_start = NULL; // start of current word
    int cur_s = 0;

    while (*p) {
        char c = *p;
        if (isspace((unsigned char)c)) {
            // end of a word
            if (tok_start) {
                size_t n = (size_t)(p - tok_start);
                char *w = str_dup_range(tok_start, n);
                if (!w) goto oom;
                if (word_counts[cur_s] + 1 > word_caps[cur_s]) {
                    int nc = word_caps[cur_s] * 2;
                    char **nw = (char **)realloc(sent_words[cur_s], (size_t)nc * sizeof(char *));
                    if (!nw) { free(w); goto oom; }
                    sent_words[cur_s] = nw; word_caps[cur_s] = nc;
                }
                sent_words[cur_s][word_counts[cur_s]++] = w;
                tok_start = NULL;
            }
            p++;
            continue;
        }

        if (is_sentence_end(c)) {
            // include delimiter into the last word if we have any word open or last emitted
            if (tok_start) {
                size_t n = (size_t)(p - tok_start);
                // + delimiter char
                char *w = (char *)malloc(n + 2);
                if (!w) goto oom;
                memcpy(w, tok_start, n);
                w[n] = c; w[n+1] = '\0';
                if (word_counts[cur_s] + 1 > word_caps[cur_s]) {
                    int nc = word_caps[cur_s] * 2;
                    char **nw = (char **)realloc(sent_words[cur_s], (size_t)nc * sizeof(char *));
                    if (!nw) { free(w); goto oom; }
                    sent_words[cur_s] = nw; word_caps[cur_s] = nc;
                }
                sent_words[cur_s][word_counts[cur_s]++] = w;
                tok_start = NULL;
            } else if (word_counts[cur_s] > 0) {
                // attach delimiter to the last emitted word
                char *last = sent_words[cur_s][word_counts[cur_s]-1];
                size_t ln = strlen(last);
                char *nw = (char *)realloc(last, ln + 2);
                if (!nw) goto oom;
                nw[ln] = c; nw[ln+1] = '\0';
                sent_words[cur_s][word_counts[cur_s]-1] = nw;
            } else {
                // sentence delimiter without word -> treat as a one-char word
                char *w = (char *)malloc(2);
                if (!w) goto oom;
                w[0] = c; w[1] = '\0';
                if (word_counts[cur_s] + 1 > word_caps[cur_s]) {
                    int nc = word_caps[cur_s] * 2;
                    char **nw = (char **)realloc(sent_words[cur_s], (size_t)nc * sizeof(char *));
                    if (!nw) { free(w); goto oom; }
                    sent_words[cur_s] = nw; word_caps[cur_s] = nc;
                }
                sent_words[cur_s][word_counts[cur_s]++] = w;
            }
            // End sentence, start a new one (but skip any trailing spaces in next loop)
            if (num_sent + 1 > cap_sent) {
                int nc = cap_sent * 2;
                char ***sw = (char ***)realloc(sent_words, (size_t)nc * sizeof(char **));
                int *wc = (int *)realloc(word_counts, (size_t)nc * sizeof(int));
                int *wcap = (int *)realloc(word_caps, (size_t)nc * sizeof(int));
                if (!sw || !wc || !wcap) goto oom;
                // Note: if any realloc returned NULL while others not, we're in OOM; simplistic check
                sent_words = sw; word_counts = wc; word_caps = wcap; cap_sent = nc;
            }
            cur_s = num_sent;
            num_sent++;
            word_caps[cur_s] = 8;
            sent_words[cur_s] = (char **)calloc((size_t)word_caps[cur_s], sizeof(char *));
            if (!sent_words[cur_s]) goto oom;
            p++;
            continue;
        }

        // start a new token if needed
        if (!tok_start) tok_start = p;
        p++;
    }

    // flush last token if present (no trailing delimiter)
    if (tok_start) {
        size_t n = (size_t)(p - tok_start);
        char *w = str_dup_range(tok_start, n);
        if (!w) goto oom;
        if (word_counts[cur_s] + 1 > word_caps[cur_s]) {
            int nc = word_caps[cur_s] * 2;
            char **nw = (char **)realloc(sent_words[cur_s], (size_t)nc * sizeof(char *));
            if (!nw) { free(w); goto oom; }
            sent_words[cur_s] = nw; word_caps[cur_s] = nc;
        }
        sent_words[cur_s][word_counts[cur_s]++] = w;
    }

    // If the last sentence is empty (no words), reduce num_sentences
    if (num_sent > 1 && word_counts[num_sent - 1] == 0) {
        free(sent_words[num_sent - 1]);
        num_sent--;
    }

    out->sent_words = sent_words;
    out->word_counts = word_counts;
    out->num_sentences = num_sent;
    free(word_caps);
    return 0;

oom:
    if (sent_words) {
        for (int i = 0; i < cap_sent; ++i) {
            if (!sent_words[i]) continue;
            // word_caps may be 0 for uninitialized sentences; free based on word_counts which is safe for initialized indices
            for (int j = 0; j < (i < num_sent ? word_counts[i] : 0); ++j) free(sent_words[i][j]);
            free(sent_words[i]);
        }
        free(sent_words);
    }
    free(word_counts);
    free(word_caps);
    memset(out, 0, sizeof(*out));
    return -1;
}

int ss_tokens_replace_or_append(ss_doc_tokens_t *doc, int sidx, int widx, const char *new_word) {
    if (!doc || !new_word) return -1;
    if (sidx < 0 || sidx >= doc->num_sentences) return -1;
    int wc = doc->word_counts[sidx];
    if (widx < 0 || widx > wc) return -1; // allow equal for append
    if (widx == wc) {
        // Append mode; if appending a bare sentence delimiter, attach to previous word
        size_t nl = strlen(new_word);
        if (nl == 1 && is_sentence_end(new_word[0]) && wc > 0) {
            char **row = doc->sent_words[sidx];
            char *last = row[wc - 1];
            size_t ln = strlen(last);
            char *nw = (char *)realloc(last, ln + 2);
            if (!nw) return -1;
            nw[ln] = new_word[0]; nw[ln+1] = '\0';
            row[wc - 1] = nw;
            return 0;
        }
        // append
        char **row = doc->sent_words[sidx];
        int newc = wc + 1;
        char **nw = (char **)realloc(row, (size_t)newc * sizeof(char *));
        if (!nw) return -1;
        nw[wc] = str_dup_c(new_word);
        if (!nw[wc]) return -1;
        doc->sent_words[sidx] = nw;
        doc->word_counts[sidx] = newc;
        return 0;
    } else {
        // replace
        char **row = doc->sent_words[sidx];
        char *rep = str_dup_c(new_word);
        if (!rep) return -1;
        free(row[widx]);
        row[widx] = rep;
        return 0;
    }
}

char *ss_tokens_compose(const ss_doc_tokens_t *doc) {
    if (!doc) return NULL;
    // compute total length: sum of words + spaces between words and between sentences
    size_t total = 0;
    for (int i = 0; i < doc->num_sentences; ++i) {
        for (int j = 0; j < doc->word_counts[i]; ++j) {
            total += strlen(doc->sent_words[i][j]);
            if (j + 1 < doc->word_counts[i]) total += 1; // space between words
        }
        if (i + 1 < doc->num_sentences) total += 1; // space between sentences
    }
    char *out = (char *)malloc(total + 1);
    if (!out) return NULL;
    size_t w = 0;
    for (int i = 0; i < doc->num_sentences; ++i) {
        for (int j = 0; j < doc->word_counts[i]; ++j) {
            const char *s = doc->sent_words[i][j];
            size_t n = strlen(s);
            memcpy(out + w, s, n); w += n;
            if (j + 1 < doc->word_counts[i]) { out[w++] = ' '; }
        }
        if (i + 1 < doc->num_sentences) out[w++] = ' ';
    }
    out[w] = '\0';
    return out;
}

void ss_tokens_free(ss_doc_tokens_t *doc) {
    if (!doc || !doc->sent_words) return;
    for (int i = 0; i < doc->num_sentences; ++i) {
        for (int j = 0; j < doc->word_counts[i]; ++j) free(doc->sent_words[i][j]);
        free(doc->sent_words[i]);
    }
    free(doc->sent_words);
    free(doc->word_counts);
    doc->sent_words = NULL; doc->word_counts = NULL; doc->num_sentences = 0;
}
