/**
 * @file lv_g2d_buf_map.c
 *
 */

/**
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: MIT
 */

#include "lv_g2d_buf_map.h"

#if LV_USE_DRAW_G2D
#include <stdio.h>
#include "lv_g2d_utils.h"
#include "g2d.h"

/*********************
 *      DEFINES
 *********************/

#define HASH_TABLE_SIZE 50

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static unsigned long _map_hash_function(void * ptr);

static lv_map_item_t * _map_create_item(void * key, struct g2d_buf * value);

static void _map_free_item(lv_map_item_t * item);

void _handle_collision(unsigned long index, lv_map_item_t * item);

lv_list_t * _list_alloc();

lv_list_t * _list_insert(lv_list_t * list, lv_map_item_t * item);

lv_map_item_t * _list_remove(lv_list_t * list);

void _list_free(lv_list_t * list);

lv_list_t ** _create_overflow_list();

void _free_overflow_list();


/**********************
 *  STATIC VARIABLES
 **********************/

static lv_buf_map_t * table;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_create_buf_map()
{
    table = (lv_buf_map_t *) lv_malloc(sizeof(lv_buf_map_t));
    table->size = HASH_TABLE_SIZE;
    table->count = 0;
    table->items = (lv_map_item_t **) lv_malloc_zeroed(table->size * sizeof(lv_map_item_t *));

    for(int i = 0; i < table->size; i++)
        table->items[i] = NULL;

    table->overflow_list = _create_overflow_list(table);
}

void lv_free_buf_map()
{
    for(int i = 0; i < table->size; i++) {
        lv_map_item_t * item = table->items[i];
        if(item != NULL)
            _map_free_item(item);
    }

    _free_overflow_list(table);
    lv_free(table->items);
    lv_free(table);
}

void lv_insert_buf_map(void * key, struct g2d_buf * value)
{
    lv_map_item_t * item = _map_create_item(key, value);
    int index = _map_hash_function(key);
    lv_map_item_t * curr_item = table->items[index];

    if(table->items[index] == NULL) {
        /* Key not found. */
        if(table->count == table->size) {
            /* Table is full. */
            _map_free_item(item);
            G2D_ASSERT_MSG(false, "Insert Error: Hash Table is full\n");
            return;
        }
    }
    else {
        if(table->items[index]->key == key) {
            /* Key already exists, update value. */
            table->items[index]->value = value;
            return;
        }
        else {
            /* Handle the collision */
            _handle_collision(index, item);
            return;
        }
    }

    /* Insert item. */
    table->items[index] = item;
    table->count++;
}

struct g2d_buf * lv_search_buf_map(void * key)
{
    int index = _map_hash_function(key);
    lv_map_item_t * item = table->items[index];
    lv_list_t * head = table->overflow_list[index];

    if(item != NULL) {
        if(item->key == key)
            return item->value;

        if(head == NULL)
            return NULL;

        item = head->item;
        head = head->next;
    }
    return NULL;
}

void lv_free_item(void * key)
{
    /* Delete an item from the table. */
    int index = _map_hash_function(key);
    lv_map_item_t * item = table->items[index];
    lv_list_t * head = table->overflow_list[index];

    if(item == NULL) {
        return;
    }
    else if(head == NULL && item->key == key) {
        /* No collision chain, just remove item. */
        table->items[index] = NULL;
        _map_free_item(item);
        table->count--;
        return;
    }
    else if(head != NULL) {
        /* Collision chain exists. */
        if(item->key == key) {
            /* Remove this item and set the head as the new item. */
            _map_free_item(item);
            lv_list_t * node = head;
            head = head->next;
            node->next = NULL;
            table->items[index] = _map_create_item(node->item->key, node->item->value);
            _list_free(node);
            table->overflow_list[index] = head;
            return;
        }

        lv_list_t * curr = head;
        lv_list_t * prev = NULL;

        while(curr) {
            if(curr->item->key == key) {
                if(prev == NULL) {
                    /* First element of the chain. */
                    _list_free(head);
                    table->overflow_list[index] = NULL;
                    return;
                }
                else {
                    /* Somewhere in the chain. */
                    prev->next = curr->next;
                    curr->next = NULL;
                    _list_free(curr);
                    table->overflow_list[index] = head;
                    return;
                }
            }
            curr = curr->next;
            prev = curr;
        }
    }
}

void print_table()
{
    LV_LOG("\nHash Table\n-------------------\n");

    for(int i = 0; i < table->size; i++) {
        if(table->items[i]) {
            LV_LOG("Index:%d, Key:%p, Value:%p\n", i, table->items[i] -> key, table->items[i]->value);
        }
    }

    LV_LOG("-------------------\n\n");
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

unsigned long _map_hash_function(void * ptr)
{
    unsigned long i = 0;
    char str[64];
    sprintf(str, "%p", ptr);

    for(int j = 0; str[j]; j++)
        i += str[j];

    return i % HASH_TABLE_SIZE;
}

void _handle_collision(unsigned long index, lv_map_item_t * item)
{
    lv_list_t * head = table->overflow_list[index];

    if(head == NULL) {
        /* Create the list. */
        head = _list_alloc();
        head->item = item;
        table->overflow_list[index] = head;
        return;
    }
    else {
        /* Insert to the list. */
        table->overflow_list[index] = _list_insert(head, item);
        return;
    }
}

lv_map_item_t * _map_create_item(void * key, struct g2d_buf * value)
{
    lv_map_item_t * item = (lv_map_item_t *) lv_malloc(sizeof(lv_map_item_t));
    item->key = key;
    item->value = value;
    return item;
}

void _map_free_item(lv_map_item_t * item)
{
    /* Also free the g2d_buf. */
    lv_free(item->key);
    g2d_free(item->value);
    item->key = NULL;
    item->value = NULL;
    lv_free(item);
    item = NULL;
}

lv_list_t * _list_alloc()
{
    lv_list_t * list = (lv_list_t *) lv_malloc(sizeof(lv_list_t));
    return list;
}

lv_list_t * _list_insert(lv_list_t * list, lv_map_item_t * item)
{
    /* Insert item in list. */
    if(!list) {
        lv_list_t * head = _list_alloc();
        head->item = item;
        head->next = NULL;
        list = head;
        return list;
    }
    else if(list->next == NULL) {
        lv_list_t * node = _list_alloc();
        node->item = item;
        node->next = NULL;
        list->next = node;
        return list;
    }

    lv_list_t * temp = list;

    while(temp->next->next)
        temp = temp->next;

    lv_list_t * node = _list_alloc();
    node->item = item;
    node->next = NULL;
    temp->next = node;
    return list;
}

lv_map_item_t * _list_remove(lv_list_t * list)
{
    if(!list)
        return NULL;
    if(!list->next)
        return NULL;

    lv_list_t * node = list->next;
    lv_list_t * temp = list;
    temp->next = NULL;
    list = node;
    lv_map_item_t * it = NULL;
    memcpy(temp->item, it, sizeof(lv_map_item_t));
    lv_free(temp->item->key);
    g2d_free(temp->item->value);
    lv_free(temp->item);
    lv_free(temp);
    return it;
}

void _list_free(lv_list_t * list)
{
    lv_list_t * temp = list;

    while(list) {
        temp = list;
        list = list->next;
        lv_free(temp->item->key);
        g2d_free(temp->item->value);
        temp->item->key = NULL;
        temp->item->value = NULL;
        lv_free(temp->item);
        temp->item = NULL;
        lv_free(temp);
        temp = NULL;
    }
}

lv_list_t ** _create_overflow_list()
{
    /* Create overflow list, an array of list. */
    lv_list_t ** lists = (lv_list_t **) lv_malloc_zeroed(table->size * sizeof(lv_list_t *));

    for(int i = 0; i < table->size; i++)
        lists[i] = NULL;

    return lists;
}

void _free_overflow_list()
{
    /* Free all lists. */
    lv_list_t ** lists = table->overflow_list;

    for(int i = 0; i < table->size; i++)
        _list_free(lists[i]);

    lv_free(lists);
    lists = NULL;
}

#endif /*LV_USE_DRAW_G2D*/