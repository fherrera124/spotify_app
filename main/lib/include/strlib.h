#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct StrListItem StrListItem;
typedef struct StrList     StrList;

struct StrListItem {
    char*        str;
    StrListItem* next;
};
struct StrList {
    StrListItem* first;
    StrListItem* last;
    int          count;
};

esp_err_t strListAppend(StrList* list, char* str);
void      strListClear(StrList* list);
int       strListFindItem(StrList* list, char* str);
bool      strListEqual(StrList* list1, StrList* list2);
