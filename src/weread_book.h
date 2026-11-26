#pragma once

#include <stdint.h>

constexpr int kWereadDayMax = 5;

struct WereadBook {
    bool ok;
    char updated_at[40];
    char slot[48];
    char why[48];
    char book_id[32];
    char title[128];
    char author[64];
    char cover_url[256];
    char category[48];
    char rating[24];
    char rating_count[32];
    char words[32];
    char quote[220];
    char quote_hit[48];
    char quote_source[32];
    char eye[32];
    char lede[96];
    char intro[480];
    char intro_hit[64];
    char voice[32];
    char recommend_lead[160];
    char recommend[480];
    char recommend_hit[64];
    char error[160];
};

struct WereadDay {
    bool ok;
    char date[16];
    char updated_at[40];
    char error[160];
    int count;
    WereadBook books[kWereadDayMax];
};

bool weread_book_fetch(const char* url, WereadBook& out);
bool weread_day_fetch(const char* url, WereadDay& out);
