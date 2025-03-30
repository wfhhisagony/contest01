#define _CRT_SECURE_NO_WARNINGS 1


#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <set>
#include <algorithm>
#include <cstring> // for memset

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

    DiskHead() {
        memset(this, 0, sizeof(DiskHead));
    }
};


// 对象元数据
struct Object {
    int id, size, tag;
    vector<int> disks;      // 副本所在硬盘 这里存虚拟盘号就行了,DiskGroup根据obj_set查找这个obj是否存在这个DiskGroup上即可
    int readed_blocks_num;  // 当前时间片下被读取的块数
    int readed_units[5];      // 每个对象最多5个对象块，表示每个对象块在当前时间片下被读取的次数，使用int数组，支持memset快速清零
    int units[5];   // 每个对象最多5个对象块，表示每个对象块在虚拟盘块上的位置(如果是DiskGroupThree，只用在第0个物理盘上的位置)
    unordered_set<int> requests;    // 读取该对象的请求req_id
    bool is_deleted;                // 对象是否已被删除
    Object() : id(0), size(0), tag(0), readed_blocks_num(0), is_deleted(false), disks(3) {
        memset(readed_units, 0, sizeof(readed_units));
        memset(units, 0, sizeof(units));
    }
};

unordered_map<int, Object> Objects; // 所有对象（obj_id:obj）

// 读取请求状态
class ReadRequest {
public:
    int req_id, obj_id, start_time;
    int active_queue_offset;    // 用于ActiveRequests出队列
    int block_num;
    vector<bool> blocks_done;   // 各块是否已读  obj的unit_index是否已读

    ReadRequest() : req_id(0), obj_id(0), start_time(0), active_queue_offset(0), block_num(0) {
        blocks_done.reserve(5);
    }

    ReadRequest(int req_id, int obj_id, int start_time) :
        req_id(req_id), obj_id(obj_id), start_time(start_time), active_queue_offset(0) {
        this->block_num = Objects[obj_id].size;
        blocks_done.resize(this->block_num, false);
    }

    bool is_completed() const {
        for (int i = 0; i < block_num; i++) {
            if (!blocks_done[i]) return false;
        }
        return true;
    }

    bool is_expired(int current_time) const {
        return current_time - start_time > 105;
    }
};


class ActiveRequests {
private:
    int current_offset = 0;  // 当前时间偏移
    unordered_map<int, list<ReadRequest>::iterator> id_to_node;  // req_id到链表节点的映射
    int max_que_size; // 105 * 450=47250  一个时间片至多处理450个请求( 1000 * 10 /16 > 450 > 1000 * 10 /16/1.5)

public:
    // unordered_map<int, vector<TargetBlock>> req_targetblock;
    vector<ReadRequest> current_overdue_requests;   // 因为push操作过期的请求，用于磁盘组删除对应的target_blocks
    list<ReadRequest> queue;  // 按start_time递增排列的双向链表，队列前是即将删除的，队列后是最新加入的

    explicit ActiveRequests(int max_que_size) : max_que_size(max_que_size) {}

    void set_max_que_size(int size) {   // 看看磁盘组至多处理多少个请求
        this->max_que_size = size;
    }


    // 删除指定req_id的请求，O(1)
    list<ReadRequest>::iterator erase(int req_id) {
        auto it = id_to_node.find(req_id);
        list<ReadRequest>::iterator res_it;
        if (it != id_to_node.end()) {
            res_it = queue.erase(it->second);
            id_to_node.erase(it);
        }
        return res_it;
    }

    bool contains(int req_id) const {
        return id_to_node.count(req_id) > 0;
    }

    // 通过req_id访问节点
    ReadRequest* get(int req_id) {
        auto it = id_to_node.find(req_id);
        if (it != id_to_node.end()) {
            return &(*it->second); // 返回节点指针
        }
        return nullptr; // 不存在时返回空指针
    }

    // 插入新请求，原队列元素剩余时间减1，移除过期的，O(reqs.size() + k)
    void push(vector<ReadRequest>& reqs) {
        current_overdue_requests.clear();
        // 更新时间偏移，相当于所有现有请求的剩余时间减1
        current_offset++;
        const int threshold = current_offset - 105;

        // 移除过期的请求（active_queue_offset <= threshold）
        while (!queue.empty() && queue.front().active_queue_offset <= threshold) {
            current_overdue_requests.push_back(queue.front());
            id_to_node.erase(queue.front().req_id);
            queue.pop_front();
        }

        // 插入新请求到队列尾部
        for (auto& req : reqs) {
            if (queue.size() >= this->max_que_size) {
                current_overdue_requests.push_back(queue.front());
                pop_oldest();
            }
            // 新请求的birth_offset为当前时间偏移

            req.active_queue_offset = current_offset;
            queue.push_back(req);
            id_to_node[req.req_id] = std::prev(queue.end());
        }
    }

    // 弹出队头请求(删除过期的请求)，O(1)
    void pop_oldest() {
        if (!queue.empty()) {
            id_to_node.erase(queue.front().req_id);
            queue.pop_front();
        }
    }

    // 辅助方法：获取当前队列大小（测试用）
    size_t size() const { return queue.size(); }
};



// 计算Read动作的令牌消耗
int calculate_read_cost(int last_token, char last_action) {
    static unordered_map<int, int> read_cost_map = {
        {64, 52}, {52,42}, {42,34},{34,28}, {28,23},{23,19},{19,16},{16,16}
    };
    if (last_action != 'R') return 64;
    return read_cost_map[last_token];
    //return max(16, static_cast<int>(ceil(head.last_token * 0.8)));
}


class DiskGroup {
public:
    enum class Type { THREE, FOUR, FIVE };
    virtual Type get_type() = 0;
    int V, G; // 每个硬盘的存储单元数、每个时间片的令牌数
    int start_disk_id;  // 起始硬盘编号
    vector<int> disk_id_list;
    unordered_set<int> obj_set;	// DiskGroup中已存储的obj对象
    vector<vector<pair<int, int>>> disk_units;   // 存储单元占用状态（disk_unit[disk_id][unit] = <obj_id, obj_unit_index>） 当未占用时obj_id=0

    vector<DiskHead> disk_heads;        // 磁头状态（disk_heads[disk_id]）
    // vector<ActiveRequests> active_requests_list;	// 每个磁盘组有自己的ActiveRequests  每个磁盘最大的ActiveRequests设置5500 = 1000 * 105 / 16 / 1.2
    ActiveRequests active_requests;

public:
    DiskGroup(int V, int G, int start_disk_id, int request_max_size, const vector<int>& disk_ids) :
        V(V), G(G), start_disk_id(start_disk_id), disk_id_list(disk_ids), active_requests(request_max_size) {

        disk_units.resize(disk_ids.size(), vector<pair<int, int>>(V, { 0, 0 }));
        disk_heads.resize(disk_ids.size());
    }

    virtual ~DiskGroup() = default;

    // 连续读取直到令牌不足或到达目标位置
    int continuouslyRead(DiskHead& head, int end) {    // 读的都是无效块
        int read_cost = calculate_read_cost(head.last_token, 'R');
        int move_steps = 0;
        int true_end = (end + 1) % V;
        while (head.pos != true_end && head.remaining_tokens >= read_cost) {
            head.remaining_tokens -= read_cost;
            head.pos = (head.pos + 1) % V;
            move_steps += 1;
            head.last_token = read_cost;
            read_cost = calculate_read_cost(read_cost, 'R');
        }
        return move_steps;
    }

    virtual vector<char> minMaxReadStrategy(int disk_index) = 0;
    virtual bool add_object(Object& obj) = 0;   // 写入
    virtual void add_requests(vector<ReadRequest>& reqs) = 0;   // 读取
    virtual void delete_object(Object& obj) = 0;  // 删除

    vector<int> process_readRequests() {
        // 重置对象读取状态
        for (int obj_id : obj_set) {
            Object& obj = Objects[obj_id];
            obj.readed_blocks_num = 0;
            memset(obj.readed_units, 0, sizeof(obj.readed_units));
        }

        for (int disk_id : disk_id_list) {
            // 重置磁盘token
            DiskHead& disk_head = disk_heads[disk_id - start_disk_id];
            disk_head.remaining_tokens = G;

            vector<char> action = minMaxReadStrategy(disk_id - start_disk_id);
            for (int i = 0; i < action.size(); i++) {
                printf("%c", action[i]);
            }
            printf("\n");
        }


        // 检查已完成的请求
        vector<int> completed_requests;
        auto it = active_requests.queue.begin();
        while (it != active_requests.queue.end()) {
            ReadRequest& req = *it;
            Object& obj = Objects[req.obj_id];
            if (obj.readed_blocks_num == 0) {
                ++it;
                continue;
            }
            for (int i = 0; i < req.block_num; i++) {
                if (req.blocks_done[i] == false && obj.readed_units[i] > 0) {
                    obj.readed_units[i]--;
                    obj.readed_blocks_num--;
                    req.blocks_done[i] = true;
                }
            }
            if (req.is_completed()) {
                completed_requests.push_back(req.req_id);
                obj.requests.erase(req.req_id);
                it = active_requests.erase(req.req_id);
            }
            else {
                ++it;
            }
        }
        return completed_requests;
    }



};


class DiskGroupThree : public DiskGroup {	// 由三个盘组成的DiskGroup，由于每个对象三个副本，则恰好可存在这三个盘上完全相同的磁盘块位置，这样只要知道一个副本的磁盘块位置，就可推测出其他副本的磁盘块位置
public:
    // 1000 * 105 / 16 / 1.5 * 3 = 13125  1000 * 105 / 16 / 3 * 3 = 6562
    multiset<int> target_blocks;	// 需要读取的磁盘块位置
    Type get_type() override { return Type::THREE; }
    int disks_free_count;        // 硬盘组的空闲单元数


public:
    DiskGroupThree(int V, int G, int start_disk_id, int request_max_size, const vector<int>& disk_ids) :
        DiskGroup(V, G, start_disk_id, request_max_size, disk_ids), disks_free_count(V) {}



    // 添加对象到虚拟盘组
    /*
    设置对象的disk[]、units[]
    分配硬盘组的空间
    */
    bool add_object(Object& obj) {
        // 先选择tag聚集点的虚拟盘组
        if (disks_free_count < obj.size) return false;

        // 在物理盘上分配空间  
        for (int rep = 0; rep < 3; ++rep) {
            obj.disks[rep] = rep;
        }
        for (int k = 0, i = 0; i < obj.size; ++k) {
            if (disk_units[0][k].first == 0) {
                obj.units[i] = k;
                for (int rep = 0; rep < 3; ++rep) { // 三块副本存的位置是相同的
                    disk_units[rep][k].first = obj.id;
                    disk_units[rep][k].second = i;
                }
                i++;
            }
        }

        disks_free_count -= obj.size;
        obj_set.insert(obj.id);
        return true;
    }


    // 为读取请求添加目标块
    void add_requests(vector<ReadRequest>& reqs) override {
        active_requests.push(reqs);
        for (ReadRequest& req : reqs) {
            Object& obj = Objects[req.obj_id];
            for (int i = 0; i < obj.size; ++i) {
                target_blocks.emplace(obj.units[i]);
            }
        }
        // 移除过期的请求块
        for (ReadRequest& req : active_requests.current_overdue_requests) {
            Object& obj = Objects[req.obj_id];
            for (int i = 0; i < obj.size; ++i) {
                target_blocks.erase(obj.units[i]);
            }
        }
        active_requests.current_overdue_requests.clear();
    }


    // 连续读到head的token不够用
    int continuouslyReadStrategy(DiskHead& head, int disk_id, int end) {
        int read_cost = calculate_read_cost(head.last_token, head.last_action);
        int move_steps = 0;
        int true_end = (end + 1) % V;
        while (head.pos != true_end && head.remaining_tokens >= read_cost) {
            int obj_id = disk_units[disk_id][head.pos].first;
            int obj_unit_index = disk_units[disk_id][head.pos].second;
            if (obj_set.count(obj_id) != 0) { // 未被删除
                Objects[obj_id].readed_units[obj_unit_index] += 1;
                Objects[obj_id].readed_blocks_num += 1;
            }
            auto it = target_blocks.find(head.pos);
            if (it != target_blocks.end()) { // 删除单个值
                target_blocks.erase(it);
            }
            head.remaining_tokens -= read_cost;
            head.pos = (head.pos + 1) % V;
            move_steps += 1;
            head.last_token = read_cost;
            head.last_action = 'R';
            read_cost = calculate_read_cost(read_cost, head.last_action);
        }
        return move_steps;
    }

    vector<char> minMaxReadStrategy(int disk_index) override {  // 一个时间片中，每个disk只会调用一次该方法
        if (target_blocks.empty()) {
            return { '#' };
        }

        int min_unit_index = *target_blocks.begin();
        int max_unit_index = *target_blocks.rbegin();

        DiskHead& head = disk_heads[disk_index];
        vector<char> actions;
        actions.reserve(int(G / 16));   // 预估最大可能动作数

        // 磁头当前位置不在目标范围内
        if (head.pos < min_unit_index || head.pos > max_unit_index) {
            int distance = (min_unit_index - head.pos + V) % V;
            if (head.last_action == 'R' && distance < 4) { // 检查是否可以一直读到min_unit_index，如果可以则读到min_unit_index
                // 先检查剩余的token是否足够
                int last_token = head.last_token;
                int totoal_cost = 0;
                for (int i = 0; i < distance; i++) {
                    int cost_token = calculate_read_cost(last_token, 'R');
                    totoal_cost += cost_token;
                    last_token = cost_token;
                }
                if (totoal_cost <= head.remaining_tokens) {  // 剩余token足够，直接一直读完
                    int move_steps = continuouslyRead(head, min_unit_index - 1);  // 读的是无效块
                    actions.insert(actions.end(), move_steps, 'r');
                    //for (int i = 0; i < move_steps; i++) {
                    //    actions.push_back('r');
                    //}
                }
            }
            // 如果还有剩余令牌，处理剩余动作  // 剩余token不足，选择是Pass过去还是Jump过去
            if (head.remaining_tokens > 0 && head.pos != min_unit_index) {
                int steps = min(head.remaining_tokens, distance);
                if (steps >= G) { // Jump
                    head.last_action = 'J';
                    head.remaining_tokens = 0;
                    head.pos = min_unit_index;
                    actions.push_back('j');
                    actions.push_back(' ');
                    // actions.push_back(min_unit_index);
                    int tmp_num = min_unit_index + 1; // 磁盘块id从1开始
                    int digit_num = 1, tmp = 1;
                    while (tmp_num) {
                        tmp_num /= 10;
                        if (tmp_num == 0) break;
                        digit_num++;
                        tmp *= 10;
                    }
                    tmp_num = min_unit_index + 1;
                    for (int i = 0; i < digit_num; i++) {
                        actions.push_back('0' + ((tmp_num / tmp) % 10));
                        tmp /= 10;
                    }
                    return actions; // 出现Jump了，本次读完，返回
                }
                else {  // Pass
                    head.pos = (head.pos + steps) % V;
                    head.remaining_tokens -= steps;
                    head.last_action = 'P';
                    actions.insert(actions.end(), steps, 'p');
                    //for (int i = 0; i < steps; i++) {
                    //    actions.push_back('p');
                    //}
                    if (head.remaining_tokens == 0) {   // token用完，无法进行其他操作，返回
                        actions.push_back('#');
                        return actions;
                    }
                }
            }

        }
        // 磁头在目标范围内，连续读取
        if (head.pos >= min_unit_index && head.pos <= max_unit_index && head.remaining_tokens > 0) {
            int move_steps = continuouslyReadStrategy(head, disk_index, max_unit_index);
            actions.insert(actions.end(), move_steps, 'r');
            // 可能的优化: if(head.last_action == 'R' && distance < 4) 再次执行head.pos > min_unit_index || head.pos < max_unit_index的操作 
            //if (head.pos == min_unit_index) {   // 恰好又回到最小单元
            //    move_steps = continuouslyReadStrategy(head, disk_index, max_unit_index);
            //}
            //actions.insert(actions.end(), move_steps, 'r');
            actions.push_back('#');
            return actions;
        }
        actions.push_back('#');
        return actions;
    }

    // 删除在磁盘组中的对象块
    void delete_object(Object& obj) override {
        for (int i = 0; i < obj.size; i++) {
            target_blocks.erase(obj.units[i]);
            // 物理盘置为空闲
            for (int rep = 0; rep < obj.disks.size(); rep++) {
                disk_units[obj.disks[rep]][obj.units[i]].first = 0;
            }
        }
        obj_set.erase(obj.id);
        disks_free_count += obj.size;
    }


    void remove_target_block(int disk_unit_index) { // 删除单个
        auto it = target_blocks.find(disk_unit_index);
        if (it != target_blocks.end()) {
            target_blocks.erase(it);
        }
    }
    void remove_target_blocks(int disk_unit_index) {    // 删除所有
        target_blocks.erase(disk_unit_index);
    }

};


// 若划分了虚拟盘，该函数的作用:
// 根据目标的磁盘号，当前的磁盘号，当前所在的磁盘的第几个三分之一，计算目标磁盘号上的起始偏移
int calc_shift(int target_disk_id, int cur_disk_id, int thirds) {
    return 0;
}
// 虚拟盘信息
struct VirtualDisk {
    int physical_disk;  // 所属物理盘ID (从0开始)
    int group_id;
    int start_unit;     // 起始存储单元 (从0开始)
    int end_unit;       // 结束存储单元 (从0开始)
};
class DiskGroupFour :public DiskGroup {  // 由四个盘组成的DiskGroup
public:
    Type get_type() override { return Type::FOUR; }

    // 1000 * 105 / 16 / 1.5 * 4 = 17500  1000 * 105 / 16 / 3 * 4 = 8750
    // disk_free_count 是每块虚拟硬盘的剩余空闲块数的最大值
    vector<int> virtual_disk_free_count;	// 虚拟磁盘组的剩余空闲块数，一个硬盘平均划分为三个虚拟磁盘

    // 虚拟盘映射 [虚拟盘组索引][副本索引] -> 虚拟盘全局索引
    const int virtual_disk_map[4][3] = {
        {0, 3, 6},
        {1, 4, 9},
        {2, 7, 10},
        {5, 8, 11}
    };	// [虚拟盘_index][镜像盘_index] 表示虚拟盘组_index(0,1,2,3)对应的所有镜像盘_index, 3个镜像盘，因为每个对象存3个副本  镜像盘_index//3得到磁盘_index(从0开始)，镜像盘_index % 3得到磁盘的thirds(第几个3分之一)。镜像盘存储的数据块的位置对应，只是差一个偏移。
    /*
    不同的虚拟盘都要有自己独立的target_blocks，但虚拟组共用一个target_blocks。
    磁盘使用区间读取策略时，根据自己三个独立的虚拟盘的target_blocks获取磁盘号的读取区间(min,max)
    */

    unordered_map<int, int> virtualIndex_virtualGroup_map = {
        {0,0},{3,0},{6,0},{1,1},{4,1},{9,1},{2,2},{7,2},{10,2},{5,3},{8,3},{11,3}
    };

    vector<multiset<int>> target_blocks;	// 每个虚拟盘组的目标块（存储相对位置）


    vector<VirtualDisk> virtual_disks;  // 虚拟盘信息, 从0开始

    DiskGroupFour(int V, int G, int start_disk_id, int request_max_size, const vector<int>& disk_ids)
        : DiskGroup(V, G, start_disk_id, request_max_size, disk_ids), target_blocks(4), virtual_disk_free_count(4, V / 3) {

        // 初始化12个虚拟盘 (4物理盘 × 3虚拟区)
        virtual_disks.resize(12);

        int chunk_size = V / 3;
        // 为每个物理盘创建3个虚拟区
        // 建立虚拟盘映射关系
        for (int group_idx = 0; group_idx < 4; ++group_idx) {
            for (int rep = 0; rep < 3; ++rep) {
                int vdisk_idx = virtual_disk_map[group_idx][rep];
                int physical_idx = vdisk_idx / 3; // 物理盘索引 (0-based)
                int chunk_num = vdisk_idx % 3;    // 虚拟区号 (0-2)

                virtual_disks[vdisk_idx] = {
                    physical_idx,
                    group_idx,
                    chunk_num * chunk_size,
                    (chunk_num + 1) * chunk_size - 1
                };
            }
        }

    }

    // 添加对象到虚拟盘组
    /*
    设置对象的disk[]、units[]
    分配硬盘组的空间
    */
    bool add_object(Object& obj) {
        // 先选择tag聚集点的虚拟盘组
        int best_group = obj.tag % 4;
        if (virtual_disk_free_count[best_group] < obj.size) {   // 虚拟磁盘空间不足，需要找其他虚拟盘组
            best_group = -1;
            for (int g = 0; g < 4; ++g) {
                if (virtual_disk_free_count[g] >= obj.size) {
                    best_group = g;
                    break;
                }
            }
        }
        if (best_group == -1) return false; // 该磁盘组无法存储该对象，应该尝试其它的磁盘组

        // 在物理盘上分配空间
        for (int rep = 0; rep < 3; ++rep) {
            int virtual_disk_idx = virtual_disk_map[best_group][rep];

            obj.disks[rep] = virtual_disk_idx;
            const auto& vdisk = virtual_disks[virtual_disk_idx];
            int disk_idx = vdisk.physical_disk;
            for (int k = vdisk.start_unit, i = 0; i < obj.size; ++k) {
                if (disk_units[disk_idx][k].first == 0) {
                    disk_units[disk_idx][k].first = obj.id;
                    disk_units[disk_idx][k].second = i;
                    obj.units[i] = k - vdisk.start_unit;
                    i++;
                }
            }
        }
        virtual_disk_free_count[best_group] -= obj.size;
        obj_set.insert(obj.id);
        return true;
    }

    // 为读取请求添加目标块
    void add_requests(vector<ReadRequest>& reqs) override {
        active_requests.push(reqs);
        for (ReadRequest& req : reqs) {
            Object& obj = Objects[req.obj_id];

            // 使用映射表快速找到虚拟盘组
            int vdisk_idx = obj.disks[0]; // 第0个副本的虚拟盘索引, obj的三个副本一定是在一组的，所以只要判断第0个即可
            auto it = virtualIndex_virtualGroup_map.find(vdisk_idx);
            if (it != virtualIndex_virtualGroup_map.end()) {
                int group_idx = it->second;
                // 添加所有块到目标集合 (使用相对位置)
                for (int i = 0; i < obj.size; ++i) {
                    target_blocks[group_idx].emplace(obj.units[i]);
                }
            }
        }

        // 移除过期的请求块
        for (ReadRequest& req : active_requests.current_overdue_requests) {
            Object& obj = Objects[req.obj_id];
            // 使用映射表快速找到虚拟盘组
            int vdisk_idx = obj.disks[0]; // 第0个副本的虚拟盘索引, obj的三个副本一定是在一组的，所以只要判断第0个即可
            auto it = virtualIndex_virtualGroup_map.find(vdisk_idx);
            if (it != virtualIndex_virtualGroup_map.end()) {
                int group_idx = it->second;
                // 添加所有块到目标集合 (使用相对位置)
                for (int i = 0; i < obj.size; ++i) {
                    target_blocks[group_idx].erase(obj.units[i]);
                }
            }
        }
        active_requests.current_overdue_requests.clear();

    }

    // 连续读到head的token不够用
    int continuouslyReadStrategy(DiskHead& head, vector<VirtualDisk>& related_VDisks, int disk_id, int end) {
        int read_cost = calculate_read_cost(head.last_token, head.last_action);
        int move_steps = 0;
        int true_end = (end + 1) % V;
        while (head.pos != true_end && head.remaining_tokens >= read_cost) {
            int obj_id = disk_units[disk_id][head.pos].first;
            int obj_unit_index = disk_units[disk_id][head.pos].second;
            if (obj_set.count(obj_id) != 0) { // 未被删除
                Objects[obj_id].readed_units[obj_unit_index] += 1;
                Objects[obj_id].readed_blocks_num += 1;
            }
            for (int i = 0; i < related_VDisks.size(); i++) {
                int group_id = related_VDisks[i].group_id;
                auto it = target_blocks[group_id].find(head.pos);
                if (it != target_blocks[group_id].end()) { // 删除单个值
                    target_blocks[group_id].erase(it);
                    break;
                }
            }

            head.remaining_tokens -= read_cost;
            head.pos = (head.pos + 1) % V;
            move_steps += 1;
            head.last_token = read_cost;
            head.last_action = 'R';
            read_cost = calculate_read_cost(read_cost, head.last_action);
        }
        return move_steps;
    }

    // 找到disk_index的磁盘上 的 需要读取的磁盘块区间，然后尝试在这区间上连续读取
    vector<char> minMaxReadStrategy(int disk_index) override {
        // 获取属于该物理盘的所有虚拟盘
        vector<VirtualDisk> related_VDisks(3);
        for (int g = 0; g < 4; ++g) {
            for (int r = 0; r < 3; ++r) {
                int vdisk_idx = virtual_disk_map[g][r];
                if (virtual_disks[vdisk_idx].physical_disk == disk_index) {
                    VirtualDisk& vdisk = virtual_disks[vdisk_idx];
                    // 按照在物理盘的实际位置排一下序
                    if (vdisk.start_unit == 0) {
                        related_VDisks[0] = vdisk;
                    }
                    else if (vdisk.start_unit == V / 3) {
                        related_VDisks[1] = vdisk;
                    }
                    else {
                        related_VDisks[2] = vdisk;
                    }
                    break;
                }
            }
        }

        DiskHead& head = disk_heads[disk_index];
        vector<char> actions;
        actions.reserve(G / 16);
        bool blocks_empty[3] = { false };
        for (int i = 0; i < 3; i++) {
            if (target_blocks[related_VDisks[i].group_id].empty()) blocks_empty[i] = true;
        }
        int first_index = -1, second_index = -1;
        for (int i = 0; i < 3; i++) {
            if (!blocks_empty[i]) {
                first_index = i;
                break;
            }
        }
        if (first_index == -1) {    // 不存在需要读取的块
            return { '#' };
        }
        for (int i = 2; i >= 0; i--) {
            if (!blocks_empty[i]) {
                second_index = i;
                break;
            }
        }
        int min_unit_index = *target_blocks[related_VDisks[first_index].group_id].begin() + related_VDisks[first_index].start_unit;
        int max_unit_index = *target_blocks[related_VDisks[second_index].group_id].begin() + related_VDisks[second_index].start_unit;

        related_VDisks.assign(related_VDisks.begin() + first_index, related_VDisks.begin() + second_index + 1);
        // 磁头当前位置不在目标范围内
        if (head.pos < min_unit_index || head.pos > max_unit_index) {
            int distance = (min_unit_index - head.pos + V) % V;
            if (head.last_action == 'R' && distance < 4) { // 检查是否可以一直读到min_unit_index，如果可以则读到min_unit_index
                // 先检查剩余的token是否足够
                int last_token = head.last_token;
                int totoal_cost = 0;
                for (int i = 0; i < distance; i++) {
                    int cost_token = calculate_read_cost(last_token, 'R');
                    totoal_cost += cost_token;
                    last_token = cost_token;
                }
                if (totoal_cost <= head.remaining_tokens) {  // 剩余token足够，直接一直读完
                    int move_steps = continuouslyRead(head, min_unit_index - 1);  // 读的是无效块
                    actions.insert(actions.end(), move_steps, 'r');
                }
            }
            // 如果还有剩余令牌，处理剩余动作  // 剩余token不足，选择是Pass过去还是Jump过去
            if (head.remaining_tokens > 0 && head.pos != min_unit_index) {
                int steps = min(head.remaining_tokens, distance);
                if (steps >= G) { // Jump
                    head.last_action = 'J';
                    head.remaining_tokens = 0;
                    head.pos = min_unit_index;
                    actions.push_back('j');
                    actions.push_back(' ');
                    // actions.push_back(min_unit_index);
                    int tmp_num = min_unit_index + 1; // 磁盘块id从1开始
                    int digit_num = 1, tmp = 1;
                    while (tmp_num) {
                        tmp_num /= 10;
                        if (tmp_num == 0) break;
                        digit_num++;
                        tmp *= 10;
                    }
                    tmp_num = min_unit_index + 1;
                    for (int i = 0; i < digit_num; i++) {
                        actions.push_back('0' + ((tmp_num / tmp) % 10));
                        tmp /= 10;
                    }
                    return actions; // 出现Jump了，本次读完，返回
                }
                else {  // Pass
                    head.pos = (head.pos + steps) % V;
                    head.remaining_tokens -= steps;
                    head.last_action = 'P';
                    actions.insert(actions.end(), steps, 'p');
                    if (head.remaining_tokens == 0) {   // token用完，无法进行其他操作，返回
                        actions.push_back('#');
                        return actions;
                    }
                }
            }

        }
        // 磁头在目标范围内，连续读取
        if (head.pos >= min_unit_index && head.pos <= max_unit_index && head.remaining_tokens > 0) {
            int move_steps = continuouslyReadStrategy(head, related_VDisks, disk_index, max_unit_index);
            actions.insert(actions.end(), move_steps, 'r');
            // 可能的优化: if(head.last_action == 'R' && distance < 4) 再次执行head.pos > min_unit_index || head.pos < max_unit_index的操作 
            //if (head.pos == min_unit_index) {   // 恰好又回到最小单元
            //    move_steps = continuouslyReadStrategy(head, related_VDisks, disk_id, max_unit_index);
            //}
            //actions.insert(actions.end(), move_steps, 'r');
            actions.push_back('#');
            return actions;
        }
        actions.push_back('#');
        return actions;
    }

    // 删除在磁盘组中的对象块
    /*
       target_blocks
       disk_units
       disk_free_count
       obj_set
    activeRequests随后再删
    */
    void delete_object(Object& obj) override {
        for (int rep = 0; rep < obj.disks.size(); rep++) {
            int vDisk_idx = obj.disks[rep];
            VirtualDisk& vDisk = virtual_disks[vDisk_idx];
            int group_id = vDisk.group_id;
            int disk_index = vDisk.physical_disk;
            for (int i = 0; i < obj.size; i++) {
                disk_units[disk_index][vDisk.start_unit + obj.units[i]].first = 0;
            }
        }
        int vDisk_idx = obj.disks[0];
        int group_id = virtualIndex_virtualGroup_map[vDisk_idx];
        for (int i = 0; i < obj.size; i++) {
            target_blocks[group_id].erase(obj.units[i]);
        }
        virtual_disk_free_count[group_id] += obj.size;
        obj_set.erase(obj.id);
    }



};

class DiskGroupFive :public DiskGroup {  // 由五个盘组成的DiskGroup

public:
    Type get_type() override { return Type::FIVE; }

    // 1000 * 105 / 16 / 1.5 * 5 = 21875  1000 * 105 / 16 / 3 * 5= 10937
    // disk_free_count 是每块虚拟硬盘的剩余空闲块数的最大值

    int virtual_disk_map[5][3] = {
        {0, 3, 6},
        {1, 9, 12},
        {2, 4, 10},
        {5, 7, 13},
        {8, 11, 14}
    };	// [虚拟盘_index][镜像盘_index] 表示虚拟盘组_index(0,1,2,3)对应的所有镜像盘_index, 3个镜像盘，因为每个对象存3个副本  镜像盘_index//3得到磁盘_index(从0开始)，镜像盘_index % 3得到磁盘的thirds(第几个3分之一)
    unordered_map<int, int> virtualIndex_virtualGroup_map = {
        {0,0},{3,0},{6,0},{1,1},{9,1},{12,1},{2,2},{4,2},{10,2},{5,3},{7,3},{13,3},{8,4},{11,4},{14,4}
    };
    vector<VirtualDisk> virtual_disks;  // 15个虚拟盘 (索引0-based)
    vector<multiset<int>> target_blocks; // 每个虚拟盘组的目标块 (存储相对位置)
    vector<int> virtual_disk_free_count; // 每个虚拟盘组的空闲单元数

    DiskGroupFive(int V, int G, int start_disk_id, int request_max_size,
        const vector<int>& disk_ids)
        : DiskGroup(V, G, start_disk_id, request_max_size, disk_ids),
        target_blocks(5), virtual_disk_free_count(5, V / 3) {

        // 初始化15个虚拟盘
        virtual_disks.resize(15);
        int chunk_size = V / 3;

        // 建立虚拟盘映射关系
        for (int group_idx = 0; group_idx < 5; ++group_idx) {
            for (int rep = 0; rep < 3; ++rep) {
                int vdisk_idx = virtual_disk_map[group_idx][rep];
                int physical_idx = vdisk_idx / 3; // 物理盘索引 (0-based)
                int chunk_num = vdisk_idx % 3;    // 虚拟区号 (0-2)

                virtual_disks[vdisk_idx] = {
                    physical_idx,
                    group_idx,
                    chunk_num * chunk_size,
                    (chunk_num + 1) * chunk_size - 1
                };
            }
        }
    }

    bool add_object(Object& obj) override {
        // 1. 选择最优虚拟盘组 (基于标签和空间)
        int best_group = obj.tag % 5;
        if (virtual_disk_free_count[best_group] < obj.size) {
            best_group = -1;
            for (int g = 0; g < 5; ++g) {
                if (virtual_disk_free_count[g] >= obj.size) {
                    best_group = g;
                    break;
                }
            }
            if (best_group == -1) return false;
        }

        // 在物理盘上分配空间
        for (int rep = 0; rep < 3; ++rep) {
            int virtual_disk_idx = virtual_disk_map[best_group][rep];

            obj.disks[rep] = virtual_disk_idx;
            const auto& vdisk = virtual_disks[virtual_disk_idx];
            int disk_idx = vdisk.physical_disk;
            for (int k = vdisk.start_unit, i = 0; i < obj.size; ++k) {
                if (disk_units[disk_idx][k].first == 0) {
                    disk_units[disk_idx][k].first = obj.id;
                    disk_units[disk_idx][k].second = i;
                    obj.units[i] = k - vdisk.start_unit;
                    i++;
                }
            }
        }
        virtual_disk_free_count[best_group] -= obj.size;
        obj_set.insert(obj.id);
        return true;
    }

    void add_requests(vector<ReadRequest>& reqs) override {
        active_requests.push(reqs);

        for (ReadRequest& req : reqs) {
            Object& obj = Objects[req.obj_id];

            // 使用映射表快速找到虚拟盘组
            int vdisk_idx = obj.disks[0]; // 第0个副本的虚拟盘索引, obj的三个副本一定是在一组的，所以只要判断第0个即可
            auto it = virtualIndex_virtualGroup_map.find(vdisk_idx);
            if (it != virtualIndex_virtualGroup_map.end()) {
                int group_idx = it->second;
                // 添加所有块到目标集合 (使用相对位置)
                for (int i = 0; i < obj.size; ++i) {
                    target_blocks[group_idx].emplace(obj.units[i]);
                }
            }
        }
        // 移除过期的请求块
        for (ReadRequest& req : active_requests.current_overdue_requests) {
            Object& obj = Objects[req.obj_id];
            // 使用映射表快速找到虚拟盘组
            int vdisk_idx = obj.disks[0]; // 第0个副本的虚拟盘索引, obj的三个副本一定是在一组的，所以只要判断第0个即可
            auto it = virtualIndex_virtualGroup_map.find(vdisk_idx);
            if (it != virtualIndex_virtualGroup_map.end()) {
                int group_idx = it->second;
                // 添加所有块到目标集合 (使用相对位置)
                for (int i = 0; i < obj.size; ++i) {
                    target_blocks[group_idx].erase(obj.units[i]);
                }
            }
        }
        active_requests.current_overdue_requests.clear();

    }

    // 连续读到head的token不够用
    // 这个函数还有待优化，当距离下一个target_block过远时，比如超过25，就要pass过去，而不是一直读过去
    int continuouslyReadStrategy(DiskHead& head, vector<VirtualDisk>& related_VDisks, int disk_id, int end) {
        int read_cost = calculate_read_cost(head.last_token, head.last_action);
        int move_steps = 0;
        int true_end = (end + 1) % V;
        while (head.pos != true_end && head.remaining_tokens >= read_cost) {
            int obj_id = disk_units[disk_id][head.pos].first;
            int obj_unit_index = disk_units[disk_id][head.pos].second;
            if (obj_set.count(obj_id) != 0) { // 未被删除
                Objects[obj_id].readed_units[obj_unit_index] += 1;
                Objects[obj_id].readed_blocks_num += 1;
            }
            for (int i = 0; i < related_VDisks.size(); i++) {
                int group_id = related_VDisks[i].group_id;
                auto it = target_blocks[group_id].find(head.pos);
                if (it != target_blocks[group_id].end()) { // 删除单个值
                    target_blocks[group_id].erase(it);
                    break;
                }
            }

            head.remaining_tokens -= read_cost;
            head.pos = (head.pos + 1) % V;
            move_steps += 1;
            head.last_token = read_cost;
            head.last_action = 'R';
            read_cost = calculate_read_cost(read_cost, head.last_action);
        }
        return move_steps;
    }

    // 找到disk_index的磁盘上 的 需要读取的磁盘块区间，然后尝试在这区间上连续读取
    vector<char> minMaxReadStrategy(int disk_index) override {
        // 获取属于该物理盘的所有虚拟盘
        vector<VirtualDisk> related_VDisks(3);
        for (int g = 0; g < 5; ++g) {
            for (int r = 0; r < 3; ++r) {
                int vdisk_idx = virtual_disk_map[g][r];
                if (virtual_disks[vdisk_idx].physical_disk == disk_index) {
                    VirtualDisk& vdisk = virtual_disks[vdisk_idx];
                    // 按照在物理盘的实际位置排一下序
                    if (vdisk.start_unit == 0) {
                        related_VDisks[0] = vdisk;
                    }
                    else if (vdisk.start_unit == V / 3) {
                        related_VDisks[1] = vdisk;
                    }
                    else {
                        related_VDisks[2] = vdisk;
                    }
                    break;
                }
            }
        }

        DiskHead& head = disk_heads[disk_index];
        vector<char> actions;
        actions.reserve(G / 16);

        bool blocks_empty[3] = { false };
        for (int i = 0; i < 3; i++) {
            if (target_blocks[related_VDisks[i].group_id].empty()) blocks_empty[i] = true;
        }
        int first_index = -1, second_index = -1;
        for (int i = 0; i < 3; i++) {
            if (!blocks_empty[i]) {
                first_index = i;
                break;
            }
        }
        if (first_index == -1) {    // 不存在需要读取的块
            return { '#' };
        }
        for (int i = 2; i >= 0; i--) {
            if (!blocks_empty[i]) {
                second_index = i;
                break;
            }
        }
        int min_unit_index = *target_blocks[related_VDisks[first_index].group_id].begin() + related_VDisks[first_index].start_unit;
        int max_unit_index = *target_blocks[related_VDisks[second_index].group_id].begin() + related_VDisks[second_index].start_unit;

        related_VDisks.assign(related_VDisks.begin() + first_index, related_VDisks.begin() + second_index + 1);

        // 磁头当前位置不在目标范围内
        if (head.pos < min_unit_index || head.pos > max_unit_index) {
            int distance = (min_unit_index - head.pos + V) % V;
            if (head.last_action == 'R' && distance < 4) { // 检查是否可以一直读到min_unit_index，如果可以则读到min_unit_index
                // 先检查剩余的token是否足够
                int last_token = head.last_token;
                int totoal_cost = 0;
                for (int i = 0; i < distance; i++) {
                    int cost_token = calculate_read_cost(last_token, 'R');
                    totoal_cost += cost_token;
                    last_token = cost_token;
                }
                if (totoal_cost <= head.remaining_tokens) {  // 剩余token足够，直接一直读完
                    int move_steps = continuouslyRead(head, min_unit_index - 1);  // 读的是无效块
                    actions.insert(actions.end(), move_steps, 'r');
                }
            }
            // 如果还有剩余令牌，处理剩余动作  剩余token不足，选择是Pass过去还是Jump过去
            if (head.remaining_tokens > 0 && head.pos != min_unit_index) {
                int steps = min(head.remaining_tokens, distance);
                if (steps >= G) { // Jump
                    head.last_action = 'J';
                    head.remaining_tokens = 0;
                    head.pos = min_unit_index;
                    actions.push_back('j');
                    actions.push_back(' ');
                    // actions.push_back(min_unit_index);
                    int tmp_num = min_unit_index + 1; // 磁盘块id从1开始
                    int digit_num = 1, tmp = 1;
                    while (tmp_num) {
                        tmp_num /= 10;
                        if (tmp_num == 0) break;
                        digit_num++;
                        tmp *= 10;
                    }
                    tmp_num = min_unit_index + 1;
                    for (int i = 0; i < digit_num; i++) {
                        actions.push_back('0' + ((tmp_num / tmp) % 10));
                        tmp /= 10;
                    }
                    return actions; // 出现Jump了，本次读完，返回
                }
                else {  // Pass
                    head.pos = (head.pos + steps) % V;
                    head.remaining_tokens -= steps;
                    head.last_action = 'P';
                    actions.insert(actions.end(), steps, 'p');
                    if (head.remaining_tokens == 0) {   // token用完，无法进行其他操作，返回
                        actions.push_back('#');
                        return actions;
                    }
                }
            }

        }
        // 磁头在目标范围内，连续读取
        if (head.pos >= min_unit_index && head.pos <= max_unit_index && head.remaining_tokens > 0) {
            int move_steps = continuouslyReadStrategy(head, related_VDisks, disk_index, max_unit_index);
            actions.insert(actions.end(), move_steps, 'r');
            // 可能的优化: if(head.last_action == 'R' && distance < 4) 再次执行head.pos > min_unit_index || head.pos < max_unit_index的操作 
            //if (head.pos == min_unit_index) {   // 恰好又回到最小单元
            //    move_steps = continuouslyReadStrategy(head, related_VDisks, disk_id, max_unit_index);
            //}
            //actions.insert(actions.end(), move_steps, 'r');
            actions.push_back('#');
            return actions;
        }
        actions.push_back('#');
        return actions;
    }

    // 删除在磁盘组中的对象块
    /*
       target_blocks
       disk_units
       disk_free_count
       obj_set
    activeRequests随后再删
    */
    void delete_object(Object& obj) override {
        for (int rep = 0; rep < obj.disks.size(); rep++) {
            int vDisk_idx = obj.disks[rep];
            VirtualDisk& vDisk = virtual_disks[vDisk_idx];
            int group_id = vDisk.group_id;
            int disk_index = vDisk.physical_disk;
            for (int i = 0; i < obj.size; i++) {
                disk_units[disk_index][vDisk.start_unit + obj.units[i]].first = 0;
            }
        }
        int vDisk_idx = obj.disks[0];
        int group_id = virtualIndex_virtualGroup_map[vDisk_idx];
        for (int i = 0; i < obj.size; i++) {
            target_blocks[group_id].erase(obj.units[i]);
        }
        virtual_disk_free_count[group_id] += obj.size;
        obj_set.erase(obj.id);
    }

};

class StorageSystem {
public:
    int N;  // 硬盘总数 3<=N<=10
    int M;  // 对象标签总数  1<=M<=16
    int V; // 每盘单元数   [1,16384]  任何时间存储系统中空余的存储单元数占总存储单元数的至少10%
    int G; // 每时间片令牌数  [64,1000]
    vector<DiskGroup*> disk_groups;


    // 根据传入的硬盘总数，初始化硬盘组，硬盘数N最大为10，最小为3。
    // 如果N%3==0，则将所有硬盘划分为多个DiskGroupThree组
    // 如果N%3==1，则将所有硬盘划分为一个DiskGroupFour组，剩余的硬盘划分为多个DiskGroupThree组
    // 如果N%3==2，则将所有硬盘划分为一个DiskGroupFive组，剩余的硬盘划分为多个DiskGroupThree组
    StorageSystem(int N, int M, int V, int G) : N(N), M(M), V(V), G(G) {
        initialize_disk_groups();
    }

    ~StorageSystem() {
        for (auto group : disk_groups) {
            delete group;
        }
    }

    void initialize_disk_groups() {
        vector<int> all_disks(N);
        for (int i = 0; i < N; ++i) {
            all_disks[i] = i + 1; // 磁盘编号从1开始
        }

        // 先处理3盘组
        int three_group_count = N / 3;
        if (N % 3 == 0) {
            // 全部都是3盘组
            three_group_count = N / 3;
        }
        else if (N % 3 == 1 && N >= 4) {
            // 保留4个盘给后面的4盘组
            three_group_count = (N - 4) / 3;
        }
        else if (N % 3 == 2 && N >= 5) {
            // 保留5个盘给后面的5盘组
            three_group_count = (N - 5) / 3;
        }


        // 先创建所有3盘组
        for (int i = 0; i < three_group_count; ++i) {
            vector<int> group_disks(all_disks.begin() + i * 3, all_disks.begin() + i * 3 + 3);
            disk_groups.push_back(new DiskGroupThree(V, G, i * 3 + 1, 12000, group_disks));
        }

        // 处理剩余的盘（4盘组或5盘组）
        int remaining_disks = N - three_group_count * 3;
        if (remaining_disks == 0) return;
        if (remaining_disks == 4) {
            // 创建4盘组
            vector<int> group4_disks(all_disks.begin() + three_group_count * 3, all_disks.end());
            disk_groups.push_back(new DiskGroupFour(V, G, three_group_count * 3 + 1, 17000, group4_disks));
        }
        else // (remaining_disks == 5) 
        {
            // 创建5盘组
            vector<int> group5_disks(all_disks.begin() + three_group_count * 3, all_disks.end());
            disk_groups.push_back(new DiskGroupFive(V, G, three_group_count * 3 + 1, 20000, group5_disks));
        }
    }


    void handle_read_events(int current_time) {
        int n_read;
        scanf("%d", &n_read);

        int disk_groups_size = disk_groups.size();
        vector<vector<ReadRequest>> newRequests(disk_groups_size);

        // 题目保证输入的obj_id有效，无需检查
        for (int i = 0; i < n_read; ++i) {
            int req_id, obj_id;
            scanf("%d%d", &req_id, &obj_id);

            ReadRequest req(req_id, obj_id, current_time);
            // 关联到对象
            Objects[obj_id].requests.emplace(req_id);
            // 关联到磁盘组
            for (int j = 0; j < disk_groups_size; ++j) {
                if (disk_groups[j]->obj_set.count(obj_id)) {
                    newRequests[j].emplace_back(req);
                    break;
                }
            }
        }
        // 加入到活跃请求队列中
        for (int i = 0; i < disk_groups_size; ++i) {
            if (newRequests[i].size()) {
                disk_groups[i]->add_requests(newRequests[i]);
            }
        }
        // 输出无请求取消（题目保证输入有效）
        vector<int> total_completed_reqs;
        for (int i = 0; i < disk_groups_size; ++i) {
            vector<int> completed_reqs = disk_groups[i]->process_readRequests();
            total_completed_reqs.insert(total_completed_reqs.end(), completed_reqs.begin(), completed_reqs.end());
        }
        printf("%d\n", total_completed_reqs.size());
        for (int req_id : total_completed_reqs) {
            printf("%d\n", req_id);
        }
        fflush(stdout);
    }

    void handle_delete_events() {
        int n_delete;
        scanf("%d", &n_delete);
        vector<int> del_ids(n_delete);
        for (int& id : del_ids) scanf("%d", &id); // 每个元素表示要删除的对象(object)编号
        int disk_groups_size = disk_groups.size();
        vector<vector<int>> group_del_ids(disk_groups_size);
        for (int i = 0; i < n_delete; ++i) {
            for (int j = 0; j < disk_groups_size; ++j) {
                if (disk_groups[j]->obj_set.count(del_ids[i])) {
                    group_del_ids[j].push_back(del_ids[i]);
                    break;
                }
            }
        }

        int abort_num = 0;  // 因删除导致的取消的请求的总数
        vector<int> objInDiskGroupIndex;
        for (int obj_id : del_ids) {
            Object& obj = Objects[obj_id];
            obj.is_deleted = true;
            for (int i = 0; i < disk_groups_size; ++i) {
                if (disk_groups[i]->obj_set.count(obj_id)) {
                    objInDiskGroupIndex.push_back(i);
                    break;
                }
            }
            abort_num += obj.requests.size();
        }

        printf("%d\n", abort_num); // 输出取消的请求数（简化为n_delete，需实际计算）
        for (int i = 0; i < del_ids.size(); i++) {
            int obj_id = del_ids[i];
            Object& obj = Objects[obj_id];
            for (int req_id : obj.requests) {
                printf("%d\n", req_id);
                disk_groups[objInDiskGroupIndex[i]]->active_requests.erase(req_id);
            }
            disk_groups[objInDiskGroupIndex[i]]->delete_object(obj);
            Objects.erase(obj_id);
        }
        fflush(stdout);
    }

    void handle_write_events() {
        int n_write;
        scanf("%d", &n_write);
        //vector<vector<vector<int>>> write_results(n_write, vector<vector<int>>(REPLICA_NUM));   // [  [ [unit1, unit2, unit3], [unit1', unit2', unit3'] , [] ], ...   ]
        //vector<int> write_obj_ids(n_write);
        for (int i = 0; i < n_write; ++i) {
            int obj_id, size, tag;
            scanf("%d%d%d", &obj_id, &size, &tag);
            //write_obj_ids[i] = obj_id;

            // 分配存储单元

            // 存储对象元数据
            Objects[obj_id] = Object();
            Object& obj = Objects[obj_id];
            obj.id = obj_id;
            obj.size = size;
            obj.tag = tag;
            obj.is_deleted = false;
            int best_group = tag % N / 3;
            if (best_group == disk_groups.size()) best_group -= 1;
            if (!disk_groups[best_group]->add_object(obj)) {
                for (int disk_group_index = 0; disk_group_index < disk_groups.size(); disk_group_index++) {
                    if (disk_group_index == best_group) continue;
                    if (disk_groups[disk_group_index]->add_object(obj)) {
                        best_group = disk_group_index;
                        break;
                    }
                }
            }

            // 输出副本信息
            printf("%d\n", obj_id);
            DiskGroup* disk_group = disk_groups[best_group];
            DiskGroup::Type type = disk_group->get_type();
            for (int j = 0; j < REPLICA_NUM; ++j) {

                if (type == DiskGroup::Type::THREE) {
                    printf("%d", obj.disks[j] + disk_group->start_disk_id); // 副本所在磁盘
                    for (int u = 0; u < obj.size; u++)  printf(" %d", obj.units[u] + 1);    // 副本块所在磁盘块
                }
                else if (type == DiskGroup::Type::FOUR) {
                    DiskGroupFour* disk_group4 = static_cast<DiskGroupFour*>(disk_group);
                    int vdisk_index = obj.disks[j];
                    VirtualDisk& virtualDisk = disk_group4->virtual_disks[vdisk_index];
                    printf("%d", virtualDisk.physical_disk + disk_group4->start_disk_id);
                    for (int u = 0; u < obj.size; u++)  printf(" %d", virtualDisk.start_unit + obj.units[u] + 1);
                }
                else {
                    DiskGroupFive* disk_group5 = (DiskGroupFive*)disk_group;
                    int vdisk_index = obj.disks[j];
                    VirtualDisk& virtualDisk = disk_group5->virtual_disks[vdisk_index];
                    printf("%d", virtualDisk.physical_disk + disk_group5->start_disk_id);
                    for (int u = 0; u < obj.size; u++)  printf(" %d", virtualDisk.start_unit + obj.units[u] + 1);
                }
                printf("\n");
            }
        }

        fflush(stdout);
    }

};

int main() {
    int T, M, N, V, G;
    scanf("%d%d%d%d%d", &T, &M, &N, &V, &G);
    StorageSystem  system(N, M, V, G);
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
        // 处理删除事件
        system.handle_delete_events();
        // 处理写入事件
        system.handle_write_events();
        // 处理读取事件
        system.handle_read_events(t);

    }

    return 0;
}