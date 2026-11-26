#pragma once

#include "weread_book.h"

void weread_ui_begin();
void weread_ui_set_book(const WereadBook& book);
void weread_ui_set_day(const WereadDay& day);
void weread_ui_tick();
void weread_ui_next();
void weread_ui_prev();
uint8_t weread_ui_page();
int weread_ui_book_index();
