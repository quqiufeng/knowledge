#pragma once
#include "db.hpp"

#include <string>

struct BooksOptions {
    std::string books_dir{"/opt/books"};
    std::string only_book;
    long limit{0};
};

void import_books(Db& db, const BooksOptions& opt);
