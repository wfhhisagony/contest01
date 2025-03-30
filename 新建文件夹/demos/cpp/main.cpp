#define _CRT_SECURE_NO_WARNINGS 1
#pragma GCC optimize(3,"Ofast","inline")    // 开启O3优化   听说OJ系统半夜提交代码速度会变快

#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>

using namespace std;

#define MAX_DISK_NUM (10 + 1)
#define MAX_DISK_SIZE (16384 + 1)
#define MAX_REQUEST_NUM (30000000 + 1)
#define MAX_OBJECT_NUM (100000 + 1)
#define REP_NUM (3)
#define FRE_PER_SLICING (1800)
#define EXTRA_TIME (105)

typedef struct Request_ {
    int object_id;
    int prev_id;
    bool is_done;
} Request;

typedef struct Object_ {
    int replica[REP_NUM + 1];
    int* unit[REP_NUM + 1];
    int size;
    int last_request_point;
    bool is_delete;
} Object;

// 磁头状态
struct DiskHead {
    int pos;                // 当前磁头位置（1~V）
    char last_action;       // 上一次动作类型（'J'/'P'/'R'）
    int last_token;         // 上一次动作消耗的令牌
    int remaining_tokens;   // 当前时间片剩余令牌（初始为G）
};


struct ReadRequest {
    int req_id, obj_id, start_time;
    vector<bool> blocks_done;       // 各块是否已读
    int readed_num;
    bool is_completed() const {
        return readed_num == blocks_done.size();
    }
};

Request request[MAX_REQUEST_NUM];
Object object[MAX_OBJECT_NUM];
unordered_map<int, ReadRequest> active_requests; // 活跃的读取请求
vector<vector<bool>> disk_bitmap;   // 存储单元占用状态（disk_bitmap[disk_id][unit]）
vector<int> disk_free_count;        // 每块硬盘的空闲单元数
vector<DiskHead> disk_heads;        // 磁头状态（disk_heads[disk_id]）

int T, M, N, V, G;
int disk[MAX_DISK_NUM][MAX_DISK_SIZE];
int disk_point[MAX_DISK_NUM];



// 初始化硬盘状态
void init_disks(int num_disks, int disk_size) {
    disk_bitmap.resize(num_disks + 1, vector<bool>(disk_size + 1, false));
    disk_free_count.resize(num_disks + 1, disk_size);
    disk_heads.resize(num_disks + 1, { 1, '#', 0, 0 });
}

// 分配存储单元（不要求连续）
vector<int> allocate_units(int disk_id, int size) {
    vector<int> units;
    for (int u = 1; u <= V && units.size() < size; ++u) {
        if (!disk_bitmap[disk_id][u]) {
            disk_bitmap[disk_id][u] = true;
            disk_free_count[disk_id]--;
            units.push_back(u);
        }
    }
    return units;
}

void timestamp_action()
{
    int timestamp;
    scanf("%*s%d", &timestamp);
    printf("TIMESTAMP %d\n", timestamp);

    fflush(stdout);
}

void do_object_delete(const int* object_unit, int* disk_unit, int size)
{
    for (int i = 1; i <= size; i++) {
        disk_unit[object_unit[i]] = 0;
    }
}

void delete_action()
{
    int n_delete;
    int abort_num = 0;
    static int _id[MAX_OBJECT_NUM];

    scanf("%d", &n_delete);
    for (int i = 1; i <= n_delete; i++) {
        scanf("%d", &_id[i]);
    }

    for (int i = 1; i <= n_delete; i++) {
        int id = _id[i];
        int current_id = object[id].last_request_point;
        while (current_id != 0) {
            if (request[current_id].is_done == false) {
                abort_num++;
            }
            current_id = request[current_id].prev_id;
        }
    }

    printf("%d\n", abort_num);
    for (int i = 1; i <= n_delete; i++) {
        int id = _id[i];
        int current_id = object[id].last_request_point;
        while (current_id != 0) {
            if (request[current_id].is_done == false) {
                printf("%d\n", current_id);
            }
            current_id = request[current_id].prev_id;
        }
        for (int j = 1; j <= REP_NUM; j++) {
            do_object_delete(object[id].unit[j], disk[object[id].replica[j]], object[id].size);
        }
        object[id].is_delete = true;
    }

    fflush(stdout);
}

void do_object_write(int* object_unit, int* disk_unit, int size, int object_id)
{
    int current_write_point = 0;
    for (int i = 1; i <= V; i++) {
        if (disk_unit[i] == 0) {
            disk_unit[i] = object_id;
            object_unit[++current_write_point] = i;
            if (current_write_point == size) {
                break;
            }
        }
    }

    assert(current_write_point == size);
}

void write_action()
{
    int n_write;
    scanf("%d", &n_write);
    for (int i = 1; i <= n_write; i++) {
        int id, size;
        scanf("%d%d%*d", &id, &size);
        object[id].last_request_point = 0;
        for (int j = 1; j <= REP_NUM; j++) {
            object[id].replica[j] = (id + j) % N + 1;
            object[id].unit[j] = static_cast<int*>(malloc(sizeof(int) * (size + 1)));
            object[id].size = size;
            object[id].is_delete = false;
            do_object_write(object[id].unit[j], disk[object[id].replica[j]], size, id);
        }

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

void read_action()
{
    int n_read;
    int request_id, object_id;
    scanf("%d", &n_read);
    for (int i = 1; i <= n_read; i++) {
        scanf("%d%d", &request_id, &object_id);
        request[request_id].object_id = object_id;
        request[request_id].prev_id = object[object_id].last_request_point;
        object[object_id].last_request_point = request_id;
        request[request_id].is_done = false;
    }

    static int current_request = 0;
    static int current_phase = 0;
    if (!current_request && n_read > 0) {
        current_request = request_id;
    }
    if (!current_request) {
        for (int i = 1; i <= N; i++) {
            printf("#\n");
        }
        printf("0\n");
    }
    else {
        current_phase++;
        object_id = request[current_request].object_id;
        for (int i = 1; i <= N; i++) {
            if (i == object[object_id].replica[1]) {
                if (current_phase % 2 == 1) {
                    printf("j %d\n", object[object_id].unit[1][current_phase / 2 + 1]);
                }
                else {
                    printf("r#\n");
                }
            }
            else {
                printf("#\n");
            }
        }

        if (current_phase == object[object_id].size * 2) {
            if (object[object_id].is_delete) {
                printf("0\n");
            }
            else {
                printf("1\n%d\n", current_request);
                request[current_request].is_done = true;
            }
            current_request = 0;
            current_phase = 0;
        }
        else {
            printf("0\n");
        }
    }

    fflush(stdout);
}

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

// 计算Read动作的令牌消耗
int calculate_read_cost(DiskHead& head) {
    if (head.last_action != 'R') return 64;
    return max(16, static_cast<int>(ceil(head.last_token * 0.8)));
}

int main()
{
    scanf("%d%d%d%d%d", &T, &M, &N, &V, &G);
    int y = 11;
    if(disk_free_count.size() > y){

    }
    y = -1;
    if(disk_free_count.size() > y){

    }
    for (int i = 1; i <= M; i++) {
        for (int j = 1; j <= (T - 1) / FRE_PER_SLICING + 1; j++) {
            scanf("%*d");
        }
    }

    for (int i = 1; i <= M; i++) {
        for (int j = 1; j <= (T - 1) / FRE_PER_SLICING + 1; j++) {
            scanf("%*d");
        }
    }

    for (int i = 1; i <= M; i++) {
        for (int j = 1; j <= (T - 1) / FRE_PER_SLICING + 1; j++) {
            scanf("%*d");
        }
    }

    printf("OK\n");
    fflush(stdout);

    for (int i = 1; i <= N; i++) {
        disk_point[i] = 1;
    }

    for (int t = 1; t <= T + EXTRA_TIME; t++) {
        timestamp_action();
        delete_action();
        write_action();
        read_action();
    }
    clean();

    return 0;
}