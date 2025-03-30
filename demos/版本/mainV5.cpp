#define _CRT_SECURE_NO_WARNINGS 1


#include <iostream>
#include <vector>
#include <unordered_map>
#include <map>
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
    bool readed_one;    // 是否在当前时间片下读取过至少一个块,避免磁头闲置
    int next_read_distance; // 下一个读取的块距离当前磁头位置
    bool wait_for_schedule; // 是否等待调度
    float concentration;    // 用于根据集中程度排序
    DiskHead() {
        memset(this, 0, sizeof(DiskHead));
    }
};

void init_diskHeads(vector<DiskHead>& heads)
{
    for (int i = 0; i < heads.size(); i++) {
        heads[i].remaining_tokens = 1000;
        heads[i].readed_one = false;
        heads[i].next_read_distance = 1e6;  // 1e6 > 16384，默认不存在下一个读取的块
        heads[i].wait_for_schedule = true;
    }
}



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

    unordered_map<int, vector<int>> objId_units_current_read;   // DiskGroup该时间片读取的，优化：用更好的存储形式，因为obj的大小最多5，一个时间片最多读取62*盘块数

    vector<DiskHead> disk_heads;        // 磁头状态（disk_heads[disk_id]）
    // vector<ActiveRequests> active_requests_list;	// 每个磁盘组有自己的ActiveRequests  每个磁盘最大的ActiveRequests设置5500 = 1000 * 105 / 16 / 1.2
    ActiveRequests active_requests;
    vector<vector<char>> actions_list;

public:
    DiskGroup(int V, int G, int start_disk_id, int request_max_size, const vector<int>& disk_ids) :
        V(V), G(G), start_disk_id(start_disk_id), disk_id_list(disk_ids), active_requests(request_max_size), actions_list(disk_ids.size()){

        disk_units.resize(disk_ids.size(), vector<pair<int, int>>(V, { 0, 0 }));
        disk_heads.resize(disk_ids.size());
        for (auto& actions : actions_list) {
            actions.reserve(G-17);
        }
    }

    virtual ~DiskGroup() = default;

    // 连续读取直到令牌不足或到达目标位置
    //int continuouslyRead(DiskHead& head, int distance) {    // 读的都是无效块
    //    int read_cost = calculate_read_cost(head.last_token, 'R');
    //    int move_steps = 0;
    //    while (head.pos != true_end && head.remaining_tokens >= read_cost) {
    //        head.remaining_tokens -= read_cost;
    //        head.pos = (head.pos + 1) % V;
    //        move_steps += 1;
    //        head.last_token = read_cost;
    //        read_cost = calculate_read_cost(read_cost, 'R');
    //    }
    //    return move_steps;
    //}

    virtual void minMaxReadStrategy(int disk_index, vector<char> &actions) = 0;
    virtual bool add_object(Object& obj) = 0;   // 写入
    virtual void add_requests(vector<ReadRequest>& reqs) = 0;   // 读取
    virtual void delete_object(Object& obj) = 0;  // 删除
    // virtual pair<int, int> get_sort_info(int disk_index) = 0; // 获取磁盘排序信息
    virtual void diskHead_sortByConcentration(vector<int>& disk_index_list) = 0;    // 排序磁盘下标


    vector<int> process_readRequests() {
        
        // 重置对象读取状态
        //for (ReadRequest& req : active_requests.queue) {
        //    Object& obj = Objects[req.obj_id];
        //    obj.readed_blocks_num = 0;
        //    memset(obj.readed_units, 0, sizeof(obj.readed_units));
        //}
        //for (int obj_id : obj_set) {
        //    Object& obj = Objects[obj_id];
        //    obj.readed_blocks_num = 0;
        //    memset(obj.readed_units, 0, sizeof(obj.readed_units));
        //}
        objId_units_current_read.clear();

        bool flag = true;
        vector<int> sorted_disk_index_list;
        sorted_disk_index_list.reserve(disk_heads.size());
        
        for (int i = 0; i < disk_heads.size(); i++) {
            DiskHead& disk_head = disk_heads[i];
            disk_head.remaining_tokens = G;
            disk_head.readed_one = false;
            disk_head.wait_for_schedule = true;
            disk_head.concentration = 0;
        }
        while (true) {
            for (int i = 0; i < disk_heads.size(); i++) {
                if (disk_heads[i].wait_for_schedule) {
                    sorted_disk_index_list.push_back(i);
                }
            }
            if (sorted_disk_index_list.empty()) break;
            diskHead_sortByConcentration(sorted_disk_index_list);
            for (int disk_index : sorted_disk_index_list) {
                DiskHead& disk_head = disk_heads[disk_index];
                minMaxReadStrategy(disk_index, actions_list[disk_index]);
            }
            sorted_disk_index_list.clear();
        }
        for (vector<char>& actions : actions_list) {
            for (int i = 0; i < actions.size(); i++) {
                printf("%c", actions[i]);
            }
            if (actions.empty()) printf("#");
            else if(actions[0] != 'j') printf("#");
            printf("\n");
            actions.clear();
        }
        
        // 检查已完成的请求
        vector<int> completed_requests;
        auto it = active_requests.queue.begin();
        while (it != active_requests.queue.end()) {
            ReadRequest& req = *it;
            int obj_id = req.obj_id;
            Object &obj = Objects[obj_id];
            if (!objId_units_current_read.count(obj_id)) {
                ++it;
                continue;

            }
            vector<int>& objId_units = objId_units_current_read[obj_id];
            for (int i = 0; i < req.block_num; i++) {
                if (req.blocks_done[i] == false && objId_units[i] > 0) {
                    objId_units[i]--;
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
    map<int, int> target_blocks;	// 需要读取的磁盘块位置 位置：计数
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
                target_blocks[obj.units[i]]++;
            }
        }
        // 移除过期的请求块
        for (ReadRequest& req : active_requests.current_overdue_requests) {
            Object& obj = Objects[req.obj_id];
            for (int i = 0; i < obj.size; ++i) {
                if (target_blocks[obj.units[i]] == 1) {
                    target_blocks.erase(obj.units[i]);
                }
                else {
                    target_blocks[obj.units[i]]--;
                }
            }
        }
        active_requests.current_overdue_requests.clear();
    }

    //// 获取排序依据：磁头到目标区间的最小距离
    //pair<int, int> get_sort_info(int disk_index) override {
    //    const DiskHead& head = disk_heads[disk_index];
    //    int head_pos = head.pos;

    //    if (target_blocks.empty()) {
    //        return { V + 1, 0 };  // 无目标块，距离设为最大值
    //    }

    //    int target_min = target_blocks.begin()->first;
    //    int target_max = target_blocks.rbegin()->first;
    //    if (head.pos >= target_min && head.pos <= target_max) {
    //        return { 0, target_max - head_pos };
    //    }
    //    // 计算到目标区间的最小距离（环形单向移动）
    //    int distance_forward = (target_min >= head_pos)
    //        ? (target_min - head_pos)
    //        : (V - head_pos + target_min);

    //    return { distance_forward, target_max - target_min };
    //}

    //// 根据距离排序磁盘索引
    //vector<int> sort_disks() override {
    //    vector<int> sorted_disk_index(disk_id_list);
    //    for (int i = 0; i < disk_id_list.size(); i++) {
    //        sorted_disk_index[i] -= start_disk_id;
    //    }
    //    // 按距离升序排序，距离相同则区间长度降序
    //    sort(sorted_disk_index.begin(), sorted_disk_index.end(),
    //        [this](int a, int b) {
    //            auto key_a = get_sort_info(a);
    //            auto key_b = get_sort_info(b);
    //            return (key_a.first < key_b.first) ||
    //                (key_a.first == key_b.first && key_a.second < key_b.second);
    //        });

    //    return sorted_disk_index;
    //}

    void diskHead_sortByDistance(vector<int>& disk_index_list)
    {
        // 按照next_read_distance升序排序
        sort(disk_index_list.begin(), disk_index_list.end(), [this](const int& a, const int& b) {
            return this->disk_heads[a].next_read_distance < this->disk_heads[b].next_read_distance;
            });
    }

    void diskHead_sortByConcentration(vector<int>& disk_index_list) {
        if (disk_index_list.empty() || target_blocks.empty()) return;
        int end_index = target_blocks.rbegin()->first;
        map<int, int>::iterator it_bigger;
        for (int i = 0; i < disk_heads.size(); i++) {
            DiskHead& head = disk_heads[i];
            if (head.pos > end_index) {
                it_bigger = target_blocks.begin();
                head.next_read_distance = (it_bigger->first - head.pos + V) % V;
            }
            else {
                it_bigger = target_blocks.lower_bound(head.pos);
                head.next_read_distance = it_bigger->first - head.pos;
            }
            int window_size = min((G - 150) / 16, end_index - it_bigger->first + 1);
            int target_count = 0;
            auto it_tmp = it_bigger;
            for (int j = it_bigger->first; j < it_bigger->first + window_size; j++) {
                if (it_tmp->first == j) {
                    target_count++;
                    it_tmp++;
                }
                if (it_tmp == target_blocks.end()) break;
            }
            head.concentration = 1.0 * target_count / window_size;
        }
        // 按照next_read_distance升序排序
        stable_sort(disk_index_list.begin(), disk_index_list.end(), [this](const int& a, const int& b) {
            return this->disk_heads[a].next_read_distance < this->disk_heads[b].next_read_distance;
            });
        // 按照concentration升序排序
        stable_sort(disk_index_list.begin(), disk_index_list.end(), [this](const int& a, const int& b) {
            return this->disk_heads[a].concentration < this->disk_heads[b].concentration;
            });
    }


    void continuouslyReadStrategy(DiskHead& head, int disk_index, vector<char>& actions) {    // 确保head.pos到end之间至少有一个token，进入这个函数的时候，head.pos的位置就是要读的
        int read_cost = calculate_read_cost(head.last_token, head.last_action);
        // 查找第一个不小于target_key的元素
        auto it_bigger = target_blocks.lower_bound(head.pos);
        while (head.remaining_tokens >= read_cost) {
            // 获取当前head.pos处的盘块信息
            int obj_id = disk_units[disk_index][head.pos].first;
            int obj_unit_index = disk_units[disk_index][head.pos].second;
            if (it_bigger->first == head.pos) {
                it_bigger->second--;
                if (it_bigger->second == 0) {
                    it_bigger = target_blocks.erase(it_bigger); // 下一个键
                }
                else {
                    it_bigger++;
                }
                if (objId_units_current_read.count(obj_id) == 0) {
                    objId_units_current_read[obj_id] = vector<int>(5, 0);
                }
                objId_units_current_read[obj_id][obj_unit_index]++;
            }

            head.remaining_tokens -= read_cost;
            head.pos = (head.pos + 1) % V;
            head.last_token = read_cost;
            head.last_action = 'R';
            actions.push_back('r');
            head.readed_one = true;

            // 推测检查下一次该怎么读取
            if (it_bigger == target_blocks.end()) {
                if (!target_blocks.empty()) {
                    head.next_read_distance = (target_blocks.begin()->first - head.pos + V) % V;
                    if (head.next_read_distance == 0 && head.remaining_tokens >= read_cost) {
                        head.wait_for_schedule = true;  // 调度完其它磁头后，可以继续调度
                    }
                    else {
                        if (head.remaining_tokens >= head.next_read_distance + 64) {
                            head.wait_for_schedule = true;
                        }
                    }
                }
                break; // 没有下一个target_block了，不要再读了
            }
            int next_distance = it_bigger->first - head.pos;
            // if (next_distance<4 || (head.remaining_tokens - 150) /16 - 6 > next_distance) {    // 连续读即可
            if (next_distance < 12) {
                read_cost = calculate_read_cost(head.last_token, head.last_action);

            }
            else {
                int steps = min(next_distance, head.remaining_tokens);
                read_cost = calculate_read_cost(0, 'P') + steps;
                if (head.remaining_tokens < read_cost) break;  // 不能再进行下次读取了，保持原位置，保留上一次操作为读取操作
                head.remaining_tokens -= steps;
                head.pos = (head.pos + steps) % V;  // 移动到下一个可以读取的位置
                head.last_action = 'P';
                actions.insert(actions.end(), steps, 'p');
                read_cost = calculate_read_cost(0, 'P');
            }
            
        }
    }


    void minMaxReadStrategy(int disk_index, vector<char> &actions) override {  // 一个时间片中，每个disk只会调用一次该方法

        DiskHead& head = disk_heads[disk_index];
        head.wait_for_schedule = false;
        if (target_blocks.empty()) {
            return;
        }
        //auto it_bigger = target_blocks.lower_bound(head.pos);
        //if (it_bigger == target_blocks.end()) {
        //    it_bigger = target_blocks.begin();
        //}
        int distance = head.next_read_distance;
        int next_uint_index = (head.next_read_distance + head.pos ) % V;

        // if (head.last_action == 'R' && distance < 4 && distance > 0 &&  (head.last_token<42 || head.concentration > 0.2)) { // 尝试一直读到target_block
        if (head.last_action == 'R' && distance < 8 && distance > 0) { // 尝试一直读到target_block
            // 先检查剩余的token是否足够
            int last_token = head.last_token;
            int totoal_cost = 0;
            for (int i = 0; i < distance; i++) {
                int cost_token = calculate_read_cost(last_token, 'R');
                totoal_cost += cost_token;
                last_token = cost_token;
            }
            if (totoal_cost <= head.remaining_tokens) {  // 剩余token足够，直接一直读完
                head.pos = next_uint_index;
                head.last_action = 'R';
                head.remaining_tokens -= totoal_cost;
                head.last_token = last_token;
                actions.insert(actions.end(), distance, 'r');
            }
        }
        // 如果还有剩余令牌，处理剩余动作  // 剩余token不足，选择是Pass过去还是Jump过去
        if (head.remaining_tokens > 0 && head.pos != next_uint_index) {
            int steps = min(head.remaining_tokens, distance);
            if (steps >= G) { // Jump
                head.last_action = 'J';
                head.remaining_tokens = 0;
                head.pos = next_uint_index;
                actions.push_back('j');
                actions.push_back(' ');
                // actions.push_back(min_unit_index);
                int tmp_num = next_uint_index + 1; // 磁盘块id从1开始
                int digit_num = 1, tmp = 1;
                while (tmp_num) {
                    tmp_num /= 10;
                    if (tmp_num == 0) break;
                    digit_num++;
                    tmp *= 10;
                }
                tmp_num = next_uint_index + 1;
                for (int i = 0; i < digit_num; i++) {
                    actions.push_back('0' + ((tmp_num / tmp) % 10));
                    tmp /= 10;
                }
                return; // 出现Jump了，本次读完，返回
            }
            else {  // Pass
                head.pos = (head.pos + steps) % V;
                head.remaining_tokens -= steps;
                head.last_action = 'P';
                actions.insert(actions.end(), steps, 'p');
                if (head.remaining_tokens == 0) {   // token用完，无法进行其他操作，返回
                    return;
                }
            }
        }
        // 磁头在目标范围内，连续读取
        if (head.pos == next_uint_index) {
            continuouslyReadStrategy(head, disk_index, actions);
            return;
        }
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



};


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

    vector<map<int, int>> target_blocks;	// 每个虚拟盘组的目标块（存储相对位置）


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
                    target_blocks[group_idx][obj.units[i]]++;
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
                // 移除所有访问块 (使用相对位置)
                for (int i = 0; i < obj.size; ++i) {
                    if (target_blocks[group_idx][obj.units[i]] == 1) {
                        target_blocks[group_idx].erase(obj.units[i]);
                    }
                    else {
                        target_blocks[group_idx][obj.units[i]]--;
                    }
                }
            }
        }
        active_requests.current_overdue_requests.clear();

    }


    void diskHead_sortByConcentration(vector<int>& disk_index_list) override {
        if (disk_index_list.empty()) return;

        // 1. 首先收集所有虚拟盘组的目标块信息
        unordered_map<int, pair<int, int>> groupIdx_targetUnits;
        for (int group_idx = 0; group_idx < disk_id_list.size(); ++group_idx) {
            if (!target_blocks[group_idx].empty()) {
                groupIdx_targetUnits[group_idx] = { target_blocks[group_idx].begin()->first, target_blocks[group_idx].rbegin()->first };
                break;
            }
        }

        if (groupIdx_targetUnits.empty()) {
            // 没有目标块需要读取，按默认顺序排序
            return;
        }
        int chunk_size = V / 3;
        // 2. 为每个磁头计算集中度和距离
        for (int disk_index : disk_index_list) {
            DiskHead& head = disk_heads[disk_index];
            vector<int> all_target_units_copy;
            vector<int> all_groupIdx_tmp(3);
            vector<int> all_groupIdx;
            // 获取该物理盘关联的所有虚拟盘
            vector<int> related_VDisk_indexs(3);
            for (int r = 0; r < 3; ++r) {
                related_VDisk_indexs[r] = r + disk_index * 3;
                all_groupIdx_tmp[r] = virtualIndex_virtualGroup_map[related_VDisk_indexs[r]];
            }
            for (int r = 0; r < 3; r++) {
                if (groupIdx_targetUnits.count(all_groupIdx_tmp[r])) {
                    int start_unit = r * chunk_size;
                    all_target_units_copy.push_back(groupIdx_targetUnits[all_groupIdx_tmp[r]].first + start_unit);
                    all_target_units_copy.push_back(groupIdx_targetUnits[all_groupIdx_tmp[r]].second + start_unit);
                    all_groupIdx.push_back(all_groupIdx_tmp[r]);
                }
            }
            if (all_target_units_copy.empty()) {
                head.next_read_distance = 1e6;
                continue;
            }
            int window_size, target_count;
            int related_vdisk_index = head.pos / chunk_size;
            if (related_vdisk_index == 3 || head.pos > all_target_units_copy[all_target_units_copy.size() - 1]) {  // head.pos在最后不使用的磁盘块上,这时其对应的下一个虚拟盘应为磁盘最开头的虚拟盘
                int min_unit_index = all_target_units_copy[0];
                int max_unit_index = all_target_units_copy[1];
                
                int start_unit = min_unit_index / chunk_size * chunk_size;
                int gourp_idx = all_groupIdx[0];
                head.next_read_distance = (min_unit_index - head.pos + V) % V;
                window_size = min((G - 150) / 16, max_unit_index - min_unit_index + 1);
                target_count = 0;
                auto it_tmp = target_blocks[gourp_idx].begin();
                for (int j = min_unit_index; j <= min_unit_index + window_size; j++) {
                    if (j == it_tmp->first + start_unit) {
                        target_count++;
                        it_tmp++;
                    }
                    if (it_tmp == target_blocks[gourp_idx].end()) break;
                }
            }
            else {
                related_vdisk_index = related_VDisk_indexs[related_vdisk_index];
                auto it_bigger = lower_bound(all_target_units_copy.begin(), all_target_units_copy.end(), head.pos);
                int index = distance(all_target_units_copy.begin(), it_bigger);
                if (index & 1) { // 奇，说明是max
                    int gourp_idx = all_groupIdx[index / 2];
                    
                    int max_unit_index = all_target_units_copy[index];
                    
                    int start_unit = max_unit_index / chunk_size * chunk_size;
                    int virtual_pos = head.pos - start_unit;
                    auto it_tmp = target_blocks[gourp_idx].lower_bound(virtual_pos);
                    int min_unit_index = it_tmp->first + start_unit;
                    head.next_read_distance = (min_unit_index - head.pos + V) % V;
                    window_size = min((G - 150) / 16, max_unit_index - min_unit_index + 1);
                    target_count = 0;
                    for(int j=min_unit_index; j<=min_unit_index+window_size; j++){
                        if (j == it_tmp->first + start_unit) {
                            target_count++;
                            it_tmp++;
                        }
                        if (it_tmp == target_blocks[gourp_idx].end()) break;
                    }
                    
                }
                else {  // 偶，说明是min
                    int gourp_idx = all_groupIdx[index / 2];
                    int min_unit_index = all_target_units_copy[index];
                    int max_unit_index = all_target_units_copy[index + 1];
                    int start_unit = min_unit_index / chunk_size * chunk_size;
                    head.next_read_distance = (min_unit_index - head.pos + V) % V;
                    window_size = min((G - 150) / 16, max_unit_index - min_unit_index + 1);
                    target_count = 0;
                    auto it_tmp = target_blocks[gourp_idx].begin();
                    for (int j = min_unit_index; j <= min_unit_index + window_size; j++) {
                        if (j == it_tmp->first + start_unit) {
                            target_count++;
                            it_tmp++;
                        }
                        if (it_tmp == target_blocks[gourp_idx].end()) break;
                    }
                }
                
            }
            head.concentration = target_count * 1.0 / window_size;
        }

        // 3. 排序策略：优先选择距离近且集中度高的磁头
        stable_sort(disk_index_list.begin(), disk_index_list.end(), [this](const int a, const int b) {
            return this->disk_heads[a].next_read_distance < this->disk_heads[b].next_read_distance;
            });

        stable_sort(disk_index_list.begin(), disk_index_list.end(), [this](const int& a, const int& b) {
            return this->disk_heads[a].concentration < this->disk_heads[b].concentration;
            });
    }



    // 连续读到head的token不够用
    void continuouslyReadStrategy(DiskHead& head, int disk_index, vector<char> &actions) {
        int read_cost = calculate_read_cost(head.last_token, head.last_action);
        int chunk_size = V / 3;
        // 查找第一个不小于target_key的元素
        int virtual_disk_index = disk_index *3 + head.pos / chunk_size;
        int group_id = virtualIndex_virtualGroup_map[virtual_disk_index];
        int start_unit = head.pos / chunk_size * chunk_size;
        auto it_bigger = target_blocks[group_id].lower_bound(head.pos - start_unit);
        while (head.remaining_tokens >= read_cost) {
            int obj_id = disk_units[disk_index][head.pos].first;
            int obj_unit_index = disk_units[disk_index][head.pos].second;
            if (it_bigger->first + start_unit == head.pos) {
                it_bigger->second--;
                if (it_bigger->second == 0) {
                    it_bigger = target_blocks[group_id].erase(it_bigger); // 下一个键
                }
                else {
                    it_bigger++;
                }
                if (objId_units_current_read.count(obj_id) == 0) {
                    objId_units_current_read[obj_id] = vector<int>(5, 0);
                }
                objId_units_current_read[obj_id][obj_unit_index]++;
            }
            

            head.remaining_tokens -= read_cost;
            head.pos = (head.pos + 1) % V;
            head.last_token = read_cost;
            head.last_action = 'R';
            actions.push_back('r');
            head.readed_one = true;

            if (it_bigger == target_blocks[group_id].end()) {
                if (!target_blocks[group_id].empty()) {
                    int flag = (target_blocks[group_id].rbegin()->first + 1) / chunk_size;
                    if (flag == 3) {
                        flag = 0;
                    }
                    int next_virtual_disk_index = disk_index * 3 + flag;
                    int next_group_id = virtualIndex_virtualGroup_map[next_virtual_disk_index];
                    int next_start_unit = flag * chunk_size;
                    head.next_read_distance = (target_blocks[next_group_id].begin()->first + next_start_unit - head.pos + V) % V;
                    if (head.next_read_distance == 0 && head.remaining_tokens >= read_cost) {
                        head.wait_for_schedule = true;  // 调度完其它磁头后，可以继续调度
                    }
                    else {
                        if (head.remaining_tokens >= head.next_read_distance + 64) {
                            head.wait_for_schedule = true;
                        }
                    }
                }
                break; // 没有下一个target_block了，不要再读了
            }

            // 推测检查下一次该怎么读取
            int next_distance = it_bigger->first + start_unit - head.pos;
            //if (next_distance<4 || (head.remaining_tokens - 150) / 16 - 6 > next_distance) {    // 连续读即可
            if(next_distance<12){
                read_cost = calculate_read_cost(head.last_token, head.last_action);
            }
            else {
                int steps = min(next_distance, head.remaining_tokens);
                read_cost = calculate_read_cost(0, 'P') + steps;
                if (head.remaining_tokens < read_cost) break;  // 不能再进行下次读取了，保持原位置，保留上一次操作为读取操作
                head.remaining_tokens -= steps;
                head.pos = (head.pos + steps) % V;  // 移动到下一个可以读取的位置
                head.last_action = 'P';
                actions.insert(actions.end(), steps, 'p');
                read_cost = calculate_read_cost(0, 'P');
            }
        }

    }

    // 找到disk_index的磁盘上 的 需要读取的磁盘块区间，然后尝试在这区间上连续读取
     void minMaxReadStrategy(int disk_index, vector<char>& actions) override {

        // 获取属于该物理盘的所有虚拟盘
        vector<VirtualDisk> related_VDisks(3);
        for (int r = 0; r < 3; r++) {
            int virtual_idx = r + disk_index * 3;
            related_VDisks[r] = virtual_disks[virtual_idx];
        }

        DiskHead& head = disk_heads[disk_index];
        head.wait_for_schedule = false;
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
            return;
        }
        for (int i = 2; i >= 0; i--) {
            if (!blocks_empty[i]) {
                second_index = i;
                break;
            }
        }
        int distance = head.next_read_distance;
        int next_uint_index = (head.next_read_distance + head.pos) % V;

        // if (head.last_action == 'R' && distance < 4 && distance > 0 && (head.last_token < 42 || head.concentration > 0.2)) { // 尝试一直读到target_block
        if (head.last_action == 'R' && distance < 8 && distance > 0) { // 尝试一直读到target_block
            // 先检查剩余的token是否足够
            int last_token = head.last_token;
            int totoal_cost = 0;
            for (int i = 0; i < distance; i++) {
                int cost_token = calculate_read_cost(last_token, 'R');
                totoal_cost += cost_token;
                last_token = cost_token;
            }
            if (totoal_cost <= head.remaining_tokens) {  // 剩余token足够，直接一直读完
                head.pos = next_uint_index;
                head.last_action = 'R';
                head.remaining_tokens -= totoal_cost;
                head.last_token = last_token;
                actions.insert(actions.end(), distance, 'r');
            }
        }
        // 如果还有剩余令牌，处理剩余动作  // 剩余token不足，选择是Pass过去还是Jump过去
        if (head.remaining_tokens > 0 && head.pos != next_uint_index) {
            int steps = min(head.remaining_tokens, distance);
            if (steps >= G) { // Jump
                head.last_action = 'J';
                head.remaining_tokens = 0;
                head.pos = next_uint_index;
                actions.push_back('j');
                actions.push_back(' ');
                // actions.push_back(min_unit_index);
                int tmp_num = next_uint_index + 1; // 磁盘块id从1开始
                int digit_num = 1, tmp = 1;
                while (tmp_num) {
                    tmp_num /= 10;
                    if (tmp_num == 0) break;
                    digit_num++;
                    tmp *= 10;
                }
                tmp_num = next_uint_index + 1;
                for (int i = 0; i < digit_num; i++) {
                    actions.push_back('0' + ((tmp_num / tmp) % 10));
                    tmp /= 10;
                }
                return; // 出现Jump了，本次读完，返回
            }
            else {  // Pass
                head.pos = (head.pos + steps) % V;
                head.remaining_tokens -= steps;
                head.last_action = 'P';
                actions.insert(actions.end(), steps, 'p');
                if (head.remaining_tokens == 0) {   // token用完，无法进行其他操作，返回
                    return;
                }
            }
        }
        // 磁头在目标范围内，连续读取
        if (head.pos == next_uint_index) {
            continuouslyReadStrategy(head, disk_index, actions);
            return;
        }
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
    vector<map<int, int>> target_blocks; // 每个虚拟盘组的目标块 (存储相对位置)
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
                        chunk_num* chunk_size,
                        (chunk_num + 1)* chunk_size - 1
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
                    target_blocks[group_idx][obj.units[i]]++;
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
                    if (target_blocks[group_idx][obj.units[i]] == 1) {
                        target_blocks[group_idx].erase(obj.units[i]);
                    }
                    else {
                        target_blocks[group_idx][obj.units[i]]--;
                    }
                }
            }
        }
        active_requests.current_overdue_requests.clear();

    }


    void diskHead_sortByConcentration(vector<int>& disk_index_list) override {
        if (disk_index_list.empty()) return;

        // 1. 首先收集所有虚拟盘组的目标块信息
        unordered_map<int, pair<int, int>> groupIdx_targetUnits;
        for (int group_idx = 0; group_idx < disk_id_list.size(); ++group_idx) {
            if (!target_blocks[group_idx].empty()) {
                groupIdx_targetUnits[group_idx] = { target_blocks[group_idx].begin()->first, target_blocks[group_idx].rbegin()->first };
                break;
            }
        }

        if (groupIdx_targetUnits.empty()) {
            // 没有目标块需要读取，按默认顺序排序
            return;
        }
        int chunk_size = V / 3;
        // 2. 为每个磁头计算集中度和距离
        for (int disk_index : disk_index_list) {
            DiskHead& head = disk_heads[disk_index];
            vector<int> all_target_units_copy;
            vector<int> all_groupIdx_tmp(3);
            vector<int> all_groupIdx;
            // 获取该物理盘关联的所有虚拟盘
            vector<int> related_VDisk_indexs(3);
            for (int r = 0; r < 3; ++r) {
                related_VDisk_indexs[r] = r + disk_index * 3;
                all_groupIdx_tmp[r] = virtualIndex_virtualGroup_map[related_VDisk_indexs[r]];
            }
            for (int r = 0; r < 3; r++) {
                if (groupIdx_targetUnits.count(all_groupIdx_tmp[r])) {
                    int start_unit = r * chunk_size;
                    all_target_units_copy.push_back(groupIdx_targetUnits[all_groupIdx_tmp[r]].first + start_unit);
                    all_target_units_copy.push_back(groupIdx_targetUnits[all_groupIdx_tmp[r]].second + start_unit);
                    all_groupIdx.push_back(all_groupIdx_tmp[r]);
                }
            }
            if (all_target_units_copy.empty()) {
                head.next_read_distance = 1e6;
                continue;
            }
            int window_size, target_count;
            int related_vdisk_index = head.pos / chunk_size;
            if (related_vdisk_index == 3 || head.pos > all_target_units_copy[all_target_units_copy.size() - 1]) {  // head.pos在最后不使用的磁盘块上,这时其对应的下一个虚拟盘应为磁盘最开头的虚拟盘
                int min_unit_index = all_target_units_copy[0];
                int max_unit_index = all_target_units_copy[1];

                int start_unit = min_unit_index / chunk_size * chunk_size;
                int gourp_idx = all_groupIdx[0];
                head.next_read_distance = (min_unit_index - head.pos + V) % V;
                window_size = min((G - 150) / 16, max_unit_index - min_unit_index + 1);
                target_count = 0;
                auto it_tmp = target_blocks[gourp_idx].begin();
                for (int j = min_unit_index; j <= min_unit_index + window_size; j++) {
                    if (j == it_tmp->first + start_unit) {
                        target_count++;
                        it_tmp++;
                    }
                    if (it_tmp == target_blocks[gourp_idx].end()) break;
                }
            }
            else {
                related_vdisk_index = related_VDisk_indexs[related_vdisk_index];
                auto it_bigger = lower_bound(all_target_units_copy.begin(), all_target_units_copy.end(), head.pos);
                int index = distance(all_target_units_copy.begin(), it_bigger);
                if (index & 1) { // 奇，说明是max
                    int gourp_idx = all_groupIdx[index / 2];

                    int max_unit_index = all_target_units_copy[index];

                    int start_unit = max_unit_index / chunk_size * chunk_size;
                    int virtual_pos = head.pos - start_unit;
                    auto it_tmp = target_blocks[gourp_idx].lower_bound(virtual_pos);
                    int min_unit_index = it_tmp->first + start_unit;
                    head.next_read_distance = (min_unit_index - head.pos + V) % V;
                    window_size = min((G - 150) / 16, max_unit_index - min_unit_index + 1);
                    target_count = 0;
                    for (int j = min_unit_index; j <= min_unit_index + window_size; j++) {
                        if (j == it_tmp->first + start_unit) {
                            target_count++;
                            it_tmp++;
                        }
                        if (it_tmp == target_blocks[gourp_idx].end()) break;
                    }

                }
                else {  // 偶，说明是min
                    int gourp_idx = all_groupIdx[index / 2];
                    int min_unit_index = all_target_units_copy[index];
                    int max_unit_index = all_target_units_copy[index + 1];
                    int start_unit = min_unit_index / chunk_size * chunk_size;
                    head.next_read_distance = (min_unit_index - head.pos + V) % V;
                    window_size = min((G - 150) / 16, max_unit_index - min_unit_index + 1);
                    target_count = 0;
                    auto it_tmp = target_blocks[gourp_idx].begin();
                    for (int j = min_unit_index; j <= min_unit_index + window_size; j++) {
                        if (j == it_tmp->first + start_unit) {
                            target_count++;
                            it_tmp++;
                        }
                        if (it_tmp == target_blocks[gourp_idx].end()) break;
                    }
                }

            }
            head.concentration = target_count * 1.0 / window_size;
        }

        // 3. 排序策略：优先选择距离近且集中度高的磁头
        stable_sort(disk_index_list.begin(), disk_index_list.end(), [this](int a, int b) {
            return this->disk_heads[a].next_read_distance < this->disk_heads[b].next_read_distance;
            });

        stable_sort(disk_index_list.begin(), disk_index_list.end(), [this](const int& a, const int& b) {
            return this->disk_heads[a].concentration < this->disk_heads[b].concentration;
            });
    }


    // 连续读到head的token不够用
    void continuouslyReadStrategy(DiskHead& head, int disk_index, vector<char>& actions) {
        int read_cost = calculate_read_cost(head.last_token, head.last_action);
        int chunk_size = V / 3;
        // 查找第一个不小于target_key的元素
        int virtual_disk_index = disk_index * 3 + head.pos / chunk_size;
        int group_id = virtualIndex_virtualGroup_map[virtual_disk_index];
        int start_unit = head.pos / chunk_size * chunk_size;
        auto it_bigger = target_blocks[group_id].lower_bound(head.pos - start_unit);
        
        while (head.remaining_tokens >= read_cost) {
            int obj_id = disk_units[disk_index][head.pos].first;
            int obj_unit_index = disk_units[disk_index][head.pos].second;
            if (it_bigger->first + start_unit == head.pos) {
                it_bigger->second--;
                if (it_bigger->second == 0) {
                    it_bigger = target_blocks[group_id].erase(it_bigger); // 下一个键
                }
                else {
                    it_bigger++;
                }
                if (objId_units_current_read.count(obj_id) == 0) {
                    objId_units_current_read[obj_id] = vector<int>(5, 0);
                }
                objId_units_current_read[obj_id][obj_unit_index]++;
            }
            

            head.remaining_tokens -= read_cost;
            head.pos = (head.pos + 1) % V;
            head.last_token = read_cost;
            head.last_action = 'R';
            actions.push_back('r');
            head.readed_one = true;

            if (it_bigger == target_blocks[group_id].end()) {
                if (!target_blocks[group_id].empty()) {
                    int flag = (target_blocks[group_id].rbegin()->first + 1) / chunk_size;
                    if (flag == 3) {
                        flag = 0;
                    }
                    int next_virtual_disk_index = disk_index * 3 + flag;
                    int next_group_id = virtualIndex_virtualGroup_map[next_virtual_disk_index];
                    int next_start_unit = flag * chunk_size;
                    head.next_read_distance = (target_blocks[next_group_id].begin()->first + next_start_unit - head.pos + V) % V;
                    if (head.next_read_distance == 0 && head.remaining_tokens >= read_cost) {
                        head.wait_for_schedule = true;  // 调度完其它磁头后，可以继续调度
                    }
                    else {
                        if (head.remaining_tokens >= head.next_read_distance + 64) {
                            head.wait_for_schedule = true;
                        }
                    }
                }
                break; // 没有下一个target_block了，不要再读了
            }

            // 推测检查下一次该怎么读取
            int next_distance = it_bigger->first + start_unit - head.pos;
            //if (next_distance<4 || (head.remaining_tokens - 150) / 16 - 6 > next_distance) {    // 连续读即可
            if (next_distance < 12) {
                read_cost = calculate_read_cost(head.last_token, head.last_action);

            }
            else {
                int steps = min(next_distance, head.remaining_tokens);
                read_cost = calculate_read_cost(0, 'P') + steps;
                if (head.remaining_tokens < read_cost) break;  // 不能再进行下次读取了，保持原位置，保留上一次操作为读取操作
                head.remaining_tokens -= steps;
                head.pos = (head.pos + steps) % V;  // 移动到下一个可以读取的位置
                head.last_action = 'P';
                actions.insert(actions.end(), steps, 'p');
                read_cost = calculate_read_cost(0, 'P');
            }
        }

    }

    // 找到disk_index的磁盘上 的 需要读取的磁盘块区间，然后尝试在这区间上连续读取
    void minMaxReadStrategy(int disk_index, vector<char>& actions) override {

        // 获取属于该物理盘的所有虚拟盘
        vector<VirtualDisk> related_VDisks(3);
        for (int r = 0; r < 3; r++) {
            int virtual_idx = r + disk_index * 3;
            related_VDisks[r] = virtual_disks[virtual_idx];
        }

        DiskHead& head = disk_heads[disk_index];
        head.wait_for_schedule = false;
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
            return;
        }
        for (int i = 2; i >= 0; i--) {
            if (!blocks_empty[i]) {
                second_index = i;
                break;
            }
        }
        int distance = head.next_read_distance;
        int next_uint_index = (head.next_read_distance + head.pos) % V;

        //if (head.last_action == 'R' && distance < 4 && distance > 0 && (head.last_token < 42 || head.concentration > 0.2)) { // 尝试一直读到target_block
          if (head.last_action == 'R' && distance < 8 && distance > 0) { // 尝试一直读到target_block
            // 先检查剩余的token是否足够
            int last_token = head.last_token;
            int totoal_cost = 0;
            for (int i = 0; i < distance; i++) {
                int cost_token = calculate_read_cost(last_token, 'R');
                totoal_cost += cost_token;
                last_token = cost_token;
            }
            if (totoal_cost <= head.remaining_tokens) {  // 剩余token足够，直接一直读完
                head.pos = next_uint_index;
                head.last_action = 'R';
                head.remaining_tokens -= totoal_cost;
                head.last_token = last_token;
                actions.insert(actions.end(), distance, 'r');
            }
        }
        // 如果还有剩余令牌，处理剩余动作  // 剩余token不足，选择是Pass过去还是Jump过去
        if (head.remaining_tokens > 0 && head.pos != next_uint_index) {
            int steps = min(head.remaining_tokens, distance);
            if (steps >= G) { // Jump
                head.last_action = 'J';
                head.remaining_tokens = 0;
                head.pos = next_uint_index;
                actions.push_back('j');
                actions.push_back(' ');
                // actions.push_back(min_unit_index);
                int tmp_num = next_uint_index + 1; // 磁盘块id从1开始
                int digit_num = 1, tmp = 1;
                while (tmp_num) {
                    tmp_num /= 10;
                    if (tmp_num == 0) break;
                    digit_num++;
                    tmp *= 10;
                }
                tmp_num = next_uint_index + 1;
                for (int i = 0; i < digit_num; i++) {
                    actions.push_back('0' + ((tmp_num / tmp) % 10));
                    tmp /= 10;
                }
                return; // 出现Jump了，本次读完，返回
            }
            else {  // Pass
                head.pos = (head.pos + steps) % V;
                head.remaining_tokens -= steps;
                head.last_action = 'P';
                actions.insert(actions.end(), steps, 'p');
                if (head.remaining_tokens == 0) {   // token用完，无法进行其他操作，返回
                    return;
                }
            }
        }
        // 磁头在目标范围内，连续读取
        if (head.pos == next_uint_index) {
            continuouslyReadStrategy(head, disk_index, actions);
            return;
        }
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
            disk_groups.push_back(new DiskGroupThree(V, G, i * 3 + 1, 200, group_disks));
        }

        // 处理剩余的盘（4盘组或5盘组）
        int remaining_disks = N - three_group_count * 3;
        if (remaining_disks == 0) return;
        if (remaining_disks == 4) {
            // 创建4盘组
            vector<int> group4_disks(all_disks.begin() + three_group_count * 3, all_disks.end());
            disk_groups.push_back(new DiskGroupFour(V, G, three_group_count * 3 + 1, 300, group4_disks));
        }
        else // (remaining_disks == 5) 
        {
            // 创建5盘组
            vector<int> group5_disks(all_disks.begin() + three_group_count * 3, all_disks.end());
            disk_groups.push_back(new DiskGroupFive(V, G, three_group_count * 3 + 1, 300, group5_disks));
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