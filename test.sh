#!/bin/bash

cd /root/toy_database/build/src

# Remove old database file if exists
rm -f ../../toydb.db

echo "=== Testing ToyDB ==="
echo ""

# Run tests
cat ../../test_queries.txt | ./toy_db 2>&1

echo ""
echo "=== Test completed ==="

