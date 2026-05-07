#!/bin/bash
# Create and seed the database
cd "$(dirname "$0")"
rm -f data.db
sqlite3 data.db < seed.sql
echo "Database created and seeded: data.db"
sqlite3 data.db "SELECT COUNT(*) || ' products, ' || (SELECT COUNT(*) FROM posts) || ' posts' FROM products;"
