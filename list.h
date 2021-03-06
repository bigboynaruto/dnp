#ifndef LIST_H
#define LIST_H
#include <stddef.h>
#include <stdbool.h>

typedef struct list_t {
	void *next;
	void *data;
} list_t;

list_t *list_new_node(void *data);
void list_append(list_t *list, void *item);
void list_cat(list_t *head, list_t *tail);
void *list_search(void *key, list_t *base, int (*cmp)(void *, void *));
list_t *list_remove(void *key, list_t *base, int (*pred)(void *, void *), bool free_data);
void list_foreach(list_t *base, void (*cb)(void *elem, void *data), void *data);
void list_free(list_t *list);
list_t *list_filter(list_t *base, int (*pred)(void *));
list_t *list_filter_by_key(void *key, list_t *base, int (*pred)(void *, void *));
list_t *list_random_sample(list_t *base, size_t count);
size_t list_length(list_t *list);
#endif //LIST_H
