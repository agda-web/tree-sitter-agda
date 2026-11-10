#include "tree_sitter/parser.h"
#include "tree_sitter/array.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define QUEUE_RESIZE(queue, _cap)                                                  \
    do {                                                                           \
        void *tmp = calloc((_cap), sizeof((queue).data[0]));                       \
        assert(tmp != NULL);                                                       \
        uint32_t count = (queue).tail - (queue).head;                              \
        for (uint32_t i = 0; i < count; i++) {                                     \
            ((uint16_t *)tmp)[i] = (queue).data[((queue).head + i) % (queue).cap]; \
        }                                                                          \
        if ((queue).data != NULL)                                                  \
            free((queue).data);                                                    \
        (queue).data = tmp;                                                        \
        (queue).head = 0;                                                          \
        (queue).tail = count;                                                      \
        (queue).cap = (_cap);                                                      \
    } while (0)

#define QUEUE_GROW(queue, _cap)                                                    \
    do {                                                                           \
        if ((queue).cap < (_cap)) {                                                \
            QUEUE_RESIZE((queue), (_cap));                                         \
        }                                                                          \
    } while (0)

#define QUEUE_PUSH(queue, el)                                                      \
    do {                                                                           \
        if ((queue).cap == 0) {                                                    \
            QUEUE_RESIZE((queue), 16);                                             \
        } else if ((queue).cap == ((queue).tail - (queue).head)) {                 \
            QUEUE_RESIZE((queue), (queue).cap * 2);                                \
        }                                                                          \
        (queue).data[(queue).tail % (queue).cap] = (el);                           \
        (queue).tail++;                                                            \
    } while (0)

#define QUEUE_POP(queue)                                                           \
    do {                                                                           \
        assert((queue).head < (queue).tail);                                       \
        (queue).head++;                                                            \
    } while (0)

#define QUEUE_FRONT(queue) (queue).data[(queue).head % (queue).cap]

#define QUEUE_EMPTY(queue) ((queue).head == (queue).tail)

#define QUEUE_NEW { .head = 0, .tail = 0, .cap = 0, .data = NULL }

#define QUEUE_FREE(queue)                                                          \
    do {                                                                           \
        if ((queue).data != NULL)                                                  \
            free((queue).data);                                                    \
    } while (0)

#define QUEUE_CLEAR(queue)                                                         \
    do {                                                                           \
        (queue).head = 0;                                                          \
        (queue).tail = 0;                                                          \
    } while (0)

enum TokenType {
    NEWLINE,
    INDENT,
    DEDENT,
    COMMENT,
};

typedef struct {
    uint32_t head;
    uint32_t tail;
    uint32_t cap;
    uint16_t *data;
} token_queue;

static token_queue token_queue_new() {
    token_queue queue = QUEUE_NEW;
    queue.data = calloc(1, sizeof(uint16_t));
    queue.cap = 1;
    return queue;
}

typedef struct {
    Array(uint16_t) indents;
    uint32_t queued_dedent_count;
    token_queue tokens;
} Scanner;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

bool tree_sitter_agda_external_scanner_scan(void *payload, TSLexer *lexer,
                                            const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;

    if (QUEUE_EMPTY(scanner->tokens)) {
        if (valid_symbols[DEDENT] && scanner->queued_dedent_count > 0) {
            scanner->queued_dedent_count--;
            QUEUE_PUSH(scanner->tokens, DEDENT);
            QUEUE_PUSH(scanner->tokens, NEWLINE);
        } else {
            bool skipped_newline = false;

            while (lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
                   lexer->lookahead == '\r' || lexer->lookahead == '\n') {
                if (lexer->lookahead == '\n') {
                    skipped_newline = true;
                    skip(lexer);
                } else {
                    skip(lexer);
                }
            }

            if (lexer->eof(lexer)) {
                if (valid_symbols[DEDENT] && scanner->indents.size > 1) {
                    array_pop(&scanner->indents);
                    QUEUE_PUSH(scanner->tokens, DEDENT);
                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                } else if (valid_symbols[NEWLINE]) {
                    QUEUE_PUSH(scanner->tokens, NEWLINE);
                }
            } else {
                bool next_token_is_comment = false;

                uint16_t indent_length = (uint16_t)lexer->get_column(lexer);

                bool indent = indent_length > *array_back(&scanner->indents);
                bool dedent = indent_length < *array_back(&scanner->indents);

                if (!next_token_is_comment) {
                    if (skipped_newline) {
                        if (indent) {
                            if (valid_symbols[INDENT]) {
                                array_push(&scanner->indents, indent_length);
                                QUEUE_PUSH(scanner->tokens, INDENT);
                            }
                        } else if (dedent) {
                            if (valid_symbols[NEWLINE]) {
                                QUEUE_PUSH(scanner->tokens, NEWLINE);
                            }
                        } else {
                            if (valid_symbols[NEWLINE]) {
                                QUEUE_PUSH(scanner->tokens, NEWLINE);
                            }
                        }
                    } else {
                        if (indent) {
                            if (valid_symbols[INDENT]) {
                                array_push(&scanner->indents, indent_length);
                                QUEUE_PUSH(scanner->tokens, INDENT);
                            }
                        } else if (dedent) {
                            array_pop(&scanner->indents);
                            while (indent_length < *array_back(&scanner->indents)) {
                                array_pop(&scanner->indents);
                                scanner->queued_dedent_count++;
                            }
                            if (valid_symbols[DEDENT]) {
                                QUEUE_PUSH(scanner->tokens, DEDENT);
                                QUEUE_PUSH(scanner->tokens, NEWLINE);
                            } else {
                                scanner->queued_dedent_count++;
                            }
                        }
                    }
                }
            }
        }
    }

    if (QUEUE_EMPTY(scanner->tokens)) {
        return false;
    }

    lexer->result_symbol = QUEUE_FRONT(scanner->tokens);
    QUEUE_POP(scanner->tokens);
    return true;
}

unsigned tree_sitter_agda_external_scanner_serialize(void *payload,
                                                     char *buffer) {
    Scanner *scanner = (Scanner *)payload;

    if (scanner->indents.size * sizeof(uint16_t) + 1 >
        TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
        return 0;
    }

    unsigned size = 0;

    buffer[size++] = (char)scanner->queued_dedent_count;

    memcpy(&buffer[size], scanner->indents.contents,
           scanner->indents.size * sizeof(uint16_t));
    size += (unsigned)(scanner->indents.size * sizeof(uint16_t));

    return size;
}

void tree_sitter_agda_external_scanner_deserialize(void *payload,
                                                   const char *buffer,
                                                   unsigned length) {
    Scanner *scanner = (Scanner *)payload;

    scanner->queued_dedent_count = 0;
    array_clear(&scanner->indents);

    if (length == 0) {
        if (buffer == NULL) {
            array_push(&scanner->indents, 0);
        }
        return;
    }

    scanner->queued_dedent_count = (uint8_t)buffer[0];

    unsigned size = 1;

    if (length > size) {
        array_reserve(&scanner->indents,
                      (uint32_t)(length - size) / sizeof(uint16_t));
        scanner->indents.size = (length - size) / sizeof(uint16_t);
        memcpy(scanner->indents.contents, &buffer[size],
               scanner->indents.size * sizeof(uint16_t));
        size += (unsigned)(scanner->indents.size * sizeof(uint16_t));
    }

    if (scanner->indents.size == 0) {
        array_push(&scanner->indents, 0);
        return;
    }

    assert(size == length);
}

void *tree_sitter_agda_external_scanner_create() {
    Scanner *scanner = calloc(1, sizeof(Scanner));
    array_init(&scanner->indents);
    scanner->tokens = token_queue_new();
    tree_sitter_agda_external_scanner_deserialize(scanner, NULL, 0);
    return scanner;
}

void tree_sitter_agda_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    array_delete(&scanner->indents);
    QUEUE_FREE(scanner->tokens);
    free(scanner);
}
