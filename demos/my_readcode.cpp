#pragma GCC optimize(3,"Ofast","inline")    // 开启O3优化   听说OJ系统半夜提交代码速度会变快

#include <cstdio>
#include <cassert>
#include <cstdlib>

#define MAX_DISK_NUM (10 + 1)
#define MAX_DISK_SIZE (16384 + 1)
#define MAX_REQUEST_NUM (30000000 + 1)
#define MAX_OBJECT_NUM (100000 + 1)
#define REP_NUM (3)
#define FRE_PER_SLICING (1800)  // 时间片
#define EXTRA_TIME (105)

typedef struct Request_ {
    int object_id;  // 当前访问请求Object的id
    int prev_id;    // 上一次访问请求的Object的id， prev_id = 0 表示没有上一次访问请求
    bool is_done;   // 请求处理完毕
} Request;

typedef struct Object_ {
    int replica[REP_NUM + 1];   // 0号不用 ， 副本  值表示该副本属于哪个硬盘
    int* unit[REP_NUM + 1]; // 0号不用  [3][size+1] 值表示各个副本在硬盘的那个块中，要和上面的replica一起用
    int size;   // 对象块的数目
    int last_request_point; // 上一次访问的Object的id
    bool is_delete; // 该对象是否被删除
} Object;

Request request[MAX_REQUEST_NUM];   // 所有的Request
Object object[MAX_OBJECT_NUM];      // 所有的Object

/*
T：代表本次数据有𝑇+105个时间片   1≤𝑇≤86400
M：代表对象标签数。对象标签编号为1 ~ 𝑀。输入数据保证1≤𝑀≤16。
N：代表存储系统中硬盘的个数，硬盘编号为1 ~ 𝑁。输入数据保证3≤𝑁≤10。
V：代表存储系统中每个硬盘的存储单元个数。存储单元编号为1 ~ 𝑉。输入数据保证1≤𝑉≤16384，任何时间存储系统中空余的存储单元数占总存储单元数的至少10%。
G：代表每个磁头每个时间片最多消耗的令牌数。输入数据保证64≤𝐺≤1000。
*/
int T, M, N, V, G;
int disk[MAX_DISK_NUM][MAX_DISK_SIZE];  // 0号单元不用
int disk_point[MAX_DISK_NUM];  // 0号单元不用

void timestamp_action() // 改函数可以不改动，因为仅仅是参赛方用于方便选手代码调试的
{
    int timestamp;
    scanf("%*s%d", &timestamp); // 读入当前时间戳

    printf("TIMESTAMP %d\n", timestamp);    // 再输出当前时间戳 (其实这仅仅是调试用)

    fflush(stdout);
}

// 这个感觉没啥好改的，就是把某个副本占用的块都给清除掉(清零)
void do_object_delete(const int* object_unit, int* disk_unit, int size)
{
    for (int i = 1; i <= size; i++) {
        disk_unit[object_unit[i]] = 0;  // 对disk[i]的【i=object[id].replica[j]】 [1...object[id].size] 清零(删除)
    }
}


void delete_action()
{
    int n_delete;
    int abort_num = 0;
    static int _id[MAX_OBJECT_NUM]; // 0号位不用, 使用[1...n_delete]   每个元素表示要删除的对象(object)编号

    scanf("%d", &n_delete); // 要删除的对象数
    for (int i = 1; i <= n_delete; i++) {
        scanf("%d", &_id[i]);   // 获取要删除的对象编号
    }

    // 第一行输出 n_abort 代表这一时间片被取消的读取请求的数量
    for (int i = 1; i <= n_delete; i++) {
        int id = _id[i];
        int current_id = object[id].last_request_point; // 最后请求该对象的请求id
        while (current_id != 0) {
            if (request[current_id].is_done == false) { // 未完成的读请求
                abort_num++;
            }
            current_id = request[current_id].prev_id;   // 继续往前找，更早之前还在请求该对象的请求id
        }
    }


    // 接下来n_abort行 每行一个数req_id[i]，代表被取消的读取请求编号（编号可以按任意顺序输出）
    printf("%d\n", abort_num);
    for (int i = 1; i <= n_delete; i++) {
        int id = _id[i];
        int current_id = object[id].last_request_point;
        // 优化：这里应该通知请求终止的，而不是在read_action()读完后才去看对象删除了没来判断是否请求是否有效
        // 优化：对象删除了就应该回收其空间了，而这个代码仅在最后才clean()清空内存
        while (current_id != 0) {  //  找到该对象影响的所有读取请求   这里和上面简直是一样的操作，重复了    优化：可以和上面的循环放在一起操作，不过就这样写，可能比较好写
            if (request[current_id].is_done == false) {
                printf("%d\n", current_id);
            }
            current_id = request[current_id].prev_id;   
        }
        for (int j = 1; j <= REP_NUM; j++) {    // 删除该对象的所有副本(3个)
            do_object_delete(object[id].unit[j], disk[object[id].replica[j]], object[id].size);
        }
        object[id].is_delete = true;
    }

    fflush(stdout);
}

// 缺陷： 没有先判断该硬盘是否有足够空间，没有对应的处理（ 不过这个可以在上一步的时候处理，以保证这个时候一定有足量的空间）
// 缺陷:  函数按顺序从存储单元1开始寻找空闲位置，可能导致存储碎片化，值得进行一下优化
void do_object_write(int* object_unit, int* disk_unit, int size, int object_id)
{
    int current_write_point = 0;
    for (int i = 1; i <= V; i++) {
        if (disk_unit[i] == 0) {    // 没有被写的才能写入 =0表示没有被写是空闲的
            disk_unit[i] = object_id;
            object_unit[++current_write_point] = i;
            if (current_write_point == size) {
                break;
            }
        }
    }

    assert(current_write_point == size);    // 需要确保有足够的硬盘空间
}

// 缺陷： 没有先判断硬盘是否有足够空间来存下该副本
void write_action()
{
    int n_write;
    scanf("%d", &n_write);
    for (int i = 1; i <= n_write; i++) {
        int id, size;
        scanf("%d%d%*d", &id, &size);   // 写忽略了标签，没有使用到标签进行优化（标签相同的往往读和删操作的时间相近）
        object[id].last_request_point = 0;
        for (int j = 1; j <= REP_NUM; j++) {
            object[id].replica[j] = (id + j) % N + 1;   // 没有先判断这块硬盘是否有足够空间来存下该副本
            object[id].unit[j] = static_cast<int*>(malloc(sizeof(int) * (size + 1)));   // 使用`malloc`分配`unit`数组但未检查内存不足，可能引发崩溃
            object[id].size = size; // 这尼玛可以放循环外面的
            object[id].is_delete = false;
            do_object_write(object[id].unit[j], disk[object[id].replica[j]], size, id);
        }
        // 打印一下写入结果
        printf("%d\n", id);
        for (int j = 1; j <= REP_NUM; j++) {
            printf("%d", object[id].replica[j]);
            for (int k = 1; k <= size; k++) {
                printf(" %d", object[id].unit[j][k]);
            }
            printf("\n");
        }
    }

    fflush(stdout);
}

/**
 缺陷:
 磁头调度逻辑过于简单，仅处理第一个副本（object[object_id].replica[1]），且采用固定的交替Jump和Read策略（current_phase % 2）。这会导致：
- 无法跨副本读取不同块，从而无法优化磁头移动路径。
- 未考虑令牌消耗的余量（如每个时间片最多G令牌），它每个时间片只操作一次，完全的浪费了很多token
- 未维护磁头的当前位置，导致每次Jump都基于初始位置（存储单元1），与实际情况不符（磁头位置应随动作动态变化）。
- 读取请求的处理仅处理了最后一个请求（`current_request`），未处理多个并发请求，导致无法高效并行处理多个读取请求。
优化：
  读取同一对象的多个请求可以共享已读块
 */
void read_action()
{
    int n_read;
    int request_id, object_id;
    scanf("%d", &n_read);   // 获取读取请求的数目
    for (int i = 1; i <= n_read; i++) {
        scanf("%d%d", &request_id, &object_id);  // 请求id以及对象id
        request[request_id].object_id = object_id;
        request[request_id].prev_id = object[object_id].last_request_point; // 上一次访问这个object的request_id
        object[object_id].last_request_point = request_id;  // 更新为这次访问这个object的request_id，但是这次也有可能有多个请求访问该object吧
        request[request_id].is_done = false;    // 还没有完成
    }
    // 缺陷：这里只处理了最后一个请求，其它请求都不管了
    // 缺陷：这里只有当没有读请求时，才能处理完之前的最后一个请求
    static int current_request = 0;
    static int current_phase = 0;
    if (!current_request && n_read > 0) {
        current_request = request_id;
    }
    if (!current_request) { // 如果没有请求，则每个磁盘都不操作
        for (int i = 1; i <= N; i++) {
            printf("#\n");
        }
        printf("0\n");  // 这个时间片完成的请求数目 为 0
    } else {
        current_phase++;
        object_id = request[current_request].object_id; // 获取这个请求请求的object_id
        for (int i = 1; i <= N; i++) {
            if (i == object[object_id].replica[1]) {    // 如果这个磁盘是这个请求的object_id的副本1
                // 缺陷： 这个跳转真的非常的浪费，相当于object的每一个对象块都要跳一次
                if (current_phase % 2 == 1) {   // 如果current_phase是奇数，则跳到磁盘存储的object的第current_phase/2+1块对象块的磁盘块位置   
                    printf("j %d\n", object[object_id].unit[1][current_phase / 2 + 1]);
                } else {
                    // 优化： 这里显然可以优化，不可能说读完就不操作了，还可以移动磁头呢
                    printf("r#\n"); // 如果current_phase是偶数，则读取当前位置的一个对象块，并直接结束操作。 
                }
            } else {
                printf("#\n");  // 这个硬盘不操作
            }
        }

        if (current_phase == object[object_id].size * 2) {  // 这一块都读完了
            if (object[object_id].is_delete) {  // 但发现这块已经被删除了，白读了，返回成功读了0个块
                printf("0\n");
            } else {
                printf("1\n%d\n", current_request);
                request[current_request].is_done = true;
            }
            // 表示这个请求处理完了
            current_request = 0;
            current_phase = 0;
        } else {
            printf("0\n");
        }
    }

    fflush(stdout);
}

// 清空内存占用
void clean()
{
    for (auto& obj : object) {
        for (int i = 1; i <= REP_NUM; i++) {
            if (obj.unit[i] == nullptr)
                continue;
            free(obj.unit[i]);
            obj.unit[i] = nullptr;
        }
    }
}

int main()
{
    scanf("%d%d%d%d%d", &T, &M, &N, &V, &G);
    // 删除操作中 各个标签 的 对象块总数
    for (int i = 1; i <= M; i++) {
        for (int j = 1; j <= (T - 1) / FRE_PER_SLICING + 1; j++) {
            scanf("%*d");   // 跳过输入
        }
    }

    // 写入操作中  各个标签的 对象块总数
    for (int i = 1; i <= M; i++) {
        for (int j = 1; j <= (T - 1) / FRE_PER_SLICING + 1; j++) {
            scanf("%*d");
        }
    }

    // 读取操作中  各个标签的 对象块总数
    for (int i = 1; i <= M; i++) {
        for (int j = 1; j <= (T - 1) / FRE_PER_SLICING + 1; j++) {
            scanf("%*d");
        }
    }
    // ... 用户的全局预处理操作  根据上面的信息合理分配硬盘的使用方式
    // 缺陷：全局预处理阶段完全忽略了输入的标签统计信息  未在写入时利用标签信息优化存储位置，导致无法降低碎片化，直接影响得分
    printf("OK\n");
    fflush(stdout);

    for (int i = 1; i <= N; i++) {  // 对所有硬盘执行操作
        disk_point[i] = 1;
    }

    for (int t = 1; t <= T + EXTRA_TIME; t++) { // 每个时间片都执行一次：时间片对齐事件，对象删除事件，对象写入事件，对象读取事件
        timestamp_action();
        delete_action();
        write_action();
        read_action();
    }
    clean();    // 释放内存

    return 0;
}