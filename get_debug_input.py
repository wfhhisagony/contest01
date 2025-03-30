#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
# @Time    : 2025/3/28 19:41
# @Author  : lqh
# @python-version 3.10
# @File    : get_debug_input.py
# @Software: PyCharm
"""


def extract_interactor_content(file_path):
    interactor_contents = []
    capture = False

    with open(file_path, 'r') as file:
        for line in file:
            line = line.strip()
            if line == '[interactor]':
                capture = True
                current_content = []
            elif line.startswith('[') and line.endswith(']') and line != '[interactor]':
                if capture:
                    interactor_contents.append('\n'.join(current_content))
                    current_content = []
                capture = False
            elif capture:
                current_content.append(line)

    # 添加最后一个[interactor]的内容（如果有）
    if capture and current_content:
        interactor_contents.append('\n'.join(current_content))

    return interactor_contents


# 使用示例
file_path = 'debug.txt'
interactor_contents = extract_interactor_content(file_path)

# 将提取的内容保存到文件
with open('interactor_contents.txt', 'w') as output_file:
    for i, content in enumerate(interactor_contents, 1):
        output_file.write(content + '\n')