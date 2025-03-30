### 文件清单
```
.
├── readme.md -------------------------- 说明文件
├── c ---------------------------------- C语言工程，CMakeLists.txt文件必选，内容部分可自定义
|   ├── CMakeLists.txt
|   └── main.c
├── c++ -------------------------------- C++语言工程，CMakeLists.txt文件必选，内容部分可自定义
|   ├── CMakeLists.txt
|   └── main.cpp
├── java ------------------------------- Java语言工程，main.java文件必选
|   └── main.java
└── python ----------------------------- Python语言工程，main.py文件必选
    └── main.py
```

### 本地编译与调试
```
C、C++的编译：
进入工程目录，运行
cmake .
make

Java的编译：
javac main.java

```



可以在Windows上使用microsoft visual c++ 进行c和cpp代码的编译，生成exe文件，例如:

1）首先打开 Developer PowerShell for VS 2022命令行，进入到main.cpp文件的目录下，输入如下代码进行编译

```
cl /EHsc main.cpp
```

生成一个main.exe文件



然后可以用python使用main.exe作为参数运行 run.py文件

PS:在Pycharm中可以通过Run->Edit Configurations直接设置运行时的命令行参数





