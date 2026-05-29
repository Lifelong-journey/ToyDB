#!/bin/bash

echo "=== ToyDB 演示脚本 ==="
echo ""

cd /root/toy_database/build/src

# 删除旧的数据库文件（可选，如果想从头开始）
# rm -f ../../toydb.db

# 运行演示脚本
cat ../../demo.sql | ./toy_db 2>&1 | grep -v "PageManager\|Insert:\|Search:\|Delete:\|MarkDirty\|BPlusTree\|FindLeaf\|Split\|Redistribute\|Merge\|RemoveEntry\|DEBUG\|InsertIntoParent"

echo ""
echo "=== 演示完成 ==="

