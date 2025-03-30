#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
# @Time    : 2025/3/29 19:01
# @Author  : lqh
# @python-version 3.10
# @File    : my_test.py
# @Software: PyCharm
"""
class TrieNode:
    def __init__(self):
        self.children = {}
        self.is_end_of_word = False  # 标记是否是一个完整单词的结尾


class Trie:
    def __init__(self):
        self.root = TrieNode()  # 初始化根节点

    def insert(self, word: str) -> None:
        """
        插入单词到字典树中
        """
        node = self.root
        for char in word:
            if char not in node.children:
                node.children[char] = TrieNode()  # 创建新的节点
            node = node.children[char]
        node.is_end_of_word = True  # 标记单词的结尾

    def search(self, word: str) -> bool:
        """
        查找字典树中是否存在该单词
        """
        node = self.root
        for char in word:
            if char not in node.children:
                return False  # 如果某个字符不存在，返回False
            node = node.children[char]
        return node.is_end_of_word  # 如果最后一个字符为单词结尾，返回True

    def startsWith(self, prefix: str) -> bool:
        """
        查找字典树中是否有以给定前缀开头的单词
        """
        node = self.root
        for char in prefix:
            if char not in node.children:
                return False  # 如果某个字符不存在，返回False
            node = node.children[char]
        return True  # 如果遍历完前缀中的字符，说明存在以此为前缀的单词

    def delete(self, word: str) -> None:
        """
        从字典树中删除指定单词
        """

        def _delete(node, word, index):
            if index == len(word):
                if not node.is_end_of_word:
                    return False  # 单词不存在
                node.is_end_of_word = False  # 删除单词结尾标记
                return len(node.children) == 0  # 如果该节点没有子节点，则可以删除此节点
            char = word[index]
            if char not in node.children:
                return False  # 单词不存在
            can_delete_child = _delete(node.children[char], word, index + 1)
            if can_delete_child:
                del node.children[char]  # 删除子节点
                return len(node.children) == 0  # 如果没有其他子节点，当前节点可以被删除
            return False

        _delete(self.root, word, 0)

