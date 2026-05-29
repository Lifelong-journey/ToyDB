#!/bin/bash

set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
BIN_DIR="$BUILD_DIR/src"
DB_FILE="$BIN_DIR/toydb.db"

# 过滤不稳定的调试日志（如页号、事务 ID 等），只保留与 SQL 语义相关的输出
filter_output() {
  sed -E \
    -e 's/PageManager: Initialized with next_page_id: [0-9]+/PageManager: Initialized with next_page_id: N/' \
    -e 's/PageManager: Allocated new page with ID: [0-9]+/PageManager: Allocated new page with ID: N/' \
    -e 's/PageManager: Fetching page [0-9]+ from buffer./PageManager: Fetching page N from buffer./' \
    -e 's/PageManager: Attempted to fetch non-existent page: [0-9]+/PageManager: Attempted to fetch non-existent page: N/' \
    -e 's/MarkDirty: Marking page [0-9]+ as dirty./MarkDirty: Marking page N as dirty./' \
    -e 's/page_id: [0-9]+/page_id: N/' \
    -e 's/BPlusTree: Destructor called./BPlusTree: Destructor called./' \
    -e 's/Transaction [0-9]+ started./Transaction TX started./' \
    -e 's/Transaction [0-9]+ committed./Transaction TX committed./' \
    -e 's/Transaction [0-9]+ rolled back./Transaction TX rolled back./' \
    -e '/Search: /d' \
    -e '/Delete: /d' \
    -e '/Insert: /d' \
    -e '/FindLeaf: /d'
}

echo "== Building toy_db =="
cd "$BUILD_DIR"
make -j4 >/dev/null

cd "$BIN_DIR"

# 确保每次回归测试从干净的数据库/日志开始
rm -f "$DB_FILE" "$BIN_DIR/toydb.log"

EXIT_CODE=0

echo ""
echo "== Running concurrency_test =="
if ./concurrency_test >/dev/null 2>&1; then
  echo "  [OK] concurrency_test"
else
  echo "  [FAIL] concurrency_test"
  EXIT_CODE=1
fi

echo ""
echo "== Running concurrency_detailed_test =="
if ./concurrency_detailed_test >/dev/null 2>&1; then
  echo "  [OK] concurrency_detailed_test"
else
  echo "  [FAIL] concurrency_detailed_test"
  EXIT_CODE=1
fi

for sql in "$ROOT_DIR/tests/"*.sql; do
  name="$(basename "$sql" .sql)"
  expected="$ROOT_DIR/tests/${name}.out"
  actual="$ROOT_DIR/tests/${name}.actual"
  expected_norm="$ROOT_DIR/tests/${name}.out.norm"
  actual_norm="$ROOT_DIR/tests/${name}.actual.norm"

  echo ""
  echo "== Running test: $name =="

  # 每个测试使用新的数据库文件，避免互相影响
  rm -f "$DB_FILE"

  # 运行并捕获输出（stdout+stderr），保证与 expected 一致
  cat "$sql" | ./toy_db >"$actual" 2>&1 || true

  # 归一化输出，去掉不稳定部分
  filter_output <"$actual" >"$actual_norm"

  if [ ! -f "$expected" ]; then
    echo "  [WARN] expected output not found, 初始化为当前归一化输出: $expected"
    cp "$actual_norm" "$expected"
    continue
  fi

  # 对比归一化后的输出
  filter_output <"$expected" >"$expected_norm"

  if diff -u "$expected_norm" "$actual_norm"; then
    echo "  [OK] $name"
  else
    echo "  [FAIL] $name"
    EXIT_CODE=1
  fi
done

echo ""
if [ "$EXIT_CODE" -eq 0 ]; then
  echo "== All regression tests passed =="
else
  echo "== Some regression tests FAILED =="
fi

exit "$EXIT_CODE"


