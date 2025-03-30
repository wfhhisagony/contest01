#define _CRT_SECURE_NO_WARNINGS 1
//#pragma GCC optimize(3,"Ofast","inline")    // 开启O3优化   听说OJ系统半夜提交代码速度会变快


#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cassert>
#include <string>
#include <cmath>

using namespace std;

#define MAX_DISK_NUM (11)      // 硬盘编号1~10
#define MAX_TAG (17)           // 标签编号1~16
#define REPLICA_NUM (3)        // 每个对象的副本数
#define MAX_DISK_SIZE (16385)  // 存储单元编号1~16384
#define FRE_PER_SLICING (1800)  // 时间片
#define EXTRA_TIME (105)

// 磁头状态
struct DiskHead {
    int pos;                // 当前磁头位置（1~V）
    char last_action;       // 上一次动作类型（'J'/'P'/'R'）
    int last_token;         // 上一次动作消耗的令牌
    int remaining_tokens;   // 当前时间片剩余令牌（初始为G）
};

// 对象元数据
struct Object {
    int id, size, tag;
    vector<int> disks;              // 副本所在硬盘
    vector<vector<int>> units;      // 每个副本的块位置  units[disk_id-1][block_id-1] = units[0...2][0...size-1] ==> block_id  id都是从1开始的
    unordered_set<int> active_requests;    // 未完成的读取请求
    bool is_deleted;                // 是否已被删除
};

// 读取请求状态
struct ReadRequest {
    int req_id, obj_id, start_time;
    vector<bool> blocks_done;       // 各块是否已读
    int readed_num;
    bool is_completed() const {
        return readed_num == blocks_done.size();
    }
    // 新增方法：计算剩余有效时间窗口
    int get_remaining_time(int current_time) const {
        return 105 - (current_time - start_time);
    }
    bool is_expired(int current_time) const {
        return current_time - start_time > 105;
    }
};

vector<DiskHead> disk_heads;        // 磁头状态（disk_heads[disk_id]）
unordered_map<int, Object> objects; // 所有对象（id到对象）
unordered_map<int, ReadRequest> active_requests; // 活跃的读取请求
unordered_map<int, ReadRequest> sleep_requests; // 超过105个时间片没有做完，则丢入这个队列，不再处理
vector<vector<bool>> disk_bitmap;   // 存储单元占用状态（disk_bitmap[disk_id][unit]）
vector<int> disk_free_count;        // 每块硬盘的空闲单元数
unordered_map<int, int> read_cost_map = {
    {64, 52}, {52,42}, {42,34},{34,28}, {28,23},{23,19},{19,16},{16,16}
};

int T, M, N, V, G;                        // 硬盘数、每盘单元数、每时间片令牌数

// 初始化硬盘状态
void init_disks(int num_disks, int disk_size) {
    disk_bitmap.resize(num_disks + 1, vector<bool>(disk_size + 1, false));
    disk_free_count.resize(num_disks + 1, disk_size);
    disk_heads.resize(num_disks + 1, { 1, '#', 0, 0 });
}

// 为标签选择候选硬盘组（示例：简单轮询分配）
vector<int> get_candidate_disks(int tag) {
    vector<int> candidates;
    for (int i = 0; i < REPLICA_NUM; ++i) {
        candidates.push_back((tag + i) % N + 1);
    }
    return candidates;
}

// 选择三块不同硬盘（优先标签候选组）
vector<int> select_disks(int tag, int size) {
    unordered_set<int> selected;
    vector<int> candidates = get_candidate_disks(tag);  // 先按照%N策略找3个

    // 优先从候选组选择
    for (int disk_id : candidates) {
        if (disk_free_count[disk_id] >= size) {
            selected.insert(disk_id);
        }
    }

    // 不足则全局补充
    if (selected.size() < REPLICA_NUM) {
        for (int disk_id = 1; disk_id <= N; ++disk_id) {    
            if (selected.count(disk_id) || disk_free_count[disk_id] < size) continue;
            selected.insert(disk_id);
            if (selected.size() == REPLICA_NUM) break;
        }
    }

    return vector<int>(selected.begin(), selected.end());
}

// 分配存储单元（不要求连续）
vector<int> allocate_units(int disk_id, int size) {
    vector<int> units;
    for (int u = 1; u <= V && units.size() < size; ++u) {   // 这可能有点慢，特别是V很大的时候
        if (!disk_bitmap[disk_id][u]) {
            disk_bitmap[disk_id][u] = true;
            disk_free_count[disk_id]--;
            units.push_back(u);
        }
    }
    return units;
}

// 处理写入事件
void handle_write_events() {
    int n_write;
    scanf("%d", &n_write);

    for (int i = 0; i < n_write; ++i) {
        int obj_id, size, tag;
        scanf("%d%d%d", &obj_id, &size, &tag);

        // 选择三块(REPLICA_NUM块)不同硬盘
        vector<int> disk_ids = select_disks(tag, size); 
        assert(disk_ids.size() == REPLICA_NUM);

        // 分配存储单元
        Object obj;
        obj.id = obj_id;
        obj.size = size;
        obj.tag = tag;
        obj.is_deleted = false;
        for (int disk_id : disk_ids) {
            vector<int> units = allocate_units(disk_id, size);
            obj.disks.push_back(disk_id);   // 各副本对应的磁盘
            obj.units.push_back(units);     // 各副本块对应的磁盘块
        }

        // 存储对象元数据
        objects[obj_id] = obj;  

        // 输出副本信息
        printf("%d\n", obj_id);
        for (int j = 0; j < REPLICA_NUM; ++j) {
            printf("%d", obj.disks[j]); // 副本所在磁盘
            for (int u : obj.units[j]) printf(" %d", u);    // 副本块所在磁盘块
            printf("\n");
        }
    }

    fflush(stdout);
}

// 处理删除事件
void handle_delete_events() {
    int n_delete;
    scanf("%d", &n_delete);
    vector<int> del_ids(n_delete);
    for (int& id : del_ids) scanf("%d", &id); // 每个元素表示要删除的对象(object)编号

    int abort_num = 0;  // 因删除导致的取消的请求的总数
    for (int obj_id : del_ids) {
        Object& obj = objects[obj_id];
        obj.is_deleted = true;

        // 释放存储单元
        for (int i = 0; i < REPLICA_NUM; ++i) {
            int disk_id = obj.disks[i];
            for (int u : obj.units[i]) {
                disk_bitmap[disk_id][u] = false;    // 这块空闲
                disk_free_count[disk_id]++; // 这块disk的空闲块增多
            }
        }
        abort_num += obj.active_requests.size();
        
    }

    printf("%d\n", abort_num); // 输出取消的请求数（简化为n_delete，需实际计算）
    for (int obj_id : del_ids) {
        Object& obj = objects[obj_id];
        // 取消关联的请求
        for (int req_id : obj.active_requests) {
            printf("%d\n", req_id);
            active_requests.erase(req_id);
        }
        objects.erase(obj_id);  // 删除该对象
    }
    fflush(stdout);
}

// 计算Read动作的令牌消耗
int calculate_read_cost(DiskHead& head) {
    if (head.last_action != 'R') return 64;
    return read_cost_map[head.last_token];
    //return max(16, static_cast<int>(ceil(head.last_token * 0.8)));
}

// 生成磁头动作序列
string generate_actions(int disk_id, const vector<int>& target_blocks, vector<int> &read_blocks) {
    DiskHead& head = disk_heads[disk_id];
    string actions;
    

    for (int block : target_blocks) {
        if (head.remaining_tokens <= 0) break;
        int steps = (block - head.pos + V) % V;
        //if (steps == 0) steps = V; // 环形处理

        // 情况1：使用Pass+Read
        if (steps <= head.remaining_tokens) {
            // 添加Pass动作
            actions.append(string(steps, 'p'));
            head.remaining_tokens -= steps;
            head.pos = (head.pos + steps) % V;
            head.last_action = 'P';

           /* for (int i = 0; i < steps; ++i) {
                actions += 'p';
                head.remaining_tokens--;
                head.pos = (head.pos % V) + 1;
                head.last_action = 'P';
                head.last_token = 1;
            }*/
            // 添加Read动作
            int cost = calculate_read_cost(head);
            if (head.remaining_tokens >= cost) {
                actions += 'r';
                head.remaining_tokens -= cost;
                head.last_action = 'R';
                head.last_token = cost;
                read_blocks.push_back(block);
                head.pos = (head.pos % V) + 1;
            }
            else break;
        }
        // 情况2：使用Jump
        else if (head.remaining_tokens >= G) {
            actions = "j " + to_string(block);
            head.remaining_tokens = 0;
            head.pos = block;
            head.last_action = 'J';
            head.last_token = G;
            break;
        }
        // 情况3：
        else {  // 剩几个token走几步
            actions.append(string(head.remaining_tokens, 'p'));
            head.pos = (head.pos + head.remaining_tokens) % V;
            head.last_action = 'P';
            head.remaining_tokens = 0;
            //for (int i = 0; i < head.remaining_tokens; ++i) {
            //    actions += 'p';
            //    head.pos = (head.pos % V) + 1;
            //    head.last_action = 'P';
            //    head.last_token = 1;
            //}
            //head.remaining_tokens = 0;
        }
    }

    //actions += '#';
    return actions;
}

// 处理读取请求调度
void handle_read_requests(int current_time) {
    vector<string> disk_actions(N + 1); // 每个磁盘的动作
    vector<int> completed_reqs;

    int process_request_num = 500;
    vector<int> to_delete_reqId;
    // 处理每个请求
    for (auto &it : active_requests) {  // 多个请求卡在这造成很长的时间浪费
        ReadRequest& req = it.second;
        if (req.is_expired(current_time)) {
            to_delete_reqId.push_back(req.req_id);
            continue;
        }
        if ((process_request_num--) == 0)break;
        
        Object& obj = objects[req.obj_id];

        // 对副本按块集中度排序（示例：按块位置方差）
        vector<int> replica_order = { 0, 1, 2 };
        //sort(replica_order.begin(), replica_order.end(), [&obj](int a, int b) {
        //    double var_a = 0, var_b = 0;
        //    double mean_a = 0, mean_b = 0;
        //    for (int u : obj.units[a]) mean_a += u;
        //    for (int u : obj.units[b]) mean_b += u;
        //    mean_a /= obj.size; mean_b /= obj.size;
        //    for (int u : obj.units[a]) var_a += pow(u - mean_a, 2);
        //    for (int u : obj.units[b]) var_b += pow(u - mean_b, 2);
        //    return var_a < var_b;
        //    });

        // 优先处理集中度高的副本
        for (int rep : replica_order) {
            int disk_id = obj.disks[rep];
            DiskHead& head = disk_heads[disk_id];
            if (head.remaining_tokens <= 0) continue;

            // 获取该副本未读的块
            vector<int> target_blocks;
            for (int i = 0; i < obj.size; ++i) {
                if (!req.blocks_done[i]) target_blocks.push_back(obj.units[rep][i]);
            }
            vector<int> read_blocks;
            // 生成动作并获取实际读取的块
            string action = generate_actions(disk_id, target_blocks, read_blocks);
            //if (!disk_actions[disk_id].empty()) disk_actions[disk_id] += " ";
            disk_actions[disk_id] += action;

            // 标记已读块
            for (int block : read_blocks) {
                for (int i = 0; i < obj.size; ++i) {    // 最大5，复杂度不高
                    if (obj.units[rep][i] == block) {
                        req.blocks_done[i] = true;
                        req.readed_num++;
                        break;
                    }
                }
            }

            if (req.is_completed()) {
                obj.active_requests.erase(req.req_id);
                completed_reqs.push_back(req.req_id);
                break;
            }
        }

        /*if (req.is_completed()) {

            
        }*/
    }

    // 输出磁头动作（按磁盘编号1~N）
    for (int i = 1; i <= N; ++i) {
        if (disk_actions[i].empty()) printf("#\n");
        else if(disk_actions[i][0] == 'j') printf("%s\n", disk_actions[i].c_str());
        else printf("%s#\n", disk_actions[i].c_str());
    }

    // 输出完成的请求
    printf("%zu\n", completed_reqs.size());
    for (int req_id : completed_reqs) {
        printf("%d\n", req_id);
        active_requests.erase(req_id);
    }

    // 删除等待超过105时间片的请求
    for (int req_id : to_delete_reqId) {
        active_requests.erase(req_id);
    }

    fflush(stdout);
}



// 处理读取事件
void handle_read_events(int current_time) {
    int n_read;
    scanf("%d", &n_read);

    // 题目保证输入的obj_id有效，无需检查
    for (int i = 0; i < n_read; ++i) {
        int req_id, obj_id;
        scanf("%d%d", &req_id, &obj_id);

        ReadRequest req;
        req.req_id = req_id;
        req.obj_id = obj_id;
        req.start_time = current_time;
        req.blocks_done.resize(objects[obj_id].size, false);
        req.readed_num = 0;
        active_requests[req_id] = req;  // 添加到活跃事件队列（HashMap）

        // 关联到对象
        objects[obj_id].active_requests.emplace(req_id);
    }

    // 输出无请求取消（题目保证输入有效）
    handle_read_requests(current_time);

}

int main() {
    //int T, M;
    scanf("%d%d%d%d%d", &T, &M, &N, &V, &G);

    // 初始化硬盘
    init_disks(N, V);

    // 全局预处理（示例：跳过输入）
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

    printf("OK\n");
    fflush(stdout);

    // 处理每个时间片
    for (int t = 1; t <= T + EXTRA_TIME; ++t) {
        // 时间片对齐事件
        int timestamp;
        scanf("%*s%d", &timestamp);
        printf("TIMESTAMP %d\n", timestamp);
        fflush(stdout);
        // 重置磁头令牌
        for (int i = 1; i <= N; ++i) disk_heads[i].remaining_tokens = G;
        // if (t <= T) {
            // 处理删除事件
        handle_delete_events();
        // 处理写入事件
        handle_write_events();
        // 处理读取事件
        handle_read_events(t);
        // } else {
            // 后105时间片无新事件
            // printf("0\n"); // 无删除
            // printf("0\n"); // 无写入
            // printf("0\n"); // 无新读取请求
            // fflush(stdout);
        // }
    }

    return 0;
}